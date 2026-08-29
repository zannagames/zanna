//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//

// Platform feature macros must appear before ANY includes.
#ifndef _DARWIN_C_SOURCE
/** Enable Darwin/BSD socket extensions required by the network adapter. */
#define _DARWIN_C_SOURCE 1 // macOS: expose BSD extensions
#endif
#ifndef _GNU_SOURCE
/** Enable GNU socket extensions required by the Linux network adapter. */
#define _GNU_SOURCE 1 // Linux: expose ip_mreq
#endif

//
// File: src/runtime/network/rt_network.c
// Purpose: TCP client and server support for Zanna.Network.Tcp and TcpServer.
//   Contains platform initialization (WSA), socket helpers, TCP connection
//   creation, send/receive, and server accept. UDP lives in rt_network_udp.c,
//   DNS in rt_network_dns.c.
// Key invariants:
//   - Every TCP and TcpServer receiver is validated by stable managed class
//     identity and payload size before a private-structure cast.
//   - TCP pool lease tokens are atomic and exclusive across connection pools.
//   - Listener close waits for registered non-blocking accept operations before
//     closing the descriptor, preventing descriptor-number reuse (ABA).
//   - Raw TCP I/O remains externally serialized as documented by the public API.
// Ownership/Lifetime:
//   - TCP/TcpServer objects own their socket and immutable endpoint strings and
//     close/free them from idempotent finalizers.
//   - Endpoint views are borrowed; pool tokens never retain a pool object.
//
// Links: src/runtime/network/rt_network_internal.h (platform abstractions),
//        src/runtime/network/rt_network_udp.c (UDP sockets),
//        src/runtime/network/rt_network_dns.c (DNS resolution),
//        src/runtime/network/rt_network.h (public API)
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements managed blocking TCP connections and listeners.
 * @details Provides platform initialization, exact endpoint validation,
 * timed multi-address connection, partial and complete I/O, persistent
 * timeouts, atomic pool leasing, and close-safe listener accept registration
 * that prevents descriptor-reuse races.
 */

#include "rt_network_internal.h"

#include "rt_heap.h"
#include "rt_platform.h"
#include "rt_time.h"
#include "rt_trap.h"

#include <setjmp.h>

//=============================================================================
// Tcp Connection Structure
//=============================================================================

/** Complete managed TCP payload with immutable endpoint and mutable socket state. */
typedef struct rt_tcp {
    uint64_t magic;                     ///< Fully initialized handle sentinel.
    socket_t sock;                      ///< Owned socket descriptor.
    char *host;                         ///< Owned immutable remote host text.
    int port;                           ///< Remote port.
    int local_port;                     ///< Captured local port.
    bool is_open;                       ///< Native connection state.
    int recv_timeout_ms;                ///< Receive timeout; zero means none.
    int send_timeout_ms;                ///< Send timeout; zero means none.
    volatile uint64_t pool_owner_token; ///< Exclusive ConnectionPool lease identity or zero.
} rt_tcp_t;

//=============================================================================
// TcpServer Structure
//=============================================================================

/** Managed TCP listener payload with atomic accept/close coordination. */
typedef struct rt_tcp_server {
    uint64_t magic;              ///< Fully initialized handle sentinel.
    socket_t sock;               ///< Listener stable while active accepts remain.
    char *address;               ///< Owned recorded bound address.
    int port;                    ///< Actual listening port.
    volatile int is_listening;   ///< Atomic accepting/closed state.
    volatile int active_accepts; ///< Registered operations allowed to access @ref sock.
} rt_tcp_server_t;

/** Payload sentinel for fully initialized managed TCP handles. */
#define RT_TCP_MAGIC UINT64_C(0x5A54435048414E44)
/** Payload sentinel for fully initialized managed TcpServer handles. */
#define RT_TCP_SERVER_MAGIC UINT64_C(0x5A54435053525652)

