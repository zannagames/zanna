//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/core/rt_bigint.h
/// @file
/// @brief Declares GC-managed arbitrary-precision signed integer operations.
///
// Purpose: Arbitrary-precision integer arithmetic for the Zanna.Math.BigInt runtime class,
// providing creation, arithmetic, comparison, bitwise, and conversion operations on heap-allocated
// big integer objects.
//
// Key invariants:
//   - All arithmetic operations produce normalized results with no leading zero limbs.
//   - Division by zero traps; callers must guard divisor before calling rt_bigint_div.
//   - Conversion rt_bigint_to_i64 saturates to INT64_MIN/INT64_MAX when the
//     value exceeds int64 range.
//   - String parsing accepts decimal plus 0x/0o/0b-prefixed forms and returns
//     NULL on invalid input.
//
// Ownership/Lifetime:
//   - Returned BigInt objects are newly allocated runtime objects whose digit
//     storage is released by their finalizer.
//   - Callers own returned references and release them through runtime object
//     ownership APIs when no longer needed.
//   - Binary operations do not consume their input arguments.
//   - Null BigInt operands consistently denote numeric zero. Nonnull operands
//     of another runtime class raise a trap.
//
// Links: src/runtime/core/rt_bigint.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//
#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//=========================================================================
// BigInt Creation
//=========================================================================

/// @brief Create a BigInt from a 64-bit integer.
/// @param val The integer value.
/// @return New BigInt object (refcount = 1).
void *rt_bigint_from_i64(int64_t val);

/// @brief Create a BigInt from a string.
/// @param str String representation with optional sign and decimal, `0x`,
///            `0o`, or `0b` radix selection.
/// @return New BigInt object, or `NULL` for null/invalid text.
/// @note Underscores are ignored during the digit scan; surrounding supported
///       ASCII whitespace is accepted.
void *rt_bigint_from_str(rt_string str);

/// @brief Create a BigInt from a byte array (big-endian two's complement).
/// @param bytes Runtime Bytes sequence; null/empty denotes zero.
/// @return New normalized BigInt object.
void *rt_bigint_from_bytes(void *bytes);

/// @brief Create a BigInt representing zero.
/// @return New BigInt object.
void *rt_bigint_zero(void);

/// @brief Create a BigInt representing one.
/// @return New BigInt object.
void *rt_bigint_one(void);

//=========================================================================
// Conversion
//=========================================================================

/// @brief Convert BigInt to 64-bit integer.
/// @param a BigInt to convert; null denotes zero.
/// @return Integer value saturated to INT64_MIN/INT64_MAX when out of range.
int64_t rt_bigint_to_i64(void *a);

/// @brief Convert BigInt to decimal string.
/// @param a BigInt to convert; null denotes zero.
/// @return New decimal string representation.
rt_string rt_bigint_to_str(void *a);

/// @brief Convert BigInt to string in given base.
/// @param a BigInt to convert; null denotes zero.
/// @param base Base (2-36).
/// @return New lowercase string representation in the specified base.
/// @note A base outside 2–36 raises a trap.
rt_string rt_bigint_to_str_base(void *a, int64_t base);

/// @brief Convert BigInt to big-endian byte array.
/// @param a BigInt to convert; null denotes zero.
/// @return New minimally sign-extended two's-complement Bytes object.
void *rt_bigint_to_bytes(void *a);

/// @brief Check if BigInt fits in 64-bit signed integer.
/// @param a BigInt to check; null denotes zero.
/// @return 1 if fits, 0 otherwise.
int8_t rt_bigint_fits_i64(void *a);

//=========================================================================
// Basic Arithmetic
//=========================================================================

/// @brief Add two BigInts.
/// @param a First operand; null denotes zero.
/// @param b Second operand; null denotes zero.
/// @return New BigInt with result.
void *rt_bigint_add(void *a, void *b);

/// @brief Subtract two BigInts (a - b).
/// @param a First operand; null denotes zero.
/// @param b Second operand; null denotes zero.
/// @return New BigInt with result.
void *rt_bigint_sub(void *a, void *b);

/// @brief Multiply two BigInts.
/// @param a First operand; null denotes zero.
/// @param b Second operand; null denotes zero.
/// @return New BigInt with result.
void *rt_bigint_mul(void *a, void *b);

/// @brief Divide two BigInts (a / b), truncated toward zero.
/// @param a Dividend; null denotes zero.
/// @param b Divisor.
/// @return New BigInt with quotient. Traps on division by zero.
void *rt_bigint_div(void *a, void *b);

/// @brief Remainder of division (a % b).
/// @param a Dividend; null denotes zero.
/// @param b Divisor.
/// @return New BigInt with dividend-signed remainder. Traps on division by zero.
void *rt_bigint_mod(void *a, void *b);

/// @brief Divide and get both quotient and remainder.
/// @param a Dividend.
/// @param b Divisor.
/// @param[out] remainder Pointer to store remainder (may be NULL).
/// @return New BigInt with quotient. Traps on division by zero.
/// @note Satisfies `a = quotient*b + remainder` and `|remainder| < |b|`.
void *rt_bigint_divmod(void *a, void *b, void **remainder);

/// @brief Negate a BigInt.
/// @param a Operand; null denotes zero.
/// @return New BigInt with negated value.
void *rt_bigint_neg(void *a);

/// @brief Absolute value.
/// @param a Operand; null denotes zero.
/// @return New BigInt with absolute value.
void *rt_bigint_abs(void *a);

