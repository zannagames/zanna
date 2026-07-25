//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/game/rt_timer.c
/// @file
/// @brief Implements deterministic frame-counted and delta-millisecond
///        countdown timers with expiration edges and completion latches.
// Purpose: Frame-counted countdown timer for Zanna games. A Timer fires after a
//   specified number of game frames and optionally repeats automatically.
//   Frame-based timing is deterministic (independent of wall-clock drift) and
//   integrates naturally with game loops that call Update() exactly once per
//   rendered frame. Typical uses: cooldowns, enemy respawns, animation delays,
//   and periodic events.
//
// Key invariants:
//   - Duration and elapsed are both integer frame counts. Duration must be > 0;
//     zero or negative durations are silently rejected by Start/StartRepeating.
//   - rt_timer_update() must be called once per frame while the timer is
//     running. It returns 1 on the frame the timer expires, 0 otherwise. For a
//     repeating timer, it fires every `duration` frames and resets elapsed to 0
//     on expiry (never stops automatically).
//   - rt_timer_is_expired() returns 1 only if the timer ran to completion and
//     is no longer running. It returns 0 for a timer that was stopped early.
//   - rt_timer_progress() returns [0, 100] as an integer percentage of elapsed
//     frames. At 0 frames elapsed it is 0; at or beyond duration it is 100.
//   - rt_timer_remaining() returns the number of frames left until expiry, or 0
//     if already expired or not started.
//
// Ownership/Lifetime:
//   - Timer objects are GC-managed (rt_obj_new_i64). rt_timer_destroy() calls
//     rt_obj_free() explicitly; the GC also reclaims them automatically.
//
// Links: src/runtime/game/rt_timer.h (public API),
//        docs/zannalib/game/core.md (frame- and millisecond-driven modes)
//
//===----------------------------------------------------------------------===//

#include "rt_timer.h"
#include "rt_object.h"
#include "rt_trap.h"
#include <limits.h>
#include <stdlib.h>

/// @brief Mutable state owned by a Timer runtime object.
struct rt_timer_impl {
    int64_t duration; // Total frames (or ms in ms_mode) for the timer
    int64_t elapsed;  // Frames (or ms) elapsed since start
    int8_t running;   // 1 if timer is running
    int8_t repeating; // 1 if timer auto-restarts
    int8_t ms_mode;   // 1 if using millisecond-based timing
    int8_t expired;   // 1 if a one-shot timer completed
};

/// @brief Safe-cast a handle to the Timer impl, trapping @p api on a class-id
///        mismatch.
/// @param timer Borrowed candidate Timer handle.
/// @param api Trap message identifying the calling API.
/// @return Borrowed implementation pointer, or `NULL` when @p timer is `NULL`.
static rt_timer checked_timer(rt_timer timer, const char *api) {
    if (!timer)
        return NULL;
    if (rt_obj_class_id(timer) != RT_TIMER_CLASS_ID) {
        rt_trap(api);
        return NULL;
    }
    return timer;
}

/// @brief Saturating int64 addition (clamps to INT64_MIN/MAX on overflow).
/// @param a First addend.
/// @param b Second addend.
/// @return Exact sum when representable, otherwise the matching signed bound.
static int64_t timer_add_sat_i64(int64_t a, int64_t b) {
    if (b > 0 && a > INT64_MAX - b)
        return INT64_MAX;
    if (b < 0 && a < INT64_MIN - b)
        return INT64_MIN;
    return a + b;
}

/// @brief Integer percentage value*100/total, clamped to [0, 100]; 0 for
///        non-positive inputs.
/// @param value Non-negative elapsed amount.
/// @param total Positive duration.
/// @return Truncated percentage in the inclusive range 0..100.
static int64_t timer_percent_i64(int64_t value, int64_t total) {
    if (value <= 0 || total <= 0)
        return 0;
    long double scaled = ((long double)value * 100.0L) / (long double)total;
    int64_t pct = scaled >= (long double)INT64_MAX ? INT64_MAX : (int64_t)scaled;
    return pct > 100 ? 100 : pct;
}

