//===----------------------------------------------------------------------===//

//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_wss_server.c
// Purpose: TLS-backed WebSocket server with broadcast and per-client messaging.
// Key invariants:
//   - Stable receiver identity and serialized Start/Stop protect every lifecycle edge.
//   - Raw, pending-TLS, and upgraded transports remain generation-tagged and
//     visible to Stop; workers alone dispose TLS state after native shutdown.
//   - Strict HTTP/frame/UTF-8 policy is shared with the plain server.
// Ownership/Lifetime:
//   - Managed slot references stabilize transport pointers; worker references
//     own final close. Stop joins/releases the pool and permits clean restart.
// Links: rt_wss_server.h, rt_ws_shared.inc, rt_tls_server_internal.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_wss_server.c
 * @brief Implements the managed TLS-backed WebSocket server.
 * @details The restartable lifecycle combines a listener, immutable TLS
 *          credentials, bounded workers, and generation-tagged raw,
 *          handshaking, or upgraded client slots. Workers share strict HTTP,
 *          frame, and UTF-8 policy with the plaintext server and exclusively
 *          dispose their TLS sessions after cancellation.
 */

#include "rt_wss_server.h"
#include "rt_platform.h"
#include "rt_websocket.h"

#include "rt_bytes.h"
#include "rt_crypto.h"
#include "rt_internal.h"
#include "rt_network_internal.h"
#include "rt_object.h"
#include "rt_socket_platform.h"
#include "rt_string.h"
#include "rt_threadpool.h"
#include "rt_tls_server_internal.h"
#include "rt_win32_wait.h"
#include "system/rt_machine.h"

#include <limits.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if RT_PLATFORM_WINDOWS
#include <process.h>
typedef CRITICAL_SECTION ws_mutex_t;
#else
#include <pthread.h>
typedef pthread_mutex_t ws_mutex_t;
#endif

#if RT_PLATFORM_WINDOWS
#define WS_MUTEX_INIT(m) (InitializeCriticalSectionEx((m), 0, 0) ? 0 : -1)
#define WS_MUTEX_LOCK(m) EnterCriticalSection(m)
#define WS_MUTEX_UNLOCK(m) LeaveCriticalSection(m)
#define WS_MUTEX_DESTROY(m) DeleteCriticalSection(m)
#else
#define WS_MUTEX_INIT(m) pthread_mutex_init(m, NULL)
#define WS_MUTEX_LOCK(m) pthread_mutex_lock(m)
#define WS_MUTEX_UNLOCK(m) pthread_mutex_unlock(m)
#define WS_MUTEX_DESTROY(m) pthread_mutex_destroy(m)
#endif

#include "rt_trap.h"
extern char *rt_ws_compute_accept_key(const char *key_cstr); // from rt_websocket.c
extern int rt_trap_get_net_code(void);

#if defined(__GNUC__) || defined(__clang__)
#define WSS_MAYBE_UNUSED __attribute__((unused))
#else
#define WSS_MAYBE_UNUSED
#endif

//=============================================================================
// Internal Structures
//=============================================================================

#define WS_SERVER_MAX_CLIENTS 128
#define WS_MAX_HANDSHAKE_HEADERS 100
#define WS_MAX_HANDSHAKE_BYTES (16u * 1024u)
#define WS_MAX_MESSAGE_BYTES (64u * 1024u * 1024u)
#define WS_CLIENT_SEND_TIMEOUT_MS 2000

// WebSocket opcodes (duplicated from rt_websocket.c to avoid dependency)
#define WS_OP_TEXT 0x01
#define WS_OP_BINARY 0x02
#define WS_OP_CONTINUATION 0x00
#define WS_OP_CLOSE 0x08
#define WS_OP_PING 0x09
#define WS_OP_PONG 0x0A
#define WS_FIN 0x80
#define WS_MASK 0x80
#define WS_CLOSE_PROTOCOL_ERROR 1002
#define WS_CLOSE_INVALID_DATA 1007

/// @brief Compute the internal worker-pool size for WSS server instances.
/// @details Uses the runtime machine adapter to query logical CPU count, then
///          clamps the result to the range accepted by @ref rt_threadpool_new.
///          This gives the TLS WebSocket server CPU-aware defaults without
///          adding another public runtime helper.
/// @return Worker count in the inclusive range 4..32.
static int64_t wss_server_default_worker_count(void) {
    int64_t cores = rt_machine_cores();
    if (cores < 1)
        cores = 1;
    // Each serviced client occupies a worker for its lifetime, so give small
    // machines a floor and everyone headroom; the cap bounds thread count.
    int64_t workers = cores * 2;
    if (workers < 4)
        workers = 4;
    if (workers > 32)
        workers = 32;
    return workers;
}

typedef struct {
    void *tcp; ///< Retained queued TCP object or upgraded TLS session.
    uint64_t generation;
    bool occupied;
    bool active;
    bool tls_transport;
    socket_t pending_socket; ///< Detached descriptor currently owned by TLS accept.
    bool pending_socket_valid;
    ws_mutex_t io; ///< Per-client write lock: broadcasts and worker control
                   ///< frames serialize per socket, not globally (VDOC-149).
} ws_client_t;

typedef struct {
    void *tcp_server;
    void *worker_pool;
    rt_tls_server_ctx_t *tls_ctx;
    char *cert_file;
    char *key_file;
    char *subprotocol;
    int64_t port;
    bool running;
    ws_client_t clients[WS_SERVER_MAX_CLIENTS];
    int client_count;
    ws_mutex_t lifecycle;
    ws_mutex_t lock;
    bool lifecycle_initialized;
    bool lock_initialized;
    int client_io_count; ///< Number of successfully initialized slot mutexes.
#if RT_PLATFORM_WINDOWS
    HANDLE accept_thread;
#else
    pthread_t accept_thread;
#endif
    bool thread_started;
} rt_ws_server_impl;

/// @brief Validate and cast a public TLS WebSocket-server receiver.
/// @details Stable identity and the complete private payload are checked before
///          any lifecycle mutex, TLS context, credential, listener, or client
///          field is read. Invalid handles emit one trap and stop locally.
/// @param object Candidate managed WssServer handle.
/// @param operation Operation-specific diagnostic.
/// @return Valid private payload, or NULL after trapping.
static rt_ws_server_impl *ws_server_require(void *object, const char *operation) {
    if (!rt_obj_is_instance(object, RT_WSS_SERVER_CLASS_ID, sizeof(rt_ws_server_impl))) {
        rt_trap(operation ? operation : "WssServer: invalid object");
        return NULL;
    }
    return (rt_ws_server_impl *)object;
}

typedef struct {
    rt_ws_server_impl *server;
    void *tcp;
    int slot;
    uint64_t generation;
} ws_accept_task_t;

typedef struct {
    int slot;
    void *tcp;
    uint64_t generation;
} ws_broadcast_target_t;

#include "rt_ws_shared.inc"

/// @brief Read a WSS server's running flag under its state mutex.
///
/// The accept worker can race with `Stop`, which clears `running` and closes the
/// listener from another thread. This helper gives post-accept and post-TLS
/// checks the same synchronization used for the client table, so workers stop
/// before completing a handshake when shutdown has already begun.
///
/// @param s WSS server implementation.
/// @return Non-zero if the server is still accepting clients.
static int ws_server_is_running_locked(rt_ws_server_impl *s) {
    int running;
    if (!s)
        return 0;
    WS_MUTEX_LOCK(&s->lock);
    running = s->running ? 1 : 0;
    WS_MUTEX_UNLOCK(&s->lock);
    return running;
}

/// @brief Cooperatively cancel a TLS accept after the WSS lifecycle stops.
/// @details The TLS layer invokes this probe between bounded socket waits. It
///          avoids relying on a cross-thread shutdown to cancel a synchronous
///          WinSock receive, which is not guaranteed by the provider.
/// @param context Owning @ref rt_ws_server_impl.
/// @return Non-zero once new client handshakes must stop.
static int ws_tls_accept_cancel_requested(void *context) {
    rt_ws_server_impl *s = (rt_ws_server_impl *)context;
    return !s || !ws_server_is_running_locked(s);
}

/// @brief Check whether a TLS client still exposes a live native socket.
/// @param[in] tcp TLS session pointer, or NULL.
/// @return 1 when the underlying socket is valid; otherwise 0.
static int ws_tls_is_open(void *tcp) {
    return tcp && (socket_t)rt_tls_get_socket((rt_tls_session_t *)tcp) != INVALID_SOCK;
}

/// @brief Close an owned TLS client session and clear its pointer.
/// @param[in,out] tcp_ptr Address of the owned TLS session pointer.
static void ws_release_tcp(void **tcp_ptr) {
    if (!tcp_ptr || !*tcp_ptr)
        return;
    rt_tls_close((rt_tls_session_t *)*tcp_ptr);
    *tcp_ptr = NULL;
}

