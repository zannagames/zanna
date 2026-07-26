//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_ecdsa_p256.h
// Purpose: Native ECDSA P-256 (secp256r1) verification and signing in pure C.
//          No external dependencies.
// Key invariants:
//   - Signing support is limited to caller-supplied private scalars.
//   - All inputs are big-endian byte arrays; no heap allocation.
//   - Verification uses public-data scalar multiplication.
//   - Private-key paths use a fixed scalar-multiply schedule; full
//     constant-time field arithmetic is not claimed.
// Ownership/Lifetime:
//   - Pure functions operating on caller-owned buffers; no ownership transfer.
// Links: src/runtime/network/rt_ecdsa_p256.c (implementation),
//        src/runtime/network/rt_tls.c (integration)
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares dependency-free P-256 signature and key-agreement services.
 * @details Defines caller-buffer contracts for SHA-256 ECDSA verification and
 * signing, public-key derivation, and ECDH shared x-coordinate computation
 * over canonical big-endian scalar and point encodings.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Verify an ECDSA P-256 (secp256r1) signature over a SHA-256 digest.
///
/// Implements the ECDSA verification algorithm per FIPS 186-4 / SEC1 §4.1.4:
///   1. Check 1 ≤ r, s < n
///   2. w = s⁻¹ mod n
///   3. u₁ = (digest · w) mod n, u₂ = (r · w) mod n
///   4. R = u₁·G + u₂·Q
///   5. Valid if R.x ≡ r (mod n)
///
/// @param pubkey_x  32-byte big-endian X coordinate of the public key point Q.
/// @param pubkey_y  32-byte big-endian Y coordinate of the public key point Q.
/// @param digest    32-byte SHA-256 digest of the signed message.
/// @param sig_r     32-byte big-endian R component of the ECDSA signature.
/// @param sig_s     32-byte big-endian S component of the ECDSA signature.
/// @return 1 if the signature is valid, 0 if invalid or on any error.
int ecdsa_p256_verify(const uint8_t pubkey_x[32],
                      const uint8_t pubkey_y[32],
                      const uint8_t digest[32],
                      const uint8_t sig_r[32],
                      const uint8_t sig_s[32]);

/// @brief Derive the public key point for a 32-byte P-256 private scalar.
/// @param privkey  32-byte big-endian private scalar d where 1 <= d < n.
/// @param pubkey_x Output 32-byte big-endian affine X coordinate.
/// @param pubkey_y Output 32-byte big-endian affine Y coordinate.
/// @return 1 on success, 0 if the private scalar or output buffers are invalid.
int ecdsa_p256_public_from_private(const uint8_t privkey[32],
                                   uint8_t pubkey_x[32],
                                   uint8_t pubkey_y[32]);

/// @brief Sign a SHA-256 digest with a P-256 private scalar.
/// @param privkey 32-byte big-endian private scalar d where 1 <= d < n.
/// @param digest  32-byte SHA-256 digest of the message to sign.
/// @param sig_r   Output 32-byte big-endian R component.
/// @param sig_s   Output 32-byte big-endian S component.
/// @return 1 on success, 0 on invalid key material, null buffers, or repeated nonce failure.
int ecdsa_p256_sign(const uint8_t privkey[32],
                    const uint8_t digest[32],
                    uint8_t sig_r[32],
                    uint8_t sig_s[32]);

/// @brief Compute a P-256 ECDH shared secret x-coordinate.
/// @param privkey 32-byte big-endian private scalar d where 1 <= d < n.
/// @param peer_x  Peer public key affine X coordinate.
/// @param peer_y  Peer public key affine Y coordinate.
/// @param shared_x Output shared point X coordinate.
/// @return 1 on success, 0 on invalid key material, null buffers, or invalid/non-canonical peer
/// point.
int ecdsa_p256_ecdh(const uint8_t privkey[32],
                    const uint8_t peer_x[32],
                    const uint8_t peer_y[32],
                    uint8_t shared_x[32]);

#ifdef __cplusplus
}
#endif
