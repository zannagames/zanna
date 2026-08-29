//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_websocket.c
// Purpose: WebSocket client implementing RFC 6455.
// Key invariants:
//   - Opening responses and frame encodings are validated strictly against RFC 6455.
//   - Client frames are masked with fresh unpredictable keys; server frames are unmasked.
//   - Native socket handles remain pointer-width and are owned by exactly one client/TLS session.
// Ownership/Lifetime:
//   - Managed WebSocket objects own URL/protocol/reason buffers and one TCP or TLS transport.
//   - Public receivers are validated before native state is read; finalization is partial-safe.
// Links: src/runtime/network/rt_websocket.h,
//        src/runtime/network/rt_websocket_internal.h,
//        src/runtime/network/rt_ws_crypto.c,
//        src/runtime/network/rt_socket_platform.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_websocket.c
 * @brief Implements the managed RFC 6455 WebSocket client.
 * @details The client parses ws and wss URLs, performs strict HTTP upgrades,
 *          negotiates optional subprotocols, masks outbound frames, validates
 *          and reassembles inbound frames, handles control traffic, and owns
 *          exactly one plain socket or TLS session until close.
 */

#include "rt_websocket.h"

#include "rt_bytes.h"
#include "rt_crypto.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_platform.h"
#include "rt_random.h"
#include "rt_socket_platform.h"
#include "rt_string.h"
#include "rt_time.h"
#include "rt_tls.h"

#include <limits.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Forward declarations (defined in rt_io.c).
#include "rt_trap.h"
extern void rt_trap_net(const char *msg, int err_code);
extern int rt_trap_get_net_code(void);

#include "rt_error.h"

#include "rt_websocket_internal.h"

static int ws_wait_socket(socket_t fd, int timeout_ms, int for_write);

// WebSocket opcodes
#define WS_OP_CONTINUATION 0x00
#define WS_OP_TEXT 0x01
#define WS_OP_BINARY 0x02
#define WS_OP_CLOSE 0x08
#define WS_OP_PING 0x09
#define WS_OP_PONG 0x0A

// Frame flags
#define WS_FIN 0x80
#define WS_MASK 0x80

// Close codes
#define WS_CLOSE_NORMAL 1000
#define WS_CLOSE_GOING_AWAY 1001
#define WS_CLOSE_PROTOCOL_ERROR 1002
#define WS_CLOSE_UNSUPPORTED 1003
#define WS_CLOSE_NO_STATUS 1005
#define WS_CLOSE_ABNORMAL 1006
#define WS_CLOSE_INVALID_DATA 1007
#define WS_CLOSE_MESSAGE_TOO_BIG 1009
#define WS_CLOSE_INTERNAL_ERROR 1011

// Maximum total size for reassembled fragmented messages (64 MB)
#define WS_MAX_REASSEMBLY_SIZE (64u * 1024u * 1024u)
#define WS_MAX_HANDSHAKE_HEADER_BYTES (16u * 1024u)

/// @brief WebSocket connection implementation.
typedef struct rt_ws_impl {
    void **vptr;             ///< Vtable pointer placeholder.
    socket_t socket_fd;      ///< Pointer-width native TCP socket.
    rt_tls_session_t *tls;   ///< TLS session (NULL for ws://).
    char *url;               ///< Original connection URL.
    char *subprotocol;       ///< Negotiated Sec-WebSocket-Protocol, if any.
    int8_t is_open;          ///< Connection state.
    int64_t close_code;      ///< Close status code.
    char *close_reason;      ///< Close reason string.
    size_t close_reason_len; ///< Length of close_reason in bytes.
    uint8_t *recv_buffer;    ///< Buffer for receiving frames.
    size_t recv_buffer_size; ///< Size of receive buffer.
    size_t recv_buffer_len;  ///< Bytes currently in buffer.
    size_t recv_buffer_pos;  ///< Read cursor within recv_buffer.
    int timeout_ms;          ///< Configured socket/TLS timeout used for retries.
} rt_ws_impl;

/// @brief Validate and cast a public WebSocket receiver.
/// @details Stable identity and complete payload size are checked before any
///          descriptor or heap pointer is read. Invalid non-null receivers emit
///          one trap and return NULL so a returning trap hook cannot permit
///          unsafe continuation.
/// @param obj Candidate managed WebSocket handle.
/// @param operation Operation-specific diagnostic.
/// @return Valid WebSocket payload, or NULL after trapping.
static rt_ws_impl *ws_require(void *obj, const char *operation) {
    if (!rt_obj_is_instance(obj, RT_WEBSOCKET_CLASS_ID, sizeof(rt_ws_impl))) {
        rt_trap(operation ? operation : "WebSocket: invalid object");
        return NULL;
    }
    return (rt_ws_impl *)obj;
}

static void ws_close_transport(rt_ws_impl *ws);

/// @brief Determine whether a host must be bracketed in an HTTP Host field.
/// @param[in] host Null-terminated hostname or address literal.
/// @return 1 when @p host contains a colon and is not already bracketed; otherwise 0.
static int host_needs_brackets(const char *host) {
    return host && strchr(host, ':') != NULL && host[0] != '[';
}

/// @brief Detect a null byte inside a length-delimited string payload.
/// @param[in] data Byte sequence to inspect.
/// @param[in] len Number of bytes available at @p data.
/// @return 1 when @p data is non-null and contains a null byte; otherwise 0.
static int ws_has_embedded_nul(const char *data, size_t len) {
    return data && memchr(data, '\0', len) != NULL;
}

/// @brief Validate a managed string and expose its borrowed byte view.
/// @details Output fields are cleared before validation. Invalid strings trap
///          with @p null_msg; lengths not representable as size_t trap with a
///          fixed WebSocket diagnostic.
/// @param[in] str Managed string handle to inspect.
/// @param[out] out Receives a borrowed pointer valid while @p str remains alive.
/// @param[out] len Receives the logical byte length, excluding the terminator.
/// @param[in] null_msg Diagnostic used when @p str is null or invalid.
/// @return 1 on success, or 0 after trapping or for null output arguments.
static int ws_string_bytes(rt_string str, const char **out, size_t *len, const char *null_msg) {
    if (!out || !len)
        return 0;
    *out = NULL;
    *len = 0;
    if (!str || !rt_string_is_handle(str)) {
        rt_trap(null_msg);
        return 0;
    }
    const char *data = rt_string_cstr(str);
    int64_t n = rt_str_len(str);
    if (!data || n < 0) {
        rt_trap(null_msg);
        return 0;
    }
    if ((uint64_t)n > (uint64_t)SIZE_MAX) {
        rt_trap("WebSocket: string is too large");
        return 0;
    }
    *out = data;
    *len = (size_t)n;
    return 1;
}

/// @brief Return the default TCP port for a WebSocket URL scheme.
/// @param[in] is_secure Nonzero for `wss`; zero for `ws`.
/// @return 443 for secure WebSockets, or 80 for plaintext WebSockets.
static int ws_default_port(int is_secure) {
    return is_secure ? 443 : 80;
}

/// @brief Format the authority value for an HTTP Host header.
/// @details IPv6 literals are bracketed and a port is appended only when it is
///          not the default for the selected WebSocket scheme.
/// @param[out] buf Destination character buffer.
/// @param[in] buf_len Capacity of @p buf including its terminator.
/// @param[in] host Null-terminated hostname or unbracketed address literal.
/// @param[in] port Positive TCP port.
/// @param[in] is_secure Nonzero for `wss`; zero for `ws`.
/// @return snprintf-style character count, or a negative value for invalid arguments
///         or formatting failure. A value at least @p buf_len indicates truncation.
static int ws_format_host_header(
    char *buf, size_t buf_len, const char *host, int port, int is_secure) {
    int include_port = 0;

    if (!buf || buf_len == 0 || !host || port <= 0)
        return -1;

    include_port = (port != ws_default_port(is_secure));
    if (include_port) {
        return snprintf(buf,
                        buf_len,
                        "%s%s%s:%d",
                        host_needs_brackets(host) ? "[" : "",
                        host,
                        host_needs_brackets(host) ? "]" : "",
                        port);
    }

    return snprintf(buf,
                    buf_len,
                    "%s%s%s",
                    host_needs_brackets(host) ? "[" : "",
                    host,
                    host_needs_brackets(host) ? "]" : "");
}

/// @brief Validate one RFC 2616 token used as a WebSocket subprotocol.
/// @param[in] value Null-terminated token candidate.
/// @return 1 for a nonempty token containing no controls or separators; otherwise 0.
static int ws_token_is_valid(const char *value) {
    static const char *kSeparators = "()<>@,;:\\\"/[]?={} \t";
    if (!value || !*value)
        return 0;
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (*p <= 0x20 || *p == 0x7Fu || strchr(kSeparators, (int)*p) != NULL)
            return 0;
    }
    return 1;
}

