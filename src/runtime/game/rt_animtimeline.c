//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/game/rt_animtimeline.c
// Purpose: Implements a fixed-capacity frame timeline containing passive
// animation/tween tracks, marker events, playback state, and event snapshots.
//
// Key invariants:
//   - Timelines hold at most 16 tracks and 32 markers in registration order.
//   - Total duration and track duration are at least one frame.
//   - Track spans are half-open [start, start + duration); marker frames may
//     include the terminal total-duration frame.
//   - One advance reports each crossed marker at most once, including loop wrap
//     and frame-zero entry semantics.
//
// Ownership/Lifetime:
//   - Timeline objects contain all track/marker state inline and need no finalizer.
//   - Track names are copied to fixed 31-byte C-string storage.
//   - PollEvents returns an independent owned immutable batch.
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Frame timeline scheduling, markers, tracks, and tween queries.

#include "rt_animtimeline.h"

#include "rt_animation_events.h"
#include "rt_object.h"
#include "rt_trap.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#define RT_ANIMTIMELINE_MAX_TRACKS 16
#define RT_ANIMTIMELINE_MAX_EVENTS 32

/// @brief Internal payload interpretation for a timeline track.
typedef enum {
    TIMELINE_TRACK_ANIM = 0,
    TIMELINE_TRACK_TWEEN = 1,
    TIMELINE_TRACK_MARKER = 2,
} timeline_track_kind_t;

/// @brief One inline passive animation, tween, or marker track.
typedef struct {
    char name[32];
    timeline_track_kind_t kind;
    int64_t start_frame;
    int64_t duration_frames;
    int64_t payload_a;
    int64_t payload_b;
    int64_t payload_c;
} rt_animtimeline_track_t;

/// @brief Registered marker and its per-play-cycle fired latch.
typedef struct {
    int64_t frame;
    int64_t event_id;
    uint8_t fired;
} rt_animtimeline_event_t;

/// @brief Native state backing a Zanna.Game.AnimTimeline object.
typedef struct {
    void *vptr;
    int64_t total_duration_frames;
    int64_t current_frame;
    rt_animtimeline_track_t tracks[RT_ANIMTIMELINE_MAX_TRACKS];
    int64_t track_count;
    rt_animtimeline_event_t events[RT_ANIMTIMELINE_MAX_EVENTS];
    int64_t event_count;
    int64_t events_fired_ids[RT_ANIMTIMELINE_MAX_EVENTS];
    int64_t events_fired_count;
    int8_t playing;
    int8_t looping;
    int8_t finished;
    int8_t entry_pending; // fire markers on the entry frame at the first advance (VDOC-278)
} rt_animtimeline_impl;

/// @brief Safe-cast a handle to the AnimTimeline impl, trapping @p api on a
///        class-id mismatch.
/// @param ptr Borrowed candidate object; may be NULL.
/// @param api Borrowed mismatch diagnostic.
/// @return Valid timeline implementation, or NULL for null/mismatch.
static rt_animtimeline_impl *checked_timeline(void *ptr, const char *api) {
    if (!ptr)
        return NULL;
    if (rt_obj_class_id(ptr) != RT_ANIMTIMELINE_CLASS_ID) {
        rt_trap(api);
        return NULL;
    }
    return (rt_animtimeline_impl *)ptr;
}

/// @brief Clamp a frame index to be non-negative.
/// @param frame Candidate frame.
/// @return @p frame or zero when negative.
static int64_t clamp_frame(int64_t frame) {
    return frame < 0 ? 0 : frame;
}

/// @brief Saturating int64 addition (clamps to INT64_MIN/MAX on overflow).
/// @param a Left operand.
/// @param b Right operand.
/// @return Exact sum or the corresponding signed bound.
static int64_t timeline_add_sat_i64(int64_t a, int64_t b) {
    if (b > 0 && a > INT64_MAX - b)
        return INT64_MAX;
    if (b < 0 && a < INT64_MIN - b)
        return INT64_MIN;
    return a + b;
}

