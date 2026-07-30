//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTNetworkTests.cpp
// Purpose: Validate Zanna.Network.Tcp and TcpServer support.
// Key invariants:
//   - Client/server communication and timeout handling remain deterministic.
//   - Local socket probes bind only to loopback or wildcard addresses.
// Ownership/Lifetime:
//   - Each helper owns and closes any native socket it creates.
//   - Runtime objects allocated by tests remain valid for each test scope.
// Links: docs/zannalib/network.md,
//        docs/adr/0229-bounded-native-gzip-decoding.md
//
//===----------------------------------------------------------------------===//

#include "tests/common/NetworkTestCompat.hpp"

#include "rt_async_socket.h"
#include "rt_box.h"
#include "rt_bytes.h"
#include "rt_compress.h"
#include "rt_connpool.h"
#include "rt_file_path.h"
#include "rt_future.h"
#include "rt_gc.h"
#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_map.h"
#include "rt_netutils.h"
#include "rt_network.h"
#include "rt_network_http_internal.h"
#include "rt_object.h"
#include "rt_platform.h"
#include "rt_result.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "rt_trap.h"

#include <atomic>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#if RT_PLATFORM_WINDOWS
#include <io.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if RT_PLATFORM_LINUX || RT_PLATFORM_WINDOWS
static constexpr int64_t kNetworkTimeoutUpperBoundMs = 5000;
#else
static constexpr int64_t kNetworkTimeoutUpperBoundMs = 500;
#endif
#if RT_PLATFORM_WINDOWS
static constexpr int64_t kNetworkTimeoutLowerBoundMs = 50;
#else
static constexpr int64_t kNetworkTimeoutLowerBoundMs = 90;
#endif

#if RT_PLATFORM_WINDOWS
static FILE *network_test_fopen_utf8(const char *path, const wchar_t *mode) {
    wchar_t *wide = rt_file_path_utf8_to_wide(path);
    if (!wide)
        return nullptr;
    FILE *stream = _wfopen(wide, mode);
    std::free(wide);
    return stream;
}

static int network_test_remove_utf8(const char *path) {
    wchar_t *wide = rt_file_path_utf8_to_wide(path);
    if (!wide)
        return -1;
    int result = _wremove(wide);
    std::free(wide);
    return result;
}
#endif

/// @brief Helper to print test result.
static void test_result(const char *name, bool passed) {
    printf("  %s: %s\n", name, passed ? "PASS" : "FAIL");
    assert(passed);
}

/// @brief Get bytes data pointer
static uint8_t *get_bytes_data(void *bytes) {
    return rt_bytes_data(bytes);
}

/// @brief Get bytes length
static int64_t get_bytes_len(void *bytes) {
    return rt_bytes_len(bytes);
}

/// @brief Create bytes from string literal
static void *make_bytes_str(const char *str) {
    size_t len = strlen(str);
    void *bytes = rt_bytes_new((int64_t)len);
    memcpy(get_bytes_data(bytes), str, len);
    return bytes;
}

/// @brief Release one test-owned managed object and run its finalizer at zero.
/// @details Test helpers use explicit ownership rather than relying on process
///          teardown so socket and Bytes regressions are visible to leak and
///          sanitizer runs.
/// @param obj Owned runtime object reference; NULL is a no-op.
static void release_test_object(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Wait for an asynchronously released object reference to reach an expected count.
/// @details Promise completion wakes Future waiters before the producing worker
///          has necessarily executed its final cleanup instruction. Reading the
///          count immediately after dropping the Future therefore races that
///          legitimate cleanup and made the ownership regression intermittent.
///          This helper performs acquire loads through the runtime's portable
///          atomic adapter and yields for at most two seconds. A leaked producer
///          reference remains observable as a timeout, while normal scheduling
///          latency cannot make the test fail spuriously.
/// @param obj Live managed object whose caller-owned reference keeps it valid.
/// @param expected Reference count required by the ownership contract.
/// @return True when @p expected was observed before the bounded deadline.
static bool wait_for_refcount(void *obj, size_t expected) {
    if (!obj)
        return false;
    rt_heap_hdr_t *header = rt_heap_hdr(obj);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
        if (rt_atomic_load_size(&header->refcnt, __ATOMIC_ACQUIRE) == expected)
            return true;
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    return rt_atomic_load_size(&header->refcnt, __ATOMIC_ACQUIRE) == expected;
}

/// @brief Atomic flag for server shutdown
static std::atomic<bool> server_ready{false};
static std::atomic<bool> server_done{false};
static std::atomic<size_t> http_gzip_encoded_size{0};
static std::string http_accept_encoding_header;
static int tcp_alloc_fail_countdown = 0;
static int http_alloc_fail_countdown = 0;
static int http_alloc_observed = 0;

/// @brief Fail one selected runtime allocation during TCP ownership tests.
/// @details Decrements @ref tcp_alloc_fail_countdown for each allocation routed
///          through @ref rt_alloc. The call that reaches zero returns NULL;
///          every other call delegates to the runtime's default allocator.
///          Tests install this hook only while the peer thread performs raw
///          socket writes and therefore makes no managed allocations.
/// @param bytes Requested allocation size in bytes.
/// @param next Runtime default allocator to invoke when this call should pass.
/// @return Allocated storage, or NULL for the selected injected failure.
static void *tcp_fail_countdown_alloc(int64_t bytes, void *(*next)(int64_t)) {
    if (tcp_alloc_fail_countdown > 0 && --tcp_alloc_fail_countdown == 0)
        return nullptr;
    return next(bytes);
}

/// @brief Count managed allocations and optionally fail one selected HTTP allocation.
/// @details The native loopback server used by HTTP fault tests performs no
///          runtime allocations, so every observed call belongs to the client
///          path under test. A zero countdown means count-only mode.
/// @param bytes Requested managed allocation size.
/// @param next Runtime default allocator.
/// @return Allocated storage, or NULL at the selected countdown.
static void *http_countdown_alloc(int64_t bytes, void *(*next)(int64_t)) {
    http_alloc_observed++;
    if (http_alloc_fail_countdown > 0 && --http_alloc_fail_countdown == 0)
        return nullptr;
    return next(bytes);
}

#if RT_PLATFORM_WINDOWS
using http_test_socket_t = SOCKET;
static constexpr http_test_socket_t kInvalidHttpTestSocket = INVALID_SOCKET;
#else
using http_test_socket_t = int;
static constexpr http_test_socket_t kInvalidHttpTestSocket = -1;
#endif

/// @brief Close one native socket owned by an HTTP fault-injection server.
/// @param socket Native socket descriptor; invalid values are ignored.
static void close_http_test_socket(http_test_socket_t socket) {
    if (socket == kInvalidHttpTestSocket)
        return;
#if RT_PLATFORM_WINDOWS
    closesocket(socket);
#else
    close(socket);
#endif
}

/// @brief Terminate the test process when its native HTTP fixture cannot start.
/// @details Fixture setup must remain active in Release builds, where `assert`
///          expressions are compiled out. Aborting makes infrastructure
///          failures visible to CTest instead of leaving a client hung.
/// @param operation Native socket operation that failed.
[[noreturn]] static void fail_native_http_fault_server(const char *operation) {
    fprintf(stderr, "native_http_fault_server: %s failed\n", operation);
    std::abort();
}

/// @brief Serve one deterministic HTTP/1.1 response without runtime allocation.
/// @details Raw OS sockets isolate the process-wide managed allocation hook to
///          the client. The server binds loopback, drains one complete request
///          head into a fixed stack buffer, sends a fixed-length response, and
///          closes both descriptors on every normal path.
/// @param port Preselected loopback TCP port.
/// @param response_body NUL-terminated ASCII body that fits the fixed response buffer.
static void native_http_fault_server(int port, const char *response_body) {
#if RT_PLATFORM_WINDOWS
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        fail_native_http_fault_server("WSAStartup");
#endif
    http_test_socket_t listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == kInvalidHttpTestSocket)
        fail_native_http_fault_server("socket");

    int reuse = 1;
#if RT_PLATFORM_WINDOWS
    (void)setsockopt(
        listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));
#else
    (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(zanna::tests::kIpv4LoopbackHostOrder);
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0)
        fail_native_http_fault_server("bind");
    if (listen(listener, 1) != 0)
        fail_native_http_fault_server("listen");
    server_ready.store(true, std::memory_order_release);

    http_test_socket_t client = accept(listener, nullptr, nullptr);
    if (client == kInvalidHttpTestSocket)
        fail_native_http_fault_server("accept");
    char request[4096];
    size_t request_len = 0;
    while (request_len < sizeof(request)) {
#if RT_PLATFORM_WINDOWS
        int received = recv(client, request + request_len, 1, 0);
#else
        ssize_t received = recv(client, request + request_len, 1, 0);
#endif
        if (received <= 0)
            break;
        request_len += static_cast<size_t>(received);
        if (request_len >= 4 && request[request_len - 4] == '\r' &&
            request[request_len - 3] == '\n' && request[request_len - 2] == '\r' &&
            request[request_len - 1] == '\n') {
            break;
        }
    }

    char response[2048];
    const int response_len = snprintf(response,
                                      sizeof(response),
                                      "HTTP/1.1 200 OK\r\n"
                                      "Content-Type: text/plain\r\n"
                                      "X-Fault-Test: stable\r\n"
                                      "Content-Length: %zu\r\n"
                                      "Connection: close\r\n"
                                      "\r\n%s",
                                      strlen(response_body),
                                      response_body);
    if (response_len <= 0 || static_cast<size_t>(response_len) >= sizeof(response))
        fail_native_http_fault_server("response formatting");
    size_t sent = 0;
    while (sent < static_cast<size_t>(response_len)) {
        const size_t remaining = static_cast<size_t>(response_len) - sent;
        const int chunk =
            remaining > static_cast<size_t>(INT_MAX) ? INT_MAX : static_cast<int>(remaining);
        const int written = send(client, response + sent, chunk, 0);
        if (written <= 0)
            break;
        sent += static_cast<size_t>(written);
    }

    close_http_test_socket(client);
    close_http_test_socket(listener);
    server_done.store(true, std::memory_order_release);
#if RT_PLATFORM_WINDOWS
    WSACleanup();
#endif
}

static bool ascii_header_name_equals(const char *line, const char *name, size_t name_len) {
    if (!line || !name)
        return false;
    for (size_t i = 0; i < name_len; i++) {
        char a = line[i];
        char b = name[i];
        if (a >= 'A' && a <= 'Z')
            a = (char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z')
            b = (char)(b + ('a' - 'A'));
        if (a != b)
            return false;
    }
    return true;
}

static bool localhost_bind_available() {
    static const bool available = []() {
#if RT_PLATFORM_WINDOWS
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
            return false;
        SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd == INVALID_SOCKET) {
            WSACleanup();
            return false;
        }
#else
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return false;
#endif

        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(zanna::tests::kIpv4LoopbackHostOrder);
        addr.sin_port = 0;

        const int rc = bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
#if RT_PLATFORM_WINDOWS
        closesocket(fd);
        WSACleanup();
#else
        close(fd);
#endif
        return rc == 0;
    }();

    return available;
}

static bool localhost_bind_available_ipv6() {
    static const bool available = []() {
#if RT_PLATFORM_WINDOWS
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
            return false;
        SOCKET fd = socket(AF_INET6, SOCK_STREAM, 0);
        if (fd == INVALID_SOCKET) {
            WSACleanup();
            return false;
        }
#else
        int fd = socket(AF_INET6, SOCK_STREAM, 0);
        if (fd < 0)
            return false;
#endif

        sockaddr_in6 addr = {};
        addr.sin6_family = AF_INET6;
        addr.sin6_addr = in6addr_loopback;
        addr.sin6_port = 0;

        const int rc = bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
#if RT_PLATFORM_WINDOWS
        closesocket(fd);
        WSACleanup();
#else
        close(fd);
#endif
        return rc == 0;
    }();

    return available;
}

static int get_free_port_ipv4(int socktype) {
#if RT_PLATFORM_WINDOWS
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return 0;
    SOCKET fd = socket(AF_INET, socktype, 0);
    if (fd == INVALID_SOCKET) {
        WSACleanup();
        return 0;
    }
#else
    int fd = socket(AF_INET, socktype, 0);
    if (fd < 0)
        return 0;
#endif

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = 0;

    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
#if RT_PLATFORM_WINDOWS
        closesocket(fd);
        WSACleanup();
#else
        close(fd);
#endif
        return 0;
    }

    socklen_t len = sizeof(addr);
    const int ok = getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len) == 0;
    const int port = ok ? ntohs(addr.sin_port) : 0;

#if RT_PLATFORM_WINDOWS
    closesocket(fd);
    WSACleanup();
#else
    close(fd);
#endif
    return port;
}

static int get_free_tcp_port_ipv4() {
    return get_free_port_ipv4(SOCK_STREAM);
}

static int get_free_udp_port_ipv4() {
    return get_free_port_ipv4(SOCK_DGRAM);
}

static int get_distinct_free_udp_port_ipv4(int avoid_port) {
    for (int i = 0; i < 16; ++i) {
        const int port = get_free_udp_port_ipv4();
        if (port > 0 && port != avoid_port)
            return port;
    }
    return 0;
}

static int get_free_port_ipv6(int socktype) {
#if RT_PLATFORM_WINDOWS
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return 0;
    SOCKET fd = socket(AF_INET6, socktype, 0);
    if (fd == INVALID_SOCKET) {
        WSACleanup();
        return 0;
    }
#else
    int fd = socket(AF_INET6, socktype, 0);
    if (fd < 0)
        return 0;
#endif

    sockaddr_in6 addr = {};
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_loopback;
    addr.sin6_port = 0;
    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
#if RT_PLATFORM_WINDOWS
        closesocket(fd);
        WSACleanup();
#else
        close(fd);
#endif
        return 0;
    }

    socklen_t len = sizeof(addr);
    const int ok = getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len) == 0;
    int port = ok ? ntohs(addr.sin6_port) : 0;
#if RT_PLATFORM_WINDOWS
    closesocket(fd);
    WSACleanup();
#else
    close(fd);
#endif
    return port;
}

static int get_free_tcp_port_ipv6() {
    return get_free_port_ipv6(SOCK_STREAM);
}

static int get_free_udp_port_ipv6() {
    return get_free_port_ipv6(SOCK_DGRAM);
}

static int get_distinct_free_udp_port_ipv6(int avoid_port) {
    for (int i = 0; i < 16; ++i) {
        const int port = get_free_udp_port_ipv6();
        if (port > 0 && port != avoid_port)
            return port;
    }
    return 0;
}

/// @brief Echo server thread function
static void echo_server_thread(int port, int num_clients) {
    void *server = rt_tcp_server_listen(port);
    assert(server != nullptr);

    printf("  Echo server started on port %d\n", port);
    server_ready = true;

    for (int i = 0; i < num_clients; i++) {
        void *client = rt_tcp_server_accept(server);
        if (!client)
            break;

        // Echo loop - receive and send back
        while (rt_tcp_is_open(client)) {
            void *data = rt_tcp_recv(client, 1024);
            int64_t len = get_bytes_len(data);
            if (len == 0) {
                // Connection closed
                release_test_object(data);
                break;
            }

            // Send back what we received
            rt_tcp_send_all(client, data);
            release_test_object(data);
        }

        rt_tcp_close(client);
        release_test_object(client);
    }

    rt_tcp_server_close(server);
    release_test_object(server);
    server_done = true;
}

static void echo_server_thread_at(const char *address, int port, int num_clients) {
    void *server = rt_tcp_server_listen_at(rt_const_cstr(address), port);
    assert(server != nullptr);

    server_ready = true;

    for (int i = 0; i < num_clients; i++) {
        void *client = rt_tcp_server_accept(server);
        if (!client)
            break;

        while (rt_tcp_is_open(client)) {
            void *data = rt_tcp_recv(client, 1024);
            int64_t len = get_bytes_len(data);
            if (len == 0) {
                release_test_object(data);
                break;
            }
            rt_tcp_send_all(client, data);
            release_test_object(data);
        }

        rt_tcp_close(client);
        release_test_object(client);
    }

    rt_tcp_server_close(server);
    release_test_object(server);
    server_done = true;
}

/// @brief Test server listen and client connect
static void test_server_client_connect() {
    printf("\nTesting Server/Client Connect:\n");

    const int port = get_free_tcp_port_ipv4();
    assert(port > 0);
    server_ready = false;
    server_done = false;

    // Start server in background thread
    std::thread server_thread(echo_server_thread, port, 1);

    // Wait for server to be ready
    while (!server_ready) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Connect client
    rt_string host = rt_const_cstr("127.0.0.1");
    void *client = rt_tcp_connect(host, port);

    test_result("Client connects successfully", client != nullptr);
    test_result("Client is open", rt_tcp_is_open(client) == 1);
    test_result("Client port is correct", rt_tcp_port(client) == port);

    // Close client and wait for server
    rt_tcp_close(client);
    server_thread.join();

    test_result("Server finished", server_done.load());
}

static void test_server_client_connect_ipv6() {
    printf("\nTesting Server/Client Connect over IPv6:\n");

    if (!localhost_bind_available_ipv6()) {
        printf("  SKIPPED: IPv6 loopback unavailable in this environment\n");
        return;
    }

    const int port = get_free_tcp_port_ipv6();
    if (port <= 0) {
        printf("  SKIPPED: could not allocate IPv6 loopback port\n");
        return;
    }

    server_ready = false;
    server_done = false;

    std::thread server_thread(echo_server_thread_at, "::1", port, 1);

    while (!server_ready)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    void *client = rt_tcp_connect(rt_const_cstr("::1"), port);

    test_result("IPv6 client connects successfully", client != nullptr);
    test_result("IPv6 client is open", rt_tcp_is_open(client) == 1);
    test_result("IPv6 client port is correct", rt_tcp_port(client) == port);
    test_result("IPv6 host property preserved",
                strcmp(rt_string_cstr(rt_tcp_host(client)), "::1") == 0);

    const char *test_msg = "ipv6";
    void *send_data = make_bytes_str(test_msg);
    rt_tcp_send_all(client, send_data);
    void *recv_data = rt_tcp_recv_exact(client, (int64_t)strlen(test_msg));
    test_result("IPv6 echo round-trip succeeds",
                memcmp(get_bytes_data(recv_data), test_msg, strlen(test_msg)) == 0);

    rt_tcp_close(client);
    server_thread.join();
    test_result("IPv6 server finished", server_done.load());
}

