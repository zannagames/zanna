//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/anim/rt_skeleton3d_animation.inc
// Purpose: Animation3D clip storage, keyframe sampling, compression, and retargeting.
// Key invariants:
//   - Published channel/key counts never exceed their authoritative owned capacities.
//   - Sparse TRS masks and finite normalized quaternion lanes govern sampling fallbacks.
// Ownership/Lifetime:
//   - Included by rt_skeleton3d.c; this is not a standalone translation unit.
//   - Animation clips own channel/key arrays and retain no skeleton during retargeting.
// Links: rt_skeleton3d.c, rt_skeleton3d.h, rt_skeleton3d_internal.h
//
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
    rt_animation3d *a = (rt_animation3d *)obj;
    int32_t channel_count;
    if (!a)
        return;
    animation3d_repair_storage(a);
    channel_count = a->initialized_channel_count;
    if (channel_count < 0)
        channel_count = 0;
    if (channel_count > a->owned_channel_capacity)
        channel_count = a->owned_channel_capacity;
    for (int32_t i = 0; i < channel_count; i++) {
        free(a->owned_channels[i].owned_keyframes);
        a->owned_channels[i].owned_keyframes = NULL;
        a->owned_channels[i].keyframes = NULL;
        a->owned_channels[i].owned_keyframe_capacity = 0;
        a->owned_channels[i].keyframe_capacity = 0;
        a->owned_channels[i].initialized_keyframe_count = 0;
        a->owned_channels[i].keyframe_count = 0;
    }
    free(a->owned_channels);
    a->owned_channels = NULL;
    a->owned_channel_capacity = 0;
    a->initialized_channel_count = 0;
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

/// @brief Retain-then-release a stable owner and republish its mutable mirror.
/// @param[in,out] owner Stable retained reference slot.
/// @param[in,out] mirror Legacy/publicly inspected mirror of @p owner.
/// @param[in] value Borrowed replacement object, or `NULL`.
/// @param[in] class_id Expected class of the retained object.
static void animation3d_assign_owned_class_ref(void **owner,
                                               void **mirror,
                                               void *value,
                                               int64_t class_id) {
    if (!owner || !mirror)
        return;
    if (*owner != value) {
        rt_obj_retain_maybe(value);
        animation3d_release_class_ref(owner, class_id);
        *owner = value;
    }
    *mirror = *owner;
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
    skeleton3d_repair_storage((rt_skeleton3d *)*slot);
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
    animation3d_repair_storage((rt_animation3d *)*slot);
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
    quat_normalize_float(q);
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
    if (name && rt_string_is_handle(name)) {
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
    a->owned_channels = NULL;
    a->owned_channel_capacity = 0;
    a->initialized_channel_count = 0;
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
    rt_animation3d *a = animation3d_checked(obj);
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
        vgfx3d_keyframe_t *initial_keys;
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
            int32_t old_cap = a->owned_channel_capacity;
            vgfx3d_anim_channel_t *nc = (vgfx3d_anim_channel_t *)realloc(
                a->owned_channels, (size_t)new_cap * sizeof(vgfx3d_anim_channel_t));
            if (!nc)
                return 0;
            if (new_cap > old_cap)
                memset(nc + old_cap, 0, (size_t)(new_cap - old_cap) * sizeof(*nc));
            a->owned_channels = nc;
            a->owned_channel_capacity = new_cap;
            a->channels = nc;
            a->channel_capacity = new_cap;
        }
        initial_keys = (vgfx3d_keyframe_t *)calloc(16u, sizeof(*initial_keys));
        if (!initial_keys)
            return 0;
        ch = &a->channels[a->channel_count];
        memset(ch, 0, sizeof(vgfx3d_anim_channel_t));
        ch->bone_index = (int32_t)bone_index;
        ch->keyframes = initial_keys;
        ch->keyframe_capacity = 16;
        ch->owned_keyframes = initial_keys;
        ch->owned_keyframe_capacity = 16;
        ch->initialized_keyframe_count = 0;
        a->channel_count++;
        a->initialized_channel_count = a->channel_count;
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
        int32_t old_cap = ch->owned_keyframe_capacity;
        vgfx3d_keyframe_t *nk = (vgfx3d_keyframe_t *)realloc(
            ch->owned_keyframes, (size_t)new_cap * sizeof(vgfx3d_keyframe_t));
        if (!nk)
            return 0;
        if (new_cap > old_cap)
            memset(nk + old_cap, 0, (size_t)(new_cap - old_cap) * sizeof(*nk));
        ch->owned_keyframes = nk;
        ch->owned_keyframe_capacity = new_cap;
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
    ch->initialized_keyframe_count = ch->keyframe_count;
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
    if (!a || !mid || !b || !isfinite(a->time) || !isfinite(mid->time) || !isfinite(b->time) ||
        !isfinite(pos_tolerance) || pos_tolerance < 0.0f || !isfinite(rot_tolerance) ||
        rot_tolerance < 0.0f || !isfinite(scl_tolerance) || scl_tolerance < 0.0f)
        return 0;
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
        uint8_t bit = (uint8_t)(1u << i);
        if (a->position_mask & bit) {
            if (!isfinite(a->position[i]) || !isfinite(mid->position[i]) ||
                !isfinite(b->position[i]))
                return 0;
            float lp = a->position[i] + alpha * (b->position[i] - a->position[i]);
            if (!isfinite(lp) || fabsf(lp - mid->position[i]) > pos_tolerance)
                return 0;
        }
        if (a->scale_mask & bit) {
            if (!isfinite(a->scale_xyz[i]) || !isfinite(mid->scale_xyz[i]) ||
                !isfinite(b->scale_xyz[i]))
                return 0;
            float ls = a->scale_xyz[i] + alpha * (b->scale_xyz[i] - a->scale_xyz[i]);
            if (!isfinite(ls) || fabsf(ls - mid->scale_xyz[i]) > scl_tolerance)
                return 0;
        }
    }
    if ((a->rotation_mask & 0x0Fu) == 0x0Fu) {
        float blended[4];
        float dot;
        for (int i = 0; i < 4; ++i)
            if (!isfinite(a->rotation[i]) || !isfinite(mid->rotation[i]) ||
                !isfinite(b->rotation[i]))
                return 0;
        quat_slerp_float(a->rotation, b->rotation, alpha, blended);
        dot = blended[0] * mid->rotation[0] + blended[1] * mid->rotation[1] +
              blended[2] * mid->rotation[2] + blended[3] * mid->rotation[3];
        if (!isfinite(dot) || fabsf(dot) < 1.0f - rot_tolerance)
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
    rt_animation3d *a = animation3d_checked(obj);
    int64_t dropped = 0;
    float pos_tol = skeleton3d_clamp_nonnegative_float(pos_tolerance, 0.0f);
    float rot_tol = skeleton3d_clamp_nonnegative_float(rot_tolerance, 0.0f);
    float scl_tol = skeleton3d_clamp_nonnegative_float(scl_tolerance, 0.0f);
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
        ch->initialized_keyframe_count = kept;
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
    rt_animation3d *a = animation3d_checked(obj);
    vgfx3d_anim_channel_t *ch = NULL;
    vgfx3d_keyframe_t *kf = NULL;
    if (!a || bone_index < 0 || bone_index >= VGFX3D_MAX_SKELETON_BONES || !isfinite(time))
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
    if (pos_in && pos_out && (kf->position_mask & 0x07u) == 0x07u) {
        for (int i = 0; i < 3; ++i) {
            kf->pos_in_tangent[i] = animation3d_sanitize_lane(pos_in[i], 0.0f);
            kf->pos_out_tangent[i] = animation3d_sanitize_lane(pos_out[i], 0.0f);
        }
        kf->cubic_mask |= 1u;
    }
    if (rot_in && rot_out && (kf->rotation_mask & 0x0Fu) == 0x0Fu) {
        for (int i = 0; i < 4; ++i) {
            kf->rot_in_tangent[i] = animation3d_sanitize_lane(rot_in[i], 0.0f);
            kf->rot_out_tangent[i] = animation3d_sanitize_lane(rot_out[i], 0.0f);
        }
        kf->cubic_mask |= 2u;
    }
    if (scl_in && scl_out && (kf->scale_mask & 0x07u) == 0x07u) {
        for (int i = 0; i < 3; ++i) {
            kf->scale_in_tangent[i] = animation3d_sanitize_lane(scl_in[i], 0.0f);
            kf->scale_out_tangent[i] = animation3d_sanitize_lane(scl_out[i], 0.0f);
        }
        kf->cubic_mask |= 4u;
    }
}

