//===----------------------------------------------------------------------===//
// Part of the Zanna project, under the GNU GPL v3. See LICENSE.
// File: src/runtime/graphics/3d/anim/rt_skeleton3d_animation.inc  (#included from rt_skeleton3d.c;
// not a standalone translation unit) Purpose: Animation3D clip implementation and keyframe sampling
//===----------------------------------------------------------------------===//
/// @file
/// @brief Implements Animation3D lifecycle, keyframes, sampling, and retargeting.
/// @details This fragment is included by `rt_skeleton3d.c`. It owns defensive
///          reference-slot helpers, time-sorted sparse TRS channel storage,
///          linear/SLERP and glTF Hermite sampling, tolerance compression, and
///          alias/name/humanoid-role retargeting with proportional translation.

/*==========================================================================
 * Animation3D implementation
 *=========================================================================*/

/// @brief GC finalizer for Animation3D — releases all channel and keyframe arrays.
/// @note The clip name is stored inline; only channel/keyframe arrays require
///       release.
/// @param[in,out] obj Animation3D storage being finalized.
static void rt_animation3d_finalize(void *obj) {
    rt_animation3d *a = (rt_animation3d *)rt_g3d_checked_or_null(obj, RT_G3D_ANIMATION3D_CLASS_ID);
    int32_t channel_count;
    if (!a)
        return;
    channel_count = animation3d_safe_channel_count(a);
    for (int32_t i = 0; i < channel_count; i++)
        free(a->channels[i].keyframes);
    free(a->channels);
    a->channels = NULL;
    a->channel_count = 0;
    a->channel_capacity = 0;
}

/// @brief Release the GC-managed object in @p slot and clear the pointer to NULL.
/// @details Mirrors the `scene3d_release_ref` pattern used in rt_scene3d.c — kept
///   as a local copy so this translation unit has no dependency on scene3d internals.
///   Safe to call with a NULL slot or a slot already holding NULL.
/// @param[in,out] slot Address of a retained runtime-object slot.
static void animation3d_release_ref(void **slot) {
    rt_g3d_ref_slot_release(slot);
}

/// @brief Release a retained private slot only when it still holds the expected 3D class.
/// @details A wrong-class non-null value is cleared as unowned corruption.
/// @param[in,out] slot Address of the retained slot.
/// @param[in] class_id Expected graphics-runtime class identifier.
static void animation3d_release_class_ref(void **slot, int64_t class_id) {
    if (!slot || !*slot)
        return;
    if (!rt_g3d_has_class(*slot, class_id)) {
        rt_g3d_ref_slot_clear_unowned(slot);
        return;
    }
    animation3d_release_ref(slot);
}

/// @brief Typed retain/release swap for private animation-runtime object slots.
/// @details Retains @p value before releasing the prior class-validated slot;
///          assigning the existing pointer is a no-op.
/// @param[in,out] slot Address of the retained destination slot.
/// @param[in] value Borrowed replacement object, or `NULL`.
/// @param[in] class_id Class expected for the prior slot value.
static void animation3d_assign_class_ref(void **slot, void *value, int64_t class_id) {
    if (!slot || *slot == value)
        return;
    rt_obj_retain_maybe(value);
    animation3d_release_class_ref(slot, class_id);
    *slot = value;
}

/// @brief Borrow a private Skeleton3D slot, clearing it if corruption changed its class.
/// @param[in,out] slot Address of the private skeleton slot.
/// @return Borrowed typed skeleton pointer, or `NULL`.
static rt_skeleton3d *animation3d_skeleton_slot(void **slot) {
    if (!slot || !*slot)
        return NULL;
    if (!rt_g3d_has_class(*slot, RT_G3D_SKELETON3D_CLASS_ID)) {
        rt_g3d_ref_slot_clear_unowned(slot);
        return NULL;
    }
    return (rt_skeleton3d *)*slot;
}

/// @brief Borrow a private Animation3D slot, clearing it if corruption changed its class.
/// @param[in,out] slot Address of the private clip slot.
/// @return Borrowed typed animation pointer, or `NULL`.
static rt_animation3d *animation3d_clip_slot(void **slot) {
    if (!slot || !*slot)
        return NULL;
    if (!rt_g3d_has_class(*slot, RT_G3D_ANIMATION3D_CLASS_ID)) {
        rt_g3d_ref_slot_clear_unowned(slot);
        return NULL;
    }
    return (rt_animation3d *)*slot;
}

/// @brief Ensure a quaternion keyframe is unit-length; substitute identity on any
///        degenerate or non-finite input so bone transforms stay numerically stable.
/// @details Two separate guards: a NaN/Inf component check (which would make the
///   magnitude computation itself non-finite) and a near-zero length check (which
///   would produce NaN on division). The identity quaternion (0,0,0,1) is used as
///   the fallback so a bad keyframe leaves the bone in its rest pose rather than
///   producing a degenerate skinning matrix that collapses or inverts geometry.
/// @param q float[4] quaternion in (x, y, z, w) order; modified in-place.
static void sanitize_keyframe_quat(float *q) {
    if (!q)
        return;
    if (!isfinite(q[0]) || !isfinite(q[1]) || !isfinite(q[2]) || !isfinite(q[3])) {
        q[0] = q[1] = q[2] = 0.0f;
        q[3] = 1.0f;
        return;
    }
    float len = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!isfinite(len) || len <= 1e-8f) {
        q[0] = q[1] = q[2] = 0.0f;
        q[3] = 1.0f;
        return;
    }
    q[0] /= len;
    q[1] /= len;
    q[2] /= len;
    q[3] /= len;
}

/// @brief Clamp one sampled animation lane to a finite range that downstream matrix
///        multiplication can tolerate.
/// @param[in] value Sampled float lane.
/// @param[in] fallback Replacement for non-finite input; itself normalized to
///                     zero when non-finite.
/// @return Finite lane clamped to `±SKELETON3D_ANIM_ABS_MAX`.
static float animation3d_sanitize_lane(float value, float fallback) {
    if (!isfinite(fallback))
        fallback = 0.0f;
    if (!isfinite(value))
        return fallback;
    if (value > SKELETON3D_ANIM_ABS_MAX)
        return SKELETON3D_ANIM_ABS_MAX;
    if (value < -SKELETON3D_ANIM_ABS_MAX)
        return -SKELETON3D_ANIM_ABS_MAX;
    return value;
}

