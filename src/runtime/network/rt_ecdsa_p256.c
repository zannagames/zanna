//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_ecdsa_p256.c
// Purpose: Native ECDSA P-256 (secp256r1) signature verification and
//          signing in pure C.
//          Implements 256-bit modular field arithmetic, Jacobian projective
//          elliptic curve point operations, and the ECDSA verify algorithm.
// Key invariants:
//   - Field elements are uint64_t[4] in big-endian limb order (limb[0] = MSW).
//   - Point-at-infinity represented by Z == 0 in Jacobian coordinates.
//   - No heap allocation; all temporaries are stack-local.
//   - Public verification uses variable-time scalar multiplication.
//   - Private-key paths use a fixed scalar-multiplication schedule; this
//     reduces obvious secret-bit branching but is not a formal side-channel proof.
// Ownership/Lifetime:
//   - Pure functions; no state, no side effects beyond output parameters.
// Links: rt_ecdsa_p256.h (API), rt_tls.c (caller)
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements pure-C ECDSA and ECDH over NIST P-256.
 * @details Provides portable multi-precision field and scalar arithmetic,
 * Jacobian point operations, public verification, fixed-schedule private
 * scalar multiplication, signing, public-key derivation, and shared-secret
 * computation without heap allocation.
 */

#include "rt_ecdsa_p256.h"
#include "rt_crypto.h"
#include <string.h>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#if defined(__GNUC__) || defined(__clang__)
/** Marks portability helpers that selected compiler paths may not reference. */
#define ECDSA_P256_MAYBE_UNUSED __attribute__((unused))
#else
/** Empty unused-helper annotation for compilers without GNU attributes. */
#define ECDSA_P256_MAYBE_UNUSED
#endif

/// @brief Volatile zero-fill that the compiler cannot optimise away.
/// @details Used to scrub secret material (private scalars, intermediate
///          field elements) from stack and heap memory before it falls out
///          of scope. The volatile-pointer write per byte forces the compiler
///          to actually emit the stores even when it can prove the buffer is
///          dead. Equivalent to `memset_s` / `explicit_bzero` on platforms
///          that have them.
/// @param ptr Buffer to zero (may be `NULL` when @p len is 0).
/// @param len Number of bytes to clear.
static void ecdsa_secure_zero(void *ptr, size_t len) {
    volatile uint8_t *p = (volatile uint8_t *)ptr;
    while (len-- > 0)
        *p++ = 0;
}

/// @brief Count leading zero bits in a 64-bit word.
/// @param value Word to inspect.
/// @return Value in 0 through 64; zero input returns 64.
static int ecdsa_clz64(uint64_t value) {
    if (value == 0)
        return 64;
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
    unsigned long index = 0;
    _BitScanReverse64(&index, value);
    return 63 - (int)index;
#elif defined(_MSC_VER)
    unsigned long index = 0;
    uint32_t high = (uint32_t)(value >> 32);
    if (high != 0) {
        _BitScanReverse(&index, high);
        return 31 - (int)index;
    }
    _BitScanReverse(&index, (uint32_t)value);
    return 63 - (int)index;
#elif defined(__GNUC__) || defined(__clang__)
    return __builtin_clzll((unsigned long long)value);
#else
    int count = 0;
    uint64_t bit = UINT64_C(1) << 63;
    while ((value & bit) == 0) {
        count++;
        bit >>= 1;
    }
    return count;
#endif
}

//=============================================================================
// 256-bit unsigned integer type (big-endian limb order: [0]=MSW, [3]=LSW)
//=============================================================================

/** Unsigned 256-bit integer in four most-significant-first 64-bit limbs. */
typedef uint64_t u256[4];

// 512-bit for multiplication intermediate
/** Unsigned 512-bit multiplication intermediate in eight big-endian limbs. */
typedef uint64_t u512[8];

/// @brief Add two 64-bit values with an incoming carry and write the result to *out.
/// @param a First addend.
/// @param b Second addend.
/// @param carry Incoming carry bit.
/// @param out Receives the wrapped 64-bit sum.
/// @return 1 if the addition overflowed (carry out), 0 otherwise.
static uint64_t u64_add_with_carry(uint64_t a, uint64_t b, uint64_t carry, uint64_t *out) {
    uint64_t sum = a + b;
    uint64_t carry1 = (sum < a);
    uint64_t sum2 = sum + carry;
    uint64_t carry2 = (sum2 < sum);
    *out = sum2;
    return carry1 | carry2;
}

/// @brief Subtract b + borrow from a, write the result to *out.
/// @param a Minuend.
/// @param b Subtrahend.
/// @param borrow Incoming borrow bit.
/// @param out Receives the wrapped 64-bit difference.
/// @return 1 if the subtraction underflowed (borrow out), 0 otherwise.
static uint64_t u64_sub_with_borrow(uint64_t a, uint64_t b, uint64_t borrow, uint64_t *out) {
    uint64_t subtrahend = b + borrow;
    uint64_t borrow1 = (subtrahend < b);
    uint64_t borrow2 = (a < subtrahend);
    *out = a - subtrahend;
    return borrow1 | borrow2;
}

/// @brief Subtract a small 64-bit value from a 256-bit integer in-place, propagating borrow.
/// @param value Mutable 256-bit big-endian-limb integer.
/// @param small 64-bit value to subtract.
static void u256_sub_small_inplace(u256 value, uint64_t small) {
    uint64_t borrow = u64_sub_with_borrow(value[3], small, 0, &value[3]);
    for (int i = 2; i >= 0 && borrow; i--)
        borrow = u64_sub_with_borrow(value[i], 0, borrow, &value[i]);
}

//=============================================================================
// P-256 curve constants (NIST FIPS 186-4 / SEC 2)
//=============================================================================

// Field prime p = 2^256 - 2^224 + 2^192 + 2^96 - 1
/** P-256 prime field modulus. */
static const u256 P256_P = {
    0xFFFFFFFF00000001ULL, 0x0000000000000000ULL, 0x00000000FFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL};

// Curve order n
/** Prime order of the P-256 generator subgroup. */
static const u256 P256_N = {
    0xFFFFFFFF00000000ULL, 0xFFFFFFFFFFFFFFFFULL, 0xBCE6FAADA7179E84ULL, 0xF3B9CAC2FC632551ULL};

// Curve parameter a = -3 mod p = p - 3
/** P-256 curve coefficient a reduced modulo @ref P256_P. */
static const u256 P256_A = {
    0xFFFFFFFF00000001ULL, 0x0000000000000000ULL, 0x00000000FFFFFFFFULL, 0xFFFFFFFFFFFFFFFCULL};

// Curve parameter b
/** P-256 curve coefficient b. */
static const u256 P256_B = {
    0x5AC635D8AA3A93E7ULL, 0xB3EBBD55769886BCULL, 0x651D06B0CC53B0F6ULL, 0x3BCE3C3E27D2604BULL};

// Generator point G (affine x coordinate)
/** Affine x coordinate of the standard P-256 generator. */
static const u256 P256_GX = {
    0x6B17D1F2E12C4247ULL, 0xF8BCE6E563A440F2ULL, 0x77037D812DEB33A0ULL, 0xF4A13945D898C296ULL};

