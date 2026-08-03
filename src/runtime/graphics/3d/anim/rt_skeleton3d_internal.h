//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/anim/rt_skeleton3d_internal.h
// Purpose: Internal shared animation/runtime structs for skeletal animation,
//   plus the internal keyed-skinning draw fast path. Layouts are private to the
//   anim runtime — not part of the public Graphics3D ABI.
//
// Key invariants:
//   - Private counts are clamped to their backing capacity before traversal.
//   - Bone names are exact retained strings and key times are stored as doubles.
//   - Draw palettes remain capped independently from the larger skeleton limit.
// Ownership/Lifetime:
//   - The owning GC payload releases every private dynamic array and retained reference.
//   - Callers of internal helpers borrow handles unless a function documents a retain.
// Links: rt_skeleton3d.c, rt_skeleton3d.h, vgfx3d_skinning.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_skeleton3d_internal.h
 * @brief Declares private skeletal-animation storage and shared runtime helpers.
 *
 * The types in this header connect skeleton construction, animation sampling,
 * weighted blending, palette generation, and Canvas3D skinning. They are
 * implementation details rather than a stable public ABI. Dynamic counts must
 * be accessed through the bounded helpers below before indexing their backing
 * arrays.
 *
 * Skeletons retain bone-name strings and aliases; animation players and
 * blenders retain their skeletons and clips. Palette, local-transform, and
 * scratch arrays are owned by the enclosing runtime object.
 */
#pragma once

#include "rt_skeleton3d.h"

#ifdef ZANNA_ENABLE_GRAPHICS

/// Maximum number of bones addressable by one renderer draw palette.
#define VGFX3D_MAX_BONES 256
/* Skeleton-level bone ceiling: rigs may exceed the 256-slot draw palette; meshes
 * bound to such rigs are partitioned at import into sub-meshes whose bone sets fit
 * VGFX3D_MAX_BONES, remapped through rt_mesh3d::bone_map. */
/// Maximum number of bones stored in one runtime skeleton.
#define VGFX3D_MAX_SKELETON_BONES 1024
/// Fixed maximum number of clips registered with one weighted blender.
#define RT_ANIM_BLEND3D_MAX_STATES 8
/// Maximum number of per-bone channels stored by an animation.
#define RT_ANIMATION3D_MAX_CHANNELS VGFX3D_MAX_SKELETON_BONES
/// Defensive maximum number of keys stored in one animation channel.
#define RT_ANIMATION3D_MAX_KEYFRAMES_PER_CHANNEL 65536

/// @brief One bone: exact retained name, parent index (-1 = root), local bind-pose
///        matrix, precomputed inverse bind pose, and bind pose decomposed into TRS.
/// @details `name` owns one runtime-string reference for the lifetime of the containing
///          Skeleton3D. Keeping the length-carrying string prevents distinct imported
///          bone names from aliasing at a fixed prefix. The decomposed form is what
///          channel sampling consumes as its fallback; caching it at bone creation
///          removes a matrix decomposition (sqrt plus quaternion extraction) per bone,
///          per sample, per frame from the hot path.
typedef struct {
    /// Exact retained runtime string used for lookup and retargeting.
    rt_string name;
    /// Parent bone index, or `-1` for a root.
    int32_t parent_index;
    /// Row-major local bind-pose matrix.
    float bind_pose_local[16];
    /// Row-major inverse model-space bind matrix used for skinning.
    float inverse_bind[16];
    /// Cached local bind translation.
    float bind_pos[3];
    /// Cached normalized local bind rotation quaternion.
    float bind_rot[4];
    /// Cached local bind scale.
    float bind_scl[3];
} vgfx3d_bone_t;