/// @brief Release one temporary managed object owned by a TCP operation.
/// @details Decrements the object's managed reference count and invokes its
///          finalizer/free path only when the count reaches zero. Receive
///          helpers use this after both ordinary failures and locally recovered
///          allocation traps so partially constructed results cannot leak.
/// @param obj Owned managed reference, or NULL for a no-op.
static void tcp_release_managed(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Copy the active trap diagnostic before removing a recovery frame.
/// @details @ref rt_trap_clear_recovery clears the thread-local diagnostic, so
///          trap-safe cleanup must first preserve the text in caller-owned
///          storage. The fallback is used when an embedder reports an empty
///          diagnostic.
/// @param buffer Destination buffer for the copied, NUL-terminated message.
/// @param buffer_size Size of @p buffer in bytes.
/// @param fallback Diagnostic used when the active trap has no message.
static void tcp_save_trap_error(char *buffer, size_t buffer_size, const char *fallback) {
    const char *error = rt_trap_get_error();
    snprintf(buffer, buffer_size, "%s", error && error[0] ? error : fallback);
}

//=============================================================================
// Managed Handle Validation and Pool Leasing
//=============================================================================

/// @brief Return a TCP implementation pointer without trapping.
/// @details Validates heap kind, the stable TCP class id, and the complete
///          implementation payload size. This form is used by internal probes
///          that must remain safe while holding another subsystem's lock.
/// @param obj Candidate managed object.
/// @return Valid TCP payload, or NULL.
static rt_tcp_t *tcp_try(void *obj) {
    if (!rt_obj_is_instance(obj, RT_TCP_CLASS_ID, sizeof(rt_tcp_t)))
        return NULL;
    rt_tcp_t *tcp = (rt_tcp_t *)obj;
    return tcp->magic == RT_TCP_MAGIC ? tcp : NULL;
}

/// @brief Validate a required public TCP receiver and report misuse.
/// @details A null receiver preserves the long-standing `NULL connection`
///          diagnostic; a non-null wrong-class or undersized value reports an
///          invalid-handle diagnostic. Callers must test the return value
///          because embedders may install a trap hook that returns.
/// @param obj Caller-supplied receiver.
/// @return Valid TCP payload, or NULL after exactly one trap.
static rt_tcp_t *tcp_require(void *obj) {
    if (!obj) {
        rt_trap("Network: NULL connection");
        return NULL;
    }
    rt_tcp_t *tcp = tcp_try(obj);
    if (!tcp) {
        rt_trap("Network: invalid TCP connection");
        return NULL;
    }
    return tcp;
}

/// @brief Retain and validate a required public TcpServer receiver.
/// @details Performs a non-trapping live retain before checking stable class
///          identity, complete payload size, and initialization magic. The
///          retained reference prevents concurrent finalization until the
///          public operation calls @ref tcp_release_managed. Callers must stop
///          when NULL is returned so a returning trap hook cannot cause a blind
///          cast.
/// @param obj Caller-supplied receiver.
/// @return Retained valid server payload, or NULL after exactly one trap.
static rt_tcp_server_t *tcp_server_require_retained(void *obj) {
    if (!obj) {
        rt_trap("Network: NULL server");
        return NULL;
    }
    if (rt_heap_try_retain_live(obj) != 1) {
        rt_trap("Network: invalid TCP server");
        return NULL;
    }
    if (!rt_obj_is_instance(obj, RT_TCP_SERVER_CLASS_ID, sizeof(rt_tcp_server_t)) ||
        ((rt_tcp_server_t *)obj)->magic != RT_TCP_SERVER_MAGIC) {
        tcp_release_managed(obj);
        rt_trap("Network: invalid TCP server");
        return NULL;
    }
    return (rt_tcp_server_t *)obj;
}

/// @brief Register one accept operation before it reads the listener descriptor.
/// @details The operation increments @c active_accepts with compare-and-swap,
///          then rechecks the listening flag. A concurrent Close first clears
///          that flag and waits for the counter to drain before closing the
///          descriptor, eliminating close/reuse ABA between readiness and
///          `accept()`. The counter never wraps.
/// @param server Retained, fully initialized TcpServer payload.
/// @return 1 when registered, 0 when close won the race, or -1 on counter
///         saturation.
static int tcp_server_begin_accept(rt_tcp_server_t *server) {
    if (!server || !rt_atomic_load_i32(&server->is_listening, __ATOMIC_ACQUIRE))
        return 0;

    int active = rt_atomic_load_i32(&server->active_accepts, __ATOMIC_RELAXED);
    for (;;) {
        if (active < 0 || active == INT_MAX)
            return -1;
        int expected = active;
        if (rt_atomic_compare_exchange_i32(
                &server->active_accepts, &expected, active + 1, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
            break;
        active = expected;
    }

    if (rt_atomic_load_i32(&server->is_listening, __ATOMIC_ACQUIRE))
        return 1;
    int previous = rt_atomic_fetch_sub_i32(&server->active_accepts, 1, __ATOMIC_RELEASE);
    if (previous <= 0)
        rt_abort("Network: TcpServer accept counter underflow");
    return 0;
}

/// @brief Unregister one completed accept operation.
/// @details Publishes all descriptor accesses before decrementing the active
///          counter. A closing thread waits on the acquire load of this counter
///          before it invalidates and closes the listener socket.
/// @param server Retained TcpServer whose accept operation was registered.
static void tcp_server_end_accept(rt_tcp_server_t *server) {
    if (!server)
        return;
    int previous = rt_atomic_fetch_sub_i32(&server->active_accepts, 1, __ATOMIC_RELEASE);
    if (previous <= 0)
        rt_abort("Network: TcpServer accept counter underflow");
}

/// @brief Stop a listener after every registered accept operation leaves.
/// @details Exactly one caller changes the listening state from one to zero.
///          That caller waits for bounded-slice, non-blocking accept loops to
///          drain, then invalidates and closes the descriptor. Other concurrent
///          Close calls are idempotent. Keeping the native descriptor open
///          until the counter reaches zero prevents the OS from reusing its
///          numeric value while an older operation still holds a snapshot.
/// @param server Fully initialized TcpServer payload.
static void tcp_server_stop(rt_tcp_server_t *server) {
    if (!server)
        return;
    if (!rt_atomic_exchange_i32(&server->is_listening, 0, __ATOMIC_ACQ_REL))
        return;

    while (rt_atomic_load_i32(&server->active_accepts, __ATOMIC_ACQUIRE) != 0)
        rt_sleep_ms(1);

    socket_t sock = server->sock;
    server->sock = INVALID_SOCK;
    if (sock != INVALID_SOCK)
        CLOSE_SOCKET(sock);
}

/// @brief Determine whether an object is a fully initialized TCP handle.
/// @details Performs the same class, payload-size, and magic validation as the
///          internal non-trapping probe. The object is neither retained nor
///          modified, so the caller must already guarantee its lifetime.
/// @param obj Candidate managed object; NULL and unrelated objects are valid
///        inputs.
/// @return 1 when @p obj is a valid TCP handle, otherwise 0.
int rt_tcp_is_handle(void *obj) {
    return tcp_try(obj) ? 1 : 0;
}

/// @brief Borrow the immutable remote endpoint stored in a TCP object.
/// @details Clears every non-NULL output before validation. On success the host
///          pointer aliases storage owned by @p obj and remains valid only
///          while that TCP object is alive; callers must not free or modify it.
///          This probe never traps or retains the object.
/// @param obj Candidate TCP handle.
/// @param host_out Receives the borrowed NUL-terminated host string.
/// @param host_len_out Receives the host length in bytes, excluding the
///        terminator.
/// @param port_out Receives the numeric remote port.
/// @return 1 when all outputs are present and @p obj is valid, otherwise 0.
int rt_tcp_endpoint_view(void *obj, const char **host_out, size_t *host_len_out, int *port_out) {
    if (host_out)
        *host_out = NULL;
    if (host_len_out)
        *host_len_out = 0;
    if (port_out)
        *port_out = 0;
    if (!host_out || !host_len_out || !port_out)
        return 0;

    rt_tcp_t *tcp = tcp_try(obj);
    if (!tcp || !tcp->host)
        return 0;
    *host_out = tcp->host;
    *host_len_out = strlen(tcp->host);
    *port_out = tcp->port;
    return 1;
}

/// @brief Atomically claim a TCP for one nonzero connection-pool identity.
/// @details Claims an unowned connection with acquire-release ordering. A
///          repeated claim by the current owner is idempotently successful;
///          a different live owner is never displaced. The operation neither
///          traps nor retains the TCP object.
/// @param obj Candidate TCP handle whose lifetime is externally protected.
/// @param owner_token Nonzero stable identity of the claiming pool.
/// @return 1 when the connection is newly or already owned by @p owner_token,
///         otherwise 0.
int rt_tcp_pool_try_claim(void *obj, uint64_t owner_token) {
    rt_tcp_t *tcp = tcp_try(obj);
    if (!tcp || owner_token == 0)
        return 0;

    uint64_t expected = 0;
    if (rt_atomic_compare_exchange_u64(
            &tcp->pool_owner_token, &expected, owner_token, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return 1;
    return expected == owner_token ? 1 : 0;
}

/// @brief Atomically clear a TCP pool lease owned by the expected pool.
/// @details Uses compare-and-swap so a stale pool cannot release a connection
///          owned by another pool. The operation is non-trapping and leaves
///          the lease unchanged when validation or ownership comparison fails.
/// @param obj Candidate TCP handle whose lifetime is externally protected.
/// @param owner_token Nonzero token expected to own the connection.
/// @return 1 when the matching lease was cleared, otherwise 0.
int rt_tcp_pool_release_claim(void *obj, uint64_t owner_token) {
    rt_tcp_t *tcp = tcp_try(obj);
    if (!tcp || owner_token == 0)
        return 0;
    uint64_t expected = owner_token;
    return rt_atomic_compare_exchange_u64(
        &tcp->pool_owner_token, &expected, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

/// @brief Snapshot a TCP connection's current pool lease token.
/// @details Reads the token with acquire ordering without trapping, retaining,
///          or changing the connection.
/// @param obj Candidate TCP handle whose lifetime is externally protected.
/// @return Current nonzero owner token, or 0 when unowned or invalid.
uint64_t rt_tcp_pool_owner(void *obj) {
    rt_tcp_t *tcp = tcp_try(obj);
    return tcp ? rt_atomic_load_u64(&tcp->pool_owner_token, __ATOMIC_ACQUIRE) : 0;
}

/// @brief Parse a numeric TCP service string returned by `getnameinfo`.
/// @details The accept path requests `NI_NUMERICSERV`, so successful calls
///          should return only decimal digits. This helper validates every
///          byte, rejects overflow, and falls back to 0 for malformed values
///          rather than accepting `atoi`'s partial parses.
/// @param service NUL-terminated numeric service string.
/// @return Port number in the inclusive range [0, 65535], or 0 on invalid input.
static int parse_numeric_service_port(const char *service) {
    unsigned int port = 0;
    if (!service || !*service)
        return 0;
    for (const unsigned char *p = (const unsigned char *)service; *p; ++p) {
        if (*p < '0' || *p > '9')
            return 0;
        unsigned int digit = (unsigned int)(*p - '0');
        if (port > (65535u - digit) / 10u)
            return 0;
        port = port * 10u + digit;
    }
    return (int)port;
}

/// @brief Expose a runtime string only when it is safe for native C APIs.
/// @details Validates the managed string handle and host-sized length, then
///          rejects any NUL byte inside the logical payload. Both outputs are
///          cleared before string validation. The returned pointer is borrowed
///          from @p value; an empty string may use a static empty literal.
/// @param value Candidate managed runtime string.
/// @param out Receives a borrowed NUL-terminated byte pointer on success.
/// @param len_out Receives the exact logical byte length, excluding the
///        terminator.
/// @return 1 when both outputs are non-NULL and @p value is a valid
///         NUL-free string, otherwise 0.
int rt_net_cstr_no_embedded_nul(rt_string value, const char **out, size_t *len_out) {
    if (!out || !len_out)
        return 0;
    *out = NULL;
    *len_out = 0;
    if (!value || !rt_string_is_handle((const void *)value))
        return 0;

    int64_t len64 = rt_str_len(value);
    if (len64 < 0 || (uint64_t)len64 > (uint64_t)SIZE_MAX)
        return 0;
    const char *cstr = rt_string_cstr(value);
    if (!cstr && len64 > 0)
        return 0;
    size_t len = (size_t)len64;
    if (cstr && memchr(cstr, '\0', len) != NULL)
        return 0;

    *out = cstr ? cstr : "";
    *len_out = len;
    return 1;
}

/// @brief Finalize a managed TCP client and release its native resources.
/// @details Clears any connection-pool lease, closes an open socket, frees the
///          owned host copy, and invalidates the implementation magic. The
///          cleanup is safe to invoke with NULL or after the socket was
///          explicitly detached or closed.
/// @param obj TCP payload being finalized, or NULL for a no-op.
static void rt_tcp_finalize(void *obj) {
    if (!obj)
        return;
    rt_tcp_t *tcp = (rt_tcp_t *)obj;
    rt_atomic_store_u64(&tcp->pool_owner_token, 0, __ATOMIC_RELEASE);
    if (tcp->is_open) {
        CLOSE_SOCKET(tcp->sock);
        tcp->is_open = false;
    }
    if (tcp->host) {
        free(tcp->host);
        tcp->host = NULL;
    }
    tcp->magic = 0;
}

/// @brief Finalize a managed TCP server and release its native resources.
/// @details Stops the listener only after registered accept operations have
///          drained, then frees the owned bound-address copy and invalidates
///          the implementation magic. NULL is accepted as a no-op.
/// @param obj TcpServer payload being finalized, or NULL.
static void rt_tcp_server_finalize(void *obj) {
    if (!obj)
        return;
    rt_tcp_server_t *server = (rt_tcp_server_t *)obj;
    tcp_server_stop(server);
    if (server->address) {
        free(server->address);
        server->address = NULL;
    }
    server->magic = 0;
}

//=============================================================================
// Socket Helpers
//=============================================================================

/// @brief Best-effort enablement of TCP_NODELAY on a native socket.
/// @details Requests immediate transmission of small writes. Failure is
///          intentionally ignored because disabling Nagle's algorithm is an
///          optimization and must not invalidate an established connection.
/// @param sock Connected native TCP socket.
static void set_nodelay(socket_t sock) {
    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&flag, sizeof(flag));
}

/// @brief Narrow a millisecond timeout to the range supported by socket APIs.
/// @details Accepts zero as the conventional infinite or disabled timeout and
///          rejects negative values and values greater than @c INT_MAX.
/// @param timeout_ms Candidate timeout in milliseconds.
/// @param out_timeout_ms Receives the narrowed timeout on success; unchanged on
///        failure.
/// @return 1 when the value and output pointer are valid, otherwise 0.
int rt_net_timeout_ms_to_int(int64_t timeout_ms, int *out_timeout_ms) {
    if (!out_timeout_ms || timeout_ms < 0 || timeout_ms > INT_MAX)
        return 0;
    *out_timeout_ms = (int)timeout_ms;
    return 1;
}

/// @brief Narrow a byte count to the range supported by native socket calls.
/// @param byte_count Candidate nonnegative byte count.
/// @param out_len Receives the narrowed count on success; unchanged on failure.
/// @return 1 when @p byte_count is in [0, @c INT_MAX] and @p out_len is
///         non-NULL, otherwise 0.
int rt_net_i64_len_to_int(int64_t byte_count, int *out_len) {
    if (!out_len || byte_count < 0 || byte_count > INT_MAX)
        return 0;
    *out_len = (int)byte_count;
    return 1;
}

/// @brief Query the local port assigned to a native socket.
/// @details Supports IPv4 and IPv6 socket addresses and converts the network
///          byte-order port to host order.
/// @param sock Bound or connected native socket.
/// @return Local port in [1, 65535], or 0 when the query or address-family
///         interpretation fails.
static int get_local_port(socket_t sock) {
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    if (getsockname(sock, (struct sockaddr *)&addr, &len) == 0) {
        if (addr.ss_family == AF_INET)
            return ntohs(((struct sockaddr_in *)&addr)->sin_port);
        if (addr.ss_family == AF_INET6)
            return ntohs(((struct sockaddr_in6 *)&addr)->sin6_port);
    }
    return 0;
}

/// @brief Transfer an established native socket and host buffer into a Tcp object.
/// @details Installs a local trap recovery point around managed object
///          allocation. If allocation traps, the accepted/connected socket and
///          native host copy are released before the original diagnostic is
///          re-raised. On success, ownership transfers to the initialized Tcp
///          finalizer and the caller receives reference count one.
/// @param sock Owned connected socket; consumed on every return path.
/// @param host_cstr Owned NUL-terminated remote-host copy; consumed on every
///        return path.
/// @param remote_port Numeric peer port.
/// @param local_port Numeric local port assigned to @p sock.
/// @return Newly initialized Tcp object, or NULL after a returning trap hook.
static void *tcp_adopt_connected_socket(socket_t sock,
                                        char *host_cstr,
                                        int remote_port,
                                        int local_port) {
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[512];
        tcp_save_trap_error(
            saved_error, sizeof(saved_error), "Network: TCP object allocation failed");
        rt_trap_clear_recovery();
        if (sock != INVALID_SOCK)
            CLOSE_SOCKET(sock);
        free(host_cstr);
        rt_trap(saved_error);
        return NULL;
    }

    rt_tcp_t *tcp = (rt_tcp_t *)rt_obj_new_i64(RT_TCP_CLASS_ID, (int64_t)sizeof(rt_tcp_t));
    rt_trap_clear_recovery();
    if (!tcp) {
        if (sock != INVALID_SOCK)
            CLOSE_SOCKET(sock);
        free(host_cstr);
        return NULL;
    }

    tcp->magic = RT_TCP_MAGIC;
    tcp->sock = sock;
    tcp->host = host_cstr;
    tcp->port = remote_port;
    tcp->local_port = local_port;
    tcp->is_open = true;
    tcp->recv_timeout_ms = 0;
    tcp->send_timeout_ms = 0;
    tcp->pool_owner_token = 0;
    rt_obj_set_finalizer(tcp, rt_tcp_finalize);
    return tcp;
}

/// @brief Transfer a native listener and address buffer into a TcpServer object.
/// @details Managed allocation is wrapped in a local recovery frame so a
///          recoverable allocation trap closes the listener and frees the
///          address copy. The listener must already be non-blocking; successful
///          publication initializes both atomic lifecycle fields before the
///          finalizer is installed.
/// @param sock Owned non-blocking listener; consumed on every return path.
/// @param address_cstr Owned NUL-terminated bound-address copy; consumed on
///        every return path.
/// @param bound_port Actual bound port, including an OS-selected ephemeral port.
/// @return Newly initialized TcpServer, or NULL after a returning trap hook.
static void *tcp_server_adopt_listener(socket_t sock, char *address_cstr, int bound_port) {
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[512];
        tcp_save_trap_error(
            saved_error, sizeof(saved_error), "Network: TCP server allocation failed");
        rt_trap_clear_recovery();
        if (sock != INVALID_SOCK)
            CLOSE_SOCKET(sock);
        free(address_cstr);
        rt_trap(saved_error);
        return NULL;
    }

    rt_tcp_server_t *server =
        (rt_tcp_server_t *)rt_obj_new_i64(RT_TCP_SERVER_CLASS_ID, (int64_t)sizeof(rt_tcp_server_t));
    rt_trap_clear_recovery();
    if (!server) {
        if (sock != INVALID_SOCK)
            CLOSE_SOCKET(sock);
        free(address_cstr);
        return NULL;
    }

    server->magic = RT_TCP_SERVER_MAGIC;
    server->sock = sock;
    server->address = address_cstr;
    server->port = bound_port;
    rt_atomic_store_i32(&server->active_accepts, 0, __ATOMIC_RELAXED);
    rt_atomic_store_i32(&server->is_listening, 1, __ATOMIC_RELEASE);
    rt_obj_set_finalizer(server, rt_tcp_server_finalize);
    return server;
}

/// @brief Connect a native socket with optional bounded waiting.
/// @details A zero timeout performs a normal blocking @c connect. For a
///          positive timeout the socket is switched to non-blocking mode,
///          connection progress is awaited for write readiness, @c SO_ERROR is
///          checked, and blocking mode is restored after success. The caller
///          owns and must close the socket after every failure; a timed failure
///          may leave it non-blocking because it is not reused.
/// @param sock Unconnected native socket.
/// @param addr Destination socket address.
/// @param addrlen Size of @p addr in bytes.
/// @param timeout_ms Nonnegative timeout in milliseconds; zero selects
///        blocking behavior.
/// @param err_out Optional output for the native error code; initialized to
///        zero and set to @c ETIMEDOUT when readiness expires.
/// @return true when the socket is connected and blocking, otherwise false.
static bool connect_socket_with_timeout(
    socket_t sock, const struct sockaddr *addr, socklen_t addrlen, int timeout_ms, int *err_out) {
    if (err_out)
        *err_out = 0;

    if (timeout_ms > 0) {
        if (!rt_socket_set_nonblocking(sock, true)) {
            if (err_out)
                *err_out = GET_LAST_ERROR();
            return false;
        }

        int connect_result = connect(sock, addr, addrlen);
        if (connect_result == SOCK_ERROR) {
            int err = GET_LAST_ERROR();
            if (rt_socket_error_is_in_progress(err)) {
                int ready = wait_socket(sock, timeout_ms, true);
                if (ready <= 0) {
                    if (err_out)
                        *err_out = ready == 0 ? ETIMEDOUT : GET_LAST_ERROR();
                    return false;
                }

                int so_error = 0;
                socklen_t len = sizeof(so_error);
                getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)&so_error, &len);
                if (so_error != 0) {
                    if (err_out)
                        *err_out = so_error;
                    return false;
                }
            } else {
                if (err_out)
                    *err_out = err;
                return false;
            }
        }

        if (!rt_socket_set_nonblocking(sock, false)) {
            if (err_out)
                *err_out = GET_LAST_ERROR();
            return false;
        }
        return true;
    }

    if (connect(sock, addr, addrlen) == SOCK_ERROR) {
        if (err_out)
            *err_out = GET_LAST_ERROR();
        return false;
    }

    return true;
}

//=============================================================================
// Tcp Client - Connection Creation
//=============================================================================

// ===========================================================================
// TCP client public API
//
// Each `rt_tcp_*` function operates on a GC-managed `rt_tcp_t`
// (or NULL). Connect functions return a fresh client; send/recv
// take a client handle; properties (`host`, `port`, `is_open`)
// expose state for diagnostics.
// ===========================================================================

/// @brief Connect to a remote TCP endpoint with the default timeout.
/// @details Delegates to @ref rt_tcp_connect_for with a 30-second deadline so
///          unreachable endpoints cannot block indefinitely.
/// @param host Managed hostname or numeric-address string.
/// @param port Remote port in the inclusive range [1, 65535].
/// @return Newly allocated connected TCP object, or NULL after reporting an
///         invalid argument, resolution failure, timeout, or socket error.
/// @see rt_tcp_connect_for
void *rt_tcp_connect(rt_string host, int64_t port) {
    // Default 30-second timeout prevents indefinite blocking on unreachable hosts.
    return rt_tcp_connect_for(host, port, 30000);
}

/// @brief Connect to a remote TCP endpoint with an explicit timeout.
/// @details Validates a nonempty host with no embedded NUL, resolves both IPv4
///          and IPv6 candidates, and attempts each address in resolver order.
///          Successful sockets suppress SIGPIPE, best-effort enable
///          TCP_NODELAY, and transfer ownership with an allocated host copy to
///          the returned managed object. The timeout applies independently to
///          each candidate address; zero requests blocking connects.
/// @param host Managed hostname or numeric-address string.
/// @param port Remote port in the inclusive range [1, 65535].
/// @param timeout_ms Per-address connection timeout in milliseconds, in
///        [0, @c INT_MAX].
/// @return Newly allocated connected TCP object, or NULL after a returning
///         trap hook.
/// @throws Err_HostNotFound when name resolution fails.
/// @throws Err_ConnectionRefused when every candidate refuses the connection.
/// @throws Err_Timeout when the last connection attempt expires.
/// @throws Err_NetworkError for other connection failures.
void *rt_tcp_connect_for(rt_string host, int64_t port, int64_t timeout_ms) {
    rt_net_init_wsa();

    const char *host_ptr = NULL;
    size_t host_len = 0;
    if (!rt_net_cstr_no_embedded_nul(host, &host_ptr, &host_len) || host_len == 0) {
        rt_trap("Network: invalid host");
        return NULL;
    }

    if (port < 1 || port > 65535) {
        rt_trap("Network: invalid port number");
        return NULL;
    }
    int timeout_int = 0;
    if (!rt_net_timeout_ms_to_int(timeout_ms, &timeout_int)) {
        rt_trap("Network: invalid timeout");
        return NULL;
    }

    // Copy host string
    char *host_cstr = (char *)malloc(host_len + 1);
    if (!host_cstr) {
        rt_trap("Network: memory allocation failed");
        return NULL;
    }
    memcpy(host_cstr, host_ptr, host_len);
    host_cstr[host_len] = '\0';

    // Resolve hostname
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", (int)port);

    int status = getaddrinfo(host_cstr, port_str, &hints, &res);
    if (status != 0) {
        free(host_cstr);
        rt_trap_net("Network: host not found", Err_HostNotFound);
        return NULL;
    }

    socket_t sock = INVALID_SOCK;
    int last_err = 0;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        socket_t candidate = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (candidate == INVALID_SOCK)
            continue;

        suppress_sigpipe(candidate);
        if (connect_socket_with_timeout(
                candidate, rp->ai_addr, (socklen_t)rp->ai_addrlen, timeout_int, &last_err)) {
            sock = candidate;
            break;
        }

        CLOSE_SOCKET(candidate);
    }
    freeaddrinfo(res);

    if (sock == INVALID_SOCK) {
        free(host_cstr);
        if (last_err == CONN_REFUSED) {
            rt_trap_net("Network: connection refused", Err_ConnectionRefused);
            return NULL;
        }
        if (rt_socket_error_is_timeout(last_err)) {
            rt_trap_net("Network: connection timeout", Err_Timeout);
            return NULL;
        }
        rt_trap_net("Network: connection failed", Err_NetworkError);
        return NULL;
    }

    // Enable TCP_NODELAY
    set_nodelay(sock);

    return tcp_adopt_connected_socket(sock, host_cstr, (int)port, get_local_port(sock));
}

//=============================================================================
// Tcp Client - Properties
//=============================================================================

// ---------------------------------------------------------------------------
// TCP client property accessors — null-safe getters returning the
// stored host/port, the local socket port (post-bind), and
// open/available state. Each is a trivial reach into `rt_tcp_t`.
// ---------------------------------------------------------------------------

/// @brief Return the host string used to open a TCP connection.
/// @details Validates the receiver and creates a managed string from the
///          immutable native host copy. Closing the socket does not erase this
///          endpoint metadata.
/// @param obj Required TCP receiver.
/// @return Managed host string, or the runtime empty-string sentinel after a
///         returning invalid-handle trap.
rt_string rt_tcp_host(void *obj) {
    rt_tcp_t *tcp = tcp_require(obj);
    if (!tcp)
        return rt_str_empty();
    return rt_const_cstr(tcp->host);
}

/// @brief Return the remote port recorded for a TCP connection.
/// @param obj Required TCP receiver.
/// @return Remote port, or 0 after a returning invalid-handle trap.
int64_t rt_tcp_port(void *obj) {
    rt_tcp_t *tcp = tcp_require(obj);
    if (!tcp)
        return 0;
    return tcp->port;
}

/// @brief Return the local port assigned to a TCP connection.
/// @details The value is captured when the socket is adopted and remains
///          available after the connection is closed.
/// @param obj Required TCP receiver.
/// @return Local port, or 0 after a returning invalid-handle trap or an
///         unavailable native port query during connection creation.
int64_t rt_tcp_local_port(void *obj) {
    rt_tcp_t *tcp = tcp_require(obj);
    if (!tcp)
        return 0;
    return tcp->local_port;
}

/// @brief Test whether a TCP object's native socket is still open.
/// @param obj Required TCP receiver.
/// @return 1 while the socket is open, otherwise 0; invalid receivers trap and
///         return 0 only when the trap hook resumes execution.
int8_t rt_tcp_is_open(void *obj) {
    rt_tcp_t *tcp = tcp_require(obj);
    if (!tcp)
        return 0;
    return tcp->is_open ? 1 : 0;
}

/// @brief Query the bytes currently available for a non-blocking TCP read.
/// @details Uses the platform @c FIONREAD equivalent. This is a best-effort
///          snapshot: zero can mean no queued bytes, a closed connection, or a
///          failed native query, and later receives may still block.
/// @param obj Required TCP receiver.
/// @return Nonnegative queued-byte count, or 0 when closed, unavailable, or
///         invalid after a returning trap.
int64_t rt_tcp_available(void *obj) {
    rt_tcp_t *tcp = tcp_require(obj);
    if (!tcp)
        return 0;
    if (!tcp->is_open)
        return 0;

    int64_t bytes_available = 0;
    if (!rt_socket_available_bytes(tcp->sock, &bytes_available))
        return 0;
    return bytes_available;
}

//=============================================================================
// Tcp Client - Send Methods
//=============================================================================

/// @brief Perform one native send from a managed Bytes payload.
/// @details Validates the receiver and Bytes handle, sends at most @c INT_MAX
///          bytes, and permits a normal short write. A socket error marks the
///          connection closed before reporting its classified network trap.
/// @param obj Required open TCP receiver.
/// @param data Required managed Bytes object; its storage is borrowed for the
///        duration of the call.
/// @return Number of bytes written, including 0 for an empty payload, or -1
///         after a returning validation or socket-error trap.
int64_t rt_tcp_send(void *obj, void *data) {
    rt_tcp_t *tcp = tcp_require(obj);
    if (!tcp)
        return -1;
    if (!data) {
        rt_trap("Network: NULL data");
        return -1;
    }
    if (!rt_bytes_is_bytes(data)) {
        rt_trap("Network: invalid Bytes data");
        return -1;
    }

    if (!tcp->is_open) {
        rt_trap_net("Network: connection closed", Err_ConnectionClosed);
        return -1;
    }

    int64_t len = bytes_len(data);
    uint8_t *buf = bytes_data(data);

    if (len == 0)
        return 0;

    // Clamp to INT_MAX to prevent silent truncation on large buffers
    int to_send = (len > INT_MAX) ? INT_MAX : (int)len;
    int sent = send(tcp->sock, (const char *)buf, to_send, SEND_FLAGS);
    if (sent == SOCK_ERROR) {
        tcp->is_open = false;
        rt_trap_net("Network: send failed", net_classify_errno());
        return -1;
    }

    return sent;
}

/// @brief Perform one native send from a managed string payload.
/// @details Sends the string's exact stored bytes without relying on
///          NUL-termination, but limits one native call to @c INT_MAX bytes.
///          A normal short write is returned to the caller. Socket errors mark
///          the connection closed and raise a classified network trap.
/// @param obj Required open TCP receiver.
/// @param text Required managed string whose storage is borrowed during send.
/// @return Number of bytes written, including 0 for an empty string, or -1
///         after a returning validation or socket-error trap.
int64_t rt_tcp_send_str(void *obj, rt_string text) {
    rt_tcp_t *tcp = tcp_require(obj);
    if (!tcp)
        return -1;
    if (!tcp->is_open) {
        rt_trap_net("Network: connection closed", Err_ConnectionClosed);
        return -1;
    }

    if (!text || !rt_string_is_handle((const void *)text)) {
        rt_trap("Network: invalid string");
        return -1;
    }
    const char *text_ptr = rt_string_cstr(text);
    int64_t len64 = rt_str_len(text);
    if (len64 < 0 || (uint64_t)len64 > (uint64_t)SIZE_MAX) {
        rt_trap("Network: invalid string length");
        return -1;
    }
    size_t len = (size_t)len64;
    if (len == 0)
        return 0;

    // Clamp to INT_MAX to prevent silent truncation on large strings
    int to_send = (len > INT_MAX) ? INT_MAX : (int)len;
    int sent = send(tcp->sock, text_ptr, to_send, SEND_FLAGS);
    if (sent == SOCK_ERROR) {
        tcp->is_open = false;
        rt_trap_net("Network: send failed", net_classify_errno());
        return -1;
    }

    return sent;
}

/// @brief Send an entire managed Bytes payload.
/// @details Loops over partial writes and chunks larger than @c INT_MAX until
///          every byte is delivered. A zero write or socket error marks the
///          connection closed and reports a classified trap.
/// @param obj Required open TCP receiver.
/// @param data Required managed Bytes object; its storage remains borrowed for
///        the duration of the operation.
void rt_tcp_send_all(void *obj, void *data) {
    rt_tcp_t *tcp = tcp_require(obj);
    if (!tcp)
        return;
    if (!data) {
        rt_trap("Network: NULL data");
        return;
    }
    if (!rt_bytes_is_bytes(data)) {
        rt_trap("Network: invalid Bytes data");
        return;
    }

    if (!tcp->is_open) {
        rt_trap_net("Network: connection closed", Err_ConnectionClosed);
        return;
    }

    int64_t len = bytes_len(data);
    uint8_t *buf = bytes_data(data);

    int64_t total_sent = 0;
    while (total_sent < len) {
        int64_t remaining = len - total_sent;
        int chunk = (int)(remaining > INT_MAX ? INT_MAX : remaining);
        int sent = send(tcp->sock, (const char *)(buf + total_sent), chunk, SEND_FLAGS);
        if (sent == SOCK_ERROR) {
            tcp->is_open = false;
            rt_trap_net("Network: send failed", net_classify_errno());
            return;
        }
        if (sent == 0) {
            tcp->is_open = false;
            rt_trap_net("Network: connection closed by peer", Err_ConnectionClosed);
            return;
        }
        total_sent += sent;
    }
}

/// @brief Send an entire raw byte range through a managed TCP connection.
/// @details Provides HTTP and WebSocket layers with the same chunked,
///          partial-write loop as @ref rt_tcp_send_all without requiring a
///          managed Bytes wrapper. A zero length accepts a NULL data pointer.
///          Any failed or zero write marks the connection closed and traps.
/// @param obj Required open TCP receiver.
/// @param data Borrowed buffer containing at least @p len readable bytes, or
///        NULL only when @p len is zero.
/// @param len Nonnegative number of bytes to send.
void rt_tcp_send_all_raw(void *obj, const void *data, int64_t len) {
    rt_tcp_t *tcp = tcp_require(obj);
    if (!tcp)
        return;
    if (len < 0) {
        rt_trap("Network: invalid data length");
        return;
    }
    if (!data && len > 0) {
        rt_trap("Network: NULL data");
        return;
    }

    if (!tcp->is_open) {
        rt_trap_net("Network: connection closed", Err_ConnectionClosed);
        return;
    }

    if (len == 0)
        return;

    const uint8_t *buf = (const uint8_t *)data;
    int64_t total_sent = 0;
    while (total_sent < len) {
        int64_t remaining = len - total_sent;
        int chunk = (int)(remaining > INT_MAX ? INT_MAX : remaining);
        int sent = send(tcp->sock, (const char *)(buf + total_sent), chunk, SEND_FLAGS);
        if (sent == SOCK_ERROR) {
            tcp->is_open = false;
            rt_trap_net("Network: send failed", net_classify_errno());
            return;
        }
        if (sent == 0) {
            tcp->is_open = false;
            rt_trap_net("Network: connection closed by peer", Err_ConnectionClosed);
            return;
        }
        total_sent += sent;
    }
}

//=============================================================================
// Tcp Client - Receive Methods
//=============================================================================

/// @brief Receive up to a requested number of bytes in one socket read.
/// @details Allocates a managed buffer, performs a single @c recv, and returns
///          an exactly sized Bytes object when the read is short. An orderly
///          peer close marks the connection closed and returns empty Bytes.
///          Expiry of the persistent receive timeout also returns empty Bytes
///          but leaves the connection open, so these cases are distinguished
///          through @ref rt_tcp_is_open. Other errors close the logical
///          connection and trap. Temporary allocations are released across
///          both returning and nonlocal trap paths.
/// @param obj Required open TCP receiver.
/// @param max_bytes Maximum bytes to read, in [0, @c INT_MAX].
/// @return Newly owned managed Bytes object, which may be empty, or NULL after
///         a returning validation, allocation, or socket-error trap.
void *rt_tcp_recv(void *obj, int64_t max_bytes) {
    rt_tcp_t *tcp = tcp_require(obj);
    if (!tcp)
        return NULL;
    if (!tcp->is_open) {
        rt_trap_net("Network: connection closed", Err_ConnectionClosed);
        return NULL;
    }

    if (max_bytes < 0) {
        rt_trap("Network: invalid receive size");
        return NULL;
    }
    if (max_bytes == 0)
        return rt_bytes_new(0);
    int recv_len = 0;
    if (!rt_net_i64_len_to_int(max_bytes, &recv_len)) {
        rt_trap("Network: receive size too large");
        return NULL;
    }

    // Allocate receive buffer
    void *result = rt_bytes_new(max_bytes);
    if (!result)
        return NULL;
    uint8_t *buf = bytes_data(result);

    int received = recv(tcp->sock, (char *)buf, recv_len, 0);
    if (received == SOCK_ERROR) {
        int receive_error = GET_LAST_ERROR();
        if (rt_socket_error_is_timeout(receive_error) ||
            rt_socket_error_is_would_block(receive_error)) {
            // Timeout - release over-allocated buffer and return empty bytes
            tcp_release_managed(result);
            return rt_bytes_new(0);
        }
        tcp->is_open = false;
        tcp_release_managed(result);
        rt_trap_net("Network: receive failed", net_classify_error(receive_error));
        return NULL;
    }

    if (received == 0) {
        // Connection closed by peer
        tcp->is_open = false;
        tcp_release_managed(result);
        return rt_bytes_new(0);
    }

    // Return exact size received (release over-allocated buffer)
    if (received < max_bytes) {
        jmp_buf recovery;
        rt_trap_set_recovery(&recovery);
        if (RT_SETJMP(recovery) != 0) {
            char saved_error[512];
            tcp_save_trap_error(
                saved_error, sizeof(saved_error), "Network: receive result allocation failed");
            rt_trap_clear_recovery();
            tcp_release_managed(result);
            rt_trap(saved_error);
            return NULL;
        }

        void *exact = rt_bytes_new(received);
        rt_trap_clear_recovery();
        if (!exact) {
            tcp_release_managed(result);
            return NULL;
        }
        memcpy(bytes_data(exact), buf, received);
        tcp_release_managed(result);
        return exact;
    }

    return result;
}

/// @brief Receive bytes once and convert them to a managed string.
/// @details Delegates socket semantics to @ref rt_tcp_recv, converts the
///          returned Bytes payload with the runtime byte-to-string operation,
///          and releases the temporary Bytes reference even if conversion
///          allocation traps.
/// @param obj Required open TCP receiver.
/// @param max_bytes Maximum bytes to read, in [0, @c INT_MAX].
/// @return Newly owned managed string, including an empty string for timeout
///         or orderly close, or the empty-string sentinel after a returning
///         trap.
rt_string rt_tcp_recv_str(void *obj, int64_t max_bytes) {
    void *bytes = rt_tcp_recv(obj, max_bytes);
    if (!bytes)
        return rt_str_empty();

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[512];
        tcp_save_trap_error(
            saved_error, sizeof(saved_error), "Network: receive string allocation failed");
        rt_trap_clear_recovery();
        tcp_release_managed(bytes);
        rt_trap(saved_error);
        return rt_str_empty();
    }

    rt_string str = rt_bytes_to_str(bytes);
    rt_trap_clear_recovery();
    tcp_release_managed(bytes);
    return str;
}

