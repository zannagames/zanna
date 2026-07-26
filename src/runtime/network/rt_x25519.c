//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_x25519.c
// Purpose: X25519 (Curve25519 ECDH, RFC 7748): 10-limb field arithmetic,
//   Montgomery-ladder scalar multiplication, and the keygen/shared-secret API.
//
// Links: rt_crypto.h, rt_crypto_internal.h (rt_secure_zero), rt_crypto.c
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_x25519.c
 * @brief Implements RFC 7748 X25519 key agreement.
 * @details The implementation uses ten mixed-radix limbs for arithmetic in
 *          GF(2^255-19), a constant-pattern Montgomery ladder for scalar
 *          multiplication, scalar clamping, and runtime entropy for key
 *          generation. Secret intermediate and output storage is scrubbed on
 *          failure paths.
 */

#include "rt_crypto.h"
#include "rt_crypto_module.h"
#include "rt_trap.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "rt_crypto_internal.h"

// ---------------------------------------------------------------------------
// Curve25519 field elements
// `fe` is an element of GF(2^255 - 19) stored in 10 mixed-radix
// limbs (alternating 26 / 25 bits). This representation lets each
// multiplication / squaring use 64-bit intermediates without
// overflow. All the `fe_*` helpers below operate on this layout
// and are the building blocks of `x25519_scalarmult`.
// ---------------------------------------------------------------------------

typedef int64_t fe[10];

/// @brief `h := f` (10-limb copy).
/// @param[out] h Destination field element.
/// @param[in] f Source field element; it may alias @p h.
static void fe_copy(fe h, const fe f) {
    for (int i = 0; i < 10; i++)
        h[i] = f[i];
}

/// @brief Set `h` to the field zero.
/// @param[out] h Field element to clear.
static void fe_0(fe h) {
    for (int i = 0; i < 10; i++)
        h[i] = 0;
}

/// @brief Set `h` to the field identity (1).
/// @param[out] h Field element to initialize.
static void fe_1(fe h) {
    h[0] = 1;
    for (int i = 1; i < 10; i++)
        h[i] = 0;
}

/// @brief `h := f + g` (limb-wise; carries deferred to next multiplication).
/// @param[out] h Destination field element; it may alias either input.
/// @param[in] f First addend.
/// @param[in] g Second addend.
static void fe_add(fe h, const fe f, const fe g) {
    for (int i = 0; i < 10; i++)
        h[i] = f[i] + g[i];
}

/// @brief `h := f - g` (limb-wise; carries deferred).
/// @param[out] h Destination field element; it may alias either input.
/// @param[in] f Minuend.
/// @param[in] g Subtrahend.
static void fe_sub(fe h, const fe f, const fe g) {
    for (int i = 0; i < 10; i++)
        h[i] = f[i] - g[i];
}

