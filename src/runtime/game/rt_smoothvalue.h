//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/game/rt_smoothvalue.h
/// @file
/// @brief Declares a finite exponential scalar smoother with target, velocity,
///        immediate-set, and impulse operations.
// Purpose: Smooth value interpolation for camera follow and UI animations, applying exponential
// smoothing each frame so the current value asymptotically approaches the target.
//
// Key invariants:
//   - Smoothing factors are clamped to [0.0, 0.999]; non-finite values become 0.
//   - Each rt_smoothvalue_update call performs exactly one elapsed-time-independent step.
//   - Values within 0.001 of the target snap exactly to it and clear velocity.
//   - rt_smoothvalue_set_immediate snaps current and target without smoothing.
//
// Ownership/Lifetime:
//   - Handles are runtime reference-counted objects.
//   - rt_smoothvalue_destroy releases the caller's owned reference; runtime GC
//     may also reclaim an otherwise unreachable object.
//
// Links: src/runtime/game/rt_smoothvalue.c (implementation)
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Runtime class identifier used to validate SmoothValue handles.
#define RT_SMOOTHVALUE_CLASS_ID INT64_C(-0x510203)

/// @brief Opaque handle to a SmoothValue instance.
typedef struct rt_smoothvalue_impl *rt_smoothvalue;

/// @brief Create a new SmoothValue.
/// @param initial Initial current and target value; non-finite input becomes
///        zero.
/// @param smoothing Old-value weight clamped to [0.0, 0.999], where zero
///        snaps on the next update and larger values move more slowly.
/// @return Owned SmoothValue handle, or `NULL` if allocation fails.
rt_smoothvalue rt_smoothvalue_new(double initial, double smoothing);

/// @brief Release one owned SmoothValue reference.
/// @param sv Owned handle to release; `NULL` is ignored.
void rt_smoothvalue_destroy(rt_smoothvalue sv);

/// @brief Get the current smoothed value.
/// @param sv Borrowed SmoothValue handle.
/// @return Current finite value, or `0.0` for null.
double rt_smoothvalue_get(rt_smoothvalue sv);

/// @brief Get the current smoothed value as an integer.
/// @param sv Borrowed SmoothValue handle.
/// @return Saturating round-half-away-from-zero result, or `0` for null.
int64_t rt_smoothvalue_get_i64(rt_smoothvalue sv);

/// @brief Get the target value.
/// @param sv Borrowed SmoothValue handle.
/// @return Finite target value, or `0.0` for null.
double rt_smoothvalue_target(rt_smoothvalue sv);

/// @brief Set the target value (current value will smoothly approach it).
/// @details Non-finite targets are ignored.
/// @param sv Borrowed SmoothValue handle.
/// @param target New finite target value.
void rt_smoothvalue_set_target(rt_smoothvalue sv, double target);

/// @brief Set both current and target value immediately (no smoothing).
/// @details Also clears velocity; non-finite values normalize to zero.
/// @param sv Borrowed SmoothValue handle.
/// @param value New value applied to both current and target.
void rt_smoothvalue_set_immediate(rt_smoothvalue sv, double value);

/// @brief Get the smoothing factor.
/// @param sv Borrowed SmoothValue handle.
/// @return Old-value weight in [0.0, 0.999], or `0.0` for null.
double rt_smoothvalue_smoothing(rt_smoothvalue sv);

/// @brief Set the smoothing factor.
/// @param sv Borrowed SmoothValue handle.
/// @param smoothing Candidate factor clamped to [0.0, 0.999]; non-finite
///        input becomes zero.
void rt_smoothvalue_set_smoothing(rt_smoothvalue sv, double smoothing);

/// @brief Update the smooth value by one frame.
/// @details Call once per frame to advance the interpolation. The current
///          value moves toward the target by: current += (target - current)
///          * (1 - smoothing). Values within 0.001 snap to the target.
/// @param sv Borrowed SmoothValue handle.
void rt_smoothvalue_update(rt_smoothvalue sv);

/// @brief Check if the value has reached the target (within epsilon).
/// @param sv Borrowed SmoothValue handle.
/// @return `1` when the absolute difference is below 0.001; a null handle is
///         treated as converged.
int8_t rt_smoothvalue_at_target(rt_smoothvalue sv);

/// @brief Get the velocity (rate of change per frame).
/// @param sv Borrowed SmoothValue handle.
/// @return Delta produced by the latest update, or `0.0` for null.
double rt_smoothvalue_velocity(rt_smoothvalue sv);

/// @brief Add an impulse to the current value.
/// @details Leaves the target unchanged so later updates smooth the displaced
///          value back toward it. Non-finite impulses are ignored.
/// @param sv Borrowed SmoothValue handle.
/// @param impulse Finite offset added immediately to the current value.
void rt_smoothvalue_impulse(rt_smoothvalue sv, double impulse);


// Time-constant damping (ADR 0250): frame-rate independent, so a reproducible
// camera or paced HUD converges identically on every machine.
/// @brief Advance the smoothing by @p dt_ms using a time-constant response.
/// @param sv Borrowed SmoothValue handle.
/// @param dt_ms Elapsed milliseconds; non-positive does not advance.
void rt_smoothvalue_update_ms(rt_smoothvalue sv, int64_t dt_ms);
/// @brief Set the damping response as an explicit millisecond time constant.
/// @param sv Borrowed SmoothValue handle.
/// @param tau_ms Time constant in milliseconds; zero snaps immediately.
void rt_smoothvalue_set_time_constant_ms(rt_smoothvalue sv, int64_t tau_ms);
/// @brief Read the damping response as a millisecond time constant.
/// @param sv Borrowed SmoothValue handle.
/// @return Time constant in milliseconds.
int64_t rt_smoothvalue_get_time_constant_ms(rt_smoothvalue sv);
/// @brief Snap the value to its target immediately, clearing velocity.
/// @param sv Borrowed SmoothValue handle.
void rt_smoothvalue_snap_to_target(rt_smoothvalue sv);
#ifdef __cplusplus
}
#endif
