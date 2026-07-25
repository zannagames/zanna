//===----------------------------------------------------------------------===//
// Part of the Zanna project, under the GNU GPL v3. See LICENSE.
// File: src/runtime/graphics/3d/anim/rt_skeleton3d_player.inc  (#included from rt_skeleton3d.c; not a standalone translation unit)
// Purpose: AnimPlayer3D single-clip playback implementation
//===----------------------------------------------------------------------===//
/**
 * @file rt_skeleton3d_player.inc
 * @brief Implements single-clip playback, crossfading, and palette generation.
 *
 * This implementation fragment manages retained skeleton and clip references,
 * playback clocks and looping policy, bind-pose fallbacks, per-bone TRS
 * crossfades, model-space matrices, skinning palettes, and previous-frame
 * snapshots used by temporal rendering. It is included by
 * `rt_skeleton3d.c` and is not a standalone translation unit.
 */
/*==========================================================================
 * AnimPlayer3D implementation
 *=========================================================================*/

/// @brief Release all references and pose buffers owned by an animation player.
/// @param[in,out] obj Player payload being finalized.
/// @details Releases the skeleton, active clip, and outgoing crossfade clip,
/// then frees the current, previous, snapshot, local, and global matrix arrays.
static void rt_anim_player3d_finalize(void *obj) {
    rt_anim_player3d *p = (rt_anim_player3d *)obj;
    animation3d_release_class_ref((void **)&p->skeleton, RT_G3D_SKELETON3D_CLASS_ID);
    animation3d_release_class_ref((void **)&p->current, RT_G3D_ANIMATION3D_CLASS_ID);
    animation3d_release_class_ref((void **)&p->crossfade_from, RT_G3D_ANIMATION3D_CLASS_ID);
    free(p->bone_palette);
    p->bone_palette = NULL;
    free(p->prev_bone_palette);
    p->prev_bone_palette = NULL;
    free(p->motion_palette_snapshot);
    p->motion_palette_snapshot = NULL;
    free(p->local_transforms);
    p->local_transforms = NULL;
    free(p->globals_buf);
    p->globals_buf = NULL;
}

/// @brief Re-evaluate a player's current animated pose and skinning palette.
/// @param[in,out] p Player to evaluate.
static void compute_bone_palette(rt_anim_player3d *p);
/// @brief Reset a player's matrix buffers to the skeleton bind pose.
/// @param[in,out] p Player to reset.
static void compute_player_bind_pose(rt_anim_player3d *p);
/// @brief Resolve the active clip's effective looping policy.
/// @param[in,out] p Player whose current clip and override are inspected.
/// @return One when looping is enabled, otherwise zero.
static int8_t anim_player_current_looping(rt_anim_player3d *p);

/// @brief Normalize a requested bone count into a pose-buffer capacity.
/// @param[in] bone_count Requested number of bone-matrix slots.
/// @return At least one slot and at most `VGFX3D_MAX_SKELETON_BONES`.
static int32_t skeleton3d_pose_capacity_for_count(int32_t bone_count) {
    if (bone_count <= 0)
        return 1;
    if (bone_count > VGFX3D_MAX_SKELETON_BONES)
        return VGFX3D_MAX_SKELETON_BONES;
    return bone_count;
}

/// @brief Grow one 16-float-per-bone pose buffer and clear its new suffix.
/// @param[in,out] buffer Address of the owned buffer pointer.
/// @param[in] old_capacity Number of existing bone slots.
/// @param[in] new_capacity Required number of bone slots.
/// @return Nonzero when no growth is needed or reallocation succeeds;
/// otherwise zero.
/// @details Existing floats are preserved. A null slot address is treated as
/// requiring no work, matching callers that grow synchronized optional buffers.
static int skeleton3d_grow_pose_buffer(float **buffer, int32_t old_capacity, int32_t new_capacity) {
    if (!buffer || new_capacity <= old_capacity)
        return 1;
    size_t old_floats = (size_t)(old_capacity > 0 ? old_capacity : 0) * 16u;
    size_t new_floats = (size_t)new_capacity * 16u;
    if (new_floats > SIZE_MAX / sizeof(float))
        return 0;
    float *grown = (float *)realloc(*buffer, new_floats * sizeof(float));
    if (!grown)
        return 0;
    memset(grown + old_floats, 0, (new_floats - old_floats) * sizeof(float));
    *buffer = grown;
    return 1;
}

