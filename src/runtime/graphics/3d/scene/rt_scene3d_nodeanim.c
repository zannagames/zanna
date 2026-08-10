//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/scene/rt_scene3d_nodeanim.c
// Purpose: Node-animation subsystem (glTF-style channel animation) for Scene3D —
//   NodeAnimation3D clips, NodeAnimator3D playback, channel sampling/application,
//   and morph-weight propagation. Split out of rt_scene3d.c; shares private
//   structs/helpers via rt_scene3d_internal.h.
// Links: rt_scene3d_internal.h, rt_morphtarget3d.h, rt_quat.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements imported node-animation clips and SceneNode3D channel playback.
/// @details Clips deep-copy bounded keyframe data; animators retain clips, cache
///   resolved subtree targets, reuse sampling/traversal scratch storage, and apply
///   validated TRS, morph-weight, and camera-property channels deterministically.

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_animcontroller3d.h"
#include "rt_box.h"
#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_g3d_ref_slots.h"
#include "rt_json.h"
#include "rt_map.h"
#include "rt_mat4.h"
#include "rt_morphtarget3d.h"
#include "rt_object.h"
#include "rt_physics3d.h"
#include "rt_pixels_internal.h"
#include "rt_quat.h"
#include "rt_scene3d.h"
#include "rt_scene3d_internal.h"
#include "rt_seq.h"
#include "rt_skeleton3d_internal.h"
#include "rt_sound3d.h"
#include "rt_string.h"
#include "rt_string_internal.h"
#include "rt_trap.h"
#include "rt_vec3.h"
#include "vgfx3d_frustum.h"
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NODE_ANIM_TIME_MAX 100000000.0
#define NODE_ANIM_SPEED_ABS_MAX 1000000.0
#define NODE_ANIM_KEY_COUNT_MAX 1000000
#define NODE_ANIM_VALUE_WIDTH_MAX 4096
#define NODE_ANIM_VALUE_COUNT_MAX 4000000
#define NODE_ANIM_NAME_BYTES_MAX 4096u
#define NODE_ANIM_TARGET_CACHE_MAX 1000000
#define NODE_ANIM_TRAVERSAL_NODE_MAX 1000000u

/// @brief Validate a runtime string used as an animation/target identifier.
/// @details Identifiers cross C-string lookup seams, so embedded NUL bytes are
///   rejected rather than silently aliasing a shorter name. A modest byte cap
///   bounds comparisons and malformed-asset work; strict UTF-8 keeps imported
///   names consistent with the scene/string serialization contract.
/// @param value Borrowed runtime string handle.
/// @param allow_empty Nonzero when an empty identifier is accepted.
/// @param out_data Optional output receiving the borrowed byte pointer.
/// @param out_length Optional output receiving the exact stored byte length.
/// @return Nonzero when the complete identifier is safe to use.
static int node_anim_string_view(rt_string value,
                                 int allow_empty,
                                 const char **out_data,
                                 size_t *out_length) {
    const char *data;
    size_t length;
    if (out_data)
        *out_data = NULL;
    if (out_length)
        *out_length = 0;
    if (!value || !rt_string_is_handle(value))
        return 0;
    data = rt_string_cstr(value);
    length = rt_string_len_bytes(value);
    if (!data || length > NODE_ANIM_NAME_BYTES_MAX || (!allow_empty && length == 0) ||
        memchr(data, '\0', length) != NULL || !rt_utf8_span_valid(data, length))
        return 0;
    if (out_data)
        *out_data = data;
    if (out_length)
        *out_length = length;
    return 1;
}

/// @brief Compare two validated animation identifiers by exact stored bytes.
/// @param lhs Borrowed first runtime string.
/// @param rhs Borrowed second runtime string.
/// @return Nonzero only when both are valid and byte-identical.
static int node_anim_string_equal(rt_string lhs, rt_string rhs) {
    const char *lhs_data;
    const char *rhs_data;
    size_t lhs_length;
    size_t rhs_length;
    return node_anim_string_view(lhs, 1, &lhs_data, &lhs_length) &&
           node_anim_string_view(rhs, 1, &rhs_data, &rhs_length) && lhs_length == rhs_length &&
           (lhs_length == 0 || memcmp(lhs_data, rhs_data, lhs_length) == 0);
}

/*==========================================================================
 * Imported node animation clips
 *=========================================================================*/

/// @brief Release a retained rt_string slot only if it still points at an rt_string handle.
/// @param slot Address of the retained runtime-string handle.
static void node_anim_release_string_slot(rt_string *slot) {
    if (!slot || !*slot)
        return;
    if (!rt_string_is_handle(*slot)) {
        rt_g3d_ref_slot_clear_unowned((void **)slot);
        return;
    }
    scene3d_release_ref((void **)slot);
}

/// @brief Release a retained NodeAnimation3D slot only if the handle still has that class.
/// @param slot Address of the retained clip pointer.
static void node_anim_release_clip_slot(rt_node_animation3d **slot) {
    if (!slot || !*slot)
        return;
    if (!rt_g3d_has_class(*slot, RT_G3D_NODEANIMATION3D_CLASS_ID)) {
        rt_g3d_ref_slot_clear_unowned((void **)slot);
        return;
    }
    scene3d_release_ref((void **)slot);
}

/// @brief Clear retained resolved-target cache entries and reset its key.
/// @param animator Borrowed animator whose cache is invalidated.
static void node_animator_clear_target_cache(rt_node_animator3d *animator);

/// @brief Compact an animator's clip table after private-state corruption.
/// @param animator Borrowed animator whose valid retained clips are compacted in place.
static void node_animator_repair_clips(rt_node_animator3d *animator) {
    int32_t count;
    int32_t write = 0;
    int changed = 0;
    if (!animator)
        return;
    count = scene3d_node_animator_clip_count(animator);
    for (int32_t read = 0; read < count; ++read) {
        rt_node_animation3d *clip = animator->animations[read];
        if (rt_g3d_has_class(clip, RT_G3D_NODEANIMATION3D_CLASS_ID)) {
            if (write != read)
                changed = 1;
            animator->animations[write++] = clip;
        } else {
            changed = 1;
            node_anim_release_clip_slot(&animator->animations[read]);
        }
    }
    int32_t kept = write;
    while (write < count)
        animator->animations[write++] = NULL;
    animator->animation_count = kept;
    if (animator->current_animation < 0 ||
        animator->current_animation >= animator->animation_count) {
        animator->current_animation = 0;
        changed = 1;
    }
    animator->playing = animator->playing ? 1 : 0;
    if (changed)
        node_animator_clear_target_cache(animator);
}

/// @brief Store @p target in the animator's channel-target cache slot,
///   retaining it and releasing any previous occupant.
/// @details Cached targets are STRONG references: a playing clip may outlive a
///   target node that was detached from the scene (remove_child releases the
///   parent's reference and can free the node). Retaining here guarantees the
///   cached pointer stays valid until the cache slot is overwritten, cleared,
///   or the animator is finalized — the descendant re-check in
///   node_anim_resolve_target then safely rejects detached nodes.
/// @param animator Borrowed animator owning the target-cache allocation.
/// @param channel_index Zero-based cache slot.
/// @param target Borrowed target retained into the slot, or `NULL` to clear it.
static void node_animator_cache_store(rt_node_animator3d *animator,
                                      int32_t channel_index,
                                      rt_scene_node3d *target) {
    rt_scene_node3d *previous;
    if (!animator || channel_index < 0 || channel_index >= animator->cached_target_capacity)
        return;
    if (target && !rt_g3d_has_class(target, RT_G3D_SCENENODE3D_CLASS_ID))
        target = NULL;
    previous = animator->cached_targets[channel_index];
    if (previous == target)
        return;
    if (target)
        rt_obj_retain_maybe(target);
    animator->cached_targets[channel_index] = target;
    if (rt_g3d_has_class(previous, RT_G3D_SCENENODE3D_CLASS_ID) && rt_obj_release_check0(previous))
        rt_obj_free(previous);
}

/// @brief Clear retained resolved-target cache entries without touching retained clips.
/// @param animator Borrowed animator whose cache entries and cache key are reset.
static void node_animator_clear_target_cache(rt_node_animator3d *animator) {
    if (!animator)
        return;
    if (!animator->cached_targets || animator->cached_target_capacity <= 0 ||
        animator->cached_target_capacity > NODE_ANIM_TARGET_CACHE_MAX) {
        free(animator->cached_targets);
        animator->cached_targets = NULL;
        animator->cached_target_capacity = 0;
        animator->cached_clip_index = -1;
        animator->cached_root = NULL;
        return;
    }
    if (animator->cached_targets && animator->cached_target_capacity > 0) {
        for (int32_t i = 0; i < animator->cached_target_capacity; i++) {
            rt_scene_node3d *entry = animator->cached_targets[i];
            if (rt_g3d_has_class(entry, RT_G3D_SCENENODE3D_CLASS_ID) &&
                rt_obj_release_check0(entry))
                rt_obj_free(entry);
        }
        memset(animator->cached_targets,
               0,
               (size_t)animator->cached_target_capacity * sizeof(*animator->cached_targets));
    }
    animator->cached_clip_index = -1;
    animator->cached_root = NULL;
}

/// @brief Ensure the per-animator channel-target cache can address @p channel_count channels.
/// @param animator Borrowed animator whose native cache allocation may grow.
/// @param channel_count Positive minimum pointer capacity.
/// @return Nonzero when sufficient cache storage is available, otherwise zero.
static int node_animator_ensure_target_cache(rt_node_animator3d *animator, int32_t channel_count) {
    rt_scene_node3d **next;
    int32_t next_capacity;
    if (!animator || channel_count <= 0 || channel_count > NODE_ANIM_TARGET_CACHE_MAX)
        return 0;
    if (!animator->cached_targets || animator->cached_target_capacity <= 0 ||
        animator->cached_target_capacity > NODE_ANIM_TARGET_CACHE_MAX) {
        if (animator->cached_targets)
            free(animator->cached_targets);
        animator->cached_targets = NULL;
        animator->cached_target_capacity = 0;
        animator->cached_clip_index = -1;
        animator->cached_root = NULL;
    }
    if (animator->cached_targets && animator->cached_target_capacity >= channel_count)
        return 1;
    next_capacity = animator->cached_target_capacity > 0 ? animator->cached_target_capacity : 16;
    while (next_capacity < channel_count) {
        if (next_capacity > INT32_MAX / 2) {
            next_capacity = channel_count;
            break;
        }
        next_capacity *= 2;
    }
    if ((size_t)next_capacity > SIZE_MAX / sizeof(*next))
        return 0;
    next = (rt_scene_node3d **)realloc(animator->cached_targets,
                                       (size_t)next_capacity * sizeof(*next));
    if (!next)
        return 0;
    if (next_capacity > animator->cached_target_capacity) {
        memset(&next[animator->cached_target_capacity],
               0,
               (size_t)(next_capacity - animator->cached_target_capacity) * sizeof(*next));
    }
    animator->cached_targets = next;
    animator->cached_target_capacity = next_capacity;
    return 1;
}

