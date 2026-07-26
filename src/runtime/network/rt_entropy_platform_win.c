//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_entropy_platform_win.c
// Purpose: Windows OS entropy adapter for runtime cryptography.
//
// Key invariants:
//   - Entropy comes from the system-preferred BCryptGenRandom provider.
//   - Large requests are chunked to the ULONG size accepted by BCrypt.
//
// Ownership/Lifetime:
//   - The adapter opens no persistent handles and owns no global state.
//
// Links: src/runtime/network/rt_entropy_platform.h
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements Windows system-preferred CSPRNG acquisition.
 * @details Chunks caller requests to the BCrypt length type, erases the full
 * destination after any provider failure, and exposes no persistent provider
 * handle or global entropy state.
 */

#include "rt_entropy_platform.h"

#ifndef WIN32_LEAN_AND_MEAN
/** Restrict the Windows SDK surface to core declarations. */
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
/** Prevent Windows headers from defining conflicting min/max macros. */
#define NOMINMAX
#endif
#include <stdint.h>
#include <windows.h>
// BCrypt declarations depend on the base Win32 types above.
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

#ifndef NT_SUCCESS
/** Test whether an NTSTATUS value denotes success. */
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

/// @brief Fill a buffer from the Windows system CSPRNG.
/// @details Calls BCryptGenRandom() with BCRYPT_USE_SYSTEM_PREFERRED_RNG,
///          chunking requests larger than ULONG_MAX. On source failure the
///          complete destination is erased so callers cannot consume a mixed
///          prefix of fresh entropy and stale trailing bytes.
/// @param buf Destination buffer. May be NULL only for zero-length requests.
/// @param len Number of bytes to produce.
/// @return 0 on success, -1 on invalid arguments or BCrypt failure.
int rt_entropy_platform_random_bytes(uint8_t *buf, size_t len) {
    if (!buf && len > 0)
        return -1;
    size_t off = 0;
    while (off < len) {
        size_t chunk = len - off;
        if (chunk > UINT32_MAX)
            chunk = UINT32_MAX;
        NTSTATUS status =
            BCryptGenRandom(NULL, buf + off, (ULONG)chunk, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (!NT_SUCCESS(status)) {
            SecureZeroMemory(buf, len);
            return -1;
        }
        off += chunk;
    }
    return 0;
}

/// @brief Fill a 64-bit scalar from the Windows entropy adapter.
/// @details Delegates to BCrypt-backed rt_entropy_platform_random_bytes() so
///          all runtime secure-random callers share one failure policy.
/// @param out Receives the random scalar on success.
/// @return 0 on success, -1 on invalid arguments or entropy failure.
int rt_entropy_platform_random_u64(uint64_t *out) {
    if (!out)
        return -1;
    *out = 0;
    return rt_entropy_platform_random_bytes((uint8_t *)out, sizeof(*out));
}
