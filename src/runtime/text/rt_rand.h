//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/text/rt_rand.h
// Purpose: Declares cryptographically secure byte and inclusive-integer
//          generation using the OS CSPRNG in compatibility mode and the
//          in-tree HMAC-DRBG in approved mode.
//
// Key invariants:
//   - Compatibility mode uses the OS CSPRNG; approved mode uses the module HMAC-DRBG.
//   - rt_crypto_rand_bytes accepts zero and returns the requested-length Bytes object.
//   - rt_crypto_rand_int uses rejection sampling and returns a value in [min, max].
//
// Ownership/Lifetime:
//   - Returned Bytes objects are newly allocated; caller must release.
//   - Approved mode and the Unix fallback retain process-global DRBG/descriptor state.
//
// Links: src/runtime/text/rt_rand.c (implementation),
//        src/runtime/collections/rt_bytes.h (returned byte container),
//        src/runtime/network/rt_crypto_module.h (approved-mode DRBG)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_rand.h
 * @brief Declares cryptographically secure byte and inclusive-integer generation.
 * @details Callers request newly allocated Bytes or a uniformly distributed
 *          signed integer over any valid inclusive range. The active crypto
 *          policy selects direct operating-system entropy or the in-tree
 *          approved HMAC-DRBG.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Generate cryptographically secure random bytes.
/// @details Compatibility mode reads the platform CSPRNG; approved mode uses
///          the module HMAC-DRBG. Zero is valid and returns an empty object.
///          Negative, unrepresentable, unallocatable, or entropy-failure
///          requests trap.
/// @param count Number of bytes to generate; must be nonnegative.
/// @return Caller-owned Bytes object containing the requested random bytes, or
///         an empty Bytes object if execution resumes after a trap.
void *rt_crypto_rand_bytes(int64_t count);

/// @brief Generate a cryptographically secure random integer in range [min, max].
/// @details Uses rejection sampling to avoid modulo bias and supports every
///          inclusive interval representable by two `int64_t` endpoints,
///          including the complete signed 64-bit domain.
/// @param min Inclusive lower bound.
/// @param max Inclusive upper bound.
/// @return Uniformly distributed random integer in `[@p min, @p max]`.
/// @note Traps when @p min exceeds @p max; entropy failure traps and aborts.
int64_t rt_crypto_rand_int(int64_t min, int64_t max);

#ifdef __cplusplus
}
#endif
