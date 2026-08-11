//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTNetworkRuntimeTests.c
// Purpose: Regression tests for networking runtime hardening fixes.
// Key invariants:
//   - Parser probes use deterministic native byte fixtures without external services.
//   - Allocation injection restores hooks and exact managed-object baselines.
//   - Forged handles trap before private payload or native lock access.
// Ownership/Lifetime:
//   - Every test releases caller-owned runtime objects after its assertions.
// Links: src/runtime/network/, src/runtime/core/rt_gc.c,
//        docs/zannalib/network.md
//
//===----------------------------------------------------------------------===//

#include "rt_gc.h"
#include "rt_http_router.h"
#include "rt_http_server.h"
#include "rt_internal.h"
#include "rt_map.h"
#include "rt_network.h"
#include "rt_object.h"
#include "rt_platform.h"
#include "rt_retry.h"
#include "rt_smtp.h"
#include "rt_sse.h"
#include "rt_string.h"
#include "rt_tls.h"
#include "rt_ws_server.h"
#include "rt_wss_server.h"

#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// -- Trap interception (VDOC-142 registration-validation tests) -------------
static jmp_buf g_trap_jmp;
static int g_trap_expected = 0;
static const char *g_last_trap = NULL;

void vm_trap(const char *msg) {
    g_last_trap = msg;
    if (g_trap_expected)
        longjmp(g_trap_jmp, 1);
    fprintf(stderr, "UNEXPECTED TRAP: %s\n", msg ? msg : "(null)");
    exit(1);
}

/// Expect `expr` to trap; capture the message and continue.
#define EXPECT_TRAP(expr)                                                                          \
    do {                                                                                           \
        g_trap_expected = 1;                                                                       \
        g_last_trap = NULL;                                                                        \
        if (setjmp(g_trap_jmp) == 0) {                                                             \
            expr;                                                                                  \
            ASSERT(0 && "Expected trap did not occur");                                            \
        }                                                                                          \
        g_trap_expected = 0;                                                                       \
    } while (0)

static int tests_run = 0;
static int tests_failed = 0;

static int g_router_alloc_calls = 0;
static int g_router_alloc_fail_countdown = 0;

/// @brief Count router managed allocations and optionally fail one exact call.
/// @details The runtime allocation hook observes every allocation routed through
///          rt_alloc, including managed object payloads and a Map's bucket and
///          entry storage. Native route metadata deliberately remains outside
///          this hook, allowing the tests to prove an unmatched route scan
///          performs zero runtime allocations and to inject a failure at every
///          result-construction allocation.
/// @param bytes Requested managed allocation size.
/// @param next Default runtime allocator used when this call is not failed.
/// @return Delegated allocation, or NULL on the armed countdown call.
static void *router_probe_alloc(int64_t bytes, void *(*next)(int64_t bytes)) {
    g_router_alloc_calls++;
    if (g_router_alloc_fail_countdown > 0 && --g_router_alloc_fail_countdown == 0)
        return NULL;
    return next(bytes);
}

#define ASSERT(cond)                                                                               \
    do {                                                                                           \
        tests_run++;                                                                               \
        if (!(cond)) {                                                                             \
            tests_failed++;                                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                        \
        }                                                                                          \
    } while (0)

extern int rt_http_server_test_parse_request(const char *raw,
                                             size_t raw_len,
                                             char **method_out,
                                             char **path_out,
                                             char **body_out,
                                             size_t *body_len_out);
extern char *rt_http_server_test_build_response(int status_code,
                                                const char *body,
                                                size_t body_len,
                                                const char **header_names,
                                                const char **header_values,
                                                size_t header_count,
                                                size_t *out_len);
extern int rt_http_parse_url_for_test(
    const char *url_str, char **host_out, int *port_out, char **path_out, int *use_tls_out);
extern int rt_ws_parse_url_for_test(
    const char *url, int *is_secure, char **host, int *port, char **path);
extern int rt_ws_validate_handshake_response_for_test(const char *response, const char *key_copy);
extern char *rt_ws_compute_accept_key(const char *key_cstr);
extern char *rt_ws_format_host_header_for_test(const char *host, int port, int is_secure);
extern int rt_ws_close_code_valid_for_test(int code);
extern int rt_http_header_name_valid_for_test(const char *name);
extern int rt_http_transfer_encoding_supported_for_test(const char *value, int *chunked_out);

static char captured_method[32];
static char captured_path[64];
static char captured_query[32];
static char captured_header[32];
static char captured_header_bytes[64];
static char captured_param[32];
static char captured_body[64];
static char captured_body_bytes[64];
static int64_t captured_header_len = 0;
static int64_t captured_body_len = 0;

static void copy_rt_string(char *dst, size_t cap, rt_string value) {
    const char *cstr = value ? rt_string_cstr(value) : NULL;
    if (!dst || cap == 0)
        return;
    if (!cstr) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, cap, "%s", cstr);
}

static void reset_captured_request_state(void) {
    captured_method[0] = '\0';
    captured_path[0] = '\0';
    captured_query[0] = '\0';
    captured_header[0] = '\0';
    memset(captured_header_bytes, 0, sizeof(captured_header_bytes));
    captured_param[0] = '\0';
    captured_body[0] = '\0';
    captured_header_len = 0;
    memset(captured_body_bytes, 0, sizeof(captured_body_bytes));
    captured_body_len = 0;
}

static void native_http_handler(void *req_obj, void *res_obj) {
    rt_string method = rt_server_req_method(req_obj);
    rt_string path = rt_server_req_path(req_obj);
    rt_string query = rt_server_req_query(req_obj, rt_const_cstr("q"));
    rt_string header = rt_server_req_header(req_obj, rt_const_cstr("X-Test"));
    rt_string param = rt_server_req_param(req_obj, rt_const_cstr("id"));
    rt_string body = rt_server_req_body(req_obj);

    copy_rt_string(captured_method, sizeof(captured_method), method);
    copy_rt_string(captured_path, sizeof(captured_path), path);
    copy_rt_string(captured_query, sizeof(captured_query), query);
    copy_rt_string(captured_header, sizeof(captured_header), header);
    copy_rt_string(captured_param, sizeof(captured_param), param);
    copy_rt_string(captured_body, sizeof(captured_body), body);
    captured_header_len = rt_str_len(header);
    if (captured_header_len > 0 && captured_header_len <= (int64_t)sizeof(captured_header_bytes)) {
        memcpy(captured_header_bytes, rt_string_cstr(header), (size_t)captured_header_len);
    }
    captured_body_len = rt_str_len(body);
    if (captured_body_len > 0 && captured_body_len <= (int64_t)sizeof(captured_body_bytes)) {
        memcpy(captured_body_bytes, rt_string_cstr(body), (size_t)captured_body_len);
    }

    rt_string_unref(method);
    rt_string_unref(path);
    rt_string_unref(query);
    rt_string_unref(header);
    rt_string_unref(param);
    rt_string_unref(body);

    rt_server_res_status(res_obj, 201);
    rt_server_res_header(res_obj, rt_const_cstr("X-Handled"), rt_const_cstr("yes"));
    rt_server_res_send(res_obj, rt_const_cstr("native-ok"));
}

