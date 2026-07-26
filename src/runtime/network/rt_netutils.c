//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_netutils.c
// Purpose: Static network utility functions for port checking, CIDR matching,
//          and IP address classification.
// Key invariants:
//   - All functions are stateless and thread-safe.
// Ownership/Lifetime:
//   - Pure functions; returned strings are GC-managed.
// Links: rt_netutils.h (API)
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements stateless network inspection and address utilities.
 * @details Provides deadline-bounded TCP reachability probes, ephemeral
 * loopback port selection, strict IPv4 CIDR membership and private-range
 * classification, and route-selected local IPv4 discovery.
 */

#include "rt_netutils.h"

#include "rt_internal.h"
#include "rt_socket_platform.h"
#include "rt_string.h"
#include "rt_time.h"

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//=============================================================================
// Port Checking
//=============================================================================

/// @brief TCP port-scan probe: returns 1 if `(host, port)` accepts a connection within
/// `timeout_ms`. Walks every address `getaddrinfo` returns (so IPv6 hosts get IPv6 attempts).
/// Uses non-blocking connect plus the shared socket readiness adapter for the timeout.
/// Default timeout 1000 ms if non-positive supplied. Useful for service health-checks and
/// "wait for port to come up" patterns during testing.
/// @param host Host name or numeric address to resolve.
/// @param port TCP port in the inclusive range 1 through 65535.
/// @param timeout_ms Shared probe deadline in milliseconds; non-positive values use 1000.
/// @return One when any resolved address accepts a connection before the deadline; zero otherwise.
int8_t rt_netutils_is_port_open(rt_string host, int64_t port, int64_t timeout_ms) {
    rt_net_init_wsa();

    const char *host_ptr = rt_string_cstr(host);
    int64_t host_len = host ? rt_str_len(host) : -1;
    if (!host_ptr || host_len <= 0 || port < 1 || port > 65535)
        return 0;
    // Reject embedded NUL so the probe targets the caller's full host string,
    // not a C-string prefix of it.
    if (memchr(host_ptr, '\0', (size_t)host_len) != NULL)
        return 0;
    if (timeout_ms <= 0)
        timeout_ms = 1000;
    if (timeout_ms > INT_MAX)
        return 0;

    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", (int)port);

    if (getaddrinfo(host_ptr, port_str, &hints, &res) != 0)
        return 0;

    // One monotonic deadline across all resolved candidates so the total
    // probe time honors timeout_ms instead of multiplying it per address.
    int64_t deadline_us = rt_clock_ticks_us() + timeout_ms * 1000;

    for (rp = res; rp != NULL; rp = rp->ai_next) {
        int64_t remaining_ms = (deadline_us - rt_clock_ticks_us()) / 1000;
        if (remaining_ms <= 0)
            break;

        socket_t sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock == INVALID_SOCK)
            continue;

        // Set non-blocking for the timed connect; skip the candidate rather
        // than risk a blocking connect that ignores the deadline.
        if (!rt_socket_set_nonblocking(sock, true)) {
            CLOSE_SOCKET(sock);
            continue;
        }

        int result = connect(sock, rp->ai_addr, (int)rp->ai_addrlen);
        if (result == 0) {
            CLOSE_SOCKET(sock);
            freeaddrinfo(res);
            return 1;
        }

        int ready = wait_socket(sock, (int)remaining_ms, true);
        if (ready > 0) {
            int so_error = 0;
            socklen_t len = sizeof(so_error);
            // A failed status read must not count as success.
            int got_status = getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)&so_error, &len);
            CLOSE_SOCKET(sock);
            if (got_status == 0 && so_error == 0) {
                freeaddrinfo(res);
                return 1;
            }
        } else {
            CLOSE_SOCKET(sock);
        }
    }

    freeaddrinfo(res);
    return 0;
}

/// @brief Ask the OS for a free ephemeral port: bind a temporary socket to `127.0.0.1:0` (port 0
/// means "you pick"), read back the assigned port via `getsockname`, then close the socket.
/// **Race window:** another process can grab the port between this call and the user's actual
/// bind. Acceptable for tests; not for production servers where you should bind directly to 0.
/// @return Ephemeral loopback TCP port selected by the OS, or zero on socket failure.
int64_t rt_netutils_get_free_port(void) {
    rt_net_init_wsa();

    socket_t sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCK)
        return 0;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(0x7f000001); // 127.0.0.1
    addr.sin_port = 0;                        // OS assigns a free port

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        CLOSE_SOCKET(sock);
        return 0;
    }

    socklen_t len = sizeof(addr);
    if (getsockname(sock, (struct sockaddr *)&addr, &len) != 0) {
        CLOSE_SOCKET(sock);
        return 0;
    }

    int64_t port = ntohs(addr.sin_port);
    CLOSE_SOCKET(sock);
    return port;
}

//=============================================================================
// CIDR Matching
//=============================================================================

/// @brief Parse an IPv4 address string to a uint32_t in host byte order.
/// @param str Null-terminated dotted-decimal IPv4 text.
/// @param out Receives the host-order 32-bit address.
/// @return True on successful IPv4 parsing; false otherwise.
static bool parse_ipv4_addr(const char *str, uint32_t *out) {
    struct in_addr addr;
    if (inet_pton(AF_INET, str, &addr) != 1)
        return false;
    *out = ntohl(addr.s_addr);
    return true;
}

