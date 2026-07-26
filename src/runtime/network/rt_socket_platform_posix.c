//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_socket_platform_posix.c
// Purpose: POSIX socket adapter implementation for the runtime networking stack.
//
// Key invariants:
//   - WinSock initialization is a no-op on POSIX-style platforms.
//   - poll() readiness waits preserve one monotonic deadline across EINTR and
//     translate error-only readiness into a current native error.
//   - macOS SIGPIPE suppression uses SO_NOSIGPIPE once per socket, while any
//     platform exposing MSG_NOSIGNAL also receives per-send suppression.
//
// Ownership/Lifetime:
//   - The adapter does not own sockets except during rt_socket_close().
//
// Links: src/runtime/network/rt_socket_platform.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_socket_platform_posix.c
 * @brief Implements the POSIX side of the native socket adapter.
 * @details The adapter wraps descriptor lifecycle and errno classification,
 *          applies nonblocking and timeout options, suppresses SIGPIPE where
 *          required, and implements interruption-safe readiness waits against
 *          a single monotonic deadline.
 */

#include "rt_socket_platform.h"

#include <string.h>
#include <time.h>

/// @brief No-op WinSock initializer for POSIX-style socket stacks.
/// @details Keeps call sites platform-neutral; only the Windows adapter has
///          process-level socket startup work to perform.
void rt_net_init_wsa(void) {}

/// @brief Close a POSIX socket descriptor.
/// @param sock File descriptor returned by socket().
/// @return Native close() result.
int rt_socket_close(socket_t sock) {
    return close(sock);
}

/// @brief Interrupt reads and writes while leaving descriptor ownership intact.
/// @param sock Connected POSIX socket descriptor.
/// @return Result from `shutdown(SHUT_RDWR)`, or -1 for an invalid descriptor.
int rt_socket_shutdown_both(socket_t sock) {
    if (sock == INVALID_SOCK)
        return -1;
    return shutdown(sock, SHUT_RDWR);
}

/// @brief Return the current POSIX socket error indicator.
/// @return The calling thread's errno value.
int rt_socket_last_error(void) {
    return errno;
}

/// @brief Classify an interrupted POSIX socket operation.
/// @param err Native errno value to test.
/// @return true when @p err is EINTR.
bool rt_socket_error_is_interrupted(int err) {
    return err == EINTR;
}

/// @brief Classify non-blocking POSIX retry conditions.
/// @param err Native errno value to test.
/// @return true when @p err is EAGAIN or EWOULDBLOCK.
bool rt_socket_error_is_would_block(int err) {
    return err == EAGAIN || err == EWOULDBLOCK;
}

/// @brief Classify an in-progress non-blocking POSIX connect().
/// @param err Native errno value to test.
/// @return true when @p err is EINPROGRESS.
bool rt_socket_error_is_in_progress(int err) {
    return err == EINPROGRESS;
}

/// @brief Classify a POSIX socket timeout error.
/// @param err Native errno value to test.
/// @return true when @p err is ETIMEDOUT.
bool rt_socket_error_is_timeout(int err) {
    return err == ETIMEDOUT;
}

/// @brief Classify a POSIX oversized datagram/message error.
/// @param err Native errno value to test.
/// @return true when @p err is EMSGSIZE.
bool rt_socket_error_is_message_too_large(int err) {
    return err == EMSGSIZE;
}

/// @brief Classify the last POSIX receive error as timeout or would-block.
/// @return true when errno is EAGAIN, EWOULDBLOCK, or ETIMEDOUT.
bool rt_socket_recv_timed_out(void) {
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT;
}

