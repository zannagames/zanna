//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/text/rt_keyderive.h
// Purpose: Public password-based PBKDF2-HMAC-SHA256 and scrypt APIs with
//          bounded cost parameters and Bytes or lowercase-hex output.
//
// Key invariants:
//   - PBKDF2 iterations are accepted from 100,000 through 10,000,000.
//   - scrypt N must be a power of two through 2^20; r/p are capped at 32 and
//     the ROMix allocation is capped at 64 MiB.
//   - Key length must be in [1, 1024] bytes.
//   - Callers should generate an independent salt for each derivation context and
//     retain it wherever the same key must be reproduced.
//   - Output is either a Bytes object or lowercase hex string, by entry point.
//
// Ownership/Lifetime:
//   - Returned objects are newly allocated; caller must release.
//   - Password strings and salt Bytes objects are borrowed for the call.
//   - Temporary derived-key buffers are securely erased before release.
//
// Links: src/runtime/text/rt_keyderive.c (implementation),
//        src/runtime/text/rt_keyderive_internal.h (limits and raw KDFs),
//        src/runtime/core/rt_string.h (password/hex strings),
//        src/runtime/collections/rt_bytes.h (salt and binary output)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_keyderive.h
 * @brief Declares policy-bounded PBKDF2 and scrypt password derivation APIs.
 * @details Borrowed password Strings and salt Bytes produce caller-owned
 *          binary or lowercase hexadecimal keys. Work factors, memory use, and
 *          result length are constrained, and scrypt remains unavailable when
 *          the configured approved cryptographic policy forbids it.
 */

#pragma once

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Derive a key using PBKDF2-SHA256.
/// @details Applies HMAC-SHA-256 for the requested policy-approved iteration
///          count. A real empty password is permitted, but null/invalid
///          passwords and empty/invalid salts trap.
/// @param password Borrowed non-null password string.
/// @param salt Borrowed non-empty Bytes salt.
/// @param iterations Number of iterations (100,000 through 10,000,000).
/// @param key_len Desired key length in bytes (1-1024).
/// @return Newly allocated Bytes object containing exactly @p key_len derived
///         bytes, owned by the caller, or null after a trapped error.
void *rt_keyderive_pbkdf2_sha256(rt_string password,
                                 void *salt,
                                 int64_t iterations,
                                 int64_t key_len);

/// @brief Derive a key using PBKDF2-SHA256 and return as hex string.
/// @details Uses the same validation and derivation as
///          `rt_keyderive_pbkdf2_sha256`, then lowercase-hex encodes the key.
/// @param password Borrowed non-null password string.
/// @param salt Borrowed non-empty Bytes salt.
/// @param iterations Number of iterations (100,000 through 10,000,000).
/// @param key_len Desired key length in bytes (1-1024).
/// @return Newly allocated lowercase hexadecimal string of length
///         `2 * key_len`, or an empty fallback after a trapped error.
rt_string rt_keyderive_pbkdf2_sha256_str(rt_string password,
                                         void *salt,
                                         int64_t iterations,
                                         int64_t key_len);

/// @brief Derive a key using scrypt-SHA256.
/// @details Implements RFC 7914 with PBKDF2-HMAC-SHA-256 around ROMix. Runtime
///          policy caps work/memory and disables scrypt in approved mode.
/// @param password Borrowed non-null password string; empty is permitted.
/// @param salt Borrowed non-empty Bytes salt.
/// @param n CPU/memory cost parameter; must be a power of two greater than 1.
/// @param r Block-size parameter from 1 through 32.
/// @param p Parallelization parameter from 1 through 32.
/// @param key_len Desired key length in bytes (1-1024).
/// @return Newly allocated Bytes object containing the derived key, or null
///         after a trapped error.
void *rt_keyderive_scrypt_sha256(
    rt_string password, void *salt, int64_t n, int64_t r, int64_t p, int64_t key_len);

/// @brief Derive a key using scrypt-SHA256 and return as hex string.
/// @details Uses the same validation, approved-mode gate, and RFC 7914
///          derivation as `rt_keyderive_scrypt_sha256`.
/// @param password Borrowed non-null password string; empty is permitted.
/// @param salt Borrowed non-empty Bytes salt.
/// @param n Power-of-two CPU/memory work factor greater than one.
/// @param r Block-size parameter from 1 through 32.
/// @param p Parallelization parameter from 1 through 32.
/// @param key_len Desired binary key length in bytes (1-1024).
/// @return Newly allocated lowercase hexadecimal string of length
///         `2 * key_len`, or an empty fallback after a trapped error.
rt_string rt_keyderive_scrypt_sha256_str(
    rt_string password, void *salt, int64_t n, int64_t r, int64_t p, int64_t key_len);

#ifdef __cplusplus
}
#endif