/// @brief End frame of a track (start + duration, saturating). 0 if NULL.
/// @param track Borrowed track; may be NULL.
/// @return Saturating exclusive end frame, or zero.
static int64_t timeline_track_end_frame(const rt_animtimeline_track_t *track) {
    if (!track)
        return 0;
    return timeline_add_sat_i64(track->start_frame, track->duration_frames);
}

/// @brief Copy a runtime string into a fixed @p cap char buffer, NUL-terminated
///        and truncated to fit. Empty buffer on NULL @p name.
/// @param dst Writable destination; may be NULL.
/// @param cap Destination capacity including terminator.
/// @param name Borrowed runtime string; may be NULL.
static void timeline_copy_name(char *dst, size_t cap, rt_string name) {
    if (!dst || cap == 0)
        return;
    dst[0] = '\0';
    if (!name)
        return;
    const char *s = rt_string_cstr(name);
    int64_t len = rt_str_len(name);
    if (len < 0)
        len = 0;
    if ((size_t)len >= cap)
        len = (int64_t)cap - 1;
    memcpy(dst, s, (size_t)len);
    dst[len] = '\0';
}

/// @brief Append a track of @p kind with the given span and payloads.
/// @details Clamps start frame to >= 0 and duration to >= 1; copies @p name
///          into the track's inline buffer.
/// @param tl Mutable timeline; may be NULL.
/// @param kind Payload interpretation.
/// @param name Borrowed optional track name.
/// @param start_frame Candidate nonnegative start.
/// @param duration_frames Candidate positive duration.
/// @param a First signed payload.
/// @param b Second signed payload.
/// @param c Third signed payload.
/// @return New zero-based track index, or -1 for null/full timeline.
static int64_t timeline_add_track(rt_animtimeline_impl *tl,
                                  timeline_track_kind_t kind,
                                  rt_string name,
                                  int64_t start_frame,
                                  int64_t duration_frames,
                                  int64_t a,
                                  int64_t b,
                                  int64_t c) {
    if (!tl)
        return -1;
    if (tl->track_count >= RT_ANIMTIMELINE_MAX_TRACKS)
        return -1;
    int64_t idx = tl->track_count++;
    rt_animtimeline_track_t *track = &tl->tracks[idx];
    memset(track, 0, sizeof(*track));
    timeline_copy_name(track->name, sizeof(track->name), name);
    track->kind = kind;
    track->start_frame = clamp_frame(start_frame);
    track->duration_frames = duration_frames <= 0 ? 1 : duration_frames;
    track->payload_a = a;
    track->payload_b = b;
    track->payload_c = c;
    return idx;
}

/// @brief Create an empty stopped animation timeline.
/// @param total_duration_frames Terminal frame; non-positive becomes one.
/// @return Owned timeline object, or NULL on allocation failure.
void *rt_animtimeline_new(int64_t total_duration_frames) {
    rt_animtimeline_impl *tl = (rt_animtimeline_impl *)rt_obj_new_i64(
        RT_ANIMTIMELINE_CLASS_ID, (int64_t)sizeof(rt_animtimeline_impl));
    if (!tl)
        return NULL;
    memset(tl, 0, sizeof(*tl));
    tl->total_duration_frames = total_duration_frames <= 0 ? 1 : total_duration_frames;
    return tl;
}

/// @brief Add a passive animation-state track.
/// @param ptr Borrowed timeline.
/// @param name Borrowed optional name copied/truncated inline.
/// @param start_frame Start frame, clamped to zero.
/// @param duration_frames Duration, normalized to at least one.
/// @param anim_state_id Animation state ID stored as payload A.
/// @return Track index, or -1 for invalid/full timeline.
int64_t rt_animtimeline_add_anim_track(void *ptr,
                                       rt_string name,
                                       int64_t start_frame,
                                       int64_t duration_frames,
                                       int64_t anim_state_id) {
    return timeline_add_track(
        checked_timeline(ptr, "AnimTimeline.AddAnimTrack: expected AnimTimeline"),
        TIMELINE_TRACK_ANIM,
        name,
        start_frame,
        duration_frames,
        anim_state_id,
        0,
        0);
}

