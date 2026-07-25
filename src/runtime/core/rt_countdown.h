//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/core/rt_countdown.h
/// @file
/// @brief Declares the GC-managed monotonic Countdown timer API.
///
// Purpose: Countdown timer providing interval-based expiration detection with start, stop, reset,
// and query operations, suitable for game loops and timed events.
//
// Key invariants:
//   - Remaining time is clamped to zero; it never goes negative.
//   - Elapsed time accumulates only while the timer is running.
//   - All times are measured in milliseconds.
//   - Expired is true when elapsed >= interval; the timer continues running after expiry.
//   - Instance methods trap when passed a NULL countdown pointer.
//
// Ownership/Lifetime:
//   - Countdown objects are heap-allocated runtime objects managed through Zanna's
//     reference-counting/GC lifetime; source callers do not free them explicitly.
//
// Links: src/runtime/core/rt_countdown.c (implementation)
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Runtime class ID for Countdown instances.
/// @details Stamped by rt_obj_new_i64 at construction and verified by the shared
///          receiver guard so an explicit receiver of another class traps instead
///          of being reinterpreted as a Countdown payload (VDOC-229).
#define RT_COUNTDOWN_CLASS_ID INT64_C(-0x430801)

/// @brief Create a new countdown timer with specified interval.
/// @param interval_ms Interval duration in milliseconds; nonpositive becomes zero.
/// @return Pointer to new countdown object (stopped, elapsed = 0).
/// @note A zero interval is expired immediately, even before it is started.
void *rt_countdown_new(int64_t interval_ms);

/// @brief Start or resume the countdown timer.
/// @param obj Countdown pointer.
/// @details Has no effect if already running.
void rt_countdown_start(void *obj);

/// @brief Stop/pause the countdown timer.
/// @param obj Countdown pointer.
/// @details Preserves elapsed time. Has no effect if already stopped.
void rt_countdown_stop(void *obj);

/// @brief Reset the countdown timer to zero elapsed time.
/// @param obj Countdown pointer.
/// @details Stops the timer and resets elapsed to 0.
/// @note A retained zero interval remains immediately expired after reset.
void rt_countdown_reset(void *obj);

/// @brief Get elapsed time in milliseconds.
/// @param obj Countdown pointer.
/// @return Total elapsed milliseconds since start/last reset.
/// @note Includes completed start/stop intervals and the current running interval.
int64_t rt_countdown_elapsed(void *obj);

/// @brief Get remaining time in milliseconds.
/// @param obj Countdown pointer.
/// @return max(0, interval - elapsed) in milliseconds.
int64_t rt_countdown_remaining(void *obj);

/// @brief Check if the countdown has expired.
/// @param obj Countdown pointer.
/// @return True if elapsed >= interval.
int8_t rt_countdown_expired(void *obj);

/// @brief Get the interval duration in milliseconds.
/// @param obj Countdown pointer.
/// @return Interval duration in milliseconds.
int64_t rt_countdown_interval(void *obj);

/// @brief Set a new interval duration.
/// @param obj Countdown pointer.
/// @param interval_ms New interval duration; nonpositive becomes zero.
/// @details Does not reset elapsed time.
void rt_countdown_set_interval(void *obj, int64_t interval_ms);

/// @brief Check if the countdown timer is currently running.
/// @param obj Countdown pointer.
/// @return Non-zero if running, zero if stopped.
int8_t rt_countdown_is_running(void *obj);

/// @brief Wait (block) until the countdown expires.
/// @param obj Countdown pointer.
/// @details If already expired, returns immediately. Starts timer if not running.
/// Long waits are slept in chunks until the timer actually expires.
/// @note Blocks the calling thread and leaves a newly started timer running.
void rt_countdown_wait(void *obj);

#ifdef __cplusplus
}
#endif