/// @brief Ensure a reusable float scratch buffer can hold @p width sampled channel values.
/// @param animator Borrowed animator owning the scratch allocation.
/// @param width Positive number of float lanes required.
/// @return Zeroed animator-owned buffer of at least @p width floats, or `NULL`.
static float *node_animator_sample_scratch(rt_node_animator3d *animator, int32_t width) {
    float *next;
    int32_t next_capacity;
    if (!animator || width <= 0 || width > NODE_ANIM_VALUE_WIDTH_MAX)
        return NULL;
    if (!animator->sample_scratch || animator->sample_scratch_capacity <= 0 ||
        animator->sample_scratch_capacity > NODE_ANIM_VALUE_WIDTH_MAX) {
        free(animator->sample_scratch);
        animator->sample_scratch = NULL;
        animator->sample_scratch_capacity = 0;
    }
    if (animator->sample_scratch && animator->sample_scratch_capacity >= width) {
        memset(animator->sample_scratch, 0, (size_t)width * sizeof(float));
        return animator->sample_scratch;
    }
    next_capacity = animator->sample_scratch_capacity > 0 ? animator->sample_scratch_capacity : 16;
    while (next_capacity < width) {
        if (next_capacity > INT32_MAX / 2) {
            next_capacity = width;
            break;
        }
        next_capacity *= 2;
    }
    if ((size_t)next_capacity > SIZE_MAX / sizeof(*next))
        return NULL;
    next = (float *)realloc(animator->sample_scratch, (size_t)next_capacity * sizeof(*next));
    if (!next)
        return NULL;
    animator->sample_scratch = next;
    animator->sample_scratch_capacity = next_capacity;
    memset(animator->sample_scratch, 0, (size_t)width * sizeof(float));
    return animator->sample_scratch;
}

/// @brief Push a node onto an animator-owned traversal stack, growing it if needed.
/// @param animator Borrowed animator owning the stack allocation.
/// @param count In/out populated stack count.
/// @param node Borrowed node to append.
/// @return Nonzero after append, otherwise zero on invalid input, overflow, or allocation failure.
static int node_animator_stack_push(rt_node_animator3d *animator,
                                    size_t *count,
                                    rt_scene_node3d *node) {
    rt_scene_node3d **next;
    size_t next_capacity;
    if (!animator || !count || !node || !rt_g3d_has_class(node, RT_G3D_SCENENODE3D_CLASS_ID) ||
        *count >= NODE_ANIM_TRAVERSAL_NODE_MAX)
        return 0;
    if (!animator->traversal_stack || animator->traversal_stack_capacity == 0 ||
        animator->traversal_stack_capacity > NODE_ANIM_TRAVERSAL_NODE_MAX) {
        free(animator->traversal_stack);
        animator->traversal_stack = NULL;
        animator->traversal_stack_capacity = 0;
    }
    if (*count >= animator->traversal_stack_capacity) {
        next_capacity =
            animator->traversal_stack_capacity > 0 ? animator->traversal_stack_capacity : 32;
        while (next_capacity <= *count) {
            if (next_capacity >= NODE_ANIM_TRAVERSAL_NODE_MAX || next_capacity > SIZE_MAX / 2)
                return 0;
            next_capacity *= 2;
        }
        if (next_capacity > NODE_ANIM_TRAVERSAL_NODE_MAX)
            next_capacity = NODE_ANIM_TRAVERSAL_NODE_MAX;
        if (next_capacity > SIZE_MAX / sizeof(rt_scene_node3d *))
            return 0;
        next = (rt_scene_node3d **)realloc(animator->traversal_stack,
                                           next_capacity * sizeof(rt_scene_node3d *));
        if (!next)
            return 0;
        animator->traversal_stack = next;
        animator->traversal_stack_capacity = next_capacity;
    }
    animator->traversal_stack[(*count)++] = node;
    return 1;
}

/// @brief Return a C string only for live rt_string handles.
/// @param value Borrowed candidate runtime-string handle.
/// @return Borrowed NUL-terminated contents, or `NULL`.
static const char *node_anim_cstr_or_null(rt_string value) {
    const char *data = NULL;
    return node_anim_string_view(value, 1, &data, NULL) ? data : NULL;
}

/// @brief GC finalizer for a NodeAnimation3D. Releases the clip name reference and
///        frees every channel's target name plus its times / values / in-tangent /
///        out-tangent buffers. Only CUBICSPLINE channels have live tangent buffers;
///        LINEAR / STEP leave those pointers NULL and `free(NULL)` is a no-op.
/// @param obj Owned NodeAnimation3D payload being finalized.
static void rt_node_animation3d_finalize(void *obj) {
    rt_node_animation3d *anim = (rt_node_animation3d *)obj;
    int32_t channel_count;
    if (!anim)
        return;
    node_anim_release_string_slot(&anim->name);
    channel_count = scene3d_node_animation_channel_count(anim);
    for (int32_t i = 0; i < channel_count; i++) {
        node_anim_release_string_slot(&anim->channels[i].target_name);
        free(anim->channels[i].times);
        free(anim->channels[i].values);
        free(anim->channels[i].in_tangents);
        free(anim->channels[i].out_tangents);
    }
    free(anim->channels);
    anim->channels = NULL;
    anim->channel_count = 0;
    anim->channel_capacity = 0;
}

/// @brief Allocate a NodeAnimation3D clip — the container for per-node TRS curves
///        imported from glTF (non-skeletal node animations).
/// @details Retains `name` so the caller can drop their reference safely. Clamps a
///          non-finite or non-positive `duration` to 1.0 so a malformed glTF asset
///          can't hand us a zero-length clip that would divide-by-zero during
///          looping playback. Looping is enabled by default — callers that want a
///          single-shot playback flip `looping` after construction.
/// @param name Borrowed clip-name string retained by the new clip.
/// @param duration Positive duration in seconds, sanitized and capped to the safe range.
/// @return Opaque clip handle, or NULL and traps on allocation failure.
void *rt_node_animation3d_new(rt_string name, double duration) {
    rt_node_animation3d *anim = (rt_node_animation3d *)rt_obj_new_i64(
        RT_G3D_NODEANIMATION3D_CLASS_ID, (int64_t)sizeof(rt_node_animation3d));
    if (!anim) {
        rt_trap("NodeAnimation3D.New: allocation failed");
        return NULL;
    }
    memset(anim, 0, sizeof(*anim));
    rt_obj_set_finalizer(anim, rt_node_animation3d_finalize);
    if (!node_anim_string_view(name, 1, NULL, NULL))
        name = rt_const_cstr("");
    rt_obj_retain_maybe(name);
    anim->name = name;
    anim->duration = (isfinite(duration) && duration > 0.0) ? duration : 1.0;
    if (anim->duration > NODE_ANIM_TIME_MAX)
        anim->duration = NODE_ANIM_TIME_MAX;
    anim->looping = 1;
    return anim;
}

/// @brief Grow the channel array on an animation clip to fit at least `needed` entries.
/// @details Doubling-growth starting at 4 channels on first allocation, with a direct
///          jump to `needed` when the doubled value would still be too small (e.g. a
///          bulk import calling with an exact count). New tail is zero-initialized so
///          uninitialized channel memory can't leak into the finalizer's free path.
///          Leaves the existing array untouched on allocation failure so callers can
///          continue using whatever was already there.
/// @param anim Borrowed clip whose native channel allocation may grow.
/// @param needed Non-negative minimum channel capacity.
/// @return 1 on success (including no-op), 0 on allocation failure.
static int node_animation_reserve_channels(rt_node_animation3d *anim, int32_t needed) {
    if (!anim || needed < 0)
        return 0;
    return scene3d_grow_array_i32(
        (void **)&anim->channels, &anim->channel_capacity, needed, 4, sizeof(*anim->channels), 1);
}

/// @brief Validate raw channel sample data before it is copied into a clip.
/// @details Enforces the invariants that `node_animation_add_channel_impl` depends on:
///   - `value_width` must match the path's dimensionality: 3 for TRANSLATION/SCALE,
///     4 for ROTATION, at least 1 for WEIGHTS/other (morph weight count varies).
///   - Every time sample must be finite and sorted. LINEAR/CUBICSPLINE require strictly
///     increasing times; STEP allows equal consecutive times per glTF's sampler rules.
///   - key_count × value_width overflow is checked before iterating so the loop bound
///     is always within size_t range.
///   - All value samples (and, for CUBICSPLINE, both tangent sets) must be finite;
///     non-finite values would produce NaN transforms that corrupt the scene graph.
/// @param path One `RT_NODE_ANIM_PATH_*` property identifier.
/// @param key_count Claimed number of keyframes.
/// @param value_width Claimed float-component count per key.
/// @param times Borrowed keyframe-time array.
/// @param values Borrowed packed output-value array.
/// @param in_tangents Borrowed packed incoming tangents for cubic interpolation.
/// @param out_tangents Borrowed packed outgoing tangents for cubic interpolation.
/// @param step Nonzero when equal adjacent key times are permitted.
/// @param cubic Nonzero when both tangent arrays are required and validated.
/// @return 1 if all data passes validation, 0 on any violation.
static int node_animation_validate_channel_data(int64_t path,
                                                int64_t key_count,
                                                int64_t value_width,
                                                const double *times,
                                                const float *values,
                                                const float *in_tangents,
                                                const float *out_tangents,
                                                int step,
                                                int cubic) {
    int64_t min_width = 1;
    if (path == RT_NODE_ANIM_PATH_TRANSLATION || path == RT_NODE_ANIM_PATH_SCALE)
        min_width = 3;
    else if (path == RT_NODE_ANIM_PATH_ROTATION)
        min_width = 4;
    if ((path == RT_NODE_ANIM_PATH_TRANSLATION || path == RT_NODE_ANIM_PATH_SCALE) &&
        value_width != 3)
        return 0;
    if (path == RT_NODE_ANIM_PATH_ROTATION && value_width != 4)
        return 0;
    if (path >= RT_NODE_ANIM_PATH_CAMERA_FOV && path <= RT_NODE_ANIM_PATH_LAST && value_width != 1)
        return 0;
    if (path == RT_NODE_ANIM_PATH_CAMERA_PROJECTION_MODE && !step)
        return 0;
    if (key_count <= 0 || key_count > NODE_ANIM_KEY_COUNT_MAX || value_width < min_width ||
        value_width > NODE_ANIM_VALUE_WIDTH_MAX || !times || !values)
        return 0;
    if (cubic && (!in_tangents || !out_tangents))
        return 0;
    if ((uint64_t)key_count > SIZE_MAX / (uint64_t)value_width)
        return 0;
    size_t value_count = (size_t)key_count * (size_t)value_width;
    if (value_count > NODE_ANIM_VALUE_COUNT_MAX)
        return 0;
    for (int64_t i = 0; i < key_count; i++) {
        if (!isfinite(times[i]) || times[i] < 0.0 || times[i] > NODE_ANIM_TIME_MAX)
            return 0;
        if (i > 0 && (times[i] < times[i - 1] || (!step && times[i] <= times[i - 1])))
            return 0;
    }
    for (size_t i = 0; i < value_count; i++) {
        if (!isfinite(values[i]))
            return 0;
        if (cubic && (!isfinite(in_tangents[i]) || !isfinite(out_tangents[i])))
            return 0;
    }
    return 1;
}