/// @brief One animation keyframe: time plus position/rotation/scale samples, each gated
///   by a presence mask so unset channels fall through to neighboring keys.
typedef struct {
    /// Key time in seconds.
    double time;
    /// Local translation sample.
    float position[3];
    /// Local rotation quaternion sample.
    float rotation[4];
    /// Local scale sample.
    float scale_xyz[3];
    /// Nonzero when this key supplies a translation sample.
    uint8_t position_mask;
    /// Nonzero when this key supplies a rotation sample.
    uint8_t rotation_mask;
    /// Nonzero when this key supplies a scale sample.
    uint8_t scale_mask;
    /* CUBICSPLINE support: per-channel Hermite tangents (value units per second).
     * cubic_mask bit 0 = position, bit 1 = rotation, bit 2 = scale; a segment
     * plays back cubic only when both bracketing keys carry the channel bit. */
    /// Bit mask selecting translation, rotation, and scale cubic tangents.
    uint8_t cubic_mask;
    /// Incoming translation derivative in value units per second.
    float pos_in_tangent[3];
    /// Outgoing translation derivative in value units per second.
    float pos_out_tangent[3];
    /// Incoming quaternion-component derivative in value units per second.
    float rot_in_tangent[4];
    /// Outgoing quaternion-component derivative in value units per second.
    float rot_out_tangent[4];
    /// Incoming scale derivative in value units per second.
    float scale_in_tangent[3];
    /// Outgoing scale derivative in value units per second.
    float scale_out_tangent[3];
} vgfx3d_keyframe_t;

/// @brief Internal status-returning form of Animation3D keyframe insertion.
/// @details The public void API delegates here for compatibility. Asset importers use the status to
///          reject a transaction when channel/keyframe growth fails instead of publishing a
///          silently truncated animation. The function validates the same arguments and preserves
///          sorted/replacement semantics as `rt_animation3d_add_keyframe`.
/// @param[in,out] anim Animation3D object.
/// @param[in] bone_index Target bone index.
/// @param[in] time Finite key time in seconds.
/// @param[in] position Optional Vec3 position; `NULL` leaves that component absent.
/// @param[in] rotation Optional Quat rotation; `NULL` leaves that component absent.
/// @param[in] scale Optional Vec3 scale; `NULL` leaves that component absent.
/// @return Nonzero when the key was inserted or replaced; zero on invalid input or allocation/
///         capacity failure.
int rt_animation3d_try_add_keyframe(
    void *anim, int64_t bone_index, double time, void *position, void *rotation, void *scale);

/// @brief A per-bone animation channel: the target bone index and its growable,
///   time-sorted keyframe array.
typedef struct {
    /// Skeleton bone sampled by this channel.
    int32_t bone_index;
    /// Owned array of keyframes sorted by ascending time.
    vgfx3d_keyframe_t *keyframes;
    /// Logical number of initialized entries.
    int32_t keyframe_count;
    /// Number of allocated entries in `keyframes`.
    int32_t keyframe_capacity;
    /// Stable allocation identity used for growth and finalization.
    vgfx3d_keyframe_t *owned_keyframes;
    /// Actual number of entries allocated in `owned_keyframes`.
    int32_t owned_keyframe_capacity;
    /// Number of initialized keyframes independently of the mutable public mirror.
    int32_t initialized_keyframe_count;
} vgfx3d_anim_channel_t;

/// @brief External-name alias: "a rig calling this bone `external` means my
///   bone `local`". Consulted by animation retargeting before role inference.
typedef struct {
    /// Imported or source-rig name, stored as a truncated null-terminated string.
    char external[64];
    /// Corresponding name in this skeleton.
    char local[64];
} rt_skeleton3d_alias;

/// @brief Private payload of a runtime skeleton.
/// @details Bones may be added until `frozen` is set by creation of a pose
/// consumer. Global-pose builders tolerate non-topological bone order and
/// defensively break invalid parent relationships.
typedef struct rt_skeleton3d {
    /// Reserved runtime dispatch pointer.
    void *vptr;
    /// Owned growable bone array.
    vgfx3d_bone_t *bones;
    /// Logical bone count.
    int32_t bone_count;
    /// Allocated capacity of `bones`.
    int32_t bone_capacity;
    /// Nonzero once structural bone mutation is prohibited.
    int8_t frozen;
    /// Owned growable external-name mapping array.
    rt_skeleton3d_alias *aliases;
    /// Logical alias count.
    int32_t alias_count;
    /// Allocated capacity of `aliases`.
    int32_t alias_capacity;
    /// Stable allocation identity for the bone table.
    vgfx3d_bone_t *owned_bones;
    /// Actual capacity of `owned_bones`.
    int32_t owned_bone_capacity;
    /// Number of initialized bone records in `owned_bones`.
    int32_t initialized_bone_count;
    /// Stable allocation identity for the alias table.
    rt_skeleton3d_alias *owned_aliases;
    /// Actual capacity of `owned_aliases`.
    int32_t owned_alias_capacity;
    /// Number of initialized alias records in `owned_aliases`.
    int32_t initialized_alias_count;
} rt_skeleton3d;