/// @brief Ensure every player pose buffer can hold a requested skeleton.
/// @param[in,out] p Player whose synchronized buffers may be enlarged.
/// @param[in] bone_count Required number of bone matrices.
/// @return Nonzero when all buffers have sufficient capacity; otherwise zero.
static int anim_player3d_ensure_pose_capacity(rt_anim_player3d *p, int32_t bone_count) {
    if (!p)
        return 0;
    int32_t new_capacity = skeleton3d_pose_capacity_for_count(bone_count);
    int32_t old_capacity = p->pose_capacity > 0 ? p->pose_capacity : 0;
    if (new_capacity <= old_capacity)
        return 1;
    if (!skeleton3d_grow_pose_buffer(&p->bone_palette, old_capacity, new_capacity) ||
        !skeleton3d_grow_pose_buffer(&p->prev_bone_palette, old_capacity, new_capacity) ||
        !skeleton3d_grow_pose_buffer(&p->motion_palette_snapshot, old_capacity, new_capacity) ||
        !skeleton3d_grow_pose_buffer(&p->local_transforms, old_capacity, new_capacity) ||
        !skeleton3d_grow_pose_buffer(&p->globals_buf, old_capacity, new_capacity))
        return 0;
    p->pose_capacity = new_capacity;
    return 1;
}

/// @brief Create an animation player bound to a target skeleton.
///
/// The player holds the current playback time, the active and
/// fading-out clip, and a trio of per-bone current, previous, and
/// snapshot palette buffers for temporal previous-pose lookup.
///
/// @param[in] skeleton Runtime `Skeleton3D` to retain and evaluate.
/// @return A new retained player initialized at the bind pose, or `NULL` for
/// an invalid skeleton or allocation failure.
/// @details Construction freezes skeleton topology and allocates synchronized
/// current, previous, snapshot, local, and global buffers. Playback begins
/// stopped with no active clip and unit speed.
void *rt_anim_player3d_new(void *skeleton) {
    rt_skeleton3d *skel =
        (rt_skeleton3d *)rt_g3d_checked_or_null(skeleton, RT_G3D_SKELETON3D_CLASS_ID);
    if (!skel) {
        rt_trap("AnimPlayer3D.New: null skeleton");
        return NULL;
    }
    int32_t bone_count = skeleton3d_safe_bone_count(skel);
    skel->bone_count = bone_count;

    rt_anim_player3d *p = (rt_anim_player3d *)rt_obj_new_i64(RT_G3D_ANIMPLAYER3D_CLASS_ID,
                                                             (int64_t)sizeof(rt_anim_player3d));
    if (!p) {
        rt_trap("AnimPlayer3D.New: memory allocation failed");
        return NULL;
    }
    p->vptr = NULL;
    p->skeleton = skel;
    rt_obj_retain_maybe(skeleton);
    p->current = NULL;
    p->crossfade_from = NULL;
    p->current_time = 0.0f;
    p->crossfade_time = 0.0f;
    p->crossfade_duration = 0.0f;
    p->crossfade_from_time = 0.0f;
    p->crossfade_from_speed = 1.0f;
    p->speed = 1.0f;
    p->playing = 0;
    p->loop_override_enabled = 0;
    p->loop_override_value = 0;
    p->crossfade_from_looping = 0;
    p->last_motion_frame = 0;
    p->has_prev_motion_palette = 0;

    p->pose_capacity = skeleton3d_pose_capacity_for_count(bone_count);
    size_t palette_size = (size_t)p->pose_capacity * 16u * sizeof(float);
    p->bone_palette = (float *)calloc(1, palette_size);
    p->prev_bone_palette = (float *)calloc(1, palette_size);
    p->motion_palette_snapshot = (float *)calloc(1, palette_size);
    p->local_transforms = (float *)calloc(1, palette_size);
    p->globals_buf = (float *)calloc(1, palette_size);
    if (!p->bone_palette || !p->prev_bone_palette || !p->motion_palette_snapshot ||
        !p->local_transforms || !p->globals_buf) {
        rt_anim_player3d_finalize(p);
        if (rt_obj_release_check0(p))
            rt_obj_free(p);
        rt_trap("AnimPlayer3D.New: memory allocation failed");
        return NULL;
    }

    skeleton3d_freeze(skel);
    compute_player_bind_pose(p);
    if (palette_size > 0) {
        memcpy(p->prev_bone_palette, p->bone_palette, palette_size);
        memcpy(p->motion_palette_snapshot, p->bone_palette, palette_size);
    }

    rt_obj_set_finalizer(p, rt_anim_player3d_finalize);
    return p;
}