/// @brief Sanitize sampled TRS data before building local bone matrices.
/// @details Position and scale lanes use their corresponding fallbacks and
///          range clamp; rotation uses the complete fallback only when all
///          four lanes are finite, then is normalized or replaced by identity.
/// @param[in,out] pos Optional writable three-float translation.
/// @param[in,out] rot Optional writable four-float quaternion.
/// @param[in,out] scl Optional writable three-float scale.
/// @param[in] fallback_pos Optional borrowed translation fallback.
/// @param[in] fallback_rot Optional borrowed quaternion fallback.
/// @param[in] fallback_scl Optional borrowed scale fallback.
static void animation3d_sanitize_trs(float *pos,
                                     float *rot,
                                     float *scl,
                                     const float *fallback_pos,
                                     const float *fallback_rot,
                                     const float *fallback_scl) {
    int valid_rot = 1;
    if (pos) {
        for (int32_t i = 0; i < 3; ++i) {
            float fallback = fallback_pos ? fallback_pos[i] : 0.0f;
            pos[i] = animation3d_sanitize_lane(pos[i], fallback);
        }
    }
    if (scl) {
        for (int32_t i = 0; i < 3; ++i) {
            float fallback = fallback_scl ? fallback_scl[i] : 1.0f;
            if (!isfinite(fallback))
                fallback = 1.0f;
            scl[i] = animation3d_sanitize_lane(scl[i], fallback);
        }
    }
    if (!rot)
        return;
    for (int32_t i = 0; i < 4; ++i) {
        if (!isfinite(rot[i])) {
            valid_rot = 0;
            break;
        }
    }
    if (!valid_rot) {
        if (fallback_rot && isfinite(fallback_rot[0]) && isfinite(fallback_rot[1]) &&
            isfinite(fallback_rot[2]) && isfinite(fallback_rot[3])) {
            memcpy(rot, fallback_rot, 4 * sizeof(float));
        } else {
            rot[0] = rot[1] = rot[2] = 0.0f;
            rot[3] = 1.0f;
        }
    }
    sanitize_keyframe_quat(rot);
}

/// @brief Create a new empty animation clip with the given identifier and duration (seconds).
/// @param[in] name Borrowed runtime name copied to 63 bytes, or `NULL`.
/// @param[in] duration Requested positive duration; nonpositive/non-finite
///                     input defaults to one second.
/// @return New GC-managed Animation3D, or `NULL` after an allocation trap.
void *rt_animation3d_new(rt_string name, double duration) {
    rt_animation3d *a = (rt_animation3d *)rt_obj_new_i64(RT_G3D_ANIMATION3D_CLASS_ID,
                                                         (int64_t)sizeof(rt_animation3d));
    if (!a) {
        rt_trap("Animation3D.New: memory allocation failed");
        return NULL;
    }
    a->vptr = NULL;
    memset(a->name, 0, 64);
    if (name) {
        const char *cstr = rt_string_cstr(name);
        if (cstr) {
            size_t len = strlen(cstr);
            if (len > 63)
                len = 63;
            memcpy(a->name, cstr, len);
        }
    }
    a->channels = NULL;
    a->channel_count = 0;
    a->channel_capacity = 0;
    a->duration = (isfinite(duration) && duration > 0.0)
                      ? skeleton3d_clamp_nonnegative_float(duration, 1.0f)
                      : 1.0f;
    a->looping = 0;
    rt_obj_set_finalizer(a, rt_animation3d_finalize);
    return a;
}