static void test_http_router_returns_owned_params_and_trims_wildcards(void) {
    void *router = rt_http_router_new();
    ASSERT(rt_obj_class_id(router) == RT_HTTP_ROUTER_CLASS_ID);
    rt_http_router_get(router, rt_const_cstr("/users/:id"));
    rt_http_router_get(router, rt_const_cstr("/static/*path"));

    void *match = rt_http_router_match(router, rt_const_cstr("GET"), rt_const_cstr("/users/42"));
    ASSERT(match != NULL);
    ASSERT(rt_obj_class_id(match) == RT_ROUTE_MATCH_CLASS_ID);
    rt_string id = rt_route_match_param(match, rt_const_cstr("id"));
    if (rt_obj_release_check0(match))
        rt_obj_free(match);
    ASSERT(id && strcmp(rt_string_cstr(id), "42") == 0);
    rt_string_unref(id);

    match = rt_http_router_match(router, rt_const_cstr("GET"), rt_const_cstr("/static/a/b/"));
    ASSERT(match != NULL);
    rt_string path = rt_route_match_param(match, rt_const_cstr("path"));
    if (rt_obj_release_check0(match))
        rt_obj_free(match);
    ASSERT(path && strcmp(rt_string_cstr(path), "a/b") == 0);
    rt_string_unref(path);

    if (rt_obj_release_check0(router))
        rt_obj_free(router);
}

/// @brief Router grammar validation and method-token semantics (VDOC-142):
///        wildcards must be terminal, capture names unique, methods compared
///        case-sensitively.
static void test_http_router_grammar_validation(void) {
    void *router = rt_http_router_new();

    // Non-terminal wildcards and duplicate capture names are rejected at
    // registration instead of silently mismatching at dispatch time.
    EXPECT_TRAP(rt_http_router_get(router, rt_const_cstr("/a/*x/c")));
    ASSERT(g_last_trap && strstr(g_last_trap, "wildcard") != NULL);
    EXPECT_TRAP(rt_http_router_get(router, rt_const_cstr("/a/:id/b/:id")));
    ASSERT(g_last_trap && strstr(g_last_trap, "duplicate") != NULL);
    EXPECT_TRAP(rt_http_router_get(router, rt_const_cstr("/a/:")));
    ASSERT(g_last_trap && strstr(g_last_trap, "requires a name") != NULL);
    EXPECT_TRAP(rt_http_router_get(router, rt_const_cstr("/a/*")));
    ASSERT(g_last_trap && strstr(g_last_trap, "requires a name") != NULL);
    ASSERT(rt_http_router_count(router) == 0);

    // Embedded NUL in a pattern must reject the whole registration rather
    // than registering a truncated prefix (VDOC-161).
    rt_string nul_pattern = rt_string_from_bytes("/a\0/b", 5);
    EXPECT_TRAP(rt_http_router_get(router, nul_pattern));
    rt_string_unref(nul_pattern);
    ASSERT(rt_http_router_count(router) == 0);

    // Empty, whitespace-bearing, and control-bearing methods are not valid
    // HTTP tokens. Extension punctuation from the token alphabet remains
    // available for custom methods.
    EXPECT_TRAP(rt_http_router_add(router, rt_const_cstr(""), rt_const_cstr("/empty")));
    ASSERT(g_last_trap && strstr(g_last_trap, "method token") != NULL);
    EXPECT_TRAP(rt_http_router_add(router, rt_const_cstr("BAD METHOD"), rt_const_cstr("/bad")));
    ASSERT(g_last_trap && strstr(g_last_trap, "method token") != NULL);
    EXPECT_TRAP(rt_http_router_add(router, rt_const_cstr("BAD\tMETHOD"), rt_const_cstr("/bad")));
    ASSERT(g_last_trap && strstr(g_last_trap, "method token") != NULL);
    ASSERT(rt_http_router_count(router) == 0);

    rt_http_router_add(router, rt_const_cstr("M-SEARCH"), rt_const_cstr("/discover"));
    void *extension_match =
        rt_http_router_match(router, rt_const_cstr("M-SEARCH"), rt_const_cstr("/discover"));
    ASSERT(extension_match != NULL);
    if (extension_match && rt_obj_release_check0(extension_match))
        rt_obj_free(extension_match);

    // Method tokens are case-sensitive per HTTP semantics.
    rt_http_router_add(router, rt_const_cstr("get"), rt_const_cstr("/lower"));
    void *match = rt_http_router_match(router, rt_const_cstr("GET"), rt_const_cstr("/lower"));
    ASSERT(match == NULL);
    match = rt_http_router_match(router, rt_const_cstr("get"), rt_const_cstr("/lower"));
    ASSERT(match != NULL);
    if (rt_obj_release_check0(match))
        rt_obj_free(match);

    if (rt_obj_release_check0(router))
        rt_obj_free(router);
}

/// @brief Router lookup must not allocate for routes that fail to match.
/// @details A large same-method table previously allocated and destroyed one
///          Map per candidate. The probe asserts a complete miss allocates
///          nothing, while successful literal and capture matches allocate
///          only their result state after the selected route is known. A
///          one-capture match performs six runtime allocations: Map payload,
///          Map buckets, name string, value string, inline-key Map entry, and
///          RouteMatch payload.
static void test_http_router_match_allocation_profile(void) {
    void *router = rt_http_router_new();
    for (int i = 0; i < 256; i++) {
        char pattern_bytes[48];
        int written = snprintf(pattern_bytes, sizeof(pattern_bytes), "/literal/%d", i);
        ASSERT(written > 0 && (size_t)written < sizeof(pattern_bytes));
        rt_string pattern = rt_string_from_bytes(pattern_bytes, (size_t)written);
        rt_http_router_get(router, pattern);
        rt_string_unref(pattern);
    }

    rt_string get = rt_const_cstr("GET");
    rt_string missing = rt_const_cstr("/literal/missing");
    rt_string literal = rt_const_cstr("/literal/255");
    g_router_alloc_calls = 0;
    g_router_alloc_fail_countdown = 0;
    rt_set_alloc_hook(router_probe_alloc);
    void *match = rt_http_router_match(router, get, missing);
    rt_set_alloc_hook(NULL);
    ASSERT(match == NULL);
    ASSERT(g_router_alloc_calls == 0);

    g_router_alloc_calls = 0;
    rt_set_alloc_hook(router_probe_alloc);
    match = rt_http_router_match(router, get, literal);
    rt_set_alloc_hook(NULL);
    ASSERT(match != NULL);
    ASSERT(g_router_alloc_calls == 1);
    if (match && rt_obj_release_check0(match))
        rt_obj_free(match);

    rt_http_router_get(router, rt_const_cstr("/captured/:id"));
    rt_string captured = rt_const_cstr("/captured/42");
    g_router_alloc_calls = 0;
    rt_set_alloc_hook(router_probe_alloc);
    match = rt_http_router_match(router, get, captured);
    rt_set_alloc_hook(NULL);
    ASSERT(match != NULL);
    ASSERT(g_router_alloc_calls == 6);
    if (match && rt_obj_release_check0(match))
        rt_obj_free(match);

    if (rt_obj_release_check0(router))
        rt_obj_free(router);
}

