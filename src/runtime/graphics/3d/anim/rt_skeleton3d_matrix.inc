//===----------------------------------------------------------------------===//
// Part of the Zanna project, under the GNU GPL v3. See LICENSE.
// File: src/runtime/graphics/3d/anim/rt_skeleton3d_matrix.inc  (#included from rt_skeleton3d.c; not
// a standalone translation unit) Purpose: Skeleton3D float row-major matrix math helpers
//===----------------------------------------------------------------------===//
/**
 * @file rt_skeleton3d_matrix.inc
 * @brief Provides bounded skeleton traversal and row-major float math helpers.
 *
 * This implementation fragment centralizes matrix composition, inversion and
 * TRS decomposition; quaternion normalization and interpolation; numeric
 * conversion policy; checked runtime casts; and retained skeleton-slot
 * management. It is included by `rt_skeleton3d.c` before the animation,
 * player, blend, and skinning fragments that consume these helpers.
 */
/*==========================================================================
 * Matrix math helpers (float, row-major)
 *=========================================================================*/

/// @brief Multiply two row-major 4×4 float matrices: `out = a * b`.
/// @param[in] a Left operand containing 16 floats.
/// @param[in] b Right operand containing 16 floats.
/// @param[out] out Destination containing 16 floats.
/// @note `out` must not overlap either input matrix.
static void mat4f_mul_local(const float *a, const float *b, float *out) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            out[r * 4 + c] = a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c] +
                             a[r * 4 + 2] * b[2 * 4 + c] + a[r * 4 + 3] * b[3 * 4 + c];
}

