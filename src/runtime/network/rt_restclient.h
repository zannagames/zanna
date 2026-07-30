//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/network/rt_restclient.h
// Purpose: REST API client with session management, persistent headers, base URL support, JSON
// convenience methods, and configurable timeouts.
//
// Key invariants:
//   - Persistent headers (e.g., Authorization) are sent with every request.
//   - Base URL and request path are joined by slash normalization, not RFC reference resolution.
//   - JSON helper methods append Content-Type/Accept application/json fields.
//   - Each request uses one monotonic deadline across every transport phase.
//   - Mutable defaults, pool configuration, and last-response state are mutex protected.
//
// Ownership/Lifetime:
//   - RestClient objects and returned response/string handles are runtime managed.
//   - BaseUrl and LastResponse return independent caller-owned references.
//
// Links: src/runtime/network/rt_restclient.c (implementation),
//        docs/adr/0228-http-end-to-end-request-deadlines.md,
//        src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_restclient.h
 * @brief Declares session-style REST requests and JSON convenience operations.
 * @details The API manages a synchronized base URL, persistent request
 *          headers, authentication state, transport options, and a retained
 *          last response. Request methods return managed Result values and
 *          preserve ownership of client configuration across calls.
 */

#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Stable managed-object class identity for RestClient handles.
/// @details Every non-null public receiver is checked against this tag and the
///          complete private payload size before mutable headers, cached
///          responses, connection pools, or native synchronization state are
///          accessed.
#define RT_RESTCLIENT_CLASS_ID INT64_C(-0x72020E)

//=============================================================================
// RestClient Creation and Configuration
//=============================================================================

/// @brief Create a synchronized REST client with a copied base URL.
/// @details Construction publishes the object only after its mutex, default
///          header Map, and private keep-alive pool are initialized. NULL keeps
///          the historical empty-base behavior; invalid or embedded-NUL String
///          handles trap. Partial construction is released on every failure.
/// @param base_url Base URL for all requests (e.g., "https://api.example.com").
/// @return Caller-owned RestClient object, or NULL after a returning trap hook.
void *rt_restclient_new(rt_string base_url);

/// @brief Get an independent copy of the configured base URL.
/// @param obj RestClient object.
/// @return Caller-owned base URL String; NULL receivers return empty String.
/// @note A non-NULL wrong-class receiver traps before payload access.
rt_string rt_restclient_base_url(void *obj);

/// @brief Transactionally set a default header for all later requests.
/// @details Replacement is case-insensitive. The complete key snapshot and new
///          Map entry are allocated before differently cased aliases are
///          removed, so a caught allocation trap preserves the prior value.
///          Names must be nonempty HTTP tokens; values containing NUL, CR, or
///          LF are rejected. Mutation and request snapshots are synchronized.
/// @param obj RestClient receiver; NULL is a no-op.
/// @param name Nonempty HTTP field-name token.
/// @param value Field value without NUL or line-breaking bytes.
void rt_restclient_set_header(void *obj, rt_string name, rt_string value);

/// @brief Remove every case-insensitive spelling of a default header.
/// @details Invalid field-name tokens are ignored. Mutation is synchronized
///          with request snapshot capture.
/// @param obj RestClient receiver; NULL is a no-op.
/// @param name HTTP field-name token to remove.
void rt_restclient_del_header(void *obj, rt_string name);

/// @brief Transactionally set `Authorization: Bearer <token>`.
/// @details Native and managed staging is released on validation, allocation,
///          or Map-update failure; the previous Authorization value is
///          retained. Embedded NUL, CR, and LF bytes are rejected.
/// @param obj RestClient receiver; NULL is a no-op.
/// @param token Bearer token, with NULL treated as empty.
void rt_restclient_set_auth_bearer(void *obj, rt_string token);

/// @brief Transactionally set base64-encoded Basic authentication.
/// @details Username/password are copied as `user:password`, encoded, prefixed,
///          and published only after every intermediate allocation succeeds.
///          Embedded NUL, CR, and LF bytes are rejected.
/// @param obj RestClient receiver; NULL is a no-op.
/// @param username Username bytes, with NULL treated as empty.
/// @param password Password bytes, with NULL treated as empty.
void rt_restclient_set_auth_basic(void *obj, rt_string username, rt_string password);

/// @brief Remove any default Authorization header.
/// @param obj RestClient receiver; NULL is a no-op.
void rt_restclient_clear_auth(void *obj);

/// @brief Set the synchronized default timeout for subsequent requests.
/// @details Each request receives one monotonic budget shared by name
///          resolution, address attempts, TLS, I/O, redirects, and response
///          transformations.
/// @param obj RestClient object.
/// @param timeout_ms Milliseconds in the inclusive range 0..INT_MAX; zero
///        disables the request deadline.
void rt_restclient_set_timeout(void *obj, int64_t timeout_ms);

/// @brief Read whether the client reuses keep-alive connections.
/// @param obj RestClient receiver; NULL returns zero.
/// @return One when enabled, otherwise zero.
int8_t rt_restclient_get_keep_alive(void *obj);