/// @brief Receive exactly a requested number of bytes.
/// @details Loops until the allocated Bytes buffer is full. Unlike
///          @ref rt_tcp_recv, a configured timeout and premature orderly close
///          are errors rather than empty successful results. Timeout leaves
///          the connection state unchanged; EOF and other socket errors mark
///          it closed. The partial result is released on every failure path.
/// @param obj Required open TCP receiver.
/// @param count Exact byte count in [0, @c INT_MAX].
/// @return Newly owned managed Bytes object of exactly @p count bytes, or NULL
///         after a returning validation, allocation, timeout, EOF, or socket
///         trap.
void *rt_tcp_recv_exact(void *obj, int64_t count) {
    rt_tcp_t *tcp = tcp_require(obj);
    if (!tcp)
        return NULL;
    if (!tcp->is_open) {
        rt_trap_net("Network: connection closed", Err_ConnectionClosed);
        return NULL;
    }

    if (count < 0) {
        rt_trap("Network: invalid receive size");
        return NULL;
    }
    if (count == 0)
        return rt_bytes_new(0);
    if (count > INT_MAX) {
        rt_trap("Network: receive size too large");
        return NULL;
    }

    void *result = rt_bytes_new(count);
    if (!result)
        return NULL;
    uint8_t *buf = bytes_data(result);

    int64_t total_received = 0;
    while (total_received < count) {
        int chunk_len = 0;
        if (!rt_net_i64_len_to_int(count - total_received, &chunk_len)) {
            tcp_release_managed(result);
            rt_trap("Network: receive size too large");
            return NULL;
        }
        int received = recv(tcp->sock, (char *)(buf + total_received), chunk_len, 0);
        if (received == SOCK_ERROR) {
            int receive_error = GET_LAST_ERROR();
            tcp_release_managed(result);
            if (rt_socket_error_is_timeout(receive_error) ||
                rt_socket_error_is_would_block(receive_error)) {
                rt_trap_net("Network: receive timeout", Err_Timeout);
                return NULL;
            }
            tcp->is_open = false;
            rt_trap_net("Network: receive failed", net_classify_error(receive_error));
            return NULL;
        }
        if (received == 0) {
            tcp->is_open = false;
            tcp_release_managed(result);
            rt_trap_net("Network: connection closed before receiving all data",
                        Err_ConnectionClosed);
            return NULL;
        }
        total_received += received;
    }

    return result;
}