/// @brief Cheap per-frame guard for channels that may have been privately corrupted after import.
/// @param channel Borrowed channel descriptor and sample-storage pointers.
/// @return Nonzero when bounded structural invariants permit safe runtime sampling.
static int node_anim_channel_runtime_valid(const rt_node_anim_channel3d *channel) {
    int64_t value_count;
    if (!channel || !channel->target_name || !channel->times || !channel->values ||
        channel->key_count <= 0 || channel->value_width <= 0 ||
        channel->key_count > NODE_ANIM_KEY_COUNT_MAX ||
        channel->value_width > NODE_ANIM_VALUE_WIDTH_MAX)
        return 0;
    if (!node_anim_string_view(channel->target_name, 0, NULL, NULL))
        return 0;
    if (channel->path < RT_NODE_ANIM_PATH_TRANSLATION || channel->path > RT_NODE_ANIM_PATH_LAST)
        return 0;
    if (channel->interpolation < RT_NODE_ANIM_INTERP_LINEAR ||
        channel->interpolation > RT_NODE_ANIM_INTERP_CUBICSPLINE)
        return 0;
    if ((channel->path == RT_NODE_ANIM_PATH_TRANSLATION ||
         channel->path == RT_NODE_ANIM_PATH_SCALE) &&
        channel->value_width != 3)
        return 0;
    if (channel->path == RT_NODE_ANIM_PATH_ROTATION && channel->value_width != 4)
        return 0;
    if (channel->path >= RT_NODE_ANIM_PATH_CAMERA_FOV && channel->path <= RT_NODE_ANIM_PATH_LAST &&
        channel->value_width != 1)
        return 0;
    if (channel->path == RT_NODE_ANIM_PATH_CAMERA_PROJECTION_MODE &&
        channel->interpolation != RT_NODE_ANIM_INTERP_STEP)
        return 0;
    if (channel->interpolation == RT_NODE_ANIM_INTERP_CUBICSPLINE &&
        (!channel->in_tangents || !channel->out_tangents))
        return 0;
    if ((uint64_t)channel->key_count > SIZE_MAX / (uint64_t)channel->value_width)
        return 0;
    value_count = (int64_t)channel->key_count * (int64_t)channel->value_width;
    if (value_count <= 0 || value_count > NODE_ANIM_VALUE_COUNT_MAX ||
        (uint64_t)channel->key_count > SIZE_MAX / sizeof(double) ||
        (uint64_t)value_count > SIZE_MAX / sizeof(float))
        return 0;
    if (!isfinite(channel->times[0]) || !isfinite(channel->times[channel->key_count - 1]) ||
        channel->times[0] < 0.0 || channel->times[channel->key_count - 1] > NODE_ANIM_TIME_MAX)
        return 0;
    return 1;
}

/// @brief Verify a bounded float sample span before interpolation/publication.
/// @param values Borrowed float array.
/// @param count Positive lane count no larger than one channel width.
/// @return Nonzero when every requested lane is finite.
static int node_anim_float_span_finite(const float *values, int32_t count) {
    if (!values || count <= 0 || count > NODE_ANIM_VALUE_WIDTH_MAX)
        return 0;
    for (int32_t i = 0; i < count; ++i) {
        if (!isfinite(values[i]))
            return 0;
    }
    return 1;
}

/// @brief Validate one retained key time and its immediate ordering constraints.
/// @param channel Borrowed structurally valid channel.
/// @param index Zero-based time index.
/// @return Nonzero when the key is finite, bounded, and locally ordered.
static int node_anim_key_time_valid(const rt_node_anim_channel3d *channel, int32_t index) {
    double time;
    int step;
    if (!channel || !channel->times || index < 0 || index >= channel->key_count)
        return 0;
    time = channel->times[index];
    step = channel->interpolation == RT_NODE_ANIM_INTERP_STEP;
    if (!isfinite(time) || time < 0.0 || time > NODE_ANIM_TIME_MAX)
        return 0;
    if (index > 0) {
        double previous = channel->times[index - 1];
        if (!isfinite(previous) || time < previous || (!step && time <= previous))
            return 0;
    }
    if (index + 1 < channel->key_count) {
        double next = channel->times[index + 1];
        if (!isfinite(next) || next < time || (!step && next <= time))
            return 0;
    }
    return 1;
}

/// @brief Add one channel (one animated property on one target node) to an animation
///        clip, taking defensive copies of the sample data.
/// @details Validation the caller doesn't have to repeat:
///          - Rejects a NULL target name, non-positive key count, non-positive value
///            width, or NULL times / values buffer.
///          - CUBICSPLINE channels require both in-tangent and out-tangent buffers.
///          - Path must be a valid TRS-or-weights selector.
///          - `key_count * value_width` is bounds-checked against `SIZE_MAX / sizeof(float)`
///            so a pathological exporter that claims billions of keys can't wrap the
///            multiplication into a small allocation.
///          The implementation deep-copies the times array (as `double`), the values
///          array, and — for CUBICSPLINE channels — the two tangent arrays, so the
///          caller can free the source buffers immediately after this returns.
/// @param obj Borrowed NodeAnimation3D handle.
/// @param target_name Borrowed nonempty target name retained by the channel.
/// @param path One `RT_NODE_ANIM_PATH_*` property identifier.
/// @param interpolation One `RT_NODE_ANIM_INTERP_*` mode.
/// @param key_count Positive bounded number of keyframes.
/// @param value_width Positive bounded float-component count per key.
/// @param times Borrowed array of @p key_count time samples.
/// @param values Borrowed packed output samples.
/// @param in_tangents Borrowed packed incoming tangents for cubic interpolation.
/// @param out_tangents Borrowed packed outgoing tangents for cubic interpolation.
/// @return The zero-based index of the new channel, or -1 on validation / allocation
///         failure.
static int64_t node_animation_add_channel_impl(void *obj,
                                               rt_string target_name,
                                               int64_t path,
                                               int64_t interpolation,
                                               int64_t key_count,
                                               int64_t value_width,
                                               const double *times,
                                               const float *values,
                                               const float *in_tangents,
                                               const float *out_tangents) {
    rt_node_animation3d *anim =
        (rt_node_animation3d *)rt_g3d_checked_or_null(obj, RT_G3D_NODEANIMATION3D_CLASS_ID);
    rt_node_anim_channel3d *channel;
    size_t time_bytes;
    size_t value_count;
    const char *target_cstr;
    if (!anim || !target_name || key_count <= 0 || value_width <= 0 || !times || !values)
        return -1;
    target_cstr = node_anim_cstr_or_null(target_name);
    if (!target_cstr || !node_anim_string_view(target_name, 0, NULL, NULL))
        return -1;
    if (interpolation < RT_NODE_ANIM_INTERP_LINEAR ||
        interpolation > RT_NODE_ANIM_INTERP_CUBICSPLINE)
        return -1;
    if (interpolation == RT_NODE_ANIM_INTERP_CUBICSPLINE && (!in_tangents || !out_tangents))
        return -1;
    if (path < RT_NODE_ANIM_PATH_TRANSLATION || path > RT_NODE_ANIM_PATH_LAST)
        return -1;
    if (key_count > INT32_MAX || value_width > INT32_MAX)
        return -1;
    if (!node_animation_validate_channel_data(path,
                                              key_count,
                                              value_width,
                                              times,
                                              values,
                                              in_tangents,
                                              out_tangents,
                                              interpolation == RT_NODE_ANIM_INTERP_STEP,
                                              interpolation == RT_NODE_ANIM_INTERP_CUBICSPLINE))
        return -1;
    value_count = (size_t)key_count * (size_t)value_width;
    if (value_count > SIZE_MAX / sizeof(float))
        return -1;
    anim->channel_count = scene3d_node_animation_channel_count(anim);
    if (!anim->channels)
        anim->channel_capacity = 0;
    if (anim->channel_count == INT32_MAX)
        return -1;
    if (!node_animation_reserve_channels(anim, anim->channel_count + 1))
        return -1;
    channel = &anim->channels[anim->channel_count];
    time_bytes = (size_t)key_count * sizeof(double);
    channel->times = (double *)malloc(time_bytes);
    channel->values = (float *)malloc(value_count * sizeof(float));
    if (interpolation == RT_NODE_ANIM_INTERP_CUBICSPLINE) {
        channel->in_tangents = (float *)malloc(value_count * sizeof(float));
        channel->out_tangents = (float *)malloc(value_count * sizeof(float));
    }
    if (!channel->times || !channel->values ||
        (interpolation == RT_NODE_ANIM_INTERP_CUBICSPLINE &&
         (!channel->in_tangents || !channel->out_tangents))) {
        free(channel->times);
        free(channel->values);
        free(channel->in_tangents);
        free(channel->out_tangents);
        memset(channel, 0, sizeof(*channel));
        return -1;
    }
    memcpy(channel->times, times, time_bytes);
    memcpy(channel->values, values, value_count * sizeof(float));
    if (interpolation == RT_NODE_ANIM_INTERP_CUBICSPLINE) {
        memcpy(channel->in_tangents, in_tangents, value_count * sizeof(float));
        memcpy(channel->out_tangents, out_tangents, value_count * sizeof(float));
    }
    rt_obj_retain_maybe(target_name);
    channel->target_name = target_name;
    channel->path = (int32_t)path;
    if (interpolation == RT_NODE_ANIM_INTERP_CUBICSPLINE)
        channel->interpolation = RT_NODE_ANIM_INTERP_CUBICSPLINE;
    else
        channel->interpolation = interpolation == RT_NODE_ANIM_INTERP_STEP
                                     ? RT_NODE_ANIM_INTERP_STEP
                                     : RT_NODE_ANIM_INTERP_LINEAR;
    channel->key_count = (int32_t)key_count;
    channel->value_width = (int32_t)value_width;
    channel->target_node_index = -1;
    return anim->channel_count++;
}