/// @brief Compare a null-terminated ASCII prefix without case sensitivity.
/// @param[in] text Text whose beginning is tested.
/// @param[in] prefix Prefix to match.
/// @return 1 when every byte of @p prefix matches the start of @p text; otherwise 0.
static int ws_ascii_ieq_prefix(const char *text, const char *prefix) {
    size_t i = 0;
    if (!text || !prefix)
        return 0;
    for (; prefix[i]; ++i) {
        unsigned char a = (unsigned char)text[i];
        unsigned char b = (unsigned char)prefix[i];
        if (a >= 'A' && a <= 'Z')
            a = (unsigned char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z')
            b = (unsigned char)(b + ('a' - 'A'));
        if (a != b)
            return 0;
    }
    return 1;
}

/// @brief Validate a parsed WebSocket host against HTTP authority delimiters.
/// @param[in] host Null-terminated host with IPv6 brackets already removed.
/// @return 1 for a nonempty host without controls, whitespace, or URL delimiters;
///         otherwise 0.
static int ws_host_is_valid(const char *host) {
    if (!host || !*host)
        return 0;
    for (const unsigned char *p = (const unsigned char *)host; *p; ++p) {
        if (*p <= 0x20 || *p == 0x7Fu || *p == '/' || *p == '?' || *p == '#')
            return 0;
    }
    return 1;
}

/// @brief Validate an HTTP request target derived from a WebSocket URL.
/// @param[in] target Null-terminated origin-form request target.
/// @return 1 when @p target begins with `/` and contains no controls,
///         whitespace, or fragment marker; otherwise 0.
static int ws_request_target_is_valid(const char *target) {
    if (!target || target[0] != '/')
        return 0;
    for (const unsigned char *p = (const unsigned char *)target; *p; ++p) {
        if (*p <= 0x20 || *p == 0x7Fu || *p == '#')
            return 0;
    }
    return 1;
}

/// @brief Convert a public millisecond timeout to the native integer range.
/// @param[in] timeout_ms Nonnegative timeout value.
/// @param[out] out_timeout_ms Optional destination for the converted timeout.
/// @return 1 when @p timeout_ms fits in int; otherwise 0.
static int ws_timeout_ms_to_int(int64_t timeout_ms, int *out_timeout_ms) {
    if (timeout_ms < 0 || timeout_ms > INT_MAX)
        return 0;
    if (out_timeout_ms)
        *out_timeout_ms = (int)timeout_ms;
    return 1;
}

/// @brief Format a Host header value through the production helper for focused tests.
/// @param[in] host Null-terminated hostname or address literal.
/// @param[in] port Positive TCP port.
/// @param[in] is_secure Nonzero to apply the `wss` default port.
/// @return Newly allocated null-terminated authority string, or NULL on invalid
///         input, truncation, formatting failure, or allocation failure.
char *rt_ws_format_host_header_for_test(const char *host, int port, int is_secure) {
    char header[512];
    int len = ws_format_host_header(header, sizeof(header), host, port, is_secure);
    char *result = NULL;

    if (len < 0 || (size_t)len >= sizeof(header))
        return NULL;

    result = (char *)malloc((size_t)len + 1);
    if (!result)
        return NULL;
    memcpy(result, header, (size_t)len + 1);
    return result;
}

/// @brief Validate a WebSocket frame opcode against RFC 6455 §5.2.
///
/// Only the six defined opcodes (continuation, text, binary, close,
/// ping, pong) are legal — reserved opcodes 0x3-0x7 and 0xB-0xF
/// must trigger a protocol-error close.
/// @param[in] opcode Four-bit frame opcode to classify.
/// @return 1 if `opcode` is a defined WS opcode, 0 otherwise.
static int ws_is_valid_opcode(uint8_t opcode) {
    switch (opcode) {
        case WS_OP_CONTINUATION:
        case WS_OP_TEXT:
        case WS_OP_BINARY:
        case WS_OP_CLOSE:
        case WS_OP_PING:
        case WS_OP_PONG:
            return 1;
        default:
            return 0;
    }
}

/// @brief Encode `len` as a big-endian 8-byte payload-length field.
///
/// Used for WebSocket frames whose payload is >= 65536 bytes
/// (RFC 6455 §5.2 — the 7+64 length encoding).
/// @param[out] out Eight-byte destination field.
/// @param[in] len Host payload length to encode.
static void ws_encode_u64_len(uint8_t out[8], size_t len) {
    uint64_t value = (uint64_t)len;
    for (int i = 7; i >= 0; i--) {
        out[i] = (uint8_t)(value & 0xFFu);
        value >>= 8;
    }
}

/// @brief Decode an 8-byte big-endian length field into a host `size_t`.
///
/// Rejects values that exceed `SIZE_MAX` so we never silently
/// truncate on 32-bit platforms.
/// @param[in] in Eight-byte network-order payload-length field.
/// @param[out] len_out Receives the host-representable payload length.
/// @return 1 on success, 0 if the value would overflow `size_t`.
static int ws_decode_u64_len(const uint8_t in[8], size_t *len_out) {
    uint64_t value = 0;
    for (int i = 0; i < 8; i++)
        value = (value << 8) | in[i];
    if (value > (uint64_t)SIZE_MAX)
        return 0;
    *len_out = (size_t)value;
    return 1;
}

/// @brief Check whether a close code may appear on the WebSocket wire.
/// @param[in] code Unsigned close status code.
/// @return 1 for defined protocol codes or application/private codes 3000-4999;
///         otherwise 0, including reserved codes 1004-1006.
static int ws_close_code_is_valid(uint16_t code) {
    if (code >= 3000 && code <= 4999)
        return 1;
    if (code < 1000 || code > 1014)
        return 0;
    return code != 1004 && code != 1005 && code != 1006;
}

/// @brief Expose close-code validation without narrowing invalid test inputs.
/// @param[in] code Integer close-code candidate.
/// @return 1 when @p code fits uint16_t and is valid on the wire; otherwise 0.
int rt_ws_close_code_valid_for_test(int code) {
    if (code < 0 || code > 65535)
        return 0;
    return ws_close_code_is_valid((uint16_t)code);
}

/// @brief Parse URL into components.
/// @details Accepts only `ws` and `wss`, supports bracketed IPv6 literals,
///          validates explicit ports and request-target bytes, ignores URL
///          fragments, and synthesizes `/` when no path is present. On
///          success, @p host and @p path are independently heap allocated.
/// @param[in] url Null-terminated WebSocket URL.
/// @param[out] is_secure Receives 1 for `wss` or 0 for `ws`.
/// @param[out] host Receives an allocated, unbracketed host string.
/// @param[out] port Receives the explicit or scheme-default TCP port.
/// @param[out] path Receives an allocated origin-form request target.
/// @return 1 on success, 0 on failure.
static int parse_ws_url(const char *url, int *is_secure, char **host, int *port, char **path) {
    *is_secure = 0;
    *host = NULL;
    *port = 80;
    *path = NULL;

    if (ws_ascii_ieq_prefix(url, "wss://")) {
        *is_secure = 1;
        *port = 443;
        url += 6;
    } else if (ws_ascii_ieq_prefix(url, "ws://")) {
        url += 5;
    } else {
        return 0; // Invalid scheme
    }

    const char *host_end = url;
    if (*url == '[') {
        const char *bracket_end = strchr(url + 1, ']');
        if (!bracket_end)
            return 0;
        size_t host_len = (size_t)(bracket_end - (url + 1));
        if (host_len == 0)
            return 0;
        *host = malloc(host_len + 1);
        if (!*host)
            return 0;
        memcpy(*host, url + 1, host_len);
        (*host)[host_len] = '\0';
        host_end = bracket_end + 1;
    } else {
        while (*host_end && *host_end != ':' && *host_end != '/' && *host_end != '?' &&
               *host_end != '#')
            host_end++;

        size_t host_len = (size_t)(host_end - url);
        if (host_len == 0)
            return 0;
        *host = malloc(host_len + 1);
        if (!*host)
            return 0;
        memcpy(*host, url, host_len);
        (*host)[host_len] = '\0';
    }

    if (*host_end && *host_end != ':' && *host_end != '/' && *host_end != '?' && *host_end != '#') {
        free(*host);
        *host = NULL;
        return 0;
    }
    if (!ws_host_is_valid(*host)) {
        free(*host);
        *host = NULL;
        return 0;
    }

    // Check for port
    if (*host_end == ':') {
        const char *p = host_end + 1;
        uint64_t port_val = 0;
        if (*p < '0' || *p > '9') {
            free(*host);
            *host = NULL;
            return 0;
        }
        while (*p >= '0' && *p <= '9') {
            port_val = port_val * 10u + (uint64_t)(*p - '0');
            if (port_val > 65535u) {
                free(*host);
                *host = NULL;
                return 0;
            }
            p++;
        }
        if (port_val == 0u || (*p != '\0' && *p != '/' && *p != '?' && *p != '#')) {
            free(*host);
            *host = NULL;
            return 0;
        }
        *port = (int)port_val;
        host_end = p;
    }

    // Path
    if (*host_end == '/') {
        const char *path_end = host_end;
        while (*path_end && *path_end != '#')
            path_end++;
        size_t path_len = (size_t)(path_end - host_end);
        *path = malloc(path_len + 1);
        if (*path) {
            memcpy(*path, host_end, path_len);
            (*path)[path_len] = '\0';
        }
    } else if (*host_end == '?') {
        const char *query_end = host_end;
        while (*query_end && *query_end != '#')
            query_end++;
        size_t query_len = (size_t)(query_end - host_end);
        *path = malloc(query_len + 2);
        if (*path) {
            (*path)[0] = '/';
            memcpy(*path + 1, host_end, query_len);
            (*path)[query_len + 1] = '\0';
        }
    } else {
        *path = strdup("/");
    }

    if (!*path) {
        free(*host);
        *host = NULL;
        return 0;
    }
    if (!ws_request_target_is_valid(*path)) {
        free(*host);
        free(*path);
        *host = NULL;
        *path = NULL;
        return 0;
    }

    return 1;
}

/// @brief Test hook exposing the otherwise-static `parse_ws_url`.
/// @details Output strings are heap-allocated on success and must be freed by
///          the caller.
/// @param[in] url Null-terminated WebSocket URL.
/// @param[out] is_secure Receives whether the URL uses `wss`.
/// @param[out] host Receives an allocated host string.
/// @param[out] port Receives the parsed or default port.
/// @param[out] path Receives an allocated request target.
/// @return 1 on success, or 0 for invalid input or allocation failure.
int rt_ws_parse_url_for_test(const char *url, int *is_secure, char **host, int *port, char **path) {
    return parse_ws_url(url, is_secure, host, port, path);
}

/// @brief Send data over connection (handles TLS vs plain TCP).
/// @param[in,out] ws Open WebSocket transport.
/// @param[in] data Bytes to send.
/// @param[in] len Number of bytes requested.
/// @return Positive byte count, zero for a closed transport, or a negative
///         transport-specific error result.
static long ws_send_partial(rt_ws_impl *ws, const void *data, size_t len) {
    if (ws->tls) {
        return rt_tls_send(ws->tls, data, len);
    } else {
        size_t chunk = len > INT_MAX ? INT_MAX : len;
        return send(ws->socket_fd, data, (int)chunk, SEND_FLAGS);
    }
}

/// @brief Return whether the last socket send failed transiently.
/// @details Plain TCP sends on nonblocking sockets can report interruption or
///          temporary backpressure. Those conditions should wait for writability
///          and retry instead of failing the whole WebSocket frame.
/// @return Non-zero when retrying the send is appropriate.
static int ws_send_should_retry(void) {
    int error = rt_socket_last_error();
    return rt_socket_error_is_interrupted(error) || rt_socket_error_is_would_block(error) ||
           rt_socket_error_is_in_progress(error);
}

/// @brief Send `len` bytes, looping on partial sends until done or failed.
///
/// Wraps `ws_send_partial` with a retry loop so callers don't have
/// to handle short writes from TCP/TLS. Transient nonblocking socket
/// failures wait for writability and retry.
/// @param[in,out] ws Open WebSocket transport.
/// @param[in] data Byte sequence to transmit.
/// @param[in] len Exact number of bytes to transmit.
/// @return 1 if all bytes sent, 0 on failure.
static int ws_send_all(rt_ws_impl *ws, const void *data, size_t len) {
    const uint8_t *ptr = (const uint8_t *)data;
    size_t total = 0;
    while (total < len) {
        long sent = ws_send_partial(ws, ptr + total, len - total);
        if (sent < 0 && !ws->tls && ws_send_should_retry()) {
            int timeout_ms = ws->timeout_ms > 0 ? ws->timeout_ms : 30000;
            if (ws_wait_socket(ws->socket_fd, timeout_ms, 1) > 0)
                continue;
            return 0;
        }
        if (sent <= 0)
            return 0;
        total += (size_t)sent;
    }
    return 1;
}

/// @brief Receive data from connection (handles TLS vs plain TCP).
/// @param[in,out] ws Open WebSocket transport.
/// @param[out] buffer Destination for received bytes.
/// @param[in] len Maximum number of bytes to receive.
/// @return Positive byte count, zero on orderly shutdown, or a negative
///         transport-specific error result.
static long ws_recv(rt_ws_impl *ws, void *buffer, size_t len) {
    if (ws->tls) {
        return rt_tls_recv(ws->tls, buffer, len);
    } else {
        size_t chunk = len > INT_MAX ? INT_MAX : len;
        return recv(ws->socket_fd, buffer, (int)chunk, 0);
    }
}

/// @brief Wait for socket to become readable or writable with timeout.
/// @param[in] fd Valid native socket handle.
/// @param[in] timeout_ms Nonnegative maximum wait in milliseconds.
/// @param[in] for_write Nonzero to wait for writability; zero for readability.
/// @return 1 if ready, 0 if timeout, -1 on error.
static int ws_wait_socket(socket_t fd, int timeout_ms, int for_write) {
    if (fd == INVALID_SOCK || timeout_ms < 0)
        return -1;
    return wait_socket(fd, timeout_ms, for_write != 0);
}

/// @brief Apply a receive timeout to the current native transport socket.
/// @param ws Valid WebSocket payload.
/// @param timeout_ms Nonnegative timeout in milliseconds.
/// @return 1 when the option was applied, otherwise 0.
static int ws_set_recv_timeout(rt_ws_impl *ws, int timeout_ms) {
    if (!ws || ws->socket_fd == INVALID_SOCK)
        return 0;
    if (ws->tls) {
        int effective_timeout = timeout_ms > 0 ? timeout_ms : 30000;
        return rt_tls_set_io_timeout(ws->tls, effective_timeout) == RT_TLS_OK ? 1 : 0;
    }
    return set_socket_timeout(ws->socket_fd, timeout_ms, true) ? 1 : 0;
}

/// @brief Case-insensitive ASCII compare of two `len`-byte regions.
///
/// Used for HTTP header name/value matching during the WebSocket
/// handshake. ASCII-only — does not lowercase non-ASCII bytes (a
/// deliberate narrow contract since headers are 7-bit ASCII per HTTP).
/// @param[in] a First byte region.
/// @param[in] b Second byte region.
/// @param[in] len Number of bytes to compare.
/// @return 1 if the regions match case-insensitively, 0 otherwise.
static int ws_ascii_ieq_n(const char *a, const char *b, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z')
            ca = (unsigned char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z')
            cb = (unsigned char)(cb + ('a' - 'A'));
        if (ca != cb)
            return 0;
    }
    return 1;
}

/// @brief Test whether a comma-separated header `value` contains `token`.
///
/// Splits on `,`, trims surrounding whitespace from each element,
/// and case-insensitively compares against `token`. Used to confirm
/// `Connection: Upgrade` (which may appear as `Connection:
/// keep-alive, Upgrade`).
/// @param[in] value Length-delimited HTTP field value.
/// @param[in] len Number of bytes in @p value.
/// @param[in] token Null-terminated token to locate.
/// @param[in] case_sensitive Nonzero for byte-exact matching; zero for ASCII
///                           case-insensitive matching.
/// @return 1 if token is present, 0 otherwise.
static int ws_header_has_token(const char *value,
                               size_t len,
                               const char *token,
                               int case_sensitive) {
    size_t token_len = strlen(token);
    size_t i = 0;
    while (i < len) {
        while (i < len && (value[i] == ' ' || value[i] == '\t' || value[i] == ','))
            i++;
        size_t start = i;
        while (i < len && value[i] != ',')
            i++;
        size_t end = i;
        while (end > start && (value[end - 1] == ' ' || value[end - 1] == '\t'))
            end--;
        if (end - start == token_len &&
            (case_sensitive ? memcmp(value + start, token, token_len) == 0
                            : ws_ascii_ieq_n(value + start, token, token_len)))
            return 1;
    }
    return 0;
}

/// @brief Trim optional HTTP whitespace from a header value slice.
///
/// Header parsing keeps values as non-owning slices into the response buffer.
/// This helper normalizes a slice by advancing past leading spaces/tabs and
/// shortening the length to exclude trailing spaces/tabs. It does not modify
/// the underlying response bytes.
///
/// @param value_io In/out pointer to the first byte of the value slice.
/// @param len_io   In/out length of the value slice in bytes.
static void ws_trim_header_value(const char **value_io, size_t *len_io) {
    const char *value;
    size_t len;
    if (!value_io || !*value_io || !len_io)
        return;
    value = *value_io;
    len = *len_io;
    while (len > 0 && (*value == ' ' || *value == '\t')) {
        value++;
        len--;
    }
    while (len > 0 && (value[len - 1] == ' ' || value[len - 1] == '\t'))
        len--;
    *value_io = value;
    *len_io = len;
}

/// @brief Validate one HTTP/1.1 field name as an ASCII token.
/// @details WebSocket handshake fields use the RFC 9110 token grammar. Bytes
///          outside visible ASCII and separator punctuation are rejected so a
///          malformed or smuggled field cannot be interpreted inconsistently.
/// @param name Non-owning field-name slice.
/// @param len Slice length.
/// @return 1 for a non-empty valid token, otherwise 0.
static int ws_http_field_name_is_valid(const char *name, size_t len) {
    static const char separators[] = "()<>@,;:\\\"/[]?={} \t";
    if (!name || len == 0)
        return 0;
    for (size_t index = 0; index < len; index++) {
        unsigned char value = (unsigned char)name[index];
        if (value <= 0x20u || value >= 0x7fu || strchr(separators, (int)value))
            return 0;
    }
    return 1;
}

/// @brief Validate a handshake field value before token-specific parsing.
/// @details Horizontal tab is accepted as HTTP optional whitespace. Other C0
///          controls, DEL, and non-ASCII bytes are rejected because every
///          WebSocket upgrade field consumed here has an ASCII grammar.
/// @param value Non-owning field-value slice.
/// @param len Slice length.
/// @return 1 when the slice is safe for strict handshake parsing.
static int ws_http_field_value_is_valid(const char *value, size_t len) {
    if (!value && len > 0)
        return 0;
    for (size_t index = 0; index < len; index++) {
        unsigned char byte = (unsigned char)value[index];
        if ((byte < 0x20u && byte != '\t') || byte >= 0x7fu)
            return 0;
    }
    return 1;
}

/// @brief Validate the server's WebSocket upgrade response (test-visible).
///
/// Per RFC 6455 §4.1 a successful handshake requires:
///   - Status line `HTTP/1.1 101 …`
///   - `Upgrade: websocket` header
///   - `Connection` header containing the `Upgrade` token
///   - `Sec-WebSocket-Accept` header equal to
///     `base64(SHA1(key_copy + WS_MAGIC))` where `WS_MAGIC` is
///     `258EAFA5-E914-47DA-95CA-C5AB0DC85B11`.
/// Made non-static to allow direct unit testing of the parser.
/// @param[in] response Null-terminated HTTP response through the empty header line.
/// @param[in] key_copy Null-terminated client Sec-WebSocket-Key value.
/// @param[in] expected_protocol Optional exact subprotocol token requested by the client.
/// @param[out] selected_protocol_out Optional destination for an allocated
///                                   negotiated protocol string; cleared first.
/// @return 1 on a valid response, 0 on any mismatch / malformed header.
static int ws_validate_handshake_response(const char *response,
                                          const char *key_copy,
                                          const char *expected_protocol,
                                          char **selected_protocol_out) {
    if (!response || !key_copy)
        return 0;
    if (selected_protocol_out)
        *selected_protocol_out = NULL;

    const char *line_end = strstr(response, "\r\n");
    if (!line_end)
        return 0;
    const size_t status_len = (size_t)(line_end - response);
    static const char switching_status[] = "HTTP/1.1 101";
    if (status_len < sizeof(switching_status) - 1u ||
        memcmp(response, switching_status, sizeof(switching_status) - 1u) != 0 ||
        (status_len > sizeof(switching_status) - 1u &&
         response[sizeof(switching_status) - 1u] != ' '))
        return 0;

    int upgrade_ok = 0;
    int connection_ok = 0;
    const char *accept_hdr = NULL;
    size_t accept_len = 0;
    const char *protocol_hdr = NULL;
    size_t protocol_len = 0;
    char negotiated_protocol[256] = {0};
    const char *p = line_end + 2;
    size_t header_count = 0;
    int saw_end = 0;
    while (*p) {
        const char *next = strstr(p, "\r\n");
        if (!next)
            return 0;
        if (next == p) {
            saw_end = 1;
            break;
        }
        if (++header_count > 100u || *p == ' ' || *p == '\t')
            return 0;

        const char *colon = (const char *)memchr(p, ':', (size_t)(next - p));
        if (!colon)
            return 0;
        size_t name_len = (size_t)(colon - p);
        const char *value = colon + 1;
        size_t value_len = (size_t)(next - value);
        if (!ws_http_field_name_is_valid(p, name_len) ||
            !ws_http_field_value_is_valid(value, value_len))
            return 0;
        ws_trim_header_value(&value, &value_len);

        if (name_len == strlen("Upgrade") && ws_ascii_ieq_n(p, "Upgrade", name_len)) {
            upgrade_ok = upgrade_ok || (value_len == strlen("websocket") &&
                                        ws_ascii_ieq_n(value, "websocket", value_len));
        } else if (name_len == strlen("Connection") && ws_ascii_ieq_n(p, "Connection", name_len)) {
            connection_ok = connection_ok || ws_header_has_token(value, value_len, "Upgrade", 0);
        } else if (name_len == strlen("Sec-WebSocket-Accept") &&
                   ws_ascii_ieq_n(p, "Sec-WebSocket-Accept", name_len)) {
            if (accept_hdr)
                return 0;
            accept_hdr = value;
            accept_len = value_len;
        } else if (name_len == strlen("Sec-WebSocket-Protocol") &&
                   ws_ascii_ieq_n(p, "Sec-WebSocket-Protocol", name_len)) {
            if (protocol_hdr)
                return 0;
            protocol_hdr = value;
            protocol_len = value_len;
        }

        p = next + 2;
    }

    if (!saw_end || !upgrade_ok || !connection_ok || !accept_hdr)
        return 0;

    if (expected_protocol && *expected_protocol) {
        if (!protocol_hdr || protocol_len == 0 || protocol_len >= sizeof(negotiated_protocol))
            return 0;
        memcpy(negotiated_protocol, protocol_hdr, protocol_len);
        negotiated_protocol[protocol_len] = '\0';
        while (protocol_len > 0 && (negotiated_protocol[protocol_len - 1] == ' ' ||
                                    negotiated_protocol[protocol_len - 1] == '\t')) {
            negotiated_protocol[--protocol_len] = '\0';
        }
        if (!ws_token_is_valid(expected_protocol) || !ws_token_is_valid(negotiated_protocol))
            return 0;
        if (strcmp(negotiated_protocol, expected_protocol) != 0)
            return 0;
    } else if (protocol_hdr && protocol_len > 0) {
        return 0;
    }

    char *expected_accept = rt_ws_compute_accept_key(key_copy);
    if (!expected_accept)
        return 0;
    size_t expected_len = strlen(expected_accept);
    int accept_ok =
        accept_len == expected_len && memcmp(accept_hdr, expected_accept, expected_len) == 0;
    free(expected_accept);
    if (accept_ok && selected_protocol_out && negotiated_protocol[0] != '\0') {
        *selected_protocol_out = strdup(negotiated_protocol);
        if (!*selected_protocol_out)
            return 0;
    }
    return accept_ok;
}

/// @brief Validate a handshake response without subprotocol negotiation for focused tests.
/// @param[in] response Null-terminated HTTP upgrade response.
/// @param[in] key_copy Client Sec-WebSocket-Key used to compute the expected accept value.
/// @return 1 when the response satisfies the production validator; otherwise 0.
int rt_ws_validate_handshake_response_for_test(const char *response, const char *key_copy) {
    return ws_validate_handshake_response(response, key_copy, NULL, NULL);
}

/// @brief Return the remaining part of one WebSocket connect deadline.
/// @param started_ms Monotonic start timestamp, or zero if unavailable.
/// @param timeout_ms Original positive timeout.
/// @return Remaining milliseconds, zero after expiry, or the original timeout
///         when the native monotonic clock is unavailable.
static int ws_connect_remaining_ms(uint64_t started_ms, int timeout_ms) {
    uint64_t now_ms = rt_socket_monotonic_ms();
    if (started_ms == 0 || now_ms == 0)
        return timeout_ms;
    if (now_ms < started_ms)
        return 0;
    uint64_t elapsed_ms = now_ms - started_ms;
    if (elapsed_ms >= (uint64_t)timeout_ms)
        return 0;
    return timeout_ms - (int)elapsed_ms;
}

/// @brief Create a pointer-width TCP socket under one address-attempt deadline.
/// @details Every nonblocking-mode transition and pending-error query must
///          succeed before a socket is published. All resolved addresses share
///          the caller's remaining timeout instead of each receiving a fresh
///          full wait.
/// @param host Validated DNS name or IP literal.
/// @param port TCP port in 1..65535.
/// @param timeout_ms Positive overall address-attempt timeout, or zero for blocking connect.
/// @return Connected native socket, or `INVALID_SOCK`.
static socket_t create_tcp_socket(const char *host, int port, int timeout_ms) {
    struct addrinfo hints, *res, *p;
    rt_net_init_wsa();
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0)
        return INVALID_SOCK;

    uint64_t started_ms = timeout_ms > 0 ? rt_socket_monotonic_ms() : 0;
    socket_t fd = INVALID_SOCK;
    for (p = res; p; p = p->ai_next) {
        int remaining_ms = timeout_ms > 0 ? ws_connect_remaining_ms(started_ms, timeout_ms) : 0;
        if (timeout_ms > 0 && remaining_ms <= 0)
            break;

        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd == INVALID_SOCK)
            continue;
        suppress_sigpipe(fd);

        if (timeout_ms > 0) {
            if (!rt_socket_set_nonblocking(fd, true)) {
                CLOSE_SOCKET(fd);
                fd = INVALID_SOCK;
                continue;
            }
            int connected = 0;
            int rc = connect(fd, p->ai_addr, (int)p->ai_addrlen);
            if (rc == 0) {
                connected = 1;
            } else {
                int error = rt_socket_last_error();
                int ready = rt_socket_error_is_in_progress(error)
                                ? ws_wait_socket(fd, remaining_ms, 1)
                                : -1;
                if (ready > 0) {
                    int so_error = 0;
                    connected = rt_socket_pending_error(fd, &so_error) && so_error == 0;
                }
            }

            if (connected && rt_socket_set_nonblocking(fd, false))
                break;
            CLOSE_SOCKET(fd);
            fd = INVALID_SOCK;
        } else {
            if (connect(fd, p->ai_addr, (int)p->ai_addrlen) == 0)
                break;

            CLOSE_SOCKET(fd);
            fd = INVALID_SOCK;
        }
    }

    freeaddrinfo(res);
    return fd;
}