/// @brief Add a passive integer tween track.
/// @param ptr Borrowed timeline.
/// @param name Borrowed optional name copied/truncated inline.
/// @param start_frame Start frame, clamped to zero.
/// @param duration_frames Duration, normalized to at least one.
/// @param from Initial integer payload.
/// @param to Final integer payload.
/// @return Track index, or -1 for invalid/full timeline.
int64_t rt_animtimeline_add_tween_track(void *ptr,
                                        rt_string name,
                                        int64_t start_frame,
                                        int64_t duration_frames,
                                        int64_t from,
                                        int64_t to) {
    return timeline_add_track(
        checked_timeline(ptr, "AnimTimeline.AddTweenTrack: expected AnimTimeline"),
        TIMELINE_TRACK_TWEEN,
        name,
        start_frame,
        duration_frames,
        from,
        to,
        0);
}

/// @brief Register a marker event at a playable frame.
/// @details Negative frames clamp to zero; frames beyond total duration and a
///          33rd marker are rejected. The terminal duration frame is valid.
/// @param ptr Borrowed timeline.
/// @param frame Marker frame.
/// @param marker_id Application-defined signed ID.
/// @return Marker index, or -1 when invalid/full/out of range.
int64_t rt_animtimeline_add_marker(void *ptr, int64_t frame, int64_t marker_id) {
    rt_animtimeline_impl *tl =
        checked_timeline(ptr, "AnimTimeline.AddMarker: expected AnimTimeline");
    if (!tl)
        return -1;
    if (tl->event_count >= RT_ANIMTIMELINE_MAX_EVENTS)
        return -1;
    // Reject markers beyond the playable range: a non-looping playhead stops at
    // total_duration and a looping one wraps before it, so a marker past the end
    // could never be crossed and would look registered but stay silent forever
    // (VDOC-278). Negative frames clamp to the start (frame 0), which is in range.
    if (frame > tl->total_duration_frames)
        return -1;
    int64_t idx = tl->event_count++;
    tl->events[idx].frame = clamp_frame(frame);
    tl->events[idx].event_id = marker_id;
    tl->events[idx].fired = 0;
    return idx;
}

/// @brief Start or resume timeline playback.
/// @details Clears finished state. Starting at frame zero arms entry-marker
///          firing; resuming later does not re-arm already crossed markers.
/// @param ptr Borrowed timeline.
void rt_animtimeline_play(void *ptr) {
    rt_animtimeline_impl *tl = checked_timeline(ptr, "AnimTimeline.Play: expected AnimTimeline");
    if (!tl)
        return;
    tl->playing = 1;
    tl->finished = 0;
    // Arm entry-frame marker firing only when starting from the very beginning,
    // so resuming mid-timeline does not re-fire already-crossed markers.
    tl->entry_pending = (tl->current_frame == 0) ? 1 : 0;
}

/// @brief Pause playback without moving the playhead or clearing markers.
/// @param ptr Borrowed timeline.
void rt_animtimeline_pause(void *ptr) {
    rt_animtimeline_impl *tl = checked_timeline(ptr, "AnimTimeline.Pause: expected AnimTimeline");
    if (tl)
        tl->playing = 0;
}

/// @brief Stop and rewind the timeline.
/// @details Clears playing/finished state, returns to frame zero, drops latest
///          fired outputs, and resets every marker latch.
/// @param ptr Borrowed timeline.
void rt_animtimeline_stop(void *ptr) {
    rt_animtimeline_impl *tl = checked_timeline(ptr, "AnimTimeline.Stop: expected AnimTimeline");
    if (!tl)
        return;
    tl->playing = 0;
    tl->finished = 0;
    tl->current_frame = 0;
    tl->entry_pending = 0;
    tl->events_fired_count = 0;
    for (int64_t i = 0; i < tl->event_count; i++)
        tl->events[i].fired = 0;
}

