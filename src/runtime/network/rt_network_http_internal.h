//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_network_http_internal.h
// Purpose: Internal helpers shared between the HTTP transport and higher-level
//          HTTP client wrappers.
// Key invariants:
//   - Private class identifiers and payload layouts are stable across every
//     HTTP wrapper that consumes the corresponding managed object.
//   - Helper APIs preserve the public transport's trap-safe ownership contract.
// Ownership/Lifetime:
//   - Declarations own no storage. Parameters are borrowed unless an individual
//     function's Doxygen contract explicitly transfers ownership.
// Links: rt_network_http.c, rt_http_client.c, rt_restclient.c,
//        docs/adr/0126-http-client-stable-identity-and-transactional-ownership.md
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_network_http_internal.h
 * @brief Declares shared native HTTP request, response, and pool interfaces.
 * @details This private boundary defines the stable managed class identities
 *          and native payload layouts shared by the HTTP transport core and
 *          its public API wrappers. It also declares request construction,
 *          header mutation, URL parsing, execution, and connection-pool
 *          helpers whose ownership contracts cross translation units.
 */

#pragma once

#include "rt_string.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

// Default per-request redirect cap / timeout (shared by client + wrappers).
#define HTTP_MAX_REDIRECTS 5
#define HTTP_DEFAULT_TIMEOUT_MS 30000

/// @brief Stable private class identity for public HttpReq objects.
/// @details Request methods validate this tag and the complete payload size
///          before accessing native URL/header/body ownership fields.
#define RT_HTTP_REQ_CLASS_ID INT64_C(-0x72020A)

/// @brief Stable private class identity for public HttpRes objects.
/// @details Response accessors use this identity to reject unrelated managed
///          values before copying status, header Map, or native body storage.
#define RT_HTTP_RES_CLASS_ID INT64_C(-0x72020B)

/// @brief Stable private class identity for internal HTTP connection pools.
/// @details HttpClient, RestClient, and standalone HttpReq keep-alive paths may
///          share this mutex-bearing payload across module boundaries.
#define RT_HTTP_CONN_POOL_CLASS_ID INT64_C(-0x72020C)

/// @brief Parsed URL structure.
typedef struct parsed_url {
    char *host;  // Allocated hostname
    int port;    // Port number (default 80 for http, 443 for https)
    char *path;  // Path including query string (allocated)
    int use_tls; // 1 for https, 0 for http
} parsed_url_t;

/// @brief HTTP header entry.
typedef struct http_header {
    char *name;
    char *value;
    struct http_header *next;
} http_header_t;

/// @brief HTTP request structure.
typedef struct rt_http_req {
    char *method;           // HTTP method
    parsed_url_t url;       // Parsed URL
    http_header_t *headers; // Linked list of headers
    uint8_t *body;          // Request body
    size_t body_len;        // Body length
    int timeout_ms;         // Timeout in milliseconds
    int tls_verify;         // 1 = verify peer certificate, 0 = allow insecure HTTPS
    int follow_redirects;   // Automatically follow redirect responses
    int max_redirects;      // Redirect limit for this request
    int accept_gzip;        // Advertise gzip support when no explicit Accept-Encoding is set
    int decode_gzip;        // Transparently decode gzip-encoded response bodies
    int keep_alive;         // Request connection reuse when paired with a pool
    void *connection_pool;  // Internal pool used by session-style HTTP clients
    int force_http1;        // Keep the transport on HTTP/1.1 even over TLS
} rt_http_req_t;

/// @brief HTTP response structure.
typedef struct rt_http_res {
    int status;        // HTTP status code
    char *status_text; // Status text (allocated)
    void *headers;     // Map of headers
    uint8_t *body;     // Response body
    size_t body_len;   // Body length
} rt_http_res_t;