/// @brief Return the offset one byte past the HTTP handshake header terminator.
/// @details The optimized handshake reader may receive the `\r\n\r\n`
///          terminator and the first WebSocket frame in the same socket read.
///          Returning the exact header-end offset lets the caller validate only
///          the HTTP headers and preserve any already-read frame bytes for the
///          frame parser.
/// @param data Response bytes read so far.
/// @param len Number of valid bytes in @p data.
/// @return Offset just after `\r\n\r\n`, or zero when the terminator is absent.
static size_t ws_handshake_header_end_offset(const char *data, size_t len) {
    if (!data || len < 4)
        return 0;
    for (size_t i = 3; i < len; i++) {
        if (data[i - 3] == '\r' && data[i - 2] == '\n' && data[i - 1] == '\r' && data[i] == '\n')
            return i + 1;
    }
    return 0;
}

/// @brief Perform WebSocket handshake.
/// @details Generates a fresh client key, sends a bounded HTTP Upgrade
///          request, reads through the header terminator, preserves any frame
///          bytes received in the same transport read, validates the response,
///          and stores an allocated negotiated subprotocol in @p ws.
/// @param[in,out] ws Connected TCP or TLS transport and handshake destination.
/// @param[in] host Validated, unbracketed server hostname or address literal.
/// @param[in] port Connected TCP port.
/// @param[in] path Validated origin-form request target.
/// @param[in] requested_subprotocol Optional exact protocol token to request.
/// @return 1 when the upgrade succeeds, or 0 for key generation, formatting,
///         transport, allocation, or response-validation failure.
static int ws_handshake(rt_ws_impl *ws,
                        const char *host,
                        int port,
                        const char *path,
                        const char *requested_subprotocol) {
    // Generate key and keep a copy for accept validation
    rt_string ws_key = generate_ws_key();
    if (!ws_key || !rt_string_is_handle(ws_key))
        return 0;
    const char *key_cstr = rt_string_cstr(ws_key);
    if (!key_cstr || rt_str_len(ws_key) <= 0) {
        rt_string_unref(ws_key);
        return 0;
    }

    char key_copy[64] = {0};
    if (key_cstr)
        strncpy(key_copy, key_cstr, sizeof(key_copy) - 1);

    // Build handshake request
    char request[2048];
    char host_header[512];
    int host_header_len =
        ws_format_host_header(host_header, sizeof(host_header), host, port, ws->tls != NULL);
    if (host_header_len < 0 || (size_t)host_header_len >= sizeof(host_header)) {
        rt_string_unref(ws_key);
        return 0;
    }
    int req_len = snprintf(request,
                           sizeof(request),
                           "GET %s HTTP/1.1\r\n"
                           "Host: %s\r\n"
                           "Upgrade: websocket\r\n"
                           "Connection: Upgrade\r\n"
                           "Sec-WebSocket-Key: %s\r\n"
                           "Sec-WebSocket-Version: 13\r\n",
                           path,
                           host_header,
                           key_cstr);
    if (req_len <= 0 || (size_t)req_len >= sizeof(request)) {
        rt_string_unref(ws_key);
        return 0;
    }
    if (requested_subprotocol && *requested_subprotocol) {
        int wrote = snprintf(request + req_len,
                             sizeof(request) - (size_t)req_len,
                             "Sec-WebSocket-Protocol: %s\r\n",
                             requested_subprotocol);
        if (wrote <= 0 || (size_t)wrote >= sizeof(request) - (size_t)req_len) {
            rt_string_unref(ws_key);
            return 0;
        }
        req_len += wrote;
    }
    if ((size_t)req_len + 2 >= sizeof(request)) {
        rt_string_unref(ws_key);
        return 0;
    }
    memcpy(request + req_len, "\r\n", 3);
    req_len += 2;

    rt_string_unref(ws_key);

    // Send request
    if (!ws_send_all(ws, request, (size_t)req_len))
        return 0;

    // Receive response headers
    char response[WS_MAX_HANDSHAKE_HEADER_BYTES];
    size_t total = 0;
    size_t header_end = 0;
    while (total < sizeof(response) - 1) {
        long n = ws_recv(ws, response + total, sizeof(response) - 1 - total);
        if (n <= 0)
            return 0;
        total += (size_t)n;

        header_end = ws_handshake_header_end_offset(response, total);
        if (header_end)
            break;
    }
    if (!header_end) {
        rt_trap_net("WebSocket: handshake response headers too large", Err_ProtocolError);
        return 0;
    }

    if (total > header_end) {
        size_t leftover_len = total - header_end;
        uint8_t *leftover = (uint8_t *)malloc(leftover_len);
        if (!leftover)
            return 0;
        memcpy(leftover, response + header_end, leftover_len);
        free(ws->recv_buffer);
        ws->recv_buffer = leftover;
        ws->recv_buffer_size = leftover_len;
        ws->recv_buffer_len = leftover_len;
        ws->recv_buffer_pos = 0;
    }
    response[header_end] = '\0';

    free(ws->subprotocol);
    ws->subprotocol = NULL;
    if (!ws_validate_handshake_response(
            response, key_copy, requested_subprotocol, &ws->subprotocol))
        return 0;
    return 1;
}