/// @brief Allocation traps during parameter/result construction must clean up
///        fully and leave the router lock usable.
/// @details Failure positions cover the parameter Map payload and buckets,
///          capture-name string, capture-value string, inline-key Map entry,
///          and RouteMatch payload. Every trap is raised after the read lock has
///          been released; subsequent Count, Add, and Match calls prove no lock
///          was stranded. The GC tracking count verifies the partially built
///          Map is not leaked.
static void test_http_router_match_oom_is_transactional(void) {
    void *router = rt_http_router_new();
    rt_http_router_get(router, rt_const_cstr("/oom/:id"));
    rt_string method = rt_const_cstr("GET");
    rt_string path = rt_const_cstr("/oom/7");
    int64_t tracked_baseline = rt_gc_tracked_count();

    for (int failure_position = 1; failure_position <= 6; failure_position++) {
        g_router_alloc_calls = 0;
        g_router_alloc_fail_countdown = failure_position;
        rt_set_alloc_hook(router_probe_alloc);
        EXPECT_TRAP(rt_http_router_match(router, method, path));
        rt_set_alloc_hook(NULL);
        ASSERT(g_router_alloc_fail_countdown == 0);
        ASSERT(g_last_trap && strstr(g_last_trap, "alloc") != NULL);
        ASSERT(rt_gc_tracked_count() == tracked_baseline);
        ASSERT(rt_http_router_count(router) == 1);
    }

    rt_http_router_get(router, rt_const_cstr("/still-usable"));
    ASSERT(rt_http_router_count(router) == 2);
    void *match = rt_http_router_match(router, method, path);
    ASSERT(match != NULL);
    if (match && rt_obj_release_check0(match))
        rt_obj_free(match);

    if (rt_obj_release_check0(router))
        rt_obj_free(router);
}

/// @brief Parameter captures require one path segment while pattern-side
///        repeated-slash normalization remains deliberate and stable.
static void test_http_router_empty_segment_semantics(void) {
    void *router = rt_http_router_new();
    rt_http_router_get(router, rt_const_cstr("/required/:id"));
    rt_http_router_get(router, rt_const_cstr("/normalized//path/"));

    void *match = rt_http_router_match(router, rt_const_cstr("GET"), rt_const_cstr("/required//"));
    ASSERT(match == NULL);
    match = rt_http_router_match(router, rt_const_cstr("GET"), rt_const_cstr("/normalized/path/"));
    ASSERT(match != NULL);
    ASSERT(match && rt_route_match_index(match) == 1);
    if (match && rt_obj_release_check0(match))
        rt_obj_free(match);

    if (rt_obj_release_check0(router))
        rt_obj_free(router);
}

/// @brief Route registration must be transactional (VDOC-144): a rejected
///        handler tag leaves no ghost route in the embedded router.
static void test_http_server_route_registration_atomic(void) {
    void *server = rt_http_server_new(0);

    // Empty handler tag is rejected before anything is committed.
    EXPECT_TRAP(rt_http_server_get(server, rt_const_cstr("/ghost"), rt_const_cstr("")));
    ASSERT(g_last_trap && strstr(g_last_trap, "handler tag") != NULL);

    // The rejected pattern must not be routable: a request for it gets a
    // clean 404 rather than a ghost-route dispatch into a missing handler.
    rt_string wire = (rt_string)rt_http_server_process_request(
        server, rt_const_cstr("GET /ghost HTTP/1.1\r\nHost: x\r\n\r\n"));
    ASSERT(wire != NULL);
    ASSERT(strncmp(rt_string_cstr(wire), "HTTP/1.1 404", 12) == 0);
    rt_string_unref(wire);

    if (rt_obj_release_check0(server))
        rt_obj_free(server);
}

static void test_http_server_parses_exact_body(void) {
    const char raw[] = "POST /submit?q=1 HTTP/1.1\r\n"
                       "Host: example.test\r\n"
                       "Content-Length: 5\r\n"
                       "\r\n"
                       "hello";
    char *method = NULL;
    char *path = NULL;
    char *body = NULL;
    size_t body_len = 0;

    ASSERT(rt_http_server_test_parse_request(raw, strlen(raw), &method, &path, &body, &body_len) ==
           1);
    ASSERT(method && strcmp(method, "POST") == 0);
    ASSERT(path && strcmp(path, "/submit") == 0);
    ASSERT(body && strcmp(body, "hello") == 0);
    ASSERT(body_len == 5);

    free(method);
    free(path);
    free(body);
}

static void test_http_server_rejects_invalid_http_version(void) {
    const char raw[] = "GET / HTTP/2.0\r\n"
                       "Host: example.test\r\n"
                       "\r\n";
    ASSERT(rt_http_server_test_parse_request(raw, strlen(raw), NULL, NULL, NULL, NULL) == 0);
}

static void test_http_server_rejects_invalid_content_length(void) {
    const char raw[] = "POST /x HTTP/1.1\r\n"
                       "Content-Length: -1\r\n"
                       "\r\n";
    ASSERT(rt_http_server_test_parse_request(raw, strlen(raw), NULL, NULL, NULL, NULL) == 0);
}

static void test_http_server_rejects_invalid_method_and_target(void) {
    const char bad_method[] = "GE\tT / HTTP/1.1\r\n"
                              "Host: example.test\r\n"
                              "\r\n";
    const char bad_target[] = "GET relative HTTP/1.1\r\n"
                              "Host: example.test\r\n"
                              "\r\n";
    ASSERT(rt_http_server_test_parse_request(
               bad_method, sizeof(bad_method) - 1, NULL, NULL, NULL, NULL) == 0);
    ASSERT(rt_http_server_test_parse_request(
               bad_target, sizeof(bad_target) - 1, NULL, NULL, NULL, NULL) == 0);
}

static void test_http_server_normalizes_absolute_form_target(void) {
    const char raw[] = "GET http://example.test/proxy?q=1 HTTP/1.1\r\n"
                       "Host: example.test\r\n"
                       "\r\n";
    char *method = NULL;
    char *path = NULL;
    ASSERT(rt_http_server_test_parse_request(raw, strlen(raw), &method, &path, NULL, NULL) == 1);
    ASSERT(method && strcmp(method, "GET") == 0);
    ASSERT(path && strcmp(path, "/proxy") == 0);
    free(method);
    free(path);
}

static void test_http_server_rejects_truncated_body(void) {
    const char raw[] = "POST /x HTTP/1.1\r\n"
                       "Content-Length: 5\r\n"
                       "\r\n"
                       "hi";
    ASSERT(rt_http_server_test_parse_request(raw, strlen(raw), NULL, NULL, NULL, NULL) == 0);
}

static void test_http_server_rejects_duplicate_content_length(void) {
    const char raw[] = "POST /x HTTP/1.1\r\n"
                       "Content-Length: 3\r\n"
                       "Content-Length: 3\r\n"
                       "\r\n"
                       "hey";
    ASSERT(rt_http_server_test_parse_request(raw, strlen(raw), NULL, NULL, NULL, NULL) == 0);
}

static void test_http_server_parses_chunked_request_body(void) {
    const char raw[] = "POST /stream HTTP/1.1\r\n"
                       "Transfer-Encoding: chunked\r\n"
                       "\r\n"
                       "5\r\nhello\r\n0\r\n\r\n";
    char *method = NULL;
    char *path = NULL;
    char *body = NULL;
    size_t body_len = 0;

    ASSERT(rt_http_server_test_parse_request(raw, strlen(raw), &method, &path, &body, &body_len) ==
           1);
    ASSERT(method && strcmp(method, "POST") == 0);
    ASSERT(path && strcmp(path, "/stream") == 0);
    ASSERT(body && strcmp(body, "hello") == 0);
    ASSERT(body_len == 5);

    free(method);
    free(path);
    free(body);
}

