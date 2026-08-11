//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_bits.c
/// @file
/// @brief Implements deterministic 64-bit scalar bit-manipulation operations.
///
// Purpose: Implements the Zanna.Bits namespace — low-level bit manipulation
//          operations on 64-bit integers. Covers bitwise AND/OR/XOR/NOT,
//          left/arithmetic-right/logical-right shifts, rotations, population
//          count, leading/trailing zero counts, bit-reverse, byte-swap, and
//          single-bit get/set/clear/toggle.
//
// Key invariants:
//   - Bit positions are numbered 0 (LSB) through 63 (MSB).
//   - Ordinary shifts define explicit out-of-range results; only rotation counts
//     are normalized modulo 64.
//   - Logical right shift treats the operand as unsigned (zero-fill from MSB).
//   - Arithmetic right shift preserves the sign bit (sign-extends).
//   - All functions are pure with no side effects; safe for concurrent use.
//   - MSVC intrinsic headers (<intrin.h>) are included when available to
//     enable hardware popcount and bit-scan instructions.
//
// Ownership/Lifetime:
//   - All functions operate on scalar int64_t values; no allocation is performed.
//   - No state is retained between calls.
//
// Links: src/runtime/core/rt_bits.h (public API)
//
//===----------------------------------------------------------------------===//

#include "rt_bits.h"

#ifdef _MSC_VER
#include <intrin.h>
#endif

// ============================================================================
// Basic Bitwise Operations
// ============================================================================

/// @brief Performs bitwise AND on two integers.
///
/// Returns a value where each bit is 1 only if the corresponding bits in
/// both operands are 1. Commonly used to:
/// - Mask off certain bits (e.g., `val AND 0xFF` to get low byte)
/// - Test if specific bits are set (e.g., `val AND mask != 0`)
/// - Clear specific bits (e.g., `val AND (NOT mask)`)
///
/// **Truth table:**
/// ```
/// A | B | A AND B
/// --+---+--------
/// 0 | 0 |   0
/// 0 | 1 |   0
/// 1 | 0 |   0
/// 1 | 1 |   1
/// ```
///
/// **Usage example:**
/// ```
/// Print Bits.And(0b1100, 0b1010)    ' Outputs: 8 (0b1000)
/// Print Bits.And(255, 0x0F)         ' Outputs: 15 (low nibble)
/// ```
///
/// @param a First operand.
/// @param b Second operand.
///
/// @return The bitwise AND of a and b.
///
/// @note O(1) time complexity.
int64_t rt_bits_and(int64_t a, int64_t b) {
    return a & b;
}

/// @brief Performs bitwise OR on two integers.
///
/// Returns a value where each bit is 1 if either (or both) of the
/// corresponding bits in the operands is 1. Commonly used to:
/// - Set specific bits (e.g., `val OR mask`)
/// - Combine flag values (e.g., `FLAG_A OR FLAG_B`)
///
/// **Truth table:**
/// ```
/// A | B | A OR B
/// --+---+-------
/// 0 | 0 |   0
/// 0 | 1 |   1
/// 1 | 0 |   1
/// 1 | 1 |   1
/// ```
///
/// **Usage example:**
/// ```
/// Print Bits.Or(0b1100, 0b1010)     ' Outputs: 14 (0b1110)
/// Print Bits.Or(0x10, 0x01)         ' Outputs: 17 (0x11)
/// ```
///
/// @param a First operand.
/// @param b Second operand.
///
/// @return The bitwise OR of a and b.
///
/// @note O(1) time complexity.
int64_t rt_bits_or(int64_t a, int64_t b) {
    return a | b;
}