/// @brief Mark this clip as looping (wraps back to t=0 at end) or one-shot (stops at end).
/// @param[in,out] obj Animation3D to configure.
/// @param[in] loop Zero for one-shot; nonzero for looping.
void rt_animation3d_set_looping(void *obj, int8_t loop) {
    rt_animation3d *a = animation3d_checked(obj);
    if (a)
        a->looping = loop ? 1 : 0;
}

/// @brief Read the looping flag (0 = one-shot, 1 = loops).
/// @param[in] obj Animation3D to inspect.
/// @return `1` for looping; `0` for one-shot or invalid input.
int8_t rt_animation3d_get_looping(void *obj) {
    rt_animation3d *a = animation3d_checked(obj);
    return a && a->looping ? 1 : 0;
}

/// @brief Total length of the clip in seconds (0.0 for NULL).
/// @param[in] obj Animation3D to inspect.
/// @return Positive finite duration, or `0.0`.
double rt_animation3d_get_duration(void *obj) {
    rt_animation3d *a = animation3d_checked(obj);
    if (!a)
        return 0.0;
    if (!isfinite(a->duration) || a->duration <= 0.0f)
        a->duration = 1.0f;
    return a->duration;
}

/// @brief The clip's display / lookup name (empty string for NULL).
/// @param[in] obj Animation3D to inspect.
/// @return Owned runtime-string copy of the inline name, or an empty string.
rt_string rt_animation3d_get_name(void *obj) {
    rt_animation3d *a = animation3d_checked(obj);
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
    for (int32_t i = 0; i < bone_count; ++i) {
        rt_string name = skel->bones[i].name;
        roles[i] = animation3d_humanoid_role(
            name && rt_string_is_handle(name) ? rt_string_cstr(name) : "");
    }
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
    const char *name =
        name_string && rt_string_is_handle(name_string) ? rt_string_cstr(name_string) : "";
    int role;
    if (!name)
        name = "";
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
        for (int32_t a = 0, alias_count = skeleton3d_safe_alias_count(src); a < alias_count; ++a) {
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
            if ((dst_roles ? dst_roles[i]
                           : animation3d_humanoid_role(
                                 dst->bones[i].name && rt_string_is_handle(dst->bones[i].name)
                                     ? rt_string_cstr(dst->bones[i].name)
                                     : "")) == role)
                return i;
    }
    return src_bone < dst_count ? src_bone : -1;
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
        double len = hypot(hypot((double)m[3], (double)m[7]), (double)m[11]);
        return isfinite(len) && len <= FLT_MAX ? (float)len : 0.0f;
    }
}

