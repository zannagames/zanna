//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTNetworkHardenTests.cpp
// Purpose: Adversarial network scenario tests verifying that every failure
//          produces a clean, categorized error code — never a crash, hang,
//          or platform-specific exception leaking through.
// Key invariants:
//   - Network failures always trap with a specific Err_* code.
//   - SIGPIPE never kills the process.
//   - Programming errors (NULL args) still hard-trap.
// Ownership/Lifetime: Creates ephemeral localhost sockets cleaned up per test.
// Links: src/runtime/network/rt_network.c, src/runtime/core/rt_error.h
//
//===----------------------------------------------------------------------===//

#include "tests/common/NetworkTestCompat.hpp"
#include "tests/common/PosixCompat.h"

#include "rt_async_socket.h"
#include "rt_box.h"
#include "rt_bytes.h"
#include "rt_error.h"
#include "rt_future.h"
#include "rt_http_client.h"
#include "rt_internal.h"
#include "rt_list.h"
#include "rt_map.h"
#include "rt_network.h"
#include "rt_string.h"

#include <cassert>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
// WinSock uses SOCKET (unsigned) with INVALID_SOCKET; POSIX uses int with -1.
typedef SOCKET sock_t;
#define SOCK_INVALID INVALID_SOCKET
#define SOCK_CLOSE(s) closesocket(s)
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define SOCK_INVALID (-1)
#define SOCK_CLOSE(s) close(s)
#endif

// ── Trap interception ──────────────────────────────────────────────────────
namespace {
jmp_buf g_trap_jmp;
const char *g_last_trap = nullptr;
bool g_trap_expected = false;
bool g_trap_returns = false;
int g_trap_count = 0;
} // namespace

extern "C" void vm_trap(const char *msg) {
    g_last_trap = msg;
    g_trap_count++;
    if (g_trap_expected)
        longjmp(g_trap_jmp, 1);
    if (g_trap_returns)
        return;
    // Unexpected trap — print and abort.
    fprintf(stderr, "UNEXPECTED TRAP: %s\n", msg ? msg : "(null)");
    _exit(1);
}

extern "C" int rt_trap_get_net_code(void);

/// Expect a trap to fire; capture it and continue.
#define EXPECT_TRAP(expr)                                                                          \
    do {                                                                                           \
        g_trap_expected = true;                                                                    \
        g_last_trap = nullptr;                                                                     \
        g_trap_count = 0;                                                                          \
        if (setjmp(g_trap_jmp) == 0) {                                                             \
            expr;                                                                                  \
            assert(false && "Expected trap did not occur");                                        \
        }                                                                                          \
        g_trap_expected = false;                                                                   \
    } while (0)

// ── Platform init/cleanup ─────────────────────────────────────────────────

static void net_init() {
#if defined(_WIN32)
    WSADATA wsa;
    int rc = WSAStartup(MAKEWORD(2, 2), &wsa);
    assert(rc == 0);
#endif
}

static void net_cleanup() {
#if defined(_WIN32)
    WSACleanup();
#endif
}

// ── Helpers ────────────────────────────────────────────────────────────────

/// Create a localhost TCP listener on a random port; returns socket and port.
static sock_t make_listener(int *out_port) {
    sock_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == SOCK_INVALID)
        return SOCK_INVALID;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(zanna::tests::kIpv4LoopbackHostOrder);
    addr.sin_port = 0;

    int rc = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc != 0) {
        SOCK_CLOSE(fd);
        return SOCK_INVALID;
    }

    rc = listen(fd, 1);
    if (rc != 0) {
        SOCK_CLOSE(fd);
        return SOCK_INVALID;
    }

#if defined(_WIN32)
    int addrlen = sizeof(addr);
#else
    socklen_t addrlen = sizeof(addr);
#endif
    getsockname(fd, (struct sockaddr *)&addr, &addrlen);
    *out_port = ntohs(addr.sin_port);

    return fd;
}

/// Platform-portable microsecond sleep.
static void sleep_us(int us) {
#if defined(_WIN32)
    Sleep((unsigned)(us / 1000));
#else
    usleep(us);
#endif
}

