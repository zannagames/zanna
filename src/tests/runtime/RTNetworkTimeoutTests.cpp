//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTNetworkTimeoutTests.cpp
// Purpose: Verify bounded TCP receive and end-to-end HTTP request deadlines.
// Key invariants:
//   - TCP timeout produces a clean empty result rather than a trap or hang.
//   - One HTTP deadline cannot be reset by incremental response progress.
// Ownership/Lifetime:
//   - Each case owns and closes its loopback listener and accepted socket.
//   - Managed test values are explicitly released after assertions.
// Links: src/runtime/network/rt_network.c,
//        src/runtime/network/rt_network_http.c
//
//===----------------------------------------------------------------------===//

#include "rt_bytes.h"
#include "rt_internal.h"
#include "rt_network.h"
#include "rt_network_internal.h"
#include "rt_object.h"
#include "rt_result.h"
#include "rt_string.h"
#include "tests/common/NetworkTestCompat.hpp"

#include <cassert>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

// -- vm_trap override ---------------------------------------------------------
namespace {
int g_trap_count = 0;
std::string g_last_trap;
} // namespace

extern "C" void vm_trap(const char *msg) {
    g_trap_count++;
    g_last_trap = msg ? msg : "";
}

// -- Test: TCP recv times out cleanly -----------------------------------------
// Strategy: Create a localhost TCP server that accepts connections but never
// sends data. Connect via rt_tcp_connect, set a 100ms recv timeout, call
// rt_tcp_recv -> should return empty bytes (length 0) without trap.
static void test_tcp_recv_timeout() {
#if defined(_WIN32)
    // Initialize WinSock2
    WSADATA wsa;
    int wsarc = WSAStartup(MAKEWORD(2, 2), &wsa);
    assert(wsarc == 0);
#endif

    // Create a TCP listener on a random port
#if defined(_WIN32)
    SOCKET listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == INVALID_SOCKET) {
        printf("  SKIP: TCP recv timeout → local listener unavailable in this environment\n");
        WSACleanup();
        return;
    }
#else
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        printf("  SKIP: TCP recv timeout → local listener unavailable in this environment\n");
        return;
    }
#endif

    int opt = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(zanna::tests::kIpv4LoopbackHostOrder);
    addr.sin_port = 0; // Let OS assign port

    int rc = bind(listener, (struct sockaddr *)&addr, sizeof(addr));
    if (rc != 0) {
#if defined(_WIN32)
        closesocket(listener);
        WSACleanup();
#else
        close(listener);
#endif
        printf("  SKIP: TCP recv timeout → local bind unavailable in this environment\n");
        return;
    }

    rc = listen(listener, 1);
    if (rc != 0) {
#if defined(_WIN32)
        closesocket(listener);
        WSACleanup();
#else
        close(listener);
#endif
        printf("  SKIP: TCP recv timeout → local listen unavailable in this environment\n");
        return;
    }

    // Get the assigned port
#if defined(_WIN32)
    int addrlen = sizeof(addr);
#else
    socklen_t addrlen = sizeof(addr);
#endif
    getsockname(listener, (struct sockaddr *)&addr, &addrlen);
    int port = ntohs(addr.sin_port);

    // Connect using the runtime API
    rt_string host = rt_string_from_bytes("127.0.0.1", 9);
    void *conn = rt_tcp_connect(host, port);
    assert(conn != NULL);

    // Accept on the server side (but never send anything)
#if defined(_WIN32)
    SOCKET client_fd = accept(listener, NULL, NULL);
    assert(client_fd != INVALID_SOCKET);
#else
    int client_fd = accept(listener, NULL, NULL);
    assert(client_fd >= 0);
#endif

    // Set a short recv timeout (100ms) and try to receive
    rt_tcp_set_recv_timeout(conn, 100);

    g_trap_count = 0;
    g_last_trap.clear();

    void *result = rt_tcp_recv(conn, 1024);
    assert(result != NULL);
    assert(rt_bytes_len(result) == 0); // Timeout -> empty bytes
    assert(g_trap_count == 0);         // No trap

    // Clean up
    rt_tcp_close(conn);
    if (result && rt_obj_release_check0(result))
        rt_obj_free(result);
    if (conn && rt_obj_release_check0(conn))
        rt_obj_free(conn);
    rt_string_unref(host);
#if defined(_WIN32)
    closesocket(client_fd);
    closesocket(listener);
    WSACleanup();
#else
    close(client_fd);
    close(listener);
#endif
}