/// @brief Resolve an external bone name through a skeleton's alias table
///        (defined in rt_skeleton3d_skeleton.inc). NULL when unmapped.
/// @param[in] s Skeleton whose aliases are searched.
/// @param[in] external Nonempty, case-sensitive external name.
/// @return The borrowed internal local-name string for the first match, or
/// `NULL` when the inputs are invalid or no alias exists.
const char *skeleton3d_alias_lookup(const rt_skeleton3d *s, const char *external);

/// @brief Animation3D payload: a named clip holding per-bone channels, total duration,
///   and the loop flag.
typedef struct rt_animation3d {
    /// Reserved runtime dispatch pointer.
    void *vptr;
    /// Null-terminated clip name truncated to 63 bytes.
    char name[64];
    /// Owned growable array of per-bone channels.
    vgfx3d_anim_channel_t *channels;
    /// Logical channel count.
    int32_t channel_count;
    /// Allocated capacity of `channels`.
    int32_t channel_capacity;
    /// Positive clip duration in seconds.
    float duration;
    /// Nonzero when playback wraps at the duration.
    int8_t looping;
    /// Stable allocation identity for the channel table.
    vgfx3d_anim_channel_t *owned_channels;
    /// Actual capacity of `owned_channels`.
    int32_t owned_channel_capacity;
    /// Number of initialized channel records in `owned_channels`.
    int32_t initialized_channel_count;
} rt_animation3d;

/// @brief AnimPlayer3D payload: the skeleton, current and crossfade-source clips with
///   their timers/speeds, loop overrides, and the current/previous/motion bone palettes
///   plus scratch transform buffers used while evaluating a pose.
typedef struct rt_anim_player3d {
    /// Reserved runtime dispatch pointer.
    void *vptr;
    /// Retained skeleton evaluated by this player.
    rt_skeleton3d *skeleton;
    /// Retained active clip, if any.
    rt_animation3d *current;
    /// Retained outgoing clip while a crossfade is active.
    rt_animation3d *crossfade_from;
    /// Current clip time in seconds.
    float current_time;
    /// Elapsed time within the active crossfade.
    float crossfade_time;
    /// Total crossfade duration in seconds.
    float crossfade_duration;
    /// Outgoing clip time in seconds.
    float crossfade_from_time;
    /// Captured outgoing clip playback speed.
    float crossfade_from_speed;
    /// Current clip playback speed.
    float speed;
    /// Nonzero while the current clip clock advances.
    int8_t playing;
    /// Nonzero when `loop_override_value` supersedes the clip flag.
    int8_t loop_override_enabled;
    /// Boolean looping value selected by the override.
    int8_t loop_override_value;
    /// Captured looping policy of the outgoing crossfade clip.
    int8_t crossfade_from_looping;
    /// Owned current skinning palette, one 4-by-4 matrix per bone.
    float *bone_palette;
    /// Owned palette exposed as the prior temporal frame.
    float *prev_bone_palette;
    /// Owned snapshot used to rotate temporal palettes once per frame.
    float *motion_palette_snapshot;
    /// Owned current local matrices.
    float *local_transforms;
    /// Owned model-space matrix scratch buffer.
    float *globals_buf;
    /// Bone slots allocated in every pose buffer.
    int32_t pose_capacity;
    /// Last canvas frame identifier used for temporal rotation.
    int64_t last_motion_frame;
    /// Nonzero after a valid previous-frame palette has been captured.
    int8_t has_prev_motion_palette;
    /// Stable retained skeleton identity, independent of the mutable mirror.
    rt_skeleton3d *owned_skeleton;
    /// Stable retained active-clip identity.
    rt_animation3d *owned_current;
    /// Stable retained outgoing-clip identity.
    rt_animation3d *owned_crossfade_from;
    /// Stable allocation identities for the five synchronized pose buffers.
    float *owned_bone_palette;
    float *owned_prev_bone_palette;
    float *owned_motion_palette_snapshot;
    float *owned_local_transforms;
    float *owned_globals_buf;
    /// Actual common capacity of all owned pose buffers.
    int32_t owned_pose_capacity;
    /// Nonzero after any frame serial, including zero, has been snapshotted.
    int8_t motion_history_initialized;
} rt_anim_player3d;