/// @brief Close and release an owned managed TCP connection.
/// @param[in,out] tcp_ptr Address of the owned managed handle; cleared on return.
static void ws_release_raw_tcp(void **tcp_ptr) {
    if (!tcp_ptr || !*tcp_ptr)
        return;
    rt_tcp_close(*tcp_ptr);
    if (rt_obj_release_check0(*tcp_ptr))
        rt_obj_free(*tcp_ptr);
    *tcp_ptr = NULL;
}

/// @brief Release one retained managed transport reference without closing it.
/// @details Client slots retain a TCP/TLS object solely to make their pointer
///          stable while a worker owns the producer reference. Removing the
///          slot must drop that reference without disposing TLS buffers that a
///          concurrent worker may still be reading. The worker performs the
///          final protocol-aware close after it leaves its receive loop.
/// @param transport_ptr Address of the retained slot pointer; cleared on return.
static void ws_release_transport_reference(void **transport_ptr) {
    if (!transport_ptr || !*transport_ptr)
        return;
    void *transport = *transport_ptr;
    *transport_ptr = NULL;
    if (rt_obj_release_check0(transport))
        rt_obj_free(transport);
}

/// @brief Interrupt a slot transport and drop only the slot-owned reference.
/// @details The slot io and server state mutexes must be held. Raw queued TCP
///          objects are closed through their public transport close path. TLS
///          sessions are interrupted with a native bidirectional shutdown so
///          a blocked worker wakes, while the worker retains sole ownership of
///          TLS state and performs @ref rt_tls_close itself.
/// @param client Occupied slot to interrupt; NULL is accepted.
static void ws_interrupt_slot_transport_locked(ws_client_t *client) {
    if (!client)
        return;
    if (client->pending_socket_valid && client->pending_socket != INVALID_SOCK)
        (void)rt_socket_shutdown_both(client->pending_socket);
    if (!client->tcp)
        return;
    if (client->tls_transport) {
        socket_t fd = (socket_t)rt_tls_get_socket((rt_tls_session_t *)client->tcp);
        if (fd != INVALID_SOCK)
            (void)rt_socket_shutdown_both(fd);
        ws_release_transport_reference(&client->tcp);
    } else {
        ws_release_raw_tcp(&client->tcp);
    }
}

/// @brief Send an exact native byte sequence through a TLS session.
/// @param[in,out] tcp Valid TLS session.
/// @param[in] data Bytes to transmit.
/// @param[in] len Exact number of bytes to send.
/// @return 1 after all bytes are sent; otherwise 0.
static int ws_server_send_raw(void *tcp, const void *data, size_t len) {
    size_t total = 0;
    while (total < len) {
        size_t remaining = len - total;
        long sent = rt_tls_send((rt_tls_session_t *)tcp, (const uint8_t *)data + total, remaining);
        if (sent <= 0)
            return 0;
        total += (size_t)sent;
    }
    return 1;
}

/// @brief Receive an exact native byte sequence through a TLS session.
/// @param[in,out] tcp Valid TLS session.
/// @param[out] buf Destination for exactly @p len bytes.
/// @param[in] len Number of bytes required.
/// @return 1 after filling @p buf; otherwise 0 on EOF or TLS failure.
static int ws_tls_recv_exact(void *tcp, uint8_t *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        long got = rt_tls_recv((rt_tls_session_t *)tcp, buf + total, len - total);
        if (got <= 0)
            return 0;
        total += (size_t)got;
    }
    return 1;
}

/// @brief Read one strictly CRLF-terminated HTTP line over TLS.
/// @details Bare LF, stray CR, embedded NUL, over-limit input, EOF, and native
///          allocation failure reject the unauthenticated upgrade. Returned
///          storage is NUL terminated and caller-owned.
/// @param tcp Valid TLS session.
/// @param max_len Maximum bytes excluding CRLF; zero permits only an empty line.
/// @param len_out Receives the exact line byte length.
/// @return Caller-owned line or NULL on rejection/failure.
static char *ws_tls_recv_line_strict(void *tcp, size_t max_len, size_t *len_out) {
    size_t cap = max_len < 128u ? max_len + 1u : 128u;
    size_t len = 0;
    if (!len_out)
        return NULL;
    char *buf = (char *)malloc(cap);
    if (!buf)
        return NULL;
    *len_out = 0;

    while (1) {
        uint8_t byte = 0;
        if (!ws_tls_recv_exact(tcp, &byte, 1)) {
            free(buf);
            return NULL;
        }
        if (byte == '\r') {
            uint8_t lf = 0;
            if (!ws_tls_recv_exact(tcp, &lf, 1) || lf != '\n') {
                free(buf);
                return NULL;
            }
            break;
        }
        if (byte == '\n' || byte == '\0' || len >= max_len) {
            free(buf);
            return NULL;
        }
        if (len + 1 >= cap) {
            size_t next_cap = cap > max_len / 2u ? max_len + 1u : cap * 2u;
            char *grown = (char *)realloc(buf, next_cap);
            if (!grown) {
                free(buf);
                return NULL;
            }
            buf = grown;
            cap = next_cap;
        }
        buf[len++] = (char)byte;
    }

    buf[len] = '\0';
    *len_out = len;
    return buf;
}

//=============================================================================
// WebSocket Framing Helpers
//=============================================================================

/// @brief Send a WebSocket frame over a raw TCP connection (server-side: no masking).
/// @details Emits one final frame through TLS using the canonical shortest
///          payload-length representation and no server-side masking key.
/// @param[in,out] tcp Valid TLS session.
/// @param[in] opcode Frame opcode.
/// @param[in] data Payload bytes, or NULL when @p len is zero.
/// @param[in] len Payload size in bytes.
/// @return 1 when the complete frame is sent; otherwise 0.
static int ws_server_send_frame(void *tcp, uint8_t opcode, const void *data, size_t len) {
    uint8_t header[10];
    size_t header_len = 2;

    if (!tcp || (len > 0 && !data))
        return 0;
    header[0] = WS_FIN | opcode;

    // Server frames are NOT masked (RFC 6455 §5.1)
    if (len < 126) {
        header[1] = (uint8_t)len;
    } else if (len < 65536) {
        header[1] = 126;
        header[2] = (uint8_t)(len >> 8);
        header[3] = (uint8_t)(len);
        header_len = 4;
    } else {
        header[1] = 127;
        ws_encode_u64_len(header + 2, len);
        header_len = 10;
    }

    // Send header
    if (!ws_server_send_raw(tcp, header, header_len))
        return 0;

    // Send data
    if (len > 0 && data)
        return ws_server_send_raw(tcp, data, len);

    return 1;
}

/// @brief Read a WebSocket frame from a TCP connection (client frames are masked).
/// @details Enforces reserved-bit, opcode, client masking, canonical length,
///          control-frame, message-size, close-code, and close-reason rules,
///          replying with an appropriate Close frame on protocol violations.
/// @param[in,out] tcp Valid TLS session.
/// @param[out] fin_out Receives whether the FIN bit is set.
/// @param[out] opcode_out Receives the validated opcode.
/// @param[out] data_out Receives an allocated unmasked payload, or NULL when empty.
/// @param[out] len_out Receives the payload size in bytes.
/// @return 1 on success; otherwise 0 after transport, allocation, or protocol failure.
static int ws_server_recv_frame(
    void *tcp, uint8_t *fin_out, uint8_t *opcode_out, uint8_t **data_out, size_t *len_out) {
    uint8_t header[2];
    uint8_t ext8[8];
    uint8_t mask[4];
    *data_out = NULL;
    *len_out = 0;

    if (!ws_tls_recv_exact(tcp, header, sizeof(header)))
        return 0;
    uint8_t first = header[0];
    uint8_t second = header[1];
    *fin_out = (first & WS_FIN) ? 1 : 0;
    *opcode_out = first & 0x0F;
    uint8_t masked = second & WS_MASK;
    size_t payload_len = second & 0x7F;

    if ((first & 0x70) != 0 || !ws_is_valid_opcode(*opcode_out)) {
        ws_server_send_frame(tcp, WS_OP_CLOSE, "\x03\xEA", 2);
        return 0;
    }

    if (!masked) {
        ws_server_send_frame(tcp, WS_OP_CLOSE, "\x03\xEA", 2);
        return 0;
    }

    // Extended length
    if (payload_len == 126) {
        if (!ws_tls_recv_exact(tcp, ext8, 2))
            return 0;
        payload_len = ((size_t)ext8[0] << 8) | ext8[1];
        if (payload_len < 126) {
            ws_server_send_frame(tcp, WS_OP_CLOSE, "\x03\xEA", 2);
            return 0;
        }
    } else if (payload_len == 127) {
        if (!ws_tls_recv_exact(tcp, ext8, 8))
            return 0;
        if (!ws_decode_u64_len(ext8, &payload_len)) {
            ws_server_send_frame(tcp, WS_OP_CLOSE, "\x03\xEA", 2);
            return 0;
        }
        if (payload_len < 65536) {
            ws_server_send_frame(tcp, WS_OP_CLOSE, "\x03\xEA", 2);
            return 0;
        }
    }

    if (*opcode_out >= 0x08 && (!*fin_out || payload_len > 125)) {
        ws_server_send_frame(tcp, WS_OP_CLOSE, "\x03\xEA", 2);
        return 0;
    }

    if (payload_len > WS_MAX_MESSAGE_BYTES) {
        ws_server_send_frame(tcp, WS_OP_CLOSE, "\x03\xF1", 2);
        return 0; // Too large
    }

    if (!ws_tls_recv_exact(tcp, mask, sizeof(mask)))
        return 0;

    // Read payload
    if (payload_len > 0) {
        *data_out = (uint8_t *)malloc(payload_len);
        if (!*data_out) {
            ws_server_send_frame(tcp, WS_OP_CLOSE, "\x03\xF3", 2);
            return 0;
        }
        if (!ws_tls_recv_exact(tcp, *data_out, payload_len)) {
            free(*data_out);
            *data_out = NULL;
            return 0;
        }

        // Unmask
        for (size_t i = 0; i < payload_len; i++)
            (*data_out)[i] ^= mask[i & 3u];
        *len_out = payload_len;
    }

    if (*opcode_out == WS_OP_CLOSE) {
        if (payload_len == 1) {
            free(*data_out);
            *data_out = NULL;
            *len_out = 0;
            ws_server_send_frame(tcp, WS_OP_CLOSE, "\x03\xEA", 2);
            return 0;
        }
        if (payload_len >= 2) {
            uint16_t close_code = ((uint16_t)(*data_out)[0] << 8) | (*data_out)[1];
            if (!ws_close_code_is_valid(close_code) ||
                (payload_len > 2 && !ws_is_valid_utf8(*data_out + 2, payload_len - 2))) {
                free(*data_out);
                *data_out = NULL;
                *len_out = 0;
                ws_server_send_frame(tcp,
                                     WS_OP_CLOSE,
                                     ws_close_code_is_valid(close_code) ? "\x03\xEF" : "\x03\xEA",
                                     2);
                return 0;
            }
        }
    }

    return 1;
}

