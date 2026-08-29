//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_network_dns.c
// Purpose: DNS resolution and IP address utilities for the Zanna runtime.
//   Provides forward/reverse DNS lookup, IP address validation, and local
//   network address discovery.
//
// Key invariants:
//   - DNS resolution uses getaddrinfo/getnameinfo (cross-platform).
//   - IPv4/IPv6 validation is strict, length-delimited, and allocation-free.
//   - Local address discovery uses getifaddrs (Unix) or GetAdaptersAddresses (Win).
//   - Numeric formatting is checked before any output buffer is inspected.
//   - Resolver and interface snapshots are released before allocation traps
//     propagate across the public boundary.
//
// Ownership/Lifetime:
//   - Returned strings and sequences are caller-owned managed references.
//   - getaddrinfo results are freed via freeaddrinfo within each function.
//   - Local-address sequences own their unique String elements.
//
// Links: src/runtime/network/rt_network_internal.h (platform abstractions),
//        src/runtime/network/rt_network.c (TCP + platform init),
//        src/runtime/network/rt_network.h (public API)
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements DNS resolution and numeric IP address utilities.
 * @details Provides forward, family-specific, exhaustive, and reverse lookup,
 * strict length-delimited IPv4/IPv6 validation, unique interface-address
 * discovery, and trap-safe release of native resolver and adapter snapshots.
 */

// Platform feature macros must appear before ANY includes. They are harmless
// on toolchains that do not consume them, so no raw OS selector is needed here.
#ifndef _DARWIN_C_SOURCE
/** Enable Darwin/BSD resolver and interface extensions. */
#define _DARWIN_C_SOURCE 1
#endif
#ifndef _GNU_SOURCE
/** Enable GNU resolver and interface extensions. */
#define _GNU_SOURCE 1
#endif

#include "rt_network_internal.h"

#include "rt_map.h"
#include "rt_platform.h"

//=============================================================================
// DNS Resolution - Static Utility Functions
//=============================================================================

