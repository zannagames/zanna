//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/text/rt_aes.h
// Purpose: AES-128/AES-256 CBC compatibility helpers, authenticated GCM byte
// encryption, and PBKDF2-derived AES-128-GCM string encryption, implemented in
// pure C with no external dependencies.
//
// Key invariants:
//   - Supports AES-128 (16-byte key) and AES-256 (32-byte key).
//   - CBC mode requires a 16-byte initialization vector (IV).
//   - PKCS7 padding is applied automatically; output is always a multiple of 16 bytes.
//   - Decryption validates and removes PKCS7 padding; returns NULL on invalid padding.
//   - Authenticated byte frames are [VAK1 magic(4)][nonce(12)][ciphertext][tag(16)].
//   - Authenticated string frames are [VAG1 magic(4)][iterations(4)][salt(16)]
//     [nonce(12)][ciphertext][tag(16)].
//
// Ownership/Lifetime:
//   - Returned strings/Bytes objects are newly allocated; caller must release.
//   - Key and IV buffers are borrowed; callers retain ownership.
//
// Links: src/runtime/text/rt_aes.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_aes.h
 * @brief Declares AES-CBC compatibility and authenticated AES-GCM APIs.
 * @details Byte operations accept borrowed keys, IVs, and optional AAD and
 *          return caller-owned Bytes or Result values. String helpers derive
 *          keys with PBKDF2 and use self-describing authenticated frames with
 *          embedded work factor, salt, nonce, ciphertext, and tag.
 */

#pragma once

#include "rt_string.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Encrypt data using AES-CBC.
/// @details Applies PKCS7 padding and selects AES-128 or AES-256 from the key
///          length. CBC is rejected while the crypto module is in approved mode.
/// @param data Required Bytes plaintext; empty plaintext is valid and produces
///        one full padding block.
/// @param key Required Bytes key of exactly 16 or 32 bytes.
/// @param iv Required Bytes initialization vector of exactly 16 bytes.
/// @return Newly allocated Bytes ciphertext whose length is a positive multiple
///         of 16, or NULL after validation/allocation/encryption failure.
/// @warning CBC is unauthenticated. Use a fresh unpredictable IV for every
///          encryption under a given key and prefer rt_aes_encrypt_auth().
void *rt_aes_encrypt(void *data, void *key, void *iv);

/// @brief Decrypt data using AES-CBC.
/// @details Decrypts complete blocks and validates/removes PKCS7 padding in a
///          constant-time final-block check. CBC is rejected in approved mode.
/// @param data Required nonempty Bytes ciphertext with length divisible by 16.
/// @param key Required Bytes key of exactly 16 or 32 bytes.
/// @param iv Required Bytes initialization vector of exactly 16 bytes.
/// @return Newly allocated plaintext Bytes, or NULL for malformed ciphertext,
///         invalid padding, rejected mode, validation, or allocation failure.
/// @warning A successful CBC padding check does not authenticate ciphertext.
void *rt_aes_decrypt(void *data, void *key, void *iv);

/// @brief Decrypt AES-CBC data and report failures as a Result.
/// @details Compatibility wrapper for @ref rt_aes_decrypt. Returns
///          `Ok(Bytes)` for valid plaintext and `Err(str)` for invalid padding,
///          malformed input, approved-mode rejection, invalid key/IV sizes, or
///          traps raised by the legacy decryptor.
/// @param data Bytes object containing ciphertext.
/// @param key Bytes object containing a 16-byte or 32-byte AES key.
/// @param iv Bytes object containing the 16-byte initialization vector.
/// @return Caller-owned Zanna.Result containing plaintext Bytes or a diagnostic string.
void *rt_aes_decrypt_result(void *data, void *key, void *iv);

/// @brief Attempt AES-CBC decryption and discard diagnostic details.
/// @details Returns `Some(Bytes)` for valid plaintext and `None` for invalid
///          padding, malformed input, approved-mode rejection, invalid key/IV
///          sizes, or traps raised by the legacy decryptor.
/// @param data Bytes object containing ciphertext.
/// @param key Bytes object containing a 16-byte or 32-byte AES key.
/// @param iv Bytes object containing the 16-byte initialization vector.
/// @return Caller-owned Zanna.Option containing plaintext Bytes, or None.
void *rt_aes_try_decrypt(void *data, void *key, void *iv);

/// @brief Encrypt data using AES-128-GCM or AES-256-GCM with optional authenticated data.
/// @details Generates a fresh 12-byte nonce and authenticates both the VAK1
///          framing header and optional caller AAD. The frame layout is
///          `[magic(4)][nonce(12)][ciphertext][tag(16)]`.
/// @param data Required Bytes plaintext.
/// @param key Required Bytes key of exactly 16 or 32 bytes.
/// @param aad Optional Bytes additional authenticated data; NULL means none.
/// @return Newly allocated framed ciphertext Bytes, or NULL after validation,
///         RNG, size, allocation, or encryption failure.
void *rt_aes_encrypt_auth(void *data, void *key, void *aad);