/// @brief Test send and receive
static void test_send_recv() {
    printf("\nTesting Send/Receive:\n");

    const int port = get_free_tcp_port_ipv4();
    assert(port > 0);
    server_ready = false;
    server_done = false;

    std::thread server_thread(echo_server_thread, port, 1);

    while (!server_ready) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    rt_string host = rt_const_cstr("127.0.0.1");
    void *client = rt_tcp_connect(host, port);
    assert(client != nullptr);

    // Test Send bytes
    const char *test_msg = "Hello, World!";
    void *send_data = make_bytes_str(test_msg);
    int64_t sent = rt_tcp_send(client, send_data);

    test_result("Send returns correct byte count", sent == (int64_t)strlen(test_msg));

    // Receive echo
    void *recv_data = rt_tcp_recv(client, 1024);
    int64_t recv_len = get_bytes_len(recv_data);

    test_result("Recv returns correct byte count", recv_len == (int64_t)strlen(test_msg));
    test_result("Recv data matches sent data",
                memcmp(get_bytes_data(recv_data), test_msg, strlen(test_msg)) == 0);

    // Test SendStr
    rt_string send_str = rt_const_cstr("Test string!");
    sent = rt_tcp_send_str(client, send_str);

    test_result("SendStr returns correct byte count", sent == 12);

    // Receive string echo
    rt_string recv_str = rt_tcp_recv_str(client, 1024);
    const char *recv_cstr = rt_string_cstr(recv_str);

    test_result("RecvStr returns correct string", strcmp(recv_cstr, "Test string!") == 0);

    rt_tcp_close(client);
    server_thread.join();
}

/// @brief Feed deterministic receive payloads without allocating during fault injection.
/// @details The server accepts one client, then waits for four caller-published
///          phases. Each phase writes a fixed raw payload and publishes its
///          completion before waiting again. This keeps the process-global
///          allocation hook isolated to the client under test.
/// @param port Loopback TCP port selected by the test.
/// @param requested Highest send phase requested by the client.
/// @param published Highest payload phase placed in the socket send buffer.
static void tcp_trap_cleanup_server(int port,
                                    std::atomic<int> &requested,
                                    std::atomic<int> &published) {
    void *server = rt_tcp_server_listen(port);
    assert(server != nullptr);
    server_ready = true;

    void *client = rt_tcp_server_accept(server);
    assert(client != nullptr);
    static const char *const payloads[] = {"a", "b", "c\n", "d"};
    static const int64_t lengths[] = {1, 1, 2, 1};
    for (int phase = 1; phase <= 4; ++phase) {
        while (requested.load(std::memory_order_acquire) < phase)
            std::this_thread::yield();
        rt_tcp_send_all_raw(client, payloads[phase - 1], lengths[phase - 1]);
        published.store(phase, std::memory_order_release);
    }

    rt_tcp_close(client);
    release_test_object(client);
    rt_tcp_server_close(server);
    release_test_object(server);
    server_done = true;
}

/// @brief Verify receive helpers release intermediate storage when result allocation traps.
/// @details Injects failures into (1) the right-sizing allocation after a short
///          `Recv`, (2) string construction after `RecvStr` has allocated its
///          temporary Bytes object, and (3) string construction after
///          `RecvLine` has accumulated a native buffer. A final exact receive
///          proves each recovered trap leaves the transport usable. Leak and
///          sanitizer runs additionally verify that no hidden temporary remains.
static void test_tcp_receive_trap_cleanup() {
    printf("\nTesting TCP receive allocation-trap cleanup:\n");

    const int port = get_free_tcp_port_ipv4();
    assert(port > 0);
    server_ready = false;
    server_done = false;
    std::atomic<int> requested{0};
    std::atomic<int> published{0};
    std::thread server_thread(
        tcp_trap_cleanup_server, port, std::ref(requested), std::ref(published));
    while (!server_ready.load(std::memory_order_acquire))
        std::this_thread::yield();

    void *client = rt_tcp_connect(rt_const_cstr("127.0.0.1"), port);
    assert(client != nullptr);
    jmp_buf recovery;

    requested.store(1, std::memory_order_release);
    while (published.load(std::memory_order_acquire) < 1)
        std::this_thread::yield();
    bool short_recv_trapped = false;
    tcp_alloc_fail_countdown = 2;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        rt_set_alloc_hook(tcp_fail_countdown_alloc);
        (void)rt_tcp_recv(client, 32);
        rt_set_alloc_hook(nullptr);
        rt_trap_clear_recovery();
    } else {
        rt_set_alloc_hook(nullptr);
        const char *error = rt_trap_get_error();
        short_recv_trapped = error && strstr(error, "allocation failed") != nullptr;
        rt_trap_clear_recovery();
    }
    test_result("Short Recv cleans its oversized Bytes after allocation trap",
                short_recv_trapped && tcp_alloc_fail_countdown == 0);

    requested.store(2, std::memory_order_release);
    while (published.load(std::memory_order_acquire) < 2)
        std::this_thread::yield();
    bool recv_str_trapped = false;
    tcp_alloc_fail_countdown = 2;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        rt_set_alloc_hook(tcp_fail_countdown_alloc);
        (void)rt_tcp_recv_str(client, 1);
        rt_set_alloc_hook(nullptr);
        rt_trap_clear_recovery();
    } else {
        rt_set_alloc_hook(nullptr);
        const char *error = rt_trap_get_error();
        recv_str_trapped = error && strstr(error, "alloc") != nullptr;
        rt_trap_clear_recovery();
    }
    test_result("RecvStr cleans its Bytes after string allocation trap",
                recv_str_trapped && tcp_alloc_fail_countdown == 0);

    requested.store(3, std::memory_order_release);
    while (published.load(std::memory_order_acquire) < 3)
        std::this_thread::yield();
    bool recv_line_trapped = false;
    tcp_alloc_fail_countdown = 1;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        rt_set_alloc_hook(tcp_fail_countdown_alloc);
        (void)rt_tcp_recv_line(client);
        rt_set_alloc_hook(nullptr);
        rt_trap_clear_recovery();
    } else {
        rt_set_alloc_hook(nullptr);
        const char *error = rt_trap_get_error();
        recv_line_trapped = error && strstr(error, "alloc") != nullptr;
        rt_trap_clear_recovery();
    }
    test_result("RecvLine cleans its native buffer after string allocation trap",
                recv_line_trapped && tcp_alloc_fail_countdown == 0);

    requested.store(4, std::memory_order_release);
    while (published.load(std::memory_order_acquire) < 4)
        std::this_thread::yield();
    void *final_byte = rt_tcp_recv_exact(client, 1);
    test_result("TCP remains usable after recovered receive allocation traps",
                final_byte && get_bytes_len(final_byte) == 1 &&
                    get_bytes_data(final_byte)[0] == (uint8_t)'d');
    release_test_object(final_byte);

    rt_tcp_close(client);
    release_test_object(client);
    server_thread.join();
    test_result("Trap-cleanup server released its transport resources", server_done.load());
}

/// @brief Test SendAll and RecvExact
static void test_send_all_recv_exact() {
    printf("\nTesting SendAll/RecvExact:\n");

    const int port = get_free_tcp_port_ipv4();
    assert(port > 0);
    server_ready = false;
    server_done = false;

    std::thread server_thread(echo_server_thread, port, 1);

    while (!server_ready) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    rt_string host = rt_const_cstr("127.0.0.1");
    void *client = rt_tcp_connect(host, port);
    assert(client != nullptr);

    // Send larger data
    const int data_size = 4096;
    void *large_data = rt_bytes_new(data_size);
    uint8_t *ptr = get_bytes_data(large_data);
    for (int i = 0; i < data_size; i++) {
        ptr[i] = (uint8_t)(i & 0xFF);
    }

    rt_tcp_send_all(client, large_data);

    // Receive exactly that many bytes
    void *recv_data = rt_tcp_recv_exact(client, data_size);

    test_result("RecvExact returns correct size", get_bytes_len(recv_data) == data_size);
    test_result("RecvExact data matches", memcmp(get_bytes_data(recv_data), ptr, data_size) == 0);

    rt_tcp_close(client);
    server_thread.join();
}

static void test_connection_pool_reuses_live_connection() {
    printf("\nTesting ConnectionPool live connection reuse:\n");

    const int port = (int)rt_netutils_get_free_port();
    if (port <= 0) {
        printf("  SKIPPED: could not allocate local port\n");
        return;
    }

    server_ready = false;
    server_done = false;

    std::thread server_thread(echo_server_thread, port, 1);
    while (!server_ready)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    void *pool = rt_connpool_new(4);
    test_result("ConnectionPool has its stable managed class identity",
                rt_obj_class_id(pool) == RT_CONNPOOL_CLASS_ID);
    void *conn1 = rt_connpool_acquire(pool, rt_const_cstr("127.0.0.1"), port);
    test_result("Pool acquire returns connection", conn1 != nullptr);
    test_result("Pooled TCP has its stable managed class identity",
                rt_obj_class_id(conn1) == RT_TCP_CLASS_ID);

    void *msg1 = make_bytes_str("one");
    rt_tcp_send_all(conn1, msg1);
    void *echo1 = rt_tcp_recv_exact(conn1, 3);
    test_result("First pooled round-trip succeeds", memcmp(get_bytes_data(echo1), "one", 3) == 0);

    rt_connpool_release(pool, conn1);
    test_result("Pool has one available connection", rt_connpool_available(pool) == 1);

    void *conn2 = rt_connpool_acquire(pool, rt_const_cstr("127.0.0.1"), port);
    test_result("Pool reuses the same live connection", conn1 == conn2);

    void *msg2 = make_bytes_str("two");
    rt_tcp_send_all(conn2, msg2);
    void *echo2 = rt_tcp_recv_exact(conn2, 3);
    test_result("Second pooled round-trip succeeds", memcmp(get_bytes_data(echo2), "two", 3) == 0);

    rt_connpool_release(pool, conn2);
    rt_connpool_clear(pool);
    server_thread.join();

    test_result("Server finished after pooled connection close", server_done.load());
}

static void test_connection_pool_tracks_fresh_acquire() {
    printf("\nTesting ConnectionPool fresh acquire tracking:\n");

    const int port = (int)rt_netutils_get_free_port();
    if (port <= 0) {
        printf("  SKIPPED: could not allocate local port\n");
        return;
    }

    server_ready = false;
    server_done = false;

    std::thread server_thread(echo_server_thread, port, 1);
    while (!server_ready)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    void *pool = rt_connpool_new(4);
    test_result("Fresh pool starts empty",
                rt_connpool_size(pool) == 0 && rt_connpool_available(pool) == 0);

    void *conn = rt_connpool_acquire(pool, rt_const_cstr("127.0.0.1"), port);
    test_result("Tracked acquire returns connection", conn != nullptr);
    test_result("Fresh acquire increments pool size immediately", rt_connpool_size(pool) == 1);
    test_result("Checked-out connection is not available", rt_connpool_available(pool) == 0);

    rt_connpool_release(pool, conn);
    test_result("Release keeps tracked connection in pool", rt_connpool_size(pool) == 1);
    test_result("Release makes tracked connection available", rt_connpool_available(pool) == 1);

    rt_connpool_clear(pool);
    server_thread.join();
    test_result("Server finished after tracked acquire cleanup", server_done.load());
}

static void test_connection_pool_clamps_max_size_and_closes_overflow() {
    printf("\nTesting ConnectionPool max-size clamp and overflow handling:\n");

    const int port = (int)rt_netutils_get_free_port();
    if (port <= 0) {
        printf("  SKIPPED: could not allocate local port\n");
        return;
    }

    server_ready = false;
    server_done = false;

    std::thread server_thread(echo_server_thread, port, 2);
    while (!server_ready)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    void *pool = rt_connpool_new(0);
    void *conn1 = rt_connpool_acquire(pool, rt_const_cstr("127.0.0.1"), port);
    void *conn2 = rt_connpool_acquire(pool, rt_const_cstr("127.0.0.1"), port);

    test_result("Clamped pool still returns first connection", conn1 != nullptr);
    test_result("Overflow acquire still returns second connection", conn2 != nullptr);
    test_result("Zero maxSize clamps to a single tracked slot", rt_connpool_size(pool) == 1);
    test_result("Tracked slot stays unavailable while checked out",
                rt_connpool_available(pool) == 0);

    rt_connpool_release(pool, conn2);
    test_result("Overflow release does not grow the tracked pool", rt_connpool_size(pool) == 1);
    test_result("Overflow release does not create an idle entry", rt_connpool_available(pool) == 0);

    rt_connpool_release(pool, conn1);
    test_result("Tracked connection becomes available after release",
                rt_connpool_available(pool) == 1);

    rt_connpool_clear(pool);
    server_thread.join();
    test_result("Server finished after overflow cleanup", server_done.load());
}

/// @brief One-shot echo helper for the stale-bytes probe: echoes a single
///        payload and closes its side immediately, so the pool's later close
///        of the client socket (which still has unread bytes and therefore
///        RSTs) has no live peer recv to break.
static void oneshot_echo_close_server_thread(int port) {
    void *server = rt_tcp_server_listen(port);
    assert(server != nullptr);
    server_ready = true;

    void *client = rt_tcp_server_accept(server);
    if (client) {
        void *data = rt_tcp_recv(client, 1024);
        if (get_bytes_len(data) > 0)
            rt_tcp_send_all(client, data);
        rt_tcp_close(client);
    }
    rt_tcp_server_close(server);
    server_done = true;
}

/// @brief Ownership and reuse-hygiene contract (VDOC-145): pooled entries hold
///        their own reference, Clear never closes checked-out handles, and a
///        connection with unread bytes is not re-pooled.
static void test_connection_pool_ownership_and_hygiene() {
    printf("\nTesting ConnectionPool ownership and reuse hygiene:\n");

    const int port = (int)rt_netutils_get_free_port();
    if (port <= 0) {
        printf("  SKIPPED: could not allocate local port\n");
        return;
    }

    server_ready = false;
    server_done = false;
    std::thread pending_server(oneshot_echo_close_server_thread, port);
    while (!server_ready)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    void *pool = rt_connpool_new(4);

    // Part 1: releasing a connection with unread (stale) bytes must close it
    // rather than re-pool it for the next caller.
    void *conn = rt_connpool_acquire(pool, rt_const_cstr("127.0.0.1"), port);
    void *msg = make_bytes_str("four");
    rt_tcp_send_all(conn, msg);
    void *partial = rt_tcp_recv_exact(conn, 2); // leave 2 echoed bytes queued
    test_result("Partial read succeeded", get_bytes_len(partial) == 2);
    pending_server.join(); // server has echoed and closed; tail bytes are queued
    rt_connpool_release(pool, conn);
    test_result("Connection with pending bytes is not re-pooled",
                rt_connpool_available(pool) == 0 && rt_connpool_size(pool) == 0);
    if (rt_obj_release_check0(conn))
        rt_obj_free(conn);

    // Parts 2/3 use a fresh echo server on a fresh port.
    const int port2 = (int)rt_netutils_get_free_port();
    if (port2 <= 0) {
        printf("  SKIPPED: could not allocate second local port\n");
        return;
    }
    server_ready = false;
    server_done = false;
    std::thread server_thread(echo_server_thread, port2, 1);
    while (!server_ready)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Part 2: a pooled entry must survive the caller dropping its reference.
    void *conn2 = rt_connpool_acquire(pool, rt_const_cstr("127.0.0.1"), port2);
    void *msg2 = make_bytes_str("abc");
    rt_tcp_send_all(conn2, msg2);
    void *echo2 = rt_tcp_recv_exact(conn2, 3);
    test_result("Round-trip before pooling succeeds", memcmp(get_bytes_data(echo2), "abc", 3) == 0);
    rt_connpool_release(pool, conn2);
    if (rt_obj_release_check0(conn2)) // caller drops its reference (simulated GC)
        rt_obj_free(conn2);
    void *conn3 = rt_connpool_acquire(pool, rt_const_cstr("127.0.0.1"), port2);
    test_result("Pooled entry survives caller dropping its reference", conn3 != nullptr);
    void *msg3 = make_bytes_str("xyz");
    rt_tcp_send_all(conn3, msg3);
    void *echo3 = rt_tcp_recv_exact(conn3, 3);
    test_result("Reused connection still round-trips",
                memcmp(get_bytes_data(echo3), "xyz", 3) == 0);

    // Part 3: Clear must not close a checked-out handle.
    rt_connpool_clear(pool);
    test_result("Clear empties the tracked pool", rt_connpool_size(pool) == 0);
    test_result("Checked-out handle stays open across Clear", rt_tcp_is_open(conn3) == 1);
    void *msg4 = make_bytes_str("end");
    rt_tcp_send_all(conn3, msg4);
    void *echo4 = rt_tcp_recv_exact(conn3, 3);
    test_result("Checked-out handle still round-trips after Clear",
                memcmp(get_bytes_data(echo4), "end", 3) == 0);
    rt_tcp_close(conn3);
    if (rt_obj_release_check0(conn3))
        rt_obj_free(conn3);

    server_thread.join();
    test_result("Server finished after hygiene checks", server_done.load());
}