/// @brief One AnimBlend3D state: a named clip with its own weight, playback time, speed,
///   and loop flag — all states evaluated and weighted together each update.
typedef struct anim_blend_state_t {
    /// Null-terminated state name truncated to 63 bytes.
    char name[64];
    /// Retained clip evaluated by this state.
    rt_animation3d *animation;
    /// Nonnegative contribution to the normalized blended pose.
    float weight;
    /// Independent playback time in seconds.
    float anim_time;
    /// Independent playback multiplier.
    float speed;
    /// Captured looping policy for this state.
    int8_t looping;
    /// Stable retained clip identity, independent of the mutable mirror.
    rt_animation3d *owned_animation;
} anim_blend_state_t;

/// @brief AnimBlend3D payload: the skeleton, a fixed array of weighted blend states, and
///   the current/previous/motion bone palettes plus scratch transform buffers.
typedef struct rt_anim_blend3d {
    /// Reserved runtime dispatch pointer.
    void *vptr;
    /// Retained skeleton shared by every state.
    rt_skeleton3d *skeleton;
    /// Contiguous fixed-capacity state table.
    anim_blend_state_t states[RT_ANIM_BLEND3D_MAX_STATES];
    /// Logical number of initialized leading states.
    int32_t state_count;
    /// Owned current skinning palette.
    float *bone_palette;
    /// Owned palette exposed as the prior temporal frame.
    float *prev_bone_palette;
    /// Owned snapshot used to rotate temporal palettes once per frame.
    float *motion_palette_snapshot;
    /// Owned local-matrix output, also reused as temporary sampled TRS storage.
    float *local_transforms;
    /// Owned bind-pose TRS scratch array.
    float *temp_state_local;
    /// Owned running normalized TRS accumulator.
    float *blend_acc_trs; /* TRS-space blend accumulator (pos3/rot4/scl3 per 16-float slot) */
    /// Owned model-space matrix scratch buffer.
    float *globals_buf;
    /// Bone slots allocated in every pose buffer.
    int32_t pose_capacity;
    /// Last canvas frame identifier used for temporal rotation.
    int64_t last_motion_frame;
    /// Nonzero after a valid previous-frame palette has been captured.
    int8_t has_prev_motion_palette;
    /// Stable retained skeleton identity.
    rt_skeleton3d *owned_skeleton;
    /// Stable allocation identities for all synchronized pose buffers.
    float *owned_bone_palette;
    float *owned_prev_bone_palette;
    float *owned_motion_palette_snapshot;
    float *owned_local_transforms;
    float *owned_temp_state_local;
    float *owned_blend_acc_trs;
    float *owned_globals_buf;
    /// Actual common capacity of all owned pose buffers.
    int32_t owned_pose_capacity;
    /// Number of initialized leading state records.
    int32_t initialized_state_count;
    /// Nonzero after any frame serial, including zero, has been snapshotted.
    int8_t motion_history_initialized;
} rt_anim_blend3d;

/// @brief Restore Skeleton3D legacy mirrors from private ownership authority.
/// @details Stack fixtures without ownership metadata retain their legacy
/// mirrors. Heap-created skeletons publish ownership metadata with their first
/// allocation, after which pointer, count, and capacity repair is deterministic.
/// @param[in,out] skeleton Skeleton storage to repair.
static inline void skeleton3d_repair_storage(rt_skeleton3d *skeleton) {
    if (!skeleton)
        return;
    if (skeleton->owned_bones || skeleton->owned_bone_capacity > 0 ||
        skeleton->initialized_bone_count > 0) {
        if (!skeleton->owned_bones || skeleton->owned_bone_capacity <= 0) {
            skeleton->owned_bones = NULL;
            skeleton->owned_bone_capacity = 0;
            skeleton->initialized_bone_count = 0;
        } else {
            if (skeleton->owned_bone_capacity > VGFX3D_MAX_SKELETON_BONES)
                skeleton->owned_bone_capacity = VGFX3D_MAX_SKELETON_BONES;
            if (skeleton->initialized_bone_count < 0)
                skeleton->initialized_bone_count = 0;
            if (skeleton->initialized_bone_count > skeleton->owned_bone_capacity)
                skeleton->initialized_bone_count = skeleton->owned_bone_capacity;
        }
        skeleton->bones = skeleton->owned_bones;
        skeleton->bone_capacity = skeleton->owned_bone_capacity;
        skeleton->bone_count = skeleton->initialized_bone_count;
    }
    if (skeleton->owned_aliases || skeleton->owned_alias_capacity > 0 ||
        skeleton->initialized_alias_count > 0) {
        if (!skeleton->owned_aliases || skeleton->owned_alias_capacity <= 0) {
            skeleton->owned_aliases = NULL;
            skeleton->owned_alias_capacity = 0;
            skeleton->initialized_alias_count = 0;
        } else {
            if (skeleton->initialized_alias_count < 0)
                skeleton->initialized_alias_count = 0;
            if (skeleton->initialized_alias_count > skeleton->owned_alias_capacity)
                skeleton->initialized_alias_count = skeleton->owned_alias_capacity;
            for (int32_t i = 0; i < skeleton->initialized_alias_count; ++i) {
                skeleton->owned_aliases[i].external[63] = '\0';
                skeleton->owned_aliases[i].local[63] = '\0';
            }
        }
        skeleton->aliases = skeleton->owned_aliases;
        skeleton->alias_capacity = skeleton->owned_alias_capacity;
        skeleton->alias_count = skeleton->initialized_alias_count;
    }
    skeleton->frozen = skeleton->frozen ? 1 : 0;
}