/// @brief Send a WebSocket frame.
/// @details Emits one final client frame with the canonical RFC 6455 payload
///          length form and a fresh unpredictable masking key. Payload bytes
///          are masked and transmitted in bounded stack chunks.
/// @param[in,out] ws Open WebSocket transport.
/// @param[in] opcode Frame opcode to place in the low four bits.
/// @param[in] data Payload bytes, or NULL when @p len is zero.
/// @param[in] len Payload size in bytes.
/// @return 1 when the complete frame is sent, or 0 on transport failure.
static int ws_send_frame(rt_ws_impl *ws, uint8_t opcode, const void *data, size_t len) {
    uint8_t header[14];
    size_t header_len = 2;

    // FIN + opcode
    header[0] = WS_FIN | opcode;

    // Mask + length
    if (len < 126) {
        header[1] = WS_MASK | (uint8_t)len;
    } else if (len < 65536) {
        header[1] = WS_MASK | 126;
        header[2] = (uint8_t)(len >> 8);
        header[3] = (uint8_t)(len);
        header_len = 4;
    } else {
        header[1] = WS_MASK | 127;
        ws_encode_u64_len(header + 2, len);
        header_len = 10;
    }

    // Generate masking key using CSPRNG (RFC 6455 §5.3 requires unpredictability)
    uint8_t mask[4];
    rt_crypto_random_bytes(mask, sizeof(mask));
    memcpy(header + header_len, mask, 4);
    header_len += 4;

    // Send header
    if (!ws_send_all(ws, header, header_len))
        return 0;

    // Mask and send data
    if (len > 0) {
        const uint8_t *src = (const uint8_t *)data;
        uint8_t chunk[4096] = {0};
        size_t offset = 0;
        while (offset < len) {
            size_t part = len - offset;
            if (part > sizeof(chunk))
                part = sizeof(chunk);
            for (size_t i = 0; i < part; i++)
                chunk[i] = src[offset + i] ^ mask[(offset + i) & 3];
            if (!ws_send_all(ws, chunk, part))
                return 0;
            offset += part;
        }
    }

    return 1;
}

/// @brief Send a close frame without allowing a trap to bypass transport cleanup.
/// @details Mask generation can trap if the platform entropy source fails. Close
///          and protocol-abort paths must still retire their uniquely owned
///          transport, so this helper catches any send-side trap, removes its
///          local recovery frame, and reports failure to the cleanup caller.
///          Ordinary socket/TLS send errors are reported through the same zero
///          result. The payload is not retained.
/// @param ws Valid WebSocket payload that still owns a transport.
/// @param payload Close-frame status/reason bytes, or NULL for an empty payload.
/// @param payload_len Number of payload bytes; RFC 6455 limits this to 125.
/// @return One when the complete masked frame was sent; zero on any failure.
static int ws_send_close_best_effort(rt_ws_impl *ws, const void *payload, size_t payload_len) {
    volatile int sent = 0;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) == 0) {
        sent = ws_send_frame(ws, WS_OP_CLOSE, payload, payload_len);
        rt_trap_clear_recovery();
    } else {
        rt_trap_clear_recovery();
    }
    return (int)sent;
}

