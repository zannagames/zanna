//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/core/rt_math.h
// Purpose: Runtime library mathematical functions for BASIC programs, including transcendental
// (sin, cos, tan, atan, exp, log), rounding (floor, ceil), and sign/absolute-value operations.
//
// Key invariants:
//   - All floating-point operations use IEEE-754 double precision.
//   - Unchecked variants may return NaN or Infinity on domain errors; callers must validate.
//   - Checked variants (with _chk suffix) trap on domain errors instead of returning NaN.
//   - Integer and float variants of abs/sgn avoid unnecessary type conversions.
//
// Ownership/Lifetime:
//   - Functions use only scalar values; several are runtime-defined
//     compositions rather than direct platform-library wrappers.
//   - No heap allocation; parameters and return values are plain numeric types.
//
// Links: src/runtime/core/rt_math.c (implementation), src/runtime/core/rt_numeric.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares unchecked scalar math helpers used by BASIC and Zia.
/// @details Floating operations preserve platform `libm` special-value
///   behavior and do not trap. The integer absolute-value helper is the sole
///   exception, trapping when the positive magnitude cannot be represented.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Compute the square root of a double value.
/// @details Implements BASIC's SQR over double-precision values. Delegates to
///          the platform libm. For trapping on negative inputs, use the checked
///          variant rt_sqrt_chk_f64 in rt_numeric.h.
/// @param x Input value (must be non-negative for a real result).
/// @return sqrt(x); may return NaN for negative inputs (unchecked variant).
double rt_sqrt(double x);

/// @brief Compute the floor of a double value (largest integer <= x).
/// @details Provides explicit downward rounding by wrapping the platform's
///          floor() function with C ABI.
/// @param x Input value.
/// @return Floor of x as a double.
double rt_floor(double x);

/// @brief Compute the ceiling of a double value (smallest integer >= x).
/// @details Provides explicit upward rounding by wrapping the platform's
///          ceil() function with C ABI.
/// @param x Input value.
/// @return Ceiling of x as a double.
double rt_ceil(double x);

/// @brief Compute the sine of an angle in radians.
/// @details Wraps the platform's sin() for BASIC trigonometric support.
/// @param x Angle in radians.
/// @return sin(x).
double rt_sin(double x);

/// @brief Compute the cosine of an angle in radians.
/// @details Wraps the platform's cos() for BASIC trigonometric support.
/// @param x Angle in radians.
/// @return cos(x).
double rt_cos(double x);

/// @brief Compute the tangent of an angle in radians.
/// @details Wraps the platform's tan() for BASIC trigonometric support.
///          Result may overflow near odd multiples of pi/2.
/// @param x Angle in radians.
/// @return tan(x).
double rt_tan(double x);

/// @brief Compute the arctangent of a value.
/// @details Wraps the platform's atan() for BASIC trigonometric support.
/// @param x Input value.
/// @return atan(x) in the closed range [-pi/2, pi/2], with endpoints reached
///   for signed infinities.
double rt_atan(double x);

/// @brief Compute the exponential e^x.
/// @details Wraps the platform's exp() for BASIC exponential support.
///          May overflow to +INF for very large x values.
/// @param x Input exponent.
/// @return e^x.
double rt_exp(double x);

/// @brief Compute the natural logarithm ln(x).
/// @details Wraps the platform's log() for BASIC logarithm support. For
///          trapping on domain errors, use the checked variant in rt_numeric.h.
/// @param x Input value (x > 0 for a real result).
/// @return ln(x); returns -INF for x=0 and NaN for x<0 (unchecked variant).
double rt_log(double x);

/// @brief Compute the sign of an integer as -1, 0, or +1.
/// @details Implements BASIC's SGN for integer values.
/// @param x Input integer.
/// @return -1 when x < 0; 0 when x == 0; +1 when x > 0.
long long rt_sgn_i64(long long x);

/// @brief Compute the sign of a double as -1.0, 0.0, or +1.0.
/// @details Implements BASIC's SGN for floating-point values. Either signed
///   zero becomes positive zero, while a NaN is returned unchanged.
/// @param x Input value.
/// @return -1.0, +0.0, or +1.0 according to sign, or @p x when it is NaN.
double rt_sgn_f64(double x);