/// @brief Receive one newline-terminated line as a managed string.
/// @details Reads one byte at a time until LF, removes an immediately preceding
///          CR, and excludes both terminators from the result. Content before
///          the newline is limited to 64 KiB. Timeout, EOF before LF, socket
///          failure, and overlong lines trap; EOF and non-timeout socket
///          failures also mark the connection closed. The native line buffer
///          is freed even when managed string allocation traps nonlocally.
/// @param obj Required open TCP receiver.
/// @return Newly owned managed line string, or the runtime empty-string
///         sentinel after a returning trap.
rt_string rt_tcp_recv_line(void *obj) {
    rt_tcp_t *tcp = tcp_require(obj);
    if (!tcp)
        return rt_str_empty();
    if (!tcp->is_open) {
        rt_trap_net("Network: connection closed", Err_ConnectionClosed);
        return rt_str_empty();
    }

    // Line buffer with initial capacity
    size_t cap = 256;
    size_t len = 0;
    char *line = (char *)malloc(cap);
    if (!line) {
        rt_trap("Network: memory allocation failed");
        return rt_str_empty();
    }

    while (1) {
        char c;
        int received = recv(tcp->sock, &c, 1, 0);
        if (received == SOCK_ERROR) {
            int receive_error = GET_LAST_ERROR();
            free(line);
            if (rt_socket_error_is_timeout(receive_error) ||
                rt_socket_error_is_would_block(receive_error)) {
                rt_trap_net("Network: receive timeout", Err_Timeout);
                return rt_str_empty();
            }
            tcp->is_open = false;
            rt_trap_net("Network: receive failed", net_classify_error(receive_error));
            return rt_str_empty();
        }
        if (received == 0) {
            free(line);
            tcp->is_open = false;
            rt_trap_net("Network: connection closed before end of line", Err_ConnectionClosed);
            return rt_str_empty();
        }

        if (c == '\n') {
            // Strip trailing CR if present
            if (len > 0 && line[len - 1] == '\r') {
                len--;
            }
            break;
        }

        // Cap at 64KB to prevent unbounded memory growth from a malicious peer.
        if (len >= 65536) {
            free(line);
            rt_trap_net("Network: line exceeds 64KB limit", Err_ProtocolError);
            return rt_str_empty();
        }

        // Add character to buffer, growing if needed.
        if (len >= cap) {
            cap *= 2;
            if (cap > 65536)
                cap = 65536;
            char *new_line = (char *)realloc(line, cap);
            if (!new_line) {
                free(line);
                rt_trap("Network: memory allocation failed");
                return rt_str_empty();
            }
            line = new_line;
        }
        line[len++] = c;
    }

    // Create the managed string under a local recovery frame so the native
    // line buffer is released even when allocation traps via longjmp.
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[512];
        tcp_save_trap_error(
            saved_error, sizeof(saved_error), "Network: receive line allocation failed");
        rt_trap_clear_recovery();
        free(line);
        rt_trap(saved_error);
        return rt_str_empty();
    }

    rt_string result = rt_string_from_bytes(line, len);
    rt_trap_clear_recovery();
    free(line);
    return result;
}