/// @brief Verify exclusive TCP leasing prevents one socket from entering two pools.
/// @details A connection checked out from pool A is intentionally released to
///          pool B. The second pool must trap without closing, adopting, or
///          mutating the connection. A live echo round-trip then proves pool
///          A's borrower remains usable before it is returned normally.
static void test_connection_pool_rejects_cross_pool_aliasing() {
    printf("\nTesting ConnectionPool exclusive cross-pool leasing:\n");

    const int port = (int)rt_netutils_get_free_port();
    if (port <= 0) {
        printf("  SKIPPED: could not allocate local port\n");
        return;
    }

    server_ready = false;
    server_done = false;
    std::thread server_thread(echo_server_thread, port, 1);
    while (!server_ready)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    rt_string host = rt_const_cstr("127.0.0.1");
    void *pool_a = rt_connpool_new(2);
    void *pool_b = rt_connpool_new(2);
    void *conn = rt_connpool_acquire(pool_a, host, port);
    test_result("Pool A acquired a live connection", conn != nullptr);

    bool saw_foreign_pool_trap = false;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        rt_connpool_release(pool_b, conn);
        rt_trap_clear_recovery();
    } else {
        const char *error = rt_trap_get_error();
        saw_foreign_pool_trap = error && strstr(error, "TCP belongs to another pool") != nullptr;
        rt_trap_clear_recovery();
    }

    test_result("Foreign-pool release traps deterministically", saw_foreign_pool_trap);
    test_result("Foreign pool did not adopt the connection", rt_connpool_size(pool_b) == 0);
    test_result("Original pool still tracks the checked-out connection",
                rt_connpool_size(pool_a) == 1 && rt_connpool_available(pool_a) == 0);
    test_result("Rejected foreign release did not close the transport", rt_tcp_is_open(conn) == 1);

    void *not_bytes = rt_seq_new();
    bool saw_invalid_bytes_trap = false;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        (void)rt_tcp_send(conn, not_bytes);
        rt_trap_clear_recovery();
    } else {
        const char *error = rt_trap_get_error();
        saw_invalid_bytes_trap = error && strstr(error, "invalid Bytes data") != nullptr;
        rt_trap_clear_recovery();
    }
    release_test_object(not_bytes);
    test_result("TCP send rejects a non-Bytes managed payload", saw_invalid_bytes_trap);

    bool saw_negative_length_trap = false;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        rt_tcp_send_all_raw(conn, nullptr, -1);
        rt_trap_clear_recovery();
    } else {
        const char *error = rt_trap_get_error();
        saw_negative_length_trap = error && strstr(error, "invalid data length") != nullptr;
        rt_trap_clear_recovery();
    }
    test_result("TCP raw send rejects a negative length", saw_negative_length_trap);

    bool saw_negative_receive_trap = false;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        (void)rt_tcp_recv(conn, -1);
        rt_trap_clear_recovery();
    } else {
        const char *error = rt_trap_get_error();
        saw_negative_receive_trap = error && strstr(error, "invalid receive size") != nullptr;
        rt_trap_clear_recovery();
    }
    test_result("TCP receive rejects a negative length", saw_negative_receive_trap);

    void *payload = make_bytes_str("lease");
    rt_tcp_send_all(conn, payload);
    void *echo = rt_tcp_recv_exact(conn, 5);
    test_result("Connection remains usable after rejected alias",
                echo && memcmp(get_bytes_data(echo), "lease", 5) == 0);
    release_test_object(echo);
    release_test_object(payload);

    rt_connpool_release(pool_a, conn);
    test_result("Owner pool accepts the normal release", rt_connpool_available(pool_a) == 1);
    release_test_object(conn); // caller reference; pool A retains the idle entry
    rt_connpool_clear(pool_a);
    release_test_object(pool_a);
    release_test_object(pool_b);
    rt_string_unref(host);

    server_thread.join();
    test_result("Server finished after exclusive-lease cleanup", server_done.load());
}

/// @brief Echo bytes on one accepted connection until the pool closes it.
/// @details The helper owns the accepted TCP reference and every received Bytes
///          value. It is intentionally independent per client so the pool
///          stress test can exercise simultaneous sockets without server-side
///          head-of-line blocking.
/// @param client Owned accepted TCP handle.
static void concurrent_pool_echo_client(void *client) {
    assert(client != nullptr);
    while (rt_tcp_is_open(client)) {
        void *data = rt_tcp_recv(client, 1024);
        if (!data)
            break;
        int64_t len = get_bytes_len(data);
        if (len == 0) {
            release_test_object(data);
            break;
        }
        rt_tcp_send_all(client, data);
        release_test_object(data);
    }
    rt_tcp_close(client);
    release_test_object(client);
}

/// @brief Accept and service a fixed number of concurrent pool connections.
/// @details Each accepted handle is transferred to its own worker thread. The
///          listener remains owned by this function and is finalized only after
///          every client observes the pool's deterministic Clear/close.
/// @param port Loopback port chosen by the test.
/// @param client_count Exact number of simultaneous clients to accept.
static void concurrent_pool_echo_server(int port, int client_count) {
    void *server = rt_tcp_server_listen(port);
    assert(server != nullptr);
    server_ready = true;

    std::vector<std::thread> clients;
    clients.reserve((size_t)client_count);
    for (int i = 0; i < client_count; i++) {
        void *client = rt_tcp_server_accept(server);
        assert(client != nullptr);
        clients.emplace_back(concurrent_pool_echo_client, client);
    }
    for (std::thread &client : clients)
        client.join();

    rt_tcp_server_close(server);
    release_test_object(server);
    server_done = true;
}

/// @brief Run one simultaneous ConnectionPool borrower through repeated I/O.
/// @details All borrowers hold their first acquisition until every peer has
///          acquired, guaranteeing the pool tracks the configured number of
///          distinct in-use sockets. Each iteration validates an exact echoed
///          byte, then the connection is released and its caller reference is
///          dropped while the pool keeps the idle reference.
/// @param pool Shared ConnectionPool handle retained by the test.
/// @param host Shared immutable loopback runtime string.
/// @param port Echo-server port.
/// @param worker_index Unique byte source for this worker.
/// @param worker_count Number of simultaneous borrowers.
/// @param rounds Number of echo round-trips to perform.
/// @param start Barrier flag published by the parent.
/// @param acquired Shared count of completed initial acquisitions.
/// @param failed Shared failure flag that releases barrier waits on error.
static void concurrent_pool_worker(void *pool,
                                   rt_string host,
                                   int port,
                                   int worker_index,
                                   int worker_count,
                                   int rounds,
                                   std::atomic<bool> &start,
                                   std::atomic<int> &acquired,
                                   std::atomic<bool> &failed) {
    while (!start.load(std::memory_order_acquire))
        std::this_thread::yield();

    void *conn = rt_connpool_acquire(pool, host, port);
    if (!conn) {
        failed.store(true, std::memory_order_release);
        return;
    }
    acquired.fetch_add(1, std::memory_order_acq_rel);
    while (acquired.load(std::memory_order_acquire) < worker_count &&
           !failed.load(std::memory_order_acquire))
        std::this_thread::yield();

    const uint8_t payload = (uint8_t)(worker_index + 1);
    for (int round = 0; round < rounds && !failed.load(std::memory_order_acquire); round++) {
        rt_tcp_send_all_raw(conn, &payload, 1);
        void *echo = rt_tcp_recv_exact(conn, 1);
        if (!echo || get_bytes_len(echo) != 1 || get_bytes_data(echo)[0] != payload)
            failed.store(true, std::memory_order_release);
        release_test_object(echo);
    }

    rt_connpool_release(pool, conn);
    release_test_object(conn);
}

/// @brief Stress ConnectionPool bookkeeping with simultaneous acquisition and release.
/// @details Eight borrowers force eight distinct tracked connections, perform
///          repeated I/O in parallel, and return them concurrently. The final
///          size/availability snapshot verifies no lost entry, duplicate slot,
///          or capacity overrun occurred before Clear closes every idle socket.
static void test_connection_pool_concurrent_bookkeeping() {
    printf("\nTesting ConnectionPool concurrent bookkeeping and I/O:\n");
    constexpr int kWorkers = 8;
    constexpr int kRounds = 25;

    const int port = (int)rt_netutils_get_free_port();
    if (port <= 0) {
        printf("  SKIPPED: could not allocate local port\n");
        return;
    }

    server_ready = false;
    server_done = false;
    std::thread server_thread(concurrent_pool_echo_server, port, kWorkers);
    while (!server_ready)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    rt_string host = rt_const_cstr("127.0.0.1");
    void *pool = rt_connpool_new(kWorkers);
    std::atomic<bool> start{false};
    std::atomic<int> acquired{0};
    std::atomic<bool> failed{false};
    std::vector<std::thread> workers;
    workers.reserve(kWorkers);
    for (int i = 0; i < kWorkers; i++) {
        workers.emplace_back(concurrent_pool_worker,
                             pool,
                             host,
                             port,
                             i,
                             kWorkers,
                             kRounds,
                             std::ref(start),
                             std::ref(acquired),
                             std::ref(failed));
    }
    start.store(true, std::memory_order_release);
    for (std::thread &worker : workers)
        worker.join();

    test_result("All concurrent borrowers completed exact echo I/O", !failed.load());
    test_result("Concurrent acquires respected tracked capacity",
                rt_connpool_size(pool) == kWorkers);
    test_result("Concurrent releases published every entry as idle",
                rt_connpool_available(pool) == kWorkers);

    rt_connpool_clear(pool);
    server_thread.join();
    test_result("Clear closed all concurrently returned sockets", server_done.load());
    release_test_object(pool);
    rt_string_unref(host);
}

/// @brief IsPortOpen probe contract (VDOC-147): validated host bytes, honest
///        SO_ERROR handling, and one overall deadline across candidates.
static void test_netutils_is_port_open_contract() {
    printf("\nTesting NetUtils.IsPortOpen contract:\n");

    const int port = (int)rt_netutils_get_free_port();
    if (port <= 0) {
        printf("  SKIPPED: could not allocate local port\n");
        return;
    }

    // Open port reports true.
    void *server = rt_tcp_server_listen(port);
    test_result("Local listener created", server != nullptr);
    test_result("IsPortOpen true for listening port",
                rt_netutils_is_port_open(rt_const_cstr("127.0.0.1"), port, 2000) == 1);
    rt_tcp_server_close(server);

    // Closed port reports false.
    const int closed_port = (int)rt_netutils_get_free_port();
    if (closed_port > 0) {
        test_result("IsPortOpen false for closed port",
                    rt_netutils_is_port_open(rt_const_cstr("127.0.0.1"), closed_port, 2000) == 0);
    }

    // Embedded NUL must reject the whole host, not probe the prefix.
    rt_string nul_host = rt_string_from_bytes("127.0.0.1\0evil", 14);
    test_result("Embedded-NUL host is rejected",
                rt_netutils_is_port_open(nul_host, port, 500) == 0);
    rt_string_unref(nul_host);

    // The timeout is a single overall deadline, not a per-candidate budget.
    auto start = std::chrono::steady_clock::now();
    rt_netutils_is_port_open(rt_const_cstr("192.0.2.1"), 81, 400);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();
    test_result("Probe honors its overall deadline", elapsed < 3000);
}

/// @brief AsyncSocket lifetime and typing contract (VDOC-157/158): Future
///        consumption owns the only remaining reference, and SendAsync
///        resolves to a boxed Integer, not a pointer-cast count.
static void test_async_socket_reference_transfer_and_boxed_send() {
    printf("\nTesting AsyncSocket reference transfer and boxed send:\n");

    const int port = (int)rt_netutils_get_free_port();
    if (port <= 0) {
        printf("  SKIPPED: could not allocate local port\n");
        return;
    }

    server_ready = false;
    server_done = false;
    std::thread server_thread(echo_server_thread, port, 1);
    while (!server_ready)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // ConnectAsync: the worker's producer reference must transfer to the
    // Future; after consuming and dropping the Future, the caller holds the
    // only reference.
    void *cf = rt_async_connect_for(rt_const_cstr("127.0.0.1"), port, 3000);
    rt_future_wait(cf);
    void *tcp = rt_future_get(cf);
    test_result("ConnectAsync resolves to a connection", tcp != nullptr);
    if (rt_obj_release_check0(cf))
        rt_obj_free(cf);
    test_result("Connect result has exactly one owner after producer cleanup",
                wait_for_refcount(tcp, 1));

    // SendAsync resolves to a boxed Integer with the byte count.
    void *payload = make_bytes_str("ping");
    void *sf = rt_async_send(tcp, payload);
    rt_future_wait(sf);
    void *sent_box = rt_future_get(sf);
    test_result("SendAsync result is a boxed Integer",
                sent_box != nullptr && rt_box_type(sent_box) == 0 /* RT_BOX_I64 */);
    test_result("SendAsync boxed count matches payload size", rt_unbox_i64(sent_box) == 4);
    if (rt_obj_release_check0(sf))
        rt_obj_free(sf);

    // RecvAsync: the echoed Bytes are solely caller-owned after consumption.
    void *rf = rt_async_recv(tcp, 16);
    rt_future_wait(rf);
    void *echoed = rt_future_get(rf);
    test_result("RecvAsync resolves to the echoed bytes",
                echoed != nullptr && get_bytes_len(echoed) == 4);
    if (rt_obj_release_check0(rf))
        rt_obj_free(rf);
    test_result("Recv result has exactly one owner after producer cleanup",
                wait_for_refcount(echoed, 1));

    rt_tcp_close(tcp);
    if (rt_obj_release_check0(tcp))
        rt_obj_free(tcp);
    server_thread.join();
}

/// @brief Line server thread function - sends lines
static void line_server_thread(int port) {
    void *server = rt_tcp_server_listen(port);
    assert(server != nullptr);

    server_ready = true;

    void *client = rt_tcp_server_accept(server);
    if (client) {
        // Send lines
        const char *lines[] = {"Line 1\n", "Line 2 with CRLF\r\n", "Last line\n"};

        for (int i = 0; i < 3; i++) {
            rt_string line = rt_const_cstr(lines[i]);
            rt_tcp_send_str(client, line);
        }

        rt_tcp_close(client);
    }

    rt_tcp_server_close(server);
    server_done = true;
}

/// @brief Test RecvLine
static void test_recv_line() {
    printf("\nTesting RecvLine:\n");

    const int port = get_free_tcp_port_ipv4();
    assert(port > 0);
    server_ready = false;
    server_done = false;

    std::thread server_thread(line_server_thread, port);

    while (!server_ready) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    rt_string host = rt_const_cstr("127.0.0.1");
    void *client = rt_tcp_connect(host, port);
    assert(client != nullptr);

    // Read line 1 (LF ending)
    rt_string line1 = rt_tcp_recv_line(client);
    test_result("RecvLine reads LF line", strcmp(rt_string_cstr(line1), "Line 1") == 0);

    // Read line 2 (CRLF ending)
    rt_string line2 = rt_tcp_recv_line(client);
    test_result("RecvLine strips CRLF", strcmp(rt_string_cstr(line2), "Line 2 with CRLF") == 0);

    // Read line 3
    rt_string line3 = rt_tcp_recv_line(client);
    test_result("RecvLine reads last line", strcmp(rt_string_cstr(line3), "Last line") == 0);

    rt_tcp_close(client);
    server_thread.join();
}

/// @brief Test server properties
static void test_server_properties() {
    printf("\nTesting Server Properties:\n");

    const int port = get_free_tcp_port_ipv4();
    assert(port > 0);

    void *server = rt_tcp_server_listen(port);
    assert(server != nullptr);

    test_result("TcpServer has its stable managed class identity",
                rt_obj_class_id(server) == RT_TCP_SERVER_CLASS_ID);
    test_result("Server port is correct", rt_tcp_server_port(server) == port);
    test_result("Server is listening", rt_tcp_server_is_listening(server) == 1);

    rt_string addr = rt_tcp_server_address(server);
    // Accept either IPv4 wildcard "0.0.0.0" or IPv6 wildcard "::" — getaddrinfo
    // with AF_UNSPEC may prefer either depending on the OS (macOS prefers IPv6).
    const char *addr_cstr = rt_string_cstr(addr);
    test_result("Server address is wildcard",
                strcmp(addr_cstr, "0.0.0.0") == 0 || strcmp(addr_cstr, "::") == 0);

    rt_tcp_server_close(server);

    test_result("Server not listening after close", rt_tcp_server_is_listening(server) == 0);
}

/// @brief Test ephemeral TcpServer bind with port 0.
static void test_server_ephemeral_port() {
    printf("\nTesting Server Ephemeral Port Binding:\n");

    void *server = rt_tcp_server_listen(0);
    assert(server != nullptr);

    const int64_t bound_port = rt_tcp_server_port(server);
    test_result("Server assigns an ephemeral port", bound_port > 0 && bound_port <= 65535);
    test_result("Ephemeral server is listening", rt_tcp_server_is_listening(server) == 1);

    rt_tcp_server_close(server);
}

/// @brief Test client properties
static void test_client_properties() {
    printf("\nTesting Client Properties:\n");

    const int port = get_free_tcp_port_ipv4();
    assert(port > 0);
    server_ready = false;
    server_done = false;

    std::thread server_thread(echo_server_thread, port, 1);

    while (!server_ready) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    rt_string host = rt_const_cstr("127.0.0.1");
    void *client = rt_tcp_connect(host, port);
    assert(client != nullptr);

    rt_string client_host = rt_tcp_host(client);
    test_result("Client host is 127.0.0.1", strcmp(rt_string_cstr(client_host), "127.0.0.1") == 0);
    test_result("Client remote port is correct", rt_tcp_port(client) == port);
    test_result("Client local port is > 0", rt_tcp_local_port(client) > 0);
    test_result("Client is open", rt_tcp_is_open(client) == 1);
    test_result("Available returns 0 initially", rt_tcp_available(client) == 0);

    rt_tcp_close(client);

    test_result("Client not open after close", rt_tcp_is_open(client) == 0);

    server_thread.join();
}

/// @brief Test accept with timeout
static void test_accept_timeout() {
    printf("\nTesting Accept Timeout:\n");

    const int port = get_free_tcp_port_ipv4();
    assert(port > 0);

    void *server = rt_tcp_server_listen(port);
    assert(server != nullptr);

    // Accept with short timeout - no client connecting
    auto start = std::chrono::steady_clock::now();
    void *client = rt_tcp_server_accept_for(server, 100); // 100ms timeout
    auto end = std::chrono::steady_clock::now();

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    test_result("Accept returns NULL on timeout", client == nullptr);
    test_result("Accept timeout is respected",
                elapsed >= kNetworkTimeoutLowerBoundMs && elapsed < kNetworkTimeoutUpperBoundMs);

    rt_tcp_server_close(server);
}

/// @brief Run one blocking accept under a thread-local recovery boundary.
/// @details Workers start together and remain blocked until the parent closes
///          the listener. Expected close races return NULL without trapping;
///          any unexpected accepted transport is closed and released so the
///          test cannot leak it.
/// @param server Shared TcpServer kept alive by the parent test reference.
/// @param go Start barrier released after every worker exists.
/// @param ready Count of workers waiting at the start barrier.
/// @param completed Count of accept calls that returned or trapped.
/// @param trapped Count of unexpected accept traps.
/// @param accepted Count of unexpected accepted client transports.
static void tcp_blocked_accept_worker(void *server,
                                      std::atomic<bool> &go,
                                      std::atomic<int> &ready,
                                      std::atomic<int> &completed,
                                      std::atomic<int> &trapped,
                                      std::atomic<int> &accepted) {
    ready.fetch_add(1, std::memory_order_acq_rel);
    while (!go.load(std::memory_order_acquire))
        std::this_thread::yield();

    void *client = nullptr;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        client = rt_tcp_server_accept(server);
        rt_trap_clear_recovery();
    } else {
        trapped.fetch_add(1, std::memory_order_acq_rel);
        rt_trap_clear_recovery();
    }
    if (client) {
        accepted.fetch_add(1, std::memory_order_acq_rel);
        rt_tcp_close(client);
        release_test_object(client);
    }
    completed.fetch_add(1, std::memory_order_acq_rel);
}