/// @brief Compute the absolute value of an integer.
/// @details Implements BASIC's ABS for integer values. Traps for the
///          most-negative two's-complement value because its magnitude cannot
///          be represented as a signed 64-bit integer.
/// @param x Input integer.
/// @return |x|.
long long rt_abs_i64(long long x);

/// @brief Compute the absolute value of a double.
/// @details Implements BASIC's ABS for floating-point values. Clears the
///          sign bit or calls the platform's fabs().
/// @param x Input value.
/// @return |x|.
double rt_abs_f64(double x);

/// @brief Return the minimum of two doubles.
/// @details Implements BASIC's MIN for floating-point values. NaN inputs
///          propagate, and signed zero is preserved so Min(-0.0, +0.0)
///          returns -0.0.
/// @param a First value.
/// @param b Second value.
/// @return The smaller of @p a and @p b.
double rt_min_f64(double a, double b);

/// @brief Return the maximum of two doubles.
/// @details Implements BASIC's MAX for floating-point values. NaN inputs
///          propagate, and signed zero is preserved so Max(-0.0, +0.0)
///          returns +0.0.
/// @param a First value.
/// @param b Second value.
/// @return The larger of @p a and @p b.
double rt_max_f64(double a, double b);

/// @brief Return the minimum of two integers.
/// @details Implements BASIC's MIN for integer values.
/// @param a First value.
/// @param b Second value.
/// @return The smaller of @p a and @p b.
long long rt_min_i64(long long a, long long b);

/// @brief Return the maximum of two integers.
/// @details Implements BASIC's MAX for integer values.
/// @param a First value.
/// @param b Second value.
/// @return The larger of @p a and @p b.
long long rt_max_i64(long long a, long long b);

//=========================================================================
// Additional Math Functions
//=========================================================================

/// @brief Compute the arc tangent of y/x using signs to determine the quadrant.
/// @details Wraps the platform's atan2() for full-range angle computation.
/// @param y Y-coordinate (numerator).
/// @param x X-coordinate (denominator).
/// @return Angle in radians in the range [-pi, pi].
double rt_atan2(double y, double x);

/// @brief Compute the arc sine of x.
/// @details Wraps the platform's asin(). Domain: [-1, 1].
/// @param x Input value.
/// @return asin(x) in the range [-pi/2, pi/2]; NaN if |x| > 1.
double rt_asin(double x);

/// @brief Compute the arc cosine of x.
/// @details Wraps the platform's acos(). Domain: [-1, 1].
/// @param x Input value.
/// @return acos(x) in the range [0, pi]; NaN if |x| > 1.
double rt_acos(double x);

/// @brief Compute the hyperbolic sine of x.
/// @details Wraps the platform's sinh(); large magnitudes may overflow to
///   signed infinity without trapping.
/// @param x Input value.
/// @return sinh(x).
double rt_sinh(double x);

/// @brief Compute the hyperbolic cosine of x.
/// @details Wraps the platform's cosh(); large magnitudes may overflow to
///   positive infinity without trapping.
/// @param x Input value.
/// @return cosh(x).
double rt_cosh(double x);

/// @brief Compute the hyperbolic tangent of x.
/// @details Wraps the platform's tanh(); signed infinities map to signed one.
/// @param x Input value.
/// @return tanh(x) in the closed range [-1, 1], or NaN for NaN input.
double rt_tanh(double x);

/// @brief Round x to the nearest integer (half-away-from-zero).
/// @details Wraps the platform's round() function.
/// @param x Input value.
/// @return Nearest integer as a double.
double rt_round(double x);

/// @brief Truncate x toward zero.
/// @details Wraps the platform's trunc() function.
/// @param x Input value.
/// @return Integer part of x (rounded toward zero).
double rt_trunc(double x);

/// @brief Compute the base-10 logarithm of x.
/// @details Wraps the platform's log10(). Zero produces negative infinity and
///   negative finite values produce NaN without trapping.
/// @param x Input value (x > 0 for a real result).
/// @return log10(x).
double rt_log10(double x);