static void test_http_server_rejects_unsupported_transfer_encoding(void) {
    const char gzip[] = "POST /stream HTTP/1.1\r\n"
                        "Transfer-Encoding: gzip\r\n"
                        "\r\n";
    const char gzip_chunked[] = "POST /stream HTTP/1.1\r\n"
                                "Transfer-Encoding: gzip, chunked\r\n"
                                "\r\n"
                                "0\r\n\r\n";
    const char chunked_gzip[] = "POST /stream HTTP/1.1\r\n"
                                "Transfer-Encoding: chunked, gzip\r\n"
                                "\r\n"
                                "0\r\n\r\n";
    const char duplicate_chunked[] = "POST /stream HTTP/1.1\r\n"
                                     "Transfer-Encoding: chunked, chunked\r\n"
                                     "\r\n"
                                     "0\r\n\r\n";
    const char trailing_comma[] = "POST /stream HTTP/1.1\r\n"
                                  "Transfer-Encoding: chunked,\r\n"
                                  "\r\n"
                                  "0\r\n\r\n";

    ASSERT(rt_http_server_test_parse_request(gzip, sizeof(gzip) - 1, NULL, NULL, NULL, NULL) == 0);
    ASSERT(rt_http_server_test_parse_request(
               gzip_chunked, sizeof(gzip_chunked) - 1, NULL, NULL, NULL, NULL) == 0);
    ASSERT(rt_http_server_test_parse_request(
               chunked_gzip, sizeof(chunked_gzip) - 1, NULL, NULL, NULL, NULL) == 0);
    ASSERT(rt_http_server_test_parse_request(
               duplicate_chunked, sizeof(duplicate_chunked) - 1, NULL, NULL, NULL, NULL) == 0);
    ASSERT(rt_http_server_test_parse_request(
               trailing_comma, sizeof(trailing_comma) - 1, NULL, NULL, NULL, NULL) == 0);
}

static void test_http_server_test_parse_preserves_nul_body(void) {
    const char raw[] = "POST /binary HTTP/1.1\r\n"
                       "Content-Length: 4\r\n"
                       "\r\n"
                       "a\0bc";
    const char expected[] = {'a', '\0', 'b', 'c'};
    char *body = NULL;
    size_t body_len = 0;

    ASSERT(rt_http_server_test_parse_request(raw, sizeof(raw) - 1, NULL, NULL, &body, &body_len) ==
           1);
    ASSERT(body_len == sizeof(expected));
    ASSERT(body != NULL && memcmp(body, expected, sizeof(expected)) == 0);
    free(body);
}

static void test_http_client_transfer_encoding_validation(void) {
    int chunked = 0;
    ASSERT(rt_http_transfer_encoding_supported_for_test("chunked", &chunked) == 1);
    ASSERT(chunked == 1);
    chunked = 0;
    ASSERT(rt_http_transfer_encoding_supported_for_test("  Chunked\t", &chunked) == 1);
    ASSERT(chunked == 1);
    ASSERT(rt_http_transfer_encoding_supported_for_test("gzip", &chunked) == 0);
    ASSERT(rt_http_transfer_encoding_supported_for_test("gzip, chunked", &chunked) == 0);
    ASSERT(rt_http_transfer_encoding_supported_for_test("chunked, gzip", &chunked) == 0);
    ASSERT(rt_http_transfer_encoding_supported_for_test("chunked, chunked", &chunked) == 0);
    ASSERT(rt_http_transfer_encoding_supported_for_test("chunked,", &chunked) == 0);
    ASSERT(rt_http_transfer_encoding_supported_for_test(", chunked", &chunked) == 0);
}

static void test_http_server_builds_large_header_block(void) {
    enum { header_count = 40 };

    const char *body = "{\"ok\":true}";
    const char *header_names[header_count];
    const char *header_values[header_count];
    char name_storage[header_count][24];
    char value_storage[header_count][40];

    for (int i = 0; i < header_count; i++) {
        snprintf(name_storage[i], sizeof(name_storage[i]), "X-Test-%02d", i);
        snprintf(value_storage[i], sizeof(value_storage[i]), "value-%02d-abcdefghijklmnop", i);
        header_names[i] = name_storage[i];
        header_values[i] = value_storage[i];
    }

    size_t resp_len = 0;
    char *resp = rt_http_server_test_build_response(
        200, body, strlen(body), header_names, header_values, header_count, &resp_len);
    ASSERT(resp != NULL);
    if (resp) {
        ASSERT(resp_len > strlen(body));
        ASSERT(strstr(resp, "X-Test-39: value-39-abcdefghijklmnop\r\n") != NULL);
        ASSERT(strstr(resp, "Content-Length: 11\r\n") != NULL);
        ASSERT(resp_len >= strlen(body));
        ASSERT(memcmp(resp + resp_len - strlen(body), body, strlen(body)) == 0);
    }
    free(resp);
}

static void test_http_server_response_filters_managed_and_injected_headers(void) {
    const char *header_names[] = {"Connection", "X-Good", "X-Bad"};
    const char *header_values[] = {"keep-alive", "ok", "evil\r\nX-Injected: yes"};
    size_t resp_len = 0;
    char *resp =
        rt_http_server_test_build_response(200, "{}", 2, header_names, header_values, 3, &resp_len);
    ASSERT(resp != NULL);
    if (resp) {
        // The builder returns bytes + length, NOT a C string; strstr on the
        // raw buffer reads past the allocation (ASan-caught). Search a
        // NUL-terminated copy instead.
        char *resp_z = (char *)malloc(resp_len + 1);
        ASSERT(resp_z != NULL);
        if (resp_z) {
            memcpy(resp_z, resp, resp_len);
            resp_z[resp_len] = '\0';
            ASSERT(strstr(resp_z, "Connection: keep-alive\r\n") == NULL);
            ASSERT(strstr(resp_z, "X-Good: ok\r\n") != NULL);
            ASSERT(strstr(resp_z, "X-Injected: yes\r\n") == NULL);
            free(resp_z);
        }
    }
    free(resp);
}

static void test_http_server_executes_bound_native_handler(void) {
    reset_captured_request_state();

    void *server = rt_http_server_new(8080);
    rt_http_server_post(server, rt_const_cstr("/users/:id"), rt_const_cstr("handle_user"));
    rt_http_server_bind_handler(
        server, rt_const_cstr("handle_user"), RT_FN_PTR_CAST((void *)&native_http_handler));

    rt_string raw = rt_const_cstr("POST /users/42?q=search HTTP/1.1\r\n"
                                  "Host: example.test\r\n"
                                  "X-Test: abc\r\n"
                                  "Content-Length: 5\r\n"
                                  "\r\n"
                                  "hello");
    rt_string response = (rt_string)rt_http_server_process_request(server, raw);
    const char *response_cstr = rt_string_cstr(response);

    ASSERT(response_cstr != NULL);
    ASSERT(strstr(response_cstr, "HTTP/1.1 201 Created\r\n") != NULL);
    ASSERT(strstr(response_cstr, "X-Handled: yes\r\n") != NULL);
    ASSERT(strstr(response_cstr, "\r\n\r\nnative-ok") != NULL);
    ASSERT(strcmp(captured_method, "POST") == 0);
    ASSERT(strcmp(captured_path, "/users/42") == 0);
    ASSERT(strcmp(captured_query, "search") == 0);
    ASSERT(strcmp(captured_header, "abc") == 0);
    ASSERT(strcmp(captured_param, "42") == 0);
    ASSERT(strcmp(captured_body, "hello") == 0);

    rt_string_unref(response);
    if (rt_obj_release_check0(server))
        rt_obj_free(server);
}

