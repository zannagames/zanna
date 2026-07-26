//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/text/rt_cipher.h
// Purpose: High-level encryption/decryption API using ChaCha20-Poly1305 AEAD in compatibility
// mode or AES-256-GCM in approved mode, with automatic nonce generation and
// PBKDF2-HMAC-SHA256 key derivation from passwords.
//
// Key invariants:
//   - Password-based format: [magic][iterations][16 bytes salt][12 bytes nonce][ciphertext][tag].
//   - Key-based format: [magic][12 bytes nonce][ciphertext][16 bytes tag].
//   - Each nonce is 12 independent CSPRNG bytes. Under random-nonce guidance,
//     callers should keep use of any one key well below roughly 2^32 messages.
//   - Decryption returns NULL for invalid/corrupt ciphertext (authentication failure).
//
// Ownership/Lifetime:
//   - Returned Bytes objects are newly allocated; caller must release.
//   - Password strings and key/AAD Bytes objects are borrowed for the call.
//
// Links: src/runtime/text/rt_cipher.c (implementation), src/runtime/network/rt_crypto.h,
// src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_cipher.h
 * @brief Declares password- and raw-key authenticated encryption operations.
 * @details The API emits self-describing caller-owned ciphertext frames,
 *          authenticates optional associated data, selects the configured
 *          approved or compatibility AEAD, and reports authentication failure
 *          through nullable, Result, or Option interfaces.
 */

#pragma once

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//=========================================================================
// Password-Based Encryption
//=========================================================================

/// @brief Encrypt data using a password.
/// @details Derives a 256-bit key from the password using PBKDF2-HMAC-SHA256
///          with a random 16-byte salt and a fixed strong work factor.
///          Generates a full-width random 12-byte nonce.
///          Uses ChaCha20-Poly1305 AEAD in compatibility mode and AES-256-GCM in approved mode.
/// @param plaintext Bytes object containing data to encrypt.
/// @param password Password string used for key derivation.
/// @return Newly allocated Bytes frame
///         `[magic(4)|iterations(4)|salt(16)|nonce(12)|ciphertext|tag(16)]`.
/// @note Traps if plaintext is NULL or password is empty.
void *rt_cipher_encrypt(void *plaintext, rt_string password);

/// @brief Encrypt data using a password and caller-supplied authenticated data.
/// @details Uses the same password derivation, mode selection, random salt, and
///          random nonce as rt_cipher_encrypt(). The AEAD tag authenticates the
///          complete format header followed by @p aad.
/// @param plaintext Required Bytes plaintext.
/// @param password Required nonempty password string.
/// @param aad Optional Bytes additional authenticated data; NULL means none.
/// @return Newly allocated framed ciphertext Bytes, or NULL after a trapped or
///         allocation/encryption failure.
void *rt_cipher_encrypt_aad(void *plaintext, rt_string password, void *aad);

/// @brief Decrypt data that was encrypted with rt_cipher_encrypt.
/// @param ciphertext Bytes object containing encrypted data.
/// @param password Password string used for key derivation.
/// @return Newly allocated plaintext Bytes, or NULL on authentication failure.
/// @note Returns NULL on authentication failure; malformed arguments may trap.
void *rt_cipher_decrypt(void *ciphertext, rt_string password);

/// @brief Decrypt password-encrypted data and report failures as a Result.
/// @details This is the production-facing companion to @ref rt_cipher_decrypt.
///          It returns `Ok(Bytes)` for valid authenticated plaintext and
///          `Err(str)` for authentication failure, malformed ciphertext, empty
///          passwords, or other runtime traps raised by the legacy decryptor.
///          Existing `Decrypt` behavior is unchanged; this wrapper simply
///          captures its failure paths into an explicit value.
/// @param ciphertext Bytes object containing encrypted data.
/// @param password Password string used for key derivation.
/// @return Caller-owned Zanna.Result containing plaintext Bytes or a diagnostic string.
void *rt_cipher_decrypt_result(void *ciphertext, rt_string password);

