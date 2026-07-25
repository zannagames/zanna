//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/core/rt_bits.h
/// @file
/// @brief Declares deterministic 64-bit scalar bit-manipulation operations.
///
// Purpose: Bit manipulation utilities implementing the Zanna.Bits runtime namespace, providing
// bitwise operations, shifts, population count, and bit scanning on 64-bit integer values.
//
// Key invariants:
//   - All operations treat values as 64-bit two's complement integers.
//   - Shift edge cases are explicitly defined and never rely on C's undefined
//     out-of-range shift behavior; rotations normalize counts modulo 64.
//   - rt_bits_count uses a platform intrinsic when available.
//   - rt_bits_leadz and rt_bits_trailz return 64 for input value 0.
//
// Ownership/Lifetime:
//   - All functions are pure with no heap allocation or side effects.
//   - No ownership transfer; parameters and return values are plain int64_t.
//
// Links: src/runtime/core/rt_bits.c (implementation)
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Bitwise AND of two values.
/// @param a Left-hand operand.
/// @param b Right-hand operand.
/// @return The bitwise conjunction (a & b).
int64_t rt_bits_and(int64_t a, int64_t b);

/// @brief Bitwise OR of two values.
/// @param a Left-hand operand.
/// @param b Right-hand operand.
/// @return The bitwise disjunction (a | b).
int64_t rt_bits_or(int64_t a, int64_t b);

/// @brief Bitwise XOR of two values.
/// @param a Left-hand operand.
/// @param b Right-hand operand.
/// @return The bitwise exclusive-or (a ^ b).
int64_t rt_bits_xor(int64_t a, int64_t b);

/// @brief Bitwise NOT of a value.
/// @param val The operand to invert.
/// @return The one's complement (~val).
int64_t rt_bits_not(int64_t val);

/// @brief Logical shift left.
/// @param val The value to shift.
/// @param count Number of bit positions to shift left (0-63).
/// @return The value with bits shifted left by @p count positions,
///         with vacated positions filled with zeros; returns zero when
///         @p count is negative or at least 64.
int64_t rt_bits_shl(int64_t val, int64_t count);

/// @brief Arithmetic shift right (sign-extended).
/// @param val The value to shift.
/// @param count Number of bit positions to shift right (0-63).
/// @return The value with bits shifted right by @p count positions,
///         with vacated positions filled with copies of the sign bit.
/// @note A negative count returns @p val unchanged; a count of at least 64
///       returns `-1` for a negative value and zero otherwise.
int64_t rt_bits_shr(int64_t val, int64_t count);

/// @brief Logical shift right (zero-fill).
/// @param val The value to shift.
/// @param count Number of bit positions to shift right (0-63).
/// @return The value with bits shifted right by @p count positions,
///         with vacated positions filled with zeros; returns zero when
///         @p count is negative or at least 64.
int64_t rt_bits_ushr(int64_t val, int64_t count);

/// @brief Rotate left.
/// @param val The value to rotate.
/// @param count Number of positions; only the low six bits are used.
/// @return The value with bits circularly shifted left by @p count positions.
int64_t rt_bits_rotl(int64_t val, int64_t count);

/// @brief Rotate right.
/// @param val The value to rotate.
/// @param count Number of positions; only the low six bits are used.
/// @return The value with bits circularly shifted right by @p count positions.
int64_t rt_bits_rotr(int64_t val, int64_t count);

/// @brief Population count (number of 1 bits).
/// @param val The value to examine.
/// @return The number of bits set to 1 in @p val (Hamming weight).
int64_t rt_bits_count(int64_t val);

/// @brief Count leading zeros.
/// @param val The value to examine.
/// @return The number of consecutive zero bits starting from the most
///         significant bit. Returns 64 if @p val is zero.
int64_t rt_bits_leadz(int64_t val);

/// @brief Count trailing zeros.
/// @param val The value to examine.
/// @return The number of consecutive zero bits starting from the least
///         significant bit. Returns 64 if @p val is zero.
int64_t rt_bits_trailz(int64_t val);

/// @brief Reverse all 64 bits.
/// @param val The value whose bits are reversed.
/// @return A value where bit 0 of @p val becomes bit 63, bit 1 becomes
///         bit 62, and so on.
int64_t rt_bits_flip(int64_t val);

/// @brief Byte swap (endian swap).
/// @param val The value whose bytes are reversed.
/// @return The value with byte order reversed (converts between
///         big-endian and little-endian representations).
int64_t rt_bits_swap(int64_t val);

/// @brief Get bit at position (0-63).
/// @param val The value to examine.
/// @param bit The zero-based bit position to read (0 = LSB, 63 = MSB).
/// @return 1 if the bit at position @p bit is set, 0 otherwise.
int8_t rt_bits_get(int64_t val, int64_t bit);

/// @brief Set bit at position (0-63).
/// @param val The original value.
/// @param bit The zero-based bit position to set (0 = LSB, 63 = MSB).
/// @return The value with the bit at position @p bit set to 1.
/// @note Out-of-range positions return @p val unchanged.
int64_t rt_bits_set(int64_t val, int64_t bit);

/// @brief Clear bit at position (0-63).
/// @param val The original value.
/// @param bit The zero-based bit position to clear (0 = LSB, 63 = MSB).
/// @return The value with the bit at position @p bit set to 0.
/// @note Out-of-range positions return @p val unchanged.
int64_t rt_bits_clear(int64_t val, int64_t bit);

/// @brief Toggle bit at position (0-63).
/// @param val The original value.
/// @param bit The zero-based bit position to toggle (0 = LSB, 63 = MSB).
/// @return The value with the bit at position @p bit flipped.
/// @note Out-of-range positions return @p val unchanged.
int64_t rt_bits_toggle(int64_t val, int64_t bit);

#ifdef __cplusplus
}
#endif
