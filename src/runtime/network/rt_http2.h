//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_http2.h
// Purpose: Internal HTTP/2 + HPACK transport used by the HTTPS runtime.
//
// Key invariants:
//   - One connection processes synchronous request/response exchanges; callers
//     must serialize access.
//   - Header names are lowercase on the wire and connection-specific HTTP/1.x
//     fields are rejected.
//   - Body and decoded-header limits are enforced before ownership is returned.
//
// Ownership/Lifetime:
//   - Connections copy the callback table but borrow its context pointer.
//   - Successful receive/round-trip calls populate caller-owned result objects
//     that must be released with the matching free helper.
//
// Links: src/runtime/network/rt_http2.c,
//        src/runtime/network/rt_hpack.c,
//        src/runtime/network/rt_http2_internal.h
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares the synchronous callback-backed HTTP/2 transport.
 * @details Defines byte I/O callbacks, owned header/request/response values,
 * client and server connection lifecycles, validated request/response exchange,
 * diagnostic access, and matching cleanup helpers.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Read up to a requested number of transport bytes.
/// @param ctx Caller-defined transport context.
/// @param buf Destination buffer.
/// @param len Maximum bytes requested.
/// @return Positive byte count no greater than @p len, or a non-positive value on EOF or failure.
typedef long (*rt_http2_read_fn)(void *ctx, uint8_t *buf, size_t len);

/// @brief Write one transport byte range.
/// @param ctx Caller-defined transport context.
/// @param buf Bytes to write.
/// @param len Number of bytes in @p buf.
/// @return Nonzero when the entire range was written; zero on failure.
typedef int (*rt_http2_write_fn)(void *ctx, const uint8_t *buf, size_t len);

/// @brief Callback-backed byte transport used by an HTTP/2 connection.
typedef struct rt_http2_io {
    /// @brief Borrowed context passed unchanged to both callbacks.
    void *ctx;
    /// @brief Blocking or synchronous read callback.
    rt_http2_read_fn read;
    /// @brief Whole-range write callback.
    rt_http2_write_fn write;
} rt_http2_io_t;

/// @brief Owned node in a singly linked HTTP/2 header list.
typedef struct rt_http2_header {
    /// @brief Owned null-terminated header name.
    char *name;
    /// @brief Owned null-terminated header value.
    char *value;
    /// @brief Next owned node, or null.
    struct rt_http2_header *next;
} rt_http2_header_t;

/// @brief Opaque state for one synchronous HTTP/2 endpoint connection.
typedef struct rt_http2_conn rt_http2_conn_t;

/// @brief Decoded HTTP/2 request and its owned payload data.
typedef struct rt_http2_request {
    /// @brief Positive peer-initiated stream identifier.
    int stream_id;
    /// @brief Owned `:method` value.
    char *method;
    /// @brief Owned `:scheme` value.
    char *scheme;
    /// @brief Owned optional `:authority` value.
    char *authority;
    /// @brief Owned `:path` value.
    char *path;
    /// @brief Owned regular headers and trailers.
    rt_http2_header_t *headers;
    /// @brief Owned body bytes, not necessarily null-terminated.
    uint8_t *body;
    /// @brief Number of bytes in the body buffer.
    size_t body_len;
} rt_http2_request_t;

/// @brief Decoded HTTP/2 response and its owned payload data.
typedef struct rt_http2_response {
    /// @brief Positive client-initiated stream identifier.
    int stream_id;
    /// @brief Final HTTP status code in the range 100 through 599.
    int status;
    /// @brief Owned regular headers and trailers.
    rt_http2_header_t *headers;
    /// @brief Owned body bytes, not necessarily null-terminated.
    uint8_t *body;
    /// @brief Number of bytes in the body buffer.
    size_t body_len;
} rt_http2_response_t;

/// @brief Allocate a client-side HTTP/2 connection over callback I/O.
/// @param io Callback table to copy; its context remains caller-owned.
/// @return New connection, or null when callbacks are invalid or allocation fails.
rt_http2_conn_t *rt_http2_client_new(const rt_http2_io_t *io);

/// @brief Allocate a server-side HTTP/2 connection over callback I/O.
/// @param io Callback table to copy; its context remains caller-owned.
/// @return New connection, or null when callbacks are invalid or allocation fails.
rt_http2_conn_t *rt_http2_server_new(const rt_http2_io_t *io);

