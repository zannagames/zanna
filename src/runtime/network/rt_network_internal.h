//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_network_internal.h
// Purpose: Shared platform-specific definitions for the networking runtime.
//   Provides socket type abstractions, SIGPIPE suppression, error classification,
//   and socket helper declarations used by TCP, UDP, and DNS modules.
//
// Key invariants:
//   - _DARWIN_C_SOURCE / _GNU_SOURCE must be defined BEFORE including this header.
//     Each .c file defines them at the top of the file before any includes.
//   - socket_t is SOCKET on Windows, int on Unix.
//   - All platform-specific socket includes are handled here.
//   - WSA initialization (Windows) must be called before any socket operation.
//
// Ownership/Lifetime:
//   - Socket handles are owned by the creating module (TCP, UDP).
//   - This header does not allocate or free any resources.
//
// Links: src/runtime/network/rt_network.c (TCP client + server),
//        src/runtime/network/rt_network_udp.c (UDP sockets),
//        src/runtime/network/rt_network_dns.c (DNS resolution),
//        src/runtime/network/rt_network.h (public API)
//
//===----------------------------------------------------------------------===//
#pragma once

#include "rt_network.h"

#include "rt_bytes.h"
#include "rt_error.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_seq.h"
#include "rt_socket_platform.h"
#include "rt_string.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//=============================================================================
// Typed Network Trap
//=============================================================================

/// @brief Raise a network trap with a typed public error code.
/// @param msg NUL-terminated diagnostic text.
/// @param err_code Public @c Err_* classification.
extern void rt_trap_net(const char *msg, int err_code);

/// @brief Map a captured native socket error to an `Err_*` network code.
/// @details Callers that must release managed buffers before trapping capture
///          the native error first, because allocator/registry cleanup may
///          overwrite `errno` or WinSock's thread-local last-error state.
/// @param e Native errno/WSA error captured immediately after socket failure.
/// @return Closest public network error category.
static inline int net_classify_error(int e) {
#if RT_PLATFORM_WINDOWS
    switch (e) {
        case WSAECONNREFUSED:
            return Err_ConnectionRefused;
        case WSAECONNRESET:
        case WSAECONNABORTED:
            return Err_ConnectionReset;
        case WSAETIMEDOUT:
            return Err_Timeout;
        case WSAENETUNREACH:
        case WSAEHOSTUNREACH:
            return Err_NetworkError;
        case WSAESHUTDOWN:
        case WSAENOTCONN:
            return Err_ConnectionClosed;
        default:
            return Err_NetworkError;
    }
#else
    switch (e) {
        case ECONNREFUSED:
            return Err_ConnectionRefused;
        case ECONNRESET:
        case EPIPE:
            return Err_ConnectionReset;
        case ETIMEDOUT:
            return Err_Timeout;
        case ENETUNREACH:
        case EHOSTUNREACH:
            return Err_NetworkError;
        case ENOTCONN:
            return Err_ConnectionClosed;
        default:
            return Err_NetworkError;
    }
#endif
}

/// @brief Classify the calling thread's current native socket error.
/// @details Convenience wrapper for paths that perform no intervening host or
///          managed cleanup after the failed socket operation.
/// @return Closest public network error category.
static inline int net_classify_errno(void) {
    return net_classify_error(GET_LAST_ERROR());
}

//=============================================================================
// Internal Bytes Access
//=============================================================================

/// @brief Borrow mutable payload storage from a managed Bytes object.
/// @details Calls through a local function pointer to preserve the internal ABI
///          boundary used across networking translation units.
/// @param obj Managed Bytes receiver.
/// @return Borrowed byte pointer, or NULL when unavailable.
static inline uint8_t *bytes_data(void *obj) {
    uint8_t *(*bytes_data_fn)(void *) = rt_bytes_data;
    uint8_t *data = bytes_data_fn(obj);
    return data;
}

/// @brief Read the logical length of a managed Bytes object.
/// @param obj Managed Bytes receiver.
/// @return Logical byte count reported by the runtime Bytes API.
static inline int64_t bytes_len(void *obj) {
    int64_t (*bytes_len_fn)(void *) = rt_bytes_len;
    int64_t len = bytes_len_fn(obj);
    return len;
}

//=============================================================================
// Runtime String Helpers
//=============================================================================

