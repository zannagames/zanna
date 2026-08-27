//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/audio/rt_sound3d_objects.c
// Purpose: Object-backed 3D audio listener/source APIs layered on top of the
//   low-level Sound3D helpers and the existing 2D voice runtime. Backs
//   `Zanna.Graphics3D.SoundListener3D` and `Zanna.Graphics3D.SoundSource3D`.
//
// Key invariants:
//   - At most one SoundListener3D may be active at a time; activation pushes
//     the listener's state into rt_sound3d.c's active-listener slot.
//   - Listeners and sources are tracked in process-global doubly-linked lists
//     so SyncBindings can walk them every frame without external state.
//   - A bound camera or scene node overrides explicit position / forward;
//     unbinding restores caller-set values.
//
// Ownership/Lifetime:
//   - Listener and source objects are heap-allocated and GC-managed.
//   - bound_node / bound_camera / sound references are retained on assignment
//     and released on unbind / finalize.
//   - Active-listener handle is a weak pointer — clearing the active listener
//     simply removes it from the active slot without touching its refcount.
//
// Links: src/runtime/audio/rt_soundlistener3d.h (SoundListener3D API),
//        src/runtime/audio/rt_soundsource3d.h (SoundSource3D API),
//        src/runtime/audio/rt_sound3d.h (low-level spatial helpers)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements graphics-bound SoundListener3D and SoundSource3D objects.
/// @details Graphics-enabled builds retain optional SceneNode3D/Camera3D
///          bindings, synchronize world transforms on the main thread, derive
///          bounded velocities from frame deltas, and forward sanitized state
///          to the low-level spatial voice API. Objects live in weak global
///          traversal lists; retained bindings/sounds are released by runtime
///          finalizers. Graphics-disabled builds emit only a translation-unit
///          guard because the low-level file supplies the sync no-op.

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_canvas3d.h"
#include "rt_graphics3d_ids.h"
#include "rt_mixgroup.h"
#include "rt_platform.h"
#include "rt_scene3d.h"
#include "rt_sound3d.h"
#include "rt_soundlistener3d.h"
#include "rt_soundsource3d.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern void *rt_obj_new_i64(int64_t class_id, int64_t byte_size);
extern void rt_obj_set_finalizer(void *obj, void (*fn)(void *));
extern int8_t rt_obj_is_instance(void *obj, int64_t class_id, size_t min_size);
extern void rt_obj_retain_maybe(void *obj);
extern int rt_obj_release_check0(void *obj);
extern void rt_obj_free(void *obj);
extern void *rt_vec3_new(double x, double y, double z);
extern double rt_vec3_x(void *v);
extern double rt_vec3_y(void *v);
extern double rt_vec3_z(void *v);
extern int64_t rt_sound_play_ex(void *sound, int64_t volume, int64_t pan);
extern int64_t rt_sound_play_loop(void *sound, int64_t volume, int64_t pan);
extern void rt_voice_stop(int64_t voice_id);
extern void rt_voice_set_volume(int64_t voice_id, int64_t volume);
extern void rt_voice_set_pan(int64_t voice_id, int64_t pan);
extern void rt_voice_set_pitch(int64_t voice_id, double pitch);
extern void rt_voice_set_occlusion(int64_t voice_id, double amount);
extern int64_t rt_voice_is_playing(int64_t voice_id);

typedef struct rt_soundlistener3d {
    void *vptr;
    rt_sound3d_listener_state state;
    void *bound_node;
    void *bound_camera;
    double last_sync_position[3];
    int8_t has_last_sync_position;
    int8_t is_active;
    struct rt_soundlistener3d *prev;
    struct rt_soundlistener3d *next;
} rt_soundlistener3d;

typedef struct rt_soundsource3d {
    void *vptr;
    void *sound;
    void *bound_node;
    double position[3];
    double velocity[3];
    double doppler_factor;
    double last_sync_position[3];
    int8_t has_last_sync_position;
    double ref_distance;
    double max_distance;
    int64_t volume;
    int64_t voice_id;
    int8_t looping;
    double pitch;      ///< User playback-rate multiplier (composes with Doppler).
    double occlusion;  ///< Occlusion amount 0..1 (game-driven, mixer-smoothed).
    int64_t mix_group; ///< Mix group for the underlying voice (default SFX).
    struct rt_soundsource3d *prev;
    struct rt_soundsource3d *next;
} rt_soundsource3d;

static rt_soundlistener3d *s_listener_head = NULL;
static rt_soundsource3d *s_source_head = NULL;
static rt_soundlistener3d *s_active_listener_obj = NULL;

#define SOUND3D_COMPONENT_ABS_MAX 1000000000000.0
#define SOUND3D_VELOCITY_ABS_MAX 1000000.0
#define SOUND3D_DISTANCE_MAX 1000000000.0
#define SOUND3D_SYNC_DT_MAX 1.0
#define SOUND3D_PITCH_MIN 0.25
#define SOUND3D_PITCH_MAX 4.0

static void sound3d_listener_repair_state(rt_soundlistener3d *listener);
static void sound3d_source_repair_state(rt_soundsource3d *source);

/// @brief Checked cast of an opaque handle to SoundListener3D; NULL on class mismatch.
/// @param obj Opaque runtime object to validate.
/// @return Listener storage, or NULL on class/size mismatch.
static rt_soundlistener3d *sound3d_listener_checked(void *obj) {
    if (!rt_obj_is_instance(obj, RT_G3D_SOUNDLISTENER3D_CLASS_ID, sizeof(rt_soundlistener3d)))
        return NULL;
    rt_soundlistener3d *listener = (rt_soundlistener3d *)obj;
    sound3d_listener_repair_state(listener);
    return listener;
}

/// @brief Checked cast of an opaque handle to SoundSource3D; NULL on class mismatch.
/// @param obj Opaque runtime object to validate.
/// @return Source storage, or NULL on class/size mismatch.
static rt_soundsource3d *sound3d_source_checked(void *obj) {
    if (!rt_obj_is_instance(obj, RT_G3D_SOUNDSOURCE3D_CLASS_ID, sizeof(rt_soundsource3d)))
        return NULL;
    rt_soundsource3d *source = (rt_soundsource3d *)obj;
    sound3d_source_repair_state(source);
    return source;
}

/// @brief Drop a GC-managed reference stored in `**slot` and null the slot.
/// @details Idempotent — safe to call on already-null slots. Used by the
///   listener/source finalizers and by bind-site setters that need to
///   release the previous target before installing a new one.
/// @param[in,out] slot Address of an owned GC reference cleared after release.
static void sound3d_release_ref(void **slot) {
    if (!slot || !*slot)
        return;
    if (rt_obj_release_check0(*slot))
        rt_obj_free(*slot);
    *slot = NULL;
}

/// @brief Release a retained Graphics3D slot only when it still has the expected class.
/// @param slot Address of the retained object slot.
/// @param class_id Required Graphics3D class identifier.
static void sound3d_release_class_ref(void **slot, int64_t class_id) {
    if (!slot || !*slot)
        return;
    if (!rt_g3d_has_class(*slot, class_id)) {
        *slot = NULL;
        return;
    }
    sound3d_release_ref(slot);
}

