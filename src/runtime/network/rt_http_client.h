//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_http_client.h
// Purpose: Session-based HTTP client with cookie jar and auto-redirect.
// Key invariants:
//   - Cookies persist across requests to the same domain.
//   - Redirect policy, cookies, defaults, and pool state are mutex protected.
//   - Redirect hops use copied inputs and one recoverable ownership transaction.
//   - Default-header replacement is case-insensitive and transactional.
// Ownership/Lifetime:
//   - Client objects and returned HttpRes/Map handles are runtime managed.
//   - Returned responses and cookie snapshots are independent caller-owned references.
// Links: rt_http_client.c (implementation), rt_network_http.c (transport)
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares synchronized stateful HttpClient session services.
 * @details Defines session construction, method-specific requests, persistent
 * validated headers and cookies, timeouts and redirect policy, transactional
 * keep-alive pooling, and caller-owned cookie snapshots and responses.
 */

#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Stable managed-object class identity for HttpClient session handles.
/// @details Public methods validate this tag, the complete private payload
///          size, and initialized synchronization state before accessing the
///          cookie jar, default headers, or keep-alive pool.
#define RT_HTTP_CLIENT_CLASS_ID INT64_C(-0x72020D)

/// @brief Create a synchronized HTTP client session.
/// @details The default timeout is 30 seconds, redirect following is enabled
///          with a five-hop limit, and keep-alive uses a private pool of eight
///          entries. The object is published only after its mutex, header Map,
///          and pool are initialized; every partial allocation is recovered.
/// @return Caller-owned HttpClient, or NULL after a returning trap hook.
void *rt_http_client_new(void);

/// @brief Issue a GET and return the final response after configured redirects.
/// @details The URL is validated and copied by exact runtime-string length.
///          Defaults and cookies are captured as synchronized snapshots for
///          each hop, and cross-origin redirects drop sensitive defaults.
/// @param client HttpClient receiver.
/// @param url Absolute HTTP or HTTPS URL.
/// @return Caller-owned HttpRes, or NULL after a returning trap hook.
void *rt_http_client_get(void *client, rt_string url);

/// @brief Issue a POST with a copied String body.
/// @details Status 301/302 rewrites POST to GET and status 303 always rewrites
///          to GET; status 307/308 preserves the method and body. Embedded NUL
///          bytes in @p body are preserved by exact length.
/// @param client HttpClient receiver.
/// @param url Absolute HTTP or HTTPS URL.
/// @param body Request body String.
/// @return Caller-owned final HttpRes, or NULL after a returning trap hook.
void *rt_http_client_post(void *client, rt_string url, rt_string body);

/// @brief Issue a PUT with a copied String body.
/// @param client HttpClient receiver.
/// @param url Absolute HTTP or HTTPS URL.
/// @param body Request body String; embedded NUL bytes are preserved.
/// @return Caller-owned final HttpRes, or NULL after a returning trap hook.
void *rt_http_client_put(void *client, rt_string url, rt_string body);

/// @brief Issue a DELETE request.
/// @param client HttpClient receiver.
/// @param url Absolute HTTP or HTTPS URL.
/// @return Caller-owned final HttpRes, or NULL after a returning trap hook.
void *rt_http_client_delete(void *client, rt_string url);

/// @brief Transactionally set a default header sent on every request.
/// @details Header names are validated as HTTP tokens; embedded NUL and CR/LF
///          injection are rejected. Replacement is case-insensitive, and a
///          failed snapshot or insertion preserves every prior spelling/value.
/// @param client HttpClient receiver; NULL is a no-op.
/// @param name Header field name.
/// @param value Header field value.
void rt_http_client_set_header(void *client, rt_string name, rt_string value);

/// @brief Set the synchronized timeout for all subsequent requests.
/// @param client HttpClient receiver; NULL is a no-op.
/// @param timeout_ms Milliseconds in the inclusive range 0..INT_MAX; zero
///        disables address/socket operation deadlines.
void rt_http_client_set_timeout(void *client, int64_t timeout_ms);

/// @brief Read whether the client reuses keep-alive connections.
/// @param client HttpClient receiver; NULL returns zero.
/// @return One when enabled, otherwise zero.
int8_t rt_http_client_get_keep_alive(void *client);

/// @brief Enable or disable keep-alive connection reuse transactionally.
/// @details Enabling builds a pool outside the client lock and rechecks the
///          configured size before publication. Disabling atomically detaches
///          the pool, then clears and releases it after unlocking.
/// @param client HttpClient receiver; NULL is a no-op.
/// @param keep_alive Nonzero to enable pooled reuse.
void rt_http_client_set_keep_alive(void *client, int8_t keep_alive);

/// @brief Replace the configured keep-alive pool capacity transactionally.
/// @details A replacement is allocated before the synchronized exchange. When
///          reuse is disabled only the future capacity is changed and the
///          temporary pool is discarded, so disabled clients retain no pool.
/// @param client HttpClient receiver; NULL is a no-op.
/// @param max_size Requested capacity; non-positive values clamp to one and
///        the transport applies its documented upper bound.
void rt_http_client_set_pool_size(void *client, int64_t max_size);

/// @brief Set the maximum automatic redirect hops.
/// @param client HttpClient receiver; NULL is a no-op.
/// @param max Maximum hops; negative values clamp to zero.
void rt_http_client_set_max_redirects(void *client, int64_t max);

/// @brief Read whether automatic 3xx redirect following is enabled.
/// @param client HttpClient receiver; NULL returns zero.
/// @return One when enabled, otherwise zero.
int8_t rt_http_client_get_follow_redirects(void *client);

/// @brief Toggle automatic 3xx redirect following.
/// @param client HttpClient receiver; NULL is a no-op.
/// @param follow Nonzero to follow redirects up to the configured limit.
void rt_http_client_set_follow_redirects(void *client, int8_t follow);

/// @brief Store a validated host-only cookie for exactly @p domain.
/// @details Names use HTTP token syntax and values use cookie-octet syntax.
///          Domain, name, and value reject embedded NUL bytes. All native
///          staging completes before the synchronized replacement, so failure
///          preserves the previous cookie. Manual cookies never match a
///          subdomain unless that exact subdomain is supplied separately.
/// @param client HttpClient receiver; NULL is a no-op.
/// @param domain Exact host name or address scope.
/// @param name Case-sensitive cookie name.
/// @param value Cookie value; an empty String is valid.
void rt_http_client_set_cookie(void *client, rt_string domain, rt_string name, rt_string value);

/// @brief Remove a stored cookie by exact domain and case-sensitive name.
/// @details The normalized-domain staging allocation occurs before locking;
///          a missing or invalid empty identity is a no-op.
/// @param client HttpClient receiver; NULL is a no-op.
/// @param domain Exact cookie domain.
/// @param name Case-sensitive cookie name.
void rt_http_client_delete_cookie(void *client, rt_string domain, rt_string name);

/// @brief Clone all cookies matching @p domain into a new Map<String,String>.
/// @details Native key/value bytes are copied while the jar is locked, then
///          managed Map construction occurs after unlocking. Trap recovery
///          releases a partial Map, partial Strings, and the native snapshot.
/// @param client HttpClient receiver; NULL returns an empty Map.
/// @param domain Host name used for normal cookie-domain matching.
/// @return Caller-owned Map snapshot, or NULL after a returning trap hook.
void *rt_http_client_get_cookies(void *client, rt_string domain);

#ifdef __cplusplus
}
#endif