/// @brief Enable or disable keep-alive connection reuse transactionally.
/// @details Enabling allocates a replacement pool before changing state and
///          rechecks the configured size before publication; disabling
///          atomically detaches and then clears the old pool.
/// @param obj RestClient receiver; NULL is a no-op.
/// @param keep_alive Nonzero to enable reuse.
void rt_restclient_set_keep_alive(void *obj, int8_t keep_alive);

/// @brief Replace the internal keep-alive pool with the requested capacity.
/// @details Allocation completes before the synchronized exchange, so failure
///          preserves both the previous pool and configured size. When reuse
///          is disabled only the future size changes and no idle pool remains.
/// @param obj RestClient receiver; NULL is a no-op.
/// @param max_size Requested capacity; non-positive values clamp to one and the
///        native HTTP pool applies its documented upper bound.
void rt_restclient_set_pool_size(void *obj, int64_t max_size);

//=============================================================================
// HTTP Methods - Raw
//=============================================================================

/// @brief Send a GET request and return its raw response.
/// @details Client defaults are copied under the mutex before transport work.
///          The response is also retained as synchronized diagnostic state.
/// @param obj RestClient receiver.
/// @param path Relative path joined to the snapshotted base URL.
/// @return Caller-owned HttpRes, or NULL after a returning trap hook.
void *rt_restclient_get(void *obj, rt_string path);

/// @brief Perform GET request and return a Result-wrapped HttpRes.
/// @details Transport/setup failures become `Result.ErrStr`; any received HTTP
///          response, including 4xx/5xx, is returned as `Result.Ok(HttpRes)`.
/// @param obj RestClient receiver.
/// @param path Relative path joined to the snapshotted base URL.
/// @return Caller-owned `Zanna.Result` containing `Ok(HttpRes)` or
///         `Err(String)`.
/// @note NULL and wrong-class receivers produce `Err(String)` without native
///       payload access. Result-construction OOM releases all partial values.
void *rt_restclient_get_result(void *obj, rt_string path);

/// @brief Send a POST request with a String body.
/// @details This raw helper does not imply a Content-Type; set one through the
///          default-header API when needed.
/// @param obj RestClient receiver.
/// @param path Relative path joined to the snapshotted base URL.
/// @param body String body copied into the request.
/// @return Caller-owned HttpRes, or NULL after a returning trap hook.
void *rt_restclient_post(void *obj, rt_string path, rt_string body);

/// @brief Perform POST request with body and return a Result-wrapped HttpRes.
/// @details Transport/setup failures become `Result.ErrStr`; HTTP status stays
///          on the response object for explicit caller handling.
/// @param obj RestClient receiver.
/// @param path Relative path joined to the snapshotted base URL.
/// @param body String body copied into the request.
/// @return Caller-owned `Zanna.Result` containing `Ok(HttpRes)` or
///         `Err(String)`.
void *rt_restclient_post_result(void *obj, rt_string path, rt_string body);

/// @brief Send a PUT request with a String body.
/// @param obj RestClient receiver.
/// @param path Relative path joined to the snapshotted base URL.
/// @param body String body copied into the request.
/// @return Caller-owned HttpRes, or NULL after a returning trap hook.
void *rt_restclient_put(void *obj, rt_string path, rt_string body);

/// @brief Perform PUT request with body and return a Result-wrapped HttpRes.
/// @details Setup and transport traps become `Err(String)`; every received HTTP
///          response remains `Ok(HttpRes)` regardless of status.
/// @param obj RestClient receiver.
/// @param path Relative path joined to the snapshotted base URL.
/// @param body String body copied into the request.
/// @return Caller-owned `Zanna.Result` containing `Ok(HttpRes)` or
///         `Err(String)`.
void *rt_restclient_put_result(void *obj, rt_string path, rt_string body);

/// @brief Send a PATCH request with a String body.
/// @param obj RestClient receiver.
/// @param path Relative path joined to the snapshotted base URL.
/// @param body String body copied into the request.
/// @return Caller-owned HttpRes, or NULL after a returning trap hook.
void *rt_restclient_patch(void *obj, rt_string path, rt_string body);

/// @brief Perform PATCH request with body and return a Result-wrapped HttpRes.
/// @details Setup and transport traps become `Err(String)`; every received HTTP
///          response remains `Ok(HttpRes)` regardless of status.
/// @param obj RestClient receiver.
/// @param path Relative path joined to the snapshotted base URL.
/// @param body String body copied into the request.
/// @return Caller-owned `Zanna.Result` containing `Ok(HttpRes)` or
///         `Err(String)`.
void *rt_restclient_patch_result(void *obj, rt_string path, rt_string body);

/// @brief Send a bodyless DELETE request.
/// @param obj RestClient receiver.
/// @param path Relative path joined to the snapshotted base URL.
/// @return Caller-owned HttpRes, or NULL after a returning trap hook.
void *rt_restclient_delete(void *obj, rt_string path);