// ── Scenario 1: Connect to nonexistent host ────────────────────────────────
static void test_connect_nonexistent_host() {
    rt_string host = rt_string_from_bytes("this.host.does.not.exist.invalid", 32);
    EXPECT_TRAP(rt_tcp_connect_for(host, 80, 2000));

    assert(g_last_trap != nullptr);
    assert(strstr(g_last_trap, "not found") != nullptr);
    int code = rt_trap_get_net_code();
    assert(code == Err_HostNotFound);

    printf("  PASS: ConnectNonexistentHost → Err_HostNotFound (%d)\n", code);
}

// ── Scenario 2: Connect to a port that refuses ─────────────────────────────
static void test_connect_refused_port() {
    // Port 1 is almost certainly not listening on localhost.
    rt_string host = rt_string_from_bytes("127.0.0.1", 9);
    EXPECT_TRAP(rt_tcp_connect_for(host, 1, 2000));

    assert(g_last_trap != nullptr);
    // Could be "connection refused", "connection failed", or "timed out" depending on OS.
    // On Windows, the WinSock error code may be reported without these keywords.
    const bool matchRefused =
        strstr(g_last_trap, "refused") != nullptr || strstr(g_last_trap, "failed") != nullptr ||
        strstr(g_last_trap, "timed out") != nullptr || strstr(g_last_trap, "timeout") != nullptr ||
        strstr(g_last_trap, "error") != nullptr;
    if (!matchRefused) {
        fprintf(stderr, "  DEBUG: actual trap = [%s]\n", g_last_trap);
    }
    assert(matchRefused);
    int code = rt_trap_get_net_code();
    // On Windows, connecting to port 1 on localhost may return Err_Timeout
    // instead of Err_ConnectionRefused (WinSock behavior varies).
    assert(code == Err_ConnectionRefused || code == Err_NetworkError || code == Err_Timeout);

    printf("  PASS: ConnectRefusedPort → code %d\n", code);
}

// ── Scenario 3: Send after remote close (SIGPIPE test) ─────────────────────
static void test_send_after_remote_close() {
    int port = 0;
    sock_t listener = make_listener(&port);
    if (listener == SOCK_INVALID) {
        printf("  SKIP: SendAfterRemoteClose → local bind unavailable in this environment\n");
        return;
    }

    rt_string host = rt_string_from_bytes("127.0.0.1", 9);
    void *conn = rt_tcp_connect(host, port);
    assert(conn != nullptr);

    // Accept then immediately close server side.
    sock_t client_fd = accept(listener, NULL, NULL);
    assert(client_fd != SOCK_INVALID);
    SOCK_CLOSE(client_fd);
    SOCK_CLOSE(listener);

    // Small delay to let the FIN propagate.
    sleep_us(50000);

    // TCP allows the first send() after peer FIN to succeed (data goes into
    // kernel send buffer; the RST comes back asynchronously).  Send in a loop
    // until the runtime traps with a network error — the key invariant is that
    // we must NOT crash via SIGPIPE.
    void *data = rt_bytes_new(1024);
    bool trapped = false;
    for (int attempt = 0; attempt < 20; ++attempt) {
        g_trap_expected = true;
        g_last_trap = nullptr;
        g_trap_count = 0;
        if (setjmp(g_trap_jmp) == 0) {
            rt_tcp_send(conn, data);
            g_trap_expected = false;
            // Send succeeded (kernel buffered) — wait for RST and retry.
            sleep_us(50000);
            continue;
        }
        g_trap_expected = false;
        trapped = true;
        break;
    }
    assert(trapped && "Expected trap did not occur after repeated sends");

    assert(g_last_trap != nullptr);
    // Should be some kind of send failure or connection closed.
    int code = rt_trap_get_net_code();
    assert(code == Err_ConnectionReset || code == Err_ConnectionClosed || code == Err_NetworkError);

    printf("  PASS: SendAfterRemoteClose → no SIGPIPE crash, code %d\n", code);

    // Connection is now broken; just release.
    rt_tcp_close(conn);
}