/// @brief Verify concurrent Close drains blocked accepts before descriptor close.
/// @details Eight indefinite accepts enter the listener concurrently. Close
///          clears publication, waits for each bounded non-blocking accept loop,
///          and then closes the descriptor. Every worker must return NULL
///          without a trap, and Close must complete within a generous bound.
static void test_tcp_server_concurrent_close_accept() {
    printf("\nTesting TcpServer concurrent Close/Accept:\n");
    constexpr int kAcceptors = 8;

    void *server = rt_tcp_server_listen(0);
    assert(server != nullptr);
    std::atomic<bool> go{false};
    std::atomic<int> ready{0};
    std::atomic<int> completed{0};
    std::atomic<int> trapped{0};
    std::atomic<int> accepted{0};
    std::vector<std::thread> workers;
    workers.reserve(kAcceptors);
    for (int i = 0; i < kAcceptors; ++i) {
        workers.emplace_back(tcp_blocked_accept_worker,
                             server,
                             std::ref(go),
                             std::ref(ready),
                             std::ref(completed),
                             std::ref(trapped),
                             std::ref(accepted));
    }
    while (ready.load(std::memory_order_acquire) != kAcceptors)
        std::this_thread::yield();
    go.store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto close_start = std::chrono::steady_clock::now();
    rt_tcp_server_close(server);
    auto close_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - close_start)
                             .count();
    for (std::thread &worker : workers)
        worker.join();

    test_result("Close drains every blocked accept", completed.load() == kAcceptors);
    test_result("Concurrent listener close produces no accept trap", trapped.load() == 0);
    test_result("Closed listener accepts no unintended transport", accepted.load() == 0);
    test_result("Close waits without hanging", close_elapsed < 3000);
    test_result("Closed listener publishes a stable stopped state",
                rt_tcp_server_is_listening(server) == 0);
    release_test_object(server);
}

/// @brief Verify native sockets are reclaimed when managed transport allocation traps.
/// @details Exercises three ownership-transfer points: listener publication,
///          outbound Tcp publication after connect, and inbound Tcp publication
///          after accept. Rebinding proves failed listener cleanup; peer EOF
///          proves both failed connection publications closed their established
///          native sockets. The listener remains reusable after failed accept.
static void test_tcp_transport_constructor_trap_cleanup() {
    printf("\nTesting TCP transport constructor allocation-trap cleanup:\n");
    jmp_buf recovery;

    const int rebind_port = get_free_tcp_port_ipv4();
    assert(rebind_port > 0);
    bool listener_alloc_trapped = false;
    tcp_alloc_fail_countdown = 1;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        rt_set_alloc_hook(tcp_fail_countdown_alloc);
        (void)rt_tcp_server_listen(rebind_port);
        rt_set_alloc_hook(nullptr);
        rt_trap_clear_recovery();
    } else {
        rt_set_alloc_hook(nullptr);
        const char *error = rt_trap_get_error();
        listener_alloc_trapped = error && strstr(error, "allocation failed") != nullptr;
        rt_trap_clear_recovery();
    }
    void *rebound = rt_tcp_server_listen(rebind_port);
    test_result("Failed TcpServer publication closes its listener",
                listener_alloc_trapped && tcp_alloc_fail_countdown == 0 && rebound != nullptr);
    rt_tcp_server_close(rebound);
    release_test_object(rebound);

    void *server = rt_tcp_server_listen(0);
    assert(server != nullptr);
    int64_t port = rt_tcp_server_port(server);
    rt_string host = rt_const_cstr("127.0.0.1");

    bool connect_alloc_trapped = false;
    tcp_alloc_fail_countdown = 1;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        rt_set_alloc_hook(tcp_fail_countdown_alloc);
        (void)rt_tcp_connect(host, port);
        rt_set_alloc_hook(nullptr);
        rt_trap_clear_recovery();
    } else {
        rt_set_alloc_hook(nullptr);
        const char *error = rt_trap_get_error();
        connect_alloc_trapped = error && strstr(error, "allocation failed") != nullptr;
        rt_trap_clear_recovery();
    }
    void *failed_connect_peer = rt_tcp_server_accept_for(server, 2000);
    assert(failed_connect_peer != nullptr);
    rt_tcp_set_recv_timeout(failed_connect_peer, 1000);
    void *connect_eof = rt_tcp_recv(failed_connect_peer, 1);
    test_result("Failed outbound Tcp publication closes the connected socket",
                connect_alloc_trapped && tcp_alloc_fail_countdown == 0 && connect_eof &&
                    get_bytes_len(connect_eof) == 0 && rt_tcp_is_open(failed_connect_peer) == 0);
    release_test_object(connect_eof);
    release_test_object(failed_connect_peer);

    void *accept_client = rt_tcp_connect(host, port);
    assert(accept_client != nullptr);
    bool accept_alloc_trapped = false;
    tcp_alloc_fail_countdown = 1;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        rt_set_alloc_hook(tcp_fail_countdown_alloc);
        (void)rt_tcp_server_accept_for(server, 2000);
        rt_set_alloc_hook(nullptr);
        rt_trap_clear_recovery();
    } else {
        rt_set_alloc_hook(nullptr);
        const char *error = rt_trap_get_error();
        accept_alloc_trapped = error && strstr(error, "allocation failed") != nullptr;
        rt_trap_clear_recovery();
    }
    rt_tcp_set_recv_timeout(accept_client, 1000);
    void *accept_eof = rt_tcp_recv(accept_client, 1);
    test_result("Failed inbound Tcp publication closes the accepted socket",
                accept_alloc_trapped && tcp_alloc_fail_countdown == 0 && accept_eof &&
                    get_bytes_len(accept_eof) == 0 && rt_tcp_is_open(accept_client) == 0);
    release_test_object(accept_eof);
    release_test_object(accept_client);

    void *reuse_client = rt_tcp_connect(host, port);
    void *reuse_peer = rt_tcp_server_accept_for(server, 2000);
    test_result("Listener remains reusable after failed accepted-Tcp publication",
                reuse_client != nullptr && reuse_peer != nullptr);
    rt_tcp_close(reuse_client);
    rt_tcp_close(reuse_peer);
    release_test_object(reuse_client);
    release_test_object(reuse_peer);
    rt_tcp_server_close(server);
    release_test_object(server);
    rt_string_unref(host);
}

/// @brief Test connect with timeout - just test that ConnectFor compiles and works
/// Note: Testing actual timeout with non-routable addresses would trap and terminate
static void test_connect_with_timeout() {
    printf("\nTesting ConnectFor:\n");

    const int port = get_free_tcp_port_ipv4();
    assert(port > 0);
    server_ready = false;
    server_done = false;

    std::thread server_thread(echo_server_thread, port, 1);

    while (!server_ready) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Connect with a long timeout (should succeed quickly)
    rt_string host = rt_const_cstr("127.0.0.1");
    void *client = rt_tcp_connect_for(host, port, 5000); // 5 second timeout

    test_result("ConnectFor succeeds to localhost", client != nullptr);
    test_result("ConnectFor client is open", rt_tcp_is_open(client) == 1);

    rt_tcp_close(client);
    server_thread.join();
}

/// @brief Test ListenAt on specific address
static void test_listen_at() {
    printf("\nTesting ListenAt:\n");

    const int port = get_free_tcp_port_ipv4();
    assert(port > 0);
    rt_string addr = rt_const_cstr("127.0.0.1");

    void *server = rt_tcp_server_listen_at(addr, port);
    assert(server != nullptr);

    rt_string bound_addr = rt_tcp_server_address(server);
    test_result("Server bound to 127.0.0.1", strcmp(rt_string_cstr(bound_addr), "127.0.0.1") == 0);
    test_result("Server port is correct", rt_tcp_server_port(server) == port);

    rt_tcp_server_close(server);
}

//=============================================================================
// UDP Tests
//=============================================================================

/// @brief Test UDP socket creation and properties
static void test_udp_new() {
    printf("\nTesting UDP New:\n");

    void *sock = rt_udp_new();
    test_result("UDP New returns socket", sock != nullptr);
    test_result("UDP has its stable managed class identity",
                rt_obj_class_id(sock) == RT_UDP_CLASS_ID);
    test_result("UDP port is 0 (unbound)", rt_udp_port(sock) == 0);
    test_result("UDP is not bound", rt_udp_is_bound(sock) == 0);

    rt_udp_close(sock);
}

/// @brief Verify UDP atomic-message, validation, and allocation ownership contracts.
/// @details Covers real zero-length datagram transmission, negative-size and
///          non-Bytes rejection, short-result allocation failure after a
///          datagram has been consumed, continued socket usability, and native
///          listener cleanup when bound-UDP managed publication traps.
static void test_udp_datagram_integrity_and_trap_cleanup() {
    printf("\nTesting UDP datagram integrity and trap cleanup:\n");
    const int receiver_port = get_free_udp_port_ipv4();
    const int sender_port = get_distinct_free_udp_port_ipv4(receiver_port);
    assert(receiver_port > 0 && sender_port > 0);

    rt_string host = rt_const_cstr("127.0.0.1");
    void *receiver = rt_udp_bind_at(host, receiver_port);
    void *sender = rt_udp_bind_at(host, sender_port);
    assert(receiver != nullptr && sender != nullptr);
    test_result("Bound UDP handles publish the stable class identity",
                rt_obj_class_id(receiver) == RT_UDP_CLASS_ID &&
                    rt_obj_class_id(sender) == RT_UDP_CLASS_ID);

    void *empty = rt_bytes_new(0);
    test_result("SendTo transmits an atomic zero-length datagram",
                rt_udp_send_to(sender, host, receiver_port, empty) == 0);
    void *empty_received = rt_udp_recv_for(receiver, 1, 1000);
    test_result("Zero-length datagram is distinguishable from RecvFor timeout",
                empty_received != nullptr && get_bytes_len(empty_received) == 0 &&
                    rt_udp_sender_port(receiver) == sender_port);
    release_test_object(empty_received);
    release_test_object(empty);

    jmp_buf recovery;
    bool negative_size_trapped = false;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        (void)rt_udp_recv(receiver, -1);
        rt_trap_clear_recovery();
    } else {
        const char *error = rt_trap_get_error();
        negative_size_trapped = error && strstr(error, "invalid receive size") != nullptr;
        rt_trap_clear_recovery();
    }
    test_result("UDP receive rejects a negative size", negative_size_trapped);

    void *not_bytes = rt_seq_new();
    bool invalid_payload_trapped = false;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        (void)rt_udp_send_to(sender, host, receiver_port, not_bytes);
        rt_trap_clear_recovery();
    } else {
        const char *error = rt_trap_get_error();
        invalid_payload_trapped = error && strstr(error, "invalid Bytes data") != nullptr;
        rt_trap_clear_recovery();
    }
    release_test_object(not_bytes);
    test_result("UDP SendTo rejects a non-Bytes managed payload", invalid_payload_trapped);

    void *first = make_bytes_str("a");
    assert(rt_udp_send_to(sender, host, receiver_port, first) == 1);
    release_test_object(first);
    bool right_size_trapped = false;
    tcp_alloc_fail_countdown = 2;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        rt_set_alloc_hook(tcp_fail_countdown_alloc);
        (void)rt_udp_recv(receiver, 32);
        rt_set_alloc_hook(nullptr);
        rt_trap_clear_recovery();
    } else {
        rt_set_alloc_hook(nullptr);
        const char *error = rt_trap_get_error();
        right_size_trapped = error && strstr(error, "allocation failed") != nullptr;
        rt_trap_clear_recovery();
    }
    test_result("UDP short receive releases its oversized Bytes after allocation trap",
                right_size_trapped && tcp_alloc_fail_countdown == 0);

    void *second = make_bytes_str("z");
    assert(rt_udp_send_to(sender, host, receiver_port, second) == 1);
    release_test_object(second);
    void *continued = rt_udp_recv_for(receiver, 4, 1000);
    test_result("UDP remains usable after recovered result allocation trap",
                continued && get_bytes_len(continued) == 1 &&
                    get_bytes_data(continued)[0] == (uint8_t)'z');
    release_test_object(continued);

    rt_udp_close(sender);
    rt_udp_close(receiver);
    test_result("UDP Close clears bound state and port",
                rt_udp_is_bound(receiver) == 0 && rt_udp_port(receiver) == 0);
    release_test_object(sender);
    release_test_object(receiver);

    const int rebind_port = get_free_udp_port_ipv4();
    assert(rebind_port > 0);
    bool bind_alloc_trapped = false;
    tcp_alloc_fail_countdown = 1;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        rt_set_alloc_hook(tcp_fail_countdown_alloc);
        (void)rt_udp_bind_at(host, rebind_port);
        rt_set_alloc_hook(nullptr);
        rt_trap_clear_recovery();
    } else {
        rt_set_alloc_hook(nullptr);
        const char *error = rt_trap_get_error();
        bind_alloc_trapped = error && strstr(error, "allocation failed") != nullptr;
        rt_trap_clear_recovery();
    }
    void *rebound = rt_udp_bind_at(host, rebind_port);
    test_result("Failed bound-UDP publication closes its native socket",
                bind_alloc_trapped && tcp_alloc_fail_countdown == 0 && rebound != nullptr);
    rt_udp_close(rebound);
    release_test_object(rebound);
    rt_string_unref(host);
}

/// @brief Test UDP bind
static void test_udp_bind() {
    printf("\nTesting UDP Bind:\n");

    const int port = get_free_udp_port_ipv4();
    assert(port > 0);

    void *sock = rt_udp_bind(port);
    test_result("UDP Bind returns socket", sock != nullptr);
    test_result("UDP port is correct", rt_udp_port(sock) == port);
    test_result("UDP is bound", rt_udp_is_bound(sock) == 1);

    rt_string addr = rt_udp_address(sock);
    test_result("UDP address is wildcard",
                strcmp(rt_string_cstr(addr), "0.0.0.0") == 0 ||
                    strcmp(rt_string_cstr(addr), "::") == 0);

    rt_udp_close(sock);
}

/// @brief Test UDP bind at specific address
static void test_udp_bind_at() {
    printf("\nTesting UDP BindAt:\n");

    const int port = get_free_udp_port_ipv4();
    assert(port > 0);
    rt_string addr = rt_const_cstr("127.0.0.1");

    void *sock = rt_udp_bind_at(addr, port);
    test_result("UDP BindAt returns socket", sock != nullptr);
    test_result("UDP port is correct", rt_udp_port(sock) == port);
    test_result("UDP is bound", rt_udp_is_bound(sock) == 1);

    rt_string bound_addr = rt_udp_address(sock);
    test_result("UDP address is 127.0.0.1", strcmp(rt_string_cstr(bound_addr), "127.0.0.1") == 0);

    rt_udp_close(sock);
}

/// @brief Test UDP send and receive on localhost
static void test_udp_send_recv() {
    printf("\nTesting UDP Send/Recv:\n");

    const int recv_port = get_free_udp_port_ipv4();
    const int send_port = get_distinct_free_udp_port_ipv4(recv_port);
    assert(recv_port > 0);
    assert(send_port > 0);

    // Create receiver
    void *receiver = rt_udp_bind(recv_port);
    assert(receiver != nullptr);

    // Create sender
    void *sender = rt_udp_bind(send_port);
    assert(sender != nullptr);

    // Send data
    const char *test_msg = "Hello UDP!";
    void *send_data = make_bytes_str(test_msg);
    rt_string host = rt_const_cstr("127.0.0.1");

    int64_t sent = rt_udp_send_to(sender, host, recv_port, send_data);
    test_result("UDP SendTo returns byte count", sent == (int64_t)strlen(test_msg));

    // Receive data
    void *recv_data = rt_udp_recv(receiver, 1024);
    int64_t recv_len = get_bytes_len(recv_data);

    test_result("UDP Recv returns correct length", recv_len == (int64_t)strlen(test_msg));
    test_result("UDP Recv data matches",
                memcmp(get_bytes_data(recv_data), test_msg, strlen(test_msg)) == 0);

    rt_udp_close(sender);
    rt_udp_close(receiver);
}

/// @brief Test UDP send and receive string
static void test_udp_send_recv_str() {
    printf("\nTesting UDP SendToStr:\n");

    const int recv_port = get_free_udp_port_ipv4();
    const int send_port = get_distinct_free_udp_port_ipv4(recv_port);
    assert(recv_port > 0);
    assert(send_port > 0);

    void *receiver = rt_udp_bind(recv_port);
    void *sender = rt_udp_bind(send_port);
    assert(receiver != nullptr);
    assert(sender != nullptr);

    rt_string host = rt_const_cstr("127.0.0.1");
    rt_string msg = rt_const_cstr("Hello from string!");

    int64_t sent = rt_udp_send_to_str(sender, host, recv_port, msg);
    test_result("UDP SendToStr returns byte count", sent == 18);

    // Receive as bytes and verify
    void *recv_data = rt_udp_recv(receiver, 1024);
    test_result("UDP Recv receives string data",
                memcmp(get_bytes_data(recv_data), "Hello from string!", 18) == 0);

    rt_udp_close(sender);
    rt_udp_close(receiver);
}

/// @brief Test UDP RecvFrom with sender info
static void test_udp_recv_from() {
    printf("\nTesting UDP RecvFrom:\n");

    const int recv_port = get_free_udp_port_ipv4();
    const int send_port = get_distinct_free_udp_port_ipv4(recv_port);
    assert(recv_port > 0);
    assert(send_port > 0);

    void *receiver = rt_udp_bind(recv_port);
    void *sender = rt_udp_bind(send_port);
    assert(receiver != nullptr);
    assert(sender != nullptr);

    rt_string host = rt_const_cstr("127.0.0.1");
    void *data = make_bytes_str("test");

    rt_udp_send_to(sender, host, recv_port, data);

    // Receive with sender info
    void *recv_data = rt_udp_recv_from(receiver, 1024);
    test_result("UDP RecvFrom returns data", get_bytes_len(recv_data) == 4);

    // Check sender info
    rt_string sender_host = rt_udp_sender_host(receiver);
    int64_t sender_port = rt_udp_sender_port(receiver);

    test_result("UDP SenderHost is 127.0.0.1",
                strcmp(rt_string_cstr(sender_host), "127.0.0.1") == 0);
    test_result("UDP SenderPort is correct", sender_port == send_port);

    rt_udp_close(sender);
    rt_udp_close(receiver);
}

