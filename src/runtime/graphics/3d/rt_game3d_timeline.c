//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/rt_game3d_timeline.c
// Purpose: Zanna.Game3D.Timeline3D — the in-engine cutscene sequencer: camera
//   cuts and spline moves, FOV ramps, entity animation, audio, subtitles,
//   letterbox/fade overlays, and polled event markers, ticked by the world's
//   scaled time with skip/stop semantics and camera-controller suspension.
// Key invariants:
//   - Tracks are immutable after play(): sorted once, ticked allocation-free.
//   - Fire-once tracks fire exactly once per play regardless of step size;
//     skip() past-fires anims (final state), silences audio, fires markers.
//   - While any camera track exists, the installed camera controller is
//     suspended (not detached); the timeline writes the camera in the
//     late-update slot so look targets read post-physics poses.
// Ownership/Lifetime:
//   - GC-managed; finalizer releases retained track objects and the world ref.
//     The world retains the active timeline; stop()/replacement releases it.
// Links: rt_game3d_internal.h, rt_path3d.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements immutable-track Game3D cutscene timelines and world playback.
/// @details Timeline3D stores bounded snapshots and retained track objects,
/// advances fire-once and interval tracks on scaled world time, applies camera
/// work after scene synchronization, draws overlays, and defines deterministic
/// skip, stop, and replacement behavior.

#include "rt_animcontroller3d.h"
#include "rt_audio.h"
#include "rt_canvas3d.h"
#include "rt_game3d.h"
#include "rt_game3d_internal.h"
#include "rt_graphics3d_ids.h"
#include "rt_object.h"
#include "rt_path3d.h"
#include "rt_string.h"
#include "rt_string_internal.h"
#include "rt_trap.h"
#include "rt_vec3.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/// @brief Check whether a bounded track text buffer contains a terminator.
/// @param text Fixed-width timeline text buffer.
/// @return Nonzero when a NUL exists within the physical buffer.
static int game3d_timeline_text_valid(const char text[RT_GAME3D_TL_TEXT_MAX]) {
    return text && memchr(text, '\0', RT_GAME3D_TL_TEXT_MAX) != NULL;
}

/// @brief Check whether a stored coordinate vector is finite and within the runtime ceiling.
/// @param value Three-component vector embedded in a track.
/// @return Nonzero when every lane is safe for camera/audio math.
static int game3d_timeline_vec_valid(const double value[3]) {
    return value && isfinite(value[0]) && isfinite(value[1]) && isfinite(value[2]) &&
           fabs(value[0]) <= RT_GAME3D_COORD_ABS_MAX && fabs(value[1]) <= RT_GAME3D_COORD_ABS_MAX &&
           fabs(value[2]) <= RT_GAME3D_COORD_ABS_MAX;
}

/// @brief Validate one initialized timeline track before playback or destruction.
/// @param track Borrowed track record.
/// @return Nonzero only when the kind-specific numeric, text, and retained-reference invariants
/// hold.
static int game3d_timeline_track_valid(const rt_game3d_tl_track *track) {
    if (!track || track->type < RT_GAME3D_TL_CAMERA_CUT || track->type > RT_GAME3D_TL_MARKER ||
        !isfinite(track->t0) || !isfinite(track->t1) || track->t0 < 0.0 || track->t1 < track->t0 ||
        track->t1 > 86400.0 || (track->fired != 0 && track->fired != 1) || track->ease < 0 ||
        track->ease > 3 || (track->positional != 0 && track->positional != 1))
        return 0;
    switch (track->type) {
        case RT_GAME3D_TL_CAMERA_CUT:
            return game3d_timeline_vec_valid(track->vec_a) &&
                   game3d_timeline_vec_valid(track->vec_b) && isfinite(track->scalar_a) &&
                   track->scalar_a >= 1.0 && track->scalar_a <= 179.0 && !track->obj_a &&
                   !track->obj_b;
        case RT_GAME3D_TL_CAMERA_MOVE:
            return rt_g3d_has_class(track->obj_a, RT_G3D_PATH3D_CLASS_ID) &&
                   (!track->obj_b || rt_g3d_is_vec3(track->obj_b) ||
                    rt_obj_is_instance(
                        track->obj_b, RT_G3D_GAME3D_ENTITY_CLASS_ID, sizeof(rt_game3d_entity)) ||
                    rt_g3d_has_class(track->obj_b, RT_G3D_PATH3D_CLASS_ID));
        case RT_GAME3D_TL_FOV_RAMP:
            return isfinite(track->scalar_a) && isfinite(track->scalar_b) &&
                   track->scalar_a >= 1.0 && track->scalar_a <= 179.0 && track->scalar_b >= 1.0 &&
                   track->scalar_b <= 179.0 && !track->obj_a && !track->obj_b;
        case RT_GAME3D_TL_ANIM:
            return game3d_timeline_text_valid(track->text_a) &&
                   game3d_timeline_text_valid(track->text_b) && isfinite(track->scalar_a) &&
                   track->scalar_a >= 0.0 && track->scalar_a <= 60.0 && !track->obj_a &&
                   !track->obj_b;
        case RT_GAME3D_TL_AUDIO:
            return rt_sound_is_handle(track->obj_a) && !track->obj_b &&
                   (!track->positional || game3d_timeline_vec_valid(track->vec_a));
        case RT_GAME3D_TL_SUBTITLE:
            return game3d_timeline_text_valid(track->text_a) && !track->obj_a && !track->obj_b;
        case RT_GAME3D_TL_LETTERBOX:
            return isfinite(track->scalar_a) && track->scalar_a >= 0.0 && track->scalar_a <= 0.45 &&
                   !track->obj_a && !track->obj_b;
        case RT_GAME3D_TL_FADE:
            return isfinite(track->scalar_a) && isfinite(track->scalar_b) &&
                   track->scalar_a >= 0.0 && track->scalar_a <= 1.0 && track->scalar_b >= 0.0 &&
                   track->scalar_b <= 1.0 && !track->obj_a && !track->obj_b;
        case RT_GAME3D_TL_MARKER:
            return !track->obj_a && !track->obj_b;
        default:
            return 0;
    }
}

/// @brief Validate the authoritative Timeline3D allocation identity and initialized-slot range.
/// @param timeline Borrowed timeline payload.
/// @return Nonzero when the allocation may be traversed and freed safely.
static int game3d_timeline_allocation_valid(const rt_game3d_timeline *timeline) {
    if (!timeline)
        return 0;
    if (!timeline->owned_tracks && timeline->track_storage_count == 0 &&
        timeline->track_storage_capacity == 0 && timeline->track_storage_cookie == 0)
        return 1;
    if (timeline->track_storage_count < 0 || timeline->track_storage_capacity <= 0 ||
        timeline->track_storage_count > timeline->track_storage_capacity ||
        timeline->track_storage_capacity > RT_GAME3D_TL_MAX_TRACKS ||
        !game3d_world_storage_is_valid(timeline->owned_tracks,
                                       (size_t)timeline->track_storage_capacity,
                                       timeline->track_storage_cookie,
                                       RT_GAME3D_TIMELINE_TRACK_STORAGE_COOKIE))
        return 0;
    return 1;
}