/// @brief `h := f * g mod 2^255 - 19`.
///
/// Implements the textbook 10×10 schoolbook multiply with the
/// `19·g_i` pre-multipliers absorbing the wrap from the prime
/// reduction. Carries are propagated in two passes so each output
/// limb fits the 25/26-bit constraint.
/// @param[out] h Destination reduced field element; it may alias either input.
/// @param[in] f First factor.
/// @param[in] g Second factor.
static void fe_mul(fe h, const fe f, const fe g) {
    int64_t f0 = f[0], f1 = f[1], f2 = f[2], f3 = f[3], f4 = f[4];
    int64_t f5 = f[5], f6 = f[6], f7 = f[7], f8 = f[8], f9 = f[9];
    int64_t g0 = g[0], g1 = g[1], g2 = g[2], g3 = g[3], g4 = g[4];
    int64_t g5 = g[5], g6 = g[6], g7 = g[7], g8 = g[8], g9 = g[9];

    int64_t g1_19 = 19 * g1, g2_19 = 19 * g2, g3_19 = 19 * g3, g4_19 = 19 * g4, g5_19 = 19 * g5;
    int64_t g6_19 = 19 * g6, g7_19 = 19 * g7, g8_19 = 19 * g8, g9_19 = 19 * g9;
    int64_t f1_2 = 2 * f1, f3_2 = 2 * f3, f5_2 = 2 * f5, f7_2 = 2 * f7, f9_2 = 2 * f9;

    int64_t h0 = f0 * g0 + f1_2 * g9_19 + f2 * g8_19 + f3_2 * g7_19 + f4 * g6_19 + f5_2 * g5_19 +
                 f6 * g4_19 + f7_2 * g3_19 + f8 * g2_19 + f9_2 * g1_19;
    int64_t h1 = f0 * g1 + f1 * g0 + f2 * g9_19 + f3 * g8_19 + f4 * g7_19 + f5 * g6_19 +
                 f6 * g5_19 + f7 * g4_19 + f8 * g3_19 + f9 * g2_19;
    int64_t h2 = f0 * g2 + f1_2 * g1 + f2 * g0 + f3_2 * g9_19 + f4 * g8_19 + f5_2 * g7_19 +
                 f6 * g6_19 + f7_2 * g5_19 + f8 * g4_19 + f9_2 * g3_19;
    int64_t h3 = f0 * g3 + f1 * g2 + f2 * g1 + f3 * g0 + f4 * g9_19 + f5 * g8_19 + f6 * g7_19 +
                 f7 * g6_19 + f8 * g5_19 + f9 * g4_19;
    int64_t h4 = f0 * g4 + f1_2 * g3 + f2 * g2 + f3_2 * g1 + f4 * g0 + f5_2 * g9_19 + f6 * g8_19 +
                 f7_2 * g7_19 + f8 * g6_19 + f9_2 * g5_19;
    int64_t h5 = f0 * g5 + f1 * g4 + f2 * g3 + f3 * g2 + f4 * g1 + f5 * g0 + f6 * g9_19 +
                 f7 * g8_19 + f8 * g7_19 + f9 * g6_19;
    int64_t h6 = f0 * g6 + f1_2 * g5 + f2 * g4 + f3_2 * g3 + f4 * g2 + f5_2 * g1 + f6 * g0 +
                 f7_2 * g9_19 + f8 * g8_19 + f9_2 * g7_19;
    int64_t h7 = f0 * g7 + f1 * g6 + f2 * g5 + f3 * g4 + f4 * g3 + f5 * g2 + f6 * g1 + f7 * g0 +
                 f8 * g9_19 + f9 * g8_19;
    int64_t h8 = f0 * g8 + f1_2 * g7 + f2 * g6 + f3_2 * g5 + f4 * g4 + f5_2 * g3 + f6 * g2 +
                 f7_2 * g1 + f8 * g0 + f9_2 * g9_19;
    int64_t h9 = f0 * g9 + f1 * g8 + f2 * g7 + f3 * g6 + f4 * g5 + f5 * g4 + f6 * g3 + f7 * g2 +
                 f8 * g1 + f9 * g0;

    // Carry
    int64_t c;
    c = (h0 + (1 << 25)) >> 26;
    h1 += c;
    h0 -= c << 26;
    c = (h4 + (1 << 25)) >> 26;
    h5 += c;
    h4 -= c << 26;
    c = (h1 + (1 << 24)) >> 25;
    h2 += c;
    h1 -= c << 25;
    c = (h5 + (1 << 24)) >> 25;
    h6 += c;
    h5 -= c << 25;
    c = (h2 + (1 << 25)) >> 26;
    h3 += c;
    h2 -= c << 26;
    c = (h6 + (1 << 25)) >> 26;
    h7 += c;
    h6 -= c << 26;
    c = (h3 + (1 << 24)) >> 25;
    h4 += c;
    h3 -= c << 25;
    c = (h7 + (1 << 24)) >> 25;
    h8 += c;
    h7 -= c << 25;
    c = (h4 + (1 << 25)) >> 26;
    h5 += c;
    h4 -= c << 26;
    c = (h8 + (1 << 25)) >> 26;
    h9 += c;
    h8 -= c << 26;
    c = (h9 + (1 << 24)) >> 25;
    h0 += c * 19;
    h9 -= c << 25;
    c = (h0 + (1 << 25)) >> 26;
    h1 += c;
    h0 -= c << 26;

    h[0] = h0;
    h[1] = h1;
    h[2] = h2;
    h[3] = h3;
    h[4] = h4;
    h[5] = h5;
    h[6] = h6;
    h[7] = h7;
    h[8] = h8;
    h[9] = h9;
}

/// @brief `h := f * f`. Just a convenience wrapper over `fe_mul`.
/// @param[out] h Destination reduced field element; it may alias @p f.
/// @param[in] f Field element to square.
static void fe_sq(fe h, const fe f) {
    fe_mul(h, f, f);
}