//=============================================================================
// Tcp Client - Timeout and Close
//=============================================================================

/// @brief Configure the persistent native receive timeout.
/// @details Applies @c SO_RCVTIMEO to an open connection and records the
///          narrowed value after the platform call succeeds. Zero disables the
///          timeout according to the socket abstraction.
/// @param obj Required open TCP receiver.
/// @param timeout_ms Timeout in milliseconds, in [0, @c INT_MAX].
void rt_tcp_set_recv_timeout(void *obj, int64_t timeout_ms) {
    rt_tcp_t *tcp = tcp_require(obj);
    if (!tcp)
        return;
    if (!tcp->is_open) {
        rt_trap_net("Network: connection closed", Err_ConnectionClosed);
        return;
    }
    int timeout_int = 0;
    if (!rt_net_timeout_ms_to_int(timeout_ms, &timeout_int)) {
        rt_trap("Network: invalid timeout");
        return;
    }
    if (!set_socket_timeout(tcp->sock, timeout_int, true)) {
        rt_trap_net("Network: setting receive timeout failed", net_classify_errno());
        return;
    }
    tcp->recv_timeout_ms = timeout_int;
}

/// @brief Configure the persistent native send timeout.
/// @details Applies @c SO_SNDTIMEO to an open connection and records the
///          narrowed value after the platform call succeeds. Zero disables the
///          timeout according to the socket abstraction.
/// @param obj Required open TCP receiver.
/// @param timeout_ms Timeout in milliseconds, in [0, @c INT_MAX].
void rt_tcp_set_send_timeout(void *obj, int64_t timeout_ms) {
    rt_tcp_t *tcp = tcp_require(obj);
    if (!tcp)
        return;
    if (!tcp->is_open) {
        rt_trap_net("Network: connection closed", Err_ConnectionClosed);
        return;
    }
    int timeout_int = 0;
    if (!rt_net_timeout_ms_to_int(timeout_ms, &timeout_int)) {
        rt_trap("Network: invalid timeout");
        return;
    }
    if (!set_socket_timeout(tcp->sock, timeout_int, false)) {
        rt_trap_net("Network: setting send timeout failed", net_classify_errno());
        return;
    }
    tcp->send_timeout_ms = timeout_int;
}

