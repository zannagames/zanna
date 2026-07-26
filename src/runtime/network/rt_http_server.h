//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_http_server.h
// Purpose: Threaded HTTP/1.1 server with routing and request/response objects.
// Key invariants:
//   - Each incoming connection handled in a thread pool worker.
//   - Routes are matched via HttpRouter; unmatched routes return 404.
//   - HTTP/1.0 and HTTP/1.1 request framing / keep-alive semantics are enforced.
// Ownership/Lifetime:
//   - Server objects are GC-managed via rt_obj_set_finalizer.
//   - ServerReq/ServerRes are created per-request and GC-managed.
// Links: rt_http_router.h (routing), rt_network.h (TCP)
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares threaded HTTP/1.1 server and handler-object services.
 * @details Defines stable managed identities, route and dispatch binding,
 * restartable listener lifecycle, synchronous parser/serializer access,
 * immutable request queries, and transactional response construction.
 */

#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//=========================================================================
// HttpServer
//=========================================================================

/// @brief Stable class identity for public HttpServer handles.
/// @details Every public server entry point validates this tag and the complete
///          native payload size before reading mutex or listener state. The ID
///          is part of the runtime C ABI and must not be reused.
#define RT_HTTP_SERVER_CLASS_ID INT64_C(-0x72020F)

/// @brief Stable class identity shared by HTTP and HTTPS ServerReq handles.
/// @details Request handles own immutable copies of the parsed method, target,
///          headers, body, and route parameters. A handler may retain a request
///          beyond dispatch; the server releases only its producer reference.
#define RT_SERVER_REQ_CLASS_ID INT64_C(-0x720211)

/// @brief Stable class identity shared by HTTP and HTTPS ServerRes handles.
/// @details Response handles own their header Map and native body snapshot.
///          Public builder functions reject unrelated or undersized objects
///          before accessing that state.
#define RT_SERVER_RES_CLASS_ID INT64_C(-0x720212)

/// @brief Create a stopped HTTP server for @p port.
/// @details Port zero requests an ephemeral kernel-assigned port when the
///          server starts. Construction creates the router and synchronization
///          state but does not open a socket or spawn workers; the worker pool
///          is created lazily by Start and reused across later restarts.
/// @param port TCP port in the inclusive range 0..65535.
/// @return Caller-owned managed HttpServer handle, or NULL after trapping on
///         invalid input or allocation failure.
void *rt_http_server_new(int64_t port);

/// @brief Register a GET route while the server is stopped.
/// @param server Valid HttpServer handle.
/// @param pattern Router pattern such as `/users/:id`.
/// @param handler_tag Non-empty tag later associated with a handler.
void rt_http_server_get(void *server, rt_string pattern, rt_string handler_tag);

/// @brief Register a POST route while the server is stopped.
/// @param server Valid HttpServer handle.
/// @param pattern Router pattern.
/// @param handler_tag Non-empty tag later associated with a handler.
void rt_http_server_post(void *server, rt_string pattern, rt_string handler_tag);

/// @brief Register a PUT route while the server is stopped.
/// @param server Valid HttpServer handle.
/// @param pattern Router pattern.
/// @param handler_tag Non-empty tag later associated with a handler.
void rt_http_server_put(void *server, rt_string pattern, rt_string handler_tag);

/// @brief Register a DELETE route while the server is stopped.
/// @param server Valid HttpServer handle.
/// @param pattern Router pattern.
/// @param handler_tag Non-empty tag later associated with a handler.
void rt_http_server_del(void *server, rt_string pattern, rt_string handler_tag);

/// @brief Native route-handler signature.
/// @details Both handles are managed and remain valid for the duration of the
///          call. Code that stores either handle must retain it independently.
/// @param req ServerReq handle for the immutable request snapshot.
/// @param res ServerRes handle to populate with status, headers, and body.
typedef void (*rt_http_server_handler_fn)(void *req, void *res);

/// @brief Dispatch signature used for VM/bytecode-backed handlers.
/// @param ctx Binding-specific context supplied at registration.
/// @param req Managed ServerReq handle.
/// @param res Managed ServerRes handle.
typedef void (*rt_http_server_handler_dispatch_fn)(void *ctx, void *req, void *res);

/// @brief Optional cleanup callback for a detached dispatch context.
/// @details Cleanup is invoked after the server lifecycle mutex is released,
///          so a cleanup callback may safely call back into server APIs.
/// @param ctx Context previously supplied to the binding.
typedef void (*rt_http_server_handler_cleanup_fn)(void *ctx);

/// @brief Bind a handler tag to a native function pointer.
/// @param server Valid, stopped HttpServer handle.
/// @param handler_tag Existing or future route tag.
/// @param entry Function pointer compatible with @ref rt_http_server_handler_fn.
void rt_http_server_bind_handler(void *server, rt_string handler_tag, void *entry);

