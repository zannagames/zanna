//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_ws_server.c
// Purpose: WebSocket server with broadcast and per-client messaging.
// Key invariants:
//   - Stable receiver identity and lifecycle serialization precede all state access.
//   - Accepted sockets are generation-tagged before queueing, so Stop sees and
//     interrupts queued, handshaking, and active clients without ABA slot reuse.
//   - Strict CRLF HTTP parsing, canonical frames, aggregate limits, and
//     incremental UTF-8 rules match rt_wss_server.c through rt_ws_shared.inc.
// Ownership/Lifetime:
//   - The server, listener, pool, and TCP clients are managed objects with
//     explicit retained references; Stop joins and releases all native workers.
// Links: rt_ws_server.h, rt_ws_shared.inc, rt_socket_platform.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_ws_server.c
 * @brief Implements the managed plaintext WebSocket server.
 * @details A serialized lifecycle owns the listener, bounded worker pool, and
 *          generation-tagged client slots. Workers perform strict upgrades,
 *          drain and validate frames, handle control traffic, and synchronize
 *          per-client writes for broadcast and direct messaging.
 */

#include "rt_ws_server.h"
#include "rt_websocket.h"

#include "rt_bytes.h"
#include "rt_crypto.h"
#include "rt_internal.h"
#include "rt_network.h"
#include "rt_network_internal.h"
#include "rt_object.h"
#include "rt_socket_platform.h"
#include "rt_string.h"
#include "rt_threadpool.h"
#include "rt_trap.h"
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

//=============================================================================
// Internal Structures
//=============================================================================

#define WS_SERVER_MAX_CLIENTS 128
#define WS_MAX_HANDSHAKE_HEADERS 100
#define WS_MAX_HANDSHAKE_BYTES (16u * 1024u)
#define WS_MAX_MESSAGE_BYTES (64u * 1024u * 1024u)
#define WS_HANDSHAKE_TIMEOUT_MS 10000
#define WS_HANDSHAKE_POLL_MS 100
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

typedef struct {
    void *tcp; // TCP connection
    uint64_t generation;
    bool occupied;
    bool active;
    ws_mutex_t io; ///< Per-client write lock: broadcasts and worker control
                   ///< frames serialize per socket, not globally (VDOC-149).
} ws_client_t;