/// @brief Perform DELETE request and return a Result-wrapped HttpRes.
/// @details Setup and transport traps become `Err(String)`; every received HTTP
///          response remains `Ok(HttpRes)` regardless of status.
/// @param obj RestClient receiver.
/// @param path Relative path joined to the snapshotted base URL.
/// @return Caller-owned `Zanna.Result` containing `Ok(HttpRes)` or
///         `Err(String)`.
void *rt_restclient_delete_result(void *obj, rt_string path);

/// @brief Send a HEAD request for response metadata without a body transfer.
/// @param obj RestClient receiver.
/// @param path Relative path joined to the snapshotted base URL.
/// @return Caller-owned HttpRes, or NULL after a returning trap hook.
void *rt_restclient_head(void *obj, rt_string path);

/// @brief Perform HEAD request and return a Result-wrapped HttpRes.
/// @details Setup and transport traps become `Err(String)`; every received HTTP
///          response remains `Ok(HttpRes)` regardless of status.
/// @param obj RestClient receiver.
/// @param path Relative path joined to the snapshotted base URL.
/// @return Caller-owned `Zanna.Result` containing `Ok(HttpRes)` or
///         `Err(String)`.
void *rt_restclient_head_result(void *obj, rt_string path);

//=============================================================================
// HTTP Methods - JSON Convenience
//=============================================================================

/// @brief Send a GET accepting JSON and parse a successful response body.
/// @details Applies `Accept: application/json`. The completed raw response
///          remains available through the last-response accessors even when
///          status is non-2xx or its body is empty.
/// @param obj RestClient receiver.
/// @param path Relative path joined to the snapshotted base URL.
/// @return Caller-owned parsed JSON value, or NULL for a non-2xx response,
///         empty body, parse failure, or after a returning trap hook.
void *rt_restclient_get_json(void *obj, rt_string path);

/// @brief Serialize JSON into a POST and parse a successful JSON response.
/// @details Applies both `Content-Type: application/json` and
///          `Accept: application/json`; all request and response temporaries
///          are released on every trap path.
/// @param obj RestClient receiver.
/// @param path Relative path joined to the snapshotted base URL.
/// @param json_body Managed JSON value serialized as the request body.
/// @return Caller-owned parsed JSON value, or NULL for a non-2xx response,
///         empty body, parse failure, or after a returning trap hook.
void *rt_restclient_post_json(void *obj, rt_string path, void *json_body);

/// @brief Serialize JSON into a PUT and parse a successful JSON response.
/// @details Applies JSON Content-Type and Accept fields. A completed raw
///          response remains cached for diagnostic inspection.
/// @param obj RestClient receiver.
/// @param path Relative path joined to the snapshotted base URL.
/// @param json_body Managed JSON value serialized as the request body.
/// @return Caller-owned parsed JSON value, or NULL for a non-2xx response,
///         empty body, parse failure, or after a returning trap hook.
void *rt_restclient_put_json(void *obj, rt_string path, void *json_body);

/// @brief Serialize JSON into a PATCH and parse a successful JSON response.
/// @details Applies JSON Content-Type and Accept fields. A completed raw
///          response remains cached for diagnostic inspection.
/// @param obj RestClient receiver.
/// @param path Relative path joined to the snapshotted base URL.
/// @param json_body Managed JSON value serialized as the request body.
/// @return Caller-owned parsed JSON value, or NULL for a non-2xx response,
///         empty body, parse failure, or after a returning trap hook.
void *rt_restclient_patch_json(void *obj, rt_string path, void *json_body);

/// @brief Send a bodyless DELETE and parse a successful JSON response.
/// @details Applies `Accept: application/json`; no Content-Type or request body
///          is added.
/// @param obj RestClient receiver.
/// @param path Relative path joined to the snapshotted base URL.
/// @return Caller-owned parsed JSON value, or NULL for a non-2xx response,
///         empty body, parse failure, or after a returning trap hook.
void *rt_restclient_delete_json(void *obj, rt_string path);

//=============================================================================
// Error Handling
//=============================================================================

/// @brief Read the synchronized status code of the last response.
/// @param obj RestClient receiver; NULL yields zero.
/// @return Last HTTP status, or zero before a response or after failure clears
///         compatibility state.
int64_t rt_restclient_last_status(void *obj);

/// @brief Get a retained snapshot of the last response object.
/// @details The cached pointer is live-retained while the client mutex is held;
///          the caller must release the returned independent reference.
/// @param obj RestClient receiver; NULL yields NULL.
/// @return Caller-owned HttpRes reference, or NULL if no response is cached.
void *rt_restclient_last_response(void *obj);

/// @brief Check whether the synchronized last status is successful.
/// @param obj RestClient receiver; NULL yields zero.
/// @return One for a status in the inclusive range 200..299; otherwise zero.
int8_t rt_restclient_last_ok(void *obj);

#ifdef __cplusplus
}
#endif
