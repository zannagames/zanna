//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: frontends/common/ConstantFolding.hpp
// Purpose: Pure arithmetic constant folding utilities for language frontends.
//
// This header provides language-agnostic constant folding operations that can
// be used by any language frontend. The functions operate on primitive values
// and return optional results (empty on overflow/error).
// Key invariants: Signed operations never execute undefined overflow; invalid
//                 arithmetic and non-representable conversions return nullopt.
// Ownership/Lifetime: Header-only, stateless value utilities.
// Links: docs/il/il-guide.md
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares checked, language-agnostic scalar constant-folding helpers.
/// @details Integer helpers avoid undefined behavior and report invalid results
///          with std::nullopt; floating-point helpers preserve IEEE 754 behavior.
//
//===----------------------------------------------------------------------===//
#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace il::frontends::common::const_fold {

//===----------------------------------------------------------------------===//
// Integer Arithmetic
//===----------------------------------------------------------------------===//

/// @brief Fold integer addition with overflow detection.
/// @param lhs Left addend.
/// @param rhs Right addend.
/// @return Result if no overflow, empty otherwise.
[[nodiscard]] inline std::optional<int64_t> foldIntAdd(int64_t lhs, int64_t rhs) noexcept {
    if ((rhs > 0 && lhs > std::numeric_limits<int64_t>::max() - rhs) ||
        (rhs < 0 && lhs < std::numeric_limits<int64_t>::min() - rhs))
        return std::nullopt;
    return lhs + rhs;
}

/// @brief Fold integer subtraction with overflow detection.
/// @param lhs Minuend.
/// @param rhs Subtrahend.
/// @return Result if no overflow, empty otherwise.
[[nodiscard]] inline std::optional<int64_t> foldIntSub(int64_t lhs, int64_t rhs) noexcept {
    if ((rhs < 0 && lhs > std::numeric_limits<int64_t>::max() + rhs) ||
        (rhs > 0 && lhs < std::numeric_limits<int64_t>::min() + rhs))
        return std::nullopt;
    return lhs - rhs;
}

/// @brief Fold integer multiplication with overflow detection.
/// @param lhs Left factor.
/// @param rhs Right factor.
/// @return Result if no overflow, empty otherwise.
[[nodiscard]] inline std::optional<int64_t> foldIntMul(int64_t lhs, int64_t rhs) noexcept {
    if (lhs > 0) {
        if ((rhs > 0 && lhs > std::numeric_limits<int64_t>::max() / rhs) ||
            (rhs < 0 && rhs < std::numeric_limits<int64_t>::min() / lhs))
            return std::nullopt;
    } else if (lhs < 0) {
        if ((rhs > 0 && lhs < std::numeric_limits<int64_t>::min() / rhs) ||
            (rhs < 0 && lhs < std::numeric_limits<int64_t>::max() / rhs))
            return std::nullopt;
    }

    if (lhs == std::numeric_limits<int64_t>::min() && rhs == -1)
        return std::nullopt;

    return lhs * rhs;
}

/// @brief Fold integer division with zero check.
/// @param lhs Dividend.
/// @param rhs Divisor.
/// @return Quotient when defined, or empty for zero division and
///         `INT64_MIN / -1`.
[[nodiscard]] inline std::optional<int64_t> foldIntDiv(int64_t lhs, int64_t rhs) noexcept {
    if (rhs == 0)
        return std::nullopt;

    // Handle MIN / -1 overflow case
    if (lhs == std::numeric_limits<int64_t>::min() && rhs == -1)
        return std::nullopt;

    return lhs / rhs;
}

/// @brief Fold integer modulo with zero check.
/// @param lhs Dividend.
/// @param rhs Divisor.
/// @return Remainder when defined, or empty for zero division and
///         `INT64_MIN % -1`.
[[nodiscard]] inline std::optional<int64_t> foldIntMod(int64_t lhs, int64_t rhs) noexcept {
    if (rhs == 0)
        return std::nullopt;

    if (lhs == std::numeric_limits<int64_t>::min() && rhs == -1)
        return std::nullopt;

    return lhs % rhs;
}

