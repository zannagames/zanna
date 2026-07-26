//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_tls_server_internal.h
// Purpose: Internal TLS 1.3 server context + accept helpers used by
//          HttpsServer and WssServer.
// Key invariants:
//   - Parsed credentials and ALPN policy are immutable after context creation.
//   - Native descriptors remain pointer-width across the server handshake.
//   - A valid accept call consumes its socket on success and handshake failure.
// Ownership/Lifetime:
//   - Callers free contexts with rt_tls_server_ctx_free after all handshakes end.
//   - Successful accepts return one managed session reference consumed by
//     rt_tls_close; invalid arguments do not transfer descriptor ownership.
// Links: src/runtime/network/rt_tls.c, src/runtime/network/rt_tls.h,
//        src/runtime/network/rt_https_server.c,
//        src/runtime/network/rt_wss_server.c
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_tls_server_internal.h
 * @brief Declares private TLS server contexts and socket acceptance.
 * @details A server context owns immutable parsed credentials, ALPN policy,
 *          timeout configuration, and an optional cancellation probe. Accept
 *          consumes a valid connected socket once handshake processing begins
 *          and returns a session that owns the socket on success.
 */

#pragma once

#include "rt_tls.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Opaque server-side TLS context (cert, key, ALPN policy).
typedef struct rt_tls_server_ctx rt_tls_server_ctx_t;

/// @brief Optional cooperative-cancellation probe for server handshakes.
/// @param context Opaque owner state supplied in @ref rt_tls_server_config_t.
/// @return Non-zero when the in-progress handshake should stop.
typedef int (*rt_tls_server_cancel_fn)(void *context);

/// @brief Server-side TLS configuration consumed by @ref rt_tls_server_ctx_new.
/// @details Paths are read once at context creation; the parsed cert chain
///          and private key are held inside the context for every
///          subsequent @ref rt_tls_server_accept_socket call.
typedef struct rt_tls_server_config {
    const char *cert_file;                    ///< PEM-encoded server cert chain (leaf first).
    const char *key_file;                     ///< PEM-encoded private key matching the leaf.
    const char *alpn_protocol;                ///< Optional comma-separated ALPN advertise list.
    int timeout_ms;                           ///< Per-handshake timeout; 0 = default 30 s.
    rt_tls_server_cancel_fn cancel_requested; ///< Optional bounded-wait cancellation probe.
    void *cancel_context;                     ///< Opaque argument for @ref cancel_requested.
} rt_tls_server_config_t;

/// @brief Initialize server TLS configuration with safe defaults.
/// @param config Configuration storage to zero and initialize; NULL is a no-op.
void rt_tls_server_config_init(rt_tls_server_config_t *config);

/// @brief Parse the cert + key files and build a shared server context.
/// @param config Certificate, private-key, ALPN, timeout, and cancellation
///        settings.
/// @return New context, or NULL on parse/IO failure (cause via @ref rt_tls_server_last_error).
rt_tls_server_ctx_t *rt_tls_server_ctx_new(const rt_tls_server_config_t *config);

/// @brief Securely release a server context and its credential material.
/// @param ctx Context to consume; NULL is a no-op.
void rt_tls_server_ctx_free(rt_tls_server_ctx_t *ctx);

/// @brief Accept a client TLS handshake on an already-connected @p socket_fd.
/// @details Allocates a fresh @ref rt_tls_session_t, drives the server
///          handshake to completion using the cert/key + ALPN policy
///          in @p ctx, and returns the live session on success. Returns
///          NULL on any handshake failure with the cause captured via
///          @ref rt_tls_server_last_error. The pointer-width descriptor avoids
///          narrowing WinSock `SOCKET` handles. When both arguments are valid,
///          this function consumes and closes @p socket_fd on failure; on
///          success the returned session owns it. When the context supplies a
///          cancellation probe, socket I/O is polled in bounded slices so a
///          synchronous receive can stop portably. An invalid context or socket
///          returns before ownership transfer.
/// @param socket_fd Connected native socket represented without narrowing.
/// @param ctx Immutable parsed credential/ALPN context that outlives the session.
/// @return Caller-owned connected session, or NULL after consuming the socket
///         for a setup/handshake failure.
rt_tls_session_t *rt_tls_server_accept_socket(intptr_t socket_fd, const rt_tls_server_ctx_t *ctx);

/// @brief Return the calling thread's latest server-side TLS diagnostic.
/// @return Thread-local native text valid until the next server setup or
///         handshake operation on this thread.
const char *rt_tls_server_last_error(void);

#ifdef __cplusplus
}
#endif