/// @brief Drop one reference and free if zero. Safe on NULL.
/// @param obj Runtime object whose local reference should be released.
static void sound3d_release_local(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Return @p value when finite, else @p fallback.
/// @param value Value to validate.
/// @param fallback Replacement for non-finite input.
/// @return Finite input or @p fallback.
static double sound3d_finite_or(double value, double fallback) {
    return isfinite(value) ? value : fallback;
}

/// @brief Clamp a finite scalar to +/- @p max_abs, substituting @p fallback for NaN/Inf.
/// @param value Value to normalize.
/// @param fallback Replacement for non-finite input.
/// @param max_abs Maximum supported magnitude.
/// @return Sanitized bounded scalar.
static double sound3d_clamp_abs_or(double value, double fallback, double max_abs) {
    value = sound3d_finite_or(value, fallback);
    if (value < -max_abs)
        return -max_abs;
    if (value > max_abs)
        return max_abs;
    return value;
}

/// @brief Clamp a positive distance, preserving the caller's fallback for invalid values.
/// @param value Distance to normalize.
/// @param fallback Replacement for negative/non-finite input.
/// @return Non-negative distance capped at @ref SOUND3D_DISTANCE_MAX.
static double sound3d_distance_or(double value, double fallback) {
    value = sound3d_finite_or(value, fallback);
    if (value < 0.0)
        value = fallback;
    if (value > SOUND3D_DISTANCE_MAX)
        return SOUND3D_DISTANCE_MAX;
    return value;
}

/// @brief Clamp velocity components to the range accepted by Doppler math.
/// @param velocity Mutable XYZ vector; NULL is ignored.
static void sound3d_clamp_velocity3(double *velocity) {
    if (!velocity)
        return;
    velocity[0] = sound3d_clamp_abs_or(velocity[0], 0.0, SOUND3D_VELOCITY_ABS_MAX);
    velocity[1] = sound3d_clamp_abs_or(velocity[1], 0.0, SOUND3D_VELOCITY_ABS_MAX);
    velocity[2] = sound3d_clamp_abs_or(velocity[2], 0.0, SOUND3D_VELOCITY_ABS_MAX);
}

/// @brief Clamp a Doppler factor to the mixer-supported range.
/// @param value Factor to normalize.
/// @return Finite factor in `[0.5, 2]`, with invalid input mapped to `1`.
static double sound3d_doppler_or(double value) {
    value = sound3d_finite_or(value, 1.0);
    if (value < 0.5)
        return 0.5;
    if (value > 2.0)
        return 2.0;
    return value;
}

/// @brief Clamp an authored pitch multiplier to the range accepted by the mixer.
/// @param value Requested or retained pitch multiplier.
/// @return Finite multiplier in `[0.25, 4]`; invalid/non-positive input becomes `1`.
static double sound3d_pitch_or(double value) {
    if (!isfinite(value) || value <= 0.0)
        return 1.0;
    if (value < SOUND3D_PITCH_MIN)
        return SOUND3D_PITCH_MIN;
    if (value > SOUND3D_PITCH_MAX)
        return SOUND3D_PITCH_MAX;
    return value;
}

/// @brief Clamp an occlusion fraction to its canonical mixer domain.
/// @param value Requested or retained occlusion amount.
/// @return Finite fraction in `[0, 1]`.
static double sound3d_occlusion_or(double value) {
    if (!isfinite(value) || value < 0.0)
        return 0.0;
    return value > 1.0 ? 1.0 : value;
}

/// @brief Compose authored and Doppler pitch without overflowing the mixer input domain.
/// @param pitch Authored playback-rate multiplier.
/// @param doppler Spatial Doppler factor.
/// @return Finite combined playback rate in `[0.25, 4]`.
static double sound3d_combined_pitch(double pitch, double doppler) {
    pitch = sound3d_pitch_or(pitch);
    doppler = sound3d_doppler_or(doppler);
    if (pitch > SOUND3D_PITCH_MAX / doppler)
        return SOUND3D_PITCH_MAX;
    return sound3d_pitch_or(pitch * doppler);
}

/// @brief Translation-unit-local copy of `rt_sound3d.c::sound3d_copy3`.
/// @details Null-source-fills-zero convention applies: missing position
///   vectors collapse to the origin rather than leaving `dst` untouched.
/// @param[out] dst Destination XYZ vector; NULL is ignored.
/// @param[in] src Source XYZ vector, or NULL to write the origin.
static void sound3d_copy3(double *dst, const double *src) {
    if (!dst)
        return;
    if (!src) {
        dst[0] = 0.0;
        dst[1] = 0.0;
        dst[2] = 0.0;
        return;
    }
    dst[0] = sound3d_clamp_abs_or(src[0], 0.0, SOUND3D_COMPONENT_ABS_MAX);
    dst[1] = sound3d_clamp_abs_or(src[1], 0.0, SOUND3D_COMPONENT_ABS_MAX);
    dst[2] = sound3d_clamp_abs_or(src[2], 0.0, SOUND3D_COMPONENT_ABS_MAX);
}

/// @brief Build a sanitized listener-state snapshot without mutating the listener object.
/// @param listener Listener object, or NULL for identity state.
/// @param out_state Receives the sanitized snapshot.
static void sound3d_listener_sanitized_state(const rt_soundlistener3d *listener,
                                             rt_sound3d_listener_state *out_state) {
    if (!out_state)
        return;
    if (!listener) {
        rt_sound3d_listener_state_identity(out_state);
        return;
    }
    rt_sound3d_listener_state_set_pose(out_state,
                                       listener->state.position,
                                       listener->state.forward,
                                       listener->state.up,
                                       listener->state.velocity);
    sound3d_clamp_velocity3(out_state->velocity);
}

/// @brief Translation-unit-local copy of `rt_sound3d.c::sound3d_vec_from_obj`.
/// @details Decodes an `rt_vec3` object through the accessor API; null
///   collapses to origin. Returns 0 only when a non-null object is not a Vec3.
/// @param[in] vec Optional runtime Vec3 object to decode.
/// @param[out] out_xyz Destination XYZ vector.
/// @return `1` for a null or valid Vec3 input; `0` for an invalid object or output buffer.
static int sound3d_vec_from_obj(void *vec, double *out_xyz) {
    if (!out_xyz)
        return 0;
    if (!vec) {
        out_xyz[0] = 0.0;
        out_xyz[1] = 0.0;
        out_xyz[2] = 0.0;
        return 1;
    }
    if (!rt_g3d_is_vec3(vec))
        return 0;
    out_xyz[0] = sound3d_clamp_abs_or(rt_vec3_x(vec), 0.0, SOUND3D_COMPONENT_ABS_MAX);
    out_xyz[1] = sound3d_clamp_abs_or(rt_vec3_y(vec), 0.0, SOUND3D_COMPONENT_ABS_MAX);
    out_xyz[2] = sound3d_clamp_abs_or(rt_vec3_z(vec), 0.0, SOUND3D_COMPONENT_ABS_MAX);
    return 1;
}

/// @brief Compute velocity from position delta and update the last-position cache.
/// @details Skips velocity computation on the first call (no prior position to
///          differentiate against) or when `dt < 1e-8` (avoids divide-by-near-zero
///          producing huge spurious velocities). The last-position cache is
///          always updated so the *next* call has a baseline. Velocity is
///          intended to drive Doppler effects in the audio core.
/// @param[out] velocity Receives the bounded velocity when a prior position and usable delta
/// time are available.
/// @param[in,out] last_position Cached position updated to @p new_position.
/// @param[in,out] has_last_position Flag indicating whether @p last_position is initialized.
/// @param[in] new_position Current position used for the velocity calculation.
/// @param[in] dt Elapsed time in seconds, sanitized to the supported synchronization range.
static void sound3d_update_velocity(double *velocity,
                                    double *last_position,
                                    int8_t *has_last_position,
                                    const double *new_position,
                                    double dt) {
    if (!velocity || !last_position || !has_last_position || !new_position)
        return;
    if (!isfinite(dt) || dt < 0.0)
        dt = 0.0;
    if (dt > SOUND3D_SYNC_DT_MAX)
        dt = SOUND3D_SYNC_DT_MAX;
    if (*has_last_position && dt <= 1e-8)
        return;
    if (*has_last_position) {
        velocity[0] = sound3d_clamp_abs_or(
            (new_position[0] - last_position[0]) / dt, 0.0, SOUND3D_VELOCITY_ABS_MAX);
        velocity[1] = sound3d_clamp_abs_or(
            (new_position[1] - last_position[1]) / dt, 0.0, SOUND3D_VELOCITY_ABS_MAX);
        velocity[2] = sound3d_clamp_abs_or(
            (new_position[2] - last_position[2]) / dt, 0.0, SOUND3D_VELOCITY_ABS_MAX);
    }
    last_position[0] = sound3d_clamp_abs_or(new_position[0], 0.0, SOUND3D_COMPONENT_ABS_MAX);
    last_position[1] = sound3d_clamp_abs_or(new_position[1], 0.0, SOUND3D_COMPONENT_ABS_MAX);
    last_position[2] = sound3d_clamp_abs_or(new_position[2], 0.0, SOUND3D_COMPONENT_ABS_MAX);
    *has_last_position = 1;
}

/// @brief Clamp a requested volume to the runtime's 0-100 scale.
/// @details Zia / BASIC user code may pass arbitrary values (negative
///   sentinels or "1000 = maximum" style conventions from other
///   engines). This chokepoint normalizes everything to the actual
///   mixer range so downstream code can assume valid input.
/// @param volume Requested runtime volume.
/// @return Volume clamped to the inclusive range `[0, 100]`.
static int64_t sound3d_clamp_volume(int64_t volume) {
    if (volume < 0)
        return 0;
    if (volume > 100)
        return 100;
    return volume;
}

/// @brief Repair all retained listener numerics and canonicalize private flags.
/// @param listener Mutable listener object; NULL is ignored.
static void sound3d_listener_repair_state(rt_soundlistener3d *listener) {
    rt_sound3d_listener_state repaired;
    if (!listener)
        return;
    sound3d_listener_sanitized_state(listener, &repaired);
    listener->state = repaired;
    listener->state.valid = 1;
    listener->is_active = s_active_listener_obj == listener ? 1 : 0;
    listener->has_last_sync_position = listener->has_last_sync_position ? 1 : 0;
    if (listener->has_last_sync_position)
        sound3d_copy3(listener->last_sync_position, listener->last_sync_position);
}

/// @brief Repair all retained source numerics before they reach spatial or mixer code.
/// @param source Mutable source object; NULL is ignored.
static void sound3d_source_repair_state(rt_soundsource3d *source) {
    if (!source)
        return;
    sound3d_copy3(source->position, source->position);
    sound3d_clamp_velocity3(source->velocity);
    source->doppler_factor = sound3d_doppler_or(source->doppler_factor);
    source->has_last_sync_position = source->has_last_sync_position ? 1 : 0;
    if (source->has_last_sync_position)
        sound3d_copy3(source->last_sync_position, source->last_sync_position);
    source->ref_distance = sound3d_distance_or(source->ref_distance, 1.0);
    if (source->ref_distance <= 0.0)
        source->ref_distance = 1.0;
    source->max_distance = sound3d_distance_or(source->max_distance, 0.0);
    if (source->max_distance > 0.0 && source->max_distance < source->ref_distance)
        source->max_distance = source->ref_distance;
    source->volume = sound3d_clamp_volume(source->volume);
    if (source->voice_id < 0)
        source->voice_id = 0;
    source->looping = source->looping ? 1 : 0;
    source->pitch = sound3d_pitch_or(source->pitch);
    source->occlusion = sound3d_occlusion_or(source->occlusion);
    if (source->mix_group < 0 || source->mix_group >= RT_MIXGROUP_MAX_GROUPS)
        source->mix_group = RT_MIXGROUP_SFX;
}

/// @brief Push a listener onto the head of the global listener list.
/// @details The list is an intrusive doubly-linked list (prev/next fields
///   live on the listener struct itself) used by `sync_bindings` to walk
///   every live listener once per tick. Insertion at head is O(1) and
///   order doesn't matter since every node is visited uniformly.
/// @param[in,out] listener Listener to link into the traversal list; NULL is ignored.
static void sound3d_listener_list_add(rt_soundlistener3d *listener) {
    RT_ASSERT_MAIN_THREAD();
    if (!listener)
        return;
    listener->prev = NULL;
    listener->next = s_listener_head;
    if (s_listener_head)
        s_listener_head->prev = listener;
    s_listener_head = listener;
}

/// @brief Splice a listener out of the global listener list.
/// @details Handles all three cases — head node (update `s_listener_head`),
///   middle node (relink neighbors' prev/next), tail node (just clear the
///   previous node's next). Both prev/next fields are zeroed on exit so the
///   listener can be re-added later without carrying stale pointers. Called
///   by the finalizer and by deactivation paths.
/// @param[in,out] listener Listener to unlink from the traversal list; NULL is ignored.
static void sound3d_listener_list_remove(rt_soundlistener3d *listener) {
    RT_ASSERT_MAIN_THREAD();
    if (!listener)
        return;
    if (listener->prev)
        listener->prev->next = listener->next;
    else if (s_listener_head == listener)
        s_listener_head = listener->next;
    if (listener->next)
        listener->next->prev = listener->prev;
    listener->prev = NULL;
    listener->next = NULL;
}

/// @brief Push an audio source onto the head of the global source list.
/// @details Mirrors `sound3d_listener_list_add` — intrusive doubly-linked
///   list, O(1) insertion, iteration order immaterial because every live
///   source is visited uniformly during `sync_bindings`.
/// @param[in,out] source Source to link into the traversal list; NULL is ignored.
static void sound3d_source_list_add(rt_soundsource3d *source) {
    RT_ASSERT_MAIN_THREAD();
    if (!source)
        return;
    source->prev = NULL;
    source->next = s_source_head;
    if (s_source_head)
        s_source_head->prev = source;
    s_source_head = source;
}

/// @brief Splice an audio source out of the global source list.
/// @details Symmetric to `sound3d_listener_list_remove`; clears both prev
///   and next on exit so the source can re-enter the list cleanly.
/// @param[in,out] source Source to unlink from the traversal list; NULL is ignored.
static void sound3d_source_list_remove(rt_soundsource3d *source) {
    RT_ASSERT_MAIN_THREAD();
    if (!source)
        return;
    if (source->prev)
        source->prev->next = source->next;
    else if (s_source_head == source)
        s_source_head = source->next;
    if (source->next)
        source->next->prev = source->prev;
    source->prev = NULL;
    source->next = NULL;
}

/// @brief Resolve a SceneNode3D's world-space position without allocating wrapper objects.
/// @param[in] node Optional SceneNode3D object whose transform should be queried.
/// @param[out] out_position Receives the world position, or the origin for a null node.
static void sound3d_get_node_world_position(void *node, double *out_position) {
    if (!out_position)
        return;
    if (!node) {
        out_position[0] = 0.0;
        out_position[1] = 0.0;
        out_position[2] = 0.0;
        return;
    }
    if (!rt_scene_node3d_get_world_position_components(
            node, &out_position[0], &out_position[1], &out_position[2])) {
        void *local_position = rt_scene_node3d_get_position(node);
        sound3d_vec_from_obj(local_position, out_position);
        sound3d_release_local(local_position);
    }
}

/// @brief Resolve a SceneNode3D's world-space direction without allocating wrapper objects.
/// @details Multiplies only the linear 3x3 part of the row-major world matrix,
///          so translation cannot cancel or clip a valid direction at large
///          world coordinates. Matrix and input lanes are scaled independently
///          before multiplication to prevent finite intermediate overflow.
///          Result is normalized; invalid or degenerate transforms fall back
///          to the supplied canonical direction.
/// @param[in] node Optional SceneNode3D object whose transform should be applied.
/// @param[in] local_direction Local-space direction to transform.
/// @param[in] fallback Direction copied when the node transform is unavailable or degenerate.
/// @param[out] out_direction Receives the normalized world-space direction.
static void sound3d_get_node_world_direction(void *node,
                                             const double *local_direction,
                                             const double *fallback,
                                             double *out_direction) {
    double world_matrix[16];
    double matrix_scale = 0.0;
    double local_scale;
    double direction_scale;
    double length;
    size_t row;
    size_t column;
    if (!out_direction)
        return;
    if (!node || !local_direction ||
        !rt_scene_node3d_get_world_matrix_components(node, world_matrix)) {
        sound3d_copy3(out_direction, fallback);
        return;
    }

    for (row = 0; row < 3; ++row) {
        for (column = 0; column < 3; ++column) {
            double lane = fabs(world_matrix[row * 4 + column]);
            if (!isfinite(lane)) {
                sound3d_copy3(out_direction, fallback);
                return;
            }
            if (lane > matrix_scale)
                matrix_scale = lane;
        }
    }
    local_scale =
        fmax(fabs(local_direction[0]), fmax(fabs(local_direction[1]), fabs(local_direction[2])));
    if (!isfinite(matrix_scale) || matrix_scale <= 0.0 || !isfinite(local_scale) ||
        local_scale <= 0.0) {
        sound3d_copy3(out_direction, fallback);
        return;
    }

    for (row = 0; row < 3; ++row) {
        out_direction[row] =
            (world_matrix[row * 4] / matrix_scale) * (local_direction[0] / local_scale) +
            (world_matrix[row * 4 + 1] / matrix_scale) * (local_direction[1] / local_scale) +
            (world_matrix[row * 4 + 2] / matrix_scale) * (local_direction[2] / local_scale);
    }
    direction_scale =
        fmax(fabs(out_direction[0]), fmax(fabs(out_direction[1]), fabs(out_direction[2])));
    if (!isfinite(direction_scale) || direction_scale <= 1e-8) {
        sound3d_copy3(out_direction, fallback);
        return;
    }
    out_direction[0] /= direction_scale;
    out_direction[1] /= direction_scale;
    out_direction[2] /= direction_scale;
    length = sqrt(out_direction[0] * out_direction[0] + out_direction[1] * out_direction[1] +
                  out_direction[2] * out_direction[2]);
    if (!isfinite(length) || length <= 1e-8) {
        sound3d_copy3(out_direction, fallback);
        return;
    }
    out_direction[0] /= length;
    out_direction[1] /= length;
    out_direction[2] /= length;
}

/// @brief World-space forward of @p node (its local -Z mapped through the node transform).
/// @details Falls back to world -Z when the node has no usable transform.
/// @param[in] node Optional SceneNode3D object to query.
/// @param[out] out_forward Receives the normalized world-space forward vector.
static void sound3d_get_node_world_forward(void *node, double *out_forward) {
    static const double local_forward[3] = {0.0, 0.0, -1.0};
    static const double fallback_forward[3] = {0.0, 0.0, -1.0};
    sound3d_get_node_world_direction(node, local_forward, fallback_forward, out_forward);
}

/// @brief World-space up of @p node (its local +Y mapped through the node transform).
/// @details Falls back to world +Y when the node has no usable transform.
/// @param[in] node Optional SceneNode3D object to query.
/// @param[out] out_up Receives the normalized world-space up vector.
static void sound3d_get_node_world_up(void *node, double *out_up) {
    static const double local_up[3] = {0.0, 1.0, 0.0};
    static const double fallback_up[3] = {0.0, 1.0, 0.0};
    sound3d_get_node_world_direction(node, local_up, fallback_up, out_up);
}

/// @brief Copy a listener's pose into the audio core's active-listener slot.
/// @details Only the one listener marked `is_active` contributes to spatial
///   mixing — other listeners update their own local state without pushing
///   it to the core, so there's no cross-talk between listeners tracked in
///   parallel (e.g. for split-screen or debug views). The audio core holds
///   a copy, so the listener's state can continue to change without
///   immediately perturbing in-flight voice params until the next sync.
/// @param[in] listener Listener whose state should be pushed when active.
static void sound3d_listener_push_active_state(rt_soundlistener3d *listener) {
    if (listener && listener->is_active)
        rt_sound3d_set_active_listener_state(&listener->state);
}

/// @brief Re-sync a listener's state from its bound camera or scene node.
/// @details Camera binding takes precedence over node binding when both are
///          set. Either source provides position + forward, after which
///          velocity is derived from the position delta over `dt` and the
///          listener-state struct is updated. If the listener is the
///          currently-active one, its state is also pushed into the audio
///          core so spatial mixing immediately reflects the new pose.
///          No-op when the listener has no binding at all (free-floating
///          listener whose state is set manually).
/// @param[in,out] listener Listener whose bound transform and velocity should be synchronized.
/// @param[in] dt Elapsed time in seconds used to derive velocity.
static void sound3d_listener_sync_binding(rt_soundlistener3d *listener, double dt) {
    double position[3];
    double forward[3];
    double up[3];
    if (!listener)
        return;
    sound3d_listener_repair_state(listener);

    if (listener->bound_camera) {
        void *camera = rt_g3d_has_class(listener->bound_camera, RT_G3D_CAMERA3D_CLASS_ID)
                           ? listener->bound_camera
                           : NULL;
        if (!camera) {
            listener->bound_camera = NULL;
        } else {
            void *camera_forward = rt_camera3d_get_forward(camera);
            void *camera_up = rt_camera3d_get_up(camera);
            if (!rt_camera3d_get_position_components(
                    camera, &position[0], &position[1], &position[2]))
                sound3d_copy3(position, listener->state.position);
            if (!camera_forward || !sound3d_vec_from_obj(camera_forward, forward))
                sound3d_copy3(forward, listener->state.forward);
            if (!camera_up || !sound3d_vec_from_obj(camera_up, up))
                sound3d_copy3(up, listener->state.up);
            sound3d_release_local(camera_forward);
            sound3d_release_local(camera_up);
            sound3d_update_velocity(listener->state.velocity,
                                    listener->last_sync_position,
                                    &listener->has_last_sync_position,
                                    position,
                                    dt);
            rt_sound3d_listener_state_set_pose(
                &listener->state, position, forward, up, listener->state.velocity);
            sound3d_listener_push_active_state(listener);
            return;
        }
    }

    if (listener->bound_node) {
        void *node = rt_g3d_has_class(listener->bound_node, RT_G3D_SCENENODE3D_CLASS_ID)
                         ? listener->bound_node
                         : NULL;
        if (!node) {
            listener->bound_node = NULL;
            return;
        }
        sound3d_get_node_world_position(node, position);
        sound3d_get_node_world_forward(node, forward);
        sound3d_get_node_world_up(node, up);
        sound3d_update_velocity(listener->state.velocity,
                                listener->last_sync_position,
                                &listener->has_last_sync_position,
                                position,
                                dt);
        rt_sound3d_listener_state_set_pose(
            &listener->state, position, forward, up, listener->state.velocity);
        sound3d_listener_push_active_state(listener);
    }
}

/// @brief Re-sync the active listener's bound pose with `dt = 0`.
/// @details Called from the source-mutation path so spatial params are
///   computed against an up-to-date listener pose without ticking the
///   velocity calculation (which would introduce a spurious jump). Zero
///   dt means the velocity-derivation step is a no-op; only position and
///   forward get refreshed.
static void sound3d_refresh_active_listener(void) {
    if (s_active_listener_obj)
        sound3d_listener_sync_binding(s_active_listener_obj, 0.0);
}

/// @brief Reap a stale voice id when the underlying audio voice has finished.
/// @details Audio voices complete asynchronously (one-shot clips reach end,
///   mixer culls under-resourced voices, etc.) and the scripting layer has
///   no callback for that. Sources lazily check liveness here and zero
///   `voice_id` so the next Play() call grabs a fresh voice instead of
///   sending commands to a now-recycled id. Returns whether the source
///   currently has a live voice, so callers can fast-skip spatial updates
///   for silent sources.
/// @param[in,out] source Source whose cached voice identifier should be checked.
/// @return Nonzero when the source owns a currently playing voice; otherwise zero.
static int8_t sound3d_source_refresh_play_state(rt_soundsource3d *source) {
    if (!source)
        return 0;
    if (source->voice_id <= 0) {
        source->voice_id = 0;
        return 0;
    }
    if (!rt_voice_is_playing(source->voice_id)) {
        source->voice_id = 0;
        return 0;
    }
    return 1;
}

/// @brief Recompute and push spatial volume + pan to a source's underlying voice.
/// @details Skips silently when the source has no live voice (refresh-play-state
///          will reap stale voice IDs as a side effect). Refreshes the active
///          listener first so the calculation uses an up-to-date pose, then
///          delegates to `rt_sound3d_compute_voice_params_ex` for the actual
///          attenuation + pan math, then pushes the results to the voice via
///          `rt_voice_set_volume` / `rt_voice_set_pan`. Called from every
///          source-mutating setter so changes take effect immediately rather
///          than at the next sync tick.
/// @param[in,out] source Source whose live voice should receive updated spatial parameters.
static void sound3d_source_apply_spatial(rt_soundsource3d *source) {
    rt_sound3d_listener_state listener;
    int64_t spatial_volume = 0;
    int64_t spatial_pan = 0;
    if (!source || !sound3d_source_refresh_play_state(source))
        return;
    sound3d_refresh_active_listener();
    rt_sound3d_get_effective_listener_state(&listener);
    rt_sound3d_compute_voice_params_ex(&listener,
                                       source->position,
                                       source->velocity,
                                       source->ref_distance,
                                       source->max_distance,
                                       sound3d_clamp_volume(source->volume),
                                       &spatial_volume,
                                       &spatial_pan,
                                       &source->doppler_factor);
    rt_voice_set_volume(source->voice_id, spatial_volume);
    rt_voice_set_pan(source->voice_id, spatial_pan);
    /* Doppler and the user pitch compose multiplicatively into the voice's
     * playback rate; occlusion is forwarded for the mixer's smoothed sweep. */
    rt_voice_set_pitch(source->voice_id,
                       sound3d_combined_pitch(source->pitch, source->doppler_factor));
    rt_voice_set_occlusion(source->voice_id, source->occlusion);
}

/// @brief Refresh the cached Doppler factor even when the source is not playing.
/// @param[in,out] source Source whose Doppler factor should be recomputed.
static void sound3d_source_refresh_doppler(rt_soundsource3d *source) {
    rt_sound3d_listener_state listener;
    int64_t ignored_volume = 0;
    int64_t ignored_pan = 0;
    if (!source)
        return;
    sound3d_refresh_active_listener();
    rt_sound3d_get_effective_listener_state(&listener);
    rt_sound3d_compute_voice_params_ex(&listener,
                                       source->position,
                                       source->velocity,
                                       source->ref_distance,
                                       source->max_distance,
                                       sound3d_clamp_volume(source->volume),
                                       &ignored_volume,
                                       &ignored_pan,
                                       &source->doppler_factor);
}

/// @brief Re-sync a source's position from its bound scene node.
/// @details No-op when the source isn't bound to a node (free-floating
///          source whose position is set manually). After updating
///          position + velocity, applies spatial mixing so the next
///          mixer tick uses the new values.
/// @param[in,out] source Source whose bound transform and velocity should be synchronized.
/// @param[in] dt Elapsed time in seconds used to derive velocity.
static void sound3d_source_sync_binding(rt_soundsource3d *source, double dt) {
    double position[3];
    if (!source || !source->bound_node)
        return;
    sound3d_source_repair_state(source);
    if (!rt_g3d_has_class(source->bound_node, RT_G3D_SCENENODE3D_CLASS_ID)) {
        source->bound_node = NULL;
        return;
    }
    sound3d_get_node_world_position(source->bound_node, position);
    sound3d_update_velocity(source->velocity,
                            source->last_sync_position,
                            &source->has_last_sync_position,
                            position,
                            dt);
    sound3d_copy3(source->position, position);
    sound3d_source_apply_spatial(source);
}

/// @brief GC finalizer for a 3D audio listener.
/// @details Three-step teardown: (1) if this listener was the active one,
///   clear the audio core's active-listener slot so in-flight voices
///   gracefully degrade to a null-listener pose rather than dereferencing
///   the freed struct; (2) unlink from the global listener list so
///   `sync_bindings` stops visiting it; (3) drop the scene-node and camera
///   back-references. The order (active-check first) is deliberate — the
///   core must be cleared before the listener memory is eligible for reuse.
/// @param[in,out] obj Listener object being finalized; NULL is ignored.
static void sound3d_listener_finalize(void *obj) {
    rt_soundlistener3d *listener = (rt_soundlistener3d *)obj;
    if (!listener)
        return;
    if (s_active_listener_obj == listener) {
        s_active_listener_obj = NULL;
        rt_sound3d_clear_active_listener_state();
    }
    sound3d_listener_list_remove(listener);
    sound3d_release_class_ref(&listener->bound_node, RT_G3D_SCENENODE3D_CLASS_ID);
    sound3d_release_class_ref(&listener->bound_camera, RT_G3D_CAMERA3D_CLASS_ID);
}

/// @brief GC finalizer for a 3D audio source.
/// @details Stops any live voice before releasing the source so the mixer
///   doesn't keep reading from a freed sound buffer one tick after the
///   source disappears. Then unlinks from the global source list (so
///   `sync_bindings` skips it), and drops references to the sound asset
///   and bound scene node. The sound's own refcount may still keep its
///   buffer alive if other sources share it — only this one source's
///   handle goes away.
/// @param[in,out] obj Source object being finalized; NULL is ignored.
static void sound3d_source_finalize(void *obj) {
    rt_soundsource3d *source = (rt_soundsource3d *)obj;
    if (!source)
        return;
    if (source->voice_id > 0)
        rt_voice_stop(source->voice_id);
    source->voice_id = 0;
    sound3d_source_list_remove(source);
    sound3d_release_ref(&source->sound);
    sound3d_release_class_ref(&source->bound_node, RT_G3D_SCENENODE3D_CLASS_ID);
}

/// @brief Per-frame tick: walk every live SoundListener3D and SoundSource3D, and re-resolve
/// each one's bound scene-node or camera into world position+forward, computing velocity from
/// the position delta over `dt`. Called by the game loop so spatial audio tracks moving entities
/// without per-source manual updates.
/// @param dt Frame delta in seconds, normalized to `[0, 1]`.
void rt_sound3d_sync_bindings(double dt) {
    RT_ASSERT_MAIN_THREAD();
    rt_soundlistener3d *listener = s_listener_head;
    rt_soundsource3d *source = s_source_head;
    if (!isfinite(dt) || dt < 0.0)
        dt = 0.0;
    if (dt > SOUND3D_SYNC_DT_MAX)
        dt = SOUND3D_SYNC_DT_MAX;
    while (listener) {
        sound3d_listener_sync_binding(listener, dt);
        listener = listener->next;
    }
    while (source) {
        sound3d_source_sync_binding(source, dt);
        source = source->next;
    }
}

/// @brief Create a 3D audio listener (the "ears" of the scene). Initializes to identity
/// (origin, forward = -Z, zero velocity). The first listener constructed becomes the active
/// one automatically; subsequent ones must be activated with `_set_is_active`.
/// @return Caller-owned SoundListener3D object, or NULL on allocation failure.
void *rt_soundlistener3d_new(void) {
    rt_soundlistener3d *listener = (rt_soundlistener3d *)rt_obj_new_i64(
        RT_G3D_SOUNDLISTENER3D_CLASS_ID, (int64_t)sizeof(rt_soundlistener3d));
    if (!listener)
        return NULL;
    memset(listener, 0, sizeof(*listener));
    rt_sound3d_listener_state_identity(&listener->state);
    sound3d_listener_list_add(listener);
    rt_obj_set_finalizer(listener, sound3d_listener_finalize);
    if (!s_active_listener_obj)
        rt_soundlistener3d_set_is_active(listener, 1);
    return listener;
}

/// @brief Read the listener's world-space position. If the listener is bound to a node/camera,
/// re-syncs the binding first to ensure the returned value reflects the current transform.
/// @param obj SoundListener3D object.
/// @return New Vec3 position object, or NULL for an invalid listener/allocation failure.
void *rt_soundlistener3d_get_position(void *obj) {
    rt_soundlistener3d *listener = sound3d_listener_checked(obj);
    rt_sound3d_listener_state state;
    if (!listener)
        return NULL;
    sound3d_listener_sync_binding(listener, 0.0);
    sound3d_listener_sanitized_state(listener, &state);
    return rt_vec3_new(state.position[0], state.position[1], state.position[2]);
}

/// @brief Manually set the listener world position. Resets the velocity tracker so the next
/// sync starts fresh (no spurious large-velocity blip from a teleport).
/// @param obj SoundListener3D object.
/// @param position Vec3 world position; invalid handles are ignored.
void rt_soundlistener3d_set_position(void *obj, void *position) {
    rt_soundlistener3d *listener = sound3d_listener_checked(obj);
    double pos[3];
    if (!listener || !position)
        return;
    if (!sound3d_vec_from_obj(position, pos))
        return;
    sound3d_copy3(listener->state.position, pos);
    sound3d_copy3(listener->last_sync_position, pos);
    listener->has_last_sync_position = 1;
    sound3d_listener_push_active_state(listener);
}

/// @brief Convenience overload of `_set_position` taking three doubles instead of a Vec3.
/// @param obj SoundListener3D object.
/// @param x World X coordinate.
/// @param y World Y coordinate.
/// @param z World Z coordinate.
void rt_soundlistener3d_set_position_vec(void *obj, double x, double y, double z) {
    rt_soundlistener3d *listener = sound3d_listener_checked(obj);
    const double position[3] = {x, y, z};
    if (!listener)
        return;
    sound3d_copy3(listener->state.position, position);
    sound3d_copy3(listener->last_sync_position, listener->state.position);
    listener->has_last_sync_position = 1;
    sound3d_listener_push_active_state(listener);
}

/// @brief Read the listener's world-space forward (look-at) vector. Re-syncs binding first
/// so the result tracks attached nodes/cameras.
/// @param obj SoundListener3D object.
/// @return New normalized Vec3 direction, or NULL for an invalid listener/allocation failure.
void *rt_soundlistener3d_get_forward(void *obj) {
    rt_soundlistener3d *listener = sound3d_listener_checked(obj);
    rt_sound3d_listener_state state;
    if (!listener)
        return NULL;
    sound3d_listener_sync_binding(listener, 0.0);
    sound3d_listener_sanitized_state(listener, &state);
    return rt_vec3_new(state.forward[0], state.forward[1], state.forward[2]);
}

/// @brief Set the listener's forward vector explicitly (for left/right pan calculations).
/// The vector is normalized inside the audio core; magnitude is irrelevant.
/// @param obj SoundListener3D object.
/// @param forward Vec3 facing direction.
void rt_soundlistener3d_set_forward(void *obj, void *forward) {
    rt_soundlistener3d *listener = sound3d_listener_checked(obj);
    double fwd[3];
    if (!listener || !forward)
        return;
    if (!sound3d_vec_from_obj(forward, fwd))
        return;
    rt_sound3d_listener_state_set_pose(&listener->state,
                                       listener->state.position,
                                       fwd,
                                       listener->state.up,
                                       listener->state.velocity);
    sound3d_listener_push_active_state(listener);
}

/// @brief Read the listener's world-space up vector. Re-syncs binding first
/// so the result tracks attached nodes.
/// @param obj SoundListener3D object.
/// @return New normalized Vec3 up vector, or NULL for an invalid listener/allocation failure.
void *rt_soundlistener3d_get_up(void *obj) {
    rt_soundlistener3d *listener = sound3d_listener_checked(obj);
    rt_sound3d_listener_state state;
    if (!listener)
        return NULL;
    sound3d_listener_sync_binding(listener, 0.0);
    sound3d_listener_sanitized_state(listener, &state);
    return rt_vec3_new(state.up[0], state.up[1], state.up[2]);
}

/// @brief Set the listener's up vector explicitly. The basis is orthonormalized
/// against the current forward vector inside the audio core.
/// @param obj SoundListener3D object.
/// @param up Vec3 up direction.
void rt_soundlistener3d_set_up(void *obj, void *up) {
    rt_soundlistener3d *listener = sound3d_listener_checked(obj);
    double upv[3];
    if (!listener || !up)
        return;
    if (!sound3d_vec_from_obj(up, upv))
        return;
    rt_sound3d_listener_state_set_pose(&listener->state,
                                       listener->state.position,
                                       listener->state.forward,
                                       upv,
                                       listener->state.velocity);
    sound3d_listener_push_active_state(listener);
}

/// @brief Read the listener's velocity (used for Doppler effects). Auto-synced from binding.
/// @param obj SoundListener3D object.
/// @return New Vec3 velocity, or NULL for an invalid listener/allocation failure.
void *rt_soundlistener3d_get_velocity(void *obj) {
    rt_soundlistener3d *listener = sound3d_listener_checked(obj);
    rt_sound3d_listener_state state;
    if (!listener)
        return NULL;
    sound3d_listener_sync_binding(listener, 0.0);
    sound3d_listener_sanitized_state(listener, &state);
    return rt_vec3_new(state.velocity[0], state.velocity[1], state.velocity[2]);
}

/// @brief Override the listener's velocity. Useful for non-physical movements (e.g., camera
/// scripted shake) where the position-delta-based auto-velocity would lie about real motion.
/// @param obj SoundListener3D object.
/// @param velocity Vec3 world velocity.
void rt_soundlistener3d_set_velocity(void *obj, void *velocity) {
    rt_soundlistener3d *listener = sound3d_listener_checked(obj);
    if (!listener || !velocity)
        return;
    if (!sound3d_vec_from_obj(velocity, listener->state.velocity))
        return;
    sound3d_clamp_velocity3(listener->state.velocity);
    sound3d_listener_push_active_state(listener);
}

/// @brief Return 1 if this listener is the currently-active one (i.e., the one feeding the
/// audio core's pan/volume calculations). Only one listener can be active at a time.
/// @param obj SoundListener3D object.
/// @return `1` when active, otherwise `0`.
int8_t rt_soundlistener3d_get_is_active(void *obj) {
    rt_soundlistener3d *listener = sound3d_listener_checked(obj);
    return listener && listener->is_active ? 1 : 0;
}

/// @brief Make this listener the active one (deactivating any previously-active listener).
/// Setting `active=0` deactivates this listener and clears the audio core's listener state,
/// which makes spatial sources use the low-level fallback listener.
/// @param obj SoundListener3D object.
/// @param active Non-zero to activate; zero to deactivate this listener.
void rt_soundlistener3d_set_is_active(void *obj, int8_t active) {
    RT_ASSERT_MAIN_THREAD();
    rt_soundlistener3d *listener = sound3d_listener_checked(obj);
    if (!listener)
        return;

    if (active) {
        if (s_active_listener_obj && s_active_listener_obj != listener)
            s_active_listener_obj->is_active = 0;
        s_active_listener_obj = listener;
        listener->is_active = 1;
        sound3d_listener_sync_binding(listener, 0.0);
        sound3d_listener_push_active_state(listener);
        return;
    }

    listener->is_active = 0;
    if (s_active_listener_obj == listener) {
        s_active_listener_obj = NULL;
        rt_sound3d_clear_active_listener_state();
    }
}

/// @brief Bind the listener to a SceneNode3D — its position and forward will track the node's
/// world transform every `_sync_bindings` tick. Replaces any prior node/camera binding.
/// @param obj SoundListener3D object.
/// @param node SceneNode3D object to retain, or NULL to clear.
void rt_soundlistener3d_bind_node(void *obj, void *node) {
    rt_soundlistener3d *listener = sound3d_listener_checked(obj);
    if (!listener)
        return;
    if (node && !rt_g3d_has_class(node, RT_G3D_SCENENODE3D_CLASS_ID))
        return;
    if (node)
        rt_obj_retain_maybe(node);
    sound3d_release_class_ref(&listener->bound_node, RT_G3D_SCENENODE3D_CLASS_ID);
    listener->bound_node = node;
    sound3d_release_class_ref(&listener->bound_camera, RT_G3D_CAMERA3D_CLASS_ID);
    listener->has_last_sync_position = 0;
    sound3d_listener_sync_binding(listener, 0.0);
}

/// @brief Detach the listener from any bound scene node. Subsequent position/forward stay at
/// the most recent values until manually changed.
/// @param obj SoundListener3D object.
void rt_soundlistener3d_clear_node_binding(void *obj) {
    rt_soundlistener3d *listener = sound3d_listener_checked(obj);
    if (!listener)
        return;
    sound3d_release_class_ref(&listener->bound_node, RT_G3D_SCENENODE3D_CLASS_ID);
}

/// @brief Bind the listener to a Camera3D — preferred over node binding for FPS-style audio
/// since the camera's forward already encodes head orientation. Replaces any prior binding.
/// @param obj SoundListener3D object.
/// @param camera Camera3D object to retain, or NULL to clear.
void rt_soundlistener3d_bind_camera(void *obj, void *camera) {
    rt_soundlistener3d *listener = sound3d_listener_checked(obj);
    if (!listener)
        return;
    if (camera && !rt_g3d_has_class(camera, RT_G3D_CAMERA3D_CLASS_ID))
        return;
    if (camera)
        rt_obj_retain_maybe(camera);
    sound3d_release_class_ref(&listener->bound_camera, RT_G3D_CAMERA3D_CLASS_ID);
    listener->bound_camera = camera;
    sound3d_release_class_ref(&listener->bound_node, RT_G3D_SCENENODE3D_CLASS_ID);
    listener->has_last_sync_position = 0;
    sound3d_listener_sync_binding(listener, 0.0);
}

/// @brief Detach the listener from any bound camera. Position/forward freeze at last sync.
/// @param obj SoundListener3D object.
void rt_soundlistener3d_clear_camera_binding(void *obj) {
    rt_soundlistener3d *listener = sound3d_listener_checked(obj);
    if (!listener)
        return;
    sound3d_release_class_ref(&listener->bound_camera, RT_G3D_CAMERA3D_CLASS_ID);
}

/// @brief Create a 3D-positioned audio source playing `sound`. Defaults: full-volume radius
/// 1 world unit, max distance 50 world units, volume 100/100, non-looping, position at
/// origin. Spatial volume/pan are computed per-frame from the active listener once playback
/// starts via `_play`.
/// @param sound Sound handle retained by the new object; may be NULL.
/// @return Caller-owned SoundSource3D object, or NULL on allocation failure.
void *rt_soundsource3d_new(void *sound) {
    rt_soundsource3d *source = (rt_soundsource3d *)rt_obj_new_i64(
        RT_G3D_SOUNDSOURCE3D_CLASS_ID, (int64_t)sizeof(rt_soundsource3d));
    if (!source)
        return NULL;
    memset(source, 0, sizeof(*source));
    if (sound)
        rt_obj_retain_maybe(sound);
    source->sound = sound;
    source->doppler_factor = 1.0;
    source->pitch = 1.0;
    source->occlusion = 0.0;
    source->mix_group = RT_MIXGROUP_SFX;
    source->ref_distance = 1.0;
    source->max_distance = 50.0;
    source->volume = 100;
    source->looping = 0;
    sound3d_source_list_add(source);
    rt_obj_set_finalizer(source, sound3d_source_finalize);
    return source;
}

/// @brief Read the source's world-space position. Re-syncs binding so the result reflects
/// the bound node's current world transform.
/// @param obj SoundSource3D object.
/// @return New Vec3 position, or NULL for an invalid source/allocation failure.
void *rt_soundsource3d_get_position(void *obj) {
    rt_soundsource3d *source = sound3d_source_checked(obj);
    double position[3];
    if (!source)
        return NULL;
    sound3d_source_sync_binding(source, 0.0);
    sound3d_copy3(position, source->position);
    return rt_vec3_new(position[0], position[1], position[2]);
}

/// @brief Allocation-free source-position readback with binding synchronization.
/// @param obj SoundSource3D object.
/// @param x Output receiving world X.
/// @param y Output receiving world Y.
/// @param z Output receiving world Z.
/// @return One for a valid source and outputs, otherwise zero.
int8_t rt_soundsource3d_get_position_components(void *obj, double *x, double *y, double *z) {
    rt_soundsource3d *source = sound3d_source_checked(obj);
    if (!source || !x || !y || !z)
        return 0;
    sound3d_source_sync_binding(source, 0.0);
    *x = source->position[0];
    *y = source->position[1];
    *z = source->position[2];
    return 1;
}

/// @brief Manually set the source's world position. Resets the velocity tracker (no jump) and
/// re-applies spatial volume/pan immediately so playback continues at the new location.
/// @param obj SoundSource3D object.
/// @param position Vec3 world position.
void rt_soundsource3d_set_position(void *obj, void *position) {
    rt_soundsource3d *source = sound3d_source_checked(obj);
    if (!source || !position)
        return;
    if (!sound3d_vec_from_obj(position, source->position))
        return;
    sound3d_copy3(source->last_sync_position, source->position);
    source->has_last_sync_position = 1;
    sound3d_source_apply_spatial(source);
}

/// @brief Convenience overload of `_set_position` taking three doubles instead of a Vec3.
/// @param obj SoundSource3D object.
/// @param x World X coordinate.
/// @param y World Y coordinate.
/// @param z World Z coordinate.
void rt_soundsource3d_set_position_vec(void *obj, double x, double y, double z) {
    rt_soundsource3d *source = sound3d_source_checked(obj);
    const double position[3] = {x, y, z};
    if (!source)
        return;
    sound3d_copy3(source->position, position);
    sound3d_copy3(source->last_sync_position, source->position);
    source->has_last_sync_position = 1;
    sound3d_source_apply_spatial(source);
}

/// @brief Shift a source's stored position by a floating-origin rebase delta.
/// @details Node-bound sources take their position from the scene node, which the
///          scene rebase already shifted, so only unbound sources (playAt / nodeless
///          playAttached) need their fallback position moved. Subtracts the delta to
///          match the scene/physics/body rebase convention (contents move by -delta).
/// @param obj SoundSource3D object.
/// @param dx Origin X displacement.
/// @param dy Origin Y displacement.
/// @param dz Origin Z displacement.
void rt_soundsource3d_rebase_origin(void *obj, double dx, double dy, double dz) {
    rt_soundsource3d *source = sound3d_source_checked(obj);
    if (!source)
        return;
    if (source->bound_node && rt_g3d_has_class(source->bound_node, RT_G3D_SCENENODE3D_CLASS_ID))
        return;
    const double delta[3] = {sound3d_clamp_abs_or(dx, 0.0, SOUND3D_COMPONENT_ABS_MAX),
                             sound3d_clamp_abs_or(dy, 0.0, SOUND3D_COMPONENT_ABS_MAX),
                             sound3d_clamp_abs_or(dz, 0.0, SOUND3D_COMPONENT_ABS_MAX)};
    for (int lane = 0; lane < 3; lane++) {
        source->position[lane] = sound3d_clamp_abs_or(
            source->position[lane] - delta[lane], 0.0, SOUND3D_COMPONENT_ABS_MAX);
    }
    if (source->has_last_sync_position) {
        for (int lane = 0; lane < 3; lane++) {
            source->last_sync_position[lane] = sound3d_clamp_abs_or(
                source->last_sync_position[lane] - delta[lane], 0.0, SOUND3D_COMPONENT_ABS_MAX);
        }
    }
    sound3d_source_apply_spatial(source);
}

/// @brief Read the source's velocity (Doppler input). Re-syncs binding before returning.
/// @param obj SoundSource3D object.
/// @return New Vec3 velocity, or NULL for an invalid source/allocation failure.
void *rt_soundsource3d_get_velocity(void *obj) {
    rt_soundsource3d *source = sound3d_source_checked(obj);
    double velocity[3];
    if (!source)
        return NULL;
    sound3d_source_sync_binding(source, 0.0);
    sound3d_copy3(velocity, source->velocity);
    sound3d_clamp_velocity3(velocity);
    return rt_vec3_new(velocity[0], velocity[1], velocity[2]);
}

/// @brief Override the source's velocity. Skips the auto-derived position-delta velocity for
/// the next frame; useful for scripted or non-Newtonian motion.
/// @param obj SoundSource3D object.
/// @param velocity Vec3 world velocity.
void rt_soundsource3d_set_velocity(void *obj, void *velocity) {
    rt_soundsource3d *source = sound3d_source_checked(obj);
    if (!source || !velocity)
        return;
    if (!sound3d_vec_from_obj(velocity, source->velocity))
        return;
    sound3d_clamp_velocity3(source->velocity);
    source->has_last_sync_position = 0;
    sound3d_source_apply_spatial(source);
}

/// @brief Latest Doppler factor computed from listener/source velocity.
/// @param obj SoundSource3D object.
/// @return Bounded factor in `[0.5, 2]`, or `1` for an invalid source.
double rt_soundsource3d_get_doppler_factor(void *obj) {
    rt_soundsource3d *source = sound3d_source_checked(obj);
    if (!source)
        return 1.0;
    sound3d_source_sync_binding(source, 0.0);
    sound3d_source_refresh_doppler(source);
    source->doppler_factor = sound3d_doppler_or(source->doppler_factor);
    return source->doppler_factor;
}

/// @brief Maximum audible distance in world units. Beyond this the source contributes 0 volume.
/// @param obj SoundSource3D object.
/// @return Stored non-negative maximum distance, or zero for an invalid source.
double rt_soundsource3d_get_max_distance(void *obj) {
    rt_soundsource3d *source = sound3d_source_checked(obj);
    return source ? sound3d_distance_or(source->max_distance, 0.0) : 0.0;
}

/// @brief Set audible-falloff distance (clamped to ≥ 0). Larger = louder for further-away
/// sources. Spatial volume/pan are recomputed immediately so playback adapts to the new range.
/// @param obj SoundSource3D object.
/// @param max_distance Requested zero-volume distance.
void rt_soundsource3d_set_max_distance(void *obj, double max_distance) {
    rt_soundsource3d *source = sound3d_source_checked(obj);
    if (!source)
        return;
    source->max_distance = sound3d_distance_or(max_distance, 0.0);
    if (source->ref_distance > 0.0 && source->max_distance > 0.0 &&
        source->max_distance < source->ref_distance)
        source->max_distance = source->ref_distance;
    sound3d_source_apply_spatial(source);
}

/// @brief Full-volume reference distance in world units. Falloff begins past this radius.
/// @param obj SoundSource3D object.
/// @return Stored non-negative reference distance, or zero for an invalid source.
double rt_soundsource3d_get_ref_distance(void *obj) {
    rt_soundsource3d *source = sound3d_source_checked(obj);
    return source ? sound3d_distance_or(source->ref_distance, 0.0) : 0.0;
}

/// @brief Set the full-volume reference radius. The max distance is raised when needed so
/// the attenuation interval remains well-formed.
/// @param obj SoundSource3D object.
/// @param ref_distance Requested positive full-volume radius.
void rt_soundsource3d_set_ref_distance(void *obj, double ref_distance) {
    rt_soundsource3d *source = sound3d_source_checked(obj);
    if (!source)
        return;
    source->ref_distance = sound3d_distance_or(ref_distance, 1.0);
    if (source->ref_distance <= 0.0)
        source->ref_distance = 1.0;
    if (source->max_distance > 0.0 && source->max_distance < source->ref_distance)
        source->max_distance = source->ref_distance;
    sound3d_source_apply_spatial(source);
}

/// @brief Read the source's nominal volume (0..100). Spatial attenuation is applied separately.
/// @param obj SoundSource3D object.
/// @return Logical volume in `[0, 100]`, or zero for an invalid source.
int64_t rt_soundsource3d_get_volume(void *obj) {
    rt_soundsource3d *source = sound3d_source_checked(obj);
    return source ? sound3d_clamp_volume(source->volume) : 0;
}

/// @brief Set the source's nominal volume (clamped to 0..100). Re-applies spatial mixing
/// immediately so an active voice picks up the change next tick.
/// @param obj SoundSource3D object.
/// @param volume Requested logical volume.
void rt_soundsource3d_set_volume(void *obj, int64_t volume) {
    rt_soundsource3d *source = sound3d_source_checked(obj);
    if (!source)
        return;
    source->volume = sound3d_clamp_volume(volume);
    sound3d_source_apply_spatial(source);
}

/// @brief Get the source's user playback-rate multiplier (1.0 default).
/// @param obj SoundSource3D object.
/// @return Positive user multiplier, or `1` for invalid/unset state.
double rt_soundsource3d_get_pitch(void *obj) {
    rt_soundsource3d *source = sound3d_source_checked(obj);
    return source ? source->pitch : 1.0;
}

/// @brief Set the source's user playback-rate multiplier.
/// @details Composes multiplicatively with the Doppler factor; the mixer
///          clamps the combined rate to 0.25–4.0. Applies immediately to a
///          voice in flight.
/// @param obj SoundSource3D object.
/// @param pitch Positive user playback-rate multiplier; invalid input becomes `1`.
void rt_soundsource3d_set_pitch(void *obj, double pitch) {
    rt_soundsource3d *source = sound3d_source_checked(obj);
    if (!source)
        return;
    source->pitch = sound3d_pitch_or(pitch);
    sound3d_source_apply_spatial(source);
}

/// @brief Get the source's occlusion amount (0 open .. 1 fully occluded).
/// @param obj SoundSource3D object.
/// @return Stored occlusion fraction, or zero for an invalid source.
double rt_soundsource3d_get_occlusion(void *obj) {
    rt_soundsource3d *source = sound3d_source_checked(obj);
    return source ? source->occlusion : 0.0;
}

/// @brief Set the source's occlusion amount (0 open .. 1 fully occluded).
/// @details The game supplies the amount (typically from its own line-of-
///          sight raycasts); the mixer applies a smoothed perceptual lowpass
///          sweep plus up to -6 dB of attenuation.
/// @param obj SoundSource3D object.
/// @param amount Requested occlusion fraction, clamped to `[0, 1]`.
void rt_soundsource3d_set_occlusion(void *obj, double amount) {
    rt_soundsource3d *source = sound3d_source_checked(obj);
    if (!source)
        return;
    source->occlusion = sound3d_occlusion_or(amount);
    sound3d_source_apply_spatial(source);
}

/// @brief Returns 1 if the source plays in a loop (vs. fire-and-forget one-shot).
/// @param obj SoundSource3D object.
/// @return `1` when future playback loops, otherwise `0`.
int8_t rt_soundsource3d_get_looping(void *obj) {
    rt_soundsource3d *source = sound3d_source_checked(obj);
    return source && source->looping ? 1 : 0;
}

/// @brief Toggle looping mode. Takes effect on the next `_play` call (does not affect a voice
/// already in flight; stop and replay to apply mid-stream).
/// @param obj SoundSource3D object.
/// @param looping Non-zero to loop future playback.
void rt_soundsource3d_set_looping(void *obj, int8_t looping) {
    rt_soundsource3d *source = sound3d_source_checked(obj);
    if (!source)
        return;
    source->looping = looping ? 1 : 0;
}

/// @brief Returns 1 if the underlying voice is still active. Auto-reaps stale voice IDs whose
/// playback has finished (so subsequent calls return 0 cleanly).
/// @param obj SoundSource3D object.
/// @return `1` while the owned voice is playing, otherwise `0`.
int8_t rt_soundsource3d_get_is_playing(void *obj) {
    rt_soundsource3d *source = sound3d_source_checked(obj);
    return source ? sound3d_source_refresh_play_state(source) : 0;
}

/// @brief Return the underlying voice ID (for low-level voice control). Returns 0 if the
/// source isn't playing or the voice has finished. Always re-checks live state first.
/// @param obj SoundSource3D object.
/// @return Positive live voice identifier, or zero when inactive/invalid.
int64_t rt_soundsource3d_get_voice_id(void *obj) {
    rt_soundsource3d *source = sound3d_source_checked(obj);
    if (!source)
        return 0;
    sound3d_source_refresh_play_state(source);
    return source->voice_id;
}

/// @brief Start playback of the bound sound. Computes initial spatial volume/pan against the
/// active listener, then plays via `rt_sound_play_loop` (looping) or `rt_sound_play_ex` (one-
/// shot). Stops any prior voice this source owned.
/// @param obj SoundSource3D object.
/// @return Positive voice identifier, or `-1` on failure.
int64_t rt_soundsource3d_play(void *obj) {
    rt_soundsource3d *source = sound3d_source_checked(obj);
    rt_sound3d_listener_state listener;
    int64_t spatial_volume = 0;
    int64_t spatial_pan = 0;
    if (!source || !source->sound)
        return -1;

    sound3d_source_sync_binding(source, 0.0);
    sound3d_refresh_active_listener();
    rt_sound3d_get_effective_listener_state(&listener);
    rt_sound3d_compute_voice_params_ex(&listener,
                                       source->position,
                                       source->velocity,
                                       source->ref_distance,
                                       source->max_distance,
                                       sound3d_clamp_volume(source->volume),
                                       &spatial_volume,
                                       &spatial_pan,
                                       &source->doppler_factor);

    if (source->voice_id > 0)
        rt_voice_stop(source->voice_id);
    source->voice_id = source->looping
                           ? rt_sound_play_loop_in_group(
                                 source->sound, spatial_volume, spatial_pan, source->mix_group)
                           : rt_sound_play_ex_in_group(
                                 source->sound, spatial_volume, spatial_pan, source->mix_group);
    if (source->voice_id <= 0) {
        // VoiceId (object state) stays 0 for "no active voice", but the
        // play CALL reports failure with -1 like every Sound.Play* variant
        // (VDOC-120).
        source->voice_id = 0;
        return -1;
    }
    rt_sound3d_register_voice_ex(source->voice_id,
                                 source->ref_distance,
                                 source->max_distance,
                                 sound3d_clamp_volume(source->volume));
    rt_voice_set_pitch(source->voice_id,
                       sound3d_combined_pitch(source->pitch, source->doppler_factor));
    rt_voice_set_occlusion(source->voice_id, source->occlusion);
    return source->voice_id;
}

/// @brief Stop the active voice (if any) and clear the source's voice ID. No-op if not playing.
/// @param obj SoundSource3D object.
void rt_soundsource3d_stop(void *obj) {
    rt_soundsource3d *source = sound3d_source_checked(obj);
    if (!source)
        return;
    if (source->voice_id > 0)
        rt_voice_stop(source->voice_id);
    source->voice_id = 0;
}

/// @brief Bind the source to a SceneNode3D — the source position will track the node's world
/// transform every `_sync_bindings` tick. Replaces any prior binding and immediately syncs.
/// @param obj SoundSource3D object.
/// @param node SceneNode3D object to retain, or NULL to clear.
void rt_soundsource3d_bind_node(void *obj, void *node) {
    rt_soundsource3d *source = sound3d_source_checked(obj);
    if (!source)
        return;
    if (node && !rt_g3d_has_class(node, RT_G3D_SCENENODE3D_CLASS_ID))
        return;
    if (node)
        rt_obj_retain_maybe(node);
    sound3d_release_class_ref(&source->bound_node, RT_G3D_SCENENODE3D_CLASS_ID);
    source->bound_node = node;
    source->has_last_sync_position = 0;
    sound3d_source_sync_binding(source, 0.0);
}

/// @brief Detach the source from any bound node. Position freezes at last sync.
/// @param obj SoundSource3D object.
void rt_soundsource3d_clear_node_binding(void *obj) {
    rt_soundsource3d *source = sound3d_source_checked(obj);
    if (!source)
        return;
    sound3d_release_class_ref(&source->bound_node, RT_G3D_SCENENODE3D_CLASS_ID);
}

/// @brief Route the source's future playback voices to a mix group.
/// @details Applies from the next play; a live voice keeps its group. Invalid
///   group ids fall back to the SFX group.
/// @param obj SoundSource3D object.
/// @param group Numeric mix-group identifier.
void rt_soundsource3d_set_mix_group(void *obj, int64_t group) {
    rt_soundsource3d *source = sound3d_source_checked(obj);
    if (!source)
        return;
    source->mix_group = (group >= 0 && group < RT_MIXGROUP_MAX_GROUPS) ? group : RT_MIXGROUP_SFX;
}

/// @brief Mix group future playback voices route to.
/// @param obj SoundSource3D object.
/// @return Stored group, or the built-in SFX group for an invalid source.
int64_t rt_soundsource3d_get_mix_group(void *obj) {
    rt_soundsource3d *source = sound3d_source_checked(obj);
    return source ? source->mix_group : RT_MIXGROUP_SFX;
}

#else
typedef int rt_graphics_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