/// @brief Set `m` to the 4×4 identity matrix.
/// @param[out] m Writable array of 16 floats.
static void mat4f_identity(float *m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

/// @brief Resolve one bone's global matrix (global = parent_global × local) by recursing into
///   its parent first, using a tri-state visited array to memoize results and break parent cycles.
/// @param[in] skeleton Skeleton providing parent indices.
/// @param[in] locals Array of `bone_count` row-major local matrices.
/// @param[in,out] globals Destination and memoization array for model-space matrices.
/// @param[in,out] state Per-bone traversal states: zero is unseen, one is
/// visiting, and two is resolved.
/// @param[in] bone_count Number of readable entries in every array.
/// @param[in] bone Bone index to resolve.
/// @details An invalid parent or detected cycle edge makes the affected bone a
/// traversal root, so its global matrix equals its local matrix.
static void skeleton3d_compute_global_bone_recursive(const rt_skeleton3d *skeleton,
                                                     const float *locals,
                                                     float *globals,
                                                     uint8_t *state,
                                                     int32_t bone_count,
                                                     int32_t bone) {
    int32_t parent;
    if (!skeleton || !locals || !globals || !state || bone < 0 || bone >= bone_count)
        return;
    if (state[bone] == 2)
        return;
    if (state[bone] == 1) {
        memcpy(&globals[bone * 16], &locals[bone * 16], 16 * sizeof(float));
        state[bone] = 2;
        return;
    }

    state[bone] = 1;
    parent = skeleton3d_valid_parent_index(skeleton, bone, bone_count);
    if (parent >= 0) {
        if (state[parent] == 1) {
            parent = -1;
        } else {
            skeleton3d_compute_global_bone_recursive(
                skeleton, locals, globals, state, bone_count, parent);
            if (state[parent] != 2)
                parent = -1;
        }
    }

    if (parent >= 0)
        mat4f_mul_local(&globals[parent * 16], &locals[bone * 16], &globals[bone * 16]);
    else
        memcpy(&globals[bone * 16], &locals[bone * 16], 16 * sizeof(float));
    state[bone] = 2;
}

/// @brief Compose global bone matrices from per-bone local matrices across the whole skeleton
///   (bounded by the safe bone count), resolving each bone through its parent chain.
/// @param[in] skeleton Skeleton providing parent indices.
/// @param[in] locals Array of row-major local matrices.
/// @param[out] globals Destination for row-major model-space matrices.
/// @param[in] bone_count Requested number of bones. A negative value or a
/// value larger than the readable skeleton count selects the full readable
/// count.
/// @details Parent order need not be topological. Invalid parents, self-links,
/// and cycle edges are treated as roots.
void skeleton3d_compute_globals_from_locals(const rt_skeleton3d *skeleton,
                                            const float *locals,
                                            float *globals,
                                            int32_t bone_count) {
    uint8_t state[VGFX3D_MAX_SKELETON_BONES];
    int32_t safe_count;
    if (!skeleton || !locals || !globals)
        return;
    safe_count = skeleton3d_safe_bone_count(skeleton);
    if (bone_count < 0 || bone_count > safe_count)
        bone_count = safe_count;
    if (bone_count <= 0)
        return;
    memset(state, 0, sizeof(state));
    for (int32_t bone = 0; bone < bone_count; bone++) {
        skeleton3d_compute_global_bone_recursive(
            skeleton, locals, globals, state, bone_count, bone);
    }
}

/// @brief Validate @p obj is a heap-allocated Mat4 and return its typed pointer (NULL on mismatch).
/// @param[in] obj Opaque candidate runtime object.
/// @return The typed matrix payload, or `NULL` for a null, non-heap, or
/// wrong-class value.
static mat4_impl *skeleton3d_mat4_checked(void *obj) {
    if (!obj || !rt_heap_is_payload(obj) || rt_obj_class_id(obj) != RT_MAT4_CLASS_ID)
        return NULL;
    return (mat4_impl *)obj;
}

/// @brief Checked cast of an opaque handle to AnimPlayer3D; NULL on class mismatch.
/// @param[in] obj Opaque candidate runtime object.
/// @return The typed player payload, or `NULL` when validation fails.
static rt_anim_player3d *anim_player3d_checked(void *obj) {
    return (rt_anim_player3d *)rt_g3d_checked_or_null(obj, RT_G3D_ANIMPLAYER3D_CLASS_ID);
}

/// @brief Checked cast of an opaque handle to AnimBlend3D; NULL on class mismatch.
/// @param[in] obj Opaque candidate runtime object.
/// @return The typed blender payload, or `NULL` when validation fails.
static rt_anim_blend3d *anim_blend3d_checked(void *obj) {
    return (rt_anim_blend3d *)rt_g3d_checked_or_null(obj, RT_G3D_ANIMBLEND3D_CLASS_ID);
}

/// @brief Checked cast of an opaque handle to Animation3D; NULL on class mismatch.
/// @param[in] obj Opaque candidate runtime object.
/// @return The typed animation payload, or `NULL` when validation fails.
static rt_animation3d *animation3d_checked(void *obj) {
    return (rt_animation3d *)rt_g3d_checked_or_null(obj, RT_G3D_ANIMATION3D_CLASS_ID);
}

/// @brief Clamp an AnimBlend3D state count without requiring contiguous live animation slots.
/// @param[in] blend Blender whose stored count is bounded.
/// @return Zero for null or nonpositive input; otherwise the stored count
/// capped at `RT_ANIM_BLEND3D_MAX_STATES`.
static int32_t anim_blend3d_state_slot_limit(const rt_anim_blend3d *blend) {
    if (!blend || blend->state_count <= 0)
        return 0;
    return blend->state_count < RT_ANIM_BLEND3D_MAX_STATES ? blend->state_count
                                                           : RT_ANIM_BLEND3D_MAX_STATES;
}

/// @brief Test whether @p value is finite and fits in float range (no NaN/Inf, no overflow on
/// cast).
/// @param[in] value Double-precision value to inspect.
/// @return Nonzero when conversion to `float` remains finite; otherwise zero.
static int skeleton3d_value_fits_float(double value) {
    return isfinite(value) && value >= -SKELETON3D_FLOAT_ABS_MAX &&
           value <= SKELETON3D_FLOAT_ABS_MAX;
}

/// @brief Convert a finite double into float range, otherwise use @p fallback.
/// @param[in] value Value to convert without saturation.
/// @param[in] fallback Result used for non-finite or out-of-range input.
/// @return The converted value when it fits, otherwise `fallback`.
static float skeleton3d_float_or(double value, float fallback) {
    if (!skeleton3d_value_fits_float(value))
        return fallback;
    return (float)value;
}

/// @brief Clamp a finite double into the full finite float range.
/// @param[in] value Value to convert with saturation.
/// @param[in] fallback Result used only for non-finite input.
/// @return `fallback` for non-finite input, `-FLT_MAX` or `FLT_MAX` for
/// overflow, or the directly converted value.
static float skeleton3d_clamp_to_float(double value, float fallback) {
    if (!isfinite(value))
        return fallback;
    if (value > SKELETON3D_FLOAT_ABS_MAX)
        return FLT_MAX;
    if (value < -SKELETON3D_FLOAT_ABS_MAX)
        return -FLT_MAX;
    return (float)value;
}

/// @brief Clamp a non-negative time/duration value into float range.
/// @param[in] value Value to sanitize.
/// @param[in] fallback Result used for non-finite input.
/// @return `fallback` for non-finite input, zero for nonpositive input,
/// `FLT_MAX` for overflow, or the converted positive value.
static float skeleton3d_clamp_nonnegative_float(double value, float fallback) {
    if (!isfinite(value))
        return fallback;
    if (value <= 0.0)
        return 0.0f;
    if (value > SKELETON3D_FLOAT_ABS_MAX)
        return FLT_MAX;
    return (float)value;
}

/// @brief Clamp an interpolation parameter to [0, 1], treating non-finite input as 0.
/// @param[in] value Parameter to sanitize.
/// @return The finite input clamped to `[0, 1]`, with non-finite values mapped
/// to zero.
static float skeleton3d_clamp01f(float value) {
    if (!isfinite(value) || value <= 0.0f)
        return 0.0f;
    if (value >= 1.0f)
        return 1.0f;
    return value;
}

/// @brief Compare double keyframe times while retaining adjacent FBX source ticks.
/// @details A scale-aware 1e-13 tolerance removes arithmetic noise without collapsing the roughly
///          2.17e-11-second tick spacing used by FBX animation curves near one second.
/// @param[in] a First time in seconds.
/// @param[in] b Second time in seconds.
/// @return Nonzero when the absolute difference is within the scale-aware
/// tolerance; otherwise zero.
static int skeleton3d_key_time_equal(double a, double b) {
    double scale = fmax(1.0, fmax(fabs(a), fabs(b)));
    return fabs(a - b) <= 1e-13 * scale;
}

/// @brief Once pose buffers exist, the skeleton topology can no longer grow safely.
/// @param[in,out] s Skeleton to mark immutable; `NULL` is ignored.
static void skeleton3d_freeze(rt_skeleton3d *s) {
    if (s)
        s->frozen = 1;
}

/// @brief Wrap a playback time into `[0, duration)` (looping). Non-finite time or a
///        non-positive/non-finite duration yields 0.0f.
/// @param[in] time Playback position in seconds, which may be negative.
/// @param[in] duration Loop duration in seconds.
/// @return The wrapped time, or zero when either input cannot define a valid
/// loop interval.
static float animation3d_wrap_time(float time, float duration) {
    if (!isfinite(time))
        return 0.0f;
    if (!isfinite(duration) || duration <= 0.0f)
        return 0.0f;
    time = fmodf(time, duration);
    if (time < 0.0f)
        time += duration;
    if (time >= duration)
        time = 0.0f;
    return time;
}

/// @brief Drop one GC reference held in `*slot` and clear the slot. NULL-safe.
/// @param[in,out] slot Address of an owned reference slot.
static void mesh3d_release_ref(void **slot) {
    rt_g3d_ref_slot_release(slot);
}

/// @brief Release a retained Skeleton3D slot only if it still points at Skeleton3D.
/// @param[in,out] slot Address of the candidate owned skeleton reference.
/// @details A stale non-skeleton value is cleared as unowned instead of being
/// released through the skeleton ownership path.
static void mesh3d_release_skeleton_slot(void **slot) {
    if (!slot || !*slot)
        return;
    if (!rt_g3d_has_class(*slot, RT_G3D_SKELETON3D_CLASS_ID)) {
        rt_g3d_ref_slot_clear_unowned(slot);
        return;
    }
    mesh3d_release_ref(slot);
}

/// @brief Retain-then-release swap: store @p value into `*slot`, releasing the previous occupant.
/// @details Retain-before-release prevents a self-reassign or shared-owner case from
///   prematurely dropping the refcount to zero during the transition.
/// @param[in,out] slot Address of the retained skeleton slot.
/// @param[in] value New skeleton payload, or `NULL` to clear the slot.
static void mesh3d_assign_skeleton_ref(void **slot, void *value) {
    if (!slot || *slot == value)
        return;
    rt_obj_retain_maybe(value);
    mesh3d_release_skeleton_slot(slot);
    *slot = value;
}

/// @brief Invert a 4x4 float matrix. Returns 0 on success, -1 if singular.
/// @param[in] m Row-major source matrix.
/// @param[out] out Destination for the inverse; it may alias `m`.
/// @return Zero on success, or `-1` when the determinant is non-finite or has
/// magnitude below `1e-12`.
static int mat4f_invert(const float *m, float *out) {
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] +
             m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] -
             m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] +
             m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] -
              m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] -
             m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] +
             m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] -
             m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] +
              m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
             m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] -
             m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] +
              m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] -
              m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] -
             m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
             m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
              m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
              m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (!isfinite(det) || fabsf(det) < 1e-12f)
        return -1;
    float inv_det = 1.0f / det;
    for (int i = 0; i < 16; i++)
        out[i] = inv[i] * inv_det;
    return 0;
}