/// @brief Close a TCP connection immediately.
/// @details Closing is idempotent for a valid TCP object and NULL is accepted
///          as a no-op. The endpoint metadata remains available, while later
///          send and receive operations report @c Err_ConnectionClosed.
/// @param obj TCP receiver, or NULL for a no-op.
void rt_tcp_close(void *obj) {
    if (!obj)
        return;

    rt_tcp_t *tcp = tcp_require(obj);
    if (!tcp)
        return;
    if (tcp->is_open) {
        CLOSE_SOCKET(tcp->sock);
        tcp->is_open = false;
    }
}

/// @brief Borrow the native socket descriptor from a TCP object.
/// @details This non-trapping internal accessor supports readiness integration
///          and ownership transfer to TLS. It neither retains the object nor
///          transfers the descriptor; callers must externally protect both
///          lifetimes.
/// @param obj Candidate TCP handle.
/// @return Native socket descriptor, or @c INVALID_SOCK for an invalid handle.
socket_t rt_tcp_socket_fd(void *obj) {
    rt_tcp_t *tcp = tcp_try(obj);
    return tcp ? tcp->sock : INVALID_SOCK;
}

/// @brief Detach a native socket from its managed TCP object.
/// @details Marks a valid connection closed and replaces its descriptor with
///          @c INVALID_SOCK without invoking the native close operation. The
///          caller must obtain the descriptor first and thereafter owns its
///          lifetime, typically when TLS or another protocol wrapper takes
///          control. Invalid handles are silently ignored.
/// @param obj TCP handle whose descriptor ownership has already been captured.
void rt_tcp_detach_socket(void *obj) {
    rt_tcp_t *tcp = tcp_try(obj);
    if (!tcp)
        return;
    tcp->sock = INVALID_SOCK;
    tcp->is_open = false;
}

