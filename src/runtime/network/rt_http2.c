//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_http2.c
// Purpose: Internal HTTP/2 + HPACK transport used by the HTTPS runtime.
//
// Key invariants:
//   - Frame lengths, stream identifiers, flow-control windows, and decoded
//     header sizes are validated before use.
//   - Client streams are odd-numbered and the synchronous server path handles
//     one active request stream at a time.
//   - Any connection-level protocol or I/O failure records a diagnostic and
//     permanently marks the connection closed.
//
// Ownership/Lifetime:
//   - Frame payloads and growable buffers are owned by their containing value.
//   - Connection objects own HPACK state but borrow the transport context.
//   - Public request and response results transfer owned headers and body bytes
//     to the caller on success.
//
// Links: src/runtime/network/rt_http2.h,
//        src/runtime/network/rt_http2_internal.h,
//        src/runtime/network/rt_hpack.c
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements synchronous HTTP/2 framing over callback transports.
 * @details Validates frames, settings, stream identifiers, header blocks, and
 * flow-control windows; coordinates connection-scoped HPACK state; performs
 * client round trips and single-active-stream server exchanges; and transfers
 * owned decoded headers and bodies only after complete protocol success.
 */

#include "rt_http2.h"

#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
/** Restrict the Windows SDK surface to core declarations. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
/** Windows compatibility alias for case-insensitive string comparison. */
#define strcasecmp _stricmp
/** Windows compatibility alias for bounded case-insensitive comparison. */
#define strncasecmp _strnicmp
#else
#include <pthread.h>
#include <strings.h>
#endif

#include "rt_http2_internal.h"

#define H2_FRAME_DATA 0x0
#define H2_FRAME_HEADERS 0x1
#define H2_FRAME_PRIORITY 0x2
#define H2_FRAME_RST_STREAM 0x3
#define H2_FRAME_SETTINGS 0x4
#define H2_FRAME_PUSH_PROMISE 0x5
#define H2_FRAME_PING 0x6
#define H2_FRAME_GOAWAY 0x7
#define H2_FRAME_WINDOW_UPDATE 0x8
#define H2_FRAME_CONTINUATION 0x9

#define H2_FLAG_END_STREAM 0x1
#define H2_FLAG_ACK 0x1
#define H2_FLAG_END_HEADERS 0x4
#define H2_FLAG_PADDED 0x8
#define H2_FLAG_PRIORITY 0x20

#define H2_SETTINGS_HEADER_TABLE_SIZE 0x1
#define H2_SETTINGS_ENABLE_PUSH 0x2
#define H2_SETTINGS_MAX_CONCURRENT_STREAMS 0x3
#define H2_SETTINGS_INITIAL_WINDOW_SIZE 0x4
#define H2_SETTINGS_MAX_FRAME_SIZE 0x5
#define H2_SETTINGS_MAX_HEADER_LIST_SIZE 0x6

#define H2_ERROR_NO_ERROR 0x0
#define H2_ERROR_PROTOCOL 0x1
#define H2_ERROR_INTERNAL 0x2
#define H2_ERROR_FLOW_CONTROL 0x3
#define H2_ERROR_SETTINGS_TIMEOUT 0x4
#define H2_ERROR_STREAM_CLOSED 0x5
#define H2_ERROR_FRAME_SIZE 0x6
#define H2_ERROR_REFUSED_STREAM 0x7
#define H2_ERROR_CANCEL 0x8
#define H2_ERROR_COMPRESSION 0x9

#define H2_DEFAULT_WINDOW_SIZE 65535u
#define H2_MAX_WINDOW_SIZE 2147483647u
#define H2_DEFAULT_FRAME_SIZE 16384u
#define H2_MAX_FRAME_SIZE 16777215u
#define H2_MAX_HEADER_BLOCK (256u * 1024u)
#define H2_DEFAULT_MAX_HEADER_LIST_SIZE (64u * 1024u)
#define H2_MAX_BUFFER_BYTES (32u * 1024u * 1024u)

/** Decoded HTTP/2 frame header plus its owned payload bytes. */
typedef struct {
    uint8_t type;       ///< Frame type code.
    uint8_t flags;      ///< Type-specific flag bits.
    uint32_t stream_id; ///< Reserved-bit-cleared stream identifier.
    uint8_t *payload;   ///< Owned frame payload.
    size_t payload_len; ///< Number of bytes in @ref payload.
} h2_frame_t;

/** Connection-scoped framing, flow-control, diagnostic, and HPACK state. */
struct rt_http2_conn {
    rt_http2_io_t io;
    int is_server;
    int started;
    int closed;
    char error[256];
    uint32_t peer_initial_window;
    uint32_t peer_max_frame_size;
    uint32_t local_max_frame_size;
    uint32_t peer_max_header_list_size;
    uint32_t local_max_header_list_size;
    int64_t peer_conn_window;
    int next_stream_id;
    int sent_goaway;
    hpack_dyn_table_t encode_table;
    hpack_dyn_table_t decode_table;
};

/** Exact HTTP/2 client connection preface required by RFC 9113. */
static const char kClientPreface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

/// @brief Store a 16-bit integer in HTTP/2 network-byte order.
/// @param dst Writable two-byte destination.
/// @param v Value to encode.
static void h2_write_u16(uint8_t *dst, uint16_t v) {
    dst[0] = (uint8_t)(v >> 8);
    dst[1] = (uint8_t)(v);
}

/// @brief Store the low 24 bits of an integer in HTTP/2 network-byte order.
/// @param dst Writable three-byte destination.
/// @param v Value whose low 24 bits are encoded.
static void h2_write_u24(uint8_t *dst, uint32_t v) {
    dst[0] = (uint8_t)(v >> 16);
    dst[1] = (uint8_t)(v >> 8);
    dst[2] = (uint8_t)(v);
}

/// @brief Store a 32-bit integer in network-byte order.
/// @param dst Writable four-byte destination.
/// @param v Value to encode.
static void h2_write_u32(uint8_t *dst, uint32_t v) {
    dst[0] = (uint8_t)(v >> 24);
    dst[1] = (uint8_t)(v >> 16);
    dst[2] = (uint8_t)(v >> 8);
    dst[3] = (uint8_t)(v);
}

/// @brief Decode a network-order 16-bit integer.
/// @param src Readable two-byte source.
/// @return Decoded host-order value.
static uint16_t h2_read_u16(const uint8_t *src) {
    return (uint16_t)(((uint16_t)src[0] << 8) | src[1]);
}

/// @brief Decode a network-order 24-bit integer.
/// @param src Readable three-byte source.
/// @return Decoded host-order value in the low 24 bits.
static uint32_t h2_read_u24(const uint8_t *src) {
    return ((uint32_t)src[0] << 16) | ((uint32_t)src[1] << 8) | (uint32_t)src[2];
}

/// @brief Decode a network-order 32-bit integer.
/// @param src Readable four-byte source.
/// @return Decoded host-order value.
static uint32_t h2_read_u32(const uint8_t *src) {
    return ((uint32_t)src[0] << 24) | ((uint32_t)src[1] << 16) | ((uint32_t)src[2] << 8) |
           (uint32_t)src[3];
}

/// @brief Replace a connection's diagnostic text without changing its open state.
/// @param conn Connection to update; may be null.
/// @param msg Message to copy, or null for a generic HTTP/2 error.
static void h2_conn_set_error(rt_http2_conn_t *conn, const char *msg) {
    if (!conn)
        return;
    snprintf(conn->error, sizeof(conn->error), "%s", msg ? msg : "HTTP/2 error");
}

/// @brief Record a fatal connection error and mark the connection closed.
/// @param conn Connection to fail.
/// @param msg Diagnostic message, or null for a generic message.
/// @return Always 0, allowing direct propagation from failure branches.
static int h2_conn_fail(rt_http2_conn_t *conn, const char *msg) {
    if (!conn)
        return 0;
    h2_conn_set_error(conn, msg);
    conn->closed = 1;
    return 0;
}

/// @brief Ensure a growable HTTP/2 buffer can hold at least a requested capacity.
/// @details Capacity grows geometrically up to `H2_MAX_BUFFER_BYTES`; existing
///          data and logical length are preserved.
/// @param buf Buffer to grow.
/// @param needed Minimum required capacity.
/// @return 1 when the capacity is available; 0 on invalid input, overflow,
///         implementation-limit violation, or allocation failure.
static int h2_buf_reserve(h2_buf_t *buf, size_t needed) {
    size_t new_cap = 0;
    uint8_t *grown = NULL;
    if (!buf)
        return 0;
    if (needed <= buf->cap)
        return 1;
    if (needed > H2_MAX_BUFFER_BYTES)
        return 0;
    new_cap = buf->cap ? buf->cap : 256;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2)
            return 0;
        new_cap *= 2;
        if (new_cap > H2_MAX_BUFFER_BYTES)
            new_cap = H2_MAX_BUFFER_BYTES;
    }
    grown = (uint8_t *)realloc(buf->data, new_cap);
    if (!grown)
        return 0;
    buf->data = grown;
    buf->cap = new_cap;
    return 1;
}