/// @brief Send a best-effort close status and retire a failed connection.
/// @details Protocol and allocation failures cannot leave a half-parsed stream
///          reusable. When the transport remains writable, a two-byte Close
///          payload communicates the RFC 6455 status before deterministic
///          teardown. The function always marks and closes locally.
/// @param ws Valid WebSocket payload.
/// @param close_code RFC 6455 close status such as 1002, 1007, 1009, or 1011.
/// @return Always zero, allowing direct use from parser failure branches.
static int ws_abort_connection(rt_ws_impl *ws, uint16_t close_code) {
    if (!ws)
        return 0;
    uint8_t payload[2] = {(uint8_t)(close_code >> 8), (uint8_t)close_code};
    if (ws->is_open && ws->socket_fd != INVALID_SOCK)
        (void)ws_send_close_best_effort(ws, payload, sizeof(payload));
    ws->close_code = close_code;
    ws->is_open = 0;
    ws_close_transport(ws);
    return 0;
}

/// @brief Read exactly n bytes from connection.
/// @details Consumes handshake look-ahead bytes from the connection receive
///          buffer before reading the TCP or TLS transport.
/// @param[in,out] ws Open WebSocket transport.
/// @param[out] buffer Destination for exactly @p len bytes.
/// @param[in] len Number of bytes required.
/// @return 1 after filling @p buffer, or 0 on EOF or transport error.
static int ws_recv_exact(rt_ws_impl *ws, void *buffer, size_t len) {
    size_t total = 0;
    while (total < len) {
        if (ws->recv_buffer_len > 0) {
            size_t available = ws->recv_buffer_len - ws->recv_buffer_pos;
            size_t needed = len - total;
            size_t take = available < needed ? available : needed;
            memcpy((uint8_t *)buffer + total, ws->recv_buffer + ws->recv_buffer_pos, take);
            total += take;
            ws->recv_buffer_pos += take;
            if (ws->recv_buffer_pos == ws->recv_buffer_len) {
                ws->recv_buffer_pos = 0;
                ws->recv_buffer_len = 0;
            }
            continue;
        }
        long n = ws_recv(ws, (uint8_t *)buffer + total, len - total);
        if (n <= 0)
            return 0;
        total += n;
    }
    return 1;
}

/// @brief Receive a WebSocket frame.
/// @details Rejects reserved bits, invalid opcodes, masked server frames,
///          non-canonical length encodings, fragmented or oversized control
///          frames, and payloads above the allocation cap. Protocol violations
///          abort the connection with the appropriate close status.
/// @param[in,out] ws Open WebSocket transport.
/// @param[out] fin_out Receives 1 if this is the final fragment.
/// @param[out] opcode_out Receives the validated frame opcode.
/// @param[out] data_out Receives an allocated payload, or NULL for an empty payload.
/// @param[out] len_out Receives the payload length.
/// @return 1 on success, 0 on error.
static int ws_recv_frame(
    rt_ws_impl *ws, uint8_t *fin_out, uint8_t *opcode_out, uint8_t **data_out, size_t *len_out) {
    uint8_t header[2];
    if (!ws_recv_exact(ws, header, 2))
        return 0;

    if ((header[0] & 0x70) != 0) {
        return ws_abort_connection(ws, WS_CLOSE_PROTOCOL_ERROR);
    }

    *fin_out = (header[0] & WS_FIN) ? 1 : 0; // H-11: expose FIN bit for reassembly
    *opcode_out = header[0] & 0x0F;
    uint8_t masked = header[1] & WS_MASK;
    size_t payload_len = header[1] & 0x7F;

    if (!ws_is_valid_opcode(*opcode_out)) {
        return ws_abort_connection(ws, WS_CLOSE_PROTOCOL_ERROR);
    }

    // Extended payload length
    if (payload_len == 126) {
        uint8_t ext[2];
        if (!ws_recv_exact(ws, ext, 2))
            return 0;
        payload_len = ((size_t)ext[0] << 8) | ext[1];
        if (payload_len < 126)
            return ws_abort_connection(ws, WS_CLOSE_PROTOCOL_ERROR);
    } else if (payload_len == 127) {
        uint8_t ext[8];
        if (!ws_recv_exact(ws, ext, 8))
            return 0;
        if ((ext[0] & 0x80u) != 0 || !ws_decode_u64_len(ext, &payload_len))
            return ws_abort_connection(ws, WS_CLOSE_PROTOCOL_ERROR);
        if (payload_len < 65536)
            return ws_abort_connection(ws, WS_CLOSE_PROTOCOL_ERROR);
    }

    // M-9: RFC 6455 §5.1 — client MUST close connection if server sends a masked frame.
    // Consume the mask key first so a zero-length malformed frame does not leave
    // unread transport bytes that turn the protocol close into a TCP reset.
    if (masked) {
        uint8_t mask_key[4];
        if (!ws_recv_exact(ws, mask_key, sizeof(mask_key)))
            return 0;
        return ws_abort_connection(ws, WS_CLOSE_PROTOCOL_ERROR);
    }

    if (*opcode_out >= 0x08 && (!*fin_out || payload_len > 125))
        return ws_abort_connection(ws, WS_CLOSE_PROTOCOL_ERROR);

    /* Reject server-controlled allocation larger than 64 MB (S-10 fix).
       This prevents a malicious server from causing malloc(huge). */
#define WS_MAX_PAYLOAD (64u * 1024u * 1024u)
    if (payload_len > WS_MAX_PAYLOAD) {
        return ws_abort_connection(ws, WS_CLOSE_MESSAGE_TOO_BIG);
    }
#undef WS_MAX_PAYLOAD

    // Payload
    *data_out = NULL;
    *len_out = payload_len;
    if (payload_len > 0) {
        *data_out = malloc(payload_len);
        if (!*data_out)
            return ws_abort_connection(ws, WS_CLOSE_INTERNAL_ERROR);
        if (!ws_recv_exact(ws, *data_out, payload_len)) {
            free(*data_out);
            *data_out = NULL;
            return 0;
        }
    }

    return 1;
}

/// @brief Forward declaration for the control-frame handler (defined below).
static void ws_handle_control(rt_ws_impl *ws, uint8_t opcode, uint8_t *data, size_t len);

/// @brief Read a complete WebSocket message, reassembling fragments.
///
/// Loops over `ws_recv_frame`, transparently handling:
///   - Control frames (ping/pong/close) — dispatched to
///     `ws_handle_control` and not returned to the caller.
///   - Continuation frames — concatenated into a growing buffer.
///   - First data frame with `fin=1` — returned immediately.
///   - Reassembly cap (`WS_MAX_REASSEMBLY_SIZE`, 64 MB) — closes
///     with code 1009 (message too big).
///   - Out-of-order frames (e.g. continuation without an opener)
///     — closes with code 1002 (protocol error).
///   - Text payloads — final UTF-8 validation; closes with 1007
///     (invalid data) on failure.
///
/// On success `*data_out` is a heap allocation owned by the caller
/// (must be `free`d) and `*opcode_out` is `WS_OP_TEXT` or `WS_OP_BINARY`.
/// @param[in,out] ws Open WebSocket connection.
/// @param[out] data_out Receives an allocated complete message payload.
/// @param[out] len_out Receives the complete payload size in bytes.
/// @param[out] opcode_out Receives WS_OP_TEXT or WS_OP_BINARY.
/// @return 1 on a complete message, 0 on close/error.
static int ws_recv_message(rt_ws_impl *ws,
                           uint8_t **data_out,
                           size_t *len_out,
                           uint8_t *opcode_out) {
    uint8_t *frag_buf = NULL;
    size_t frag_len = 0;
    uint8_t frag_opcode = 0;
    int frag_active = 0;

    *data_out = NULL;
    *len_out = 0;
    *opcode_out = 0;

    while (ws->is_open) {
        uint8_t fin = 0;
        uint8_t opcode = 0;
        uint8_t *data = NULL;
        size_t len = 0;

        if (!ws_recv_frame(ws, &fin, &opcode, &data, &len)) {
            free(frag_buf);
            if (ws->close_code == 0)
                ws->close_code = WS_CLOSE_ABNORMAL;
            ws->is_open = 0;
            ws_close_transport(ws);
            return 0;
        }

        if (opcode >= 0x08) {
            ws_handle_control(ws, opcode, data, len);
            free(data);
            continue;
        }

        if (opcode == WS_OP_CONTINUATION) {
            if (!frag_active) {
                free(data);
                free(frag_buf);
                return ws_abort_connection(ws, WS_CLOSE_PROTOCOL_ERROR);
            }
        } else if (opcode == WS_OP_TEXT || opcode == WS_OP_BINARY) {
            if (frag_active) {
                free(data);
                free(frag_buf);
                return ws_abort_connection(ws, WS_CLOSE_PROTOCOL_ERROR);
            }

            if (fin) {
                if (opcode == WS_OP_TEXT && !ws_is_valid_utf8(data, len)) {
                    free(data);
                    rt_ws_close_with(ws, WS_CLOSE_INVALID_DATA, rt_str_empty());
                    return 0;
                }
                *data_out = data;
                *len_out = len;
                *opcode_out = opcode;
                return 1;
            }

            frag_active = 1;
            frag_opcode = opcode;
        } else {
            free(data);
            free(frag_buf);
            return ws_abort_connection(ws, WS_CLOSE_PROTOCOL_ERROR);
        }

        if (len > 0) {
            if (frag_len > WS_MAX_REASSEMBLY_SIZE || len > WS_MAX_REASSEMBLY_SIZE - frag_len) {
                free(data);
                free(frag_buf);
                rt_ws_close_with(ws, WS_CLOSE_MESSAGE_TOO_BIG, rt_str_empty());
                return 0;
            }

            uint8_t *new_buf = (uint8_t *)realloc(frag_buf, frag_len + len);
            if (!new_buf) {
                free(data);
                free(frag_buf);
                return ws_abort_connection(ws, WS_CLOSE_INTERNAL_ERROR);
            }
            frag_buf = new_buf;
            memcpy(frag_buf + frag_len, data, len);
            frag_len += len;
        }
        free(data);

        if (fin) {
            if (frag_opcode == WS_OP_TEXT && !ws_is_valid_utf8(frag_buf, frag_len)) {
                free(frag_buf);
                rt_ws_close_with(ws, WS_CLOSE_INVALID_DATA, rt_str_empty());
                return 0;
            }
            *data_out = frag_buf;
            *len_out = frag_len;
            *opcode_out = frag_opcode;
            return 1;
        }
    }

    free(frag_buf);
    return 0;
}

