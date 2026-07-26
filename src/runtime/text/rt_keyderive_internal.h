//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/text/rt_keyderive_internal.h
// Purpose: Internal header exposing the raw PBKDF2-HMAC-SHA256 function for
//          use by crypto components, plus raw scrypt and shared public-policy
//          limits without depending on runtime string types.
//
// Key invariants:
//   - Raw PBKDF2 permits iteration count one for RFC 7914's internal calls;
//     public wrappers enforce the 100,000 minimum.
//   - Public KDF outputs are capped at 1,024 bytes; raw PBKDF2 has the larger
//     internal cap needed to construct scrypt's B buffer.
//   - scrypt work, block, parallelism, key length, and ROMix memory are bounded.
//
// Ownership/Lifetime:
//   - All input and output buffers are caller-owned and borrowed.
//   - Raw helpers allocate temporary scratch and securely erase sensitive
//     intermediates before releasing successful-path storage.
//
// Links: src/runtime/text/rt_keyderive.c (implementation),
//        src/runtime/text/rt_keyderive.h (public wrappers),
//        src/runtime/text/rt_hash.h (raw HMAC-SHA-256)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_keyderive_internal.h
 * @brief Declares raw KDF primitives and their shared resource limits.
 * @details Crypto components use the buffer-oriented PBKDF2-HMAC-SHA256 and
 *          scrypt helpers without depending on managed Strings. Inputs and
 *          output remain caller-owned, while temporary working allocations and
 *          sensitive intermediates are scrubbed internally.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Minimum iteration count accepted by public PBKDF2 wrappers.
#define RT_PBKDF2_MIN_ITERATIONS 100000U
/// Maximum PBKDF2 iteration count accepted by raw and public APIs.
#define RT_PBKDF2_MAX_ITERATIONS 10000000U
/// Maximum public PBKDF2 output length in bytes.
#define RT_PBKDF2_MAX_KEY_LEN 1024U

/// Default base-two logarithm of scrypt's N work factor.
#define RT_SCRYPT_DEFAULT_N_LOG2 14U
/// Default scrypt block-size parameter.
#define RT_SCRYPT_DEFAULT_R 8U
/// Default scrypt parallelization parameter.
#define RT_SCRYPT_DEFAULT_P 1U
/// Maximum supported base-two logarithm of scrypt's N work factor.
#define RT_SCRYPT_MAX_N_LOG2 20U
/// Maximum supported scrypt block-size parameter.
#define RT_SCRYPT_MAX_R 32U
/// Maximum supported scrypt parallelization parameter.
#define RT_SCRYPT_MAX_P 32U
/// Maximum public scrypt output length in bytes.
#define RT_SCRYPT_MAX_KEY_LEN 1024U
/// Maximum ROMix allocation and raw PBKDF2 scratch output in bytes.
#define RT_SCRYPT_MAX_MEMORY (64U * 1024U * 1024U)

/// @brief Raw PBKDF2-HMAC-SHA256 key derivation (RFC 2898 / RFC 8018).
/// @details Derives a key of arbitrary length from a password and salt by
///          iteratively applying HMAC-SHA256. This is the low-level C function
///          used by password-based encryption and scrypt. It allocates a
///          temporary `salt || INT32_BE(block)` buffer while the caller
///          provides output storage. Empty password and salt ranges are valid.
/// @param password     Password bytes (not NUL-terminated).
/// @param password_len Password length in bytes.
/// @param salt         Salt bytes.
/// @param salt_len     Salt length in bytes.
/// @param iterations   Number of PBKDF2 iterations (must be positive; public wrappers enforce
/// policy).
/// @param out          Output buffer for the derived key.
/// @param out_len      Positive derived-key length, capped internally by
///                     RT_SCRYPT_MAX_MEMORY.
void rt_keyderive_pbkdf2_sha256_raw(const uint8_t *password,
                                    size_t password_len,
                                    const uint8_t *salt,
                                    size_t salt_len,
                                    uint32_t iterations,
                                    uint8_t *out,
                                    size_t out_len);

/// @brief Raw scrypt-SHA256 key derivation (RFC 7914).
/// @details Applies PBKDF2-HMAC-SHA-256 with one iteration, ROMix for each
///          parallel block, then a final one-iteration PBKDF2. Parameters must
///          satisfy `rt_keyderive_scrypt_params_supported`; invalid buffers,
///          parameters, and allocation failures trap.
/// @param password Borrowed password bytes; null only when length is zero.
/// @param password_len Number of password bytes.
/// @param salt Borrowed salt bytes; null only when length is zero.
/// @param salt_len Number of salt bytes.
/// @param n Power-of-two ROMix work factor.
/// @param r Positive block-size parameter.
/// @param p Positive parallelization parameter.
/// @param out Writable output buffer.
/// @param out_len Number of derived bytes to produce.
void rt_keyderive_scrypt_sha256_raw(const uint8_t *password,
                                    size_t password_len,
                                    const uint8_t *salt,
                                    size_t salt_len,
                                    uint64_t n,
                                    uint32_t r,
                                    uint32_t p,
                                    uint8_t *out,
                                    size_t out_len);

/// @brief Return whether scrypt parameters satisfy runtime CPU/memory policy.
/// @details Checks power-of-two N, configured maxima, output length, every
///          relevant `size_t` product, and the 64-MiB ROMix limit without
///          allocating.
/// @param n Candidate ROMix work factor.
/// @param r Candidate block-size parameter.
/// @param p Candidate parallelization parameter.
/// @param out_len Candidate derived-key length.
/// @return Non-zero when the parameter set is supported, otherwise zero.
int rt_keyderive_scrypt_params_supported(uint64_t n, uint32_t r, uint32_t p, size_t out_len);

#ifdef __cplusplus
}
#endif
