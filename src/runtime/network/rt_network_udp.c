//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_network_udp.c
// Purpose: UDP socket support for Zanna.Network.Udp. Provides creation, binding,
//   send/receive, multicast group management, broadcast, and timeout control.
//
// Key invariants:
//   - UDP sockets have a stable managed class identity and initialization magic.
//   - Send/receive use Berkeley sockets API with platform abstraction.
//   - Oversized datagrams are consumed and rejected rather than truncated or
//     left queued to wedge every subsequent receive.
//   - Multicast uses IP_ADD_MEMBERSHIP / IP_DROP_MEMBERSHIP.
//   - Not thread-safe; external synchronization required for concurrent access.
//
// Ownership/Lifetime:
//   - rt_udp objects are GC-managed; the socket is closed by the finalizer.
//   - Received data is returned as GC-managed Bytes objects.
//
// Links: src/runtime/network/rt_network_internal.h (platform abstractions),
//        src/runtime/network/rt_network.c (TCP + platform init),
//        src/runtime/network/rt_network.h (public API)
//
//===----------------------------------------------------------------------===//

// Platform feature macros must appear before ANY includes.
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "rt_network_internal.h"

#include "rt_map.h"
#include "rt_trap.h"

#include <setjmp.h>

#if !RT_PLATFORM_WINDOWS
#include <sys/uio.h>
#endif

typedef struct rt_udp {
    uint64_t magic;                     // RT_UDP_MAGIC for fully initialized handles
    socket_t sock;                      // Socket descriptor
    char *address;                      // Bound address (allocated, or NULL if unbound)
    int port;                           // Bound port (0 if unbound)
    int family;                         // AF_INET or AF_INET6
    bool is_bound;                      // Whether socket is bound
    bool is_open;                       // Socket state
    char sender_host[INET6_ADDRSTRLEN]; // Last sender host
    int sender_port;                    // Last sender port
    int recv_timeout_ms;                // Receive timeout (0 = none)
} rt_udp_t;

#define RT_UDP_MAGIC UINT64_C(0x5A55445048414E44)

static void rt_udp_finalize(void *obj);

/// @brief Release one temporary managed value owned by a UDP operation.
/// @details Decrements the managed reference count and frees at zero. Receive
///          paths call this on ordinary socket errors and on locally recovered
///          right-sizing allocation traps.
/// @param obj Owned managed reference, or NULL for a no-op.
static void udp_release_managed(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Preserve the active trap diagnostic across recovery-frame cleanup.
/// @details Clearing a legacy recovery frame also clears its thread-local
///          message. This helper copies that message before UDP cleanup and
///          rethrow; @p fallback is used for an empty embedder diagnostic.
/// @param buffer Destination NUL-terminated diagnostic buffer.
/// @param buffer_size Size of @p buffer in bytes.
/// @param fallback Message used when no active trap text exists.
static void udp_save_trap_error(char *buffer, size_t buffer_size, const char *fallback) {
    const char *error = rt_trap_get_error();
    snprintf(buffer, buffer_size, "%s", error && error[0] ? error : fallback);
}

/// @brief Return a validated UDP payload without trapping.
/// @details Checks managed heap kind, stable UDP class id, complete payload
///          size, and initialization magic before any socket field is read.
/// @param obj Candidate opaque receiver.
/// @return Valid UDP payload, or NULL.
static rt_udp_t *udp_try(void *obj) {
    if (!rt_obj_is_instance(obj, RT_UDP_CLASS_ID, sizeof(rt_udp_t)))
        return NULL;
    rt_udp_t *udp = (rt_udp_t *)obj;
    return udp->magic == RT_UDP_MAGIC ? udp : NULL;
}

/// @brief Validate a required public UDP receiver and report misuse.
/// @details Null receivers retain the established `NULL socket` diagnostic;
///          forged, wrong-class, undersized, or uninitialized values report an
///          invalid-handle trap. Callers must stop on NULL because an embedder's
///          trap hook may return.
/// @param obj Caller-supplied receiver.
/// @return Valid UDP payload, or NULL after exactly one trap.
static rt_udp_t *udp_require(void *obj) {
    if (!obj) {
        rt_trap("Network: NULL socket");
        return NULL;
    }
    rt_udp_t *udp = udp_try(obj);
    if (!udp) {
        rt_trap("Network: invalid UDP socket");
        return NULL;
    }
    return udp;
}

/// @brief Transfer a native datagram socket and optional address into a UDP object.
/// @details Managed publication is protected by a local recovery boundary. If
///          object allocation traps after bind/socket creation, this helper
///          closes @p sock and frees @p address before propagating the original
///          diagnostic. Successful publication initializes all fields before
///          installing the idempotent finalizer.
/// @param sock Owned native UDP socket; consumed on every return path.
/// @param address Owned bound-address string, or NULL for an unbound socket;
///        consumed on every return path.
/// @param port Actual local port, or zero for an unbound socket.
/// @param family Native AF_INET or AF_INET6 family.
/// @param is_bound Nonzero when @p sock has completed bind().
/// @return Initialized managed UDP handle, or NULL after a returning trap hook.
static void *udp_adopt_socket(socket_t sock, char *address, int port, int family, int is_bound) {
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[512];
        udp_save_trap_error(saved_error, sizeof(saved_error), "Network: UDP allocation failed");
        rt_trap_clear_recovery();
        if (sock != INVALID_SOCK)
            CLOSE_SOCKET(sock);
        free(address);
        rt_trap(saved_error);
        return NULL;
    }

    rt_udp_t *udp = (rt_udp_t *)rt_obj_new_i64(RT_UDP_CLASS_ID, (int64_t)sizeof(rt_udp_t));
    rt_trap_clear_recovery();
    if (!udp) {
        if (sock != INVALID_SOCK)
            CLOSE_SOCKET(sock);
        free(address);
        return NULL;
    }

    udp->magic = RT_UDP_MAGIC;
    udp->sock = sock;
    udp->address = address;
    udp->port = port;
    udp->family = family;
    udp->is_bound = is_bound != 0;
    udp->is_open = true;
    udp->sender_host[0] = '\0';
    udp->sender_port = 0;
    udp->recv_timeout_ms = 0;
    rt_obj_set_finalizer(udp, rt_udp_finalize);
    return udp;
}