/// @brief Query whether playback is active.
/// @param ptr Borrowed timeline.
/// @return One when playing; otherwise zero.
int8_t rt_animtimeline_is_playing(void *ptr) {
    rt_animtimeline_impl *tl =
        checked_timeline(ptr, "AnimTimeline.IsPlaying: expected AnimTimeline");
    return tl ? tl->playing : 0;
}

/// @brief Query whether non-looping playback reached total duration.
/// @param ptr Borrowed timeline.
/// @return One when finished; otherwise zero.
int8_t rt_animtimeline_is_finished(void *ptr) {
    rt_animtimeline_impl *tl =
        checked_timeline(ptr, "AnimTimeline.IsFinished: expected AnimTimeline");
    return tl ? tl->finished : 0;
}

/// @brief Return the current playhead frame.
/// @param ptr Borrowed timeline.
/// @return Nonnegative frame, or zero on invalid input.
int64_t rt_animtimeline_get_current_frame(void *ptr) {
    rt_animtimeline_impl *tl =
        checked_timeline(ptr, "AnimTimeline.CurrentFrame: expected AnimTimeline");
    return tl ? tl->current_frame : 0;
}

/// @brief Enable or disable wraparound at total duration.
/// @param ptr Borrowed timeline.
/// @param loop Nonzero to enable looping.
void rt_animtimeline_set_looping(void *ptr, int8_t loop) {
    rt_animtimeline_impl *tl =
        checked_timeline(ptr, "AnimTimeline.SetLooping: expected AnimTimeline");
    if (tl)
        tl->looping = loop ? 1 : 0;
}

/// @brief True if @p frame lies in the half-open span (before, after] — used
///        to detect markers crossed during an advance step.
/// @param before Previous playhead.
/// @param after New playhead without wrap.
/// @param frame Marker frame.
/// @return One when crossed; otherwise zero.
static int8_t timeline_crossed(int64_t before, int64_t after, int64_t frame) {
    return frame > before && frame <= after;
}

/// @brief Advance a playing timeline by positive frames and collect markers.
/// @details Clears prior fired output even when no movement occurs. Non-looping
///          playback clamps/stops at total duration. Looping playback uses
///          modulo wrap, resets marker latches, and reports markers on both
///          sides of the wrap; advances spanning a full cycle may report every
///          marker once. Frame-zero entry markers fire on the first advance.
/// @param ptr Borrowed timeline.
/// @param delta_frames Positive frame delta; non-positive values only clear
///        latest fired output.
void rt_animtimeline_advance(void *ptr, int64_t delta_frames) {
    rt_animtimeline_impl *tl = checked_timeline(ptr, "AnimTimeline.Advance: expected AnimTimeline");
    if (!tl)
        return;
    tl->events_fired_count = 0;
    if (delta_frames <= 0)
        return;
    if (!tl->playing || tl->finished)
        return;

    int64_t before = tl->current_frame;
    int64_t after_unwrapped = timeline_add_sat_i64(before, delta_frames);
    int64_t after = after_unwrapped;
    int8_t wrapped = 0;
    int8_t spanned_full_cycle = delta_frames >= tl->total_duration_frames;
    if (after_unwrapped >= tl->total_duration_frames) {
        if (tl->looping && tl->total_duration_frames > 0) {
            after = after_unwrapped % tl->total_duration_frames;
            wrapped = 1;
            for (int64_t i = 0; i < tl->event_count; i++)
                tl->events[i].fired = 0;
        } else {
            after = tl->total_duration_frames;
            tl->finished = 1;
            tl->playing = 0;
        }
    }

    for (int64_t i = 0; i < tl->event_count && tl->events_fired_count < RT_ANIMTIMELINE_MAX_EVENTS;
         i++) {
        int8_t fire = 0;
        if (!tl->events[i].fired && timeline_crossed(before, after, tl->events[i].frame))
            fire = 1;
        // Entry markers: a marker exactly on the start frame is missed by the
        // half-open (before, after] crossing on the first advance, so fire it
        // once on entry (typically frame 0) (VDOC-278).
        if (tl->entry_pending && !tl->events[i].fired && tl->events[i].frame == before)
            fire = 1;
        if (wrapped &&
            (spanned_full_cycle || tl->events[i].frame > before || tl->events[i].frame <= after))
            fire = 1;
        if (!fire)
            continue;
        tl->events[i].fired = 1;
        tl->events_fired_ids[tl->events_fired_count++] = tl->events[i].event_id;
    }
    tl->entry_pending = 0;
    tl->current_frame = after;
}

