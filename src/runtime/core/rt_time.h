//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/core/rt_time.h
// Purpose: Declares portable sleep, millisecond/microsecond elapsed clocks, the
// Zanna.Time.Clock wrappers, and shared POSIX fallback ratcheting.
//
// Key invariants:
//   - Clock values prefer a monotonic source; POSIX uses realtime as a failure
//     fallback, ratcheted through a process-local floor so it stays
//     non-decreasing, and all-source failure returns 0 (VDOC-223).
//   - Negative sleep durations are clamped to 0.
//   - The 64-bit Clock sleep wrapper also clamps above INT32_MAX.
//   - All functions are thread-safe. Realtime fallback paths use caller-owned
//     atomic floors; preferred monotonic paths retain no runtime state.
//
// Ownership/Lifetime:
//   - All APIs operate on scalar values only and allocate no heap state.
//
// Links: src/runtime/core/rt_time.c (implementation),
//        include/zanna/runtime/rt.h (umbrella public header)
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Portable runtime sleep, elapsed-clock, and fallback-ratchet API.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Sleep for approximately @p ms milliseconds.
/// @details Uses Win32 `Sleep` or POSIX `nanosleep`. POSIX retries with the
///          remaining duration after `EINTR`; other errors end the wait.
///          Scheduler and platform timer resolution may extend the actual
///          elapsed duration.
/// @param ms Milliseconds to sleep; negative values become zero.
void rt_sleep_ms(int32_t ms);

/// @brief Return milliseconds from the best available elapsed-time clock.
/// @details Windows prefers QueryPerformanceCounter and falls back to
///          GetTickCount64. POSIX prefers CLOCK_MONOTONIC and falls back to a
///          process-local ratcheted CLOCK_REALTIME reading.
/// @return Milliseconds from an unspecified epoch, or zero if all queries fail
///         or after a returning signed-overflow trap.
int64_t rt_timer_ms(void);

/// @brief Zanna.Time.Clock.Sleep entry point.
/// @details Clamps to `[0, INT32_MAX]` before delegating to
///          @ref rt_sleep_ms.
/// @param ms Requested milliseconds.
void rt_clock_sleep(int64_t ms);

/// @brief Zanna.Time.Clock.Ticks entry point.
/// @details Exact 64-bit ABI wrapper around @ref rt_timer_ms.
/// @return Millisecond tick count with that function's fallback/error behavior.
int64_t rt_clock_ticks(void);

/// @brief Zanna.Time.Clock.TicksUs entry point.
/// @details Uses the same preferred/fallback platform clocks as
///          @ref rt_timer_ms but scales to microseconds with checked arithmetic
///          and an independent POSIX realtime floor.
/// @return Microseconds from an unspecified epoch, or zero if all queries fail
///         or after a returning signed-overflow trap.
int64_t rt_clock_ticks_us(void);

/// @brief Clamp a fallback clock reading up to a process-local monotonic floor.
/// @details Internal helper shared by the Clock, Stopwatch, and Countdown POSIX
/// CLOCK_REALTIME fallbacks. A lock-free CAS-max keeps the returned sequence
/// non-decreasing even if wall-clock time is adjusted backward (VDOC-223). Not a
/// registered runtime surface symbol.
/// @param floor Caller-owned process-local floor for one scale/call site.
/// @param candidate Freshly sampled fallback value.
/// @return The greater of @p candidate and the retained floor.
/// @pre @p floor is non-null, atomically aligned, and remains live during all
///      concurrent calls.
int64_t rt_time_monotonic_ratchet(int64_t *floor, int64_t candidate);

#ifdef __cplusplus
}
#endif