/// @brief Add a STEP or LINEAR channel to an animation clip (no tangent data required).
/// @details Thin wrapper around `node_animation_add_channel_impl` that passes NULL for
///   both tangent arrays. Only CUBICSPLINE interpolation needs tangents; for STEP and
///   LINEAR the tangent pointers are never read so passing NULL is safe and explicit.
///   Callers that want CUBICSPLINE must use `rt_node_animation3d_add_cubic_channel`.
/// @param obj           NodeAnimation3D clip handle.
/// @param target_name   Name of the scene node that this channel drives.
/// @param path          RT_NODE_ANIM_PATH_* constant (TRANSLATION/ROTATION/SCALE/WEIGHTS).
/// @param interpolation RT_NODE_ANIM_INTERP_STEP or RT_NODE_ANIM_INTERP_LINEAR.
/// @param key_count     Number of keyframes.
/// @param value_width   Floats per keyframe (3 for vec3, 4 for quat, N for weights).
/// @param times         Monotonically increasing time samples (seconds), length key_count.
/// @param values        Flattened sample values, length key_count * value_width.
/// @return Zero-based channel index on success, -1 on validation or allocation failure.
int64_t rt_node_animation3d_add_channel(void *obj,
                                        rt_string target_name,
                                        int64_t path,
                                        int64_t interpolation,
                                        int64_t key_count,
                                        int64_t value_width,
                                        const double *times,
                                        const float *values) {
    return node_animation_add_channel_impl(
        obj, target_name, path, interpolation, key_count, value_width, times, values, NULL, NULL);
}

/// @brief Add a CUBICSPLINE (Catmull-Rom / glTF cubic) channel to an animation clip.
/// @details Thin wrapper that hard-wires RT_NODE_ANIM_INTERP_CUBICSPLINE and passes
///   both tangent arrays through to `node_animation_add_channel_impl`. The tangent
///   arrays must each have the same length as `values` (key_count * value_width floats).
///   Validation inside the impl will reject NULL tangent pointers for cubic channels.
///   This separation from `rt_node_animation3d_add_channel` keeps the non-cubic hot
///   path free of tangent-buffer bookkeeping.
/// @param obj           NodeAnimation3D clip handle.
/// @param target_name   Name of the scene node driven by this channel.
/// @param path          RT_NODE_ANIM_PATH_* constant.
/// @param key_count     Number of keyframes.
/// @param value_width   Floats per keyframe.
/// @param times         Monotonically increasing time samples (seconds).
/// @param values        Flattened output sample values.
/// @param in_tangents   In-tangent per sample (same layout as @p values).
/// @param out_tangents  Out-tangent per sample (same layout as @p values).
/// @return Zero-based channel index on success, -1 on validation or allocation failure.
int64_t rt_node_animation3d_add_cubic_channel(void *obj,
                                              rt_string target_name,
                                              int64_t path,
                                              int64_t key_count,
                                              int64_t value_width,
                                              const double *times,
                                              const float *values,
                                              const float *in_tangents,
                                              const float *out_tangents) {
    return node_animation_add_channel_impl(obj,
                                           target_name,
                                           path,
                                           RT_NODE_ANIM_INTERP_CUBICSPLINE,
                                           key_count,
                                           value_width,
                                           times,
                                           values,
                                           in_tangents,
                                           out_tangents);
}

/// @brief Bind animation channel @p channel_index to scene-node @p node_index; an out-of-range
///   channel index is ignored, and an invalid @p node_index unbinds the channel (sets it to -1).
/// @param obj Borrowed NodeAnimation3D handle.
/// @param channel_index Zero-based channel index.
/// @param node_index Stable imported source-node index, or an invalid value to unbind.
void rt_node_animation3d_set_channel_target_node_index(void *obj,
                                                       int64_t channel_index,
                                                       int64_t node_index) {
    rt_node_animation3d *anim =
        (rt_node_animation3d *)rt_g3d_checked_or_null(obj, RT_G3D_NODEANIMATION3D_CLASS_ID);
    int32_t channel_count;
    if (!anim || channel_index < 0)
        return;
    channel_count = scene3d_node_animation_channel_count(anim);
    anim->channel_count = channel_count;
    if (channel_index >= channel_count)
        return;
    if (node_index < 0 || node_index > INT32_MAX)
        anim->channels[channel_index].target_node_index = -1;
    else
        anim->channels[channel_index].target_node_index = (int32_t)node_index;
    anim->channels[channel_index].cached_root = NULL;
    anim->channels[channel_index].cached_target = NULL;
}

/// @brief GC finalizer for a NodeAnimator3D — releases retained clip references
///        and frees the animations pointer array.
/// @details Each clip in `animations[]` was retained during construction via
///   `rt_obj_retain_maybe`; the matching release here ensures clips are not freed
///   while the animator is still alive, and are released precisely when the animator
///   itself is collected. The root node pointer is NOT released — the animator borrows
///   that reference from the scene, so releasing it here would cause a double-free.
/// @param obj Owned NodeAnimator3D payload being finalized.
static void rt_node_animator3d_finalize(void *obj) {
    rt_node_animator3d *animator = (rt_node_animator3d *)obj;
    int32_t clip_count;
    if (!animator)
        return;
    node_animator_repair_clips(animator);
    clip_count = scene3d_node_animator_clip_count(animator);
    for (int32_t i = 0; i < clip_count; i++)
        node_anim_release_clip_slot(&animator->animations[i]);
    free(animator->animations);
    node_animator_clear_target_cache(animator); /* releases retained targets */
    free(animator->cached_targets);
    free(animator->sample_scratch);
    free(animator->traversal_stack);
    animator->animations = NULL;
    animator->cached_targets = NULL;
    animator->sample_scratch = NULL;
    animator->traversal_stack = NULL;
    animator->animation_count = 0;
    animator->animation_capacity = 0;
    animator->cached_target_capacity = 0;
    animator->sample_scratch_capacity = 0;
    animator->traversal_stack_capacity = 0;
    animator->root = NULL;
}

/// @brief Allocate a NodeAnimator3D that owns a set of pre-loaded animation clips.
/// @details Allocates the animator object and a contiguous pointer array sized exactly
///   for @p clip_count entries. Each clip is retained so the animator keeps the clips
///   alive independent of the caller's own references. Defaults to playing back clip 0
///   at speed 1.0 with `playing = 1` — call rt_node_animator3d_set_speed /
///   rt_node_animator3d_play_clip after construction to override. The `root` field is
///   left NULL until the animator is bound to a scene node via
///   `rt_scene_node3d_bind_node_animator`.
/// @param clips      Array of NodeAnimation3D clip handles, must not be NULL.
/// @param clip_count Number of clips; must be in [1, INT32_MAX].
/// @return New animator handle, or NULL (with trap) on allocation failure.
void *rt_node_animator3d_new_from_clips(void **clips, int64_t clip_count) {
    rt_node_animator3d *animator;
    if (!clips || clip_count <= 0 || clip_count > INT32_MAX)
        return NULL;
    if ((size_t)clip_count > SIZE_MAX / sizeof(void *))
        return NULL;
    for (int32_t i = 0; i < (int32_t)clip_count; i++) {
        if (!rt_g3d_has_class(clips[i], RT_G3D_NODEANIMATION3D_CLASS_ID))
            return NULL;
    }
    animator = (rt_node_animator3d *)rt_obj_new_i64(RT_G3D_NODEANIMATOR3D_CLASS_ID,
                                                    (int64_t)sizeof(rt_node_animator3d));
    if (!animator) {
        rt_trap("NodeAnimator3D.New: allocation failed");
        return NULL;
    }
    memset(animator, 0, sizeof(*animator));
    rt_obj_set_finalizer(animator, rt_node_animator3d_finalize);
    animator->animations = (rt_node_animation3d **)calloc((size_t)clip_count, sizeof(void *));
    if (!animator->animations) {
        scene3d_release_ref((void **)&animator);
        return NULL;
    }
    for (int32_t i = 0; i < (int32_t)clip_count; i++) {
        rt_obj_retain_maybe(clips[i]);
        animator->animations[i] = (rt_node_animation3d *)clips[i];
    }
    animator->animation_count = (int32_t)clip_count;
    animator->animation_capacity = (int32_t)clip_count;
    animator->current_animation = 0;
    animator->cached_clip_index = -1;
    animator->speed = 1.0;
    animator->playing = 1;
    return animator;
}

/// @brief Allocate a NodeAnimator3D that owns a single NodeAnimation3D clip.
/// @param clip Borrowed NodeAnimation3D handle retained by the new animator.
/// @return New owned NodeAnimator3D handle, or `NULL` after invalid-input/allocation failure.
void *rt_node_animator3d_new(void *clip) {
    void *clips[1];
    if (!rt_g3d_has_class(clip, RT_G3D_NODEANIMATION3D_CLASS_ID)) {
        rt_trap("NodeAnimator3D.New: clip must be NodeAnimation3D");
        return NULL;
    }
    clips[0] = clip;
    return rt_node_animator3d_new_from_clips(clips, 1);
}

/// @brief Get a NodeAnimation3D's name, or an empty string for invalid handles.
/// @param obj Borrowed NodeAnimation3D handle.
/// @return Borrowed clip-name runtime string, or an empty runtime string.
rt_string rt_node_animation3d_get_name(void *obj) {
    rt_node_animation3d *anim =
        (rt_node_animation3d *)rt_g3d_checked_or_null(obj, RT_G3D_NODEANIMATION3D_CLASS_ID);
    return (anim && node_anim_string_view(anim->name, 1, NULL, NULL)) ? anim->name
                                                                      : rt_const_cstr("");
}

