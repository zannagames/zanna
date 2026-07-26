//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/transform/OverflowArithmetic.hpp
// Purpose: Portable overflow-checked arithmetic for long long values.
//          On GCC/Clang, delegates to __builtin_*_overflow through explicit
//          wrappers.  On MSVC, provides equivalent manual implementations.
// Key invariants:
//   - Semantics match __builtin_*_overflow: returns true on overflow.
//   - Result is always written (even on overflow, wraps like unsigned).
// Ownership/Lifetime: Header-only, no state.
// Links: il/transform/ConstFold.cpp, il/transform/SCCP.cpp
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Defines portable signed-overflow detection with deterministic results.
 *
 * @details Each helper reports whether the mathematical operation exceeds the
 *          `long long` domain while still storing the corresponding wrapped
 *          two's-complement bit pattern. Compiler intrinsics implement the fast
 *          path where available; the fallback performs explicit range checks
 *          and unsigned-domain arithmetic without signed undefined behavior.
 */

#pragma once

#include <limits>

namespace il::transform::detail {

/// @brief Return the wrapped two's-complement result of signed addition.
/// @details Computes in the corresponding unsigned type so the stored result is
///          deterministic even when the mathematical signed operation overflows.
///          This mirrors the value written by compiler overflow intrinsics.
/// @param a Left-hand operand.
/// @param b Right-hand operand.
/// @return Wrapped result cast back to @c long @c long.
inline long long wrappingAdd(long long a, long long b) {
    return static_cast<long long>(static_cast<unsigned long long>(a) +
                                  static_cast<unsigned long long>(b));
}

/// @brief Return the wrapped two's-complement result of signed subtraction.
/// @param a Left-hand operand.
/// @param b Right-hand operand.
/// @return Wrapped result cast back to @c long @c long.
inline long long wrappingSub(long long a, long long b) {
    return static_cast<long long>(static_cast<unsigned long long>(a) -
                                  static_cast<unsigned long long>(b));
}

/// @brief Return the wrapped two's-complement result of signed multiplication.
/// @param a Left-hand operand.
/// @param b Right-hand operand.
/// @return Wrapped result cast back to @c long @c long.
inline long long wrappingMul(long long a, long long b) {
    return static_cast<long long>(static_cast<unsigned long long>(a) *
                                  static_cast<unsigned long long>(b));
}

/// @brief Detect signed addition overflow while always writing a wrapped result.
/// @details GCC and Clang use their overflow intrinsic.  Other compilers use a
///          range check and the same unsigned wrapping helper used for results.
/// @param a Left-hand operand.
/// @param b Right-hand operand.
/// @param result Destination for the wrapped result.
/// @return @c true when the mathematical signed addition overflows.
inline bool addOverflows(long long a, long long b, long long &result) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_add_overflow(a, b, &result);
#else
    result = wrappingAdd(a, b);
    return (b > 0 && a > (std::numeric_limits<long long>::max)() - b) ||
           (b < 0 && a < (std::numeric_limits<long long>::min)() - b);
#endif
}

/// @brief Detect signed subtraction overflow while always writing a wrapped result.
/// @param a Left-hand operand.
/// @param b Right-hand operand.
/// @param result Destination for the wrapped result.
/// @return @c true when the mathematical signed subtraction overflows.
inline bool subOverflows(long long a, long long b, long long &result) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_sub_overflow(a, b, &result);
#else
    result = wrappingSub(a, b);
    return (b > 0 && a < (std::numeric_limits<long long>::min)() + b) ||
           (b < 0 && a > (std::numeric_limits<long long>::max)() + b);
#endif
}

/// @brief Detect signed multiplication overflow while always writing a wrapped result.
/// @param a Left-hand operand.
/// @param b Right-hand operand.
/// @param result Destination for the wrapped result.
/// @return @c true when the mathematical signed multiplication overflows.
inline bool mulOverflows(long long a, long long b, long long &result) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_mul_overflow(a, b, &result);
#else
    result = wrappingMul(a, b);
    if (a == 0 || b == 0)
        return false;
    const long long max = (std::numeric_limits<long long>::max)();
    const long long min = (std::numeric_limits<long long>::min)();
    if (a > 0)
        return b > 0 ? a > max / b : b < min / a;
    return b > 0 ? a < min / b : a != 0 && b < max / a;
#endif
}

/// @brief Compatibility wrapper for legacy call sites that expect a pointer result.
/// @param a Left-hand operand.
/// @param b Right-hand operand.
/// @param result Destination for the wrapped result.
/// @return @c true when the mathematical signed addition overflows.
inline bool overflow_add(long long a, long long b, long long *result) {
    *result = static_cast<long long>(static_cast<unsigned long long>(a) +
                                     static_cast<unsigned long long>(b));
    return addOverflows(a, b, *result);
}

/// @brief Compatibility wrapper for legacy call sites that expect a pointer result.
/// @param a Left-hand operand.
/// @param b Right-hand operand.
/// @param result Destination for the wrapped result.
/// @return @c true when the mathematical signed subtraction overflows.
inline bool overflow_sub(long long a, long long b, long long *result) {
    return subOverflows(a, b, *result);
}

/// @brief Compatibility wrapper for legacy call sites that expect a pointer result.
/// @param a Left-hand operand.
/// @param b Right-hand operand.
/// @param result Destination for the wrapped result.
/// @return @c true when the mathematical signed multiplication overflows.
inline bool overflow_mul(long long a, long long b, long long *result) {
    return mulOverflows(a, b, *result);
}

} // namespace il::transform::detail