/// @brief Build TRS matrix from float arrays (row-major).
/// @param[in] pos Three-component translation.
/// @param[in] quat Rotation quaternion in `(x, y, z, w)` order.
/// @param[in] scl Three-component scale.
/// @param[out] out Destination row-major affine matrix.
static void build_trs_float(const float *pos, const float *quat, const float *scl, float *out) {
    float x = quat[0], y = quat[1], z = quat[2], w = quat[3];
    float x2 = x + x, y2 = y + y, z2 = z + z;
    float xx = x * x2, xy = x * y2, xz = x * z2;
    float yy = y * y2, yz = y * z2, zz = z * z2;
    float wx = w * x2, wy = w * y2, wz = w * z2;

    out[0] = (1.0f - (yy + zz)) * scl[0];
    out[1] = (xy - wz) * scl[1];
    out[2] = (xz + wy) * scl[2];
    out[3] = pos[0];
    out[4] = (xy + wz) * scl[0];
    out[5] = (1.0f - (xx + zz)) * scl[1];
    out[6] = (yz - wx) * scl[2];
    out[7] = pos[1];
    out[8] = (xz - wy) * scl[0];
    out[9] = (yz + wx) * scl[1];
    out[10] = (1.0f - (xx + yy)) * scl[2];
    out[11] = pos[2];
    out[12] = 0.0f;
    out[13] = 0.0f;
    out[14] = 0.0f;
    out[15] = 1.0f;
}