/// @brief Get a NodeAnimation3D's duration in seconds, clamped to the runtime-safe range.
/// @param obj Borrowed NodeAnimation3D handle.
/// @return Non-negative duration capped at `NODE_ANIM_TIME_MAX`, or zero.
double rt_node_animation3d_get_duration(void *obj) {
    rt_node_animation3d *anim =
        (rt_node_animation3d *)rt_g3d_checked_or_null(obj, RT_G3D_NODEANIMATION3D_CLASS_ID);
    double duration = anim ? anim->duration : 0.0;
    if (!isfinite(duration) || duration < 0.0)
        return 0.0;
    return duration > NODE_ANIM_TIME_MAX ? NODE_ANIM_TIME_MAX : duration;
}

/// @brief Get the number of valid channels retained by a NodeAnimation3D clip.
/// @param obj Borrowed NodeAnimation3D handle.
/// @return Safe non-negative channel count, or zero for invalid input.
int64_t rt_node_animation3d_get_channel_count(void *obj) {
    rt_node_animation3d *anim =
        (rt_node_animation3d *)rt_g3d_checked_or_null(obj, RT_G3D_NODEANIMATION3D_CLASS_ID);
    return scene3d_node_animation_channel_count(anim);
}

/// @brief Validate and downcast a NodeAnimator3D handle without trapping.
/// @param obj Borrowed candidate runtime handle.
/// @return Borrowed typed payload on a class match, otherwise `NULL`.
static rt_node_animator3d *node_animator_ref(void *obj) {
    return (rt_node_animator3d *)rt_g3d_checked_or_null(obj, RT_G3D_NODEANIMATOR3D_CLASS_ID);
}

/// @brief Validate and downcast a NodeAnimator3D handle, trapping public API misuse.
/// @param obj Borrowed candidate runtime handle.
/// @param method Borrowed trap message emitted for an invalid handle.
/// @return Borrowed typed payload, or `NULL` after trapping.
static rt_node_animator3d *node_animator_checked(void *obj, const char *method) {
    rt_node_animator3d *animator = node_animator_ref(obj);
    if (!animator)
        rt_trap(method);
    return animator;
}

/// @brief Number of clips retained by a NodeAnimator3D.
/// @param obj Borrowed NodeAnimator3D handle.
/// @return Safe non-negative clip count, or zero after invalid-handle trapping.
int64_t rt_node_animator3d_get_clip_count(void *obj) {
    rt_node_animator3d *animator =
        node_animator_checked(obj, "NodeAnimator3D.ClipCount: invalid animator");
    node_animator_repair_clips(animator);
    return scene3d_node_animator_clip_count(animator);
}

/// @brief Borrow a clip retained by a NodeAnimator3D.
/// @param obj Borrowed NodeAnimator3D handle.
/// @param index Zero-based clip index.
/// @return Borrowed validated NodeAnimation3D handle, or `NULL`.
void *rt_node_animator3d_get_clip(void *obj, int64_t index) {
    rt_node_animator3d *animator =
        node_animator_checked(obj, "NodeAnimator3D.GetClip: invalid animator");
    int32_t clip_count;
    node_animator_repair_clips(animator);
    clip_count = scene3d_node_animator_clip_count(animator);
    if (!animator || index < 0 || index >= clip_count)
        return NULL;
    return rt_g3d_checked_or_null(animator->animations[index], RT_G3D_NODEANIMATION3D_CLASS_ID);
}

/// @brief Get the name of a clip retained by a NodeAnimator3D.
/// @param obj Borrowed NodeAnimator3D handle.
/// @param index Zero-based clip index.
/// @return Borrowed clip-name runtime string, or an empty runtime string.
rt_string rt_node_animator3d_get_clip_name(void *obj, int64_t index) {
    void *clip = rt_node_animator3d_get_clip(obj, index);
    return rt_node_animation3d_get_name(clip);
}

/// @brief Get the currently selected clip name.
/// @param obj Borrowed NodeAnimator3D handle.
/// @return Borrowed selected clip-name string, or an empty runtime string.
rt_string rt_node_animator3d_get_current_clip(void *obj) {
    rt_node_animator3d *animator =
        node_animator_checked(obj, "NodeAnimator3D.CurrentClip: invalid animator");
    int32_t clip_count;
    node_animator_repair_clips(animator);
    clip_count = scene3d_node_animator_clip_count(animator);
    if (!animator || clip_count <= 0 || animator->current_animation < 0 ||
        animator->current_animation >= clip_count)
        return rt_const_cstr("");
    return rt_node_animation3d_get_name(animator->animations[animator->current_animation]);
}

/// @brief Select a clip by name and restart playback from time zero.
/// @param obj Borrowed NodeAnimator3D handle.
/// @param name Borrowed exact clip-name runtime string; invalid strings select empty.
/// @return Nonzero when a matching clip was selected, otherwise zero.
int8_t rt_node_animator3d_play(void *obj, rt_string name) {
    rt_node_animator3d *animator =
        node_animator_checked(obj, "NodeAnimator3D.Play: invalid animator");
    int32_t clip_count;
    if (!node_anim_string_view(name, 1, NULL, NULL))
        return 0;
    node_animator_repair_clips(animator);
    clip_count = scene3d_node_animator_clip_count(animator);
    for (int32_t i = 0; i < clip_count; i++) {
        if (node_anim_string_equal(animator->animations[i]->name, name)) {
            animator->current_animation = i;
            animator->time = 0.0;
            animator->playing = 1;
            node_animator_clear_target_cache(animator);
            return 1;
        }
    }
    return 0;
}

/// @brief Stop a NodeAnimator3D without clearing its selected clip.
/// @param obj Borrowed NodeAnimator3D handle.
void rt_node_animator3d_stop(void *obj) {
    rt_node_animator3d *animator =
        node_animator_checked(obj, "NodeAnimator3D.Stop: invalid animator");
    if (animator)
        animator->playing = 0;
}

/// @brief Set global playback speed, accepting negative values for reverse playback.
/// @param obj Borrowed NodeAnimator3D handle.
/// @param speed Playback multiplier sanitized and clamped to the supported range.
void rt_node_animator3d_set_speed(void *obj, double speed) {
    rt_node_animator3d *animator =
        node_animator_checked(obj, "NodeAnimator3D.SetSpeed: invalid animator");
    if (!animator)
        return;
    if (!isfinite(speed))
        speed = 1.0;
    if (speed > NODE_ANIM_SPEED_ABS_MAX)
        speed = NODE_ANIM_SPEED_ABS_MAX;
    else if (speed < -NODE_ANIM_SPEED_ABS_MAX)
        speed = -NODE_ANIM_SPEED_ABS_MAX;
    animator->speed = speed;
}

/// @brief Get the global playback speed multiplier.
/// @param obj Borrowed NodeAnimator3D handle.
/// @return Finite playback multiplier, defaulting to `1.0`.
double rt_node_animator3d_get_speed(void *obj) {
    rt_node_animator3d *animator =
        node_animator_checked(obj, "NodeAnimator3D.Speed: invalid animator");
    double speed = animator ? animator->speed : 1.0;
    if (!isfinite(speed))
        return 1.0;
    if (speed > NODE_ANIM_SPEED_ABS_MAX)
        return NODE_ANIM_SPEED_ABS_MAX;
    if (speed < -NODE_ANIM_SPEED_ABS_MAX)
        return -NODE_ANIM_SPEED_ABS_MAX;
    return speed;
}

/// @brief Set the current playback time, sanitized to a finite non-negative value.
/// @param obj Borrowed NodeAnimator3D handle.
/// @param time Requested seconds clamped to the supported time range.
void rt_node_animator3d_set_time(void *obj, double time) {
    rt_node_animator3d *animator =
        node_animator_checked(obj, "NodeAnimator3D.SetTime: invalid animator");
    if (!animator)
        return;
    if (!isfinite(time))
        time = 0.0;
    if (time < 0.0)
        time = 0.0;
    if (time > NODE_ANIM_TIME_MAX)
        time = NODE_ANIM_TIME_MAX;
    animator->time = time;
}

/// @brief Get the current playback time.
/// @param obj Borrowed NodeAnimator3D handle.
/// @return Finite playback time, or zero for invalid/corrupt state.
double rt_node_animator3d_get_time(void *obj) {
    rt_node_animator3d *animator =
        node_animator_checked(obj, "NodeAnimator3D.Time: invalid animator");
    double time = animator ? animator->time : 0.0;
    if (!isfinite(time) || time <= 0.0)
        return 0.0;
    return time > NODE_ANIM_TIME_MAX ? NODE_ANIM_TIME_MAX : time;
}

/// @brief Whether the animator is actively advancing time.
/// @param obj Borrowed NodeAnimator3D handle.
/// @return Canonical playing flag, or zero after invalid-handle trapping.
int8_t rt_node_animator3d_get_playing(void *obj) {
    rt_node_animator3d *animator =
        node_animator_checked(obj, "NodeAnimator3D.Playing: invalid animator");
    return animator && animator->playing ? 1 : 0;
}

/// @brief Normalize a float[4] quaternion in-place; substitute the identity quaternion
///        if the magnitude is too small to normalize safely.
/// @details A degenerate quaternion (all-zero or near-zero) would produce NaN after
///   division. The identity quaternion (0, 0, 0, 1) is substituted instead so a bad
///   asset frame leaves the node in its rest orientation rather than exploding the
///   scene graph. The 1e-8 threshold is smaller than any numerically meaningful
///   quaternion from a non-degenerate rotation.
/// @param q float[4] quaternion in (x, y, z, w) order; modified in-place.
static void node_anim_normalize_quat(float *q) {
    float max_abs;
    float len;
    if (!q)
        return;
    if (!isfinite(q[0]) || !isfinite(q[1]) || !isfinite(q[2]) || !isfinite(q[3])) {
        q[0] = 0.0f;
        q[1] = 0.0f;
        q[2] = 0.0f;
        q[3] = 1.0f;
        return;
    }
    max_abs = fmaxf(fmaxf(fabsf(q[0]), fabsf(q[1])), fmaxf(fabsf(q[2]), fabsf(q[3])));
    if (!isfinite(max_abs) || max_abs <= 1e-20f) {
        q[0] = 0.0f;
        q[1] = 0.0f;
        q[2] = 0.0f;
        q[3] = 1.0f;
        return;
    }
    q[0] /= max_abs;
    q[1] /= max_abs;
    q[2] /= max_abs;
    q[3] /= max_abs;
    len = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (len > 1e-8f) {
        q[0] /= len;
        q[1] /= len;
        q[2] /= len;
        q[3] /= len;
    } else {
        q[0] = 0.0f;
        q[1] = 0.0f;
        q[2] = 0.0f;
        q[3] = 1.0f;
    }
}