/// @brief Restore one animation channel's keyframe mirrors from ownership authority.
/// @param[in,out] channel Channel storage to repair.
static inline void animation3d_repair_channel_storage(vgfx3d_anim_channel_t *channel) {
    if (!channel)
        return;
    if (channel->owned_keyframes || channel->owned_keyframe_capacity > 0 ||
        channel->initialized_keyframe_count > 0) {
        if (!channel->owned_keyframes || channel->owned_keyframe_capacity <= 0) {
            channel->owned_keyframes = NULL;
            channel->owned_keyframe_capacity = 0;
            channel->initialized_keyframe_count = 0;
        } else {
            if (channel->owned_keyframe_capacity > RT_ANIMATION3D_MAX_KEYFRAMES_PER_CHANNEL)
                channel->owned_keyframe_capacity = RT_ANIMATION3D_MAX_KEYFRAMES_PER_CHANNEL;
            if (channel->initialized_keyframe_count < 0)
                channel->initialized_keyframe_count = 0;
            if (channel->initialized_keyframe_count > channel->owned_keyframe_capacity)
                channel->initialized_keyframe_count = channel->owned_keyframe_capacity;
        }
        channel->keyframes = channel->owned_keyframes;
        channel->keyframe_capacity = channel->owned_keyframe_capacity;
        channel->keyframe_count = channel->initialized_keyframe_count;
    }
}

/// @brief Restore an Animation3D channel table and every initialized key table.
/// @param[in,out] animation Clip storage to repair.
static inline void animation3d_repair_storage(rt_animation3d *animation) {
    if (!animation)
        return;
    animation->name[63] = '\0';
    animation->looping = animation->looping ? 1 : 0;
    if (animation->owned_channels || animation->owned_channel_capacity > 0 ||
        animation->initialized_channel_count > 0) {
        if (!animation->owned_channels || animation->owned_channel_capacity <= 0) {
            animation->owned_channels = NULL;
            animation->owned_channel_capacity = 0;
            animation->initialized_channel_count = 0;
        } else {
            if (animation->owned_channel_capacity > RT_ANIMATION3D_MAX_CHANNELS)
                animation->owned_channel_capacity = RT_ANIMATION3D_MAX_CHANNELS;
            if (animation->initialized_channel_count < 0)
                animation->initialized_channel_count = 0;
            if (animation->initialized_channel_count > animation->owned_channel_capacity)
                animation->initialized_channel_count = animation->owned_channel_capacity;
            for (int32_t i = 0; i < animation->initialized_channel_count; ++i)
                animation3d_repair_channel_storage(&animation->owned_channels[i]);
        }
        animation->channels = animation->owned_channels;
        animation->channel_capacity = animation->owned_channel_capacity;
        animation->channel_count = animation->initialized_channel_count;
    }
}

