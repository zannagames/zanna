//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/network/rt_crypto.h
// Purpose: Pure-C cryptographic primitives for TLS support: SHA-256/384/512,
//          HMAC-SHA2, HKDF, ChaCha20-Poly1305 and AES-GCM AEAD, X25519 key
//          exchange, and secure random-byte generation.
//
// Frontend exposure boundary:
//   - This header is primarily internal runtime support for TLS and related
//     subsystems.
//   - Zia/BASIC surface area should be added deliberately through
//     src/il/runtime/runtime.def rather than inferred from declarations here.
//
// Key invariants:
//   - All key material and digests are handled as raw byte arrays in caller-provided buffers.
//   - Functions do not allocate heap memory for outputs.
//   - ChaCha20-Poly1305 provides authenticated encryption with 16-byte tags.
//   - X25519 computes a 32-byte shared secret from a private key and public key.
//   - AEAD decryptors authenticate before writing plaintext and compare tags
//     without data-dependent early exit.
//
// Ownership/Lifetime:
//   - Pure functions operating on caller-owned buffers; no ownership transfer.
//   - Callers must provide output buffers of sufficient size (documented per function).
//
// Links: src/runtime/network/rt_crypto.c (implementation)
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares caller-buffer cryptographic primitives for runtime TLS.
 * @details Defines incremental and one-shot SHA-2, HMAC and HKDF derivation,
 * ChaCha20-Poly1305 and AES-GCM authenticated encryption, X25519 agreement,
 * constant-time comparison, secure wiping, and strong random generation.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//=========================================================================
// SHA-256
//=========================================================================

/// @brief Incremental SHA-256 state.
/// @details Exposed so callers with streaming inputs can avoid re-hashing
///          prior bytes. The layout is part of the runtime ABI for internal
///          consumers such as TLS transcript hashing.
typedef struct rt_sha256_ctx {
    /// Eight FIPS 180-4 chaining words.
    uint32_t state[8];
    /// Number of message bits absorbed so far.
    uint64_t count;
    /// Partial 64-byte message block.
    uint8_t buffer[64];
} rt_sha256_ctx;

/// @brief Initialize a SHA-256 context for incremental hashing.
/// @param ctx Context to initialize.
void rt_sha256_init(rt_sha256_ctx *ctx);

/// @brief Append bytes to an incremental SHA-256 hash.
/// @param ctx Context to update.
/// @param data Bytes to append (may be NULL when @p len is zero).
/// @param len Number of bytes to append.
void rt_sha256_update(rt_sha256_ctx *ctx, const void *data, size_t len);

/// @brief Finalize an incremental SHA-256 hash.
/// @details Finalization mutates the context state. Copy the context first
///          when a snapshot digest is needed without consuming the running hash.
/// @param ctx Context to finalize.
/// @param digest Output buffer for the 32-byte (256-bit) hash digest.
void rt_sha256_final(rt_sha256_ctx *ctx, uint8_t digest[32]);

/// @brief Compute SHA-256 hash.
/// @param data Pointer to the input data to hash.
/// @param len Length of @p data in bytes.
/// @param digest Output buffer for the 32-byte (256-bit) hash digest.
void rt_sha256(const void *data, size_t len, uint8_t digest[32]);

/// @brief Compute SHA-384 hash.
/// @param data Pointer to the input data to hash.
/// @param len Length of @p data in bytes.
/// @param digest Output buffer for the 48-byte (384-bit) hash digest.
void rt_sha384(const void *data, size_t len, uint8_t digest[48]);

/// @brief Compute SHA-512 hash.
/// @param data Pointer to the input data to hash.
/// @param len Length of @p data in bytes.
/// @param digest Output buffer for the 64-byte (512-bit) hash digest.
void rt_sha512(const void *data, size_t len, uint8_t digest[64]);

//=========================================================================
// HMAC-SHA2
//=========================================================================

