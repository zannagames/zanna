//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTRestClientTests.cpp
// Purpose: Validate Zanna.Network.RestClient behavior.
// Key invariants:
//   - Local HTTP fixtures bind only to loopback addresses.
//   - Redirect and keep-alive state is captured deterministically.
// Ownership/Lifetime:
//   - Each helper owns and closes any native socket it creates.
//   - Runtime response objects remain valid for each test scope.
// Links: src/runtime/network/rt_restclient.c,
//        src/runtime/network/rt_http_server.c
//
//===----------------------------------------------------------------------===//

#include "tests/common/NetworkTestCompat.hpp"

#include "rt_gc.h"
#include "rt_http_server.h"
#include "rt_internal.h"
#include "rt_netutils.h"
#include "rt_network.h"
#include "rt_object.h"
#include "rt_restclient.h"
#include "rt_result.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "rt_trap.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <setjmp.h>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET local_sock_t;
#define LOCAL_SOCK_INVALID INVALID_SOCKET
#define LOCAL_SOCK_CLOSE(s) closesocket(s)
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int local_sock_t;
#define LOCAL_SOCK_INVALID (-1)
#define LOCAL_SOCK_CLOSE(s) close(s)
#endif

static void test_result(bool cond, const char *name) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", name);
        assert(false);
    }
}

static int rest_alloc_fail_countdown = 0;
static int rest_alloc_observed = 0;

/// @brief Count runtime-managed allocations and fail one selected boundary.
/// @details Native loopback fixtures are not active while this hook is
///          installed, so every observed allocation belongs to the RestClient
///          constructor, header transaction, or Result path under test.
/// @param bytes Requested allocation size.
/// @param next Runtime's default allocator.
/// @return Allocated storage, or NULL at the selected countdown.
static void *rest_countdown_alloc(int64_t bytes, void *(*next)(int64_t)) {
    rest_alloc_observed++;
    if (rest_alloc_fail_countdown > 0 && --rest_alloc_fail_countdown == 0)
        return nullptr;
    return next(bytes);
}

/// @brief Release one test-owned managed reference immediately at zero.
/// @param object Runtime object or String; NULL is a no-op.
static void release_test_object(void *object) {
    if (object && rt_obj_release_check0(object))
        rt_obj_free(object);
}

static std::atomic<bool> rest_server_ready{false};
static std::atomic<bool> rest_server_failed{false};
static std::atomic<int> rest_keepalive_accept_count{0};
static std::atomic<bool> rest_keepalive_reused{false};
static std::string rest_keepalive_connection_headers[2];
static std::string rest_redirect_target_authorization_header;
static std::string rest_redirect_target_api_key_header;
static std::string rest_redirect_location;
static std::string rest_atomic_header_value;
static std::mutex rest_atomic_header_lock;

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

static std::string read_http_header_value(void *client, const char *name) {
    std::string header_value;
    const size_t name_len = strlen(name);
    while (true) {
        rt_string line = rt_tcp_recv_line(client);
        const char *cstr = rt_string_cstr(line);
        const bool done = !cstr || *cstr == '\0';
        if (cstr && ascii_header_name_equals(cstr, name, name_len) && cstr[name_len] == ':') {
            const char *value = cstr + name_len + 1;
            while (*value == ' ' || *value == '\t')
                value++;
            header_value = value;
        }
        rt_string_unref(line);
        if (done)
            break;
    }
    return header_value;
}

static void send_text(void *client, const char *text) {
    rt_tcp_send_str(client, rt_const_cstr(text));
}

static void rest_redirect_source_handler(void *req_obj, void *res_obj) {
    (void)req_obj;
    rt_server_res_status(res_obj, 302);
    rt_server_res_header(
        res_obj, rt_const_cstr("Location"), rt_const_cstr(rest_redirect_location.c_str()));
    rt_server_res_send(res_obj, rt_const_cstr(""));
}

