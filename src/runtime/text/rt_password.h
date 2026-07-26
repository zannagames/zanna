//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/text/rt_password.h
// Purpose: Password hashing with automatic salt generation using scrypt-SHA256
// in compatibility mode or PBKDF2-HMAC-SHA256 in approved mode, plus strict
// stored-record parsing and fixed-time final-value comparison.
//
// Key invariants:
//   - Hash output includes its salt, algorithm, and cost parameters; no
//     separate salt or parameter storage is needed.
//   - Verification's final 32-byte comparison does not exit on the first mismatch;
//     parsing, algorithm selection, and KDF work are not constant-time.
//   - Default Hash output uses bounded scrypt in compatibility mode and PBKDF2 in approved mode.
//   - Verification rejects work factors outside the implementation caps.
//   - Supported hash formats are "SCRYPT$ln$r$p$salt_b64$hash_b64" and legacy
//     "PBKDF2$iterations$salt_b64$hash_b64".
//
// Ownership/Lifetime:
//   - Returned hash strings are newly allocated; caller must release.
//   - Password strings are borrowed for the duration of the call; not retained.
//
// Links: src/runtime/text/rt_password.c (implementation), src/runtime/text/rt_keyderive.h,
// src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_password.h
 * @brief Declares salted password hashing and stored-record verification.
 * @details The API emits caller-owned SCRYPT or PBKDF2 records containing
 *          algorithm, cost, salt, and derived value, supports validated custom
 *          costs, checks whether records require rehashing, and reports
 *          mismatches or malformed records as false.
 */

#pragma once

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Hash a password using the active cryptographic-module policy.
/// @details Uses scrypt-SHA256 and the `SCRYPT$...` format in compatibility mode;
///          approved mode uses PBKDF2-HMAC-SHA256 and `PBKDF2$...`. Both generate
///          a fresh 16-byte cryptographic salt and store all verification
///          parameters in the returned record.
/// @param password Borrowed runtime string whose complete byte sequence is hashed.
/// @return Newly allocated encoded record suitable for storage, or an empty
///         string after a validation or cryptographic trap.
rt_string rt_password_hash(rt_string password);

/// @brief Hash a password with an explicit PBKDF2 iteration count.
/// @details Uses PBKDF2-HMAC-SHA256 with a fresh 16-byte salt. Iteration counts
///          outside `[100000, 10000000]` trap rather than being clamped.
/// @param password Borrowed runtime string whose complete byte sequence is hashed.
/// @param iterations PBKDF2 iteration count from 100,000 through 10,000,000.
/// @return Newly allocated `PBKDF2$...` record, or an empty string after a trap.
rt_string rt_password_hash_with_iterations(rt_string password, int64_t iterations);

/// @brief Hash a password using scrypt with default memory-hard parameters.
/// @details Generates a fresh 16-byte salt and records the default N, r, and p
///          values. Traps when scrypt is disabled by approved mode.
/// @param password Borrowed runtime string whose complete byte sequence is hashed.
/// @return Newly allocated `SCRYPT$...` record, or an empty string after a trap.
rt_string rt_password_hash_scrypt(rt_string password);

/// @brief Hash a password using scrypt with caller-provided parameters at or above policy minimums.
/// @details @p n must be a supported power of two; @p r and @p p must be
///          positive and within runtime caps. Every parameter must also meet
///          the password policy minimum. Invalid settings trap.
/// @param password Borrowed runtime string whose complete byte sequence is hashed.
/// @param n Scrypt CPU/memory work factor.
/// @param r Scrypt block-size parameter.
/// @param p Scrypt parallelization parameter.
/// @return Newly allocated `SCRYPT$...` record, or an empty string after a trap.
rt_string rt_password_hash_scrypt_params(rt_string password, int64_t n, int64_t r, int64_t p);

/// @brief Verify a password against a stored hash.
/// @details Strictly parses the supported record, re-derives the 32-byte value,
///          and compares that final value without a first-mismatch exit.
///          Malformed records, disabled algorithms, and ordinary mismatches
///          return zero; invalid handles or cryptographic failures may trap.
/// @param password Borrowed runtime password to verify.
/// @param hash Borrowed stored record produced by a password-hash operation.
/// @return `1` if the password matches, otherwise `0`.
int8_t rt_password_verify(rt_string password, rt_string hash);

/// @brief Report whether a stored password hash should be upgraded to current defaults.
/// @details In compatibility mode, valid scrypt records at or above every
///          current policy minimum are current; PBKDF2 and weaker scrypt
///          records need rehashing. In approved mode, valid PBKDF2 records at
///          or above the default iteration count are current.
/// @param hash Borrowed encoded record to inspect.
/// @return `1` for invalid, unsupported, legacy, or under-policy records;
///         otherwise `0`.
int8_t rt_password_needs_rehash(rt_string hash);

#ifdef __cplusplus
}
#endif
