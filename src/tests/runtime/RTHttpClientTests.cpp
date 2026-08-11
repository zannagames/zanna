//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTHttpClientTests.cpp
// Purpose: Validate HttpClient identity, transactional ownership, and
//          synchronized session mutation under faults and concurrency.
// Key invariants:
//   - Every managed allocation boundary restores the tracked-object baseline.
//   - Failed default-header replacement preserves the prior complete value.
//   - Cookie, pool, and header snapshots are safe under concurrent mutation.
// Ownership/Lifetime:
//   - Each test releases every managed reference it creates.
//   - The header-capture fixture owns and closes all native sockets.
// Links: src/runtime/network/rt_http_client.c,
//        src/runtime/network/rt_http_client.h, src/runtime/rt_platform.h
//
//===----------------------------------------------------------------------===//

#include "tests/common/NetworkTestCompat.hpp"

#include "rt_gc.h"
#include "rt_http_client.h"
#include "rt_internal.h"
#include "rt_map.h"
#include "rt_network.h"
#include "rt_object.h"
#include "rt_platform.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "rt_trap.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if RT_PLATFORM_WINDOWS
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using header_socket_t = SOCKET;
static constexpr header_socket_t HEADER_SOCKET_INVALID = INVALID_SOCKET;
#define HEADER_SOCKET_CLOSE(socket_value) closesocket(socket_value)
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using header_socket_t = int;
static constexpr header_socket_t HEADER_SOCKET_INVALID = -1;
#define HEADER_SOCKET_CLOSE(socket_value) close(socket_value)
#endif

static int http_client_alloc_fail_countdown = 0;
static int http_client_alloc_observed = 0;

/// @brief Report one assertion using the runtime-test convention.
/// @param condition Condition that must be true.
/// @param name Human-readable assertion name.
static void test_result(bool condition, const char *name) {
    std::printf("  %s: %s\n", name, condition ? "PASS" : "FAIL");
    assert(condition);
}

/// @brief Count runtime-managed allocations and fail one selected boundary.
/// @details Native socket and cookie storage does not pass through this hook.
///          Tests install it only around a deterministic managed operation and
///          always restore the default allocator before leaving recovery.
/// @param bytes Requested payload size.
/// @param next Runtime allocator to invoke when this boundary is not rejected.
/// @return Allocated storage, or NULL at the selected countdown.
static void *http_client_countdown_alloc(int64_t bytes, void *(*next)(int64_t)) {
    http_client_alloc_observed++;
    if (http_client_alloc_fail_countdown > 0 && --http_client_alloc_fail_countdown == 0)
        return nullptr;
    return next(bytes);
}

/// @brief Release one caller-owned runtime reference immediately at zero.
/// @param object Runtime object or String; NULL is a no-op.
static void release_managed(void *object) {
    if (object && rt_obj_release_check0(object))
        rt_obj_free(object);
}

/// @brief Wait for a native socket to become readable within a bounded interval.
/// @param socket_value Socket to inspect.
/// @param timeout_ms Maximum wait in milliseconds.
/// @return True when read or accept can make progress.
static bool socket_is_readable(header_socket_t socket_value, int timeout_ms) {
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(socket_value, &read_set);
    timeval timeout{};
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
#if RT_PLATFORM_WINDOWS
    return select(0, &read_set, nullptr, nullptr, &timeout) > 0;
#else
    return select(socket_value + 1, &read_set, nullptr, nullptr, &timeout) > 0;
#endif
}

/// @brief Open a loopback-only TCP listener on an ephemeral port.
/// @details The socket is fully bound and listening before it is returned, so
///          the client cannot race a separate server-start thread. The caller
///          owns the returned socket and must close it.
/// @param out_port Receives the assigned host-order port.
/// @return Listening socket, or @ref HEADER_SOCKET_INVALID on failure.
static header_socket_t open_header_listener(int *out_port) {
    if (out_port)
        *out_port = 0;
    header_socket_t listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == HEADER_SOCKET_INVALID)
        return HEADER_SOCKET_INVALID;

    int reuse = 1;
