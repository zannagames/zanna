//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_entropy_platform.h
// Purpose: Shared OS entropy adapter for runtime cryptography.
//
// Key invariants:
//   - Entropy is sourced only from operating-system CSPRNG APIs.
//   - Callers decide whether an entropy failure traps, aborts, or propagates.
//   - No deterministic fallback is provided by this layer.
//
// Ownership/Lifetime:
//   - The caller owns the output buffer.
//   - The adapter allocates no long-lived state.
//
// Links: src/runtime/network/rt_crypto.c,
//        src/runtime/network/rt_crypto_module.c
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares the fail-closed operating-system entropy adapter.
 * @details Defines caller-buffer byte and scalar requests backed exclusively
 * by platform CSPRNG facilities, with no deterministic fallback or retained
 * adapter state.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Fill a caller-owned buffer with operating-system entropy.
/// @details Uses the strongest platform CSPRNG available to the adapter
///          implementation. The function never falls back to deterministic
///          bytes; failure is reported to the caller.
/// @param buf Destination buffer. May be NULL only when @p len is zero.
/// @param len Number of bytes to write.
/// @return 0 on success, -1 if the platform entropy source fails or @p buf is
///         NULL with a non-zero length.
int rt_entropy_platform_random_bytes(uint8_t *buf, size_t len);

/// @brief Read one 64-bit value from the operating-system entropy source.
/// @details This convenience helper centralizes the common "random suffix"
///          pattern used by file-system helpers. It delegates to
///          rt_entropy_platform_random_bytes(), so it inherits the same
///          fail-closed behavior and never fabricates deterministic fallback
///          bytes.
/// @param out Receives the random value on success.
/// @return 0 on success, -1 when @p out is NULL or the platform entropy source
///         fails.
int rt_entropy_platform_random_u64(uint64_t *out);

#ifdef __cplusplus
}
#endif