/// @brief Write the identity quaternion into a four-float destination.
/// @param[out] out Destination in `(x, y, z, w)` order.
static void quat_identity_float(float *out);

/// @brief SLERP between two quaternions (float arrays, x,y,z,w).
/// @param[in] a Starting quaternion.
/// @param[in] b Ending quaternion.
/// @param[in] t Interpolation fraction, clamped to `[0, 1]`.
/// @param[out] out Destination normalized quaternion.
/// @details The shortest hemisphere is selected. Nearly equal inputs use
/// normalized linear interpolation, while a non-finite dot product or
/// degenerate result produces the identity quaternion.
static void quat_slerp_float(const float *a, const float *b, float t, float *out) {
    float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    float nb[4] = {b[0], b[1], b[2], b[3]};
    t = skeleton3d_clamp01f(t);
    if (dot < 0.0f) {
        dot = -dot;
        nb[0] = -nb[0];
        nb[1] = -nb[1];
        nb[2] = -nb[2];
        nb[3] = -nb[3];
    }
    if (!isfinite(dot)) {
        quat_identity_float(out);
        return;
    }
    if (dot > 1.0f)
        dot = 1.0f;
    if (dot < -1.0f)
        dot = -1.0f;
    if (dot > 0.9995f) {
        /* Nearly identical: linear interpolation + normalize */
        for (int i = 0; i < 4; i++)
            out[i] = a[i] + t * (nb[i] - a[i]);
    } else {
        float theta = acosf(dot);
        float sin_theta = sinf(theta);
        float wa = sinf((1.0f - t) * theta) / sin_theta;
        float wb = sinf(t * theta) / sin_theta;
        for (int i = 0; i < 4; i++)
            out[i] = wa * a[i] + wb * nb[i];
    }
    /* Normalize */
    float len = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3]);
    if (isfinite(len) && len > 1e-8f)
        for (int i = 0; i < 4; i++)
            out[i] /= len;
    else
        quat_identity_float(out);
}

