//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/game/rt_spriteanim.c
/// @file
/// @brief Implements tick-based sprite-frame playback with looping,
///        ping-pong, one-shot, pausing, and fractional speed.
// Purpose: Frame-index animation controller for Zanna sprite sheets. Advances
//   an integer frame index through a configurable sequence at a per-update
//   speed multiplier and configurable update ticks per displayed frame.
//   Supports looping, ping-pong (forward then reverse), one-shot (stops at
//   last frame), and manual frame control. The controller does not draw
//   anything — it only computes which frame to display each update, leaving
//   rendering to the sprite/spritebatch layer.
//
// Key invariants:
//   - Frames are identified by non-negative integers (indices into a sprite
//     sheet row). The range [start_frame, end_frame] is inclusive. There is no
//     compile-time cap on frame count — any integer range is valid.
//   - Animation speed is a unitless [0,10] multiplier. Each no-argument
//     Update() adds it to a fractional tick accumulator; frame_duration says
//     how many whole ticks each displayed frame lasts.
//   - Loop mode: wraps from end_frame back to start_frame automatically.
//   - PingPong mode: plays start→end, then end→start, alternating direction.
//   - One-shot mode displays the terminal frame for its full duration, then
//     stops when the next frame step is attempted.
//   - Calling Reset() returns to start_frame and clears the complete flag.
//   - The current frame is always in [start_frame, end_frame].
//
// Ownership/Lifetime:
//   - SpriteAnim objects are GC-managed (rt_obj_new_i64). They hold no external
//     resources and require no finalizer beyond the GC reclaiming the struct.
//
// Links: src/runtime/game/rt_spriteanim.h (public API),
//        src/runtime/graphics/rt_spritebatch.h (rendering),
//        docs/zannalib/game.md (SpriteAnim section)
//
//===----------------------------------------------------------------------===//

#include "rt_spriteanim.h"
#include "rt_object.h"
#include "rt_trap.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>

/// @brief Mutable playback state owned by a SpriteAnimation runtime object.
struct rt_spriteanim_impl {
    int64_t start_frame;    ///< First frame index.
    int64_t end_frame;      ///< Last frame index (inclusive).
    int64_t current_frame;  ///< Current frame index.
    int64_t frame_duration; ///< Frames to display each animation frame.
    int64_t frame_counter;  ///< Counter for frame timing.

    double speed;       ///< Playback speed multiplier.
    double speed_accum; ///< Accumulator for fractional speed.

    int8_t playing;       ///< 1 if animation is playing.
    int8_t paused;        ///< 1 if animation is paused.
    int8_t loop;          ///< 1 if animation loops.
    int8_t pingpong;      ///< 1 if animation ping-pongs.
    int8_t finished;      ///< 1 if one-shot animation completed.
    int8_t direction;     ///< 1 = forward, -1 = backward (for pingpong).
    int8_t frame_changed; ///< 1 if frame changed this update.
};

/// @brief Safe-cast a handle to the SpriteAnim impl, trapping @p api on a
///        class-id mismatch.
/// @param anim Borrowed candidate SpriteAnimation handle.
/// @param api Trap message identifying the calling API.
/// @return Borrowed implementation pointer, or `NULL` when @p anim is `NULL`.
static rt_spriteanim checked_spriteanim(rt_spriteanim anim, const char *api) {
    if (!anim)
        return NULL;
    if (rt_obj_class_id(anim) != RT_SPRITEANIM_CLASS_ID) {
        rt_trap(api);
        return NULL;
    }
    return anim;
}

/// @brief Integer percentage value*100/total, clamped to [0, 100]; 0 for
///        non-positive inputs.
/// @param value Non-negative progress numerator.
/// @param total Positive progress denominator.
/// @return Truncated percentage in the inclusive range 0..100.
static int64_t spriteanim_percent_i64(int64_t value, int64_t total) {
    if (value <= 0 || total <= 0)
        return 0;
    long double scaled = ((long double)value * 100.0L) / (long double)total;
    int64_t pct = scaled >= (long double)INT64_MAX ? INT64_MAX : (int64_t)scaled;
    return pct > 100 ? 100 : pct;
}

