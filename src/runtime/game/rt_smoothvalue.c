//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/game/rt_smoothvalue.c
/// @file
/// @brief Implements a finite, clamped exponential scalar smoother with
///        convergence snapping and per-update velocity.
// Purpose: Exponential-smoothing scalar for Zanna games. A SmoothValue glides
//   its current value toward a target each frame using the classic
//   "exponential moving average" formula:
//       current = current × smoothing + target × (1 - smoothing)
//   This produces a natural deceleration curve — the closer the value is to
//   the target, the slower it moves. Typical uses: smooth camera follow,
//   health bar animation, velocity damping, and UI slide-in panels.
//
// Key invariants:
//   - Smoothing factor ∈ [0.0, 0.999]. At 0.0 the value snaps to the target
//     instantly each frame. At 0.999 it barely moves per frame. Values >= 1.0
//     are clamped to 0.999 to prevent the value from stalling permanently.
//   - SMOOTH_EPSILON (0.001) is the convergence threshold: once |current -
//     target| < epsilon, current is snapped to target and velocity is zeroed.
//     This prevents infinite asymptotic drift at low smoothing values.
//   - velocity is the per-frame delta (current_frame - prev_frame). It is
//     zeroed on snap. Useful for secondary motion effects (motion blur, trails).
//   - rt_smoothvalue_set_immediate() sets both current and target to a value
//     and zeros velocity — equivalent to constructing a new SmoothValue at that
//     position. Use this to teleport without a visible interpolation glitch.
//   - rt_smoothvalue_impulse() directly offsets current without touching target.
//     On the next update() the value will smoothly return toward the target.
//
// Ownership/Lifetime:
//   - SmoothValue objects are GC-managed (rt_obj_new_i64). rt_smoothvalue_destroy()
//     calls rt_obj_free() explicitly; the GC also collects them automatically.
//
// Links: src/runtime/game/rt_smoothvalue.h (public API),
//        docs/zannalib/game.md (SmoothValue section)
//
//===----------------------------------------------------------------------===//

#include "rt_smoothvalue.h"
#include "rt_object.h"
#include "rt_trap.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>

/// @brief Absolute convergence threshold used for snapping and target tests.
#define SMOOTH_EPSILON 0.001

/// @brief Mutable state owned by a SmoothValue runtime object.
struct rt_smoothvalue_impl {
    double current;   ///< Current interpolated value.
    double target;    ///< Target value to approach.
    double smoothing; ///< Smoothing factor (0.0-1.0).
    double velocity;  ///< Current rate of change.
};

/// @brief Safe-cast a handle to the SmoothValue impl, trapping @p api on a
///        class-id mismatch.
/// @param sv Borrowed candidate SmoothValue handle.
/// @param api Trap message identifying the calling API.
/// @return Borrowed implementation pointer, or `NULL` when @p sv is `NULL`.
static rt_smoothvalue checked_smoothvalue(rt_smoothvalue sv, const char *api) {
    if (!sv)
        return NULL;
    if (rt_obj_class_id(sv) != RT_SMOOTHVALUE_CLASS_ID) {
        rt_trap(api);
        return NULL;
    }
    return sv;
}

/// @brief Return @p value if finite, else @p fallback (NaN/Inf sanitizer).
/// @param value Candidate floating-point value.
/// @param fallback Replacement used for NaN or either infinity.
/// @return @p value when finite; otherwise @p fallback.
static double smooth_finite_or(double value, double fallback) {
    return isfinite(value) ? value : fallback;
}

/// @brief Clamp a smoothing factor to [0.0, 0.999] (non-finite -> 0.0).
/// @details The 0.999 ceiling keeps the exponential smoother from freezing
///          (a factor of 1.0 would never converge toward the target).
/// @param smoothing Candidate exponential old-value weight.
/// @return Finite clamped factor.
static double smooth_clamp_smoothing(double smoothing) {
    if (!isfinite(smoothing))
        return 0.0;
    if (smoothing < 0.0)
        return 0.0;
    if (smoothing > 0.999)
        return 0.999;
    return smoothing;
}