// ── Scenario 4: Recv on a closed connection ────────────────────────────────
static void test_recv_on_closed_connection() {
    int port = 0;
    sock_t listener = make_listener(&port);
    if (listener == SOCK_INVALID) {
        printf("  SKIP: RecvOnClosedConnection → local bind unavailable in this environment\n");
        return;
    }

    rt_string host = rt_string_from_bytes("127.0.0.1", 9);
    void *conn = rt_tcp_connect(host, port);
    assert(conn != nullptr);

    sock_t client_fd = accept(listener, NULL, NULL);
    assert(client_fd != SOCK_INVALID);
    SOCK_CLOSE(client_fd);
    SOCK_CLOSE(listener);

    // Close our own connection, then try to recv.
    rt_tcp_close(conn);

    EXPECT_TRAP(rt_tcp_recv(conn, 1024));

    assert(g_last_trap != nullptr);
    assert(strstr(g_last_trap, "closed") != nullptr);
    int code = rt_trap_get_net_code();
    assert(code == Err_ConnectionClosed);

    printf("  PASS: RecvOnClosedConnection → Err_ConnectionClosed (%d)\n", code);
}

// ── Scenario 5: DNS lookup for nonexistent domain ──────────────────────────
static void test_dns_nonexistent_domain() {
    rt_string domain = rt_string_from_bytes("nonexistent.invalid", 19);
    EXPECT_TRAP(rt_dns_resolve(domain));

    assert(g_last_trap != nullptr);
    assert(strstr(g_last_trap, "not found") != nullptr);
    int code = rt_trap_get_net_code();
    assert(code == Err_DnsError);

    printf("  PASS: DnsNonexistentDomain → Err_DnsError (%d)\n", code);
}

// ── Scenario 6: Embedded NUL network inputs ────────────────────────────────
// Runtime strings with embedded NUL bytes must not be truncated by OS APIs.
static void test_embedded_nul_network_inputs_rejected() {
    const char hidden_host[] = "127.0.0.1\0.example.invalid";
    const char hidden_group[] = "224.0.0.1\0.example.invalid";
    rt_string host = rt_string_from_bytes(hidden_host, sizeof(hidden_host) - 1);
    rt_string group = rt_string_from_bytes(hidden_group, sizeof(hidden_group) - 1);

    EXPECT_TRAP(rt_tcp_connect_for(host, 80, 1));
    assert(g_last_trap != nullptr && strstr(g_last_trap, "invalid host") != nullptr);
    EXPECT_TRAP(rt_tcp_server_listen_at(host, 0));
    assert(g_last_trap != nullptr && strstr(g_last_trap, "invalid address") != nullptr);

    EXPECT_TRAP(rt_dns_resolve(host));
    assert(g_last_trap != nullptr && strstr(g_last_trap, "invalid hostname") != nullptr);
    EXPECT_TRAP(rt_dns_resolve_all(host));
    assert(g_last_trap != nullptr && strstr(g_last_trap, "invalid hostname") != nullptr);
    EXPECT_TRAP(rt_dns_resolve4(host));
    assert(g_last_trap != nullptr && strstr(g_last_trap, "invalid hostname") != nullptr);
    EXPECT_TRAP(rt_dns_resolve6(host));
    assert(g_last_trap != nullptr && strstr(g_last_trap, "invalid hostname") != nullptr);
    EXPECT_TRAP(rt_dns_reverse(host));
    assert(g_last_trap != nullptr && strstr(g_last_trap, "invalid address") != nullptr);

    assert(rt_dns_is_ipv4(host) == 0);
    assert(rt_dns_is_ipv6(host) == 0);
    assert(rt_dns_is_ip(host) == 0);

    EXPECT_TRAP(rt_udp_bind_at(host, 0));
    assert(g_last_trap != nullptr && strstr(g_last_trap, "invalid address") != nullptr);

    void *udp = rt_udp_new();
    void *payload = rt_bytes_new(1);
    EXPECT_TRAP(rt_udp_send_to(udp, host, 9, payload));
    assert(g_last_trap != nullptr && strstr(g_last_trap, "invalid host") != nullptr);
    EXPECT_TRAP(rt_udp_send_to_str(udp, host, 9, rt_const_cstr("x")));
    assert(g_last_trap != nullptr && strstr(g_last_trap, "invalid host") != nullptr);
    EXPECT_TRAP(rt_udp_join_group(udp, group));
    assert(g_last_trap != nullptr && strstr(g_last_trap, "invalid multicast") != nullptr);

    rt_udp_close(udp);
    if (rt_obj_release_check0(payload))
        rt_obj_free(payload);
    rt_string_unref(host);
    rt_string_unref(group);

    printf("  PASS: EmbeddedNulNetworkInputs -> rejected before OS calls\n");
}