/// @brief Compute HMAC-SHA256.
/// @param key Pointer to the HMAC key.
/// @param key_len Length of @p key in bytes.
/// @param data Pointer to the input data to authenticate.
/// @param data_len Length of @p data in bytes.
/// @param mac Output buffer for the 32-byte message authentication code.
void rt_hmac_sha256(
    const uint8_t *key, size_t key_len, const void *data, size_t data_len, uint8_t mac[32]);

/// @brief Compute HMAC-SHA384.
/// @param key Pointer to the HMAC key.
/// @param key_len Length of @p key in bytes.
/// @param data Pointer to the input data to authenticate.
/// @param data_len Length of @p data in bytes.
/// @param mac Output buffer for the 48-byte message authentication code.
void rt_hmac_sha384(
    const uint8_t *key, size_t key_len, const void *data, size_t data_len, uint8_t mac[48]);

/// @brief Compute HMAC-SHA512.
/// @param key Pointer to the HMAC key.
/// @param key_len Length of @p key in bytes.
/// @param data Pointer to the input data to authenticate.
/// @param data_len Length of @p data in bytes.
/// @param mac Output buffer for the 64-byte message authentication code.
void rt_hmac_sha512(
    const uint8_t *key, size_t key_len, const void *data, size_t data_len, uint8_t mac[64]);

//=========================================================================
// HKDF-SHA256 (RFC 5869)
//=========================================================================

/// @brief HKDF-Extract.
/// @param salt Optional salt value (can be NULL for zero-length salt).
/// @param salt_len Length of @p salt in bytes.
/// @param ikm Input keying material.
/// @param ikm_len Length of @p ikm in bytes.
/// @param prk Output buffer for the 32-byte pseudorandom key.
void rt_hkdf_extract(
    const uint8_t *salt, size_t salt_len, const uint8_t *ikm, size_t ikm_len, uint8_t prk[32]);

/// @brief HKDF-Expand.
/// @param prk Pseudorandom key from HKDF-Extract (32 bytes).
/// @param info Application-specific context and info (can be NULL).
/// @param info_len Length of @p info in bytes.
/// @param okm Output buffer for the derived keying material.
/// @param okm_len Desired length of output keying material in bytes
///               (at most 255 * 32 = 8160 bytes per RFC 5869).
/// @return 0 on success; -1 for invalid pointers or an excessive output length.
int rt_hkdf_expand(
    const uint8_t prk[32], const uint8_t *info, size_t info_len, uint8_t *okm, size_t okm_len);

/// @brief HKDF-Expand-Label for TLS 1.3.
/// @param secret The 32-byte secret from which to derive keying material.
/// @param label The TLS 1.3 label string (without "tls13 " prefix).
/// @param context Hash context or transcript hash (can be NULL).
/// @param context_len Length of @p context in bytes.
/// @param out Output buffer for the derived keying material.
/// @param out_len Desired length of output in bytes.
/// @return 0 on success; -1 for invalid input or a label/context/output length
///         that cannot be encoded.
int rt_hkdf_expand_label(const uint8_t secret[32],
                         const char *label,
                         const uint8_t *context,
                         size_t context_len,
                         uint8_t *out,
                         size_t out_len);

/// @brief HKDF-Extract using HMAC-SHA384 (RFC 5869, hash = SHA-384).
/// @param salt Optional salt value (NULL for the all-zero default salt).
/// @param salt_len Length of @p salt in bytes.
/// @param ikm Input keying material.
/// @param ikm_len Length of @p ikm in bytes.
/// @param prk Output buffer for the 48-byte pseudorandom key.
void rt_hkdf_extract_sha384(
    const uint8_t *salt, size_t salt_len, const uint8_t *ikm, size_t ikm_len, uint8_t prk[48]);

/// @brief HKDF-Expand using HMAC-SHA384.
/// @param prk Pseudorandom key from `rt_hkdf_extract_sha384` (48 bytes).
/// @param info Application-specific info string (NULL for none).
/// @param info_len Length of @p info in bytes.
/// @param okm Output buffer for the derived keying material.
/// @param okm_len Desired length in bytes (max 255 * 48 = 12240 per RFC 5869).
/// @return 0 on success; non-zero when @p okm_len exceeds the per-call cap.
int rt_hkdf_expand_sha384(
    const uint8_t prk[48], const uint8_t *info, size_t info_len, uint8_t *okm, size_t okm_len);