/// @brief Performs bitwise XOR (exclusive OR) on two integers.
///
/// Returns a value where each bit is 1 if exactly one of the corresponding
/// bits in the operands is 1. Commonly used to:
/// - Toggle specific bits (e.g., `val XOR mask`)
/// - Simple encryption/obfuscation (e.g., `data XOR key`)
/// - Swap values without a temporary (`a = a XOR b; b = a XOR b; a = a XOR b`)
///
/// **Truth table:**
/// ```
/// A | B | A XOR B
/// --+---+--------
/// 0 | 0 |   0
/// 0 | 1 |   1
/// 1 | 0 |   1
/// 1 | 1 |   0
/// ```
///
/// **Usage example:**
/// ```
/// Print Bits.Xor(0b1100, 0b1010)    ' Outputs: 6 (0b0110)
/// Print Bits.Xor(val, val)          ' Always 0 (self-XOR)
/// ```
///
/// @param a First operand.
/// @param b Second operand.
///
/// @return The bitwise XOR of a and b.
///
/// @note O(1) time complexity.
/// @note XOR is its own inverse: `(a XOR b) XOR b == a`
int64_t rt_bits_xor(int64_t a, int64_t b) {
    return a ^ b;
}

/// @brief Performs bitwise NOT (complement) on an integer.
///
/// Returns a value where each bit is flipped: 0 becomes 1, and 1 becomes 0.
/// This is equivalent to subtracting the value from -1 (i.e., `NOT x == -1 - x`
/// in two's complement).
///
/// **Examples:**
/// ```
/// NOT 0  = -1 (all bits set)
/// NOT -1 = 0  (all bits cleared)
/// NOT 1  = -2 (0xFFFFFFFFFFFFFFFE)
/// ```
///
/// **Usage example:**
/// ```
/// Print Bits.Not(0)                 ' Outputs: -1
/// Print Bits.Not(-1)                ' Outputs: 0
/// Dim mask = Bits.Not(0xFF)         ' Creates mask 0xFFFFFFFFFFFFFF00
/// ```
///
/// @param val The value to complement.
///
/// @return The bitwise complement of val.
///
/// @note O(1) time complexity.
int64_t rt_bits_not(int64_t val) {
    return ~val;
}

// ============================================================================
// Shift Operations
// ============================================================================

/// @brief Shifts bits left by a specified number of positions.
///
/// Moves all bits toward the most significant position. Vacated positions
/// on the right are filled with zeros. Bits shifted out on the left are lost.
///
/// **Visual example (8-bit simplified):**
/// ```
/// val = 0b00110101 (53)
/// Shl(val, 2) = 0b11010100 (212)
///               ^^-- shifted in zeros
/// ```
///
/// **Multiplication relationship:**
/// `Shl(val, n)` is equivalent to `val * 2^n` (when no overflow occurs).
///
/// **Edge cases:**
/// - count < 0: returns 0
/// - count >= 64: returns 0 (all bits shifted out)
///
/// **Usage example:**
/// ```
/// Print Bits.Shl(1, 3)              ' Outputs: 8 (1 * 2^3)
/// Print Bits.Shl(5, 4)              ' Outputs: 80 (5 * 16)
/// ```
///
/// @param val The value to shift.
/// @param count Number of positions to shift (0-63 for meaningful results).
///
/// @return The left-shifted value, or 0 if count is out of range.
///
/// @note O(1) time complexity.
/// @note Implemented using unsigned arithmetic for well-defined behavior.
///
/// @see rt_bits_shr For arithmetic right shift
/// @see rt_bits_ushr For logical right shift
/// @see rt_bits_rotl For rotation (no bit loss)
int64_t rt_bits_shl(int64_t val, int64_t count) {
    // Clamp count to valid range
    if (count < 0 || count >= 64)
        return 0;
    // Cast to unsigned for well-defined behavior, then back to signed
    return (int64_t)((uint64_t)val << count);
}