/// @brief Read CLOCK_MONOTONIC without entering the managed trap subsystem.
/// @details Socket lifetime loops use this direct adapter so an elapsed-time
///          query cannot non-locally exit while an in-flight listener operation
///          is registered. Seconds and nanoseconds are converted with unsigned
///          arithmetic after validating the native fields.
/// @return Monotonic milliseconds, UINT64_MAX when conversion would overflow,
///         or zero if the platform query fails or returns malformed fields.
uint64_t rt_socket_monotonic_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0 || now.tv_nsec < 0 ||
        now.tv_nsec >= 1000000000L)
        return 0;
    const uint64_t seconds = (uint64_t)now.tv_sec;
    const uint64_t fractional_ms = (uint64_t)now.tv_nsec / UINT64_C(1000000);
    if (seconds > UINT64_MAX / UINT64_C(1000))
        return UINT64_MAX;
    const uint64_t whole_ms = seconds * UINT64_C(1000);
    if (whole_ms > UINT64_MAX - fractional_ms)
        return UINT64_MAX;
    return whole_ms + fractional_ms;
}

/// @brief Detect accept() failures caused by listener shutdown races.
/// @param err Native errno value returned after accept() failed.
/// @return true for expected close/interruption errors during shutdown.
bool rt_socket_accept_interrupted_by_close(int err) {
    return err == EBADF || err == EINVAL || err == EINTR || err == ECONNABORTED;
}

/// @brief Suppress SIGPIPE for sockets on platforms with per-socket support.
/// @details macOS accepts SO_NOSIGPIPE; platforms exposing MSG_NOSIGNAL use
///          SEND_FLAGS on send() as a second line of defense.
/// @param sock Socket descriptor to configure.
void suppress_sigpipe(socket_t sock) {
#if RT_PLATFORM_MACOS && defined(SO_NOSIGPIPE)
    int val = 1;
    setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &val, sizeof(val));
#endif
    (void)sock;
}

/// @brief Toggle O_NONBLOCK on a POSIX socket.
/// @param sock Socket descriptor to update.
/// @param nonblocking true to enable non-blocking mode, false to restore blocking mode.
/// @return true when both fcntl calls succeed, false otherwise.
bool rt_socket_set_nonblocking(socket_t sock, bool nonblocking) {
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0)
        return false;
    int new_flags = nonblocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    if (new_flags == flags)
        return true;
    return fcntl(sock, F_SETFL, new_flags) == 0;
}

/// @brief Query queued read bytes for a POSIX socket.
/// @details Uses FIONREAD via ioctl() to report the queued byte count.
/// @param sock Socket descriptor to inspect.
/// @param bytes_out Receives the queued byte count on success.
/// @return true if the query succeeded, false otherwise.
bool rt_socket_available_bytes(socket_t sock, int64_t *bytes_out) {
    if (!bytes_out)
        return false;
    *bytes_out = 0;
    int bytes_available = 0;
    if (ioctl(sock, FIONREAD, &bytes_available) != 0)
        return false;
    if (bytes_available < 0)
        return false;
    *bytes_out = (int64_t)bytes_available;
    return true;
}

/// @brief Query `SO_ERROR` using the POSIX `socklen_t` ABI.
/// @param sock Socket descriptor to inspect.
/// @param error_out Receives zero or the pending errno-style socket error.
/// @return true when getsockopt succeeds, false otherwise.
bool rt_socket_pending_error(socket_t sock, int *error_out) {
    if (!error_out)
        return false;
    *error_out = 0;
    socklen_t error_len = (socklen_t)sizeof(*error_out);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, error_out, &error_len) != 0)
        return false;
    if (error_len != (socklen_t)sizeof(*error_out)) {
        *error_out = 0;
        errno = EIO;
        return false;
    }
    return true;
}

/// @brief Apply SO_RCVTIMEO or SO_SNDTIMEO to a POSIX socket.
/// @param sock Socket descriptor to configure.
/// @param timeout_ms Timeout in milliseconds; negative values are treated as zero.
/// @param is_recv true selects SO_RCVTIMEO, false selects SO_SNDTIMEO.
/// @return True when setsockopt succeeds; otherwise false.
bool set_socket_timeout(socket_t sock, int timeout_ms, bool is_recv) {
    if (timeout_ms < 0)
        timeout_ms = 0;
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return setsockopt(sock, SOL_SOCKET, is_recv ? SO_RCVTIMEO : SO_SNDTIMEO, &tv, sizeof(tv)) == 0;
}