/// @brief Release one caller-owned managed DNS staging reference.
/// @details Handles both Strings and ordinary objects through the common
///          deferred-release/free protocol. NULL is accepted so recovered-trap
///          cleanup can be unconditional.
/// @param obj Owned managed reference, or NULL.
static void dns_release_owned(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Copy the current trap diagnostic into caller-owned stack storage.
/// @details The trap text is thread-local and may be overwritten by cleanup, so
///          allocation-recovery paths snapshot it before clearing their frame.
/// @param buffer Destination buffer.
/// @param capacity Destination capacity in bytes.
/// @param fallback Text used when the trap supplied no diagnostic.
static void dns_save_trap_error(char *buffer, size_t capacity, const char *fallback) {
    if (!buffer || capacity == 0)
        return;
    const char *error = rt_trap_get_error();
    snprintf(buffer, capacity, "%s", error && error[0] ? error : fallback);
}

/// @brief Format one IPv4/IPv6 sockaddr as a checked numeric host string.
/// @details Uses `getnameinfo(NI_NUMERICHOST)` so scoped IPv6 addresses retain
///          their `%zone` suffix. The function validates family-specific minimum
///          payload sizes and initializes @p output before the native call,
///          preventing callers from inspecting uninitialized bytes on failure.
/// @param address Native socket address.
/// @param address_len Available bytes at @p address.
/// @param output Destination character buffer.
/// @param output_capacity Destination capacity in bytes.
/// @return One for a non-empty numeric IPv4/IPv6 result, otherwise zero.
static int dns_format_numeric_address(const struct sockaddr *address,
                                      size_t address_len,
                                      char *output,
                                      size_t output_capacity) {
    if (output && output_capacity > 0)
        output[0] = '\0';
    if (!address || !output || output_capacity == 0)
        return 0;
    if ((address->sa_family == AF_INET && address_len < sizeof(struct sockaddr_in)) ||
        (address->sa_family == AF_INET6 && address_len < sizeof(struct sockaddr_in6)) ||
        (address->sa_family != AF_INET && address->sa_family != AF_INET6)) {
        return 0;
    }
    if (address_len > (size_t)INT_MAX || output_capacity > (size_t)INT_MAX)
        return 0;
    int status = getnameinfo(address,
                             (socklen_t)address_len,
                             output,
                             (socklen_t)output_capacity,
                             NULL,
                             0,
                             NI_NUMERICHOST);
    return status == 0 && output[0] != '\0';
}

/// @brief Check whether an owning address sequence already contains exact bytes.
/// @details Local interface APIs and DNS resolvers may publish duplicate records
///          for aliases or repeated resolver entries. A small linear scan avoids
///          duplicate managed String allocation; interface/address lists are
///          normally short enough that a hash table would cost more.
/// @param seq Live Seq whose elements are managed Strings.
/// @param address Numeric address bytes.
/// @param address_len Number of bytes in @p address.
/// @return Nonzero when an equal String is already present.
static int dns_seq_contains_address(void *seq, const char *address, size_t address_len) {
    if (!seq || !address)
        return 0;
    int64_t count = rt_seq_len(seq);
    for (int64_t i = 0; i < count; ++i) {
        rt_string existing = (rt_string)rt_seq_get(seq, i);
        if (!existing || !rt_string_is_handle(existing))
            continue;
        int64_t existing_len = rt_str_len(existing);
        if (existing_len >= 0 && (uint64_t)existing_len == (uint64_t)address_len &&
            memcmp(rt_string_cstr(existing), address, address_len) == 0) {
            return 1;
        }
    }
    return 0;
}

/// @brief Check a length-delimited string for strict dotted-decimal IPv4 syntax.
/// @details Requires four one-to-three-digit decimal octets, rejects values over
///          255, leading zeroes on multi-digit octets, and any total length
///          outside 7..15 bytes. No DNS lookup or allocation occurs.
/// @param addr Candidate address bytes.
/// @param len Exact byte length.
/// @return True only for canonical dotted-decimal IPv4 text.
static bool parse_ipv4(const char *addr, size_t len) {
    if (!addr || len < 7 || len > 15)
        return false;

    int parts = 0;
    int value = 0;
    int digits = 0;

    for (size_t i = 0; i < len; ++i) {
        char ch = addr[i];
        if (ch >= '0' && ch <= '9') {
            if (digits == 1 && value == 0)
                return false;
            if (++digits > 3)
                return false;
            value = value * 10 + (ch - '0');
            if (value > 255)
                return false;
        } else if (ch == '.') {
            if (digits == 0 || parts >= 3)
                return false;
            parts++;
            value = 0;
            digits = 0;
        } else {
            return false;
        }
    }

    return digits > 0 && parts == 3;
}

/// @brief Check length-delimited IPv6 text, including an optional scope zone.
/// @details Validates the address portion with `inet_pton(AF_INET6)`. A `%zone`
///          suffix is accepted when non-empty and composed of portable interface
///          identifier characters (`A-Z`, `a-z`, digits, `_`, `.`, or `-`).
///          The zone is intentionally not resolved here, keeping validation
///          allocation-free and free of network/interface lookups.
/// @param addr Candidate address bytes.
/// @param len Exact byte length.
/// @return True for a syntactically valid IPv6 address and optional zone.
static bool parse_ipv6(const char *addr, size_t len) {
    if (!addr || len < 2 || len >= NI_MAXHOST)
        return false;

    const char *zone = (const char *)memchr(addr, '%', len);
    size_t address_len = zone ? (size_t)(zone - addr) : len;
    if (address_len == 0 || address_len >= INET6_ADDRSTRLEN)
        return false;
    if (zone) {
        size_t zone_len = len - address_len - 1;
        if (zone_len == 0)
            return false;
        for (size_t i = 0; i < zone_len; ++i) {
            unsigned char ch = (unsigned char)zone[1 + i];
            bool valid = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                         (ch >= '0' && ch <= '9') || ch == '_' || ch == '.' || ch == '-';
            if (!valid)
                return false;
        }
    }

    char numeric[INET6_ADDRSTRLEN];
    memcpy(numeric, addr, address_len);
    numeric[address_len] = '\0';
    struct in6_addr result;
    return inet_pton(AF_INET6, numeric, &result) == 1;
}

/// @brief Resolve the first formattable numeric address in one native family.
/// @details Validates the managed hostname before calling `getaddrinfo`, scans
///          past unsupported or malformed resolver records, and releases the
///          complete native result list before allocating the returned String.
///          That ordering prevents a managed allocation trap from leaking the
///          resolver snapshot. Resolver order is otherwise preserved.
/// @param hostname Managed hostname with no embedded NUL bytes.
/// @param family `AF_UNSPEC`, `AF_INET`, or `AF_INET6`.
/// @param failure_message Diagnostic used when no usable result is available.
/// @return Caller-owned numeric address String; an empty String only when an
///         embedder trap hook returns.
static rt_string dns_resolve_family(rt_string hostname, int family, const char *failure_message) {
    const char *host_ptr = NULL;
    size_t host_len = 0;
    if (!rt_net_cstr_no_embedded_nul(hostname, &host_ptr, &host_len) || host_len == 0) {
        rt_trap("Network: invalid hostname");
        return rt_str_empty();
    }

    struct addrinfo hints = {0};
    hints.ai_family = family;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *result = NULL;
    int ret = getaddrinfo(host_ptr, NULL, &hints, &result);
    if (ret != 0 || !result) {
        if (result)
            freeaddrinfo(result);
        rt_trap_net(failure_message, Err_DnsError);
        return rt_str_empty();
    }

    char ip_str[NI_MAXHOST];
    bool found = false;
    for (struct addrinfo *rp = result; rp; rp = rp->ai_next) {
        if (dns_format_numeric_address(
                rp->ai_addr, (size_t)rp->ai_addrlen, ip_str, sizeof(ip_str))) {
            found = true;
            break;
        }
    }
    freeaddrinfo(result);

    if (!found) {
        rt_trap_net(failure_message, Err_DnsError);
        return rt_str_empty();
    }
    return rt_string_from_bytes(ip_str, strlen(ip_str));
}

/// @brief Resolve a hostname to the first usable IPv4 or IPv6 numeric address.
/// @details Preserves the native resolver's preference order while skipping
///          records whose family, size, or numeric formatting is invalid. Use
///          @ref rt_dns_resolve4 or @ref rt_dns_resolve6 when the socket family
///          must be deterministic.
/// @param hostname Non-empty managed hostname without embedded NUL bytes.
/// @return Caller-owned numeric address String.
rt_string rt_dns_resolve(rt_string hostname) {
    rt_net_init_wsa();
    return dns_resolve_family(hostname, AF_UNSPEC, "Network: hostname not found");
}

/// @brief Resolve every unique IPv4 and IPv6 address for a hostname.
/// @details Results remain in resolver order and the returned Seq owns each
///          String. Exact duplicate numeric records are omitted. A recovery
///          frame covers every managed allocation while the `addrinfo` list is
///          live, so an allocation trap releases the partially built sequence
///          and native resolver storage before propagating. Raw Seq insertion
///          transfers each String's initial reference without a retain/release
///          pair.
/// @param hostname Non-empty managed hostname without embedded NUL bytes.
/// @return Caller-owned, element-owning Seq, or NULL only after a returning
///         embedder trap hook observes an error.
void *rt_dns_resolve_all(rt_string hostname) {
    rt_net_init_wsa();

    const char *host_ptr = NULL;
    size_t host_len = 0;
    if (!rt_net_cstr_no_embedded_nul(hostname, &host_ptr, &host_len) || host_len == 0) {
        rt_trap("Network: invalid hostname");
        return NULL;
    }

    struct addrinfo hints = {0};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *result = NULL;
    int ret = getaddrinfo(host_ptr, NULL, &hints, &result);
    if (ret != 0 || !result) {
        if (result)
            freeaddrinfo(result);
        rt_trap_net("Network: hostname not found", Err_DnsError);
        return NULL;
    }

    void *volatile seq = NULL;
    rt_string volatile addr_str = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[512];
        dns_save_trap_error(
            saved_error, sizeof(saved_error), "Network: failed to allocate DNS results");
        rt_trap_clear_recovery();
        freeaddrinfo(result);
        dns_release_owned((void *)addr_str);
        dns_release_owned((void *)seq);
        rt_trap(saved_error);
        return NULL;
    }

    seq = rt_seq_with_capacity_owned(8);
    if (!seq) {
        rt_trap("Network: failed to allocate DNS results");
    }
    for (struct addrinfo *rp = result; rp != NULL; rp = rp->ai_next) {
        char ip_str[NI_MAXHOST];
        if (!dns_format_numeric_address(
                rp->ai_addr, (size_t)rp->ai_addrlen, ip_str, sizeof(ip_str))) {
            continue;
        }
        size_t ip_len = strlen(ip_str);
        if (dns_seq_contains_address((void *)seq, ip_str, ip_len))
            continue;
        addr_str = rt_string_from_bytes(ip_str, ip_len);
        if (!addr_str)
            rt_trap("Network: failed to allocate DNS address");
        rt_seq_push_raw((void *)seq, (void *)addr_str);
        addr_str = NULL;
    }

    rt_trap_clear_recovery();
    freeaddrinfo(result);
    if (rt_seq_len((void *)seq) == 0) {
        dns_release_owned((void *)seq);
        rt_trap_net("Network: hostname has no usable address", Err_DnsError);
        return NULL;
    }
    return (void *)seq;
}