#if defined(IPV6_JOIN_GROUP) && !defined(IPV6_ADD_MEMBERSHIP)
#define IPV6_ADD_MEMBERSHIP IPV6_JOIN_GROUP
#endif
#if defined(IPV6_LEAVE_GROUP) && !defined(IPV6_DROP_MEMBERSHIP)
#define IPV6_DROP_MEMBERSHIP IPV6_LEAVE_GROUP
#endif

/// @brief Create and configure one native UDP socket.
/// @details Suppresses SIGPIPE and, for IPv6, explicitly selects dual-stack or
///          IPv6-only operation when the platform exposes @c IPV6_V6ONLY.
/// @param family Native @c AF_INET or @c AF_INET6 family.
/// @param dual_stack Nonzero to accept IPv4-mapped traffic on IPv6.
/// @return Owned socket, or @c INVALID_SOCK after closing a partially
///         configured candidate.
static socket_t udp_create_socket(int family, int dual_stack) {
    socket_t sock = socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCK)
        return INVALID_SOCK;
    suppress_sigpipe(sock);
#ifdef IPV6_V6ONLY
    if (family == AF_INET6) {
        int v6only = dual_stack ? 0 : 1;
        if (setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&v6only, sizeof(v6only)) !=
            0) {
            CLOSE_SOCKET(sock);
            return INVALID_SOCK;
        }
    }
#endif
    return sock;
}

/// @brief Receive one datagram while detecting native truncation.
/// @details Uses @c WSAEMSGSIZE on Windows and @c recvmsg flags on POSIX so an
///          oversized datagram is consumed but never silently published as
///          complete.
/// @param sock Open native UDP socket.
/// @param buf Writable receive buffer.
/// @param recv_len Buffer capacity accepted by the native API.
/// @param sender_addr Receives the datagram source address.
/// @param sender_len In/out capacity and actual source-address length.
/// @param truncated_out Optional flag, cleared first and set on truncation.
/// @return Received byte count, or @c SOCK_ERROR.
static int udp_recvfrom_checked(socket_t sock,
                                uint8_t *buf,
                                int recv_len,
                                struct sockaddr_storage *sender_addr,
                                socklen_t *sender_len,
                                int *truncated_out) {
    if (truncated_out)
        *truncated_out = 0;
#if RT_PLATFORM_WINDOWS
    int received =
        recvfrom(sock, (char *)buf, recv_len, 0, (struct sockaddr *)sender_addr, sender_len);
    if (received == SOCK_ERROR && WSAGetLastError() == WSAEMSGSIZE && truncated_out)
        *truncated_out = 1;
    return received;
#else
    struct iovec iov;
    struct msghdr msg;
    ssize_t received;
    memset(&iov, 0, sizeof(iov));
    memset(&msg, 0, sizeof(msg));
    iov.iov_base = buf;
    iov.iov_len = (size_t)recv_len;
    msg.msg_name = sender_addr;
    msg.msg_namelen = *sender_len;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    received = recvmsg(sock, &msg, 0);
    if (received >= 0) {
        *sender_len = msg.msg_namelen;
#ifdef MSG_TRUNC
        if ((msg.msg_flags & MSG_TRUNC) != 0 && truncated_out)
            *truncated_out = 1;
#endif
        if (received > recv_len) {
            if (truncated_out)
                *truncated_out = 1;
            received = recv_len;
        }
    }
    return received < 0 ? SOCK_ERROR : (int)received;
#endif
}

/// @brief Convert an IPv4 destination to IPv4-mapped IPv6 socket form.
/// @param src Source IPv4 address and port.
/// @param dst Receives the zeroed `::ffff:a.b.c.d` address.
/// @param dst_len Optional output receiving `sizeof(*dst)`.
static void udp_make_v4_mapped(const struct sockaddr_in *src,
                               struct sockaddr_in6 *dst,
                               socklen_t *dst_len) {
    memset(dst, 0, sizeof(*dst));
    dst->sin6_family = AF_INET6;
    dst->sin6_port = src->sin_port;
    dst->sin6_addr.s6_addr[10] = 0xFF;
    dst->sin6_addr.s6_addr[11] = 0xFF;
    memcpy(&dst->sin6_addr.s6_addr[12], &src->sin_addr, sizeof(src->sin_addr));
    if (dst_len)
        *dst_len = (socklen_t)sizeof(*dst);
}