/// @brief `out := z^(-1) mod 2^255 - 19` via Fermat's little theorem.
///
/// Computes `z^(p-2)` where `p = 2^255 - 19` using the standard
/// 254-square + 11-multiply addition chain — the inversion you'd
/// find in any reference Curve25519 implementation. Constant-time.
/// @param[out] out Destination multiplicative inverse; it may alias @p z.
/// @param[in] z Field element to invert.
static void fe_invert(fe out, const fe z) {
    fe t0, t1, t2, t3;

    fe_sq(t0, z);
    fe_sq(t1, t0);
    fe_sq(t1, t1);
    fe_mul(t1, z, t1);
    fe_mul(t0, t0, t1);
    fe_sq(t2, t0);
    fe_mul(t1, t1, t2);
    fe_sq(t2, t1);
    for (int i = 1; i < 5; i++)
        fe_sq(t2, t2);
    fe_mul(t1, t2, t1);
    fe_sq(t2, t1);
    for (int i = 1; i < 10; i++)
        fe_sq(t2, t2);
    fe_mul(t2, t2, t1);
    fe_sq(t3, t2);
    for (int i = 1; i < 20; i++)
        fe_sq(t3, t3);
    fe_mul(t2, t3, t2);
    fe_sq(t2, t2);
    for (int i = 1; i < 10; i++)
        fe_sq(t2, t2);
    fe_mul(t1, t2, t1);
    fe_sq(t2, t1);
    for (int i = 1; i < 50; i++)
        fe_sq(t2, t2);
    fe_mul(t2, t2, t1);
    fe_sq(t3, t2);
    for (int i = 1; i < 100; i++)
        fe_sq(t3, t3);
    fe_mul(t2, t3, t2);
    fe_sq(t2, t2);
    for (int i = 1; i < 50; i++)
        fe_sq(t2, t2);
    fe_mul(t1, t2, t1);
    fe_sq(t1, t1);
    for (int i = 1; i < 5; i++)
        fe_sq(t1, t1);
    fe_mul(out, t1, t0);
}

/// @brief Decode 32 little-endian bytes into a 10-limb field element.
///
/// The top bit of `s[31]` is silently masked off — RFC 7748 §5
/// requires this for X25519 to make the function tolerate
/// non-canonical encodings. Each limb extracts a 25- or 26-bit
/// slice using the alternating mixed-radix layout.
/// @param[out] h Destination mixed-radix field element.
/// @param[in] s 32-byte little-endian u-coordinate.
static void fe_from_bytes(fe h, const uint8_t s[32]) {
    h[0] = ((int64_t)s[0] | ((int64_t)s[1] << 8) | ((int64_t)s[2] << 16) |
            (((int64_t)s[3] & 0x03) << 24)) &
           0x3ffffff;
    h[1] = (((int64_t)s[3] >> 2) | ((int64_t)s[4] << 6) | ((int64_t)s[5] << 14) |
            (((int64_t)s[6] & 0x07) << 22)) &
           0x1ffffff;
    h[2] = (((int64_t)s[6] >> 3) | ((int64_t)s[7] << 5) | ((int64_t)s[8] << 13) |
            (((int64_t)s[9] & 0x1f) << 21)) &
           0x3ffffff;
    h[3] = (((int64_t)s[9] >> 5) | ((int64_t)s[10] << 3) | ((int64_t)s[11] << 11) |
            (((int64_t)s[12] & 0x3f) << 19)) &
           0x1ffffff;
    h[4] = (((int64_t)s[12] >> 6) | ((int64_t)s[13] << 2) | ((int64_t)s[14] << 10) |
            ((int64_t)s[15] << 18)) &
           0x3ffffff;
    h[5] = ((int64_t)s[16] | ((int64_t)s[17] << 8) | ((int64_t)s[18] << 16) |
            (((int64_t)s[19] & 0x01) << 24)) &
           0x1ffffff;
    h[6] = (((int64_t)s[19] >> 1) | ((int64_t)s[20] << 7) | ((int64_t)s[21] << 15) |
            (((int64_t)s[22] & 0x07) << 23)) &
           0x3ffffff;
    h[7] = (((int64_t)s[22] >> 3) | ((int64_t)s[23] << 5) | ((int64_t)s[24] << 13) |
            (((int64_t)s[25] & 0x0f) << 21)) &
           0x1ffffff;
    h[8] = (((int64_t)s[25] >> 4) | ((int64_t)s[26] << 4) | ((int64_t)s[27] << 12) |
            (((int64_t)s[28] & 0x3f) << 20)) &
           0x3ffffff;
    h[9] = (((int64_t)s[28] >> 6) | ((int64_t)s[29] << 2) | ((int64_t)s[30] << 10) |
            ((int64_t)s[31] << 18)) &
           0x1ffffff;
}