static void test_http_server_accessor_preserves_nul_header_and_query_key_length(void) {
    reset_captured_request_state();

    void *server = rt_http_server_new(8086);
    rt_http_server_post(server, rt_const_cstr("/users/:id"), rt_const_cstr("handle_user"));
    rt_http_server_bind_handler(
        server, rt_const_cstr("handle_user"), RT_FN_PTR_CAST((void *)&native_http_handler));

    const char raw[] = "POST /users/42?q%00x=bad&q=good HTTP/1.1\r\n"
                       "Host: example.test\r\n"
                       "X-Test: a\0b\r\n"
                       "Content-Length: 0\r\n"
                       "\r\n";
    const char expected_header[] = {'a', '\0', 'b'};
    rt_string raw_str = rt_string_from_bytes(raw, sizeof(raw) - 1);
    rt_string response = (rt_string)rt_http_server_process_request(server, raw_str);
    const char *response_cstr = rt_string_cstr(response);

    ASSERT(response_cstr != NULL);
    ASSERT(strstr(response_cstr, "HTTP/1.1 201 Created\r\n") != NULL);
    ASSERT(strcmp(captured_query, "good") == 0);
    ASSERT(captured_header_len == (int64_t)sizeof(expected_header));
    ASSERT(memcmp(captured_header_bytes, expected_header, sizeof(expected_header)) == 0);

    rt_string_unref(response);
    rt_string_unref(raw_str);
    if (rt_obj_release_check0(server))
        rt_obj_free(server);
}

static void test_http_server_process_request_preserves_nul_body_and_decodes_query_key(void) {
    reset_captured_request_state();

    void *server = rt_http_server_new(8085);
    rt_http_server_post(server, rt_const_cstr("/users/:id"), rt_const_cstr("handle_user"));
    rt_http_server_bind_handler(
        server, rt_const_cstr("handle_user"), RT_FN_PTR_CAST((void *)&native_http_handler));

    const char raw[] = "POST /users/42?%71=search HTTP/1.1\r\n"
                       "Host: example.test\r\n"
                       "X-Test: abc\r\n"
                       "Content-Length: 4\r\n"
                       "\r\n"
                       "hi\0!";
    const char expected_body[] = {'h', 'i', '\0', '!'};
    rt_string raw_str = rt_string_from_bytes(raw, sizeof(raw) - 1);
    rt_string response = (rt_string)rt_http_server_process_request(server, raw_str);
    const char *response_cstr = rt_string_cstr(response);

    ASSERT(response_cstr != NULL);
    ASSERT(strstr(response_cstr, "HTTP/1.1 201 Created\r\n") != NULL);
    ASSERT(strcmp(captured_query, "search") == 0);
    ASSERT(captured_body_len == 4);
    ASSERT(memcmp(captured_body_bytes, expected_body, sizeof(expected_body)) == 0);

    rt_string_unref(response);
    rt_string_unref(raw_str);
    if (rt_obj_release_check0(server))
        rt_obj_free(server);
}

static void test_http_server_executes_bound_native_handler_for_chunked_body(void) {
    reset_captured_request_state();

    void *server = rt_http_server_new(8082);
    rt_http_server_post(server, rt_const_cstr("/stream/:id"), rt_const_cstr("handle_stream"));
    rt_http_server_bind_handler(
        server, rt_const_cstr("handle_stream"), RT_FN_PTR_CAST((void *)&native_http_handler));

    rt_string raw = rt_const_cstr("POST /stream/7?q=resume HTTP/1.1\r\n"
                                  "Host: example.test\r\n"
                                  "X-Test: xyz\r\n"
                                  "Transfer-Encoding: chunked\r\n"
                                  "\r\n"
                                  "2\r\nhe\r\n"
                                  "3\r\nllo\r\n"
                                  "0\r\n\r\n");
    rt_string response = (rt_string)rt_http_server_process_request(server, raw);
    const char *response_cstr = rt_string_cstr(response);

    ASSERT(response_cstr != NULL);
    ASSERT(strstr(response_cstr, "HTTP/1.1 201 Created\r\n") != NULL);
    ASSERT(strstr(response_cstr, "\r\n\r\nnative-ok") != NULL);
    ASSERT(strcmp(captured_method, "POST") == 0);
    ASSERT(strcmp(captured_path, "/stream/7") == 0);
    ASSERT(strcmp(captured_query, "resume") == 0);
    ASSERT(strcmp(captured_header, "xyz") == 0);
    ASSERT(strcmp(captured_param, "7") == 0);
    ASSERT(strcmp(captured_body, "hello") == 0);

    rt_string_unref(response);
    if (rt_obj_release_check0(server))
        rt_obj_free(server);
}

static void test_http_server_http10_defaults_to_close(void) {
    void *server = rt_http_server_new(8083);
    rt_http_server_get(server, rt_const_cstr("/ping"), rt_const_cstr("handle_ping"));
    rt_http_server_bind_handler(
        server, rt_const_cstr("handle_ping"), RT_FN_PTR_CAST((void *)&native_http_handler));

    rt_string raw = rt_const_cstr("GET /ping HTTP/1.0\r\n"
                                  "Host: example.test\r\n"
                                  "\r\n");
    rt_string response = (rt_string)rt_http_server_process_request(server, raw);
    const char *response_cstr = rt_string_cstr(response);

    ASSERT(response_cstr != NULL);
    ASSERT(strstr(response_cstr, "Connection: close\r\n") != NULL);

    rt_string_unref(response);
    if (rt_obj_release_check0(server))
        rt_obj_free(server);
}

static void test_http_server_http10_keepalive_opt_in(void) {
    void *server = rt_http_server_new(8084);
    rt_http_server_get(server, rt_const_cstr("/ping"), rt_const_cstr("handle_ping"));
    rt_http_server_bind_handler(
        server, rt_const_cstr("handle_ping"), RT_FN_PTR_CAST((void *)&native_http_handler));

    rt_string raw = rt_const_cstr("GET /ping HTTP/1.0\r\n"
                                  "Host: example.test\r\n"
                                  "Connection: keep-alive\r\n"
                                  "\r\n");
    rt_string response = (rt_string)rt_http_server_process_request(server, raw);
    const char *response_cstr = rt_string_cstr(response);

    ASSERT(response_cstr != NULL);
    ASSERT(strstr(response_cstr, "Connection: keep-alive\r\n") != NULL);

    rt_string_unref(response);
    if (rt_obj_release_check0(server))
        rt_obj_free(server);
}

static void test_http_server_reports_missing_handler_binding(void) {
    void *server = rt_http_server_new(8081);
    rt_http_server_get(server, rt_const_cstr("/missing"), rt_const_cstr("missing_handler"));

    rt_string raw = rt_const_cstr("GET /missing HTTP/1.1\r\nHost: example.test\r\n\r\n");
    rt_string response = (rt_string)rt_http_server_process_request(server, raw);
    const char *response_cstr = rt_string_cstr(response);

    ASSERT(response_cstr != NULL);
    ASSERT(strstr(response_cstr, "HTTP/1.1 500 Internal Server Error\r\n") != NULL);
    ASSERT(strstr(response_cstr, "{\"error\":\"Handler not registered\"}") != NULL);

    rt_string_unref(response);
    if (rt_obj_release_check0(server))
        rt_obj_free(server);
}

static void test_http_parse_url_accepts_ipv6_literal(void) {
    char *host = NULL;
    char *path = NULL;
    int port = 0;
    int use_tls = 0;

    ASSERT(rt_http_parse_url_for_test(
               "https://[2001:db8::1]:8443/api?q=1", &host, &port, &path, &use_tls) == 1);
    ASSERT(host && strcmp(host, "2001:db8::1") == 0);
    ASSERT(port == 8443);
    ASSERT(path && strcmp(path, "/api?q=1") == 0);
    ASSERT(use_tls == 1);

    free(host);
    free(path);
}

