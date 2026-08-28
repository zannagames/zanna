//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/anim/rt_skeleton3d_player.inc
// Purpose: AnimPlayer3D playback, crossfading, pose evaluation, and palette output.
// Key invariants:
//   - Playback clocks remain finite and bounded for looping and non-looping clips.
//   - All current, prior, local, global, and snapshot buffers share one valid capacity.
// Ownership/Lifetime:
//   - Included by rt_skeleton3d.c; this is not a standalone translation unit.
//   - Players retain their skeleton/clips and own every current and temporal pose buffer.
// Links: rt_skeleton3d.c, rt_skeleton3d.h, rt_skeleton3d_internal.h
//
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
    animation3d_release_class_ref((void **)&p->owned_skeleton, RT_G3D_SKELETON3D_CLASS_ID);
    animation3d_release_class_ref((void **)&p->owned_current, RT_G3D_ANIMATION3D_CLASS_ID);
    animation3d_release_class_ref((void **)&p->owned_crossfade_from, RT_G3D_ANIMATION3D_CLASS_ID);
    p->skeleton = NULL;
    p->current = NULL;
    p->crossfade_from = NULL;
    free(p->owned_bone_palette);
    p->owned_bone_palette = NULL;
    p->bone_palette = NULL;
    free(p->owned_prev_bone_palette);
    p->owned_prev_bone_palette = NULL;
    p->prev_bone_palette = NULL;
    free(p->owned_motion_palette_snapshot);
    p->owned_motion_palette_snapshot = NULL;
    p->motion_palette_snapshot = NULL;
    free(p->owned_local_transforms);
    p->owned_local_transforms = NULL;
    p->local_transforms = NULL;
    free(p->owned_globals_buf);
    p->owned_globals_buf = NULL;
    p->globals_buf = NULL;
    free(p->owned_crossfade_from_pose);
    p->owned_crossfade_from_pose = NULL;
    p->crossfade_from_pose = NULL;
    p->owned_from_pose_capacity = 0;
    p->crossfade_from_is_pose = 0;
    p->owned_pose_capacity = 0;
    p->pose_capacity = 0;
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

/// @brief Ensure every player pose buffer can hold a requested skeleton.
/// @param[in,out] p Player whose synchronized buffers may be enlarged.
/// @param[in] bone_count Required number of bone matrices.
/// @return Nonzero when all buffers have sufficient capacity; otherwise zero.
static int anim_player3d_ensure_pose_capacity(rt_anim_player3d *p, int32_t bone_count) {
    float *staged_bone_palette;
    float *staged_prev_palette;
    float *staged_snapshot;
    float *staged_locals;
    float *staged_globals;
    size_t old_floats;
    size_t new_floats;
    int reset_motion_history;
    if (!p)
        return 0;
    anim_player3d_repair_storage(p);
    int32_t new_capacity = skeleton3d_pose_capacity_for_count(bone_count);
    int32_t old_capacity = p->owned_pose_capacity > 0 ? p->owned_pose_capacity : 0;
    if (new_capacity <= old_capacity && p->owned_bone_palette && p->owned_prev_bone_palette &&
        p->owned_motion_palette_snapshot && p->owned_local_transforms && p->owned_globals_buf)
        return 1;
    if (new_capacity < old_capacity)
        new_capacity = old_capacity;
    reset_motion_history = !p->owned_prev_bone_palette || !p->owned_motion_palette_snapshot;
    old_floats = (size_t)old_capacity * 16u;
    new_floats = (size_t)new_capacity * 16u;
    if (new_floats > SIZE_MAX / sizeof(float))
        return 0;
    staged_bone_palette = (float *)calloc(new_floats, sizeof(float));
    staged_prev_palette = (float *)calloc(new_floats, sizeof(float));
    staged_snapshot = (float *)calloc(new_floats, sizeof(float));
    staged_locals = (float *)calloc(new_floats, sizeof(float));
    staged_globals = (float *)calloc(new_floats, sizeof(float));
    if (!staged_bone_palette || !staged_prev_palette || !staged_snapshot || !staged_locals ||
        !staged_globals) {
        free(staged_bone_palette);
        free(staged_prev_palette);
        free(staged_snapshot);
        free(staged_locals);
        free(staged_globals);
        return 0;
    }
    if (old_floats > 0) {
        if (p->owned_bone_palette)
            memcpy(staged_bone_palette, p->owned_bone_palette, old_floats * sizeof(float));
        if (p->owned_prev_bone_palette)
            memcpy(staged_prev_palette, p->owned_prev_bone_palette, old_floats * sizeof(float));
        if (p->owned_motion_palette_snapshot)
            memcpy(staged_snapshot, p->owned_motion_palette_snapshot, old_floats * sizeof(float));
        if (p->owned_local_transforms)
            memcpy(staged_locals, p->owned_local_transforms, old_floats * sizeof(float));
        if (p->owned_globals_buf)
            memcpy(staged_globals, p->owned_globals_buf, old_floats * sizeof(float));
    }
    free(p->owned_bone_palette);
    free(p->owned_prev_bone_palette);
    free(p->owned_motion_palette_snapshot);
    free(p->owned_local_transforms);
    free(p->owned_globals_buf);
    p->owned_bone_palette = staged_bone_palette;
    p->owned_prev_bone_palette = staged_prev_palette;
    p->owned_motion_palette_snapshot = staged_snapshot;
    p->owned_local_transforms = staged_locals;
    p->owned_globals_buf = staged_globals;
    p->owned_pose_capacity = new_capacity;
    if (reset_motion_history) {
        p->has_prev_motion_palette = 0;
        p->motion_history_initialized = 0;
    }
    anim_player3d_repair_storage(p);
    return 1;
}