/// @brief Perform server-side WebSocket upgrade handshake.
/// @details Reads a strictly bounded CRLF HTTP request over TLS, validates the
///          shared RFC 6455 fields, HTTPS origin authority, and optional exact
///          subprotocol, then constructs and attempts the 101 response.
/// @param[in,out] tcp Established TLS session.
/// @param[in] required_subprotocol Optional exact protocol token the request must offer.
/// @return 1 after accepting and formatting a valid upgrade, or 0 on parsing,
///         validation, allocation, or formatting failure.
static int ws_server_handshake(void *tcp, const char *required_subprotocol) {
    ws_handshake_headers_t headers;
    size_t total_bytes = 0;
    size_t line_len = 0;
    memset(&headers, 0, sizeof(headers));

    char *line = ws_tls_recv_line_strict(tcp, WS_MAX_HANDSHAKE_BYTES - 2u, &line_len);
    if (!line)
        return 0;
    total_bytes = line_len + 2u;
    if (!ws_request_line_is_valid(line)) {
        free(line);
        return 0;
    }
    free(line);

    int header_lines = 0;
    while (1) {
        if (++header_lines > WS_MAX_HANDSHAKE_HEADERS)
            return 0;
        if (total_bytes > WS_MAX_HANDSHAKE_BYTES - 2u)
            return 0;
        size_t remaining = WS_MAX_HANDSHAKE_BYTES - total_bytes - 2u;
        line = ws_tls_recv_line_strict(tcp, remaining, &line_len);
        if (!line)
            return 0;
        total_bytes += line_len + 2u;
        if (line_len == 0) {
            free(line);
            break;
        }
        if (!ws_handshake_parse_header(&headers, line)) {
            free(line);
            return 0;
        }
        free(line);
    }

    if (!ws_handshake_headers_are_valid(&headers, required_subprotocol, "https", 443))
        return 0;

    // Compute accept key
    char *accept = rt_ws_compute_accept_key(headers.key);
    if (!accept)
        return 0;

    // Send upgrade response
    char response[768];
    int rlen = snprintf(response,
                        sizeof(response),
                        "HTTP/1.1 101 Switching Protocols\r\n"
                        "Upgrade: websocket\r\n"
                        "Connection: Upgrade\r\n"
                        "Sec-WebSocket-Accept: %s\r\n",
                        accept);
    free(accept);
    if (rlen <= 0 || (size_t)rlen >= sizeof(response))
        return 0;
    if (required_subprotocol && *required_subprotocol) {
        int wrote = snprintf(response + rlen,
                             sizeof(response) - (size_t)rlen,
                             "Sec-WebSocket-Protocol: %s\r\n",
                             required_subprotocol);
        if (wrote <= 0 || (size_t)wrote >= sizeof(response) - (size_t)rlen)
            return 0;
        rlen += wrote;
    }
    if ((size_t)rlen + 2 >= sizeof(response))
        return 0;
    memcpy(response + rlen, "\r\n", 3);
    rlen += 2;

    ws_server_send_raw(tcp, response, (size_t)rlen);

    return 1;
}

//=============================================================================
// Finalizer
//=============================================================================

/// @brief GC finalizer: stop the accept thread and close all client connections, then destroy the
/// platform mutex. Calling `_stop` first is idempotent, so this is safe even if the user already
/// stopped the server explicitly.
/// @param[in,out] obj Managed WSS server payload, or NULL.
static void rt_ws_server_finalize(void *obj) {
    if (!obj)
        return;
    rt_ws_server_impl *s = (rt_ws_server_impl *)obj;
    rt_wss_server_stop(s);
    rt_tls_server_ctx_free(s->tls_ctx);
    s->tls_ctx = NULL;
    free(s->cert_file);
    free(s->key_file);
    free(s->subprotocol);
    s->cert_file = NULL;
    s->key_file = NULL;
    s->subprotocol = NULL;
    if (s->worker_pool && rt_obj_release_check0(s->worker_pool))
        rt_obj_free(s->worker_pool);
    s->worker_pool = NULL;
    if (s->client_io_count > 0) {
        for (int i = 0; i < s->client_io_count; i++)
            WS_MUTEX_DESTROY(&s->clients[i].io);
    }
    if (s->lock_initialized)
        WS_MUTEX_DESTROY(&s->lock);
    if (s->lifecycle_initialized)
        WS_MUTEX_DESTROY(&s->lifecycle);
}

/// @brief Retire one generation of an occupied WSS client slot.
/// @details Generation matching prevents a delayed worker from clearing a
///          newly reused slot. The slot reference is released without closing
///          live TLS state; the worker owns the closing reference. A NULL slot
///          pointer is also retired because Stop or a failed broadcast may
///          already have interrupted and detached that transport.
/// @param s Owning server implementation.
/// @param slot Fixed-table slot index.
/// @param generation Generation captured when the accept task was queued.
/// @param tcp Worker-owned transport expected in the slot, or NULL during the
///            raw-to-TLS handoff gap.
static void ws_server_remove_client(rt_ws_server_impl *s,
                                    int slot,
                                    uint64_t generation,
                                    void *tcp) {
    if (!s || slot < 0 || slot >= WS_SERVER_MAX_CLIENTS)
        return;

    WS_MUTEX_LOCK(&s->clients[slot].io);
    WS_MUTEX_LOCK(&s->lock);
    if (slot < s->client_count && s->clients[slot].occupied &&
        s->clients[slot].generation == generation &&
        (!s->clients[slot].tcp || !tcp || s->clients[slot].tcp == tcp)) {
        if (s->clients[slot].tcp == tcp)
            ws_release_transport_reference(&s->clients[slot].tcp);
        s->clients[slot].active = false;
        s->clients[slot].occupied = false;
        s->clients[slot].tls_transport = false;
        s->clients[slot].pending_socket = INVALID_SOCK;
        s->clients[slot].pending_socket_valid = false;
    }
    WS_MUTEX_UNLOCK(&s->lock);
    WS_MUTEX_UNLOCK(&s->clients[slot].io);
}

/// @brief Clear a detached descriptor published during raw-to-TLS handoff.
/// @details Generation matching prevents delayed failure cleanup from erasing
///          a descriptor belonging to a reused slot. The function only edits
///          visibility metadata; descriptor ownership remains with either the
///          in-progress TLS accept or its explicit trap cleanup path.
/// @param s Owning server.
/// @param slot Fixed client-table index.
/// @param generation Reserved slot generation.
static void ws_server_clear_pending_socket(rt_ws_server_impl *s, int slot, uint64_t generation) {
    if (!s || slot < 0 || slot >= WS_SERVER_MAX_CLIENTS)
        return;
    WS_MUTEX_LOCK(&s->clients[slot].io);
    WS_MUTEX_LOCK(&s->lock);
    if (slot < s->client_count && s->clients[slot].occupied &&
        s->clients[slot].generation == generation) {
        s->clients[slot].pending_socket = INVALID_SOCK;
        s->clients[slot].pending_socket_valid = false;
    }
    WS_MUTEX_UNLOCK(&s->lock);
    WS_MUTEX_UNLOCK(&s->clients[slot].io);
}

