//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_rsa.h
// Purpose: Native RSA key management, parsing, and signature verification
//          (PKCS#1 v1.5 and RSASSA-PSS) used by the TLS 1.3 and X.509 layers.
//          Supports PKCS#1 RSAPublicKey, PKCS#1 RSAPrivateKey, and PKCS#8
//          PrivateKeyInfo DER formats.
// Key invariants:
//   - rt_rsa_key_t owns its heap buffers; always call rt_rsa_key_free to release.
//   - Signature verification is public-data-only; not constant-time on sig/key.
//   - Maximum modulus size: RT_RSA_MAX_MOD_BYTES (512 bytes = 4096-bit RSA).
// Ownership/Lifetime:
//   - Parse output parameters must first be initialized with rt_rsa_key_init.
//     On success the previous initialized contents are released and *out holds
//     heap-allocated buffers; caller must call rt_rsa_key_free(out) when done.
// Links: src/runtime/network/rt_rsa.c (implementation),
//        src/runtime/network/rt_tls_verify.c (caller)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_rsa.h
 * @brief Declares native RSA key parsing, signing, and verification services.
 * @details The API represents owned big-endian RSA components, parses strict
 *          PKCS#1 and PKCS#8 DER encodings, and supports SHA-2-based PKCS#1
 *          v1.5 or PSS signatures. Callers initialize key structures before
 *          parsing and securely free them after use.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Hash function selector for RSA-PSS and PKCS#1 v1.5 signing/verification.
/// @details Maps to the SHA-2 family digest length and the corresponding
///          OID inside the DigestInfo structure used by PKCS#1 v1.5.
typedef enum {
    RT_RSA_HASH_SHA256 = 0, ///< SHA-256 (32-byte digest, OID 2.16.840.1.101.3.4.2.1).
    RT_RSA_HASH_SHA384 = 1, ///< SHA-384 (48-byte digest, OID 2.16.840.1.101.3.4.2.2).
    RT_RSA_HASH_SHA512 = 2, ///< SHA-512 (64-byte digest, OID 2.16.840.1.101.3.4.2.3).
} rt_rsa_hash_t;

/// @brief Parsed RSA key material with separately-owned big-endian components.
/// @details The key fields are big-endian byte arrays sized exactly to their
///          significant byte length (no leading zero padding). All three
///          buffers are heap-allocated by the parse routines and freed by
///          @ref rt_rsa_key_free. Only the public components are populated
///          when parsing an RSAPublicKey; PKCS#1 / PKCS#8 private-key parses
///          additionally populate @c private_exponent.
typedef struct {
    uint8_t *modulus;           ///< Public modulus n (big-endian, no leading zeros).
    size_t modulus_len;         ///< Length of @c modulus in bytes.
    uint8_t *public_exponent;   ///< Public exponent e (big-endian, typically 65537).
    size_t public_exponent_len; ///< Length of @c public_exponent in bytes.
    uint8_t *private_exponent;  ///< Private exponent d (big-endian); NULL for public-only keys.
    size_t
        private_exponent_len; ///< Length of @c private_exponent in bytes (0 when @c d is absent).
} rt_rsa_key_t;

/// @brief Zero-initialize an RSA key for parsing or safe cleanup.
/// @details Call this before the first parse. It owns no allocation afterward
///          and may be passed directly to @ref rt_rsa_key_free.
/// @param key Key structure to initialize; NULL is a no-op.
void rt_rsa_key_init(rt_rsa_key_t *key);

/// @brief Securely erase and release all components owned by an RSA key.
/// @details Modulus, public exponent, and private exponent allocations are
///          scrubbed before release, then the structure is reset to zero.
/// @param key Initialized key to consume; NULL is a no-op.
void rt_rsa_key_free(rt_rsa_key_t *key);

/// @brief Parse a PKCS#1 RSAPublicKey DER blob (SEQUENCE { modulus INTEGER, exponent INTEGER }).
/// @details Requires exact canonical DER consumption and validates a nonzero
///          odd 1024-to-4096-bit modulus plus an odd public exponent of at
///          least three. @p out remains unchanged on failure.
/// @param der Complete DER byte span.
/// @param der_len Number of bytes in @p der.
/// @param out Initialized key object replaced on success.
/// @return One on success; zero for malformed, unsupported, or unallocatable
///         input.
int rt_rsa_parse_public_key_pkcs1(const uint8_t *der, size_t der_len, rt_rsa_key_t *out);