static void test_http_parse_url_rejects_crlf_injection(void) {
    char *host = NULL;
    char *path = NULL;
    int port = 0;
    int use_tls = 0;
    ASSERT(rt_http_parse_url_for_test(
               "http://example.test/\r\nX-Evil: yes", &host, &port, &path, &use_tls) == 0);
    free(host);
    free(path);
}

static void test_http_parse_url_rejects_bad_ports(void) {
    char *host = NULL;
    char *path = NULL;
    int port = 0;
    int use_tls = 0;
    ASSERT(rt_http_parse_url_for_test(
               "http://example.test:80junk/path", &host, &port, &path, &use_tls) == 0);
    free(host);
    free(path);
    host = NULL;
    path = NULL;
    ASSERT(rt_http_parse_url_for_test(
               "http://example.test:70000/path", &host, &port, &path, &use_tls) == 0);
    free(host);
    free(path);
}

static void test_http_parse_url_accepts_case_insensitive_scheme(void) {
    char *host = NULL;
    char *path = NULL;
    int port = 0;
    int use_tls = 0;

    ASSERT(rt_http_parse_url_for_test(
               "HTTPS://example.test:9443/path?q=1", &host, &port, &path, &use_tls) == 1);
    ASSERT(host && strcmp(host, "example.test") == 0);
    ASSERT(port == 9443);
    ASSERT(path && strcmp(path, "/path?q=1") == 0);
    ASSERT(use_tls == 1);
    free(host);
    free(path);
}

static void test_http_parse_url_rejects_malformed_ipv6_authority(void) {
    char *host = NULL;
    char *path = NULL;
    int port = 0;
    int use_tls = 0;

    ASSERT(rt_http_parse_url_for_test("http://[::1]junk/path", &host, &port, &path, &use_tls) == 0);
    free(host);
    free(path);
    host = NULL;
    path = NULL;
    ASSERT(rt_http_parse_url_for_test("http://example.test:/path", &host, &port, &path, &use_tls) ==
           0);
    free(host);
    free(path);
}

static void test_http_header_name_validation_rejects_invalid_fields(void) {
    ASSERT(rt_http_header_name_valid_for_test("X-Test") == 1);
    ASSERT(rt_http_header_name_valid_for_test("Bad:Name") == 0);
    ASSERT(rt_http_header_name_valid_for_test("Bad Name") == 0);
    ASSERT(rt_http_header_name_valid_for_test("") == 0);
}

static void test_http_server_rejects_invalid_request_header_names(void) {
    const char bad_space[] = "GET / HTTP/1.1\r\n"
                             "Bad Header: value\r\n"
                             "\r\n";
    const char missing_colon[] = "GET / HTTP/1.1\r\n"
                                 "BrokenHeader\r\n"
                                 "\r\n";
    ASSERT(rt_http_server_test_parse_request(
               bad_space, sizeof(bad_space) - 1, NULL, NULL, NULL, NULL) == 0);
    ASSERT(rt_http_server_test_parse_request(
               missing_colon, sizeof(missing_colon) - 1, NULL, NULL, NULL, NULL) == 0);
}

static void test_ws_parse_url_accepts_ipv6_literal(void) {
    int secure = 0;
    int port = 0;
    char *host = NULL;
    char *path = NULL;

    ASSERT(rt_ws_parse_url_for_test("ws://[::1]:9000/chat", &secure, &host, &port, &path) == 1);
    ASSERT(secure == 0);
    ASSERT(host && strcmp(host, "::1") == 0);
    ASSERT(port == 9000);
    ASSERT(path && strcmp(path, "/chat") == 0);

    free(host);
    free(path);
}

static void test_ws_parse_url_rejects_bad_authorities(void) {
    int secure = 0;
    int port = 0;
    char *host = NULL;
    char *path = NULL;

    ASSERT(rt_ws_parse_url_for_test("ws://:80/chat", &secure, &host, &port, &path) == 0);
    free(host);
    free(path);
    host = NULL;
    path = NULL;

    ASSERT(rt_ws_parse_url_for_test(
               "ws://example.test:80junk/chat", &secure, &host, &port, &path) == 0);
    free(host);
    free(path);
    host = NULL;
    path = NULL;

    ASSERT(rt_ws_parse_url_for_test("ws://example.test:70000/chat", &secure, &host, &port, &path) ==
           0);
    free(host);
    free(path);
}

static void test_ws_parse_url_accepts_case_insensitive_scheme(void) {
    int secure = 0;
    int port = 0;
    char *host = NULL;
    char *path = NULL;

    ASSERT(rt_ws_parse_url_for_test("WSS://example.test/chat", &secure, &host, &port, &path) == 1);
    ASSERT(secure == 1);
    ASSERT(port == 443);
    ASSERT(host && strcmp(host, "example.test") == 0);
    ASSERT(path && strcmp(path, "/chat") == 0);
    free(host);
    free(path);
}

static void test_ws_parse_url_rejects_request_injection(void) {
    int secure = 0;
    int port = 0;
    char *host = NULL;
    char *path = NULL;

    ASSERT(rt_ws_parse_url_for_test(
               "ws://example.test/\r\nX-Evil: yes", &secure, &host, &port, &path) == 0);
    free(host);
    free(path);
}

static void test_ws_handshake_validation_accepts_valid_response(void) {
    static const char key[] = "dGhlIHNhbXBsZSBub25jZQ==";
    char *accept = rt_ws_compute_accept_key(key);
    ASSERT(accept != NULL);
    if (!accept)
        return;

    char response[512];
    snprintf(response,
             sizeof(response),
             "HTTP/1.1 101 Switching Protocols\r\n"
             "Upgrade: websocket\r\n"
             "Connection: keep-alive, Upgrade\r\n"
             "Sec-WebSocket-Accept: %s\r\n"
             "\r\n",
             accept);

    ASSERT(rt_ws_validate_handshake_response_for_test(response, key) == 1);
    free(accept);
}

static void test_ws_handshake_validation_rejects_spurious_101(void) {
    static const char key[] = "dGhlIHNhbXBsZSBub25jZQ==";
    char *accept = rt_ws_compute_accept_key(key);
    ASSERT(accept != NULL);
    if (!accept)
        return;

    char response[512];
    snprintf(response,
             sizeof(response),
             "HTTP/1.1 200 OK\r\n"
             "X-Debug: 101 here\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Accept: %s\r\n"
             "\r\n",
             accept);

    ASSERT(rt_ws_validate_handshake_response_for_test(response, key) == 0);
    free(accept);
}

static void test_ws_handshake_validation_rejects_unsolicited_subprotocol(void) {
    static const char key[] = "dGhlIHNhbXBsZSBub25jZQ==";
    char *accept = rt_ws_compute_accept_key(key);
    ASSERT(accept != NULL);
    if (!accept)
        return;

    char response[512];
    snprintf(response,
             sizeof(response),
             "HTTP/1.1 101 Switching Protocols\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Accept: %s\r\n"
             "Sec-WebSocket-Protocol: chat.v1\r\n"
             "\r\n",
             accept);

    ASSERT(rt_ws_validate_handshake_response_for_test(response, key) == 0);
    free(accept);
}