/// @brief Try to append one keyframe to a bone's time-sorted track.
/// @details This status-returning internal implementation lets transactional importers reject a
///          complete asset when channel/keyframe growth fails. Equal-time keys replace the existing
///          value, matching the compatibility void API.
/// @param[in,out] obj Animation3D to modify.
/// @param[in] bone_index Target bone index in `[0,1023]`.
/// @param[in] time Key time; negatives clamp to zero, non-finite or
///                 non-float-range values fail.
/// @param[in] position Optional borrowed Vec3 translation.
/// @param[in] rotation Optional borrowed Quat rotation.
/// @param[in] scale Optional borrowed Vec3 scale.
/// @return Nonzero when the key was inserted or replaced.
int rt_animation3d_try_add_keyframe(
    void *obj, int64_t bone_index, double time, void *position, void *rotation, void *scale) {
    rt_animation3d *a = (rt_animation3d *)rt_g3d_checked_or_null(obj, RT_G3D_ANIMATION3D_CLASS_ID);
    if (!a || bone_index < 0 || bone_index >= VGFX3D_MAX_SKELETON_BONES || !isfinite(time) ||
        fabs(time) > FLT_MAX)
        return 0;
    if (time < 0.0)
        time = 0.0;
    if ((position && !rt_g3d_is_vec3(position)) || (rotation && !rt_g3d_is_quat(rotation)) ||
        (scale && !rt_g3d_is_vec3(scale)))
        return 0;
    vgfx3d_keyframe_t new_kf;
    memset(&new_kf, 0, sizeof(new_kf));
    new_kf.time = time;
    if (position) {
        double px = rt_vec3_x(position);
        double py = rt_vec3_y(position);
        double pz = rt_vec3_z(position);
        if (skeleton3d_value_fits_float(px)) {
            new_kf.position[0] = (float)px;
            new_kf.position_mask |= 1u;
        }
        if (skeleton3d_value_fits_float(py)) {
            new_kf.position[1] = (float)py;
            new_kf.position_mask |= 2u;
        }
        if (skeleton3d_value_fits_float(pz)) {
            new_kf.position[2] = (float)pz;
            new_kf.position_mask |= 4u;
        }
    }
    if (rotation) {
        double qx = rt_quat_x(rotation);
        double qy = rt_quat_y(rotation);
        double qz = rt_quat_z(rotation);
        double qw = rt_quat_w(rotation);
        if (skeleton3d_value_fits_float(qx) && skeleton3d_value_fits_float(qy) &&
            skeleton3d_value_fits_float(qz) && skeleton3d_value_fits_float(qw)) {
            new_kf.rotation[0] = (float)qx;
            new_kf.rotation[1] = (float)qy;
            new_kf.rotation[2] = (float)qz;
            new_kf.rotation[3] = (float)qw;
            new_kf.rotation_mask = 0x0Fu;
        } else {
            new_kf.rotation[3] = 1.0f;
        }
    } else {
        new_kf.rotation[3] = 1.0f;
    }
    sanitize_keyframe_quat(new_kf.rotation);
    new_kf.scale_xyz[0] = 1.0f;
    new_kf.scale_xyz[1] = 1.0f;
    new_kf.scale_xyz[2] = 1.0f;
    if (scale) {
        double sx = rt_vec3_x(scale);
        double sy = rt_vec3_y(scale);
        double sz = rt_vec3_z(scale);
        if (skeleton3d_value_fits_float(sx)) {
            new_kf.scale_xyz[0] = (float)sx;
            new_kf.scale_mask |= 1u;
        }
        if (skeleton3d_value_fits_float(sy)) {
            new_kf.scale_xyz[1] = (float)sy;
            new_kf.scale_mask |= 2u;
        }
        if (skeleton3d_value_fits_float(sz)) {
            new_kf.scale_xyz[2] = (float)sz;
            new_kf.scale_mask |= 4u;
        }
    }

    a->channel_count = animation3d_safe_channel_count(a);
    if (!a->channels)
        a->channel_capacity = 0;

    /* Find or create channel for this bone */
    vgfx3d_anim_channel_t *ch = NULL;
    for (int32_t i = 0; i < a->channel_count; i++)
        if (a->channels[i].bone_index == (int32_t)bone_index) {
            ch = &a->channels[i];
            break;
        }

    if (!ch) {
        if (a->channel_count >= RT_ANIMATION3D_MAX_CHANNELS)
            return 0;
        if (a->channel_count >= a->channel_capacity) {
            if (a->channel_capacity > INT32_MAX / 2)
                return 0;
            int32_t new_cap = a->channel_capacity == 0 ? 8 : a->channel_capacity * 2;
            if (new_cap > RT_ANIMATION3D_MAX_CHANNELS)
                new_cap = RT_ANIMATION3D_MAX_CHANNELS;
            if (new_cap <= a->channel_count)
                return 0;
            if ((size_t)new_cap > SIZE_MAX / sizeof(vgfx3d_anim_channel_t))
                return 0;
            vgfx3d_anim_channel_t *nc = (vgfx3d_anim_channel_t *)realloc(
                a->channels, (size_t)new_cap * sizeof(vgfx3d_anim_channel_t));
            if (!nc)
                return 0;
            a->channels = nc;
            a->channel_capacity = new_cap;
        }
        ch = &a->channels[a->channel_count++];
        memset(ch, 0, sizeof(vgfx3d_anim_channel_t));
        ch->bone_index = (int32_t)bone_index;
    }

    ch->keyframe_count = animation3d_safe_keyframe_count(ch);
    if (!ch->keyframes)
        ch->keyframe_capacity = 0;
    if (ch->keyframe_count >= ch->keyframe_capacity) {
        if (ch->keyframe_count >= RT_ANIMATION3D_MAX_KEYFRAMES_PER_CHANNEL)
            return 0;
        if (ch->keyframe_capacity > INT32_MAX / 2)
            return 0;
        int32_t new_cap = ch->keyframe_capacity == 0 ? 16 : ch->keyframe_capacity * 2;
        if (new_cap > RT_ANIMATION3D_MAX_KEYFRAMES_PER_CHANNEL)
            new_cap = RT_ANIMATION3D_MAX_KEYFRAMES_PER_CHANNEL;
        if (new_cap <= ch->keyframe_count)
            return 0;
        if ((size_t)new_cap > SIZE_MAX / sizeof(vgfx3d_keyframe_t))
            return 0;
        vgfx3d_keyframe_t *nk = (vgfx3d_keyframe_t *)realloc(
            ch->keyframes, (size_t)new_cap * sizeof(vgfx3d_keyframe_t));
        if (!nk)
            return 0;
        ch->keyframes = nk;
        ch->keyframe_capacity = new_cap;
    }

    int32_t insert = ch->keyframe_count;
    if (insert > 0 && ch->keyframes[insert - 1].time <= new_kf.time) {
        if (skeleton3d_key_time_equal(ch->keyframes[insert - 1].time, new_kf.time)) {
            ch->keyframes[insert - 1] = new_kf;
            return 1;
        }
    } else {
        while (insert > 0 && ch->keyframes[insert - 1].time > new_kf.time + 1e-13)
            insert--;
    }
    if (insert < ch->keyframe_count &&
        skeleton3d_key_time_equal(ch->keyframes[insert].time, new_kf.time)) {
        ch->keyframes[insert] = new_kf;
        return 1;
    }
    if (insert < ch->keyframe_count) {
        memmove(&ch->keyframes[insert + 1],
                &ch->keyframes[insert],
                (size_t)(ch->keyframe_count - insert) * sizeof(ch->keyframes[0]));
    }
    ch->keyframes[insert] = new_kf;
    ch->keyframe_count++;
    return 1;
}

/// @brief Append one keyframe through the compatibility-preserving public void API.
/// @details Callers requiring failure status use `rt_animation3d_try_add_keyframe` internally.
/// @param[in,out] obj Animation3D to modify.
/// @param[in] bone_index Target bone index.
/// @param[in] time Key time in seconds.
/// @param[in] position Optional borrowed Vec3 translation.
/// @param[in] rotation Optional borrowed Quat rotation.
/// @param[in] scale Optional borrowed Vec3 scale.
void rt_animation3d_add_keyframe(
    void *obj, int64_t bone_index, double time, void *position, void *rotation, void *scale) {
    (void)rt_animation3d_try_add_keyframe(obj, bone_index, time, position, rotation, scale);
}

/// @brief True when @p mid is reproduced within tolerance by interpolating between
///   @p a and @p b at @p mid's time (lerp for position/scale, slerp for rotation).
/// @details Cubic keys, mismatched component-presence masks, invalid spans, and
///          out-of-segment middle times are never considered redundant.
/// @param[in] a Previously kept keyframe.
/// @param[in] mid Candidate keyframe.
/// @param[in] b Following keyframe.
/// @param[in] pos_tolerance Maximum absolute translation-lane error.
/// @param[in] rot_tolerance Maximum `1 - abs(quaternion dot)` error.
/// @param[in] scl_tolerance Maximum absolute scale-lane error.
/// @return Nonzero only when every present component is reconstructible.
static int animation3d_keyframe_is_redundant(const vgfx3d_keyframe_t *a,
                                             const vgfx3d_keyframe_t *mid,
                                             const vgfx3d_keyframe_t *b,
                                             float pos_tolerance,
                                             float rot_tolerance,
                                             float scl_tolerance) {
    double span = b->time - a->time;
    float alpha;
    if (a->cubic_mask || mid->cubic_mask || b->cubic_mask)
        return 0; /* never reduce across Hermite segments */
    if (a->position_mask != mid->position_mask || mid->position_mask != b->position_mask ||
        a->rotation_mask != mid->rotation_mask || mid->rotation_mask != b->rotation_mask ||
        a->scale_mask != mid->scale_mask || mid->scale_mask != b->scale_mask)
        return 0;
    if (!(span > 0.0f))
        return 0;
    alpha = (float)((mid->time - a->time) / span);
    if (!(alpha >= 0.0f && alpha <= 1.0f))
        return 0;
    for (int i = 0; i < 3; i++) {
        float lp = a->position[i] + alpha * (b->position[i] - a->position[i]);
        float ls = a->scale_xyz[i] + alpha * (b->scale_xyz[i] - a->scale_xyz[i]);
        if (fabsf(lp - mid->position[i]) > pos_tolerance)
            return 0;
        if (fabsf(ls - mid->scale_xyz[i]) > scl_tolerance)
            return 0;
    }
    {
        float blended[4];
        float dot;
        quat_slerp_float(a->rotation, b->rotation, alpha, blended);
        dot = blended[0] * mid->rotation[0] + blended[1] * mid->rotation[1] +
              blended[2] * mid->rotation[2] + blended[3] * mid->rotation[3];
        if (fabsf(dot) < 1.0f - rot_tolerance)
            return 0;
    }
    return 1;
}