// ── Scenario 7: HTTP request with malformed URL ────────────────────────────
// Note: This test is skipped if rt_http_get is not available (link-time check).
// The HTTP functions wrap rt_tcp_connect which we've already tested, so we
// verify the URL validation path specifically.
static void test_http_malformed_url() {
    // rt_url_parse traps on empty URLs (the parser is lenient for
    // scheme-less strings, treating them as relative path references).
    rt_string bad_url = rt_string_from_bytes("", 0);
    EXPECT_TRAP(rt_url_parse(bad_url));

    assert(g_last_trap != nullptr);
    // Should mention "invalid URL" or "Invalid URL".
    assert(strstr(g_last_trap, "nvalid URL") != nullptr ||
           strstr(g_last_trap, "parse URL") != nullptr);
    int code = rt_trap_get_net_code();
    assert(code == Err_InvalidUrl);

    printf("  PASS: HttpMalformedUrl → Err_InvalidUrl (%d)\n", code);
}

// ── Scenario 8: Connection stall mid-transfer (recv timeout) ───────────────
static void test_connection_stall_mid_transfer() {
    int port = 0;
    sock_t listener = make_listener(&port);
    if (listener == SOCK_INVALID) {
        printf("  SKIP: ConnectionStallMidTransfer → local bind unavailable in this environment\n");
        return;
    }

    rt_string host = rt_string_from_bytes("127.0.0.1", 9);
    void *conn = rt_tcp_connect(host, port);
    assert(conn != nullptr);

    sock_t client_fd = accept(listener, NULL, NULL);
    assert(client_fd != SOCK_INVALID);

    // Send a few bytes then stall (never send more).
    const char *partial = "partial";
    send(client_fd, partial, 7, 0);

    // Set a very short recv timeout (200ms).
    rt_tcp_set_recv_timeout(conn, 200);

    // First recv should succeed (gets partial data).
    g_trap_count = 0;
    void *result = rt_tcp_recv(conn, 1024);
    assert(result != nullptr);
    assert(rt_bytes_len(result) == 7);
    assert(g_trap_count == 0);

    // Second recv should timeout (server is stalling).
    result = rt_tcp_recv(conn, 1024);
    assert(result != nullptr);
    assert(rt_bytes_len(result) == 0); // Timeout → empty bytes.
    assert(g_trap_count == 0);

    printf("  PASS: ConnectionStallMidTransfer → timeout returns empty bytes\n");

    rt_tcp_close(conn);
    SOCK_CLOSE(client_fd);
    SOCK_CLOSE(listener);
}

// ── Scenario 9: Network unreachable (RFC 5737 TEST-NET) ────────────────────
static void test_network_unreachable() {
    // 192.0.2.1 is RFC 5737 TEST-NET-1 — should be unreachable on any real network.
    rt_string host = rt_string_from_bytes("192.0.2.1", 9);
    EXPECT_TRAP(rt_tcp_connect_for(host, 80, 1000));

    assert(g_last_trap != nullptr);
    int code = rt_trap_get_net_code();
    // Could be Err_Timeout (most common) or Err_NetworkError.
    assert(code == Err_Timeout || code == Err_NetworkError);

    printf("  PASS: NetworkUnreachable → code %d\n", code);
}

// ── Scenario 10: Resolve IPv4 for nonexistent domain ───────────────────────
static void test_dns_resolve4_nonexistent() {
    rt_string domain = rt_string_from_bytes("nohost.invalid", 14);
    EXPECT_TRAP(rt_dns_resolve4(domain));

    assert(g_last_trap != nullptr);
    int code = rt_trap_get_net_code();
    assert(code == Err_DnsError);

    printf("  PASS: DnsResolve4Nonexistent → Err_DnsError (%d)\n", code);
}

// ── Scenario 11: Reverse DNS for non-routable address ──────────────────────
static void test_dns_reverse_invalid() {
    rt_string addr = rt_string_from_bytes("192.0.2.1", 9);
    EXPECT_TRAP(rt_dns_reverse(addr));

    assert(g_last_trap != nullptr);
    int code = rt_trap_get_net_code();
    assert(code == Err_DnsError);

    printf("  PASS: DnsReverseInvalid → Err_DnsError (%d)\n", code);
}