/// @brief Snap-cut to a new animation clip, resetting playback time to 0.
/// For smooth transitions, prefer `rt_anim_player3d_crossfade`.
/// @param[in,out] obj Player to modify.
/// @param[in] animation New `Animation3D`, or `NULL` to clear the current clip.
/// @details The player retains the new clip, releases any outgoing crossfade
/// clip, resets temporal motion history, and immediately recomputes the pose.
/// Passing `NULL` stops playback and produces the bind pose.
void rt_anim_player3d_play(void *obj, void *animation) {
    rt_anim_player3d *p = anim_player3d_checked(obj);
    rt_animation3d *anim = animation ? animation3d_checked(animation) : NULL;
    if (!p || (animation && !anim))
        return;
    animation3d_assign_class_ref((void **)&p->current, anim, RT_G3D_ANIMATION3D_CLASS_ID);
    p->current_time = 0.0f;
    p->playing = anim ? 1 : 0;
    animation3d_assign_class_ref((void **)&p->crossfade_from, NULL, RT_G3D_ANIMATION3D_CLASS_ID);
    p->crossfade_time = 0.0f;
    p->crossfade_duration = 0.0f;
    p->crossfade_from_time = 0.0f;
    p->has_prev_motion_palette = 0;
    p->last_motion_frame = 0;
    compute_bone_palette(p);
}

/// @brief Cross-fade smoothly into a new animation clip over `duration` seconds.
///
/// Both clips run in parallel during the fade; per-bone TRS is
/// linearly blended (lerp + slerp) by the elapsed-fade ratio.
///
/// @param[in,out] obj Player to transition.
/// @param[in] animation Valid target `Animation3D` retained by the player.
/// @param[in] duration Fade duration in seconds.
/// @details A nonpositive or non-finite duration becomes an immediate play.
/// The outgoing clip keeps its captured time, speed, and looping policy while
/// the target begins at time zero. Missing channels on either clip sample the
/// corresponding skeleton bind transform.
void rt_anim_player3d_crossfade(void *obj, void *animation, double duration) {
    rt_anim_player3d *p = anim_player3d_checked(obj);
    rt_animation3d *anim = animation3d_checked(animation);
    rt_animation3d *current;
    if (!p || !anim)
        return;
    if (!isfinite(duration) || duration <= 0.0) {
        rt_anim_player3d_play(obj, anim);
        return;
    }
    current = animation3d_clip_slot((void **)&p->current);
    animation3d_assign_class_ref((void **)&p->crossfade_from, current, RT_G3D_ANIMATION3D_CLASS_ID);
    p->crossfade_from_time = p->current_time;
    p->crossfade_from_speed = isfinite(p->speed) ? p->speed : 1.0f;
    p->crossfade_from_looping = anim_player_current_looping(p);
    animation3d_assign_class_ref((void **)&p->current, anim, RT_G3D_ANIMATION3D_CLASS_ID);
    p->current_time = 0.0f;
    p->crossfade_time = 0.0f;
    p->crossfade_duration = skeleton3d_clamp_nonnegative_float(duration, FLT_MAX);
    p->playing = 1;
    p->has_prev_motion_palette = 0;
    p->last_motion_frame = 0;
    compute_bone_palette(p);
}

/// @brief Stop playback and return the output pose to the skeleton bind pose.
/// @param[in,out] obj Player to stop.
/// @details The current clip remains retained and its current time remains
/// available, but the outgoing fade is discarded, motion history is reset,
/// and the visible palette is immediately restored to the bind pose.
void rt_anim_player3d_stop(void *obj) {
    rt_anim_player3d *p = anim_player3d_checked(obj);
    if (!p)
        return;
    p->playing = 0;
    animation3d_assign_class_ref((void **)&p->crossfade_from, NULL, RT_G3D_ANIMATION3D_CLASS_ID);
    p->crossfade_time = 0.0f;
    p->crossfade_duration = 0.0f;
    p->crossfade_from_time = 0.0f;
    p->has_prev_motion_palette = 0;
    p->last_motion_frame = 0;
    compute_player_bind_pose(p);
}