/// @brief Bind a handler tag to a dispatcher with persistent context.
/// @details Replacing a binding publishes the new tuple atomically. When the
///          old context differs, its cleanup callback runs after publication.
/// @param server Valid, stopped HttpServer handle.
/// @param handler_tag Existing or future route tag.
/// @param dispatch Function compatible with @ref rt_http_server_handler_dispatch_fn.
/// @param ctx Opaque context passed to @p dispatch.
/// @param cleanup Optional @ref rt_http_server_handler_cleanup_fn.
void rt_http_server_bind_handler_dispatch(
    void *server, rt_string handler_tag, void *dispatch, void *ctx, void *cleanup);

/// @brief Start accepting connections on a background accept thread.
/// @details The operation is idempotent and serialized against route changes,
///          binding changes, and Stop. A failure rolls back any partially
///          opened listener or newly created worker pool.
/// @param server Valid HttpServer handle.
void rt_http_server_start(void *server);

/// @brief Stop accepting, interrupt active connections, and wait for workers to exit.
/// @details Safe to call repeatedly. When invoked by one of this server's own
///          worker callbacks, it avoids waiting on that worker to prevent a
///          self-deadlock while still stopping the listener.
/// @param server Valid HttpServer handle; NULL is a compatibility no-op.
void rt_http_server_stop(void *server);

/// @brief Return a synchronized snapshot of the configured or bound port.
/// @param server Valid HttpServer handle.
/// @return Port in 0..65535, including the assigned ephemeral port after Start.
int64_t rt_http_server_port(void *server);

/// @brief Check whether the accept loop is published as running.
/// @param server Valid HttpServer handle.
/// @return One while running; zero when stopped or for NULL.
int8_t rt_http_server_is_running(void *server);

/// @brief Process one complete HTTP/1 request synchronously without a socket.
/// @details Uses the same parser, router, handler, and serializer as the live
///          server. The returned String contains the complete wire response.
/// @param server Valid HttpServer handle.
/// @param raw_request Complete request bytes in a managed String.
/// @return Caller-owned response String, or an empty String after a returning trap hook.
void *rt_http_server_process_request(void *server, rt_string raw_request);

//=========================================================================
// ServerReq — Request object for handlers
//=========================================================================

/// @brief Copy the parsed HTTP method from a ServerReq.
/// @param req Valid ServerReq handle; null produces an empty String.
/// @return Caller-owned String, or an empty String for NULL.
rt_string rt_server_req_method(void *req);

/// @brief Copy the parsed path without its query component.
/// @param req Valid ServerReq handle; null produces an empty String.
/// @return Caller-owned String, or an empty String for NULL.
rt_string rt_server_req_path(void *req);

/// @brief Copy the exact request-body bytes, preserving embedded NULs.
/// @param req Valid ServerReq handle; null produces an empty String.
/// @return Caller-owned String, or an empty String when no body is present.
rt_string rt_server_req_body(void *req);

/// @brief Look up a request header case-insensitively and return a copy.
/// @param req Valid ServerReq handle.
/// @param name Managed header-name String.
/// @return Caller-owned value String, or an empty String when absent.
rt_string rt_server_req_header(void *req, rt_string name);

/// @brief Copy one named route parameter captured by HttpRouter.
/// @param req Valid ServerReq handle.
/// @param name Parameter name without the leading colon.
/// @return Caller-owned decoded value String, or an empty String when absent.
rt_string rt_server_req_param(void *req, rt_string name);

/// @brief Decode and copy the first matching query-string value.
/// @param req Valid ServerReq handle.
/// @param name Query parameter name.
/// @return Caller-owned decoded value String, or an empty String when absent.
rt_string rt_server_req_query(void *req, rt_string name);

//=========================================================================
// ServerRes — Response object for handlers
//=========================================================================

/// @brief Set an HTTP status in the inclusive range 100..599.
/// @param res Valid ServerRes handle.
/// @param code HTTP status code.
/// @return The same response handle for builder chaining.
void *rt_server_res_status(void *res, int64_t code);

/// @brief Transactionally set a non-framework response header.
/// @details Header names are compared case-insensitively. Invalid field names,
///          CR/LF injection, and framework-owned framing headers are rejected.
/// @param res Valid ServerRes handle.
/// @param name Header field name.
/// @param value Header field value.
/// @return The same response handle for builder chaining.
void *rt_server_res_header(void *res, rt_string name, rt_string value);

/// @brief Transactionally replace the response body with an owned byte copy.
/// @details Allocation failure preserves the prior body and finalized state.
/// @param res Valid ServerRes handle.
/// @param body Managed String; NULL finalizes an empty body.
void rt_server_res_send(void *res, rt_string body);

/// @brief Set JSON content type and transactionally copy a serialized JSON body.
/// @details Stages a cloned header Map and exact body copy, then publishes both
///          together. Allocation failure preserves the complete prior response.
/// @param res Valid ServerRes handle.
/// @param json_str Pre-serialized JSON bytes; syntax is not validated.
void rt_server_res_json(void *res, rt_string json_str);

#ifdef __cplusplus
}
#endif