/// @brief Round-half-away-from-zero to int64, saturating; 0 for non-finite.
/// @param value Floating-point value to convert.
/// @return Nearest signed integer with ties away from zero, saturated at the
///         signed 64-bit bounds.
static int64_t smooth_round_to_i64(double value) {
    if (!isfinite(value))
        return 0;
    if (value >= (double)INT64_MAX)
        return INT64_MAX;
    if (value <= (double)INT64_MIN)
        return INT64_MIN;
    return (int64_t)(value + (value >= 0 ? 0.5 : -0.5));
}

/// @brief Create a SmoothValue initialized at its target.
/// @param initial Initial current and target value; non-finite input becomes
///        zero.
/// @param smoothing Exponential old-value weight clamped to [0.0, 0.999].
/// @return Owned SmoothValue handle, or `NULL` if allocation fails.
rt_smoothvalue rt_smoothvalue_new(double initial, double smoothing) {
    struct rt_smoothvalue_impl *sv = (struct rt_smoothvalue_impl *)rt_obj_new_i64(
        RT_SMOOTHVALUE_CLASS_ID, (int64_t)sizeof(struct rt_smoothvalue_impl));
    if (!sv)
        return NULL;

    initial = smooth_finite_or(initial, 0.0);
    sv->current = initial;
    sv->target = initial;
    sv->velocity = 0.0;

    sv->smoothing = smooth_clamp_smoothing(smoothing);

    return sv;
}

/// @brief Release one owned SmoothValue reference.
/// @param sv Owned handle to release; `NULL` is ignored.
void rt_smoothvalue_destroy(rt_smoothvalue sv) {
    sv = checked_smoothvalue(sv, "SmoothValue.Destroy: expected Zanna.Game.SmoothValue");
    if (sv && rt_obj_release_check0(sv))
        rt_obj_free(sv);
}

/// @brief Get the current smoothed value.
/// @param sv Borrowed SmoothValue handle.
/// @return Current finite value, or `0.0` for a null handle.
double rt_smoothvalue_get(rt_smoothvalue sv) {
    sv = checked_smoothvalue(sv, "SmoothValue.Value: expected Zanna.Game.SmoothValue");
    if (!sv)
        return 0.0;
    return sv->current;
}

/// @brief Return the current smoothed value rounded to the nearest integer.
/// @param sv Borrowed SmoothValue handle.
/// @return Saturating round-half-away-from-zero result, or `0` for null.
int64_t rt_smoothvalue_get_i64(rt_smoothvalue sv) {
    sv = checked_smoothvalue(sv, "SmoothValue.ValueI64: expected Zanna.Game.SmoothValue");
    if (!sv)
        return 0;
    return smooth_round_to_i64(sv->current);
}

/// @brief Get the target value that the smooth value is converging toward.
/// @param sv Borrowed SmoothValue handle.
/// @return Current finite target, or `0.0` for a null handle.
double rt_smoothvalue_target(rt_smoothvalue sv) {
    sv = checked_smoothvalue(sv, "SmoothValue.Target: expected Zanna.Game.SmoothValue");
    if (!sv)
        return 0.0;
    return sv->target;
}

/// @brief Set a new target value for the smooth interpolation to converge toward.
/// @details Non-finite inputs are ignored so the existing target remains
///          usable.
/// @param sv Borrowed SmoothValue handle.
/// @param target New finite target.
void rt_smoothvalue_set_target(rt_smoothvalue sv, double target) {
    sv = checked_smoothvalue(sv, "SmoothValue.Target.set: expected Zanna.Game.SmoothValue");
    if (!sv)
        return;
    sv->target = smooth_finite_or(target, sv->target);
}

/// @brief Snap both current and target to a value instantly (no interpolation).
/// @details Also clears velocity; non-finite input is normalized to zero.
/// @param sv Borrowed SmoothValue handle.
/// @param value Finite value assigned to both endpoints.
void rt_smoothvalue_set_immediate(rt_smoothvalue sv, double value) {
    sv = checked_smoothvalue(sv, "SmoothValue.SetImmediate: expected Zanna.Game.SmoothValue");
    if (!sv)
        return;
    value = smooth_finite_or(value, 0.0);
    sv->current = value;
    sv->target = value;
    sv->velocity = 0.0;
}