/// @brief Tolerance-based keyframe reduction: drop keys whose values are
///   reconstructible by interpolating their kept neighbours. Cubic (Hermite)
///   keys and keys with mismatched presence masks are always kept, as are the
///   first and last key of every channel.
/// @param[in,out] obj Animation3D whose channels to reduce in place.
/// @param[in] pos_tolerance Translation-lane error tolerance.
/// @param[in] rot_tolerance Quaternion-dot error tolerance.
/// @param[in] scl_tolerance Scale-lane error tolerance.
/// @return The number of keyframes dropped across all channels.
int64_t rt_animation3d_compress_keyframes(void *obj,
                                          double pos_tolerance,
                                          double rot_tolerance,
                                          double scl_tolerance) {
    rt_animation3d *a = (rt_animation3d *)rt_g3d_checked_or_null(obj, RT_G3D_ANIMATION3D_CLASS_ID);
    int64_t dropped = 0;
    float pos_tol = (float)pos_tolerance;
    float rot_tol = (float)rot_tolerance;
    float scl_tol = (float)scl_tolerance;
    if (!a)
        return 0;
    for (int32_t c = 0, count = animation3d_safe_channel_count(a); c < count; c++) {
        vgfx3d_anim_channel_t *ch = &a->channels[c];
        int32_t n = animation3d_safe_keyframe_count(ch);
        int32_t kept;
        if (n < 3)
            continue;
        kept = 1; /* first key always survives */
        for (int32_t i = 1; i < n; i++) {
            if (i < n - 1 && animation3d_keyframe_is_redundant(&ch->keyframes[kept - 1],
                                                               &ch->keyframes[i],
                                                               &ch->keyframes[i + 1],
                                                               pos_tol,
                                                               rot_tol,
                                                               scl_tol)) {
                dropped++;
                continue;
            }
            if (kept != i)
                ch->keyframes[kept] = ch->keyframes[i];
            kept++;
        }
        ch->keyframe_count = kept;
    }
    return dropped;
}

/// @brief Attach CUBICSPLINE Hermite tangents to the keyframe at (@p bone_index,
///   @p time). Each in/out pair may be NULL to leave that channel linear; passing
///   tangents marks the channel cubic in the keyframe's cubic_mask.
/// @details Import-facing: the glTF loader emits keyframes first (time-sorted) and
///   then decorates them with the sampler's authored or derived tangents so
///   skeletal playback reproduces the source spline exactly per segment.
/// @param[in,out] obj Animation3D containing the existing key.
/// @param[in] bone_index Target channel bone index.
/// @param[in] time Existing key time matched by runtime tolerance.
/// @param[in] pos_in Optional borrowed three-float incoming position tangent.
/// @param[in] pos_out Optional borrowed three-float outgoing position tangent.
/// @param[in] rot_in Optional borrowed four-float incoming rotation tangent.
/// @param[in] rot_out Optional borrowed four-float outgoing rotation tangent.
/// @param[in] scl_in Optional borrowed three-float incoming scale tangent.
/// @param[in] scl_out Optional borrowed three-float outgoing scale tangent.
void rt_animation3d_set_keyframe_tangents(void *obj,
                                          int64_t bone_index,
                                          double time,
                                          const float *pos_in,
                                          const float *pos_out,
                                          const float *rot_in,
                                          const float *rot_out,
                                          const float *scl_in,
                                          const float *scl_out) {
    rt_animation3d *a = (rt_animation3d *)rt_g3d_checked_or_null(obj, RT_G3D_ANIMATION3D_CLASS_ID);
    vgfx3d_anim_channel_t *ch = NULL;
    vgfx3d_keyframe_t *kf = NULL;
    if (!a || !isfinite(time))
        return;
    for (int32_t c = 0, count = animation3d_safe_channel_count(a); c < count; c++) {
        if (a->channels[c].bone_index == (int32_t)bone_index) {
            ch = &a->channels[c];
            break;
        }
    }
    if (!ch)
        return;
    for (int32_t k = 0, count = animation3d_safe_keyframe_count(ch); k < count; k++) {
        if (skeleton3d_key_time_equal(ch->keyframes[k].time, time)) {
            kf = &ch->keyframes[k];
            break;
        }
    }
    if (!kf)
        return;
    if (pos_in && pos_out) {
        memcpy(kf->pos_in_tangent, pos_in, 3 * sizeof(float));
        memcpy(kf->pos_out_tangent, pos_out, 3 * sizeof(float));
        kf->cubic_mask |= 1u;
    }
    if (rot_in && rot_out) {
        memcpy(kf->rot_in_tangent, rot_in, 4 * sizeof(float));
        memcpy(kf->rot_out_tangent, rot_out, 4 * sizeof(float));
        kf->cubic_mask |= 2u;
    }
    if (scl_in && scl_out) {
        memcpy(kf->scale_in_tangent, scl_in, 3 * sizeof(float));
        memcpy(kf->scale_out_tangent, scl_out, 3 * sizeof(float));
        kf->cubic_mask |= 4u;
    }
}

/// @brief Mark this clip as looping (wraps back to t=0 at end) or one-shot (stops at end).
/// @param[in,out] obj Animation3D to configure.
/// @param[in] loop Zero for one-shot; nonzero for looping.
void rt_animation3d_set_looping(void *obj, int8_t loop) {
    rt_animation3d *a = (rt_animation3d *)rt_g3d_checked_or_null(obj, RT_G3D_ANIMATION3D_CLASS_ID);
    if (a)
        a->looping = loop ? 1 : 0;
}

/// @brief Read the looping flag (0 = one-shot, 1 = loops).
/// @param[in] obj Animation3D to inspect.
/// @return `1` for looping; `0` for one-shot or invalid input.
int8_t rt_animation3d_get_looping(void *obj) {
    rt_animation3d *a = (rt_animation3d *)rt_g3d_checked_or_null(obj, RT_G3D_ANIMATION3D_CLASS_ID);
    return a && a->looping ? 1 : 0;
}