/// @brief Step the animation one frame in its current direction, applying
///        loop/ping-pong/one-shot end behavior.
/// @param anim Borrowed SpriteAnimation implementation.
/// @return Non-zero if a clip boundary (start or end) was crossed this step.
static int8_t rt_spriteanim_advance_one_frame(rt_spriteanim anim) {
    int8_t crossed_end = 0;
    int8_t crossed_start = 0;

    if (anim->direction > 0) {
        if (anim->current_frame >= anim->end_frame)
            crossed_end = 1;
        else
            anim->current_frame++;
    } else {
        if (anim->current_frame <= anim->start_frame)
            crossed_start = 1;
        else
            anim->current_frame--;
    }

    if (anim->pingpong) {
        if (anim->direction == 1 && crossed_end) {
            anim->direction = -1;
            anim->current_frame = anim->end_frame - 1;
            if (anim->current_frame < anim->start_frame) {
                if (!anim->loop) {
                    anim->current_frame = anim->start_frame;
                    anim->finished = 1;
                    anim->playing = 0;
                    return 1;
                }
                anim->current_frame = anim->start_frame;
            }
        } else if (anim->direction == -1 && crossed_start) {
            if (anim->loop) {
                anim->direction = 1;
                anim->current_frame = anim->start_frame + 1;
                if (anim->current_frame > anim->end_frame)
                    anim->current_frame = anim->start_frame;
            } else {
                anim->current_frame = anim->start_frame;
                anim->finished = 1;
                anim->playing = 0;
                return 1;
            }
        }
    } else if (crossed_end) {
        if (anim->loop) {
            anim->current_frame = anim->start_frame;
        } else {
            anim->current_frame = anim->end_frame;
            anim->finished = 1;
            anim->playing = 0;
            return 1;
        }
    }

    return 0;
}

/// @brief Create a stopped, single-frame looping animation controller.
/// @details Defaults to frame zero, six update ticks per displayed frame,
///          forward direction, and speed 1.0.
/// @return Owned SpriteAnimation handle, or `NULL` if allocation fails.
rt_spriteanim rt_spriteanim_new(void) {
    struct rt_spriteanim_impl *anim = (struct rt_spriteanim_impl *)rt_obj_new_i64(
        RT_SPRITEANIM_CLASS_ID, (int64_t)sizeof(struct rt_spriteanim_impl));
    if (!anim)
        return NULL;

    anim->start_frame = 0;
    anim->end_frame = 0;
    anim->current_frame = 0;
    anim->frame_duration = 6; // Default: six update ticks per displayed frame
    anim->frame_counter = 0;

    anim->speed = 1.0;
    anim->speed_accum = 0.0;

    anim->playing = 0;
    anim->paused = 0;
    anim->loop = 1; // Default to looping
    anim->pingpong = 0;
    anim->finished = 0;
    anim->direction = 1;
    anim->frame_changed = 0;

    return anim;
}

/// @brief Release one owned SpriteAnimation reference.
/// @param anim Owned handle to release; `NULL` is ignored.
void rt_spriteanim_destroy(rt_spriteanim anim) {
    anim = checked_spriteanim(anim, "SpriteAnimation.Destroy: expected Zanna.Game.SpriteAnimation");
    if (anim && rt_obj_release_check0(anim))
        rt_obj_free(anim);
}

/// @brief Configure the inclusive frame range and ticks per displayed frame.
/// @details Negative starts clamp to zero, ends below the start collapse to a
///          single-frame range, and durations below one clamp to one. The call
///          resets position, timing, direction, and completion state without
///          changing play, pause, loop, ping-pong, or speed settings.
/// @param anim Borrowed SpriteAnimation handle.
/// @param start_frame First sprite-sheet frame index.
/// @param end_frame Last sprite-sheet frame index, inclusive.
/// @param frame_duration Number of accumulated update ticks per frame step.
void rt_spriteanim_setup(rt_spriteanim anim,
                         int64_t start_frame,
                         int64_t end_frame,
                         int64_t frame_duration) {
    anim = checked_spriteanim(anim, "SpriteAnimation.Setup: expected Zanna.Game.SpriteAnimation");
    if (!anim)
        return;

    if (start_frame < 0)
        start_frame = 0;
    if (end_frame < start_frame)
        end_frame = start_frame;
    if (frame_duration < 1)
        frame_duration = 1;

    anim->start_frame = start_frame;
    anim->end_frame = end_frame;
    anim->frame_duration = frame_duration;
    anim->current_frame = start_frame;
    anim->frame_counter = 0;
    anim->direction = 1;
    anim->finished = 0;
}