/// @brief Send a frame under the client's own io lock so a slow TLS peer
///        only stalls its own send, never other clients' (VDOC-149).
/// @param[in,out] s Owning WSS server payload.
/// @param[in] slot Client slot whose I/O mutex serializes the frame.
/// @param[in,out] tcp TLS client session associated with @p slot.
/// @param[in] opcode Frame opcode.
/// @param[in] data Payload bytes, or NULL when @p len is zero.
/// @param[in] len Payload size in bytes.
/// @return 1 when the frame is sent; otherwise 0, including invalid arguments.
static int ws_server_send_locked(
    rt_ws_server_impl *s, int slot, void *tcp, uint8_t opcode, const void *data, size_t len) {
    int ok = 0;
    if (!s || !tcp || slot < 0 || slot >= WS_SERVER_MAX_CLIENTS)
        return 0;
    WS_MUTEX_LOCK(&s->clients[slot].io);
    ok = ws_server_send_frame(tcp, opcode, data, len);
    WS_MUTEX_UNLOCK(&s->clients[slot].io);
    return ok;
}

/// @brief Service one upgraded TLS WebSocket until shutdown or protocol error.
/// @details Runtime traps caused by peer-controlled I/O are recovered inside
///          the worker so one disconnected client cannot escape the pool
///          thread or bypass generation-safe slot cleanup.
/// @param s Owning WSS server.
/// @param slot Reserved fixed-table slot.
/// @param generation Slot generation captured at accept time.
/// @param tcp Worker-owned TLS session.
static void ws_client_run(rt_ws_server_impl *s, int slot, uint64_t generation, void *tcp) {
    if (!s || !tcp || slot < 0)
        return;

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        rt_trap_clear_recovery();
        ws_server_remove_client(s, slot, generation, tcp);
        return;
    }

    while (ws_server_is_running_locked(s) && ws_tls_is_open(tcp)) {
        uint8_t fin = 0;
        uint8_t opcode = 0;
        uint8_t *data = NULL;
        size_t len = 0;

        if (!ws_server_recv_frame(tcp, &fin, &opcode, &data, &len))
            break;

        if (opcode == WS_OP_PING) {
            if (!ws_server_send_locked(s, slot, tcp, WS_OP_PONG, data, len)) {
                free(data);
                break;
            }
            free(data);
            continue;
        }

        if (opcode == WS_OP_PONG) {
            free(data);
            continue;
        }

        if (opcode == WS_OP_CLOSE) {
            ws_server_send_locked(s, slot, tcp, WS_OP_CLOSE, data, len);
            free(data);
            break;
        }

        if (opcode == WS_OP_CONTINUATION) {
            free(data);
            ws_server_send_locked(s, slot, tcp, WS_OP_CLOSE, "\x03\xEA", 2);
            break;
        }

        const int text_message = opcode == WS_OP_TEXT;
        ws_utf8_state_t utf8;
        ws_utf8_state_init(&utf8);
        if (text_message && !ws_utf8_state_feed(&utf8, data, len)) {
            free(data);
            ws_server_send_locked(s, slot, tcp, WS_OP_CLOSE, "\x03\xEF", 2);
            break;
        }
        size_t message_len = len;
        free(data);

        if (fin) {
            if (text_message && !ws_utf8_state_is_complete(&utf8)) {
                ws_server_send_locked(s, slot, tcp, WS_OP_CLOSE, "\x03\xEF", 2);
                break;
            }
            continue;
        }

        while (ws_server_is_running_locked(s) && ws_tls_is_open(tcp)) {
            uint8_t next_fin = 0;
            uint8_t next_opcode = 0;
            uint8_t *next_data = NULL;
            size_t next_len = 0;

            if (!ws_server_recv_frame(tcp, &next_fin, &next_opcode, &next_data, &next_len))
                goto done;

            if (next_opcode == WS_OP_PING) {
                int ok = ws_server_send_locked(s, slot, tcp, WS_OP_PONG, next_data, next_len);
                free(next_data);
                if (!ok)
                    goto done;
                continue;
            }

            if (next_opcode == WS_OP_PONG) {
                free(next_data);
                continue;
            }

            if (next_opcode == WS_OP_CLOSE) {
                ws_server_send_locked(s, slot, tcp, WS_OP_CLOSE, next_data, next_len);
                free(next_data);
                goto done;
            }

            if (next_opcode != WS_OP_CONTINUATION) {
                free(next_data);
                ws_server_send_locked(s, slot, tcp, WS_OP_CLOSE, "\x03\xEA", 2);
                goto done;
            }
            if (next_len > WS_MAX_MESSAGE_BYTES - message_len) {
                free(next_data);
                ws_server_send_locked(s, slot, tcp, WS_OP_CLOSE, "\x03\xF1", 2);
                goto done;
            }
            message_len += next_len;
            if (text_message && !ws_utf8_state_feed(&utf8, next_data, next_len)) {
                free(next_data);
                ws_server_send_locked(s, slot, tcp, WS_OP_CLOSE, "\x03\xEF", 2);
                goto done;
            }
            free(next_data);
            if (next_fin) {
                if (text_message && !ws_utf8_state_is_complete(&utf8)) {
                    ws_server_send_locked(s, slot, tcp, WS_OP_CLOSE, "\x03\xEF", 2);
                    goto done;
                }
                break;
            }
        }
    }

done:
    rt_trap_clear_recovery();
    ws_server_remove_client(s, slot, generation, tcp);
}

/// @brief Upgrade one pre-registered raw TCP task to TLS and WebSocket.
/// @details The raw slot remains visible to Stop while queued. During TLS
///          handoff the raw descriptor is detached exactly once and the slot
///          stays occupied; bounded TLS polling observes lifecycle cancellation
///          during the brief pointer-free handshake window. A successful TLS
///          session is retained into the same generation before the HTTP upgrade, making
///          that second handshake interruptible by Stop.
/// @param arg Heap-owned @ref ws_accept_task_t; consumed unconditionally.
static void ws_accept_task_run(void *arg) {
    ws_accept_task_t *task = (ws_accept_task_t *)arg;
    rt_ws_server_impl *s = task ? task->server : NULL;
    void *tcp = task ? task->tcp : NULL;
    rt_tls_session_t *tls = NULL;
    int slot = task ? task->slot : -1;
    int reserved_slot = slot;
    uint64_t generation = task ? task->generation : 0;
    intptr_t socket_fd = (intptr_t)INVALID_SOCK;
    int detached = 0;

    free(task);
    if (!s || !tcp || slot < 0 || slot >= WS_SERVER_MAX_CLIENTS ||
        !ws_server_is_running_locked(s)) {
        ws_server_remove_client(s, slot, generation, tcp);
        ws_release_raw_tcp(&tcp);
        return;
    }

    // Serialize the ownership transition with Stop. The task and slot each
    // hold one managed reference to the same raw TCP payload.
    WS_MUTEX_LOCK(&s->clients[slot].io);
    WS_MUTEX_LOCK(&s->lock);
    if (slot >= 0 && slot < s->client_count && s->running && s->clients[slot].occupied &&
        s->clients[slot].generation == generation && !s->clients[slot].tls_transport &&
        s->clients[slot].tcp == tcp) {
        socket_fd = (intptr_t)rt_tcp_socket_fd(tcp);
        if ((socket_t)socket_fd != INVALID_SOCK) {
            rt_tcp_detach_socket(tcp);
            ws_release_transport_reference(&s->clients[slot].tcp);
            s->clients[slot].pending_socket = (socket_t)socket_fd;
            s->clients[slot].pending_socket_valid = true;
            detached = 1;
        }
    }
    WS_MUTEX_UNLOCK(&s->lock);
    WS_MUTEX_UNLOCK(&s->clients[slot].io);

    if (!detached) {
        ws_server_remove_client(s, slot, generation, tcp);
        ws_release_raw_tcp(&tcp);
        return;
    }

    // Drop the task's detached TCP wrapper; TLS consumes the native descriptor.
    if (rt_obj_release_check0(tcp))
        rt_obj_free(tcp);
    tcp = NULL;
    // Session construction performs one managed allocation. A trap at that
    // boundary occurs before TLS can consume the descriptor, so close it and
    // retire the visible slot locally instead of escaping the pool task.
    jmp_buf tls_recovery;
    rt_trap_set_recovery(&tls_recovery);
    if (RT_SETJMP(tls_recovery) != 0) {
        rt_trap_clear_recovery();
        ws_server_clear_pending_socket(s, slot, generation);
        (void)rt_socket_close((socket_t)socket_fd);
        ws_server_remove_client(s, slot, generation, NULL);
        return;
    }
    tls = rt_tls_server_accept_socket(socket_fd, s->tls_ctx);
    rt_trap_clear_recovery();
    if (!tls) {
        ws_server_clear_pending_socket(s, slot, generation);
        ws_server_remove_client(s, slot, generation, NULL);
        return;
    }

    // Publish a retained TLS reference before reading the HTTP upgrade so Stop
    // can interrupt a client that finishes TLS but never sends HTTP headers.
    WS_MUTEX_LOCK(&s->clients[slot].io);
    WS_MUTEX_LOCK(&s->lock);
    int published = slot >= 0 && slot < s->client_count && s->running &&
                    s->clients[slot].occupied && s->clients[slot].generation == generation &&
                    !s->clients[slot].tcp;
    if (published) {
        rt_obj_retain_known(tls);
        s->clients[slot].tcp = tls;
        s->clients[slot].tls_transport = true;
        s->clients[slot].pending_socket = INVALID_SOCK;
        s->clients[slot].pending_socket_valid = false;
    }
    WS_MUTEX_UNLOCK(&s->lock);
    WS_MUTEX_UNLOCK(&s->clients[slot].io);
    if (!published) {
        ws_server_remove_client(s, slot, generation, NULL);
        rt_tls_close(tls);
        return;
    }

    // The WebSocket upgrade allocates managed Strings and may trap. Recover
    // locally so the slot reference and worker-owned TLS reference both retire.
    jmp_buf handshake_recovery;
    rt_trap_set_recovery(&handshake_recovery);
    if (RT_SETJMP(handshake_recovery) != 0) {
        rt_trap_clear_recovery();
        ws_server_remove_client(s, slot, generation, tls);
        rt_tls_close(tls);
        return;
    }
    int handshake_ok = ws_server_is_running_locked(s) && ws_server_handshake(tls, s->subprotocol);
    if (handshake_ok) {
        socket_t tls_socket = (socket_t)rt_tls_get_socket(tls);
        if (rt_tls_set_io_timeout(tls, WS_CLIENT_SEND_TIMEOUT_MS) != RT_TLS_OK ||
            tls_socket == INVALID_SOCK || !set_socket_timeout(tls_socket, 0, true))
            handshake_ok = 0;
    }
    rt_trap_clear_recovery();
    if (!handshake_ok) {
        ws_server_remove_client(s, slot, generation, tls);
        rt_tls_close(tls);
        return;
    }

    WS_MUTEX_LOCK(&s->lock);
    if (slot >= 0 && slot < s->client_count && s->running && s->clients[slot].occupied &&
        s->clients[slot].generation == generation && s->clients[slot].tls_transport &&
        s->clients[slot].tcp == tls) {
        s->clients[slot].active = true;
    } else
        slot = -1;
    WS_MUTEX_UNLOCK(&s->lock);

    if (slot < 0) {
        ws_server_remove_client(s, reserved_slot, generation, tls);
        rt_tls_close(tls);
        return;
    }

    ws_client_run(s, slot, generation, tls);
    rt_tls_close(tls);
}