/// @brief Resolve the first usable IPv4 record for a hostname.
/// @details Constrains `getaddrinfo` to `AF_INET`, validates native record
///          sizes, and releases the result list before allocating the managed
///          return value.
/// @param hostname Non-empty managed hostname without embedded NUL bytes.
/// @return Caller-owned dotted-decimal IPv4 String.
rt_string rt_dns_resolve4(rt_string hostname) {
    rt_net_init_wsa();
    return dns_resolve_family(hostname, AF_INET, "Network: no IPv4 address found");
}

/// @brief Resolve the first usable IPv6 record for a hostname.
/// @details Constrains `getaddrinfo` to `AF_INET6`, retains a native scope zone
///          when one is present, and frees native resolver state before managed
///          String allocation.
/// @param hostname Non-empty managed hostname without embedded NUL bytes.
/// @return Caller-owned numeric IPv6 String.
rt_string rt_dns_resolve6(rt_string hostname) {
    rt_net_init_wsa();
    return dns_resolve_family(hostname, AF_INET6, "Network: no IPv6 address found");
}

/// @brief Resolve a numeric IPv4 or IPv6 address back to a hostname.
/// @details Applies the same strict, length-delimited syntax accepted by the
///          public predicates, including optional IPv6 `%zone` identifiers.
///          `getaddrinfo(AI_NUMERICHOST)` constructs a fully initialized native
///          sockaddr, after which `getnameinfo(NI_NAMEREQD)` requires a PTR
///          result. Native storage is released before managed allocation.
/// @param ip_address Canonical IPv4 or syntactically valid IPv6 String.
/// @return Caller-owned hostname String; an empty String only after a returning
///         trap hook observes an error.
rt_string rt_dns_reverse(rt_string ip_address) {
    rt_net_init_wsa();

    const char *addr_ptr = NULL;
    size_t addr_len = 0;
    if (!rt_net_cstr_no_embedded_nul(ip_address, &addr_ptr, &addr_len) || addr_len == 0) {
        rt_trap("Network: invalid address");
        return rt_str_empty();
    }

    int family = parse_ipv4(addr_ptr, addr_len)   ? AF_INET
                 : parse_ipv6(addr_ptr, addr_len) ? AF_INET6
                                                  : AF_UNSPEC;
    if (family == AF_UNSPEC) {
        rt_trap_net("Network: invalid IP address", Err_InvalidUrl);
        return rt_str_empty();
    }

    struct addrinfo hints = {0};
    hints.ai_family = family;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST;
    struct addrinfo *result = NULL;
    int status = getaddrinfo(addr_ptr, NULL, &hints, &result);
    if (status != 0 || !result) {
        if (result)
            freeaddrinfo(result);
        rt_trap_net("Network: invalid IP address", Err_InvalidUrl);
        return rt_str_empty();
    }

    char host[NI_MAXHOST] = {0};
    bool found = false;
    for (struct addrinfo *rp = result; rp; rp = rp->ai_next) {
        if (!rp->ai_addr || rp->ai_addrlen <= 0 || rp->ai_addrlen > INT_MAX)
            continue;
        status = getnameinfo(rp->ai_addr,
                             (socklen_t)rp->ai_addrlen,
                             host,
                             (socklen_t)sizeof(host),
                             NULL,
                             0,
                             NI_NAMEREQD);
        if (status == 0 && host[0] != '\0' && memchr(host, '\0', sizeof(host))) {
            found = true;
            break;
        }
    }
    freeaddrinfo(result);
    if (!found) {
        rt_trap_net("Network: reverse lookup failed", Err_DnsError);
        return rt_str_empty();
    }

    return rt_string_from_bytes(host, strlen(host));
}