/// @brief Handle one validated WebSocket control frame.
/// @details Ping payloads are echoed as Pong, Pong frames are ignored, and
///          Close frames validate the optional status and UTF-8 reason before
///          recording peer state, replying, and retiring the transport.
/// @param[in,out] ws Open WebSocket connection whose state may be closed.
/// @param[in] opcode WS_OP_PING, WS_OP_PONG, or WS_OP_CLOSE.
/// @param[in] data Borrowed control-frame payload.
/// @param[in] len Payload length, already bounded to 125 bytes.
static void ws_handle_control(rt_ws_impl *ws, uint8_t opcode, uint8_t *data, size_t len) {
    switch (opcode) {
        case WS_OP_PING:
            // Respond with pong
            if (!ws_send_frame(ws, WS_OP_PONG, data, len)) {
                ws->is_open = 0;
                ws->close_code = WS_CLOSE_ABNORMAL;
            }
            break;

        case WS_OP_PONG:
            // Ignore pongs
            break;

        case WS_OP_CLOSE:
            // Parse close code and reason
            ws->is_open = 0;
            if (len == 1) {
                ws->close_code = WS_CLOSE_PROTOCOL_ERROR;
                if (!ws_send_close_best_effort(ws, "\x03\xEA", 2))
                    ws->close_code = WS_CLOSE_ABNORMAL;
                ws_close_transport(ws);
                break;
            }
            if (len >= 2) {
                uint16_t code = ((uint16_t)data[0] << 8) | data[1];
                if (!ws_close_code_is_valid(code)) {
                    ws->close_code = WS_CLOSE_PROTOCOL_ERROR;
                    if (!ws_send_close_best_effort(ws, "\x03\xEA", 2))
                        ws->close_code = WS_CLOSE_ABNORMAL;
                    ws_close_transport(ws);
                    break;
                }
                if (len > 2 && !ws_is_valid_utf8(data + 2, len - 2)) {
                    ws->close_code = WS_CLOSE_INVALID_DATA;
                    if (!ws_send_close_best_effort(ws, "\x03\xEF", 2))
                        ws->close_code = WS_CLOSE_ABNORMAL;
                    ws_close_transport(ws);
                    break;
                }
                ws->close_code = code;
                free(ws->close_reason);
                ws->close_reason = NULL;
                ws->close_reason_len = 0;
                if (len > 2) {
                    ws->close_reason = malloc(len - 1);
                    if (ws->close_reason) {
                        memcpy(ws->close_reason, data + 2, len - 2);
                        ws->close_reason[len - 2] = '\0';
                        ws->close_reason_len = len - 2;
                    }
                }
            } else {
                ws->close_code = WS_CLOSE_NO_STATUS;
            }
            // Send close response
            if (!ws_send_close_best_effort(ws, data, len))
                ws->close_code = WS_CLOSE_ABNORMAL;
            ws_close_transport(ws);
            break;
    }
}

/// @brief Deterministically close the TCP/TLS transport. Idempotent: leaves
///        `tls == NULL` and `socket_fd == INVALID_SOCK` so later calls (including the
///        GC finalizer) no-op.
/// @param[in,out] ws WebSocket payload, or NULL.
static void ws_close_transport(rt_ws_impl *ws) {
    if (!ws)
        return;
    if (ws->tls) {
        rt_tls_close(ws->tls);
        ws->tls = NULL;
        ws->socket_fd = INVALID_SOCK;
    } else if (ws->socket_fd != INVALID_SOCK) {
        CLOSE_SOCKET(ws->socket_fd);
        ws->socket_fd = INVALID_SOCK;
    }
}

/// @brief Finalizer for WebSocket connections.
/// @details Idempotently retires the transport and releases every native
///          allocation owned by a partially or fully initialized payload.
/// @param[in,out] obj WebSocket payload supplied by the managed object runtime.
static void rt_ws_finalize(void *obj) {
    if (!obj)
        return;
    rt_ws_impl *ws = obj;
    ws_close_transport(ws);
    free(ws->url);
    free(ws->subprotocol);
    free(ws->close_reason);
    free(ws->recv_buffer);
    ws->url = NULL;
    ws->subprotocol = NULL;
    ws->close_reason = NULL;
    ws->close_reason_len = 0;
    ws->recv_buffer = NULL;
    ws->recv_buffer_size = 0;
    ws->recv_buffer_len = 0;
    ws->recv_buffer_pos = 0;
}

/// @brief Connect to a WebSocket URL with a 30-second default timeout.
/// @param[in] url Managed `ws://` or `wss://` URL.
/// @return Caller-owned managed WebSocket handle, or NULL after a returning trap hook.
/// @see rt_ws_connect_for
void *rt_ws_connect(rt_string url) {
    return rt_ws_connect_for_protocol(url, 30000, NULL); // 30 second default timeout
}

/// @brief Connect with the default timeout and request one WebSocket subprotocol.
/// @param[in] url Managed `ws://` or `wss://` URL.
/// @param[in] subprotocol Managed RFC token to send in Sec-WebSocket-Protocol.
/// @return Caller-owned managed WebSocket handle, or NULL after a returning trap hook.
void *rt_ws_connect_protocol(rt_string url, rt_string subprotocol) {
    return rt_ws_connect_for_protocol(url, 30000, subprotocol);
}

/// @brief Connect to a WebSocket URL (`ws://` or `wss://`) with explicit timeout.
///
/// Resolves the host, opens a TCP (or TLS for `wss://`) connection,
/// and performs the WebSocket handshake (RFC 6455 §4): generates a
/// random 16-byte key, sends an HTTP/1.1 Upgrade request, and
/// validates the server's `Sec-WebSocket-Accept` against the
/// expected `base64(SHA1(key + WS_MAGIC))`. On success returns a
/// GC-managed `rt_ws_impl` with `is_open == 1`.
/// @param[in] url Managed `ws://` or `wss://` URL.
/// @param[in] timeout_ms Nonnegative connect and per-I/O timeout in milliseconds.
/// @return Caller-owned managed WebSocket handle, or NULL after a returning trap hook.
/// @throws Err_InvalidUrl on bad URL,
///         generic trap on connect/handshake failure or NULL URL.
void *rt_ws_connect_for(rt_string url, int64_t timeout_ms) {
    return rt_ws_connect_for_protocol(url, timeout_ms, NULL);
}

/// @brief Release one caller-owned managed WebSocket reference.
/// @param object Managed object, or NULL.
static void ws_release_managed(void *object) {
    if (object && rt_obj_release_check0(object))
        rt_obj_free(object);
}

/// @brief Copy the current trap diagnostic before connection cleanup.
/// @param output Destination buffer.
/// @param capacity Destination capacity including its terminator.
/// @param fallback Message used when no active trap text exists.
static void ws_save_trap(char *output, size_t capacity, const char *fallback) {
    if (!output || capacity == 0)
        return;
    const char *error = rt_trap_get_error();
    snprintf(output, capacity, "%s", error && error[0] ? error : fallback);
}

/// @brief Connect and publish one stable WebSocket object transactionally.
/// @details URL parsing stages native host/path storage first. All subsequent
///          managed allocation, socket, TLS, and HTTP-upgrade work executes
///          under one recovery frame so a trap releases the partial object and
///          its uniquely owned transport before preserving the original
///          categorized diagnostic.
/// @param url Valid managed `ws://` or `wss://` URL String.
/// @param timeout_ms Overall resolved-address connect and per-I/O timeout.
/// @param subprotocol Optional single managed protocol token.
/// @return Caller-owned stable WebSocket handle, or NULL after a returning trap hook.
void *rt_ws_connect_for_protocol(rt_string url, int64_t timeout_ms, rt_string subprotocol) {
    const char *url_cstr = NULL;
    const char *protocol_cstr = NULL;
    size_t url_len = 0;
    size_t protocol_len = 0;
    int timeout_int = 0;
    if (!ws_string_bytes(url, &url_cstr, &url_len, "WebSocket: NULL URL"))
        return NULL;
    if (url_len == 0 || ws_has_embedded_nul(url_cstr, url_len)) {
        rt_trap("WebSocket: invalid URL");
        return NULL;
    }
    if (subprotocol &&
        !ws_string_bytes(subprotocol, &protocol_cstr, &protocol_len, "WebSocket: NULL subprotocol"))
        return NULL;
    if (protocol_cstr && ws_has_embedded_nul(protocol_cstr, protocol_len)) {
        rt_trap("WebSocket: invalid subprotocol");
        return NULL;
    }
    if (!ws_timeout_ms_to_int(timeout_ms, &timeout_int)) {
        rt_trap("WebSocket: invalid timeout");
        return NULL;
    }
    if (protocol_cstr && *protocol_cstr && !ws_token_is_valid(protocol_cstr)) {
        rt_trap("WebSocket: invalid subprotocol");
        return NULL;
    }

    int is_secure;
    char *host = NULL;
    int port;
    char *path = NULL;

    if (!parse_ws_url(url_cstr, &is_secure, &host, &port, &path)) {
        rt_trap_net("WebSocket: invalid URL", Err_InvalidUrl);
        return NULL;
    }

    char *volatile owned_host = host;
    char *volatile owned_path = path;
    rt_ws_impl *volatile ws = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[512];
        int saved_net_code = rt_trap_get_net_code();
        ws_save_trap(saved_error, sizeof(saved_error), "WebSocket: connection failed");
        rt_trap_clear_recovery();
        free((void *)owned_path);
        free((void *)owned_host);
        ws_release_managed((void *)ws);
        if (saved_net_code != 0)
            rt_trap_net(saved_error, saved_net_code);
        else
            rt_trap(saved_error);
        return NULL;
    }

    ws = (rt_ws_impl *)rt_obj_new_i64(RT_WEBSOCKET_CLASS_ID, (int64_t)sizeof(rt_ws_impl));
    if (!ws) {
        rt_trap("WebSocket: object allocation failed");
        rt_trap_clear_recovery();
        free((void *)owned_path);
        free((void *)owned_host);
        return NULL;
    }
    memset((void *)ws, 0, sizeof(rt_ws_impl));
    ((rt_ws_impl *)ws)->socket_fd = INVALID_SOCK;
    rt_obj_set_finalizer((void *)ws, rt_ws_finalize);
    ((rt_ws_impl *)ws)->url = strdup(url_cstr);
    if (!((rt_ws_impl *)ws)->url)
        rt_trap("WebSocket: URL allocation failed");
    ((rt_ws_impl *)ws)->timeout_ms = timeout_int;

    // Connect TCP (with timeout)
    ((rt_ws_impl *)ws)->socket_fd = create_tcp_socket((const char *)owned_host, port, timeout_int);
    if (((rt_ws_impl *)ws)->socket_fd == INVALID_SOCK)
        rt_trap_net("WebSocket: connection failed", Err_NetworkError);

    // Set socket-level recv/send timeout for handshake phase
    if (timeout_ms > 0) {
        if (!set_socket_timeout(((rt_ws_impl *)ws)->socket_fd, timeout_int, true) ||
            !set_socket_timeout(((rt_ws_impl *)ws)->socket_fd, timeout_int, false))
            rt_trap_net("WebSocket: failed to configure socket timeout", Err_NetworkError);
    }

    // TLS handshake if secure
    if (is_secure) {
        rt_tls_config_t config;
        rt_tls_config_init(&config);
        config.hostname = (const char *)owned_host;
        config.alpn_protocol = "http/1.1";
        config.timeout_ms = timeout_int;

        ((rt_ws_impl *)ws)->tls = rt_tls_new((intptr_t)((rt_ws_impl *)ws)->socket_fd, &config);
        if (!((rt_ws_impl *)ws)->tls) {
            const char *detail = rt_tls_last_error();
            char message[512];
            if (detail && *detail) {
                snprintf(message, sizeof(message), "WebSocket: TLS setup failed: %s", detail);
            } else {
                snprintf(message, sizeof(message), "%s", "WebSocket: TLS setup failed");
            }
            rt_trap_net(message, Err_TlsError);
        }

        if (rt_tls_handshake(((rt_ws_impl *)ws)->tls) != RT_TLS_OK) {
            const char *detail = rt_tls_get_error(((rt_ws_impl *)ws)->tls);
            char message[512];
            if (detail && *detail) {
                snprintf(message, sizeof(message), "WebSocket: TLS handshake failed: %s", detail);
            } else {
                snprintf(message, sizeof(message), "%s", "WebSocket: TLS handshake failed");
            }
            rt_trap_net(message, Err_TlsError);
        }
    }

    // WebSocket handshake
    if (!ws_handshake((rt_ws_impl *)ws,
                      (const char *)owned_host,
                      port,
                      (const char *)owned_path,
                      protocol_cstr))
        rt_trap_net("WebSocket: handshake failed", Err_ProtocolError);

    free((void *)owned_host);
    free((void *)owned_path);
    owned_host = NULL;
    owned_path = NULL;

    ((rt_ws_impl *)ws)->is_open = 1;
    rt_trap_clear_recovery();
    return (void *)ws;
}