/// @brief Total length of the clip in seconds (0.0 for NULL).
/// @param[in] obj Animation3D to inspect.
/// @return Positive finite duration, or `0.0`.
double rt_animation3d_get_duration(void *obj) {
    rt_animation3d *a = (rt_animation3d *)rt_g3d_checked_or_null(obj, RT_G3D_ANIMATION3D_CLASS_ID);
    return (a && isfinite(a->duration) && a->duration > 0.0f) ? a->duration : 0.0;
}

/// @brief The clip's display / lookup name (empty string for NULL).
/// @param[in] obj Animation3D to inspect.
/// @return Owned runtime-string copy of the inline name, or an empty string.
rt_string rt_animation3d_get_name(void *obj) {
    rt_animation3d *a = (rt_animation3d *)rt_g3d_checked_or_null(obj, RT_G3D_ANIMATION3D_CLASS_ID);
    return a ? rt_const_cstr(a->name) : rt_const_cstr("");
}

/// @brief Infer a canonical humanoid-joint id from a bone name so a clip can retarget across
///   skeletons that use different naming conventions (mixamo, Unreal, Blender). @details Lowercases
///   the name, drops separators and a `mixamorig` prefix, detects side (left/right, incl. a
///   trailing l/r on limbs), and matches a base joint by keyword. Returns 0 for unrecognized names
///   (caller falls back to exact-name then index matching). Bare `leg`/`arm` follow mixamo, where
///   `Leg` is the calf and `Arm` the upper arm (`UpLeg`/`ForeArm` disambiguate the others).
/// @param[in] raw Borrowed NUL-terminated bone name.
/// @return Zero for no recognized role; otherwise an encoded base-role and
///         left/right discriminator.
static int animation3d_humanoid_role(const char *raw) {
    char b[64];
    int n = 0;
    const char *s;
    int side = 0; /* 0 center, 1 left, 2 right */
    int base = 0;
    if (!raw || !raw[0])
        return 0;
    for (const char *p = raw; *p && n < 63; ++p) {
        char c = *p;
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (c == '_' || c == ' ' || c == '.' || c == '-' || c == ':')
            continue;
        b[n++] = c;
    }
    b[n] = '\0';
    s = b;
    if (strncmp(s, "mixamorig", 9) == 0)
        s += 9;
    if (strstr(s, "left"))
        side = 1;
    else if (strstr(s, "right"))
        side = 2;
    if (strstr(s, "hip") || strstr(s, "pelvis"))
        base = 1;
    else if (strstr(s, "upperchest"))
        base = 4;
    else if (strstr(s, "chest"))
        base = 3;
    else if (strstr(s, "spine"))
        base = 2;
    else if (strstr(s, "neck"))
        base = 5;
    else if (strstr(s, "head"))
        base = 6;
    else if (strstr(s, "shoulder") || strstr(s, "clavicle"))
        base = 7;
    else if (strstr(s, "forearm") || strstr(s, "lowerarm"))
        base = 9;
    else if (strstr(s, "upperarm") || strstr(s, "uparm") || strstr(s, "arm"))
        base = 8;
    else if (strstr(s, "hand"))
        base = 10;
    else if (strstr(s, "upperleg") || strstr(s, "upleg") || strstr(s, "thigh"))
        base = 11;
    else if (strstr(s, "lowerleg") || strstr(s, "calf") || strstr(s, "shin") || strstr(s, "leg"))
        base = 12;
    else if (strstr(s, "foot") || strstr(s, "ankle"))
        base = 13;
    else if (strstr(s, "toe") || strstr(s, "ball"))
        base = 14;
    if (base == 0)
        return 0;
    if (side == 0 && base >= 7) { /* limb without an explicit side word: trailing l/r */
        char last = n > 0 ? b[n - 1] : '\0';
        if (last == 'l')
            side = 1;
        else if (last == 'r')
            side = 2;
    }
    return base * 4 + side;
}

/// @brief Build a per-bone cache of inferred humanoid role identifiers.
/// @param[in] skel Skeleton whose complete safe bone range to classify.
/// @return Owned `bone_count`-element integer array, or `NULL` for an empty
///         skeleton, size overflow, or allocation failure.
static int32_t *animation3d_build_humanoid_role_cache(const rt_skeleton3d *skel) {
    int32_t *roles;
    int32_t bone_count = skeleton3d_safe_bone_count(skel);
    if (bone_count <= 0)
        return NULL;
    if ((size_t)bone_count > SIZE_MAX / sizeof(*roles))
        return NULL;
    roles = (int32_t *)malloc((size_t)bone_count * sizeof(*roles));
    if (!roles)
        return NULL;
    for (int32_t i = 0; i < bone_count; ++i)
        roles[i] = animation3d_humanoid_role(
            skel->bones[i].name ? rt_string_cstr(skel->bones[i].name) : "");
    return roles;
}

/// @brief Map a source-skeleton bone to the best destination bone for retargeting:
///   explicit alias mapping first (`Skeleton3D.SetBoneAlias`), then exact name match,
///   then humanoid-role mapping (which handles cross-convention skeletons).
/// @details A source-side alias reversal is attempted after exact name and
///          before role inference; if all semantic mappings fail, the same
///          numeric index is used when it exists in the destination.
/// @param[in] src Source skeleton.
/// @param[in] dst Destination skeleton.
/// @param[in] src_roles Optional source role cache.
/// @param[in] dst_roles Optional destination role cache.
/// @param[in] src_bone Source bone index to map.
/// @return Destination bone index, or -1 if @p src_bone is invalid or has no match.
static int32_t animation3d_retarget_find_bone(const rt_skeleton3d *src,
                                              const rt_skeleton3d *dst,
                                              const int32_t *src_roles,
                                              const int32_t *dst_roles,
                                              int32_t src_bone) {
    int32_t src_count = skeleton3d_safe_bone_count(src);
    int32_t dst_count = skeleton3d_safe_bone_count(dst);
    if (!src || !dst || src_bone < 0 || src_bone >= src_count)
        return -1;
    rt_string name_string = src->bones[src_bone].name;
    const char *name = name_string ? rt_string_cstr(name_string) : "";
    int role;
    if (name && name[0] != '\0') {
        /* Destination-side alias: "a rig calling this bone <name> means my
         * bone <alias.local>" — the deterministic override for arbitrary
         * downloaded rigs. */
        const char *aliased = skeleton3d_alias_lookup(dst, name);
        if (aliased && aliased[0] != '\0') {
            for (int32_t i = 0; i < dst_count; ++i)
                if (skeleton3d_name_equals_cstr(dst->bones[i].name, aliased))
                    return i;
        }
        for (int32_t i = 0; i < dst_count; ++i)
            if (skeleton3d_names_equal(dst->bones[i].name, name_string))
                return i;
        /* Source-side alias reversal: if the source skeleton declares that a
         * canonical external name maps to this bone, try that canonical name
         * against the destination (exact, then its aliases). */
        for (int32_t a = 0; a < src->alias_count; ++a) {
            if (strcmp(src->aliases[a].local, name) != 0)
                continue;
            const char *canonical = src->aliases[a].external;
            const char *dst_local = skeleton3d_alias_lookup(dst, canonical);
            const char *want = dst_local && dst_local[0] != '\0' ? dst_local : canonical;
            for (int32_t i = 0; i < dst_count; ++i)
                if (skeleton3d_name_equals_cstr(dst->bones[i].name, want))
                    return i;
        }
    }
    /* No exact-name match: try humanoid role mapping (handles cross-convention skeletons). */
    role = src_roles ? src_roles[src_bone] : animation3d_humanoid_role(name);
    if (role != 0) {
        for (int32_t i = 0; i < dst_count; ++i)
            if ((dst_roles
                     ? dst_roles[i]
                     : animation3d_humanoid_role(
                           dst->bones[i].name ? rt_string_cstr(dst->bones[i].name) : "")) == role)
                return i;
    }
    return src_bone < dst_count ? src_bone : -1;
}