/// @brief HKDF-Expand-Label for TLS 1.3 with the SHA-384 cipher suite family.
/// @param secret The 48-byte secret from which to derive keying material.
/// @param label TLS 1.3 label string (without the "tls13 " prefix; this
///        function prepends it internally).
/// @param context Hash context or transcript hash (NULL for none).
/// @param context_len Length of @p context in bytes.
/// @param out Output buffer for the derived keying material.
/// @param out_len Desired length of output in bytes.
/// @return 0 on success; non-zero on encoding / length overflow.
int rt_hkdf_expand_label_sha384(const uint8_t secret[48],
                                const char *label,
                                const uint8_t *context,
                                size_t context_len,
                                uint8_t *out,
                                size_t out_len);

//=========================================================================
// ChaCha20-Poly1305 AEAD
//=========================================================================

/// @brief Encrypt with ChaCha20-Poly1305.
/// @param key The 256-bit encryption key (32 bytes).
/// @param nonce The 96-bit nonce (12 bytes, must be unique per key).
/// @param aad Pointer to additional authenticated data (can be NULL).
/// @param aad_len Length of @p aad in bytes.
/// @param plaintext Pointer to the plaintext to encrypt.
/// @param plaintext_len Length of @p plaintext in bytes.
/// @param ciphertext Output buffer for ciphertext and 16-byte
///                   authentication tag (must hold plaintext_len + 16 bytes).
/// @return Ciphertext length (plaintext_len + 16 for tag).
size_t rt_chacha20_poly1305_encrypt(const uint8_t key[32],
                                    const uint8_t nonce[12],
                                    const void *aad,
                                    size_t aad_len,
                                    const void *plaintext,
                                    size_t plaintext_len,
                                    uint8_t *ciphertext);

/// @brief Decrypt with ChaCha20-Poly1305.
/// @param key The 256-bit decryption key (32 bytes).
/// @param nonce The 96-bit nonce (12 bytes, same as used during encryption).
/// @param aad Pointer to additional authenticated data (can be NULL).
/// @param aad_len Length of @p aad in bytes.
/// @param ciphertext Pointer to the ciphertext with appended 16-byte tag.
/// @param ciphertext_len Length of @p ciphertext in bytes (including tag).
/// @param plaintext Output buffer for decrypted data (must hold
///                  ciphertext_len - 16 bytes).
/// @return Plaintext length on success; -1 on invalid input, excessive length,
///         counter exhaustion, or authentication failure.
long rt_chacha20_poly1305_decrypt(const uint8_t key[32],
                                  const uint8_t nonce[12],
                                  const void *aad,
                                  size_t aad_len,
                                  const void *ciphertext,
                                  size_t ciphertext_len,
                                  uint8_t *plaintext);

//=========================================================================
// AES-128-GCM AEAD (NIST SP 800-38D)
//=========================================================================

/// @brief Encrypt with AES-128-GCM.
/// @param key The 128-bit encryption key (16 bytes).
/// @param nonce The 96-bit nonce (12 bytes, must be unique per key).
/// @param aad Pointer to additional authenticated data (can be NULL).
/// @param aad_len Length of @p aad in bytes.
/// @param plaintext Pointer to the plaintext to encrypt.
/// @param plaintext_len Length of @p plaintext in bytes.
/// @param ciphertext Output buffer for ciphertext and 16-byte
///                   authentication tag (must hold plaintext_len + 16 bytes).
/// @return Ciphertext length (plaintext_len + 16 for tag).
size_t rt_aes128_gcm_encrypt(const uint8_t key[16],
                             const uint8_t nonce[12],
                             const void *aad,
                             size_t aad_len,
                             const void *plaintext,
                             size_t plaintext_len,
                             uint8_t *ciphertext);