//=============================================================================
// TcpServer - Creation
//=============================================================================

/// @brief Create a non-blocking TCP listener for a local endpoint.
/// @details Resolves IPv4 and IPv6 bind candidates and selects the first socket
///          that can bind, listen with @c SOMAXCONN, and enter non-blocking
///          mode. @c SO_REUSEADDR supports hot restart; wildcard IPv6
///          candidates request dual-stack operation when the platform exposes
///          @c IPV6_V6ONLY. A port of zero requests an OS-assigned ephemeral
///          port, which is queried before the listener enters managed
///          ownership.
/// @param address Optional NUL-terminated local address. NULL uses
///        @c AI_PASSIVE and all interfaces.
/// @param port Local port in [0, 65535].
/// @return Newly allocated managed TcpServer, or NULL after a returning
///         address, bind, permission, allocation, or configuration trap.
static void *rt_tcp_server_listen_impl(const char *address, int64_t port) {
    rt_net_init_wsa();

    if (port < 0 || port > 65535) {
        rt_trap("Network: invalid port number");
        return NULL;
    }

    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *rp = NULL;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", (int)port);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (!address)
        hints.ai_flags = AI_PASSIVE;

    if (getaddrinfo(address, port_str, &hints, &res) != 0) {
        rt_trap("Network: invalid address");
        return NULL;
    }

    socket_t sock = INVALID_SOCK;
    int last_err = 0;
    int bound_family = AF_UNSPEC;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        socket_t candidate = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (candidate == INVALID_SOCK)
            continue;

        suppress_sigpipe(candidate);

        int reuse = 1;
        setsockopt(candidate, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
#ifdef IPV6_V6ONLY
        if (rp->ai_family == AF_INET6) {
            int v6only = address ? 1 : 0;
            setsockopt(candidate, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&v6only, sizeof(v6only));
        }
#endif

        if (bind(candidate, rp->ai_addr, (socklen_t)rp->ai_addrlen) == 0 &&
            listen(candidate, SOMAXCONN) == 0 && rt_socket_set_nonblocking(candidate, true)) {
            sock = candidate;
            bound_family = rp->ai_family;
            break;
        }

        last_err = GET_LAST_ERROR();
        CLOSE_SOCKET(candidate);
    }

    freeaddrinfo(res);

    if (sock == INVALID_SOCK) {
        if (last_err == ADDR_IN_USE) {
            rt_trap_net("Network: port already in use", Err_NetworkError);
            return NULL;
        }
        if (last_err == PERM_DENIED) {
            rt_trap_net("Network: permission denied (port < 1024?)", Err_NetworkError);
            return NULL;
        }
        rt_trap_net("Network: bind failed", Err_NetworkError);
        return NULL;
    }

    const char *bound_addr = address ? address : (bound_family == AF_INET6 ? "::" : "0.0.0.0");
    char *addr_cstr = (char *)malloc(strlen(bound_addr) + 1);
    if (!addr_cstr) {
        CLOSE_SOCKET(sock);
        rt_trap("Network: memory allocation failed");
        return NULL;
    }
    memcpy(addr_cstr, bound_addr, strlen(bound_addr) + 1);

    return tcp_server_adopt_listener(sock, addr_cstr, port == 0 ? get_local_port(sock) : (int)port);
}

/// @brief Listen for TCP connections on all local interfaces.
/// @details Delegates wildcard address-family selection to the internal
///          listener factory. The stored address reflects the selected family
///          as either `0.0.0.0` or `::`.
/// @param port Local port in [0, 65535]; zero requests an ephemeral port.
/// @return Newly allocated managed TcpServer, or NULL after a returning trap.
void *rt_tcp_server_listen(int64_t port) {
    return rt_tcp_server_listen_impl(NULL, port);
}

/// @brief Listen for TCP connections on a specific local address.
/// @details Requires a nonempty managed address string with no embedded NUL,
///          then resolves and binds it through the shared listener factory.
/// @param address Managed local hostname or numeric IPv4/IPv6 address.
/// @param port Local port in [0, 65535]; zero requests an ephemeral port.
/// @return Newly allocated managed TcpServer, or NULL after a returning
///         validation, resolution, bind, or allocation trap.
void *rt_tcp_server_listen_at(rt_string address, int64_t port) {
    const char *addr_ptr = NULL;
    size_t addr_len = 0;
    if (!rt_net_cstr_no_embedded_nul(address, &addr_ptr, &addr_len) || addr_len == 0) {
        rt_trap("Network: invalid address");
        return NULL;
    }
    return rt_tcp_server_listen_impl(addr_ptr, port);
}

//=============================================================================
// TcpServer - Properties
//=============================================================================

/// @brief Return the local port recorded for a TCP server.
/// @details Safely retains the server while reading the value. For an
///          ephemeral bind this is the actual port selected by the OS.
/// @param obj Required TcpServer receiver.
/// @return Bound port, or 0 after a returning invalid-handle trap.
int64_t rt_tcp_server_port(void *obj) {
    rt_tcp_server_t *server = tcp_server_require_retained(obj);
    if (!server)
        return 0;
    int64_t port = server->port;
    tcp_release_managed(server);
    return port;
}

/// @brief Return the address recorded for a TCP server.
/// @details Retains the server across managed string creation and releases it
///          on ordinary and recovered allocation-trap paths. The address
///          metadata remains available after the listener is closed.
/// @param obj Required TcpServer receiver.
/// @return Newly owned managed address string, or the runtime empty-string
///         sentinel after a returning trap.
rt_string rt_tcp_server_address(void *obj) {
    rt_tcp_server_t *server = tcp_server_require_retained(obj);
    if (!server)
        return rt_str_empty();

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[512];
        tcp_save_trap_error(
            saved_error, sizeof(saved_error), "Network: server address allocation failed");
        rt_trap_clear_recovery();
        tcp_release_managed(server);
        rt_trap(saved_error);
        return rt_str_empty();
    }

    rt_string address = rt_const_cstr(server->address);
    rt_trap_clear_recovery();
    tcp_release_managed(server);
    return address;
}