/// @brief Append a byte range to a growable HTTP/2 buffer.
/// @param buf Destination buffer.
/// @param src Source bytes; may be null only when @p len is zero.
/// @param len Number of bytes to append.
/// @return 1 on success; 0 on invalid input, size overflow, limit violation, or allocation failure.
int h2_buf_append(h2_buf_t *buf, const void *src, size_t len) {
    if (!buf || (!src && len > 0))
        return 0;
    if (len == 0)
        return 1;
    if (buf->len > SIZE_MAX - len)
        return 0;
    if (!h2_buf_reserve(buf, buf->len + len))
        return 0;
    memcpy(buf->data + buf->len, src, len);
    buf->len += len;
    return 1;
}

/// @brief Append one byte to a growable HTTP/2 buffer.
/// @param buf Destination buffer.
/// @param b Byte to append.
/// @return 1 on success; 0 when the buffer cannot grow.
int h2_buf_append_byte(h2_buf_t *buf, uint8_t b) {
    return h2_buf_append(buf, &b, 1);
}

/// @brief Release a growable HTTP/2 buffer and reset all fields to zero.
/// @param buf Buffer to release; may be null.
void h2_buf_free(h2_buf_t *buf) {
    if (!buf)
        return;
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

/// @brief Validate a length-delimited HTTP/2 header name.
/// @param name Header-name bytes.
/// @param len Number of bytes to validate.
/// @return Nonzero for a nonempty lowercase name containing no spaces or control bytes.
int h2_header_name_bytes_is_valid(const char *name, size_t len) {
    if (!name || len == 0)
        return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c <= 0x20 || c == 0x7f)
            return 0;
        if (c >= 'A' && c <= 'Z')
            return 0;
    }
    return 1;
}

/// @brief Validate a null-terminated HTTP/2 header name.
/// @param name Header name to validate.
/// @return Nonzero when the complete name satisfies the transport's HTTP/2 rules.
int h2_header_name_is_valid(const char *name) {
    return name && h2_header_name_bytes_is_valid(name, strlen(name));
}

/// @brief Validate a length-delimited HTTP/2 header value.
/// @param value Header-value bytes.
/// @param len Number of bytes to validate.
/// @return Nonzero when the range contains no null, carriage-return, or line-feed bytes.
int h2_header_value_bytes_is_valid(const char *value, size_t len) {
    if (!value)
        return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)value[i];
        if (c == '\0' || c == '\r' || c == '\n')
            return 0;
    }
    return 1;
}

/// @brief Validate a null-terminated HTTP/2 header value.
/// @param value Header value to validate.
/// @return Nonzero when the complete value satisfies the transport's value rules.
int h2_header_value_is_valid(const char *value) {
    return value && h2_header_value_bytes_is_valid(value, strlen(value));
}

/// @brief Parse an HTTP/2 `:status` pseudo-header value.
/// @param value Three-character decimal status text.
/// @param status_out Receives the parsed status code.
/// @return 1 for a decimal value in the range 100 through 599; 0 otherwise.
static int h2_parse_status_code(const char *value, int *status_out) {
    if (!value || !status_out || strlen(value) != 3)
        return 0;
    if (!isdigit((unsigned char)value[0]) || !isdigit((unsigned char)value[1]) ||
        !isdigit((unsigned char)value[2]))
        return 0;
    int status = (value[0] - '0') * 100 + (value[1] - '0') * 10 + (value[2] - '0');
    if (status < 100 || status > 599)
        return 0;
    *status_out = status;
    return 1;
}

/// @brief Allocate a null-terminated copy of a byte range.
/// @param src Source bytes; must be readable when @p len is nonzero.
/// @param len Number of bytes to copy.
/// @return Newly allocated string, or null on length overflow or allocation failure.
char *h2_strdup_range(const uint8_t *src, size_t len) {
    if (len == SIZE_MAX)
        return NULL;
    char *out = (char *)malloc(len + 1);
    if (!out)
        return NULL;
    if (len > 0)
        memcpy(out, src, len);
    out[len] = '\0';
    return out;
}