static void rest_redirect_target_handler(void *req_obj, void *res_obj) {
    rt_string authorization = rt_server_req_header(req_obj, rt_const_cstr("Authorization"));
    rt_string api_key = rt_server_req_header(req_obj, rt_const_cstr("X-API-Key"));
    rest_redirect_target_authorization_header = rt_string_cstr(authorization);
    rest_redirect_target_api_key_header = rt_string_cstr(api_key);
    rt_string_unref(authorization);
    rt_string_unref(api_key);
    rt_server_res_send(res_obj, rt_const_cstr("final"));
}

/// @brief Capture the transactional-header test value and return an empty 200.
/// @param req_obj ServerReq managed receiver.
/// @param res_obj ServerRes managed receiver.
static void rest_atomic_header_handler(void *req_obj, void *res_obj) {
    rt_string name = rt_const_cstr("X-Atomic");
    rt_string value = rt_server_req_header(req_obj, name);
    {
        std::lock_guard<std::mutex> guard(rest_atomic_header_lock);
        rest_atomic_header_value = rt_string_cstr(value);
    }
    rt_string_unref(value);
    rt_string_unref(name);
    rt_server_res_send(res_obj, rt_const_cstr(""));
}

static void wait_for_server() {
    for (int i = 0; i < 200 && !rest_server_ready.load(); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

static int get_bindable_local_port() {
    local_sock_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == LOCAL_SOCK_INVALID)
        return 0;

    int opt = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(zanna::tests::kIpv4LoopbackHostOrder);
    addr.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        LOCAL_SOCK_CLOSE(fd);
        return 0;
    }

    socklen_t addr_len = (socklen_t)sizeof(addr);
    if (getsockname(fd, (struct sockaddr *)&addr, &addr_len) != 0) {
        LOCAL_SOCK_CLOSE(fd);
        return 0;
    }

    const int port = ntohs(addr.sin_port);
    LOCAL_SOCK_CLOSE(fd);
    return port;
}

static void rest_keepalive_server_thread(int port) {
    void *server = rt_tcp_server_listen(port);
    if (!server) {
        rest_server_failed = true;
        rest_server_ready = true;
        return;
    }

    rest_server_failed = false;
    rest_server_ready = true;
    rest_keepalive_accept_count = 0;
    rest_keepalive_reused = false;
    rest_keepalive_connection_headers[0].clear();
    rest_keepalive_connection_headers[1].clear();

    void *client = rt_tcp_server_accept_for(server, 5000);
    if (!client) {
        rt_tcp_server_close(server);
        return;
    }

    rest_keepalive_accept_count = 1;
    rt_string request_line = rt_tcp_recv_line(client);
    rt_string_unref(request_line);
    rest_keepalive_connection_headers[0] = read_http_header_value(client, "Connection");
    send_text(client,
              "HTTP/1.1 200 OK\r\n"
              "Content-Length: 2\r\n"
              "Connection: keep-alive\r\n"
              "\r\n"
              "ok");

    void *second_client = rt_tcp_server_accept_for(server, 750);
    if (second_client) {
        rest_keepalive_accept_count = 2;
        request_line = rt_tcp_recv_line(second_client);
        rt_string_unref(request_line);
        rest_keepalive_connection_headers[1] = read_http_header_value(second_client, "Connection");
        send_text(second_client,
                  "HTTP/1.1 200 OK\r\n"
                  "Content-Length: 2\r\n"
                  "Connection: close\r\n"
                  "\r\n"
                  "ok");
        rt_tcp_close(second_client);
        rt_tcp_close(client);
        rt_tcp_server_close(server);
        return;
    }

    request_line = rt_tcp_recv_line(client);
    rt_string_unref(request_line);
    rest_keepalive_connection_headers[1] = read_http_header_value(client, "Connection");
    rest_keepalive_reused = true;
    send_text(client,
              "HTTP/1.1 200 OK\r\n"
              "Content-Length: 2\r\n"
              "Connection: close\r\n"
              "\r\n"
              "ok");
    rt_tcp_close(client);
    rt_tcp_server_close(server);
}

//=============================================================================
// Creation Tests
//=============================================================================