/// @brief Validate authoritative Timeline3D allocation and every initialized track.
/// @param timeline Borrowed timeline payload.
/// @return Nonzero when storage and all kind-specific track invariants agree.
static int game3d_timeline_storage_valid(const rt_game3d_timeline *timeline) {
    if (!game3d_timeline_allocation_valid(timeline))
        return 0;
    for (int32_t i = 0; i < timeline->track_storage_count; ++i) {
        if (!game3d_timeline_track_valid(&timeline->owned_tracks[i]))
            return 0;
    }
    return 1;
}

/// @brief Restore mutable timeline mirrors and scalar state from validated track storage.
/// @param timeline Mutable timeline payload.
/// @return Nonzero when storage is valid and state was repaired, otherwise zero.
static int game3d_timeline_repair_state(rt_game3d_timeline *timeline) {
    double duration = 0.0;
    int has_camera_tracks = 0;
    if (!game3d_timeline_storage_valid(timeline))
        return 0;
    timeline->tracks = timeline->owned_tracks;
    timeline->track_count = timeline->track_storage_count;
    timeline->track_capacity = timeline->track_storage_capacity;
    for (int32_t i = 0; i < timeline->track_count; ++i) {
        if (timeline->tracks[i].t1 > duration)
            duration = timeline->tracks[i].t1;
        if (timeline->tracks[i].type == RT_GAME3D_TL_CAMERA_CUT ||
            timeline->tracks[i].type == RT_GAME3D_TL_CAMERA_MOVE ||
            timeline->tracks[i].type == RT_GAME3D_TL_FOV_RAMP)
            has_camera_tracks = 1;
        if (i > 0 && timeline->tracks[i - 1].t0 > timeline->tracks[i].t0)
            timeline->sorted = 0;
    }
    timeline->duration = duration;
    timeline->time =
        duration > 0.0 ? game3d_nonnegative_clamped_or(timeline->time, 0.0, duration) : 0.0;
    timeline->playing = timeline->playing ? 1 : 0;
    timeline->finished = timeline->finished ? 1 : 0;
    timeline->just_finished = timeline->just_finished ? 1 : 0;
    timeline->skippable = timeline->skippable ? 1 : 0;
    timeline->sorted = timeline->sorted ? 1 : 0;
    timeline->has_camera_tracks = has_camera_tracks ? 1 : 0;
    if (timeline->fired_marker_count < 0 ||
        timeline->fired_marker_count > RT_GAME3D_TL_MAX_MARKERS_PER_STEP)
        timeline->fired_marker_count = 0;
    if (!game3d_timeline_text_valid(timeline->active_subtitle))
        timeline->active_subtitle[RT_GAME3D_TL_TEXT_MAX - 1] = '\0';
    timeline->letterbox_amount =
        game3d_nonnegative_clamped_or(timeline->letterbox_amount, 0.0, 0.45);
    timeline->fade_alpha = game3d_nonnegative_clamped_or(timeline->fade_alpha, 0.0, 1.0);
    return 1;
}

/// @brief Release only kind-validated retained objects from one timeline track.
/// @param track Mutable initialized track; invalid references are cleared as unowned corruption.
static void game3d_timeline_release_track_refs(rt_game3d_tl_track *track) {
    if (!track)
        return;
    if (track->type == RT_GAME3D_TL_CAMERA_MOVE) {
        game3d_release_typed_ref(&track->obj_a, RT_G3D_PATH3D_CLASS_ID);
        if (track->obj_b &&
            (rt_g3d_is_vec3(track->obj_b) ||
             rt_obj_is_instance(
                 track->obj_b, RT_G3D_GAME3D_ENTITY_CLASS_ID, sizeof(rt_game3d_entity)) ||
             rt_g3d_has_class(track->obj_b, RT_G3D_PATH3D_CLASS_ID)))
            game3d_release_ref(&track->obj_b);
        else
            track->obj_b = NULL;
    } else if (track->type == RT_GAME3D_TL_AUDIO) {
        if (rt_sound_is_handle(track->obj_a))
            game3d_release_ref(&track->obj_a);
        else
            track->obj_a = NULL;
        track->obj_b = NULL;
    } else {
        track->obj_a = NULL;
        track->obj_b = NULL;
    }
}

//=========================================================================
// Lifecycle
//=========================================================================

/// @brief GC finalizer: release retained track objects, buffers, and world ref.
/// @param obj Finalized Timeline3D payload; `NULL` is ignored.
static void game3d_timeline_finalize(void *obj) {
    rt_game3d_timeline *timeline = (rt_game3d_timeline *)obj;
    if (!timeline)
        return;
    int allocation_valid = game3d_timeline_allocation_valid(timeline);
    int32_t track_count = allocation_valid ? timeline->track_storage_count : 0;
    for (int32_t i = 0; i < track_count; ++i)
        game3d_timeline_release_track_refs(&timeline->owned_tracks[i]);
    if (allocation_valid)
        free(timeline->owned_tracks);
    timeline->tracks = NULL;
    timeline->owned_tracks = NULL;
    timeline->track_count = 0;
    timeline->track_capacity = 0;
    timeline->track_storage_count = 0;
    timeline->track_storage_capacity = 0;
    timeline->track_storage_cookie = 0;
    game3d_release_typed_ref(&timeline->world, RT_G3D_GAME3D_WORLD_CLASS_ID);
}

/// @brief Create an empty timeline bound to @p world (installed via playTimeline).
/// @param world_obj Borrowed live World3D handle retained by the timeline.
/// @return New GC-managed Timeline3D handle, or `NULL` after validation or allocation failure.
void *rt_game3d_timeline_new(void *world_obj) {
    rt_game3d_world *world =
        game3d_world_checked(world_obj, "Game3D.Timeline3D.New: invalid world");
    if (!world)
        return NULL;
    rt_game3d_timeline *timeline = (rt_game3d_timeline *)rt_obj_new_i64(
        RT_G3D_GAME3D_TIMELINE_CLASS_ID, (int64_t)sizeof(*timeline));
    if (!timeline) {
        rt_trap("Game3D.Timeline3D.New: allocation failed");
        return NULL;
    }
    memset(timeline, 0, sizeof(*timeline));
    rt_obj_set_finalizer(timeline, game3d_timeline_finalize);
    game3d_assign_ref(&timeline->world, world);
    timeline->skippable = 1;
    return timeline;
}