/// @brief Allocate an ASCII-lowercase copy of a string.
/// @param src Source string; null is treated as an empty string.
/// @return Newly allocated copy, or null on allocation failure.
char *h2_strdup_lower(const char *src) {
    size_t len = src ? strlen(src) : 0;
    if (len == SIZE_MAX)
        return NULL;
    char *out = (char *)malloc(len + 1);
    if (!out)
        return NULL;
    for (size_t i = 0; i < len; i++) {
        char c = src[i];
        out[i] = (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
    }
    out[len] = '\0';
    return out;
}

/// @brief Identify header fields forbidden in HTTP/2 because they are connection-specific.
/// @details Also rejects `te` except for the sole permitted value `trailers`.
/// @param name Header name; null is treated as not connection-specific.
/// @param value Header value used to validate `te`; may be null.
/// @return Nonzero when the field must not appear in HTTP/2.
static int h2_header_is_connection_specific(const char *name, const char *value) {
    if (!name)
        return 0;
    if (strcasecmp(name, "connection") == 0 || strcasecmp(name, "proxy-connection") == 0 ||
        strcasecmp(name, "keep-alive") == 0 || strcasecmp(name, "upgrade") == 0 ||
        strcasecmp(name, "transfer-encoding") == 0) {
        return 1;
    }
    if (strcasecmp(name, "te") == 0 && value && strcasecmp(value, "trailers") != 0)
        return 1;
    return 0;
}

/// @brief Append an owned copy of a header field to the end of a linked list.
/// @param list Address of the list head.
/// @param name Null-terminated header name to copy.
/// @param value Null-terminated header value to copy.
/// @return 1 on success; 0 for invalid input or allocation failure.
int rt_http2_header_append_copy(rt_http2_header_t **list, const char *name, const char *value) {
    rt_http2_header_t *node = NULL;
    rt_http2_header_t **tail = list;
    if (!list || !name || !value)
        return 0;
    node = (rt_http2_header_t *)calloc(1, sizeof(*node));
    if (!node)
        return 0;
    node->name = strdup(name);
    node->value = strdup(value);
    if (!node->name || !node->value) {
        free(node->name);
        free(node->value);
        free(node);
        return 0;
    }
    while (*tail)
        tail = &(*tail)->next;
    *tail = node;
    return 1;
}

/// @brief Find the first case-insensitive header-name match.
/// @param list Header list to search; may be null.
/// @param name Header name to find.
/// @return Borrowed value pointer for the first match, or null when absent.
const char *rt_http2_header_get(const rt_http2_header_t *list, const char *name) {
    for (const rt_http2_header_t *it = list; it; it = it->next) {
        if (it->name && name && strcasecmp(it->name, name) == 0)
            return it->value;
    }
    return NULL;
}

/// @brief Release an entire owned HTTP/2 header list.
/// @param headers First list node; may be null.
void rt_http2_headers_free(rt_http2_header_t *headers) {
    while (headers) {
        rt_http2_header_t *next = headers->next;
        free(headers->name);
        free(headers->value);
        free(headers);
        headers = next;
    }
}

/// @brief Compute an HTTP/2 SETTINGS_MAX_HEADER_LIST_SIZE total.
/// @details RFC 7540 accounts each decoded header as name bytes, value bytes,
///          plus 32 bytes of overhead. This helper also returns a field count
///          so callers can enforce a separate structural cap.
/// @param headers Decoded header chain to measure.
/// @param size_out Receives the computed byte total.
/// @param count_out Optional destination for number of fields.
/// @return 1 on successful accounting, 0 on arithmetic overflow.
static int h2_header_list_account(const rt_http2_header_t *headers,
                                  size_t *size_out,
                                  size_t *count_out) {
    size_t total = 0;
    size_t count = 0;
    if (!size_out)
        return 0;
    for (const rt_http2_header_t *it = headers; it; it = it->next) {
        size_t name_len = it->name ? strlen(it->name) : 0;
        size_t value_len = it->value ? strlen(it->value) : 0;
        size_t field_size = 0;
        if (name_len > SIZE_MAX - value_len || name_len + value_len > SIZE_MAX - 32u)
            return 0;
        field_size = name_len + value_len + 32u;
        if (total > SIZE_MAX - field_size)
            return 0;
        total += field_size;
        count++;
    }
    *size_out = total;
    if (count_out)
        *count_out = count;
    return 1;
}

/// @brief Add one field to an HTTP/2 header-list byte total.
/// @details Applies the same name + value + 32-byte accounting used by
///          SETTINGS_MAX_HEADER_LIST_SIZE. This is used before HPACK encoding
///          so compressed size cannot hide an oversized logical header list.
/// @param total_io Running total to update.
/// @param name Header name; NULL is treated as empty.
/// @param value Header value; NULL is treated as empty.
/// @return 1 if the addition succeeds, 0 on arithmetic overflow.
static int h2_header_list_add_field(size_t *total_io, const char *name, const char *value) {
    size_t name_len = name ? strlen(name) : 0;
    size_t value_len = value ? strlen(value) : 0;
    size_t field_size = 0;
    if (!total_io)
        return 0;
    if (name_len > SIZE_MAX - value_len || name_len + value_len > SIZE_MAX - 32u)
        return 0;
    field_size = name_len + value_len + 32u;
    if (*total_io > SIZE_MAX - field_size)
        return 0;
    *total_io += field_size;
    return 1;
}

/// @brief Release every allocation in a decoded request and reset it to zero.
/// @param req Request to clear; may be null.
void rt_http2_request_free(rt_http2_request_t *req) {
    if (!req)
        return;
    free(req->method);
    free(req->scheme);
    free(req->authority);
    free(req->path);
    rt_http2_headers_free(req->headers);
    free(req->body);
    memset(req, 0, sizeof(*req));
}

/// @brief Release every allocation in a decoded response and reset it to zero.
/// @param res Response to clear; may be null.
void rt_http2_response_free(rt_http2_response_t *res) {
    if (!res)
        return;
    rt_http2_headers_free(res->headers);
    free(res->body);
    memset(res, 0, sizeof(*res));
}

/// @brief Fill a buffer by repeatedly invoking the connection's read callback.
/// @param conn Connection providing transport I/O and error storage.
/// @param buf Destination buffer; must be non-null even for a zero-length request.
/// @param len Exact number of bytes required.
/// @return 1 after all bytes are read; 0 after invalid input, EOF, or callback failure closes the
///         connection.
static int h2_io_read_exact(rt_http2_conn_t *conn, uint8_t *buf, size_t len) {
    size_t total = 0;
    if (!conn || !buf || !conn->io.read)
        return h2_conn_fail(conn, "HTTP/2: invalid read callback");
    while (total < len) {
        long n = conn->io.read(conn->io.ctx, buf + total, len - total);
        if (n <= 0)
            return h2_conn_fail(conn, "HTTP/2: read failed");
        total += (size_t)n;
    }
    return 1;
}

/// @brief Write an entire byte range through the connection's whole-range callback.
/// @details Splits requests at `INT_MAX` so callback implementations backed by
///          signed platform lengths can handle large buffers safely.
/// @param conn Connection providing transport I/O and error storage.
/// @param buf Source bytes; may be null only when @p len is zero.
/// @param len Number of bytes to write.
/// @return 1 after all chunks are written; 0 after invalid input or callback failure closes the
///         connection.
static int h2_io_write_all(rt_http2_conn_t *conn, const uint8_t *buf, size_t len) {
    size_t total = 0;
    if (!conn || (!buf && len > 0) || !conn->io.write)
        return h2_conn_fail(conn, "HTTP/2: invalid write callback");
    while (total < len) {
        size_t chunk = len - total;
        if (chunk > INT_MAX)
            chunk = INT_MAX;
        if (!conn->io.write(conn->io.ctx, buf + total, chunk))
            return h2_conn_fail(conn, "HTTP/2: write failed");
        total += chunk;
    }
    return 1;
}

/// @brief Release a parsed frame's owned payload and reset the frame to zero.
/// @param frame Frame to clear; may be null.
static void h2_frame_free(h2_frame_t *frame) {
    if (!frame)
        return;
    free(frame->payload);
    memset(frame, 0, sizeof(*frame));
}

/// @brief Serialize and write one HTTP/2 frame.
/// @param conn Destination connection.
/// @param type Frame type octet.
/// @param flags Frame flags octet.
/// @param stream_id 31-bit stream identifier, or zero for a connection frame.
/// @param payload Optional payload bytes.
/// @param payload_len Number of payload bytes, limited to the protocol's 24-bit maximum.
/// @return 1 on success; 0 after invalid parameters or I/O failure closes the connection.
static int h2_send_frame(rt_http2_conn_t *conn,
                         uint8_t type,
                         uint8_t flags,
                         uint32_t stream_id,
                         const uint8_t *payload,
                         size_t payload_len) {
    uint8_t header[9];
    if (!conn || stream_id > 0x7fffffffu || payload_len > H2_MAX_FRAME_SIZE)
        return h2_conn_fail(conn, "HTTP/2: invalid frame parameters");
    h2_write_u24(header, (uint32_t)payload_len);
    header[3] = type;
    header[4] = flags;
    header[5] = (uint8_t)((stream_id >> 24) & 0x7f);
    header[6] = (uint8_t)(stream_id >> 16);
    header[7] = (uint8_t)(stream_id >> 8);
    header[8] = (uint8_t)stream_id;
    if (!h2_io_write_all(conn, header, sizeof(header)))
        return 0;
    if (payload_len > 0 && !h2_io_write_all(conn, payload, payload_len))
        return 0;
    return 1;
}

/// @brief Read one HTTP/2 frame header and its complete payload.
/// @details Enforces the locally advertised maximum frame size and masks the
///          reserved stream-ID bit. On success, @p frame owns any allocated payload.
/// @param conn Source connection.
/// @param frame Output frame, reset before parsing.
/// @return 1 on success; 0 on invalid input, oversized input, allocation failure, or I/O failure.
static int h2_read_frame(rt_http2_conn_t *conn, h2_frame_t *frame) {
    uint8_t header[9];
    size_t payload_len = 0;
    if (!conn || !frame)
        return 0;
    memset(frame, 0, sizeof(*frame));
    if (!h2_io_read_exact(conn, header, sizeof(header)))
        return 0;
    payload_len = h2_read_u24(header);
    if (payload_len > conn->local_max_frame_size)
        return h2_conn_fail(conn, "HTTP/2: oversized frame");
    frame->type = header[3];
    frame->flags = header[4];
    frame->stream_id = h2_read_u32(header + 5) & 0x7fffffffu;
    frame->payload_len = payload_len;
    if (payload_len > 0) {
        frame->payload = (uint8_t *)malloc(payload_len);
        if (!frame->payload)
            return h2_conn_fail(conn, "HTTP/2: frame allocation failed");
        if (!h2_io_read_exact(conn, frame->payload, payload_len)) {
            h2_frame_free(frame);
            return 0;
        }
    }
    return 1;
}

/// @brief Send this endpoint's initial non-ACK SETTINGS frame.
/// @details Advertises the local header-list limit and, for clients, disables
///          server push.
/// @param conn Connection whose endpoint role and limits are advertised.
/// @return 1 on success; 0 for invalid input or I/O failure.
static int h2_send_settings(rt_http2_conn_t *conn) {
    uint8_t payload[12];
    size_t pos = 0;
    if (!conn)
        return 0;
    if (!conn->is_server) {
        h2_write_u16(payload + pos, H2_SETTINGS_ENABLE_PUSH);
        h2_write_u32(payload + pos + 2, 0);
        pos += 6;
    }
    h2_write_u16(payload + pos, H2_SETTINGS_MAX_HEADER_LIST_SIZE);
    h2_write_u32(payload + pos + 2, conn->local_max_header_list_size);
    pos += 6;
    return h2_send_frame(conn, H2_FRAME_SETTINGS, 0, 0, payload, pos);
}

/// @brief Validate and apply one peer SETTINGS parameter.
/// @details Unknown identifiers are ignored as required by HTTP/2. Changes to
///          the initial stream window optionally adjust an active stream's
///          current send window by the same signed delta.
/// @param conn Connection whose peer limits are updated.
/// @param id SETTINGS identifier.
/// @param value Peer-advertised value.
/// @param stream_window_io Optional active stream send window to adjust.
/// @return 1 when the setting is valid or unknown; 0 for a recognized value outside its legal
///         range.
static int h2_apply_setting(rt_http2_conn_t *conn,
                            uint16_t id,
                            uint32_t value,
                            int64_t *stream_window_io) {
    switch (id) {
        case H2_SETTINGS_HEADER_TABLE_SIZE:
            if (value > H2_MAX_DYNAMIC_TABLE_SIZE)
                return 0;
            hpack_dyn_table_set_max_size(&conn->encode_table, value);
            break;
        case H2_SETTINGS_ENABLE_PUSH:
            if (value != 0 && value != 1)
                return 0;
            break;
        case H2_SETTINGS_MAX_CONCURRENT_STREAMS:
            break;
        case H2_SETTINGS_INITIAL_WINDOW_SIZE: {
            if (value > H2_MAX_WINDOW_SIZE)
                return 0;
            int64_t delta = (int64_t)value - (int64_t)conn->peer_initial_window;
            conn->peer_initial_window = value;
            if (stream_window_io)
                *stream_window_io += delta;
            break;
        }
        case H2_SETTINGS_MAX_FRAME_SIZE:
            if (value < H2_DEFAULT_FRAME_SIZE || value > H2_MAX_FRAME_SIZE)
                return 0;
            conn->peer_max_frame_size = value;
            break;
        case H2_SETTINGS_MAX_HEADER_LIST_SIZE:
            conn->peer_max_header_list_size = value;
            break;
        default:
            break;
    }
    return 1;
}

/// @brief Return consumed receive-window credit with a WINDOW_UPDATE frame.
/// @param conn Destination connection.
/// @param stream_id Stream identifier, or zero for connection-level credit.
/// @param increment Positive 31-bit flow-control increment.
/// @return 1 on success; 0 after an invalid increment or I/O failure.
static int h2_send_window_update(rt_http2_conn_t *conn, uint32_t stream_id, uint32_t increment) {
    uint8_t payload[4];
    if (increment == 0 || increment > 0x7fffffffu)
        return h2_conn_fail(conn, "HTTP/2: invalid WINDOW_UPDATE increment");
    payload[0] = (uint8_t)((increment >> 24) & 0x7f);
    payload[1] = (uint8_t)(increment >> 16);
    payload[2] = (uint8_t)(increment >> 8);
    payload[3] = (uint8_t)increment;
    return h2_send_frame(conn, H2_FRAME_WINDOW_UPDATE, 0, stream_id, payload, sizeof(payload));
}

/// @brief Send an RST_STREAM frame with a protocol error code.
/// @param conn Destination connection.
/// @param stream_id Stream to reset.
/// @param error_code HTTP/2 error code.
/// @return 1 on success; 0 on invalid frame parameters or I/O failure.
static int h2_send_rst_stream(rt_http2_conn_t *conn, uint32_t stream_id, uint32_t error_code) {
    uint8_t payload[4];
    payload[0] = (uint8_t)(error_code >> 24);
    payload[1] = (uint8_t)(error_code >> 16);
    payload[2] = (uint8_t)(error_code >> 8);
    payload[3] = (uint8_t)error_code;
    return h2_send_frame(conn, H2_FRAME_RST_STREAM, 0, stream_id, payload, sizeof(payload));
}

/// @brief Extract the HPACK fragment and END_STREAM state from a HEADERS payload.
/// @details Skips optional pad-length and priority fields and verifies that
///          declared padding fits within the payload.
/// @param frame HEADERS frame to inspect.
/// @param fragment_out Optional receiver for a borrowed pointer into the frame payload.
/// @param fragment_len_out Optional receiver for the fragment length excluding padding.
/// @param end_stream_out Optional receiver for the END_STREAM flag state.
/// @return 1 for a structurally valid HEADERS payload; 0 otherwise.
static int h2_parse_headers_payload(const h2_frame_t *frame,
                                    const uint8_t **fragment_out,
                                    size_t *fragment_len_out,
                                    int *end_stream_out) {
    size_t pos = 0;
    uint8_t pad_len = 0;
    if (!frame || frame->type != H2_FRAME_HEADERS)
        return 0;
    if (frame->flags & H2_FLAG_PADDED) {
        if (frame->payload_len < 1)
            return 0;
        pad_len = frame->payload[0];
        pos++;
    }
    if (frame->flags & H2_FLAG_PRIORITY) {
        if (frame->payload_len < pos + 5)
            return 0;
        pos += 5;
    }
    if (frame->payload_len < pos + pad_len)
        return 0;
    if (fragment_out)
        *fragment_out = frame->payload + pos;
    if (fragment_len_out)
        *fragment_len_out = frame->payload_len - pos - pad_len;
    if (end_stream_out)
        *end_stream_out = (frame->flags & H2_FLAG_END_STREAM) != 0;
    return 1;
}

/// @brief Extract application bytes and END_STREAM state from a DATA payload.
/// @param frame DATA frame to inspect.
/// @param data_out Optional receiver for a borrowed pointer into the frame payload.
/// @param data_len_out Optional receiver for the application byte count excluding padding.
/// @param end_stream_out Optional receiver for the END_STREAM flag state.
/// @return 1 for a structurally valid DATA payload; 0 otherwise.
static int h2_parse_data_payload(const h2_frame_t *frame,
                                 const uint8_t **data_out,
                                 size_t *data_len_out,
                                 int *end_stream_out) {
    size_t pos = 0;
    uint8_t pad_len = 0;
    if (!frame || frame->type != H2_FRAME_DATA)
        return 0;
    if (frame->flags & H2_FLAG_PADDED) {
        if (frame->payload_len < 1)
            return 0;
        pad_len = frame->payload[0];
        pos++;
    }
    if (frame->payload_len < pos + pad_len)
        return 0;
    if (data_out)
        *data_out = frame->payload + pos;
    if (data_len_out)
        *data_len_out = frame->payload_len - pos - pad_len;
    if (end_stream_out)
        *end_stream_out = (frame->flags & H2_FLAG_END_STREAM) != 0;
    return 1;
}

/// @brief Collect a HEADERS fragment and its required CONTINUATION sequence.
/// @details Rejects interleaved frames, mismatched stream identifiers, and
///          aggregate blocks above `H2_MAX_HEADER_BLOCK`.
/// @param conn Connection used to read continuation frames and record failures.
/// @param first Initial HEADERS frame.
/// @param block_out Receives an owned contiguous HPACK block.
/// @param end_stream_out Optional receiver for END_STREAM from the initial HEADERS frame.
/// @return 1 after END_HEADERS is reached; 0 on malformed input, allocation failure, or I/O
///         failure.
static int h2_collect_header_block(rt_http2_conn_t *conn,
                                   const h2_frame_t *first,
                                   h2_buf_t *block_out,
                                   int *end_stream_out) {
    const uint8_t *fragment = NULL;
    size_t fragment_len = 0;
    h2_frame_t frame;
    if (!conn || !first || !block_out)
        return 0;
    memset(block_out, 0, sizeof(*block_out));
    if (!h2_parse_headers_payload(first, &fragment, &fragment_len, end_stream_out) ||
        !h2_buf_append(block_out, fragment, fragment_len)) {
        h2_buf_free(block_out);
        return h2_conn_fail(conn, "HTTP/2: malformed HEADERS frame");
    }
    if (block_out->len > H2_MAX_HEADER_BLOCK) {
        h2_buf_free(block_out);
        return h2_conn_fail(conn, "HTTP/2: header block too large");
    }
    if (first->flags & H2_FLAG_END_HEADERS)
        return 1;

    while (1) {
        memset(&frame, 0, sizeof(frame));
        if (!h2_read_frame(conn, &frame)) {
            h2_buf_free(block_out);
            return 0;
        }
        if (frame.type != H2_FRAME_CONTINUATION || frame.stream_id != first->stream_id) {
            h2_frame_free(&frame);
            h2_buf_free(block_out);
            return h2_conn_fail(conn, "HTTP/2: invalid CONTINUATION sequence");
        }
        if (!h2_buf_append(block_out, frame.payload, frame.payload_len)) {
            h2_frame_free(&frame);
            h2_buf_free(block_out);
            return h2_conn_fail(conn, "HTTP/2: header block allocation failed");
        }
        if (block_out->len > H2_MAX_HEADER_BLOCK) {
            h2_frame_free(&frame);
            h2_buf_free(block_out);
            return h2_conn_fail(conn, "HTTP/2: header block too large");
        }
        if (frame.flags & H2_FLAG_END_HEADERS) {
            h2_frame_free(&frame);
            return 1;
        }
        h2_frame_free(&frame);
    }
}

/// @brief Collect and HPACK-decode one header sequence under local list limits.
/// @details Enforces both the RFC header-list byte accounting and a structural
///          limit of 256 fields after decompression.
/// @param conn Connection providing decode-table state and configured limits.
/// @param first Initial HEADERS frame.
/// @param decoded_out Receives an owned decoded header list on success.
/// @param end_stream_out Optional receiver for END_STREAM from the initial frame.
/// @param decode_error Diagnostic text recorded when HPACK decoding fails.
/// @return 1 on success; 0 on collection, decompression, size, or allocation failure.
static int h2_decode_header_list(rt_http2_conn_t *conn,
                                 const h2_frame_t *first,
                                 rt_http2_header_t **decoded_out,
                                 int *end_stream_out,
                                 const char *decode_error) {
    h2_buf_t header_block;
    if (!conn || !first || !decoded_out)
        return 0;
    memset(&header_block, 0, sizeof(header_block));
    *decoded_out = NULL;
    if (!h2_collect_header_block(conn, first, &header_block, end_stream_out))
        return 0;
    if (!hpack_decode_header_block(
            &conn->decode_table, header_block.data, header_block.len, decoded_out)) {
        h2_buf_free(&header_block);
        return h2_conn_fail(conn, decode_error);
    }
    h2_buf_free(&header_block);
    {
        size_t decoded_size = 0;
        size_t decoded_count = 0;
        if (!h2_header_list_account(*decoded_out, &decoded_size, &decoded_count) ||
            decoded_size > conn->local_max_header_list_size || decoded_count > 256u) {
            rt_http2_headers_free(*decoded_out);
            *decoded_out = NULL;
            return h2_conn_fail(conn, "HTTP/2: decoded header list too large");
        }
    }
    return 1;
}

/// @brief Append copied regular trailer fields to an existing header list.
/// @param dest Address of the destination list head.
/// @param decoded Borrowed decoded trailer list.
/// @return 1 when every trailer is regular and copied; 0 on pseudo-headers,
///         connection-specific fields, or allocation failure.
static int h2_append_trailer_headers(rt_http2_header_t **dest, const rt_http2_header_t *decoded) {
    for (const rt_http2_header_t *it = decoded; it; it = it->next) {
        if (it->name[0] == ':' || h2_header_is_connection_specific(it->name, it->value) ||
            !rt_http2_header_append_copy(dest, it->name, it->value)) {
            return 0;
        }
    }
    return 1;
}

/// @brief Consume enough of an unsupported concurrent request frame to preserve framing and reset
///        its stream.
/// @details Header continuations are collected and discarded; DATA bytes
///          restore connection-level receive credit before REFUSED_STREAM is sent.
/// @param conn Server connection currently processing another request.
/// @param frame Initial HEADERS or DATA frame on the concurrent odd-numbered stream.
/// @return 1 after sending REFUSED_STREAM; 0 on invalid input, malformed framing, or I/O failure.
static int h2_refuse_concurrent_request_stream(rt_http2_conn_t *conn, const h2_frame_t *frame) {
    h2_buf_t discard = {0};
    const uint8_t *data_ptr = NULL;
    size_t data_len = 0;
    int end_stream = 0;
    if (!conn || !frame || frame->stream_id == 0 || (frame->stream_id & 1u) == 0u)
        return h2_conn_fail(conn, "HTTP/2: invalid concurrent request stream");
    if (frame->type == H2_FRAME_HEADERS) {
        if (!h2_collect_header_block(conn, frame, &discard, &end_stream))
            return 0;
        h2_buf_free(&discard);
    } else if (frame->type == H2_FRAME_DATA) {
        if (!h2_parse_data_payload(frame, &data_ptr, &data_len, &end_stream))
            return h2_conn_fail(conn, "HTTP/2: invalid concurrent request body");
        if (data_len > 0 && !h2_send_window_update(conn, 0, (uint32_t)data_len))
            return 0;
    } else {
        return h2_conn_fail(conn, "HTTP/2: unsupported concurrent request frame");
    }
    return h2_send_rst_stream(conn, frame->stream_id, H2_ERROR_REFUSED_STREAM);
}

/// @brief Split an encoded HPACK block across HEADERS and CONTINUATION frames.
/// @param conn Destination connection and source of the peer frame-size limit.
/// @param stream_id Target stream identifier.
/// @param block Encoded HPACK bytes.
/// @param block_len Number of bytes in @p block.
/// @param end_stream Whether a single-frame HEADERS block should also close the stream.
/// @return 1 after the complete block is written; 0 on invalid state or I/O failure.
static int h2_send_headers_block(rt_http2_conn_t *conn,
                                 uint32_t stream_id,
                                 const uint8_t *block,
                                 size_t block_len,
                                 int end_stream) {
    size_t pos = 0;
    if (!conn)
        return 0;
    if (block_len == 0) {
        uint8_t flags = H2_FLAG_END_HEADERS;
        if (end_stream)
            flags |= H2_FLAG_END_STREAM;
        return h2_send_frame(conn, H2_FRAME_HEADERS, flags, stream_id, NULL, 0);
    }
    while (pos < block_len) {
        size_t chunk = block_len - pos;
        uint8_t flags = 0;
        if (chunk > conn->peer_max_frame_size)
            chunk = conn->peer_max_frame_size;
        if (chunk == 0)
            return h2_conn_fail(conn, "HTTP/2: peer max frame size is zero");
        if (pos == 0 && end_stream && block_len == chunk)
            flags |= H2_FLAG_END_STREAM;
        if (pos + chunk == block_len)
            flags |= H2_FLAG_END_HEADERS;
        if (!h2_send_frame(conn,
                           pos == 0 ? H2_FRAME_HEADERS : H2_FRAME_CONTINUATION,
                           flags,
                           stream_id,
                           block + pos,
                           chunk)) {
            return 0;
        }
        pos += chunk;
    }
    return 1;
}

/// @brief Process connection-control and asynchronous frames shared by client and server loops.
/// @details Validates and acknowledges SETTINGS and PING, applies flow-control
///          updates, handles GOAWAY and resets, ignores valid PRIORITY or
///          unknown extension frames, and rejects unsupported push promises.
/// @param conn Connection whose protocol state is updated.
/// @param frame Parsed frame to consider.
/// @param stream_id Currently active stream identifier.
/// @param stream_window_io Optional active stream send window.
/// @param handled_out Optional receiver set nonzero when no higher-level processing is needed.
/// @return 1 when the frame is valid, whether handled or left for the caller; 0 on protocol or I/O
///         failure.
static int h2_handle_common_frame(rt_http2_conn_t *conn,
                                  const h2_frame_t *frame,
                                  uint32_t stream_id,
                                  int64_t *stream_window_io,
                                  int *handled_out) {
    if (handled_out)
        *handled_out = 0;
    if (!conn || !frame)
        return 0;
    switch (frame->type) {
        case H2_FRAME_SETTINGS:
            if (frame->stream_id != 0 ||
                ((frame->flags & H2_FLAG_ACK) == 0 && (frame->payload_len % 6) != 0) ||
                ((frame->flags & H2_FLAG_ACK) != 0 && frame->payload_len != 0)) {
                return h2_conn_fail(conn, "HTTP/2: malformed SETTINGS frame");
            }
            if ((frame->flags & H2_FLAG_ACK) == 0) {
                for (size_t pos = 0; pos < frame->payload_len; pos += 6) {
                    uint16_t id = h2_read_u16(frame->payload + pos);
                    uint32_t value = h2_read_u32(frame->payload + pos + 2);
                    if (!h2_apply_setting(conn, id, value, stream_window_io))
                        return h2_conn_fail(conn, "HTTP/2: invalid SETTINGS value");
                }
                if (!h2_send_frame(conn, H2_FRAME_SETTINGS, H2_FLAG_ACK, 0, NULL, 0))
                    return 0;
            }
            if (handled_out)
                *handled_out = 1;
            return 1;

        case H2_FRAME_PING:
            if (frame->stream_id != 0 || frame->payload_len != 8)
                return h2_conn_fail(conn, "HTTP/2: malformed PING frame");
            if ((frame->flags & H2_FLAG_ACK) == 0 &&
                !h2_send_frame(conn, H2_FRAME_PING, H2_FLAG_ACK, 0, frame->payload, 8)) {
                return 0;
            }
            if (handled_out)
                *handled_out = 1;
            return 1;

        case H2_FRAME_WINDOW_UPDATE: {
            uint32_t increment = 0;
            if (frame->payload_len != 4)
                return h2_conn_fail(conn, "HTTP/2: malformed WINDOW_UPDATE frame");
            increment = h2_read_u32(frame->payload) & 0x7fffffffu;
            if (increment == 0)
                return h2_conn_fail(conn, "HTTP/2: zero WINDOW_UPDATE");
            if (frame->stream_id == 0) {
                if (conn->peer_conn_window > (int64_t)H2_MAX_WINDOW_SIZE - (int64_t)increment)
                    return h2_conn_fail(conn, "HTTP/2: connection flow-control window overflow");
                conn->peer_conn_window += increment;
            } else if (frame->stream_id == stream_id && stream_window_io) {
                if (*stream_window_io > (int64_t)H2_MAX_WINDOW_SIZE - (int64_t)increment)
                    return h2_conn_fail(conn, "HTTP/2: stream flow-control window overflow");
                *stream_window_io += increment;
            }
            if (handled_out)
                *handled_out = 1;
            return 1;
        }

        case H2_FRAME_PRIORITY:
            if (frame->stream_id == 0 || frame->payload_len != 5)
                return h2_conn_fail(conn, "HTTP/2: malformed PRIORITY frame");
            if (handled_out)
                *handled_out = 1;
            return 1;

        case H2_FRAME_GOAWAY:
            if (frame->stream_id != 0 || frame->payload_len < 8)
                return h2_conn_fail(conn, "HTTP/2: malformed GOAWAY frame");
            conn->closed = 1;
            if (handled_out)
                *handled_out = 1;
            return 1;

        case H2_FRAME_PUSH_PROMISE:
            return h2_conn_fail(conn, "HTTP/2: PUSH_PROMISE is not supported");

        case H2_FRAME_RST_STREAM:
            if (frame->payload_len != 4)
                return h2_conn_fail(conn, "HTTP/2: malformed RST_STREAM frame");
            if (frame->stream_id == stream_id)
                return h2_conn_fail(conn, "HTTP/2: stream reset by peer");
            if (handled_out)
                *handled_out = 1;
            return 1;

        default:
            if (frame->type != H2_FRAME_DATA && frame->type != H2_FRAME_HEADERS &&
                frame->type != H2_FRAME_CONTINUATION && handled_out) {
                *handled_out = 1;
            }
            return 1;
    }
}

/// @brief Read and process control frames until connection and stream send credit are positive.
/// @param conn Connection whose peer flow-control window is monitored.
/// @param stream_id Active outbound stream.
/// @param stream_window_io Optional active stream send window.
/// @return 1 when data can be sent; 0 on I/O, protocol, or unexpected-frame failure.
static int h2_wait_for_send_window(rt_http2_conn_t *conn,
                                   uint32_t stream_id,
                                   int64_t *stream_window_io) {
    while (conn->peer_conn_window <= 0 || (stream_window_io && *stream_window_io <= 0)) {
        h2_frame_t frame;
        int handled = 0;
        memset(&frame, 0, sizeof(frame));
        if (!h2_read_frame(conn, &frame))
            return 0;
        if (!h2_handle_common_frame(conn, &frame, stream_id, stream_window_io, &handled)) {
            h2_frame_free(&frame);
            return 0;
        }
        if (!handled) {
            h2_frame_free(&frame);
            return h2_conn_fail(conn, "HTTP/2: unexpected frame while waiting for send window");
        }
        h2_frame_free(&frame);
    }
    return 1;
}

/// @brief Send a complete body as flow-controlled DATA frames ending the stream.
/// @details Honors both connection and initial per-stream peer windows, reading
///          control frames when more credit is required.
/// @param conn Destination connection.
/// @param stream_id Target stream identifier.
/// @param data Body bytes; must be readable when @p data_len is nonzero.
/// @param data_len Number of body bytes.
/// @return 1 after an END_STREAM DATA frame is sent; 0 on protocol or I/O failure.
static int h2_send_data(rt_http2_conn_t *conn,
                        uint32_t stream_id,
                        const uint8_t *data,
                        size_t data_len) {
    size_t pos = 0;
    int64_t stream_window = (int64_t)conn->peer_initial_window;
    while (pos < data_len) {
        size_t max_chunk = conn->peer_max_frame_size;
        int end_stream = 0;
        if (!h2_wait_for_send_window(conn, stream_id, &stream_window))
            return 0;
        if (conn->peer_conn_window < (int64_t)max_chunk)
            max_chunk = (size_t)conn->peer_conn_window;
        if (stream_window < (int64_t)max_chunk)
            max_chunk = (size_t)stream_window;
        if (max_chunk > data_len - pos)
            max_chunk = data_len - pos;
        end_stream = (pos + max_chunk == data_len);
        if (!h2_send_frame(conn,
                           H2_FRAME_DATA,
                           end_stream ? H2_FLAG_END_STREAM : 0,
                           stream_id,
                           data + pos,
                           max_chunk)) {
            return 0;
        }
        conn->peer_conn_window -= (int64_t)max_chunk;
        stream_window -= (int64_t)max_chunk;
        pos += max_chunk;
    }
    if (data_len == 0)
        return h2_send_frame(conn, H2_FRAME_DATA, H2_FLAG_END_STREAM, stream_id, NULL, 0);
    return 1;
}

/// @brief Build and validate the HPACK block for an outbound request.
/// @details Emits required pseudo-headers first. Caller-supplied pseudo-fields,
///          `host`, connection-specific fields, and null-valued nodes are
///          skipped; remaining fields are subject to the peer's logical and
///          encoded header-list limits.
/// @param conn Connection providing encoder state and peer limits.
/// @param method Request `:method`.
/// @param scheme Request `:scheme`.
/// @param authority Request `:authority`.
/// @param path Request `:path`.
/// @param headers Optional regular header list.
/// @param out Receives an owned encoded block.
/// @return 1 on success; 0 on invalid input, invalid field data, limit violation, or allocation
///         failure.
static int h2_build_request_block(rt_http2_conn_t *conn,
                                  const char *method,
                                  const char *scheme,
                                  const char *authority,
                                  const char *path,
                                  const rt_http2_header_t *headers,
                                  h2_buf_t *out) {
    size_t header_list_size = 0;
    if (!conn || !method || !scheme || !authority || !path || !out)
        return 0;
    if (!h2_header_list_add_field(&header_list_size, ":method", method) ||
        !h2_header_list_add_field(&header_list_size, ":scheme", scheme) ||
        !h2_header_list_add_field(&header_list_size, ":authority", authority) ||
        !h2_header_list_add_field(&header_list_size, ":path", path)) {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    if (!hpack_encode_header_field(out, &conn->encode_table, ":method", method) ||
        !hpack_encode_header_field(out, &conn->encode_table, ":scheme", scheme) ||
        !hpack_encode_header_field(out, &conn->encode_table, ":authority", authority) ||
        !hpack_encode_header_field(out, &conn->encode_table, ":path", path)) {
        h2_buf_free(out);
        return 0;
    }
    for (const rt_http2_header_t *it = headers; it; it = it->next) {
        if (!it->name || !it->value)
            continue;
        if (it->name[0] == ':')
            continue;
        if (strcasecmp(it->name, "host") == 0)
            continue;
        if (h2_header_is_connection_specific(it->name, it->value))
            continue;
        if (!h2_header_list_add_field(&header_list_size, it->name, it->value)) {
            h2_buf_free(out);
            return 0;
        }
        if (!hpack_encode_header_field(out, &conn->encode_table, it->name, it->value)) {
            h2_buf_free(out);
            return 0;
        }
    }
    if (header_list_size > conn->peer_max_header_list_size) {
        h2_buf_free(out);
        return 0;
    }
    if (out->len > conn->peer_max_header_list_size) {
        h2_buf_free(out);
        return 0;
    }
    return 1;
}

/// @brief Build and validate the HPACK block for an outbound response.
/// @details Emits `:status` first and skips caller-supplied pseudo-fields,
///          connection-specific fields, and null-valued nodes.
/// @param conn Connection providing encoder state and peer limits.
/// @param status HTTP status code in the range 100 through 599.
/// @param headers Optional regular header list.
/// @param out Receives an owned encoded block.
/// @return 1 on success; 0 on invalid input, invalid field data, limit violation, or allocation
///         failure.
static int h2_build_response_block(rt_http2_conn_t *conn,
                                   int status,
                                   const rt_http2_header_t *headers,
                                   h2_buf_t *out) {
    char status_buf[4];
    size_t header_list_size = 0;
    if (!conn || status < 100 || status > 599 || !out)
        return 0;
    snprintf(status_buf, sizeof(status_buf), "%d", status);
    if (!h2_header_list_add_field(&header_list_size, ":status", status_buf))
        return 0;
    memset(out, 0, sizeof(*out));
    if (!hpack_encode_header_field(out, &conn->encode_table, ":status", status_buf)) {
        h2_buf_free(out);
        return 0;
    }
    for (const rt_http2_header_t *it = headers; it; it = it->next) {
        if (!it->name || !it->value)
            continue;
        if (it->name[0] == ':')
            continue;
        if (h2_header_is_connection_specific(it->name, it->value))
            continue;
        if (!h2_header_list_add_field(&header_list_size, it->name, it->value)) {
            h2_buf_free(out);
            return 0;
        }
        if (!hpack_encode_header_field(out, &conn->encode_table, it->name, it->value)) {
            h2_buf_free(out);
            return 0;
        }
    }
    if (header_list_size > conn->peer_max_header_list_size) {
        h2_buf_free(out);
        return 0;
    }
    if (out->len > conn->peer_max_header_list_size) {
        h2_buf_free(out);
        return 0;
    }
    return 1;
}

/// @brief Allocate common client/server HTTP/2 state with RFC default limits.
/// @param io Valid callback table to copy; its context remains borrowed.
/// @param is_server Nonzero for server endpoint behavior, zero for client behavior.
/// @return New connection on success, or null for invalid callbacks or allocation failure.
static rt_http2_conn_t *h2_conn_new_common(const rt_http2_io_t *io, int is_server) {
    rt_http2_conn_t *conn = NULL;
    if (!io || !io->read || !io->write)
        return NULL;
    conn = (rt_http2_conn_t *)calloc(1, sizeof(*conn));
    if (!conn)
        return NULL;
    conn->io = *io;
    conn->is_server = is_server ? 1 : 0;
    conn->peer_initial_window = H2_DEFAULT_WINDOW_SIZE;
    conn->peer_max_frame_size = H2_DEFAULT_FRAME_SIZE;
    conn->local_max_frame_size = H2_DEFAULT_FRAME_SIZE;
    conn->peer_max_header_list_size = H2_MAX_HEADER_BLOCK;
    conn->local_max_header_list_size = H2_DEFAULT_MAX_HEADER_LIST_SIZE;
    conn->peer_conn_window = H2_DEFAULT_WINDOW_SIZE;
    conn->next_stream_id = 1;
    conn->encode_table.max_bytes = 4096;
    conn->decode_table.max_bytes = 4096;
    return conn;
}

/// @brief Allocate a client-side HTTP/2 connection.
/// @param io Callback table to copy; its context remains caller-owned.
/// @return New connection, or null for invalid callbacks or allocation failure.
rt_http2_conn_t *rt_http2_client_new(const rt_http2_io_t *io) {
    return h2_conn_new_common(io, 0);
}

/// @brief Allocate a server-side HTTP/2 connection.
/// @param io Callback table to copy; its context remains caller-owned.
/// @return New connection, or null for invalid callbacks or allocation failure.
rt_http2_conn_t *rt_http2_server_new(const rt_http2_io_t *io) {
    return h2_conn_new_common(io, 1);
}

/// @brief Release a connection and both of its HPACK dynamic tables.
/// @param conn Connection to release; may be null.
void rt_http2_conn_free(rt_http2_conn_t *conn) {
    if (!conn)
        return;
    hpack_dyn_table_free(&conn->encode_table);
    hpack_dyn_table_free(&conn->decode_table);
    free(conn);
}

/// @brief Read a connection's most recent diagnostic.
/// @param conn Connection to inspect; may be null.
/// @return Borrowed connection-owned message, `"no error"`, or a static null-connection message.
const char *rt_http2_get_error(const rt_http2_conn_t *conn) {
    if (!conn)
        return "HTTP/2: null connection";
    return conn->error[0] ? conn->error : "no error";
}

/// @brief Determine whether a connection remains open and has complete I/O callbacks.
/// @param conn Connection to inspect.
/// @return Nonzero when further protocol I/O may be attempted; zero otherwise.
int rt_http2_conn_is_usable(const rt_http2_conn_t *conn) {
    return conn && !conn->closed && conn->io.read && conn->io.write;
}

/// @brief Lazily send the client connection preface and initial SETTINGS.
/// @param conn Client connection to start.
/// @return 1 when already or newly started; 0 on invalid input or I/O failure.
static int h2_client_start(rt_http2_conn_t *conn) {
    if (!conn)
        return 0;
    if (conn->started)
        return 1;
    if (!h2_io_write_all(conn, (const uint8_t *)kClientPreface, sizeof(kClientPreface) - 1) ||
        !h2_send_settings(conn)) {
        return 0;
    }
    conn->started = 1;
    return 1;
}

/// @brief Lazily validate the client preface and send the server's initial SETTINGS.
/// @param conn Server connection to start.
/// @return 1 when already or newly started; 0 on invalid preface, invalid input, or I/O failure.
static int h2_server_start(rt_http2_conn_t *conn) {
    uint8_t preface[sizeof(kClientPreface) - 1];
    if (!conn)
        return 0;
    if (conn->started)
        return 1;
    if (!h2_io_read_exact(conn, preface, sizeof(preface)))
        return 0;
    if (memcmp(preface, kClientPreface, sizeof(preface)) != 0)
        return h2_conn_fail(conn, "HTTP/2: invalid client preface");
    if (!h2_send_settings(conn))
        return 0;
    conn->started = 1;
    return 1;
}

/// @brief Append received body bytes without exceeding a caller-defined limit.
/// @param body Accumulation buffer.
/// @param max_body_len Maximum permitted total bytes.
/// @param src Incoming body bytes; may be null only when @p len is zero.
/// @param len Number of incoming bytes.
/// @return 1 on success; 0 on invalid input, limit violation, overflow, or allocation failure.
static int h2_append_body(h2_buf_t *body, size_t max_body_len, const uint8_t *src, size_t len) {
    if (!body || (!src && len > 0))
        return 0;
    if (len == 0)
        return 1;
    if (body->len > max_body_len || len > max_body_len - body->len)
        return 0;
    return h2_buf_append(body, src, len);
}

/// @brief Send one request and synchronously receive its complete final response.
/// @details Allocates the next odd stream, processes connection-control frames
///          and informational responses, validates response pseudo-headers,
///          restores receive-window credit, and appends valid trailers.
/// @param conn Client connection.
/// @param method Request `:method`.
/// @param scheme Request `:scheme`.
/// @param authority Request `:authority`.
/// @param path Request `:path`.
/// @param headers Optional regular request headers.
/// @param body Optional request body bytes.
/// @param body_len Number of request body bytes.
/// @param max_body_len Maximum response body bytes accepted.
/// @param out_res Receives an owned complete response and is reset before processing.
/// @return 1 on success; 0 on invalid input, protocol error, limit violation, I/O failure, or
///         allocation failure.
int rt_http2_client_roundtrip(rt_http2_conn_t *conn,
                              const char *method,
                              const char *scheme,
                              const char *authority,
                              const char *path,
                              const rt_http2_header_t *headers,
                              const uint8_t *body,
                              size_t body_len,
                              size_t max_body_len,
                              rt_http2_response_t *out_res) {
    h2_buf_t req_block = {0};
    h2_buf_t res_body = {0};
    uint32_t stream_id = 0;
    int saw_response_headers = 0;
    if (!conn || !out_res)
        return 0;
    memset(out_res, 0, sizeof(*out_res));
    if (!h2_client_start(conn))
        return 0;
    if ((conn->next_stream_id & 1) == 0 || conn->next_stream_id <= 0)
        return h2_conn_fail(conn, "HTTP/2: invalid client stream id");
    stream_id = (uint32_t)conn->next_stream_id;
    conn->next_stream_id += 2;
    if (!h2_build_request_block(conn, method, scheme, authority, path, headers, &req_block))
        return h2_conn_fail(conn, "HTTP/2: failed to encode request headers");
    if (!h2_send_headers_block(conn, stream_id, req_block.data, req_block.len, body_len == 0) ||
        (body_len > 0 && !h2_send_data(conn, stream_id, body, body_len))) {
        h2_buf_free(&req_block);
        return 0;
    }
    h2_buf_free(&req_block);

    while (1) {
        h2_frame_t frame;
        int handled = 0;
        memset(&frame, 0, sizeof(frame));
        if (!h2_read_frame(conn, &frame)) {
            h2_buf_free(&res_body);
            rt_http2_response_free(out_res);
            return 0;
        }
        if (!h2_handle_common_frame(conn, &frame, stream_id, NULL, &handled)) {
            h2_frame_free(&frame);
            h2_buf_free(&res_body);
            rt_http2_response_free(out_res);
            return 0;
        }
        if (handled) {
            h2_frame_free(&frame);
            continue;
        }

        if (frame.type == H2_FRAME_HEADERS && frame.stream_id == stream_id) {
            rt_http2_header_t *decoded = NULL;
            int end_stream = 0;
            if (!h2_decode_header_list(
                    conn, &frame, &decoded, &end_stream, "HTTP/2: invalid response headers")) {
                h2_frame_free(&frame);
                h2_buf_free(&res_body);
                rt_http2_response_free(out_res);
                return 0;
            }
            if (!saw_response_headers) {
                int saw_status = 0;
                int status_tmp = 0;
                rt_http2_header_t *response_headers = NULL;
                for (rt_http2_header_t *it = decoded; it; it = it->next) {
                    if (strcmp(it->name, ":status") == 0) {
                        if (saw_status) {
                            rt_http2_headers_free(response_headers);
                            rt_http2_headers_free(decoded);
                            h2_frame_free(&frame);
                            h2_buf_free(&res_body);
                            rt_http2_response_free(out_res);
                            return h2_conn_fail(conn, "HTTP/2: duplicate response status");
                        }
                        if (!h2_parse_status_code(it->value, &status_tmp)) {
                            rt_http2_headers_free(response_headers);
                            rt_http2_headers_free(decoded);
                            h2_frame_free(&frame);
                            h2_buf_free(&res_body);
                            rt_http2_response_free(out_res);
                            return h2_conn_fail(conn, "HTTP/2: invalid response status");
                        }
                        saw_status = 1;
                    } else if (it->name[0] != ':') {
                        if (h2_header_is_connection_specific(it->name, it->value) ||
                            !rt_http2_header_append_copy(&response_headers, it->name, it->value)) {
                            rt_http2_headers_free(response_headers);
                            rt_http2_headers_free(decoded);
                            h2_frame_free(&frame);
                            h2_buf_free(&res_body);
                            rt_http2_response_free(out_res);
                            return h2_conn_fail(conn, "HTTP/2: invalid response header");
                        }
                    } else {
                        rt_http2_headers_free(response_headers);
                        rt_http2_headers_free(decoded);
                        h2_frame_free(&frame);
                        h2_buf_free(&res_body);
                        rt_http2_response_free(out_res);
                        return h2_conn_fail(conn, "HTTP/2: invalid response pseudo-header");
                    }
                }
                if (!saw_status) {
                    rt_http2_headers_free(response_headers);
                    rt_http2_headers_free(decoded);
                    h2_frame_free(&frame);
                    h2_buf_free(&res_body);
                    rt_http2_response_free(out_res);
                    return h2_conn_fail(conn, "HTTP/2: missing response status");
                }
                if (status_tmp >= 100 && status_tmp < 200) {
                    rt_http2_headers_free(response_headers);
                    if (status_tmp == 101 || end_stream) {
                        rt_http2_headers_free(decoded);
                        h2_frame_free(&frame);
                        h2_buf_free(&res_body);
                        rt_http2_response_free(out_res);
                        return h2_conn_fail(conn, "HTTP/2: invalid informational response");
                    }
                    rt_http2_headers_free(decoded);
                    h2_frame_free(&frame);
                    continue;
                }
                out_res->status = status_tmp;
                out_res->headers = response_headers;
                saw_response_headers = 1;
            } else {
                if (!h2_append_trailer_headers(&out_res->headers, decoded)) {
                    rt_http2_headers_free(decoded);
                    h2_frame_free(&frame);
                    h2_buf_free(&res_body);
                    rt_http2_response_free(out_res);
                    return h2_conn_fail(conn, "HTTP/2: invalid response trailers");
                }
                if (!end_stream) {
                    rt_http2_headers_free(decoded);
                    h2_frame_free(&frame);
                    h2_buf_free(&res_body);
                    rt_http2_response_free(out_res);
                    return h2_conn_fail(conn, "HTTP/2: response trailers missing END_STREAM");
                }
            }
            rt_http2_headers_free(decoded);
            if (end_stream) {
                out_res->stream_id = (int)stream_id;
                out_res->body = res_body.data;
                out_res->body_len = res_body.len;
                h2_frame_free(&frame);
                return 1;
            }
        } else if (frame.type == H2_FRAME_DATA && frame.stream_id == stream_id) {
            const uint8_t *data_ptr = NULL;
            size_t data_len = 0;
            int end_stream = 0;
            if (!saw_response_headers) {
                h2_frame_free(&frame);
                h2_buf_free(&res_body);
                rt_http2_response_free(out_res);
                return h2_conn_fail(conn, "HTTP/2: response DATA before HEADERS");
            }
            if (!h2_parse_data_payload(&frame, &data_ptr, &data_len, &end_stream) ||
                !h2_append_body(&res_body, max_body_len, data_ptr, data_len) ||
                (data_len > 0 && (!h2_send_window_update(conn, 0, (uint32_t)data_len) ||
                                  !h2_send_window_update(conn, stream_id, (uint32_t)data_len)))) {
                h2_frame_free(&frame);
                h2_buf_free(&res_body);
                rt_http2_response_free(out_res);
                return h2_conn_fail(conn, "HTTP/2: invalid response body");
            }
            if (end_stream) {
                out_res->stream_id = (int)stream_id;
                out_res->body = res_body.data;
                out_res->body_len = res_body.len;
                h2_frame_free(&frame);
                return 1;
            }
        } else {
            h2_frame_free(&frame);
            h2_buf_free(&res_body);
            rt_http2_response_free(out_res);
            return h2_conn_fail(conn, "HTTP/2: unexpected frame during response");
        }
        h2_frame_free(&frame);
    }
}

/// @brief Receive and validate the next complete request on a synchronous server connection.
/// @details Enforces odd client stream IDs, required pseudo-headers and their
///          ordering, body limits, flow-control updates, and trailer rules.
///          Additional concurrent request streams are refused.
/// @param conn Server connection.
/// @param max_body_len Maximum request body bytes accepted.
/// @param out_req Receives an owned complete request and is reset before processing.
/// @return 1 on success; 0 on invalid input, protocol error, limit violation, I/O failure, or
///         allocation failure.
int rt_http2_server_receive_request(rt_http2_conn_t *conn,
                                    size_t max_body_len,
                                    rt_http2_request_t *out_req) {
    h2_buf_t body = {0};
    uint32_t active_stream = 0;
    if (!conn || !out_req)
        return 0;
    memset(out_req, 0, sizeof(*out_req));
    if (!h2_server_start(conn))
        return 0;

    while (1) {
        h2_frame_t frame;
        int handled = 0;
        memset(&frame, 0, sizeof(frame));
        if (!h2_read_frame(conn, &frame)) {
            h2_buf_free(&body);
            rt_http2_request_free(out_req);
            return 0;
        }
        if (active_stream == 0) {
            if (!h2_handle_common_frame(conn, &frame, 0, NULL, &handled)) {
                h2_frame_free(&frame);
                h2_buf_free(&body);
                rt_http2_request_free(out_req);
                return 0;
            }
            if (handled) {
                h2_frame_free(&frame);
                continue;
            }
            if (frame.type != H2_FRAME_HEADERS || frame.stream_id == 0 ||
                (frame.stream_id & 1u) == 0u) {
                h2_frame_free(&frame);
                h2_buf_free(&body);
                rt_http2_request_free(out_req);
                return h2_conn_fail(conn, "HTTP/2: expected request HEADERS");
            }
            {
                rt_http2_header_t *decoded = NULL;
                int end_stream = 0;
                int saw_regular = 0;
                if (!h2_decode_header_list(
                        conn, &frame, &decoded, &end_stream, "HTTP/2: invalid request headers")) {
                    h2_frame_free(&frame);
                    h2_buf_free(&body);
                    rt_http2_request_free(out_req);
                    return 0;
                }
                active_stream = frame.stream_id;
                out_req->stream_id = (int)active_stream;
                for (rt_http2_header_t *it = decoded; it; it = it->next) {
                    if (it->name[0] == ':') {
                        if (saw_regular) {
                            rt_http2_headers_free(decoded);
                            h2_frame_free(&frame);
                            h2_buf_free(&body);
                            rt_http2_request_free(out_req);
                            return h2_conn_fail(conn, "HTTP/2: pseudo-header after regular header");
                        }
                        if (strcmp(it->name, ":method") == 0 && !out_req->method)
                            out_req->method = strdup(it->value);
                        else if (strcmp(it->name, ":scheme") == 0 && !out_req->scheme)
                            out_req->scheme = strdup(it->value);
                        else if (strcmp(it->name, ":authority") == 0 && !out_req->authority)
                            out_req->authority = strdup(it->value);
                        else if (strcmp(it->name, ":path") == 0 && !out_req->path)
                            out_req->path = strdup(it->value);
                        else {
                            rt_http2_headers_free(decoded);
                            h2_frame_free(&frame);
                            h2_buf_free(&body);
                            rt_http2_request_free(out_req);
                            return h2_conn_fail(conn, "HTTP/2: unsupported request pseudo-header");
                        }
                    } else {
                        saw_regular = 1;
                        if (h2_header_is_connection_specific(it->name, it->value) ||
                            !rt_http2_header_append_copy(&out_req->headers, it->name, it->value)) {
                            rt_http2_headers_free(decoded);
                            h2_frame_free(&frame);
                            h2_buf_free(&body);
                            rt_http2_request_free(out_req);
                            return h2_conn_fail(conn, "HTTP/2: invalid request header");
                        }
                    }
                }
                rt_http2_headers_free(decoded);
                if (!out_req->method || !out_req->scheme || !out_req->path) {
                    h2_frame_free(&frame);
                    h2_buf_free(&body);
                    rt_http2_request_free(out_req);
                    return h2_conn_fail(conn, "HTTP/2: incomplete request pseudo-headers");
                }
                if (out_req->authority && !rt_http2_header_get(out_req->headers, "host") &&
                    !rt_http2_header_append_copy(&out_req->headers, "host", out_req->authority)) {
                    h2_frame_free(&frame);
                    h2_buf_free(&body);
                    rt_http2_request_free(out_req);
                    return h2_conn_fail(conn, "HTTP/2: host header allocation failed");
                }
                if (end_stream) {
                    out_req->body = body.data;
                    out_req->body_len = body.len;
                    h2_frame_free(&frame);
                    return 1;
                }
            }
        } else {
            if (!h2_handle_common_frame(conn, &frame, active_stream, NULL, &handled)) {
                h2_frame_free(&frame);
                h2_buf_free(&body);
                rt_http2_request_free(out_req);
                return 0;
            }
            if (handled) {
                h2_frame_free(&frame);
                continue;
            }
            if (frame.stream_id != active_stream) {
                if (frame.stream_id != 0 && (frame.stream_id & 1u) != 0u &&
                    (frame.type == H2_FRAME_HEADERS || frame.type == H2_FRAME_DATA)) {
                    if (!h2_refuse_concurrent_request_stream(conn, &frame)) {
                        h2_frame_free(&frame);
                        h2_buf_free(&body);
                        rt_http2_request_free(out_req);
                        return 0;
                    }
                    h2_frame_free(&frame);
                    continue;
                }
                h2_frame_free(&frame);
                h2_buf_free(&body);
                rt_http2_request_free(out_req);
                return h2_conn_fail(conn, "HTTP/2: unexpected frame on unrelated request stream");
            }
            if (frame.type == H2_FRAME_HEADERS) {
                rt_http2_header_t *decoded = NULL;
                int end_stream = 0;
                if (!h2_decode_header_list(
                        conn, &frame, &decoded, &end_stream, "HTTP/2: invalid request trailers")) {
                    h2_frame_free(&frame);
                    h2_buf_free(&body);
                    rt_http2_request_free(out_req);
                    return 0;
                }
                if (!h2_append_trailer_headers(&out_req->headers, decoded)) {
                    rt_http2_headers_free(decoded);
                    h2_frame_free(&frame);
                    h2_buf_free(&body);
                    rt_http2_request_free(out_req);
                    return h2_conn_fail(conn, "HTTP/2: invalid request trailers");
                }
                rt_http2_headers_free(decoded);
                if (!end_stream) {
                    h2_frame_free(&frame);
                    h2_buf_free(&body);
                    rt_http2_request_free(out_req);
                    return h2_conn_fail(conn, "HTTP/2: request trailers missing END_STREAM");
                }
                out_req->body = body.data;
                out_req->body_len = body.len;
                h2_frame_free(&frame);
                return 1;
            } else if (frame.type == H2_FRAME_DATA) {
                const uint8_t *data_ptr = NULL;
                size_t data_len = 0;
                int end_stream = 0;
                if (!h2_parse_data_payload(&frame, &data_ptr, &data_len, &end_stream) ||
                    !h2_append_body(&body, max_body_len, data_ptr, data_len) ||
                    (data_len > 0 &&
                     (!h2_send_window_update(conn, 0, (uint32_t)data_len) ||
                      !h2_send_window_update(conn, active_stream, (uint32_t)data_len)))) {
                    h2_frame_free(&frame);
                    h2_buf_free(&body);
                    rt_http2_request_free(out_req);
                    return h2_conn_fail(conn, "HTTP/2: invalid request body");
                }
                if (end_stream) {
                    out_req->body = body.data;
                    out_req->body_len = body.len;
                    h2_frame_free(&frame);
                    return 1;
                }
            } else {
                h2_frame_free(&frame);
                h2_buf_free(&body);
                rt_http2_request_free(out_req);
                return h2_conn_fail(conn, "HTTP/2: unexpected frame in request body");
            }
        }
        h2_frame_free(&frame);
    }
}

/// @brief Encode and send a complete response on a server-side stream.
/// @param conn Server connection.
/// @param stream_id Positive request stream identifier.
/// @param status HTTP status code in the range 100 through 599.
/// @param headers Optional regular response headers.
/// @param body Optional response body bytes.
/// @param body_len Number of response body bytes.
/// @return 1 on success; 0 on invalid input, encoding failure, protocol error, or I/O failure.
int rt_http2_server_send_response(rt_http2_conn_t *conn,
                                  int stream_id,
                                  int status,
                                  const rt_http2_header_t *headers,
                                  const uint8_t *body,
                                  size_t body_len) {
    h2_buf_t block = {0};
    if (!conn || stream_id <= 0)
        return 0;
    if (!h2_build_response_block(conn, status, headers, &block))
        return h2_conn_fail(conn, "HTTP/2: failed to encode response headers");
    if (!h2_send_headers_block(conn, (uint32_t)stream_id, block.data, block.len, body_len == 0) ||
        (body_len > 0 && !h2_send_data(conn, (uint32_t)stream_id, body, body_len))) {
        h2_buf_free(&block);
        return 0;
    }
    h2_buf_free(&block);
    return 1;
}