/// @brief Spherical-linear interpolation between two unit quaternions along the
///        shortest arc on the 4-sphere.
/// @details Both inputs are normalized before use so callers need not pre-normalize.
///   The dot product is computed in double precision and the sign of @p b is flipped
///   when `dot < 0` to guarantee the shortest-path interpolation (without this, a
///   270° spin would be taken instead of a 90° spin for near-antipodal quaternions).
///   When `dot > 0.9995` (quaternions nearly identical) the implementation falls back
///   to normalized LERP to avoid the `acos(1.0)` domain-error that produces NaN at
///   the identity separation angle.
/// @param a     Source quaternion (x, y, z, w), must not be NULL.
/// @param b     Target quaternion (x, y, z, w), must not be NULL.
/// @param alpha Interpolation parameter in [0, 1]; 0 returns @p a, 1 returns @p b.
/// @param out   float[4] output quaternion, must not be NULL.
static void node_anim_slerp_quat(const float *a, const float *b, double alpha, float *out) {
    float q0[4];
    float q1[4];
    double dot;
    if (!a || !b || !out)
        return;
    if (!isfinite(alpha) || alpha < 0.0)
        alpha = 0.0;
    else if (alpha > 1.0)
        alpha = 1.0;
    memcpy(q0, a, sizeof(q0));
    memcpy(q1, b, sizeof(q1));
    node_anim_normalize_quat(q0);
    node_anim_normalize_quat(q1);
    dot = (double)q0[0] * q1[0] + (double)q0[1] * q1[1] + (double)q0[2] * q1[2] +
          (double)q0[3] * q1[3];
    if (dot < 0.0) {
        dot = -dot;
        for (int i = 0; i < 4; i++)
            q1[i] = -q1[i];
    }
    if (!isfinite(dot)) {
        out[0] = 0.0f;
        out[1] = 0.0f;
        out[2] = 0.0f;
        out[3] = 1.0f;
        return;
    }
    if (dot > 1.0)
        dot = 1.0;
    if (dot < -1.0)
        dot = -1.0;
    if (dot > 0.9995) {
        for (int i = 0; i < 4; i++)
            out[i] = (float)((double)q0[i] + ((double)q1[i] - (double)q0[i]) * alpha);
        node_anim_normalize_quat(out);
        return;
    }
    {
        double theta0 = acos(dot);
        double theta = theta0 * alpha;
        double sin_theta = sin(theta);
        double sin_theta0 = sin(theta0);
        double s0 = cos(theta) - dot * sin_theta / sin_theta0;
        double s1 = sin_theta / sin_theta0;
        for (int i = 0; i < 4; i++)
            out[i] = (float)(s0 * q0[i] + s1 * q1[i]);
        node_anim_normalize_quat(out);
    }
}

/// @brief Sample an animation channel at the given time and write interpolated values
///        to @p out_values, dispatching over STEP / LINEAR / CUBICSPLINE interpolation.
/// @details The implementation binary-searches for the bracketing keyframe interval
///   [lo, hi], then computes alpha = (time - t0) / (t1 - t0) clamped to [0, 1].
///   Dispatch:
///   - STEP: copies the `lo` keyframe values unchanged.
///   - CUBICSPLINE: evaluates the glTF cubic Hermite spline using the precomputed
///     h00/h10/h01/h11 basis polynomials, scaling tangents by the interval length dt.
///     Rotation channels are re-normalized after cubic evaluation.
///   - LINEAR (default): component-wise lerp, with SLERP for ROTATION channels to
///     maintain unit-quaternion properties across the interval.
///   Time clamping at the clip endpoints avoids out-of-range array access.
/// @param channel    Fully validated channel with at least one keyframe.
/// @param time       Playback time in seconds.
/// @param out_values Caller-allocated buffer of at least channel->value_width floats.
/// @return Nonzero after publishing a finite sample, otherwise zero for corrupt retained data.
static int node_anim_sample_channel(const rt_node_anim_channel3d *channel,
                                    double time,
                                    float *out_values) {
    int32_t lo;
    int32_t hi;
    double t0;
    double t1;
    double alpha;
    if (!channel || !out_values || channel->key_count <= 0 || channel->value_width <= 0)
        return 0;
    if (!isfinite(time))
        time = 0.0;
    if (channel->interpolation == RT_NODE_ANIM_INTERP_STEP) {
        int32_t upper_lo = 0;
        int32_t upper_hi = channel->key_count;
        int32_t sample_index;
        while (upper_lo < upper_hi) {
            int32_t mid = upper_lo + (upper_hi - upper_lo) / 2;
            if (!node_anim_key_time_valid(channel, mid))
                return 0;
            if (channel->times[mid] <= time)
                upper_lo = mid + 1;
            else
                upper_hi = mid;
        }
        sample_index = upper_lo - 1;
        if (sample_index < 0)
            sample_index = 0;
        if (sample_index >= channel->key_count)
            sample_index = channel->key_count - 1;
        const float *sample = &channel->values[(size_t)sample_index * (size_t)channel->value_width];
        if (!node_anim_key_time_valid(channel, sample_index) ||
            !node_anim_float_span_finite(sample, channel->value_width))
            return 0;
        memcpy(out_values, sample, (size_t)channel->value_width * sizeof(float));
        if (channel->path == RT_NODE_ANIM_PATH_ROTATION && channel->value_width >= 4)
            node_anim_normalize_quat(out_values);
        return 1;
    }
    if (time <= channel->times[0]) {
        if (!node_anim_float_span_finite(channel->values, channel->value_width))
            return 0;
        memcpy(out_values, channel->values, (size_t)channel->value_width * sizeof(float));
        if (channel->path == RT_NODE_ANIM_PATH_ROTATION && channel->value_width >= 4)
            node_anim_normalize_quat(out_values);
        return 1;
    }
    if (time >= channel->times[channel->key_count - 1]) {
        const float *sample =
            &channel->values[(size_t)(channel->key_count - 1) * (size_t)channel->value_width];
        if (!node_anim_float_span_finite(sample, channel->value_width))
            return 0;
        memcpy(out_values, sample, (size_t)channel->value_width * sizeof(float));
        if (channel->path == RT_NODE_ANIM_PATH_ROTATION && channel->value_width >= 4)
            node_anim_normalize_quat(out_values);
        return 1;
    }
    lo = 0;
    hi = channel->key_count - 1;
    while (hi - lo > 1) {
        int32_t mid = lo + (hi - lo) / 2;
        if (!node_anim_key_time_valid(channel, mid))
            return 0;
        if (channel->times[mid] <= time)
            lo = mid;
        else
            hi = mid;
    }
    t0 = channel->times[lo];
    t1 = channel->times[hi];
    if (!node_anim_key_time_valid(channel, lo) || !node_anim_key_time_valid(channel, hi) ||
        !isfinite(t0) || !isfinite(t1) || t0 < 0.0 || t1 <= t0 || t1 > NODE_ANIM_TIME_MAX)
        return 0;
    alpha = (t1 > t0 && channel->interpolation != RT_NODE_ANIM_INTERP_STEP)
                ? (time - t0) / (t1 - t0)
                : 0.0;
    if (alpha < 0.0)
        alpha = 0.0;
    else if (alpha > 1.0)
        alpha = 1.0;
    if (channel->interpolation == RT_NODE_ANIM_INTERP_CUBICSPLINE && channel->in_tangents &&
        channel->out_tangents) {
        double dt = t1 - t0;
        double u2 = alpha * alpha;
        double u3 = u2 * alpha;
        double h00 = 2.0 * u3 - 3.0 * u2 + 1.0;
        double h10 = u3 - 2.0 * u2 + alpha;
        double h01 = -2.0 * u3 + 3.0 * u2;
        double h11 = u3 - u2;
        if (!isfinite(dt) || dt < 0.0)
            dt = 0.0;
        if (channel->path == RT_NODE_ANIM_PATH_ROTATION && channel->value_width == 4) {
            float a[4];
            float b[4];
            float out_tangent[4];
            float in_tangent[4];
            double dot;
            size_t a_base = (size_t)lo * (size_t)channel->value_width;
            size_t b_base = (size_t)hi * (size_t)channel->value_width;
            if (!node_anim_float_span_finite(&channel->values[a_base], 4) ||
                !node_anim_float_span_finite(&channel->values[b_base], 4) ||
                !node_anim_float_span_finite(&channel->out_tangents[a_base], 4) ||
                !node_anim_float_span_finite(&channel->in_tangents[b_base], 4))
                return 0;
            memcpy(a, &channel->values[a_base], sizeof(a));
            memcpy(b, &channel->values[b_base], sizeof(b));
            memcpy(out_tangent, &channel->out_tangents[a_base], sizeof(out_tangent));
            memcpy(in_tangent, &channel->in_tangents[b_base], sizeof(in_tangent));
            node_anim_normalize_quat(a);
            node_anim_normalize_quat(b);
            dot = (double)a[0] * b[0] + (double)a[1] * b[1] + (double)a[2] * b[2] +
                  (double)a[3] * b[3];
            if (isfinite(dot) && dot < 0.0) {
                for (int32_t i = 0; i < 4; i++) {
                    b[i] = -b[i];
                    in_tangent[i] = -in_tangent[i];
                }
            }
            for (int32_t i = 0; i < 4; i++) {
                out_values[i] = (float)(h00 * a[i] + h10 * dt * out_tangent[i] + h01 * b[i] +
                                        h11 * dt * in_tangent[i]);
            }
            if (!node_anim_float_span_finite(out_values, 4))
                return 0;
            node_anim_normalize_quat(out_values);
            return 1;
        }
        if (!node_anim_float_span_finite(
                &channel->values[(size_t)lo * (size_t)channel->value_width],
                channel->value_width) ||
            !node_anim_float_span_finite(
                &channel->values[(size_t)hi * (size_t)channel->value_width],
                channel->value_width) ||
            !node_anim_float_span_finite(
                &channel->out_tangents[(size_t)lo * (size_t)channel->value_width],
                channel->value_width) ||
            !node_anim_float_span_finite(
                &channel->in_tangents[(size_t)hi * (size_t)channel->value_width],
                channel->value_width))
            return 0;
        for (int32_t i = 0; i < channel->value_width; i++) {
            size_t ai = (size_t)lo * (size_t)channel->value_width + (size_t)i;
            size_t bi = (size_t)hi * (size_t)channel->value_width + (size_t)i;
            out_values[i] =
                (float)(h00 * channel->values[ai] + h10 * dt * channel->out_tangents[ai] +
                        h01 * channel->values[bi] + h11 * dt * channel->in_tangents[bi]);
        }
        if (!node_anim_float_span_finite(out_values, channel->value_width))
            return 0;
        if (channel->path == RT_NODE_ANIM_PATH_ROTATION && channel->value_width >= 4)
            node_anim_normalize_quat(out_values);
        return 1;
    }
    if (channel->path == RT_NODE_ANIM_PATH_ROTATION && channel->value_width >= 4) {
        const float *a = &channel->values[(size_t)lo * (size_t)channel->value_width];
        const float *b = &channel->values[(size_t)hi * (size_t)channel->value_width];
        if (!node_anim_float_span_finite(a, 4) || !node_anim_float_span_finite(b, 4))
            return 0;
        node_anim_slerp_quat(a, b, alpha, out_values);
        return node_anim_float_span_finite(out_values, 4);
    }
    if (!node_anim_float_span_finite(&channel->values[(size_t)lo * (size_t)channel->value_width],
                                     channel->value_width) ||
        !node_anim_float_span_finite(&channel->values[(size_t)hi * (size_t)channel->value_width],
                                     channel->value_width))
        return 0;
    for (int32_t i = 0; i < channel->value_width; i++) {
        float a = channel->values[(size_t)lo * (size_t)channel->value_width + (size_t)i];
        float b = channel->values[(size_t)hi * (size_t)channel->value_width + (size_t)i];
        out_values[i] = (float)((double)a + ((double)b - (double)a) * alpha);
    }
    return node_anim_float_span_finite(out_values, channel->value_width);
}