/// @brief Fold integer negation with overflow detection.
/// @param val Value to negate.
/// @return Result if no overflow, empty otherwise.
[[nodiscard]] inline std::optional<int64_t> foldIntNeg(int64_t val) noexcept {
    if (val == std::numeric_limits<int64_t>::min())
        return std::nullopt;

    return -val;
}

/// @brief Fold integer absolute value with overflow detection.
/// @param val Value whose magnitude is requested.
/// @return Result if no overflow, empty otherwise.
[[nodiscard]] inline std::optional<int64_t> foldIntAbs(int64_t val) noexcept {
    if (val == std::numeric_limits<int64_t>::min())
        return std::nullopt;

    return val < 0 ? -val : val;
}

//===----------------------------------------------------------------------===//
// Floating-Point Arithmetic
//===----------------------------------------------------------------------===//

/// @brief Fold floating-point addition.
/// @param lhs Left addend.
/// @param rhs Right addend.
/// @return IEEE 754 sum.
[[nodiscard]] inline double foldFloatAdd(double lhs, double rhs) noexcept {
    return lhs + rhs;
}

/// @brief Fold floating-point subtraction.
/// @param lhs Minuend.
/// @param rhs Subtrahend.
/// @return IEEE 754 difference.
[[nodiscard]] inline double foldFloatSub(double lhs, double rhs) noexcept {
    return lhs - rhs;
}

/// @brief Fold floating-point multiplication.
/// @param lhs Left factor.
/// @param rhs Right factor.
/// @return IEEE 754 product.
[[nodiscard]] inline double foldFloatMul(double lhs, double rhs) noexcept {
    return lhs * rhs;
}

/// @brief Fold floating-point division.
/// @param lhs Dividend.
/// @param rhs Divisor.
/// @return Result, or NaN/Inf for division by zero.
[[nodiscard]] inline double foldFloatDiv(double lhs, double rhs) noexcept {
    return lhs / rhs; // IEEE 754 handles div by zero
}

/// @brief Fold floating-point negation.
/// @param val Value to negate.
/// @return Negated IEEE 754 value.
[[nodiscard]] inline double foldFloatNeg(double val) noexcept {
    return -val;
}

/// @brief Fold floating-point absolute value.
/// @param val Value whose magnitude is requested.
/// @return Absolute value computed by std::fabs.
[[nodiscard]] inline double foldFloatAbs(double val) noexcept {
    return std::fabs(val);
}

/// @brief Fold floating-point power.
/// @param base Base operand.
/// @param exp Exponent operand.
/// @return Value computed by std::pow, including its IEEE error results.
[[nodiscard]] inline double foldFloatPow(double base, double exp) noexcept {
    return std::pow(base, exp);
}

/// @brief Fold floating-point square root.
/// @param val Radicand.
/// @return Value computed by std::sqrt, including NaN for invalid inputs.
[[nodiscard]] inline double foldFloatSqrt(double val) noexcept {
    return std::sqrt(val);
}

//===----------------------------------------------------------------------===//
// Comparison Operations
//===----------------------------------------------------------------------===//

/// @brief Fold integer comparison (less than).
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @return True when @p lhs is less than @p rhs.
[[nodiscard]] inline bool foldIntLt(int64_t lhs, int64_t rhs) noexcept {
    return lhs < rhs;
}

/// @brief Fold integer comparison (less than or equal).
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @return True when @p lhs is less than or equal to @p rhs.
[[nodiscard]] inline bool foldIntLe(int64_t lhs, int64_t rhs) noexcept {
    return lhs <= rhs;
}

/// @brief Fold integer comparison (greater than).
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @return True when @p lhs is greater than @p rhs.
[[nodiscard]] inline bool foldIntGt(int64_t lhs, int64_t rhs) noexcept {
    return lhs > rhs;
}

/// @brief Fold integer comparison (greater than or equal).
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @return True when @p lhs is greater than or equal to @p rhs.
[[nodiscard]] inline bool foldIntGe(int64_t lhs, int64_t rhs) noexcept {
    return lhs >= rhs;
}

