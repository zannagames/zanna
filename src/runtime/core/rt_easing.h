//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/core/rt_easing.h
// Purpose: Easing functions for animation and interpolation implementing Robert Penner conventions,
// providing linear, quadratic, cubic, quartic, sine, exponential, circular, back, elastic, and
// bounce variants.
//
// Key invariants:
//   - Input t is expected in [0.0, 1.0], but inputs are not uniformly clamped:
//     polynomial, sine, back, and bounce curves extrapolate; exponential and
//     elastic curves pin selected endpoints; circular curves may produce NaN
//     when an out-of-range input makes a square-root radicand negative.
//   - f(0.0) = 0.0 and f(1.0) = 1.0 for all standard easing functions.
//   - Back and elastic variants may produce output outside [0, 1] due to overshoot.
//   - All functions use double precision IEEE-754 arithmetic.
//
// Ownership/Lifetime:
//   - All functions are pure and stateless; no allocation or side effects.
//   - Parameters and return values are plain double; no ownership transfer.
//
// Links: src/runtime/core/rt_easing.c (implementation), src/runtime/collections/rt_tween.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares normalized scalar easing functions for interpolation.
/// @details Each easing curve maps the conventional animation parameter
///   @p t from zero (start) to one (end). Unless a function says otherwise,
///   callers should supply a finite value in [0, 1].

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Computes a linear easing value (no acceleration or deceleration).
/// @param t Normalized time in the range [0.0, 1.0], where 0.0 is the start
///   and 1.0 is the end of the interpolation.
/// @return The eased value, equal to t (identity function).
double rt_ease_linear(double t);

/// @brief Computes a quadratic ease-in (accelerating from zero velocity).
/// @param t Normalized time in the range [0.0, 1.0].
/// @return The eased value, starting slow and accelerating. Equivalent to t^2.
double rt_ease_in_quad(double t);

/// @brief Computes a quadratic ease-out (decelerating to zero velocity).
/// @param t Normalized time in the range [0.0, 1.0].
/// @return The eased value, starting fast and decelerating toward the target.
double rt_ease_out_quad(double t);

/// @brief Computes a quadratic ease-in-out (acceleration then deceleration).
/// @param t Normalized time in the range [0.0, 1.0].
/// @return The eased value, accelerating in the first half and decelerating
///   in the second half for a smooth S-curve.
double rt_ease_in_out_quad(double t);

/// @brief Computes a cubic ease-in (accelerating from zero velocity, steeper
///   than quadratic).
/// @param t Normalized time in the range [0.0, 1.0].
/// @return The eased value. Equivalent to t^3.
double rt_ease_in_cubic(double t);

/// @brief Computes a cubic ease-out (decelerating to zero velocity, steeper
///   than quadratic).
/// @param t Normalized time in the range [0.0, 1.0].
/// @return The eased value, starting fast and decelerating sharply.
double rt_ease_out_cubic(double t);

/// @brief Computes a cubic ease-in-out (steep acceleration then deceleration).
/// @param t Normalized time in the range [0.0, 1.0].
/// @return The eased value with a pronounced S-curve, more dramatic than
///   the quadratic variant.
double rt_ease_in_out_cubic(double t);

/// @brief Computes a quartic ease-in (accelerating from zero velocity,
///   steeper than cubic).
/// @param t Normalized time in the range [0.0, 1.0].
/// @return The eased value. Equivalent to t^4.
double rt_ease_in_quart(double t);

/// @brief Computes a quartic ease-out (decelerating to zero velocity,
///   steeper than cubic).
/// @param t Normalized time in the range [0.0, 1.0].
/// @return The eased value, starting very fast and decelerating sharply.
double rt_ease_out_quart(double t);

/// @brief Computes a quartic ease-in-out (very steep acceleration then
///   deceleration).
/// @param t Normalized time in the range [0.0, 1.0].
/// @return The eased value with a very pronounced S-curve.
double rt_ease_in_out_quart(double t);

/// @brief Computes a sinusoidal ease-in (gentle acceleration using a sine
///   wave).
/// @param t Normalized time in the range [0.0, 1.0].
/// @return The eased value, accelerating gently. Uses a quarter-period sine
///   curve.
double rt_ease_in_sine(double t);

/// @brief Computes a sinusoidal ease-out (gentle deceleration using a sine
///   wave).
/// @param t Normalized time in the range [0.0, 1.0].
/// @return The eased value, decelerating gently toward the target.
double rt_ease_out_sine(double t);