/// @brief Apply morph-target weights from an animation down a node subtree (recursive).
/// @details A glTF node can expand into several primitive child nodes during import. Each
///   primitive owns its own MorphTarget3D object, so weights must be copied to every morphed mesh
///   under the animated node rather than only the first morph payload encountered.
/// @param animator Borrowed animator providing reusable traversal storage.
/// @param node Borrowed subtree root receiving the sampled weights.
/// @param weights Borrowed array of sampled weight values.
/// @param weight_count Positive number of readable weights.
static void node_anim_apply_weights_recursive(rt_node_animator3d *animator,
                                              rt_scene_node3d *node,
                                              const float *weights,
                                              int32_t weight_count) {
    size_t count = 0;
    if (!animator || !node || !weights || weight_count <= 0)
        return;
    if (!node_animator_stack_push(animator, &count, node)) {
        rt_trap("NodeAnimation3D: traversal stack allocation failed");
        return;
    }
    while (count > 0) {
        rt_scene_node3d *current = animator->traversal_stack[--count];
        rt_mesh3d *mesh = rt_g3d_has_class(current->mesh, RT_G3D_MESH3D_CLASS_ID)
                              ? (rt_mesh3d *)current->mesh
                              : NULL;
        if (mesh && mesh->morph_targets_ref) {
            int64_t shape_count = rt_morphtarget3d_get_shape_count(mesh->morph_targets_ref);
            if (shape_count > INT32_MAX)
                shape_count = INT32_MAX;
            int32_t limit = (int32_t)((shape_count < weight_count) ? shape_count : weight_count);
            for (int32_t i = 0; i < limit; i++)
                rt_morphtarget3d_set_weight(mesh->morph_targets_ref, i, weights[i]);
            for (int32_t i = limit; i < (int32_t)shape_count; i++)
                rt_morphtarget3d_set_weight(mesh->morph_targets_ref, i, 0.0);
        }
        for (int32_t i = scene3d_node_child_count(current) - 1; i >= 0; i--) {
            rt_scene_node3d *child = (rt_scene_node3d *)rt_g3d_checked_or_null(
                current->children[i], RT_G3D_SCENENODE3D_CLASS_ID);
            if (!child)
                continue;
            if (!node_animator_stack_push(animator, &count, child)) {
                rt_trap("NodeAnimation3D: traversal stack allocation failed");
                return;
            }
        }
    }
}

/// @brief Whether @p node lies in @p root's subtree (walks parent links up to root).
/// @param root Borrowed prospective ancestor.
/// @param node Borrowed node whose parent chain is searched.
/// @return Nonzero when @p root is @p node or one of its ancestors.
static int scene_node_is_descendant_of(rt_scene_node3d *root, rt_scene_node3d *node) {
    rt_scene_node3d *slow = node;
    rt_scene_node3d *fast = node;
    size_t visited = 0;
    if (!rt_g3d_has_class(root, RT_G3D_SCENENODE3D_CLASS_ID))
        return 0;
    while (node && visited++ < NODE_ANIM_TRAVERSAL_NODE_MAX) {
        if (!rt_g3d_has_class(node, RT_G3D_SCENENODE3D_CLASS_ID))
            return 0;
        if (node == root)
            return 1;
        node = node->parent;

        slow = rt_g3d_has_class(slow, RT_G3D_SCENENODE3D_CLASS_ID) ? slow->parent : NULL;
        if (rt_g3d_has_class(fast, RT_G3D_SCENENODE3D_CLASS_ID))
            fast = fast->parent;
        else
            fast = NULL;
        if (rt_g3d_has_class(fast, RT_G3D_SCENENODE3D_CLASS_ID))
            fast = fast->parent;
        else
            fast = NULL;
        if (slow && slow == fast)
            return 0;
    }
    return 0;
}

/// @brief Depth-first search the subtree rooted at @p root for the node whose import index
///   equals @p import_index (used to bind animation channels to imported nodes); NULL if none.
/// @param animator Borrowed animator providing reusable traversal storage.
/// @param root Borrowed subtree root.
/// @param import_index Non-negative stable imported node index.
/// @return Borrowed matching node, or `NULL`.
static rt_scene_node3d *node_anim_find_by_import_index(rt_node_animator3d *animator,
                                                       rt_scene_node3d *root,
                                                       int32_t import_index) {
    size_t count = 0;
    rt_scene_node3d *result = NULL;
    if (!animator || !root || import_index < 0)
        return NULL;
    if (!node_animator_stack_push(animator, &count, root)) {
        rt_trap("NodeAnimation3D: traversal stack allocation failed");
        return NULL;
    }
    while (count > 0) {
        rt_scene_node3d *current = animator->traversal_stack[--count];
        if (current->import_index == import_index) {
            result = current;
            break;
        }
        for (int32_t i = scene3d_node_child_count(current) - 1; i >= 0; i--) {
            rt_scene_node3d *child = (rt_scene_node3d *)rt_g3d_checked_or_null(
                current->children[i], RT_G3D_SCENENODE3D_CLASS_ID);
            if (!child)
                continue;
            if (!node_animator_stack_push(animator, &count, child)) {
                rt_trap("NodeAnimation3D: traversal stack allocation failed");
                return NULL;
            }
        }
    }
    return result;
}

/// @brief Find the first exact-name match using animator-owned bounded traversal storage.
/// @param animator Borrowed animator providing reusable stack capacity.
/// @param root Borrowed subtree root.
/// @param target_name Borrowed validated runtime identifier.
/// @return Borrowed matching descendant, or `NULL`.
static rt_scene_node3d *node_anim_find_by_name(rt_node_animator3d *animator,
                                               rt_scene_node3d *root,
                                               rt_string target_name) {
    size_t count = 0;
    size_t visited = 0;
    if (!animator || !rt_g3d_has_class(root, RT_G3D_SCENENODE3D_CLASS_ID) ||
        !node_anim_string_view(target_name, 0, NULL, NULL))
        return NULL;
    if (!node_animator_stack_push(animator, &count, root)) {
        rt_trap("NodeAnimation3D: traversal stack allocation failed");
        return NULL;
    }
    while (count > 0 && visited++ < NODE_ANIM_TRAVERSAL_NODE_MAX) {
        rt_scene_node3d *current = animator->traversal_stack[--count];
        if (current->name && !rt_string_is_handle(current->name))
            current->name = NULL;
        else if (node_anim_string_equal(current->name, target_name))
            return current;
        for (int32_t i = scene3d_node_child_count(current) - 1; i >= 0; --i) {
            rt_scene_node3d *child = (rt_scene_node3d *)rt_g3d_checked_or_null(
                current->children[i], RT_G3D_SCENENODE3D_CLASS_ID);
            if (!child)
                continue;
            if (!node_animator_stack_push(animator, &count, child)) {
                rt_trap("NodeAnimation3D: traversal stack allocation failed");
                return NULL;
            }
        }
    }
    if (count > 0)
        rt_trap("NodeAnimation3D: traversal node budget exceeded");
    return NULL;
}

/// @brief Resolve an animation channel's target node within @p root's subtree, by name.
/// @details Caches the resolved node on the channel keyed by the target name, so repeated frames
/// skip
///          the by-name search unless the name changes.
/// @param animator Borrowed animator owning the resolved-target cache.
/// @param root Borrowed animation subtree root.
/// @param channel Borrowed channel containing an import index or target name.
/// @param channel_index Zero-based channel/cache index.
/// @return Borrowed matching descendant retained by the cache, or `NULL`.
static rt_scene_node3d *node_anim_resolve_target(rt_node_animator3d *animator,
                                                 rt_scene_node3d *root,
                                                 rt_node_anim_channel3d *channel,
                                                 int32_t channel_index) {
    rt_scene_node3d *cached_target = NULL;
    if (!animator || !root || !channel || !channel->target_name)
        return NULL;
    if (!node_anim_string_view(channel->target_name, 0, NULL, NULL))
        return NULL;
    if (animator->cached_root != root || animator->cached_clip_index != animator->current_animation)
        node_animator_clear_target_cache(animator);
    animator->cached_root = root;
    animator->cached_clip_index = animator->current_animation;
    if (channel_index >= 0 && channel_index < animator->cached_target_capacity)
        cached_target = animator->cached_targets[channel_index];
    if (cached_target && !rt_g3d_has_class(cached_target, RT_G3D_SCENENODE3D_CLASS_ID)) {
        animator->cached_targets[channel_index] = NULL;
        cached_target = NULL;
    }
    if (channel->target_node_index >= 0) {
        if (cached_target && cached_target->import_index == channel->target_node_index &&
            scene_node_is_descendant_of(root, cached_target))
            return cached_target;
        cached_target = node_anim_find_by_import_index(animator, root, channel->target_node_index);
        node_animator_cache_store(animator, channel_index, cached_target);
        if (cached_target)
            return cached_target;
        return NULL;
    }
    if (cached_target && scene_node_is_descendant_of(root, cached_target)) {
        if (node_anim_string_equal(cached_target->name, channel->target_name))
            return cached_target;
    }
    cached_target = node_anim_find_by_name(animator, root, channel->target_name);
    node_animator_cache_store(animator, channel_index, cached_target);
    return cached_target;
}