//=============================================================================
// Accept Loop
//=============================================================================

#if RT_PLATFORM_WINDOWS
static unsigned __stdcall ws_accept_loop(void *arg)
#else
static void *ws_accept_loop(void *arg)
#endif
{
    rt_ws_server_impl *s = (rt_ws_server_impl *)arg;

    while (ws_server_is_running_locked(s)) {
        void *tcp = rt_tcp_server_accept_for(s->tcp_server, 1000);
        if (!tcp)
            continue;
        if (!ws_server_is_running_locked(s)) {
            ws_release_raw_tcp(&tcp);
            break;
        }

        ws_accept_task_t *task = (ws_accept_task_t *)malloc(sizeof(*task));
        if (!task) {
            ws_release_raw_tcp(&tcp);
            continue;
        }
        int slot = -1;
        uint64_t generation = 0;
        WS_MUTEX_LOCK(&s->lock);
        for (int i = 0; i < WS_SERVER_MAX_CLIENTS; ++i) {
            if (!s->clients[i].occupied) {
                slot = i;
                break;
            }
        }
        if (slot >= 0) {
            generation = ++s->clients[slot].generation;
            if (generation == 0)
                generation = ++s->clients[slot].generation;
            s->clients[slot].occupied = true;
            s->clients[slot].active = false;
            s->clients[slot].tls_transport = false;
            s->clients[slot].pending_socket = INVALID_SOCK;
            s->clients[slot].pending_socket_valid = false;
            s->clients[slot].tcp = tcp;
            rt_obj_retain_known(tcp);
            if (slot >= s->client_count)
                s->client_count = slot + 1;
        }
        WS_MUTEX_UNLOCK(&s->lock);
        if (slot < 0) {
            free(task);
            ws_release_raw_tcp(&tcp);
            continue;
        }
        task->server = s;
        task->tcp = tcp;
        task->slot = slot;
        task->generation = generation;
        if (!s->worker_pool || !rt_threadpool_submit_fn(s->worker_pool, ws_accept_task_run, task)) {
            free(task);
            ws_server_remove_client(s, slot, generation, tcp);
            ws_release_raw_tcp(&tcp);
        }
    }

#if RT_PLATFORM_WINDOWS
    return 0;
#else
    return NULL;
#endif
}

//=============================================================================
// Public API
//=============================================================================

/// @brief Construct a WebSocket server bound (lazily) to `port`. Validates port range (0–65535; 0
/// requests an OS-assigned ephemeral port reported by `Port` after Start) up front; allocates the
/// impl with mutex + finalizer and parses immutable TLS credentials, but does
/// not bind the TCP socket until Start.
/// @param[in] port TCP port from 0 through 65535; zero requests an ephemeral port.
/// @param[in] cert_file Nonempty managed path to a PEM certificate chain.
/// @param[in] key_file Nonempty managed path to the matching PEM private key.
/// @return Caller-owned managed WSS server, or NULL after a returning trap hook.
void *rt_wss_server_new(int64_t port, rt_string cert_file, rt_string key_file) {
    if (port < 0 || port > 65535) {
        rt_trap("WssServer: invalid port");
        return NULL;
    }

    if (!cert_file || !key_file || !rt_string_is_handle(cert_file) ||
        !rt_string_is_handle(key_file)) {
        rt_trap("WssServer: invalid certificate or key path");
        return NULL;
    }
    const char *cert_cstr = rt_string_cstr(cert_file);
    const char *key_cstr = rt_string_cstr(key_file);
    int64_t cert_len = rt_str_len(cert_file);
    int64_t key_len = rt_str_len(key_file);
    if (!cert_cstr || !key_cstr || cert_len <= 0 || key_len <= 0 ||
        (uint64_t)cert_len > (uint64_t)SIZE_MAX || (uint64_t)key_len > (uint64_t)SIZE_MAX ||
        memchr(cert_cstr, '\0', (size_t)cert_len) != NULL ||
        memchr(key_cstr, '\0', (size_t)key_len) != NULL) {
        rt_trap("WssServer: invalid certificate or key path");
        return NULL;
    }

    rt_ws_server_impl *s = (rt_ws_server_impl *)rt_obj_new_i64(RT_WSS_SERVER_CLASS_ID,
                                                               (int64_t)sizeof(rt_ws_server_impl));
    if (!s) {
        rt_trap("WssServer: memory allocation failed");
        return NULL;
    }
    memset(s, 0, sizeof(*s));
    rt_obj_set_finalizer(s, rt_ws_server_finalize);
    s->port = port;
    s->cert_file = strdup(cert_cstr);
    s->key_file = strdup(key_cstr);
    if (!s->cert_file || !s->key_file) {
        if (rt_obj_release_check0(s))
            rt_obj_free(s);
        rt_trap("WssServer: failed to copy certificate paths");
        return NULL;
    }
    {
        rt_tls_server_config_t tls_cfg;
        rt_tls_server_config_init(&tls_cfg);
        tls_cfg.cert_file = s->cert_file;
        tls_cfg.key_file = s->key_file;
        tls_cfg.alpn_protocol = "http/1.1";
        // Match the 30 s TLS-server handshake default (rt_tls_server_ctx_new).
        // Debug builds compute the full TLS 1.3 flight in whole seconds per
        // side, and a tighter 10 s budget loses that race on slower hosts
        // while buying no additional protection (Stop still cancels promptly
        // through cancel_requested polling).
        tls_cfg.timeout_ms = 30000;
        tls_cfg.cancel_requested = ws_tls_accept_cancel_requested;
        tls_cfg.cancel_context = s;
        s->tls_ctx = rt_tls_server_ctx_new(&tls_cfg);
        if (!s->tls_ctx) {
            char error[512];
            const char *message = rt_tls_server_last_error();
            snprintf(error,
                     sizeof(error),
                     "%s",
                     message && message[0] ? message : "WssServer: TLS context creation failed");
            if (rt_obj_release_check0(s))
                rt_obj_free(s);
            rt_trap(error);
            return NULL;
        }
    }
    if (WS_MUTEX_INIT(&s->lifecycle) != 0) {
        if (rt_obj_release_check0(s))
            rt_obj_free(s);
        rt_trap("WssServer: lifecycle mutex initialization failed");
        return NULL;
    }
    s->lifecycle_initialized = true;
    if (WS_MUTEX_INIT(&s->lock) != 0) {
        if (rt_obj_release_check0(s))
            rt_obj_free(s);
        rt_trap("WssServer: state mutex initialization failed");
        return NULL;
    }
    s->lock_initialized = true;
    for (int i = 0; i < WS_SERVER_MAX_CLIENTS; i++) {
        if (WS_MUTEX_INIT(&s->clients[i].io) != 0) {
            if (rt_obj_release_check0(s))
                rt_obj_free(s);
            rt_trap("WssServer: client mutex initialization failed");
            return NULL;
        }
        s->client_io_count = i + 1;
    }
    return s;
}