/// @brief Multiply two unit quaternions (`out = a * b`), safe for aliased output.
static void animation3d_quat_mul(const float *a, const float *b, float *out) {
    float r[4];
    r[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    r[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    r[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    r[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    memcpy(out, r, sizeof(r));
}

/// @brief Conjugate (= inverse for unit quaternions), safe for aliased output.
static void animation3d_quat_conjugate(const float *q, float *out) {
    out[0] = -q[0];
    out[1] = -q[1];
    out[2] = -q[2];
    out[3] = q[3];
}

/// @brief Rotate a 3-vector by a unit quaternion, safe for aliased output.
static void animation3d_quat_rotate_vec3(const float *q, const float *v, float *out) {
    /* v' = v + 2*cross(q.xyz, cross(q.xyz, v) + q.w*v) */
    float t[3];
    float c[3];
    t[0] = q[1] * v[2] - q[2] * v[1] + q[3] * v[0];
    t[1] = q[2] * v[0] - q[0] * v[2] + q[3] * v[1];
    t[2] = q[0] * v[1] - q[1] * v[0] + q[3] * v[2];
    c[0] = q[1] * t[2] - q[2] * t[1];
    c[1] = q[2] * t[0] - q[0] * t[2];
    c[2] = q[0] * t[1] - q[1] * t[0];
    out[0] = v[0] + 2.0f * c[0];
    out[1] = v[1] + 2.0f * c[1];
    out[2] = v[2] + 2.0f * c[2];
}

/// @brief Extract a bone's parent-relative bind rotation (scale-normalized) and translation.
/// @details Out-of-range bones read as identity so root parents (-1) compose cleanly.
static void animation3d_bone_bind_local_trs_scaled(
    const rt_skeleton3d *skel, int32_t bone, float out_rot[4], float out_pos[3], float out_scl[3]);

static void animation3d_bone_bind_local_trs(const rt_skeleton3d *skel,
                                            int32_t bone,
                                            float out_rot[4],
                                            float out_pos[3]) {
    float scl[3];
    animation3d_bone_bind_local_trs_scaled(skel, bone, out_rot, out_pos, scl);
}

/// @brief Bind-local rotation/translation/scale of a bone; identity fallbacks out of range.
static void animation3d_bone_bind_local_trs_scaled(
    const rt_skeleton3d *skel, int32_t bone, float out_rot[4], float out_pos[3], float out_scl[3]) {
    out_rot[0] = out_rot[1] = out_rot[2] = 0.0f;
    out_rot[3] = 1.0f;
    out_pos[0] = out_pos[1] = out_pos[2] = 0.0f;
    out_scl[0] = out_scl[1] = out_scl[2] = 1.0f;
    if (!skel || bone < 0 || bone >= skeleton3d_safe_bone_count(skel))
        return;
    {
        const float *m = skel->bones[bone].bind_pose_local;
        float col[3][3];
        float len[3];
        out_pos[0] = m[3];
        out_pos[1] = m[7];
        out_pos[2] = m[11];
        for (int c = 0; c < 3; c++) {
            col[c][0] = m[c];
            col[c][1] = m[4 + c];
            col[c][2] = m[8 + c];
            len[c] = sqrtf(col[c][0] * col[c][0] + col[c][1] * col[c][1] + col[c][2] * col[c][2]);
            if (!(len[c] > 1e-9f) || !isfinite(len[c]))
                return; /* degenerate bind: keep identity rotation */
            col[c][0] /= len[c];
            col[c][1] /= len[c];
            col[c][2] /= len[c];
            out_scl[c] = len[c];
        }
        quat_from_matrix3_float(col[0][0],
                                col[1][0],
                                col[2][0],
                                col[0][1],
                                col[1][1],
                                col[2][1],
                                col[0][2],
                                col[1][2],
                                col[2][2],
                                out_rot);
        sanitize_keyframe_quat(out_rot);
    }
}

/// @brief Compose a bone chain's bind-global rotation (identity for bone < 0).
static void animation3d_bind_global_rotation(const rt_skeleton3d *skel,
                                             int32_t bone,
                                             float out_rot[4]) {
    int32_t chain[VGFX3D_MAX_SKELETON_BONES];
    int32_t depth = 0;
    float local_rot[4];
    float local_pos[3];
    out_rot[0] = out_rot[1] = out_rot[2] = 0.0f;
    out_rot[3] = 1.0f;
    if (!skel)
        return;
    for (int32_t walk = bone; walk >= 0 && depth < VGFX3D_MAX_SKELETON_BONES;
         walk = skeleton3d_valid_parent_index(skel, walk, skeleton3d_safe_bone_count(skel))) {
        chain[depth++] = walk;
    }
    for (int32_t i = depth - 1; i >= 0; i--) {
        animation3d_bone_bind_local_trs(skel, chain[i], local_rot, local_pos);
        animation3d_quat_mul(out_rot, local_rot, out_rot);
    }
    sanitize_keyframe_quat(out_rot);
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
static void sample_channel_trs_with_fallback(const vgfx3d_anim_channel_t *ch,
                                             float t,
                                             const float *fallback_pos,
                                             const float *fallback_rot,
                                             const float *fallback_scl,
                                             float *out_pos,
                                             float *out_rot,
                                             float *out_scl);

/// @brief Fold a child's local TRS under an accumulated parent TRS (`acc = acc * child`).
static void animation3d_trs_compose(float acc_pos[3],
                                    float acc_rot[4],
                                    float acc_scl[3],
                                    const float child_pos[3],
                                    const float child_rot[4],
                                    const float child_scl[3]) {
    float scaled[3];
    float rotated[3];
    scaled[0] = acc_scl[0] * child_pos[0];
    scaled[1] = acc_scl[1] * child_pos[1];
    scaled[2] = acc_scl[2] * child_pos[2];
    animation3d_quat_rotate_vec3(acc_rot, scaled, rotated);
    acc_pos[0] += rotated[0];
    acc_pos[1] += rotated[1];
    acc_pos[2] += rotated[2];
    animation3d_quat_mul(acc_rot, child_rot, acc_rot);
    acc_scl[0] *= child_scl[0];
    acc_scl[1] *= child_scl[1];
    acc_scl[2] *= child_scl[2];
}

/// @brief qsort comparator for key times.
static int animation3d_time_cmp(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    return da < db ? -1 : (da > db ? 1 : 0);
}

static int animation3d_vec3_normalize(float v[3]);

/// @brief Rotation aligning two reference directions onto two target directions.
/// @details Gram-Schmidt both pairs into right-handed bases and compose R = U * R_ref^T —
///          a full 3-DOF solve for bones with two mapped children (hips: spine + a thigh),
///          where a single aim axis would leave the twist unconstrained.
/// @return Nonzero on success; zero when either pair is degenerate (parallel/zero).
static int animation3d_quat_from_two_dirs(const float rest1[3],
                                          const float rest2[3],
                                          const float dir1[3],
                                          const float dir2[3],
                                          float out[4]) {
    float r1[3];
    float r2[3];
    float u1[3];
    float u2[3];
    memcpy(r1, rest1, sizeof(r1));
    memcpy(r2, rest2, sizeof(r2));
    memcpy(u1, dir1, sizeof(u1));
    memcpy(u2, dir2, sizeof(u2));
    if (!animation3d_vec3_normalize(r1) || !animation3d_vec3_normalize(u1))
        return 0;
    {
        float d = r2[0] * r1[0] + r2[1] * r1[1] + r2[2] * r1[2];
        r2[0] -= d * r1[0];
        r2[1] -= d * r1[1];
        r2[2] -= d * r1[2];
        d = u2[0] * u1[0] + u2[1] * u1[1] + u2[2] * u1[2];
        u2[0] -= d * u1[0];
        u2[1] -= d * u1[1];
        u2[2] -= d * u1[2];
    }
    if (!animation3d_vec3_normalize(r2) || !animation3d_vec3_normalize(u2))
        return 0;
    {
        float r3[3];
        float u3[3];
        float m[9];
        r3[0] = r1[1] * r2[2] - r1[2] * r2[1];
        r3[1] = r1[2] * r2[0] - r1[0] * r2[2];
        r3[2] = r1[0] * r2[1] - r1[1] * r2[0];
        u3[0] = u1[1] * u2[2] - u1[2] * u2[1];
        u3[1] = u1[2] * u2[0] - u1[0] * u2[2];
        u3[2] = u1[0] * u2[1] - u1[1] * u2[0];
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                m[i * 3 + j] = u1[i] * r1[j] + u2[i] * r2[j] + u3[i] * r3[j];
        quat_from_matrix3_float(m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], out);
        sanitize_keyframe_quat(out);
    }
    return 1;
}

/// @brief Shortest-arc unit quaternion rotating direction @p u onto @p v.
static void animation3d_quat_shortest_arc(const float u[3], const float v[3], float out[4]) {
    float d = u[0] * v[0] + u[1] * v[1] + u[2] * v[2];
    float c[3];
    c[0] = u[1] * v[2] - u[2] * v[1];
    c[1] = u[2] * v[0] - u[0] * v[2];
    c[2] = u[0] * v[1] - u[1] * v[0];
    if (d < -0.999999f) {
        /* Opposite directions: rotate half-turn about any perpendicular axis. */
        float axis[3] = {1.0f, 0.0f, 0.0f};
        if (fabsf(u[0]) > 0.9f) {
            axis[0] = 0.0f;
            axis[1] = 1.0f;
        }
        c[0] = u[1] * axis[2] - u[2] * axis[1];
        c[1] = u[2] * axis[0] - u[0] * axis[2];
        c[2] = u[0] * axis[1] - u[1] * axis[0];
        out[0] = c[0];
        out[1] = c[1];
        out[2] = c[2];
        out[3] = 0.0f;
    } else {
        out[0] = c[0];
        out[1] = c[1];
        out[2] = c[2];
        out[3] = 1.0f + d;
    }
    sanitize_keyframe_quat(out);
}

/// @brief Normalize a 3-vector in place; returns 0 for a degenerate length.
static int animation3d_vec3_normalize(float v[3]) {
    float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!(len > 1e-6f) || !isfinite(len))
        return 0;
    v[0] /= len;
    v[1] /= len;
    v[2] /= len;
    return 1;
}

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
    out->owned_channels = out->channels;
    out->owned_channel_capacity = src_channel_count;
    out->channel_capacity = src_channel_count;
    src_roles = animation3d_build_humanoid_role_cache(src_skel);
    dst_roles = animation3d_build_humanoid_role_cache(dst_skel);
    /* Pose-based retarget: per union key time, compose the full source pose and solve the
     * destination hierarchy parent-first. Interior mapped bones AIM their bone direction at
     * the source's world bone direction (bind-pose differences cancel exactly — an A-pose
     * mocap rig drives a T-pose auto-rig without inheriting the bind offset); leaf bones use
     * the constant rest-delta sandwich; translations use the rest-delta with bone-length
     * scaling. Multi-source parent chains (Bip01+Pelvis -> Hips) fold ancestor-first. */
    {
        int32_t n_src = skeleton3d_safe_bone_count(src_skel);
        int32_t n_dst = skeleton3d_safe_bone_count(dst_skel);
        int ok = n_src > 0 && n_dst > 0;
        const vgfx3d_anim_channel_t **src_channel_of = NULL;
        int32_t *dst_src = NULL;    /* driving TOP src bone per dst bone, or -1 */
        int32_t *dst_anchor = NULL; /* bottom-of-chain src bone (position anchor) */
        int32_t *dst_chain_len = NULL;
        int32_t (*dst_chain)[8] = NULL;
        int32_t *child_of = NULL;  /* first mapped dst child, or -1 */
        int32_t *child2_of = NULL; /* second mapped dst child for triad solves, or -1 */
        float (*a_rot)[4] = NULL;
        float (*f_rot)[4] = NULL;      /* inv(srcBindGlobalRot(anchor)) * dstBindGlobalRot */
        float (*bind_c_pos)[3] = NULL; /* composed source bind translation */
        float *pos_scale = NULL;
        double *times = NULL;
        int32_t time_count = 0;
        float (*sg_pos)[3] = NULL;
        float (*sg_rot)[4] = NULL;
        float (*sg_scl)[3] = NULL;
        float (*dg_pos)[3] = NULL;
        float (*dg_rot)[4] = NULL;
        vgfx3d_keyframe_t **keys_of = NULL;
        int32_t *src_order = NULL;
        int32_t *dst_order = NULL;
        if (ok) {
            src_channel_of =
                (const vgfx3d_anim_channel_t **)calloc((size_t)n_src, sizeof(*src_channel_of));
            dst_src = (int32_t *)malloc((size_t)n_dst * sizeof(*dst_src));
            dst_anchor = (int32_t *)malloc((size_t)n_dst * sizeof(*dst_anchor));
            dst_chain_len = (int32_t *)calloc((size_t)n_dst, sizeof(*dst_chain_len));
            dst_chain = (int32_t (*)[8])calloc((size_t)n_dst, sizeof(*dst_chain));
            child_of = (int32_t *)malloc((size_t)n_dst * sizeof(*child_of));
            child2_of = (int32_t *)malloc((size_t)n_dst * sizeof(*child2_of));
            a_rot = (float (*)[4])calloc((size_t)n_dst, sizeof(*a_rot));
            f_rot = (float (*)[4])calloc((size_t)n_dst, sizeof(*f_rot));
            bind_c_pos = (float (*)[3])calloc((size_t)n_dst, sizeof(*bind_c_pos));
            pos_scale = (float *)malloc((size_t)n_dst * sizeof(*pos_scale));
            sg_pos = (float (*)[3])malloc((size_t)n_src * sizeof(*sg_pos));
            sg_rot = (float (*)[4])malloc((size_t)n_src * sizeof(*sg_rot));
            sg_scl = (float (*)[3])malloc((size_t)n_src * sizeof(*sg_scl));
            dg_pos = (float (*)[3])malloc((size_t)n_dst * sizeof(*dg_pos));
            dg_rot = (float (*)[4])malloc((size_t)n_dst * sizeof(*dg_rot));
            keys_of = (vgfx3d_keyframe_t **)calloc((size_t)n_dst, sizeof(*keys_of));
            src_order = (int32_t *)malloc((size_t)n_src * sizeof(*src_order));
            dst_order = (int32_t *)malloc((size_t)n_dst * sizeof(*dst_order));
            ok = src_order && dst_order && src_channel_of && dst_src && dst_anchor &&
                 dst_chain_len && dst_chain && child_of && child2_of && a_rot && f_rot &&
                 bind_c_pos && pos_scale && sg_pos && sg_rot && sg_scl && dg_pos && dg_rot &&
                 keys_of;
        }
        if (ok) {
            for (int32_t d = 0; d < n_dst; ++d) {
                dst_src[d] = -1;
                dst_anchor[d] = -1;
                child_of[d] = -1;
                child2_of[d] = -1;
            }
            for (int32_t i = 0; i < src_channel_count; ++i) {
                int32_t b = src_anim->channels[i].bone_index;
                if (b >= 0 && b < n_src && !src_channel_of[b])
                    src_channel_of[b] = &src_anim->channels[i];
            }
            /* Claim destination bones: topmost chain member wins; descendants whose
             * channels map to the same destination extend the fold chain. */
            for (int32_t i = 0; i < src_channel_count; ++i) {
                int32_t sb = src_anim->channels[i].bone_index;
                int32_t d =
                    animation3d_retarget_find_bone(src_skel, dst_skel, src_roles, dst_roles, sb);
                if (d < 0 || d >= n_dst || dst_src[d] >= 0)
                    continue;
                int deferred = 0;
                for (int32_t walk = skeleton3d_valid_parent_index(src_skel, sb, n_src);
                     walk >= 0 && !deferred;
                     walk = skeleton3d_valid_parent_index(src_skel, walk, n_src)) {
                    if (src_channel_of[walk] &&
                        animation3d_retarget_find_bone(
                            src_skel, dst_skel, src_roles, dst_roles, walk) == d)
                        deferred = 1;
                }
                if (deferred)
                    continue;
                dst_src[d] = sb;
                dst_chain[d][0] = sb;
                dst_chain_len[d] = 1;
                int extended = 1;
                while (extended && dst_chain_len[d] < 8) {
                    extended = 0;
                    for (int32_t j = 0; j < src_channel_count && !extended; ++j) {
                        int32_t cb = src_anim->channels[j].bone_index;
                        if (skeleton3d_valid_parent_index(src_skel, cb, n_src) !=
                                dst_chain[d][dst_chain_len[d] - 1] ||
                            animation3d_retarget_find_bone(
                                src_skel, dst_skel, src_roles, dst_roles, cb) != d)
                            continue;
                        dst_chain[d][dst_chain_len[d]++] = cb;
                        extended = 1;
                    }
                }
                dst_anchor[d] = dst_chain[d][dst_chain_len[d] - 1];
            }
            /* Aim-pair selection: among each bone's mapped children pick the pair whose
             * bind directions are least parallel (max |cross|) — hips must pair a thigh
             * with the spine, not the two near-parallel thighs; the chest a clavicle
             * with the neck, not the two mirrored clavicles. */
            for (int32_t d = 0; d < n_dst; ++d) {
                int32_t kids[6];
                int32_t kid_count = 0;
                for (int32_t c = 0; c < n_dst && kid_count < 6; ++c)
                    if (dst_src[c] >= 0 && skeleton3d_valid_parent_index(dst_skel, c, n_dst) == d)
                        kids[kid_count++] = c;
                if (kid_count == 1) {
                    child_of[d] = kids[0];
                } else if (kid_count >= 2) {
                    float best = -1.0f;
                    for (int32_t i = 0; i < kid_count; ++i)
                        for (int32_t j = i + 1; j < kid_count; ++j) {
                            float r1[4];
                            float p1[3];
                            float r2[4];
                            float p2[3];
                            float cx[3];
                            float score;
                            animation3d_bone_bind_local_trs(dst_skel, kids[i], r1, p1);
                            animation3d_bone_bind_local_trs(dst_skel, kids[j], r2, p2);
                            if (!animation3d_vec3_normalize(p1) || !animation3d_vec3_normalize(p2))
                                continue;
                            cx[0] = p1[1] * p2[2] - p1[2] * p2[1];
                            cx[1] = p1[2] * p2[0] - p1[0] * p2[2];
                            cx[2] = p1[0] * p2[1] - p1[1] * p2[0];
                            score = cx[0] * cx[0] + cx[1] * cx[1] + cx[2] * cx[2];
                            if (score > best) {
                                best = score;
                                child_of[d] = kids[i];
                                child2_of[d] = kids[j];
                            }
                        }
                    if (child_of[d] < 0)
                        child_of[d] = kids[0];
                }
            }
            /* Imported skeletons need not store bones parent-first (glTF keeps joint-array
             * order); build explicit parent-first traversal orders for both hierarchies. */
            {
                int32_t placed = 0;
                int32_t guard = 0;
                int32_t *seen =
                    (int32_t *)calloc((size_t)(n_src > n_dst ? n_src : n_dst), sizeof(int32_t));
                ok = seen != NULL;
                while (ok && placed < n_src && guard++ <= n_src) {
                    for (int32_t b = 0; b < n_src; ++b) {
                        int32_t sp = skeleton3d_valid_parent_index(src_skel, b, n_src);
                        if (!seen[b] && (sp < 0 || seen[sp] == 2)) {
                            src_order[placed++] = b;
                            seen[b] = 1;
                        }
                    }
                    for (int32_t b = 0; b < n_src; ++b)
                        if (seen[b] == 1)
                            seen[b] = 2;
                }
                if (ok && placed < n_src)
                    for (int32_t b = 0; b < n_src; ++b)
                        if (!seen[b])
                            src_order[placed++] = b; /* cycles: stable fallback */
                if (ok) {
                    memset(seen, 0, (size_t)(n_src > n_dst ? n_src : n_dst) * sizeof(int32_t));
                    placed = 0;
                    guard = 0;
                    while (placed < n_dst && guard++ <= n_dst) {
                        for (int32_t b = 0; b < n_dst; ++b) {
                            int32_t dp = skeleton3d_valid_parent_index(dst_skel, b, n_dst);
                            if (!seen[b] && (dp < 0 || seen[dp] == 2)) {
                                dst_order[placed++] = b;
                                seen[b] = 1;
                            }
                        }
                        for (int32_t b = 0; b < n_dst; ++b)
                            if (seen[b] == 1)
                                seen[b] = 2;
                    }
                    if (placed < n_dst)
                        for (int32_t b = 0; b < n_dst; ++b)
                            if (!seen[b])
                                dst_order[placed++] = b;
                }
                free(seen);
            }
            /* Per-bone constants. */
            for (int32_t d = 0; d < n_dst; ++d) {
                if (dst_src[d] < 0)
                    continue;
                float src_pg[4];
                float dst_pg[4];
                float inv_dst_pg[4];
                float src_bind_rot[4];
                float src_bind_pos[3];
                float src_bind_scl[3];
                float dst_bind_rot[4];
                float dst_bind_pos[3];
                animation3d_bind_global_rotation(
                    src_skel, skeleton3d_valid_parent_index(src_skel, dst_src[d], n_src), src_pg);
                animation3d_bind_global_rotation(
                    dst_skel, skeleton3d_valid_parent_index(dst_skel, d, n_dst), dst_pg);
                animation3d_quat_conjugate(dst_pg, inv_dst_pg);
                animation3d_quat_mul(inv_dst_pg, src_pg, a_rot[d]);
                sanitize_keyframe_quat(a_rot[d]);
                /* Composed source bind local (chain fold). */
                float cb_rot[4];
                float cb_pos[3];
                float cb_scl[3];
                animation3d_bone_bind_local_trs_scaled(
                    src_skel, dst_chain[d][0], cb_rot, cb_pos, cb_scl);
                for (int32_t k = 1; k < dst_chain_len[d]; ++k) {
                    animation3d_bone_bind_local_trs_scaled(
                        src_skel, dst_chain[d][k], src_bind_rot, src_bind_pos, src_bind_scl);
                    animation3d_trs_compose(
                        cb_pos, cb_rot, cb_scl, src_bind_pos, src_bind_rot, src_bind_scl);
                }
                memcpy(bind_c_pos[d], cb_pos, sizeof(cb_pos));
                animation3d_bone_bind_local_trs(dst_skel, d, dst_bind_rot, dst_bind_pos);
                {
                    float src_bg[4];
                    float dst_bg[4];
                    float inv_src_bg[4];
                    animation3d_bind_global_rotation(src_skel, dst_anchor[d], src_bg);
                    animation3d_bind_global_rotation(dst_skel, d, dst_bg);
                    animation3d_quat_conjugate(src_bg, inv_src_bg);
                    animation3d_quat_mul(inv_src_bg, dst_bg, f_rot[d]);
                    sanitize_keyframe_quat(f_rot[d]);
                }
                {
                    float src_len = sqrtf(cb_pos[0] * cb_pos[0] + cb_pos[1] * cb_pos[1] +
                                          cb_pos[2] * cb_pos[2]);
                    float dst_len = animation3d_bone_bind_length(dst_skel, d);
                    pos_scale[d] =
                        (src_len > 1e-6f && dst_len > 1e-6f) ? (dst_len / src_len) : 1.0f;
                }
            }
            /* Union key-time grid over every mapped chain channel. */
            {
                int32_t total = 0;
                for (int32_t d = 0; d < n_dst; ++d)
                    for (int32_t k = 0; k < dst_chain_len[d]; ++k)
                        if (src_channel_of[dst_chain[d][k]])
                            total +=
                                animation3d_safe_keyframe_count(src_channel_of[dst_chain[d][k]]);
                if (total <= 0)
                    ok = 0;
                if (ok) {
                    times = (double *)malloc((size_t)total * sizeof(double));
                    ok = times != NULL;
                }
                if (ok) {
                    for (int32_t d = 0; d < n_dst; ++d)
                        for (int32_t k = 0; k < dst_chain_len[d]; ++k) {
                            const vgfx3d_anim_channel_t *ch = src_channel_of[dst_chain[d][k]];
                            if (!ch)
                                continue;
                            int32_t kc = animation3d_safe_keyframe_count(ch);
                            for (int32_t q = 0; q < kc; ++q)
                                times[time_count++] = ch->keyframes[q].time;
                        }
                    qsort(times, (size_t)time_count, sizeof(double), animation3d_time_cmp);
                    int32_t unique = 0;
                    for (int32_t q = 0; q < time_count; ++q)
                        if (unique == 0 || times[q] > times[unique - 1])
                            times[unique++] = times[q];
                    time_count = unique;
                    /* Cubic source segments flatten to lines on a sparse grid; subdivide
                     * wide spans to ~30 Hz so sampled keys preserve curve shape. Dense
                     * mocap grids are already finer and gain no keys. */
                    if (time_count > 1) {
                        int32_t est = time_count;
                        for (int32_t q = 1; q < time_count; ++q)
                            est += (int32_t)((times[q] - times[q - 1]) / 0.034);
                        double *densified = est > time_count
                                                ? (double *)malloc((size_t)est * sizeof(double))
                                                : NULL;
                        if (densified) {
                            int32_t n = 0;
                            for (int32_t q = 0; q < time_count; ++q) {
                                if (q > 0) {
                                    double span = times[q] - times[q - 1];
                                    int32_t subs = (int32_t)(span / 0.034);
                                    for (int32_t u = 1; u <= subs && n < est; ++u)
                                        densified[n++] =
                                            times[q - 1] + span * (double)u / (double)(subs + 1);
                                }
                                if (n < est)
                                    densified[n++] = times[q];
                            }
                            free(times);
                            times = densified;
                            time_count = n;
                            if (time_count > RT_ANIMATION3D_MAX_KEYFRAMES_PER_CHANNEL)
                                time_count = RT_ANIMATION3D_MAX_KEYFRAMES_PER_CHANNEL;
                        }
                    }
                    if (time_count > RT_ANIMATION3D_MAX_KEYFRAMES_PER_CHANNEL)
                        time_count = RT_ANIMATION3D_MAX_KEYFRAMES_PER_CHANNEL;
                }
            }
            if (ok)
                for (int32_t d = 0; d < n_dst && ok; ++d)
                    if (dst_src[d] >= 0) {
                        keys_of[d] = (vgfx3d_keyframe_t *)calloc((size_t)time_count,
                                                                 sizeof(vgfx3d_keyframe_t));
                        ok = keys_of[d] != NULL;
                    }
        }
        if (ok) {
            for (int32_t ti = 0; ti < time_count; ++ti) {
                float t = (float)times[ti];
                /* Source pose (skeleton order is parent-first by construction). */
                for (int32_t so = 0; so < n_src; ++so) {
                    int32_t sb = src_order[so];
                    float br[4];
                    float bp[3];
                    float bs[3];
                    float lr[4];
                    float lp[3];
                    float ls[3];
                    animation3d_bone_bind_local_trs_scaled(src_skel, sb, br, bp, bs);
                    sample_channel_trs_with_fallback(src_channel_of[sb], t, bp, br, bs, lp, lr, ls);
                    int32_t sp = skeleton3d_valid_parent_index(src_skel, sb, n_src);
                    if (sp < 0) {
                        memcpy(sg_pos[sb], lp, sizeof(lp));
                        memcpy(sg_rot[sb], lr, sizeof(lr));
                        memcpy(sg_scl[sb], ls, sizeof(ls));
                    } else {
                        memcpy(sg_pos[sb], sg_pos[sp], sizeof(lp));
                        memcpy(sg_rot[sb], sg_rot[sp], sizeof(lr));
                        memcpy(sg_scl[sb], sg_scl[sp], sizeof(ls));
                        animation3d_trs_compose(sg_pos[sb], sg_rot[sb], sg_scl[sb], lp, lr, ls);
                    }
                }
                /* Destination pose, parent-first, emitting keys for mapped bones. */
                for (int32_t od = 0; od < n_dst; ++od) {
                    int32_t d = dst_order[od];
                    float pg_rot[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                    float pg_pos[3] = {0.0f, 0.0f, 0.0f};
                    int32_t pd = skeleton3d_valid_parent_index(dst_skel, d, n_dst);
                    if (pd >= 0) {
                        memcpy(pg_rot, dg_rot[pd], sizeof(pg_rot));
                        memcpy(pg_pos, dg_pos[pd], sizeof(pg_pos));
                    }
                    float bind_rot[4];
                    float bind_pos[3];
                    animation3d_bone_bind_local_trs(dst_skel, d, bind_rot, bind_pos);
                    float l_rot[4];
                    float l_pos[3];
                    if (dst_src[d] < 0) {
                        memcpy(l_rot, bind_rot, sizeof(l_rot));
                        memcpy(l_pos, bind_pos, sizeof(l_pos));
                    } else {
                        /* Composed source local sample for translation + leaf rotation. */
                        float c_rot[4];
                        float c_pos[3];
                        float c_scl[3];
                        {
                            float br[4];
                            float bp[3];
                            float bs[3];
                            animation3d_bone_bind_local_trs_scaled(
                                src_skel, dst_chain[d][0], br, bp, bs);
                            sample_channel_trs_with_fallback(src_channel_of[dst_chain[d][0]],
                                                             t,
                                                             bp,
                                                             br,
                                                             bs,
                                                             c_pos,
                                                             c_rot,
                                                             c_scl);
                            for (int32_t k = 1; k < dst_chain_len[d]; ++k) {
                                float lr2[4];
                                float lp2[3];
                                float ls2[3];
                                animation3d_bone_bind_local_trs_scaled(
                                    src_skel, dst_chain[d][k], br, bp, bs);
                                sample_channel_trs_with_fallback(
                                    src_channel_of[dst_chain[d][k]], t, bp, br, bs, lp2, lr2, ls2);
                                animation3d_trs_compose(c_pos, c_rot, c_scl, lp2, lr2, ls2);
                            }
                        }
                        (void)c_rot;
                        {
                            float delta[3];
                            delta[0] = c_pos[0] - bind_c_pos[d][0];
                            delta[1] = c_pos[1] - bind_c_pos[d][1];
                            delta[2] = c_pos[2] - bind_c_pos[d][2];
                            animation3d_quat_rotate_vec3(a_rot[d], delta, delta);
                            l_pos[0] = bind_pos[0] + pos_scale[d] * delta[0];
                            l_pos[1] = bind_pos[1] + pos_scale[d] * delta[1];
                            l_pos[2] = bind_pos[2] + pos_scale[d] * delta[2];
                        }
                        int aimed = 0;
                        int32_t dc = child_of[d];
                        if (dc >= 0 && dst_anchor[dc] >= 0 && dst_anchor[d] >= 0) {
                            float src_dir[3];
                            float rest_world[4];
                            float child_bind_rot[4];
                            float child_bind_pos[3];
                            float rest_dir[3];
                            src_dir[0] = sg_pos[dst_anchor[dc]][0] - sg_pos[dst_anchor[d]][0];
                            src_dir[1] = sg_pos[dst_anchor[dc]][1] - sg_pos[dst_anchor[d]][1];
                            src_dir[2] = sg_pos[dst_anchor[dc]][2] - sg_pos[dst_anchor[d]][2];
                            animation3d_quat_mul(pg_rot, bind_rot, rest_world);
                            animation3d_bone_bind_local_trs(
                                dst_skel, dc, child_bind_rot, child_bind_pos);
                            animation3d_quat_rotate_vec3(rest_world, child_bind_pos, rest_dir);
                            int32_t dc2 = child2_of[d];
                            if (dc2 >= 0 && dst_anchor[dc2] >= 0) {
                                float src_dir2[3];
                                float rest_dir2[3];
                                float arc[4];
                                src_dir2[0] = sg_pos[dst_anchor[dc2]][0] - sg_pos[dst_anchor[d]][0];
                                src_dir2[1] = sg_pos[dst_anchor[dc2]][1] - sg_pos[dst_anchor[d]][1];
                                src_dir2[2] = sg_pos[dst_anchor[dc2]][2] - sg_pos[dst_anchor[d]][2];
                                animation3d_bone_bind_local_trs(
                                    dst_skel, dc2, child_bind_rot, child_bind_pos);
                                animation3d_quat_rotate_vec3(rest_world, child_bind_pos, rest_dir2);
                                if (animation3d_quat_from_two_dirs(
                                        rest_dir, rest_dir2, src_dir, src_dir2, arc)) {
                                    float world_rot[4];
                                    float inv_pg[4];
                                    animation3d_quat_mul(arc, rest_world, world_rot);
                                    animation3d_quat_conjugate(pg_rot, inv_pg);
                                    animation3d_quat_mul(inv_pg, world_rot, l_rot);
                                    sanitize_keyframe_quat(l_rot);
                                    aimed = 1;
                                }
                            }
                            if (!aimed && animation3d_vec3_normalize(src_dir)) {
                                /* Single aim child: start from the absolute
                                 * bind-frame transfer so the source's roll about
                                 * the bone axis (forearm/neck twist) survives,
                                 * then apply only the minimal swing that re-aims
                                 * the bone direction. Bind-compatible rigs make
                                 * the swing an identity, i.e. the exact source
                                 * pose including twist. */
                                float world_abs[4];
                                float abs_dir[3];
                                animation3d_quat_mul(
                                    sg_rot[dst_anchor[d]], f_rot[d], world_abs);
                                animation3d_bone_bind_local_trs(
                                    dst_skel, dc, child_bind_rot, child_bind_pos);
                                animation3d_quat_rotate_vec3(
                                    world_abs, child_bind_pos, abs_dir);
                                if (animation3d_vec3_normalize(abs_dir)) {
                                    float arc[4];
                                    float world_rot[4];
                                    float inv_pg[4];
                                    animation3d_quat_shortest_arc(abs_dir, src_dir, arc);
                                    animation3d_quat_mul(arc, world_abs, world_rot);
                                    animation3d_quat_conjugate(pg_rot, inv_pg);
                                    animation3d_quat_mul(inv_pg, world_rot, l_rot);
                                    sanitize_keyframe_quat(l_rot);
                                    aimed = 1;
                                } else if (animation3d_vec3_normalize(rest_dir)) {
                                    float arc[4];
                                    float world_rot[4];
                                    float inv_pg[4];
                                    animation3d_quat_shortest_arc(rest_dir, src_dir, arc);
                                    animation3d_quat_mul(arc, rest_world, world_rot);
                                    animation3d_quat_conjugate(pg_rot, inv_pg);
                                    animation3d_quat_mul(inv_pg, world_rot, l_rot);
                                    sanitize_keyframe_quat(l_rot);
                                    aimed = 1;
                                }
                            }
                        }
                        if (!aimed) {
                            /* No aim child: transfer the source bone's ABSOLUTE world
                             * orientation through the constant bind-frame swap, localized
                             * against the runtime parent — consistent with aimed parents
                             * and exact for bind-compatible rigs. */
                            float world_rot[4];
                            float inv_pg[4];
                            animation3d_quat_mul(sg_rot[dst_anchor[d]], f_rot[d], world_rot);
                            animation3d_quat_conjugate(pg_rot, inv_pg);
                            animation3d_quat_mul(inv_pg, world_rot, l_rot);
                            sanitize_keyframe_quat(l_rot);
                        }
                        vgfx3d_keyframe_t *key = &keys_of[d][ti];
                        key->time = times[ti];
                        memcpy(key->position, l_pos, sizeof(l_pos));
                        memcpy(key->rotation, l_rot, sizeof(l_rot));
                        key->scale_xyz[0] = 1.0f;
                        key->scale_xyz[1] = 1.0f;
                        key->scale_xyz[2] = 1.0f;
                        key->position_mask = 0x07u;
                        key->rotation_mask = 0x0Fu;
                        key->scale_mask = 0x07u;
                    }
                    /* Compose this bone's global for its children. */
                    memcpy(dg_rot[d], pg_rot, sizeof(pg_rot));
                    memcpy(dg_pos[d], pg_pos, sizeof(pg_pos));
                    {
                        float one[3] = {1.0f, 1.0f, 1.0f};
                        float scl_acc[3] = {1.0f, 1.0f, 1.0f};
                        animation3d_trs_compose(dg_pos[d], dg_rot[d], scl_acc, l_pos, l_rot, one);
                    }
                }
            }
            /* Publish channels. */
            for (int32_t d = 0; d < n_dst && ok; ++d) {
                if (dst_src[d] < 0)
                    continue;
                if (out->channel_count >= out->channel_capacity) {
                    ok = 0;
                    break;
                }
                vgfx3d_anim_channel_t *ch = &out->channels[out->channel_count];
                memset(ch, 0, sizeof(*ch));
                ch->bone_index = d;
                ch->keyframes = keys_of[d];
                ch->owned_keyframes = keys_of[d];
                ch->keyframe_count = time_count;
                ch->keyframe_capacity = time_count;
                ch->initialized_keyframe_count = time_count;
                ch->owned_keyframe_capacity = time_count;
                keys_of[d] = NULL; /* ownership moved into the clip */
                out->channel_count++;
                out->initialized_channel_count = out->channel_count;
            }
        }
        if (keys_of)
            for (int32_t d = 0; d < n_dst; ++d)
                free(keys_of[d]);
        free(keys_of);
        free(src_channel_of);
        free(dst_src);
        free(dst_anchor);
        free(dst_chain_len);
        free(dst_chain);
        free(child_of);
        free(child2_of);
        free(a_rot);
        free(f_rot);
        free(bind_c_pos);
        free(pos_scale);
        free(times);
        free(sg_pos);
        free(sg_rot);
        free(sg_scl);
        free(dg_pos);
        free(dg_rot);
        free(src_order);
        free(dst_order);
        if (!ok) {
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

/// @brief Pin a bone channel's travel so the clip plays in place.
/// @details Gameplay owns actor translation (the choreography clock places actors);
///          mocap clips carry real root travel that would fight it. Every position
///          key's X/Z lanes (and Y too unless @p keep_vertical) are pinned to the
///          FIRST key's values, and the corresponding cubic tangent lanes are
///          zeroed. Mutates @p anim in place — call it on a retargeted copy, never
///          on a shared source clip.
/// @param anim Animation3D to modify.
/// @param bone_index Root/hips bone whose channel is pinned.
/// @param keep_vertical Nonzero preserves Y motion (crouches, slides).
/// @return One when a channel was modified; zero when none matched.
int64_t rt_animation3d_strip_root_motion(void *anim, int64_t bone_index, int8_t keep_vertical) {
    rt_animation3d *a = animation3d_checked(anim);
    if (!a)
        return 0;
    int32_t channel_count = animation3d_safe_channel_count(a);
    for (int32_t c = 0; c < channel_count; ++c) {
        vgfx3d_anim_channel_t *ch = &a->channels[c];
        int32_t key_count = animation3d_safe_keyframe_count(ch);
        if (ch->bone_index != bone_index || key_count <= 0)
            continue;
        float baseline[3];
        uint8_t baseline_mask = ch->keyframes[0].position_mask & 0x07u;
        for (int lane = 0; lane < 3; ++lane)
            baseline[lane] = ch->keyframes[0].position[lane];
        for (int32_t k = 0; k < key_count; ++k) {
            vgfx3d_keyframe_t *key = &ch->keyframes[k];
            for (int lane = 0; lane < 3; ++lane) {
                if (lane == 1 && keep_vertical)
                    continue;
                if ((key->position_mask & (uint8_t)(1u << lane)) &&
                    (baseline_mask & (uint8_t)(1u << lane)))
                    key->position[lane] = baseline[lane];
                key->pos_in_tangent[lane] =
                    lane == 1 && keep_vertical ? key->pos_in_tangent[lane] : 0.0f;
                key->pos_out_tangent[lane] =
                    lane == 1 && keep_vertical ? key->pos_out_tangent[lane] : 0.0f;
            }
        }
        return 1;
    }
    return 0;
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