/// @brief Arithmetic right shift (sign-preserving).
///
/// Moves all bits toward the least significant position. Vacated positions
/// on the left are filled with copies of the sign bit (bit 63), preserving
/// the sign of negative numbers.
///
/// **Visual example (8-bit simplified):**
/// ```
/// Positive: 0b01100100 (100)
/// Shr(100, 2) = 0b00011001 (25)
///               ^^-- sign bit (0) replicated
///
/// Negative: 0b11001000 (-56 in two's complement)
/// Shr(-56, 2) = 0b11110010 (-14)
///               ^^-- sign bit (1) replicated
/// ```
///
/// **Division relationship:**
/// For positive numbers, `Shr(val, n)` equals `val / 2^n` (integer division).
/// For negative numbers, it rounds toward negative infinity.
///
/// **Edge cases:**
/// - count < 0: returns val unchanged
/// - count >= 64: returns -1 (if negative) or 0 (if non-negative)
///
/// **Usage example:**
/// ```
/// Print Bits.Shr(100, 2)            ' Outputs: 25
/// Print Bits.Shr(-100, 2)           ' Outputs: -25
/// ```
///
/// @param val The value to shift.
/// @param count Number of positions to shift.
///
/// @return The arithmetic right-shifted value.
///
/// @note O(1) time complexity.
/// @note Preserves sign by replicating the sign bit.
///
/// @see rt_bits_ushr For logical right shift (zero-fill)
/// @see rt_bits_shl For left shift
int64_t rt_bits_shr(int64_t val, int64_t count) {
    if (count < 0)
        return val;
    if (count >= 64)
        return (val < 0) ? -1 : 0;
    uint64_t u = (uint64_t)val;
    uint64_t shifted = u >> (unsigned)count;
    if (val < 0 && count > 0) {
        uint64_t sign_mask = UINT64_MAX << (64 - (unsigned)count);
        shifted |= sign_mask;
    }
    return (int64_t)shifted;
}

/// @brief Logical right shift (zero-fill).
///
/// Moves all bits toward the least significant position. Vacated positions
/// on the left are always filled with zeros, regardless of the sign bit.
/// This treats the value as an unsigned 64-bit quantity.
///
/// **Visual example (8-bit simplified):**
/// ```
/// Negative: 0b11001000 (-56 in two's complement = 200 unsigned)
/// UShr(-56, 2) = 0b00110010 (50)
///                ^^-- zeros shifted in (not sign bits)
/// ```
///
/// **Difference from Shr:**
/// ```
/// val = -8 (0xFFFFFFFFFFFFFFF8)
/// Shr(-8, 2)  = -2 (sign-extended: 0xFFFFFFFFFFFFFFFE)
/// UShr(-8, 2) = 4611686018427387902 (zero-filled: 0x3FFFFFFFFFFFFFFE)
/// ```
///
/// **Edge cases:**
/// - count < 0: returns 0
/// - count >= 64: returns 0
///
/// **Usage example:**
/// ```
/// Print Bits.UShr(100, 2)           ' Outputs: 25 (same as Shr)
/// Print Bits.UShr(-8, 2)            ' Outputs: 4611686018427387902
/// ```
///
/// @param val The value to shift.
/// @param count Number of positions to shift (0-63 for meaningful results).
///
/// @return The logical right-shifted value, or 0 if count is out of range.
///
/// @note O(1) time complexity.
/// @note Treats the value as unsigned (no sign extension).
///
/// @see rt_bits_shr For arithmetic right shift (sign-preserving)
/// @see rt_bits_shl For left shift
int64_t rt_bits_ushr(int64_t val, int64_t count) {
    // Logical shift right (zero-fill)
    if (count < 0 || count >= 64)
        return 0;
    return (int64_t)((uint64_t)val >> count);
}

// ============================================================================
// Rotate Operations
// ============================================================================