/// @brief Restore an AnimPlayer3D's retained and buffer mirrors from stable identities.
/// @param[in,out] player Player storage to repair.
static inline void anim_player3d_repair_storage(rt_anim_player3d *player) {
    if (!player)
        return;
    player->skeleton = player->owned_skeleton;
    player->current = player->owned_current;
    player->crossfade_from = player->owned_crossfade_from;
    player->bone_palette = player->owned_bone_palette;
    player->prev_bone_palette = player->owned_prev_bone_palette;
    player->motion_palette_snapshot = player->owned_motion_palette_snapshot;
    player->local_transforms = player->owned_local_transforms;
    player->globals_buf = player->owned_globals_buf;
    player->pose_capacity = player->owned_pose_capacity;
    player->playing = player->playing ? 1 : 0;
    player->loop_override_enabled = player->loop_override_enabled ? 1 : 0;
    player->loop_override_value = player->loop_override_value ? 1 : 0;
    player->crossfade_from_looping = player->crossfade_from_looping ? 1 : 0;
    player->has_prev_motion_palette = player->has_prev_motion_palette ? 1 : 0;
    player->motion_history_initialized = player->motion_history_initialized ? 1 : 0;
}

/// @brief Restore an AnimBlend3D's retained, state, and buffer mirrors.
/// @param[in,out] blend Blender storage to repair.
static inline void anim_blend3d_repair_storage(rt_anim_blend3d *blend) {
    if (!blend)
        return;
    blend->skeleton = blend->owned_skeleton;
    blend->bone_palette = blend->owned_bone_palette;
    blend->prev_bone_palette = blend->owned_prev_bone_palette;
    blend->motion_palette_snapshot = blend->owned_motion_palette_snapshot;
    blend->local_transforms = blend->owned_local_transforms;
    blend->temp_state_local = blend->owned_temp_state_local;
    blend->blend_acc_trs = blend->owned_blend_acc_trs;
    blend->globals_buf = blend->owned_globals_buf;
    blend->pose_capacity = blend->owned_pose_capacity;
    if (blend->initialized_state_count < 0)
        blend->initialized_state_count = 0;
    if (blend->initialized_state_count > RT_ANIM_BLEND3D_MAX_STATES)
        blend->initialized_state_count = RT_ANIM_BLEND3D_MAX_STATES;
    blend->state_count = blend->initialized_state_count;
    for (int32_t i = 0; i < blend->initialized_state_count; ++i) {
        blend->states[i].animation = blend->states[i].owned_animation;
        blend->states[i].name[63] = '\0';
        blend->states[i].looping = blend->states[i].looping ? 1 : 0;
    }
    blend->has_prev_motion_palette = blend->has_prev_motion_palette ? 1 : 0;
    blend->motion_history_initialized = blend->motion_history_initialized ? 1 : 0;
}

/// @brief Bound a private dynamic-array count by its backing pointer and capacity.
/// @param[in] array Backing allocation, which must be nonnull for a positive result.
/// @param[in] count Stored logical element count.
/// @param[in] capacity Stored allocation capacity.
/// @return Zero for missing storage or nonpositive metadata; otherwise the
/// lesser of `count` and `capacity`.
static inline int32_t skeleton3d_clamped_array_count(const void *array,
                                                     int32_t count,
                                                     int32_t capacity) {
    if (!array || count <= 0 || capacity <= 0)
        return 0;
    return count < capacity ? count : capacity;
}

/// @brief Safe number of bones that may be read directly from a Skeleton3D.
/// @param[in] skeleton Skeleton whose array metadata is validated.
/// @return A nonnegative readable count capped by both allocated capacity and
/// `VGFX3D_MAX_SKELETON_BONES`.
static inline int32_t skeleton3d_safe_bone_count(const rt_skeleton3d *skeleton) {
    rt_skeleton3d *mutable_skeleton = (rt_skeleton3d *)(uintptr_t)skeleton;
    if (mutable_skeleton &&
        (mutable_skeleton->owned_bones || mutable_skeleton->owned_bone_capacity > 0 ||
         mutable_skeleton->initialized_bone_count > 0))
        skeleton3d_repair_storage(mutable_skeleton);
    int32_t count = skeleton ? skeleton3d_clamped_array_count(
                                   skeleton->bones, skeleton->bone_count, skeleton->bone_capacity)
                             : 0;
    return count < VGFX3D_MAX_SKELETON_BONES ? count : VGFX3D_MAX_SKELETON_BONES;
}

/// @brief Return a valid parent index for @p bone, or -1 when the stored parent is unusable.
/// @param[in] skeleton Skeleton containing the relationship.
/// @param[in] bone Child bone whose parent is requested.
/// @param[in] bone_count Valid readable bone count for this traversal.
/// @return A distinct parent index in `[0, bone_count)`, or `-1` for an invalid
/// skeleton, child, root, self-parent, or out-of-range parent.
static inline int32_t skeleton3d_valid_parent_index(const rt_skeleton3d *skeleton,
                                                    int32_t bone,
                                                    int32_t bone_count) {
    int32_t parent;
    if (!skeleton || !skeleton->bones || bone < 0 || bone >= bone_count)
        return -1;
    parent = skeleton->bones[bone].parent_index;
    if (parent < 0 || parent >= bone_count || parent == bone)
        return -1;
    return parent;
}