static void test_new_client() {
    void *client = rt_restclient_new(rt_const_cstr("https://api.example.com"));

    test_result(client != NULL, "new_client: should create client");

    rt_string base = rt_restclient_base_url(client);
    test_result(strcmp(rt_string_cstr(base), "https://api.example.com") == 0,
                "new_client: should store base URL");
}

static void test_new_client_empty_url() {
    void *client = rt_restclient_new(rt_const_cstr(""));

    test_result(client != NULL, "new_client_empty: should create client with empty URL");

    rt_string base = rt_restclient_base_url(client);
    test_result(strlen(rt_string_cstr(base)) == 0, "new_client_empty: should have empty base URL");
}

static void test_new_client_null_url() {
    void *client = rt_restclient_new(NULL);

    test_result(client != NULL, "new_client_null_url: should create client with empty base URL");

    rt_string base = rt_restclient_base_url(client);
    test_result(rt_str_len(base) == 0, "new_client_null_url: should have empty base URL");
}

static void test_new_client_null() {
    rt_string base = rt_restclient_base_url(NULL);
    test_result(strlen(rt_string_cstr(base)) == 0, "null_client: should return empty string");
}

/// @brief Verify stable identity, copied BaseUrl ownership, and constructor cleanup.
/// @details A count-only construction records every managed allocation after a
///          caller-owned base String is established. Each boundary is then
///          failed beneath an outer recovery frame and must restore the exact
///          tracked-object baseline. A forged Seq receiver must trap without
///          changing the Seq payload.
static void test_restclient_identity_and_constructor_allocation_cleanup() {
    rt_string base = rt_const_cstr("https://identity.example");
    const int64_t baseline = rt_gc_tracked_count();

    rest_alloc_fail_countdown = 0;
    rest_alloc_observed = 0;
    rt_set_alloc_hook(rest_countdown_alloc);
    void *client = rt_restclient_new(base);
    rt_set_alloc_hook(nullptr);
    const int allocation_count = rest_alloc_observed;
    test_result(client && rt_obj_class_id(client) == RT_RESTCLIENT_CLASS_ID,
                "identity: RestClient publishes its stable class ID");

    rt_string first_base = rt_restclient_base_url(client);
    release_test_object(first_base);
    rt_string second_base = rt_restclient_base_url(client);
    test_result(strcmp(rt_string_cstr(second_base), "https://identity.example") == 0,
                "identity: BaseUrl returns an independent caller-owned copy");
    release_test_object(second_base);
    release_test_object(client);
    test_result(rt_gc_tracked_count() == baseline,
                "constructor: count-only client releases to baseline");

    int trapped_count = 0;
    bool all_failures_clean = allocation_count > 0;
    for (int fail_at = 1; fail_at <= allocation_count; ++fail_at) {
        void *volatile failed_client = nullptr;
        volatile bool trapped = false;
        jmp_buf recovery;
        rest_alloc_fail_countdown = fail_at;
        rest_alloc_observed = 0;
        rt_trap_set_recovery(&recovery);
        if (setjmp(recovery) == 0) {
            rt_set_alloc_hook(rest_countdown_alloc);
            failed_client = rt_restclient_new(base);
            rt_set_alloc_hook(nullptr);
            rt_trap_clear_recovery();
        } else {
            rt_set_alloc_hook(nullptr);
            trapped = true;
            rt_trap_clear_recovery();
        }
        trapped_count += trapped ? 1 : 0;
        all_failures_clean = all_failures_clean && trapped && failed_client == nullptr;
        release_test_object((void *)failed_client);
        all_failures_clean = all_failures_clean && rt_gc_tracked_count() == baseline;
    }
    rest_alloc_fail_countdown = 0;
    test_result(all_failures_clean && trapped_count == allocation_count,
                "constructor: every managed allocation failure cleans partial state");

    void *wrong = rt_seq_new();
    volatile bool wrong_receiver_trapped = false;
    jmp_buf receiver_recovery;
    rt_trap_set_recovery(&receiver_recovery);
    if (setjmp(receiver_recovery) == 0) {
        (void)rt_restclient_get_keep_alive(wrong);
        rt_trap_clear_recovery();
    } else {
        wrong_receiver_trapped = true;
        rt_trap_clear_recovery();
    }
    test_result(wrong_receiver_trapped && rt_seq_len(wrong) == 0,
                "identity: forged receiver traps before RestClient payload access");
    release_test_object(wrong);
    release_test_object(base);
}