/// @brief Whether @p anim already has an animation channel targeting @p bone.
/// @details Guards retargeting against two source bones that map to the same destination
///          bone both emitting channels — the first mapping wins, avoiding duplicates.
/// @param[in] anim Animation3D whose safe channel range to scan.
/// @param[in] bone Destination bone index.
/// @return Nonzero when a matching channel already exists.
static int animation3d_has_channel_for_bone(const rt_animation3d *anim, int32_t bone) {
    int32_t channel_count;
    if (!anim)
        return 0;
    channel_count = animation3d_safe_channel_count(anim);
    for (int32_t i = 0; i < channel_count; ++i)
        if (anim->channels[i].bone_index == bone)
            return 1;
    return 0;
}

/// @brief Bone length = magnitude of the bind-pose local translation (offset
///   from its parent). Used to scale retargeted translations between skeletons
///   of different proportions. Row-major translation lives at m[3]/m[7]/m[11].
/// @param[in] skel Skeleton providing parent-relative bind transforms.
/// @param[in] bone Zero-based bone index.
/// @return Finite translation-vector length, or zero.
static float animation3d_bone_bind_length(const rt_skeleton3d *skel, int32_t bone) {
    const float *m;
    if (!skel || bone < 0 || bone >= skeleton3d_safe_bone_count(skel))
        return 0.0f;
    m = skel->bones[bone].bind_pose_local;
    {
        float len = sqrtf(m[3] * m[3] + m[7] * m[7] + m[11] * m[11]);
        return isfinite(len) ? len : 0.0f;
    }
}