// Generator point G (affine y coordinate)
/** Affine y coordinate of the standard P-256 generator. */
static const u256 P256_GY = {
    0x4FE342E2FE1A7F9BULL, 0x8EE7EB4A7C0F9E16ULL, 0x2BCE33576B315ECEULL, 0xCBB6406837BF51F5ULL};

//=============================================================================
// 256-bit arithmetic helpers
//=============================================================================

/// @brief Zero-fill a 256-bit integer (4 × 64-bit big-endian limbs).
/// @param r Output integer to clear.
static void u256_zero(u256 r) {
    r[0] = r[1] = r[2] = r[3] = 0;
}

/// @brief Copy a 256-bit integer limb-by-limb.
/// @param r Output integer.
/// @param a Input integer.
static void u256_copy(u256 r, const u256 a) {
    r[0] = a[0];
    r[1] = a[1];
    r[2] = a[2];
    r[3] = a[3];
}

/// @brief OR-reduce all limbs to test whether the value is zero.
/// @details Constant-time: same instruction count regardless of value.
///          A short-circuit `||` would leak which limb was non-zero
///          through branch timing.
/// @param a Integer to inspect.
/// @return Non-zero only when every limb is zero.
static int u256_is_zero(const u256 a) {
    return (a[0] | a[1] | a[2] | a[3]) == 0;
}

/// @brief Compare two 256-bit integers — returns -1 / 0 / 1 (a < b / a == b / a > b).
/// @details Walks limbs from most-significant down (limb 0 first
///          because the storage is big-endian). Used by the
///          modular-reduction step (`fp_reduce_once`,
///          `sn_reduce_once`) to decide whether a subtract is
///          needed. Not constant-time on purpose — the caller
///          uses it only on intermediate field-element comparisons,
///          not on secret values.
/// @param a First integer.
/// @param b Second integer.
/// @return -1 when @p a is smaller, 0 when equal, or 1 when greater.
static int u256_cmp(const u256 a, const u256 b) {
    for (int i = 0; i < 4; i++) {
        if (a[i] < b[i])
            return -1;
        if (a[i] > b[i])
            return 1;
    }
    return 0;
}

/// @brief Load a 256-bit integer from a 32-byte big-endian byte buffer (network order).
/// @details ECDSA signatures and public-key components cross the wire as
///          big-endian byte arrays per SEC1 / X.509 conventions. This
///          packs them into the internal 4×64-bit limb representation
///          while preserving big-endian limb ordering (limb 0 = most
///          significant 8 bytes).
/// @param r Output 256-bit limb representation.
/// @param b Input 32-byte big-endian encoding.
static void u256_from_bytes(u256 r, const uint8_t b[32]) {
    for (int i = 0; i < 4; i++) {
        r[i] = 0;
        for (int j = 0; j < 8; j++)
            r[i] = (r[i] << 8) | b[i * 8 + j];
    }
}

/// @brief Serialize a 256-bit integer into a 32-byte big-endian byte buffer.
/// @details Inverse of `u256_from_bytes`. Used to emit signature
///          components (`r`, `s`) and public-key coordinates back
///          out to wire format.
/// @param out Output 32-byte big-endian encoding.
/// @param a Input 256-bit limb representation.
static void u256_to_bytes(uint8_t out[32], const u256 a) {
    for (int i = 0; i < 4; i++) {
        uint64_t limb = a[i];
        for (int j = 7; j >= 0; j--) {
            out[i * 8 + j] = (uint8_t)(limb & 0xFF);
            limb >>= 8;
        }
    }
}

/// @brief Add two 256-bit integers with carry: `r = a + b`, returns the carry-out (0 or 1).
/// @details Uses plain unsigned wraparound so this works on compilers and
///          architectures without 128-bit integers or x86 carry intrinsics,
///          including MSVC ARM64.
/// @param r Output wrapped sum.
/// @param a First addend.
/// @param b Second addend.
/// @return Carry-out bit.
static uint64_t u256_add(u256 r, const u256 a, const u256 b) {
    uint64_t carry = 0;
    for (int i = 3; i >= 0; i--)
        carry = u64_add_with_carry(a[i], b[i], carry, &r[i]);
    return carry;
}

/// @brief Subtract two 256-bit integers with borrow: `r = a - b`, returns the borrow-out (0 or 1).
/// @details Mirror of `u256_add`, implemented with portable unsigned
///          wraparound instead of compiler-specific borrow intrinsics.
/// @param r Output wrapped difference.
/// @param a Minuend.
/// @param b Subtrahend.
/// @return Borrow-out bit.
static uint64_t u256_sub(u256 r, const u256 a, const u256 b) {
    uint64_t borrow = 0;
    for (int i = 3; i >= 0; i--)
        borrow = u64_sub_with_borrow(a[i], b[i], borrow, &r[i]);
    return borrow;
}

/// @brief 256×256 → 512 schoolbook multiplication of two big-endian-limb integers.
/// @details The field / scalar code stores 256-bit integers as four
///          64-bit limbs in big-endian limb order. To keep the wide
///          multiply portable across compilers without `__int128` we
///          expand both operands into little-endian 32-bit digits, do
///          base-2^32 schoolbook multiplication (8×8 = 64 partial
///          products), propagate carries in a single linear pass,
///          then pack the result back into eight 64-bit big-endian
///          limbs. Time complexity is O(n²) but for n=8 32-bit
///          digits it's ~64 multiplies which the optimizer keeps in
///          registers. Used by `fp_reduce_512` and the modular-mul
///          helpers when a true 512-bit intermediate is needed.
/// @param r Output eight-limb product.
/// @param a First four-limb factor.
/// @param b Second four-limb factor.
static ECDSA_P256_MAYBE_UNUSED void u256_mul_wide(u512 r, const u256 a, const u256 b) {
    uint32_t aw[8], bw[8];
    uint64_t acc[16];

    memset(acc, 0, sizeof(acc));

    for (int i = 0; i < 4; i++) {
        uint64_t limb_a = a[3 - i];
        uint64_t limb_b = b[3 - i];
        aw[2 * i] = (uint32_t)(limb_a & 0xFFFFFFFFu);
        aw[2 * i + 1] = (uint32_t)(limb_a >> 32);
        bw[2 * i] = (uint32_t)(limb_b & 0xFFFFFFFFu);
        bw[2 * i + 1] = (uint32_t)(limb_b >> 32);
    }

    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++)
            acc[i + j] += (uint64_t)aw[i] * (uint64_t)bw[j];
    }

    for (int i = 0; i < 15; i++) {
        acc[i + 1] += acc[i] >> 32;
        acc[i] &= 0xFFFFFFFFu;
    }
    acc[15] &= 0xFFFFFFFFu;

    for (int i = 0; i < 8; i++)
        r[7 - i] = (acc[2 * i + 1] << 32) | acc[2 * i];
}

/// @brief Compute the bit-length of a 512-bit little-endian integer (index 0 = LSW).
/// @param limbs Eight-limb little-endian value.
/// @return Position of the highest set bit + 1; 0 if the value is zero.
static ECDSA_P256_MAYBE_UNUSED int u512_bit_length_le(const uint64_t limbs[8]) {
    for (int i = 7; i >= 0; i--) {
        if (limbs[i] != 0)
            return i * 64 + (64 - ecdsa_clz64(limbs[i]));
    }
    return 0;
}