/// @brief Sweep Result.Err construction for an invalid RestClient receiver.
/// @details The stable-identity error path performs no network work, making its
///          managed allocation sequence deterministic. Every failed String or
///          Result allocation must propagate once and leave no partial graph.
static void test_restclient_result_allocation_cleanup() {
    void *wrong = rt_seq_new();
    rt_string path = rt_const_cstr("items");
    const int64_t baseline = rt_gc_tracked_count();

    rest_alloc_fail_countdown = 0;
    rest_alloc_observed = 0;
    rt_set_alloc_hook(rest_countdown_alloc);
    void *result = rt_restclient_get_result(wrong, path);
    rt_set_alloc_hook(nullptr);
    const int allocation_count = rest_alloc_observed;
    test_result(result && rt_result_is_err(result) == 1,
                "result: invalid receiver returns Result.Err");
    release_test_object(result);
    test_result(rt_gc_tracked_count() == baseline,
                "result: count-only error Result releases to baseline");

    bool all_failures_clean = allocation_count > 0;
    int trap_count = 0;
    for (int fail_at = 1; fail_at <= allocation_count; ++fail_at) {
        void *volatile failed_result = nullptr;
        volatile bool trapped = false;
        jmp_buf recovery;
        rest_alloc_fail_countdown = fail_at;
        rt_trap_set_recovery(&recovery);
        if (setjmp(recovery) == 0) {
            rt_set_alloc_hook(rest_countdown_alloc);
            failed_result = rt_restclient_get_result(wrong, path);
            rt_set_alloc_hook(nullptr);
            rt_trap_clear_recovery();
        } else {
            rt_set_alloc_hook(nullptr);
            trapped = true;
            rt_trap_clear_recovery();
        }
        trap_count += trapped ? 1 : 0;
        all_failures_clean = all_failures_clean && trapped && failed_result == nullptr;
        release_test_object((void *)failed_result);
        all_failures_clean = all_failures_clean && rt_gc_tracked_count() == baseline;
    }
    rest_alloc_fail_countdown = 0;
    test_result(all_failures_clean && trap_count == allocation_count,
                "result: every construction allocation failure releases partial values");
    release_test_object(path);
    release_test_object(wrong);
}

//=============================================================================
// Header Configuration Tests
//=============================================================================

static void test_set_header() {
    void *client = rt_restclient_new(rt_const_cstr("https://api.example.com"));

    // Setting a header shouldn't crash
    rt_restclient_set_header(
        client, rt_const_cstr("X-Custom-Header"), rt_const_cstr("CustomValue"));
    rt_restclient_set_header(client, NULL, rt_const_cstr("Ignored"));
    rt_restclient_set_header(client, rt_const_cstr("X-Ignored"), NULL);
    rt_string bad_name = rt_string_from_bytes("X-Bad\0Name", 10);
    rt_string bad_value = rt_string_from_bytes("bad\r\nvalue", 10);
    rt_restclient_set_header(client, bad_name, rt_const_cstr("Ignored"));
    rt_restclient_set_header(client, rt_const_cstr("X-Bad"), bad_value);
    rt_restclient_del_header(client, NULL);
    rt_restclient_del_header(client, bad_name);
    rt_string_unref(bad_name);
    rt_string_unref(bad_value);

    test_result(true, "set_header: should set header without crash");
}

static void test_del_header() {
    void *client = rt_restclient_new(rt_const_cstr("https://api.example.com"));

    rt_restclient_set_header(
        client, rt_const_cstr("X-Custom-Header"), rt_const_cstr("CustomValue"));

    // Deleting a header shouldn't crash
    rt_restclient_del_header(client, rt_const_cstr("X-Custom-Header"));

    test_result(true, "del_header: should delete header without crash");
}

