//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_entropy_platform_posix.c
// Purpose: POSIX OS entropy adapter for runtime cryptography.
//
// Key invariants:
//   - macOS uses arc4random_buf(), which is kernel-seeded and does not fail.
//   - Linux tries getrandom() before falling back to /dev/urandom.
//   - Generic POSIX platforms use /dev/urandom.
//
// Ownership/Lifetime:
//   - File descriptors opened for /dev/urandom are closed before return.
//
// Links: src/runtime/network/rt_entropy_platform.h
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements POSIX-family operating-system entropy acquisition.
 * @details Uses arc4random_buf on macOS, bounded getrandom requests on Linux,
 * and close-on-exec /dev/urandom reads as a checked fallback while preserving
 * interrupted-read and descriptor-error semantics.
 */

#include "rt_entropy_platform.h"
#include "rt_platform.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#if RT_PLATFORM_LINUX
#include <sys/random.h>
#endif
#include <unistd.h>

/** Maximum bytes requested from one Linux getrandom system call. */
#define RT_ENTROPY_GETRANDOM_CHUNK ((size_t)256u * 1024u)

#if RT_PLATFORM_MACOS
/// @brief Darwin libc CSPRNG entry point.
/// @details Declared locally because older SDK feature sets do not always
///          expose the prototype through the included C headers.
extern void arc4random_buf(void *buf, size_t nbytes);
#endif

/// @brief Fill a buffer from POSIX-style operating-system entropy sources.
/// @details macOS uses arc4random_buf(); Linux first uses getrandom() and falls
///          back to /dev/urandom when the syscall is unavailable or blocked by
///          a sandbox, preserving any bytes already produced. Generic POSIX
///          platforms read /dev/urandom directly.
/// @param buf Destination buffer. May be NULL only for zero-length requests.
/// @param len Number of bytes to produce.
/// @return 0 on success, -1 on invalid arguments or source failure.
int rt_entropy_platform_random_bytes(uint8_t *buf, size_t len) {
    if (!buf && len > 0)
        return -1;
    if (len == 0)
        return 0;

    size_t off = 0;

#if RT_PLATFORM_MACOS
    arc4random_buf(buf, len);
    return 0;
#elif RT_PLATFORM_LINUX
    while (off < len) {
        size_t request = len - off;
        if (request > RT_ENTROPY_GETRANDOM_CHUNK)
            request = RT_ENTROPY_GETRANDOM_CHUNK;
        ssize_t n = getrandom(buf + off, request, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == ENOSYS || errno == EPERM || errno == EACCES)
                break;
            return -1;
        }
        if (n == 0)
            return -1;
        off += (size_t)n;
    }
    if (off == len)
        return 0;
#endif

#ifdef O_CLOEXEC
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
#else
    int fd = open("/dev/urandom", O_RDONLY);
#endif
    if (fd < 0)
        return -1;

#ifndef O_CLOEXEC
    int descriptor_flags = fcntl(fd, F_GETFD);
    if (descriptor_flags < 0 || fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0) {
        int saved_errno = errno;
        (void)close(fd);
        errno = saved_errno;
        return -1;
    }
#endif

    while (off < len) {
        size_t request = len - off;
        if (request > (size_t)SSIZE_MAX)
            request = (size_t)SSIZE_MAX;
        ssize_t n = read(fd, buf + off, request);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            int saved_errno = errno;
            (void)close(fd);
            errno = saved_errno;
            return -1;
        }
        if (n == 0) {
            int saved_errno = EIO;
            (void)close(fd);
            errno = saved_errno;
            return -1;
        }
        off += (size_t)n;
    }
    if (close(fd) < 0)
        return -1;
    return 0;
}

/// @brief Fill a 64-bit scalar from the POSIX entropy adapter.
/// @details Keeps temporary-file and atomic-save helpers from opening
///          /dev/urandom directly. The byte-level adapter owns Linux
///          getrandom(), macOS arc4random_buf(), and /dev/urandom fallback
///          policy.
/// @param out Receives the random scalar on success.
/// @return 0 on success, -1 on invalid arguments or entropy failure.
int rt_entropy_platform_random_u64(uint64_t *out) {
    if (!out)
        return -1;
    return rt_entropy_platform_random_bytes((uint8_t *)out, sizeof(*out));
}