/// @brief Start listening: bind the TCP server, mark `running=true`, and spawn the accept loop
/// on a dedicated CRT-aware thread. Idempotent — calling
/// while already running is a no-op.
/// @param[in] obj Managed WSS server receiver; NULL is a no-op.
void rt_wss_server_start(void *obj) {
    if (!obj)
        return;
    rt_ws_server_impl *s = ws_server_require(obj, "WssServer.Start: invalid object");
    if (!s)
        return;
    WS_MUTEX_LOCK(&s->lifecycle);
    if (ws_server_is_running_locked(s)) {
        WS_MUTEX_UNLOCK(&s->lifecycle);
        return;
    }

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char error[512];
        const char *message = rt_trap_get_error();
        int net_code = rt_trap_get_net_code();
        snprintf(error,
                 sizeof(error),
                 "%s",
                 message && message[0] ? message : "WssServer: start failed");
        rt_trap_clear_recovery();
        WS_MUTEX_LOCK(&s->lock);
        s->running = false;
        WS_MUTEX_UNLOCK(&s->lock);
        if (s->tcp_server) {
            rt_tcp_server_close(s->tcp_server);
            if (rt_obj_release_check0(s->tcp_server))
                rt_obj_free(s->tcp_server);
            s->tcp_server = NULL;
        }
        if (s->worker_pool) {
            void *worker_pool = s->worker_pool;
            s->worker_pool = NULL;
            rt_threadpool_shutdown(worker_pool);
            if (rt_obj_release_check0(worker_pool))
                rt_obj_free(worker_pool);
        }
        WS_MUTEX_UNLOCK(&s->lifecycle);
        if (net_code != 0)
            rt_trap_net(error, net_code);
        else
            rt_trap(error);
        return;
    }

    s->tcp_server = rt_tcp_server_listen(s->port);
    if (!s->tcp_server)
        rt_trap("WssServer: failed to bind listener");
    int64_t bound_port = rt_tcp_server_port(s->tcp_server);
    if (!s->worker_pool) {
        s->worker_pool = rt_threadpool_new(wss_server_default_worker_count());
        if (!s->worker_pool)
            rt_trap("WssServer: worker pool allocation failed");
    }
    WS_MUTEX_LOCK(&s->lock);
    s->port = bound_port;
    s->running = true;
    WS_MUTEX_UNLOCK(&s->lock);

#if RT_PLATFORM_WINDOWS
    uintptr_t thread_handle = _beginthreadex(NULL, 0, ws_accept_loop, s, 0, NULL);
    s->accept_thread = thread_handle ? (HANDLE)thread_handle : NULL;
    s->thread_started = s->accept_thread != NULL;
#else
    s->thread_started = pthread_create(&s->accept_thread, NULL, ws_accept_loop, s) == 0;
#endif
    if (!s->thread_started) {
        rt_trap("WssServer: failed to start accept thread");
    }
    rt_trap_clear_recovery();
    WS_MUTEX_UNLOCK(&s->lifecycle);
}

/// @brief Stop the server: set `running=false`, close the TCP listener (which unblocks
/// `accept_for`), join the accept thread, then close every active client
/// connection under the mutex. Designed to be safely called from any thread.
/// @param[in] obj Managed WSS server receiver; NULL is a no-op.
void rt_wss_server_stop(void *obj) {
    if (!obj)
        return;
    rt_ws_server_impl *s = ws_server_require(obj, "WssServer.Stop: invalid object");
    if (!s)
        return;
    if (s->lifecycle_initialized)
        WS_MUTEX_LOCK(&s->lifecycle);
    if (s->lock_initialized) {
        WS_MUTEX_LOCK(&s->lock);
        s->running = false;
        WS_MUTEX_UNLOCK(&s->lock);
    } else {
        s->running = false;
    }

    if (s->tcp_server) {
        rt_tcp_server_close(s->tcp_server);
        if (rt_obj_release_check0(s->tcp_server))
            rt_obj_free(s->tcp_server);
        s->tcp_server = NULL;
    }

    if (s->thread_started) {
#if RT_PLATFORM_WINDOWS
        if (rt_win32_join_thread_handle(s->accept_thread) == RT_WIN32_THREAD_JOIN_FAILED)
            abort();
#else
        pthread_join(s->accept_thread, NULL);
#endif
        s->thread_started = false;
    }

    if (s->client_io_count > 0 && s->lock_initialized) {
        for (int i = 0; i < s->client_io_count; i++) {
            WS_MUTEX_LOCK(&s->clients[i].io);
            WS_MUTEX_LOCK(&s->lock);
            if (i < s->client_count && s->clients[i].occupied)
                ws_interrupt_slot_transport_locked(&s->clients[i]);
            s->clients[i].active = false;
            WS_MUTEX_UNLOCK(&s->lock);
            WS_MUTEX_UNLOCK(&s->clients[i].io);
        }
    }

    void *worker_pool = s->worker_pool;
    s->worker_pool = NULL;
    if (worker_pool) {
        rt_threadpool_shutdown(worker_pool);
        if (rt_obj_release_check0(worker_pool))
            rt_obj_free(worker_pool);
    }
    if (s->lock_initialized) {
        WS_MUTEX_LOCK(&s->lock);
        for (int i = 0; i < WS_SERVER_MAX_CLIENTS; ++i) {
            s->clients[i].tcp = NULL;
            s->clients[i].active = false;
            s->clients[i].occupied = false;
            s->clients[i].tls_transport = false;
            s->clients[i].pending_socket = INVALID_SOCK;
            s->clients[i].pending_socket_valid = false;
        }
        s->client_count = 0;
        WS_MUTEX_UNLOCK(&s->lock);
    } else {
        s->client_count = 0;
    }
    if (s->lifecycle_initialized)
        WS_MUTEX_UNLOCK(&s->lifecycle);
}

/// @brief Atomically replace the required WSS subprotocol while stopped.
/// @details Exact managed length, storage, embedded NULs, and HTTP token
///          syntax are validated before lifecycle serialization. Start cannot
///          race the pointer swap, so TLS workers never observe freed policy.
/// @param[in] obj Managed WSS server receiver; NULL is a no-op.
/// @param[in] subprotocol Optional managed RFC token; NULL or empty clears the requirement.
void rt_wss_server_set_subprotocol(void *obj, rt_string subprotocol) {
    if (!obj)
        return;
    rt_ws_server_impl *s = ws_server_require(obj, "WssServer.SetSubprotocol: invalid object");
    if (!s)
        return;
    if (subprotocol && !rt_string_is_handle(subprotocol)) {
        rt_trap("WssServer.SetSubprotocol: invalid String");
        return;
    }
    const char *protocol = subprotocol ? rt_string_cstr(subprotocol) : NULL;
    int64_t protocol_len64 = subprotocol ? rt_str_len(subprotocol) : 0;
    char *copy = NULL;

    if (protocol_len64 < 0 || (uint64_t)protocol_len64 >= (uint64_t)SIZE_MAX ||
        (protocol_len64 > 0 &&
         (!protocol || memchr(protocol, '\0', (size_t)protocol_len64) != NULL))) {
        rt_trap("WssServer.SetSubprotocol: invalid String storage");
        return;
    }
    if (protocol_len64 > 0) {
        size_t protocol_len = (size_t)protocol_len64;
        copy = (char *)malloc(protocol_len + 1);
        if (!copy) {
            rt_trap("WssServer.SetSubprotocol: memory allocation failed");
            return;
        }
        memcpy(copy, protocol, protocol_len);
        copy[protocol_len] = '\0';
        if (!ws_token_is_valid(copy)) {
            free(copy);
            rt_trap("WssServer.SetSubprotocol: invalid subprotocol token");
            return;
        }
    }

    WS_MUTEX_LOCK(&s->lifecycle);
    if (ws_server_is_running_locked(s)) {
        WS_MUTEX_UNLOCK(&s->lifecycle);
        free(copy);
        rt_trap("WssServer.SetSubprotocol: cannot change subprotocol while server is running");
        return;
    }
    WS_MUTEX_LOCK(&s->lock);
    char *previous = s->subprotocol;
    s->subprotocol = copy;
    WS_MUTEX_UNLOCK(&s->lock);
    WS_MUTEX_UNLOCK(&s->lifecycle);
    free(previous);
}