static void test_null_client_headers() {
    // Operations on NULL client should be safe (no-op)
    rt_restclient_set_header(NULL, rt_const_cstr("Header"), rt_const_cstr("Value"));
    rt_restclient_del_header(NULL, rt_const_cstr("Header"));

    test_result(true, "null_client_headers: should handle NULL safely");
}

/// @brief Verify transactional replacement and synchronized concurrent headers.
/// @details The first managed allocation in a case-insensitive replacement is
///          failed after an old value exists; a loopback request must still
///          observe that old value. Multiple threads then replace the same
///          header repeatedly, and the next request must observe one complete
///          published value rather than a missing, torn, or duplicate entry.
static void test_restclient_transactional_and_concurrent_headers() {
    const int port = get_bindable_local_port();
    if (port <= 0) {
        printf("SKIPPED: restclient_transactional_headers (local bind unavailable)\n");
        return;
    }

    void *server = rt_http_server_new(port);
    rt_string handler_name = rt_const_cstr("atomic-header");
    rt_string route = rt_const_cstr("/value");
    rt_http_server_bind_handler(server, handler_name, (void *)&rest_atomic_header_handler);
    rt_http_server_get(server, route, handler_name);
    rt_http_server_start(server);
    if (rt_http_server_is_running(server) != 1) {
        release_test_object(route);
        release_test_object(handler_name);
        release_test_object(server);
        printf("SKIPPED: restclient_transactional_headers (server start failed)\n");
        return;
    }

    char base_buffer[128];
    snprintf(base_buffer,
             sizeof(base_buffer),
             "http://127.0.0.1:%lld",
             (long long)rt_http_server_port(server));
    rt_string base = rt_const_cstr(base_buffer);
    rt_string name = rt_const_cstr("X-Atomic");
    rt_string request_path = rt_const_cstr("value");
    rt_string old_value = rt_const_cstr("preserved-old");
    rt_string new_value = rt_const_cstr("rejected-new");
    void *client = rt_restclient_new(base);
    rt_restclient_set_header(client, name, old_value);

    bool replacement_trapped = false;
    jmp_buf recovery;
    rest_alloc_fail_countdown = 1;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        rt_set_alloc_hook(rest_countdown_alloc);
        rt_restclient_set_header(client, name, new_value);
        rt_set_alloc_hook(nullptr);
        rt_trap_clear_recovery();
    } else {
        rt_set_alloc_hook(nullptr);
        replacement_trapped = true;
        rt_trap_clear_recovery();
    }
    rest_alloc_fail_countdown = 0;

    void *first_response = rt_restclient_get(client, request_path);
    std::string first_value;
    {
        std::lock_guard<std::mutex> guard(rest_atomic_header_lock);
        first_value = rest_atomic_header_value;
    }
    test_result(replacement_trapped && first_value == "preserved-old",
                "headers: failed replacement preserves the previous complete value");
    release_test_object(first_response);

    constexpr int kThreadCount = 6;
    constexpr int kIterations = 40;
    std::vector<rt_string> values;
    std::vector<std::thread> workers;
    for (int i = 0; i < kThreadCount; i++) {
        char value_buffer[32];
        snprintf(value_buffer, sizeof(value_buffer), "thread-%d", i);
        values.push_back(rt_const_cstr(value_buffer));
    }
    for (int i = 0; i < kThreadCount; i++) {
        workers.emplace_back([client, name, value = values[(size_t)i]]() {
            for (int iteration = 0; iteration < kIterations; iteration++)
                rt_restclient_set_header(client, name, value);
        });
    }
    for (std::thread &worker : workers)
        worker.join();

    void *second_response = rt_restclient_get(client, request_path);
    std::string concurrent_value;
    {
        std::lock_guard<std::mutex> guard(rest_atomic_header_lock);
        concurrent_value = rest_atomic_header_value;
    }
    bool complete_value = false;
    for (rt_string value : values) {
        if (concurrent_value == rt_string_cstr(value))
            complete_value = true;
    }
    test_result(rt_http_res_status(second_response) == 200 && complete_value,
                "headers: concurrent replacements publish one complete value");

    rt_http_server_stop(server);
    release_test_object(second_response);
    for (rt_string value : values)
        release_test_object((void *)value);
    release_test_object(client);
    release_test_object(new_value);
    release_test_object(old_value);
    release_test_object(request_path);
    release_test_object(name);
    release_test_object(base);
    release_test_object(route);
    release_test_object(handler_name);
    release_test_object(server);
}