/// @brief Write the identity quaternion (0,0,0,1) into `out[4]`.
/// @param[out] out Writable four-float quaternion.
static void quat_identity_float(float *out) {
    out[0] = 0.0f;
    out[1] = 0.0f;
    out[2] = 0.0f;
    out[3] = 1.0f;
}

/// @brief Normalize a quaternion in place, falling back to identity when any lane
///        is non-finite or the length underflows (≤ 1e-8).
/// @param[in,out] q Four-float quaternion, or `NULL`.
static void quat_normalize_float(float *q) {
    float len;
    if (!q)
        return;
    if (!isfinite(q[0]) || !isfinite(q[1]) || !isfinite(q[2]) || !isfinite(q[3])) {
        quat_identity_float(q);
        return;
    }
    len = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!isfinite(len) || len <= 1e-8f) {
        quat_identity_float(q);
        return;
    }
    for (int i = 0; i < 4; i++)
        q[i] /= len;
}

/// @brief Convert a 3x3 rotation matrix to a unit quaternion in `out[4]`.
/// @details Shepperd's method: branches on the largest of trace/r00/r11/r22 to
///          keep the `sqrt` argument well away from zero, avoiding the precision
///          loss the naive `sqrt(1+trace)` form suffers near 180° rotations.
///          Non-finite input yields identity; the result is normalized.
/// @param[in] r00 Matrix element at row 0, column 0.
/// @param[in] r01 Matrix element at row 0, column 1.
/// @param[in] r02 Matrix element at row 0, column 2.
/// @param[in] r10 Matrix element at row 1, column 0.
/// @param[in] r11 Matrix element at row 1, column 1.
/// @param[in] r12 Matrix element at row 1, column 2.
/// @param[in] r20 Matrix element at row 2, column 0.
/// @param[in] r21 Matrix element at row 2, column 1.
/// @param[in] r22 Matrix element at row 2, column 2.
/// @param[out] out Destination quaternion in `(x, y, z, w)` order.
static void quat_from_matrix3_float(float r00,
                                    float r01,
                                    float r02,
                                    float r10,
                                    float r11,
                                    float r12,
                                    float r20,
                                    float r21,
                                    float r22,
                                    float *out) {
    float tr;
    if (!out)
        return;
    if (!isfinite(r00) || !isfinite(r01) || !isfinite(r02) || !isfinite(r10) || !isfinite(r11) ||
        !isfinite(r12) || !isfinite(r20) || !isfinite(r21) || !isfinite(r22)) {
        quat_identity_float(out);
        return;
    }
    tr = r00 + r11 + r22;
    if (tr > 0.0f) {
        float s = sqrtf(tr + 1.0f) * 2.0f;
        out[3] = 0.25f * s;
        out[0] = (r21 - r12) / s;
        out[1] = (r02 - r20) / s;
        out[2] = (r10 - r01) / s;
    } else if (r00 > r11 && r00 > r22) {
        float s = sqrtf(1.0f + r00 - r11 - r22) * 2.0f;
        out[3] = (r21 - r12) / s;
        out[0] = 0.25f * s;
        out[1] = (r01 + r10) / s;
        out[2] = (r02 + r20) / s;
    } else if (r11 > r22) {
        float s = sqrtf(1.0f + r11 - r00 - r22) * 2.0f;
        out[3] = (r02 - r20) / s;
        out[0] = (r01 + r10) / s;
        out[1] = 0.25f * s;
        out[2] = (r12 + r21) / s;
    } else {
        float s = sqrtf(1.0f + r22 - r00 - r11) * 2.0f;
        out[3] = (r10 - r01) / s;
        out[0] = (r02 + r20) / s;
        out[1] = (r12 + r21) / s;
        out[2] = 0.25f * s;
    }
    quat_normalize_float(out);
}

