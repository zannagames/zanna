//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/text/rt_hash_util.c
// Purpose: SipHash-2-4 per-process seed initialization. Provides a single
//          shared seed across all translation units via extern linkage.
//
// Key invariants:
//   - The seed is initialized exactly once per process via pthread_once /
//     InitOnceExecuteOnce for thread safety.
//   - The seed is sourced from the OS CSPRNG (/dev/urandom or BCryptGenRandom).
//
// Ownership/Lifetime:
//   - Global state lives for the process lifetime; no cleanup needed.
//
// Links: src/runtime/text/rt_hash_util.h (public API)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_hash_util.c
 * @brief Initializes the process-wide SipHash key from platform entropy.
 * @details A native once primitive selects a single initializer, fills both
 *          64-bit key halves through the operating-system CSPRNG, publishes
 *          readiness with appropriate ordering, and traps instead of using a
 *          predictable fallback when entropy acquisition fails.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "rt_trap.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// BCrypt declarations depend on the base Win32 types above.
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif
#else
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#endif

/// First 64-bit half of the process-wide SipHash-2-4 key.
uint64_t rt_siphash_k0_ = 0;
/// Second 64-bit half of the process-wide SipHash-2-4 key.
uint64_t rt_siphash_k1_ = 0;
/// Publication flag set with release ordering after both key halves are ready.
int rt_siphash_seeded_ = 0;

/// @brief Fill `buf` with `len` random bytes from the OS CSPRNG.
/// @details Platform split:
///          - **Windows** → `BCryptGenRandom` with the system-preferred
///            RNG; requests are chunked to the API's 32-bit byte-count limit.
///          - **Unix** → loop on `read("/dev/urandom")` because short
///            reads are legal even from `/dev/urandom` (rare in
///            practice, but kernel guarantees only "at least one byte").
///          Returns 0 on success, -1 on any failure (open, read, or
///          BCryptGenRandom error).
/// @param buf Writable destination; must be non-null when @p len is non-zero.
/// @param len Number of random bytes requested.
/// @return Zero on success, or `-1` on invalid input or entropy-source failure.
static int hash_random_fill(uint8_t *buf, size_t len) {
    if (len == 0)
        return 0;
    if (!buf)
        return -1;
#ifdef _WIN32
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
#else
#ifdef O_CLOEXEC
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
#else
    int fd = open("/dev/urandom", O_RDONLY);
#endif
    if (fd < 0)
        return -1;
    size_t done = 0;
    while (done < len) {
        ssize_t r = read(fd, buf + done, len - done);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        if (r == 0) {
            close(fd);
            return -1;
        }
        done += (size_t)r;
    }
    close(fd);
    return 0;
#endif
}

/// @brief Sample 16 bytes of CSPRNG entropy into the SipHash 128-bit key.
/// @details Run-once via `pthread_once` / `InitOnceExecuteOnce` (see
///          `rt_hash_ensure_seeded_`). On CSPRNG failure it traps, but a
///          returning trap hook currently lets the function publish the
///          zero-initialized key as seeded; callers must not treat that path
///          as a successful entropy downgrade.
static void hash_seed_init(void) {
    uint8_t buf[16];
    if (hash_random_fill(buf, 16) == 0) {
        memcpy(&rt_siphash_k0_, buf, 8);
        memcpy(&rt_siphash_k1_, buf + 8, 8);
    } else {
        rt_trap("Hash.Fast: OS CSPRNG unavailable");
    }
#if defined(_MSC_VER) && !defined(__clang__)
    rt_siphash_seeded_ = 1;
#else
    __atomic_store_n(&rt_siphash_seeded_, 1, __ATOMIC_RELEASE);
#endif
}

#ifdef _WIN32
static INIT_ONCE g_hash_seed_once_ = INIT_ONCE_STATIC_INIT;

/// @brief Win32 InitOnce callback shim — adapts our void-returning init to BOOL CALLBACK.
/// @param InitOnce Borrowed once-control object supplied by Windows.
/// @param Parameter Unused caller parameter supplied by `InitOnceExecuteOnce`.
/// @param Context Unused context-output slot supplied by Windows.
/// @return `TRUE` after invoking the seed initializer.
static BOOL CALLBACK hash_seed_once_cb(PINIT_ONCE InitOnce, PVOID Parameter, PVOID *Context) {
    (void)InitOnce;
    (void)Parameter;
    (void)Context;
    hash_seed_init();
    return TRUE;
}

/// @brief Idempotent: ensure the SipHash key has been seeded (Windows path).
/// @details Safe to call from any thread, any number of times — the
///          OS's `InitOnceExecuteOnce` serializes the first invocation
///          and short-circuits all subsequent ones with no lock cost.
void rt_hash_ensure_seeded_(void) {
    InitOnceExecuteOnce(&g_hash_seed_once_, hash_seed_once_cb, NULL, NULL);
}
#else
static pthread_once_t g_hash_seed_once_ = PTHREAD_ONCE_INIT;

/// @brief Idempotent: ensure the SipHash key has been seeded (POSIX path).
/// @details Same contract as the Windows variant — `pthread_once`
///          guarantees `hash_seed_init` runs exactly once across all
///          threads, with subsequent calls returning immediately.
void rt_hash_ensure_seeded_(void) {
    pthread_once(&g_hash_seed_once_, hash_seed_init);
}
#endif