/// @brief Computes a sinusoidal ease-in-out (gentle S-curve using a sine
///   wave).
/// @param t Normalized time in the range [0.0, 1.0].
/// @return The eased value with a natural, smooth S-curve. Uses a
///   half-period cosine curve.
double rt_ease_in_out_sine(double t);

/// @brief Computes an exponential ease-in (very slow start, rapid
///   acceleration).
/// @param t Normalized time in the range [0.0, 1.0].
/// @return The eased value, nearly flat at t=0 then accelerating
///   exponentially. Returns 0 when t is exactly 0.
double rt_ease_in_expo(double t);

/// @brief Computes an exponential ease-out (rapid start, very slow finish).
/// @param t Normalized time in the range [0.0, 1.0].
/// @return The eased value, starting very fast and approaching the target
///   asymptotically. Returns 1 when t is exactly 1.
double rt_ease_out_expo(double t);

/// @brief Computes an exponential ease-in-out (exponential acceleration then
///   deceleration).
/// @param t Normalized time in the range [0.0, 1.0].
/// @return The eased value with exponential S-curve. Handles the boundary
///   cases t=0 and t=1 exactly.
double rt_ease_in_out_expo(double t);

/// @brief Computes a circular ease-in (acceleration along a quarter-circle
///   arc).
/// @param t Normalized time in the range [0.0, 1.0].
/// @return The eased value, following a circular arc from slow to fast.
double rt_ease_in_circ(double t);

/// @brief Computes a circular ease-out (deceleration along a quarter-circle
///   arc).
/// @param t Normalized time in the range [0.0, 1.0].
/// @return The eased value, following a circular arc from fast to slow.
double rt_ease_out_circ(double t);

/// @brief Computes a circular ease-in-out (circular S-curve).
/// @param t Normalized time in the range [0.0, 1.0].
/// @return The eased value, following a smooth circular S-curve.
double rt_ease_in_out_circ(double t);

/// @brief Computes a back ease-in (pulls back slightly before accelerating
///   forward).
/// @param t Normalized time in the range [0.0, 1.0].
/// @return The eased value. Output may go slightly negative near t=0 due to
///   the overshoot, creating an anticipation effect.
double rt_ease_in_back(double t);

/// @brief Computes a back ease-out (overshoots the target then settles
///   back).
/// @param t Normalized time in the range [0.0, 1.0].
/// @return The eased value. Output may exceed 1.0 near t=1 before settling
///   to exactly 1.0, creating an overshoot effect.
double rt_ease_out_back(double t);

/// @brief Computes a back ease-in-out (anticipation and overshoot at both
///   ends).
/// @param t Normalized time in the range [0.0, 1.0].
/// @return The eased value with slight undershoot at the start and overshoot
///   near the end.
double rt_ease_in_out_back(double t);

/// @brief Computes an elastic ease-in (oscillates like a spring before
///   accelerating).
/// @param t Normalized time in the range [0.0, 1.0].
/// @return The eased value with spring-like oscillation. Output oscillates
///   around 0 before snapping to the target. Returns 0 at t=0, 1 at t=1.
double rt_ease_in_elastic(double t);

/// @brief Computes an elastic ease-out (overshoots and oscillates around the
///   target like a spring).
/// @param t Normalized time in the range [0.0, 1.0].
/// @return The eased value with spring-like oscillation around 1.0 before
///   settling. Returns 0 at t=0, 1 at t=1.
double rt_ease_out_elastic(double t);

/// @brief Computes an elastic ease-in-out (spring oscillation at both ends).
/// @param t Normalized time in the range [0.0, 1.0].
/// @return The eased value with elastic oscillation in the first and second
///   halves. Handles boundary cases t=0 and t=1 exactly.
double rt_ease_in_out_elastic(double t);

/// @brief Computes a bounce ease-in (bouncing effect at the start).
/// @param t Normalized time in the range [0.0, 1.0].
/// @return The eased value, simulating a ball bouncing in reverse before
///   reaching the target. Computed as 1 - ease_out_bounce(1 - t).
double rt_ease_in_bounce(double t);

/// @brief Computes a bounce ease-out (bouncing effect at the end, like a
///   ball hitting the floor).
/// @param t Normalized time in the range [0.0, 1.0].
/// @return The eased value, simulating a ball bouncing toward rest at 1.0.
///   Output stays within [0.0, 1.0].
double rt_ease_out_bounce(double t);

/// @brief Computes a bounce ease-in-out (bouncing at both start and end).
/// @param t Normalized time in the range [0.0, 1.0].
/// @return The eased value with bounce effects at both ends of the
///   interpolation.
double rt_ease_in_out_bounce(double t);

#ifdef __cplusplus
}
#endif