/// @brief Decompose a row-major 4x4 float matrix into translation, rotation
///        (quaternion) and scale.
/// @details Scale per axis is the length of each basis column; the basis is
///          divided through by scale before quaternion extraction so the rotation
///          is orthonormal. Non-finite entries degrade to 0/identity/unit.
/// @param[in] m Source row-major affine matrix.
/// @param[out] out_pos Destination three-component translation.
/// @param[out] out_rot Destination normalized rotation quaternion.
/// @param[out] out_scl Destination three-component nonnegative scale.
static void mat4f_decompose_trs(const float *m, float *out_pos, float *out_rot, float *out_scl) {
    float sx;
    float sy;
    float sz;
    float inv_sx;
    float inv_sy;
    float inv_sz;
    if (!m || !out_pos || !out_rot || !out_scl)
        return;
    out_pos[0] = isfinite(m[3]) ? m[3] : 0.0f;
    out_pos[1] = isfinite(m[7]) ? m[7] : 0.0f;
    out_pos[2] = isfinite(m[11]) ? m[11] : 0.0f;
    sx = sqrtf(m[0] * m[0] + m[4] * m[4] + m[8] * m[8]);
    sy = sqrtf(m[1] * m[1] + m[5] * m[5] + m[9] * m[9]);
    sz = sqrtf(m[2] * m[2] + m[6] * m[6] + m[10] * m[10]);
    out_scl[0] = isfinite(sx) && sx > 1e-6f ? sx : 1.0f;
    out_scl[1] = isfinite(sy) && sy > 1e-6f ? sy : 1.0f;
    out_scl[2] = isfinite(sz) && sz > 1e-6f ? sz : 1.0f;
    inv_sx = 1.0f / out_scl[0];
    inv_sy = 1.0f / out_scl[1];
    inv_sz = 1.0f / out_scl[2];
    quat_from_matrix3_float(m[0] * inv_sx,
                            m[1] * inv_sy,
                            m[2] * inv_sz,
                            m[4] * inv_sx,
                            m[5] * inv_sy,
                            m[6] * inv_sz,
                            m[8] * inv_sx,
                            m[9] * inv_sy,
                            m[10] * inv_sz,
                            out_rot);
}
