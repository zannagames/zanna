//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_math.c
// Purpose: Thin C-library wrappers providing the BASIC runtime's math ABI.
//   Each function delegates to the corresponding C math-library routine
//   (sqrt, floor, ceil, sin, cos, etc.) so its operation-specific special-value
//   behaviour is shared across VM and native execution paths. The one
//   exception is rt_abs_i64, which adds an overflow check and fires rt_trap
//   on INT64_MIN because abs(INT64_MIN) is undefined in two's complement.
//
// Key invariants:
//   - Floating-point wrappers do not add traps for NaN or infinities; each
//     operation follows its C math-library semantics.
//   - rt_abs_i64(INT64_MIN) fires rt_trap("integer overflow in abs").
//   - All functions are exposed with C linkage (extern "C") so C++ callers
//     and both the VM and native backends can link them directly.
//   - No global state. Floating helpers and integer comparisons are pure;
//     rt_abs_i64 raises a trap for its unrepresentable input.
//
// Ownership/Lifetime:
//   - No heap allocation. All functions are stateless wrappers.
//
// Links: src/runtime/core/rt_math.h (public API),
//        src/runtime/core/rt_trap.h (rt_trap for abs overflow)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the runtime's scalar mathematical ABI.

#include "rt_math.h"
#include "rt.hpp"
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