/// @brief Return the 64-bit limb at position limb_index of a 256-bit little-endian value after
///        left-shifting it by shift bits.  Used by the bitwise long-division in u512_mod_u256.
/// @param mod_le Four-limb little-endian source value.
/// @param shift Left shift in bits.
/// @param limb_index Requested limb index in the shifted result.
/// @return Requested 64-bit limb, with out-of-range source portions treated as zero.
static ECDSA_P256_MAYBE_UNUSED uint64_t u256_shifted_limb_le(const uint64_t mod_le[4],
                                                             int shift,
                                                             int limb_index) {
    const int word_shift = shift / 64;
    const int bit_shift = shift % 64;
    const int base = limb_index - word_shift;
    uint64_t limb = 0;

    if (base >= 0 && base < 4)
        limb |= mod_le[base] << bit_shift;
    if (bit_shift != 0 && base - 1 >= 0 && base - 1 < 4)
        limb |= mod_le[base - 1] >> (64 - bit_shift);
    return limb;
}

/// @brief Compare a 512-bit little-endian value against a 256-bit modulus shifted left by shift
/// bits.
/// @param value_le Eight-limb value to compare.
/// @param mod_le Four-limb modulus.
/// @param shift Modulus left shift in bits.
/// @return -1 / 0 / 1 (value < shifted_mod / equal / greater).
static ECDSA_P256_MAYBE_UNUSED int u512_cmp_shifted_u256_le(const uint64_t value_le[8],
                                                            const uint64_t mod_le[4],
                                                            int shift) {
    for (int i = 7; i >= 0; i--) {
        const uint64_t shifted = u256_shifted_limb_le(mod_le, shift, i);
        if (value_le[i] < shifted)
            return -1;
        if (value_le[i] > shifted)
            return 1;
    }
    return 0;
}

/// @brief Subtract (mod << shift) from a 512-bit little-endian value in-place; propagates borrow.
/// @param value_le Mutable eight-limb value.
/// @param mod_le Four-limb modulus.
/// @param shift Modulus left shift in bits.
static ECDSA_P256_MAYBE_UNUSED void u512_sub_shifted_u256_le(uint64_t value_le[8],
                                                             const uint64_t mod_le[4],
                                                             int shift) {
    uint64_t borrow = 0;
    for (int i = 0; i < 8; i++) {
        const uint64_t shifted = u256_shifted_limb_le(mod_le, shift, i);
        borrow = u64_sub_with_borrow(value_le[i], shifted, borrow, &value_le[i]);
    }
}

/// @brief Compute wide mod mod using binary long division (generic fallback for any modulus).
///        Converts between big-endian and little-endian limb order for the shift helpers.
/// @param out Output four-limb remainder.
/// @param wide Input eight-limb dividend.
/// @param mod Four-limb modulus.
static ECDSA_P256_MAYBE_UNUSED void u512_mod_u256(u256 out, const u512 wide, const u256 mod) {
    uint64_t value_le[8];
    uint64_t mod_le[4];
    int shift;

    for (int i = 0; i < 8; i++)
        value_le[i] = wide[7 - i];
    for (int i = 0; i < 4; i++)
        mod_le[i] = mod[3 - i];

    shift = u512_bit_length_le(value_le) - 256;
    if (shift < 0)
        shift = 0;

    for (; shift >= 0; shift--) {
        if (u512_cmp_shifted_u256_le(value_le, mod_le, shift) >= 0)
            u512_sub_shifted_u256_le(value_le, mod_le, shift);
    }

    for (int i = 0; i < 4; i++)
        out[3 - i] = value_le[i];
}

//=============================================================================
// Field arithmetic mod p (NIST P-256 prime)
//=============================================================================

/// @brief Reduce `a` modulo P256_P assuming `a < 2p` — at most one subtract needed.
/// @details Tries `a - p`. If that borrows, `a` was already less than
///          `p` so we keep the original. Otherwise the difference is
///          the reduced value. Used after every field addition: the
///          sum is in `[0, 2p)`, this brings it back into `[0, p)`.
/// @param r Output reduced field element.
/// @param a Input in the range `[0, 2p)`.
static void fp_reduce_once(u256 r, const u256 a) {
    u256 tmp;
    uint64_t borrow = u256_sub(tmp, a, P256_P);
    // If borrow, a < p, so keep a; otherwise use a - p
    if (borrow)
        u256_copy(r, a);
    else
        u256_copy(r, tmp);
}

/// @brief Modular addition in the P-256 prime field: `r = (a + b) mod p`.
/// @details Two cases handled:
///          - **Carry on add** → result was ≥ 2²⁵⁶, so subtract p
///            to bring it into range.
///          - **No carry** → result is in `[0, 2p)`, so reduce once.
///          Used by every elliptic-curve point operation
///          (doubling, addition).
/// @param r Output field element.
/// @param a First reduced field element.
/// @param b Second reduced field element.
static void fp_add(u256 r, const u256 a, const u256 b) {
    uint64_t carry = u256_add(r, a, b);
    if (carry) {
        // Subtract p to bring back in range
        u256_sub(r, r, P256_P);
    } else {
        fp_reduce_once(r, r);
    }
}

/// @brief Modular subtraction in the P-256 prime field: `r = (a - b) mod p`.
/// @details If the unsigned subtraction borrows (`a < b`), we add
///          `p` back to wrap into the positive residue. Otherwise
///          the result is already in `[0, p)`.
/// @param r Output field element.
/// @param a Minuend field element.
/// @param b Subtrahend field element.
static void fp_sub(u256 r, const u256 a, const u256 b) {
    uint64_t borrow = u256_sub(r, a, b);
    if (borrow) {
        // Add p to make positive
        u256_add(r, r, P256_P);
    }
}

/// @brief r = (a + b) mod mod — generic version for any modulus (assumes a, b < mod).
/// @param r Output residue.
/// @param a First reduced operand.
/// @param b Second reduced operand.
/// @param mod Modulus.
static void u256_mod_add_generic(u256 r, const u256 a, const u256 b, const u256 mod) {
    uint64_t carry = u256_add(r, a, b);
    if (carry || u256_cmp(r, mod) >= 0)
        u256_sub(r, r, mod);
}

/// @brief r = (2 * a) mod mod — generic version for any modulus.
/// @param r Output residue.
/// @param a Reduced operand.
/// @param mod Modulus.
static void u256_mod_double_generic(u256 r, const u256 a, const u256 mod) {
    u256_mod_add_generic(r, a, a, mod);
}

/// @brief r = (a * b) mod mod — generic double-and-add multiplication (any modulus).
///        Used for order-n scalar arithmetic where Solinas reduction doesn't apply.
/// @param r Output residue.
/// @param a First operand.
/// @param b Second operand.
/// @param mod Modulus.
static void u256_mod_mul_generic(u256 r, const u256 a, const u256 b, const u256 mod) {
    u256 result, base;
    u256_zero(result);
    u256_copy(base, a);
    while (u256_cmp(base, mod) >= 0)
        u256_sub(base, base, mod);

    for (int limb = 3; limb >= 0; limb--) {
        uint64_t word = b[limb];
        for (int bit = 0; bit < 64; bit++) {
            if (word & 1ULL)
                u256_mod_add_generic(result, result, base, mod);
            u256_mod_double_generic(base, base, mod);
            word >>= 1;
        }
    }
    u256_copy(r, result);
}