//=========================================================================
// Comparison
//=========================================================================

/// @brief Compare two BigInts.
/// @param a First operand; null denotes zero.
/// @param b Second operand; null denotes zero.
/// @return -1 if a < b, 0 if a == b, 1 if a > b.
int64_t rt_bigint_cmp(void *a, void *b);

/// @brief Check if two BigInts are equal.
/// @param a First operand; null denotes zero.
/// @param b Second operand; null denotes zero.
/// @return 1 if equal, 0 otherwise.
int8_t rt_bigint_eq(void *a, void *b);

/// @brief Check if BigInt is zero.
/// @param a Operand; null denotes zero.
/// @return 1 if zero, 0 otherwise.
int8_t rt_bigint_is_zero(void *a);

/// @brief Check if BigInt is negative.
/// @param a Operand; null denotes zero.
/// @return 1 if negative, 0 otherwise.
int8_t rt_bigint_is_negative(void *a);

/// @brief Get the sign of a BigInt.
/// @param a Operand; null denotes zero.
/// @return -1 if negative, 0 if zero, 1 if positive.
int64_t rt_bigint_sign(void *a);

//=========================================================================
// Bitwise Operations
//=========================================================================

/// @brief Bitwise AND.
/// @param a First operand; null denotes zero.
/// @param b Second operand; null denotes zero.
/// @return New BigInt with result.
/// @note Uses infinite-width two's-complement semantics.
void *rt_bigint_and(void *a, void *b);

/// @brief Bitwise OR.
/// @param a First operand; null denotes zero.
/// @param b Second operand; null denotes zero.
/// @return New BigInt with result.
/// @note Uses infinite-width two's-complement semantics.
void *rt_bigint_or(void *a, void *b);

/// @brief Bitwise XOR.
/// @param a First operand; null denotes zero.
/// @param b Second operand; null denotes zero.
/// @return New BigInt with result.
/// @note Uses infinite-width two's-complement semantics.
void *rt_bigint_xor(void *a, void *b);

/// @brief Bitwise NOT (one's complement).
/// @param a Operand; null denotes zero.
/// @return New BigInt equal to `-(a + 1)`.
void *rt_bigint_not(void *a);

/// @brief Left shift.
/// @param a Value to shift; null denotes zero.
/// @param n Number of bits to shift; nonpositive returns an unchanged clone.
/// @return New BigInt with result.
void *rt_bigint_shl(void *a, int64_t n);

/// @brief Right shift (arithmetic).
/// @param a Value to shift; null denotes zero.
/// @param n Number of bits to shift; nonpositive returns an unchanged clone.
/// @return New BigInt rounded toward negative infinity.
void *rt_bigint_shr(void *a, int64_t n);

//=========================================================================
// Advanced Operations
//=========================================================================

/// @brief Compute power (a^n).
/// @param a Base; null denotes zero.
/// @param n Exponent (must be non-negative).
/// @return New BigInt with result. Traps on negative exponent.
void *rt_bigint_pow(void *a, int64_t n);

/// @brief Compute modular exponentiation (a^n mod m).
/// @param a Base; null denotes zero.
/// @param n Nonnegative BigInt exponent; null denotes zero.
/// @param m Nonzero modulus.
/// @return New least nonnegative residue in `[0, |m|)`.
/// @note Zero modulus and negative exponent raise traps.
void *rt_bigint_pow_mod(void *a, void *n, void *m);

/// @brief Compute greatest common divisor.
/// @param a First operand; null denotes zero.
/// @param b Second operand; null denotes zero.
/// @return New nonnegative `gcd(|a|, |b|)`, including zero for `(0,0)`.
void *rt_bigint_gcd(void *a, void *b);

/// @brief Compute least common multiple.
/// @param a First operand; null denotes zero.
/// @param b Second operand; null denotes zero.
/// @return New nonnegative LCM, or zero when either operand is zero.
void *rt_bigint_lcm(void *a, void *b);

/// @brief Get the number of bits needed to represent the value.
/// @param a Operand; null denotes zero.
/// @return Number of bits (excluding sign).
int64_t rt_bigint_bit_length(void *a);

/// @brief Test a specific bit.
/// @param a Value to test; null denotes zero.
/// @param n Bit index (0 = LSB); negative indexes return false.
/// @return 1 if bit is set, 0 otherwise.
/// @note Uses infinite-width two's-complement sign extension.
int8_t rt_bigint_test_bit(void *a, int64_t n);

/// @brief Set a specific bit.
/// @param a Value; null denotes zero.
/// @param n Bit index (0 = LSB); negative returns an unchanged clone.
/// @return New BigInt with bit set.
/// @note Uses infinite-width two's-complement semantics.
void *rt_bigint_set_bit(void *a, int64_t n);

/// @brief Clear a specific bit.
/// @param a Value; null denotes zero.
/// @param n Bit index (0 = LSB); negative returns an unchanged clone.
/// @return New BigInt with bit cleared.
/// @note Uses infinite-width two's-complement semantics.
void *rt_bigint_clear_bit(void *a, int64_t n);

/// @brief Integer square root (floor).
/// @param a Value (must be non-negative); null denotes zero.
/// @return New BigInt with floor(sqrt(a)). Traps on negative input.
void *rt_bigint_sqrt(void *a);

#ifdef __cplusplus
}
#endif