/// @brief Enable or disable looping; when enabled, the animation restarts after the last frame.
/// @param anim Borrowed SpriteAnimation handle.
/// @param loop Zero for one-shot behavior, nonzero to loop.
void rt_spriteanim_set_loop(rt_spriteanim anim, int8_t loop) {
    anim = checked_spriteanim(anim, "SpriteAnimation.SetLoop: expected Zanna.Game.SpriteAnimation");
    if (!anim)
        return;
    anim->loop = loop ? 1 : 0;
}

/// @brief Enable or disable ping-pong mode (forward then reverse, then forward again).
/// @param anim Borrowed SpriteAnimation handle.
/// @param pingpong Zero for forward-only playback, nonzero for alternating
///        direction.
void rt_spriteanim_set_pingpong(rt_spriteanim anim, int8_t pingpong) {
    anim = checked_spriteanim(anim,
                              "SpriteAnimation.SetPingPong: expected Zanna.Game.SpriteAnimation");
    if (!anim)
        return;
    anim->pingpong = pingpong ? 1 : 0;
}

/// @brief Return whether looping is enabled for this animation.
/// @param anim Borrowed SpriteAnimation handle.
/// @return `1` when looping is enabled; otherwise `0`.
int8_t rt_spriteanim_loop(rt_spriteanim anim) {
    anim = checked_spriteanim(anim, "SpriteAnimation.Loop: expected Zanna.Game.SpriteAnimation");
    return anim ? anim->loop : 0;
}

/// @brief Return whether ping-pong mode is enabled for this animation.
/// @param anim Borrowed SpriteAnimation handle.
/// @return `1` when ping-pong is enabled; otherwise `0`.
int8_t rt_spriteanim_pingpong(rt_spriteanim anim) {
    anim =
        checked_spriteanim(anim, "SpriteAnimation.PingPong: expected Zanna.Game.SpriteAnimation");
    return anim ? anim->pingpong : 0;
}

/// @brief Start playback from the first frame, resetting all internal counters.
/// @details Clears pause/completion state, restores forward direction, and
///          discards any fractional-speed accumulation.
/// @param anim Borrowed SpriteAnimation handle.
void rt_spriteanim_play(rt_spriteanim anim) {
    anim = checked_spriteanim(anim, "SpriteAnimation.Play: expected Zanna.Game.SpriteAnimation");
    if (!anim)
        return;
    anim->current_frame = anim->start_frame;
    anim->frame_counter = 0;
    anim->playing = 1;
    anim->paused = 0;
    anim->finished = 0;
    anim->direction = 1;
    anim->speed_accum = 0.0;
}

/// @brief Stop playback entirely (not paused — position is not preserved for resume).
/// @details Restores the first frame and initial timing/direction state without
///          changing clip or mode configuration.
/// @param anim Borrowed SpriteAnimation handle.
void rt_spriteanim_stop(rt_spriteanim anim) {
    anim = checked_spriteanim(anim, "SpriteAnimation.Stop: expected Zanna.Game.SpriteAnimation");
    if (!anim)
        return;
    anim->current_frame = anim->start_frame;
    anim->frame_counter = 0;
    anim->playing = 0;
    anim->paused = 0;
    anim->finished = 0;
    anim->direction = 1;
    anim->speed_accum = 0.0;
    anim->frame_changed = 0;
}

/// @brief Pause a playing animation so it can be resumed from the current frame.
/// @param anim Borrowed SpriteAnimation handle; stopped animations are
///        unchanged.
void rt_spriteanim_pause(rt_spriteanim anim) {
    anim = checked_spriteanim(anim, "SpriteAnimation.Pause: expected Zanna.Game.SpriteAnimation");
    if (!anim)
        return;
    if (anim->playing)
        anim->paused = 1;
}

/// @brief Resume a paused animation from the frame where it was paused.
/// @param anim Borrowed SpriteAnimation handle.
void rt_spriteanim_resume(rt_spriteanim anim) {
    anim = checked_spriteanim(anim, "SpriteAnimation.Resume: expected Zanna.Game.SpriteAnimation");
    if (!anim)
        return;
    anim->paused = 0;
}