/// @brief Reduce a 512-bit product modulo the P-256 prime using Solinas reduction.
///        Implements the NIST FIPS 186-4 formula for p = 2^256 - 2^224 + 2^192 + 2^96 - 1.
///        Input: t[0..7] big-endian 512-bit; output: r = t mod p, 256-bit.
#if defined(__GNUC__) || defined(__clang__)
/// @brief Reduce a 512-bit product modulo the P-256 field prime on compilers that support the
///        optimized implementation.
/// @param r Output reduced field element.
/// @param t Input eight-limb product.
static ECDSA_P256_MAYBE_UNUSED void fp_reduce_512(u256 r, const u512 t) {
#else
/// @brief Reduce a 512-bit product modulo the P-256 field prime on other supported compilers.
/// @param r Output reduced field element.
/// @param t Input eight-limb product.
static void fp_reduce_512(u256 r, const u512 t) {
#endif
    // Extract 32-bit words from the 512-bit value (c0=MSW, c15=LSW)
    // t[0] is the most significant 64 bits, t[7] is least significant
    uint64_t c[16];
    for (int i = 0; i < 8; i++) {
        c[2 * i] = t[i] >> 32;
        c[2 * i + 1] = t[i] & 0xFFFFFFFFULL;
    }

    // NIST reduction formulas for P-256 (FIPS 186-4, D.2.3)
    // s1 = (c7, c6, c5, c4, c3, c2, c1, c0) — the low 256 bits
    // s2 = (c15, c14, c13, c12, c11, 0, 0, 0)
    // s3 = (0, c15, c14, c13, c12, 0, 0, 0)
    // s4 = (c15, c14, 0, 0, 0, c10, c9, c8)
    // s5 = (c8, c13, c15, c14, c13, c11, c10, c9)
    // s6 = (c10, c8, 0, 0, 0, c13, c12, c11)
    // s7 = (c11, c9, 0, 0, c15, c14, c13, c12)
    // s8 = (c12, 0, c10, c9, c8, c15, c14, c13)
    // s9 = (c13, 0, c11, c10, c9, 0, c15, c14)
    // result = s1 + 2*s2 + 2*s3 + s4 + s5 − s6 − s7 − s8 − s9 (mod p)

    // We compute using int128 accumulators per 32-bit column (columns c0..c7)
    // Column layout: result = (r7, r6, r5, r4, r3, r2, r1, r0)
    // where r0 is the least significant 32-bit word.
    // In our c[] array, c[15] is the LSW and c[0] is the MSW of the 512-bit input.
    // Rename for clarity: let w[i] = c[15-i], so w[0]=c15(LSW), w[15]=c0(MSW)
    // The NIST formulas use the convention where index 0 = LSW.
    // So s1 = (w[7], w[6], w[5], w[4], w[3], w[2], w[1], w[0])
    //    where w[i] = c[15-i]

    // Let's use w[0..15] where w[0] is the least significant 32-bit word
    uint32_t w[16];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)c[15 - i];

    // Accumulate in int64_t to handle carries and borrows
    int64_t acc[8];
    memset(acc, 0, sizeof(acc));

    // s1 = (w7, w6, w5, w4, w3, w2, w1, w0)
    for (int i = 0; i < 8; i++)
        acc[i] += w[i];

    // 2 * s2 = 2 * (w11, 0, 0, 0, 0, w15, w14, w13) -- WRONG, let me redo
    // Actually the NIST formulas in FIPS 186-4 D.2.3 are:
    // (using 0-indexed from LSW, an 8-element tuple)
    // s1 = (c0, c1, c2, c3, c4, c5, c6, c7) [low 256 bits]
    // But our c[] has c0=MSW. Let me just use w[] = LSW-first.
    // w[0..7] = lower 256 bits, w[8..15] = upper 256 bits
    //
    // NIST P-256 reduction (from HAC / FIPS 186-4):
    // s1 = (w[7], w[6], w[5], w[4], w[3], w[2], w[1], w[0])
    // s2 = (w[15], w[14], w[13], w[12], w[11], 0, 0, 0)
    // s3 = (0, w[15], w[14], w[13], w[12], 0, 0, 0)
    // s4 = (w[15], w[14], 0, 0, 0, w[10], w[9], w[8])
    // s5 = (w[8], w[13], w[15], w[14], w[13], w[11], w[10], w[9])
    // s6 = (w[10], w[8], 0, 0, 0, w[13], w[12], w[11])
    // s7 = (w[11], w[9], 0, 0, w[15], w[14], w[13], w[12])
    // s8 = (w[12], 0, w[10], w[9], w[8], w[15], w[14], w[13])
    // s9 = (w[13], 0, w[11], w[10], w[9], 0, w[15], w[14])
    //
    // result = s1 + 2*s2 + 2*s3 + s4 + s5 - s6 - s7 - s8 - s9 (mod p)

    // Reset and recompute properly
    memset(acc, 0, sizeof(acc));

    // s1
    for (int i = 0; i < 8; i++)
        acc[i] += (int64_t)w[i];

    // 2 * s2: (w[15], w[14], w[13], w[12], w[11], 0, 0, 0)
    acc[3] += 2 * (int64_t)w[11];
    acc[4] += 2 * (int64_t)w[12];
    acc[5] += 2 * (int64_t)w[13];
    acc[6] += 2 * (int64_t)w[14];
    acc[7] += 2 * (int64_t)w[15];

    // 2 * s3: (0, w[15], w[14], w[13], w[12], 0, 0, 0)
    acc[3] += 2 * (int64_t)w[12];
    acc[4] += 2 * (int64_t)w[13];
    acc[5] += 2 * (int64_t)w[14];
    acc[6] += 2 * (int64_t)w[15];

    // s4: (w[15], w[14], 0, 0, 0, w[10], w[9], w[8])
    acc[0] += (int64_t)w[8];
    acc[1] += (int64_t)w[9];
    acc[2] += (int64_t)w[10];
    acc[6] += (int64_t)w[14];
    acc[7] += (int64_t)w[15];

    // s5: (w[8], w[13], w[15], w[14], w[13], w[11], w[10], w[9])
    acc[0] += (int64_t)w[9];
    acc[1] += (int64_t)w[10];
    acc[2] += (int64_t)w[11];
    acc[3] += (int64_t)w[13];
    acc[4] += (int64_t)w[14];
    acc[5] += (int64_t)w[15];
    acc[6] += (int64_t)w[13];
    acc[7] += (int64_t)w[8];

    // -s6: -(w[10], w[8], 0, 0, 0, w[13], w[12], w[11])
    acc[0] -= (int64_t)w[11];
    acc[1] -= (int64_t)w[12];
    acc[2] -= (int64_t)w[13];
    acc[6] -= (int64_t)w[8];
    acc[7] -= (int64_t)w[10];

    // -s7: -(w[11], w[9], 0, 0, w[15], w[14], w[13], w[12])
    acc[0] -= (int64_t)w[12];
    acc[1] -= (int64_t)w[13];
    acc[2] -= (int64_t)w[14];
    acc[3] -= (int64_t)w[15];
    acc[6] -= (int64_t)w[9];
    acc[7] -= (int64_t)w[11];

    // -s8: -(w[12], 0, w[10], w[9], w[8], w[15], w[14], w[13])
    acc[0] -= (int64_t)w[13];
    acc[1] -= (int64_t)w[14];
    acc[2] -= (int64_t)w[15];
    acc[3] -= (int64_t)w[8];
    acc[4] -= (int64_t)w[9];
    acc[5] -= (int64_t)w[10];
    acc[7] -= (int64_t)w[12];

    // -s9: -(w[13], 0, w[11], w[10], w[9], 0, w[15], w[14])
    acc[0] -= (int64_t)w[14];
    acc[1] -= (int64_t)w[15];
    acc[3] -= (int64_t)w[9];
    acc[4] -= (int64_t)w[10];
    acc[5] -= (int64_t)w[11];
    acc[7] -= (int64_t)w[13];

    // Propagate carries through the 32-bit columns
    for (int i = 0; i < 7; i++) {
        acc[i + 1] += acc[i] >> 32;
        acc[i] &= 0xFFFFFFFF;
    }

    // Convert back to u256
    // Handle final carries/borrows by adding/subtracting multiples of p
    // Pack 32-bit words into 64-bit limbs (big-endian limb order)
    r[3] = ((uint64_t)(uint32_t)acc[1] << 32) | (uint64_t)(uint32_t)acc[0];
    r[2] = ((uint64_t)(uint32_t)acc[3] << 32) | (uint64_t)(uint32_t)acc[2];
    r[1] = ((uint64_t)(uint32_t)acc[5] << 32) | (uint64_t)(uint32_t)acc[4];
    r[0] = ((uint64_t)(uint32_t)acc[7] << 32) | (uint64_t)(uint32_t)acc[6];

    // Reduce: the result may be slightly negative or > p, so add/sub p as needed
    // acc[7] carries tell us how many times p to add/subtract
    int64_t top = acc[7] >> 32;
    if (top > 0) {
        // Subtract p up to a few times
        for (int64_t i = 0; i < top + 1; i++) {
            if (u256_cmp(r, P256_P) >= 0)
                u256_sub(r, r, P256_P);
            else
                break;
        }
    } else if (top < 0) {
        // Add p to make positive
        for (int64_t i = 0; i > top - 1; i--) {
            u256_add(r, r, P256_P);
            if (u256_cmp(r, P256_P) < 0 || (r[0] >> 63) == 0)
                break;
        }
    }

    // Final conditional subtraction
    fp_reduce_once(r, r);
}