/// @brief Check for canonical dotted-decimal IPv4 syntax without allocation.
/// @details Requires four decimal octets in the range 0..255 and rejects
///          multi-digit leading zeroes, embedded NULs, and trailing bytes.
/// @param address Managed candidate String.
/// @return One for valid canonical IPv4 text, otherwise zero.
int8_t rt_dns_is_ipv4(rt_string address) {
    const char *addr_ptr = NULL;
    size_t addr_len = 0;
    if (!rt_net_cstr_no_embedded_nul(address, &addr_ptr, &addr_len) || addr_len == 0)
        return 0;

    return parse_ipv4(addr_ptr, addr_len) ? 1 : 0;
}

/// @brief Check IPv6 syntax, including an optional non-empty scope zone.
/// @details The address portion is validated with `inet_pton`; the optional
///          `%zone` suffix is checked without resolving an interface name.
/// @param address Managed candidate String.
/// @return One for valid IPv6 text, otherwise zero.
int8_t rt_dns_is_ipv6(rt_string address) {
    const char *addr_ptr = NULL;
    size_t addr_len = 0;
    if (!rt_net_cstr_no_embedded_nul(address, &addr_ptr, &addr_len) || addr_len == 0)
        return 0;

    return parse_ipv6(addr_ptr, addr_len) ? 1 : 0;
}