/// @brief Serialise a field element to 32 little-endian bytes.
///
/// Performs full modular reduction (subtract `q · p` where `q` is
/// computed from a tentative high-limb propagation, then propagate
/// carries) so the output is the unique canonical representation
/// — important so that two different limb shapes for the same
/// value still produce identical wire bytes.
/// @param[out] s Destination 32-byte canonical little-endian encoding.
/// @param[in] h Field element to reduce and serialize.
static void fe_to_bytes(uint8_t s[32], const fe h) {
    int64_t h0 = h[0], h1 = h[1], h2 = h[2], h3 = h[3], h4 = h[4];
    int64_t h5 = h[5], h6 = h[6], h7 = h[7], h8 = h[8], h9 = h[9];

    int64_t q = (19 * h9 + (1LL << 24)) >> 25;
    q = (h0 + q) >> 26;
    q = (h1 + q) >> 25;
    q = (h2 + q) >> 26;
    q = (h3 + q) >> 25;
    q = (h4 + q) >> 26;
    q = (h5 + q) >> 25;
    q = (h6 + q) >> 26;
    q = (h7 + q) >> 25;
    q = (h8 + q) >> 26;
    q = (h9 + q) >> 25;

    h0 += 19 * q;

    int64_t c = h0 >> 26;
    h1 += c;
    h0 -= c << 26;
    c = h1 >> 25;
    h2 += c;
    h1 -= c << 25;
    c = h2 >> 26;
    h3 += c;
    h2 -= c << 26;
    c = h3 >> 25;
    h4 += c;
    h3 -= c << 25;
    c = h4 >> 26;
    h5 += c;
    h4 -= c << 26;
    c = h5 >> 25;
    h6 += c;
    h5 -= c << 25;
    c = h6 >> 26;
    h7 += c;
    h6 -= c << 26;
    c = h7 >> 25;
    h8 += c;
    h7 -= c << 25;
    c = h8 >> 26;
    h9 += c;
    h8 -= c << 26;
    c = h9 >> 25;
    h9 -= c << 25;

    s[0] = (uint8_t)h0;
    s[1] = (uint8_t)(h0 >> 8);
    s[2] = (uint8_t)(h0 >> 16);
    s[3] = (uint8_t)((h0 >> 24) | (h1 << 2));
    s[4] = (uint8_t)(h1 >> 6);
    s[5] = (uint8_t)(h1 >> 14);
    s[6] = (uint8_t)((h1 >> 22) | (h2 << 3));
    s[7] = (uint8_t)(h2 >> 5);
    s[8] = (uint8_t)(h2 >> 13);
    s[9] = (uint8_t)((h2 >> 21) | (h3 << 5));
    s[10] = (uint8_t)(h3 >> 3);
    s[11] = (uint8_t)(h3 >> 11);
    s[12] = (uint8_t)((h3 >> 19) | (h4 << 6));
    s[13] = (uint8_t)(h4 >> 2);
    s[14] = (uint8_t)(h4 >> 10);
    s[15] = (uint8_t)(h4 >> 18);
    s[16] = (uint8_t)h5;
    s[17] = (uint8_t)(h5 >> 8);
    s[18] = (uint8_t)(h5 >> 16);
    s[19] = (uint8_t)((h5 >> 24) | (h6 << 1));
    s[20] = (uint8_t)(h6 >> 7);
    s[21] = (uint8_t)(h6 >> 15);
    s[22] = (uint8_t)((h6 >> 23) | (h7 << 3));
    s[23] = (uint8_t)(h7 >> 5);
    s[24] = (uint8_t)(h7 >> 13);
    s[25] = (uint8_t)((h7 >> 21) | (h8 << 4));
    s[26] = (uint8_t)(h8 >> 4);
    s[27] = (uint8_t)(h8 >> 12);
    s[28] = (uint8_t)((h8 >> 20) | (h9 << 6));
    s[29] = (uint8_t)(h9 >> 2);
    s[30] = (uint8_t)(h9 >> 10);
    s[31] = (uint8_t)(h9 >> 18);
}

/// @brief Conditionally exchange two field elements without secret-dependent branches.
/// @param[in,out] a First field element.
/// @param[in,out] b Second field element.
/// @param[in] swap Low bit selects exchange when one and no change when zero.
static void fe_cswap(fe a, fe b, int swap) {
    uint64_t mask = UINT64_C(0) - (uint64_t)(swap & 1);
    for (int i = 0; i < 10; i++) {
        uint64_t x = (uint64_t)a[i];
        uint64_t y = (uint64_t)b[i];
        uint64_t t = mask & (x ^ y);
        a[i] = (int64_t)(x ^ t);
        b[i] = (int64_t)(y ^ t);
    }
}