/// @brief Decrypt data produced by rt_aes_encrypt_auth.
/// @details Verifies the VAK1 header and GCM tag using the same optional AAD.
///          Header, key, ciphertext, tag, or AAD mismatch is authentication
///          failure and never returns unauthenticated plaintext.
/// @param data Required framed ciphertext Bytes from rt_aes_encrypt_auth().
/// @param key Required Bytes key of exactly 16 or 32 bytes.
/// @param aad Optional Bytes AAD identical to the encryption input.
/// @return Newly allocated plaintext Bytes, or NULL on authentication,
///         framing, validation, or allocation failure.
void *rt_aes_decrypt_auth(void *data, void *key, void *aad);

/// @brief Decrypt AES-GCM authenticated data and report failures as a Result.
/// @details Compatibility wrapper for @ref rt_aes_decrypt_auth. Returns
///          `Ok(Bytes)` when the ciphertext and optional AAD authenticate, and
///          `Err(str)` for tag mismatch, malformed input, invalid key sizes, or
///          traps raised by the legacy decryptor.
/// @param data Framed ciphertext produced by @ref rt_aes_encrypt_auth.
/// @param key Bytes object containing a 16-byte or 32-byte AES key.
/// @param aad Additional authenticated data; may be NULL.
/// @return Caller-owned Zanna.Result containing plaintext Bytes or a diagnostic string.
void *rt_aes_decrypt_auth_result(void *data, void *key, void *aad);

/// @brief Attempt AES-GCM authenticated decryption and discard diagnostic details.
/// @details Returns `Some(Bytes)` only when ciphertext and AAD authenticate.
///          Any failure is represented as `None`.
/// @param data Framed ciphertext produced by @ref rt_aes_encrypt_auth.
/// @param key Bytes object containing a 16-byte or 32-byte AES key.
/// @param aad Additional authenticated data; may be NULL.
/// @return Caller-owned Zanna.Option containing plaintext Bytes, or None.
void *rt_aes_try_decrypt_auth(void *data, void *key, void *aad);

/// @brief Encrypt string using PBKDF2-derived AES-128-GCM.
/// @details Generates a random 16-byte salt and 12-byte nonce, derives a
///          128-bit key with 300,000 PBKDF2-HMAC-SHA256 iterations, and
///          authenticates the entire 36-byte header as GCM AAD.
/// @param data Required runtime plaintext string; empty is valid.
/// @param password Required nonempty runtime password string.
/// @return Newly allocated Bytes frame
///         `[VAG1(4)|iterations-BE32(4)|salt(16)|nonce(12)|ciphertext|tag(16)]`,
///         or NULL after validation, RNG, derivation, allocation, or encryption failure.
void *rt_aes_encrypt_str(rt_string data, rt_string password);

/// @brief Decrypt to string using the authenticated string format.
/// @details Accepts the current AES-128-GCM format and the legacy
///          [16-byte IV | AES-256-CBC ciphertext] format for compatibility.
///          Legacy password derivation considers at most the first 256 bytes;
///          a legacy IV beginning with `VAG1` is ambiguous with current framing.
/// @param data Required Bytes containing a VAG1 authenticated frame or legacy
///        `[IV(16)|AES-256-CBC ciphertext]` payload.
/// @param password Required nonempty runtime password string.
/// @return Newly allocated plaintext string on success. Authentication,
///         framing, password, padding, or allocation failures trap and provide
///         an empty fallback only if trap handling returns.
rt_string rt_aes_decrypt_str(void *data, rt_string password);

/// @brief Decrypt an AES encrypted string and report failures as a Result.
/// @details Compatibility wrapper for @ref rt_aes_decrypt_str. Returns
///          `Ok(str)` for valid plaintext and `Err(str)` for authentication,
///          format, password, or runtime-trap failures.
/// @param data Bytes object containing encrypted string payload.
/// @param password Password string used for key derivation.
/// @return Caller-owned Zanna.Result containing plaintext string or diagnostic string.
void *rt_aes_decrypt_str_result(void *data, rt_string password);

/// @brief Attempt AES encrypted string decryption and discard diagnostic details.
/// @details Returns `Some(str)` for valid plaintext and `None` when decryption
///          fails or the legacy decryptor traps.
/// @param data Bytes object containing encrypted string payload.
/// @param password Password string used for key derivation.
/// @return Caller-owned Zanna.Option containing plaintext string, or None.
void *rt_aes_try_decrypt_str(void *data, rt_string password);

#ifdef __cplusplus
}
#endif