/// @brief Attempt password-based decryption and discard diagnostic details.
/// @details Returns `Some(Bytes)` for valid authenticated plaintext and `None`
///          for authentication failure or any trap raised by the legacy
///          decryptor. Use @ref rt_cipher_decrypt_result when callers need a
///          failure message.
/// @param ciphertext Bytes object containing encrypted data.
/// @param password Password string used for key derivation.
/// @return Caller-owned Zanna.Option containing plaintext Bytes, or None.
void *rt_cipher_try_decrypt(void *ciphertext, rt_string password);

/// @brief Decrypt password-encrypted data while authenticating caller-supplied AAD.
/// @details Selects the algorithm from the authenticated frame, re-derives the
///          key, and verifies header, ciphertext, tag, and @p aad before
///          publishing plaintext. Approved mode rejects non-approved frames.
/// @param ciphertext Required framed ciphertext Bytes.
/// @param password Required nonempty password used for encryption.
/// @param aad Optional Bytes AAD identical to the encryption input.
/// @return Newly allocated plaintext Bytes, or NULL on authentication failure;
///         malformed arguments and disallowed formats may trap.
void *rt_cipher_decrypt_aad(void *ciphertext, rt_string password, void *aad);

/// @brief Decrypt password-encrypted data with AAD and report failures as a Result.
/// @details Authenticates the same additional data used at encryption time.
///          Returns `Ok(Bytes)` on success and `Err(str)` for authentication,
///          format, argument, or runtime-trap failures.
/// @param ciphertext Framed ciphertext produced by @ref rt_cipher_encrypt_aad.
/// @param password Password string used for key derivation.
/// @param aad Additional authenticated data; may be NULL when encryption used none.
/// @return Caller-owned Zanna.Result containing plaintext Bytes or a diagnostic string.
void *rt_cipher_decrypt_aad_result(void *ciphertext, rt_string password, void *aad);

/// @brief Attempt password-based AAD decryption and discard diagnostic details.
/// @details Returns `Some(Bytes)` only when both ciphertext and AAD authenticate.
///          Authentication failure, malformed input, and legacy traps all
///          produce `None`.
/// @param ciphertext Framed ciphertext produced by @ref rt_cipher_encrypt_aad.
/// @param password Password string used for key derivation.
/// @param aad Additional authenticated data; may be NULL when encryption used none.
/// @return Caller-owned Zanna.Option containing plaintext Bytes, or None.
void *rt_cipher_try_decrypt_aad(void *ciphertext, rt_string password, void *aad);

//=========================================================================
// Key-Based Encryption (for pre-derived keys)
//=========================================================================

/// @brief Encrypt data using a raw 256-bit key.
/// @details Generates a full-width random 12-byte nonce. Uses
///          ChaCha20-Poly1305 in compatibility mode or AES-256-GCM in approved mode.
/// @param plaintext Bytes object containing data to encrypt.
/// @param key Bytes object containing exactly 32 bytes (256-bit key).
/// @return Newly allocated Bytes frame `[magic(4)|nonce(12)|ciphertext|tag(16)]`.
/// @note Traps if plaintext is NULL or key is not 32 bytes.
void *rt_cipher_encrypt_with_key(void *plaintext, void *key);

/// @brief Encrypt data using a raw 256-bit key and caller-supplied authenticated data.
/// @details Skips password derivation, generates a full-width random nonce, and
///          authenticates the complete raw-key frame header followed by @p aad.
/// @param plaintext Required Bytes plaintext.
/// @param key Required Bytes key of exactly 32 bytes.
/// @param aad Optional Bytes additional authenticated data; NULL means none.
/// @return Newly allocated framed ciphertext Bytes, or NULL after a trapped or
///         allocation/encryption failure.
void *rt_cipher_encrypt_with_key_aad(void *plaintext, void *key, void *aad);

/// @brief Decrypt data that was encrypted with rt_cipher_encrypt_with_key.
/// @param ciphertext Bytes object containing encrypted data.
/// @param key Bytes object containing exactly 32 bytes (256-bit key).
/// @return Newly allocated plaintext Bytes, or NULL on authentication failure.
/// @note Traps only if ciphertext is malformed or the key has the wrong size.
void *rt_cipher_decrypt_with_key(void *ciphertext, void *key);