/// @brief Decrypt with AES-128-GCM.
/// @param key The 128-bit decryption key (16 bytes).
/// @param nonce The 96-bit nonce (12 bytes, same as used during encryption).
/// @param aad Pointer to additional authenticated data (can be NULL).
/// @param aad_len Length of @p aad in bytes.
/// @param ciphertext Pointer to the ciphertext with appended 16-byte tag.
/// @param ciphertext_len Length of @p ciphertext in bytes (including tag).
/// @param plaintext Output buffer for decrypted data (must hold
///                  ciphertext_len - 16 bytes).
/// @return Plaintext length on success; -1 on invalid input, excessive length,
///         or authentication failure.
long rt_aes128_gcm_decrypt(const uint8_t key[16],
                           const uint8_t nonce[12],
                           const void *aad,
                           size_t aad_len,
                           const void *ciphertext,
                           size_t ciphertext_len,
                           uint8_t *plaintext);

/// @brief Encrypt with AES-256-GCM.
/// @param key The 256-bit encryption key (32 bytes).
/// @param nonce The 96-bit nonce (12 bytes, must be unique per key).
/// @param aad Pointer to additional authenticated data (can be NULL).
/// @param aad_len Length of @p aad in bytes.
/// @param plaintext Pointer to the plaintext to encrypt.
/// @param plaintext_len Length of @p plaintext in bytes.
/// @param ciphertext Output buffer for ciphertext and 16-byte authentication
///        tag (must hold plaintext_len + 16 bytes).
/// @return Ciphertext length (plaintext_len + 16 for tag).
size_t rt_aes256_gcm_encrypt(const uint8_t key[32],
                             const uint8_t nonce[12],
                             const void *aad,
                             size_t aad_len,
                             const void *plaintext,
                             size_t plaintext_len,
                             uint8_t *ciphertext);

/// @brief Decrypt with AES-256-GCM.
/// @param key The 256-bit decryption key (32 bytes).
/// @param nonce The 96-bit nonce (12 bytes, same as used during encryption).
/// @param aad Pointer to additional authenticated data (can be NULL).
/// @param aad_len Length of @p aad in bytes.
/// @param ciphertext Pointer to the ciphertext with appended 16-byte tag.
/// @param ciphertext_len Length of @p ciphertext in bytes (including tag).
/// @param plaintext Output buffer for decrypted data (must hold
///        ciphertext_len - 16 bytes).
/// @return Plaintext length on success; -1 on invalid input, excessive length,
///         or authentication failure.
long rt_aes256_gcm_decrypt(const uint8_t key[32],
                           const uint8_t nonce[12],
                           const void *aad,
                           size_t aad_len,
                           const void *ciphertext,
                           size_t ciphertext_len,
                           uint8_t *plaintext);

//=========================================================================
// X25519 Key Exchange
//=========================================================================

/// @brief Generate X25519 key pair.
/// @param secret Output buffer for the 32-byte private key (randomly generated).
/// @param public_key Output buffer for the 32-byte public key derived from
///                   @p secret.
void rt_x25519_keygen(uint8_t secret[32], uint8_t public_key[32]);

/// @brief Compute X25519 shared secret.
/// @param secret The local 32-byte private key.
/// @param peer_public The peer's 32-byte public key.
/// @param shared Output buffer for the 32-byte shared secret (result of the
///               Diffie-Hellman computation on Curve25519).
/// @return 0 on success, -1 if the peer public key produces an all-zero secret.
int rt_x25519(const uint8_t secret[32], const uint8_t peer_public[32], uint8_t shared[32]);

//=========================================================================
// Random
//=========================================================================

/// @brief Generate cryptographically secure random bytes.
/// @details Uses the approved crypto module when enabled, otherwise the
///          platform entropy adapter. Entropy failure clears @p buf, traps,
///          and terminates rather than returning predictable bytes.
/// @param buf Output buffer to fill with random data.
/// @param len Number of random bytes to generate.
void rt_crypto_random_bytes(uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