/// @brief Create a new timer (starts stopped with zero duration).
/// @return Owned Timer handle, or `NULL` if allocation fails.
rt_timer rt_timer_new(void) {
    struct rt_timer_impl *timer = (struct rt_timer_impl *)rt_obj_new_i64(
        RT_TIMER_CLASS_ID, (int64_t)sizeof(struct rt_timer_impl));
    if (!timer) {
        return NULL;
    }

    timer->duration = 0;
    timer->elapsed = 0;
    timer->running = 0;
    timer->repeating = 0;
    timer->ms_mode = 0;
    timer->expired = 0;

    return timer;
}

/// @brief Release one owned Timer reference.
/// @param timer Owned handle to release; `NULL` is ignored.
void rt_timer_destroy(rt_timer timer) {
    timer = checked_timer(timer, "Timer.Destroy: expected Zanna.Game.Timer");
    if (timer && rt_obj_release_check0(timer))
        rt_obj_free(timer);
}

/// @brief Start a one-shot timer that expires after the given number of frames.
/// @details Restarts the timer in frame mode and clears prior repetition and
///          completion state. Nonpositive durations are ignored.
/// @param timer Borrowed Timer handle.
/// @param frames Positive frame duration.
void rt_timer_start(rt_timer timer, int64_t frames) {
    timer = checked_timer(timer, "Timer.Start: expected Zanna.Game.Timer");
    if (!timer || frames <= 0)
        return;

    timer->duration = frames;
    timer->elapsed = 0;
    timer->running = 1;
    timer->repeating = 0;
    timer->ms_mode = 0;
    timer->expired = 0;
}

/// @brief Start a repeating timer that auto-restarts when it expires.
/// @details Restarts in frame mode and clears prior completion state.
/// @param timer Borrowed Timer handle.
/// @param frames Positive frames per cycle.
void rt_timer_start_repeating(rt_timer timer, int64_t frames) {
    timer = checked_timer(timer, "Timer.StartRepeating: expected Zanna.Game.Timer");
    if (!timer || frames <= 0)
        return;

    timer->duration = frames;
    timer->elapsed = 0;
    timer->running = 1;
    timer->repeating = 1;
    timer->ms_mode = 0;
    timer->expired = 0;
}

/// @brief Stop the timer (elapsed value is preserved for queries).
/// @details Clears the one-shot expiration latch without changing duration,
///          elapsed amount, repetition, or timing mode.
/// @param timer Borrowed Timer handle.
void rt_timer_stop(rt_timer timer) {
    timer = checked_timer(timer, "Timer.Stop: expected Zanna.Game.Timer");
    if (!timer)
        return;
    timer->running = 0;
    timer->expired = 0;
}

/// @brief Reset the elapsed counter to zero without changing running/repeating state.
/// @details Also clears the expiration latch while preserving duration and
///          frame/millisecond mode.
/// @param timer Borrowed Timer handle.
void rt_timer_reset(rt_timer timer) {
    timer = checked_timer(timer, "Timer.Reset: expected Zanna.Game.Timer");
    if (!timer)
        return;
    timer->elapsed = 0;
    timer->expired = 0;
}

/// @brief Advance the timer by one tick. Returns 1 if the timer expired this tick.
/// @details Only frame-mode running timers advance. A repeating timer resets
///          elapsed to zero; a one-shot stops and latches expiration.
/// @param timer Borrowed Timer handle.
/// @return `1` on a frame-mode cycle boundary; otherwise `0`.
int8_t rt_timer_update(rt_timer timer) {
    timer = checked_timer(timer, "Timer.Update: expected Zanna.Game.Timer");
    if (!timer || !timer->running) {
        return 0;
    }
    // Enforce the timer's mode: the frame-based Update() is a no-op on a timer
    // started in millisecond mode, so a mismatched call cannot silently advance a
    // millisecond timer by one "frame" and reinterpret its units (VDOC-264).
    if (timer->ms_mode) {
        return 0;
    }

    timer->elapsed = timer_add_sat_i64(timer->elapsed, 1);

    if (timer->elapsed >= timer->duration) {
        if (timer->repeating) {
            // Wrap around for repeating timers
            timer->elapsed = 0;
        } else {
            timer->running = 0;
            timer->expired = 1;
        }
        return 1; // Timer expired this frame
    }

    return 0;
}

