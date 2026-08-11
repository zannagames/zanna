//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/codegen/test_magic_division.cpp
// Purpose: Exhaustively validate the shared magic-number division math against
//          reference integer division, emulating the exact instruction
//          sequences both backends emit (high-multiply, adjust, shift, sign
//          fix). Guards the constants that replace IDIV/SDIV/UDIV at runtime.
// Key invariants:
//   - Signed: quotients truncate toward zero for every (dividend, divisor).
//   - Unsigned: exact floor quotients including the needsAdd fixup path.
// Ownership/Lifetime: Pure computation; no fixtures.
// Links: src/codegen/common/MagicDivision.hpp
//
//===----------------------------------------------------------------------===//

#include "tests/TestHarness.hpp"

#include "codegen/common/MagicDivision.hpp"

#include <cstdint>
#include <limits>
#include <vector>

namespace {

/// @brief Portable signed high-64 multiply (the SMULH / one-operand IMUL result).
int64_t mulhiS64(int64_t a, int64_t b) {
#if defined(__SIZEOF_INT128__)
    return static_cast<int64_t>(
        __extension__((static_cast<__int128>(a) * static_cast<__int128>(b)) >> 64));
#else
    // 32-bit split fallback.
    const uint64_t ua = static_cast<uint64_t>(a);
    const uint64_t ub = static_cast<uint64_t>(b);
    const uint64_t aLo = ua & 0xFFFFFFFFULL, aHi = ua >> 32;
    const uint64_t bLo = ub & 0xFFFFFFFFULL, bHi = ub >> 32;
    const uint64_t lolo = aLo * bLo;
    const uint64_t lohi = aLo * bHi;
    const uint64_t hilo = aHi * bLo;
    const uint64_t hihi = aHi * bHi;
    const uint64_t carry = ((lolo >> 32) + (lohi & 0xFFFFFFFFULL) + (hilo & 0xFFFFFFFFULL)) >> 32;
    uint64_t hi = hihi + (lohi >> 32) + (hilo >> 32) + carry;
    // Signed correction.
    if (a < 0)
        hi -= ub;
    if (b < 0)
        hi -= ua;
    return static_cast<int64_t>(hi);
#endif
}

/// @brief Portable unsigned high-64 multiply (UMULH / one-operand MUL result).
uint64_t mulhiU64(uint64_t a, uint64_t b) {
#if defined(__SIZEOF_INT128__)
    return static_cast<uint64_t>(__extension__((static_cast<unsigned __int128>(a) *
                                                static_cast<unsigned __int128>(b)) >>
                                               64));
#else
    const uint64_t aLo = a & 0xFFFFFFFFULL, aHi = a >> 32;
    const uint64_t bLo = b & 0xFFFFFFFFULL, bHi = b >> 32;
    const uint64_t lolo = aLo * bLo;
    const uint64_t lohi = aLo * bHi;
    const uint64_t hilo = aHi * bLo;
    const uint64_t hihi = aHi * bHi;
    const uint64_t carry = ((lolo >> 32) + (lohi & 0xFFFFFFFFULL) + (hilo & 0xFFFFFFFFULL)) >> 32;
    return hihi + (lohi >> 32) + (hilo >> 32) + carry;
#endif
}

/// @brief Emulate the emitted signed magic-divide sequence for divisor d >= 2.
int64_t emulateSignedMagic(int64_t x, int64_t d) {
    const zanna::codegen::MagicNumber magic = zanna::codegen::computeSignedMagic(d);
    int64_t hi = mulhiS64(x, magic.multiplier);
    if (magic.needsAdd)
        hi += x;
    if (magic.shift > 0)
        hi >>= magic.shift;
    hi += static_cast<int64_t>(static_cast<uint64_t>(x) >> 63);
    return hi;
}

/// @brief Emulate the emitted unsigned magic-divide sequence.
uint64_t emulateUnsignedMagic(uint64_t x, uint64_t d) {
    const auto magic = zanna::codegen::computeUnsignedMagic(d);
    if (!magic.has_value())
        return ~uint64_t{0}; // caller only passes suitable divisors
    uint64_t hi = mulhiU64(x, magic->multiplier);
    if (magic->needsAdd) {
        const uint64_t t = (x - hi) >> 1;
        hi += t;
    }
    if (magic->shift > 0)
        hi >>= magic->shift;
    return hi;
}

const std::vector<int64_t> kSignedDividends = {
    0,
    1,
    -1,
    2,
    -2,
    3,
    -3,
    9,
    -9,
    10,
    -10,
    99,
    -99,
    100,
    -100,
    12345678,
    -12345678,
    50000000,
    2147483647,
    -2147483648LL,
    4611686018427387903LL,
    std::numeric_limits<int64_t>::max(),
    std::numeric_limits<int64_t>::min(),
    std::numeric_limits<int64_t>::max() - 1,
    std::numeric_limits<int64_t>::min() + 1,
};

} // namespace

TEST(MagicDivision, SignedQuotientsMatchReference) {
    const std::vector<int64_t> divisors = {2,    3,    5,     7,     9,       10,   11,
                                           12,   25,   125,   641,   1000,    4095, 4097,
                                           65537, 1LL << 32, (1LL << 62) - 1};
    for (int64_t d : divisors) {
        for (int64_t x : kSignedDividends) {
            const int64_t expected = x / d;
            const int64_t got = emulateSignedMagic(x, d);
            EXPECT_EQ(got, expected);
        }
    }
}

TEST(MagicDivision, UnsignedQuotientsMatchReference) {
    const std::vector<uint64_t> divisors = {3,     5,   7,     9,    10,         11,
                                            12,    25,  125,   641,  1000,       4095,
                                            65537, 3ULL << 32, 0xFFFFFFFFFFFFFFFFULL >> 1,
                                            0xFFFFFFFFFFFFFFFFULL - 1};
    const std::vector<uint64_t> dividends = {0,
                                             1,
                                             2,
                                             9,
                                             10,
                                             99,
                                             12345678,
                                             0x7FFFFFFFULL,
                                             0x100000000ULL,
                                             0x7FFFFFFFFFFFFFFFULL,
                                             0x8000000000000000ULL,
                                             0xFFFFFFFFFFFFFFFFULL,
                                             0xFFFFFFFFFFFFFFFEULL};
    for (uint64_t d : divisors) {
        if ((d & (d - 1)) == 0)
            continue; // powers of two use the shift path, not magic
        for (uint64_t x : dividends) {
            const uint64_t expected = x / d;
            const uint64_t got = emulateUnsignedMagic(x, d);
            EXPECT_EQ(got, expected);
        }
    }
}

TEST(MagicDivision, RejectsUnsuitableDivisors) {
    EXPECT_EQ(zanna::codegen::computeSignedMagic(0).multiplier, 0);
    EXPECT_EQ(zanna::codegen::computeSignedMagic(1).multiplier, 0);
    EXPECT_EQ(zanna::codegen::computeSignedMagic(-5).multiplier, 0);
    EXPECT_FALSE(zanna::codegen::computeUnsignedMagic(0).has_value());
    EXPECT_FALSE(zanna::codegen::computeUnsignedMagic(1).has_value());
    EXPECT_FALSE(zanna::codegen::computeUnsignedMagic(8).has_value()); // pow2
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, argv);
    return zanna_test::run_all_tests();
}