/// @brief Test an IPv6 address for the IPv4-mapped prefix.
/// @param addr IPv6 address to inspect.
/// @return One for `::ffff:0:0/96`, otherwise zero.
static int udp_is_v4_mapped_addr(const struct in6_addr *addr) {
    const unsigned char *bytes = (const unsigned char *)addr->s6_addr;
    static const unsigned char prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
    return memcmp(bytes, prefix, sizeof(prefix)) == 0;
}

/// @brief Record numeric sender metadata after a successful datagram receive.
/// @details Clears prior metadata first, renders IPv4 and IPv6 numerically, and
///          presents IPv4-mapped IPv6 sources in ordinary dotted-decimal form.
/// @param udp UDP payload whose last-sender fields are updated.
/// @param addr Received source socket address.
/// @param addr_len Source address length supplied by the socket API.
static void udp_store_sender_info(rt_udp_t *udp, const struct sockaddr *addr, socklen_t addr_len) {
    (void)addr_len;
    if (!udp || !addr) {
        return;
    }

    udp->sender_host[0] = '\0';
    udp->sender_port = 0;

    if (addr->sa_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)addr;
        inet_ntop(AF_INET, &in->sin_addr, udp->sender_host, sizeof(udp->sender_host));
        udp->sender_port = ntohs(in->sin_port);
        return;
    }

    if (addr->sa_family == AF_INET6) {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)addr;
        if (udp_is_v4_mapped_addr(&in6->sin6_addr)) {
            struct in_addr mapped_v4;
            memcpy(&mapped_v4, &in6->sin6_addr.s6_addr[12], sizeof(mapped_v4));
            inet_ntop(AF_INET, &mapped_v4, udp->sender_host, sizeof(udp->sender_host));
        } else {
            inet_ntop(AF_INET6, &in6->sin6_addr, udp->sender_host, sizeof(udp->sender_host));
        }
        udp->sender_port = ntohs(in6->sin6_port);
    }
}

/// @brief Resolve a destination compatible with a UDP socket's family.
/// @details Preserves resolver order, restricts IPv4 sockets to IPv4 records,
///          and maps IPv4 results into IPv6 form for dual-stack sockets.
/// @param host Nonempty NUL-terminated destination host.
/// @param port Destination port.
/// @param socket_family Socket's @c AF_INET or @c AF_INET6 family.
/// @param addr_out Receives the selected address.
/// @param addr_len_out Receives its native length.
/// @return Zero on success, otherwise -1.
static int udp_resolve_destination(const char *host,
                                   int port,
                                   int socket_family,
                                   struct sockaddr_storage *addr_out,
                                   socklen_t *addr_len_out) {
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *rp = NULL;
    char port_str[16];

    if (!host || !*host || !addr_out || !addr_len_out)
        return -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res)
        return -1;

    for (rp = res; rp != NULL; rp = rp->ai_next) {
        if (socket_family == AF_INET && rp->ai_family != AF_INET)
            continue;
        if (socket_family == AF_INET6 && rp->ai_family == AF_INET) {
            udp_make_v4_mapped((const struct sockaddr_in *)rp->ai_addr,
                               (struct sockaddr_in6 *)addr_out,
                               addr_len_out);
            freeaddrinfo(res);
            return 0;
        }
        if (socket_family != AF_INET6 && socket_family != AF_INET)
            continue;
        if (socket_family == AF_INET6 && rp->ai_family != AF_INET6)
            continue;
        if ((socklen_t)rp->ai_addrlen > (socklen_t)sizeof(*addr_out))
            continue;
        memcpy(addr_out, rp->ai_addr, rp->ai_addrlen);
        *addr_len_out = (socklen_t)rp->ai_addrlen;
        freeaddrinfo(res);
        return 0;
    }

    freeaddrinfo(res);
    return -1;
}