/// @brief Derive the remaining time for an interrupted POSIX readiness wait.
/// @details Uses the adapter's monotonic clock and the original start time so
///          repeated signals cannot restart the full timeout. A clock failure
///          returns -1, allowing the caller to preserve EINTR instead of
///          accidentally turning a finite wait into an unbounded one.
/// @param start_ms Monotonic timestamp captured before the first poll call.
/// @param timeout_ms Original positive timeout in milliseconds.
/// @return Remaining milliseconds, zero after expiry, or -1 when elapsed time
///         cannot be measured safely.
static int rt_socket_remaining_wait_ms(uint64_t start_ms, int timeout_ms) {
    uint64_t now_ms = rt_socket_monotonic_ms();
    if (start_ms == 0 || now_ms == 0)
        return -1;
    if (now_ms < start_ms)
        return 0;
    uint64_t elapsed_ms = now_ms - start_ms;
    if (elapsed_ms >= (uint64_t)timeout_ms)
        return 0;
    return timeout_ms - (int)elapsed_ms;
}

/// @brief Convert error-only poll readiness into a deterministic POSIX error.
/// @details Read-side hangup is reported as readiness so the caller can perform
///          recv() and observe orderly EOF. POLLERR is resolved through
///          SO_ERROR before any later cleanup can overwrite errno. Write-side
///          hangup becomes ECONNRESET, while an otherwise unexplained event is
///          reported as EIO.
/// @param sock Socket descriptor whose poll event was returned.
/// @param revents Native poll result flags.
/// @param for_write True for write readiness, false for read readiness.
/// @return One for readable EOF, otherwise negative with errno initialized.
static int rt_socket_classify_poll_event(socket_t sock, short revents, bool for_write) {
    if (revents & POLLNVAL) {
        errno = EBADF;
        return -1;
    }
    if (!for_write && (revents & POLLHUP))
        return 1;
    if (revents & POLLERR) {
        int socket_error = 0;
        socklen_t error_len = (socklen_t)sizeof(socket_error);
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &socket_error, &error_len) == 0 &&
            socket_error != 0) {
            errno = socket_error;
        } else {
            errno = EIO;
        }
        return -1;
    }
    if (revents & POLLHUP) {
        errno = ECONNRESET;
        return -1;
    }
    errno = EIO;
    return -1;
}

/// @brief Wait for a POSIX socket to become readable or writable.
/// @details Uses poll() with one monotonic overall deadline. EINTR recomputes
///          the remaining duration rather than restarting the full timeout.
///          Error-only events initialize errno from SO_ERROR, and an orderly
///          read-side hangup is returned as readiness so recv() can observe EOF.
/// @param sock Socket descriptor to wait on.
/// @param timeout_ms Timeout in milliseconds.
/// @param for_write true waits for POLLOUT/write readiness; false waits for read readiness.
/// @return Positive when ready, zero on timeout, negative on error.
int wait_socket(socket_t sock, int timeout_ms, bool for_write) {
    if (timeout_ms < 0) {
        errno = EINVAL;
        return -1;
    }
    if (sock == INVALID_SOCK) {
        errno = EBADF;
        return -1;
    }

    const short requested_event = for_write ? POLLOUT : POLLIN;
    const uint64_t start_ms = timeout_ms > 0 ? rt_socket_monotonic_ms() : 0;
    int remaining_ms = timeout_ms;
    for (;;) {
        struct pollfd pfd;
        pfd.fd = sock;
        pfd.events = requested_event;
        pfd.revents = 0;

        int result = poll(&pfd, 1, remaining_ms);
        if (result == 0)
            return 0;
        if (result > 0) {
            if (pfd.revents & requested_event)
                return 1;
            return rt_socket_classify_poll_event(sock, pfd.revents, for_write);
        }

        int poll_error = errno;
        if (poll_error != EINTR)
            return -1;
        if (timeout_ms == 0)
            return 0;
        remaining_ms = rt_socket_remaining_wait_ms(start_ms, timeout_ms);
        if (remaining_ms == 0)
            return 0;
        if (remaining_ms < 0) {
            errno = poll_error;
            return -1;
        }
    }
}