/// @brief Build global matrices and skinning palette from local bone transforms.
/// @param[in] skel Skeleton supplying hierarchy and inverse-bind matrices.
/// @param[in] locals Array of one row-major local matrix per readable bone.
/// @param[out] globals Scratch/output array receiving model-space matrices.
/// @param[out] palette Output array receiving `global * inverse_bind` skinning
/// matrices.
static void compute_palette_from_locals(const rt_skeleton3d *skel,
                                        const float *locals,
                                        float *globals,
                                        float *palette) {
    int32_t bone_count = skeleton3d_safe_bone_count(skel);
    if (!skel || !locals || !globals || !palette || bone_count <= 0)
        return;

    skeleton3d_compute_globals_from_locals(skel, locals, globals, bone_count);
    for (int32_t i = 0; i < bone_count; i++)
        mat4f_mul_local(&globals[i * 16], skel->bones[i].inverse_bind, &palette[i * 16]);
}

/// @brief Force a player to the skeleton bind pose in local, global, and skinning buffers.
/// @param[in,out] p Player whose buffers are reset.
/// @details The local bind matrices are copied from the skeleton, then hierarchy
/// composition and inverse-bind multiplication regenerate the other buffers.
static void compute_player_bind_pose(rt_anim_player3d *p) {
    rt_skeleton3d *skel = p ? animation3d_skeleton_slot((void **)&p->skeleton) : NULL;
    int32_t bone_count = skeleton3d_safe_bone_count(skel);
    if (p && !anim_player3d_ensure_pose_capacity(p, bone_count))
        return;
    if (!p || !skel || bone_count <= 0 || !p->local_transforms || !p->globals_buf ||
        !p->bone_palette)
        return;
    skel->bone_count = bone_count;
    for (int32_t i = 0; i < bone_count; i++)
        memcpy(&p->local_transforms[i * 16], skel->bones[i].bind_pose_local, 16 * sizeof(float));
    compute_palette_from_locals(skel, p->local_transforms, p->globals_buf, p->bone_palette);
}