/// @brief Return the number of markers fired by the latest advance.
/// @param ptr Borrowed timeline.
/// @return Count from zero through 32.
int64_t rt_animtimeline_events_fired_count(void *ptr) {
    rt_animtimeline_impl *tl =
        checked_timeline(ptr, "AnimTimeline.EventsFiredCount: expected AnimTimeline");
    return tl ? tl->events_fired_count : 0;
}

/// @brief Read a latest-advance marker ID by index.
/// @param ptr Borrowed timeline.
/// @param index Zero-based fired-marker position.
/// @return Marker ID, or zero for invalid input/index.
int64_t rt_animtimeline_event_fired_id(void *ptr, int64_t index) {
    rt_animtimeline_impl *tl =
        checked_timeline(ptr, "AnimTimeline.EventFiredId: expected AnimTimeline");
    if (!tl || index < 0 || index >= tl->events_fired_count)
        return 0;
    return tl->events_fired_ids[index];
}

/// @brief Snapshot marker IDs fired by the latest advance.
/// @param ptr Borrowed timeline.
/// @return Owned immutable AnimationEventBatch, empty for invalid timeline, or
///         NULL on snapshot allocation failure.
void *rt_animtimeline_poll_events(void *ptr) {
    rt_animtimeline_impl *tl =
        checked_timeline(ptr, "AnimTimeline.PollEvents: expected AnimTimeline");
    if (!tl)
        return rt_animation_event_batch_from_ids(NULL, 0);
    return rt_animation_event_batch_from_ids(tl->events_fired_ids, tl->events_fired_count);
}

/// @brief Return a track after bounds validation.
/// @param tl Borrowed timeline; may be NULL.
/// @param index Zero-based track index.
/// @return Borrowed track pointer, or NULL.
static rt_animtimeline_track_t *timeline_track(rt_animtimeline_impl *tl, int64_t index) {
    if (!tl || index < 0 || index >= tl->track_count)
        return NULL;
    return &tl->tracks[index];
}

/// @brief Fraction (0..1) of @p track completed at the timeline's current frame:
///        0 before/at the start, 1 at/after the end, linear in between.
/// @param tl Borrowed valid timeline.
/// @param track Borrowed valid track.
/// @return Clamped linear fraction from 0.0 through 1.0.
static double timeline_track_frac(const rt_animtimeline_impl *tl,
                                  const rt_animtimeline_track_t *track) {
    if (tl->current_frame <= track->start_frame)
        return 0.0;
    if (tl->current_frame >= timeline_track_end_frame(track))
        return 1.0;
    return (double)(tl->current_frame - track->start_frame) / (double)track->duration_frames;
}

/// @brief Interpolate an integer from @p a to @p b by fraction @p frac, anchored
///        on the exact endpoints (frac 0 -> a, frac 1 -> b) and rounded
///        half-away-from-zero with saturation. Wide ranges use long double.
/// @param a Start value.
/// @param b End value.
/// @param frac Interpolation fraction.
/// @return Endpoint/clamped rounded interpolation.
static int64_t timeline_lerp_i64(int64_t a, int64_t b, double frac) {
    if (a == b || frac <= 0.0)
        return a;
    if (frac >= 1.0)
        return b;
    long double result = (long double)a + (long double)frac * ((long double)b - (long double)a);
    if (!isfinite((double)result))
        return frac < 0.5 ? a : b;
    long double rounded = result >= 0.0L ? floorl(result + 0.5L) : ceill(result - 0.5L);
    if (rounded >= (long double)INT64_MAX)
        return INT64_MAX;
    if (rounded <= (long double)INT64_MIN)
        return INT64_MIN;
    return (int64_t)rounded;
}