/// @brief Append a zeroed track (grows the array); NULL on failure/while playing.
/// @param timeline Timeline payload whose track array may grow.
/// @param type Internal `RT_GAME3D_TL_*` track discriminator.
/// @param t0 Candidate non-negative start time in seconds.
/// @param t1 Candidate end time, clamped to be no earlier than @p t0.
/// @param api_name Diagnostic recorded when mutation is attempted during playback.
/// @return Borrowed new track slot, or `NULL` on immutable state or allocation failure.
static rt_game3d_tl_track *game3d_timeline_append(
    rt_game3d_timeline *timeline, int8_t type, double t0, double t1, const char *api_name) {
    if (!timeline)
        return NULL;
    if (timeline->playing) {
        rt_trap(api_name);
        return NULL;
    }
    if (!game3d_timeline_repair_state(timeline)) {
        rt_trap("Game3D.Timeline3D: corrupt track storage");
        return NULL;
    }
    if (timeline->track_count >= timeline->track_capacity) {
        if (timeline->track_count >= RT_GAME3D_TL_MAX_TRACKS) {
            rt_trap("Game3D.Timeline3D: track limit exceeded");
            return NULL;
        }
        int32_t new_cap = 0;
        if (!game3d_checked_capacity_i32(timeline->track_capacity,
                                         timeline->track_count + 1,
                                         8,
                                         sizeof(*timeline->tracks),
                                         &new_cap)) {
            rt_trap("Game3D.Timeline3D: track capacity overflow");
            return NULL;
        }
        if (new_cap > RT_GAME3D_TL_MAX_TRACKS)
            new_cap = RT_GAME3D_TL_MAX_TRACKS;
        int32_t old_cap = timeline->track_storage_capacity;
        rt_game3d_tl_track *grown =
            (rt_game3d_tl_track *)realloc(timeline->owned_tracks, (size_t)new_cap * sizeof(*grown));
        if (!grown)
            return NULL;
        memset(grown + old_cap, 0, (size_t)(new_cap - old_cap) * sizeof(*grown));
        timeline->tracks = grown;
        timeline->owned_tracks = grown;
        timeline->track_capacity = new_cap;
        timeline->track_storage_capacity = new_cap;
        timeline->track_storage_cookie = game3d_world_storage_cookie_value(
            grown, (size_t)new_cap, RT_GAME3D_TIMELINE_TRACK_STORAGE_COOKIE);
    }
    rt_game3d_tl_track *track = &timeline->tracks[timeline->track_count++];
    memset(track, 0, sizeof(*track));
    timeline->track_storage_count = timeline->track_count;
    track->type = type;
    track->insertion_order = (uint64_t)(timeline->track_count - 1);
    t0 = game3d_nonnegative_clamped_or(t0, 0.0, 86400.0);
    t1 = game3d_nonnegative_clamped_or(t1, t0, 86400.0);
    if (t1 < t0)
        t1 = t0;
    track->t0 = t0;
    track->t1 = t1;
    if (t1 > timeline->duration)
        timeline->duration = t1;
    timeline->sorted = 0;
    return track;
}

/// @brief Copy a validated runtime string into a bounded track field on a UTF-8 boundary.
/// @param[out] dst Fixed `RT_GAME3D_TL_TEXT_MAX`-byte destination.
/// @param text Borrowed runtime string, or `NULL` for empty text.
/// @return Nonzero when the complete input was a valid NUL-free UTF-8 string.
static int game3d_timeline_copy_text(char *dst, rt_string text) {
    const char *src;
    size_t length;
    size_t copied;
    if (!dst)
        return 0;
    memset(dst, 0, RT_GAME3D_TL_TEXT_MAX);
    if (!text)
        return 1;
    if (!rt_string_is_handle(text))
        return 0;
    src = rt_string_cstr(text);
    length = rt_string_len_bytes(text);
    if (!src || memchr(src, '\0', length) != NULL || !rt_utf8_span_valid(src, length))
        return 0;
    copied = length < RT_GAME3D_TL_TEXT_MAX ? length : RT_GAME3D_TL_TEXT_MAX - 1u;
    while (copied > 0 && !rt_utf8_span_valid(src, copied))
        copied--;
    if (copied > 0)
        memcpy(dst, src, copied);
    dst[copied] = '\0';
    return 1;
}

//=========================================================================
// Track add-API (fluent)
//=========================================================================

/// @brief Camera cut: pose applied at t, held until the next camera key.
/// @param obj Borrowed Timeline3D handle.
/// @param t Non-negative cut time in seconds.
/// @param pos Borrowed Vec3 camera position.
/// @param look Borrowed Vec3 look-at point.
/// @param fov Vertical field of view clamped to `[1, 179]`.
/// @return Original borrowed timeline handle for fluent chaining.
void *rt_game3d_timeline_add_camera_cut(void *obj, double t, void *pos, void *look, double fov) {
    rt_game3d_timeline *timeline =
        game3d_timeline_checked(obj, "Game3D.Timeline3D.addCameraCut: invalid timeline");
    if (!rt_g3d_is_vec3(pos) || !rt_g3d_is_vec3(look)) {
        rt_trap("Game3D.Timeline3D.addCameraCut: pos and lookAt must be Vec3");
        return obj;
    }
    rt_game3d_tl_track *track = game3d_timeline_append(
        timeline,
        RT_GAME3D_TL_CAMERA_CUT,
        t,
        t,
        "Game3D.Timeline3D.addCameraCut: tracks are immutable while playing");
    if (track) {
        track->vec_a[0] = game3d_clamp_coord_or(rt_vec3_x(pos), 0.0);
        track->vec_a[1] = game3d_clamp_coord_or(rt_vec3_y(pos), 0.0);
        track->vec_a[2] = game3d_clamp_coord_or(rt_vec3_z(pos), 0.0);
        track->vec_b[0] = game3d_clamp_coord_or(rt_vec3_x(look), 0.0);
        track->vec_b[1] = game3d_clamp_coord_or(rt_vec3_y(look), 0.0);
        track->vec_b[2] = game3d_clamp_coord_or(rt_vec3_z(look), 0.0);
        track->scalar_a = game3d_clamp(game3d_finite_or(fov, 60.0), 1.0, 179.0);
        timeline->has_camera_tracks = 1;
    }
    return obj;
}

/// @brief Camera spline move over [t0,t1]; look = Vec3 | Entity3D | Path3D | NULL.
/// @param obj Borrowed Timeline3D handle.
/// @param t0 Non-negative move start time in seconds.
/// @param t1 Move end time, clamped not earlier than @p t0.
/// @param path Borrowed Path3D retained as the camera trajectory.
/// @param look_target Borrowed Vec3, Entity3D, or Path3D retained as the target, or `NULL`.
/// @param ease `RT_GAME3D_EASE_*` selector clamped to the supported range.
/// @return Original borrowed timeline handle for fluent chaining.
void *rt_game3d_timeline_add_camera_move(
    void *obj, double t0, double t1, void *path, void *look_target, int64_t ease) {
    rt_game3d_timeline *timeline =
        game3d_timeline_checked(obj, "Game3D.Timeline3D.addCameraMove: invalid timeline");
    int look_is_entity =
        rt_obj_is_instance(look_target, RT_G3D_GAME3D_ENTITY_CLASS_ID, sizeof(rt_game3d_entity));
    if (!rt_g3d_has_class(path, RT_G3D_PATH3D_CLASS_ID)) {
        rt_trap("Game3D.Timeline3D.addCameraMove: path must be Path3D");
        return obj;
    }
    if (look_target && !rt_g3d_is_vec3(look_target) && !look_is_entity &&
        !rt_g3d_has_class(look_target, RT_G3D_PATH3D_CLASS_ID)) {
        rt_trap("Game3D.Timeline3D.addCameraMove: look target must be Vec3, Entity3D, or Path3D");
        return obj;
    }
    if (look_is_entity) {
        rt_game3d_entity *look_entity = (rt_game3d_entity *)look_target;
        if (!look_entity->alive || look_entity->destroyed) {
            rt_trap("Game3D.Timeline3D.addCameraMove: look entity is destroyed");
            return obj;
        }
    }
    rt_game3d_tl_track *track = game3d_timeline_append(
        timeline,
        RT_GAME3D_TL_CAMERA_MOVE,
        t0,
        t1,
        "Game3D.Timeline3D.addCameraMove: tracks are immutable while playing");
    if (track) {
        game3d_assign_ref(&track->obj_a, path);
        game3d_assign_ref(&track->obj_b, look_target);
        track->ease = (int8_t)game3d_clamp((double)ease, 0.0, 3.0);
        timeline->has_camera_tracks = 1;
    }
    return obj;
}