/// @brief Return the current smoothing factor.
/// @param sv Borrowed SmoothValue handle.
/// @return Old-value weight in [0.0, 0.999], or `0.0` for null.
double rt_smoothvalue_smoothing(rt_smoothvalue sv) {
    sv = checked_smoothvalue(sv, "SmoothValue.Smoothing: expected Zanna.Game.SmoothValue");
    if (!sv)
        return 0.0;
    return sv->smoothing;
}

/// @brief Set the smoothing factor [0.0, 0.999]; higher = slower interpolation.
/// @param sv Borrowed SmoothValue handle.
/// @param smoothing Candidate factor; non-finite input becomes zero and
///        out-of-range input is clamped.
void rt_smoothvalue_set_smoothing(rt_smoothvalue sv, double smoothing) {
    sv = checked_smoothvalue(sv, "SmoothValue.Smoothing.set: expected Zanna.Game.SmoothValue");
    if (!sv)
        return;
    sv->smoothing = smooth_clamp_smoothing(smoothing);
}

/// @brief Update the smoothvalue state (called per frame/tick).
/// @details Applies one exponential step independent of elapsed wall time,
///          records the step delta as velocity, and snaps within
///          @ref SMOOTH_EPSILON. A non-finite arithmetic result falls back to
///          the target.
/// @param sv Borrowed SmoothValue handle.
void rt_smoothvalue_update(rt_smoothvalue sv) {
    sv = checked_smoothvalue(sv, "SmoothValue.Update: expected Zanna.Game.SmoothValue");
    if (!sv)
        return;

    // Exponential smoothing: current = current * smoothing + target * (1 - smoothing)
    double prev = sv->current;
    double factor = 1.0 - sv->smoothing;
    sv->current = sv->current * sv->smoothing + sv->target * factor;
    if (!isfinite(sv->current))
        sv->current = sv->target;

    // Calculate velocity
    sv->velocity = sv->current - prev;

    // Snap to target if very close (prevent floating point drift)
    if (fabs(sv->current - sv->target) < SMOOTH_EPSILON) {
        sv->current = sv->target;
        sv->velocity = 0.0;
    }
}


//=============================================================================
// Time-Constant Damping (ADR 0250)
//=============================================================================
//
// `Update()` applies one exponential step per call, so how fast a value chases
// its target depends on the frame rate. A 144 Hz machine, a 30 Hz machine, and
// a fixed-step headless capture all converge differently — which makes the
// result unusable for a reproducible camera or a paced HUD.
//
// `UpdateMs(dt)` instead uses the standard frame-rate-independent form
// `f = dt / (dt + tau)`, where `tau` is a time constant in milliseconds: the
// value covers ~63% of its remaining distance every `tau`. Same dt sequence,
// same result, on every machine.

/// @brief Advance the smoothing by @p dt_ms using a time-constant response.
/// @details `tau` is taken from the smoothing factor so a value configured the
///          old way still behaves sensibly: `tau_ms = smoothing / (1 -
///          smoothing) * 16.667`, i.e. the millisecond time constant that
///          reproduces the current per-frame factor at 60 Hz. Callers wanting
///          an explicit time constant should use
///          @ref rt_smoothvalue_set_time_constant_ms.
///
///          A non-positive @p dt_ms does not advance. A `tau` of zero snaps to
///          the target, matching `smoothing == 0`.
/// @param sv Borrowed SmoothValue handle.
/// @param dt_ms Elapsed milliseconds since the previous call.
void rt_smoothvalue_update_ms(rt_smoothvalue sv, int64_t dt_ms) {
    sv = checked_smoothvalue(sv, "SmoothValue.UpdateMs: expected Zanna.Game.SmoothValue");
    if (!sv || dt_ms <= 0)
        return;

    double tau;
    if (sv->smoothing >= 1.0)
        return; // never converges; matches Update()'s behaviour for smoothing == 1
    if (sv->smoothing <= 0.0) {
        tau = 0.0;
    } else {
        tau = sv->smoothing / (1.0 - sv->smoothing) * (1000.0 / 60.0);
    }

    double prev = sv->current;
    if (tau <= 0.0) {
        sv->current = sv->target;
    } else {
        double f = (double)dt_ms / ((double)dt_ms + tau);
        sv->current = sv->current + (sv->target - sv->current) * f;
    }
    if (!isfinite(sv->current))
        sv->current = sv->target;

    sv->velocity = sv->current - prev;

    if (fabs(sv->current - sv->target) < SMOOTH_EPSILON) {
        sv->current = sv->target;
        sv->velocity = 0.0;
    }
}