/// @brief Reset the animation to its first frame without changing play/pause state.
/// @details Clears completion, counters, direction, and fractional-speed
///          accumulation.
/// @param anim Borrowed SpriteAnimation handle.
void rt_spriteanim_reset(rt_spriteanim anim) {
    anim = checked_spriteanim(anim, "SpriteAnimation.Reset: expected Zanna.Game.SpriteAnimation");
    if (!anim)
        return;
    anim->current_frame = anim->start_frame;
    anim->frame_counter = 0;
    anim->direction = 1;
    anim->finished = 0;
    anim->speed_accum = 0.0;
}

/// @brief Accumulate one speed-scaled tick and advance any due frame steps.
/// @details The call clears the frame-changed flag first. Stopped, paused, and
///          finished animations do not accumulate time. Fractional speed is
///          retained across calls, and speeds above one may produce multiple
///          ticks or frame steps in a single call.
/// @param anim Borrowed SpriteAnimation handle.
/// @return `1` only when a non-looping clip completes during this update;
///         otherwise `0`.
int8_t rt_spriteanim_update(rt_spriteanim anim) {
    anim = checked_spriteanim(anim, "SpriteAnimation.Update: expected Zanna.Game.SpriteAnimation");
    if (!anim)
        return 0;

    anim->frame_changed = 0;

    if (!anim->playing || anim->paused || anim->finished)
        return 0;

    // Apply speed multiplier
    if (!isfinite(anim->speed) || anim->speed < 0.0)
        anim->speed = 0.0;
    if (!isfinite(anim->speed_accum))
        anim->speed_accum = 0.0;
    anim->speed_accum += anim->speed;
    if (!isfinite(anim->speed_accum))
        anim->speed_accum = 0.0;
    while (anim->speed_accum >= 1.0) {
        anim->speed_accum -= 1.0;
        anim->frame_counter++;
    }

    while (anim->frame_counter >= anim->frame_duration && !anim->finished) {
        anim->frame_counter -= anim->frame_duration;
        anim->frame_changed = 1;
        if (rt_spriteanim_advance_one_frame(anim))
            return 1;
    }

    return 0;
}

/// @brief Return the current frame index within the sprite sheet.
/// @param anim Borrowed SpriteAnimation handle.
/// @return Current configured frame index, or `0` for null.
int64_t rt_spriteanim_frame(rt_spriteanim anim) {
    anim = checked_spriteanim(anim, "SpriteAnimation.Frame: expected Zanna.Game.SpriteAnimation");
    if (!anim)
        return 0;
    return anim->current_frame;
}

/// @brief Jump to a specific frame, clamped to [start_frame, end_frame].
/// @details Clears the whole-tick frame counter but leaves fractional speed,
///          play state, direction, and completion state unchanged.
/// @param anim Borrowed SpriteAnimation handle.
/// @param frame Requested sprite-sheet frame index.
void rt_spriteanim_set_frame(rt_spriteanim anim, int64_t frame) {
    anim =
        checked_spriteanim(anim, "SpriteAnimation.SetFrame: expected Zanna.Game.SpriteAnimation");
    if (!anim)
        return;
    if (frame < anim->start_frame)
        frame = anim->start_frame;
    if (frame > anim->end_frame)
        frame = anim->end_frame;
    anim->current_frame = frame;
    anim->frame_counter = 0;
}

/// @brief Return how many update ticks each frame is displayed before advancing.
/// @param anim Borrowed SpriteAnimation handle.
/// @return Positive configured duration, or `0` for null.
int64_t rt_spriteanim_frame_duration(rt_spriteanim anim) {
    anim = checked_spriteanim(anim,
                              "SpriteAnimation.FrameDuration: expected Zanna.Game.SpriteAnimation");
    if (!anim)
        return 0;
    return anim->frame_duration;
}

/// @brief Set how many update ticks each frame is displayed (minimum 1).
/// @param anim Borrowed SpriteAnimation handle.
/// @param duration Requested tick count, clamped to at least one.
void rt_spriteanim_set_frame_duration(rt_spriteanim anim, int64_t duration) {
    anim = checked_spriteanim(
        anim, "SpriteAnimation.SetFrameDuration: expected Zanna.Game.SpriteAnimation");
    if (!anim)
        return;
    if (duration < 1)
        duration = 1;
    anim->frame_duration = duration;
}

