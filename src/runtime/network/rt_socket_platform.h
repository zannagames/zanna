//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_socket_platform.h
// Purpose: Shared socket platform adapter for the runtime networking stack.
//          Centralizes native socket type selection, socket constants, WinSock
//          startup, SIGPIPE suppression, non-blocking mode, timeouts, and
//          readiness waits.
//
// Key invariants:
//   - socket_t is the only native socket handle type used by network modules.
//   - rt_net_init_wsa() is idempotent and a no-op outside Windows.
//   - Finite readiness waits use one monotonic deadline across interruption.
//   - POSIX send calls use SEND_FLAGS when MSG_NOSIGNAL is exposed; macOS also
//     uses suppress_sigpipe() at socket creation for older SDK/socket paths.
//
// Ownership/Lifetime:
//   - The adapter never takes ownership of sockets except CLOSE_SOCKET.
//   - Callers remain responsible for closing any socket they create.
//
// Links: src/runtime/network/rt_network_internal.h,
//        src/runtime/network/rt_socket_platform_win.c,
//        src/runtime/network/rt_socket_platform_posix.c
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_socket_platform.h
 * @brief Defines the runtime's cross-platform native socket adapter.
 * @details This boundary selects the native socket type and constants and
 *          declares platform-neutral startup, close, shutdown, error
 *          classification, nonblocking, timeout, SIGPIPE, and readiness
 *          operations. Network modules retain ownership of handles passed
 *          through the adapter.
 */

#pragma once

#include "rt_platform.h"

#include <stdbool.h>
#include <stdint.h>

#if RT_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")

typedef SOCKET socket_t;
#define INVALID_SOCK INVALID_SOCKET
#define SOCK_ERROR SOCKET_ERROR
#define EINPROGRESS_VAL WSAEWOULDBLOCK
#define CONN_REFUSED WSAECONNREFUSED
#define ADDR_IN_USE WSAEADDRINUSE
#define PERM_DENIED WSAEACCES
#define SEND_FLAGS 0
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

typedef int socket_t;
#define INVALID_SOCK (-1)
#define SOCK_ERROR (-1)
#define EINPROGRESS_VAL EINPROGRESS
#define CONN_REFUSED ECONNREFUSED
#define ADDR_IN_USE EADDRINUSE
#define PERM_DENIED EACCES
#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif
#endif

#define CLOSE_SOCKET(s) rt_socket_close(s)
#define GET_LAST_ERROR() rt_socket_last_error()
#define WOULD_BLOCK rt_socket_error_is_would_block(rt_socket_last_error())

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Initialize Windows Sockets once for the current process.
/// @details On Windows this calls WSAStartup(2.2) using an idempotent,
///          thread-safe state machine and holds that startup reference until
///          process teardown so CRT-less native executables never depend on
///          an uninitialized CRT exit table. On non-Windows platforms the
///          function is a no-op.
void rt_net_init_wsa(void);

/// @brief Close a native socket handle.
/// @details Dispatches to closesocket() on Windows and close() on POSIX-style
///          platforms. The socket is no longer valid after this call succeeds.
/// @param sock Socket handle to close.
/// @return Native close result.
int rt_socket_close(socket_t sock);

/// @brief Disable both directions of an established socket without releasing its descriptor.
/// @details Wraps `shutdown(..., SHUT_RDWR)` on POSIX-style platforms and
///          `shutdown(..., SD_BOTH)` on Windows. This requests interruption of
///          transport I/O while preserving descriptor ownership for the
///          worker's eventual close path. Callers that require bounded
///          cancellation also use finite receive waits because a provider need
///          not cancel an already-blocked synchronous call. Calling it with
///          @ref INVALID_SOCK fails without invoking a native syscall.
/// @param sock Connected socket whose reads and writes should be interrupted.
/// @return Native shutdown result: zero on success, negative/socket-error on failure.
int rt_socket_shutdown_both(socket_t sock);

/// @brief Return the last native socket error for the calling thread.
/// @details Wraps WSAGetLastError() on Windows and errno elsewhere.
/// @return Native socket error code.
int rt_socket_last_error(void);

/// @brief Test whether @p err represents an interrupted socket operation.
/// @param err Native error code to classify.
/// @return Non-zero if retrying after interruption is appropriate.
bool rt_socket_error_is_interrupted(int err);

/// @brief Test whether @p err represents would-block / try-again.
/// @param err Native error code to classify.
/// @return Non-zero if readiness waiting and retrying is appropriate.
bool rt_socket_error_is_would_block(int err);

/// @brief Test whether @p err represents an in-progress non-blocking connect.
/// @param err Native error code to classify.
/// @return Non-zero if the caller should wait for write readiness.
bool rt_socket_error_is_in_progress(int err);