/// @brief Test UDP IPv6 bind, send, receive, and sender metadata.
static void test_udp_send_recv_ipv6() {
    printf("\nTesting UDP IPv6 Send/Recv:\n");

    if (!localhost_bind_available_ipv6()) {
        printf("  SKIPPED: IPv6 loopback unavailable in this environment\n");
        return;
    }

    const int recv_port = get_free_udp_port_ipv6();
    const int send_port = get_distinct_free_udp_port_ipv6(recv_port);
    if (recv_port <= 0 || send_port <= 0 || recv_port == send_port) {
        printf("  SKIPPED: could not allocate IPv6 loopback UDP ports\n");
        return;
    }

    void *receiver = rt_udp_bind_at(rt_const_cstr("::1"), recv_port);
    void *sender = rt_udp_bind_at(rt_const_cstr("::1"), send_port);

    test_result("UDP IPv6 receiver bound", receiver != nullptr);
    test_result("UDP IPv6 sender bound", sender != nullptr);

    const char *test_msg = "Hello UDP IPv6!";
    void *send_data = make_bytes_str(test_msg);
    int64_t sent = rt_udp_send_to(sender, rt_const_cstr("::1"), recv_port, send_data);
    test_result("UDP IPv6 SendTo returns byte count", sent == (int64_t)strlen(test_msg));

    void *recv_data = rt_udp_recv_from(receiver, 1024);
    int64_t recv_len = get_bytes_len(recv_data);
    test_result("UDP IPv6 RecvFrom returns correct length", recv_len == (int64_t)strlen(test_msg));
    test_result("UDP IPv6 payload matches",
                memcmp(get_bytes_data(recv_data), test_msg, strlen(test_msg)) == 0);

    rt_string sender_host = rt_udp_sender_host(receiver);
    test_result("UDP IPv6 SenderHost is ::1", strcmp(rt_string_cstr(sender_host), "::1") == 0);
    test_result("UDP IPv6 SenderPort is correct", rt_udp_sender_port(receiver) == send_port);

    rt_udp_close(sender);
    rt_udp_close(receiver);
}

/// @brief Test UDP receive timeout
static void test_udp_recv_timeout() {
    printf("\nTesting UDP RecvFor timeout:\n");

    const int port = get_free_udp_port_ipv4();
    assert(port > 0);
    void *sock = rt_udp_bind(port);
    assert(sock != nullptr);

    // Receive with short timeout - no data coming
    auto start = std::chrono::steady_clock::now();
    void *data = rt_udp_recv_for(sock, 1024, 100); // 100ms timeout
    auto end = std::chrono::steady_clock::now();

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    test_result("UDP RecvFor returns NULL on timeout", data == nullptr);
    test_result("UDP RecvFor timeout is respected",
                elapsed >= kNetworkTimeoutLowerBoundMs && elapsed < kNetworkTimeoutUpperBoundMs);

    rt_udp_close(sock);
}

/// @brief Test UDP broadcast enable
static void test_udp_broadcast() {
    printf("\nTesting UDP SetBroadcast:\n");

    void *sock = rt_udp_new();
    assert(sock != nullptr);

    // Enable broadcast - just test it doesn't crash
    rt_udp_set_broadcast(sock, 1);
    test_result("UDP SetBroadcast(true) succeeds", true);

    rt_udp_set_broadcast(sock, 0);
    test_result("UDP SetBroadcast(false) succeeds", true);

    rt_udp_close(sock);
}

/// @brief Test UDP set receive timeout
static void test_udp_set_recv_timeout() {
    printf("\nTesting UDP SetRecvTimeout:\n");

    const int port = get_free_udp_port_ipv4();
    assert(port > 0);
    void *sock = rt_udp_bind(port);
    assert(sock != nullptr);

    // Set recv timeout
    rt_udp_set_recv_timeout(sock, 50);

    // Try to receive with timeout
    auto start = std::chrono::steady_clock::now();
    void *data = rt_udp_recv(sock, 1024); // Should timeout
    auto end = std::chrono::steady_clock::now();

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    // recv with timeout set returns empty bytes (not NULL like RecvFor)
    test_result("UDP SetRecvTimeout recv returns", data != nullptr);
    test_result("UDP SetRecvTimeout returns empty on timeout", get_bytes_len(data) == 0);
    test_result("UDP SetRecvTimeout is respected",
                elapsed >= 40 && elapsed < kNetworkTimeoutUpperBoundMs);

    rt_udp_close(sock);
}

//=============================================================================
// DNS Tests
//=============================================================================

/// @brief Evaluate one DNS text predicate without leaking its managed input.
/// @details Runtime test literals are ordinary caller-owned Strings. This
///          helper gives parser tables concise syntax while ensuring each
///          temporary is released immediately after the allocation-free
///          predicate returns.
/// @param predicate DNS predicate under test.
/// @param text NUL-terminated candidate address.
/// @return Predicate result.
static int8_t run_dns_predicate(int8_t (*predicate)(rt_string), const char *text) {
    rt_string value = rt_const_cstr(text);
    int8_t result = predicate(value);
    rt_string_unref(value);
    return result;
}

/// @brief Validate that a DNS result sequence contains unique IP Strings.
/// @details Checks the managed type, public IP predicate, and pairwise byte
///          equality. Resolver and interface lists are intentionally small, so
///          the quadratic comparison keeps the test independent of Map/Set
///          behavior that is audited separately.
/// @param addresses Element-owning DNS Seq.
/// @return True when every entry is a valid IP and no two entries are equal.
static bool dns_addresses_are_unique_and_valid(void *addresses) {
    if (!addresses)
        return false;
    int64_t count = rt_seq_len(addresses);
    for (int64_t i = 0; i < count; ++i) {
        rt_string current = (rt_string)rt_seq_get(addresses, i);
        if (!current || !rt_string_is_handle(current) || rt_dns_is_ip(current) != 1)
            return false;
        const char *current_bytes = rt_string_cstr(current);
        for (int64_t j = 0; j < i; ++j) {
            rt_string earlier = (rt_string)rt_seq_get(addresses, j);
            if (strcmp(current_bytes, rt_string_cstr(earlier)) == 0)
                return false;
        }
    }
    return true;
}

/// @brief Test DNS resolve localhost
static void test_dns_resolve_localhost() {
    printf("\nTesting DNS Resolve localhost:\n");

    rt_string hostname = rt_const_cstr("localhost");
    rt_string result = rt_dns_resolve(hostname);

    const char *ip = rt_string_cstr(result);
    test_result("DNS Resolve localhost returns IP", ip != nullptr);
    test_result("DNS Resolve localhost is loopback",
                strcmp(ip, "127.0.0.1") == 0 || strcmp(ip, "::1") == 0);
    rt_string_unref(result);
    rt_string_unref(hostname);
}

/// @brief Test DNS resolve4 localhost
static void test_dns_resolve4_localhost() {
    printf("\nTesting DNS Resolve4 localhost:\n");

    rt_string hostname = rt_const_cstr("localhost");
    rt_string result = rt_dns_resolve4(hostname);

    const char *ip = rt_string_cstr(result);
    test_result("DNS Resolve4 localhost returns IP", ip != nullptr);
    test_result("DNS Resolve4 localhost is 127.0.0.1", strcmp(ip, "127.0.0.1") == 0);
    rt_string_unref(result);
    rt_string_unref(hostname);
}

/// @brief Test DNS IsIPv4
static void test_dns_is_ipv4() {
    printf("\nTesting DNS IsIPv4:\n");

    test_result("IsIPv4('127.0.0.1') = true", run_dns_predicate(rt_dns_is_ipv4, "127.0.0.1") == 1);
    test_result("IsIPv4('192.168.1.1') = true",
                run_dns_predicate(rt_dns_is_ipv4, "192.168.1.1") == 1);
    test_result("IsIPv4('0.0.0.0') = true", run_dns_predicate(rt_dns_is_ipv4, "0.0.0.0") == 1);
    test_result("IsIPv4('255.255.255.255') = true",
                run_dns_predicate(rt_dns_is_ipv4, "255.255.255.255") == 1);
    test_result("IsIPv4('256.0.0.1') = false", run_dns_predicate(rt_dns_is_ipv4, "256.0.0.1") == 0);
    test_result("IsIPv4('1.2.3') = false", run_dns_predicate(rt_dns_is_ipv4, "1.2.3") == 0);
    test_result("IsIPv4('01.2.3.4') = false", run_dns_predicate(rt_dns_is_ipv4, "01.2.3.4") == 0);
    test_result("IsIPv4 oversized octet text = false",
                run_dns_predicate(rt_dns_is_ipv4, "0000000000000000001.2.3.4") == 0);
    test_result("IsIPv4 trailing dot = false", run_dns_predicate(rt_dns_is_ipv4, "1.2.3.4.") == 0);
    test_result("IsIPv4('hello') = false", run_dns_predicate(rt_dns_is_ipv4, "hello") == 0);
    test_result("IsIPv4('') = false", run_dns_predicate(rt_dns_is_ipv4, "") == 0);
    test_result("IsIPv4('::1') = false", run_dns_predicate(rt_dns_is_ipv4, "::1") == 0);
}

/// @brief Test DNS IsIPv6
static void test_dns_is_ipv6() {
    printf("\nTesting DNS IsIPv6:\n");

    test_result("IsIPv6('::1') = true", run_dns_predicate(rt_dns_is_ipv6, "::1") == 1);
    test_result("IsIPv6('::') = true", run_dns_predicate(rt_dns_is_ipv6, "::") == 1);
    test_result("IsIPv6('2001:db8::1') = true",
                run_dns_predicate(rt_dns_is_ipv6, "2001:db8::1") == 1);
    test_result("IsIPv6('fe80::1') = true", run_dns_predicate(rt_dns_is_ipv6, "fe80::1") == 1);
    test_result("IsIPv6 scoped numeric zone = true",
                run_dns_predicate(rt_dns_is_ipv6, "fe80::1%1") == 1);
    test_result("IsIPv6 scoped named zone = true",
                run_dns_predicate(rt_dns_is_ipv6, "fe80::1%en0") == 1);
    test_result("IsIPv6 empty zone = false", run_dns_predicate(rt_dns_is_ipv6, "fe80::1%") == 0);
    test_result("IsIPv6 invalid zone = false",
                run_dns_predicate(rt_dns_is_ipv6, "fe80::1%bad/zone") == 0);
    test_result("IsIPv6 duplicate zone separator = false",
                run_dns_predicate(rt_dns_is_ipv6, "fe80::1%1%2") == 0);
    test_result("IsIPv6('127.0.0.1') = false", run_dns_predicate(rt_dns_is_ipv6, "127.0.0.1") == 0);
    test_result("IsIPv6('hello') = false", run_dns_predicate(rt_dns_is_ipv6, "hello") == 0);
    test_result("IsIPv6('') = false", run_dns_predicate(rt_dns_is_ipv6, "") == 0);
}

/// @brief Test DNS IsIP
static void test_dns_is_ip() {
    printf("\nTesting DNS IsIP:\n");

    test_result("IsIP('127.0.0.1') = true", run_dns_predicate(rt_dns_is_ip, "127.0.0.1") == 1);
    test_result("IsIP('::1') = true", run_dns_predicate(rt_dns_is_ip, "::1") == 1);
    test_result("IsIP scoped IPv6 = true", run_dns_predicate(rt_dns_is_ip, "fe80::1%1") == 1);
    test_result("IsIP('hello') = false", run_dns_predicate(rt_dns_is_ip, "hello") == 0);
    test_result("IsIP('') = false", run_dns_predicate(rt_dns_is_ip, "") == 0);
}

/// @brief Test DNS LocalHost
static void test_dns_local_host() {
    printf("\nTesting DNS LocalHost:\n");

    rt_string hostname = rt_dns_local_host();
    const char *name = rt_string_cstr(hostname);

    test_result("DNS LocalHost returns non-empty", name != nullptr && strlen(name) > 0);
    printf("  Local hostname: %s\n", name);
    rt_string_unref(hostname);
}

/// @brief Test DNS LocalAddrs
static void test_dns_local_addrs() {
    printf("\nTesting DNS LocalAddrs:\n");

    void *addrs = rt_dns_local_addrs();
    int64_t count = rt_seq_len(addrs);

    test_result("DNS LocalAddrs returns Seq", addrs != nullptr);
    test_result("DNS LocalAddrs has entries", count > 0);
    test_result("DNS LocalAddrs entries are unique valid IPs",
                dns_addresses_are_unique_and_valid(addrs));

    printf("  Found %lld local addresses:\n", (long long)count);
    for (int64_t i = 0; i < count && i < 5; i++) // Limit output
    {
        rt_string addr = (rt_string)rt_seq_get(addrs, i);
        printf("    - %s\n", rt_string_cstr(addr));
    }
    if (count > 5)
        printf("    ... and %lld more\n", (long long)(count - 5));
    release_test_object(addrs);
}

/// @brief Test DNS ResolveAll localhost
static void test_dns_resolve_all() {
    printf("\nTesting DNS ResolveAll localhost:\n");

    rt_string hostname = rt_const_cstr("localhost");
    void *addrs = rt_dns_resolve_all(hostname);
    int64_t count = rt_seq_len(addrs);

    test_result("DNS ResolveAll returns Seq", addrs != nullptr);
    test_result("DNS ResolveAll has entries", count > 0);
    test_result("DNS ResolveAll entries are unique valid IPs",
                dns_addresses_are_unique_and_valid(addrs));

    // Check first entry is localhost IP
    if (count > 0) {
        rt_string first = (rt_string)rt_seq_get(addrs, 0);
        const char *ip = rt_string_cstr(first);
        printf("  First address: %s\n", ip);
        // Either 127.0.0.1 or ::1 is acceptable
        bool valid = (strcmp(ip, "127.0.0.1") == 0 || strcmp(ip, "::1") == 0);
        test_result("DNS ResolveAll first is valid localhost", valid);
    }
    release_test_object(addrs);
    rt_string_unref(hostname);
}

/// @brief Verify DNS snapshot cleanup across injected managed allocation traps.
/// @details Fails the owning Seq allocation and the first result-String
///          allocation for `ResolveAll`, then fails the first local-address
///          String allocation. Each nested DNS recovery frame must release its
///          native snapshot and partial managed graph before rethrowing. The GC
///          tracked count verifies that no partial Seq remains; successful
///          retries prove the allocator hook and DNS paths remain reusable.
static void test_dns_allocation_trap_cleanup() {
    printf("\nTesting DNS managed-allocation trap cleanup:\n");

    rt_string hostname = rt_const_cstr("localhost");
    const int64_t tracked_baseline = rt_gc_tracked_count();
    for (int fail_at = 1; fail_at <= 2; ++fail_at) {
        bool trapped = false;
        void *result = nullptr;
        jmp_buf recovery;
        tcp_alloc_fail_countdown = fail_at;
        rt_trap_set_recovery(&recovery);
        if (setjmp(recovery) == 0) {
            rt_set_alloc_hook(tcp_fail_countdown_alloc);
            result = rt_dns_resolve_all(hostname);
            rt_set_alloc_hook(nullptr);
            rt_trap_clear_recovery();
        } else {
            rt_set_alloc_hook(nullptr);
            trapped = true;
            rt_trap_clear_recovery();
        }
        release_test_object(result);
        test_result(fail_at == 1 ? "ResolveAll cleans failed Seq allocation"
                                 : "ResolveAll cleans failed address allocation",
                    trapped && tcp_alloc_fail_countdown == 0 &&
                        rt_gc_tracked_count() == tracked_baseline);
    }

    bool local_trapped = false;
    void *local_result = nullptr;
    jmp_buf recovery;
    tcp_alloc_fail_countdown = 2;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        rt_set_alloc_hook(tcp_fail_countdown_alloc);
        local_result = rt_dns_local_addrs();
        rt_set_alloc_hook(nullptr);
        rt_trap_clear_recovery();
    } else {
        rt_set_alloc_hook(nullptr);
        local_trapped = true;
        rt_trap_clear_recovery();
    }
    release_test_object(local_result);
    test_result("LocalAddrs cleans native snapshot and partial Seq on allocation trap",
                local_trapped && tcp_alloc_fail_countdown == 0 &&
                    rt_gc_tracked_count() == tracked_baseline);

    void *retry = rt_dns_resolve_all(hostname);
    test_result("ResolveAll succeeds after recovered allocation traps",
                retry && rt_seq_len(retry) > 0 && dns_addresses_are_unique_and_valid(retry));
    release_test_object(retry);
    test_result("Successful DNS retry releases back to tracked baseline",
                rt_gc_tracked_count() == tracked_baseline);
    rt_string_unref(hostname);
}

//=============================================================================
// HTTP Tests
//=============================================================================

#include "rt_map.h"

static std::string read_http_header_value(void *client, const char *name) {
    std::string value_out;
    const size_t name_len = strlen(name);

    while (true) {
        rt_string line = rt_tcp_recv_line(client);
        const char *cstr = rt_string_cstr(line);
        const bool done = !cstr || *cstr == '\0';
        if (cstr && ascii_header_name_equals(cstr, name, name_len) && cstr[name_len] == ':') {
            const char *value = cstr + name_len + 1;
            while (*value == ' ' || *value == '\t')
                value++;
            value_out = value;
        }
        rt_string_unref(line);
        if (done)
            break;
    }

    return value_out;
}

/// @brief Mock HTTP server - returns fixed response
static void http_server_thread(int port, const char *response_body, int response_status) {
    void *server = rt_tcp_server_listen(port);
    assert(server != nullptr);

    server_ready = true;

    void *client = rt_tcp_server_accept(server);
    if (client) {
        // Read request line and headers (drain them)
        char buf[4096];
        int pos = 0;
        while (pos < (int)sizeof(buf) - 1) {
            void *data = rt_tcp_recv(client, 1);
            if (get_bytes_len(data) == 0)
                break;
            buf[pos++] = get_bytes_data(data)[0];
            // Look for end of headers
            if (pos >= 4 && buf[pos - 4] == '\r' && buf[pos - 3] == '\n' && buf[pos - 2] == '\r' &&
                buf[pos - 1] == '\n') {
                break;
            }
        }

        // Build HTTP response
        char response[8192];
        snprintf(response,
                 sizeof(response),
                 "HTTP/1.1 %d OK\r\n"
                 "Content-Type: text/plain\r\n"
                 "Content-Length: %zu\r\n"
                 "X-Test-Header: test-value\r\n"
                 "\r\n%s",
                 response_status,
                 strlen(response_body),
                 response_body);

        rt_string resp_str = rt_const_cstr(response);
        rt_tcp_send_str(client, resp_str);

        rt_tcp_close(client);
    }

    rt_tcp_server_close(server);
    server_done = true;
}