/// @brief Compute the base-2 logarithm of x.
/// @details Wraps the platform's log2(). Zero produces negative infinity and
///   negative finite values produce NaN without trapping.
/// @param x Input value (x > 0 for a real result).
/// @return log2(x).
double rt_log2(double x);

/// @brief Compute the floating-point remainder of x / y.
/// @details Wraps the platform's fmod(). A finite result has the dividend's
///   sign; a zero divisor or infinite dividend produces NaN.
/// @param x Dividend.
/// @param y Divisor (must not be zero).
/// @return Remainder of x / y.
double rt_fmod(double x, double y);

/// @brief Compute sqrt(x*x + y*y) without spurious intermediate overflow.
/// @details Wraps the platform's hypot() for scaled Euclidean distance
///   computation. The result may still be infinite when the true magnitude
///   exceeds the representable range.
/// @param x First component.
/// @param y Second component.
/// @return Hypotenuse length.
double rt_hypot(double x, double y);

/// @brief Clamp a double value to the range [lo, hi].
/// @details Swaps finite inverted bounds, then returns lo if val < lo, hi if
///   val > hi, otherwise val. A NaN value propagates; NaN bounds follow normal
///   unordered comparison behavior.
/// @param val Value to clamp.
/// @param lo  Lower bound (inclusive).
/// @param hi  Upper bound (inclusive).
/// @return Clamped value.
double rt_clamp_f64(double val, double lo, double hi);

/// @brief Clamp an integer value to the range [lo, hi].
/// @details Swaps inverted bounds, then returns lo if val < lo, hi if val > hi,
///   otherwise val.
/// @param val Value to clamp.
/// @param lo  Lower bound (inclusive).
/// @param hi  Upper bound (inclusive).
/// @return Clamped value.
long long rt_clamp_i64(long long val, long long lo, long long hi);

/// @brief Compute linear interpolation between two values.
/// @details Returns a + t * (b - a). When t is 0.0 the result is a;
///          when t is 1.0 the result is b. The implementation preserves exact
///          endpoints and avoids intermediate overflow for opposite-sign
///          finite endpoints.
/// @param a Start value.
/// @param b End value.
/// @param t Interpolation parameter (typically in [0, 1]).
/// @return Interpolated value.
double rt_lerp(double a, double b, double t);

/// @brief Wrap a double value to the range [lo, hi).
/// @details Computes val modulo the range width so the result lies
///          within [lo, hi) for finite inputs and a positive finite width.
///          A non-positive width returns @p lo; non-finite arithmetic may
///          produce NaN.
/// @param val Value to wrap.
/// @param lo  Lower bound (inclusive).
/// @param hi  Upper bound (exclusive).
/// @return Wrapped value in [lo, hi).
double rt_wrap_f64(double val, double lo, double hi);

/// @brief Wrap an integer value to the range [lo, hi).
/// @details Computes val modulo the range width so the result lies
///          within [lo, hi). The arithmetic is overflow-safe across the full
///          signed 64-bit domain. A non-positive range returns @p lo.
/// @param val Value to wrap.
/// @param lo  Lower bound (inclusive).
/// @param hi  Upper bound (exclusive).
/// @return Wrapped value in [lo, hi).
long long rt_wrap_i64(long long val, long long lo, long long hi);

/// @brief Return the mathematical constant Pi (3.14159...).
/// @return Pi as a double.
double rt_math_pi(void);

/// @brief Return Euler's number e (2.71828...).
/// @return e as a double.
double rt_math_e(void);

/// @brief Return the mathematical constant Tau (2 * Pi = 6.28318...).
/// @return Tau as a double.
double rt_math_tau(void);

/// @brief Convert an angle from radians to degrees.
/// @param radians Angle in radians.
/// @return Equivalent angle in degrees.
double rt_deg(double radians);

/// @brief Convert an angle from degrees to radians.
/// @param degrees Angle in degrees.
/// @return Equivalent angle in radians.
double rt_rad(double degrees);

#ifdef __cplusplus
}
#endif