// ── Scenario 12: HTTP invalid Content-Length is rejected ───────────────────
static void test_http_invalid_content_length() {
    int port = 0;
    sock_t listener = make_listener(&port);
    if (listener == SOCK_INVALID) {
        printf("  SKIP: HttpInvalidContentLength → local bind unavailable in this environment\n");
        return;
    }

    std::thread server([listener]() {
        sock_t client_fd = accept(listener, NULL, NULL);
        assert(client_fd != SOCK_INVALID);

        char byte = 0;
        char tail[4] = {0, 0, 0, 0};
        while (recv(client_fd, &byte, 1, 0) == 1) {
            tail[0] = tail[1];
            tail[1] = tail[2];
            tail[2] = tail[3];
            tail[3] = byte;
            if (tail[0] == '\r' && tail[1] == '\n' && tail[2] == '\r' && tail[3] == '\n')
                break;
        }

        const char *response = "HTTP/1.1 200 OK\r\n"
                               "Content-Length: nope\r\n"
                               "\r\n";
        send(client_fd, response, (int)strlen(response), 0);
        SOCK_CLOSE(client_fd);
        SOCK_CLOSE(listener);
    });

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/bad-length", port);
    EXPECT_TRAP(rt_http_get(rt_const_cstr(url)));

    assert(g_last_trap != nullptr);
    assert(strstr(g_last_trap, "Content-Length") != nullptr);
    int code = rt_trap_get_net_code();
    assert(code == Err_ProtocolError);

    printf("  PASS: HttpInvalidContentLength → Err_ProtocolError (%d)\n", code);
    server.join();
}

// ── Scenario 13: A truncated HTTP status code is rejected safely ───────────
static void test_http_truncated_status_code() {
    int port = 0;
    sock_t listener = make_listener(&port);
    if (listener == SOCK_INVALID) {
        printf("  SKIP: HttpTruncatedStatusCode → local bind unavailable\n");
        return;
    }

    std::thread server([listener]() {
        sock_t client_fd = accept(listener, NULL, NULL);
        assert(client_fd != SOCK_INVALID);

        char byte = 0;
        char tail[4] = {0, 0, 0, 0};
        while (recv(client_fd, &byte, 1, 0) == 1) {
            tail[0] = tail[1];
            tail[1] = tail[2];
            tail[2] = tail[3];
            tail[3] = byte;
            if (tail[0] == '\r' && tail[1] == '\n' && tail[2] == '\r' && tail[3] == '\n')
                break;
        }

        static const char response[] = "HTTP/1.1 \r\n";
        send(client_fd, response, (int)(sizeof(response) - 1), 0);
        SOCK_CLOSE(client_fd);
        SOCK_CLOSE(listener);
    });

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/short-status", port);
    EXPECT_TRAP(rt_http_get(rt_const_cstr(url)));

    assert(g_last_trap != nullptr);
    assert(strstr(g_last_trap, "response") != nullptr || strstr(g_last_trap, "HTTP") != nullptr);
    assert(rt_trap_get_net_code() == Err_ProtocolError);

    printf("  PASS: HttpTruncatedStatusCode → rejected without an out-of-bounds read\n");
    server.join();
}

// ── Scenario 14: HTTP unsupported Transfer-Encoding is rejected ────────────
static void test_http_unsupported_transfer_encoding() {
    int port = 0;
    sock_t listener = make_listener(&port);
    if (listener == SOCK_INVALID) {
        printf("  SKIP: HttpUnsupportedTransferEncoding → local bind unavailable\n");
        return;
    }

    std::thread server([listener]() {
        sock_t client_fd = accept(listener, NULL, NULL);
        assert(client_fd != SOCK_INVALID);

        char byte = 0;
        char tail[4] = {0, 0, 0, 0};
        while (recv(client_fd, &byte, 1, 0) == 1) {
            tail[0] = tail[1];
            tail[1] = tail[2];
            tail[2] = tail[3];
            tail[3] = byte;
            if (tail[0] == '\r' && tail[1] == '\n' && tail[2] == '\r' && tail[3] == '\n')
                break;
        }

        const char *response = "HTTP/1.1 200 OK\r\n"
                               "Transfer-Encoding: gzip\r\n"
                               "Content-Length: 5\r\n"
                               "\r\n"
                               "hello";
        send(client_fd, response, (int)strlen(response), 0);
        SOCK_CLOSE(client_fd);
        SOCK_CLOSE(listener);
    });

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/bad-transfer", port);
    EXPECT_TRAP(rt_http_get(rt_const_cstr(url)));

    assert(g_last_trap != nullptr);
    assert(strstr(g_last_trap, "Transfer-Encoding") != nullptr);
    int code = rt_trap_get_net_code();
    assert(code == Err_ProtocolError);

    printf("  PASS: HttpUnsupportedTransferEncoding → Err_ProtocolError (%d)\n", code);
    server.join();
}