/// @brief ADR 0302: ensure the frozen-pose crossfade buffer can hold @p bone_count bones.
/// @details Lazily allocated on the first freeze so players that never opt in pay
///   nothing. Grows with the pose capacity; contents are irrelevant after a grow
///   because a freeze always rewrites every populated slot.
/// @param[in,out] p Player whose frozen-pose buffer may be (re)allocated.
/// @param[in] bone_count Required bone slots.
/// @return Nonzero when the buffer holds at least @p bone_count slots.
static int anim_player3d_ensure_from_pose(rt_anim_player3d *p, int32_t bone_count) {
    int32_t capacity;
    size_t floats;
    float *staged;
    if (!p)
        return 0;
    anim_player3d_repair_storage(p);
    capacity = skeleton3d_pose_capacity_for_count(bone_count);
    if (p->owned_crossfade_from_pose && p->owned_from_pose_capacity >= capacity)
        return 1;
    if (capacity < p->owned_from_pose_capacity)
        capacity = p->owned_from_pose_capacity;
    floats = (size_t)capacity * 16u;
    if (floats > SIZE_MAX / sizeof(float))
        return 0;
    staged = (float *)calloc(floats, sizeof(float));
    if (!staged)
        return 0;
    free(p->owned_crossfade_from_pose);
    p->owned_crossfade_from_pose = staged;
    p->owned_from_pose_capacity = capacity;
    anim_player3d_repair_storage(p);
    return 1;
}