/// @brief Return a managed snapshot of the configured WSS subprotocol.
/// @details The native policy is copied under the state mutex; managed String
///          allocation happens outside the mutex and has local trap recovery
///          so the native snapshot cannot leak on allocation failure.
/// @param[in] obj Managed WSS server receiver, or NULL.
/// @return Caller-owned managed protocol String; empty when none is configured,
///         or NULL after an invalid-object or allocation trap.
rt_string rt_wss_server_subprotocol(void *obj) {
    if (!obj)
        return rt_str_empty();
    rt_ws_server_impl *s = ws_server_require(obj, "WssServer.Subprotocol: invalid object");
    if (!s)
        return NULL;
    WS_MUTEX_LOCK(&s->lock);
    size_t len = s->subprotocol ? strlen(s->subprotocol) : 0;
    char *snapshot = NULL;
    if (len > 0) {
        snapshot = (char *)malloc(len + 1);
        if (snapshot)
            memcpy(snapshot, s->subprotocol, len + 1);
    }
    WS_MUTEX_UNLOCK(&s->lock);
    if (len == 0)
        return rt_str_empty();
    if (!snapshot) {
        rt_trap("WssServer.Subprotocol: memory allocation failed");
        return NULL;
    }

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char error[512];
        const char *message = rt_trap_get_error();
        int net_code = rt_trap_get_net_code();
        snprintf(error,
                 sizeof(error),
                 "%s",
                 message && message[0] ? message : "WssServer.Subprotocol: allocation failed");
        rt_trap_clear_recovery();
        free(snapshot);
        if (net_code != 0)
            rt_trap_net(error, net_code);
        else
            rt_trap(error);
        return NULL;
    }
    rt_string result = rt_string_from_bytes(snapshot, len);
    rt_trap_clear_recovery();
    free(snapshot);
    return result;
}

/// @brief Send a TEXT frame to every connected client. Clients whose send fails are marked
/// inactive and their TCP handles released — handles dead-client cleanup as a side effect.
/// Per-client I/O serialization prevents control-frame interleaving without
/// holding the client-list mutex across TLS writes.
/// @param[in] obj Managed WSS server receiver; NULL is a no-op.
/// @param[in] message Managed String payload; NULL sends an empty text message.
void rt_wss_server_broadcast(void *obj, rt_string message) {
    if (!obj)
        return;
    rt_ws_server_impl *s = ws_server_require(obj, "WssServer.Broadcast: invalid object");
    if (!s)
        return;
    if (message && !rt_string_is_handle(message)) {
        rt_trap("WssServer.Broadcast: invalid String");
        return;
    }
    const char *msg = rt_string_cstr(message);
    int64_t len64 = message ? rt_str_len(message) : 0;
    if (len64 < 0 || (uint64_t)len64 > (uint64_t)SIZE_MAX || (len64 > 0 && !msg)) {
        rt_trap("WssServer.Broadcast: invalid String storage");
        return;
    }
    size_t len = (size_t)len64; // full byte length: embedded NUL is data
    ws_broadcast_target_t targets[WS_SERVER_MAX_CLIENTS];
    int target_count = 0;

    WS_MUTEX_LOCK(&s->lock);
    for (int i = 0; i < s->client_count; i++) {
        if (s->clients[i].active && s->clients[i].tcp)
            targets[target_count++] =
                (ws_broadcast_target_t){i, s->clients[i].tcp, s->clients[i].generation};
    }
    WS_MUTEX_UNLOCK(&s->lock);

    for (int i = 0; i < target_count; i++) {
        int should_send = 0;
        int ok = 1;
        WS_MUTEX_LOCK(&s->clients[targets[i].slot].io);
        WS_MUTEX_LOCK(&s->lock);
        if (targets[i].slot < s->client_count && s->clients[targets[i].slot].occupied &&
            s->clients[targets[i].slot].active &&
            s->clients[targets[i].slot].generation == targets[i].generation &&
            s->clients[targets[i].slot].tls_transport &&
            s->clients[targets[i].slot].tcp == targets[i].tcp && ws_tls_is_open(targets[i].tcp)) {
            should_send = 1;
        }
        WS_MUTEX_UNLOCK(&s->lock);

        if (should_send)
            ok = ws_server_send_frame(targets[i].tcp, WS_OP_TEXT, msg, len);

        WS_MUTEX_LOCK(&s->lock);
        if (!ok && targets[i].slot < s->client_count && s->clients[targets[i].slot].occupied &&
            s->clients[targets[i].slot].generation == targets[i].generation &&
            s->clients[targets[i].slot].tcp == targets[i].tcp) {
            ws_interrupt_slot_transport_locked(&s->clients[targets[i].slot]);
            s->clients[targets[i].slot].active = false;
        }
        WS_MUTEX_UNLOCK(&s->lock);
        WS_MUTEX_UNLOCK(&s->clients[targets[i].slot].io);
    }
}

/// @brief Broadcast one binary frame to every active TLS client.
/// @details Uses the same generation checks, per-client serialization, and
///          failed-transport cleanup as text broadcast.
/// @param[in] obj Managed WSS server receiver; NULL is a no-op.
/// @param[in] data Managed Bytes payload; NULL is a no-op.
void rt_wss_server_broadcast_bytes(void *obj, void *data) {
    if (!obj || !data)
        return;
    rt_ws_server_impl *s = ws_server_require(obj, "WssServer.BroadcastBytes: invalid object");
    if (!s)
        return;
    if (!rt_bytes_is_bytes(data)) {
        rt_trap("WssServer.BroadcastBytes: invalid Bytes");
        return;
    }
    int64_t len = rt_bytes_len(data);
    const uint8_t *ptr = len > 0 ? rt_bytes_data_const(data) : NULL;
    if (len < 0 || (uint64_t)len > (uint64_t)SIZE_MAX || (len > 0 && !ptr)) {
        rt_trap("WssServer.BroadcastBytes: invalid Bytes storage");
        return;
    }

    ws_broadcast_target_t targets[WS_SERVER_MAX_CLIENTS];
    int target_count = 0;

    WS_MUTEX_LOCK(&s->lock);
    for (int i = 0; i < s->client_count; i++) {
        if (s->clients[i].active && s->clients[i].tcp)
            targets[target_count++] =
                (ws_broadcast_target_t){i, s->clients[i].tcp, s->clients[i].generation};
    }
    WS_MUTEX_UNLOCK(&s->lock);

    for (int i = 0; i < target_count; i++) {
        int should_send = 0;
        int ok = 1;
        WS_MUTEX_LOCK(&s->clients[targets[i].slot].io);
        WS_MUTEX_LOCK(&s->lock);
        if (targets[i].slot < s->client_count && s->clients[targets[i].slot].occupied &&
            s->clients[targets[i].slot].active &&
            s->clients[targets[i].slot].generation == targets[i].generation &&
            s->clients[targets[i].slot].tls_transport &&
            s->clients[targets[i].slot].tcp == targets[i].tcp && ws_tls_is_open(targets[i].tcp)) {
            should_send = 1;
        }
        WS_MUTEX_UNLOCK(&s->lock);

        if (should_send)
            ok = ws_server_send_frame(targets[i].tcp, WS_OP_BINARY, ptr, (size_t)len);

        WS_MUTEX_LOCK(&s->lock);
        if (!ok && targets[i].slot < s->client_count && s->clients[targets[i].slot].occupied &&
            s->clients[targets[i].slot].generation == targets[i].generation &&
            s->clients[targets[i].slot].tcp == targets[i].tcp) {
            ws_interrupt_slot_transport_locked(&s->clients[targets[i].slot]);
            s->clients[targets[i].slot].active = false;
        }
        WS_MUTEX_UNLOCK(&s->lock);
        WS_MUTEX_UNLOCK(&s->clients[targets[i].slot].io);
    }
}

/// @brief Count active clients (only counts slots flagged active — slots from disconnected
/// clients are skipped). Reads under the mutex for a consistent snapshot.
/// @param[in] obj Managed WSS server receiver, or NULL.
/// @return Number of fully upgraded active clients, or 0 for NULL or invalid receivers.
int64_t rt_wss_server_client_count(void *obj) {
    if (!obj)
        return 0;
    rt_ws_server_impl *s = ws_server_require(obj, "WssServer.ClientCount: invalid object");
    if (!s)
        return 0;
    WS_MUTEX_LOCK(&s->lock);
    int64_t count = 0;
    for (int i = 0; i < s->client_count; i++)
        if (s->clients[i].active)
            count++;
    WS_MUTEX_UNLOCK(&s->lock);
    return count;
}