/// @brief Test whether the playhead lies inside a track's half-open span.
/// @param ptr Borrowed timeline.
/// @param track_index Zero-based track index.
/// @return One for `start <= current < end`; otherwise zero.
int8_t rt_animtimeline_track_is_active(void *ptr, int64_t track_index) {
    rt_animtimeline_impl *tl =
        checked_timeline(ptr, "AnimTimeline.TrackIsActive: expected AnimTimeline");
    rt_animtimeline_track_t *track = timeline_track(tl, track_index);
    if (!track)
        return 0;
    return tl->current_frame >= track->start_frame &&
           tl->current_frame < timeline_track_end_frame(track);
}

/// @brief Return a track's current linear completion fraction.
/// @param ptr Borrowed timeline.
/// @param track_index Zero-based track index.
/// @return Clamped 0.0–1.0 fraction, or 0.0 for invalid input.
double rt_animtimeline_track_progress(void *ptr, int64_t track_index) {
    rt_animtimeline_impl *tl =
        checked_timeline(ptr, "AnimTimeline.TrackProgress: expected AnimTimeline");
    rt_animtimeline_track_t *track = timeline_track(tl, track_index);
    if (!track)
        return 0.0;
    return timeline_track_frac(tl, track);
}

/// @brief Return track payload A.
/// @param ptr Borrowed timeline.
/// @param track_index Zero-based track index.
/// @return Stored payload, or zero for invalid input.
int64_t rt_animtimeline_track_payload_a(void *ptr, int64_t track_index) {
    rt_animtimeline_impl *tl =
        checked_timeline(ptr, "AnimTimeline.TrackPayloadA: expected AnimTimeline");
    rt_animtimeline_track_t *track = timeline_track(tl, track_index);
    return track ? track->payload_a : 0;
}

/// @brief Return track payload B.
/// @param ptr Borrowed timeline.
/// @param track_index Zero-based track index.
/// @return Stored payload, or zero for invalid input.
int64_t rt_animtimeline_track_payload_b(void *ptr, int64_t track_index) {
    rt_animtimeline_impl *tl =
        checked_timeline(ptr, "AnimTimeline.TrackPayloadB: expected AnimTimeline");
    rt_animtimeline_track_t *track = timeline_track(tl, track_index);
    return track ? track->payload_b : 0;
}

/// @brief Return payload C or a tween's current interpolated value.
/// @details Tween tracks derive a half-away-from-zero, saturating interpolation
///          between payloads A/B at current track progress. Other track kinds
///          return stored payload C.
/// @param ptr Borrowed timeline.
/// @param track_index Zero-based track index.
/// @return Derived/stored value, or zero for invalid input.
int64_t rt_animtimeline_track_payload_c(void *ptr, int64_t track_index) {
    rt_animtimeline_impl *tl =
        checked_timeline(ptr, "AnimTimeline.TrackPayloadC: expected AnimTimeline");
    rt_animtimeline_track_t *track = timeline_track(tl, track_index);
    if (!track)
        return 0;
    // For a tween track, payload C is the CURRENT interpolated value between the
    // from (A) and to (B) endpoints at the timeline's current frame — computed on
    // read, not a stored constant. Previously it was always 0, so "tween track"
    // and "current interpolated value" described work that never happened
    // (VDOC-277). The timeline stays a passive scheduler: it reports the value and
    // the caller applies it to its own target.
    if (track->kind == TIMELINE_TRACK_TWEEN)
        return timeline_lerp_i64(track->payload_a, track->payload_b, timeline_track_frac(tl, track));
    return track->payload_c;
}