/// @brief Resolve and bind a managed UDP socket.
/// @details Tries IPv4/IPv6 candidates in resolver order with @c SO_REUSEADDR,
///          records the actual OS-selected ephemeral port, copies the chosen
///          address text, and transfers the native socket to managed ownership.
/// @param address Optional local address; NULL selects all interfaces.
/// @param port Local port in [0, 65535], with zero requesting an ephemeral port.
/// @return Newly owned managed Udp object, or NULL after a returning trap.
static void *rt_udp_bind_impl(const char *address, int64_t port) {
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *rp = NULL;
    socket_t sock = INVALID_SOCK;
    int last_err = 0;
    int family = AF_INET;
    int actual_port = (int)port;
    char *addr_cstr = NULL;
    char port_str[16];

    rt_net_init_wsa();

    if (port < 0 || port > 65535) {
        rt_trap("Network: invalid port number");
        return NULL;
    }
    if (address && *address == '\0') {
        rt_trap("Network: invalid address");
        return NULL;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    if (!address)
        hints.ai_flags = AI_PASSIVE;

    snprintf(port_str, sizeof(port_str), "%d", (int)port);
    if (getaddrinfo(address, port_str, &hints, &res) != 0 || !res) {
        rt_trap("Network: invalid address");
        return NULL;
    }

    for (rp = res; rp != NULL; rp = rp->ai_next) {
        int reuse = 1;
        socket_t candidate = udp_create_socket(rp->ai_family, rp->ai_family == AF_INET6);
        if (candidate == INVALID_SOCK)
            continue;

        setsockopt(candidate, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

        if (bind(candidate, rp->ai_addr, (socklen_t)rp->ai_addrlen) == 0) {
            sock = candidate;
            family = rp->ai_family;
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

    if (port == 0) {
        struct sockaddr_storage bound_addr;
        socklen_t len = sizeof(bound_addr);
        if (getsockname(sock, (struct sockaddr *)&bound_addr, &len) == 0) {
            if (((struct sockaddr *)&bound_addr)->sa_family == AF_INET) {
                actual_port = ntohs(((struct sockaddr_in *)&bound_addr)->sin_port);
            } else if (((struct sockaddr *)&bound_addr)->sa_family == AF_INET6) {
                actual_port = ntohs(((struct sockaddr_in6 *)&bound_addr)->sin6_port);
            }
        }
    }

    {
        const char *addr_ptr = address ? address : (family == AF_INET6 ? "::" : "0.0.0.0");
        size_t addr_len = strlen(addr_ptr);
        addr_cstr = (char *)malloc(addr_len + 1);
        if (!addr_cstr) {
            CLOSE_SOCKET(sock);
            rt_trap("Network: memory allocation failed");
            return NULL;
        }
        memcpy(addr_cstr, addr_ptr, addr_len + 1);
    }

    return udp_adopt_socket(sock, addr_cstr, actual_port, family, 1);
}

/// @brief GC finalizer: close the socket if still open and free the bound-address string.
/// @details Clears bound state, endpoint metadata, and initialization magic
///          after releasing native resources. NULL is a no-op.
/// @param obj Udp payload being finalized, or NULL.
static void rt_udp_finalize(void *obj) {
    if (!obj)
        return;
    rt_udp_t *udp = (rt_udp_t *)obj;
    if (udp->is_open) {
        CLOSE_SOCKET(udp->sock);
        udp->is_open = false;
    }
    udp->is_bound = false;
    udp->port = 0;
    if (udp->address) {
        free(udp->address);
        udp->address = NULL;
    }
    udp->magic = 0;
}

//=============================================================================
// Udp - Creation
//=============================================================================

/// @brief Create an unbound UDP socket. Prefers an IPv6 dual-stack socket when available so the
/// same handle can send to IPv4 and IPv6 destinations; falls back to IPv4-only when the platform
/// cannot create a dual-stack datagram socket.
/// @return Newly owned managed Udp object, or NULL after a returning creation
///         or allocation trap.
void *rt_udp_new(void) {
    rt_net_init_wsa();

    socket_t sock = udp_create_socket(AF_INET6, 1);
    int family = AF_INET6;
    if (sock == INVALID_SOCK) {
        sock = udp_create_socket(AF_INET, 0);
        family = AF_INET;
    }
    if (sock == INVALID_SOCK) {
        rt_trap("Network: failed to create UDP socket");
        return NULL;
    }

    return udp_adopt_socket(sock, NULL, 0, family, 0);
}

/// @brief Create a UDP socket and bind it to all interfaces (`0.0.0.0`) on `port`. Pass
/// `port=0` to let the OS pick a free port (read it back via `rt_udp_port`). Convenience wrapper
/// over `rt_udp_bind_at` for the common "listen on any address" case.
/// @param port Local port in [0, 65535].
/// @return Newly owned bound Udp object, or NULL after a returning trap.
void *rt_udp_bind(int64_t port) {
    return rt_udp_bind_impl(NULL, port);
}

/// @brief Create and bind a UDP socket to `(address, port)`. Supports IPv4 literals, IPv6
/// literals, and hostnames that resolve to a local interface address. When `port==0` the OS
/// assigns a free port and the actual port is queried back via `getsockname`.
/// @param address Nonempty managed local address without embedded NUL.
/// @param port Local port in [0, 65535].
/// @return Newly owned bound Udp object, or NULL after a returning trap.
void *rt_udp_bind_at(rt_string address, int64_t port) {
    const char *addr_ptr = NULL;
    size_t addr_len = 0;
    if (!rt_net_cstr_no_embedded_nul(address, &addr_ptr, &addr_len) || addr_len == 0) {
        rt_trap("Network: invalid address");
        return NULL;
    }
    return rt_udp_bind_impl(addr_ptr, port);
}

//=============================================================================
// Udp - Properties
//=============================================================================

/// @brief Read the bound port (0 for unbound sockets created via `rt_udp_new`). Useful when the
/// constructor used `port=0` and you need to discover which ephemeral port the OS assigned.
/// @param obj Required Udp receiver.
/// @return Recorded bound port, or zero when unbound/closed or after a
///         returning invalid-handle trap.
int64_t rt_udp_port(void *obj) {
    rt_udp_t *udp = udp_require(obj);
    if (!udp)
        return 0;
    return udp->port;
}

/// @brief Read the bound address as an rt_string ("0.0.0.0" if bound to all interfaces). Returns
/// the empty string for unbound sockets.
/// @param obj Required Udp receiver.
/// @return Newly owned address String, or empty when unbound.
rt_string rt_udp_address(void *obj) {
    rt_udp_t *udp = udp_require(obj);
    if (!udp)
        return rt_str_empty();
    if (udp->address)
        return rt_const_cstr(udp->address);
    return rt_str_empty();
}

/// @brief Returns 1 if the socket was created via `bind()` (i.e. has a fixed local port); 0 if
/// it was created via `rt_udp_new` and only sends.
/// @param obj Required Udp receiver.
/// @return One while explicitly bound, otherwise zero.
int8_t rt_udp_is_bound(void *obj) {
    rt_udp_t *udp = udp_require(obj);
    if (!udp)
        return 0;
    return udp->is_bound ? 1 : 0;
}

//=============================================================================
// Udp - Send Methods
//=============================================================================

/// @brief Send a Bytes payload as a single UDP datagram to `(host, port)`. Caps payload at the
/// IPv4 UDP max of 65507 bytes (65535 IP packet − 20 IP header − 8 UDP header); large permitted
/// datagrams can still be fragmented or rejected according to the path MTU. Resolves `host`
/// through `getaddrinfo`, supporting IPv4, IPv6, and DNS.
/// Returns the byte count actually sent. Traps with specific kinds for EMSGSIZE, host-not-found,
/// and generic send errors so callers can distinguish recoverable failures.
/// @param obj Required open Udp receiver.
/// @param host Nonempty managed destination host without embedded NUL.
/// @param port Destination port in [1, 65535].
/// @param data Required managed Bytes payload.
/// @return Exact datagram byte count, or -1 after a returning trap.
int64_t rt_udp_send_to(void *obj, rt_string host, int64_t port, void *data) {
    rt_udp_t *udp = udp_require(obj);
    if (!udp)
        return -1;
    if (!data) {
        rt_trap("Network: NULL data");
        return -1;
    }
    if (!rt_bytes_is_bytes(data)) {
        rt_trap("Network: invalid Bytes data");
        return -1;
    }
    if (!udp->is_open) {
        rt_trap_net("Network: socket closed", Err_ConnectionClosed);
        return -1;
    }

    const char *host_ptr = NULL;
    size_t host_len = 0;
    if (!rt_net_cstr_no_embedded_nul(host, &host_ptr, &host_len) || host_len == 0) {
        rt_trap("Network: invalid host");
        return -1;
    }

    if (port < 1 || port > 65535) {
        rt_trap("Network: invalid port number");
        return -1;
    }

    int64_t len = bytes_len(data);
    uint8_t *buf = bytes_data(data);

    // Check packet size
    if (len > 65507) {
        rt_trap_net("Network: message too large (max 65507 bytes for UDP)", Err_NetworkError);
        return -1;
    }

    // Resolve destination
    struct sockaddr_storage dest_addr;
    socklen_t dest_len = 0;
    if (udp_resolve_destination(host_ptr, (int)port, udp->family, &dest_addr, &dest_len) != 0) {
        rt_trap_net("Network: host not found", Err_HostNotFound);
        return -1;
    }

    uint8_t empty_datagram = 0;
    const uint8_t *payload = len > 0 ? buf : &empty_datagram;
    int sent = sendto(udp->sock,
                      (const char *)payload,
                      (int)len,
                      SEND_FLAGS,
                      (struct sockaddr *)&dest_addr,
                      dest_len);
    if (sent == SOCK_ERROR) {
        int err = GET_LAST_ERROR();
        if (rt_socket_error_is_message_too_large(err)) {
            rt_trap_net("Network: message too large", Err_NetworkError);
            return -1;
        }
        rt_trap_net("Network: send failed", net_classify_error(err));
        return -1;
    }
    if ((int64_t)sent != len) {
        rt_trap_net("Network: partial datagram send", Err_NetworkError);
        return -1;
    }

    return sent;
}

/// @brief String-payload variant of `rt_udp_send_to`. Sends the rt_string's UTF-8 bytes
/// (without a NUL terminator) as a single datagram. Same 65507-byte cap and error semantics.
/// @param obj Required open Udp receiver.
/// @param host Nonempty managed destination host without embedded NUL.
/// @param port Destination port in [1, 65535].
/// @param text Required managed String payload.
/// @return Exact datagram byte count, or -1 after a returning trap.
int64_t rt_udp_send_to_str(void *obj, rt_string host, int64_t port, rt_string text) {
    rt_udp_t *udp = udp_require(obj);
    if (!udp)
        return -1;
    if (!udp->is_open) {
        rt_trap_net("Network: socket closed", Err_ConnectionClosed);
        return -1;
    }

    const char *host_ptr = NULL;
    size_t host_len = 0;
    if (!rt_net_cstr_no_embedded_nul(host, &host_ptr, &host_len) || host_len == 0) {
        rt_trap("Network: invalid host");
        return -1;
    }

    if (port < 1 || port > 65535) {
        rt_trap("Network: invalid port number");
        return -1;
    }

    if (!text) {
        rt_trap("Network: NULL string");
        return -1;
    }
    if (!rt_string_is_handle((const void *)text)) {
        rt_trap("Network: invalid string data");
        return -1;
    }
    int64_t len64 = rt_str_len(text);
    if (len64 < 0 || (uint64_t)len64 > (uint64_t)SIZE_MAX) {
        rt_trap("Network: invalid string length");
        return -1;
    }
    size_t len = (size_t)len64;
    const char *text_ptr = rt_string_cstr(text);

    if (len > 65507) {
        rt_trap_net("Network: message too large (max 65507 bytes for UDP)", Err_NetworkError);
        return -1;
    }

    // Resolve destination
    struct sockaddr_storage dest_addr;
    socklen_t dest_len = 0;
    if (udp_resolve_destination(host_ptr, (int)port, udp->family, &dest_addr, &dest_len) != 0) {
        rt_trap_net("Network: host not found", Err_HostNotFound);
        return -1;
    }

    char empty_datagram = 0;
    const char *payload = len > 0 ? text_ptr : &empty_datagram;
    int sent =
        sendto(udp->sock, payload, (int)len, SEND_FLAGS, (struct sockaddr *)&dest_addr, dest_len);
    if (sent == SOCK_ERROR) {
        int err = GET_LAST_ERROR();
        if (rt_socket_error_is_message_too_large(err)) {
            rt_trap_net("Network: message too large", Err_NetworkError);
            return -1;
        }
        rt_trap_net("Network: send failed", net_classify_error(err));
        return -1;
    }
    if ((size_t)sent != len) {
        rt_trap_net("Network: partial datagram send", Err_NetworkError);
        return -1;
    }

    return sent;
}

//=============================================================================
// Udp - Receive Methods
//=============================================================================

/// @brief Convenience alias for `rt_udp_recv_from` — receive a single datagram up to `max_bytes`
/// long. The sender's address is recorded and accessible via `rt_udp_sender_host` / `_port`.
/// @param obj Required open Udp receiver.
/// @param max_bytes Receive-buffer capacity in [0, @c INT_MAX].
/// @return Newly owned exact-sized Bytes, possibly empty, or NULL after a
///         returning trap.
void *rt_udp_recv(void *obj, int64_t max_bytes) {
    return rt_udp_recv_from(obj, max_bytes);
}

/// @brief Receive one UDP datagram, capturing the sender's address into `udp->sender_host/port`.
/// Right-sizes the result `Bytes` if the actual datagram was shorter than `max_bytes` (datagram
/// boundaries are meaningful in UDP; trailing zeros must NOT be exposed). On socket timeout
/// (EAGAIN / WSAETIMEDOUT, configured via `set_recv_timeout`), returns an empty Bytes rather
/// than trapping — lets receive loops poll cheaply.
/// @param obj Required open Udp receiver.
/// @param max_bytes Receive-buffer capacity in [0, @c INT_MAX].
/// @return Newly owned exact-sized Bytes, including empty Bytes on persistent
///         timeout, or NULL after a returning trap.
void *rt_udp_recv_from(void *obj, int64_t max_bytes) {
    rt_udp_t *udp = udp_require(obj);
    if (!udp)
        return NULL;
    if (!udp->is_open) {
        rt_trap_net("Network: socket closed", Err_ConnectionClosed);
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

    struct sockaddr_storage sender_addr;
    socklen_t sender_len = sizeof(sender_addr);

    int truncated = 0;
    int received =
        udp_recvfrom_checked(udp->sock, buf, recv_len, &sender_addr, &sender_len, &truncated);

    if (truncated) {
        udp_release_managed(result);
        rt_trap_net("Network: datagram exceeds receive buffer", Err_ProtocolError);
        return NULL;
    }

    if (received == SOCK_ERROR) {
        int receive_error = GET_LAST_ERROR();
        if (rt_socket_error_is_timeout(receive_error) ||
            rt_socket_error_is_would_block(receive_error)) {
            // Release over-allocated buffer and return empty bytes on timeout
            udp_release_managed(result);
            return rt_bytes_new(0);
        }
        udp_release_managed(result);
        rt_trap_net("Network: receive failed", net_classify_error(receive_error));
        return NULL;
    }

    // Return exact size received
    if (received < max_bytes) {
        jmp_buf recovery;
        rt_trap_set_recovery(&recovery);
        if (setjmp(recovery) != 0) {
            char saved_error[512];
            udp_save_trap_error(
                saved_error, sizeof(saved_error), "Network: UDP result allocation failed");
            rt_trap_clear_recovery();
            udp_release_managed(result);
            rt_trap(saved_error);
            return NULL;
        }

        void *exact = rt_bytes_new(received);
        rt_trap_clear_recovery();
        if (!exact) {
            udp_release_managed(result);
            return NULL;
        }
        if (received > 0)
            memcpy(bytes_data(exact), buf, received);
        udp_release_managed(result);
        udp_store_sender_info(udp, (const struct sockaddr *)&sender_addr, sender_len);
        return exact;
    }

    udp_store_sender_info(udp, (const struct sockaddr *)&sender_addr, sender_len);
    return result;
}

/// @brief Bounded-wait variant: `select()` for up to `timeout_ms`, then receive (or return NULL
/// on no-data). Distinct from setting socket-level recv timeout: this is one-shot and returns
/// NULL on expiry, while `set_recv_timeout` sets a persistent socket option that returns empty
/// Bytes.
/// @param obj Required open Udp receiver.
/// @param max_bytes Receive-buffer capacity in [0, @c INT_MAX].
/// @param timeout_ms One-shot readiness timeout in [0, @c INT_MAX]
///        milliseconds; zero skips readiness waiting.
/// @return Newly owned exact-sized Bytes, or NULL on readiness timeout or after
///         a returning trap.
void *rt_udp_recv_for(void *obj, int64_t max_bytes, int64_t timeout_ms) {
    rt_udp_t *udp = udp_require(obj);
    if (!udp)
        return NULL;
    if (!udp->is_open) {
        rt_trap_net("Network: socket closed", Err_ConnectionClosed);
        return NULL;
    }

    if (timeout_ms < 0) {
        rt_trap("Network: invalid timeout");
        return NULL;
    }
    if (max_bytes < 0) {
        rt_trap("Network: invalid receive size");
        return NULL;
    }
    if (max_bytes == 0)
        return rt_bytes_new(0);
    int ignored_recv_len = 0;
    if (!rt_net_i64_len_to_int(max_bytes, &ignored_recv_len)) {
        rt_trap("Network: receive size too large");
        return NULL;
    }

    // Use select for timeout
    if (timeout_ms > 0) {
        int timeout_int = 0;
        if (!rt_net_timeout_ms_to_int(timeout_ms, &timeout_int)) {
            rt_trap("Network: invalid timeout");
            return NULL;
        }
        int ready = wait_socket(udp->sock, timeout_int, false);
        if (ready == 0) {
            // Timeout - return NULL
            return NULL;
        }
        if (ready < 0) {
            rt_trap_net("Network: receive failed", net_classify_errno());
            return NULL;
        }
    }

    return rt_udp_recv_from(obj, max_bytes);
}

/// @brief Read the numeric IPv4 or IPv6 source of the most recently received datagram. Empty until
/// the first successful `recv*`.
/// @param obj Required Udp receiver.
/// @return Newly owned numeric sender-address String, or empty before receipt.
rt_string rt_udp_sender_host(void *obj) {
    rt_udp_t *udp = udp_require(obj);
    if (!udp)
        return rt_str_empty();
    return rt_const_cstr(udp->sender_host);
}

/// @brief Read the source port of the most recently received datagram. 0 until the first recv*.
/// @param obj Required Udp receiver.
/// @return Last sender port, or zero before a successful receive.
int64_t rt_udp_sender_port(void *obj) {
    rt_udp_t *udp = udp_require(obj);
    if (!udp)
        return 0;
    return udp->sender_port;
}

//=============================================================================
// Udp - Options and Close
//=============================================================================

/// @brief Toggle SO_BROADCAST on the socket. Required before sending to 255.255.255.255 or any
/// directed-broadcast address; without it the kernel returns EACCES.
/// @param obj Required open Udp receiver.
/// @param enable Nonzero to enable broadcast, zero to disable it.
void rt_udp_set_broadcast(void *obj, int8_t enable) {
    rt_udp_t *udp = udp_require(obj);
    if (!udp)
        return;
    if (!udp->is_open) {
        rt_trap_net("Network: socket closed", Err_ConnectionClosed);
        return;
    }

    int flag = enable ? 1 : 0;
    if (setsockopt(udp->sock, SOL_SOCKET, SO_BROADCAST, (const char *)&flag, sizeof(flag)) ==
        SOCK_ERROR) {
        rt_trap_net("Network: failed to set broadcast option", Err_NetworkError);
        return;
    }
}

/// @brief Subscribe to an IPv4 or IPv6 multicast group. IPv4 uses `IP_ADD_MEMBERSHIP`;
/// IPv6 uses `IPV6_ADD_MEMBERSHIP`.
/// @param obj Required open Udp receiver.
/// @param group_addr Numeric IPv4 address in 224.0.0.0/4 or IPv6 address in
///        ff00::/8.
void rt_udp_join_group(void *obj, rt_string group_addr) {
    rt_udp_t *udp = udp_require(obj);
    if (!udp)
        return;
    if (!udp->is_open) {
        rt_trap_net("Network: socket closed", Err_ConnectionClosed);
        return;
    }

    const char *addr_ptr = NULL;
    size_t addr_len = 0;
    if (!rt_net_cstr_no_embedded_nul(group_addr, &addr_ptr, &addr_len) || addr_len == 0) {
        rt_trap("Network: invalid multicast address");
        return;
    }

    struct in_addr mcast_addr;
    if (inet_pton(AF_INET, addr_ptr, &mcast_addr) == 1) {
        uint32_t addr_val = ntohl(mcast_addr.s_addr);
        if ((addr_val & 0xF0000000) != 0xE0000000) {
            rt_trap("Network: invalid multicast address (must be 224.0.0.0 - 239.255.255.255)");
            return;
        }

        {
            struct ip_mreq mreq;
            mreq.imr_multiaddr = mcast_addr;
            mreq.imr_interface.s_addr = htonl(INADDR_ANY);

            if (setsockopt(
                    udp->sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char *)&mreq, sizeof(mreq)) ==
                SOCK_ERROR) {
                rt_trap_net("Network: failed to join multicast group", Err_NetworkError);
                return;
            }
        }
        return;
    }

    {
        struct in6_addr mcast_addr6;
        if (inet_pton(AF_INET6, addr_ptr, &mcast_addr6) != 1) {
            rt_trap("Network: invalid multicast address");
            return;
        }
        if (mcast_addr6.s6_addr[0] != 0xFF) {
            rt_trap("Network: invalid multicast address (IPv6 multicast must be ff00::/8)");
            return;
        }
#ifdef IPV6_ADD_MEMBERSHIP
        {
            struct ipv6_mreq mreq6;
            memset(&mreq6, 0, sizeof(mreq6));
            mreq6.ipv6mr_multiaddr = mcast_addr6;
            mreq6.ipv6mr_interface = 0;
            if (setsockopt(udp->sock,
                           IPPROTO_IPV6,
                           IPV6_ADD_MEMBERSHIP,
                           (const char *)&mreq6,
                           sizeof(mreq6)) == SOCK_ERROR) {
                rt_trap_net("Network: failed to join multicast group", Err_NetworkError);
                return;
            }
        }
        return;
#else
        rt_trap_net("Network: IPv6 multicast is not supported on this platform", Err_NetworkError);
#endif
    }
}

/// @brief Unsubscribe from an IPv4 or IPv6 multicast group. Tolerant of bad input — silently
/// no-ops on closed sockets, empty addresses, or malformed IPs.
/// @param obj Required valid Udp receiver.
/// @param group_addr Numeric multicast address to leave.
void rt_udp_leave_group(void *obj, rt_string group_addr) {
    rt_udp_t *udp = udp_require(obj);
    if (!udp)
        return;
    if (!udp->is_open)
        return; // Silently ignore if closed

    const char *addr_ptr = NULL;
    size_t addr_len = 0;
    if (!rt_net_cstr_no_embedded_nul(group_addr, &addr_ptr, &addr_len) || addr_len == 0)
        return;

    struct in_addr mcast_addr;
    if (inet_pton(AF_INET, addr_ptr, &mcast_addr) == 1) {
        struct ip_mreq mreq;
        mreq.imr_multiaddr = mcast_addr;
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        setsockopt(udp->sock, IPPROTO_IP, IP_DROP_MEMBERSHIP, (const char *)&mreq, sizeof(mreq));
        return;
    }

#ifdef IPV6_DROP_MEMBERSHIP
    {
        struct in6_addr mcast_addr6;
        if (inet_pton(AF_INET6, addr_ptr, &mcast_addr6) != 1)
            return;
        {
            struct ipv6_mreq mreq6;
            memset(&mreq6, 0, sizeof(mreq6));
            mreq6.ipv6mr_multiaddr = mcast_addr6;
            mreq6.ipv6mr_interface = 0;
            setsockopt(
                udp->sock, IPPROTO_IPV6, IPV6_DROP_MEMBERSHIP, (const char *)&mreq6, sizeof(mreq6));
        }
    }
#endif
}

/// @brief Set a persistent socket-level recv timeout via SO_RCVTIMEO. Subsequent `recv*` calls
/// that exceed this duration return empty Bytes (rather than the per-call NULL of `recv_for`).
/// Pass `0` to clear (block indefinitely).
/// @param obj Required open Udp receiver.
/// @param timeout_ms Timeout in [0, @c INT_MAX] milliseconds.
void rt_udp_set_recv_timeout(void *obj, int64_t timeout_ms) {
    rt_udp_t *udp = udp_require(obj);
    if (!udp)
        return;
    if (!udp->is_open) {
        rt_trap_net("Network: socket closed", Err_ConnectionClosed);
        return;
    }
    int timeout_int = 0;
    if (!rt_net_timeout_ms_to_int(timeout_ms, &timeout_int)) {
        rt_trap("Network: invalid timeout");
        return;
    }
    if (!set_socket_timeout(udp->sock, timeout_int, true)) {
        rt_trap_net("Network: setting receive timeout failed", net_classify_errno());
        return;
    }
    udp->recv_timeout_ms = timeout_int;
}

/// @brief Explicit close — releases the kernel socket immediately rather than waiting for GC.
/// Idempotent (no-op on already-closed sockets). Bound state and port are cleared while the last
/// bound address remains available for diagnostics.
/// @param obj Udp receiver, or NULL for a no-op.
void rt_udp_close(void *obj) {
    if (!obj)
        return;

    rt_udp_t *udp = udp_require(obj);
    if (!udp)
        return;
    if (udp->is_open) {
        CLOSE_SOCKET(udp->sock);
        udp->is_open = false;
        udp->is_bound = false;
        udp->port = 0;
    }
}