/// @brief Read the configured or currently bound TCP port.
/// @details Before Start this is the constructor value; after binding it is
///          the actual listener port, including an assigned ephemeral port.
/// @param[in] obj Managed WSS server receiver, or NULL.
/// @return Port from 0 through 65535, or 0 for NULL or invalid receivers.
int64_t rt_wss_server_port(void *obj) {
    if (!obj)
        return 0;
    rt_ws_server_impl *s = ws_server_require(obj, "WssServer.Port: invalid object");
    if (!s)
        return 0;
    WS_MUTEX_LOCK(&s->lock);
    int64_t port = s->port;
    WS_MUTEX_UNLOCK(&s->lock);
    return port;
}

/// @brief Returns 1 between successful `_start` and `_stop`; 0 otherwise.
/// @param[in] obj Managed WSS server receiver, or NULL.
/// @return 1 while running; otherwise 0.
int8_t rt_wss_server_is_running(void *obj) {
    if (!obj)
        return 0;
    rt_ws_server_impl *s = ws_server_require(obj, "WssServer.IsRunning: invalid object");
    return s && ws_server_is_running_locked(s) ? 1 : 0;
}

/// @brief Synchronously accept, TLS-upgrade, and WebSocket-upgrade one connection.
/// @details This private compatibility helper snapshots the listener and
///          subprotocol under lifecycle/state synchronization. Descriptor
///          ownership moves exactly once from the accepted TCP wrapper into
///          the TLS session before HTTP parsing.
/// @param obj Managed WssServer receiver.
/// @return Caller-owned TLS session carrying WebSocket frames, or NULL.
static WSS_MAYBE_UNUSED void *rt_wss_server_accept(void *obj) {
    if (!obj)
        return NULL;
    rt_ws_server_impl *s = ws_server_require(obj, "WssServer.Accept: invalid object");
    if (!s)
        return NULL;

    WS_MUTEX_LOCK(&s->lifecycle);
    void *listener = s->tcp_server;
    if (listener)
        rt_obj_retain_known(listener);
    WS_MUTEX_LOCK(&s->lock);
    char *subprotocol = s->subprotocol ? strdup(s->subprotocol) : NULL;
    int snapshot_failed = s->subprotocol && !subprotocol;
    WS_MUTEX_UNLOCK(&s->lock);
    WS_MUTEX_UNLOCK(&s->lifecycle);
    if (snapshot_failed) {
        ws_release_transport_reference(&listener);
        rt_trap("WssServer.Accept: memory allocation failed");
        return NULL;
    }
    if (!listener) {
        free(subprotocol);
        return NULL;
    }

    void *tcp = rt_tcp_server_accept(listener);
    ws_release_transport_reference(&listener);
    if (!tcp) {
        free(subprotocol);
        return NULL;
    }

    intptr_t socket_fd = (intptr_t)rt_tcp_socket_fd(tcp);
    if ((socket_t)socket_fd == INVALID_SOCK) {
        ws_release_raw_tcp(&tcp);
        free(subprotocol);
        return NULL;
    }
    rt_tcp_detach_socket(tcp);
    ws_release_transport_reference(&tcp);

    rt_tls_session_t *tls = rt_tls_server_accept_socket(socket_fd, s->tls_ctx);
    if (!tls) {
        free(subprotocol);
        return NULL;
    }
    int upgraded = ws_server_handshake(tls, subprotocol);
    free(subprotocol);
    if (!upgraded) {
        void *failed_tls = tls;
        ws_release_tcp(&failed_tls);
        return NULL;
    }
    return tls;
}

/// @brief Receive one complete WebSocket message from a client. Handles **frame fragmentation**
/// (assembles continuation frames until FIN), **control frames** (auto-PONG to PING, auto-close
/// echo on CLOSE), and **UTF-8 validation** for text frames. Caps reassembled message size at
/// 64 MiB (per WebSocket security best practice) — over-cap closes with status 0x03F1
/// "Message Too Big". Invalid UTF-8 in text frames closes with 0x03EF "Invalid Payload".
/// Returns the decoded message as rt_string, or empty string on connection close/error.
/// @param[in,out] tcp Upgraded TLS client session; NULL returns an empty String.
/// @return Caller-owned managed String containing the exact text or binary
///         message bytes, or an empty String on close/error.
static WSS_MAYBE_UNUSED rt_string rt_wss_server_client_recv(void *tcp) {
    if (!tcp)
        return rt_string_from_bytes("", 0);

    while (ws_tls_is_open(tcp)) {
        uint8_t fin = 0;
        uint8_t opcode = 0;
        uint8_t *data = NULL;
        size_t len = 0;
        if (!ws_server_recv_frame(tcp, &fin, &opcode, &data, &len))
            break;

        if (opcode == WS_OP_PING) {
            ws_server_send_frame(tcp, WS_OP_PONG, data, len);
            free(data);
            continue;
        }

        if (opcode == WS_OP_CLOSE) {
            // Send close response
            ws_server_send_frame(tcp, WS_OP_CLOSE, data, len);
            free(data);
            break;
        }

        if (opcode == WS_OP_TEXT || opcode == WS_OP_BINARY || opcode == WS_OP_CONTINUATION) {
            uint8_t *message = NULL;
            size_t message_len = 0;
            uint8_t message_opcode = opcode;

            if (opcode == WS_OP_CONTINUATION) {
                free(data);
                ws_server_send_frame(tcp, WS_OP_CLOSE, "\x03\xEA", 2);
                break;
            }

            if (fin) {
                message = data;
                message_len = len;
            } else {
                message = data;
                message_len = len;
                while (1) {
                    uint8_t next_fin = 0;
                    uint8_t next_opcode = 0;
                    uint8_t *next_data = NULL;
                    size_t next_len = 0;
                    if (!ws_server_recv_frame(
                            tcp, &next_fin, &next_opcode, &next_data, &next_len)) {
                        free(message);
                        return rt_string_from_bytes("", 0);
                    }

                    if (next_opcode >= 0x08) {
                        if (next_opcode == WS_OP_PING)
                            ws_server_send_frame(tcp, WS_OP_PONG, next_data, next_len);
                        else if (next_opcode == WS_OP_CLOSE)
                            ws_server_send_frame(tcp, WS_OP_CLOSE, next_data, next_len);
                        free(next_data);
                        if (next_opcode == WS_OP_CLOSE) {
                            free(message);
                            return rt_string_from_bytes("", 0);
                        }
                        continue;
                    }

                    if (next_opcode != WS_OP_CONTINUATION) {
                        free(next_data);
                        free(message);
                        ws_server_send_frame(tcp, WS_OP_CLOSE, "\x03\xEA", 2);
                        return rt_string_from_bytes("", 0);
                    }

                    if (message_len > 64u * 1024u * 1024u ||
                        next_len > 64u * 1024u * 1024u - message_len) {
                        free(next_data);
                        free(message);
                        ws_server_send_frame(tcp, WS_OP_CLOSE, "\x03\xF1", 2);
                        return rt_string_from_bytes("", 0);
                    }

                    uint8_t *grown = (uint8_t *)realloc(message, message_len + next_len);
                    if (!grown) {
                        free(next_data);
                        free(message);
                        return rt_string_from_bytes("", 0);
                    }
                    message = grown;
                    memcpy(message + message_len, next_data, next_len);
                    message_len += next_len;
                    free(next_data);

                    if (next_fin)
                        break;
                }
            }

            if (message_opcode == WS_OP_TEXT && !ws_is_valid_utf8(message, message_len)) {
                free(message);
                ws_server_send_frame(tcp, WS_OP_CLOSE, "\x03\xEF", 2);
                break;
            }

            rt_string result = rt_string_from_bytes((const char *)message, message_len);
            free(message);
            return result;
        }

        free(data);
    }

    return rt_string_from_bytes("", 0);
}

/// @brief Send a TEXT frame to a single client. On send failure, closes the connection (caller
/// can then drop the handle). Companion to `_client_recv` for the per-connection message loop.
/// @param[in,out] tcp Upgraded TLS client session; NULL is a no-op.
/// @param[in] message Managed String payload; NULL sends an empty frame.
static WSS_MAYBE_UNUSED void rt_wss_server_client_send(void *tcp, rt_string message) {
    if (!tcp)
        return;
    const char *msg = rt_string_cstr(message);
    int64_t len64 = message ? rt_str_len(message) : 0;
    size_t len = (msg && len64 > 0) ? (size_t)len64 : 0; // full byte length: embedded NUL is data
    if (!ws_server_send_frame(tcp, WS_OP_TEXT, msg, len))
        rt_tls_close((rt_tls_session_t *)tcp);
}

/// @brief Send a polite WebSocket CLOSE frame (no payload) and tear down the TCP connection.
/// Use when terminating a single client without affecting the server's other connections.
/// @param[in,out] tcp Upgraded TLS client session; NULL is a no-op.
static WSS_MAYBE_UNUSED void rt_wss_server_client_close(void *tcp) {
    if (!tcp)
        return;
    ws_server_send_frame(tcp, WS_OP_CLOSE, NULL, 0);
    rt_tls_close((rt_tls_session_t *)tcp);
}
