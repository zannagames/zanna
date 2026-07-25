//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/game/rt_timer.h
/// @file
/// @brief Declares frame-counted and delta-millisecond countdown timer APIs.
// Purpose: Deterministic frame- or millisecond-based countdown timing with
// update-time expiration edges, completion state, and integer progress.
//
// Key invariants:
//   - Duration uses frames in frame mode and milliseconds in ms mode; it must be >= 1.
//   - Frame and millisecond update calls are no-ops on the opposite mode.
//   - Update returns a one-shot expiration edge on the expiring frame.
//   - IsExpired is a completion latch for one-shot timers, cleared by restart,
//     reset, or stop.
//   - Progress is reported as an integer percentage in [0, 100].
//
// Ownership/Lifetime:
//   - Timer handles are runtime reference-counted objects.
//   - rt_timer_destroy releases the caller's owned reference.
//
// Links: src/runtime/game/rt_timer.c (implementation)
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Runtime class identifier used to validate Timer handles.
#define RT_TIMER_CLASS_ID INT64_C(-0x510201)

/// @brief Opaque handle to a Timer instance.
typedef struct rt_timer_impl *rt_timer;

/// @brief Allocates and initializes a new Timer in the stopped state.
/// @return Owned Timer handle with zero duration, or `NULL` if allocation
///         fails.
rt_timer rt_timer_new(void);

/// @brief Release one owned Timer reference.
/// @param timer Owned handle to release; `NULL` is ignored.
void rt_timer_destroy(rt_timer timer);

/// @brief Starts a one-shot countdown timer.
///
/// The timer counts down from the given duration and expires once.
/// Calling this on a running timer restarts it with the new duration.
/// @param timer The timer to start.
/// @param frames Number of frames until expiration. Must be >= 1.
void rt_timer_start(rt_timer timer, int64_t frames);

/// @brief Starts a repeating countdown timer that auto-restarts on
///   expiration.
///
/// Each time the timer's countdown reaches zero, it fires (update returns
/// 1) and immediately restarts for another cycle.
/// @param timer The timer to start.
/// @param frames Number of frames per cycle. Must be >= 1.
void rt_timer_start_repeating(rt_timer timer, int64_t frames);

/// @brief Stop the timer while preserving elapsed time for queries.
/// @details Clears the one-shot expiration latch but leaves duration,
///          repetition, and timing mode unchanged.
/// @param timer The timer to stop.
void rt_timer_stop(rt_timer timer);

/// @brief Resets the timer's elapsed count to zero without changing its
///   running/stopped state or duration.
///
/// If the timer is running, it effectively restarts the current countdown
/// from the beginning.
/// @param timer The timer to reset.
void rt_timer_reset(rt_timer timer);

/// @brief Advances the timer by one frame and checks for expiration.
///
/// Must be called exactly once per game frame while the timer is running.
/// For repeating timers, automatically restarts the countdown on each
/// expiration.
/// @param timer The timer to update.
/// @return 1 if the timer expired on this frame, 0 otherwise. For
///   repeating timers, returns 1 on each cycle completion.
int8_t rt_timer_update(rt_timer timer);

/// @brief Queries whether the timer is currently counting down.
/// @param timer The timer to query.
/// @return 1 if the timer is running (started and not yet stopped),
///   0 if stopped.
int8_t rt_timer_is_running(rt_timer timer);

/// @brief Queries whether the timer completed a one-shot countdown.
/// @param timer The timer to query.
/// @return 1 only after a one-shot timer ran to completion. Timers that were
///   never started, stopped early, reset, restarted, or repeating return 0.
int8_t rt_timer_is_expired(rt_timer timer);

/// @brief Retrieves the number of frames elapsed since the timer was
///   started or last reset.
/// @details The shared query returns milliseconds when the timer is in ms mode.
/// @param timer The timer to query.
/// @return Stored elapsed amount in the active unit, or `0` for null.
int64_t rt_timer_elapsed(rt_timer timer);

/// @brief Retrieves the number of frames remaining before expiration.
/// @details The shared query returns milliseconds when the timer is in ms mode.
/// @param timer The timer to query.
/// @return Non-negative remainder in the active unit, or `0` for no duration.
int64_t rt_timer_remaining(rt_timer timer);

/// @brief Retrieves the timer's progress as an integer percentage.
/// @param timer The timer to query.
/// @return A value from 0 (just started) to 100 (fully elapsed / expired).
int64_t rt_timer_progress(rt_timer timer);

/// @brief Retrieves the total duration the timer was configured with.
/// @param timer The timer to query.
/// @return Duration in frames or milliseconds according to the active mode.
int64_t rt_timer_duration(rt_timer timer);

/// @brief Queries whether the timer is set to repeat automatically.
/// @param timer The timer to query.
/// @return 1 if the timer was started with rt_timer_start_repeating(),
///   0 if it is a one-shot timer.
int8_t rt_timer_is_repeating(rt_timer timer);

/// @brief Report whether the timer is in millisecond mode (1) or frame mode (0).
/// @param timer The timer to query.
/// @return 1 when started with rt_timer_start_ms/_repeating_ms, 0 for frame mode.
int8_t rt_timer_is_ms(rt_timer timer);

/// @brief Changes the timer's duration without restarting or stopping it.
///
/// The new duration takes effect on the current or next countdown cycle.
/// If the elapsed time already exceeds the new duration, the timer will
/// expire on the next update.
/// @param timer The timer to modify.
/// @param frames New positive duration in the timer's current unit.
void rt_timer_set_duration(rt_timer timer, int64_t frames);

// =========================================================================
// Millisecond-based timer mode
// =========================================================================
// These methods use milliseconds instead of frame counts, enabling
// delta-time-independent timing for variable-framerate game loops.
// A timer started with StartMs must be updated with UpdateMs (not Update).

/// @brief Starts a one-shot ms-based countdown timer.
/// @param timer The timer to start.
/// @param duration_ms Duration in milliseconds. Must be >= 1.
void rt_timer_start_ms(rt_timer timer, int64_t duration_ms);

/// @brief Starts a repeating ms-based countdown timer.
/// @param timer The timer to start.
/// @param interval_ms Interval in milliseconds. Must be >= 1.
void rt_timer_start_repeating_ms(rt_timer timer, int64_t interval_ms);

/// @brief Advances the timer by dt milliseconds and checks for expiration.
/// @details Repeating timers retain overshoot modulo the interval. One call
///          returns a single edge even if @p dt spans multiple cycles.
/// @param timer The timer to update.
/// @param dt Positive elapsed milliseconds.
/// @return 1 if the timer expired this update, 0 otherwise.
int8_t rt_timer_update_ms(rt_timer timer, int64_t dt);

/// @brief Retrieve the shared elapsed amount through the ms-named API.
/// @param timer The timer to query.
/// @return Elapsed milliseconds in ms mode, or elapsed frames in frame mode.
int64_t rt_timer_elapsed_ms(rt_timer timer);

/// @brief Retrieve the shared remaining amount through the ms-named API.
/// @param timer The timer to query.
/// @return Remaining milliseconds in ms mode or frames in frame mode; zero
///         when elapsed/no duration.
int64_t rt_timer_remaining_ms(rt_timer timer);

#ifdef __cplusplus
}
#endif