/// @brief Test whether a TCP server is accepting connections.
/// @details Retains the managed receiver and takes an acquire-ordered snapshot
///          of its atomic listening flag.
/// @param obj Required TcpServer receiver.
/// @return 1 while listening, otherwise 0; invalid receivers trap and return 0
///         only when the trap hook resumes execution.
int8_t rt_tcp_server_is_listening(void *obj) {
    rt_tcp_server_t *server = tcp_server_require_retained(obj);
    if (!server)
        return 0;
    int listening = rt_atomic_load_i32(&server->is_listening, __ATOMIC_ACQUIRE);
    tcp_release_managed(server);
    return listening ? 1 : 0;
}

//=============================================================================
// TcpServer - Accept and Close
//=============================================================================

/// @brief Accept the next TCP client without an application timeout.
/// @details Delegates to @ref rt_tcp_server_accept_for with a zero timeout.
///          Internally the non-blocking listener is polled in bounded slices so
///          a concurrent close can still terminate the operation.
/// @param obj Required listening TcpServer receiver.
/// @return Newly allocated connected TCP object, or NULL when the server closes
///         or after a returning accept/configuration/allocation trap.
void *rt_tcp_server_accept(void *obj) {
    return rt_tcp_server_accept_for(obj, 0);
}

/// @brief Accept a TCP client with an optional timeout.
/// @details Retains and registers against the server before reading its
///          descriptor, preventing concurrent close-and-reuse races. The
///          non-blocking listener is polled in at most 100-millisecond slices;
///          a monotonic deadline is used when available, with a decrementing
///          fallback otherwise. Zero waits indefinitely, while a positive
///          timeout returns NULL without trapping when no client arrives.
///          Accepted sockets are restored to blocking mode, configured for
///          SIGPIPE suppression and TCP_NODELAY, and adopted with numeric peer
///          and local endpoint metadata.
/// @param obj Required listening TcpServer receiver.
/// @param timeout_ms Timeout in milliseconds, in [0, @c INT_MAX]; zero waits
///        until a connection arrives or the server closes.
/// @return Newly allocated connected TCP object, or NULL on timeout, concurrent
///         server close, or after a returning validation, accept,
///         configuration, or allocation trap.
void *rt_tcp_server_accept_for(void *obj, int64_t timeout_ms) {
    rt_tcp_server_t *server = tcp_server_require_retained(obj);
    if (!server)
        return NULL;
    if (!rt_atomic_load_i32(&server->is_listening, __ATOMIC_ACQUIRE)) {
        tcp_release_managed(server);
        rt_trap_net("Network: server not listening", Err_ConnectionClosed);
        return NULL;
    }
    if (timeout_ms < 0) {
        tcp_release_managed(server);
        rt_trap("Network: invalid timeout");
        return NULL;
    }

    int timeout_int = 0;
    if (!rt_net_timeout_ms_to_int(timeout_ms, &timeout_int)) {
        tcp_release_managed(server);
        rt_trap("Network: invalid timeout");
        return NULL;
    }

    int registered = tcp_server_begin_accept(server);
    if (registered <= 0) {
        tcp_release_managed(server);
        if (registered < 0)
            rt_trap("Network: too many concurrent accepts");
        return NULL;
    }

    socket_t listener = server->sock;
    socket_t client_sock = INVALID_SOCK;
    struct sockaddr_storage client_addr;
    socklen_t client_len = 0;
    memset(&client_addr, 0, sizeof(client_addr));
    int accept_error = 0;
    uint64_t start_ms = timeout_int > 0 ? rt_socket_monotonic_ms() : 0;
    uint64_t deadline_ms = 0;
    if (start_ms != 0) {
        uint64_t timeout_u64 = (uint64_t)(unsigned int)timeout_int;
        deadline_ms = start_ms > UINT64_MAX - timeout_u64 ? UINT64_MAX : start_ms + timeout_u64;
    }
    int fallback_remaining_ms = timeout_int;

    // The listener is non-blocking. Bounded readiness slices let Close clear
    // the lifecycle flag and wait for this registered operation without
    // closing (and potentially reusing) the descriptor underneath us.
    for (;;) {
        if (!rt_atomic_load_i32(&server->is_listening, __ATOMIC_ACQUIRE))
            break;

        int wait_ms = 100;
        if (timeout_int > 0) {
            if (start_ms != 0) {
                uint64_t now_ms = rt_socket_monotonic_ms();
                if (now_ms == 0) {
                    start_ms = 0;
                } else {
                    if (now_ms >= deadline_ms)
                        break;
                    uint64_t remaining = deadline_ms - now_ms;
                    wait_ms = remaining < UINT64_C(100) ? (int)remaining : 100;
                    if (wait_ms <= 0)
                        break;
                }
            }
            if (start_ms == 0) {
                if (fallback_remaining_ms <= 0)
                    break;
                wait_ms = fallback_remaining_ms < 100 ? fallback_remaining_ms : 100;
                fallback_remaining_ms -= wait_ms;
            }
        }

        int ready = wait_socket(listener, wait_ms, false);
        if (ready == 0)
            continue;
        if (ready < 0) {
            int err = GET_LAST_ERROR();
            if (!rt_atomic_load_i32(&server->is_listening, __ATOMIC_ACQUIRE) ||
                rt_socket_accept_interrupted_by_close(err))
                break;
            if (rt_socket_error_is_interrupted(err) || rt_socket_error_is_would_block(err))
                continue;
            accept_error = err;
            break;
        }

        client_len = sizeof(client_addr);
        client_sock = accept(listener, (struct sockaddr *)&client_addr, &client_len);
        if (client_sock != INVALID_SOCK)
            break;
        client_len = 0;

        int err = GET_LAST_ERROR();
        if (rt_socket_error_is_interrupted(err) || rt_socket_error_is_would_block(err))
            continue;
        if (!rt_atomic_load_i32(&server->is_listening, __ATOMIC_ACQUIRE) ||
            rt_socket_accept_interrupted_by_close(err))
            break;
        accept_error = err;
        break;
    }

    tcp_server_end_accept(server);
    tcp_release_managed(server);

    if (client_sock == INVALID_SOCK) {
        if (accept_error != 0)
            rt_trap_net("Network: accept failed", Err_NetworkError);
        return NULL;
    }

    // Some socket stacks inherit non-blocking state from the listener. Public
    // Tcp receive/send methods retain their established blocking semantics.
    if (!rt_socket_set_nonblocking(client_sock, false)) {
        CLOSE_SOCKET(client_sock);
        rt_trap_net("Network: configuring accepted socket failed", Err_NetworkError);
        return NULL;
    }

    // accept() copied peer metadata into independent stack storage before the
    // registered listener operation ended. Reuse that snapshot rather than
    // issuing a second peer-name syscall or retaining listener-owned state.

    suppress_sigpipe(client_sock);

    // Enable TCP_NODELAY
    set_nodelay(client_sock);

    // Get client info
    char host_buf[NI_MAXHOST];
    char service_buf[NI_MAXSERV];
    if (client_len == 0 || getnameinfo((struct sockaddr *)&client_addr,
                                       client_len,
                                       host_buf,
                                       sizeof(host_buf),
                                       service_buf,
                                       sizeof(service_buf),
                                       NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        snprintf(host_buf, sizeof(host_buf), "%s", "");
        snprintf(service_buf, sizeof(service_buf), "%d", 0);
    }

    char *host_cstr = (char *)malloc(strlen(host_buf) + 1);
    if (!host_cstr) {
        CLOSE_SOCKET(client_sock);
        rt_trap("Network: memory allocation failed");
        return NULL;
    }
    memcpy(host_cstr, host_buf, strlen(host_buf) + 1);

    return tcp_adopt_connected_socket(client_sock,
                                      host_cstr,
                                      parse_numeric_service_port(service_buf),
                                      get_local_port(client_sock));
}

/// @brief Stop a TCP server and close its listener.
/// @details NULL and repeated closes are no-ops. A valid close first prevents
///          new accepts, waits for registered accept operations to release
///          their descriptor snapshots, and then closes the listener. The
///          managed server object and its bound endpoint metadata remain alive.
/// @param obj TcpServer receiver, or NULL for a no-op.
void rt_tcp_server_close(void *obj) {
    if (!obj)
        return;

    rt_tcp_server_t *server = tcp_server_require_retained(obj);
    if (!server)
        return;
    tcp_server_stop(server);
    tcp_release_managed(server);
}

//=============================================================================
// Udp Socket Structure
//=============================================================================