/// @brief Constant-time X25519 scalar multiplication (RFC 7748).
///
/// Implements the Montgomery ladder over Curve25519. The scalar is
/// clamped per RFC 7748 §5 (clear bits 0,1,2 of byte 0; set bit 6
/// and clear bit 7 of byte 31). The conditional swap uses a XOR
/// mask derived from `swap = b_prev XOR b_curr` so each iteration
/// performs the same operations regardless of the secret bit.
/// @param[out] out 32-byte canonical little-endian result.
/// @param[in] scalar 32-byte scalar; copied, clamped, and securely erased internally.
/// @param[in] point 32-byte peer u-coordinate; its high bit is ignored per RFC 7748.
static void x25519_scalarmult(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
    fe x1, x2, z2, x3, z3, tmp0, tmp1;
    uint8_t e[32];

    memcpy(e, scalar, 32);
    e[0] &= 248;
    e[31] &= 127;
    e[31] |= 64;

    fe_from_bytes(x1, point);
    fe_1(x2);
    fe_0(z2);
    fe_copy(x3, x1);
    fe_1(z3);

    int swap = 0;
    for (int pos = 254; pos >= 0; pos--) {
        int b = (e[pos / 8] >> (pos & 7)) & 1;
        swap ^= b;
        fe_cswap(x2, x3, swap);
        fe_cswap(z2, z3, swap);
        swap = b;

        fe_sub(tmp0, x3, z3);
        fe_sub(tmp1, x2, z2);
        fe_add(x2, x2, z2);
        fe_add(z2, x3, z3);
        fe_mul(z3, tmp0, x2);
        fe_mul(z2, z2, tmp1);
        fe_sq(tmp0, tmp1);
        fe_sq(tmp1, x2);
        fe_add(x3, z3, z2);
        fe_sub(z2, z3, z2);
        fe_mul(x2, tmp1, tmp0);
        fe_sub(tmp1, tmp1, tmp0);
        fe_sq(z2, z2);
        fe fe_121666 = {121666, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        fe_mul(z3, tmp1, fe_121666);
        fe_sq(x3, x3);
        fe_add(tmp0, tmp0, z3);
        fe_mul(z3, x1, z2);
        fe_mul(z2, tmp1, tmp0);
    }

    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);

    fe_invert(z2, z2);
    fe_mul(x2, x2, z2);
    fe_to_bytes(out, x2);
    rt_secure_zero(e, sizeof(e));
    rt_secure_zero(x1, sizeof(x1));
    rt_secure_zero(x2, sizeof(x2));
    rt_secure_zero(z2, sizeof(z2));
    rt_secure_zero(x3, sizeof(x3));
    rt_secure_zero(z3, sizeof(z3));
    rt_secure_zero(tmp0, sizeof(tmp0));
    rt_secure_zero(tmp1, sizeof(tmp1));
}

static const uint8_t x25519_basepoint[32] = {9};

/// @brief Generate an X25519 keypair: random 32-byte `secret`, derived `public_key`.
///
/// The clamping required by RFC 7748 §5 happens inside
/// `x25519_scalarmult` — callers can therefore pass any 32 random
/// bytes as the secret without pre-clamping.
/// @param[out] secret Buffer that receives 32 bytes from the runtime CSPRNG.
/// @param[out] public_key Buffer that receives the corresponding public u-coordinate.
/// @note Traps when either output buffer is NULL or the entropy source fails.
void rt_x25519_keygen(uint8_t secret[32], uint8_t public_key[32]) {
    if (!secret || !public_key) {
        rt_trap("X25519.Keygen: output buffer is null");
        return;
    }
    rt_crypto_random_bytes(secret, 32);
    x25519_scalarmult(public_key, secret, x25519_basepoint);
}

/// @brief Compute the X25519 shared secret = `peer_public · secret` on Curve25519.
///
/// Used by TLS 1.3 (RFC 8446 §7.4.2) and other protocols that need
/// an ECDH key agreement step.
/// @param[in] secret Local 32-byte private scalar.
/// @param[in] peer_public Peer 32-byte public u-coordinate.
/// @param[out] shared Buffer that receives the 32-byte shared secret.
/// @return 0 for a nonzero shared secret, or -1 for null arguments or the
///         all-zero result associated with a low-order peer input.
int rt_x25519(const uint8_t secret[32], const uint8_t peer_public[32], uint8_t shared[32]) {
    if (!secret || !peer_public || !shared)
        return -1;
    x25519_scalarmult(shared, secret, peer_public);
    uint8_t any = 0;
    for (int i = 0; i < 32; i++)
        any |= shared[i];
    return any == 0 ? -1 : 0;
}