#ifdef __cplusplus
extern "C" {
#endif

// HTTP client core (defined in rt_network_http.c, consumed by rt_network_http_api.c).

/// @brief Validate an HTTP method or header name against token syntax.
/// @param method Nonempty NUL-terminated candidate token.
/// @return One for a valid token, otherwise zero.
int http_method_is_token(const char *method);

/// @brief Detect a NUL inside a managed String's logical payload.
/// @param text Managed String to inspect; NULL reports no embedded NUL.
/// @return One when a NUL precedes the logical end, otherwise zero.
int http_rt_string_has_embedded_nul(rt_string text);

/// @brief Free and clear the native host/path fields of a parsed HTTP URL.
/// @param url Parsed URL whose owned fields are released.
void free_parsed_url(parsed_url_t *url);

/// @brief Parse an HTTP/HTTPS URL into native request components.
/// @details Accepts bracketed IPv6, validates ports and request-target safety,
///          strips fragments, and supplies the default scheme port and `/` path.
/// @param url_str Nonempty NUL-terminated URL.
/// @param result Receives owned parsed fields and normalized TLS state.
/// @return Zero on success, otherwise -1.
int parse_url(const char *url_str, parsed_url_t *result);

/// @brief Free a complete native request-header linked list.
/// @param headers Owned list head, or NULL.
void free_headers(http_header_t *headers);

/// @brief Append one validated native request header.
/// @details The request is unchanged when validation or any allocation fails.
///          This helper reports allocation failure through its return value so
///          public callers can raise exactly one trap after preserving state.
/// @param req Request that will own the copied header node.
/// @param name Non-empty HTTP field-name token.
/// @param value Field value without CR or LF bytes.
/// @return One when appended; zero for invalid input or allocation failure.
int add_header(rt_http_req_t *req, const char *name, const char *value);

/// @brief Atomically replace every case-insensitive request header match.
/// @details A complete replacement node is allocated before existing entries
///          are removed, so OOM leaves the prior request headers untouched.
/// @param req Request whose header list will be updated.
/// @param name Non-empty HTTP field-name token.
/// @param value Field value without CR or LF bytes.
/// @return One when replaced; zero for invalid input or allocation failure.
int set_header(rt_http_req_t *req, const char *name, const char *value);

/// @brief Check for a request header name case-insensitively.
/// @param req Request whose list is searched.
/// @param name Header name to find.
/// @return true on a match, otherwise false.
bool has_header(rt_http_req_t *req, const char *name);

/// @brief Remove all case-insensitive request-header matches.
/// @param req Request whose owned list is mutated.
/// @param name Header name to remove.
void remove_header(rt_http_req_t *req, const char *name);

/// @brief Case-insensitively remove all entries named @p name from a Map.
/// @details The key snapshot is built before mutation. Managed allocation traps
///          are contained, the partial snapshot is released, and the Map is
///          left unchanged when the snapshot cannot be constructed.
/// @param map Header-name-keyed managed Map.
/// @param name Non-empty NUL-terminated HTTP field name.
/// @return One after removal (including a miss); zero on invalid input or
///         snapshot allocation failure.
int rt_http_header_map_remove_ci(void *map, const char *name);

/// @brief Transactionally replace a case-insensitive header-map entry.
/// @details Existing spellings are snapshotted first. The new exact key/value
///          is inserted before differently cased aliases are removed, so a
///          managed allocation trap leaves every prior entry intact. Traps are
///          contained and reported through the return value so callers can
///          unlock mutable owners before raising a public diagnostic.
/// @param map Header-name-keyed managed Map.
/// @param name Valid managed HTTP field-name String.
/// @param value Managed value retained by the Map on success.
/// @return One after atomic replacement; zero on invalid input or allocation
///         failure.
int rt_http_header_map_set_ci(void *map, rt_string name, void *value);

/// @brief Copy a managed String into a request-owned body buffer.
/// @param req Native request that receives the buffer.
/// @param body Managed String whose exact logical bytes are copied.
void set_request_body_from_string(rt_http_req_t *req, rt_string body);

/// @brief Execute a configured HTTP request.
/// @param req Fully initialized request.
/// @param redirects_remaining Remaining redirect-hop budget.
/// @return Newly owned managed response, or NULL after a returning trap.
rt_http_res_t *do_http_request(rt_http_req_t *req, int redirects_remaining);

/// @brief Stream a successful HTTP response into an open file.
/// @param req Fully initialized download request.
/// @param redirects_remaining Remaining redirect-hop budget.
/// @param out Open writable destination stream.
/// @return One after a complete 2xx body, otherwise zero.
int do_http_download_request(rt_http_req_t *req, int redirects_remaining, FILE *out);

/// @brief Create an internal HTTP connection pool for keep-alive reuse.
/// @param max_size Requested entry cap; nonpositive or oversized values select
///        the implementation hard limit.
/// @return Newly owned managed pool, or NULL on allocation/initialization failure.
void *rt_http_conn_pool_new(int64_t max_size);

/// @brief Test stable identity and initialized native state of an HTTP pool.
/// @details This predicate does not retain @p pool. Public ownership-transfer
///          boundaries must retain and then repeat this check before storing it.
/// @param pool Candidate managed object.
/// @return Nonzero only for a fully sized, mutex-initialized HTTP pool.
int rt_http_conn_pool_is_handle(void *pool);

/// @brief Drop every idle connection from an internal HTTP connection pool.
/// @details Safely retains and validates the pool, then closes all entries under
///          its mutex. NULL is a no-op.
/// @param pool Managed pool receiver, or NULL.
void rt_http_conn_pool_clear(void *pool);

/// @brief Lazily created process-wide pool backing standalone keep-alive
///        `HttpReq` sends (thread-safe once-init; never freed).
/// @return Process-lifetime managed pool, or NULL after a returning trap.
void *rt_http_default_connection_pool(void);

/// @brief Toggle keep-alive / pooled transport on a request.
/// @param obj Required HttpReq receiver.
/// @param keep_alive Nonzero to enable reuse, zero to close after response.
/// @return @p obj for fluent chaining.
void *rt_http_req_set_keep_alive(void *obj, int8_t keep_alive);

/// @brief Attach an internal HTTP connection pool to a request.
/// @param obj Required HttpReq receiver.
/// @param pool Managed pool to retain, or NULL to detach.
/// @return @p obj for fluent chaining.
void *rt_http_req_set_connection_pool(void *obj, void *pool);

/// @brief Return true when a cross-origin redirect should drop the header.
/// @param name Header field name compared case-insensitively.
/// @return One for a credential-bearing sensitive header, otherwise zero.
int8_t rt_http_header_is_sensitive_for_cross_origin_redirect(const char *name);

/// @brief Return true when a comma-separated HTTP header contains @p token.
/// @param value Header field value.
/// @param token Token to find case-insensitively.
/// @return One when present, otherwise zero.
int8_t rt_http_header_value_has_token(const char *value, const char *token);

/// @brief Compare two absolute URLs for same-origin equality.
/// @param lhs First absolute URL String.
/// @param rhs Second absolute URL String.
/// @return One when scheme, host, and port match, otherwise zero.
int8_t rt_http_url_has_same_origin(rt_string lhs, rt_string rhs);

/// @brief Resolve a redirect Location value against the current absolute URL.
/// @param current_url Current absolute request URL.
/// @param location Absolute, scheme-relative, or relative Location String.
/// @return Newly owned resolved absolute URL String.
rt_string rt_http_resolve_redirect_url(rt_string current_url, rt_string location);

#ifdef __cplusplus
}
#endif