/// @brief Mock HTTP server for chunked encoding
static void http_chunked_server_thread(int port) {
    void *server = rt_tcp_server_listen(port);
    assert(server != nullptr);

    server_ready = true;

    void *client = rt_tcp_server_accept(server);
    if (client) {
        // Read request line and headers
        char buf[4096];
        int pos = 0;
        while (pos < (int)sizeof(buf) - 1) {
            void *data = rt_tcp_recv(client, 1);
            if (get_bytes_len(data) == 0)
                break;
            buf[pos++] = get_bytes_data(data)[0];
            if (pos >= 4 && buf[pos - 4] == '\r' && buf[pos - 3] == '\n' && buf[pos - 2] == '\r' &&
                buf[pos - 1] == '\n') {
                break;
            }
        }

        // Build chunked HTTP response
        const char *response = "HTTP/1.1 200 OK\r\n"
                               "Transfer-Encoding: chunked\r\n"
                               "Content-Type: text/plain\r\n"
                               "\r\n"
                               "5\r\nHello\r\n"
                               "6\r\nWorld!\r\n"
                               "0\r\n\r\n";

        rt_string resp_str = rt_const_cstr(response);
        rt_tcp_send_str(client, resp_str);

        rt_tcp_close(client);
    }

    rt_tcp_server_close(server);
    server_done = true;
}

/// @brief Mock HTTP server for redirects
static void http_redirect_server_thread(int port, int target_port) {
    void *server = rt_tcp_server_listen(port);
    assert(server != nullptr);

    server_ready = true;

    void *client = rt_tcp_server_accept(server);
    if (client) {
        // Read request
        char buf[4096];
        int pos = 0;
        while (pos < (int)sizeof(buf) - 1) {
            void *data = rt_tcp_recv(client, 1);
            if (get_bytes_len(data) == 0)
                break;
            buf[pos++] = get_bytes_data(data)[0];
            if (pos >= 4 && buf[pos - 4] == '\r' && buf[pos - 3] == '\n' && buf[pos - 2] == '\r' &&
                buf[pos - 1] == '\n') {
                break;
            }
        }

        // Send 302 redirect
        char response[512];
        snprintf(response,
                 sizeof(response),
                 "HTTP/1.1 302 Found\r\n"
                 "Location: http://127.0.0.1:%d/final\r\n"
                 "Content-Length: 0\r\n"
                 "\r\n",
                 target_port);

        rt_string resp_str = rt_const_cstr(response);
        rt_tcp_send_str(client, resp_str);

        rt_tcp_close(client);
    }

    rt_tcp_server_close(server);
    server_done = true;
}

/// @brief Mock HTTP server that first emits a relative redirect, then serves the final target.
static void http_relative_redirect_server_thread(int port) {
    void *server = rt_tcp_server_listen(port);
    assert(server != nullptr);

    server_ready = true;

    for (int i = 0; i < 2; i++) {
        void *client = rt_tcp_server_accept(server);
        if (!client)
            break;

        char buf[4096];
        int pos = 0;
        while (pos < (int)sizeof(buf) - 1) {
            void *data = rt_tcp_recv(client, 1);
            if (get_bytes_len(data) == 0)
                break;
            buf[pos++] = get_bytes_data(data)[0];
            if (pos >= 4 && buf[pos - 4] == '\r' && buf[pos - 3] == '\n' && buf[pos - 2] == '\r' &&
                buf[pos - 1] == '\n') {
                break;
            }
        }
        buf[pos] = '\0';

        if (i == 0) {
            const char *response = "HTTP/1.1 302 Found\r\n"
                                   "Location: final\r\n"
                                   "Content-Length: 0\r\n"
                                   "\r\n";
            rt_tcp_send_str(client, rt_const_cstr(response));
        } else {
            const char *response = "HTTP/1.1 200 OK\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Content-Length: 18\r\n"
                                   "\r\nRelative redirect!";
            rt_tcp_send_str(client, rt_const_cstr(response));
        }

        rt_tcp_close(client);
    }

    rt_tcp_server_close(server);
    server_done = true;
}

/// @brief Mock HTTP server that sends a gzip-encoded response body.
static void http_gzip_server_thread(int port, const char *plain_body) {
    void *server = rt_tcp_server_listen(port);
    assert(server != nullptr);

    server_ready = true;
    http_accept_encoding_header.clear();

    void *client = rt_tcp_server_accept(server);
    if (client) {
        rt_string request_line = rt_tcp_recv_line(client);
        rt_string_unref(request_line);
        http_accept_encoding_header = read_http_header_value(client, "Accept-Encoding");

        void *plain_bytes = make_bytes_str(plain_body);
        void *gzip_bytes = rt_compress_gzip(plain_bytes);
        const size_t gzip_len = (size_t)get_bytes_len(gzip_bytes);

        char headers[512];
        const int header_len = snprintf(headers,
                                        sizeof(headers),
                                        "HTTP/1.1 200 OK\r\n"
                                        "Content-Type: text/plain\r\n"
                                        "Content-Encoding: gzip\r\n"
                                        "Content-Length: %zu\r\n"
                                        "\r\n",
                                        gzip_len);
        assert(header_len > 0);

        void *response = rt_bytes_new((int64_t)header_len + (int64_t)gzip_len);
        uint8_t *response_ptr = get_bytes_data(response);
        memcpy(response_ptr, headers, (size_t)header_len);
        memcpy(response_ptr + header_len, get_bytes_data(gzip_bytes), gzip_len);
        rt_tcp_send_all(client, response);

        rt_tcp_close(client);
    }

    rt_tcp_server_close(server);
    server_done = true;
}

/// @brief Serve a highly compressible body used to exercise HTTP expansion limits.
/// @param port Loopback listener port.
/// @param plain_len Decoded payload size; the payload is filled with one byte.
static void http_gzip_expansion_server_thread(int port, size_t plain_len) {
    void *server = rt_tcp_server_listen(port);
    assert(server != nullptr);

    server_ready = true;
    http_gzip_encoded_size = 0;

    void *client = rt_tcp_server_accept(server);
    if (client) {
        rt_string request_line = rt_tcp_recv_line(client);
        rt_string_unref(request_line);
        (void)read_http_header_value(client, "Accept-Encoding");

        void *plain_bytes = rt_bytes_new((int64_t)plain_len);
        memset(get_bytes_data(plain_bytes), 'Z', plain_len);
        void *gzip_bytes = rt_compress_gzip(plain_bytes);
        const size_t gzip_len = (size_t)get_bytes_len(gzip_bytes);
        http_gzip_encoded_size = gzip_len;

        char headers[512];
        const int header_len = snprintf(headers,
                                        sizeof(headers),
                                        "HTTP/1.1 200 OK\r\n"
                                        "Content-Type: application/octet-stream\r\n"
                                        "Content-Encoding: gzip\r\n"
                                        "Content-Length: %zu\r\n"
                                        "\r\n",
                                        gzip_len);
        assert(header_len > 0 && (size_t)header_len < sizeof(headers));
        rt_tcp_send_str(client, rt_const_cstr(headers));
        rt_tcp_send_all(client, gzip_bytes);

        release_test_object(gzip_bytes);
        release_test_object(plain_bytes);
        rt_tcp_close(client);
    }

    rt_tcp_server_close(server);
    server_done = true;
}

/// @brief Mock HTTP server that records Accept-Encoding and serves an identity body.
static void http_identity_download_server_thread(int port, const char *body) {
    void *server = rt_tcp_server_listen(port);
    assert(server != nullptr);

    server_ready = true;
    http_accept_encoding_header.clear();

    void *client = rt_tcp_server_accept(server);
    if (client) {
        rt_string request_line = rt_tcp_recv_line(client);
        rt_string_unref(request_line);
        http_accept_encoding_header = read_http_header_value(client, "Accept-Encoding");

        char response[8192];
        snprintf(response,
                 sizeof(response),
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: text/plain\r\n"
                 "Content-Length: %zu\r\n"
                 "\r\n%s",
                 strlen(body),
                 body);
        rt_tcp_send_str(client, rt_const_cstr(response));
        rt_tcp_close(client);
    }

    rt_tcp_server_close(server);
    server_done = true;
}

/// @brief Test Http.Get with mock server
static void test_http_get() {
    printf("\nTesting Http.Get:\n");

    const int port = get_free_tcp_port_ipv4();
    assert(port > 0);
    const char *body = "Hello from HTTP!";
    server_ready = false;
    server_done = false;

    std::thread server_thread(http_server_thread, port, body, 200);

    while (!server_ready) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/test", port);
    rt_string url_str = rt_const_cstr(url);

    rt_string result = rt_http_get(url_str);
    const char *result_cstr = rt_string_cstr(result);

    test_result("Http.Get returns response body", result_cstr != nullptr);
    test_result("Http.Get body matches", strcmp(result_cstr, body) == 0);

    server_thread.join();
}

/// @brief Test Http.GetBytes with mock server
static void test_http_get_bytes() {
    printf("\nTesting Http.GetBytes:\n");

    const int port = get_free_tcp_port_ipv4();
    assert(port > 0);
    const char *body = "Binary data here";
    server_ready = false;
    server_done = false;

    std::thread server_thread(http_server_thread, port, body, 200);

    while (!server_ready) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/bytes", port);
    rt_string url_str = rt_const_cstr(url);

    void *result = rt_http_get_bytes(url_str);

    test_result("Http.GetBytes returns Bytes", result != nullptr);
    test_result("Http.GetBytes length matches", get_bytes_len(result) == (int64_t)strlen(body));
    test_result("Http.GetBytes data matches",
                memcmp(get_bytes_data(result), body, strlen(body)) == 0);

    server_thread.join();
}

/// @brief Test Http.Head with mock server
static void test_http_head() {
    printf("\nTesting Http.Head:\n");

    const int port = get_free_tcp_port_ipv4();
    assert(port > 0);
    server_ready = false;
    server_done = false;

    std::thread server_thread(http_server_thread, port, "ignored body", 200);

    while (!server_ready) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/head", port);
    rt_string url_str = rt_const_cstr(url);

    void *headers = rt_http_head(url_str);

    test_result("Http.Head returns Map", headers != nullptr);

    // Check for expected header
    rt_string header_name = rt_const_cstr("x-test-header");
    rt_string header_val = (rt_string)rt_map_get(headers, header_name);
    const char *val = rt_string_cstr(header_val);

    test_result("Http.Head contains X-Test-Header", strcmp(val, "test-value") == 0);

    server_thread.join();
}

/// @brief Test chunked transfer encoding
static void test_http_chunked() {
    printf("\nTesting Http chunked encoding:\n");

    const int port = get_free_tcp_port_ipv4();
    assert(port > 0);
    server_ready = false;
    server_done = false;

    std::thread server_thread(http_chunked_server_thread, port);

    while (!server_ready) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/chunked", port);
    rt_string url_str = rt_const_cstr(url);

    rt_string result = rt_http_get(url_str);
    const char *result_cstr = rt_string_cstr(result);

    test_result("Http chunked returns body", result_cstr != nullptr);
    test_result("Http chunked body decoded", strcmp(result_cstr, "HelloWorld!") == 0);

    server_thread.join();
}

/// @brief Test HttpReq builder pattern
static void test_http_req_builder() {
    printf("\nTesting HttpReq builder:\n");

    void *null_result = rt_http_req_send_result(NULL);
    test_result("HttpReq.SendResult(NULL) returns Result.Err", rt_result_is_err(null_result) == 1);
    release_test_object(null_result);

    const int port = get_free_tcp_port_ipv4();
    assert(port > 0);
    server_ready = false;
    server_done = false;

    std::thread server_thread(http_server_thread, port, "response body", 201);

    while (!server_ready) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/api", port);

    // Build request using chainable API
    void *req = rt_http_req_new(rt_const_cstr("GET"), rt_const_cstr(url));
    test_result("HttpReq.New returns object", req != nullptr);

    req = rt_http_req_set_header(req, rt_const_cstr("X-Custom"), rt_const_cstr("value"));
    test_result("HttpReq.SetHeader returns same object", req != nullptr);
    req = rt_http_req_set_header(req, NULL, rt_const_cstr("ignored"));
    test_result("HttpReq.SetHeader ignores NULL name", req != nullptr);
    req = rt_http_req_set_header(req, rt_const_cstr("X-Ignored"), NULL);
    test_result("HttpReq.SetHeader ignores NULL value", req != nullptr);
    req = rt_http_req_set_body_str(req, NULL);
    test_result("HttpReq.SetBodyStr(NULL) clears body", req != nullptr);

    req = rt_http_req_set_timeout(req, 5000);
    test_result("HttpReq.SetTimeout returns same object", req != nullptr);

    req = rt_http_req_set_force_http1(req, 1);
    test_result("HttpReq.SetForceHttp1 returns same object", req != nullptr);

    req = rt_http_req_allow_insecure_certificates_for_testing(req);
    test_result("HttpReq.AllowInsecureCertificatesForTesting returns same object", req != nullptr);

    void *res = rt_http_req_send(req);
    test_result("HttpReq.Send returns HttpRes", res != nullptr);

    // Check response
    int64_t status = rt_http_res_status(res);
    test_result("HttpRes.Status is 201", status == 201);

    rt_string status_text = rt_http_res_status_text(res);
    test_result("HttpRes.StatusText is OK", strcmp(rt_string_cstr(status_text), "OK") == 0);

    int8_t is_ok = rt_http_res_is_ok(res);
    test_result("HttpRes.IsOk is true for 2xx", is_ok == 1);

    rt_string body = rt_http_res_body_str(res);
    test_result("HttpRes.BodyStr matches", strcmp(rt_string_cstr(body), "response body") == 0);

    void *body_bytes = rt_http_res_body(res);
    test_result("HttpRes.Body returns Bytes", get_bytes_len(body_bytes) == 13);

    void *headers = rt_http_res_headers(res);
    test_result("HttpRes.Headers returns Map", headers != nullptr);

    rt_string content_type = rt_http_res_header(res, rt_const_cstr("content-type"));
    test_result("HttpRes.Header retrieves header",
                strcmp(rt_string_cstr(content_type), "text/plain") == 0);
    rt_string missing_header = rt_http_res_header(res, NULL);
    test_result("HttpRes.Header(NULL) returns empty", rt_str_len(missing_header) == 0);

    server_thread.join();
}

/// @brief Fail every managed allocation used to construct an HTTP error Result.
/// @details A count-only pass records the deterministic `SendResult(NULL)`
///          allocation count. Each boundary is then failed beneath an outer
///          recovery frame. The helper must propagate exactly one allocation
///          trap, return no partial Result, and restore the exact managed-object
///          baseline instead of recursively retrying Result construction.
static void test_http_send_result_allocation_cleanup() {
    printf("\nTesting HttpReq.SendResult allocation cleanup:\n");

    const int64_t baseline = rt_gc_tracked_count();
    http_alloc_fail_countdown = 0;
    http_alloc_observed = 0;
    rt_set_alloc_hook(http_countdown_alloc);
    void *count_result = rt_http_req_send_result(NULL);
    rt_set_alloc_hook(nullptr);
    const int allocation_count = http_alloc_observed;
    test_result("Count-only SendResult returns Result.Err",
                count_result && rt_result_is_err(count_result) == 1);
    release_test_object(count_result);
    test_result("Count-only SendResult releases its complete Result graph",
                rt_gc_tracked_count() == baseline);

    int trap_count = 0;
    bool all_failures_clean = allocation_count > 0;
    for (int fail_at = 1; fail_at <= allocation_count; ++fail_at) {
        void *result = nullptr;
        bool trapped = false;
        jmp_buf recovery;
        http_alloc_fail_countdown = fail_at;
        http_alloc_observed = 0;
        rt_trap_set_recovery(&recovery);
        if (setjmp(recovery) == 0) {
            rt_set_alloc_hook(http_countdown_alloc);
            result = rt_http_req_send_result(NULL);
            rt_set_alloc_hook(nullptr);
            rt_trap_clear_recovery();
        } else {
            rt_set_alloc_hook(nullptr);
            trapped = true;
            rt_trap_clear_recovery();
        }
        trap_count += trapped ? 1 : 0;
        all_failures_clean = all_failures_clean && trapped && result == nullptr;
        release_test_object(result);
        all_failures_clean = all_failures_clean && rt_gc_tracked_count() == baseline;
    }
    http_alloc_fail_countdown = 0;
    test_result("Every SendResult construction allocation failure propagates once",
                all_failures_clean && trap_count == allocation_count);
    test_result("Every SendResult construction failure releases partial values",
                rt_gc_tracked_count() == baseline);
}