/// @brief Check whether the timer is currently counting.
/// @param timer Borrowed Timer handle.
/// @return `1` when running in either mode; otherwise `0`.
int8_t rt_timer_is_running(rt_timer timer) {
    timer = checked_timer(timer, "Timer.IsRunning: expected Zanna.Game.Timer");
    return timer ? timer->running : 0;
}

/// @brief Check the one-shot completion latch.
/// @param timer Borrowed Timer handle.
/// @return `1` only after a one-shot reaches its duration and before stop,
///         reset, or restart clears the latch.
int8_t rt_timer_is_expired(rt_timer timer) {
    timer = checked_timer(timer, "Timer.IsExpired: expected Zanna.Game.Timer");
    if (!timer)
        return 0;
    return timer->expired ? 1 : 0;
}

/// @brief Get the elapsed amount in the timer's active unit.
/// @param timer Borrowed Timer handle.
/// @return Frames in frame mode or milliseconds in ms mode; `0` for null.
int64_t rt_timer_elapsed(rt_timer timer) {
    timer = checked_timer(timer, "Timer.Elapsed: expected Zanna.Game.Timer");
    return timer ? timer->elapsed : 0;
}

/// @brief Get the remaining amount in the timer's active unit.
/// @param timer Borrowed Timer handle.
/// @return Non-negative frames or milliseconds, or `0` for no duration.
int64_t rt_timer_remaining(rt_timer timer) {
    timer = checked_timer(timer, "Timer.Remaining: expected Zanna.Game.Timer");
    if (!timer || timer->duration == 0)
        return 0;

    int64_t remaining = timer->duration - timer->elapsed;
    return (remaining > 0) ? remaining : 0;
}

/// @brief Get the timer progress as a percentage (0–100).
/// @param timer Borrowed Timer handle.
/// @return Truncated elapsed/duration percentage, or `0` for no duration.
int64_t rt_timer_progress(rt_timer timer) {
    timer = checked_timer(timer, "Timer.Progress: expected Zanna.Game.Timer");
    if (!timer || timer->duration == 0)
        return 0;

    return timer_percent_i64(timer->elapsed, timer->duration);
}

/// @brief Get the total duration the timer was started with.
/// @param timer Borrowed Timer handle.
/// @return Frames in frame mode or milliseconds in ms mode; `0` for null.
int64_t rt_timer_duration(rt_timer timer) {
    timer = checked_timer(timer, "Timer.Duration: expected Zanna.Game.Timer");
    return timer ? timer->duration : 0;
}

/// @brief Check whether the timer is in repeating (auto-restart) mode.
/// @param timer Borrowed Timer handle.
/// @return `1` when configured to repeat; otherwise `0`.
int8_t rt_timer_is_repeating(rt_timer timer) {
    timer = checked_timer(timer, "Timer.IsRepeating: expected Zanna.Game.Timer");
    return timer ? timer->repeating : 0;
}

/// @brief Report whether the timer is in millisecond mode (VDOC-264).
/// @details Lets callers inspect the active update/query contract: a millisecond
///          timer is advanced with `UpdateMs`/read with `ElapsedMs`, a frame timer
///          with `Update`/`Elapsed`. The cross-mode update calls are no-ops.
/// @param timer Borrowed Timer handle.
/// @return 1 when the timer was started in millisecond mode, 0 for frame mode.
int8_t rt_timer_is_ms(rt_timer timer) {
    timer = checked_timer(timer, "Timer.IsMs: expected Zanna.Game.Timer");
    return timer ? timer->ms_mode : 0;
}

/// @brief Set the duration value.
/// @details Uses the timer's current unit, preserves elapsed/running/mode
///          state, and clears expiration only when elapsed is below the new
///          positive duration.
/// @param timer Borrowed Timer handle.
/// @param frames Positive duration in frames or milliseconds according to mode.
void rt_timer_set_duration(rt_timer timer, int64_t frames) {
    timer = checked_timer(timer, "Timer.Duration.set: expected Zanna.Game.Timer");
    if (!timer || frames <= 0)
        return;
    timer->duration = frames;
    if (timer->elapsed < timer->duration)
        timer->expired = 0;
}