/// @brief Decrypt raw-key encrypted data and report failures as a Result.
/// @details Returns `Ok(Bytes)` for valid authenticated plaintext and `Err(str)`
///          for wrong keys, tampering, malformed ciphertext, invalid key size,
///          or other traps raised by the legacy decryptor.
/// @param ciphertext Bytes object containing encrypted data.
/// @param key Bytes object containing exactly 32 bytes.
/// @return Caller-owned Zanna.Result containing plaintext Bytes or a diagnostic string.
void *rt_cipher_decrypt_with_key_result(void *ciphertext, void *key);

/// @brief Attempt raw-key decryption and discard diagnostic details.
/// @details Returns `Some(Bytes)` for valid authenticated plaintext and `None`
///          for authentication, format, key-size, or runtime-trap failures.
/// @param ciphertext Bytes object containing encrypted data.
/// @param key Bytes object containing exactly 32 bytes.
/// @return Caller-owned Zanna.Option containing plaintext Bytes, or None.
void *rt_cipher_try_decrypt_with_key(void *ciphertext, void *key);

/// @brief Decrypt key-encrypted data while authenticating caller-supplied AAD.
/// @details Selects the algorithm from the authenticated frame and verifies the
///          header, ciphertext, tag, and @p aad with the supplied 32-byte key.
///          Approved mode rejects non-approved frames.
/// @param ciphertext Required framed ciphertext Bytes.
/// @param key Required Bytes key of exactly 32 bytes.
/// @param aad Optional Bytes AAD identical to the encryption input.
/// @return Newly allocated plaintext Bytes, or NULL on authentication failure;
///         malformed arguments and disallowed formats may trap.
void *rt_cipher_decrypt_with_key_aad(void *ciphertext, void *key, void *aad);

/// @brief Decrypt raw-key encrypted data with AAD and report failures as a Result.
/// @details Authenticates the same additional data used at encryption time.
///          Returns `Ok(Bytes)` on success and `Err(str)` for authentication,
///          format, key-size, or runtime-trap failures.
/// @param ciphertext Framed ciphertext produced by @ref rt_cipher_encrypt_with_key_aad.
/// @param key Bytes object containing exactly 32 bytes.
/// @param aad Additional authenticated data; may be NULL when encryption used none.
/// @return Caller-owned Zanna.Result containing plaintext Bytes or a diagnostic string.
void *rt_cipher_decrypt_with_key_aad_result(void *ciphertext, void *key, void *aad);

/// @brief Attempt raw-key AAD decryption and discard diagnostic details.
/// @details Returns `Some(Bytes)` only when both ciphertext and AAD authenticate.
///          Any failure is represented as `None`.
/// @param ciphertext Framed ciphertext produced by @ref rt_cipher_encrypt_with_key_aad.
/// @param key Bytes object containing exactly 32 bytes.
/// @param aad Additional authenticated data; may be NULL when encryption used none.
/// @return Caller-owned Zanna.Option containing plaintext Bytes, or None.
void *rt_cipher_try_decrypt_with_key_aad(void *ciphertext, void *key, void *aad);

//=========================================================================
// Key Generation
//=========================================================================

/// @brief Generate a random 256-bit encryption key.
/// @return Newly allocated Bytes object containing 32 CSPRNG bytes suitable for
///         the raw-key APIs.
void *rt_cipher_generate_key(void);

/// @brief Derive a 256-bit key from a password and salt.
/// @details Uses PBKDF2-HMAC-SHA256 to derive a key from the password.
/// @param password Password string.
/// @param salt Bytes object containing salt (recommended: 16+ bytes).
/// @return Newly allocated Bytes object containing the 32-byte derived key, or
///         NULL after validation/allocation failure.
void *rt_cipher_derive_key(rt_string password, void *salt);

#ifdef __cplusplus
}
#endif