/// @brief Test whether @p err represents a socket timeout.
/// @param err Native error code to classify.
/// @return Non-zero if the operation failed because its timeout expired.
bool rt_socket_error_is_timeout(int err);

/// @brief Test whether @p err reports an oversized atomic datagram/message.
/// @param err Native socket error code to classify.
/// @return Non-zero for EMSGSIZE/WSAEMSGSIZE, otherwise zero.
bool rt_socket_error_is_message_too_large(int err);

/// @brief Test whether the last receive operation timed out or would block.
/// @details Used by public receive helpers that map socket timeouts to a
///          zero-byte result instead of a typed trap.
/// @return Non-zero when the last socket error was timeout/would-block.
bool rt_socket_recv_timed_out(void);

/// @brief Read a non-decreasing native elapsed-time clock in milliseconds.
/// @details This adapter never allocates or raises a runtime trap. It is used
///          by socket loops that must maintain cleanup invariants while
///          calculating an overall deadline. A zero result means the native
///          monotonic clock was unavailable; callers must retain a bounded
///          fallback policy rather than treating zero as an elapsed deadline.
/// @return Native monotonic milliseconds, or zero when unavailable.
uint64_t rt_socket_monotonic_ms(void);

/// @brief Test whether accept failed because another thread closed the listener.
/// @details Server shutdown paths close listening sockets from another thread;
///          this classifier lets accept loops exit quietly for that expected
///          race and trap only for real errors.
/// @param err Native accept error code.
/// @return Non-zero if @p err is expected during listener shutdown.
bool rt_socket_accept_interrupted_by_close(int err);

/// @brief Suppress SIGPIPE for a newly-created socket when the platform needs it.
/// @details macOS uses SO_NOSIGPIPE on the socket; POSIX platforms that expose
///          MSG_NOSIGNAL also use SEND_FLAGS per send call. Windows has no SIGPIPE.
/// @param sock Socket handle to configure.
void suppress_sigpipe(socket_t sock);

/// @brief Toggle a socket between blocking and non-blocking I/O.
/// @param sock Socket handle to update.
/// @param nonblocking True for non-blocking mode, false for blocking mode.
/// @return true on success, false if the native syscall failed.
bool rt_socket_set_nonblocking(socket_t sock, bool nonblocking);

/// @brief Query how many bytes are currently queued for reading.
/// @details Wraps FIONREAD/ioctlsocket where available and reports failure as
///          false so callers can preserve their previous "unknown means zero"
///          behavior.
/// @param sock Socket handle to inspect.
/// @param bytes_out Receives the queued byte count when the query succeeds.
/// @return true on success, false when the platform cannot answer or the call fails.
bool rt_socket_available_bytes(socket_t sock, int64_t *bytes_out);

/// @brief Query the pending asynchronous error for a socket.
/// @details Wraps `getsockopt(SOL_SOCKET, SO_ERROR)` with the platform's native
///          option-length type. A successful query can report zero (no pending
///          error) or a native error code; query failure is distinguished by
///          the Boolean return value.
/// @param sock Socket handle to inspect.
/// @param error_out Receives the pending native error code on success.
/// @return true when the socket option was read, false for invalid arguments or
///         a native query failure.
bool rt_socket_pending_error(socket_t sock, int *error_out);

/// @brief Set socket timeout for send or receive operations.
/// @details Negative timeout values are normalized to zero. Windows expects a
///          millisecond DWORD; POSIX-style platforms receive struct timeval.
/// @param sock Socket handle to configure.
/// @param timeout_ms Timeout in milliseconds.
/// @param is_recv True for SO_RCVTIMEO, false for SO_SNDTIMEO.
/// @return true when the native socket option was applied, false on failure.
bool set_socket_timeout(socket_t sock, int timeout_ms, bool is_recv);

/// @brief Wait for socket readability or writability.
/// @details Uses poll() on POSIX platforms where available and select()
///          otherwise. Returns immediately for timeout_ms == 0. Interrupted
///          waits preserve one monotonic overall deadline; they never restart
///          the full duration. POSIX error-only readiness initializes errno
///          from SO_ERROR, while read-side hangup is reported as readiness so
///          the caller can observe orderly EOF with recv().
/// @param sock Socket handle to wait on.
/// @param timeout_ms Timeout in milliseconds.
/// @param for_write True waits for write readiness, false for read readiness.
/// @return Positive when ready, 0 on timeout, negative on native error.
int wait_socket(socket_t sock, int timeout_ms, bool for_write);

#ifdef __cplusplus
}
#endif
