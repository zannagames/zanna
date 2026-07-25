//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/game/rt_spriteanim.h
/// @file
/// @brief Declares a tick-based sprite-frame animation controller with loop,
///        ping-pong, one-shot, pause, and fractional-speed modes.
// Purpose: Frame-based sprite animation controller tracking frame index, timing, playback mode
// (loop/ping-pong), and speed for game sprite animations.
//
// Key invariants:
//   - Frame indices are bounded by the range set in rt_spriteanim_setup.
//   - Frame duration must be >= 1 tick.
//   - Speed multiplier scales how quickly the internal timer advances.
//   - rt_spriteanim_update takes no delta argument and advances its internal
//     timing once per call.
//
// Ownership/Lifetime:
//   - SpriteAnimation handles are reference-counted GC objects.
//     rt_spriteanim_destroy releases the caller's reference.
//
// Links: src/runtime/game/rt_spriteanim.c (implementation),
// src/runtime/game/rt_tween.h
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Runtime class identifier used to validate SpriteAnimation handles.
#define RT_SPRITEANIM_CLASS_ID INT64_C(-0x510219)

/// @brief Opaque handle to a SpriteAnimation instance.
typedef struct rt_spriteanim_impl *rt_spriteanim;

/// @brief Allocates and initializes a new SpriteAnimation in the stopped
///   state.
/// @details Defaults to frame zero, six update ticks per displayed frame,
///          looping enabled, forward direction, and speed 1.0.
/// @return Owned SpriteAnimation handle, or `NULL` if allocation fails.
rt_spriteanim rt_spriteanim_new(void);

/// @brief Release one owned SpriteAnimation reference.
/// @param anim Owned handle to release; `NULL` is ignored.
void rt_spriteanim_destroy(rt_spriteanim anim);

/// @brief Configures the animation's frame range and timing.
/// @details Resets position, whole-tick timing, direction, and completion
///          without changing play, pause, loop, ping-pong, or speed settings.
/// @param anim Borrowed animation handle.
/// @param start_frame First frame index; negative values clamp to zero.
/// @param end_frame Last frame index, inclusive; values below the normalized
///        start collapse the clip to one frame.
/// @param frame_duration Number of game frames to display each animation
///   frame before advancing; values below one clamp to one.
void rt_spriteanim_setup(rt_spriteanim anim,
                         int64_t start_frame,
                         int64_t end_frame,
                         int64_t frame_duration);

/// @brief Enables or disables looping playback.
/// @param anim Borrowed animation handle.
/// @param loop Zero for one-shot playback, nonzero to loop.
void rt_spriteanim_set_loop(rt_spriteanim anim, int8_t loop);

/// @brief Enables or disables ping-pong (palindrome) playback.
///
/// When enabled, the animation plays forward to the last frame, then
/// reverses back to the first frame, and repeats. Requires looping to be
/// enabled for continuous ping-pong.
/// @param anim Borrowed animation handle.
/// @param pingpong Zero for forward-only playback, nonzero for alternating
///        direction.
void rt_spriteanim_set_pingpong(rt_spriteanim anim, int8_t pingpong);

/// @brief Queries whether looping is enabled for this animation.
/// @param anim The animation to query.
/// @return 1 if the animation loops, 0 for one-shot playback.
int8_t rt_spriteanim_loop(rt_spriteanim anim);

/// @brief Queries whether ping-pong mode is enabled for this animation.
/// @param anim The animation to query.
/// @return 1 if ping-pong is active, 0 for normal forward playback.
int8_t rt_spriteanim_pingpong(rt_spriteanim anim);

/// @brief Starts or restarts the animation from the first frame.
///
/// Resets the internal timer and frame index to the beginning and enters
/// the playing state.
/// @param anim The animation to play.
void rt_spriteanim_play(rt_spriteanim anim);

/// @brief Stops the animation and resets it to the first frame.
///
/// Unlike pause, stop resets the playback position entirely.
/// @param anim The animation to stop.
void rt_spriteanim_stop(rt_spriteanim anim);

/// @brief Pauses the animation at its current frame without resetting.
///
/// Call rt_spriteanim_resume() to continue from where it was paused.
/// @param anim The animation to pause.
void rt_spriteanim_pause(rt_spriteanim anim);

/// @brief Resumes a previously paused animation from its current position.
/// @param anim The animation to resume. Has no effect if not paused.
void rt_spriteanim_resume(rt_spriteanim anim);