/// @brief Check whether a String is canonical IPv4 or valid IPv6 text.
/// @param address Managed candidate String.
/// @return One when either family-specific predicate succeeds, otherwise zero.
int8_t rt_dns_is_ip(rt_string address) {
    return rt_dns_is_ipv4(address) || rt_dns_is_ipv6(address);
}

/// @brief Read the local host name into a bounded managed String.
/// @details Initializes the native buffer and forces its last byte to NUL. A
///          native failure traps and returns immediately, so a returning trap
///          hook cannot publish an empty or stale buffer as a successful host.
/// @return Caller-owned local hostname String.
rt_string rt_dns_local_host(void) {
    rt_net_init_wsa();

    char hostname[256];
    memset(hostname, 0, sizeof(hostname));
    if (gethostname(hostname, sizeof(hostname)) != 0) {
        rt_trap_net("Network: failed to get hostname", Err_DnsError);
        return rt_str_empty();
    }
    hostname[sizeof(hostname) - 1] = '\0';

    return rt_string_from_bytes(hostname, strlen(hostname));
}

#if RT_PLATFORM_WINDOWS
#define DNS_ADAPTER_INITIAL_BYTES 15000UL
#define DNS_ADAPTER_MAX_BYTES (16UL * 1024UL * 1024UL)
#define DNS_ADAPTER_MAX_ATTEMPTS 3

/// @brief Capture a bounded Windows adapter-address snapshot.
/// @details Retries `GetAdaptersAddresses` at most three times when the adapter
///          set grows between sizing and enumeration. Each requested size is
///          checked against a 16 MiB ceiling before allocation, preventing an
///          unstable or corrupted native size from causing unbounded memory
///          growth. The caller releases a successful snapshot with `free`.
/// @return Owned adapter list on success, otherwise NULL.
static PIP_ADAPTER_ADDRESSES dns_windows_adapter_snapshot(void) {
    ULONG buffer_len = DNS_ADAPTER_INITIAL_BYTES;
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    for (int attempt = 0; attempt < DNS_ADAPTER_MAX_ATTEMPTS; ++attempt) {
        if (buffer_len == 0 || buffer_len > DNS_ADAPTER_MAX_BYTES)
            return NULL;
        PIP_ADAPTER_ADDRESSES snapshot = (PIP_ADAPTER_ADDRESSES)malloc((size_t)buffer_len);
        if (!snapshot)
            return NULL;
        ULONG status = GetAdaptersAddresses(AF_UNSPEC, flags, NULL, snapshot, &buffer_len);
        if (status == NO_ERROR)
            return snapshot;
        free(snapshot);
        if (status != ERROR_BUFFER_OVERFLOW)
            return NULL;
    }
    return NULL;
}
#endif