/// @brief Return the URL with which the connection was opened.
/// @param[in] obj Managed WebSocket receiver, or NULL.
/// @return Caller-owned managed URL string; empty for NULL or a payload without a URL,
///         and NULL after an invalid-object trap.
rt_string rt_ws_url(void *obj) {
    if (!obj)
        return rt_str_empty();
    rt_ws_impl *ws = ws_require(obj, "WebSocket.Url: invalid object");
    if (!ws)
        return NULL;
    if (!ws->url)
        return rt_str_empty();
    return rt_string_from_bytes(ws->url, strlen(ws->url));
}

/// @brief True (1) if the connection is still established, false (0) once closed/error.
/// @param[in] obj Managed WebSocket receiver, or NULL.
/// @return 1 while open; otherwise 0, including NULL or invalid receivers.
int8_t rt_ws_is_open(void *obj) {
    if (!obj)
        return 0;
    rt_ws_impl *ws = ws_require(obj, "WebSocket.IsOpen: invalid object");
    if (!ws)
        return 0;
    return ws->is_open;
}

/// @brief WebSocket close code (1000-4999 per RFC 6455 §7.4). 0 if still open.
/// @param[in] obj Managed WebSocket receiver, or NULL.
/// @return Recorded close status, or 0 when none is available.
int64_t rt_ws_close_code(void *obj) {
    if (!obj)
        return 0;
    rt_ws_impl *ws = ws_require(obj, "WebSocket.CloseCode: invalid object");
    if (!ws)
        return 0;
    return ws->close_code;
}

/// @brief Optional close reason text supplied by the peer. Empty string when none.
/// @param[in] obj Managed WebSocket receiver, or NULL.
/// @return Caller-owned managed reason string; empty when absent, or NULL after
///         an invalid-object trap.
rt_string rt_ws_close_reason(void *obj) {
    if (!obj)
        return rt_str_empty();
    rt_ws_impl *ws = ws_require(obj, "WebSocket.CloseReason: invalid object");
    if (!ws)
        return NULL;
    if (!ws->close_reason)
        return rt_str_empty();
    return rt_string_from_bytes(ws->close_reason, ws->close_reason_len);
}

/// @brief Return the subprotocol selected during the opening handshake.
/// @param[in] obj Managed WebSocket receiver, or NULL.
/// @return Caller-owned managed protocol string; empty when none was selected,
///         or NULL after an invalid-object trap.
rt_string rt_ws_subprotocol(void *obj) {
    if (!obj)
        return rt_str_empty();
    rt_ws_impl *ws = ws_require(obj, "WebSocket.Subprotocol: invalid object");
    if (!ws)
        return NULL;
    if (!ws->subprotocol)
        return rt_str_empty();
    return rt_string_from_bytes(ws->subprotocol, strlen(ws->subprotocol));
}

/// @brief Send a text frame. Rejects invalid UTF-8 payloads before they hit the wire.
/// @param[in] obj Managed WebSocket receiver; NULL is a no-op.
/// @param[in] text Managed UTF-8 String payload; NULL sends an empty text frame.
/// @throws Err_ConnectionClosed if `is_open == 0`,
///         Err_NetworkError if the underlying send fails.
void rt_ws_send(void *obj, rt_string text) {
    if (!obj)
        return;
    rt_ws_impl *ws = ws_require(obj, "WebSocket.Send: invalid object");
    if (!ws)
        return;
    if (!ws->is_open) {
        rt_trap_net("WebSocket: connection is closed", Err_ConnectionClosed);
        return;
    }

    if (text && !rt_string_is_handle(text)) {
        rt_trap("WebSocket.Send: invalid String");
        return;
    }
    const char *cstr = text ? rt_string_cstr(text) : NULL;
    int64_t len64 = text ? rt_str_len(text) : 0;
    if (len64 < 0 || (uint64_t)len64 > (uint64_t)SIZE_MAX) {
        rt_trap("WebSocket: invalid text length");
        return;
    }
    size_t len = (size_t)len64;
    if (!cstr && len > 0) {
        rt_trap("WebSocket: invalid text");
        return;
    }

    if (cstr && !ws_is_valid_utf8((const uint8_t *)cstr, len)) {
        rt_trap_net("WebSocket: invalid UTF-8 text frame", Err_ProtocolError);
        return;
    }

    if (!ws_send_frame(ws, WS_OP_TEXT, cstr, len)) {
        ws->is_open = 0;
        ws_close_transport(ws);
        rt_trap_net("WebSocket: send failed", Err_NetworkError);
    }
}

/// @brief Send a binary frame containing the bytes of `data`.
/// @param[in] obj Managed WebSocket receiver; NULL is a no-op.
/// @param[in] data Managed Bytes payload, including an empty Bytes object.
/// @throws Err_ConnectionClosed if closed, Err_NetworkError on send failure.
void rt_ws_send_bytes(void *obj, void *data) {
    if (!obj)
        return;
    rt_ws_impl *ws = ws_require(obj, "WebSocket.SendBytes: invalid object");
    if (!ws)
        return;
    if (!ws->is_open) {
        rt_trap_net("WebSocket: connection is closed", Err_ConnectionClosed);
        return;
    }
    if (!data || !rt_bytes_is_bytes(data)) {
        rt_trap("WebSocket: NULL bytes");
        return;
    }

    int64_t len = rt_bytes_len(data);
    if (len < 0 || (uint64_t)len > (uint64_t)SIZE_MAX) {
        rt_trap("WebSocket: invalid bytes length");
        return;
    }
    const uint8_t *buffer = len > 0 ? rt_bytes_data_const(data) : NULL;
    if (len > 0 && !buffer) {
        rt_trap("WebSocket: invalid Bytes storage");
        return;
    }
    if (!ws_send_frame(ws, WS_OP_BINARY, buffer, (size_t)len)) {
        ws->is_open = 0;
        ws_close_transport(ws);
        rt_trap_net("WebSocket: send failed", Err_NetworkError);
        return;
    }
}

/// @brief Send an empty PING control frame to keep the connection alive.
///
/// Silently no-ops on a closed connection. The peer's PONG (if any)
/// is consumed transparently inside `ws_recv_message`.
/// @param[in] obj Managed WebSocket receiver; NULL is a no-op.
void rt_ws_ping(void *obj) {
    if (!obj)
        return;
    rt_ws_impl *ws = ws_require(obj, "WebSocket.Ping: invalid object");
    if (!ws)
        return;
    if (!ws->is_open)
        return;

    if (!ws_send_frame(ws, WS_OP_PING, NULL, 0)) {
        ws->is_open = 0;
        ws->close_code = WS_CLOSE_ABNORMAL;
        ws_close_transport(ws);
    }
}

/// @brief Publish an owned native message as one exact managed String.
/// @details Managed allocation runs under a local recovery frame. The native
///          frame/reassembly buffer is freed before any allocation trap is
///          re-raised, so a completed receive cannot leak on OOM.
/// @param message Native buffer owned by this helper; NULL is valid for zero length.
/// @param message_len Exact byte count, including embedded NUL bytes.
/// @return Caller-owned String, or NULL after a returning trap hook.
static rt_string ws_string_from_owned_message(uint8_t *message, size_t message_len) {
    rt_string volatile result = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[512];
        ws_save_trap(saved_error, sizeof(saved_error), "WebSocket: String allocation failed");
        rt_trap_clear_recovery();
        free(message);
        rt_trap(saved_error);
        return NULL;
    }
    result = rt_string_from_bytes((const char *)message, message_len);
    rt_trap_clear_recovery();
    free(message);
    return (rt_string)result;
}

/// @brief Publish an owned native message as one exact managed Bytes object.
/// @details The native frame/reassembly buffer is released on success, a
///          returning allocation failure, and a non-local allocation trap.
///          `rt_bytes_from_raw` performs one contiguous copy rather than
///          per-element setters.
/// @param message Native buffer owned by this helper; NULL is valid for zero length.
/// @param message_len Exact byte count.
/// @return Caller-owned Bytes, or NULL after a returning trap hook.
static void *ws_bytes_from_owned_message(uint8_t *message, size_t message_len) {
    void *volatile result = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[512];
        ws_save_trap(saved_error, sizeof(saved_error), "WebSocket: Bytes allocation failed");
        rt_trap_clear_recovery();
        free(message);
        rt_trap(saved_error);
        return NULL;
    }
    result = rt_bytes_from_raw(message, message_len);
    rt_trap_clear_recovery();
    free(message);
    return (void *)result;
}

/// @brief Block until a complete message arrives and return it as a string.
/// @details Returns the payload bytes as one exact managed String, regardless
///          of whether the frame opcode was text or binary. Text payloads have
///          already passed UTF-8 validation; binary bytes are copied verbatim.
/// @param[in] obj Managed WebSocket receiver, or NULL.
/// @return Caller-owned managed message String, an empty String on close or
///         receive error, or NULL after an invalid-object or allocation trap.
rt_string rt_ws_recv(void *obj) {
    if (!obj)
        return rt_str_empty();
    rt_ws_impl *ws = ws_require(obj, "WebSocket.Recv: invalid object");
    if (!ws)
        return NULL;
    uint8_t *message = NULL;
    size_t message_len = 0;
    uint8_t opcode = 0;
    if (!ws_recv_message(ws, &message, &message_len, &opcode))
        return rt_str_empty();
    (void)opcode;
    return ws_string_from_owned_message(message, message_len);
}