/// @brief Copy an animation channel onto @p dst_bone, scaling its translation keys by @p pos_scale.
/// @details Duplicates the source keyframes; translation keys are multiplied by the bone-length
/// ratio
///          (rotations/scales transfer unchanged) so motion fits a differently-proportioned
///          skeleton.
/// @param[in,out] dst Destination Animation3D with a preallocated free channel
///                    slot.
/// @param[in] src Source channel and keyframe array.
/// @param[in] dst_bone Destination skeleton bone index.
/// @param[in] pos_scale Positive translation scale; invalid/nonpositive values
///                      become one.
/// @return 1 on success, 0 if the channel table is full or a keyframe copy fails.
static int animation3d_retarget_copy_channel(rt_animation3d *dst,
                                             const vgfx3d_anim_channel_t *src,
                                             int32_t dst_bone,
                                             float pos_scale) {
    static const float fallback_pos[3] = {0.0f, 0.0f, 0.0f};
    static const float fallback_rot[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    static const float fallback_scl[3] = {1.0f, 1.0f, 1.0f};
    int32_t keyframe_count = animation3d_safe_keyframe_count(src);
    if (!dst || !src)
        return 0;
    dst->channel_count = animation3d_safe_channel_count(dst);
    if (dst->channel_count >= dst->channel_capacity)
        return 0;
    if (keyframe_count > 0 && !src->keyframes)
        return 0;
    if (!isfinite(pos_scale) || pos_scale <= 0.0f)
        pos_scale = 1.0f;
    vgfx3d_anim_channel_t *out = &dst->channels[dst->channel_count++];
    memset(out, 0, sizeof(*out));
    out->bone_index = dst_bone;
    out->keyframe_count = keyframe_count;
    out->keyframe_capacity = keyframe_count;
    if (keyframe_count == 0)
        return 1;
    out->keyframes =
        (vgfx3d_keyframe_t *)malloc((size_t)keyframe_count * sizeof(vgfx3d_keyframe_t));
    if (!out->keyframes) {
        out->keyframe_count = 0;
        out->keyframe_capacity = 0;
        return 0;
    }
    memcpy(out->keyframes, src->keyframes, (size_t)keyframe_count * sizeof(*out->keyframes));
    /* Proportional retarget: scale translation keys by the bone-length ratio so a
     * clip authored on one skeleton fits a differently-proportioned one.
     * Rotations and scales transfer unchanged. */
    for (int32_t k = 0; k < out->keyframe_count; ++k) {
        if (!isfinite(out->keyframes[k].time) || out->keyframes[k].time < 0.0)
            out->keyframes[k].time = 0.0;
        if (pos_scale != 1.0f) {
            out->keyframes[k].position[0] =
                skeleton3d_clamp_to_float((double)out->keyframes[k].position[0] * pos_scale, 0.0f);
            out->keyframes[k].position[1] =
                skeleton3d_clamp_to_float((double)out->keyframes[k].position[1] * pos_scale, 0.0f);
            out->keyframes[k].position[2] =
                skeleton3d_clamp_to_float((double)out->keyframes[k].position[2] * pos_scale, 0.0f);
        }
        animation3d_sanitize_trs(out->keyframes[k].position,
                                 out->keyframes[k].rotation,
                                 out->keyframes[k].scale_xyz,
                                 fallback_pos,
                                 fallback_rot,
                                 fallback_scl);
    }
    return 1;
}

/// @brief Retarget an animation clip from a source skeleton onto a destination skeleton.
/// @details Builds a new clip preserving duration/looping, then for each source channel
///          maps its bone to the destination (by humanoid role or index), scales translation
///          keys by the bone-length ratio so the motion fits differing proportions, and
///          carries rotations/scales over unchanged. Bones with no mapping are skipped.
/// @param[in] animation Source Animation3D.
/// @param[in] src_skeleton Source Skeleton3D interpreting channel indexes.
/// @param[in] dst_skeleton Destination Skeleton3D.
/// @return New Animation3D handle, or NULL on invalid input or allocation failure.
void *rt_animation3d_retarget(void *animation, void *src_skeleton, void *dst_skeleton) {
    rt_animation3d *src_anim = animation3d_checked(animation);
    rt_skeleton3d *src_skel =
        (rt_skeleton3d *)rt_g3d_checked_or_null(src_skeleton, RT_G3D_SKELETON3D_CLASS_ID);
    rt_skeleton3d *dst_skel =
        (rt_skeleton3d *)rt_g3d_checked_or_null(dst_skeleton, RT_G3D_SKELETON3D_CLASS_ID);
    void *out_obj;
    rt_animation3d *out;
    int32_t *src_roles = NULL;
    int32_t *dst_roles = NULL;
    int32_t src_channel_count;
    if (!src_anim || !src_skel || !dst_skel)
        return NULL;
    {
        rt_string name = rt_const_cstr(src_anim->name);
        out_obj = rt_animation3d_new(name, src_anim->duration);
        rt_string_unref(name);
    }
    out = animation3d_checked(out_obj);
    if (!out)
        return NULL;
    out->looping = src_anim->looping ? 1 : 0;
    src_channel_count = animation3d_safe_channel_count(src_anim);
    if (src_channel_count <= 0)
        return out_obj;
    if ((size_t)src_channel_count > SIZE_MAX / sizeof(vgfx3d_anim_channel_t)) {
        animation3d_release_ref(&out_obj);
        return NULL;
    }
    out->channels =
        (vgfx3d_anim_channel_t *)calloc((size_t)src_channel_count, sizeof(*out->channels));
    if (!out->channels) {
        animation3d_release_ref(&out_obj);
        return NULL;
    }
    out->channel_capacity = src_channel_count;
    src_roles = animation3d_build_humanoid_role_cache(src_skel);
    dst_roles = animation3d_build_humanoid_role_cache(dst_skel);
    for (int32_t i = 0; i < src_channel_count; ++i) {
        const vgfx3d_anim_channel_t *src_ch = &src_anim->channels[i];
        int32_t dst_bone = animation3d_retarget_find_bone(
            src_skel, dst_skel, src_roles, dst_roles, src_ch->bone_index);
        float src_len, dst_len, pos_scale;
        if (dst_bone < 0 || animation3d_has_channel_for_bone(out, dst_bone))
            continue;
        src_len = animation3d_bone_bind_length(src_skel, src_ch->bone_index);
        dst_len = animation3d_bone_bind_length(dst_skel, dst_bone);
        pos_scale = (src_len > 1e-6f && dst_len > 1e-6f) ? (dst_len / src_len) : 1.0f;
        if (!animation3d_retarget_copy_channel(out, src_ch, dst_bone, pos_scale)) {
            free(src_roles);
            free(dst_roles);
            animation3d_release_ref(&out_obj);
            return NULL;
        }
    }
    free(src_roles);
    free(dst_roles);
    return out_obj;
}

/*==========================================================================
 * Keyframe sampling
 *=========================================================================*/

/// @brief Resolve a keyframe's optional components against bind-pose fallback TRS.
/// @details Position and scale are resolved lane by lane from presence masks;
///          rotation is used only when all four lanes are present. The
///          resulting TRS is sanitized and its quaternion normalized.
/// @param[in] key Keyframe to resolve.
/// @param[in] fallback_pos Borrowed three-float bind translation.
/// @param[in] fallback_rot Borrowed four-float bind quaternion.
/// @param[in] fallback_scl Borrowed three-float bind scale.
/// @param[out] out_pos Writable resolved translation.
/// @param[out] out_rot Writable resolved quaternion.
/// @param[out] out_scl Writable resolved scale.
static void keyframe_effective_trs(const vgfx3d_keyframe_t *key,
                                   const float *fallback_pos,
                                   const float *fallback_rot,
                                   const float *fallback_scl,
                                   float *out_pos,
                                   float *out_rot,
                                   float *out_scl) {
    if (!key || !fallback_pos || !fallback_rot || !fallback_scl)
        return;
    for (int32_t i = 0; i < 3; i++) {
        uint8_t bit = (uint8_t)(1u << i);
        out_pos[i] = (key->position_mask & bit) ? key->position[i] : fallback_pos[i];
        out_scl[i] = (key->scale_mask & bit) ? key->scale_xyz[i] : fallback_scl[i];
    }
    if ((key->rotation_mask & 0x0Fu) == 0x0Fu) {
        memcpy(out_rot, key->rotation, 4 * sizeof(float));
    } else {
        memcpy(out_rot, fallback_rot, 4 * sizeof(float));
    }
    animation3d_sanitize_trs(out_pos, out_rot, out_scl, fallback_pos, fallback_rot, fallback_scl);
}

/// @brief Sample a channel at time t, returning separate TRS components.
/// @details Missing/empty channels return fallback TRS, one-key channels
///          return that key, and out-of-range time clamps to the endpoint.
///          Interior keys are found by binary search. Position and scale use
///          Hermite only when both bracketing tangent masks agree, otherwise
///          lerp; rotation uses componentwise normalized Hermite only within
///          one hemisphere, otherwise shortest-arc SLERP.
/// @param[in] ch Optional channel with time-sorted keyframes.
/// @param[in] t Sample time; non-finite input becomes zero.
/// @param[in] fallback_pos Borrowed three-float bind translation.
/// @param[in] fallback_rot Borrowed four-float bind quaternion.
/// @param[in] fallback_scl Borrowed three-float bind scale.
/// @param[out] out_pos Writable sampled translation.
/// @param[out] out_rot Writable sampled quaternion.
/// @param[out] out_scl Writable sampled scale.
static void sample_channel_trs_with_fallback(const vgfx3d_anim_channel_t *ch,
                                             float t,
                                             const float *fallback_pos,
                                             const float *fallback_rot,
                                             const float *fallback_scl,
                                             float *out_pos,
                                             float *out_rot,
                                             float *out_scl) {
    int32_t keyframe_count = animation3d_safe_keyframe_count(ch);
    if (!isfinite(t))
        t = 0.0f;
    if (!ch || keyframe_count <= 0 || !ch->keyframes) {
        memcpy(out_pos, fallback_pos, 3 * sizeof(float));
        memcpy(out_rot, fallback_rot, 4 * sizeof(float));
        memcpy(out_scl, fallback_scl, 3 * sizeof(float));
        animation3d_sanitize_trs(
            out_pos, out_rot, out_scl, fallback_pos, fallback_rot, fallback_scl);
        return;
    }
    if (keyframe_count == 1) {
        keyframe_effective_trs(
            &ch->keyframes[0], fallback_pos, fallback_rot, fallback_scl, out_pos, out_rot, out_scl);
        return;
    }

    if (t <= ch->keyframes[0].time) {
        keyframe_effective_trs(
            &ch->keyframes[0], fallback_pos, fallback_rot, fallback_scl, out_pos, out_rot, out_scl);
        return;
    }
    if (t >= ch->keyframes[keyframe_count - 1].time) {
        keyframe_effective_trs(&ch->keyframes[keyframe_count - 1],
                               fallback_pos,
                               fallback_rot,
                               fallback_scl,
                               out_pos,
                               out_rot,
                               out_scl);
        return;
    }

    /* Find bracketing keyframes */
    int k0 = 0, k1 = keyframe_count - 1;
    while (k1 - k0 > 1) {
        int mid = k0 + (k1 - k0) / 2;
        if (ch->keyframes[mid].time <= t)
            k0 = mid;
        else
            k1 = mid;
    }

    double t0 = ch->keyframes[k0].time;
    double t1 = ch->keyframes[k1].time;
    float alpha = skeleton3d_clamp01f((float)((t1 > t0) ? ((double)t - t0) / (t1 - t0) : 0.0));

    const vgfx3d_keyframe_t *key0 = &ch->keyframes[k0];
    const vgfx3d_keyframe_t *key1 = &ch->keyframes[k1];
    float pos0[3], rot0[4], scl0[3];
    float pos1[3], rot1[4], scl1[3];
    keyframe_effective_trs(key0, fallback_pos, fallback_rot, fallback_scl, pos0, rot0, scl0);
    keyframe_effective_trs(key1, fallback_pos, fallback_rot, fallback_scl, pos1, rot1, scl1);

    {
        /* glTF CUBICSPLINE Hermite basis; a channel plays cubic only when both
         * bracketing keys carry its tangents. Tangents are stored in value units
         * per second, so they scale by the segment duration. */
        float dt = (float)(t1 - t0);
        float a2 = alpha * alpha;
        float a3 = a2 * alpha;
        float h00 = 2.0f * a3 - 3.0f * a2 + 1.0f;
        float h10 = a3 - 2.0f * a2 + alpha;
        float h01 = -2.0f * a3 + 3.0f * a2;
        float h11 = a3 - a2;
        uint8_t both_cubic = (uint8_t)(key0->cubic_mask & key1->cubic_mask);
        int pos_cubic = (both_cubic & 1u) && dt > 0.0f;
        int rot_cubic = (both_cubic & 2u) && dt > 0.0f;
        int scl_cubic = (both_cubic & 4u) && dt > 0.0f;
        for (int i = 0; i < 3; i++) {
            if (pos_cubic) {
                out_pos[i] = h00 * pos0[i] + h10 * dt * key0->pos_out_tangent[i] + h01 * pos1[i] +
                             h11 * dt * key1->pos_in_tangent[i];
            } else {
                out_pos[i] = pos0[i] + alpha * (pos1[i] - pos0[i]);
            }
            if (scl_cubic) {
                out_scl[i] = h00 * scl0[i] + h10 * dt * key0->scale_out_tangent[i] + h01 * scl1[i] +
                             h11 * dt * key1->scale_in_tangent[i];
            } else {
                out_scl[i] = scl0[i] + alpha * (scl1[i] - scl0[i]);
            }
        }
        if (rot_cubic && (rot0[0] * rot1[0] + rot0[1] * rot1[1] + rot0[2] * rot1[2] +
                          rot0[3] * rot1[3]) >= 0.0f) {
            /* Componentwise Hermite + renormalize (glTF spec); hemisphere flips
             * between stored keys fall back to slerp below. */
            float sum = 0.0f;
            for (int i = 0; i < 4; i++) {
                out_rot[i] = h00 * rot0[i] + h10 * dt * key0->rot_out_tangent[i] + h01 * rot1[i] +
                             h11 * dt * key1->rot_in_tangent[i];
                sum += out_rot[i] * out_rot[i];
            }
            if (sum > 1e-12f) {
                float inv = 1.0f / sqrtf(sum);
                for (int i = 0; i < 4; i++)
                    out_rot[i] *= inv;
            } else {
                quat_slerp_float(rot0, rot1, alpha, out_rot);
            }
        } else {
            quat_slerp_float(rot0, rot1, alpha, out_rot);
        }
    }
    animation3d_sanitize_trs(out_pos, out_rot, out_scl, fallback_pos, fallback_rot, fallback_scl);
}

/// @brief Sample a channel at time t, producing a local TRS matrix; the bone's
///   pre-decomposed bind TRS is the fallback (no per-sample matrix decomposition).
/// @param[in] ch Optional resolved animation channel.
/// @param[in] t Sample time in seconds.
/// @param[in] bone Bone providing cached bind TRS.
/// @param[out] out_local Writable row-major local matrix of 16 floats.
static void sample_channel_with_fallback(const vgfx3d_anim_channel_t *ch,
                                         float t,
                                         const vgfx3d_bone_t *bone,
                                         float *out_local) {
    float pos[3], rot[4], scl[3];
    sample_channel_trs_with_fallback(
        ch, t, bone->bind_pos, bone->bind_rot, bone->bind_scl, pos, rot, scl);
    build_trs_float(pos, rot, scl, out_local);
}

/// @brief Sample an already-resolved (possibly NULL) channel's TRS at @p time, falling
///   back to the bone's cached bind TRS when the channel is absent. Lets callers
///   that pre-resolved the channel skip both the per-bone O(channels) lookup and
///   the per-sample bind-matrix decomposition.
/// @param[in] channel Optional resolved animation channel.
/// @param[in] time Sample time in seconds.
/// @param[in] bone Bone providing cached bind TRS.
/// @param[out] out_pos Writable sampled translation.
/// @param[out] out_rot Writable sampled quaternion.
/// @param[out] out_scl Writable sampled scale.
static void sample_animation_channel_trs(const vgfx3d_anim_channel_t *channel,
                                         float time,
                                         const vgfx3d_bone_t *bone,
                                         float *out_pos,
                                         float *out_rot,
                                         float *out_scl) {
    sample_channel_trs_with_fallback(
        channel, time, bone->bind_pos, bone->bind_rot, bone->bind_scl, out_pos, out_rot, out_scl);
}