/// @brief FOV ramp lerped over [t0,t1].
/// @param obj Borrowed Timeline3D handle.
/// @param t0 Non-negative ramp start time.
/// @param t1 Ramp end time, clamped not earlier than @p t0.
/// @param fov0 Initial vertical FOV in degrees.
/// @param fov1 Final vertical FOV in degrees.
/// @param ease `RT_GAME3D_EASE_*` selector.
/// @return Original borrowed timeline handle for fluent chaining.
void *rt_game3d_timeline_add_fov_ramp(
    void *obj, double t0, double t1, double fov0, double fov1, int64_t ease) {
    rt_game3d_timeline *timeline =
        game3d_timeline_checked(obj, "Game3D.Timeline3D.addFovRamp: invalid timeline");
    rt_game3d_tl_track *track =
        game3d_timeline_append(timeline,
                               RT_GAME3D_TL_FOV_RAMP,
                               t0,
                               t1,
                               "Game3D.Timeline3D.addFovRamp: tracks are immutable while playing");
    if (track) {
        track->scalar_a = game3d_clamp(game3d_finite_or(fov0, 60.0), 1.0, 179.0);
        track->scalar_b = game3d_clamp(game3d_finite_or(fov1, 60.0), 1.0, 179.0);
        track->ease = (int8_t)game3d_clamp((double)ease, 0.0, 3.0);
        timeline->has_camera_tracks = 1;
    }
    return obj;
}

/// @brief Fire Animator3D.crossfade on a named entity at t.
/// @param obj Borrowed Timeline3D handle.
/// @param t Non-negative fire time in seconds.
/// @param entity_name Borrowed entity name snapshotted into the track.
/// @param state_name Borrowed animation state name snapshotted into the track.
/// @param crossfade_seconds Non-negative blend duration, capped at 60 seconds.
/// @return Original borrowed timeline handle for fluent chaining.
void *rt_game3d_timeline_add_anim(
    void *obj, double t, rt_string entity_name, rt_string state_name, double crossfade_seconds) {
    rt_game3d_timeline *timeline =
        game3d_timeline_checked(obj, "Game3D.Timeline3D.addAnim: invalid timeline");
    char entity_text[RT_GAME3D_TL_TEXT_MAX];
    char state_text[RT_GAME3D_TL_TEXT_MAX];
    if (!game3d_timeline_copy_text(entity_text, entity_name) ||
        !game3d_timeline_copy_text(state_text, state_name)) {
        rt_trap("Game3D.Timeline3D.addAnim: names must be valid UTF-8 without embedded NUL");
        return obj;
    }
    rt_game3d_tl_track *track =
        game3d_timeline_append(timeline,
                               RT_GAME3D_TL_ANIM,
                               t,
                               t,
                               "Game3D.Timeline3D.addAnim: tracks are immutable while playing");
    if (track) {
        memcpy(track->text_a, entity_text, sizeof(track->text_a));
        memcpy(track->text_b, state_text, sizeof(track->text_b));
        track->scalar_a = game3d_nonnegative_clamped_or(crossfade_seconds, 0.0, 60.0);
    }
    return obj;
}

/// @brief Fire an audio clip at t (2D, or positional at @p position).
/// @param obj Borrowed Timeline3D handle.
/// @param t Non-negative fire time in seconds.
/// @param clip Borrowed non-null audio clip retained by the track.
/// @param positional Nonzero to play the clip at a world position.
/// @param position Borrowed Vec3 required for positional playback.
/// @return Original borrowed timeline handle for fluent chaining.
void *rt_game3d_timeline_add_audio(
    void *obj, double t, void *clip, int8_t positional, void *position) {
    rt_game3d_timeline *timeline =
        game3d_timeline_checked(obj, "Game3D.Timeline3D.addAudio: invalid timeline");
    if (!clip || !rt_sound_is_handle(clip)) {
        rt_trap("Game3D.Timeline3D.addAudio: clip must be Sound");
        return obj;
    }
    if (positional && !rt_g3d_is_vec3(position)) {
        rt_trap("Game3D.Timeline3D.addAudio: positional audio needs a Vec3 position");
        return obj;
    }
    rt_game3d_tl_track *track =
        game3d_timeline_append(timeline,
                               RT_GAME3D_TL_AUDIO,
                               t,
                               t,
                               "Game3D.Timeline3D.addAudio: tracks are immutable while playing");
    if (track) {
        game3d_assign_ref(&track->obj_a, clip);
        track->positional = positional ? 1 : 0;
        if (positional) {
            track->vec_a[0] = game3d_clamp_coord_or(rt_vec3_x(position), 0.0);
            track->vec_a[1] = game3d_clamp_coord_or(rt_vec3_y(position), 0.0);
            track->vec_a[2] = game3d_clamp_coord_or(rt_vec3_z(position), 0.0);
        }
    }
    return obj;
}

/// @brief Subtitle text shown over [t0,t1].
/// @param obj Borrowed Timeline3D handle.
/// @param t0 Non-negative display start time.
/// @param t1 Display end time, clamped not earlier than @p t0.
/// @param text Borrowed subtitle text snapshotted into the track.
/// @return Original borrowed timeline handle for fluent chaining.
void *rt_game3d_timeline_add_subtitle(void *obj, double t0, double t1, rt_string text) {
    rt_game3d_timeline *timeline =
        game3d_timeline_checked(obj, "Game3D.Timeline3D.addSubtitle: invalid timeline");
    char snapshot[RT_GAME3D_TL_TEXT_MAX];
    if (!game3d_timeline_copy_text(snapshot, text)) {
        rt_trap("Game3D.Timeline3D.addSubtitle: text must be valid UTF-8 without embedded NUL");
        return obj;
    }
    rt_game3d_tl_track *track =
        game3d_timeline_append(timeline,
                               RT_GAME3D_TL_SUBTITLE,
                               t0,
                               t1,
                               "Game3D.Timeline3D.addSubtitle: tracks are immutable while playing");
    if (track)
        memcpy(track->text_a, snapshot, sizeof(track->text_a));
    return obj;
}