/// @brief Fold integer comparison (equal).
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @return True when both operands are equal.
[[nodiscard]] inline bool foldIntEq(int64_t lhs, int64_t rhs) noexcept {
    return lhs == rhs;
}

/// @brief Fold integer comparison (not equal).
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @return True when the operands differ.
[[nodiscard]] inline bool foldIntNe(int64_t lhs, int64_t rhs) noexcept {
    return lhs != rhs;
}

/// @brief Fold floating-point comparison (less than).
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @return Result of the IEEE 754 less-than comparison.
[[nodiscard]] inline bool foldFloatLt(double lhs, double rhs) noexcept {
    return lhs < rhs;
}

/// @brief Fold floating-point comparison (less than or equal).
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @return Result of the IEEE 754 less-than-or-equal comparison.
[[nodiscard]] inline bool foldFloatLe(double lhs, double rhs) noexcept {
    return lhs <= rhs;
}

/// @brief Fold floating-point comparison (greater than).
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @return Result of the IEEE 754 greater-than comparison.
[[nodiscard]] inline bool foldFloatGt(double lhs, double rhs) noexcept {
    return lhs > rhs;
}

/// @brief Fold floating-point comparison (greater than or equal).
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @return Result of the IEEE 754 greater-than-or-equal comparison.
[[nodiscard]] inline bool foldFloatGe(double lhs, double rhs) noexcept {
    return lhs >= rhs;
}

/// @brief Fold floating-point comparison (equal).
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @return Result of the IEEE 754 equality comparison.
[[nodiscard]] inline bool foldFloatEq(double lhs, double rhs) noexcept {
    return lhs == rhs;
}

/// @brief Fold floating-point comparison (not equal).
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @return Result of the IEEE 754 inequality comparison.
[[nodiscard]] inline bool foldFloatNe(double lhs, double rhs) noexcept {
    return lhs != rhs;
}

//===----------------------------------------------------------------------===//
// Logical Operations
//===----------------------------------------------------------------------===//

/// @brief Fold logical AND.
/// @param lhs Left Boolean operand.
/// @param rhs Right Boolean operand.
/// @return Logical conjunction of the operands.
[[nodiscard]] inline bool foldAnd(bool lhs, bool rhs) noexcept {
    return lhs && rhs;
}

/// @brief Fold logical OR.
/// @param lhs Left Boolean operand.
/// @param rhs Right Boolean operand.
/// @return Logical disjunction of the operands.
[[nodiscard]] inline bool foldOr(bool lhs, bool rhs) noexcept {
    return lhs || rhs;
}

/// @brief Fold logical NOT.
/// @param val Boolean operand.
/// @return Logical negation of @p val.
[[nodiscard]] inline bool foldNot(bool val) noexcept {
    return !val;
}

/// @brief Fold logical XOR.
/// @param lhs Left Boolean operand.
/// @param rhs Right Boolean operand.
/// @return True when exactly one operand is true.
[[nodiscard]] inline bool foldXor(bool lhs, bool rhs) noexcept {
    return lhs != rhs;
}

//===----------------------------------------------------------------------===//
// Bitwise Operations
//===----------------------------------------------------------------------===//

/// @brief Fold bitwise AND.
/// @param lhs Left integer word.
/// @param rhs Right integer word.
/// @return Bitwise conjunction of the operands.
[[nodiscard]] inline int64_t foldBitAnd(int64_t lhs, int64_t rhs) noexcept {
    return lhs & rhs;
}

/// @brief Fold bitwise OR.
/// @param lhs Left integer word.
/// @param rhs Right integer word.
/// @return Bitwise disjunction of the operands.
[[nodiscard]] inline int64_t foldBitOr(int64_t lhs, int64_t rhs) noexcept {
    return lhs | rhs;
}