/// @brief Resolve a channel's target node by name, sample the channel, and write the
///        TRS or weight result onto the target.
/// @details Resolution uses `find_by_name` on the animator root subtree at every call,
///   which is an O(n) tree walk. For clips with many channels this is called once per
///   channel per frame; the expectation is that scene graphs are small enough that the
///   linear search dominates only for very large skeletal hierarchies, which in practice
///   use the skeleton system rather than node animation.
///   A stack buffer of 16 floats covers all standard TRS channels (max width 4 for
///   quaternion); only WEIGHTS channels with more than 16 targets spill to a heap
///   allocation. The heap buffer is always freed before returning.
///   SCALE values are sanitised through `scene3d_scale_or_unit` to avoid degenerate
///   inverse matrices when an asset provides a near-zero scale keyframe.
/// @param animator Borrowed animator providing cache and sample scratch storage.
/// @param root    Root of the scene subtree searched for the target node.
/// @param channel Channel to sample; must have a valid target_name.
/// @param channel_index Zero-based channel/cache index.
/// @param time    Playback time in seconds.
static void node_anim_apply_channel(rt_node_animator3d *animator,
                                    rt_scene_node3d *root,
                                    rt_node_anim_channel3d *channel,
                                    int32_t channel_index,
                                    double time) {
    rt_scene_node3d *target;
    float stack_values[16];
    float *values = stack_values;
    int32_t width;
    if (!root || !channel || !channel->target_name)
        return;
    if (!node_anim_channel_runtime_valid(channel))
        return;
    width = channel->value_width;
    if (width <= 0)
        return;
    if ((size_t)width > SIZE_MAX / sizeof(float))
        return;
    if (width > (int32_t)(sizeof(stack_values) / sizeof(stack_values[0]))) {
        values = node_animator_sample_scratch(animator, width);
        if (!values)
            return;
    } else {
        memset(values, 0, sizeof(stack_values));
    }
    target = node_anim_resolve_target(animator, root, channel, channel_index);
    if (!target)
        return;
    if (!node_anim_sample_channel(channel, time, values))
        return;
    switch (channel->path) {
        case RT_NODE_ANIM_PATH_TRANSLATION:
            if (width >= 3) {
                double next[3] = {scene3d_clamp_abs_or(values[0], 0.0),
                                  scene3d_clamp_abs_or(values[1], 0.0),
                                  scene3d_clamp_abs_or(values[2], 0.0)};
                if (target->position[0] == next[0] && target->position[1] == next[1] &&
                    target->position[2] == next[2])
                    break;
                target->position[0] = next[0];
                target->position[1] = next[1];
                target->position[2] = next[2];
                mark_dirty(target);
            }
            break;
        case RT_NODE_ANIM_PATH_ROTATION:
            if (width >= 4) {
                double next[4] = {values[0], values[1], values[2], values[3]};
                scene3d_quat_normalize_local(next);
                if (target->rotation[0] == next[0] && target->rotation[1] == next[1] &&
                    target->rotation[2] == next[2] && target->rotation[3] == next[3])
                    break;
                target->rotation[0] = next[0];
                target->rotation[1] = next[1];
                target->rotation[2] = next[2];
                target->rotation[3] = next[3];
                mark_dirty(target);
            }
            break;
        case RT_NODE_ANIM_PATH_SCALE:
            if (width >= 3) {
                double next[3] = {scene3d_scale_or_unit(values[0]),
                                  scene3d_scale_or_unit(values[1]),
                                  scene3d_scale_or_unit(values[2])};
                if (target->scale_xyz[0] == next[0] && target->scale_xyz[1] == next[1] &&
                    target->scale_xyz[2] == next[2])
                    break;
                target->scale_xyz[0] = next[0];
                target->scale_xyz[1] = next[1];
                target->scale_xyz[2] = next[2];
                mark_dirty(target);
            }
            break;
        case RT_NODE_ANIM_PATH_WEIGHTS:
            node_anim_apply_weights_recursive(animator, target, values, width);
            break;
        case RT_NODE_ANIM_PATH_CAMERA_FOV:
            if (width == 1 && rt_g3d_has_class(target->camera, RT_G3D_CAMERA3D_CLASS_ID))
                rt_camera3d_set_retained_fov(target->camera, values[0]);
            break;
        case RT_NODE_ANIM_PATH_CAMERA_ASPECT:
            if (width == 1 && rt_g3d_has_class(target->camera, RT_G3D_CAMERA3D_CLASS_ID))
                rt_camera3d_sync_render_aspect(target->camera, values[0]);
            break;
        case RT_NODE_ANIM_PATH_CAMERA_NEAR:
            if (width == 1 && rt_g3d_has_class(target->camera, RT_G3D_CAMERA3D_CLASS_ID))
                rt_camera3d_set_near_plane(target->camera, values[0]);
            break;
        case RT_NODE_ANIM_PATH_CAMERA_FAR:
            if (width == 1 && rt_g3d_has_class(target->camera, RT_G3D_CAMERA3D_CLASS_ID))
                rt_camera3d_set_far_plane(target->camera, values[0]);
            break;
        case RT_NODE_ANIM_PATH_CAMERA_ORTHO_SIZE:
            if (width == 1 && rt_g3d_has_class(target->camera, RT_G3D_CAMERA3D_CLASS_ID))
                rt_camera3d_set_ortho_size(target->camera, values[0]);
            break;
        case RT_NODE_ANIM_PATH_CAMERA_PROJECTION_MODE:
            if (width == 1 && rt_g3d_has_class(target->camera, RT_G3D_CAMERA3D_CLASS_ID))
                rt_camera3d_set_is_ortho(target->camera, values[0] >= 0.5f);
            break;
    }
}

/// @brief Advance an animator's playback time and apply all channels of the current
///        clip to the bound scene subtree.
/// @details Time advance is: `time += dt * speed` clamped to finite values.
///   After advance, looping clips wrap via `fmod` into [0, duration); one-shot clips
///   clamp to `duration` and set `playing = 0` so callers can detect clip end.
///   Out-of-range `current_animation` is reset to 0 defensively (can happen when the
///   clip set is hot-swapped). A NULL `root` node is a fast-out — the animator must
///   have been bound via `rt_scene_node3d_bind_node_animator` before updates have any
///   effect. All channels are applied in index order with no blending; to blend two
///   clips the caller must manage two animators and lerp the results at the scene node.
/// @param animator Animator to advance; silently no-ops on NULL or non-playing state.
/// @param dt       Delta time in seconds since the last update; non-finite values are
///                 ignored so a stalled timer cannot corrupt the playback position.
void node_animator_update(rt_node_animator3d *animator, double dt) {
    rt_node_animation3d *clip;
    double duration;
    double step;
    int32_t clip_count;
    int32_t channel_count;
    if (!animator || !animator->playing || !animator->root)
        return;
    if (!rt_g3d_has_class(animator->root, RT_G3D_SCENENODE3D_CLASS_ID)) {
        animator->root = NULL;
        return;
    }
    node_animator_repair_clips(animator);
    clip_count = scene3d_node_animator_clip_count(animator);
    if (clip_count <= 0)
        return;
    animator->animation_count = clip_count;
    if (animator->current_animation < 0 || animator->current_animation >= clip_count)
        animator->current_animation = 0;
    clip = animator->animations[animator->current_animation];
    if (!rt_g3d_has_class(clip, RT_G3D_NODEANIMATION3D_CLASS_ID))
        return;
    duration = clip->duration;
    if (!isfinite(duration) || duration <= 0.0)
        duration = 1.0;
    else if (duration > NODE_ANIM_TIME_MAX)
        duration = NODE_ANIM_TIME_MAX;
    clip->duration = duration;
    channel_count = scene3d_node_animation_channel_count(clip);
    clip->channel_count = channel_count;
    if (channel_count > 0 && !node_animator_ensure_target_cache(animator, channel_count)) {
        rt_trap("NodeAnimator3D.Update: target cache allocation failed");
        return;
    }
    double speed = isfinite(animator->speed) ? animator->speed : 1.0;
    if (speed > NODE_ANIM_SPEED_ABS_MAX)
        speed = NODE_ANIM_SPEED_ABS_MAX;
    else if (speed < -NODE_ANIM_SPEED_ABS_MAX)
        speed = -NODE_ANIM_SPEED_ABS_MAX;
    step = (isfinite(dt) && dt > 0.0) ? dt * speed : 0.0;
    if (step > NODE_ANIM_TIME_MAX)
        step = NODE_ANIM_TIME_MAX;
    else if (step < -NODE_ANIM_TIME_MAX)
        step = -NODE_ANIM_TIME_MAX;
    if (isfinite(step))
        animator->time += step;
    if (!isfinite(animator->time))
        animator->time = 0.0;
    if (duration > 0.0) {
        if (clip->looping) {
            animator->time = fmod(animator->time, duration);
            if (animator->time < 0.0)
                animator->time += duration;
        } else if ((speed >= 0.0 && animator->time >= duration) || animator->time > duration) {
            animator->time = duration;
            animator->playing = 0;
        } else if ((speed < 0.0 && animator->time <= 0.0) || animator->time < 0.0) {
            animator->time = 0.0;
            animator->playing = 0;
        }
    }
    for (int32_t i = 0; i < channel_count; i++)
        node_anim_apply_channel(animator, animator->root, &clip->channels[i], i, animator->time);
}

/// @brief Public wrapper that advances a NodeAnimator3D by @p dt seconds.
/// @param obj Borrowed NodeAnimator3D handle.
/// @param dt Finite positive frame delta in seconds; other values do not advance time.
void rt_node_animator3d_update(void *obj, double dt) {
    rt_node_animator3d *animator =
        node_animator_checked(obj, "NodeAnimator3D.Update: invalid animator");
    node_animator_update(animator, dt);
}

#endif /* ZANNA_ENABLE_GRAPHICS */