/// @brief Rotates bits left by a specified number of positions.
///
/// Circular shift where bits shifted out of the left side wrap around
/// to the right side. Unlike shift operations, no bits are lost.
///
/// **Visual example (8-bit simplified):**
/// ```
/// val = 0b11010010
/// RotL(val, 3) = 0b10010110
///                   ^^^-- these bits wrapped from left to right
/// ```
///
/// **Rotation property:**
/// `RotL(val, n)` followed by `RotR(val, n)` returns the original value.
/// `RotL(val, 64)` returns the original value (full rotation).
///
/// **Usage example:**
/// ```
/// Print Bits.RotL(0x0123456789ABCDEF, 8)
/// ' Outputs: 0x23456789ABCDEF01 (first byte moved to end)
/// ```
///
/// @param val The value to rotate.
/// @param count Number of positions to rotate. Automatically normalized
///              to the range 0-63 using modulo (count AND 63).
///
/// @return The left-rotated value.
///
/// @note O(1) time complexity.
/// @note count is always normalized to 0-63, so negative and large values work.
///
/// @see rt_bits_rotr For right rotation
/// @see rt_bits_shl For shift (with bit loss)
int64_t rt_bits_rotl(int64_t val, int64_t count) {
    uint64_t u = (uint64_t)val;
    // Normalize count to 0-63
    int shift = (int)((uint64_t)count & 63u);
    if (shift == 0)
        return val;
    return (int64_t)((u << shift) | (u >> (64 - shift)));
}

/// @brief Rotates bits right by a specified number of positions.
///
/// Circular shift where bits shifted out of the right side wrap around
/// to the left side. Unlike shift operations, no bits are lost.
///
/// **Visual example (8-bit simplified):**
/// ```
/// val = 0b11010010
/// RotR(val, 3) = 0b01011010
///                ^^^-- these bits wrapped from right to left
/// ```
///
/// **Usage example:**
/// ```
/// Print Bits.RotR(0x0123456789ABCDEF, 8)
/// ' Outputs: 0xEF0123456789ABCD (last byte moved to front)
/// ```
///
/// @param val The value to rotate.
/// @param count Number of positions to rotate. Automatically normalized
///              to the range 0-63 using modulo (count AND 63).
///
/// @return The right-rotated value.
///
/// @note O(1) time complexity.
/// @note count is always normalized to 0-63, so negative and large values work.
///
/// @see rt_bits_rotl For left rotation
/// @see rt_bits_shr For shift (with bit loss or sign extension)
int64_t rt_bits_rotr(int64_t val, int64_t count) {
    uint64_t u = (uint64_t)val;
    // Normalize count to 0-63
    int shift = (int)((uint64_t)count & 63u);
    if (shift == 0)
        return val;
    return (int64_t)((u >> shift) | (u << (64 - shift)));
}

// ============================================================================
// Bit Counting Operations
// ============================================================================