/// @brief Set the damping response as an explicit millisecond time constant.
/// @details The value covers ~63% of its remaining distance every @p tau_ms.
///          Stored as the equivalent 60 Hz smoothing factor so `Smoothing`,
///          `Update()`, and `UpdateMs()` stay consistent with one another.
///          Zero snaps immediately; negative values are clamped to zero.
/// @param sv Borrowed SmoothValue handle.
/// @param tau_ms Time constant in milliseconds.
void rt_smoothvalue_set_time_constant_ms(rt_smoothvalue sv, int64_t tau_ms) {
    sv = checked_smoothvalue(sv, "SmoothValue.TimeConstantMs: expected Zanna.Game.SmoothValue");
    if (!sv)
        return;
    if (tau_ms <= 0) {
        sv->smoothing = 0.0;
        return;
    }
    double tau = (double)tau_ms;
    double frame = 1000.0 / 60.0;
    sv->smoothing = tau / (tau + frame);
}

/// @brief Read the damping response as a millisecond time constant.
/// @param sv Borrowed SmoothValue handle.
/// @return Time constant in milliseconds, or zero for an immediate snap.
int64_t rt_smoothvalue_get_time_constant_ms(rt_smoothvalue sv) {
    sv = checked_smoothvalue(sv, "SmoothValue.TimeConstantMs: expected Zanna.Game.SmoothValue");
    if (!sv || sv->smoothing <= 0.0)
        return 0;
    if (sv->smoothing >= 1.0)
        return INT64_MAX;
    return (int64_t)(sv->smoothing / (1.0 - sv->smoothing) * (1000.0 / 60.0) + 0.5);
}

/// @brief Snap the value to its target immediately, clearing velocity.
/// @param sv Borrowed SmoothValue handle.
void rt_smoothvalue_snap_to_target(rt_smoothvalue sv) {
    sv = checked_smoothvalue(sv, "SmoothValue.SnapToTarget: expected Zanna.Game.SmoothValue");
    if (!sv)
        return;
    sv->current = sv->target;
    sv->velocity = 0.0;
}
/// @brief Check whether the current value has converged to the target (within epsilon).
/// @param sv Borrowed SmoothValue handle.
/// @return `1` when the absolute difference is below
///         @ref SMOOTH_EPSILON; a null handle is treated as converged.
int8_t rt_smoothvalue_at_target(rt_smoothvalue sv) {
    sv = checked_smoothvalue(sv, "SmoothValue.AtTarget: expected Zanna.Game.SmoothValue");
    if (!sv)
        return 1;
    return fabs(sv->current - sv->target) < SMOOTH_EPSILON ? 1 : 0;
}

/// @brief Get the per-frame velocity (change in value since last update).
/// @param sv Borrowed SmoothValue handle.
/// @return Delta produced by the latest update, or `0.0` for null.
double rt_smoothvalue_velocity(rt_smoothvalue sv) {
    sv = checked_smoothvalue(sv, "SmoothValue.Velocity: expected Zanna.Game.SmoothValue");
    if (!sv)
        return 0.0;
    return sv->velocity;
}

/// @brief Apply an instant offset to the current value (bypasses smoothing).
/// @details Leaves the target unchanged so later updates relax the displaced
///          value back toward it. Non-finite impulses are ignored; overflowed
///          results reset to the target.
/// @param sv Borrowed SmoothValue handle.
/// @param impulse Finite offset added directly to the current value.
void rt_smoothvalue_impulse(rt_smoothvalue sv, double impulse) {
    sv = checked_smoothvalue(sv, "SmoothValue.Impulse: expected Zanna.Game.SmoothValue");
    if (!sv)
        return;
    if (!isfinite(impulse))
        return;
    sv->current += impulse;
    if (!isfinite(sv->current))
        sv->current = sv->target;
}