/// @brief Generic fallback reduction: r = t mod P256_P via binary long division.
///        Used when the Solinas fast path is unavailable (e.g., non-GCC/Clang compilers).
/// @param r Output reduced field element.
/// @param t Input eight-limb product.
static ECDSA_P256_MAYBE_UNUSED void fp_reduce_512_generic(u256 r, const u512 t) {
    u512_mod_u256(r, t, P256_P);
}

/// @brief r = a * b mod p — field multiplication with Solinas reduction.
/// @param r Output product reduced modulo the P-256 field prime.
/// @param a First field element.
/// @param b Second field element.
static void fp_mul(u256 r, const u256 a, const u256 b) {
    u256_mod_mul_generic(r, a, b, P256_P);
}

/// @brief r = a^2 mod p — field squaring.
/// @param r Output square reduced modulo the P-256 field prime.
/// @param a Field element to square.
static void fp_sqr(u256 r, const u256 a) {
    fp_mul(r, a, a);
}

/// @brief r = a^exp mod p — binary square-and-multiply exponentiation over the P-256 field.
/// @param r Output exponentiation result.
/// @param a Field element used as the base.
/// @param exp Unsigned 256-bit exponent.
static void fp_pow(u256 r, const u256 a, const u256 exp) {
    u256 base, result;
    u256_copy(base, a);
    result[0] = 0;
    result[1] = 0;
    result[2] = 0;
    result[3] = 1; // result = 1

    for (int i = 0; i < 4; i++) {
        for (int bit = 63; bit >= 0; bit--) {
            fp_sqr(result, result);
            if ((exp[i] >> bit) & 1)
                fp_mul(result, result, base);
        }
    }
    u256_copy(r, result);
}

/// @brief r = a^(-1) mod p — field inversion via Fermat's little theorem: a^(p-2) mod p.
/// @param r Output multiplicative inverse.
/// @param a Field element to invert.
static void fp_inv(u256 r, const u256 a) {
    u256 exp;
    u256_copy(exp, P256_P);
    // p - 2
    u256_sub_small_inplace(exp, 2);

    fp_pow(r, a, exp);
}

//=============================================================================
// Scalar arithmetic mod n (curve order)
//=============================================================================

/// @brief r = a mod n — conditional subtraction of the curve order (assumes a < 2n).
/// @param r Output scalar reduced modulo the P-256 group order.
/// @param a Input integer in the range `[0, 2n)`.
static void sn_reduce_once(u256 r, const u256 a) {
    u256 tmp;
    uint64_t borrow = u256_sub(tmp, a, P256_N);
    if (borrow)
        u256_copy(r, a);
    else
        u256_copy(r, tmp);
}

/// @brief r = (a + b) mod n — scalar addition modulo the curve order.
/// @param r Output scalar sum.
/// @param a First reduced scalar.
/// @param b Second reduced scalar.
static void sn_add(u256 r, const u256 a, const u256 b) {
    uint64_t carry = u256_add(r, a, b);
    if (carry || u256_cmp(r, P256_N) >= 0)
        u256_sub(r, r, P256_N);
}

/// @brief r = (a * b) mod n — scalar multiplication modulo the curve order.
/// @param r Output scalar product.
/// @param a First scalar operand.
/// @param b Second scalar operand.
static void sn_mul(u256 r, const u256 a, const u256 b) {
    u256_mod_mul_generic(r, a, b, P256_N);
}

/// @brief r = a^(-1) mod n — scalar inversion modulo the curve order via Fermat: a^(n-2) mod n.
/// @param r Output multiplicative inverse modulo the group order.
/// @param a Scalar to invert.
static void sn_inv(u256 r, const u256 a) {
    u256 exp;
    u256_copy(exp, P256_N);
    // n - 2
    u256_sub_small_inplace(exp, 2);

    // Square-and-multiply: a^exp mod n
    u256 base, result;
    u256_copy(base, a);
    u256_zero(result);
    result[3] = 1; // result = 1

    for (int i = 0; i < 4; i++) {
        for (int bit = 63; bit >= 0; bit--) {
            sn_mul(result, result, result);
            if ((exp[i] >> bit) & 1)
                sn_mul(result, result, base);
        }
    }
    u256_copy(r, result);
}

//=============================================================================
// Jacobian projective point operations on P-256
// Affine (x, y) = (X/Z^2, Y/Z^3), point at infinity has Z = 0
//=============================================================================

typedef struct {
    u256 X, Y, Z;
} jpoint;