//=============================================================================
// Authentication Tests
//=============================================================================

static void test_set_auth_bearer() {
    void *client = rt_restclient_new(rt_const_cstr("https://api.example.com"));

    // Setting bearer auth shouldn't crash
    rt_restclient_set_auth_bearer(client, rt_const_cstr("my-token-12345"));

    test_result(true, "set_auth_bearer: should set bearer auth without crash");
}

static void test_set_auth_basic() {
    void *client = rt_restclient_new(rt_const_cstr("https://api.example.com"));

    // Setting basic auth shouldn't crash
    rt_restclient_set_auth_basic(client, rt_const_cstr("username"), rt_const_cstr("password"));

    test_result(true, "set_auth_basic: should set basic auth without crash");
}

static void test_set_auth_null_values() {
    void *client = rt_restclient_new(rt_const_cstr("https://api.example.com"));

    rt_restclient_set_auth_bearer(client, NULL);
    rt_restclient_set_auth_basic(client, NULL, NULL);

    test_result(true, "set_auth_null_values: should treat NULL credentials as empty strings");
}

static void test_clear_auth() {
    void *client = rt_restclient_new(rt_const_cstr("https://api.example.com"));

    rt_restclient_set_auth_bearer(client, rt_const_cstr("token"));
    rt_restclient_clear_auth(client);

    test_result(true, "clear_auth: should clear auth without crash");
}

static void test_null_client_auth() {
    // Auth operations on NULL client should be safe
    rt_restclient_set_auth_bearer(NULL, rt_const_cstr("token"));
    rt_restclient_set_auth_basic(NULL, rt_const_cstr("user"), rt_const_cstr("pass"));
    rt_restclient_clear_auth(NULL);

    test_result(true, "null_client_auth: should handle NULL safely");
}

//=============================================================================
// Timeout Tests
//=============================================================================

static void test_set_timeout() {
    void *client = rt_restclient_new(rt_const_cstr("https://api.example.com"));

    rt_restclient_set_timeout(client, 60000); // 60 seconds

    test_result(true, "set_timeout: should set timeout without crash");
}

static void test_set_timeout_null() {
    rt_restclient_set_timeout(NULL, 5000);

    test_result(true, "set_timeout_null: should handle NULL safely");
}

static void test_keepalive_defaults() {
    void *client = rt_restclient_new(rt_const_cstr("https://api.example.com"));
    test_result(rt_restclient_get_keep_alive(client) == 1,
                "keepalive_default: should default to enabled");
}

static void test_keepalive_toggle_and_pool_resize() {
    void *client = rt_restclient_new(rt_const_cstr("https://api.example.com"));
    rt_restclient_set_keep_alive(client, 0);
    test_result(rt_restclient_get_keep_alive(client) == 0,
                "keepalive_toggle: should disable keepalive");
    rt_restclient_set_pool_size(client, 2);
    rt_restclient_set_keep_alive(client, 1);
    test_result(rt_restclient_get_keep_alive(client) == 1,
                "keepalive_toggle: should re-enable keepalive");
}

//=============================================================================
// Status Tests (without actual HTTP)
//=============================================================================

static void test_last_status_initial() {
    void *client = rt_restclient_new(rt_const_cstr("https://api.example.com"));

    int64_t status = rt_restclient_last_status(client);
    test_result(status == 0, "last_status_initial: should be 0 initially");
}

static void test_last_response_initial() {
    void *client = rt_restclient_new(rt_const_cstr("https://api.example.com"));

    void *response = rt_restclient_last_response(client);
    test_result(response == NULL, "last_response_initial: should be NULL initially");
}