/// @brief Open a loopback listener on an OS-selected port.
/// @param port_out Receives the selected port on success.
/// @return Owned listener, or @c INVALID_SOCK when local binding is unavailable.
static socket_t open_http_timeout_listener(int *port_out) {
    if (port_out)
        *port_out = 0;
    rt_net_init_wsa();
    socket_t listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == INVALID_SOCK)
        return INVALID_SOCK;

    int reuse = 1;
    (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(zanna::tests::kIpv4LoopbackHostOrder);
    address.sin_port = 0;
    if (bind(listener, (sockaddr *)&address, sizeof(address)) != 0 || listen(listener, 2) != 0) {
        CLOSE_SOCKET(listener);
        return INVALID_SOCK;
    }

#if RT_PLATFORM_WINDOWS
    int address_len = sizeof(address);
#else
    socklen_t address_len = sizeof(address);
#endif
    if (getsockname(listener, (sockaddr *)&address, &address_len) != 0) {
        CLOSE_SOCKET(listener);
        return INVALID_SOCK;
    }
    if (port_out)
        *port_out = ntohs(address.sin_port);
    return listener;
}

/// @brief Drain one HTTP request head from a loopback client.
/// @param client Connected accepted socket.
static void drain_http_timeout_request(socket_t client) {
    char request[4096];
    size_t request_len = 0;
    while (request_len < sizeof(request)) {
        int received = recv(client, request + request_len, 1, 0);
        if (received <= 0)
            break;
        request_len += (size_t)received;
        if (request_len >= 4 && memcmp(request + request_len - 4, "\r\n\r\n", 4) == 0)
            break;
    }
}

/// @brief Send a complete small fixture buffer.
/// @param client Connected accepted socket.
/// @param data Bytes to send.
/// @param len Exact byte count.
/// @return True on complete delivery.
static bool send_http_timeout_fixture(socket_t client, const char *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        size_t remaining = len - sent;
        int chunk = remaining > (size_t)INT_MAX ? INT_MAX : (int)remaining;
        int written = send(client, data + sent, chunk, SEND_FLAGS);
        if (written <= 0)
            return false;
        sent += (size_t)written;
    }
    return true;
}

/// @brief Test whether an HTTP SendResult contains the timeout diagnostic.
/// @param result Candidate Result returned by the request API.
/// @return True only for an Err(String) naming elapsed timeout.
static bool http_result_is_timeout(void *result) {
    if (!result || rt_result_is_err(result) != 1)
        return false;
    rt_string error = rt_result_unwrap_err_str(result);
    return error && strstr(rt_string_cstr(error), "timed out") != nullptr;
}

/// @brief Prove partial HTTP body progress cannot reset the request timeout.
/// @details The loopback peer sends a valid fixed-length response one byte
///          every 25 ms. A 150 ms per-read timeout would incorrectly permit
///          the roughly 800 ms response, while one absolute request deadline
///          deterministically returns `Result.Err` with a timeout diagnostic.
static void test_http_trickle_honors_end_to_end_deadline() {
    int port = 0;
    socket_t listener = open_http_timeout_listener(&port);
    if (listener == INVALID_SOCK) {
        printf("  SKIP: HTTP trickle deadline → local listener unavailable\n");
        return;
    }

    std::thread server([listener]() {
        socket_t client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCK) {
            CLOSE_SOCKET(listener);
            return;
        }
        suppress_sigpipe(client);
        drain_http_timeout_request(client);

        static const char response_head[] = "HTTP/1.1 200 OK\r\n"
                                            "Content-Length: 32\r\n"
                                            "Connection: close\r\n"
                                            "\r\n";
        (void)send_http_timeout_fixture(client, response_head, sizeof(response_head) - 1);
        for (int i = 0; i < 32; i++) {
            const char byte = (char)('a' + i % 26);
            if (send(client, &byte, 1, 0) != 1)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        CLOSE_SOCKET(client);
        CLOSE_SOCKET(listener);
    });

    char url[96];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/trickle", port);
    void *request = rt_http_req_new(rt_const_cstr("GET"), rt_const_cstr(url));
    assert(request != nullptr);
    assert(rt_http_req_set_timeout(request, 150) == request);
    assert(rt_http_req_set_force_http1(request, 1) == request);

    auto started = std::chrono::steady_clock::now();
    void *result = rt_http_req_send_result(request);
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - started)
                          .count();
    server.join();

    assert(result != nullptr);
    assert(http_result_is_timeout(result));
    assert(elapsed_ms < 2000);

    if (result && rt_obj_release_check0(result))
        rt_obj_free(result);
    if (request && rt_obj_release_check0(request))
        rt_obj_free(request);
}

