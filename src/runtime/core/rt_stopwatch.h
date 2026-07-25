//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/core/rt_stopwatch.h
// Purpose: High-precision stopwatch for benchmarking and performance measurement, supporting
// start/stop/reset operations and elapsed-time queries in nanoseconds, microseconds, and
// milliseconds.
//
// Key invariants:
//   - Accumulated time normally uses a monotonic source. POSIX realtime
//     fallback readings are individually ratcheted non-decreasing, and Windows
//     falls back from QPC to GetTickCount64.
//   - Primary and fallback epochs are not normalized; an interval spanning a
//     source switch may jump or become negative.
//   - Nanosecond resolution is used where the platform clock permits.
//   - Elapsed queries while running include time since the last start call.
//   - rt_stopwatch_restart resets elapsed to zero and immediately starts timing.
//   - Receiver methods validate runtime kind, class, and payload size and assume
//     invalid-receiver traps do not return.
//   - Stopwatch instance state is not internally synchronized.
//
// Ownership/Lifetime:
//   - Stopwatch objects are heap-allocated runtime objects managed through Zanna's
//     reference-counting/GC lifetime; source callers do not free them explicitly.
//
// Links: src/runtime/core/rt_stopwatch.c (implementation)
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Runtime-managed elapsed-time Stopwatch API.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Runtime class ID for Stopwatch instances.
/// @details Stamped by rt_obj_new_i64 at construction and verified by the shared
///          receiver guard so an explicit receiver of another class traps instead
///          of being reinterpreted as a Stopwatch payload (VDOC-229).
#define RT_STOPWATCH_CLASS_ID INT64_C(-0x430802)

/// @brief Create a new stopped stopwatch.
/// @return New runtime-managed stopwatch, or `NULL` after a returning
///         allocation-failure trap.
void *rt_stopwatch_new(void);

/// @brief Create and immediately start a new stopwatch.
/// @return New runtime-managed running stopwatch.
/// @warning This convenience path assumes allocation and receiver traps do not
///          return.
void *rt_stopwatch_start_new(void);

/// @brief Start or resume the stopwatch.
/// @param obj Valid Stopwatch handle.
/// @details Has no effect if already running. Otherwise records the best
///          available platform timestamp and marks the instance running.
/// @warning The instance is not thread-safe, and invalid-receiver traps are
///          expected not to return.
void rt_stopwatch_start(void *obj);

/// @brief Stop/pause the stopwatch.
/// @param obj Valid Stopwatch handle.
/// @details Has no effect if stopped. Otherwise snapshots one interval, adds it
///          with signed-overflow checks, and marks the instance stopped.
///          Overflow in interval subtraction or accumulation traps without
///          completing the stop transition.
/// @warning The instance is not thread-safe, and invalid-receiver traps are
///          expected not to return.
void rt_stopwatch_stop(void *obj);

/// @brief Reset the stopwatch to zero and stop it.
/// @param obj Valid Stopwatch handle.
/// @warning The instance is not thread-safe, and invalid-receiver traps are
///          expected not to return.
void rt_stopwatch_reset(void *obj);

/// @brief Reset and immediately start the stopwatch.
/// @param obj Valid Stopwatch handle.
/// @details Clears accumulation, records one timestamp, and marks the instance
///          running. It is semantically equivalent to Reset followed by Start
///          but not atomic with respect to unsynchronized readers.
/// @warning Invalid-receiver traps are expected not to return.
void rt_stopwatch_restart(void *obj);

/// @brief Get elapsed time in nanoseconds.
/// @param obj Valid Stopwatch handle.
/// @return Completed accumulation plus the current interval when running.
///         Mixed clock sources can make the value negative. Signed overflow
///         traps.
/// @warning Invalid-receiver traps are expected not to return.
int64_t rt_stopwatch_elapsed_ns(void *obj);

/// @brief Get elapsed time in microseconds.
/// @param obj Valid Stopwatch handle.
/// @return Elapsed nanoseconds divided by 1,000 with truncation toward zero.
///         Overflow is checked before unit conversion.
/// @warning Invalid-receiver traps are expected not to return.
int64_t rt_stopwatch_elapsed_us(void *obj);

/// @brief Get elapsed time in milliseconds.
/// @param obj Valid Stopwatch handle.
/// @return Elapsed nanoseconds divided by 1,000,000 with truncation toward
///         zero. Overflow is checked before unit conversion.
/// @warning Invalid-receiver traps are expected not to return.
int64_t rt_stopwatch_elapsed_ms(void *obj);

/// @brief Check if the stopwatch is currently running.
/// @param obj Valid Stopwatch handle.
/// @return Non-zero if running, zero if stopped.
/// @warning The instance is not thread-safe, and invalid-receiver traps are
///          expected not to return.
int8_t rt_stopwatch_is_running(void *obj);

#ifdef __cplusplus
}
#endif