/// @brief ADR 0302: capture the CURRENT blended local pose as the crossfade source.
/// @details Decomposes every bone's local matrix into TRS into the frozen-pose buffer,
///   marks the pose as the fade source, and releases any outgoing clip. The fade that
///   follows therefore departs from exactly what was rendered last — the in-flight
///   blend, the held last frame of a finished one-shot, or the bind pose after Stop.
/// @param[in,out] p Player whose visible pose is frozen.
/// @return Nonzero when the pose was captured.
static int anim_player3d_freeze_current_pose(rt_anim_player3d *p) {
    rt_skeleton3d *skel = p ? animation3d_skeleton_slot((void **)&p->skeleton) : NULL;
    int32_t bone_count = skeleton3d_safe_bone_count(skel);
    if (!p || !skel || bone_count <= 0 || !p->local_transforms)
        return 0;
    if (!anim_player3d_ensure_from_pose(p, bone_count))
        return 0;
    for (int32_t bone = 0; bone < bone_count; bone++) {
        float *slot = &p->crossfade_from_pose[bone * 16];
        mat4f_decompose_trs(&p->local_transforms[bone * 16], &slot[0], &slot[3], &slot[7]);
    }
    animation3d_assign_owned_class_ref((void **)&p->owned_crossfade_from,
                                       (void **)&p->crossfade_from,
                                       NULL,
                                       RT_G3D_ANIMATION3D_CLASS_ID);
    p->crossfade_from_is_pose = 1;
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
    p->owned_skeleton = skel;
    rt_obj_retain_maybe(skeleton);
    p->current = NULL;
    p->owned_current = NULL;
    p->crossfade_from = NULL;
    p->owned_crossfade_from = NULL;
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
    p->motion_history_initialized = 0;
    p->crossfade_from_pose = NULL;
    p->owned_crossfade_from_pose = NULL;
    p->owned_from_pose_capacity = 0;
    p->crossfade_from_is_pose = 0;
    p->transition_continuity = 0;

    p->pose_capacity = skeleton3d_pose_capacity_for_count(bone_count);
    size_t palette_size = (size_t)p->pose_capacity * 16u * sizeof(float);
    p->bone_palette = (float *)calloc(1, palette_size);
    p->prev_bone_palette = (float *)calloc(1, palette_size);
    p->motion_palette_snapshot = (float *)calloc(1, palette_size);
    p->local_transforms = (float *)calloc(1, palette_size);
    p->globals_buf = (float *)calloc(1, palette_size);
    p->owned_bone_palette = p->bone_palette;
    p->owned_prev_bone_palette = p->prev_bone_palette;
    p->owned_motion_palette_snapshot = p->motion_palette_snapshot;
    p->owned_local_transforms = p->local_transforms;
    p->owned_globals_buf = p->globals_buf;
    p->owned_pose_capacity = p->pose_capacity;
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
    animation3d_assign_owned_class_ref(
        (void **)&p->owned_current, (void **)&p->current, anim, RT_G3D_ANIMATION3D_CLASS_ID);
    p->current_time = 0.0f;
    p->playing = anim ? 1 : 0;
    animation3d_assign_owned_class_ref((void **)&p->owned_crossfade_from,
                                       (void **)&p->crossfade_from,
                                       NULL,
                                       RT_G3D_ANIMATION3D_CLASS_ID);
    p->crossfade_from_is_pose = 0;
    p->crossfade_time = 0.0f;
    p->crossfade_duration = 0.0f;
    p->crossfade_from_time = 0.0f;
    p->has_prev_motion_palette = 0;
    p->motion_history_initialized = 0;
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
    {
        /* ADR 0302: with continuity on, a fade issued while another fade is in
         * flight, or from a clip that has finished (or been stopped), departs from
         * the FROZEN visible pose. A retrigger from a still-playing clip with no
         * fade in flight keeps the moving-clip source (the historical path). */
        int mid_fade = (animation3d_clip_slot((void **)&p->crossfade_from) != NULL) ||
                       p->crossfade_from_is_pose;
        int frozen = 0;
        if (p->transition_continuity && (mid_fade || !p->playing))
            frozen = anim_player3d_freeze_current_pose(p);
        if (!frozen) {
            animation3d_assign_owned_class_ref((void **)&p->owned_crossfade_from,
                                               (void **)&p->crossfade_from,
                                               current,
                                               RT_G3D_ANIMATION3D_CLASS_ID);
            p->crossfade_from_is_pose = 0;
        }
    }
    p->crossfade_from_time = p->current_time;
    p->crossfade_from_speed = isfinite(p->speed) ? p->speed : 1.0f;
    p->crossfade_from_looping = anim_player_current_looping(p);
    animation3d_assign_owned_class_ref(
        (void **)&p->owned_current, (void **)&p->current, anim, RT_G3D_ANIMATION3D_CLASS_ID);
    p->current_time = 0.0f;
    p->crossfade_time = 0.0f;
    p->crossfade_duration = skeleton3d_clamp_nonnegative_float(duration, FLT_MAX);
    p->playing = 1;
    p->has_prev_motion_palette = 0;
    p->motion_history_initialized = 0;
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
    animation3d_assign_owned_class_ref((void **)&p->owned_crossfade_from,
                                       (void **)&p->crossfade_from,
                                       NULL,
                                       RT_G3D_ANIMATION3D_CLASS_ID);
    p->crossfade_from_is_pose = 0;
    p->crossfade_time = 0.0f;
    p->crossfade_duration = 0.0f;
    p->crossfade_from_time = 0.0f;
    p->has_prev_motion_palette = 0;
    p->motion_history_initialized = 0;
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
     * against bind pose instead of popping at transition boundaries. ADR 0302: the
     * source may be the frozen visible pose instead of a clip. */
    if ((crossfade_from || (p->crossfade_from_is_pose && p->crossfade_from_pose)) && current &&
        p->crossfade_duration > 0.0f) {
        float factor = skeleton3d_clamp01f(p->crossfade_time / p->crossfade_duration);
        int from_pose = (!crossfade_from && p->crossfade_from_is_pose) ? 1 : 0;
        /* Map bone -> channel index for each clip once (O(channels)) so the per-bone
         * blend below resolves channels with an O(1) lookup instead of the prior
         * O(channels) scan per bone (turning O(bones*channels) into O(bones+channels)).
         * Duplicate corrupt channels follow ordinary pose evaluation's last-channel-wins
         * behavior; a NULL channel reproduces the find-miss bind-pose fallback exactly. */
        int32_t from_ch[VGFX3D_MAX_SKELETON_BONES];
        int32_t to_ch[VGFX3D_MAX_SKELETON_BONES];
        int32_t mapped =
            bone_count < VGFX3D_MAX_SKELETON_BONES ? bone_count : VGFX3D_MAX_SKELETON_BONES;
        int32_t from_count = from_pose ? 0 : animation3d_safe_channel_count(crossfade_from);
        int32_t to_count = animation3d_safe_channel_count(current);
        for (int32_t b = 0; b < mapped; b++) {
            from_ch[b] = -1;
            to_ch[b] = -1;
        }
        for (int32_t cidx = 0; cidx < from_count; cidx++) {
            int32_t b = crossfade_from->channels[cidx].bone_index;
            if (b >= 0 && b < mapped)
                from_ch[b] = cidx;
        }
        for (int32_t cidx = 0; cidx < to_count; cidx++) {
            int32_t b = current->channels[cidx].bone_index;
            if (b >= 0 && b < mapped)
                to_ch[b] = cidx;
        }

        for (int32_t bone = 0; bone < bone_count; bone++) {
            const vgfx3d_anim_channel_t *from_channel =
                (!from_pose && bone < mapped && from_ch[bone] >= 0)
                    ? &crossfade_from->channels[from_ch[bone]]
                    : NULL;
            const vgfx3d_anim_channel_t *to_channel =
                (bone < mapped && to_ch[bone] >= 0) ? &current->channels[to_ch[bone]] : NULL;
            float from_pos[3], from_rot[4], from_scl[3];
            float to_pos[3], to_rot[4], to_scl[3];
            float blend_pos[3], blend_rot[4], blend_scl[3];
            if (from_pose && bone < p->owned_from_pose_capacity) {
                const float *slot = &p->crossfade_from_pose[bone * 16];
                memcpy(from_pos, &slot[0], 3 * sizeof(float));
                memcpy(from_rot, &slot[3], 4 * sizeof(float));
                memcpy(from_scl, &slot[7], 3 * sizeof(float));
            } else {
                sample_animation_channel_trs(from_channel,
                                             p->crossfade_from_time,
                                             &skel->bones[bone],
                                             from_pos,
                                             from_rot,
                                             from_scl);
            }
            sample_animation_channel_trs(
                to_channel, p->current_time, &skel->bones[bone], to_pos, to_rot, to_scl);
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
    int8_t target_hit_endpoint = 0;
    int8_t fade_complete = 0;
    if (!p)
        return;
    current = animation3d_clip_slot((void **)&p->current);
    if (!isfinite(delta_time) || delta_time < 0.0)
        return;
    crossfade_from = animation3d_clip_slot((void **)&p->crossfade_from);
    if (!current) {
        if (crossfade_from)
            animation3d_assign_owned_class_ref((void **)&p->owned_crossfade_from,
                                               (void **)&p->crossfade_from,
                                               NULL,
                                               RT_G3D_ANIMATION3D_CLASS_ID);
        p->playing = 0;
        compute_player_bind_pose(p);
        return;
    }
    if (!p->playing && !crossfade_from && !p->crossfade_from_is_pose)
        return;
    if (!isfinite(p->speed))
        p->speed = 1.0f;
    if (!isfinite(current->duration) || current->duration <= 0.0f)
        current->duration = 1.0f;

    if (p->playing) {
        p->current_time = animation3d_advance_clock(p->current_time,
                                                    delta_time,
                                                    p->speed,
                                                    current->duration,
                                                    anim_player_current_looping(p),
                                                    &target_hit_endpoint);
        if (target_hit_endpoint)
            p->playing = 0;
    }

    /* Update crossfade (a clip source advances its own clock; a frozen pose holds) */
    if (crossfade_from || p->crossfade_from_is_pose) {
        if (crossfade_from) {
            if (!isfinite(p->crossfade_from_speed))
                p->crossfade_from_speed = 1.0f;
            if (!isfinite(crossfade_from->duration) || crossfade_from->duration <= 0.0f)
                crossfade_from->duration = 1.0f;
            p->crossfade_from_time = animation3d_advance_clock(p->crossfade_from_time,
                                                               delta_time,
                                                               p->crossfade_from_speed,
                                                               crossfade_from->duration,
                                                               p->crossfade_from_looping,
                                                               NULL);
        }
        if (!isfinite(p->crossfade_duration) || p->crossfade_duration <= 0.0f) {
            fade_complete = 1;
        } else {
            if (!isfinite(p->crossfade_time) || p->crossfade_time < 0.0f)
                p->crossfade_time = 0.0f;
            if (delta_time >= (double)p->crossfade_duration - p->crossfade_time) {
                p->crossfade_time = p->crossfade_duration;
                fade_complete = 1;
            } else {
                p->crossfade_time = (float)((double)p->crossfade_time + delta_time);
            }
        }
        if (fade_complete) {
            animation3d_assign_owned_class_ref((void **)&p->owned_crossfade_from,
                                               (void **)&p->crossfade_from,
                                               NULL,
                                               RT_G3D_ANIMATION3D_CLASS_ID);
            p->crossfade_from_is_pose = 0;
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
    if (!p)
        return 1.0;
    if (!isfinite(p->speed))
        p->speed = 1.0f;
    return p->speed;
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
    rt_animation3d *current;
    if (!p)
        return 0.0;
    current = animation3d_clip_slot((void **)&p->current);
    if (!isfinite(p->current_time))
        p->current_time = 0.0f;
    if (current && (!isfinite(current->duration) || current->duration <= 0.0f))
        current->duration = 1.0f;
    if (current && !anim_player_current_looping(p)) {
        if (p->current_time < 0.0f)
            p->current_time = 0.0f;
        if (p->current_time > current->duration)
            p->current_time = current->duration;
    }
    return p->current_time;
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
        p->motion_history_initialized = 0;
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
    if (!p->motion_history_initialized || p->last_motion_frame != frame_serial) {
        if (p->prev_bone_palette && p->motion_palette_snapshot && bone_count > 0 &&
            p->motion_history_initialized) {
            memcpy(p->prev_bone_palette, p->motion_palette_snapshot, palette_size);
            p->has_prev_motion_palette = 1;
        } else if (!p->prev_bone_palette) {
            p->has_prev_motion_palette = 0;
        }
        if (p->motion_palette_snapshot && bone_count > 0)
            memcpy(p->motion_palette_snapshot, p->bone_palette, palette_size);
        p->last_motion_frame = frame_serial;
        p->motion_history_initialized = 1;
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
    if (!anim_player3d_ensure_pose_capacity(p, bone_count) || bone_index >= p->owned_pose_capacity)
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