/// @brief Prove redirect recursion consumes the original request budget.
/// @details Each of two response heads arrives within the configured per-hop
///          timeout, but their combined delay exceeds it. Recomputing a
///          deadline for the redirected clone would therefore succeed
///          incorrectly.
static void test_http_redirect_reuses_original_deadline() {
    int port = 0;
    socket_t listener = open_http_timeout_listener(&port);
    if (listener == INVALID_SOCK) {
        printf("  SKIP: HTTP redirect deadline → local listener unavailable\n");
        return;
    }

    std::thread server([listener, port]() {
        for (int hop = 0; hop < 2; hop++) {
            if (wait_socket(listener, 1000, false) <= 0)
                break;
            socket_t client = accept(listener, nullptr, nullptr);
            if (client == INVALID_SOCK)
                break;
            suppress_sigpipe(client);
            drain_http_timeout_request(client);
            std::this_thread::sleep_for(std::chrono::milliseconds(90));

            char response[256];
            int response_len = 0;
            if (hop == 0) {
                response_len = snprintf(response,
                                        sizeof(response),
                                        "HTTP/1.1 302 Found\r\n"
                                        "Location: http://127.0.0.1:%d/final\r\n"
                                        "Content-Length: 0\r\n"
                                        "Connection: close\r\n"
                                        "\r\n",
                                        port);
            } else {
                response_len = snprintf(response,
                                        sizeof(response),
                                        "HTTP/1.1 200 OK\r\n"
                                        "Content-Length: 2\r\n"
                                        "Connection: close\r\n"
                                        "\r\n"
                                        "ok");
            }
            if (response_len > 0 && (size_t)response_len < sizeof(response))
                (void)send_http_timeout_fixture(client, response, (size_t)response_len);
            CLOSE_SOCKET(client);
        }
        CLOSE_SOCKET(listener);
    });

    char url[96];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/start", port);
    void *request = rt_http_req_new(rt_const_cstr("GET"), rt_const_cstr(url));
    assert(request != nullptr);
    assert(rt_http_req_set_timeout(request, 150) == request);
    assert(rt_http_req_set_follow_redirects(request, 1) == request);
    assert(rt_http_req_set_force_http1(request, 1) == request);

    void *result = rt_http_req_send_result(request);
    server.join();
    assert(http_result_is_timeout(result));

    if (result && rt_obj_release_check0(result))
        rt_obj_free(result);
    if (request && rt_obj_release_check0(request))
        rt_obj_free(request);
}

/// @brief Prove a partial TLS record cannot reset the HTTP request deadline.
/// @details The peer emits a syntactically plausible TLS record header and
///          payload one byte at a time. Record-layer readiness must use HTTP's
///          absolute deadline rather than restarting the socket timeout after
///          each received byte.
static void test_https_record_trickle_honors_http_deadline() {
    int port = 0;
    socket_t listener = open_http_timeout_listener(&port);
    if (listener == INVALID_SOCK) {
        printf("  SKIP: HTTPS record deadline → local listener unavailable\n");
        return;
    }

    std::thread server([listener]() {
        if (wait_socket(listener, 1000, false) <= 0) {
            CLOSE_SOCKET(listener);
            return;
        }
        socket_t client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCK) {
            CLOSE_SOCKET(listener);
            return;
        }
        suppress_sigpipe(client);
        uint8_t record[37] = {};
        record[0] = 22;   // Handshake record
        record[1] = 0x03; // TLS 1.2 legacy record version
        record[2] = 0x03;
        record[3] = 0;
        record[4] = 32;
        for (size_t i = 0; i < sizeof(record); i++) {
            if (send(client, (const char *)&record[i], 1, SEND_FLAGS) != 1)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        CLOSE_SOCKET(client);
        CLOSE_SOCKET(listener);
    });

    char url[96];
    snprintf(url, sizeof(url), "https://127.0.0.1:%d/trickle", port);
    void *request = rt_http_req_new(rt_const_cstr("GET"), rt_const_cstr(url));
    assert(request != nullptr);
    assert(rt_http_req_set_timeout(request, 150) == request);
    assert(rt_http_req_set_force_http1(request, 1) == request);
    assert(rt_http_req_allow_insecure_certificates_for_testing(request) == request);

    void *result = rt_http_req_send_result(request);
    server.join();
    assert(http_result_is_timeout(result));

    if (result && rt_obj_release_check0(result))
        rt_obj_free(result);
    if (request && rt_obj_release_check0(request))
        rt_obj_free(request);
}

int main() {
#if !RT_PLATFORM_WINDOWS
    std::signal(SIGPIPE, SIG_IGN);
#endif
    test_tcp_recv_timeout();
    printf("  PASS: TCP recv with 100ms timeout -> empty bytes, no crash\n");
    test_http_trickle_honors_end_to_end_deadline();
    printf("  PASS: HTTP trickle cannot reset the end-to-end deadline\n");
    test_http_redirect_reuses_original_deadline();
    printf("  PASS: HTTP redirects preserve the original deadline\n");
    test_https_record_trickle_honors_http_deadline();
    printf("  PASS: HTTPS TLS records preserve the HTTP deadline\n");

    printf("All network-timeout tests passed.\n");
    return 0;
}