/// @brief Borrow an rt_string as a C string for OS networking APIs.
/// @details Runtime strings may contain embedded NUL bytes, while getaddrinfo,
/// inet_pton, and similar APIs stop at the first NUL. Return 0 for NULL,
/// invalid, oversized, or embedded-NUL strings so public wrappers can reject
/// them before they reach the OS.
/// @param value Candidate managed String.
/// @param out Receives a borrowed NUL-terminated pointer on success.
/// @param len_out Receives the logical byte length on success.
/// @return One for a valid NUL-free String and complete outputs, otherwise zero.
int rt_net_cstr_no_embedded_nul(rt_string value, const char **out, size_t *len_out);

//=============================================================================
// Socket Helpers (defined in rt_network.c, used by UDP too)
//=============================================================================

/// @brief Convert a public int64 millisecond timeout to the socket helper range.
/// @param timeout_ms Candidate nonnegative timeout.
/// @param out_timeout_ms Receives the narrowed timeout on success.
/// @return 1 on success, 0 if negative or larger than INT_MAX.
int rt_net_timeout_ms_to_int(int64_t timeout_ms, int *out_timeout_ms);

/// @brief Convert a nonnegative byte count to an int-sized socket API length.
/// @param byte_count Candidate count.
/// @param out_len Receives the narrowed count on success.
/// @return 1 on success, 0 if the count is larger than INT_MAX.
int rt_net_i64_len_to_int(int64_t byte_count, int *out_len);

/// @brief Wait for socket readiness against one interruption-safe deadline.
/// @details Delegates to the platform adapter; read-side orderly closure is
///          considered readable so the next recv can return EOF.
/// @param sock Native socket descriptor or handle.
/// @param timeout_ms Non-negative overall timeout in milliseconds.
/// @param for_write True for write readiness, false for read readiness.
/// @return 1 if ready, 0 if timeout, -1 with a native error on failure.
int wait_socket(socket_t sock, int timeout_ms, bool for_write);

/// @brief Access the raw socket owned by a Tcp object.
/// @param obj Candidate Tcp handle.
/// @return Socket descriptor, or INVALID_SOCK for NULL/invalid objects.
socket_t rt_tcp_socket_fd(void *obj);

/// @brief Validate a managed TCP handle without raising a trap.
/// @details Checks heap kind, stable class identity, and the complete private
///          payload size. Internal containers use this predicate before
///          retaining or storing opaque handles supplied through public APIs.
/// @param obj Candidate managed object.
/// @return Nonzero only for a live, fully sized TCP connection object.
int rt_tcp_is_handle(void *obj);

/// @brief Borrow the immutable endpoint stored in a TCP handle.
/// @details This helper is allocation-free and never traps. The host view
///          remains owned by @p obj and is valid only while the caller retains
///          that object. It is intended for connection-pool key comparison.
/// @param obj Candidate TCP handle.
/// @param host_out Receives the borrowed NUL-terminated host bytes.
/// @param host_len_out Receives the exact host byte length.
/// @param port_out Receives the remote port.
/// @return Nonzero on success; zero for invalid handles or incomplete output
///         pointers.
int rt_tcp_endpoint_view(void *obj, const char **host_out, size_t *host_len_out, int *port_out);

/// @brief Claim a TCP connection for one connection-pool identity.
/// @details Atomically changes an unleased token from zero to @p owner_token.
///          Repeating the operation with the same token is idempotent; a
///          different nonzero owner is never overwritten.
/// @param obj Valid TCP handle.
/// @param owner_token Nonzero process-local pool identity.
/// @return Nonzero when the caller owns (or already owned) the lease.
int rt_tcp_pool_try_claim(void *obj, uint64_t owner_token);

/// @brief Release a TCP connection-pool lease owned by @p owner_token.
/// @details Uses compare-and-exchange so a stale pool cannot clear a newer
///          owner's lease. Invalid handles, zero tokens, and owner mismatches
///          are harmless failures.
/// @param obj Candidate TCP handle.
/// @param owner_token Expected current pool identity.
/// @return Nonzero when the token was cleared; zero otherwise.
int rt_tcp_pool_release_claim(void *obj, uint64_t owner_token);

/// @brief Snapshot the current connection-pool lease token.
/// @param obj Candidate TCP handle.
/// @return Zero for an unleased/invalid TCP; otherwise its owning pool token.
uint64_t rt_tcp_pool_owner(void *obj);

/// @brief Detach the raw socket from a Tcp object without closing it.
/// @details Marks the Tcp object closed and returns ownership of the socket to the caller.
/// @param obj Tcp handle whose descriptor ownership was obtained by the caller.
void rt_tcp_detach_socket(void *obj);