/// @brief Receive a message with an upper bound on wait time (returns NULL on timeout).
///
/// Probes the TLS read buffer first because `select()` only sees
/// the raw socket — already-decrypted bytes wouldn't show up.
/// Then waits for socket readiness and temporarily applies the requested
/// receive timeout before delegating to rt_ws_recv. A zero timeout retains the
/// transport's current blocking behavior.
/// @param[in] obj Managed WebSocket receiver.
/// @param[in] timeout_ms Nonnegative timeout in milliseconds.
/// @return Caller-owned managed String, or NULL when closed, timed out, invalid,
///         or after a returning trap hook.
rt_string rt_ws_recv_for(void *obj, int64_t timeout_ms) {
    if (!obj)
        return NULL;
    rt_ws_impl *ws = ws_require(obj, "WebSocket.RecvFor: invalid object");
    if (!ws)
        return NULL;
    int timeout_int = 0;
    if (!ws->is_open)
        return NULL;
    if (!ws_timeout_ms_to_int(timeout_ms, &timeout_int)) {
        rt_trap("WebSocket: invalid timeout");
        return NULL;
    }

    // Wait for data to arrive with timeout.
    // Check bytes retained after the HTTP upgrade and the TLS record buffer
    // first: readiness on the raw socket cannot see either source.
    if (timeout_ms > 0) {
        if (ws->recv_buffer_len > ws->recv_buffer_pos) {
            // Upgrade read already captured frame bytes — skip readiness wait.
        } else if (ws->tls && rt_tls_has_buffered_data(ws->tls)) {
            // Data already available in TLS buffer — skip readiness wait.
        } else {
            int ready = ws_wait_socket(ws->socket_fd, timeout_int, 0);
            if (ready == 0)
                return NULL;
            if (ready < 0) {
                ws->is_open = 0;
                ws->close_code = WS_CLOSE_ABNORMAL;
                ws_close_transport(ws);
                return NULL;
            }
        }
    }

    if (timeout_ms > 0 && !ws_set_recv_timeout(ws, timeout_int)) {
        rt_trap_net("WebSocket: failed to configure receive timeout", Err_NetworkError);
        return NULL;
    }
    rt_string volatile result = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[512];
        int saved_net_code = rt_trap_get_net_code();
        ws_save_trap(saved_error, sizeof(saved_error), "WebSocket: timed receive failed");
        rt_trap_clear_recovery();
        if (timeout_ms > 0 && ws->socket_fd != INVALID_SOCK &&
            !ws_set_recv_timeout(ws, ws->timeout_ms)) {
            ws->is_open = 0;
            ws->close_code = WS_CLOSE_ABNORMAL;
            ws_close_transport(ws);
        }
        if (saved_net_code != 0)
            rt_trap_net(saved_error, saved_net_code);
        else
            rt_trap(saved_error);
        return NULL;
    }
    result = rt_ws_recv(obj);
    rt_trap_clear_recovery();
    if (timeout_ms > 0 && ws->socket_fd != INVALID_SOCK &&
        !ws_set_recv_timeout(ws, ws->timeout_ms)) {
        ws_release_managed((void *)result);
        ws->is_open = 0;
        ws->close_code = WS_CLOSE_ABNORMAL;
        ws_close_transport(ws);
        rt_trap_net("WebSocket: failed to restore receive timeout", Err_NetworkError);
        return NULL;
    }
    if (result && rt_str_len(result) == 0 && !ws->is_open) {
        rt_string_unref((rt_string)result);
        return NULL;
    }
    return (rt_string)result;
}

/// @brief Block for a complete message and return its raw bytes.
/// @param[in] obj Managed WebSocket receiver, or NULL.
/// @return Caller-owned managed Bytes object, an empty Bytes object on close
///         or receive error, or NULL after an invalid-object or allocation trap.
void *rt_ws_recv_bytes(void *obj) {
    if (!obj)
        return rt_bytes_new(0);
    rt_ws_impl *ws = ws_require(obj, "WebSocket.RecvBytes: invalid object");
    if (!ws)
        return NULL;
    uint8_t *message = NULL;
    size_t message_len = 0;
    uint8_t opcode = 0;
    if (!ws_recv_message(ws, &message, &message_len, &opcode))
        return rt_bytes_new(0);
    (void)opcode;
    return ws_bytes_from_owned_message(message, message_len);
}

/// @brief Receive a binary message with a timeout (NULL on timeout).
/// @details The next complete text or binary message is returned as exact raw
///          bytes. Buffered TLS and handshake look-ahead data bypass the socket
///          readiness wait. A zero timeout retains current blocking behavior.
/// @param[in] obj Managed WebSocket receiver.
/// @param[in] timeout_ms Nonnegative timeout in milliseconds.
/// @return Caller-owned managed Bytes object, or NULL when closed, timed out,
///         invalid, or after a returning trap hook.
/// @see rt_ws_recv_for
void *rt_ws_recv_bytes_for(void *obj, int64_t timeout_ms) {
    if (!obj)
        return NULL;
    rt_ws_impl *ws = ws_require(obj, "WebSocket.RecvBytesFor: invalid object");
    if (!ws)
        return NULL;
    int timeout_int = 0;
    if (!ws->is_open)
        return NULL;
    if (!ws_timeout_ms_to_int(timeout_ms, &timeout_int)) {
        rt_trap("WebSocket: invalid timeout");
        return NULL;
    }

    // Wait for data to arrive with timeout.
    // Check bytes retained after the HTTP upgrade and the TLS record buffer
    // first: readiness on the raw socket cannot see either source.
    if (timeout_ms > 0) {
        if (ws->recv_buffer_len > ws->recv_buffer_pos) {
            // Upgrade read already captured frame bytes — skip readiness wait.
        } else if (ws->tls && rt_tls_has_buffered_data(ws->tls)) {
            // Data already available in TLS buffer — skip readiness wait.
        } else {
            int ready = ws_wait_socket(ws->socket_fd, timeout_int, 0);
            if (ready == 0)
                return NULL;
            if (ready < 0) {
                ws->is_open = 0;
                ws->close_code = WS_CLOSE_ABNORMAL;
                ws_close_transport(ws);
                return NULL;
            }
        }
    }

    if (timeout_ms > 0 && !ws_set_recv_timeout(ws, timeout_int)) {
        rt_trap_net("WebSocket: failed to configure receive timeout", Err_NetworkError);
        return NULL;
    }
    void *volatile result = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[512];
        int saved_net_code = rt_trap_get_net_code();
        ws_save_trap(saved_error, sizeof(saved_error), "WebSocket: timed Bytes receive failed");
        rt_trap_clear_recovery();
        if (timeout_ms > 0 && ws->socket_fd != INVALID_SOCK &&
            !ws_set_recv_timeout(ws, ws->timeout_ms)) {
            ws->is_open = 0;
            ws->close_code = WS_CLOSE_ABNORMAL;
            ws_close_transport(ws);
        }
        if (saved_net_code != 0)
            rt_trap_net(saved_error, saved_net_code);
        else
            rt_trap(saved_error);
        return NULL;
    }
    result = rt_ws_recv_bytes(obj);
    rt_trap_clear_recovery();
    if (timeout_ms > 0 && ws->socket_fd != INVALID_SOCK &&
        !ws_set_recv_timeout(ws, ws->timeout_ms)) {
        ws_release_managed((void *)result);
        ws->is_open = 0;
        ws->close_code = WS_CLOSE_ABNORMAL;
        ws_close_transport(ws);
        rt_trap_net("WebSocket: failed to restore receive timeout", Err_NetworkError);
        return NULL;
    }
    if (result && rt_bytes_len(result) == 0 && !ws->is_open) {
        ws_release_managed((void *)result);
        return NULL;
    }
    return (void *)result;
}

/// @brief Send a normal close (code 1000) and mark the connection closed.
/// @param[in] obj Managed WebSocket receiver; NULL is a no-op.
/// @see rt_ws_close_with
void rt_ws_close(void *obj) {
    rt_ws_close_with(obj, WS_CLOSE_NORMAL, rt_str_empty());
}

/// @brief Send a close frame with a specific code and optional reason text.
///
/// Builds a close payload of `[code:u16-be][reason]`, hands it to
/// `ws_send_frame`, clears `is_open`, then completes the RFC 6455 §7.1.1
/// closing handshake: waits (bounded, ~1s) for the peer's close reply,
/// draining any in-flight data frames, and deterministically closes the
/// TCP/TLS transport. Silently no-ops on NULL or already-closed
/// connections. The GC finalizer (`rt_ws_finalize`) only releases memory
/// after this and is a no-op on the already-closed transport.
/// @param[in] obj Managed WebSocket receiver; NULL is a no-op.
/// @param[in] code Valid RFC 6455 or application close code.
/// @param[in] reason Optional managed UTF-8 reason of at most 123 bytes.
void rt_ws_close_with(void *obj, int64_t code, rt_string reason) {
    if (!obj)
        return;
    rt_ws_impl *ws = ws_require(obj, "WebSocket.Close: invalid object");
    if (!ws)
        return;
    if (!ws->is_open) {
        ws_close_transport(ws);
        return;
    }

    if (reason && !rt_string_is_handle(reason)) {
        rt_trap("WebSocket.Close: invalid reason String");
        return;
    }
    const char *reason_cstr = reason ? rt_string_cstr(reason) : NULL;
    int64_t reason_len64 = reason ? rt_str_len(reason) : 0;
    if (reason_len64 < 0 || (uint64_t)reason_len64 > (uint64_t)SIZE_MAX) {
        rt_trap("WebSocket: invalid close reason length");
        return;
    }
    size_t reason_len = (size_t)reason_len64;
    if (code < 0 || code > 4999 || !ws_close_code_is_valid((uint16_t)code)) {
        rt_trap_net("WebSocket: invalid close code", Err_ProtocolError);
        return;
    }
    if (!reason_cstr && reason_len > 0) {
        rt_trap_net("WebSocket: invalid close reason", Err_ProtocolError);
        return;
    }
    if (reason_len > 123 ||
        (reason_cstr && !ws_is_valid_utf8((const uint8_t *)reason_cstr, reason_len))) {
        rt_trap_net("WebSocket: invalid close reason", Err_ProtocolError);
        return;
    }

    size_t payload_len = 2 + reason_len;
    uint8_t payload[125];
    payload[0] = (uint8_t)(code >> 8);
    payload[1] = (uint8_t)(code);
    if (reason_len > 0)
        memcpy(payload + 2, reason_cstr, reason_len);

    if (!ws_send_close_best_effort(ws, payload, payload_len))
        ws->close_code = WS_CLOSE_ABNORMAL;

    ws->is_open = 0;
    if (ws->close_code != WS_CLOSE_ABNORMAL)
        ws->close_code = code;

    // Complete the closing handshake: bounded wait for the peer's close
    // reply, discarding any in-flight data frames, then close the transport
    // instead of leaving the socket/TLS session to GC finalization. The
    // frame cap bounds total time when a server streams tiny frames.
    enum { WS_CLOSE_REPLY_TIMEOUT_MS = 1000, WS_CLOSE_DRAIN_MAX_FRAMES = 32 };

    if (ws->socket_fd != INVALID_SOCK && ws_set_recv_timeout(ws, WS_CLOSE_REPLY_TIMEOUT_MS)) {
        for (int frames = 0; frames < WS_CLOSE_DRAIN_MAX_FRAMES; frames++) {
            uint8_t fin = 0;
            uint8_t opcode = 0;
            uint8_t *data = NULL;
            size_t data_len = 0;
            if (!ws_recv_frame(ws, &fin, &opcode, &data, &data_len))
                break; // timeout, EOF, or protocol error — stop waiting
            int got_close = (opcode == WS_OP_CLOSE);
            free(data);
            if (got_close)
                break;
        }
    }
    ws_close_transport(ws);
}