/// @brief Letterbox bars covering @p amount of the height over [t0,t1].
/// @param obj Borrowed Timeline3D handle.
/// @param t0 Non-negative display start time.
/// @param t1 Display end time, clamped not earlier than @p t0.
/// @param amount Per-edge screen-height fraction clamped to `[0, 0.45]`.
/// @return Original borrowed timeline handle for fluent chaining.
void *rt_game3d_timeline_add_letterbox(void *obj, double t0, double t1, double amount) {
    rt_game3d_timeline *timeline =
        game3d_timeline_checked(obj, "Game3D.Timeline3D.addLetterbox: invalid timeline");
    rt_game3d_tl_track *track = game3d_timeline_append(
        timeline,
        RT_GAME3D_TL_LETTERBOX,
        t0,
        t1,
        "Game3D.Timeline3D.addLetterbox: tracks are immutable while playing");
    if (track)
        track->scalar_a = game3d_clamp(game3d_finite_or(amount, 0.1), 0.0, 0.45);
    return obj;
}

/// @brief Full-screen fade from alpha a0 to a1 over [t0,t1].
/// @param obj Borrowed Timeline3D handle.
/// @param t0 Non-negative fade start time.
/// @param t1 Fade end time, clamped not earlier than @p t0.
/// @param a0 Initial alpha clamped to `[0, 1]`.
/// @param a1 Final alpha clamped to `[0, 1]`.
/// @return Original borrowed timeline handle for fluent chaining.
void *rt_game3d_timeline_add_fade(void *obj, double t0, double t1, double a0, double a1) {
    rt_game3d_timeline *timeline =
        game3d_timeline_checked(obj, "Game3D.Timeline3D.addFade: invalid timeline");
    rt_game3d_tl_track *track =
        game3d_timeline_append(timeline,
                               RT_GAME3D_TL_FADE,
                               t0,
                               t1,
                               "Game3D.Timeline3D.addFade: tracks are immutable while playing");
    if (track) {
        track->scalar_a = game3d_clamp(game3d_finite_or(a0, 0.0), 0.0, 1.0);
        track->scalar_b = game3d_clamp(game3d_finite_or(a1, 0.0), 0.0, 1.0);
    }
    return obj;
}

/// @brief Polled event marker fired when the playhead crosses t.
/// @param obj Borrowed Timeline3D handle.
/// @param t Non-negative marker time in seconds.
/// @param id Caller-defined marker identifier.
/// @return Original borrowed timeline handle for fluent chaining.
void *rt_game3d_timeline_add_marker(void *obj, double t, int64_t id) {
    rt_game3d_timeline *timeline =
        game3d_timeline_checked(obj, "Game3D.Timeline3D.addMarker: invalid timeline");
    rt_game3d_tl_track *track =
        game3d_timeline_append(timeline,
                               RT_GAME3D_TL_MARKER,
                               t,
                               t,
                               "Game3D.Timeline3D.addMarker: tracks are immutable while playing");
    if (track)
        track->marker_id = id;
    return obj;
}

//=========================================================================
// Properties and polling
//=========================================================================

/// @brief Get the latest track end time.
/// @param obj Borrowed Timeline3D handle.
/// @return Timeline duration in seconds, or zero when invalid.
double rt_game3d_timeline_get_duration(void *obj) {
    rt_game3d_timeline *timeline =
        game3d_timeline_checked(obj, "Game3D.Timeline3D.get_duration: invalid timeline");
    return timeline && game3d_timeline_repair_state(timeline) ? timeline->duration : 0.0;
}

/// @brief Get the current playhead time.
/// @param obj Borrowed Timeline3D handle.
/// @return Current playhead time in seconds, or zero when invalid.
double rt_game3d_timeline_get_time(void *obj) {
    rt_game3d_timeline *timeline =
        game3d_timeline_checked(obj, "Game3D.Timeline3D.get_time: invalid timeline");
    return timeline && game3d_timeline_repair_state(timeline) ? timeline->time : 0.0;
}

/// @brief Get whether the playhead is advancing.
/// @param obj Borrowed Timeline3D handle.
/// @return Nonzero while playing; otherwise zero.
int8_t rt_game3d_timeline_get_playing(void *obj) {
    rt_game3d_timeline *timeline =
        game3d_timeline_checked(obj, "Game3D.Timeline3D.get_playing: invalid timeline");
    return timeline && game3d_timeline_repair_state(timeline) ? timeline->playing : 0;
}

/// @brief Get whether playback has reached or skipped to the end.
/// @param obj Borrowed Timeline3D handle.
/// @return Nonzero after completion; otherwise zero.
int8_t rt_game3d_timeline_get_finished(void *obj) {
    rt_game3d_timeline *timeline =
        game3d_timeline_checked(obj, "Game3D.Timeline3D.get_finished: invalid timeline");
    return timeline && game3d_timeline_repair_state(timeline) ? timeline->finished : 0;
}

/// @brief Get whether explicit skip requests are accepted.
/// @param obj Borrowed Timeline3D handle.
/// @return Nonzero when skippable; otherwise zero.
int8_t rt_game3d_timeline_get_skippable(void *obj) {
    rt_game3d_timeline *timeline =
        game3d_timeline_checked(obj, "Game3D.Timeline3D.get_skippable: invalid timeline");
    return timeline && game3d_timeline_repair_state(timeline) ? timeline->skippable : 0;
}

/// @brief Enable or disable explicit timeline skipping.
/// @param obj Borrowed Timeline3D handle.
/// @param skippable Nonzero to accept skip requests.
void rt_game3d_timeline_set_skippable(void *obj, int8_t skippable) {
    rt_game3d_timeline *timeline =
        game3d_timeline_checked(obj, "Game3D.Timeline3D.set_skippable: invalid timeline");
    if (timeline)
        timeline->skippable = skippable ? 1 : 0;
}

/// @brief One-shot: true for the step after the timeline reached its end.
/// @param obj Borrowed Timeline3D handle.
/// @return Current completion-transition flag, or zero when invalid.
int8_t rt_game3d_timeline_just_finished(void *obj) {
    rt_game3d_timeline *timeline =
        game3d_timeline_checked(obj, "Game3D.Timeline3D.justFinished: invalid timeline");
    return timeline && game3d_timeline_repair_state(timeline) ? timeline->just_finished : 0;
}

/// @brief Get the number of marker events fired during the latest step.
/// @param obj Borrowed Timeline3D handle.
/// @return Buffered marker count, or zero when invalid.
int64_t rt_game3d_timeline_events_fired_count(void *obj) {
    rt_game3d_timeline *timeline =
        game3d_timeline_checked(obj, "Game3D.Timeline3D.eventsFiredCount: invalid timeline");
    return timeline && game3d_timeline_repair_state(timeline) ? timeline->fired_marker_count : 0;
}

/// @brief Read a marker identifier fired during the latest step.
/// @param obj Borrowed Timeline3D handle.
/// @param index Zero-based buffered marker index.
/// @return Marker identifier, or zero when out of range.
int64_t rt_game3d_timeline_event_fired_id(void *obj, int64_t index) {
    rt_game3d_timeline *timeline =
        game3d_timeline_checked(obj, "Game3D.Timeline3D.eventFiredId: invalid timeline");
    if (!timeline || !game3d_timeline_repair_state(timeline) || index < 0 ||
        index >= timeline->fired_marker_count)
        return 0;
    return timeline->fired_markers[index];
}