static void test_last_ok_initial() {
    void *client = rt_restclient_new(rt_const_cstr("https://api.example.com"));

    int8_t ok = rt_restclient_last_ok(client);
    test_result(ok == 0, "last_ok_initial: should be false initially");
}

static void test_last_status_null() {
    int64_t status = rt_restclient_last_status(NULL);
    test_result(status == 0, "last_status_null: should return 0 for NULL");
}

static void test_last_response_null() {
    void *response = rt_restclient_last_response(NULL);
    test_result(response == NULL, "last_response_null: should return NULL for NULL");
}

static void test_last_ok_null() {
    int8_t ok = rt_restclient_last_ok(NULL);
    test_result(ok == 0, "last_ok_null: should return false for NULL");
}

static void test_get_result_null_client() {
    void *result = rt_restclient_get_result(NULL, rt_const_cstr("items"));
    test_result(rt_result_is_err(result) == 1, "get_result_null_client: should return Err");
}

//=============================================================================
// Lifecycle / Finalizer Tests
//=============================================================================

/// @brief Regression test for RC-1: finalizer must be registered for every client.
///
/// Creating many clients exercises the allocation path and ensures no crash
/// occurs from missing or double-registered finalizers. Under ASAN, any
/// finalizer bug surfaces as a leak or invalid-free report.
static void test_restclient_many_instances() {
    const int COUNT = 100;

    for (int i = 0; i < COUNT; i++) {
        void *c = rt_restclient_new(rt_const_cstr("https://api.example.com"));
        test_result(c != NULL, "many_instances: client created");

        // Exercise all post-init paths that the finalizer must clean up
        rt_restclient_set_header(c, rt_const_cstr("X-Index"), rt_const_cstr("val"));
        rt_restclient_set_auth_bearer(c, rt_const_cstr("tok"));
        rt_restclient_set_timeout(c, 5000);

        // Verify state is coherent
        rt_string base = rt_restclient_base_url(c);
        test_result(strcmp(rt_string_cstr(base), "https://api.example.com") == 0,
                    "many_instances: base URL preserved");
        test_result(rt_restclient_last_status(c) == 0, "many_instances: last_status initially 0");
    }

    test_result(true, "many_instances: all 100 instances created without crash");
}

static void test_restclient_keepalive_reuse() {
    const int port = (int)rt_netutils_get_free_port();
    if (port <= 0) {
        printf("SKIPPED: restclient_keepalive_reuse (local bind unavailable)\n");
        return;
    }

    rest_server_ready = false;
    rest_server_failed = false;
    std::thread server(rest_keepalive_server_thread, port);
    wait_for_server();
    if (rest_server_failed) {
        server.join();
        printf("SKIPPED: restclient_keepalive_reuse (local bind unavailable)\n");
        return;
    }

    char base_url[128];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d", port);

    void *client = rt_restclient_new(rt_const_cstr(base_url));
    test_result(rt_restclient_get_keep_alive(client) == 1,
                "rest_keepalive: keepalive should default on");

    void *result1 = rt_restclient_get_result(client, rt_const_cstr("items"));
    test_result(rt_result_is_ok(result1) == 1, "rest_keepalive: GetResult should succeed");
    void *res1 = rt_result_unwrap(result1);
    test_result(rt_http_res_status(res1) == 200, "rest_keepalive: first request should succeed");

    void *res2 = rt_restclient_get(client, rt_const_cstr("items"));
    test_result(rt_http_res_status(res2) == 200, "rest_keepalive: second request should succeed");

    server.join();

    test_result(rest_keepalive_reused.load(), "rest_keepalive: requests should reuse one socket");
    test_result(rest_keepalive_accept_count.load() == 1,
                "rest_keepalive: second accept should not be needed");
    test_result(rest_keepalive_connection_headers[0].find("keep-alive") != std::string::npos,
                "rest_keepalive: first request should advertise keepalive");
    test_result(rest_keepalive_connection_headers[1].find("keep-alive") != std::string::npos,
                "rest_keepalive: second request should advertise keepalive");
}

