//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/MagicDivision.hpp
// Purpose: Target-independent magic-number computation for strength-reducing
//          integer division by a constant into multiply + shift sequences
//          (Hacker's Delight, 2nd edition, §10). Shared by the AArch64
//          peephole and the x86-64 division lowering so both backends apply
//          identical arithmetic.
// Key invariants:
//   - Signed: for divisor d >= 2, floor(x/d) = (mulh(x, M) [+ x] >> S) +
//     (x < 0 ? 1 : 0) for all signed 64-bit x.
//   - Unsigned: for non-power-of-2 d > 1, floor(x/d) via UMULH multiplier
//     with optional libdivide-style subtract-shift fixup.
// Ownership/Lifetime: Pure value helpers; no state.
// Links: src/codegen/aarch64/peephole/StrengthReduce.cpp,
//        src/codegen/x86_64/LowerDiv.cpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <optional>

/// @file
/// @brief Computes target-independent multiply/shift constants for division.

namespace zanna::codegen {

/// @brief Magic number parameters for signed division by a constant.
/// @details For a positive divisor d, multiplier M and post-shift S satisfy
///          floor(x / d) = floor((x * M) >> (64 + S)) + sign correction. The
///          high half of the 128-bit product comes from SMULH (AArch64) or
///          one-operand IMUL (x86-64).
struct MagicNumber {
    long long multiplier{0}; ///< Magic multiplier M (signed 64-bit).
    int shift{0};            ///< Post-shift amount S (arithmetic shift right).
    bool needsAdd{false};    ///< True if the dividend must be added after the
                             ///< high multiply (multiplier overflowed 2^63).
};

/// @brief Magic number parameters for unsigned division by a constant.
struct UnsignedMagicNumber {
    uint64_t multiplier{0}; ///< Magic multiplier M.
    unsigned shift{0};      ///< Post-shift amount.
    bool needsAdd{false};   ///< True when the libdivide-style fixup applies.
};

/// @brief Compute floor(log2(value)) for a non-zero 64-bit integer.
/// @param value Nonzero unsigned integer.
/// @return Greatest exponent `e` such that `2^e <= value`.
/// @pre @p value is nonzero.
[[nodiscard]] inline unsigned floorLog2U64(uint64_t value) noexcept {
    unsigned log = 0;
    // Guard BEFORE the shift: (1 << 64) is undefined behavior, and the old
    // operand order evaluated the shift first once log hit 63 (UBSan-caught).
    while (log < 63 && (uint64_t{1} << (log + 1)) <= value)
        ++log;
    return log;
}

/// @brief Divide a 128-bit unsigned integer (hi:lo) by a 64-bit divisor.
/// @param hi      Upper 64 bits of the numerator.
/// @param lo      Lower 64 bits of the numerator.
/// @param divisor 64-bit unsigned divisor.
/// @param[out] rem Receives the remainder.
/// @return Low 64-bit quotient.
/// @pre @p divisor is nonzero and the mathematical quotient fits in 64 bits.
[[nodiscard]] inline uint64_t divU128ByU64(uint64_t hi,
                                           uint64_t lo,
                                           uint64_t divisor,
                                           uint64_t &rem) noexcept {
#if defined(_MSC_VER) && !defined(__clang__) && (defined(_M_X64) || defined(_M_AMD64))
    unsigned __int64 remainder = 0;
    const unsigned __int64 quotient = _udiv128(hi, lo, divisor, &remainder);
    rem = remainder;
    return quotient;
#elif defined(_MSC_VER) && !defined(__clang__)
    uint64_t quotient = 0;
    rem = 0;
    for (int bit = 127; bit >= 0; --bit) {
        const uint64_t nextBit = (bit >= 64) ? ((hi >> (bit - 64)) & 1u) : ((lo >> bit) & 1u);
        const uint64_t carry = rem >> 63;
        rem = (rem << 1) | nextBit;
        if (carry || rem >= divisor) {
            rem -= divisor;
            if (bit < 64)
                quotient |= uint64_t{1} << bit;
        }
    }
    return quotient;
#else
    const unsigned __int128 numerator =
        (static_cast<unsigned __int128>(hi) << 64) | static_cast<unsigned __int128>(lo);
    rem = static_cast<uint64_t>(numerator % divisor);
    return static_cast<uint64_t>(numerator / divisor);
#endif
}

/// @brief Compute the magic number for signed division by a constant.
/// @details Algorithm from Hacker's Delight (2nd edition, §10-4). Given a
///          positive divisor d, finds M and S such that for any signed x:
///          floor(x / d) = (mulh(x, M) [+ x if needsAdd] >> S) + (x < 0 ? 1 : 0).
/// @param d The divisor (must be >= 2).
/// @return Magic number parameters; the default-constructed result (multiplier
///         0) signals an unsuitable divisor.
[[nodiscard]] inline MagicNumber computeSignedMagic(long long d) noexcept {
    MagicNumber result{};

    if (d < 2)
        return result;

    const auto ud = static_cast<uint64_t>(d);
    constexpr uint64_t twoP63 = static_cast<uint64_t>(1) << 63;

    const uint64_t rem = twoP63 % ud;
    const uint64_t nc = twoP63 - 1 - rem;

    int p = 63;
    uint64_t q1 = twoP63 / nc;
    uint64_t r1 = twoP63 - q1 * nc;
    uint64_t q2 = twoP63 / ud;
    uint64_t r2 = twoP63 - q2 * ud;

    for (;;) {
        ++p;
        q1 = 2 * q1;
        r1 = 2 * r1;
        if (r1 >= nc) {
            q1 += 1;
            r1 -= nc;
        }
        q2 = 2 * q2;
        r2 = 2 * r2;
        if (r2 >= ud) {
            q2 += 1;
            r2 -= ud;
        }

        const uint64_t delta = ud - r2;
        if (q1 < delta || (q1 == delta && r1 == 0))
            continue;

        break;
    }

    result.multiplier = static_cast<long long>(q2 + 1);
    result.shift = p - 64;

    // If the multiplier does not fit in signed 64-bit, encode it minus 2^64
    // and add the dividend after the high multiply to compensate:
    // mulh(x, M - 2^64) + x == mulh(x, M).
    if (static_cast<uint64_t>(result.multiplier) >= twoP63) {
        result.needsAdd = true;
    }

    return result;
}

/// @brief Compute the magic number for unsigned division by a constant.
/// @details Returns the UMULH multiplier and post-shift for the optimized
///          unsigned divide. `needsAdd` selects the libdivide-style
///          subtract-then-shift correction used when the exact multiplier
///          overflows 64 bits.
/// @param d The unsigned divisor; must be > 1 and not a power of 2.
/// @return Magic parameters, or nullopt when @p d is unsuitable.
[[nodiscard]] inline std::optional<UnsignedMagicNumber> computeUnsignedMagic(uint64_t d) noexcept {
    if (d <= 1)
        return std::nullopt;
    if ((d & (d - 1)) == 0)
        return std::nullopt;

    const unsigned floorLog2 = floorLog2U64(d);
    uint64_t rem = 0;
    uint64_t proposed = divU128ByU64(uint64_t{1} << floorLog2, 0, d, rem);
    const uint64_t e = d - rem;

    UnsignedMagicNumber result{};
    result.shift = floorLog2;
    if (e < (uint64_t{1} << floorLog2)) {
        result.multiplier = proposed + 1;
        result.needsAdd = false;
        return result;
    }

    proposed += proposed;
    const uint64_t twiceRem = rem + rem;
    if (twiceRem >= d || twiceRem < rem)
        proposed += 1;
    result.multiplier = proposed + 1;
    result.needsAdd = true;
    return result;
}

} // namespace zanna::codegen