/// @brief Currently displayed subtitle ("" when none) — plan 25's hook point.
/// @param obj Borrowed Timeline3D handle.
/// @return New runtime string containing the active subtitle or empty text.
rt_string rt_game3d_timeline_active_subtitle(void *obj) {
    rt_game3d_timeline *timeline =
        game3d_timeline_checked(obj, "Game3D.Timeline3D.activeSubtitle: invalid timeline");
    return rt_const_cstr(
        timeline && game3d_timeline_repair_state(timeline) ? timeline->active_subtitle : "");
}

//=========================================================================
// Playback engine
//=========================================================================

/// @brief qsort comparator: by start time and then by immutable append order.
/// @param a Borrowed pointer to the first track.
/// @param b Borrowed pointer to the second track.
/// @return Negative, zero, or positive according to playback ordering.
static int game3d_timeline_track_cmp(const void *a, const void *b) {
    const rt_game3d_tl_track *ta = (const rt_game3d_tl_track *)a;
    const rt_game3d_tl_track *tb = (const rt_game3d_tl_track *)b;
    if (ta->t0 < tb->t0)
        return -1;
    if (ta->t0 > tb->t0)
        return 1;
    if (ta->insertion_order != tb->insertion_order)
        return ta->insertion_order < tb->insertion_order ? -1 : 1;
    return 0;
}

/// @brief Reset the playhead and fire-once latches; sort tracks once.
/// @param timeline Timeline payload to prepare for playback.
static void game3d_timeline_reset(rt_game3d_timeline *timeline) {
    if (!game3d_timeline_repair_state(timeline)) {
        rt_trap("Game3D.Timeline3D: corrupt track storage");
        return;
    }
    if (!timeline->sorted && timeline->track_count > 1) {
        qsort(timeline->tracks,
              (size_t)timeline->track_count,
              sizeof(rt_game3d_tl_track),
              game3d_timeline_track_cmp);
    }
    timeline->sorted = 1;
    for (int32_t i = 0; i < timeline->track_count; ++i)
        timeline->tracks[i].fired = 0;
    timeline->time = 0.0;
    timeline->playing = 1;
    timeline->finished = 0;
    timeline->just_finished = 0;
    timeline->fired_marker_count = 0;
    timeline->active_subtitle[0] = '\0';
    timeline->letterbox_amount = 0.0;
    timeline->fade_alpha = 0.0;
}

/// @brief Apply an ease curve to a normalized fraction.
/// @param frac Candidate interpolation fraction, clamped to `[0, 1]`.
/// @param ease Linear, smoothstep, ease-in, or ease-out selector.
/// @return Eased interpolation fraction in `[0, 1]`.
static double game3d_timeline_ease(double frac, int8_t ease) {
    frac = game3d_clamp(frac, 0.0, 1.0);
    switch (ease) {
        case 1:
            return frac * frac * (3.0 - 2.0 * frac);
        case 2:
            return frac * frac;
        case 3:
            return 1.0 - (1.0 - frac) * (1.0 - frac);
        default:
            return frac;
    }
}

/// @brief Fire one point track (anim / audio / marker). @p silent skips audio.
/// @param world Borrowed world providing entity and audio services.
/// @param timeline Timeline payload receiving marker events.
/// @param track Point track to latch and execute.
/// @param silent Nonzero to suppress audio and force animation crossfades to zero.
static void game3d_timeline_fire(rt_game3d_world *world,
                                 rt_game3d_timeline *timeline,
                                 rt_game3d_tl_track *track,
                                 int silent) {
    track->fired = 1;
    switch (track->type) {
        case RT_GAME3D_TL_ANIM: {
            rt_string entity_name = rt_const_cstr(track->text_a);
            void *entity = rt_game3d_world_find_entity(world, entity_name);
            rt_string_unref(entity_name);
            void *animator = entity ? game3d_entity_anim_ref((rt_game3d_entity *)entity) : NULL;
            void *controller = animator ? rt_game3d_animator_get_controller(animator) : NULL;
            if (controller) {
                rt_string state_name = rt_const_cstr(track->text_b);
                (void)rt_anim_controller3d_crossfade(
                    controller, state_name, silent ? 0.0 : track->scalar_a);
                rt_string_unref(state_name);
            }
            break;
        }
        case RT_GAME3D_TL_AUDIO: {
            if (silent || !world->audio || !track->obj_a)
                break;
            if (track->positional) {
                void *position = rt_vec3_new(track->vec_a[0], track->vec_a[1], track->vec_a[2]);
                void *voice = rt_game3d_audio_play_at(world->audio, track->obj_a, position);
                game3d_release_ref(&voice);
                game3d_release_ref(&position);
            } else {
                (void)rt_game3d_audio_play2d(world->audio, track->obj_a);
            }
            break;
        }
        case RT_GAME3D_TL_MARKER: {
            if (timeline->fired_marker_count < RT_GAME3D_TL_MAX_MARKERS_PER_STEP)
                timeline->fired_markers[timeline->fired_marker_count++] = track->marker_id;
            break;
        }
        default:
            break;
    }
}

/// @brief Pre-physics tick. See internal header.
/// @param world Borrowed world containing the active timeline.
/// @param dt Candidate scaled simulation delta, sanitized before advancing.
/// @return Nonzero while camera tracks should suspend the normal camera controller.
int game3d_world_timeline_pre(rt_game3d_world *world, double dt) {
    if (!world)
        return 0;
    rt_game3d_timeline *timeline = (rt_game3d_timeline *)rt_g3d_checked_or_null(
        world->active_timeline, RT_G3D_GAME3D_TIMELINE_CLASS_ID);
    if (!timeline)
        return 0;
    if (!game3d_timeline_repair_state(timeline)) {
        timeline->playing = 0;
        rt_trap("Game3D.Timeline3D: corrupt track storage");
        return 0;
    }
    timeline->fired_marker_count = 0;
    timeline->just_finished = 0;
    if (!timeline->playing)
        return 0;

    double prev = timeline->time;
    timeline->time += game3d_clamp_dt(dt);
    if (timeline->time >= timeline->duration) {
        timeline->time = timeline->duration;
    }

    /* Fire point tracks the playhead crossed this step: prev < t0 <= time,
     * including t0 == 0 on the first step. */
    for (int32_t i = 0; i < timeline->track_count; ++i) {
        rt_game3d_tl_track *track = &timeline->tracks[i];
        if (track->fired)
            continue;
        if (track->type != RT_GAME3D_TL_ANIM && track->type != RT_GAME3D_TL_AUDIO &&
            track->type != RT_GAME3D_TL_MARKER)
            continue;
        int crossed = (track->t0 <= timeline->time) &&
                      (track->t0 > prev || (prev == 0.0 && track->t0 == 0.0));
        if (crossed)
            game3d_timeline_fire(world, timeline, track, 0);
    }

    /* Overlay state (letterbox / fade / subtitle) for the render pass. */
    timeline->letterbox_amount = 0.0;
    timeline->fade_alpha = 0.0;
    timeline->active_subtitle[0] = '\0';
    for (int32_t i = 0; i < timeline->track_count; ++i) {
        rt_game3d_tl_track *track = &timeline->tracks[i];
        if (timeline->time < track->t0 || timeline->time > track->t1)
            continue;
        double span = track->t1 - track->t0;
        double frac = span > 1e-12 ? (timeline->time - track->t0) / span : 1.0;
        if (track->type == RT_GAME3D_TL_LETTERBOX) {
            if (track->scalar_a > timeline->letterbox_amount)
                timeline->letterbox_amount = track->scalar_a;
        } else if (track->type == RT_GAME3D_TL_FADE) {
            double alpha = track->scalar_a + (track->scalar_b - track->scalar_a) * frac;
            if (alpha > timeline->fade_alpha)
                timeline->fade_alpha = alpha;
        } else if (track->type == RT_GAME3D_TL_SUBTITLE) {
            (void)game3d_utf8_copy_bounded(
                timeline->active_subtitle, RT_GAME3D_TL_TEXT_MAX, track->text_a);
        }
    }

    if (timeline->time >= timeline->duration && timeline->playing) {
        timeline->playing = 0;
        timeline->finished = 1;
        timeline->just_finished = 1;
    }
    return timeline->has_camera_tracks && (timeline->playing || timeline->finished);
}