static void test_restclient_cross_origin_redirect_strips_sensitive_headers() {
    const int target_port = get_bindable_local_port();
    const int redirect_port = get_bindable_local_port();
    if (target_port <= 0 || redirect_port <= 0) {
        printf("SKIPPED: restclient_cross_origin_redirect (local bind unavailable)\n");
        return;
    }

    void *target_server = rt_http_server_new(target_port);
    void *redirect_server = rt_http_server_new(redirect_port);

    rt_http_server_bind_handler(
        target_server, rt_const_cstr("target"), (void *)&rest_redirect_target_handler);
    rt_http_server_bind_handler(
        redirect_server, rt_const_cstr("redirect"), (void *)&rest_redirect_source_handler);
    rt_http_server_get(target_server, rt_const_cstr("/target"), rt_const_cstr("target"));
    rt_http_server_get(redirect_server, rt_const_cstr("/redirect"), rt_const_cstr("redirect"));
    rt_http_server_start(target_server);
    rt_http_server_start(redirect_server);

    if (!(rt_http_server_is_running(target_server) == 1 &&
          rt_http_server_is_running(redirect_server) == 1 &&
          rt_http_server_port(target_server) > 0 && rt_http_server_port(redirect_server) > 0)) {
        rt_http_server_stop(redirect_server);
        rt_http_server_stop(target_server);
        printf("SKIPPED: restclient_cross_origin_redirect (local bind unavailable)\n");
        return;
    }

    char location_buf[128];
    snprintf(location_buf,
             sizeof(location_buf),
             "http://127.0.0.1:%lld/target",
             (long long)rt_http_server_port(target_server));
    rest_redirect_location = location_buf;
    rest_redirect_target_authorization_header.clear();
    rest_redirect_target_api_key_header.clear();

    char base_url[128];
    snprintf(base_url,
             sizeof(base_url),
             "http://127.0.0.1:%lld",
             (long long)rt_http_server_port(redirect_server));

    void *client = rt_restclient_new(rt_const_cstr(base_url));
    rt_restclient_set_header(
        client, rt_const_cstr("Authorization"), rt_const_cstr("Bearer rest-secret"));
    rt_restclient_set_header(client, rt_const_cstr("X-API-Key"), rt_const_cstr("rest-api-key"));

    void *res = rt_restclient_get(client, rt_const_cstr("redirect"));
    test_result(rt_http_res_status(res) == 200,
                "rest_redirect: request should follow cross-origin redirect");

    rt_http_server_stop(redirect_server);
    rt_http_server_stop(target_server);

    test_result(rest_redirect_target_authorization_header.empty(),
                "rest_redirect: should strip Authorization across origins");
    test_result(rest_redirect_target_api_key_header.empty(),
                "rest_redirect: should strip API key header across origins");
}

//=============================================================================
// Main
//=============================================================================

int main() {
    // Creation tests
    test_new_client();
    test_new_client_empty_url();
    test_new_client_null_url();
    test_new_client_null();
    test_restclient_identity_and_constructor_allocation_cleanup();
    test_restclient_result_allocation_cleanup();

    // Header tests
    test_set_header();
    test_del_header();
    test_null_client_headers();
    test_restclient_transactional_and_concurrent_headers();

    // Auth tests
    test_set_auth_bearer();
    test_set_auth_basic();
    test_set_auth_null_values();
    test_clear_auth();
    test_null_client_auth();

    // Timeout tests
    test_set_timeout();
    test_set_timeout_null();
    test_keepalive_defaults();
    test_keepalive_toggle_and_pool_resize();

    // Status tests
    test_last_status_initial();
    test_last_response_initial();
    test_last_ok_initial();
    test_last_status_null();
    test_last_response_null();
    test_last_ok_null();
    test_get_result_null_client();

    // Lifecycle / finalizer regression (RC-1)
    test_restclient_many_instances();
    test_restclient_keepalive_reuse();
    test_restclient_cross_origin_redirect_strips_sensitive_headers();

    printf("All RestClient tests passed!\n");
    return 0;
}