/// @brief Compute the bone palette from the current animation state.
/// @param[in,out] p Player whose pose, model-space matrices, and palette are
/// recomputed.
/// @details Evaluation begins from the bind pose and replaces local transforms
/// for readable channels of the current clip. During a crossfade, every bone
/// samples both clips with bind-pose fallback, translations and scales are
/// linearly interpolated, and rotations use shortest-path SLERP.
static void compute_bone_palette(rt_anim_player3d *p) {
    rt_skeleton3d *skel = p ? animation3d_skeleton_slot((void **)&p->skeleton) : NULL;
    rt_animation3d *current = p ? animation3d_clip_slot((void **)&p->current) : NULL;
    rt_animation3d *crossfade_from = p ? animation3d_clip_slot((void **)&p->crossfade_from) : NULL;
    int32_t bone_count = skeleton3d_safe_bone_count(skel);
    if (p && !anim_player3d_ensure_pose_capacity(p, bone_count))
        return;
    if (!p || !skel || bone_count == 0 || !p->local_transforms || !p->globals_buf ||
        !p->bone_palette)
        return;
    skel->bone_count = bone_count;

    /* Start with bind pose for all bones */
    for (int32_t i = 0; i < bone_count; i++)
        memcpy(&p->local_transforms[i * 16], skel->bones[i].bind_pose_local, 16 * sizeof(float));

    /* Override with animated transforms from current animation */
    if (current) {
        int32_t channel_count = animation3d_safe_channel_count(current);
        current->channel_count = channel_count;
        for (int32_t c = 0; c < channel_count; c++) {
            int32_t bone = current->channels[c].bone_index;
            if (bone >= 0 && bone < bone_count) {
                sample_channel_with_fallback(&current->channels[c],
                                             p->current_time,
                                             &skel->bones[bone],
                                             &p->local_transforms[bone * 16]);
            }
        }
    }

    /* Crossfade over every bone so target-only and source-only channels blend
     * against bind pose instead of popping at transition boundaries. */
    if (crossfade_from && current && p->crossfade_duration > 0.0f) {
        float factor = skeleton3d_clamp01f(p->crossfade_time / p->crossfade_duration);
        /* Map bone -> channel index for each clip once (O(channels)) so the per-bone
         * blend below resolves channels with an O(1) lookup instead of the prior
         * O(channels) scan per bone (turning O(bones*channels) into O(bones+channels)).
         * A NULL channel reproduces the original find-miss bind-pose fallback exactly. */
        int32_t from_ch[VGFX3D_MAX_SKELETON_BONES];
        int32_t to_ch[VGFX3D_MAX_SKELETON_BONES];
        int32_t mapped =
            bone_count < VGFX3D_MAX_SKELETON_BONES ? bone_count : VGFX3D_MAX_SKELETON_BONES;
        int32_t from_count = animation3d_safe_channel_count(crossfade_from);
        int32_t to_count = animation3d_safe_channel_count(current);
        for (int32_t b = 0; b < mapped; b++) {
            from_ch[b] = -1;
            to_ch[b] = -1;
        }
        for (int32_t cidx = 0; cidx < from_count; cidx++) {
            int32_t b = crossfade_from->channels[cidx].bone_index;
            if (b >= 0 && b < mapped && from_ch[b] < 0)
                from_ch[b] = cidx;
        }
        for (int32_t cidx = 0; cidx < to_count; cidx++) {
            int32_t b = current->channels[cidx].bone_index;
            if (b >= 0 && b < mapped && to_ch[b] < 0)
                to_ch[b] = cidx;
        }

        for (int32_t bone = 0; bone < bone_count; bone++) {
            const vgfx3d_anim_channel_t *from_channel =
                (bone < mapped && from_ch[bone] >= 0) ? &crossfade_from->channels[from_ch[bone]]
                                                      : NULL;
            const vgfx3d_anim_channel_t *to_channel =
                (bone < mapped && to_ch[bone] >= 0) ? &current->channels[to_ch[bone]] : NULL;
            float from_pos[3], from_rot[4], from_scl[3];
            float to_pos[3], to_rot[4], to_scl[3];
            float blend_pos[3], blend_rot[4], blend_scl[3];
            sample_animation_channel_trs(from_channel,
                                         p->crossfade_from_time,
                                         &skel->bones[bone],
                                         from_pos,
                                         from_rot,
                                         from_scl);
            sample_animation_channel_trs(to_channel,
                                         p->current_time,
                                         &skel->bones[bone],
                                         to_pos,
                                         to_rot,
                                         to_scl);
            for (int i = 0; i < 3; i++) {
                blend_pos[i] = from_pos[i] + factor * (to_pos[i] - from_pos[i]);
                blend_scl[i] = from_scl[i] + factor * (to_scl[i] - from_scl[i]);
            }
            quat_slerp_float(from_rot, to_rot, factor, blend_rot);
            animation3d_sanitize_trs(blend_pos, blend_rot, blend_scl, from_pos, from_rot, from_scl);
            build_trs_float(blend_pos, blend_rot, blend_scl, &p->local_transforms[bone * 16]);
        }
    }

    compute_palette_from_locals(skel, p->local_transforms, p->globals_buf, p->bone_palette);
}

/// @brief True if the player's currently active clip is set to loop.
/// @param[in,out] p Player whose effective policy is requested.
/// @return The explicit loop override when enabled, otherwise the current
/// clip's loop flag; zero when there is no valid current clip.
static int8_t anim_player_current_looping(rt_anim_player3d *p) {
    rt_animation3d *current = p ? animation3d_clip_slot((void **)&p->current) : NULL;
    if (!current)
        return 0;
    if (p->loop_override_enabled)
        return p->loop_override_value ? 1 : 0;
    return current->looping ? 1 : 0;
}

