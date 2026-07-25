//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/core/FPCast.hpp
// Purpose: Define deterministic checked floating-point-to-integer conversion
//          helpers with round-to-nearest, ties-to-even semantics.
// Key invariants:
//   - Non-finite signed operands report Invalid.
//   - Non-finite or negative unsigned operands report Invalid.
//   - Values outside the requested integer domain report Overflow.
// Ownership/Lifetime: Header-only value helpers with no retained state.
// Links: il/core/Opcode.hpp, il/verify/InstructionChecker.cpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace il::core {

/// @brief Failure category for checked floating-point-to-integer conversion.
enum class CheckedFPCastFailure {
    /// Conversion succeeded.
    None,
    /// Operand is non-finite or invalid for the signedness.
    Invalid,
    /// Rounded value is outside the requested bit-width domain.
    Overflow,
};

/// @brief Checked conversion result carrying either a bit-pattern value or failure.
struct CheckedFPCastResult {
    /// Conversion status.
    CheckedFPCastFailure failure{CheckedFPCastFailure::None};

    /// Signed value or bit-preserving storage for a successful unsigned result.
    int64_t value{0};

    /// @brief Test whether conversion completed without failure.
    /// @return True exactly when @ref failure is CheckedFPCastFailure::None.
    [[nodiscard]] bool ok() const noexcept {
        return failure == CheckedFPCastFailure::None;
    }
};

/// @brief Round a finite double to the nearest integral value, breaking ties to even.
/// @param operand Floating-point value to round.
/// @return Integral-valued double with the sign of @p operand preserved.
/// @note Non-finite inputs are not rejected here; checked entry points validate them first.
inline double roundToNearestTiesToEven(double operand) {
    const bool negative = std::signbit(operand);
    double integral = 0.0;
    const double fractional = std::modf(std::fabs(operand), &integral);

    if (fractional > 0.5) {
        integral += 1.0;
    } else if (fractional == 0.5 && std::fmod(integral, 2.0) != 0.0) {
        integral += 1.0;
    }

    return negative ? -integral : integral;
}

/// @brief Compute the inclusive lower bound of a signed integer domain as a double.
/// @param bits Requested width in the range 1 through 64.
/// @return `-2^(bits-1)`, with the exact int64 minimum conversion for 64 bits.
inline double signedLowerBoundForBits(int bits) {
    if (bits >= 64)
        return static_cast<double>(std::numeric_limits<int64_t>::min());
    return -std::ldexp(1.0, bits - 1);
}

/// @brief Compute the exclusive upper bound of a signed integer domain.
/// @param bits Requested width in the range 1 through 64.
/// @return `2^(bits-1)` as a double.
inline double signedUpperExclusiveForBits(int bits) {
    if (bits >= 64)
        return 9223372036854775808.0;
    return std::ldexp(1.0, bits - 1);
}

/// @brief Compute the exclusive upper bound of an unsigned integer domain.
/// @param bits Requested width in the range 1 through 64.
/// @return `2^bits` as a double.
inline double unsignedUpperExclusiveForBits(int bits) {
    if (bits >= 64)
        return 18446744073709551616.0;
    return std::ldexp(1.0, bits);
}

/// @brief Convert a double to a signed integer using ties-to-even rounding.
/// @param operand Floating-point operand.
/// @param bits Target signed width in the range 1 through 64.
/// @return Successful signed value, Invalid for non-finite input, or Overflow
///         when the rounded result lies outside the target domain.
inline CheckedFPCastResult checkedFpToSiRte(double operand, int bits) {
    if (!std::isfinite(operand))
        return {CheckedFPCastFailure::Invalid, 0};

    const double rounded = roundToNearestTiesToEven(operand);
    if (!std::isfinite(rounded) || rounded < signedLowerBoundForBits(bits) ||
        rounded >= signedUpperExclusiveForBits(bits)) {
        return {CheckedFPCastFailure::Overflow, 0};
    }

    return {CheckedFPCastFailure::None, static_cast<int64_t>(rounded)};
}

/// @brief Convert a double to an unsigned integer using ties-to-even rounding.
/// @param operand Non-negative floating-point operand.
/// @param bits Target unsigned width in the range 1 through 64.
/// @return Successful unsigned bit pattern stored in `value`, Invalid for
///         non-finite/negative input, or Overflow outside the target domain.
/// @details The unsigned payload is copied byte-for-byte into the signed
///          storage field so all 64 result bits remain representable.
inline CheckedFPCastResult checkedFpToUiRte(double operand, int bits) {
    if (!std::isfinite(operand) || operand < 0.0)
        return {CheckedFPCastFailure::Invalid, 0};

    if (operand >= unsignedUpperExclusiveForBits(bits))
        return {CheckedFPCastFailure::Overflow, 0};

    const double rounded = roundToNearestTiesToEven(operand);
    if (!std::isfinite(rounded) || rounded >= unsignedUpperExclusiveForBits(bits))
        return {CheckedFPCastFailure::Overflow, 0};

    uint64_t payload = static_cast<uint64_t>(rounded);
    int64_t stored = 0;
    std::memcpy(&stored, &payload, sizeof(stored));
    return {CheckedFPCastFailure::None, stored};
}

} // namespace il::core
