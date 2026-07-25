//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/game/rt_animtimeline.h
// Purpose: Runtime bridge for Zanna.Game.AnimTimeline — a passive frame-based
//   scheduler that stores animation-state/tween payload spans plus discrete
//   markers, with play/pause/stop, looping, and event reporting.
//
// Key invariants:
//   - Timelines are heap-allocated opaque `void *` handles.
//   - Each timeline holds at most 16 tracks and 32 markers.
//   - All time is measured in integer frames; track indices are 0-based and
//     returned by the add_* functions in insertion order.
//   - Track spans are half-open; marker frames may equal total duration.
//   - advance() drives only the playhead and marker reporting. It does not push
//     values to concrete targets: this is a passive scheduler. Tween tracks
//     report their current interpolated value through payload C (computed on
//     read from payloads A/B and the track's progress); anim tracks report the
//     target state ID through payload A. The caller applies these to its own
//     AnimStateMachine / tween target.
//
// Ownership/Lifetime:
//   - rt_animtimeline_new returns an owned handle reclaimed by the GC.
//   - Track names are copied into fixed 32-byte inline buffers and truncated
//     to 31 bytes. Input strings are not retained.
//   - PollEvents returns a separately owned immutable snapshot.
//
// Links: src/runtime/game/rt_animtimeline.c (implementation)
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Passive frame timeline, track query, and marker-event API.
#pragma once

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Runtime class ID for AnimTimeline objects.
#define RT_ANIMTIMELINE_CLASS_ID INT64_C(-0x51021B)

/// @brief Create a timeline spanning @p total_duration_frames frames.
/// @param total_duration_frames Terminal playhead frame; non-positive becomes one.
/// @return Owned stopped timeline, or NULL on allocation.
void *rt_animtimeline_new(int64_t total_duration_frames);
/// @brief Add a passive track carrying an animation state ID for a frame span.
/// @param tl Borrowed timeline.
/// @param name Borrowed name copied to at most 31 bytes.
/// @param start_frame Start clamped to zero.
/// @param duration_frames Duration normalized to at least one.
/// @param anim_state_id State ID stored in payload A.
/// @return New 0-based index, or -1 for invalid/full timeline.
int64_t rt_animtimeline_add_anim_track(
    void *tl, rt_string name, int64_t start_frame, int64_t duration_frames, int64_t anim_state_id);
/// @brief Add a tween track carrying integer endpoints @p from and @p to.
/// @details payload C reports the current interpolated value between the
///          endpoints at the timeline's current frame; payloads A/B carry the
///          raw from/to. The caller applies the value to its own target.
/// @param tl Borrowed timeline.
/// @param name Borrowed name copied to at most 31 bytes.
/// @param start_frame Start clamped to zero.
/// @param duration_frames Duration normalized to at least one.
/// @param from Initial value stored in payload A.
/// @param to Final value stored in payload B.
/// @return New 0-based index, or -1 for invalid/full timeline.
int64_t rt_animtimeline_add_tween_track(void *tl,
                                        rt_string name,
                                        int64_t start_frame,
                                        int64_t duration_frames,
                                        int64_t from,
                                        int64_t to);