/// @brief Enumerate unique numeric addresses on active local interfaces.
/// @details Windows uses a bounded `GetAdaptersAddresses` snapshot and skips
///          adapters not marked operational; POSIX-style systems walk one
///          `getifaddrs` snapshot. Numeric formatting is checked and preserves
///          IPv6 `%zone` identifiers. The returned Seq owns each unique String.
///          If managed allocation traps during enumeration, the native snapshot,
///          current String, and partial Seq are all released before propagation.
/// @return Caller-owned element-owning Seq. Native enumeration failure yields
///         an empty Seq; managed allocation failure traps and returns NULL only
///         when an embedder trap hook returns.
void *rt_dns_local_addrs(void) {
    rt_net_init_wsa();

    void *volatile seq = rt_seq_with_capacity_owned(8);
    if (!seq)
        return NULL;

#if RT_PLATFORM_WINDOWS
    PIP_ADAPTER_ADDRESSES addrs = dns_windows_adapter_snapshot();
    if (!addrs)
        return seq;
#else
    struct ifaddrs *addrs = NULL;
    if (getifaddrs(&addrs) == -1)
        return (void *)seq;
#endif

    rt_string volatile addr_str = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[512];
        dns_save_trap_error(
            saved_error, sizeof(saved_error), "Network: failed to allocate local addresses");
        rt_trap_clear_recovery();
#if RT_PLATFORM_WINDOWS
        free(addrs);
#else
        freeifaddrs(addrs);
#endif
        dns_release_owned((void *)addr_str);
        dns_release_owned((void *)seq);
        rt_trap(saved_error);
        return NULL;
    }

#if RT_PLATFORM_WINDOWS
    for (PIP_ADAPTER_ADDRESSES adapter = addrs; adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp)
            continue;

        for (PIP_ADAPTER_UNICAST_ADDRESS ua = adapter->FirstUnicastAddress; ua; ua = ua->Next) {
            if (!ua->Address.lpSockaddr || ua->Address.iSockaddrLength <= 0)
                continue;
            char ip_str[NI_MAXHOST];
            if (!dns_format_numeric_address(ua->Address.lpSockaddr,
                                            (size_t)ua->Address.iSockaddrLength,
                                            ip_str,
                                            sizeof(ip_str)))
                continue;
            size_t ip_len = strlen(ip_str);
            if (dns_seq_contains_address((void *)seq, ip_str, ip_len))
                continue;
            addr_str = rt_string_from_bytes(ip_str, ip_len);
            if (!addr_str)
                rt_trap("Network: failed to allocate local address");
            rt_seq_push_raw((void *)seq, (void *)addr_str);
            addr_str = NULL;
        }
    }
#else
    for (struct ifaddrs *ifa = addrs; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL || (ifa->ifa_flags & IFF_UP) == 0)
            continue;

        int family = ifa->ifa_addr->sa_family;
        size_t address_len = family == AF_INET    ? sizeof(struct sockaddr_in)
                             : family == AF_INET6 ? sizeof(struct sockaddr_in6)
                                                  : 0;
        if (address_len == 0)
            continue;
        char ip_str[NI_MAXHOST];
        if (!dns_format_numeric_address(ifa->ifa_addr, address_len, ip_str, sizeof(ip_str)))
            continue;
        size_t ip_len = strlen(ip_str);
        if (dns_seq_contains_address((void *)seq, ip_str, ip_len))
            continue;
        addr_str = rt_string_from_bytes(ip_str, ip_len);
        if (!addr_str)
            rt_trap("Network: failed to allocate local address");
        rt_seq_push_raw((void *)seq, (void *)addr_str);
        addr_str = NULL;
    }
#endif

    rt_trap_clear_recovery();
#if RT_PLATFORM_WINDOWS
    free(addrs);
#else
    freeifaddrs(addrs);
#endif
    return (void *)seq;
}