/// @brief Verify stable HTTP identities, forged-handle rejection, and accessor cleanup.
/// @details Uses a raw server so the tracked-count baseline contains only
///          client-owned objects. Invalid body/pool updates must preserve the
///          previous request state. Header-copy and single-header accessors are
///          then failed at each managed allocation boundary and must return to
///          the exact baseline after recovery.
static void test_http_identity_and_accessor_trap_cleanup() {
    printf("\nTesting HTTP stable identity and accessor trap cleanup:\n");

    const int64_t initial_tracked = rt_gc_tracked_count();
    const int port = get_free_tcp_port_ipv4();
    assert(port > 0);
    server_ready = false;
    server_done = false;
    std::thread server(native_http_fault_server, port, "identity-body");
    while (!server_ready.load(std::memory_order_acquire))
        std::this_thread::yield();

    char url_buffer[96];
    snprintf(url_buffer, sizeof(url_buffer), "http://127.0.0.1:%d/identity", port);
    rt_string method = rt_const_cstr("GET");
    rt_string url = rt_const_cstr(url_buffer);
    rt_string body_string = rt_const_cstr("preserved-body");
    rt_string header_name = rt_const_cstr("X-Fault-Test");
    void *request = rt_http_req_new(method, url);
    void *response = rt_http_req_send(request);
    server.join();

    test_result("HttpReq publishes its stable class identity",
                rt_obj_is_instance(request, RT_HTTP_REQ_CLASS_ID, sizeof(rt_http_req_t)) == 1);
    test_result("HttpRes publishes its stable class identity",
                rt_obj_is_instance(response, RT_HTTP_RES_CLASS_ID, sizeof(rt_http_res_t)) == 1);

    void *pool = rt_http_conn_pool_new(4);
    test_result("HTTP connection pool publishes stable initialized identity",
                rt_http_conn_pool_is_handle(pool) == 1);
    test_result("HttpReq accepts a validated retained pool",
                rt_http_req_set_connection_pool(request, pool) == request &&
                    static_cast<rt_http_req_t *>(request)->connection_pool == pool);
    test_result("HttpReq stores a copied baseline body",
                rt_http_req_set_body_str(request, body_string) == request &&
                    static_cast<rt_http_req_t *>(request)->body_len == strlen("preserved-body") &&
                    memcmp(static_cast<rt_http_req_t *>(request)->body,
                           "preserved-body",
                           strlen("preserved-body")) == 0);

    void *wrong = rt_seq_new();
    jmp_buf recovery;
    bool body_trapped = false;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        (void)rt_http_req_set_body(request, wrong);
        rt_trap_clear_recovery();
    } else {
        body_trapped = true;
        rt_trap_clear_recovery();
    }
    test_result("Rejected Bytes body preserves prior request body",
                body_trapped &&
                    static_cast<rt_http_req_t *>(request)->body_len == strlen("preserved-body") &&
                    memcmp(static_cast<rt_http_req_t *>(request)->body,
                           "preserved-body",
                           strlen("preserved-body")) == 0);

    bool pool_trapped = false;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        (void)rt_http_req_set_connection_pool(request, wrong);
        rt_trap_clear_recovery();
    } else {
        pool_trapped = true;
        rt_trap_clear_recovery();
    }
    test_result("Rejected pool preserves prior retained pool",
                pool_trapped && static_cast<rt_http_req_t *>(request)->connection_pool == pool);

    bool request_receiver_trapped = false;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        (void)rt_http_req_set_timeout(wrong, 1);
        rt_trap_clear_recovery();
    } else {
        request_receiver_trapped = true;
        rt_trap_clear_recovery();
    }
    bool response_receiver_trapped = false;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        (void)rt_http_res_headers(wrong);
        rt_trap_clear_recovery();
    } else {
        response_receiver_trapped = true;
        rt_trap_clear_recovery();
    }
    bool response_predicate_trapped = false;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        (void)rt_http_res_is_ok(wrong);
        rt_trap_clear_recovery();
    } else {
        response_predicate_trapped = true;
        rt_trap_clear_recovery();
    }
    bool pool_receiver_trapped = false;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        rt_http_conn_pool_clear(wrong);
        rt_trap_clear_recovery();
    } else {
        pool_receiver_trapped = true;
        rt_trap_clear_recovery();
    }
    test_result("Forged HTTP receivers trap before native payload access",
                request_receiver_trapped && response_receiver_trapped &&
                    response_predicate_trapped && pool_receiver_trapped && rt_seq_len(wrong) == 0);

    const int64_t accessor_baseline = rt_gc_tracked_count();
    bool all_header_copy_failures_clean = true;
    int header_copy_traps = 0;
    for (int fail_at = 1; fail_at <= 8; ++fail_at) {
        void *copy = nullptr;
        bool trapped = false;
        http_alloc_fail_countdown = fail_at;
        http_alloc_observed = 0;
        rt_trap_set_recovery(&recovery);
        if (setjmp(recovery) == 0) {
            rt_set_alloc_hook(http_countdown_alloc);
            copy = rt_http_res_headers(response);
            rt_set_alloc_hook(nullptr);
            rt_trap_clear_recovery();
        } else {
            rt_set_alloc_hook(nullptr);
            trapped = true;
            rt_trap_clear_recovery();
        }
        if (trapped)
            header_copy_traps++;
        release_test_object(copy);
        all_header_copy_failures_clean =
            all_header_copy_failures_clean && rt_gc_tracked_count() == accessor_baseline;
    }
    test_result("HttpRes.Headers releases every partial Map/Seq/String snapshot",
                all_header_copy_failures_clean && header_copy_traps >= 4);

    bool all_header_lookup_failures_clean = true;
    int header_lookup_traps = 0;
    for (int fail_at = 1; fail_at <= 2; ++fail_at) {
        rt_string value = nullptr;
        bool trapped = false;
        http_alloc_fail_countdown = fail_at;
        http_alloc_observed = 0;
        rt_trap_set_recovery(&recovery);
        if (setjmp(recovery) == 0) {
            rt_set_alloc_hook(http_countdown_alloc);
            value = rt_http_res_header(response, header_name);
            rt_set_alloc_hook(nullptr);
            rt_trap_clear_recovery();
        } else {
            rt_set_alloc_hook(nullptr);
            trapped = true;
            rt_trap_clear_recovery();
        }
        if (trapped)
            header_lookup_traps++;
        if (value)
            rt_string_unref(value);
        all_header_lookup_failures_clean =
            all_header_lookup_failures_clean && rt_gc_tracked_count() == accessor_baseline;
    }
    test_result("HttpRes.Header releases lookup key/value/result on allocation traps",
                all_header_lookup_failures_clean && header_lookup_traps == 2);

    release_test_object(wrong);
    release_test_object(response);
    release_test_object(request);
    release_test_object(pool);
    rt_string_unref(header_name);
    rt_string_unref(body_string);
    rt_string_unref(url);
    rt_string_unref(method);
    test_result("HTTP identity test returns to its managed-object baseline",
                rt_gc_tracked_count() == initial_tracked);
}

/// @brief Fail every managed allocation in one complete `Http.Get` transaction.
/// @details A successful count-only pass establishes the exact allocation count
///          for the warmed deterministic path. Each subsequent pass fails one
///          position, catches the propagated trap, joins the allocation-free
///          native server, and verifies that parsing/publication/conversion left
///          no tracked object behind.
static void test_http_one_shot_allocation_sweep() {
    printf("\nTesting Http.Get allocation-by-allocation cleanup:\n");

    auto run_server = [](int port) {
        server_ready = false;
        server_done = false;
        return std::thread(native_http_fault_server, port, "fault-body");
    };

    int count_port = get_free_tcp_port_ipv4();
    assert(count_port > 0);
    std::thread count_server = run_server(count_port);
    while (!server_ready.load(std::memory_order_acquire))
        std::this_thread::yield();
    char count_url_buffer[96];
    snprintf(
        count_url_buffer, sizeof(count_url_buffer), "http://127.0.0.1:%d/fault-count", count_port);
    rt_string count_url = rt_const_cstr(count_url_buffer);
    const int64_t baseline = rt_gc_tracked_count();
    http_alloc_fail_countdown = 0;
    http_alloc_observed = 0;
    rt_set_alloc_hook(http_countdown_alloc);
    rt_string counted_result = rt_http_get(count_url);
    rt_set_alloc_hook(nullptr);
    count_server.join();
    const int allocation_count = http_alloc_observed;
    test_result("Count-only Http.Get completes through the native server",
                counted_result && strcmp(rt_string_cstr(counted_result), "fault-body") == 0 &&
                    allocation_count > 0);
    rt_string_unref(counted_result);
    test_result("Count-only Http.Get leaves no temporary managed objects",
                rt_gc_tracked_count() == baseline);

    bool all_failures_trapped = true;
    bool all_failures_clean = true;
    for (int fail_at = 1; fail_at <= allocation_count; ++fail_at) {
        const int port = get_free_tcp_port_ipv4();
        assert(port > 0);
        std::thread server = run_server(port);
        while (!server_ready.load(std::memory_order_acquire))
            std::this_thread::yield();
        char url_buffer[96];
        snprintf(url_buffer, sizeof(url_buffer), "http://127.0.0.1:%d/fault", port);
        rt_string url = rt_const_cstr(url_buffer);
        const int64_t iteration_baseline = rt_gc_tracked_count();
        rt_string result = nullptr;
        bool trapped = false;
        jmp_buf recovery;
        http_alloc_fail_countdown = fail_at;
        http_alloc_observed = 0;
        rt_trap_set_recovery(&recovery);
        if (setjmp(recovery) == 0) {
            rt_set_alloc_hook(http_countdown_alloc);
            result = rt_http_get(url);
            rt_set_alloc_hook(nullptr);
            rt_trap_clear_recovery();
        } else {
            rt_set_alloc_hook(nullptr);
            trapped = true;
            rt_trap_clear_recovery();
        }
        server.join();
        if (result)
            rt_string_unref(result);
        all_failures_trapped = all_failures_trapped && trapped && http_alloc_fail_countdown == 0;
        all_failures_clean = all_failures_clean && rt_gc_tracked_count() == iteration_baseline;
        rt_string_unref(url);
    }
    test_result("Every managed Http.Get allocation failure propagates exactly once",
                all_failures_trapped);
    test_result("Every Http.Get allocation failure releases the full transaction",
                all_failures_clean && rt_gc_tracked_count() == baseline);
    rt_string_unref(count_url);
}

/// @brief Test HTTP redirects
static void test_http_redirect() {
    printf("\nTesting Http redirect:\n");

    const int redirect_port = get_free_tcp_port_ipv4();
    const int target_port = get_free_tcp_port_ipv4();
    assert(redirect_port > 0);
    assert(target_port > 0);
    assert(redirect_port != target_port);
    server_ready = false;
    server_done = false;

    // Start redirect server
    std::thread redirect_thread(http_redirect_server_thread, redirect_port, target_port);

    while (!server_ready) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Start target server
    std::atomic<bool> target_ready{false};
    std::thread target_thread([target_port, &target_ready]() {
        void *server = rt_tcp_server_listen(target_port);
        assert(server != nullptr);
        target_ready = true;

        void *client = rt_tcp_server_accept(server);
        if (client) {
            // Drain request
            char buf[4096];
            int pos = 0;
            while (pos < (int)sizeof(buf) - 1) {
                void *data = rt_tcp_recv(client, 1);
                if (get_bytes_len(data) == 0)
                    break;
                buf[pos++] = get_bytes_data(data)[0];
                if (pos >= 4 && buf[pos - 4] == '\r' && buf[pos - 3] == '\n' &&
                    buf[pos - 2] == '\r' && buf[pos - 1] == '\n') {
                    break;
                }
            }

            const char *response = "HTTP/1.1 200 OK\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Content-Length: 12\r\n"
                                   "\r\nFinal target";

            rt_tcp_send_str(client, rt_const_cstr(response));
            rt_tcp_close(client);
        }
        rt_tcp_server_close(server);
    });

    while (!target_ready) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/redirect", redirect_port);

    rt_string result = rt_http_get(rt_const_cstr(url));
    const char *result_cstr = rt_string_cstr(result);

    test_result("Http redirect follows Location", strcmp(result_cstr, "Final target") == 0);

    redirect_thread.join();
    target_thread.join();
}

/// @brief Test relative redirect resolution against the current request URL.
static void test_http_relative_redirect() {
    printf("\nTesting Http relative redirect:\n");

    const int port = get_free_tcp_port_ipv4();
    assert(port > 0);
    server_ready = false;
    server_done = false;

    std::thread server_thread(http_relative_redirect_server_thread, port);
    while (!server_ready)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/start", port);

    rt_string result = rt_http_get(rt_const_cstr(url));
    const char *result_cstr = rt_string_cstr(result);
    test_result("Http relative redirect follows sibling path",
                strcmp(result_cstr, "Relative redirect!") == 0);

    server_thread.join();
}

/// @brief Test transparent gzip decoding and automatic Accept-Encoding negotiation.
static void test_http_gzip_response() {
    printf("\nTesting Http gzip response decoding:\n");

    const int port = get_free_tcp_port_ipv4();
    assert(port > 0);
    const char *body = "Hello from gzip!";
    server_ready = false;
    server_done = false;

    std::thread server_thread(http_gzip_server_thread, port, body);
    while (!server_ready)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/gzip", port);

    void *req = rt_http_req_new(rt_const_cstr("GET"), rt_const_cstr(url));
    void *res = rt_http_req_send(req);
    test_result("HttpReq gzip response returns HttpRes", res != nullptr);

    rt_string result = rt_http_res_body_str(res);
    test_result("Http gzip body is transparently decompressed",
                strcmp(rt_string_cstr(result), body) == 0);

    rt_string content_encoding = rt_http_res_header(res, rt_const_cstr("content-encoding"));
    test_result("Http gzip response strips Content-Encoding after decode",
                strcmp(rt_string_cstr(content_encoding), "") == 0);

    char expected_len[32];
    snprintf(expected_len, sizeof(expected_len), "%zu", strlen(body));
    rt_string content_length = rt_http_res_header(res, rt_const_cstr("content-length"));
    test_result("Http gzip response normalizes Content-Length to decoded body",
                strcmp(rt_string_cstr(content_length), expected_len) == 0);

    server_thread.join();
    test_result("Http requests advertise gzip support by default",
                http_accept_encoding_header.find("gzip") != std::string::npos);
}

/// @brief Verify HTTP rejects excessive gzip expansion before body publication.
static void test_http_gzip_expansion_limit() {
    printf("\nTesting Http gzip expansion limit:\n");

    constexpr size_t plain_len = 8u * 1024u * 1024u;
    const int port = get_free_tcp_port_ipv4();
    assert(port > 0);
    server_ready = false;
    server_done = false;
    http_gzip_encoded_size = 0;

    std::thread server_thread(http_gzip_expansion_server_thread, port, plain_len);
    while (!server_ready)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/gzip-limit", port);
    void *req = rt_http_req_new(rt_const_cstr("GET"), rt_const_cstr(url));
    void *result = rt_http_req_send_result(req);

    server_thread.join();
    const size_t encoded_len = http_gzip_encoded_size.load();
    test_result("Fixture exceeds the configured expansion budget",
                encoded_len > 0 && (uint64_t)plain_len > (uint64_t)encoded_len * UINT64_C(128) +
                                                             UINT64_C(1024) * 1024u);
    test_result("HttpReq rejects excessive gzip expansion",
                result && rt_result_is_err(result) == 1);
    if (result && rt_result_is_err(result)) {
        rt_string error = rt_result_unwrap_err_str(result);
        test_result("Expansion failure is a protocol error",
                    error && strstr(rt_string_cstr(error), "invalid gzip") != nullptr);
    }

    release_test_object(result);
    release_test_object(req);
}

/// @brief Test Http.Download writes streamed content to disk.
static void test_http_download() {
    printf("\nTesting Http download:\n");

    const int port = get_free_tcp_port_ipv4();
    assert(port > 0);
    const char *body = "Download payload for disk";
    server_ready = false;
    server_done = false;

    std::thread server_thread(http_server_thread, port, body, 200);
    while (!server_ready)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/download", port);
    char path[256];
#if RT_PLATFORM_WINDOWS
    snprintf(path, sizeof(path), "/tmp/zanna_http_download_\xE6\x9D\xB1\xE4\xBA\xAC_%d.txt", port);
    network_test_remove_utf8(path);
    FILE *existing = network_test_fopen_utf8(path, L"wb");
#else
    snprintf(path, sizeof(path), "/tmp/zanna_http_download_%d.txt", port);
    remove(path);
    FILE *existing = fopen(path, "wb");
#endif
    assert(existing != nullptr);
    assert(fwrite("stale", 1, 5, existing) == 5);
    assert(fclose(existing) == 0);
#if !RT_PLATFORM_WINDOWS
    assert(chmod(path, 0640) == 0);
#endif

    int8_t ok = rt_http_download(rt_const_cstr(url), rt_const_cstr(path));
    test_result("Http.Download succeeds", ok == 1);

#if RT_PLATFORM_WINDOWS
    FILE *f = network_test_fopen_utf8(path, L"rb");
#else
    FILE *f = fopen(path, "rb");
#endif
    test_result("Http.Download creates destination file", f != nullptr);
    char buffer[128] = {0};
    size_t read_len = f ? fread(buffer, 1, sizeof(buffer) - 1, f) : 0;
    if (f)
        fclose(f);
#if !RT_PLATFORM_WINDOWS
    struct stat downloaded_stat = {};
    const bool mode_preserved =
        stat(path, &downloaded_stat) == 0 && (downloaded_stat.st_mode & 0777) == 0640;
#else
    wchar_t *wide_path = rt_file_path_utf8_to_wide(path);
    struct _stat64 downloaded_stat = {};
    const bool mode_preserved =
        wide_path && _wstat64(wide_path, &downloaded_stat) == 0 &&
        (downloaded_stat.st_mode & (_S_IREAD | _S_IWRITE)) == (_S_IREAD | _S_IWRITE);
    std::free(wide_path);
#endif
#if RT_PLATFORM_WINDOWS
    network_test_remove_utf8(path);
#else
    remove(path);
#endif

    test_result("Http.Download writes expected bytes", read_len == strlen(body));
    test_result("Http.Download file contents match", strcmp(buffer, body) == 0);
    test_result("Http.Download preserves existing ordinary file permissions", mode_preserved);

    server_thread.join();
}

/// @brief Test Http.Download keeps identity encoding so the stream can stay on-disk.
static void test_http_download_identity_encoding() {
    printf("\nTesting Http download identity encoding:\n");

    const int port = get_free_tcp_port_ipv4();
    assert(port > 0);
    const char *body = "Download payload for identity stream";
    server_ready = false;
    server_done = false;

    std::thread server_thread(http_identity_download_server_thread, port, body);
    while (!server_ready)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/download-identity", port);
    char path[128];
    snprintf(path, sizeof(path), "/tmp/zanna_http_download_identity_%d.txt", port);
    remove(path);

    int8_t ok = rt_http_download(rt_const_cstr(url), rt_const_cstr(path));
    test_result("Http.Download identity request succeeds", ok == 1);

    FILE *f = fopen(path, "rb");
    test_result("Http.Download identity output exists", f != nullptr);
    char buffer[128] = {0};
    size_t read_len = f ? fread(buffer, 1, sizeof(buffer) - 1, f) : 0;
    if (f)
        fclose(f);
    remove(path);

    server_thread.join();

    test_result("Http.Download does not advertise gzip by default",
                http_accept_encoding_header.find("gzip") == std::string::npos);
    test_result("Http.Download identity body length matches", read_len == strlen(body));
    test_result("Http.Download identity body matches", strcmp(buffer, body) == 0);
}

/// @brief Verify streaming download contains managed parser traps and removes staging files.
/// @details The first response-head managed allocation is failed while a raw
///          server performs no runtime allocation. The public Boolean API must
///          return false without propagating, leave the destination absent, and
///          restore the tracked-object baseline. Embedded-NUL URL/path inputs
///          are also rejected before any filesystem or transport side effect.
static void test_http_download_trap_and_input_cleanup() {
    printf("\nTesting Http.Download trap containment and input validation:\n");

    const int port = get_free_tcp_port_ipv4();
    assert(port > 0);
    server_ready = false;
    server_done = false;
    std::thread server(native_http_fault_server, port, "download-fault");
    while (!server_ready.load(std::memory_order_acquire))
        std::this_thread::yield();

    char url_buffer[96];
    char path_buffer[160];
    snprintf(url_buffer, sizeof(url_buffer), "http://127.0.0.1:%d/download-fault", port);
    snprintf(path_buffer, sizeof(path_buffer), "/tmp/zanna_http_download_fault_%d.txt", port);
    remove(path_buffer);
    rt_string url = rt_const_cstr(url_buffer);
    rt_string path = rt_const_cstr(path_buffer);
    const int64_t baseline = rt_gc_tracked_count();

    http_alloc_fail_countdown = 1;
    http_alloc_observed = 0;
    rt_set_alloc_hook(http_countdown_alloc);
    int8_t ok = rt_http_download(url, path);
    rt_set_alloc_hook(nullptr);
    server.join();
    FILE *unexpected = fopen(path_buffer, "rb");
    if (unexpected)
        fclose(unexpected);
    test_result("Http.Download converts response allocation trap to false",
                ok == 0 && http_alloc_fail_countdown == 0);
    test_result("Failed Http.Download removes staged and destination content",
                unexpected == nullptr && rt_gc_tracked_count() == baseline);
    remove(path_buffer);

    const char hidden_url_bytes[] = {'h', 't', 't', 'p', ':', '/', '/', 'x', '\0', 'y'};
    const char hidden_path_bytes[] = {'/', 't', 'm', 'p', '/', 'x', '\0', 'y'};
    rt_string hidden_url = rt_string_from_bytes(hidden_url_bytes, sizeof(hidden_url_bytes));
    rt_string hidden_path = rt_string_from_bytes(hidden_path_bytes, sizeof(hidden_path_bytes));
    test_result("Http.Download rejects embedded-NUL URL before transport",
                rt_http_download(hidden_url, path) == 0);
    test_result("Http.Download rejects embedded-NUL destination before filesystem access",
                rt_http_download(url, hidden_path) == 0);

    rt_string_unref(hidden_path);
    rt_string_unref(hidden_url);
    rt_string_unref(path);
    rt_string_unref(url);
}