/// @brief Set a Jacobian point to "the point at infinity" (the curve's identity element).
/// @details In Jacobian projective coordinates, the point at infinity
///          is conventionally `(X=0, Y=1, Z=0)`. The Z=0 is the
///          discriminator that `jpoint_is_infinity` checks. We set
///          Y=1 (not 0) by convention so the point isn't all-zero,
///          though most callers don't read X/Y when Z=0.
/// @param P Output point initialized to the identity representation.
static void jpoint_set_infinity(jpoint *P) {
    u256_zero(P->X);
    u256_zero(P->Y);
    u256_zero(P->Z);
    P->Y[3] = 1; // Convention: Y=1 for point at infinity
}

/// @brief Test whether a Jacobian point is the point at infinity (Z component is zero).
/// @param P Point to inspect.
/// @return Nonzero when `P` is the point at infinity; zero otherwise.
static int jpoint_is_infinity(const jpoint *P) {
    return u256_is_zero(P->Z);
}

/// @brief Initialize a Jacobian point from affine coordinates by setting Z=1.
/// @details In Jacobian form, an affine point `(x, y)` maps to
///          `(X, Y, Z) = (x, y, 1)`. The Z=1 lets us skip the
///          division step in `jpoint_to_affine` until we actually
///          need affine output. Used to load the generator G and
///          parsed public-key points into Jacobian form.
/// @param P Output Jacobian point.
/// @param x Affine X coordinate.
/// @param y Affine Y coordinate.
static void jpoint_from_affine(jpoint *P, const u256 x, const u256 y) {
    u256_copy(P->X, x);
    u256_copy(P->Y, y);
    u256_zero(P->Z);
    P->Z[3] = 1; // Z = 1
}

/// @brief Convert a Jacobian point to affine coordinates.
/// @param x Output affine X coordinate.
/// @param y Output affine Y coordinate.
/// @param P Jacobian point to convert.
static void jpoint_to_affine(u256 x, u256 y, const jpoint *P);

/// @brief Double a Jacobian point: R = 2P (affine formula with one field inversion).
///        Deliberately trades speed for simpler, auditable arithmetic — acceptable because
///        TLS only needs a small number of scalar multiplications per handshake.
/// @param R Output doubled point.
/// @param P Point to double.
static void jpoint_double(jpoint *R, const jpoint *P) {
    static const u256 THREE = {0, 0, 0, 3};

    if (jpoint_is_infinity(P)) {
        jpoint_set_infinity(R);
        return;
    }

    u256 x1, y1, lambda, numerator, denominator, tmp;

    jpoint_to_affine(x1, y1, P);
    if (u256_is_zero(y1)) {
        jpoint_set_infinity(R);
        return;
    }

    fp_sqr(numerator, x1);
    fp_add(tmp, numerator, numerator);
    fp_add(numerator, tmp, numerator);   // 3*x^2
    fp_sub(numerator, numerator, THREE); // 3*x^2 - 3

    fp_add(denominator, y1, y1); // 2*y
    fp_inv(denominator, denominator);
    fp_mul(lambda, numerator, denominator);

    fp_sqr(R->X, lambda);
    fp_sub(R->X, R->X, x1);
    fp_sub(R->X, R->X, x1);

    fp_sub(tmp, x1, R->X);
    fp_mul(R->Y, lambda, tmp);
    fp_sub(R->Y, R->Y, y1);

    u256_zero(R->Z);
    R->Z[3] = 1;
}

/// @brief Add two Jacobian curve points: Res = P + Q (affine formula with one field inversion).
///        Handles point-at-infinity identity cases and the equal-point case by delegating to
///        jpoint_double.
/// @param Res Output sum.
/// @param P First point.
/// @param Q Second point.
static void jpoint_add(jpoint *Res, const jpoint *P, const jpoint *Q) {
    if (jpoint_is_infinity(P)) {
        memcpy(Res, Q, sizeof(jpoint));
        return;
    }
    if (jpoint_is_infinity(Q)) {
        memcpy(Res, P, sizeof(jpoint));
        return;
    }

    u256 x1, y1, x2, y2, lambda, numerator, denominator, tmp;

    jpoint_to_affine(x1, y1, P);
    jpoint_to_affine(x2, y2, Q);

    if (u256_cmp(x1, x2) == 0) {
        if (u256_cmp(y1, y2) != 0) {
            jpoint_set_infinity(Res);
            return;
        }
        jpoint_double(Res, P);
        return;
    }

    fp_sub(numerator, y2, y1);
    fp_sub(denominator, x2, x1);
    fp_inv(denominator, denominator);
    fp_mul(lambda, numerator, denominator);

    fp_sqr(Res->X, lambda);
    fp_sub(Res->X, Res->X, x1);
    fp_sub(Res->X, Res->X, x2);

    fp_sub(tmp, x1, Res->X);
    fp_mul(Res->Y, lambda, tmp);
    fp_sub(Res->Y, Res->Y, y1);

    u256_zero(Res->Z);
    Res->Z[3] = 1;
}

/// @brief Scalar multiplication on the curve: `R = k * P` (the core EC primitive).
/// @details Double-and-add walking from MSB to LSB of the scalar `k`:
///          for each bit, double the running result, then add `P`
///          if the bit is 1. The `started` flag skips the leading
///          zero bits so we don't double the point-at-infinity
///          unnecessarily.
///          NOT constant-time: the per-bit branch on `(k[i] >> bit) & 1`
///          is observable through timing/cache. **This is acceptable
///          because Zanna's ECDSA path only uses scalar-mul on
///          *public* values during verification (G * u₁ + Q * u₂),
///          never on the secret signing scalar.** A signing path would
///          need a constant-time variant (Montgomery ladder, regular
///          windowing).
/// @param R Output scalar multiple.
/// @param k Public 256-bit scalar.
/// @param P Curve point to multiply.
static void jpoint_scalar_mul(jpoint *R, const u256 k, const jpoint *P) {
    jpoint_set_infinity(R);

    // Find the highest set bit
    int started = 0;
    for (int i = 0; i < 4; i++) {
        for (int bit = 63; bit >= 0; bit--) {
            if (started) {
                jpoint tmp;
                jpoint_double(&tmp, R);
                memcpy(R, &tmp, sizeof(jpoint));
            }

            if ((k[i] >> bit) & 1) {
                started = 1;
                jpoint tmp;
                jpoint_add(&tmp, R, P);
                memcpy(R, &tmp, sizeof(jpoint));
            }
        }
    }
}

/// @brief Read one bit from a 256-bit scalar using least-significant-bit numbering.
/// @param k Scalar to inspect.
/// @param bit Bit index in the inclusive range 0 through 255.
/// @return Zero or one according to the selected bit.
static int u256_bit_at(const u256 k, int bit) {
    int limb = 3 - (bit / 64);
    int shift = bit & 63;
    return (int)((k[limb] >> shift) & 1u);
}

/// @brief Conditionally copy a Jacobian point using a full-limb selection mask.
/// @param dst Destination point, retained where mask bits are zero.
/// @param src Source point, selected where mask bits are one.
/// @param mask Selection mask; callers pass zero to retain `dst` or `UINT64_MAX` to select `src`.
static void jpoint_cmov(jpoint *dst, const jpoint *src, uint64_t mask) {
    for (int i = 0; i < 4; i++) {
        dst->X[i] = (dst->X[i] & ~mask) | (src->X[i] & mask);
        dst->Y[i] = (dst->Y[i] & ~mask) | (src->Y[i] & mask);
        dst->Z[i] = (dst->Z[i] & ~mask) | (src->Z[i] & mask);
    }
}