/// @brief Fold bitwise XOR.
/// @param lhs Left integer word.
/// @param rhs Right integer word.
/// @return Bitwise exclusive-or of the operands.
[[nodiscard]] inline int64_t foldBitXor(int64_t lhs, int64_t rhs) noexcept {
    return lhs ^ rhs;
}

/// @brief Fold bitwise NOT.
/// @param val Integer word to complement.
/// @return Bitwise complement of @p val.
[[nodiscard]] inline int64_t foldBitNot(int64_t val) noexcept {
    return ~val;
}

/// @brief Fold left shift with overflow check.
/// @details Performs the shift through the unsigned representation, producing
///          deterministic two's-complement bit behavior without signed overflow.
/// @param val Bit pattern to shift.
/// @param shift Shift count.
/// @return Shifted word when @p shift is in [0, 63], empty otherwise.
[[nodiscard]] inline std::optional<int64_t> foldShl(int64_t val, int64_t shift) noexcept {
    if (shift < 0 || shift >= 64)
        return std::nullopt;

    const auto bits = static_cast<uint64_t>(val) << static_cast<unsigned>(shift);
    return static_cast<int64_t>(bits);
}

/// @brief Fold arithmetic right shift with overflow check.
/// @details Explicitly fills high bits for negative inputs instead of relying
///          on implementation-defined signed right-shift behavior.
/// @param val Bit pattern to shift.
/// @param shift Shift count.
/// @return Sign-extended result when @p shift is in [0, 63], empty otherwise.
[[nodiscard]] inline std::optional<int64_t> foldShr(int64_t val, int64_t shift) noexcept {
    if (shift < 0 || shift >= 64)
        return std::nullopt;

    if (shift == 0)
        return val;
    auto bits = static_cast<uint64_t>(val) >> static_cast<unsigned>(shift);
    if (val < 0)
        bits |= ~uint64_t{0} << (64U - static_cast<unsigned>(shift));
    return static_cast<int64_t>(bits);
}

//===----------------------------------------------------------------------===//
// Type Conversions
//===----------------------------------------------------------------------===//

/// @brief Convert integer to floating-point.
/// @param val Signed integer value.
/// @return Nearest representable double produced by the language conversion.
[[nodiscard]] inline double intToFloat(int64_t val) noexcept {
    return static_cast<double>(val);
}

/// @brief Convert floating-point to integer (truncate toward zero).
/// @param val Floating-point value to convert.
/// @return Truncated integer, or empty for NaN, infinity, or a value outside
///         the signed 64-bit range.
[[nodiscard]] inline std::optional<int64_t> floatToInt(double val) noexcept {
    if (std::isnan(val) || std::isinf(val))
        return std::nullopt;

    constexpr double kInt64UpperExclusive = 0x1p63;
    constexpr double kInt64LowerInclusive = -0x1p63;
    if (val >= kInt64UpperExclusive || val < kInt64LowerInclusive)
        return std::nullopt;

    return static_cast<int64_t>(val);
}

/// @brief Convert floating-point to integer (floor).
/// @param val Floating-point value to round downward.
/// @return Floored integer, or empty when the rounded value is non-finite or
///         outside the signed 64-bit range.
[[nodiscard]] inline std::optional<int64_t> floatFloor(double val) noexcept {
    double floored = std::floor(val);
    return floatToInt(floored);
}

/// @brief Convert floating-point to integer (ceiling).
/// @param val Floating-point value to round upward.
/// @return Ceiled integer, or empty when the rounded value is non-finite or
///         outside the signed 64-bit range.
[[nodiscard]] inline std::optional<int64_t> floatCeil(double val) noexcept {
    double ceiled = std::ceil(val);
    return floatToInt(ceiled);
}

/// @brief Convert floating-point to integer (round to nearest).
/// @details Uses std::round, so halfway cases round away from zero.
/// @param val Floating-point value to round.
/// @return Rounded integer, or empty when the rounded value is non-finite or
///         outside the signed 64-bit range.
[[nodiscard]] inline std::optional<int64_t> floatRound(double val) noexcept {
    double rounded = std::round(val);
    return floatToInt(rounded);
}

} // namespace il::frontends::common::const_fold