/// @brief Advance playback by `delta_time` seconds and refresh the bone palette.
///
/// Handles wrap-around for looping clips, fade-in / fade-out for
/// active cross-fades, then rebuilds the current local, global, and
/// skinning matrix buffers.
///
/// @param[in,out] obj Player to advance.
/// @param[in] delta_time Nonnegative finite elapsed time in seconds.
/// @details The current clock is scaled by playback speed. Looping playback
/// wraps in either direction; non-looping playback clamps at an endpoint and
/// stops. Crossfade elapsed time advances independently of playback speed,
/// while the outgoing clip advances at its captured speed.
void rt_anim_player3d_update(void *obj, double delta_time) {
    rt_anim_player3d *p = anim_player3d_checked(obj);
    rt_animation3d *current;
    rt_animation3d *crossfade_from;
    if (!p)
        return;
    current = animation3d_clip_slot((void **)&p->current);
    if (!p->playing || !current)
        return;
    if (!isfinite(delta_time) || delta_time < 0.0)
        return;
    if (!isfinite(p->speed))
        p->speed = 1.0f;

    p->current_time += skeleton3d_clamp_to_float(delta_time * (double)p->speed, 0.0f);
    if (!isfinite(p->current_time))
        p->current_time = 0.0f;

    /* Handle looping / end */
    if (anim_player_current_looping(p)) {
        if (current->duration > 0.0f)
            p->current_time = animation3d_wrap_time(p->current_time, current->duration);
    } else {
        if (p->current_time >= current->duration) {
            p->current_time = current->duration;
            p->playing = 0;
        } else if (p->current_time <= 0.0f && p->speed < 0.0f) {
            p->current_time = 0.0f;
            p->playing = 0;
        }
    }

    /* Update crossfade */
    crossfade_from = animation3d_clip_slot((void **)&p->crossfade_from);
    if (crossfade_from) {
        p->crossfade_time += skeleton3d_clamp_to_float(delta_time, 0.0f);
        p->crossfade_from_time +=
            skeleton3d_clamp_to_float(delta_time * (double)p->crossfade_from_speed, 0.0f);
        if (!isfinite(p->crossfade_time))
            p->crossfade_time = 0.0f;
        if (!isfinite(p->crossfade_from_time))
            p->crossfade_from_time = 0.0f;
        if (crossfade_from->duration > 0.0f) {
            if (p->crossfade_from_looping)
                p->crossfade_from_time =
                    animation3d_wrap_time(p->crossfade_from_time, crossfade_from->duration);
            else if (p->crossfade_from_time > crossfade_from->duration)
                p->crossfade_from_time = crossfade_from->duration;
            else if (p->crossfade_from_time < 0.0f)
                p->crossfade_from_time = 0.0f;
        }
        if (p->crossfade_time >= p->crossfade_duration) {
            animation3d_assign_class_ref(
                (void **)&p->crossfade_from, NULL, RT_G3D_ANIMATION3D_CLASS_ID);
            p->crossfade_time = 0.0f;
            p->crossfade_duration = 0.0f;
            p->crossfade_from_time = 0.0f;
            p->crossfade_from_speed = 1.0f;
            p->crossfade_from_looping = 0;
        }
    }

    compute_bone_palette(p);
}

/// @brief Time-scale factor applied per `update` call (1.0 = real time, 0.5 = slow-mo, etc.).
/// @param[in,out] obj Player to modify.
/// @param[in] speed Playback multiplier. Non-finite input becomes `1.0`, and
/// finite input is saturated to the float range; negative values are allowed.
void rt_anim_player3d_set_speed(void *obj, double speed) {
    rt_anim_player3d *p = anim_player3d_checked(obj);
    if (p) {
        if (!isfinite(speed))
            speed = 1.0;
        p->speed = skeleton3d_clamp_to_float(speed, 1.0f);
    }
}

/// @brief Current speed multiplier (1.0 if `obj` is NULL).
/// @param[in] obj Player to query.
/// @return The finite stored speed, or `1.0` for an invalid player or corrupt
/// non-finite value.
double rt_anim_player3d_get_speed(void *obj) {
    rt_anim_player3d *p = anim_player3d_checked(obj);
    return (p && isfinite(p->speed)) ? p->speed : 1.0;
}

/// @brief True if the player is currently advancing time on `update` calls.
/// @param[in] obj Player to query.
/// @return One when playback is active, otherwise zero.
int8_t rt_anim_player3d_is_playing(void *obj) {
    rt_anim_player3d *p = anim_player3d_checked(obj);
    return p && p->playing ? 1 : 0;
}

/// @brief Current playback time within the active clip (seconds).
/// @param[in] obj Player to query.
/// @return The finite stored target-clip time in seconds, or zero for invalid
/// or non-finite state.
double rt_anim_player3d_get_time(void *obj) {
    rt_anim_player3d *p = anim_player3d_checked(obj);
    return (p && isfinite(p->current_time)) ? p->current_time : 0.0;
}