/// @brief Return the count of elements in the spriteanim.
/// @param anim Borrowed SpriteAnimation handle.
/// @return Inclusive range length, saturated at @ref INT64_MAX, or `0` for
///         null.
int64_t rt_spriteanim_frame_count(rt_spriteanim anim) {
    anim =
        checked_spriteanim(anim, "SpriteAnimation.FrameCount: expected Zanna.Game.SpriteAnimation");
    if (!anim)
        return 0;
    int64_t diff = anim->end_frame - anim->start_frame;
    if (diff == INT64_MAX)
        return INT64_MAX;
    return diff + 1;
}

/// @brief Check whether the animation is currently playing (not paused or stopped).
/// @param anim Borrowed SpriteAnimation handle.
/// @return `1` when playing and not paused; otherwise `0`.
int8_t rt_spriteanim_is_playing(rt_spriteanim anim) {
    anim =
        checked_spriteanim(anim, "SpriteAnimation.IsPlaying: expected Zanna.Game.SpriteAnimation");
    if (!anim)
        return 0;
    return anim->playing && !anim->paused;
}

/// @brief Check whether the animation is paused (can be resumed).
/// @param anim Borrowed SpriteAnimation handle.
/// @return `1` when paused; otherwise `0`.
int8_t rt_spriteanim_is_paused(rt_spriteanim anim) {
    anim =
        checked_spriteanim(anim, "SpriteAnimation.IsPaused: expected Zanna.Game.SpriteAnimation");
    if (!anim)
        return 0;
    return anim->paused;
}

/// @brief Check whether a non-looping animation has reached its last frame.
/// @param anim Borrowed SpriteAnimation handle.
/// @return `1` when one-shot playback has completed; otherwise `0`.
int8_t rt_spriteanim_is_finished(rt_spriteanim anim) {
    anim =
        checked_spriteanim(anim, "SpriteAnimation.IsFinished: expected Zanna.Game.SpriteAnimation");
    if (!anim)
        return 0;
    return anim->finished;
}

/// @brief Return the animation progress as a percentage (0-100).
/// @details Progress is based only on the current frame's forward position, so
///          it decreases during the reverse leg of ping-pong playback.
/// @param anim Borrowed SpriteAnimation handle.
/// @return Truncated percentage in 0..100; a single-frame clip reports 100.
int64_t rt_spriteanim_progress(rt_spriteanim anim) {
    anim =
        checked_spriteanim(anim, "SpriteAnimation.Progress: expected Zanna.Game.SpriteAnimation");
    if (!anim)
        return 0;
    int64_t total = anim->end_frame - anim->start_frame;
    if (total <= 0)
        return 100;
    int64_t current = anim->current_frame - anim->start_frame;
    return spriteanim_percent_i64(current, total);
}

/// @brief Set the playback speed multiplier, clamped to [0.0, 10.0] (1.0 = normal).
/// @param anim Borrowed SpriteAnimation handle.
/// @param speed Candidate multiplier; negative and non-finite values become
///        zero.
void rt_spriteanim_set_speed(rt_spriteanim anim, double speed) {
    anim =
        checked_spriteanim(anim, "SpriteAnimation.SetSpeed: expected Zanna.Game.SpriteAnimation");
    if (!anim)
        return;
    if (!isfinite(speed) || speed < 0.0)
        speed = 0.0;
    if (speed > 10.0)
        speed = 10.0;
    anim->speed = speed;
}

/// @brief Return the current playback speed multiplier.
/// @param anim Borrowed SpriteAnimation handle.
/// @return Configured multiplier, or `1.0` for null.
double rt_spriteanim_speed(rt_spriteanim anim) {
    anim = checked_spriteanim(anim, "SpriteAnimation.Speed: expected Zanna.Game.SpriteAnimation");
    if (!anim)
        return 1.0;
    return anim->speed;
}

/// @brief Check whether the last update attempted at least one frame step.
/// @details This is normally equivalent to an index change, but the terminal
///          step that completes a one-shot clip also sets the flag while
///          retaining its endpoint frame.
/// @param anim Borrowed SpriteAnimation handle.
/// @return `1` when a frame step was processed; otherwise `0`.
int8_t rt_spriteanim_frame_changed(rt_spriteanim anim) {
    anim = checked_spriteanim(anim,
                              "SpriteAnimation.FrameChanged: expected Zanna.Game.SpriteAnimation");
    if (!anim)
        return 0;
    return anim->frame_changed;
}