/// @brief Release an HTTP/2 connection and its HPACK state.
/// @param conn Connection to release; may be null.
void rt_http2_conn_free(rt_http2_conn_t *conn);

/// @brief Read the connection's most recent diagnostic message.
/// @param conn Connection to inspect; may be null.
/// @return Borrowed message valid until the connection is modified or freed.
const char *rt_http2_get_error(const rt_http2_conn_t *conn);

/// @brief Test whether a connection can still participate in protocol I/O.
/// @param conn Connection to inspect.
/// @return Nonzero when the connection is open and has both I/O callbacks.
int rt_http2_conn_is_usable(const rt_http2_conn_t *conn);

/// @brief Send one client request and synchronously receive its final response.
/// @details Starts the connection on first use, handles informational responses,
///          appends trailers to the returned header list, and closes the
///          logical stream after END_STREAM.
/// @param conn Client-side connection.
/// @param method Request `:method` value.
/// @param scheme Request `:scheme` value.
/// @param authority Request `:authority` value.
/// @param path Request `:path` value.
/// @param headers Optional regular request headers.
/// @param body Optional request body bytes.
/// @param body_len Number of request body bytes.
/// @param max_body_len Maximum response body bytes accepted.
/// @param out_res Receives an owned response; initialized to empty before processing.
/// @return 1 on a complete response; 0 on validation, protocol, I/O, or allocation failure.
int rt_http2_client_roundtrip(rt_http2_conn_t *conn,
                              const char *method,
                              const char *scheme,
                              const char *authority,
                              const char *path,
                              const rt_http2_header_t *headers,
                              const uint8_t *body,
                              size_t body_len,
                              size_t max_body_len,
                              rt_http2_response_t *out_res);

/// @brief Synchronously receive the next client request on a server connection.
/// @details Reads and validates request pseudo-headers, regular headers,
///          optional trailers, and body data. Concurrent request streams are
///          refused because this transport processes one active request at a time.
/// @param conn Server-side connection.
/// @param max_body_len Maximum request body bytes accepted.
/// @param out_req Receives an owned request; initialized to empty before processing.
/// @return 1 on a complete request; 0 on validation, protocol, I/O, or allocation failure.
int rt_http2_server_receive_request(rt_http2_conn_t *conn,
                                    size_t max_body_len,
                                    rt_http2_request_t *out_req);

/// @brief Send a complete response on an existing server-side stream.
/// @param conn Server-side connection.
/// @param stream_id Positive request stream identifier.
/// @param status HTTP status code in the range 100 through 599.
/// @param headers Optional regular response headers.
/// @param body Optional response body bytes.
/// @param body_len Number of response body bytes.
/// @return 1 on success; 0 on invalid input, protocol, I/O, or allocation failure.
int rt_http2_server_send_response(rt_http2_conn_t *conn,
                                  int stream_id,
                                  int status,
                                  const rt_http2_header_t *headers,
                                  const uint8_t *body,
                                  size_t body_len);

/// @brief Release an owned linked list of HTTP/2 headers.
/// @param headers First node to release; may be null.
void rt_http2_headers_free(rt_http2_header_t *headers);

/// @brief Release all allocations in a decoded request and reset it to zero.
/// @param req Request to clear; may be null.
void rt_http2_request_free(rt_http2_request_t *req);

/// @brief Release all allocations in a decoded response and reset it to zero.
/// @param res Response to clear; may be null.
void rt_http2_response_free(rt_http2_response_t *res);

/// @brief Append copied name and value strings to an owned header list.
/// @param list Address of the list head to update.
/// @param name Null-terminated header name to copy.
/// @param value Null-terminated header value to copy.
/// @return 1 on success; 0 for invalid input or allocation failure.
int rt_http2_header_append_copy(rt_http2_header_t **list, const char *name, const char *value);

/// @brief Find the first case-insensitive header-name match in a list.
/// @param list Header list to search; may be null.
/// @param name Header name to find.
/// @return Borrowed value pointer for the first match, or null when absent.
const char *rt_http2_header_get(const rt_http2_header_t *list, const char *name);

#ifdef __cplusplus
}
#endif