/// @brief Safe number of channels that may be read directly from an Animation3D.
/// @param[in] animation Clip whose channel metadata is validated.
/// @return A nonnegative readable count capped by allocated capacity and
/// `RT_ANIMATION3D_MAX_CHANNELS`.
static inline int32_t animation3d_safe_channel_count(const rt_animation3d *animation) {
    rt_animation3d *mutable_animation = (rt_animation3d *)(uintptr_t)animation;
    if (mutable_animation &&
        (mutable_animation->owned_channels || mutable_animation->owned_channel_capacity > 0 ||
         mutable_animation->initialized_channel_count > 0))
        animation3d_repair_storage(mutable_animation);
    int32_t count = animation ? skeleton3d_clamped_array_count(animation->channels,
                                                               animation->channel_count,
                                                               animation->channel_capacity)
                              : 0;
    return count < RT_ANIMATION3D_MAX_CHANNELS ? count : RT_ANIMATION3D_MAX_CHANNELS;
}

/// @brief Safe number of keyframes that may be read directly from one animation channel.
/// @param[in] channel Channel whose keyframe metadata is validated.
/// @return A nonnegative readable count capped by allocated capacity and
/// `RT_ANIMATION3D_MAX_KEYFRAMES_PER_CHANNEL`.
static inline int32_t animation3d_safe_keyframe_count(const vgfx3d_anim_channel_t *channel) {
    vgfx3d_anim_channel_t *mutable_channel = (vgfx3d_anim_channel_t *)(uintptr_t)channel;
    if (mutable_channel &&
        (mutable_channel->owned_keyframes || mutable_channel->owned_keyframe_capacity > 0 ||
         mutable_channel->initialized_keyframe_count > 0))
        animation3d_repair_channel_storage(mutable_channel);
    int32_t count = channel ? skeleton3d_clamped_array_count(channel->keyframes,
                                                             channel->keyframe_count,
                                                             channel->keyframe_capacity)
                            : 0;
    return count < RT_ANIMATION3D_MAX_KEYFRAMES_PER_CHANNEL
               ? count
               : RT_ANIMATION3D_MAX_KEYFRAMES_PER_CHANNEL;
}

/// @brief Whether every channel in @p animation can address a bone in @p skeleton.
/// @param[in] animation Clip whose readable channels are inspected.
/// @param[in] skeleton Skeleton defining the valid bone-index range.
/// @return Nonzero when both objects are valid and every readable channel
/// targets an existing bone; otherwise zero.
static inline int8_t animation3d_channels_fit_skeleton(const rt_animation3d *animation,
                                                       const rt_skeleton3d *skeleton) {
    if (!animation || !skeleton)
        return 0;
    int32_t bone_count = skeleton3d_safe_bone_count(skeleton);
    int32_t channel_count = animation3d_safe_channel_count(animation);
    for (int32_t i = 0; i < channel_count; i++) {
        int32_t bone = animation->channels[i].bone_index;
        if (bone < 0 || bone >= bone_count)
            return 0;
    }
    return 1;
}

/// @brief Safe number of fixed-capacity blend states that may be read from AnimBlend3D.
/// @param[in] blend Blender whose leading state entries are validated.
/// @return The lesser of the stored count and fixed capacity, truncated at the
/// first state without an animation; zero for invalid or empty input.
static inline int32_t animblend3d_safe_state_count(const rt_anim_blend3d *blend) {
    rt_anim_blend3d *mutable_blend = (rt_anim_blend3d *)(uintptr_t)blend;
    int32_t limit;
    int32_t count = 0;
    if (mutable_blend && (mutable_blend->owned_skeleton || mutable_blend->owned_bone_palette ||
                          mutable_blend->initialized_state_count > 0))
        anim_blend3d_repair_storage(mutable_blend);
    if (!blend || blend->state_count <= 0)
        return 0;
    limit = blend->state_count < RT_ANIM_BLEND3D_MAX_STATES ? blend->state_count
                                                            : RT_ANIM_BLEND3D_MAX_STATES;
    while (count < limit && blend->states[count].animation)
        count++;
    return count;
}