/// @brief Seek to an absolute time in the current clip.
/// Resets the motion-blur previous-pose snapshot so blur doesn't span the discontinuity.
/// @param[in,out] obj Player to seek.
/// @param[in] time Requested target-clip time in seconds.
/// @details Looping clips wrap the requested time. Non-looping clips clamp to
/// their duration, and a missing clip accepts any nonnegative float-range time.
/// Seeking does not change the playing flag or discard an active crossfade.
void rt_anim_player3d_set_time(void *obj, double time) {
    rt_anim_player3d *p = anim_player3d_checked(obj);
    if (p) {
        rt_animation3d *current = animation3d_clip_slot((void **)&p->current);
        if (!isfinite(time))
            time = 0.0;
        if (current && current->duration > 0.0f && anim_player_current_looping(p)) {
            p->current_time =
                animation3d_wrap_time(skeleton3d_clamp_to_float(time, 0.0f), current->duration);
        } else {
            if (time < 0.0)
                time = 0.0;
            if (current && current->duration > 0.0f && time > current->duration)
                time = current->duration;
            p->current_time = skeleton3d_float_or(time, 0.0f);
        }
        p->has_prev_motion_palette = 0;
        p->last_motion_frame = 0;
        if (current)
            compute_bone_palette(p);
        else
            compute_player_bind_pose(p);
    }
}

/// @brief Promote last frame's pose into the "previous" slot for motion-blur shaders.
///
/// Called at the start of each frame's draw — promotes the last
/// snapshot to `prev_bone_palette` and snapshots the current pose
/// into `motion_palette_snapshot` for the *next* frame to consume.
/// Returns the previous palette (or NULL if no prior frame yet).
///
/// @param[in,out] p Player whose temporal palettes are rotated.
/// @param[in] frame_serial Canvas frame identifier; repeated draws with the
/// same identifier do not rotate the buffers again.
/// @return The borrowed previous-frame palette after at least two distinct
/// frame snapshots, otherwise `NULL`.
static const float *anim_player_prepare_prev_palette(rt_anim_player3d *p, int64_t frame_serial) {
    rt_skeleton3d *skel;
    if (!p || !p->bone_palette)
        return NULL;
    skel = animation3d_skeleton_slot((void **)&p->skeleton);
    int32_t bone_count = skeleton3d_safe_bone_count(skel);
    if (skel)
        skel->bone_count = bone_count;
    if (!anim_player3d_ensure_pose_capacity(p, bone_count))
        return NULL;
    size_t palette_size = (size_t)bone_count * 16 * sizeof(float);
    if (p->last_motion_frame != frame_serial) {
        if (p->prev_bone_palette && p->motion_palette_snapshot && bone_count > 0 &&
            p->last_motion_frame != 0) {
            memcpy(p->prev_bone_palette, p->motion_palette_snapshot, palette_size);
            p->has_prev_motion_palette = 1;
        } else if (!p->prev_bone_palette) {
            p->has_prev_motion_palette = 0;
        }
        if (p->motion_palette_snapshot && bone_count > 0)
            memcpy(p->motion_palette_snapshot, p->bone_palette, palette_size);
        p->last_motion_frame = frame_serial;
    }
    return p->has_prev_motion_palette ? p->prev_bone_palette : NULL;
}

/// @brief Copy one bone's current model-space matrix into a new runtime Mat4.
/// @param[in,out] obj Player containing the evaluated pose.
/// @param[in] bone_index Zero-based skeleton bone index.
/// @return A newly allocated matrix object, or `NULL` for an invalid player,
/// skeleton, index, or global-pose buffer.
/// @details The returned matrix is relative to the skeleton origin; applying
/// the rendered model transform is the caller's responsibility.
void *rt_anim_player3d_get_bone_matrix(void *obj, int64_t bone_index) {
    rt_anim_player3d *p = anim_player3d_checked(obj);
    rt_skeleton3d *skel;
    int32_t bone_count;
    if (!p)
        return NULL;
    skel = animation3d_skeleton_slot((void **)&p->skeleton);
    if (!skel)
        return NULL;
    bone_count = skeleton3d_safe_bone_count(skel);
    skel->bone_count = bone_count;
    if (bone_index < 0 || bone_index >= bone_count)
        return NULL;
    if (!p->globals_buf)
        compute_bone_palette(p);
    if (!p->globals_buf)
        return NULL;
    const float *m = &p->globals_buf[bone_index * 16];
    return rt_mat4_new(m[0],
                       m[1],
                       m[2],
                       m[3],
                       m[4],
                       m[5],
                       m[6],
                       m[7],
                       m[8],
                       m[9],
                       m[10],
                       m[11],
                       m[12],
                       m[13],
                       m[14],
                       m[15]);
}