/// @brief Counts the number of set bits (population count / Hamming weight).
///
/// Returns the number of bits that are 1 in the value. Also known as
/// "popcount" or "Hamming weight". Useful for:
/// - Counting flags that are set
/// - Calculating parity (odd/even count of 1s)
/// - Bitboard operations in games (chess, checkers)
///
/// **Examples:**
/// ```
/// Count(0b00000000) = 0
/// Count(0b00000001) = 1
/// Count(0b11111111) = 8
/// Count(0xFF00FF00FF00FF00) = 32
/// Count(-1) = 64 (all bits set)
/// ```
///
/// **Usage example:**
/// ```
/// Dim flags = 0b10110101
/// Print Bits.Count(flags)           ' Outputs: 5 (five bits set)
/// ```
///
/// @param val The value to count bits in.
///
/// @return The number of 1 bits (0 to 64).
///
/// @note O(1) time complexity (uses hardware instruction when available).
/// @note Implementation uses compiler intrinsics on GCC/Clang/MSVC, with
///       portable fallback using parallel counting algorithm.
int64_t rt_bits_count(int64_t val) {
    // Population count (number of 1 bits)
// The compiler builtin is used only where it expands inline: without a hardware
// population-count instruction GCC lowers it to libgcc's __popcountdi2, and
// Zanna's native linker does not pull compiler runtime archive members.
#if (defined(__GNUC__) || defined(__clang__)) &&                                                   \
    (defined(__POPCNT__) || defined(__aarch64__) || defined(__ARM_NEON))
    return (int64_t)__builtin_popcountll((unsigned long long)val);
#elif defined(_MSC_VER)
    return (int64_t)__popcnt64((unsigned __int64)val);
#else
    // Portable fallback using parallel bit counting
    uint64_t u = (uint64_t)val;
    u = u - ((u >> 1) & 0x5555555555555555ULL);
    u = (u & 0x3333333333333333ULL) + ((u >> 2) & 0x3333333333333333ULL);
    u = (u + (u >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return (int64_t)((u * 0x0101010101010101ULL) >> 56);
#endif
}

/// @brief Counts the number of leading zero bits.
///
/// Returns the number of consecutive zero bits starting from the most
/// significant bit (bit 63) until the first 1 bit is encountered.
/// Also known as "clz" (count leading zeros).
///
/// **Relationship to bit width:**
/// `LeadZ(val)` tells you how many of the top bits are unused. The
/// effective bit width is `64 - LeadZ(val)`.
///
/// **Examples:**
/// ```
/// LeadZ(0) = 64                    (all zeros)
/// LeadZ(1) = 63                    (0x0000000000000001)
/// LeadZ(255) = 56                  (0x00000000000000FF)
/// LeadZ(0x8000000000000000) = 0    (MSB is set)
/// LeadZ(-1) = 0                    (all bits set)
/// ```
///
/// **Usage example:**
/// ```
/// Dim val = 1000
/// Dim width = 64 - Bits.LeadZ(val)
/// Print "Value fits in " + Str(width) + " bits"    ' "10 bits"
/// ```
///
/// @param val The value to examine.
///
/// @return The number of leading zero bits (0 to 64).
///
/// @note O(1) time complexity (uses hardware instruction when available).
/// @note Returns 64 for val = 0 (all bits are zero).
///
/// @see rt_bits_trailz For counting trailing zeros
int64_t rt_bits_leadz(int64_t val) {
    // Count leading zeros
    if (val == 0)
        return 64;
#if defined(__GNUC__) || defined(__clang__)
    return (int64_t)__builtin_clzll((unsigned long long)val);
#elif defined(_MSC_VER)
    unsigned long idx;
    if (_BitScanReverse64(&idx, (unsigned __int64)val)) {
        return (int64_t)(63 - idx);
    }
    return 64;
#else
    // Portable fallback using binary search
    uint64_t u = (uint64_t)val;
    int64_t n = 0;
    if ((u & 0xFFFFFFFF00000000ULL) == 0) {
        n += 32;
        u <<= 32;
    }
    if ((u & 0xFFFF000000000000ULL) == 0) {
        n += 16;
        u <<= 16;
    }
    if ((u & 0xFF00000000000000ULL) == 0) {
        n += 8;
        u <<= 8;
    }
    if ((u & 0xF000000000000000ULL) == 0) {
        n += 4;
        u <<= 4;
    }
    if ((u & 0xC000000000000000ULL) == 0) {
        n += 2;
        u <<= 2;
    }
    if ((u & 0x8000000000000000ULL) == 0) {
        n += 1;
    }
    return n;
#endif
}

/// @brief Counts the number of trailing zero bits.
///
/// Returns the number of consecutive zero bits starting from the least
/// significant bit (bit 0) until the first 1 bit is encountered.
/// Also known as "ctz" (count trailing zeros).
///
/// **Relationship to powers of 2:**
/// For powers of 2, `TrailZ(val)` gives the exponent: `2^n` has n trailing zeros.
/// This is useful for efficiently computing `log2` for powers of 2.
///
/// **Examples:**
/// ```
/// TrailZ(0) = 64                   (all zeros)
/// TrailZ(1) = 0                    (0x0000000000000001)
/// TrailZ(2) = 1                    (0x0000000000000002)
/// TrailZ(8) = 3                    (0x0000000000000008)
/// TrailZ(0x8000000000000000) = 63  (only MSB set)
/// TrailZ(12) = 2                   (0b1100, two trailing zeros)
/// ```
///
/// **Usage example:**
/// ```
/// Dim alignment = 256
/// Dim log2 = Bits.TrailZ(alignment)
/// Print "Alignment is 2^" + Str(log2)    ' "2^8"
/// ```
///
/// @param val The value to examine.
///
/// @return The number of trailing zero bits (0 to 64).
///
/// @note O(1) time complexity (uses hardware instruction when available).
/// @note Returns 64 for val = 0 (all bits are zero).
///
/// @see rt_bits_leadz For counting leading zeros
int64_t rt_bits_trailz(int64_t val) {
    // Count trailing zeros
    if (val == 0)
        return 64;
#if defined(__GNUC__) || defined(__clang__)
    return (int64_t)__builtin_ctzll((unsigned long long)val);
#elif defined(_MSC_VER)
    unsigned long idx;
    if (_BitScanForward64(&idx, (unsigned __int64)val)) {
        return (int64_t)idx;
    }
    return 64;
#else
    // Portable fallback using binary search
    uint64_t u = (uint64_t)val;
    int64_t n = 0;
    if ((u & 0x00000000FFFFFFFFULL) == 0) {
        n += 32;
        u >>= 32;
    }
    if ((u & 0x000000000000FFFFULL) == 0) {
        n += 16;
        u >>= 16;
    }
    if ((u & 0x00000000000000FFULL) == 0) {
        n += 8;
        u >>= 8;
    }
    if ((u & 0x000000000000000FULL) == 0) {
        n += 4;
        u >>= 4;
    }
    if ((u & 0x0000000000000003ULL) == 0) {
        n += 2;
        u >>= 2;
    }
    if ((u & 0x0000000000000001ULL) == 0) {
        n += 1;
    }
    return n;
#endif
}

// ============================================================================
// Bit Manipulation Operations
// ============================================================================

/// @brief Reverses the order of all 64 bits.
///
/// Reflects the bit pattern so that bit 0 becomes bit 63, bit 1 becomes
/// bit 62, and so on. Useful for:
/// - Working with LSB-first vs MSB-first data
/// - CRC calculations
/// - Certain FFT algorithms
///
/// **Visual example (8-bit simplified):**
/// ```
/// val = 0b11010010
/// Flip(val) = 0b01001011
///             ^      ^
///             |      |
///             bit 7  bit 0 swapped
/// ```
///
/// **Usage example:**
/// ```
/// Dim val = 0x0F0F0F0F0F0F0F0F
/// Dim rev = Bits.Flip(val)
/// ' rev = 0xF0F0F0F0F0F0F0F0
/// ```
///
/// @param val The value to bit-reverse.
///
/// @return The bit-reversed value.
///
/// @note O(1) time complexity (constant number of operations).
/// @note Self-inverse: `Flip(Flip(val)) == val`
///
/// @see rt_bits_swap For byte-level reversal (endian swap)
int64_t rt_bits_flip(int64_t val) {
    // Reverse all 64 bits
    uint64_t u = (uint64_t)val;
    // Swap adjacent bits
    u = ((u >> 1) & 0x5555555555555555ULL) | ((u & 0x5555555555555555ULL) << 1);
    // Swap adjacent 2-bit pairs
    u = ((u >> 2) & 0x3333333333333333ULL) | ((u & 0x3333333333333333ULL) << 2);
    // Swap adjacent 4-bit nibbles
    u = ((u >> 4) & 0x0F0F0F0F0F0F0F0FULL) | ((u & 0x0F0F0F0F0F0F0F0FULL) << 4);
    // Swap adjacent bytes
    u = ((u >> 8) & 0x00FF00FF00FF00FFULL) | ((u & 0x00FF00FF00FF00FFULL) << 8);
    // Swap adjacent 2-byte pairs
    u = ((u >> 16) & 0x0000FFFF0000FFFFULL) | ((u & 0x0000FFFF0000FFFFULL) << 16);
    // Swap adjacent 4-byte pairs
    u = (u >> 32) | (u << 32);
    return (int64_t)u;
}

/// @brief Reverses the order of all 8 bytes (endian swap).
///
/// Swaps the byte order of a 64-bit value, converting between big-endian
/// and little-endian representations. Byte 0 becomes byte 7, byte 1 becomes
/// byte 6, and so on. Bits within each byte remain in the same order.
///
/// **Visual example:**
/// ```
/// val = 0x0123456789ABCDEF
/// Swap(val) = 0xEFCDAB8967452301
///
/// Bytes before: [01][23][45][67][89][AB][CD][EF]
/// Bytes after:  [EF][CD][AB][89][67][45][23][01]
/// ```
///
/// **Common uses:**
/// - Converting network byte order (big-endian) to host byte order
/// - Reading binary files with different endianness
/// - Protocol conversions
///
/// **Usage example:**
/// ```
/// Dim netValue = Bits.Swap(hostValue)    ' Host to network order
/// Dim hostValue = Bits.Swap(netValue)    ' Network to host order
/// ```
///
/// @param val The value to byte-swap.
///
/// @return The byte-swapped value.
///
/// @note O(1) time complexity (uses hardware instruction when available).
/// @note Self-inverse: `Swap(Swap(val)) == val`
///
/// @see rt_bits_flip For bit-level reversal
int64_t rt_bits_swap(int64_t val) {
    // Byte swap (endian swap)
#if defined(__GNUC__) || defined(__clang__)
    return (int64_t)__builtin_bswap64((uint64_t)val);
#elif defined(_MSC_VER)
    return (int64_t)_byteswap_uint64((unsigned __int64)val);
#else
    uint64_t u = (uint64_t)val;
    u = ((u >> 8) & 0x00FF00FF00FF00FFULL) | ((u & 0x00FF00FF00FF00FFULL) << 8);
    u = ((u >> 16) & 0x0000FFFF0000FFFFULL) | ((u & 0x0000FFFF0000FFFFULL) << 16);
    u = (u >> 32) | (u << 32);
    return (int64_t)u;
#endif
}

// ============================================================================
// Single Bit Operations
// ============================================================================

/// @brief Tests whether a specific bit is set.
///
/// Examines a single bit at the specified position and returns whether
/// it is 1 (true) or 0 (false). Bit positions are numbered 0-63, where
/// bit 0 is the least significant bit.
///
/// **Bit position examples:**
/// ```
/// val = 0b1010 (10 decimal)
/// Get(val, 0) = false  (bit 0 is 0)
/// Get(val, 1) = true   (bit 1 is 1)
/// Get(val, 2) = false  (bit 2 is 0)
/// Get(val, 3) = true   (bit 3 is 1)
/// ```
///
/// **Usage example:**
/// ```
/// Dim flags = 0b10110
/// If Bits.Get(flags, 2) Then
///     Print "Flag 2 is set"
/// End If
/// ```
///
/// @param val The value to test.
/// @param bit The bit position to test (0-63).
///
/// @return True if the bit is set (1), false if cleared (0) or if bit
///         position is out of range.
///
/// @note O(1) time complexity.
/// @note Returns false for out-of-range bit positions (< 0 or >= 64).
///
/// @see rt_bits_set For setting a bit to 1
/// @see rt_bits_clear For clearing a bit to 0
/// @see rt_bits_toggle For flipping a bit
int8_t rt_bits_get(int64_t val, int64_t bit) {
    // Return 1 if bit is set (bit position 0-63)
    if (bit < 0 || bit >= 64)
        return 0;
    return ((uint64_t)val >> bit) & 1;
}

/// @brief Sets a specific bit to 1.
///
/// Returns a new value with the specified bit set to 1. All other bits
/// remain unchanged. This is equivalent to `val OR (1 << bit)`.
///
/// **Usage example:**
/// ```
/// Dim flags = 0
/// flags = Bits.Set(flags, 0)    ' Set bit 0: flags = 1
/// flags = Bits.Set(flags, 2)    ' Set bit 2: flags = 5 (0b101)
/// flags = Bits.Set(flags, 2)    ' Already set, no change: flags = 5
/// ```
///
/// @param val The original value.
/// @param bit The bit position to set (0-63).
///
/// @return A new value with the specified bit set to 1, or the original
///         value unchanged if bit position is out of range.
///
/// @note O(1) time complexity.
/// @note Idempotent: setting an already-set bit has no effect.
/// @note Does not modify val; returns a new value.
///
/// @see rt_bits_clear For clearing a bit to 0
/// @see rt_bits_toggle For flipping a bit
/// @see rt_bits_get For testing if a bit is set
int64_t rt_bits_set(int64_t val, int64_t bit) {
    // Return val with bit set
    if (bit < 0 || bit >= 64)
        return val;
    return (int64_t)((uint64_t)val | (1ULL << bit));
}

/// @brief Clears a specific bit to 0.
///
/// Returns a new value with the specified bit cleared to 0. All other bits
/// remain unchanged. This is equivalent to `val AND NOT(1 << bit)`.
///
/// **Usage example:**
/// ```
/// Dim flags = 0b11111111
/// flags = Bits.Clear(flags, 0)    ' Clear bit 0: flags = 0b11111110
/// flags = Bits.Clear(flags, 7)    ' Clear bit 7: flags = 0b01111110
/// ```
///
/// @param val The original value.
/// @param bit The bit position to clear (0-63).
///
/// @return A new value with the specified bit cleared to 0, or the original
///         value unchanged if bit position is out of range.
///
/// @note O(1) time complexity.
/// @note Idempotent: clearing an already-cleared bit has no effect.
/// @note Does not modify val; returns a new value.
///
/// @see rt_bits_set For setting a bit to 1
/// @see rt_bits_toggle For flipping a bit
/// @see rt_bits_get For testing if a bit is set
int64_t rt_bits_clear(int64_t val, int64_t bit) {
    // Return val with bit cleared
    if (bit < 0 || bit >= 64)
        return val;
    return (int64_t)((uint64_t)val & ~(1ULL << bit));
}

/// @brief Toggles (flips) a specific bit.
///
/// Returns a new value with the specified bit inverted: if it was 0, it
/// becomes 1; if it was 1, it becomes 0. All other bits remain unchanged.
/// This is equivalent to `val XOR (1 << bit)`.
///
/// **Usage example:**
/// ```
/// Dim flags = 0b1010
/// flags = Bits.Toggle(flags, 0)    ' Toggle bit 0: flags = 0b1011
/// flags = Bits.Toggle(flags, 0)    ' Toggle again: flags = 0b1010
/// flags = Bits.Toggle(flags, 1)    ' Toggle bit 1: flags = 0b1000
/// ```
///
/// @param val The original value.
/// @param bit The bit position to toggle (0-63).
///
/// @return A new value with the specified bit toggled, or the original
///         value unchanged if bit position is out of range.
///
/// @note O(1) time complexity.
/// @note Self-inverse: toggling the same bit twice returns the original value.
/// @note Does not modify val; returns a new value.
///
/// @see rt_bits_set For unconditionally setting a bit to 1
/// @see rt_bits_clear For unconditionally clearing a bit to 0
/// @see rt_bits_get For testing if a bit is set
int64_t rt_bits_toggle(int64_t val, int64_t bit) {
    // Return val with bit flipped
    if (bit < 0 || bit >= 64)
        return val;
    return (int64_t)((uint64_t)val ^ (1ULL << bit));
}