// Ensure functions are visible with C linkage when included in C++ builds
#ifdef __cplusplus
extern "C" {
#endif

/// @brief Compute the non-negative square root of the input.
/// @details Delegates to @c sqrt from <math.h> so IEEE-754 semantics apply to
///          NaN and infinity.  The wrapper exists to keep the runtime ABI
///          stable and document the absence of explicit traps for negative
///          inputs.
/// @param x Input value supplied by BASIC user code.
/// @return Principal square root of @p x; NaN when @p x is negative.
double rt_sqrt(double x) {
    return sqrt(x);
}

/// @brief Round a floating-point value down to the nearest integer.
/// @details Calls @c floor so fractional inputs move toward negative
///          infinity.  NaN and infinities propagate unchanged, matching BASIC
///          expectations and avoiding additional trap hooks.
/// @param x Input value to round downward.
/// @return Largest integer value less than or equal to @p x.
double rt_floor(double x) {
    return floor(x);
}

/// @brief Round a floating-point value up to the nearest integer.
/// @details Wraps @c ceil to move fractional inputs toward positive
///          infinity while allowing special values (NaN, ±inf) to propagate
///          unchanged.
/// @param x Input value to round upward.
/// @return Smallest integer value greater than or equal to @p x.
double rt_ceil(double x) {
    return ceil(x);
}

/// @brief Compute the sine of an angle expressed in radians.
/// @details Defers to @c sin so the runtime inherits the host's precision and
///          handling for special values.  NaN inputs yield NaN, and infinite
///          arguments propagate NaN without trapping.
/// @param x Angle in radians.
/// @return Sine of @p x.
double rt_sin(double x) {
    return sin(x);
}

/// @brief Compute the cosine of an angle expressed in radians.
/// @details Uses @c cos from <math.h>, inheriting IEEE-754 behaviour for NaN
///          and infinity.  The wrapper preserves BASIC semantics without
///          introducing additional range checks.
/// @param x Angle in radians.
/// @return Cosine of @p x.
double rt_cos(double x) {
    return cos(x);
}

/// @brief Compute the tangent of an angle expressed in radians.
/// @details Delegates to @c tan from <math.h>, inheriting IEEE-754 behaviour
///          for NaN and infinity.  The wrapper allows BASIC programs to compute
///          tangents without explicit calls to sin/cos division.
/// @param x Angle in radians.
/// @return Tangent of @p x.
double rt_tan(double x) {
    return tan(x);
}

/// @brief Compute the arctangent of a value.
/// @details Uses @c atan from <math.h> to compute the principal value of the
///          arctangent.  Result is in radians, in the range [-π/2, π/2].
/// @param x Input value.
/// @return Arctangent of @p x in radians.
double rt_atan(double x) {
    return atan(x);
}

/// @brief Compute the exponential function (e^x).
/// @details Delegates to @c exp from <math.h>, providing the natural
///          exponential function for BASIC programs.  Overflow produces
///          infinity per IEEE-754 semantics.
/// @param x Exponent value.
/// @return e raised to the power of @p x.
double rt_exp(double x) {
    return exp(x);
}

/// @brief Compute the natural logarithm (base e).
/// @details Uses @c log from <math.h> to compute the natural logarithm.
///          Returns NaN for negative inputs and -infinity for zero input,
///          following IEEE-754 semantics.
/// @param x Input value (must be positive for real result).
/// @return Natural logarithm of @p x.
double rt_log(double x) {
    return log(x);
}

/// @brief Compute the sign of a 64-bit signed integer.
/// @details Returns -1 for negative values, 0 for zero, and 1 for positive
///          values.  This is the classic SGN function from BASIC.
/// @param v Signed integer input.
/// @return -1, 0, or 1 depending on the sign of @p v.
long long rt_sgn_i64(long long v) {
    if (v < 0)
        return -1;
    if (v > 0)
        return 1;
    return 0;
}

/// @brief Compute the sign of a double-precision floating-point value.
/// @details Returns -1.0 for negative values, 0.0 for either signed zero, and
///          1.0 for positive values. NaN returns NaN. The zero result is +0.0.
/// @param v Floating-point input.
/// @return -1.0, 0.0, or 1.0 depending on the sign of @p v.
double rt_sgn_f64(double v) {
    if (isnan(v))
        return v;
    if (v < 0.0)
        return -1.0;
    if (v > 0.0)
        return 1.0;
    return 0.0;
}

/// @brief Compute the absolute value of a 64-bit signed integer.
/// @details Mirrors BASIC's overflow semantics by trapping when @p v equals
///          @c LLONG_MIN (whose absolute value cannot be represented).  Other
///          values are returned as their non-negative magnitude.
/// @param v Signed integer input.
/// @return Absolute value of @p v when representable; returns zero after
///         reporting a trap for overflow.
long long rt_abs_i64(long long v) {
    if (v == LLONG_MIN)
        return rt_trap("rt_abs_i64: overflow"), 0;
    return v < 0 ? -v : v;
}

/// @brief Compute the magnitude of a double-precision floating-point value.
/// @details Delegates to @c fabs so NaN and infinity semantics follow the C
///          standard library.  The result is always non-negative, clearing
///          the sign bit of signed zero.
/// @param v Floating-point input.
/// @return Non-negative magnitude of @p v.
double rt_abs_f64(double v) {
    return fabs(v);
}

/// @brief Return the smaller of two double-precision floating-point values.
/// @details Implements BASIC MIN for floating-point arguments.
/// @param a First input value.
/// @param b Second input value.
/// @return The smaller of @p a and @p b.
double rt_min_f64(double a, double b) {
    if (isnan(a))
        return a;
    if (isnan(b))
        return b;
    if (a == b)
        return signbit(a) ? a : b;
    return a < b ? a : b;
}

/// @brief Return the larger of two double-precision floating-point values.
/// @details Implements BASIC MAX for floating-point arguments.
/// @param a First input value.
/// @param b Second input value.
/// @return The larger of @p a and @p b.
double rt_max_f64(double a, double b) {
    if (isnan(a))
        return a;
    if (isnan(b))
        return b;
    if (a == b)
        return signbit(a) ? b : a;
    return a > b ? a : b;
}

/// @brief Return the smaller of two 64-bit signed integers.
/// @details Implements BASIC MIN for integer arguments.
/// @param a First input value.
/// @param b Second input value.
/// @return The smaller of @p a and @p b.
long long rt_min_i64(long long a, long long b) {
    return a < b ? a : b;
}

/// @brief Return the larger of two 64-bit signed integers.
/// @details Implements BASIC MAX for integer arguments.
/// @param a First input value.
/// @param b Second input value.
/// @return The larger of @p a and @p b.
long long rt_max_i64(long long a, long long b) {
    return a > b ? a : b;
}

//=========================================================================
// Additional Math Functions
//=========================================================================

#ifndef M_PI
/// @brief Double-precision approximation of pi when the C library omits M_PI.
#define M_PI 3.14159265358979323846
#endif
#ifndef M_E
/// @brief Double-precision approximation of Euler's number when omitted by libc.
#define M_E 2.71828182845904523536
#endif
/// @brief Tau expressed as exactly twice this translation unit's pi constant.
#define M_TAU (2.0 * M_PI)

/// @brief Compute arc tangent of y/x using signs to determine quadrant.
/// @details Delegates to `atan2`, preserving its signed-zero, infinity, and NaN
///   quadrant rules without adding runtime traps.
/// @param y Y coordinate.
/// @param x X coordinate.
/// @return Angle in radians in range [-pi, pi].
double rt_atan2(double y, double x) {
    return atan2(y, x);
}

/// @brief Compute arc sine of x.
/// @details Delegates to `asin`; values outside [-1, 1] produce NaN according
///   to the platform math library.
/// @param x Input in range [-1, 1].
/// @return Angle in radians in range [-pi/2, pi/2].
double rt_asin(double x) {
    return asin(x);
}

/// @brief Compute arc cosine of x.
/// @details Delegates to `acos`; values outside [-1, 1] produce NaN according
///   to the platform math library.
/// @param x Input in range [-1, 1].
/// @return Angle in radians in range [0, pi].
double rt_acos(double x) {
    return acos(x);
}

/// @brief Compute hyperbolic sine of x.
/// @details Delegates to `sinh`; sufficiently large magnitudes may overflow to
///   signed infinity without trapping.
/// @param x Input value.
/// @return sinh(x).
double rt_sinh(double x) {
    return sinh(x);
}

/// @brief Compute hyperbolic cosine of x.
/// @details Delegates to `cosh`; sufficiently large magnitudes may overflow to
///   positive infinity without trapping.
/// @param x Input value.
/// @return cosh(x).
double rt_cosh(double x) {
    return cosh(x);
}

/// @brief Compute hyperbolic tangent of x.
/// @details Delegates to `tanh`, which approaches signed one for infinite inputs.
/// @param x Input value.
/// @return tanh(x).
double rt_tanh(double x) {
    return tanh(x);
}

/// @brief Round to nearest integer, away from zero on tie.
/// @details Delegates to `round`; signed zero, NaN, and infinities propagate.
/// @param x Input value.
/// @return Rounded value.
double rt_round(double x) {
    return round(x);
}

/// @brief Truncate toward zero.
/// @details Delegates to `trunc`; signed zero, NaN, and infinities propagate.
/// @param x Input value.
/// @return Truncated value.
double rt_trunc(double x) {
    return trunc(x);
}

/// @brief Compute base-10 logarithm.
/// @details Delegates to `log10`; zero produces negative infinity and negative
///   finite input produces NaN without trapping.
/// @param x Input (must be positive).
/// @return log10(x).
double rt_log10(double x) {
    return log10(x);
}

/// @brief Compute base-2 logarithm.
/// @details Delegates to `log2`; zero produces negative infinity and negative
///   finite input produces NaN without trapping.
/// @param x Input (must be positive).
/// @return log2(x).
double rt_log2(double x) {
    return log2(x);
}

/// @brief Compute floating-point remainder.
/// @details Delegates to `fmod`. A zero divisor or infinite dividend produces
///   NaN; a finite result has the dividend's sign.
/// @param x Dividend.
/// @param y Divisor.
/// @return Remainder of x/y.
double rt_fmod(double x, double y) {
    return fmod(x, y);
}

/// @brief Compute the Euclidean norm without spurious intermediate overflow.
/// @details Delegates to `hypot`, which scales its calculation but may still
///   return infinity when the mathematical result is not representable.
/// @param x First value.
/// @param y Second value.
/// @return Hypotenuse.
double rt_hypot(double x, double y) {
    return hypot(x, y);
}

/// @brief Clamp a value to a range [lo, hi].
/// @details Finite inverted bounds are exchanged before comparison. A NaN
///   @p val propagates because both comparisons are false; NaN bounds follow
///   ordinary IEEE comparison behavior and are not exchanged.
/// @param val Value to clamp.
/// @param lo Lower bound.
/// @param hi Upper bound.
/// @return Clamped value. If lo > hi, the bounds are swapped.
double rt_clamp_f64(double val, double lo, double hi) {
    // Handle inverted range by swapping
    if (lo > hi) {
        double tmp = lo;
        lo = hi;
        hi = tmp;
    }
    if (val < lo)
        return lo;
    if (val > hi)
        return hi;
    return val;
}

/// @brief Clamp an integer to a range [lo, hi].
/// @details Exchanges inverted bounds before applying inclusive comparisons.
/// @param val Value to clamp.
/// @param lo Lower bound.
/// @param hi Upper bound.
/// @return Clamped value. If lo > hi, the bounds are swapped.
long long rt_clamp_i64(long long val, long long lo, long long hi) {
    // Handle inverted range by swapping
    if (lo > hi) {
        long long tmp = lo;
        lo = hi;
        hi = tmp;
    }
    if (val < lo)
        return lo;
    if (val > hi)
        return hi;
    return val;
}

/// @brief Linear interpolation between a and b.
/// @details Preserves exact endpoints for @p t equal to zero or one and
///   propagates the first NaN encountered in @p a, @p b, then @p t. Opposite-
///   sign endpoints use a weighted sum to avoid overflow from `b - a` during
///   ordinary interpolation. Values of @p t outside [0, 1] extrapolate.
/// @param a Start value.
/// @param b End value.
/// @param t Interpolation factor (0 = a, 1 = b).
/// @return Interpolated value.
double rt_lerp(double a, double b, double t) {
    if (isnan(a))
        return a;
    if (isnan(b))
        return b;
    if (isnan(t))
        return t;
    if (t == 0.0)
        return a;
    if (t == 1.0)
        return b;
    if ((a <= 0.0 && b >= 0.0) || (a >= 0.0 && b <= 0.0))
        return (1.0 - t) * a + t * b;
    return a + t * (b - a);
}

/// @brief Wrap a value to range [lo, hi).
/// @details Uses `fmod` and adds one range width for negative remainders.
///   A non-positive range returns @p lo. Finite valid inputs produce the stated
///   half-open result; non-finite values follow `fmod` and may produce NaN.
/// @param val Value to wrap.
/// @param lo Lower bound (inclusive).
/// @param hi Upper bound (exclusive).
/// @return Wrapped value.
double rt_wrap_f64(double val, double lo, double hi) {
    double range = hi - lo;
    if (range <= 0.0)
        return lo;

    double result = fmod(val - lo, range);
    if (result < 0.0)
        result += range;
    return result + lo;
}

/// @brief Wrap an integer to range [lo, hi).
/// @details Uses unsigned distance arithmetic so the full signed 64-bit domain
///   is handled without signed overflow. A non-positive range returns @p lo.
/// @param val Value to wrap.
/// @param lo Lower bound (inclusive).
/// @param hi Upper bound (exclusive).
/// @return Wrapped value.
long long rt_wrap_i64(long long val, long long lo, long long hi) {
    if (hi <= lo)
        return lo;

    uint64_t range = (uint64_t)hi - (uint64_t)lo;
    uint64_t wrapped = 0;
    if (val >= lo) {
        wrapped = ((uint64_t)val - (uint64_t)lo) % range;
    } else {
        uint64_t distance = (uint64_t)lo - (uint64_t)val;
        uint64_t rem = distance % range;
        wrapped = rem == 0 ? 0 : range - rem;
    }

    uint64_t result_bits = (uint64_t)lo + wrapped;
    long long result;
    memcpy(&result, &result_bits, sizeof(result));
    return result;
}

/// @brief Return the constant Pi.
/// @details Uses the platform `M_PI` when available, otherwise the local
///   double-precision fallback.
/// @return Pi (3.14159...).
double rt_math_pi(void) {
    return M_PI;
}

/// @brief Return Euler's number.
/// @details Uses the platform `M_E` when available, otherwise the local
///   double-precision fallback.
/// @return e (2.71828...).
double rt_math_e(void) {
    return M_E;
}

/// @brief Return Tau (2*Pi).
/// @details Computed at compile time as twice the same pi constant returned by
///   @ref rt_math_pi.
/// @return Tau (6.28318...).
double rt_math_tau(void) {
    return M_TAU;
}

/// @brief Convert radians to degrees.
/// @details Multiplies by 180/pi; NaN and infinity propagate without trapping.
/// @param radians Angle in radians.
/// @return Angle in degrees.
double rt_deg(double radians) {
    return radians * (180.0 / M_PI);
}

/// @brief Convert degrees to radians.
/// @details Multiplies by pi/180; NaN and infinity propagate without trapping.
/// @param degrees Angle in degrees.
/// @return Angle in radians.
double rt_rad(double degrees) {
    return degrees * (M_PI / 180.0);
}

#ifdef __cplusplus
}
#endif