#if RT_PLATFORM_WINDOWS
    (void)setsockopt(
        listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));
#else
    (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(zanna::tests::kIpv4LoopbackHostOrder);
    address.sin_port = 0;
    if (bind(listener, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0 ||
        listen(listener, 8) != 0) {
        HEADER_SOCKET_CLOSE(listener);
        return HEADER_SOCKET_INVALID;
    }

#if RT_PLATFORM_WINDOWS
    int address_size = sizeof(address);
#else
    socklen_t address_size = sizeof(address);
#endif
    if (getsockname(listener, reinterpret_cast<sockaddr *>(&address), &address_size) != 0) {
        HEADER_SOCKET_CLOSE(listener);
        return HEADER_SOCKET_INVALID;
    }
    if (out_port)
        *out_port = ntohs(address.sin_port);
    return listener;
}

/// @brief Send an entire small HTTP fixture response on a native socket.
/// @param socket_value Connected client socket.
/// @param bytes Response bytes to transmit.
/// @return True when every byte was sent.
static bool send_all(header_socket_t socket_value, const char *bytes) {
    size_t offset = 0;
    const size_t length = std::strlen(bytes);
    while (offset < length) {
#if RT_PLATFORM_WINDOWS
        int sent = send(socket_value, bytes + offset, static_cast<int>(length - offset), 0);
#else
        ssize_t sent = send(socket_value, bytes + offset, length - offset, 0);
#endif
        if (sent <= 0)
            return false;
        offset += static_cast<size_t>(sent);
    }
    return true;
}

/// @brief Capture a fixed number of complete HTTP/1 request header blocks.
/// @details Accept and receive operations are bounded. Each connection receives
///          a minimal close-delimited response, then both accepted sockets and
///          the listener are closed. Results are written only by this thread
///          and consumed after join.
/// @param listener Owned listening socket; always closed before return.
/// @param expected_requests Number of requests to accept.
/// @param captures Destination vector for raw request headers.
/// @param failed Set true on timeout, malformed input, or socket failure.
static void capture_header_requests(header_socket_t listener,
                                    int expected_requests,
                                    std::vector<std::string> *captures,
                                    std::atomic<bool> *failed) {
    static const char response[] = "HTTP/1.1 200 OK\r\n"
                                   "Content-Length: 2\r\n"
                                   "Connection: close\r\n"
                                   "\r\n"
                                   "ok";
    for (int request_index = 0; request_index < expected_requests; request_index++) {
        if (!socket_is_readable(listener, 10000)) {
            failed->store(true);
            break;
        }
        header_socket_t client = accept(listener, nullptr, nullptr);
        if (client == HEADER_SOCKET_INVALID) {
            failed->store(true);
            break;
        }

        std::string request;
        char buffer[1024];
        while (request.find("\r\n\r\n") == std::string::npos && request.size() < 65536u) {
            if (!socket_is_readable(client, 5000)) {
                failed->store(true);
                break;
            }
#if RT_PLATFORM_WINDOWS
            int received = recv(client, buffer, static_cast<int>(sizeof(buffer)), 0);
#else
            ssize_t received = recv(client, buffer, sizeof(buffer), 0);
#endif
            if (received <= 0) {
                failed->store(true);
                break;
            }
            request.append(buffer, static_cast<size_t>(received));
        }
        if (request.find("\r\n\r\n") == std::string::npos)
            failed->store(true);
        captures->push_back(request);
        if (!send_all(client, response))
            failed->store(true);
        HEADER_SOCKET_CLOSE(client);
    }
    HEADER_SOCKET_CLOSE(listener);
}

/// @brief Extract all values for one case-insensitive HTTP header name.
/// @param request Complete raw HTTP/1 request header block.
/// @param wanted_name Header name without a colon.
/// @return Trimmed values in wire order.
static std::vector<std::string> extract_header_values(const std::string &request,
                                                      const char *wanted_name) {
    std::vector<std::string> values;
    size_t line_start = request.find("\r\n");
    if (line_start == std::string::npos)
        return values;
    line_start += 2;
    while (line_start < request.size()) {
        size_t line_end = request.find("\r\n", line_start);
        if (line_end == std::string::npos || line_end == line_start)
            break;
        size_t colon = request.find(':', line_start);
        if (colon != std::string::npos && colon < line_end) {
            size_t name_length = colon - line_start;
            bool matches = name_length == std::strlen(wanted_name);
            for (size_t i = 0; matches && i < name_length; i++) {
                unsigned char left = static_cast<unsigned char>(request[line_start + i]);
                unsigned char right = static_cast<unsigned char>(wanted_name[i]);
                if (left >= 'A' && left <= 'Z')
                    left = static_cast<unsigned char>(left + ('a' - 'A'));
                if (right >= 'A' && right <= 'Z')
                    right = static_cast<unsigned char>(right + ('a' - 'A'));
                matches = left == right;
            }
            if (matches) {
                size_t value_start = colon + 1;
                while (value_start < line_end &&
                       (request[value_start] == ' ' || request[value_start] == '\t')) {
                    value_start++;
                }
                size_t value_end = line_end;
                while (value_end > value_start &&
                       (request[value_end - 1] == ' ' || request[value_end - 1] == '\t')) {
                    value_end--;
                }
                values.emplace_back(request.substr(value_start, value_end - value_start));
            }
        }
        line_start = line_end + 2;
    }
    return values;
}

/// @brief Verify stable identity and cleanup at every constructor allocation.
/// @details A count-only construction establishes the exact managed allocation
///          sequence. Each boundary is then rejected beneath recovery, and the
///          runtime registry must return to the pre-call baseline. A forged Seq
///          receiver must trap before its payload is interpreted as a client.
static void test_identity_and_constructor_cleanup() {
    std::printf("\nTesting HttpClient identity and constructor cleanup:\n");
    const int64_t baseline = rt_gc_tracked_count();

    http_client_alloc_fail_countdown = 0;
    http_client_alloc_observed = 0;
    rt_set_alloc_hook(http_client_countdown_alloc);
    void *client = rt_http_client_new();
    rt_set_alloc_hook(nullptr);
    const int allocation_count = http_client_alloc_observed;
    test_result(client && rt_obj_class_id(client) == RT_HTTP_CLIENT_CLASS_ID,
                "constructor publishes the stable HttpClient class ID");
    release_managed(client);
    test_result(rt_gc_tracked_count() == baseline,
                "successful constructor releases to its exact baseline");

    bool all_clean = allocation_count > 0;
    int traps = 0;
    for (int fail_at = 1; fail_at <= allocation_count; fail_at++) {
        void *volatile failed_client = nullptr;
        volatile bool trapped = false;
        jmp_buf recovery;
        http_client_alloc_fail_countdown = fail_at;
        http_client_alloc_observed = 0;
        rt_trap_set_recovery(&recovery);
        if (setjmp(recovery) == 0) {
            rt_set_alloc_hook(http_client_countdown_alloc);
            failed_client = rt_http_client_new();
            rt_set_alloc_hook(nullptr);
            rt_trap_clear_recovery();
        } else {
            rt_set_alloc_hook(nullptr);
            trapped = true;
            rt_trap_clear_recovery();
        }
        traps += trapped ? 1 : 0;
        all_clean = all_clean && trapped && failed_client == nullptr;
        release_managed((void *)failed_client);
        all_clean = all_clean && rt_gc_tracked_count() == baseline;
    }
    http_client_alloc_fail_countdown = 0;
    test_result(all_clean && traps == allocation_count,
                "every constructor allocation failure cleans partial state");

    void *wrong = rt_seq_new();
    volatile bool forged_trapped = false;
    jmp_buf forged_recovery;
    rt_trap_set_recovery(&forged_recovery);
    if (setjmp(forged_recovery) == 0) {
        (void)rt_http_client_get_keep_alive(wrong);
        rt_trap_clear_recovery();
    } else {
        forged_trapped = true;
        rt_trap_clear_recovery();
    }
    test_result(forged_trapped && rt_seq_len(wrong) == 0,
                "forged receiver traps before HttpClient payload access");
    release_managed(wrong);
    test_result(rt_gc_tracked_count() == baseline,
                "identity probes leave no managed objects behind");
}

/// @brief Sweep the malformed-URL request transaction allocation by allocation.
/// @details The URL reaches HttpReq parsing but cannot reach transport. This
///          deterministically exercises copied URL/method/request ownership and
///          the outer request recovery frame. Every injected failure and the
///          normal invalid-URL trap must leave the reusable client unchanged.
static void test_request_transaction_cleanup() {
    std::printf("\nTesting HttpClient request transaction cleanup:\n");
    const int64_t entry_baseline = rt_gc_tracked_count();
    void *client = rt_http_client_new();
    rt_string invalid_url = rt_const_cstr("http://");
    const int64_t baseline = rt_gc_tracked_count();

    http_client_alloc_fail_countdown = 0;
    http_client_alloc_observed = 0;
    volatile bool calibration_trapped = false;
    jmp_buf calibration_recovery;
    rt_trap_set_recovery(&calibration_recovery);
    if (setjmp(calibration_recovery) == 0) {
        rt_set_alloc_hook(http_client_countdown_alloc);
        (void)rt_http_client_get(client, invalid_url);
        rt_set_alloc_hook(nullptr);
        rt_trap_clear_recovery();
    } else {
        rt_set_alloc_hook(nullptr);
        calibration_trapped = true;
        rt_trap_clear_recovery();
    }
    const int allocation_count = http_client_alloc_observed;
    test_result(calibration_trapped && allocation_count > 0 && rt_gc_tracked_count() == baseline,
                "invalid request releases its complete transaction");

    volatile bool all_clean = true;
    volatile int traps = 0;
    for (volatile int fail_at = 1; fail_at <= allocation_count;) {
        void *volatile response = nullptr;
        volatile bool trapped = false;
        jmp_buf recovery;
        http_client_alloc_fail_countdown = fail_at;
        http_client_alloc_observed = 0;
        rt_trap_set_recovery(&recovery);
        if (setjmp(recovery) == 0) {
            rt_set_alloc_hook(http_client_countdown_alloc);
            response = rt_http_client_get(client, invalid_url);
            rt_set_alloc_hook(nullptr);
            rt_trap_clear_recovery();
        } else {
            rt_set_alloc_hook(nullptr);
            trapped = true;
            rt_trap_clear_recovery();
        }
        traps += trapped ? 1 : 0;
        all_clean = all_clean && trapped && response == nullptr;
        release_managed((void *)response);
        all_clean = all_clean && rt_gc_tracked_count() == baseline &&
                    rt_http_client_get_keep_alive(client) == 1;
        // Advance in the body: C++20 deprecates reading the value
        // of an assignment to a volatile operand.
        fail_at = fail_at + 1;
    }
    http_client_alloc_fail_countdown = 0;
    test_result(all_clean && traps == allocation_count,
                "every pre-transport allocation failure is recoverable and leak-free");

    release_managed((void *)invalid_url);
    release_managed(client);
    test_result(rt_gc_tracked_count() == entry_baseline,
                "request transaction test releases its client and input");
}

/// @brief Verify GetCookies snapshot cleanup at every managed allocation.
/// @details Native cookie name/value copies are captured under the mutex before
///          managed Map construction. The sweep proves that a failed Map, key,
///          value, or insertion releases all partial values and leaves the jar
///          usable for a subsequent snapshot.
static void test_cookie_snapshot_allocation_cleanup() {
    std::printf("\nTesting HttpClient cookie snapshot allocation cleanup:\n");
    const int64_t entry_baseline = rt_gc_tracked_count();
    void *client = rt_http_client_new();
    rt_string domain = rt_const_cstr("example.test");
    rt_string name = rt_const_cstr("session");
    rt_string value = rt_const_cstr("complete-value");
    rt_http_client_set_cookie(client, domain, name, value);
    const int64_t baseline = rt_gc_tracked_count();

    http_client_alloc_fail_countdown = 0;
    http_client_alloc_observed = 0;
    rt_set_alloc_hook(http_client_countdown_alloc);
    void *snapshot = rt_http_client_get_cookies(client, domain);
    rt_set_alloc_hook(nullptr);
    const int allocation_count = http_client_alloc_observed;
    rt_string observed = (rt_string)rt_map_get(snapshot, name);
    test_result(observed && std::strcmp(rt_string_cstr(observed), "complete-value") == 0,
                "successful cookie snapshot contains a complete copied value");
    release_managed(snapshot);
    test_result(rt_gc_tracked_count() == baseline,
                "successful cookie snapshot releases to baseline");

    bool all_clean = allocation_count > 0;
    int traps = 0;
    for (int fail_at = 1; fail_at <= allocation_count; fail_at++) {
        void *volatile failed_snapshot = nullptr;
        volatile bool trapped = false;
        jmp_buf recovery;
        http_client_alloc_fail_countdown = fail_at;
        rt_trap_set_recovery(&recovery);
        if (setjmp(recovery) == 0) {
            rt_set_alloc_hook(http_client_countdown_alloc);
            failed_snapshot = rt_http_client_get_cookies(client, domain);
            rt_set_alloc_hook(nullptr);
            rt_trap_clear_recovery();
        } else {
            rt_set_alloc_hook(nullptr);
            trapped = true;
            rt_trap_clear_recovery();
        }
        traps += trapped ? 1 : 0;
        all_clean = all_clean && trapped && failed_snapshot == nullptr;
        release_managed((void *)failed_snapshot);
        all_clean = all_clean && rt_gc_tracked_count() == baseline;
    }
    http_client_alloc_fail_countdown = 0;
    test_result(all_clean && traps == allocation_count,
                "every cookie snapshot allocation failure releases partial state");

    snapshot = rt_http_client_get_cookies(client, domain);
    observed = (rt_string)rt_map_get(snapshot, name);
    test_result(observed && std::strcmp(rt_string_cstr(observed), "complete-value") == 0,
                "cookie jar remains usable after the allocation sweep");
    release_managed(snapshot);
    release_managed((void *)value);
    release_managed((void *)name);
    release_managed((void *)domain);
    release_managed(client);
    test_result(rt_gc_tracked_count() == entry_baseline,
                "cookie snapshot test releases all managed state");
}

/// @brief Verify failed and concurrent default-header replacement on the wire.
/// @details Each managed allocation in a case-insensitive replacement is
///          rejected on a fresh client. A loopback server must observe exactly
///          one prior header with its old value. A final multi-threaded update
///          must publish exactly one complete writer value. Clients disable
///          keep-alive so each request maps to one bounded fixture connection.
static void test_transactional_and_concurrent_headers() {
    std::printf("\nTesting HttpClient transactional and concurrent headers:\n");
    const int64_t entry_baseline = rt_gc_tracked_count();
    rt_string old_name = rt_const_cstr("X-Atomic");
    rt_string new_name = rt_const_cstr("x-atomic");
    rt_string old_value = rt_const_cstr("preserved-old");
    rt_string new_value = rt_const_cstr("rejected-new");

    void *calibration_client = rt_http_client_new();
    rt_http_client_set_header(calibration_client, old_name, old_value);
    http_client_alloc_fail_countdown = 0;
    http_client_alloc_observed = 0;
    rt_set_alloc_hook(http_client_countdown_alloc);
    rt_http_client_set_header(calibration_client, new_name, new_value);
    rt_set_alloc_hook(nullptr);
    const int allocation_count = http_client_alloc_observed;
    release_managed(calibration_client);
    test_result(allocation_count > 0, "header replacement exposes managed allocation boundaries");

    int port = 0;
    header_socket_t listener = open_header_listener(&port);
    if (listener == HEADER_SOCKET_INVALID || port <= 0) {
        if (listener != HEADER_SOCKET_INVALID)
            HEADER_SOCKET_CLOSE(listener);
        std::printf("  SKIP: loopback listener unavailable\n");
        release_managed((void *)new_value);
        release_managed((void *)old_value);
        release_managed((void *)new_name);
        release_managed((void *)old_name);
        return;
    }

    std::vector<std::string> captures;
    std::atomic<bool> server_failed{false};
    std::thread server(
        capture_header_requests, listener, allocation_count + 1, &captures, &server_failed);
    char url_buffer[128];
    std::snprintf(url_buffer, sizeof(url_buffer), "http://127.0.0.1:%d/headers", port);
    rt_string url = rt_const_cstr(url_buffer);

    volatile bool all_preserved = true;
    for (volatile int fail_at = 1; fail_at <= allocation_count;) {
        void *client = rt_http_client_new();
        rt_http_client_set_keep_alive(client, 0);
        rt_http_client_set_header(client, old_name, old_value);
        const int64_t baseline = rt_gc_tracked_count();
        volatile bool trapped = false;
        jmp_buf recovery;
        http_client_alloc_fail_countdown = fail_at;
        rt_trap_set_recovery(&recovery);
        if (setjmp(recovery) == 0) {
            rt_set_alloc_hook(http_client_countdown_alloc);
            rt_http_client_set_header(client, new_name, new_value);
            rt_set_alloc_hook(nullptr);
            rt_trap_clear_recovery();
        } else {
            rt_set_alloc_hook(nullptr);
            trapped = true;
            rt_trap_clear_recovery();
        }
        http_client_alloc_fail_countdown = 0;
        all_preserved = all_preserved && trapped && rt_gc_tracked_count() == baseline;
        void *response = rt_http_client_get(client, url);
        all_preserved = all_preserved && response && rt_http_res_status(response) == 200;
        release_managed(response);
        release_managed(client);
        // Advance in the body: C++20 deprecates reading the value
        // of an assignment to a volatile operand.
        fail_at = fail_at + 1;
    }

    void *concurrent_client = rt_http_client_new();
    rt_http_client_set_keep_alive(concurrent_client, 0);
    rt_http_client_set_header(concurrent_client, old_name, old_value);
    constexpr int writer_count = 6;
    constexpr int iterations = 60;
    std::vector<rt_string> writer_values;
    std::vector<std::thread> writers;
    for (int writer = 0; writer < writer_count; writer++) {
        char value_buffer[32];
        std::snprintf(value_buffer, sizeof(value_buffer), "writer-%d", writer);
        writer_values.push_back(rt_const_cstr(value_buffer));
    }
    for (int writer = 0; writer < writer_count; writer++) {
        writers.emplace_back([concurrent_client, new_name, value = writer_values[writer]]() {
            for (int iteration = 0; iteration < iterations; iteration++)
                rt_http_client_set_header(concurrent_client, new_name, value);
        });
    }
    for (std::thread &writer : writers)
        writer.join();
    void *response = rt_http_client_get(concurrent_client, url);
    all_preserved = all_preserved && response && rt_http_res_status(response) == 200;
    release_managed(response);
    release_managed(concurrent_client);

    server.join();
    bool wire_values_valid =
        !server_failed.load() && captures.size() == static_cast<size_t>(allocation_count + 1);
    for (int index = 0; wire_values_valid && index < allocation_count; index++) {
        std::vector<std::string> values = extract_header_values(captures[index], "X-Atomic");
        wire_values_valid = values.size() == 1 && values[0] == "preserved-old";
    }
    if (wire_values_valid) {
        std::vector<std::string> values = extract_header_values(captures.back(), "X-Atomic");
        bool complete_writer = false;
        if (values.size() == 1) {
            for (rt_string writer_value : writer_values) {
                if (values[0] == rt_string_cstr(writer_value))
                    complete_writer = true;
            }
        }
        wire_values_valid = complete_writer;
    }
    test_result(all_preserved && wire_values_valid,
                "failed and concurrent replacements publish one complete header value");

    for (rt_string writer_value : writer_values)
        release_managed((void *)writer_value);
    release_managed((void *)url);
    release_managed((void *)new_value);
    release_managed((void *)old_value);
    release_managed((void *)new_name);
    release_managed((void *)old_name);
    test_result(rt_gc_tracked_count() == entry_baseline,
                "header transaction test releases clients, requests, and responses");
}

/// @brief Stress cookie snapshots and pool replacement on one shared client.
/// @details Writers replace a single cookie with immutable complete values,
///          readers repeatedly clone the jar, and independent workers toggle
///          and resize the keep-alive pool. Every observed cookie must be one
///          complete writer value, no worker may deadlock, and all superseded
///          pools must release when the shared client is destroyed.
static void test_concurrent_cookie_and_pool_state() {
    std::printf("\nTesting HttpClient concurrent cookie and pool state:\n");
    const int64_t entry_baseline = rt_gc_tracked_count();
    void *client = rt_http_client_new();
    rt_string domain = rt_const_cstr("concurrent.test");
    rt_string name = rt_const_cstr("token");
    std::vector<rt_string> values;
    for (int index = 0; index < 4; index++) {
        char value_buffer[32];
        std::snprintf(value_buffer, sizeof(value_buffer), "complete-%d", index);
        values.push_back(rt_const_cstr(value_buffer));
    }
    rt_http_client_set_cookie(client, domain, name, values[0]);

    std::atomic<bool> valid{true};
    std::vector<std::thread> workers;
    for (int writer = 0; writer < 4; writer++) {
        workers.emplace_back([client, domain, name, value = values[writer]]() {
            for (int iteration = 0; iteration < 160; iteration++)
                rt_http_client_set_cookie(client, domain, name, value);
        });
    }
    for (int reader = 0; reader < 2; reader++) {
        workers.emplace_back([client, domain, name, &values, &valid]() {
            for (int iteration = 0; iteration < 160; iteration++) {
                void *snapshot = rt_http_client_get_cookies(client, domain);
                rt_string observed = snapshot ? (rt_string)rt_map_get(snapshot, name) : nullptr;
                bool complete = observed != nullptr;
                for (size_t index = 0; complete && index < values.size(); index++) {
                    if (std::strcmp(rt_string_cstr(observed), rt_string_cstr(values[index])) == 0) {
                        release_managed(snapshot);
                        snapshot = nullptr;
                        observed = nullptr;
                        break;
                    }
                    if (index + 1 == values.size())
                        complete = false;
                }
                if (!complete)
                    valid.store(false);
                release_managed(snapshot);
            }
        });
    }
    workers.emplace_back([client]() {
        for (int iteration = 0; iteration < 120; iteration++)
            rt_http_client_set_keep_alive(client, static_cast<int8_t>(iteration & 1));
    });
    workers.emplace_back([client]() {
        for (int iteration = 0; iteration < 120; iteration++)
            rt_http_client_set_pool_size(client, 1 + (iteration % 16));
    });
    for (std::thread &worker : workers)
        worker.join();

    rt_http_client_set_keep_alive(client, 1);
    void *final_snapshot = rt_http_client_get_cookies(client, domain);
    test_result(valid.load() && rt_map_len(final_snapshot) == 1 &&
                    rt_http_client_get_keep_alive(client) == 1,
                "concurrent snapshots and pool swaps preserve valid session state");
    release_managed(final_snapshot);
    for (rt_string value : values)
        release_managed((void *)value);
    release_managed((void *)name);
    release_managed((void *)domain);
    release_managed(client);
    test_result(rt_gc_tracked_count() == entry_baseline,
                "concurrent session mutation releases every superseded pool and snapshot");
}

/// @brief Run the focused HttpClient correctness suite.
/// @return Zero when every assertion passes.
int main() {
#if RT_PLATFORM_WINDOWS
    WSADATA winsock_data{};
    assert(WSAStartup(MAKEWORD(2, 2), &winsock_data) == 0);
#endif

    test_identity_and_constructor_cleanup();
    test_request_transaction_cleanup();
    test_cookie_snapshot_allocation_cleanup();
    test_transactional_and_concurrent_headers();
    test_concurrent_cookie_and_pool_state();

#if RT_PLATFORM_WINDOWS
    WSACleanup();
#endif
    std::printf("\nAll HttpClient tests passed.\n");
    return 0;
}