// ── Scenario 15: UDP oversized datagrams are not silently truncated ────────
static void test_udp_oversized_datagram_traps() {
    void *receiver = rt_udp_bind_at(rt_const_cstr("127.0.0.1"), 0);
    void *sender = rt_udp_bind_at(rt_const_cstr("127.0.0.1"), 0);
    assert(receiver != nullptr);
    assert(sender != nullptr);

    int64_t recv_port = rt_udp_port(receiver);
    void *payload = rt_bytes_new(16);
    for (int64_t i = 0; i < 16; ++i)
        rt_bytes_set(payload, i, (uint8_t)i);

    int64_t sent = rt_udp_send_to(sender, rt_const_cstr("127.0.0.1"), recv_port, payload);
    assert(sent == 16);

    EXPECT_TRAP(rt_udp_recv(receiver, 4));
    assert(g_last_trap != nullptr);
    assert(strstr(g_last_trap, "datagram") != nullptr);
    int code = rt_trap_get_net_code();
    assert(code == Err_ProtocolError);

    printf("  PASS: UdpOversizedDatagram → Err_ProtocolError (%d)\n", code);

    // The rejected datagram must be consumed. Leaving it queued would make
    // every later bounded receive trap on the same packet and permanently
    // wedge an otherwise healthy socket.
    void *followup = rt_bytes_new(1);
    rt_bytes_set(followup, 0, 0x5A);
    assert(rt_udp_send_to(sender, rt_const_cstr("127.0.0.1"), recv_port, followup) == 1);
    void *received = rt_udp_recv_for(receiver, 4, 1000);
    assert(received != nullptr);
    assert(rt_bytes_len(received) == 1);
    assert(rt_bytes_get(received, 0) == 0x5A);
    printf("  PASS: UdpOversizedDatagram → rejected packet consumed, socket progressed\n");

    if (rt_obj_release_check0(received))
        rt_obj_free(received);
    if (rt_obj_release_check0(followup))
        rt_obj_free(followup);
    if (rt_obj_release_check0(payload))
        rt_obj_free(payload);
    rt_udp_close(sender);
    rt_udp_close(receiver);
    if (rt_obj_release_check0(sender))
        rt_obj_free(sender);
    if (rt_obj_release_check0(receiver))
        rt_obj_free(receiver);
}

// ── Scenario 16: Async connect failure resolves as Future error ───────────
static void test_async_connect_failure_surfaces_as_future_error() {
    void *future = rt_async_connect_for(rt_const_cstr("127.0.0.1"), 1, 2000);
    assert(future != nullptr);

    assert(rt_future_wait_for(future, 5000) == 1);
    assert(rt_future_is_error(future) == 1);

    rt_string error = rt_future_get_error(future);
    const char *msg = rt_string_cstr(error);
    assert(msg != nullptr && *msg != '\0');

    printf("  PASS: AsyncConnectFailure → Future error [%s]\n", msg);
    rt_string_unref(error);
    if (rt_obj_release_check0(future))
        rt_obj_free(future);
}

// ── Scenario 17: HttpClient stops after a returning trap hook ─────────────
static void test_http_client_returning_trap_safety() {
    void *client = rt_http_client_new();
    assert(client != nullptr);

    g_last_trap = nullptr;
    g_trap_count = 0;
    g_trap_returns = true;
    void *response =
        rt_http_client_get(client, reinterpret_cast<rt_string>(static_cast<uintptr_t>(0x12345u)));
    g_trap_returns = false;

    assert(response == nullptr);
    assert(g_trap_count > 0);
    assert(g_last_trap != nullptr);
    if (rt_obj_release_check0(client))
        rt_obj_free(client);

    printf("  PASS: HttpClientReturningTrap -> invalid URL stops after cleanup\n");
}