/// @brief True when @p value has an embedded NUL (its C-string view would
///        classify a prefix different from the caller's runtime string).
/// @param value Runtime String to inspect.
/// @return Nonzero when an embedded null occurs before the logical end.
static int netutils_string_has_embedded_nul(rt_string value) {
    const char *cstr = value ? rt_string_cstr(value) : NULL;
    int64_t len = value ? rt_str_len(value) : 0;
    if (!cstr || len <= 0)
        return 0;
    return memchr(cstr, '\0', (size_t)len) != NULL;
}

/// @brief Test whether an IPv4 address belongs to a CIDR range.
/// @details Parses an optional prefix length, builds a host-order mask, and
///          compares masked address values. `/0` matches every valid IPv4
///          address; absent prefixes default to `/32`.
/// @param ip IPv4 address String.
/// @param cidr IPv4 network String with an optional `/0` through `/32` prefix.
/// @return One on a match; zero for no match, malformed input, or embedded nulls.
int8_t rt_netutils_match_cidr(rt_string ip, rt_string cidr) {
    const char *ip_str = rt_string_cstr(ip);
    const char *cidr_str = rt_string_cstr(cidr);
    if (!ip_str || !cidr_str || netutils_string_has_embedded_nul(ip) ||
        netutils_string_has_embedded_nul(cidr))
        return 0;

    // Parse CIDR notation: "10.0.0.0/8"
    char network[64];
    int prefix_len = 32;
    const char *slash = strchr(cidr_str, '/');
    if (slash) {
        size_t net_len = (size_t)(slash - cidr_str);
        if (net_len >= sizeof(network))
            return 0;
        memcpy(network, cidr_str, net_len);
        network[net_len] = '\0';
        char *end = NULL;
        long parsed_prefix = strtol(slash + 1, &end, 10);
        if (end == slash + 1 || *end != '\0' || parsed_prefix < 0 || parsed_prefix > 32)
            return 0;
        prefix_len = (int)parsed_prefix;
    } else {
        size_t len = strlen(cidr_str);
        if (len >= sizeof(network))
            return 0;
        memcpy(network, cidr_str, len + 1);
    }

    uint32_t ip_val, net_val;
    if (!parse_ipv4_addr(ip_str, &ip_val))
        return 0;
    if (!parse_ipv4_addr(network, &net_val))
        return 0;

    if (prefix_len == 0)
        return 1; // /0 matches everything

    uint32_t mask = ~((1u << (32 - prefix_len)) - 1);
    return (ip_val & mask) == (net_val & mask) ? 1 : 0;
}

/// @brief Returns 1 if `ip` is in an RFC 1918 private range (10/8, 172.16/12, 192.168/16) or
/// loopback (127/8). This is classification only, not a complete trust or special-use check.
/// @param ip IPv4 address String.
/// @return One for RFC 1918 or loopback addresses; zero for public or invalid input.
int8_t rt_netutils_is_private_ip(rt_string ip) {
    const char *ip_str = rt_string_cstr(ip);
    if (!ip_str || netutils_string_has_embedded_nul(ip))
        return 0;

    uint32_t addr;
    if (!parse_ipv4_addr(ip_str, &addr))
        return 0;

    // RFC 1918 private ranges:
    // 10.0.0.0/8       (10.0.0.0 – 10.255.255.255)
    // 172.16.0.0/12    (172.16.0.0 – 172.31.255.255)
    // 192.168.0.0/16   (192.168.0.0 – 192.168.255.255)
    // Also: 127.0.0.0/8 (loopback)

    if ((addr >> 24) == 10)
        return 1;
    if ((addr >> 20) == (172 << 4 | 1)) // 172.16-31
        return 1;
    if ((addr >> 16) == (192 << 8 | 168))
        return 1;
    if ((addr >> 24) == 127)
        return 1;

    return 0;
}

/// @brief Discover the IPv4 address of the interface the OS would route to the public internet.
/// Trick: open a UDP socket, "connect" it to 8.8.8.8:53 (no actual packets sent — UDP connect is
/// just a routing table lookup), then `getsockname` to read which local IP the kernel assigned.
/// Returns "127.0.0.1" if anything fails (e.g. a completely offline machine). This chooses the
/// route for one destination rather than enumerating interfaces or defining a stable primary IP.
/// @return Caller-owned selected IPv4 String, or loopback on failure.
rt_string rt_netutils_local_ipv4(void) {
    rt_net_init_wsa();

    // Create a UDP socket and "connect" to a public IP to determine
    // which local interface would be used (no actual traffic sent).
    socket_t sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCK)
        return rt_string_from_bytes("127.0.0.1", 9);

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(53); // DNS port (arbitrary)
    inet_pton(AF_INET, "8.8.8.8", &dest.sin_addr);

    if (connect(sock, (struct sockaddr *)&dest, sizeof(dest)) != 0) {
        CLOSE_SOCKET(sock);
        return rt_string_from_bytes("127.0.0.1", 9);
    }

    struct sockaddr_in local_addr;
    socklen_t len = sizeof(local_addr);
    if (getsockname(sock, (struct sockaddr *)&local_addr, &len) != 0) {
        CLOSE_SOCKET(sock);
        return rt_string_from_bytes("127.0.0.1", 9);
    }

    CLOSE_SOCKET(sock);

    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &local_addr.sin_addr, ip_str, sizeof(ip_str));
    return rt_string_from_bytes(ip_str, strlen(ip_str));
}