/// @brief Resets the animation to the first frame without changing the
///   play/pause/stop state.
/// @param anim The animation to reset.
void rt_spriteanim_reset(rt_spriteanim anim);

/// @brief Advances the animation by one game frame.
///
/// Should be called once per game frame while the animation is playing.
/// Adds the speed multiplier to a fractional tick accumulator and may process
/// multiple ticks or frame steps when the multiplier exceeds one.
/// @param anim The animation to update.
/// @return 1 if the animation just completed on this frame (relevant for
///   one-shot animations; always 0 for looping animations), 0 otherwise.
int8_t rt_spriteanim_update(rt_spriteanim anim);

/// @brief Retrieves the current sprite-sheet frame index.
/// @param anim The animation to query.
/// @return The frame index within [start_frame, end_frame] as configured
///   by rt_spriteanim_setup().
int64_t rt_spriteanim_frame(rt_spriteanim anim);

/// @brief Jumps to a specific frame index without affecting play state.
/// @details Clears the whole-tick frame counter but preserves fractional
///          speed, direction, and completion state.
/// @param anim The animation to modify.
/// @param frame The desired frame index. Clamped to [start_frame,
///   end_frame].
void rt_spriteanim_set_frame(rt_spriteanim anim, int64_t frame);

/// @brief Retrieves the number of game frames each animation frame is
///   displayed.
/// @param anim The animation to query.
/// @return The frame duration in game frames.
int64_t rt_spriteanim_frame_duration(rt_spriteanim anim);

/// @brief Changes the number of game frames each animation frame is
///   displayed.
/// @param anim The animation to modify.
/// @param duration New frame duration in game frames, clamped to at least one.
void rt_spriteanim_set_frame_duration(rt_spriteanim anim, int64_t duration);

/// @brief Retrieves the total number of frames in the animation sequence.
/// @param anim The animation to query.
/// @return The frame count (end_frame - start_frame + 1).
int64_t rt_spriteanim_frame_count(rt_spriteanim anim);

/// @brief Queries whether the animation is currently in the playing state.
/// @param anim The animation to query.
/// @return 1 if actively playing (not stopped or paused), 0 otherwise.
int8_t rt_spriteanim_is_playing(rt_spriteanim anim);

/// @brief Queries whether the animation is currently paused.
/// @param anim The animation to query.
/// @return 1 if paused (can be resumed), 0 otherwise.
int8_t rt_spriteanim_is_paused(rt_spriteanim anim);

/// @brief Queries whether a one-shot animation has reached its final frame.
/// @details A forward one-shot finishes at the end frame; a non-looping
///          ping-pong finishes after returning to the start frame.
/// @param anim The animation to query.
/// @return 1 if the animation has finished and will not advance further
///   (only meaningful for non-looping animations), 0 otherwise.
int8_t rt_spriteanim_is_finished(rt_spriteanim anim);

/// @brief Retrieves the animation progress as an integer percentage.
/// @details The value reflects forward position within the frame range and
///          therefore decreases during a ping-pong reverse leg.
/// @param anim The animation to query.
/// @return A value from 0 (just started) to 100 (reached last frame).
int64_t rt_spriteanim_progress(rt_spriteanim anim);

/// @brief Sets the playback speed multiplier.
/// @param anim The animation to modify.
/// @param speed Speed multiplier applied to frame advancement. 1.0 is
///   normal speed, 2.0 is double speed, and 0.5 is half speed. Values are
///   clamped to [0, 10]; non-finite and negative values become zero.
void rt_spriteanim_set_speed(rt_spriteanim anim, double speed);

/// @brief Retrieves the current playback speed multiplier.
/// @param anim The animation to query.
/// @return The speed multiplier (1.0 = normal).
double rt_spriteanim_speed(rt_spriteanim anim);

/// @brief Queries whether the most recent update processed a frame step.
///
/// Useful for triggering game events (e.g., sound effects, hitbox changes)
/// synchronized with animation timing. Usually the displayed index changes,
/// but the terminal one-shot step also reports true while retaining its
/// endpoint frame.
/// @param anim The animation to query.
/// @return `1` if a frame step was attempted during the last update;
///         otherwise `0`.
int8_t rt_spriteanim_frame_changed(rt_spriteanim anim);

#ifdef __cplusplus
}
#endif