/// @brief Fixed-schedule scalar multiplication for private-key operations.
/// @details Executes one double and one add for every scalar bit, then
///          conditionally selects the add result with a mask. Point formulas
///          still contain exceptional-case branches, so this is a hardening
///          step rather than a complete constant-time implementation.
/// @param R Output scalar multiple.
/// @param k Private 256-bit scalar.
/// @param P Curve point to multiply.
static void jpoint_scalar_mul_fixed(jpoint *R, const u256 k, const jpoint *P) {
    jpoint_set_infinity(R);
    for (int bit = 255; bit >= 0; bit--) {
        jpoint doubled;
        jpoint added;
        jpoint_double(&doubled, R);
        jpoint_add(&added, &doubled, P);
        memcpy(R, &doubled, sizeof(jpoint));
        jpoint_cmov(R, &added, UINT64_C(0) - (uint64_t)u256_bit_at(k, bit));
    }
}

/// @brief Convert a Jacobian projective point to affine coordinates: x = X/Z^2, y = Y/Z^3.
///        Returns (0, 0) for the point at infinity.
/// @param x Output affine X coordinate.
/// @param y Output affine Y coordinate.
/// @param P Jacobian point to convert.
static void jpoint_to_affine(u256 x, u256 y, const jpoint *P) {
    if (jpoint_is_infinity(P)) {
        u256_zero(x);
        u256_zero(y);
        return;
    }

    u256 z_inv, z_inv2, z_inv3;
    fp_inv(z_inv, P->Z);
    fp_sqr(z_inv2, z_inv);
    fp_mul(z_inv3, z_inv2, z_inv);

    fp_mul(x, P->X, z_inv2);
    fp_mul(y, P->Y, z_inv3);
}

/// @brief Validate that affine coordinates canonically encode a finite point on P-256.
/// @param x Candidate affine X coordinate.
/// @param y Candidate affine Y coordinate.
/// @return Nonzero when both coordinates are in the field and satisfy the curve equation.
static int p256_point_valid(const u256 x, const u256 y) {
    if (u256_cmp(x, P256_P) >= 0 || u256_cmp(y, P256_P) >= 0)
        return 0;
    if (u256_is_zero(x) && u256_is_zero(y))
        return 0;

    u256 lhs, rhs, x2, x3, ax;
    fp_sqr(lhs, y);
    fp_sqr(x2, x);
    fp_mul(x3, x2, x);
    fp_mul(ax, P256_A, x);
    fp_add(rhs, x3, ax);
    fp_add(rhs, rhs, P256_B);
    return u256_cmp(lhs, rhs) == 0;
}

//=============================================================================
// ECDSA P-256 Verification (FIPS 186-4 / SEC1 §4.1.4)
//=============================================================================

/// @brief Verify an ECDSA-P256 signature against a public key and pre-computed message digest,
/// per FIPS 186-4 / SEC1 §4.1.4. Performs full input validation (1 ≤ r, s < n; public key on
/// curve), then computes R = s^-1·(e·G + r·Q) and checks that R.x ≡ r (mod n). All internal
/// scalar math runs against the built-in u256/fp/sn helpers — no external crypto deps.
/// @param pubkey_x Public key's 32-byte big-endian affine X coordinate.
/// @param pubkey_y Public key's 32-byte big-endian affine Y coordinate.
/// @param digest 32-byte message digest interpreted as a big-endian scalar.
/// @param sig_r Signature's 32-byte big-endian R component.
/// @param sig_s Signature's 32-byte big-endian S component.
/// @return 1 on a valid signature; 0 on bad input, a point at infinity, or a signature mismatch.
int ecdsa_p256_verify(const uint8_t pubkey_x[32],
                      const uint8_t pubkey_y[32],
                      const uint8_t digest[32],
                      const uint8_t sig_r[32],
                      const uint8_t sig_s[32]) {
    if (!pubkey_x || !pubkey_y || !digest || !sig_r || !sig_s)
        return 0;

    u256 r, s, e, qx, qy;

    u256_from_bytes(r, sig_r);
    u256_from_bytes(s, sig_s);
    u256_from_bytes(e, digest);
    u256_from_bytes(qx, pubkey_x);
    u256_from_bytes(qy, pubkey_y);

    // Step 1: Check 1 ≤ r, s < n
    if (u256_is_zero(r) || u256_cmp(r, P256_N) >= 0)
        return 0;
    if (u256_is_zero(s) || u256_cmp(s, P256_N) >= 0)
        return 0;

    // Step 2: Validate public key is canonical and on the curve.
    if (!p256_point_valid(qx, qy))
        return 0;

    // Step 3: w = s^(-1) mod n
    u256 w;
    sn_inv(w, s);

    // Step 4: u1 = e*w mod n, u2 = r*w mod n
    u256 u1, u2;
    sn_mul(u1, e, w);
    sn_mul(u2, r, w);

    // Step 5: R = u1*G + u2*Q
    jpoint G, Q, R1, R2, R;
    jpoint_from_affine(&G, P256_GX, P256_GY);
    jpoint_from_affine(&Q, qx, qy);

    jpoint_scalar_mul(&R1, u1, &G);
    jpoint_scalar_mul(&R2, u2, &Q);
    jpoint_add(&R, &R1, &R2);

    if (jpoint_is_infinity(&R))
        return 0;

    // Step 6: Convert to affine and check x ≡ r (mod n)
    u256 rx, ry;
    jpoint_to_affine(rx, ry, &R);

    // Reduce rx mod n (in case rx >= n, though unlikely for P-256)
    sn_reduce_once(rx, rx);

    return u256_cmp(rx, r) == 0 ? 1 : 0;
}

/// @brief Derive the ECDSA-P256 public key from a 32-byte private key.
/// @details Computes `Q = d * G` where `d` is the private scalar and
///          `G` is the curve generator. Used at TLS-server startup
///          to validate that a loaded EC private key matches the
///          public key in the loaded certificate (or to surface
///          the public key when only the private is provided).
///          Validates `1 ≤ d < n` per FIPS 186-4.
/// @param privkey 32-byte big-endian private scalar `d`.
/// @param pubkey_x Output buffer for the 32-byte big-endian affine X coordinate.
/// @param pubkey_y Output buffer for the 32-byte big-endian affine Y coordinate.
/// @return 1 on success; 0 for null buffers, an out-of-range private key, or an identity result.
int ecdsa_p256_public_from_private(const uint8_t privkey[32],
                                   uint8_t pubkey_x[32],
                                   uint8_t pubkey_y[32]) {
    if (!privkey || !pubkey_x || !pubkey_y)
        return 0;

    u256 d;
    u256 qx, qy;
    jpoint G;
    jpoint Q;
    int ok = 0;

    u256_from_bytes(d, privkey);
    if (u256_is_zero(d) || u256_cmp(d, P256_N) >= 0)
        goto done;

    jpoint_from_affine(&G, P256_GX, P256_GY);
    jpoint_scalar_mul_fixed(&Q, d, &G);
    if (jpoint_is_infinity(&Q))
        goto done;
    jpoint_to_affine(qx, qy, &Q);
    u256_to_bytes(pubkey_x, qx);
    u256_to_bytes(pubkey_y, qy);
    ok = 1;

done:
    ecdsa_secure_zero(d, sizeof(d));
    ecdsa_secure_zero(qx, sizeof(qx));
    ecdsa_secure_zero(qy, sizeof(qy));
    ecdsa_secure_zero(&Q, sizeof(Q));
    return ok;
}