//=============================================================================
// Url Tests
//=============================================================================

/// @brief Test URL parsing with all components.
static void test_url_parse_full() {
    printf("  test_url_parse_full...\n");

    void *url = rt_url_parse(rt_const_cstr(
        "https://user:pass@example.com:8080/path/to/resource?foo=bar&baz=qux#section"));

    const char *scheme = rt_string_cstr(rt_url_scheme(url));
    const char *user = rt_string_cstr(rt_url_user(url));
    const char *pass = rt_string_cstr(rt_url_pass(url));
    const char *host = rt_string_cstr(rt_url_host(url));
    int64_t port = rt_url_port(url);
    const char *path = rt_string_cstr(rt_url_path(url));
    const char *query = rt_string_cstr(rt_url_query(url));
    const char *fragment = rt_string_cstr(rt_url_fragment(url));

    test_result("URL scheme parsed", strcmp(scheme, "https") == 0);
    test_result("URL user parsed", strcmp(user, "user") == 0);
    test_result("URL pass parsed", strcmp(pass, "pass") == 0);
    test_result("URL host parsed", strcmp(host, "example.com") == 0);
    test_result("URL port parsed", port == 8080);
    test_result("URL path parsed", strcmp(path, "/path/to/resource") == 0);
    test_result("URL query parsed", strcmp(query, "foo=bar&baz=qux") == 0);
    test_result("URL fragment parsed", strcmp(fragment, "section") == 0);
}

/// @brief Test URL parsing with minimal URL.
static void test_url_parse_minimal() {
    printf("  test_url_parse_minimal...\n");

    void *url = rt_url_parse(rt_const_cstr("http://localhost"));

    const char *scheme = rt_string_cstr(rt_url_scheme(url));
    const char *host = rt_string_cstr(rt_url_host(url));
    int64_t port = rt_url_port(url);
    const char *path = rt_string_cstr(rt_url_path(url));

    test_result("Minimal URL scheme", strcmp(scheme, "http") == 0);
    test_result("Minimal URL host", strcmp(host, "localhost") == 0);
    test_result("Minimal URL port is 0", port == 0);
    test_result("Minimal URL path is empty", strlen(path) == 0);
}

/// @brief Test URL building from scratch.
static void test_url_new() {
    printf("  test_url_new...\n");

    void *url = rt_url_new();
    rt_url_set_scheme(url, rt_const_cstr("https"));
    rt_url_set_host(url, rt_const_cstr("api.example.com"));
    rt_url_set_port(url, 443);
    rt_url_set_path(url, rt_const_cstr("/v1/users"));
    rt_url_set_query(url, rt_const_cstr("page=1"));
    rt_url_set_fragment(url, rt_const_cstr("top"));

    const char *full = rt_string_cstr(rt_url_full(url));

    // Port 443 is default for https, so it shouldn't appear
    test_result("URL built correctly",
                strcmp(full, "https://api.example.com/v1/users?page=1#top") == 0);
}

/// @brief Test HostPort with default and non-default ports.
static void test_url_host_port() {
    printf("  test_url_host_port...\n");

    // With default port
    void *url1 = rt_url_parse(rt_const_cstr("http://example.com:80/"));
    const char *hp1 = rt_string_cstr(rt_url_host_port(url1));
    test_result("HostPort hides default port", strcmp(hp1, "example.com") == 0);

    // With non-default port
    void *url2 = rt_url_parse(rt_const_cstr("http://example.com:8080/"));
    const char *hp2 = rt_string_cstr(rt_url_host_port(url2));
    test_result("HostPort shows non-default port", strcmp(hp2, "example.com:8080") == 0);
}

/// @brief Test Authority string.
static void test_url_authority() {
    printf("  test_url_authority...\n");

    void *url = rt_url_parse(rt_const_cstr("ftp://admin:secret@ftp.example.com:21/"));
    const char *auth = rt_string_cstr(rt_url_authority(url));

    test_result("Authority includes user:pass@host:port",
                strcmp(auth, "admin:secret@ftp.example.com:21") == 0);
}

/// @brief Test query parameter manipulation.
static void test_url_query_params() {
    printf("  test_url_query_params...\n");

    void *url = rt_url_parse(rt_const_cstr("http://example.com/?a=1&b=2"));

    // Test HasQueryParam
    test_result("HasQueryParam returns true for 'a'",
                rt_url_has_query_param(url, rt_const_cstr("a")) == 1);
    test_result("HasQueryParam returns true for 'b'",
                rt_url_has_query_param(url, rt_const_cstr("b")) == 1);
    test_result("HasQueryParam returns false for 'c'",
                rt_url_has_query_param(url, rt_const_cstr("c")) == 0);

    // Test GetQueryParam
    const char *val_a = rt_string_cstr(rt_url_get_query_param(url, rt_const_cstr("a")));
    const char *val_b = rt_string_cstr(rt_url_get_query_param(url, rt_const_cstr("b")));
    test_result("GetQueryParam returns correct value for 'a'", strcmp(val_a, "1") == 0);
    test_result("GetQueryParam returns correct value for 'b'", strcmp(val_b, "2") == 0);

    // Test SetQueryParam
    rt_url_set_query_param(url, rt_const_cstr("c"), rt_const_cstr("3"));
    test_result("SetQueryParam adds new param",
                rt_url_has_query_param(url, rt_const_cstr("c")) == 1);

    // Test DelQueryParam
    rt_url_del_query_param(url, rt_const_cstr("b"));
    test_result("DelQueryParam removes param",
                rt_url_has_query_param(url, rt_const_cstr("b")) == 0);
}

/// @brief Test QueryMap.
static void test_url_query_map() {
    printf("  test_url_query_map...\n");

    void *url = rt_url_parse(rt_const_cstr("http://example.com/?name=John&age=30"));
    void *map = rt_url_query_map(url);

    int64_t len = rt_map_len(map);
    test_result("QueryMap has 2 entries", len == 2);

    const char *name = rt_string_cstr(rt_map_get_str(map, rt_const_cstr("name")));
    const char *age = rt_string_cstr(rt_map_get_str(map, rt_const_cstr("age")));
    test_result("QueryMap has correct name", strcmp(name, "John") == 0);
    test_result("QueryMap has correct age", strcmp(age, "30") == 0);
}

/// @brief Test URL clone.
static void test_url_clone() {
    printf("  test_url_clone...\n");

    void *url = rt_url_parse(rt_const_cstr("https://example.com/path?query=1#frag"));
    void *clone = rt_url_clone(url);

    // Modify original
    rt_url_set_host(url, rt_const_cstr("modified.com"));

    const char *original_host = rt_string_cstr(rt_url_host(url));
    const char *clone_host = rt_string_cstr(rt_url_host(clone));

    test_result("Clone has original host", strcmp(clone_host, "example.com") == 0);
    test_result("Original was modified", strcmp(original_host, "modified.com") == 0);
}

/// @brief Test URL resolve (relative URL resolution).
static void test_url_resolve() {
    printf("  test_url_resolve...\n");

    void *base = rt_url_parse(rt_const_cstr("http://example.com/a/b/c"));

    // Absolute path
    void *r1 = rt_url_resolve(base, rt_const_cstr("/d/e"));
    const char *full1 = rt_string_cstr(rt_url_full(r1));
    test_result("Resolve absolute path", strcmp(full1, "http://example.com/d/e") == 0);

    // Relative path
    void *r2 = rt_url_resolve(base, rt_const_cstr("d"));
    const char *full2 = rt_string_cstr(rt_url_full(r2));
    printf("    Relative path result: %s\n", full2);
    test_result("Resolve relative path", strcmp(full2, "http://example.com/a/b/d") == 0);

    // Different scheme
    void *r3 = rt_url_resolve(base, rt_const_cstr("https://other.com/x"));
    const char *full3 = rt_string_cstr(rt_url_full(r3));
    test_result("Resolve different scheme", strcmp(full3, "https://other.com/x") == 0);

    // RFC dot-segment removal also applies to schemed and authority references.
    void *r4 = rt_url_resolve(base, rt_const_cstr("https://other.com/a/../x"));
    const char *full4 = rt_string_cstr(rt_url_full(r4));
    test_result("Resolve schemed path normalizes dot segments",
                strcmp(full4, "https://other.com/x") == 0);

    void *r5 = rt_url_resolve(base, rt_const_cstr("//other.com/a/../x?y=1#f"));
    const char *full5 = rt_string_cstr(rt_url_full(r5));
    test_result("Resolve authority path normalizes dot segments",
                strcmp(full5, "http://other.com/x?y=1#f") == 0);
}

/// @brief Test percent encoding/decoding.
static void test_url_encode_decode() {
    printf("  test_url_encode_decode...\n");

    // Test encoding
    const char *plain = "hello world!@#$%";
    rt_string encoded = rt_url_encode(rt_const_cstr(plain));
    const char *enc_str = rt_string_cstr(encoded);
    test_result("Encode contains no spaces", strchr(enc_str, ' ') == NULL);
    test_result("Encode starts with hello", strncmp(enc_str, "hello", 5) == 0);

    // Test decoding
    rt_string decoded = rt_url_decode(rt_const_cstr("hello%20world%21"));
    const char *dec_str = rt_string_cstr(decoded);
    test_result("Decode restores string", strcmp(dec_str, "hello world!") == 0);

    // Test plus decoding. Plain URL decoding follows RFC 3986; query decoding
    // keeps HTML form compatibility by treating '+' as a space.
    rt_string decoded_plus = rt_url_decode(rt_const_cstr("hello+world"));
    const char *dec_plus = rt_string_cstr(decoded_plus);
    test_result("Decode preserves plus", strcmp(dec_plus, "hello+world") == 0);

    const char binary_plain[] = {'a', '\0', ' ', 'b'};
    rt_string binary_input = rt_string_from_bytes(binary_plain, sizeof(binary_plain));
    rt_string binary_encoded = rt_url_encode(binary_input);
    test_result("Encode preserves embedded NUL as %00",
                strcmp(rt_string_cstr(binary_encoded), "a%00%20b") == 0);
    rt_string binary_decoded = rt_url_decode(binary_encoded);
    test_result("Decode restores embedded NUL bytes",
                rt_str_len(binary_decoded) == (int64_t)sizeof(binary_plain) &&
                    memcmp(rt_string_cstr(binary_decoded), binary_plain, sizeof(binary_plain)) ==
                        0);
}

/// @brief Test query string encoding/decoding.
static void test_url_encode_decode_query() {
    printf("  test_url_encode_decode_query...\n");

    // Create a map and encode it
    void *map = rt_map_new();
    rt_map_set_str(map, rt_const_cstr("name"), rt_const_cstr("John Doe"));
    rt_map_set_str(map, rt_const_cstr("city"), rt_const_cstr("New York"));

    rt_string query = rt_url_encode_query(map);
    const char *query_str = rt_string_cstr(query);

    // Query should contain encoded values
    test_result("EncodeQuery contains =", strchr(query_str, '=') != NULL);
    test_result("EncodeQuery contains &", strchr(query_str, '&') != NULL);

    // Decode back
    void *decoded_map = rt_url_decode_query(query);
    int64_t len = rt_map_len(decoded_map);
    test_result("DecodeQuery has 2 entries", len == 2);
    test_result("DecodeQuery name matches",
                strcmp(rt_string_cstr(rt_map_get_str(decoded_map, rt_const_cstr("name"))),
                       "John Doe") == 0);
    test_result("DecodeQuery city matches",
                strcmp(rt_string_cstr(rt_map_get_str(decoded_map, rt_const_cstr("city"))),
                       "New York") == 0);

    void *plus_map = rt_url_decode_query(rt_const_cstr("q=hello+world"));
    test_result(
        "DecodeQuery plus as space",
        strcmp(rt_string_cstr(rt_map_get_str(plus_map, rt_const_cstr("q"))), "hello world") == 0);

    const char key_bytes[] = {'k', '\0', 'y'};
    const char value_bytes[] = {'v', '\0', 'x'};
    rt_string binary_key = rt_string_from_bytes(key_bytes, sizeof(key_bytes));
    rt_string binary_value = rt_string_from_bytes(value_bytes, sizeof(value_bytes));
    void *binary_map = rt_map_new();
    rt_map_set_str(binary_map, binary_key, binary_value);
    rt_string binary_query = rt_url_encode_query(binary_map);
    test_result("EncodeQuery percent-encodes embedded NUL key/value",
                strcmp(rt_string_cstr(binary_query), "k%00y=v%00x") == 0);
    void *binary_decoded_map = rt_url_decode_query(binary_query);
    rt_string roundtrip_value = rt_map_get_str(binary_decoded_map, binary_key);
    test_result("DecodeQuery restores embedded NUL key/value",
                roundtrip_value && rt_str_len(roundtrip_value) == (int64_t)sizeof(value_bytes) &&
                    memcmp(rt_string_cstr(roundtrip_value), value_bytes, sizeof(value_bytes)) == 0);
}

/// @brief Test URL validation.
static void test_url_is_valid() {
    printf("  test_url_is_valid...\n");

    test_result("Valid http URL", rt_url_is_valid(rt_const_cstr("http://example.com")) == 1);
    test_result("Valid https URL", rt_url_is_valid(rt_const_cstr("https://example.com/path")) == 1);
    test_result("Valid URL with port",
                rt_url_is_valid(rt_const_cstr("http://example.com:8080")) == 1);
    test_result("Empty string is invalid", rt_url_is_valid(rt_const_cstr("")) == 0);
    const char hidden_suffix[] = {'h', 't', 't', 'p', ':', '/', '/', 'e', 'x', '\0', ' '};
    rt_string hidden_url = rt_string_from_bytes(hidden_suffix, sizeof(hidden_suffix));
    test_result("Embedded NUL URL is invalid", rt_url_is_valid(hidden_url) == 0);
}

/// @brief Test scheme is lowercased.
static void test_url_scheme_case() {
    printf("  test_url_scheme_case...\n");

    void *url = rt_url_parse(rt_const_cstr("HTTP://EXAMPLE.COM"));
    const char *scheme = rt_string_cstr(rt_url_scheme(url));

    test_result("Scheme is lowercased", strcmp(scheme, "http") == 0);
}

int main() {
    const bool canBindLocal = localhost_bind_available();

    if (canBindLocal) {
        printf("=== Zanna.Network.Tcp/TcpServer Tests ===\n");

        test_server_properties();
        test_server_ephemeral_port();
        test_listen_at();
        test_accept_timeout();
        test_tcp_server_concurrent_close_accept();
        test_tcp_transport_constructor_trap_cleanup();
        test_server_client_connect();
        test_server_client_connect_ipv6();
        test_client_properties();
        test_send_recv();
        test_tcp_receive_trap_cleanup();
        test_send_all_recv_exact();
        test_connection_pool_reuses_live_connection();
        test_connection_pool_tracks_fresh_acquire();
        test_connection_pool_clamps_max_size_and_closes_overflow();
        test_connection_pool_ownership_and_hygiene();
        test_connection_pool_rejects_cross_pool_aliasing();
        test_connection_pool_concurrent_bookkeeping();
        test_netutils_is_port_open_contract();
        test_async_socket_reference_transfer_and_boxed_send();
        test_recv_line();
        test_connect_with_timeout();

        printf("\n=== Zanna.Network.Udp Tests ===\n");

        test_udp_new();
        test_udp_datagram_integrity_and_trap_cleanup();
        test_udp_bind();
        test_udp_bind_at();
        test_udp_send_recv();
        test_udp_send_recv_str();
        test_udp_recv_from();
        test_udp_send_recv_ipv6();
        test_udp_recv_timeout();
        test_udp_broadcast();
        test_udp_set_recv_timeout();
    } else {
        printf("=== Zanna.Network Tcp/Udp Tests ===\n");
        printf("  SKIPPED: local bind unavailable in this environment\n");
    }

    printf("\n=== Zanna.Network.Dns Tests ===\n");

    test_dns_resolve_localhost();
    test_dns_resolve4_localhost();
    test_dns_is_ipv4();
    test_dns_is_ipv6();
    test_dns_is_ip();
    test_dns_local_host();
    test_dns_local_addrs();
    test_dns_resolve_all();
    test_dns_allocation_trap_cleanup();

    printf("\n=== Zanna.Network.Http Tests ===\n");
    if (canBindLocal) {
        test_http_get();
        test_http_get_bytes();
        test_http_head();
        test_http_chunked();
        test_http_req_builder();
        test_http_send_result_allocation_cleanup();
        test_http_identity_and_accessor_trap_cleanup();
        test_http_one_shot_allocation_sweep();
        test_http_redirect();
        test_http_relative_redirect();
        test_http_gzip_response();
        test_http_gzip_expansion_limit();
        test_http_download();
        test_http_download_identity_encoding();
        test_http_download_trap_and_input_cleanup();
    } else {
        printf("  SKIPPED: local bind unavailable in this environment\n");
    }

    printf("\n=== Zanna.Network.Url Tests ===\n");

    test_url_parse_full();
    test_url_parse_minimal();
    test_url_new();
    test_url_host_port();
    test_url_authority();
    test_url_query_params();
    test_url_query_map();
    test_url_clone();
    test_url_resolve();
    test_url_encode_decode();
    test_url_encode_decode_query();
    test_url_is_valid();
    test_url_scheme_case();

    printf("\nAll tests passed!\n");
    return 0;
}