/// @brief Draw a skinned mesh using an explicit model matrix and a caller-
///        supplied keyed bone palette (internal fast path used by the
///        animation player to avoid recomputing the pose per draw).
/// @param[in,out] canvas Canvas receiving the draw command.
/// @param[in,out] mesh Mesh to skin and draw.
/// @param[in] model_matrix Row-major 4-by-4 model matrix in double precision.
/// @param[in] material Material applied to the draw.
/// @param[in] motion_key Stable identity used to associate temporal motion data.
/// @param[in] bone_palette Current skinning matrices.
/// @param[in] prev_bone_palette Optional previous-frame skinning matrices.
/// @param[in] bone_count Number of matrices available in each supplied palette.
void rt_canvas3d_draw_mesh_matrix_skinned_keyed(void *canvas,
                                                void *mesh,
                                                const double *model_matrix,
                                                void *material,
                                                const void *motion_key,
                                                const float *bone_palette,
                                                const float *prev_bone_palette,
                                                int32_t bone_count);
/// @brief Bounds-aware variant of the keyed skinned draw path.
/// @details Used by Scene3D after it has expanded local bounds for skeletal motion. The explicit
///   bounds are forwarded to Canvas3D for frustum/shadow fitting while CPU skinning and GPU
///   skinning selection remain unchanged.
/// @param[in,out] canvas Canvas receiving the draw command.
/// @param[in,out] mesh Mesh to skin and draw.
/// @param[in] model_matrix Row-major 4-by-4 model matrix in double precision.
/// @param[in] material Material applied to the draw.
/// @param[in] motion_key Stable identity used to associate temporal motion data.
/// @param[in] bone_palette Current skinning matrices.
/// @param[in] prev_bone_palette Optional previous-frame skinning matrices.
/// @param[in] bone_count Number of matrices available in each supplied palette.
/// @param[in] local_bounds_min Three-component lower local-space bound.
/// @param[in] local_bounds_max Three-component upper local-space bound.
/// @param[in] conservative_bounds Nonzero when the supplied bounds should be
/// treated as conservative animated coverage.
/// @param[in] disable_occlusion Nonzero to bypass occlusion culling for this draw.
void rt_canvas3d_draw_mesh_matrix_skinned_keyed_bounds(void *canvas,
                                                       void *mesh,
                                                       const double *model_matrix,
                                                       void *material,
                                                       const void *motion_key,
                                                       const float *bone_palette,
                                                       const float *prev_bone_palette,
                                                       int32_t bone_count,
                                                       const float *local_bounds_min,
                                                       const float *local_bounds_max,
                                                       int8_t conservative_bounds,
                                                       int8_t disable_occlusion);

/// @brief Queue instance_count copies of a skinned mesh, each posed by its own packed palette
///        (R18); GPU backends chunk palettes into shared-slot hardware draws, other paths fall
///        back to one skinned draw per instance.
/// @param[in,out] canvas Canvas receiving the instance draws.
/// @param[in,out] mesh_obj Mesh shared by every instance.
/// @param[in] material Material shared by every instance.
/// @param[in] instance_matrices Packed array of `instance_count` model matrices.
/// @param[in] instance_count Number of requested instances.
/// @param[in] bone_palettes Packed current palettes in instance-major order.
/// @param[in] prev_bone_palettes Optional packed previous palettes in
/// instance-major order.
/// @param[in] bone_count Number of matrices in each instance palette.
void rt_canvas3d_draw_mesh_instanced_skinned(void *canvas,
                                             void *mesh_obj,
                                             void *material,
                                             const float *instance_matrices,
                                             int32_t instance_count,
                                             const float *bone_palettes,
                                             const float *prev_bone_palettes,
                                             int32_t bone_count);

/// @brief Build global bone matrices from contiguous local matrices, tolerating non-topological
///        parent order and breaking parent cycles by treating the cycle edge as a root.
/// @param[in] skeleton Skeleton supplying parent relationships.
/// @param[in] locals Array of `bone_count` row-major local matrices.
/// @param[out] globals Destination for `bone_count` model-space matrices.
/// @param[in] bone_count Number of bones to evaluate.
void skeleton3d_compute_globals_from_locals(const rt_skeleton3d *skeleton,
                                            const float *locals,
                                            float *globals,
                                            int32_t bone_count);

#endif