/// @brief Choose a look-at up vector, falling back to +X when the view is near-vertical.
/// @details A cutscene camera looking straight up/down makes (0,1,0) parallel to the
///          view direction, so the look-at cross product degenerates to zero and the
///          camera basis becomes NaN. Mirrors the rail camera's near-vertical guard.
/// @param eye Three-component camera position.
/// @param look Three-component look-at point.
/// @param[out] up Required three-component safe up-vector destination.
static void game3d_timeline_safe_up(const double eye[3], const double look[3], double up[3]) {
    double view[3] = {look[0] - eye[0], look[1] - eye[1], look[2] - eye[2]};
    double len = sqrt(view[0] * view[0] + view[1] * view[1] + view[2] * view[2]);
    up[0] = 0.0;
    up[1] = 1.0;
    up[2] = 0.0;
    if (isfinite(len) && len > 1e-9 && fabs(view[1] / len) > 0.99) {
        up[0] = 1.0;
        up[1] = 0.0;
        up[2] = 0.0;
    }
}

/// @brief Camera application in the late-update slot. See internal header.
/// @param world Borrowed world whose camera receives the active cut, move, and FOV ramp.
void game3d_world_timeline_camera(rt_game3d_world *world) {
    if (!world || !world->camera)
        return;
    rt_game3d_timeline *timeline = (rt_game3d_timeline *)rt_g3d_checked_or_null(
        world->active_timeline, RT_G3D_GAME3D_TIMELINE_CLASS_ID);
    if (!timeline || !timeline->has_camera_tracks)
        return;
    if (!game3d_timeline_repair_state(timeline)) {
        timeline->playing = 0;
        rt_trap("Game3D.Timeline3D: corrupt track storage");
        return;
    }
    double now = timeline->time;

    /* Latest camera key at or before the playhead wins; an active move
     * overrides an earlier cut. */
    rt_game3d_tl_track *cut = NULL;
    rt_game3d_tl_track *move = NULL;
    for (int32_t i = 0; i < timeline->track_count; ++i) {
        rt_game3d_tl_track *track = &timeline->tracks[i];
        if (track->t0 > now)
            break; /* sorted by t0 */
        if (track->type == RT_GAME3D_TL_CAMERA_CUT) {
            if (!move || track->t0 >= move->t1)
                cut = track;
        } else if (track->type == RT_GAME3D_TL_CAMERA_MOVE) {
            move = track;
            cut = NULL;
        }
    }
    if (move && (now <= move->t1 || !cut)) {
        void *path = rt_g3d_checked_or_null(move->obj_a, RT_G3D_PATH3D_CLASS_ID);
        if (path) {
            double span = move->t1 - move->t0;
            double frac = span > 1e-12 ? (now - move->t0) / span : 1.0;
            frac = game3d_timeline_ease(frac, move->ease);
            double eye[3];
            double tangent[3];
            rt_path3d_eval_spline_raw(path, frac, eye, tangent);
            double look[3] = {eye[0] + tangent[0], eye[1] + tangent[1], eye[2] + tangent[2]};
            if (move->obj_b) {
                if (rt_g3d_is_vec3(move->obj_b)) {
                    look[0] = rt_vec3_x(move->obj_b);
                    look[1] = rt_vec3_y(move->obj_b);
                    look[2] = rt_vec3_z(move->obj_b);
                } else if (rt_g3d_has_class(move->obj_b, RT_G3D_GAME3D_ENTITY_CLASS_ID)) {
                    rt_game3d_entity *entity = (rt_game3d_entity *)move->obj_b;
                    double pos[3];
                    if (game3d_entity_alive_or_record(entity) &&
                        game3d_entity_world_position_components(entity, pos)) {
                        look[0] = pos[0];
                        look[1] = pos[1];
                        look[2] = pos[2];
                    }
                } else if (rt_g3d_has_class(move->obj_b, RT_G3D_PATH3D_CLASS_ID)) {
                    double lp[3];
                    rt_path3d_eval_spline_raw(move->obj_b, frac, lp, NULL);
                    look[0] = lp[0];
                    look[1] = lp[1];
                    look[2] = lp[2];
                }
            }
            double up[3];
            game3d_timeline_safe_up(eye, look, up);
            rt_camera3d_look_at_components(world->camera,
                                           game3d_clamp_coord_or(eye[0], 0.0),
                                           game3d_clamp_coord_or(eye[1], 0.0),
                                           game3d_clamp_coord_or(eye[2], 0.0),
                                           game3d_clamp_coord_or(look[0], 0.0),
                                           game3d_clamp_coord_or(look[1], 0.0),
                                           game3d_clamp_coord_or(look[2], 0.0),
                                           up[0],
                                           up[1],
                                           up[2]);
        }
    } else if (cut) {
        double up[3];
        game3d_timeline_safe_up(cut->vec_a, cut->vec_b, up);
        rt_camera3d_look_at_components(world->camera,
                                       cut->vec_a[0],
                                       cut->vec_a[1],
                                       cut->vec_a[2],
                                       cut->vec_b[0],
                                       cut->vec_b[1],
                                       cut->vec_b[2],
                                       up[0],
                                       up[1],
                                       up[2]);
        rt_camera3d_set_fov(world->camera, cut->scalar_a);
    }

    /* FOV ramps override cut FOV while active. */
    for (int32_t i = 0; i < timeline->track_count; ++i) {
        rt_game3d_tl_track *track = &timeline->tracks[i];
        if (track->type != RT_GAME3D_TL_FOV_RAMP)
            continue;
        if (now < track->t0 || now > track->t1)
            continue;
        double span = track->t1 - track->t0;
        double frac = span > 1e-12 ? (now - track->t0) / span : 1.0;
        frac = game3d_timeline_ease(frac, track->ease);
        rt_camera3d_set_fov(world->camera,
                            track->scalar_a + (track->scalar_b - track->scalar_a) * frac);
    }
}