// ── Main ───────────────────────────────────────────────────────────────────
// ── URL grammar alignment (VDOC-135) ───────────────────────────────────────
static void test_url_grammar_alignment() {
    auto S = [](const char *c) { return rt_string_from_bytes(c, strlen(c)); };

    // Parse now applies the same character rules as IsValid: an unencoded
    // space is rejected instead of silently preserved.
    rt_string spaced = S("http://exa mple.com");
    assert(rt_url_is_valid(spaced) == 0);
    EXPECT_TRAP(rt_url_parse(spaced));
    rt_string_unref(spaced);

    // Authority-less scheme forms parse a scheme (RFC `scheme:`).
    rt_string mailto = S("mailto:user@example.com");
    void *m = rt_url_parse(mailto);
    rt_string scheme = rt_url_scheme(m);
    assert(strcmp(rt_string_cstr(scheme), "mailto") == 0);
    rt_string_unref(scheme);
    rt_string_unref(mailto);

    // host:port-looking spellings keep their pre-existing parse (no scheme).
    rt_string hostport = S("localhost:8080");
    void *hp = rt_url_parse(hostport);
    rt_string hp_scheme = rt_url_scheme(hp);
    assert(rt_string_cstr(hp_scheme)[0] == '\0');
    rt_string_unref(hp_scheme);
    rt_string_unref(hostport);

    // Strict absolute network validation: scheme AND host required.
    rt_string abs_ok = S("https://example.com/path");
    assert(rt_url_is_valid_absolute(abs_ok) == 1);
    rt_string_unref(abs_ok);
    rt_string rel = S("abc");
    assert(rt_url_is_valid(rel) == 1);          // permissive reference check
    assert(rt_url_is_valid_absolute(rel) == 0); // strict network check
    rt_string_unref(rel);
    rt_string no_host = S("mailto:user@example.com");
    assert(rt_url_is_valid_absolute(no_host) == 0);
    rt_string_unref(no_host);

    printf("  PASS: UrlGrammarAlignment -> Parse/IsValid/IsValidAbsolute agree\n");
}

// ── URL.EncodeQuery value stringification policy (VDOC-136) ────────────────
static void test_url_encode_query_value_policy() {
    auto S = [](const char *c) { return rt_string_from_bytes(c, strlen(c)); };

    // Boxed scalars format with the canonical scalar formatting.
    void *map = rt_map_new();
    rt_map_set(map, S("s"), rt_box_str(S("a b")));
    rt_map_set(map, S("i"), rt_box_i64(42));
    rt_map_set(map, S("b"), rt_box_i1(1));
    rt_string encoded = rt_url_encode_query(map);
    const char *enc = rt_string_cstr(encoded);
    assert(strstr(enc, "s=a%20b") != nullptr);
    assert(strstr(enc, "i=42") != nullptr);
    assert(strstr(enc, "b=true") != nullptr);
    rt_string_unref(encoded);

    // Raw string handles (Map[String,String] storage) still pass verbatim.
    void *raw_map = rt_map_new();
    rt_map_set(raw_map, S("k"), S("v"));
    rt_string raw_encoded = rt_url_encode_query(raw_map);
    assert(strcmp(rt_string_cstr(raw_encoded), "k=v") == 0);
    rt_string_unref(raw_encoded);

    // Arbitrary objects trap instead of being reinterpreted as string handles.
    void *bad_map = rt_map_new();
    rt_map_set(bad_map, S("obj"), rt_list_new());
    EXPECT_TRAP(rt_url_encode_query(bad_map));

    printf("  PASS: UrlEncodeQueryValuePolicy -> scalars format, objects trap\n");
}

int main() {
    test_url_grammar_alignment();
    test_url_encode_query_value_policy();
    net_init();

    test_connect_nonexistent_host();
    test_connect_refused_port();
    test_send_after_remote_close();
    test_recv_on_closed_connection();
    test_dns_nonexistent_domain();
    test_embedded_nul_network_inputs_rejected();
    test_http_malformed_url();
    test_connection_stall_mid_transfer();
    test_network_unreachable();
    test_dns_resolve4_nonexistent();
    test_dns_reverse_invalid();
    test_http_invalid_content_length();
    test_http_truncated_status_code();
    test_http_unsupported_transfer_encoding();
    test_udp_oversized_datagram_traps();
    test_async_connect_failure_surfaces_as_future_error();
    test_http_client_returning_trap_safety();

    net_cleanup();

    printf("All network-harden tests passed.\n");
    return 0;
}