// =========================================================================
// Millisecond-based timer mode
// =========================================================================

/// @brief Start a one-shot millisecond-based timer (drive with `_update_ms(dt_ms)`).
/// Switches the timer into ms mode; subsequent frame-based ops should use the ms variants.
/// @param timer Borrowed Timer handle.
/// @param duration_ms Positive duration in milliseconds.
void rt_timer_start_ms(rt_timer timer, int64_t duration_ms) {
    timer = checked_timer(timer, "Timer.StartMs: expected Zanna.Game.Timer");
    if (!timer || duration_ms <= 0)
        return;
    timer->duration = duration_ms;
    timer->elapsed = 0;
    timer->running = 1;
    timer->repeating = 0;
    timer->ms_mode = 1;
    timer->expired = 0;
}

/// @brief Start a repeating millisecond-based timer that fires every @p interval_ms.
/// @param timer Borrowed Timer handle.
/// @param interval_ms Positive cycle duration in milliseconds.
void rt_timer_start_repeating_ms(rt_timer timer, int64_t interval_ms) {
    timer = checked_timer(timer, "Timer.StartRepeatingMs: expected Zanna.Game.Timer");
    if (!timer || interval_ms <= 0)
        return;
    timer->duration = interval_ms;
    timer->elapsed = 0;
    timer->running = 1;
    timer->repeating = 1;
    timer->ms_mode = 1;
    timer->expired = 0;
}

/// @brief Advance an ms-mode timer by @p dt milliseconds. Returns 1 on the tick it expires.
/// Repeating timers preserve overshoot modulo the duration for timing accuracy.
/// @details The implementation preserves overshoot with modulo, so one call
///          reports a single edge even when @p dt spans multiple cycles.
/// @param timer Borrowed Timer handle.
/// @param dt Positive elapsed milliseconds.
/// @return `1` when at least one ms-mode cycle boundary is reached; otherwise
///         `0`.
int8_t rt_timer_update_ms(rt_timer timer, int64_t dt) {
    timer = checked_timer(timer, "Timer.UpdateMs: expected Zanna.Game.Timer");
    if (!timer || !timer->running || dt <= 0)
        return 0;
    // Enforce the timer's mode: the millisecond-based UpdateMs() is a no-op on a
    // timer started in frame mode, so a mismatched call cannot advance a frame
    // timer by `dt` "frames" and reinterpret its units (VDOC-264).
    if (!timer->ms_mode)
        return 0;

    timer->elapsed = timer_add_sat_i64(timer->elapsed, dt);

    if (timer->elapsed >= timer->duration) {
        if (timer->repeating) {
            // Wrap around, preserving overshoot for accuracy
            timer->elapsed %= timer->duration;
            if (timer->elapsed < 0)
                timer->elapsed = 0;
        } else {
            timer->elapsed = timer->duration;
            timer->running = 0;
            timer->expired = 1;
        }
        return 1; // Timer expired this update
    }

    return 0;
}

/// @brief Read the elapsed milliseconds since the timer started.
/// @details This aliases the shared elapsed field and therefore returns frame
///          ticks if called while the timer is in frame mode.
/// @param timer Borrowed Timer handle.
/// @return Stored elapsed amount, or `0` for null.
int64_t rt_timer_elapsed_ms(rt_timer timer) {
    timer = checked_timer(timer, "Timer.ElapsedMs: expected Zanna.Game.Timer");
    return timer ? timer->elapsed : 0;
}

/// @brief Read the milliseconds remaining before an ms-mode timer expires (0 if past).
/// @details This aliases the shared duration/elapsed fields and therefore uses
///          frame units if called while the timer is in frame mode.
/// @param timer Borrowed Timer handle.
/// @return Non-negative stored remainder, or `0` for no duration.
int64_t rt_timer_remaining_ms(rt_timer timer) {
    timer = checked_timer(timer, "Timer.RemainingMs: expected Zanna.Game.Timer");
    if (!timer || timer->duration == 0)
        return 0;
    int64_t remaining = timer->duration - timer->elapsed;
    return (remaining > 0) ? remaining : 0;
}