/// @brief Parse a two-prime PKCS#1 RSAPrivateKey DER blob and retain n/e/d.
/// @details Requires version zero, exact consumption of all mandatory CRT
///          INTEGERs, and valid public/private components. CRT fields are
///          validated as nonzero then securely discarded. @p out remains
///          unchanged on failure.
/// @param der Complete DER byte span.
/// @param der_len Number of bytes in @p der.
/// @param out Initialized key object replaced on success.
/// @return One on success; zero for malformed, multi-prime, invalid, or
///         unallocatable input.
int rt_rsa_parse_private_key_pkcs1(const uint8_t *der, size_t der_len, rt_rsa_key_t *out);

/// @brief Parse a PKCS#8 PrivateKeyInfo DER blob containing an rsaEncryption key.
/// @details Requires version zero, the rsaEncryption OID, absent or canonical
///          NULL algorithm parameters, and an exact nested PKCS#1 private key.
///          @p out remains unchanged on failure.
/// @param der Complete DER byte span.
/// @param der_len Number of bytes in @p der.
/// @param out Initialized key object replaced on success.
/// @return One on success; zero for malformed input, an unrecognized
///         algorithm, or invalid nested key material.
int rt_rsa_parse_private_key_pkcs8(const uint8_t *der, size_t der_len, rt_rsa_key_t *out);

/// @brief Compare the public components of two parsed RSA keys.
/// @param lhs First key.
/// @param rhs Second key.
/// @return One when modulus and exponent lengths and bytes match; otherwise
///         zero, including missing components.
int rt_rsa_public_equals(const rt_rsa_key_t *lhs, const rt_rsa_key_t *rhs);

/// @brief Produce an RSASSA-PSS signature over a pre-computed digest.
/// @details Uses a hash-length random salt and MGF1, then applies the private
///          exponent with a constant-operation Montgomery ladder. Scratch
///          encoded-message storage is securely erased.
/// @param key Valid private key containing modulus, public exponent, and
///        private exponent.
/// @param hash_id SHA-2 algorithm matching @p digest.
/// @param digest Precomputed message digest.
/// @param digest_len Digest length required by @p hash_id.
/// @param sig_out Caller buffer receiving a modulus-width signature.
/// @param sig_len_out In/out buffer capacity and produced length.
/// @return One on success; zero for invalid input, insufficient capacity, or
///         encoding/arithmetic failure.
int rt_rsa_pss_sign(const rt_rsa_key_t *key,
                    rt_rsa_hash_t hash_id,
                    const uint8_t *digest,
                    size_t digest_len,
                    uint8_t *sig_out,
                    size_t *sig_len_out);

/// @brief Verify an RSASSA-PSS signature against a pre-computed digest.
/// @details Applies the public exponent, validates the complete RFC 8017 PSS
///          encoding with hash-length salt, and compares hashes in constant
///          time.
/// @param key Valid RSA public key.
/// @param hash_id SHA-2 algorithm matching @p digest.
/// @param digest Expected precomputed message digest.
/// @param digest_len Digest length required by @p hash_id.
/// @param sig Modulus-width signature bytes.
/// @param sig_len Number of bytes in @p sig.
/// @return One only when the signature is valid; otherwise zero.
int rt_rsa_pss_verify(const rt_rsa_key_t *key,
                      rt_rsa_hash_t hash_id,
                      const uint8_t *digest,
                      size_t digest_len,
                      const uint8_t *sig,
                      size_t sig_len);

/// @brief Verify a PKCS#1 v1.5 signature against a pre-computed digest.
/// @details Requires exact `00 01 FF... 00 DigestInfo digest` structure, at
///          least eight padding bytes, the canonical SHA-2 DigestInfo prefix,
///          and constant-time byte agreement.
/// @param key Valid RSA public key.
/// @param hash_id SHA-2 algorithm expected in DigestInfo.
/// @param digest Expected precomputed message digest.
/// @param digest_len Digest length required by @p hash_id.
/// @param sig Modulus-width signature bytes.
/// @param sig_len Number of bytes in @p sig.
/// @return One only when padding, algorithm, and digest are valid; otherwise
///         zero.
int rt_rsa_pkcs1_v15_verify(const rt_rsa_key_t *key,
                            rt_rsa_hash_t hash_id,
                            const uint8_t *digest,
                            size_t digest_len,
                            const uint8_t *sig,
                            size_t sig_len);

#ifdef __cplusplus
}
#endif