typedef struct {
    void *tcp_server;
    void *worker_pool;
    int64_t port;
    char *subprotocol;
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

/// @brief Validate and cast a public plain WebSocket-server receiver.
/// @details Wrong-class, stale, and undersized handles emit one trap before any
///          mutex, listener, client, or heap field is read. NULL handling stays
///          at each public method so its existing no-op/sentinel contract is
///          preserved.
/// @param object Candidate managed server handle.
/// @param operation Operation-specific diagnostic.
/// @return Valid private payload, or NULL after trapping.
static rt_ws_server_impl *ws_server_require(void *object, const char *operation) {
    if (!rt_obj_is_instance(object, RT_WS_SERVER_CLASS_ID, sizeof(rt_ws_server_impl))) {
        rt_trap(operation ? operation : "WsServer: invalid object");
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

/// @brief Close and release one owned managed TCP connection reference.
/// @param[in,out] tcp_ptr Address of the owned handle; cleared before return.
static void ws_release_tcp(void **tcp_ptr) {
    if (!tcp_ptr || !*tcp_ptr)
        return;
    rt_tcp_close(*tcp_ptr);
    if (rt_obj_release_check0(*tcp_ptr))
        rt_obj_free(*tcp_ptr);
    *tcp_ptr = NULL;
}

/// @brief Drop one retained managed reference without changing transport state.
/// @param object_ptr Address of a retained managed pointer; cleared on return.
static void ws_release_managed_reference(void **object_ptr) {
    if (!object_ptr || !*object_ptr)
        return;
    void *object = *object_ptr;
    *object_ptr = NULL;
    if (rt_obj_release_check0(object))
        rt_obj_free(object);
}

/// @brief Interrupt a slot's TCP transport and drop only the slot-owned reference.
/// @details The worker retains its own reference and performs the final close
///          after bidirectional shutdown wakes any blocked handshake or frame
///          receive. The slot io and server state mutexes must be held.
/// @param[in,out] client Occupied client slot whose transport is interrupted.
static void ws_interrupt_slot_transport_locked(ws_client_t *client) {
    if (!client || !client->tcp)
        return;
    socket_t fd = rt_tcp_socket_fd(client->tcp);
    if (fd != INVALID_SOCK)
        (void)rt_socket_shutdown_both(fd);
    ws_release_managed_reference(&client->tcp);
}

/// @brief Compute the internal worker-pool size for WS server instances.
/// @details Mirrors the WSS server: CPU-aware default clamped to the range
///          accepted by @ref rt_threadpool_new.
/// @return Worker count in the inclusive range 4..32.
static int64_t ws_server_default_worker_count(void) {
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

/// @brief Read the server's running flag under its state mutex.
/// @param[in,out] s Server payload, or NULL.
/// @return 1 when the server is running; otherwise 0.
static int ws_server_is_running_locked(rt_ws_server_impl *s) {
    int running;
    if (!s)
        return 0;
    WS_MUTEX_LOCK(&s->lock);
    running = s->running ? 1 : 0;
    WS_MUTEX_UNLOCK(&s->lock);
    return running;
}

/// @brief Send an exact native byte sequence over a runtime TCP connection.
/// @details Large writes are split to the platform send() range and interrupted
///          system calls are retried.
/// @param[in] tcp Valid runtime TCP connection.
/// @param[in] data Bytes to transmit.
/// @param[in] len Exact number of bytes to send.
/// @return 1 after all bytes are sent; otherwise 0 on an invalid socket, EOF, or send error.
static int ws_server_send_raw(void *tcp, const void *data, size_t len) {
    socket_t sock = rt_tcp_socket_fd(tcp);
    size_t total = 0;

    if (sock == INVALID_SOCK)
        return 0;

    while (total < len) {
        size_t remaining = len - total;
        int chunk = (int)(remaining > (size_t)INT_MAX ? INT_MAX : remaining);
        int sent = send(sock, (const char *)data + total, chunk, SEND_FLAGS);
        if (sent == SOCK_ERROR) {
            if (rt_socket_error_is_interrupted(rt_socket_last_error()))
                continue;
            return 0;
        }
        if (sent == 0)
            return 0;
        total += (size_t)sent;
    }

    return 1;
}

/// @brief Receive exactly one native byte span without managed intermediates.
/// @details Interrupted syscalls retry; EOF, timeout, and all other transport
///          errors return failure for the worker's orderly connection cleanup.
/// @param tcp Valid runtime TCP connection.
/// @param data Destination buffer.
/// @param len Exact byte count to receive.
/// @return Nonzero only when all @p len bytes were read.
static int ws_tcp_recv_exact_native(void *tcp, uint8_t *data, size_t len) {
    socket_t sock = rt_tcp_socket_fd(tcp);
    size_t total = 0;
    if (sock == INVALID_SOCK || (!data && len > 0))
        return 0;
    while (total < len) {
        size_t remaining = len - total;
        int chunk = (int)(remaining > (size_t)INT_MAX ? INT_MAX : remaining);
        int received = recv(sock, (char *)data + total, chunk, 0);
        if (received == SOCK_ERROR) {
            if (rt_socket_error_is_interrupted(rt_socket_last_error()))
                continue;
            return 0;
        }
        if (received == 0)
            return 0;
        total += (size_t)received;
    }
    return 1;
}

/// @brief Receive handshake bytes with prompt server-stop cancellation.
/// @details Short readiness waits avoid relying on another thread's shutdown
///          call to interrupt a synchronous Winsock receive. One monotonic
///          deadline still preserves the ten-second aggregate handshake limit.
/// @param server Owning server whose running state controls cancellation.
/// @param tcp Valid runtime TCP connection.
/// @param deadline_ms Absolute monotonic deadline, or zero when unavailable.
/// @param timeout_polls_remaining Fallback timeout budget when no clock exists.
/// @param data Destination buffer.
/// @param len Exact byte count to receive.
/// @return Nonzero only when all bytes arrive before cancellation or timeout.
static int ws_tcp_recv_exact_handshake(rt_ws_server_impl *server,
                                       void *tcp,
                                       uint64_t deadline_ms,
                                       int *timeout_polls_remaining,
                                       uint8_t *data,
                                       size_t len) {
    socket_t sock = rt_tcp_socket_fd(tcp);
    size_t total = 0;
    if (!server || sock == INVALID_SOCK || !timeout_polls_remaining || (!data && len > 0))
        return 0;

    while (total < len) {
        if (!ws_server_is_running_locked(server))
            return 0;

        int wait_ms = WS_HANDSHAKE_POLL_MS;
        if (deadline_ms != 0) {
            uint64_t now_ms = rt_socket_monotonic_ms();
            if (now_ms != 0) {
                if (now_ms >= deadline_ms)
                    return 0;
                uint64_t remaining_ms = deadline_ms - now_ms;
                if (remaining_ms < (uint64_t)wait_ms)
                    wait_ms = (int)remaining_ms;
            }
        }

        int ready = wait_socket(sock, wait_ms, false);
        if (ready == 0) {
            if (--*timeout_polls_remaining <= 0)
                return 0;
            continue;
        }
        if (ready < 0)
            return 0;

        size_t remaining = len - total;
        int chunk = (int)(remaining > (size_t)INT_MAX ? INT_MAX : remaining);
        int received = recv(sock, (char *)data + total, chunk, 0);
        if (received == SOCK_ERROR) {
            if (rt_socket_error_is_interrupted(rt_socket_last_error()))
                continue;
            return 0;
        }
        if (received == 0)
            return 0;
        total += (size_t)received;
    }
    return 1;
}

/// @brief Read one strictly CRLF-terminated HTTP line into native storage.
/// @details Bare LF, embedded NUL, stray CR, over-limit input, EOF, and native
///          allocation failure all reject the handshake. The caller owns the
///          returned NUL-terminated buffer and frees it with `free()`.
/// @param[in] server Owning server whose running flag permits continued reads.
/// @param[in] tcp Valid runtime TCP connection.
/// @param[in] deadline_ms Absolute monotonic handshake deadline, or zero when unavailable.
/// @param[in,out] timeout_polls_remaining Fallback poll budget when no clock is available.
/// @param[in] max_len Maximum line bytes excluding CRLF.
/// @param[out] len_out Receives the exact line length.
/// @return Caller-owned line, or NULL on rejection/failure.
static char *ws_tcp_recv_line_strict(rt_ws_server_impl *server,
                                     void *tcp,
                                     uint64_t deadline_ms,
                                     int *timeout_polls_remaining,
                                     size_t max_len,
                                     size_t *len_out) {
    size_t cap = max_len < 256u ? max_len + 1u : 256u;
    size_t len = 0;
    char *line = NULL;
    if (!len_out || cap == 0)
        return NULL;
    *len_out = 0;
    line = (char *)malloc(cap);
    if (!line)
        return NULL;

    for (;;) {
        uint8_t byte = 0;
        if (!ws_tcp_recv_exact_handshake(
                server, tcp, deadline_ms, timeout_polls_remaining, &byte, 1)) {
            free(line);
            return NULL;
        }
        if (byte == '\r') {
            uint8_t lf = 0;
            if (!ws_tcp_recv_exact_handshake(
                    server, tcp, deadline_ms, timeout_polls_remaining, &lf, 1) ||
                lf != '\n') {
                free(line);
                return NULL;
            }
            break;
        }
        if (byte == '\n' || byte == '\0' || len >= max_len) {
            free(line);
            return NULL;
        }
        if (len + 1 >= cap) {
            size_t next_cap = cap > max_len / 2u ? max_len + 1u : cap * 2u;
            char *grown = (char *)realloc(line, next_cap);
            if (!grown) {
                free(line);
                return NULL;
            }
            line = grown;
            cap = next_cap;
        }
        line[len++] = (char)byte;
    }

    line[len] = '\0';
    *len_out = len;
    return line;
}

//=============================================================================
// WebSocket Framing Helpers
//=============================================================================

/// @brief Send a WebSocket frame over a raw TCP connection (server-side: no masking).
/// @details Emits one final frame using the canonical shortest payload-length
///          form. RFC 6455 server frames are sent without a masking key.
/// @param[in] tcp Valid runtime TCP connection.
/// @param[in] opcode Frame opcode to encode.
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
///          control-frame, message-size, close-code, and close-reason rules.
///          Protocol violations receive an appropriate Close frame.
/// @param[in] tcp Valid runtime TCP connection.
/// @param[out] fin_out Receives whether the FIN bit is set.
/// @param[out] opcode_out Receives the validated opcode.
/// @param[out] data_out Receives an allocated unmasked payload, or NULL when empty.
/// @param[out] len_out Receives the payload size in bytes.
/// @return 1 on success; otherwise 0 after transport, allocation, or protocol failure.
static int ws_server_recv_frame(
    void *tcp, uint8_t *fin_out, uint8_t *opcode_out, uint8_t **data_out, size_t *len_out) {
    uint8_t header[2];
    uint8_t ext[8];
    uint8_t mask[4];
    *data_out = NULL;
    *len_out = 0;

    if (!ws_tcp_recv_exact_native(tcp, header, sizeof(header)))
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
        if (!ws_tcp_recv_exact_native(tcp, ext, 2))
            return 0;
        payload_len = ((size_t)ext[0] << 8) | ext[1];
        if (payload_len < 126) {
            ws_server_send_frame(tcp, WS_OP_CLOSE, "\x03\xEA", 2);
            return 0;
        }
    } else if (payload_len == 127) {
        if (!ws_tcp_recv_exact_native(tcp, ext, 8))
            return 0;
        if (!ws_decode_u64_len(ext, &payload_len)) {
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

    if (!ws_tcp_recv_exact_native(tcp, mask, sizeof(mask)))
        return 0;

    // Read payload
    if (payload_len > 0) {
        *data_out = (uint8_t *)malloc(payload_len);
        if (!*data_out) {
            ws_server_send_frame(tcp, WS_OP_CLOSE, "\x03\xF3", 2);
            return 0;
        }
        if (!ws_tcp_recv_exact_native(tcp, *data_out, payload_len)) {
            free(*data_out);
            *data_out = NULL;
            return 0;
        }
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
/// @details Reads a strictly CRLF-terminated request within aggregate line,
///          header-count, byte, and time limits; validates required RFC 6455
///          fields and origin authority; and emits the 101 response.
/// @param[in,out] server Running server used for cancellation checks.
/// @param[in] tcp Accepted runtime TCP connection.
/// @param[in] required_subprotocol Optional exact protocol token the request must offer.
/// @return 1 after sending a valid upgrade response; otherwise 0.
static int ws_server_handshake(rt_ws_server_impl *server,
                               void *tcp,
                               const char *required_subprotocol) {
    ws_handshake_headers_t headers;
    size_t total_bytes = 0;
    size_t line_len = 0;
    uint64_t started_ms = rt_socket_monotonic_ms();
    uint64_t deadline_ms = started_ms + WS_HANDSHAKE_TIMEOUT_MS;
    int timeout_polls_remaining = WS_HANDSHAKE_TIMEOUT_MS / WS_HANDSHAKE_POLL_MS;
    if (started_ms == 0 || deadline_ms < started_ms)
        deadline_ms = 0;
    memset(&headers, 0, sizeof(headers));

    char *line = ws_tcp_recv_line_strict(
        server, tcp, deadline_ms, &timeout_polls_remaining, WS_MAX_HANDSHAKE_BYTES - 2u, &line_len);
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
        line = ws_tcp_recv_line_strict(
            server, tcp, deadline_ms, &timeout_polls_remaining, remaining, &line_len);
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

    if (!ws_handshake_headers_are_valid(&headers, required_subprotocol, "http", 80))
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

    return ws_server_send_raw(tcp, response, (size_t)rlen);
}

//=============================================================================
// Finalizer
//=============================================================================

/// @brief GC finalizer: stop the accept thread and close all client connections, then destroy the
/// platform mutex. Calling `_stop` first is idempotent, so this is safe even if the user already
/// stopped the server explicitly.
/// @param[in,out] obj Managed WebSocket server payload, or NULL.
static void rt_ws_server_finalize(void *obj) {
    if (!obj)
        return;
    rt_ws_server_impl *s = (rt_ws_server_impl *)obj;
    rt_ws_server_stop(s);
    free(s->subprotocol);
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

//=============================================================================
// Client Servicing (VDOC-148: parity with rt_wss_server.c)
//=============================================================================

/// @brief Deactivate a client slot and release its TCP handle (slot io lock +
///        state lock held so a concurrent broadcast cannot write to a handle
///        being torn down). Lock order: slot io, then state.
/// @param[in,out] s Owning server payload.
/// @param[in] slot Reserved client-slot index.
/// @param[in] generation Generation that must still identify the reservation.
/// @param[in] tcp Expected TCP handle; released only when it still matches the slot.
static void ws_server_remove_client(rt_ws_server_impl *s,
                                    int slot,
                                    uint64_t generation,
                                    void *tcp) {
    if (!s || slot < 0 || slot >= WS_SERVER_MAX_CLIENTS)
        return;

    WS_MUTEX_LOCK(&s->clients[slot].io);
    WS_MUTEX_LOCK(&s->lock);
    if (slot < s->client_count && s->clients[slot].occupied &&
        s->clients[slot].generation == generation) {
        if (s->clients[slot].tcp == tcp)
            ws_release_tcp(&s->clients[slot].tcp);
        s->clients[slot].active = false;
        s->clients[slot].occupied = false;
    }
    WS_MUTEX_UNLOCK(&s->lock);
    WS_MUTEX_UNLOCK(&s->clients[slot].io);
}

/// @brief Send a frame under the client's own io lock so worker control
///        frames and broadcasts never interleave on one socket — without a
///        global write lock that would let one slow peer stall every other
///        client's sends (VDOC-149).
/// @param[in,out] s Owning server payload.
/// @param[in] slot Client slot whose I/O mutex serializes the frame.
/// @param[in] tcp Runtime TCP connection associated with @p slot.
/// @param[in] opcode Frame opcode to send.
/// @param[in] data Payload bytes, or NULL when @p len is zero.
/// @param[in] len Payload length in bytes.
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

/// @brief Per-client frame loop: answers PING with PONG, echoes CLOSE,
///        validates framing/UTF-8, drains data frames, and deactivates the
///        slot on disconnect — the same protocol servicing as the WSS server.
/// @details The TCP receive helpers trap on connection errors, so the loop
///          installs a trap-recovery point and treats any trap as an orderly
///          disconnect for this client (worker threads must never abort the
///          process because a peer vanished).
/// @param[in,out] s Owning server payload.
/// @param[in] slot Reserved client-slot index.
/// @param[in] generation Generation protecting the slot from ABA reuse.
/// @param[in] tcp Worker-retained runtime TCP connection.
static void ws_client_run(rt_ws_server_impl *s, int slot, uint64_t generation, void *tcp) {
    if (!s || !tcp || slot < 0)
        return;

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        rt_trap_clear_recovery();
        ws_server_remove_client(s, slot, generation, tcp);
        return;
    }

    while (ws_server_is_running_locked(s) && rt_tcp_is_open(tcp)) {
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

        while (ws_server_is_running_locked(s) && rt_tcp_is_open(tcp)) {
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

/// @brief Worker-pool task: handshake an accepted socket, register the client,
///        then run its frame-servicing loop.
/// @param[in] arg Owned ws_accept_task_t allocation; released on entry.
static void ws_accept_task_run(void *arg) {
    ws_accept_task_t *task = (ws_accept_task_t *)arg;
    rt_ws_server_impl *s = task ? task->server : NULL;
    void *tcp = task ? task->tcp : NULL;
    int slot = task ? task->slot : -1;
    int reserved_slot = slot;
    uint64_t generation = task ? task->generation : 0;

    free(task);
    if (!s || !tcp || !ws_server_is_running_locked(s)) {
        ws_server_remove_client(s, slot, generation, tcp);
        ws_release_tcp(&tcp);
        return;
    }

    // The handshake reads via trapping TCP helpers; a peer vanishing
    // mid-handshake must reject the client, not abort the process.
    jmp_buf hs_recovery;
    rt_trap_set_recovery(&hs_recovery);
    if (setjmp(hs_recovery) != 0) {
        rt_trap_clear_recovery();
        ws_server_remove_client(s, slot, generation, tcp);
        ws_release_tcp(&tcp);
        return;
    }
    int handshake_ok = ws_server_handshake(s, tcp, s->subprotocol);
    if (handshake_ok) {
        rt_tcp_set_recv_timeout(tcp, 0);
        rt_tcp_set_send_timeout(tcp, WS_CLIENT_SEND_TIMEOUT_MS);
    }
    rt_trap_clear_recovery();
    if (!handshake_ok) {
        ws_server_remove_client(s, slot, generation, tcp);
        ws_release_tcp(&tcp);
        return;
    }

    WS_MUTEX_LOCK(&s->lock);
    if (slot >= 0 && slot < s->client_count && s->running && s->clients[slot].occupied &&
        s->clients[slot].generation == generation && s->clients[slot].tcp == tcp) {
        s->clients[slot].active = true;
    } else {
        slot = -1;
    }
    WS_MUTEX_UNLOCK(&s->lock);

    if (slot < 0) {
        ws_server_remove_client(s, reserved_slot, generation, tcp);
        ws_release_tcp(&tcp);
        return;
    }

    ws_client_run(s, slot, generation, tcp);
    ws_release_tcp(&tcp);
}

//=============================================================================
// Accept Loop
//=============================================================================

/// @brief Accept plaintext TCP clients and submit generation-tagged handshake tasks.
/// @details Reserves each visible client slot before worker submission so Stop
///          can interrupt queued and handshaking connections. Rejected,
///          over-capacity, and unqueued connections are closed immediately.
/// @param[in] arg Retained server payload valid until the accept thread is joined.
/// @return Platform thread result: zero on Windows or NULL on POSIX.
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
            ws_release_tcp(&tcp);
            break;
        }

        // Reserve a visible slot before queueing so Stop can close queued and
        // handshaking sockets rather than waiting indefinitely for pool work.
        ws_accept_task_t *task = (ws_accept_task_t *)malloc(sizeof(*task));
        if (!task) {
            ws_release_tcp(&tcp);
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
            s->clients[slot].tcp = tcp;
            rt_obj_retain_known(tcp);
            if (slot >= s->client_count)
                s->client_count = slot + 1;
        }
        WS_MUTEX_UNLOCK(&s->lock);
        if (slot < 0) {
            free(task);
            ws_release_tcp(&tcp);
            continue;
        }
        task->server = s;
        task->tcp = tcp;
        task->slot = slot;
        task->generation = generation;
        if (!s->worker_pool || !rt_threadpool_submit_fn(s->worker_pool, ws_accept_task_run, task)) {
            free(task);
            ws_server_remove_client(s, slot, generation, tcp);
            ws_release_tcp(&tcp);
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
/// impl with mutex + finalizer, but does NOT bind the TCP socket — that happens on `_start`.
/// @param[in] port TCP port from 0 through 65535; zero requests an ephemeral port.
/// @return Caller-owned managed server handle, or NULL after a returning allocation trap.
void *rt_ws_server_new(int64_t port) {
    if (port < 0 || port > 65535) {
        rt_trap("WsServer: invalid port");
        return NULL;
    }

    rt_ws_server_impl *s = (rt_ws_server_impl *)rt_obj_new_i64(RT_WS_SERVER_CLASS_ID,
                                                               (int64_t)sizeof(rt_ws_server_impl));
    if (!s) {
        rt_trap("WsServer: memory allocation failed");
        return NULL;
    }
    memset(s, 0, sizeof(*s));
    rt_obj_set_finalizer(s, rt_ws_server_finalize);
    s->port = port;
    if (WS_MUTEX_INIT(&s->lifecycle) != 0) {
        if (rt_obj_release_check0(s))
            rt_obj_free(s);
        rt_trap("WsServer: lifecycle mutex initialization failed");
        return NULL;
    }
    s->lifecycle_initialized = true;
    if (WS_MUTEX_INIT(&s->lock) != 0) {
        if (rt_obj_release_check0(s))
            rt_obj_free(s);
        rt_trap("WsServer: state mutex initialization failed");
        return NULL;
    }
    s->lock_initialized = true;
    for (int i = 0; i < WS_SERVER_MAX_CLIENTS; i++) {
        if (WS_MUTEX_INIT(&s->clients[i].io) != 0) {
            if (rt_obj_release_check0(s))
                rt_obj_free(s);
            rt_trap("WsServer: client mutex initialization failed");
            return NULL;
        }
        s->client_io_count = i + 1;
    }
    return s;
}

/// @brief Start listening: bind the TCP server, mark `running=true`, and spawn the accept loop
/// on a dedicated CRT-aware thread. Idempotent — calling
/// while already running is a no-op.
/// @param[in] obj Managed WebSocket server receiver; NULL is a no-op.
void rt_ws_server_start(void *obj) {
    if (!obj)
        return;
    rt_ws_server_impl *s = ws_server_require(obj, "WsServer.Start: invalid object");
    if (!s)
        return;
    WS_MUTEX_LOCK(&s->lifecycle);
    if (ws_server_is_running_locked(s)) {
        WS_MUTEX_UNLOCK(&s->lifecycle);
        return;
    }

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char error[512];
        const char *message = rt_trap_get_error();
        int net_code = rt_trap_get_net_code();
        snprintf(
            error, sizeof(error), "%s", message && message[0] ? message : "WsServer: start failed");
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
        rt_trap("WsServer: failed to bind listener");
    int64_t bound_port = rt_tcp_server_port(s->tcp_server);
    if (!s->worker_pool) {
        s->worker_pool = rt_threadpool_new(ws_server_default_worker_count());
        if (!s->worker_pool)
            rt_trap("WsServer: worker pool allocation failed");
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
        rt_trap("WsServer: failed to start accept thread");
    }
    rt_trap_clear_recovery();
    WS_MUTEX_UNLOCK(&s->lifecycle);
}

/// @brief Stop the server: set `running=false`, close the TCP listener (which unblocks
/// `accept_for`), join the accept thread, then close every active client
/// connection under the mutex. Designed to be safely called from any thread.
/// @param[in] obj Managed WebSocket server receiver; NULL is a no-op.
void rt_ws_server_stop(void *obj) {
    if (!obj)
        return;
    rt_ws_server_impl *s = ws_server_require(obj, "WsServer.Stop: invalid object");
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
            s->clients[i].active = false;
            s->clients[i].occupied = false;
            s->clients[i].tcp = NULL;
        }
        s->client_count = 0;
        WS_MUTEX_UNLOCK(&s->lock);
    } else {
        s->client_count = 0;
    }
    if (s->lifecycle_initialized)
        WS_MUTEX_UNLOCK(&s->lifecycle);
}

/// @brief Atomically replace the required subprotocol while the server is stopped.
/// @details The exact managed String length is validated, including embedded
///          NUL rejection, before creating a native NUL-terminated copy.
///          Lifecycle serialization prevents Start from publishing a worker
///          that observes a freed token.
/// @param[in] obj Managed WebSocket server receiver; NULL is a no-op.
/// @param[in] subprotocol Optional managed RFC token; NULL or empty clears the requirement.
void rt_ws_server_set_subprotocol(void *obj, rt_string subprotocol) {
    if (!obj)
        return;
    rt_ws_server_impl *s = ws_server_require(obj, "WsServer.SetSubprotocol: invalid object");
    if (!s)
        return;
    if (subprotocol && !rt_string_is_handle(subprotocol)) {
        rt_trap("WsServer.SetSubprotocol: invalid String");
        return;
    }
    const char *protocol = subprotocol ? rt_string_cstr(subprotocol) : NULL;
    int64_t protocol_len64 = subprotocol ? rt_str_len(subprotocol) : 0;
    char *copy = NULL;

    if (protocol_len64 < 0 || (uint64_t)protocol_len64 >= (uint64_t)SIZE_MAX ||
        (protocol_len64 > 0 &&
         (!protocol || memchr(protocol, '\0', (size_t)protocol_len64) != NULL))) {
        rt_trap("WsServer.SetSubprotocol: invalid String storage");
        return;
    }
    if (protocol_len64 > 0) {
        size_t protocol_len = (size_t)protocol_len64;
        copy = (char *)malloc(protocol_len + 1);
        if (!copy) {
            rt_trap("WsServer.SetSubprotocol: memory allocation failed");
            return;
        }
        memcpy(copy, protocol, protocol_len);
        copy[protocol_len] = '\0';
        if (!ws_token_is_valid(copy)) {
            free(copy);
            rt_trap("WsServer.SetSubprotocol: invalid subprotocol token");
            return;
        }
    }

    WS_MUTEX_LOCK(&s->lifecycle);
    if (ws_server_is_running_locked(s)) {
        WS_MUTEX_UNLOCK(&s->lifecycle);
        free(copy);
        rt_trap("WsServer.SetSubprotocol: cannot change subprotocol while server is running");
        return;
    }
    WS_MUTEX_LOCK(&s->lock);
    char *previous = s->subprotocol;
    s->subprotocol = copy;
    WS_MUTEX_UNLOCK(&s->lock);
    WS_MUTEX_UNLOCK(&s->lifecycle);
    free(previous);
}

/// @brief Return a managed snapshot of the configured subprotocol.
/// @details Native state is copied under the state mutex. A local recovery
///          point frees that snapshot before propagating an allocation trap
///          from managed String construction.
/// @param[in] obj Managed WebSocket server receiver, or NULL.
/// @return Caller-owned managed protocol String; empty when none is configured,
///         or NULL after an invalid-object or allocation trap.
rt_string rt_ws_server_subprotocol(void *obj) {
    if (!obj)
        return rt_str_empty();
    rt_ws_server_impl *s = ws_server_require(obj, "WsServer.Subprotocol: invalid object");
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
        rt_trap("WsServer.Subprotocol: memory allocation failed");
        return NULL;
    }

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char error[512];
        const char *message = rt_trap_get_error();
        int net_code = rt_trap_get_net_code();
        snprintf(error,
                 sizeof(error),
                 "%s",
                 message && message[0] ? message : "WsServer.Subprotocol: allocation failed");
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
/// Snapshots the client list, then sends per client under that client's io
/// mutex so broadcast bytes never interleave with its worker's PONG/CLOSE
/// frames — and a slow peer only stalls its own send, not other clients.
/// @param[in] obj Managed WebSocket server receiver; NULL is a no-op.
/// @param[in] message Managed String payload; NULL sends an empty text message.
void rt_ws_server_broadcast(void *obj, rt_string message) {
    if (!obj)
        return;
    rt_ws_server_impl *s = ws_server_require(obj, "WsServer.Broadcast: invalid object");
    if (!s)
        return;
    if (message && !rt_string_is_handle(message)) {
        rt_trap("WsServer.Broadcast: invalid String");
        return;
    }
    const char *msg = rt_string_cstr(message);
    int64_t len64 = message ? rt_str_len(message) : 0;
    if (len64 < 0 || (uint64_t)len64 > (uint64_t)SIZE_MAX || (len64 > 0 && !msg)) {
        rt_trap("WsServer.Broadcast: invalid String storage");
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
            s->clients[targets[i].slot].tcp == targets[i].tcp && rt_tcp_is_open(targets[i].tcp))
            should_send = 1;
        WS_MUTEX_UNLOCK(&s->lock);

        if (should_send)
            ok = ws_server_send_frame(targets[i].tcp, WS_OP_TEXT, msg, len);

        WS_MUTEX_LOCK(&s->lock);
        if (!ok && targets[i].slot < s->client_count && s->clients[targets[i].slot].occupied &&
            s->clients[targets[i].slot].generation == targets[i].generation &&
            s->clients[targets[i].slot].tcp == targets[i].tcp) {
            ws_release_tcp(&s->clients[targets[i].slot].tcp);
            s->clients[targets[i].slot].active = false;
        }
        WS_MUTEX_UNLOCK(&s->lock);
        WS_MUTEX_UNLOCK(&s->clients[targets[i].slot].io);
    }
}

/// @brief Broadcast one binary frame to every active client.
/// @details Uses the same per-client serialization, generation validation, and
///          failed-client cleanup as the text broadcast.
/// @param[in] obj Managed WebSocket server receiver; NULL is a no-op.
/// @param[in] data Managed Bytes payload; NULL is a no-op.
void rt_ws_server_broadcast_bytes(void *obj, void *data) {
    if (!obj || !data)
        return;
    rt_ws_server_impl *s = ws_server_require(obj, "WsServer.BroadcastBytes: invalid object");
    if (!s)
        return;
    if (!rt_bytes_is_bytes(data)) {
        rt_trap("WsServer.BroadcastBytes: invalid Bytes");
        return;
    }
    int64_t len = rt_bytes_len(data);
    const uint8_t *ptr = len > 0 ? rt_bytes_data_const(data) : NULL;
    if (len < 0 || (uint64_t)len > (uint64_t)SIZE_MAX || (len > 0 && !ptr)) {
        rt_trap("WsServer.BroadcastBytes: invalid Bytes storage");
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
            s->clients[targets[i].slot].tcp == targets[i].tcp && rt_tcp_is_open(targets[i].tcp))
            should_send = 1;
        WS_MUTEX_UNLOCK(&s->lock);

        if (should_send)
            ok = ws_server_send_frame(targets[i].tcp, WS_OP_BINARY, ptr, (size_t)len);

        WS_MUTEX_LOCK(&s->lock);
        if (!ok && targets[i].slot < s->client_count && s->clients[targets[i].slot].occupied &&
            s->clients[targets[i].slot].generation == targets[i].generation &&
            s->clients[targets[i].slot].tcp == targets[i].tcp) {
            ws_release_tcp(&s->clients[targets[i].slot].tcp);
            s->clients[targets[i].slot].active = false;
        }
        WS_MUTEX_UNLOCK(&s->lock);
        WS_MUTEX_UNLOCK(&s->clients[targets[i].slot].io);
    }
}

/// @brief Count active clients (only counts slots flagged active — slots from disconnected
/// clients are skipped). Reads under the mutex for a consistent snapshot.
/// @param[in] obj Managed WebSocket server receiver, or NULL.
/// @return Number of active upgraded clients, or 0 for NULL or invalid receivers.
int64_t rt_ws_server_client_count(void *obj) {
    if (!obj)
        return 0;
    rt_ws_server_impl *s = ws_server_require(obj, "WsServer.ClientCount: invalid object");
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

/// @brief Read the server's configured or bound TCP port.
/// @details Before Start this is the constructor value; after binding it is
///          the actual listener port, including an OS-assigned ephemeral port.
/// @param[in] obj Managed WebSocket server receiver, or NULL.
/// @return Port from 0 through 65535, or 0 for NULL or invalid receivers.
int64_t rt_ws_server_port(void *obj) {
    if (!obj)
        return 0;
    rt_ws_server_impl *s = ws_server_require(obj, "WsServer.Port: invalid object");
    if (!s)
        return 0;
    WS_MUTEX_LOCK(&s->lock);
    int64_t port = s->port;
    WS_MUTEX_UNLOCK(&s->lock);
    return port;
}

/// @brief Returns 1 between successful `_start` and `_stop`; 0 otherwise.
/// @param[in] obj Managed WebSocket server receiver, or NULL.
/// @return 1 while running; otherwise 0.
int8_t rt_ws_server_is_running(void *obj) {
    if (!obj)
        return 0;
    rt_ws_server_impl *s = ws_server_require(obj, "WsServer.IsRunning: invalid object");
    return s && ws_server_is_running_locked(s) ? 1 : 0;
}

/// @brief **Synchronous** accept (alternative to the background `_start` thread). Blocks until
/// a client connects, performs the WebSocket handshake, returns the per-client TCP handle.
/// Returns NULL on accept failure or handshake rejection. Use this when the application owns
/// the accept loop instead of relying on the background broadcast model.
/// @param[in] obj Managed WebSocket server receiver.
/// @return Caller-owned managed TCP client after a successful upgrade, or NULL
///         when no listener exists, accept fails, or the handshake is rejected.
void *rt_ws_server_accept(void *obj) {
    if (!obj)
        return NULL;
    rt_ws_server_impl *s = ws_server_require(obj, "WsServer.Accept: invalid object");
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
        ws_release_managed_reference(&listener);
        rt_trap("WsServer.Accept: memory allocation failed");
        return NULL;
    }
    if (!listener) {
        free(subprotocol);
        return NULL;
    }

    void *tcp = rt_tcp_server_accept(listener);
    ws_release_managed_reference(&listener);
    if (!tcp) {
        free(subprotocol);
        return NULL;
    }

    int upgraded = ws_server_handshake(s, tcp, subprotocol);
    free(subprotocol);
    if (!upgraded) {
        ws_release_tcp(&tcp);
        return NULL;
    }

    return tcp;
}

/// @brief Receive one complete WebSocket message from a client. Handles **frame fragmentation**
/// (assembles continuation frames until FIN), **control frames** (auto-PONG to PING, auto-close
/// echo on CLOSE), and **UTF-8 validation** for text frames. Caps reassembled message size at
/// 64 MiB (per WebSocket security best practice) — over-cap closes with status 0x03F1
/// "Message Too Big". Invalid UTF-8 in text frames closes with 0x03EF "Invalid Payload".
/// Returns the decoded message as rt_string, or empty string on connection close/error.
/// @param[in] tcp Managed upgraded TCP client; NULL returns an empty String.
/// @return Caller-owned managed String containing the exact text or binary
///         message bytes; empty on close/error, or NULL after an invalid-client
///         or result-allocation trap.
rt_string rt_ws_server_client_recv(void *tcp) {
    if (!tcp)
        return rt_str_empty();
    if (!rt_tcp_is_handle(tcp)) {
        rt_trap("WsServer.ClientRecv: invalid TCP client");
        return NULL;
    }

    while (rt_tcp_is_open(tcp)) {
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
                        return rt_str_empty();
                    }

                    if (next_opcode >= 0x08) {
                        if (next_opcode == WS_OP_PING)
                            ws_server_send_frame(tcp, WS_OP_PONG, next_data, next_len);
                        else if (next_opcode == WS_OP_CLOSE)
                            ws_server_send_frame(tcp, WS_OP_CLOSE, next_data, next_len);
                        free(next_data);
                        if (next_opcode == WS_OP_CLOSE) {
                            free(message);
                            return rt_str_empty();
                        }
                        continue;
                    }

                    if (next_opcode != WS_OP_CONTINUATION) {
                        free(next_data);
                        free(message);
                        ws_server_send_frame(tcp, WS_OP_CLOSE, "\x03\xEA", 2);
                        return rt_str_empty();
                    }

                    if (message_len > WS_MAX_MESSAGE_BYTES ||
                        next_len > WS_MAX_MESSAGE_BYTES - message_len) {
                        free(next_data);
                        free(message);
                        ws_server_send_frame(tcp, WS_OP_CLOSE, "\x03\xF1", 2);
                        return rt_str_empty();
                    }

                    if (next_len > 0) {
                        uint8_t *grown = (uint8_t *)realloc(message, message_len + next_len);
                        if (!grown) {
                            free(next_data);
                            free(message);
                            ws_server_send_frame(tcp, WS_OP_CLOSE, "\x03\xF3", 2);
                            return rt_str_empty();
                        }
                        message = grown;
                        memcpy(message + message_len, next_data, next_len);
                    }
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

            jmp_buf result_recovery;
            rt_trap_set_recovery(&result_recovery);
            if (setjmp(result_recovery) != 0) {
                char error[512];
                const char *trap_message = rt_trap_get_error();
                int net_code = rt_trap_get_net_code();
                snprintf(error,
                         sizeof(error),
                         "%s",
                         trap_message && trap_message[0]
                             ? trap_message
                             : "WsServer.ClientRecv: result allocation failed");
                rt_trap_clear_recovery();
                free(message);
                if (net_code != 0)
                    rt_trap_net(error, net_code);
                else
                    rt_trap(error);
                return NULL;
            }
            rt_string result = rt_string_from_bytes((const char *)message, message_len);
            rt_trap_clear_recovery();
            free(message);
            return result;
        }

        free(data);
    }

    return rt_str_empty();
}

/// @brief Send a TEXT frame to a single client. On send failure, closes the connection (caller
/// can then drop the handle). Companion to `_client_recv` for the per-connection message loop.
/// @param[in] tcp Managed upgraded TCP client; NULL is a no-op.
/// @param[in] message Managed String payload; NULL sends an empty frame.
void rt_ws_server_client_send(void *tcp, rt_string message) {
    if (!tcp)
        return;
    if (!rt_tcp_is_handle(tcp)) {
        rt_trap("WsServer.ClientSend: invalid TCP client");
        return;
    }
    if (message && !rt_string_is_handle(message)) {
        rt_trap("WsServer.ClientSend: invalid String");
        return;
    }
    const char *msg = rt_string_cstr(message);
    int64_t len64 = message ? rt_str_len(message) : 0;
    if (len64 < 0 || (uint64_t)len64 > (uint64_t)SIZE_MAX || (len64 > 0 && !msg)) {
        rt_trap("WsServer.ClientSend: invalid String storage");
        return;
    }
    size_t len = (size_t)len64;
    if (!ws_server_send_frame(tcp, WS_OP_TEXT, msg, len))
        rt_tcp_close(tcp);
}

/// @brief Send a polite WebSocket CLOSE frame (no payload) and tear down the TCP connection.
/// Use when terminating a single client without affecting the server's other connections.
/// @param[in] tcp Managed upgraded TCP client; NULL is a no-op.
void rt_ws_server_client_close(void *tcp) {
    if (!tcp)
        return;
    if (!rt_tcp_is_handle(tcp)) {
        rt_trap("WsServer.ClientClose: invalid TCP client");
        return;
    }
    ws_server_send_frame(tcp, WS_OP_CLOSE, NULL, 0);
    rt_tcp_close(tcp);
}