/// @brief Overlay pass: letterbox bars, fade quad, subtitle. See internal header.
/// @param world Borrowed world whose Canvas3D receives the active overlays.
void game3d_world_timeline_overlay(rt_game3d_world *world) {
    if (!world || !world->canvas)
        return;
    rt_game3d_timeline *timeline = (rt_game3d_timeline *)rt_g3d_checked_or_null(
        world->active_timeline, RT_G3D_GAME3D_TIMELINE_CLASS_ID);
    if (!timeline)
        return;
    int64_t width = world->width > 0 ? world->width : 0;
    int64_t height = world->height > 0 ? world->height : 0;
    if (width <= 0 || height <= 0)
        return;
    if (timeline->letterbox_amount > 0.0) {
        int64_t bar = (int64_t)((double)height * timeline->letterbox_amount);
        if (bar > 0) {
            rt_canvas3d_draw_rect2d(world->canvas, 0, 0, width, bar, 0x000000);
            rt_canvas3d_draw_rect2d(world->canvas, 0, height - bar, width, bar, 0x000000);
        }
    }
    if (timeline->fade_alpha > 0.0)
        rt_canvas3d_draw_rect2d_alpha(
            world->canvas, 0, 0, width, height, 0x000000, timeline->fade_alpha);
    if (timeline->active_subtitle[0] != '\0') {
        rt_string text = rt_const_cstr(timeline->active_subtitle);
        int64_t text_x = width / 2 - (int64_t)(strlen(timeline->active_subtitle) * 4);
        if (text_x < 8)
            text_x = 8;
        rt_canvas3d_draw_text2d(
            world->canvas, text_x, (int64_t)((double)height * 0.85), text, 0xFFFFFF);
        rt_string_unref(text);
    }
}

//=========================================================================
// Play / skip / stop and world installation
//=========================================================================

/// @brief Install and start a timeline (one per world; replacing stops the old).
/// @param world_obj Borrowed live World3D handle.
/// @param timeline_obj Borrowed same-world Timeline3D handle retained as active.
void rt_game3d_world_play_timeline(void *world_obj, void *timeline_obj) {
    rt_game3d_world *world =
        game3d_world_checked(world_obj, "Game3D.World3D.playTimeline: invalid world");
    rt_game3d_timeline *timeline = game3d_timeline_checked(
        timeline_obj, "Game3D.World3D.playTimeline: timeline must be Timeline3D");
    if (!world || !timeline)
        return;
    if (!game3d_timeline_repair_state(timeline)) {
        rt_trap("Game3D.World3D.playTimeline: corrupt timeline track storage");
        return;
    }
    void *bound_world = rt_g3d_checked_or_null(timeline->world, RT_G3D_GAME3D_WORLD_CLASS_ID);
    if (bound_world && bound_world != world) {
        rt_trap("Game3D.World3D.playTimeline: timeline belongs to another world");
        return;
    }
    rt_game3d_timeline *previous = (rt_game3d_timeline *)rt_g3d_checked_or_null(
        world->active_timeline, RT_G3D_GAME3D_TIMELINE_CLASS_ID);
    if (previous && previous != timeline)
        previous->playing = 0;
    game3d_assign_typed_ref(&world->active_timeline, timeline, RT_G3D_GAME3D_TIMELINE_CLASS_ID);
    game3d_timeline_reset(timeline);
}

/// @brief The world's active timeline (NULL when none).
/// @param world_obj Borrowed live World3D handle.
/// @return Borrowed active Timeline3D handle, or `NULL`.
void *rt_game3d_world_active_timeline(void *world_obj) {
    rt_game3d_world *world =
        game3d_world_checked(world_obj, "Game3D.World3D.activeTimeline: invalid world");
    return world ? rt_g3d_checked_or_null(world->active_timeline, RT_G3D_GAME3D_TIMELINE_CLASS_ID)
                 : NULL;
}

/// @brief Stop and uninstall the world's active timeline (controller resumes).
/// @param world_obj Borrowed live World3D handle.
void rt_game3d_world_stop_timeline(void *world_obj) {
    rt_game3d_world *world =
        game3d_world_checked(world_obj, "Game3D.World3D.stopTimeline: invalid world");
    if (!world)
        return;
    rt_game3d_timeline *timeline = (rt_game3d_timeline *)rt_g3d_checked_or_null(
        world->active_timeline, RT_G3D_GAME3D_TIMELINE_CLASS_ID);
    if (timeline)
        timeline->playing = 0;
    game3d_release_typed_ref(&world->active_timeline, RT_G3D_GAME3D_TIMELINE_CLASS_ID);
}

/// @brief Skip to the end: past-fire pending tracks in order (anims apply their
///   final state instantly, audio stays silent, markers fire), apply the end
///   camera, and finish. Gated by `skippable`.
/// @param obj Borrowed Timeline3D handle.
void rt_game3d_timeline_skip(void *obj) {
    rt_game3d_timeline *timeline =
        game3d_timeline_checked(obj, "Game3D.Timeline3D.skip: invalid timeline");
    if (!timeline || !timeline->playing || !timeline->skippable)
        return;
    if (!game3d_timeline_repair_state(timeline)) {
        timeline->playing = 0;
        rt_trap("Game3D.Timeline3D.skip: corrupt track storage");
        return;
    }
    rt_game3d_world *world =
        (rt_game3d_world *)rt_g3d_checked_or_null(timeline->world, RT_G3D_GAME3D_WORLD_CLASS_ID);
    if (!world)
        return;
    for (int32_t i = 0; i < timeline->track_count; ++i) {
        rt_game3d_tl_track *track = &timeline->tracks[i];
        if (track->fired)
            continue;
        if (track->type == RT_GAME3D_TL_ANIM || track->type == RT_GAME3D_TL_MARKER)
            game3d_timeline_fire(world, timeline, track, 1);
        else if (track->type == RT_GAME3D_TL_AUDIO)
            track->fired = 1; /* silent past-fire */
    }
    timeline->time = timeline->duration;
    timeline->playing = 0;
    timeline->finished = 1;
    timeline->just_finished = 1;
    timeline->letterbox_amount = 0.0;
    timeline->fade_alpha = 0.0;
    timeline->active_subtitle[0] = '\0';
    /* Apply the end-of-timeline camera immediately. */
    game3d_world_timeline_camera(world);
}

/// @brief Stop playback (controller resumes next step); keeps the playhead.
/// @param obj Borrowed Timeline3D handle.
void rt_game3d_timeline_stop(void *obj) {
    rt_game3d_timeline *timeline =
        game3d_timeline_checked(obj, "Game3D.Timeline3D.stop: invalid timeline");
    if (!timeline)
        return;
    timeline->playing = 0;
    rt_game3d_world *world =
        (rt_game3d_world *)rt_g3d_checked_or_null(timeline->world, RT_G3D_GAME3D_WORLD_CLASS_ID);
    if (world && world->active_timeline == (void *)timeline)
        game3d_release_typed_ref(&world->active_timeline, RT_G3D_GAME3D_TIMELINE_CLASS_ID);
}