/// @brief Verify strict status, singleton-field, and header-grammar handling.
/// @details The valid case includes trailing optional whitespace around the
///          accept value. Invalid cases exercise a prefix-confusable status,
///          duplicate singleton accept fields, and obsolete folded input.
static void test_ws_handshake_validation_rejects_ambiguous_responses(void) {
    static const char key[] = "dGhlIHNhbXBsZSBub25jZQ==";
    char *accept = rt_ws_compute_accept_key(key);
    ASSERT(accept != NULL);
    if (!accept)
        return;

    char response[1024];
    snprintf(response,
             sizeof(response),
             "HTTP/1.1 101 Switching Protocols\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Accept: %s \t\r\n"
             "\r\n",
             accept);
    ASSERT(rt_ws_validate_handshake_response_for_test(response, key) == 1);

    snprintf(response,
             sizeof(response),
             "HTTP/1.1 1010 Confusable\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Accept: %s\r\n"
             "\r\n",
             accept);
    ASSERT(rt_ws_validate_handshake_response_for_test(response, key) == 0);

    snprintf(response,
             sizeof(response),
             "HTTP/1.1 101 Switching Protocols\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Accept: %s\r\n"
             "Sec-WebSocket-Accept: %s\r\n"
             "\r\n",
             accept,
             accept);
    ASSERT(rt_ws_validate_handshake_response_for_test(response, key) == 0);

    snprintf(response,
             sizeof(response),
             "HTTP/1.1 101 Switching Protocols\r\n"
             "Upgrade: websocket\r\n"
             " Connection: Upgrade\r\n"
             "Sec-WebSocket-Accept: %s\r\n"
             "\r\n",
             accept);
    ASSERT(rt_ws_validate_handshake_response_for_test(response, key) == 0);
    free(accept);
}

static void test_ws_host_header_formatting(void) {
    char *header = rt_ws_format_host_header_for_test("example.com", 80, 0);
    ASSERT(header != NULL);
    ASSERT(header && strcmp(header, "example.com") == 0);
    free(header);

    header = rt_ws_format_host_header_for_test("example.com", 8443, 1);
    ASSERT(header != NULL);
    ASSERT(header && strcmp(header, "example.com:8443") == 0);
    free(header);

    header = rt_ws_format_host_header_for_test("::1", 443, 1);
    ASSERT(header != NULL);
    ASSERT(header && strcmp(header, "[::1]") == 0);
    free(header);

    header = rt_ws_format_host_header_for_test("::1", 9000, 0);
    ASSERT(header != NULL);
    ASSERT(header && strcmp(header, "[::1]:9000") == 0);
    free(header);
}

static void test_ws_close_code_validation(void) {
    ASSERT(rt_ws_close_code_valid_for_test(1000) == 1);
    ASSERT(rt_ws_close_code_valid_for_test(1014) == 1);
    ASSERT(rt_ws_close_code_valid_for_test(3000) == 1);
    ASSERT(rt_ws_close_code_valid_for_test(4999) == 1);
    ASSERT(rt_ws_close_code_valid_for_test(999) == 0);
    ASSERT(rt_ws_close_code_valid_for_test(1004) == 0);
    ASSERT(rt_ws_close_code_valid_for_test(1005) == 0);
    ASSERT(rt_ws_close_code_valid_for_test(1006) == 0);
    ASSERT(rt_ws_close_code_valid_for_test(1015) == 0);
    ASSERT(rt_ws_close_code_valid_for_test(5000) == 0);
}

/// @brief Verify WsServer objects have stable identity and reject forged receivers.
/// @details Construction does not start network resources, so this probe is
///          deterministic in restricted test environments. Each forged call
///          must trap before reading the undersized payload or a mutex field.
static void test_ws_server_stable_identity(void) {
    void *server = rt_ws_server_new(0);
    ASSERT(server != NULL);
    ASSERT(server && rt_obj_class_id(server) == RT_WS_SERVER_CLASS_ID);
    ASSERT(server && rt_ws_server_port(server) == 0);

    void *forged = rt_obj_new_i64(INT64_C(0x123456), 1);
    ASSERT(forged != NULL);
    EXPECT_TRAP((void)rt_ws_server_port(forged));
    EXPECT_TRAP((void)rt_ws_server_client_count(forged));
    EXPECT_TRAP(rt_ws_server_broadcast(forged, NULL));

    if (forged && rt_obj_release_check0(forged))
        rt_obj_free(forged);
    if (server && rt_obj_release_check0(server))
        rt_obj_free(server);
}

/// @brief Fail the first managed allocation of both WebSocket server constructors.
/// @details The plain constructor must not publish a partial mutex-bearing
///          object. WSS validates borrowed credential Strings first, then must
///          likewise leave the exact tracked-object baseline when its server
///          payload allocation fails; no credential file is opened at this boundary.
static void test_websocket_server_constructor_allocation_transactions(void) {
    const int64_t baseline = rt_gc_tracked_count();
    void *volatile result = NULL;

    g_router_alloc_fail_countdown = 1;
    rt_set_alloc_hook(router_probe_alloc);
    EXPECT_TRAP(result = rt_ws_server_new(0));
    rt_set_alloc_hook(NULL);
    ASSERT(result == NULL);
    ASSERT(g_router_alloc_fail_countdown == 0);
    ASSERT(rt_gc_tracked_count() == baseline);

    rt_string certificate = rt_const_cstr("missing-cert.pem");
    rt_string private_key = rt_const_cstr("missing-key.pem");
    const int64_t credential_baseline = rt_gc_tracked_count();
    result = NULL;
    g_router_alloc_fail_countdown = 1;
    rt_set_alloc_hook(router_probe_alloc);
    EXPECT_TRAP(result = rt_wss_server_new(0, certificate, private_key));
    rt_set_alloc_hook(NULL);
    ASSERT(result == NULL);
    ASSERT(g_router_alloc_fail_countdown == 0);
    ASSERT(rt_gc_tracked_count() == credential_baseline);
    rt_string_unref(certificate);
    rt_string_unref(private_key);
    ASSERT(rt_gc_tracked_count() == baseline);
}

/// @brief Verify SSE receiver identity and first-allocation constructor rollback.
/// @details Forged managed objects must trap before native lock or socket state
///          is interpreted. Failing the constructor's client payload allocation
///          must leave the exact GC tracking baseline without attempting DNS or
///          transport setup.
static void test_sse_identity_and_constructor_transaction(void) {
    void *forged = rt_obj_new_i64(INT64_C(0x123456), 1);
    ASSERT(forged != NULL);
    EXPECT_TRAP((void)rt_sse_is_open(forged));
    EXPECT_TRAP(rt_sse_close(forged));
    EXPECT_TRAP((void)rt_sse_last_event_id(forged));
    if (forged && rt_obj_release_check0(forged))
        rt_obj_free(forged);

    rt_string url = rt_const_cstr("http://127.0.0.1:1/events");
    const int64_t baseline = rt_gc_tracked_count();
    void *volatile result = NULL;
    g_router_alloc_fail_countdown = 1;
    rt_set_alloc_hook(router_probe_alloc);
    EXPECT_TRAP(result = rt_sse_connect(url));
    rt_set_alloc_hook(NULL);
    ASSERT(result == NULL);
    ASSERT(g_router_alloc_fail_countdown == 0);
    ASSERT(rt_gc_tracked_count() == baseline);
    rt_string_unref(url);
}