/// @brief Add a discrete marker that fires @p marker_id when @p frame is reached.
/// @param tl Borrowed timeline.
/// @param frame Frame clamped to zero; values beyond total duration are rejected.
/// @param marker_id Application-defined signed ID.
/// @return New 0-based marker index, or -1 for invalid/full/out-of-range input.
int64_t rt_animtimeline_add_marker(void *tl, int64_t frame, int64_t marker_id);
/// @brief Begin/resume playback.
/// @param tl Borrowed timeline.
void rt_animtimeline_play(void *tl);
/// @brief Pause playback, keeping the current frame.
/// @param tl Borrowed timeline.
void rt_animtimeline_pause(void *tl);
/// @brief Stop playback and reset to the first frame.
/// @details Also clears finished/latest-event state and resets marker latches.
/// @param tl Borrowed timeline.
void rt_animtimeline_stop(void *tl);
/// @brief True while the timeline is actively playing.
/// @param tl Borrowed timeline.
/// @return One when playing; otherwise zero.
int8_t rt_animtimeline_is_playing(void *tl);
/// @brief True once a non-looping timeline has reached its end.
/// @param tl Borrowed timeline.
/// @return One when finished; otherwise zero.
int8_t rt_animtimeline_is_finished(void *tl);
/// @brief Current playhead frame.
/// @param tl Borrowed timeline.
/// @return Current nonnegative frame, or zero on invalid input.
int64_t rt_animtimeline_get_current_frame(void *tl);
/// @brief Enable/disable looping at the end of the timeline.
/// @param tl Borrowed timeline.
/// @param loop Nonzero to wrap.
void rt_animtimeline_set_looping(void *tl, int8_t loop);
/// @brief Advance the playhead by @p delta_frames, firing crossed markers.
/// @details Latest-event output is cleared on every call. Positive playing
///          advances handle entry-frame markers, loop wrap, and full-cycle
///          spans; non-looping playback clamps and stops at total duration.
/// @param tl Borrowed timeline.
/// @param delta_frames Positive frame delta.
void rt_animtimeline_advance(void *tl, int64_t delta_frames);
/// @brief Number of markers fired during the most recent advance().
/// @param tl Borrowed timeline.
/// @return Count from zero through 32.
int64_t rt_animtimeline_events_fired_count(void *tl);
/// @brief Marker id of the @p index-th event fired during the last advance().
/// @param tl Borrowed timeline.
/// @param index Zero-based fired-marker index.
/// @return Marker ID, or zero for invalid input/index.
int64_t rt_animtimeline_event_fired_id(void *tl, int64_t index);
/// @brief Snapshot marker IDs fired by the most recent advance().
/// @details Returns an immutable Zanna.Game.AnimationEventBatch whose contents
///          are independent from later Advance(), Play(), Stop(), or looping
///          state changes. This is the composable replacement for reading
///          EventsFiredCount and EventFiredId from mutable state.
/// @param tl Borrowed AnimTimeline object.
/// @return New Zanna.Game.AnimationEventBatch object, or NULL on allocation failure.
void *rt_animtimeline_poll_events(void *tl);
/// @brief True if the track at @p track_index is active at the current frame.
/// @param tl Borrowed timeline.
/// @param track_index Zero-based track index.
/// @return One in the half-open track span; otherwise zero.
int8_t rt_animtimeline_track_is_active(void *tl, int64_t track_index);
/// @brief Normalized [0,1] progress of the track at @p track_index.
/// @param tl Borrowed timeline.
/// @param track_index Zero-based track index.
/// @return Clamped linear fraction, or 0.0 for invalid input.
double rt_animtimeline_track_progress(void *tl, int64_t track_index);
/// @brief Track payload slot A (anim track: state id; tween track: from value).
/// @param tl Borrowed timeline.
/// @param track_index Zero-based track index.
/// @return Stored payload, or zero for invalid input.
int64_t rt_animtimeline_track_payload_a(void *tl, int64_t track_index);
/// @brief Track payload slot B (tween track: to value).
/// @param tl Borrowed timeline.
/// @param track_index Zero-based track index.
/// @return Stored payload, or zero for invalid input.
int64_t rt_animtimeline_track_payload_b(void *tl, int64_t track_index);
/// @brief Track payload slot C. For a tween track this is the current
///        interpolated value between payloads A and B at the timeline's current
///        frame (the `from` value before/at its start and `to` at/after its end);
///        for other track kinds it is 0.
/// @details Interpolation rounds half away from zero and saturates to int64.
/// @param tl Borrowed timeline.
/// @param track_index Zero-based track index.
/// @return Derived/stored payload, or zero for invalid input.
int64_t rt_animtimeline_track_payload_c(void *tl, int64_t track_index);

#ifdef __cplusplus
}
#endif