/// @brief Compute P-256 ECDH shared point x-coordinate.
/// @details Validates both the private scalar and the peer public key, then
///          computes `S = d * Q_peer` and returns S.x. This is the primitive
///          needed by a TLS 1.3 secp256r1 key-share implementation. Uses the
///          fixed-schedule scalar-multiply path for the private scalar.
/// @param privkey 32-byte big-endian private scalar `d`.
/// @param peer_x Peer's 32-byte big-endian affine X coordinate.
/// @param peer_y Peer's 32-byte big-endian affine Y coordinate.
/// @param shared_x Output buffer for the shared point's 32-byte big-endian X coordinate.
/// @return 1 on success; 0 for null buffers, invalid key material, or an identity result.
int ecdsa_p256_ecdh(const uint8_t privkey[32],
                    const uint8_t peer_x[32],
                    const uint8_t peer_y[32],
                    uint8_t shared_x[32]) {
    if (!privkey || !peer_x || !peer_y || !shared_x)
        return 0;

    u256 d, qx, qy;
    u256 sx, sy;
    jpoint Q;
    jpoint R;
    int ok = 0;

    u256_from_bytes(d, privkey);
    u256_from_bytes(qx, peer_x);
    u256_from_bytes(qy, peer_y);
    if (u256_is_zero(d) || u256_cmp(d, P256_N) >= 0)
        goto done;
    if (!p256_point_valid(qx, qy))
        goto done;

    jpoint_from_affine(&Q, qx, qy);
    jpoint_scalar_mul_fixed(&R, d, &Q);
    if (jpoint_is_infinity(&R))
        goto done;
    jpoint_to_affine(sx, sy, &R);
    u256_to_bytes(shared_x, sx);
    ok = 1;

done:
    ecdsa_secure_zero(d, sizeof(d));
    ecdsa_secure_zero(qx, sizeof(qx));
    ecdsa_secure_zero(qy, sizeof(qy));
    ecdsa_secure_zero(sx, sizeof(sx));
    ecdsa_secure_zero(sy, sizeof(sy));
    ecdsa_secure_zero(&R, sizeof(R));
    return ok;
}

/// @brief Produce an ECDSA-P256 signature over `digest` using the private key.
/// @details Implements FIPS 186-4 / SEC1 §4.1.3 ECDSA signing:
///          1. Validate `1 ≤ d < n`.
///          2. Generate a random per-signature nonce `k` via
///             `rt_crypto_random_bytes`.
///          3. Compute the curve point `R = k * G`, take its
///             x-coordinate mod n as `r`.
///          4. Compute `s = k⁻¹ * (e + r * d) mod n`, where `e`
///             is the message digest as a scalar.
///          5. Retry with a fresh nonce if any of `r`, `s`, or
///             `e + r*d` came out zero (each rare but mathematically
///             possible).
///          6. Canonicalize to low-S form (`s = n - s` if `s > n/2`)
///             so signatures verify against any conforming verifier
///             — some validators reject high-S signatures as
///             malleable.
///          The 32-attempt cap is paranoia against pathological
///          RNG output.
/// @param privkey 32-byte big-endian private scalar `d`.
/// @param digest 32-byte message digest interpreted as a big-endian scalar.
/// @param sig_r Output buffer for the 32-byte big-endian R component.
/// @param sig_s Output buffer for the 32-byte big-endian S component.
/// @return 1 on success; 0 for null buffers, an invalid private scalar, or repeated nonce failure.
int ecdsa_p256_sign(const uint8_t privkey[32],
                    const uint8_t digest[32],
                    uint8_t sig_r[32],
                    uint8_t sig_s[32]) {
    if (!privkey || !digest || !sig_r || !sig_s)
        return 0;

    u256 d, e;
    u256_from_bytes(d, privkey);
    u256_from_bytes(e, digest);
    sn_reduce_once(e, e);
    if (u256_is_zero(d) || u256_cmp(d, P256_N) >= 0) {
        ecdsa_secure_zero(d, sizeof(d));
        ecdsa_secure_zero(e, sizeof(e));
        return 0;
    }

    jpoint G;
    jpoint_from_affine(&G, P256_GX, P256_GY);

    for (int attempt = 0; attempt < 32; attempt++) {
        uint8_t nonce_bytes[32];
        u256 k;
        u256 r, s, tmp, kinv;
        jpoint R;
        u256 rx, ry;
        int produced = 0;

        rt_crypto_random_bytes(nonce_bytes, sizeof(nonce_bytes));
        u256_from_bytes(k, nonce_bytes);
        sn_reduce_once(k, k);
        if (u256_is_zero(k) || u256_cmp(k, P256_N) >= 0)
            goto attempt_done;

        jpoint_scalar_mul_fixed(&R, k, &G);
        if (jpoint_is_infinity(&R))
            goto attempt_done;

        jpoint_to_affine(rx, ry, &R);
        sn_reduce_once(r, rx);
        if (u256_is_zero(r))
            goto attempt_done;

        sn_mul(tmp, r, d);
        sn_add(tmp, tmp, e);
        if (u256_is_zero(tmp))
            goto attempt_done;

        sn_inv(kinv, k);
        sn_mul(s, kinv, tmp);
        if (u256_is_zero(s))
            goto attempt_done;

        // Canonicalize to low-S form for broader interoperability.
        u256 half_n = {0x7FFFFFFF80000000ULL,
                       0x7FFFFFFFFFFFFFFFULL,
                       0xDE737D56D38BCF42ULL,
                       0x79DCE5617E3192A8ULL};
        if (u256_cmp(s, half_n) > 0)
            u256_sub(s, P256_N, s);

        u256_to_bytes(sig_r, r);
        u256_to_bytes(sig_s, s);
        produced = 1;

    attempt_done:
        ecdsa_secure_zero(nonce_bytes, sizeof(nonce_bytes));
        ecdsa_secure_zero(k, sizeof(k));
        ecdsa_secure_zero(r, sizeof(r));
        ecdsa_secure_zero(s, sizeof(s));
        ecdsa_secure_zero(tmp, sizeof(tmp));
        ecdsa_secure_zero(kinv, sizeof(kinv));
        ecdsa_secure_zero(rx, sizeof(rx));
        ecdsa_secure_zero(ry, sizeof(ry));
        ecdsa_secure_zero(&R, sizeof(R));
        if (produced) {
            ecdsa_secure_zero(d, sizeof(d));
            ecdsa_secure_zero(e, sizeof(e));
            return 1;
        }
    }

    ecdsa_secure_zero(d, sizeof(d));
    ecdsa_secure_zero(e, sizeof(e));
    return 0;
}