/// @brief Verify SMTP stable identity, forged-receiver rejection, and constructor rollback.
/// @details Construction allocates native locks and a host copy after the
///          managed payload. A forced failure of that first managed allocation
///          must leave the exact GC baseline. Forged objects must trap before
///          any native mutex, credential, diagnostic, or transport field is read.
static void test_smtp_identity_and_constructor_transaction(void) {
    rt_string host = rt_const_cstr("127.0.0.1");
    void *client = rt_smtp_new(host, 25);
    ASSERT(client != NULL);
    ASSERT(client && rt_obj_class_id(client) == RT_SMTP_CLASS_ID);
    if (client && rt_obj_release_check0(client))
        rt_obj_free(client);

    void *forged = rt_obj_new_i64(INT64_C(0x123456), 1);
    ASSERT(forged != NULL);
    EXPECT_TRAP((void)rt_smtp_last_error(forged));
    EXPECT_TRAP(rt_smtp_set_tls(forged, 1));
    EXPECT_TRAP(rt_smtp_set_auth(forged, NULL, NULL));
    EXPECT_TRAP((void)rt_smtp_send(forged, NULL, NULL, NULL, NULL));
    EXPECT_TRAP(rt_smtp_close(forged));
    if (forged && rt_obj_release_check0(forged))
        rt_obj_free(forged);

    const int64_t baseline = rt_gc_tracked_count();
    void *volatile result = NULL;
    g_router_alloc_fail_countdown = 1;
    rt_set_alloc_hook(router_probe_alloc);
    EXPECT_TRAP(result = rt_smtp_new(host, 25));
    rt_set_alloc_hook(NULL);
    ASSERT(result == NULL);
    ASSERT(g_router_alloc_fail_countdown == 0);
    ASSERT(rt_gc_tracked_count() == baseline);
    rt_string_unref(host);
}

static void test_url_decode_plus_semantics(void) {
    rt_string decoded = rt_url_decode(rt_const_cstr("a+b%20c"));
    ASSERT(decoded && strcmp(rt_string_cstr(decoded), "a+b c") == 0);
    rt_string_unref(decoded);

    void *map = rt_url_decode_query(rt_const_cstr("q=a+b"));
    rt_string value = rt_map_get_str(map, rt_const_cstr("q"));
    ASSERT(value && strcmp(rt_string_cstr(value), "a b") == 0);
    if (rt_obj_release_check0(map))
        rt_obj_free(map);
}

static void test_url_ipv6_building_and_authority_validation(void) {
    ASSERT(rt_url_is_valid(rt_const_cstr("http://[::1]junk/path")) == 0);
    ASSERT(rt_url_is_valid(rt_const_cstr("http://example.test:/path")) == 0);

    void *url = rt_url_new();
    rt_url_set_scheme(url, rt_const_cstr("HTTP"));
    rt_url_set_host(url, rt_const_cstr("::1"));
    rt_url_set_path(url, rt_const_cstr("/"));

    rt_string host_port = rt_url_host_port(url);
    rt_string authority = rt_url_authority(url);
    rt_string full = rt_url_full(url);
    ASSERT(host_port && strcmp(rt_string_cstr(host_port), "[::1]") == 0);
    ASSERT(authority && strcmp(rt_string_cstr(authority), "[::1]") == 0);
    ASSERT(full && strcmp(rt_string_cstr(full), "http://[::1]/") == 0);
    rt_string_unref(host_port);
    rt_string_unref(authority);
    rt_string_unref(full);
    if (rt_obj_release_check0(url))
        rt_obj_free(url);
}

static void test_retry_negative_delays_normalize_to_zero(void) {
    void *fixed = rt_retry_new(1, -100);
    void *expo = rt_retry_exponential(1, -100, -50);
    ASSERT(rt_retry_next_delay(fixed) == 0);
    ASSERT(rt_retry_next_delay(expo) == 0);
    if (rt_obj_release_check0(fixed))
        rt_obj_free(fixed);
    if (rt_obj_release_check0(expo))
        rt_obj_free(expo);
}

static void test_tls_extract_cn_uses_subject_not_issuer(void) {
    static const uint8_t cert[] = {0x30, 0x3E, 0x30, 0x3C, 0xA0, 0x03, 0x02, 0x01, 0x02, 0x02, 0x01,
                                   0x01, 0x30, 0x00, 0x30, 0x15, 0x31, 0x13, 0x30, 0x11, 0x06, 0x03,
                                   0x55, 0x04, 0x03, 0x0C, 0x0A, 'i',  's',  's',  'u',  'e',  'r',
                                   '.',  'c',  'o',  'm',  0x30, 0x00, 0x30, 0x17, 0x31, 0x15, 0x30,
                                   0x13, 0x06, 0x03, 0x55, 0x04, 0x03, 0x0C, 0x0C, 's',  'u',  'b',
                                   'j',  'e',  'c',  't',  '.',  't',  'e',  's',  't'};
    char cn[256] = {0};
    ASSERT(tls_extract_cn(cert, sizeof(cert), cn) == 1);
    ASSERT(strcmp(cn, "subject.test") == 0);
}

int main(void) {
    test_http_router_returns_owned_params_and_trims_wildcards();
    test_http_router_grammar_validation();
    test_http_router_match_allocation_profile();
    test_http_router_match_oom_is_transactional();
    test_http_router_empty_segment_semantics();
    test_http_server_route_registration_atomic();
    test_http_server_parses_exact_body();
    test_http_server_rejects_invalid_http_version();
    test_http_server_rejects_invalid_content_length();
    test_http_server_rejects_invalid_method_and_target();
    test_http_server_normalizes_absolute_form_target();
    test_http_server_rejects_truncated_body();
    test_http_server_rejects_duplicate_content_length();
    test_http_server_parses_chunked_request_body();
    test_http_server_rejects_unsupported_transfer_encoding();
    test_http_server_test_parse_preserves_nul_body();
    test_http_client_transfer_encoding_validation();
    test_http_server_builds_large_header_block();
    test_http_server_response_filters_managed_and_injected_headers();
    test_http_server_executes_bound_native_handler();
    test_http_server_accessor_preserves_nul_header_and_query_key_length();
    test_http_server_process_request_preserves_nul_body_and_decodes_query_key();
    test_http_server_executes_bound_native_handler_for_chunked_body();
    test_http_server_http10_defaults_to_close();
    test_http_server_http10_keepalive_opt_in();
    test_http_server_reports_missing_handler_binding();
    test_http_parse_url_accepts_ipv6_literal();
    test_http_parse_url_rejects_crlf_injection();
    test_http_parse_url_rejects_bad_ports();
    test_http_parse_url_accepts_case_insensitive_scheme();
    test_http_parse_url_rejects_malformed_ipv6_authority();
    test_http_header_name_validation_rejects_invalid_fields();
    test_http_server_rejects_invalid_request_header_names();
    test_ws_parse_url_accepts_ipv6_literal();
    test_ws_parse_url_rejects_bad_authorities();
    test_ws_parse_url_accepts_case_insensitive_scheme();
    test_ws_parse_url_rejects_request_injection();
    test_ws_handshake_validation_accepts_valid_response();
    test_ws_handshake_validation_rejects_spurious_101();
    test_ws_handshake_validation_rejects_unsolicited_subprotocol();
    test_ws_handshake_validation_rejects_ambiguous_responses();
    test_ws_host_header_formatting();
    test_ws_close_code_validation();
    test_ws_server_stable_identity();
    test_websocket_server_constructor_allocation_transactions();
    test_sse_identity_and_constructor_transaction();
    test_smtp_identity_and_constructor_transaction();
    test_url_decode_plus_semantics();
    test_url_ipv6_building_and_authority_validation();
    test_retry_negative_delays_normalize_to_zero();
    test_tls_extract_cn_uses_subject_not_issuer();

    printf("%d/%d tests passed\n", tests_run - tests_failed, tests_run);
    return tests_failed > 0 ? 1 : 0;
}
