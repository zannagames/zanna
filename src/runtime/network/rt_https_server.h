//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_https_server.h
// Purpose: TLS-backed HTTP/1.1 and HTTP/2 server using the in-tree TLS 1.3 runtime.
// Key invariants:
//   - Public receivers carry a stable class identity and complete native state.
//   - Lifecycle/configuration mutations are serialized; Start/Stop are idempotent.
//   - HTTP and HTTPS callbacks share managed ServerReq/ServerRes identities.
// Ownership/Lifetime:
//   - The managed server owns copied credential paths, TLS context, router, and
//     the lazily created worker pool until finalization.
// Links: rt_http_server.h (shared callback API), rt_tls_server_internal.h
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares the managed TLS-backed HTTP/1.1 and HTTP/2 server.
 * @details Defines credential-bound construction, synchronized route and
 * handler registration, restartable TLS listener lifecycle, and shared
 * ServerReq/ServerRes handler contracts.
 */

#pragma once

#include "rt_http_server.h"
#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Stable class identity for public HttpsServer handles.
/// @details Public HTTPS entry points validate this ID and the full native
///          payload before accessing TLS, listener, or synchronization state.
#define RT_HTTPS_SERVER_CLASS_ID INT64_C(-0x720210)

/// @brief Create a stopped TLS-backed HTTP/1.1 and HTTP/2 server.
/// @details Certificate and private-key paths are validated as managed Strings
///          and copied. TLS context construction is part of the constructor
///          transaction; any failure releases all partial native state.
/// @param port TCP port in 0..65535; zero requests an ephemeral port.
/// @param cert_file Path to the PEM certificate chain.
/// @param key_file Path to the PEM private key.
/// @return Caller-owned HttpsServer handle, or NULL after trapping.
void *rt_https_server_new(int64_t port, rt_string cert_file, rt_string key_file);

/// @brief Register a GET route while the HTTPS server is configurable.
/// @details Registration is transactional and serialized against lifecycle
///          transitions and synchronous dispatch. Empty/embedded-NUL tags trap.
/// @param server Valid stopped HttpsServer handle.
/// @param pattern Router pattern such as `/users/:id`.
/// @param handler_tag Non-empty tag later associated with a handler.
void rt_https_server_get(void *server, rt_string pattern, rt_string handler_tag);

/// @brief Register a POST route while the HTTPS server is configurable.
/// @param server Valid stopped HttpsServer handle.
/// @param pattern Router pattern.
/// @param handler_tag Non-empty handler tag.
void rt_https_server_post(void *server, rt_string pattern, rt_string handler_tag);

/// @brief Register a PUT route while the HTTPS server is configurable.
/// @param server Valid stopped HttpsServer handle.
/// @param pattern Router pattern.
/// @param handler_tag Non-empty handler tag.
void rt_https_server_put(void *server, rt_string pattern, rt_string handler_tag);

/// @brief Register a DELETE route while the HTTPS server is configurable.
/// @param server Valid stopped HttpsServer handle.
/// @param pattern Router pattern.
/// @param handler_tag Non-empty handler tag.
void rt_https_server_del(void *server, rt_string pattern, rt_string handler_tag);

/// @brief Bind a native handler to a route tag while stopped.
/// @details Replacement publishes the new binding under the lifecycle lock;
///          cleanup for a different prior context runs only after unlocking.
/// @param server Valid stopped HttpsServer handle.
/// @param handler_tag Existing or future route tag.
/// @param entry Function compatible with @ref rt_http_server_handler_fn.
void rt_https_server_bind_handler(void *server, rt_string handler_tag, void *entry);

/// @brief Atomically bind a dispatcher, context, and optional cleanup callback.
/// @param server Valid stopped HttpsServer handle.
/// @param handler_tag Existing or future route tag.
/// @param dispatch Function compatible with @ref rt_http_server_handler_dispatch_fn.
/// @param ctx Opaque context passed to the dispatcher.
/// @param cleanup Optional @ref rt_http_server_handler_cleanup_fn.
void rt_https_server_bind_handler_dispatch(
    void *server, rt_string handler_tag, void *dispatch, void *ctx, void *cleanup);

/// @brief Start the serialized TLS listener and background accept loop.
/// @details Creates the worker pool lazily on first use. Concurrent/repeated
///          calls publish one listener; failure rolls back partial resources.
/// @param server Valid HttpsServer handle.
void rt_https_server_start(void *server);

/// @brief Stop accepting, interrupt TLS sessions, and drain workers.
/// @details Safe to call repeatedly or concurrently. A server worker invoking
///          Stop avoids waiting for itself while still retiring the listener.
/// @param server Valid HttpsServer handle; NULL is a compatibility no-op.
void rt_https_server_stop(void *server);

/// @brief Return a synchronized snapshot of the configured or bound port.
/// @param server Valid HttpsServer handle.
/// @return Port in 0..65535, including an assigned ephemeral port after Start.
int64_t rt_https_server_port(void *server);

/// @brief Return one while the HTTPS accept loop is published as running.
/// @param server Valid HttpsServer handle.
/// @return One while running; zero when stopped or for NULL.
int8_t rt_https_server_is_running(void *server);

#ifdef __cplusplus
}
#endif
