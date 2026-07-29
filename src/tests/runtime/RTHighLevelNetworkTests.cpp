//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTHighLevelNetworkTests.cpp
// Purpose: Integration-style coverage for higher-level network APIs.
// Key invariants:
//   - Local network fixtures bind only to loopback addresses.
//   - Captured protocol state remains deterministic across tests.
// Ownership/Lifetime:
//   - Each helper owns and closes any native socket it creates.
//   - Runtime objects remain valid for each test scope.
// Links: src/runtime/network/rt_http_client.c,
//        src/runtime/network/rt_sse.c, src/runtime/network/rt_smtp.c,
//        docs/zannalib/network.md
//
//===----------------------------------------------------------------------===//

#include "tests/common/NetworkTestCompat.hpp"

#include "rt_bytes.h"
#include "rt_http2.h"
#include "rt_http_client.h"
#include "rt_http_router.h"
#include "rt_http_server.h"
#include "rt_https_server.h"
#include "rt_map.h"
#include "rt_netutils.h"
#include "rt_network.h"
#include "rt_object.h"
#include "rt_platform.h"
#include "rt_restclient.h"
#include "rt_result.h"
#include "rt_smtp.h"
#include "rt_sse.h"
#include "rt_string.h"
#include "rt_tls.h"
#include "rt_websocket.h"
#include "rt_ws_server.h"
#include "rt_wss_server.h"

#include <atomic>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cctype>
#include <chrono>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if RT_PLATFORM_WINDOWS
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

static void test_result(const char *name, bool passed) {
    printf("  %s: %s\n", name, passed ? "PASS" : "FAIL");
    assert(passed);
}

// -- Trap interception (validation-strictness tests) -------------------------
static jmp_buf g_trap_jmp;
static bool g_trap_expected = false;
static const char *g_last_trap = nullptr;

extern "C" void vm_trap(const char *msg) {
    g_last_trap = msg;
    if (g_trap_expected)
        longjmp(g_trap_jmp, 1);
    fprintf(stderr, "UNEXPECTED TRAP: %s\n", msg ? msg : "(null)");
    _exit(1);
}

/// Expect `expr` to trap; capture the message and continue.
#define EXPECT_TRAP(expr)                                                                          \
    do {                                                                                           \
        g_trap_expected = true;                                                                    \
        g_last_trap = nullptr;                                                                     \
        if (setjmp(g_trap_jmp) == 0) {                                                             \
            expr;                                                                                  \
            assert(false && "Expected trap did not occur");                                        \
        }                                                                                          \
        g_trap_expected = false;                                                                   \
    } while (0)

static std::atomic<bool> api_server_ready{false};
static std::atomic<bool> api_server_failed{false};
static std::atomic<int> http_keepalive_accept_count{0};
static std::atomic<bool> http_keepalive_reused{false};
static std::atomic<bool> smtp_fixture_client_accepted{false};
static std::atomic<bool> smtp_fixture_tls_client_hello{false};
static std::string smtp_captured_message;
static std::string http_cookie_headers[3];
static std::string http_keepalive_connection_headers[2];
static std::string sse_resume_header;
static std::string redirect_target_authorization_header;
static std::string redirect_target_api_key_header;
static std::string redirect_source_location;
static std::atomic<unsigned> tls_fixture_counter{0};

static const char *LOCALHOST_TEST_KEY_PEM = R"PEM(-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQg+Z1xhQRSU9+jKQhH
9R9DeB1DObDrQG6uuJYh2fGU/gOhRANCAATfYC4JF5vgz0f005FgdcIvzq+XWoK2
WkHv9ylmizkXwiiwONBMUiHLJp0aQ5prsy/qG1qvxIA+EemN8nsM73O/
-----END PRIVATE KEY-----
)PEM";

static const char *LOCALHOST_TEST_CERT_PEM = R"PEM(-----BEGIN CERTIFICATE-----
MIIBmTCCAT+gAwIBAgIUMx/aHjSr1BLKVJWLkjEW8tVBwEwwCgYIKoZIzj0EAwIw
FDESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTI2MDQxOTAwNDY0OVoXDTM2MDQxNjAw
NDY0OVowFDESMBAGA1UEAwwJbG9jYWxob3N0MFkwEwYHKoZIzj0CAQYIKoZIzj0D
AQcDQgAE32AuCReb4M9H9NORYHXCL86vl1qCtlpB7/cpZos5F8IosDjQTFIhyyad
GkOaa7Mv6htar8SAPhHpjfJ7DO9zv6NvMG0wHQYDVR0OBBYEFH8rprP1CxiHqLBg
7tp3in6Op8rZMB8GA1UdIwQYMBaAFH8rprP1CxiHqLBg7tp3in6Op8rZMA8GA1Ud
EwEB/wQFMAMBAf8wGgYDVR0RBBMwEYIJbG9jYWxob3N0hwR/AAABMAoGCCqGSM49
BAMCA0gAMEUCICfrWIQjaBKJOeHsEFydx3kmB3xZA27GVaokzpkBKShNAiEApv2B
ptOACq7G5MbeXCED94+Klf9Txx0gZ+qg8GckbdA=
-----END CERTIFICATE-----
)PEM";

static const char *LOCALHOST_RSA_ROOT_CERT_PEM = R"PEM(-----BEGIN CERTIFICATE-----
MIIDEzCCAfugAwIBAgICEAEwDQYJKoZIhvcNAQELBQAwGjEYMBYGA1UEAwwPVmlw
ZXIgVGVzdCBSb290MB4XDTI0MDEwMTAwMDAwMFoXDTM4MDEwMTAwMDAwMFowGjEY
MBYGA1UEAwwPVmlwZXIgVGVzdCBSb290MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8A
MIIBCgKCAQEAsCvt6uYssvJ6Qlsn8LQAmJ0YUbN2gOYwyj2RV3s8UejGMQPleNIf
eRvoeD/WZuG25Vf35YJ6djd0Vw1Mgt0Uk6M8C8oORoGQ6XRsGlNBOqzDiZbuDqxH
KZ1kP3C69p7Ey5cOmk+tgo3vVJgwFAGPhR2f4540UvJ+rAe255wbs9IJ5uqFkKHI
ccb+lrNZZaFPFGBSmI1Czm6ggPx3RHByOtSBepqB6VZzv05rzDV1WHFCGhlyBClJ
BfG29kPuj5n03MUPxfw8NAMahPFszFPoA71oxan4qzffWyKP7FubAUTzxArI8sf5
3otkyWp1d6snlyHlAI7kAb4vdIrwqZRcMQIDAQABo2MwYTAdBgNVHQ4EFgQUZsaT
+usuVwXWs0SWZvbAykvJFP8wHwYDVR0jBBgwFoAUZsaT+usuVwXWs0SWZvbAykvJ
FP8wDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMCAQYwDQYJKoZIhvcNAQEL
BQADggEBAI+1UE7O2TCevcQfQJZSPGJw8vNGkzCv9tMh4qhV+zfmtjLkmefvQ+0M
FJr/adXHYGE0WjwQ8VK4pfmd6gs26/PCEDREy0JpSXxrkyEiyei5WZvrNBvGQL4m
+XAngI5eTX7UwL7YBP/z8oUHQZOWXGsRyCNFpKi5zZKMUoR8aHuc1f9JDkyNbeon
by85P8iOkGoFFWGMSSI8+lQqPLAYITEUfzqWsyU/T9yXuGzaeOh+lLnq/sOygumJ
crDCbvYzu1M8VSpiGtQ9tiFSqIyQruau7RM69GFicRtKfWOOnHEafY5cZ+V9Dder
RXrTo5++oZs1QvYkMwOG9dD4/fGdRTY=
-----END CERTIFICATE-----
)PEM";

static const char *LOCALHOST_RSA_LEAF_CERT_PEM = R"PEM(-----BEGIN CERTIFICATE-----
MIIDPTCCAiWgAwIBAgICIAEwDQYJKoZIhvcNAQELBQAwGjEYMBYGA1UEAwwPVmlw
ZXIgVGVzdCBSb290MB4XDTI0MDEwMTAwMDAwMFoXDTM4MDEwMTAwMDAwMFowFDES
MBAGA1UEAwwJbG9jYWxob3N0MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKC
AQEAhKKdI7etjnO2Jnf/6gT9VrQEFCRmU73zic7zvHXkpyg/5zWy+cz7kdV87Ol6
h+5mvl63sspIpCeQ4DbQenlzhyMyDwO+YCZhkZp1Pu6Tq/XoixFL30JHL8uWyvd+
IJDY/Ihn163GdFHJ5aoiljUeZu9xEsYz8qqTR3hwDBpQpeTL3Bd4qIfVUeD5vazF
nEAjOzQpGv3yTmZVn8p5vPxkwusjOHwhXSrIDjAw/PoffsdHXGutjDxZMBdwviEd
VShtoVWN6L5SQZK/y01P4FXN+YpgAcBNUA7vovJO76iaPeCQR2vnj3R/rxq/vvug
sO7JA04QImdhyZ4qbZoLUHJ69wIDAQABo4GSMIGPMBoGA1UdEQQTMBGCCWxvY2Fs
aG9zdIcEfwAAATAMBgNVHRMBAf8EAjAAMA4GA1UdDwEB/wQEAwIFoDATBgNVHSUE
DDAKBggrBgEFBQcDATAdBgNVHQ4EFgQUUrU52T/pEly3nsAxjlpTVltClS0wHwYD
VR0jBBgwFoAUZsaT+usuVwXWs0SWZvbAykvJFP8wDQYJKoZIhvcNAQELBQADggEB
AA1wTY/oNMXbAONfawVKoz+biuEFiSwsy4XHKyZmhsFpOcjGlotWl14hjJp/zRBG
6B4PZwYT7D43/1C6wC3q5AOD+kjcrGik4Ef5WFggSZJUl43Ln51TNm5yhWhCT6nZ
0IVv2vKWRmoy3JMRG4hDfAT3Z+SaiwEZPpXnIvClXOgIl+DAuC+8h8CMRrJQ4mVN
aPCWTHi1eZAIiIcJ7Z55yWyWBmH7wx+y1YtYnDXH5ZeXSsm2EMJJtOeX/MPVStpT
cx+Oj/wMKAolQHbKLlO3Y2cBkuJ/cRM252X3QJfC+wk0T6n5MfF8MBlNj/z2fure
KwmzLhH+CCh6NMsSird2hjw=
-----END CERTIFICATE-----
)PEM";

static const char *LOCALHOST_RSA_LEAF_KEY_PEM = R"PEM(-----BEGIN PRIVATE KEY-----
MIIEvAIBADANBgkqhkiG9w0BAQEFAASCBKYwggSiAgEAAoIBAQCEop0jt62Oc7Ym
d//qBP1WtAQUJGZTvfOJzvO8deSnKD/nNbL5zPuR1Xzs6XqH7ma+XreyykikJ5Dg
NtB6eXOHIzIPA75gJmGRmnU+7pOr9eiLEUvfQkcvy5bK934gkNj8iGfXrcZ0Ucnl
qiKWNR5m73ESxjPyqpNHeHAMGlCl5MvcF3ioh9VR4Pm9rMWcQCM7NCka/fJOZlWf
ynm8/GTC6yM4fCFdKsgOMDD8+h9+x0dca62MPFkwF3C+IR1VKG2hVY3ovlJBkr/L
TU/gVc35imABwE1QDu+i8k7vqJo94JBHa+ePdH+vGr+++6Cw7skDThAiZ2HJnipt
mgtQcnr3AgMBAAECggEAAYRuDBpG+9V6tLBJhT037dIoJuYrep3hEGc+gqxcJP7y
tmvMOy85hGln3YG70JxihC1VxMFypy2lXHibK2OCZ6i/eFqc4zMH3tILP27/0ZVl
h1owd/RP+ypFHyz0MAihDmS7+SgqChXWG5tSX6K/JnSjxh5Dj8KAHu7MgOYNR1s1
UHqJ/Ldqm2/XzQHkYvAKC6wM7uBXK/voxqH2ob6R7ksPrI6geJMfIFEVXbxzJvnS
azS2iRu7dr47fO2L5q28OiYb6jFxi7pOb8Fa0Tr/cW9mOECZ8MidOXu7AjP5BNiJ
0sgSTXgZ6LqCk2xZBC6r7YUZ+m0onT7XXpSb9ilXBQKBgQC569FRs+TLMBXhJ4Oa
12TP9kDtqDHACx8hieLxQQpVRQVXpHSaSEcAukaVxmWt5W0xlk/1o5CezAICAKst
8tYqkAQo1YnPPPNkX7ZtlOi+fxOmwLDBlUXpvSGkO0dI719HwDkbIL3Pn7o+rD7Z
IyfO2UKQ/CkWnnu4Puw82mj7AwKBgQC2oQxrT+V+qGcGKTAvK9UNSvMjr17HrGau
RqHq9ljMjjIQ1c83EsCibuk9hm4zTti6C2EuDtxciS3OVQAJwFjR4BcAnt9jQr4l
bfmZFE4xpbF6TEHYY4EuTvZOV+1CYd9uNQS18GzhD45r4zSr539/pcaxcsJsuKC/
NU6GW8sj/QKBgGJxX9MACrwfiOY/8uow9Js8y6JK9ZS3DtPGW9jcVGlT84E1fdwX
OylCeI9jjoEmQswHx+zLn47FfKaszfa1ZvsAaINqld6aalGScFjTiO0dAj3AN5c4
v90EnOSF0rfmry+hs1sO2hIuhAIdV+XHPJPE6/8y1Vq5rc6f2pxaFU4bAoGALs3/
fNExI9DM9os/yhcVtx5qSc78H3hTqH55qNoRz/rxYdcqEBdCP17lb9swCv4+FRAt
i7xLRXvyvVqTc+xT1xXzTzloTuwgBz+0JENL9vVcEtfQWEDILrIV9eYa7FRhCsGT
v30qqlNuUMAeE6B00KYP0hJzOaHnsJlc0ppb6ZECgYAvxGRLH2tM01RHzpVEuMJo
BOEO0CXN4TOLIzzOJ/+U30eEGGLMdTgQvNApbqpEZlErSyVW85Xq5M57kyMce4/K
cgD9SkQcscJsGtN3qe481Cha/aHxF0SpE6ZTrmcVSXCdwPxON7ZGf2KRakvBUMkb
s9ACvsoKGa8Ahrkw0OK3kA==
-----END PRIVATE KEY-----
)PEM";

struct temp_tls_files_t {
    std::string cert_path;
    std::string key_path;
    std::string ca_path;
    bool valid = false;

    temp_tls_files_t() = default;
    temp_tls_files_t(const temp_tls_files_t &) = delete;
    temp_tls_files_t &operator=(const temp_tls_files_t &) = delete;

    temp_tls_files_t(temp_tls_files_t &&other) noexcept
        : cert_path(std::move(other.cert_path)), key_path(std::move(other.key_path)),
          ca_path(std::move(other.ca_path)), valid(other.valid) {
        other.cert_path.clear();
        other.key_path.clear();
        other.ca_path.clear();
        other.valid = false;
    }

    temp_tls_files_t &operator=(temp_tls_files_t &&other) noexcept {
        if (this == &other)
            return *this;
        std::error_code ec;
        if (!cert_path.empty())
            std::filesystem::remove(cert_path, ec);
        if (!key_path.empty())
            std::filesystem::remove(key_path, ec);
        if (!ca_path.empty())
            std::filesystem::remove(ca_path, ec);
        cert_path = std::move(other.cert_path);
        key_path = std::move(other.key_path);
        ca_path = std::move(other.ca_path);
        valid = other.valid;
        other.cert_path.clear();
        other.key_path.clear();
        other.ca_path.clear();
        other.valid = false;
        return *this;
    }

    ~temp_tls_files_t() {
        std::error_code ec;
        if (!cert_path.empty())
            std::filesystem::remove(cert_path, ec);
        if (!key_path.empty())
            std::filesystem::remove(key_path, ec);
        if (!ca_path.empty())
            std::filesystem::remove(ca_path, ec);
    }
};

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

static void send_text(void *client, const char *text) {
    rt_tcp_send_str(client, rt_const_cstr(text));
}

static void http_server_keepalive_handler(void *req_obj, void *res_obj) {
    (void)req_obj;
    rt_server_res_send(res_obj, rt_const_cstr("ok"));
}

static void http_redirect_source_handler(void *req_obj, void *res_obj) {
    (void)req_obj;
    rt_server_res_status(res_obj, 302);
    rt_server_res_header(
        res_obj, rt_const_cstr("Location"), rt_const_cstr(redirect_source_location.c_str()));
    rt_server_res_send(res_obj, rt_const_cstr(""));
}

static void http_redirect_target_handler(void *req_obj, void *res_obj) {
    rt_string authorization = rt_server_req_header(req_obj, rt_const_cstr("Authorization"));
    rt_string api_key = rt_server_req_header(req_obj, rt_const_cstr("X-API-Key"));
    redirect_target_authorization_header = rt_string_cstr(authorization);
    redirect_target_api_key_header = rt_string_cstr(api_key);
    rt_string_unref(authorization);
    rt_string_unref(api_key);
    rt_server_res_send(res_obj, rt_const_cstr("final"));
}

static void read_http_request_headers(void *client) {
    while (true) {
        rt_string line = rt_tcp_recv_line(client);
        const char *cstr = rt_string_cstr(line);
        const bool done = !cstr || *cstr == '\0';
        rt_string_unref(line);
        if (done)
            break;
    }
}

static std::string read_http_cookie_header(void *client) {
    std::string cookie_header;
    while (true) {
        rt_string line = rt_tcp_recv_line(client);
        const char *cstr = rt_string_cstr(line);
        const bool done = !cstr || *cstr == '\0';
        if (cstr && ascii_header_name_equals(cstr, "Cookie", 6) && cstr[6] == ':') {
            const char *value = cstr + 7;
            while (*value == ' ' || *value == '\t')
                value++;
            cookie_header = value;
        }
        rt_string_unref(line);
        if (done)
            break;
    }
    return cookie_header;
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

static bool write_text_file(const std::string &path, const char *contents) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out.write(contents, (std::streamsize)strlen(contents));
    return out.good();
}

static temp_tls_files_t create_temp_tls_files_with_contents(const char *cert_pem,
                                                            const char *key_pem,
                                                            const char *ca_pem = nullptr) {
    temp_tls_files_t files;
    std::error_code ec;
    const auto temp_dir = std::filesystem::temp_directory_path(ec);
    if (ec)
        return files;

    const unsigned id = ++tls_fixture_counter;
    files.cert_path =
        (temp_dir / ("zanna_tls_fixture_" + std::to_string(id) + "_cert.pem")).string();
    files.key_path = (temp_dir / ("zanna_tls_fixture_" + std::to_string(id) + "_key.pem")).string();
    if (ca_pem)
        files.ca_path =
            (temp_dir / ("zanna_tls_fixture_" + std::to_string(id) + "_ca.pem")).string();
    if (!write_text_file(files.cert_path, cert_pem) || !write_text_file(files.key_path, key_pem) ||
        (ca_pem && !write_text_file(files.ca_path, ca_pem))) {
        std::error_code cleanup_ec;
        std::filesystem::remove(files.cert_path, cleanup_ec);
        std::filesystem::remove(files.key_path, cleanup_ec);
        if (!files.ca_path.empty())
            std::filesystem::remove(files.ca_path, cleanup_ec);
        files.cert_path.clear();
        files.key_path.clear();
        files.ca_path.clear();
        return files;
    }

    files.valid = true;
    return files;
}

static temp_tls_files_t create_temp_tls_files() {
    return create_temp_tls_files_with_contents(LOCALHOST_TEST_CERT_PEM, LOCALHOST_TEST_KEY_PEM);
}

static bool wait_for_condition(const std::function<bool()> &predicate, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
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

static bool tls_send_all(rt_tls_session_t *tls, const void *data, size_t len) {
    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    size_t sent = 0;
    while (sent < len) {
        const long rc = rt_tls_send(tls, bytes + sent, len - sent);
        if (rc <= 0) {
            fprintf(stderr,
                    "tls_send_all: rt_tls_send returned %ld after %zu/%zu bytes: %s\n",
                    rc,
                    sent,
                    len,
                    rt_tls_last_error() ? rt_tls_last_error() : "(no error)");
            return false;
        }
        sent += (size_t)rc;
    }
    return true;
}

static bool tls_recv_exact(rt_tls_session_t *tls, void *data, size_t len) {
    uint8_t *bytes = static_cast<uint8_t *>(data);
    size_t received = 0;
    while (received < len) {
        const long rc = rt_tls_recv(tls, bytes + received, len - received);
        if (rc <= 0)
            return false;
        received += (size_t)rc;
    }
    return true;
}

static std::string tls_recv_line(rt_tls_session_t *tls) {
    std::string line;
    while (true) {
        char ch = '\0';
        const long rc = rt_tls_recv(tls, &ch, 1);
        if (rc <= 0)
            return line;
        if (ch == '\n')
            break;
        if (ch != '\r')
            line.push_back(ch);
    }
    return line;
}

static std::vector<std::pair<std::string, std::string>> tls_read_http_headers(
    rt_tls_session_t *tls) {
    std::vector<std::pair<std::string, std::string>> headers;
    while (true) {
        const std::string line = tls_recv_line(tls);
        if (line.empty())
            break;
        const size_t colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        std::string name = line.substr(0, colon);
        for (char &ch : name) {
            if (ch >= 'A' && ch <= 'Z')
                ch = (char)(ch + ('a' - 'A'));
        }
        size_t value_start = colon + 1;
        while (value_start < line.size() && (line[value_start] == ' ' || line[value_start] == '\t'))
            value_start++;
        headers.emplace_back(std::move(name), line.substr(value_start));
    }
    return headers;
}

static std::string tls_find_http_header(
    const std::vector<std::pair<std::string, std::string>> &headers, const char *name) {
    std::string key(name ? name : "");
    for (char &ch : key) {
        if (ch >= 'A' && ch <= 'Z')
            ch = (char)(ch + ('a' - 'A'));
    }
    for (const auto &entry : headers) {
        if (entry.first == key)
            return entry.second;
    }
    return std::string();
}

static std::string tls_recv_string(rt_tls_session_t *tls, size_t len) {
    std::string out(len, '\0');
    if (len == 0)
        return out;
    if (!tls_recv_exact(tls, out.data(), len))
        return std::string();
    return out;
}

static rt_tls_session_t *connect_local_tls_server_with_timeout(int port, int timeout_ms) {
    rt_tls_config_t config;
    rt_tls_config_init(&config);
    config.hostname = "127.0.0.1";
    config.alpn_protocol = "http/1.1";
    config.verify_cert = 0;
    config.timeout_ms = timeout_ms;
    return rt_tls_connect("127.0.0.1", (uint16_t)port, &config);
}

static rt_tls_session_t *connect_local_tls_server(int port) {
    return connect_local_tls_server_with_timeout(port, 30000);
}

static rt_tls_session_t *connect_local_tls_server_with_retries(int port) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        rt_tls_session_t *tls = connect_local_tls_server_with_timeout(port, 30000);
        if (tls)
            return tls;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return nullptr;
}

static rt_tls_session_t *connect_local_tls_server_with_alpn(int port, const char *alpn) {
    rt_tls_config_t config;
    rt_tls_config_init(&config);
    config.hostname = "127.0.0.1";
    config.alpn_protocol = alpn;
    config.verify_cert = 0;
    config.timeout_ms = 30000;
    return rt_tls_connect("127.0.0.1", (uint16_t)port, &config);
}

static rt_tls_session_t *connect_local_tls_server_verified(int port,
                                                           const char *connect_host,
                                                           const char *hostname,
                                                           const char *ca_file) {
    rt_tls_config_t config;
    rt_tls_config_init(&config);
    config.hostname = hostname;
    config.alpn_protocol = "http/1.1";
    config.ca_file = ca_file;
    config.verify_cert = 1;
    config.timeout_ms = 30000;
    return rt_tls_connect(connect_host, (uint16_t)port, &config);
}

static long tls_http2_read_cb(void *ctx, uint8_t *buf, size_t len) {
    return rt_tls_recv((rt_tls_session_t *)ctx, buf, len);
}

static int tls_http2_write_cb(void *ctx, const uint8_t *buf, size_t len) {
    return rt_tls_send((rt_tls_session_t *)ctx, buf, len) == (long)len;
}

static bool tls_recv_ws_frame(rt_tls_session_t *tls,
                              uint8_t *opcode_out,
                              std::vector<uint8_t> *payload_out) {
    uint8_t header[2] = {0, 0};
    if (!tls_recv_exact(tls, header, sizeof(header)))
        return false;

    uint8_t opcode = (uint8_t)(header[0] & 0x0F);
    int masked = (header[1] & 0x80) != 0;
    size_t payload_len = (size_t)(header[1] & 0x7F);
    if (payload_len == 126) {
        uint8_t ext[2];
        if (!tls_recv_exact(tls, ext, sizeof(ext)))
            return false;
        payload_len = ((size_t)ext[0] << 8) | ext[1];
    } else if (payload_len == 127) {
        uint8_t ext[8];
        if (!tls_recv_exact(tls, ext, sizeof(ext)))
            return false;
        payload_len = 0;
        for (int i = 0; i < 8; i++)
            payload_len = (payload_len << 8) | ext[i];
    }

    uint8_t mask[4] = {0, 0, 0, 0};
    if (masked && !tls_recv_exact(tls, mask, sizeof(mask)))
        return false;

    payload_out->assign(payload_len, 0);
    if (payload_len > 0 && !tls_recv_exact(tls, payload_out->data(), payload_len))
        return false;
    if (masked) {
        for (size_t i = 0; i < payload_out->size(); i++)
            (*payload_out)[i] ^= mask[i & 3];
    }

    if (opcode_out)
        *opcode_out = opcode;
    return true;
}

static bool tls_send_ws_client_close(rt_tls_session_t *tls) {
    const uint8_t close_frame[] = {0x88, 0x80, 0x00, 0x00, 0x00, 0x00};
    return tls_send_all(tls, close_frame, sizeof(close_frame));
}

static void https_server_secure_handler(void *req_obj, void *res_obj) {
    (void)req_obj;
    rt_string header_name = rt_const_cstr("X-Mixed-Case");
    rt_string header_value = rt_const_cstr("normalized");
    rt_string body = rt_const_cstr("secure-ok");
    rt_server_res_header(res_obj, header_name, header_value);
    rt_server_res_send(res_obj, body);
    rt_string_unref(body);
    rt_string_unref(header_value);
    rt_string_unref(header_name);
}

/// @brief Build a deliberately body-bearing 204 response for suppression tests.
/// @details The HTTP/2 server must serialize a zero-length DATA payload and a
///          matching `content-length: 0` field despite the handler-supplied
///          body, because status 204 never permits a response body.
static void https_server_no_content_handler(void *req_obj, void *res_obj) {
    (void)req_obj;
    rt_string body = rt_const_cstr("must-not-be-sent");
    rt_server_res_status(res_obj, 204);
    rt_server_res_send(res_obj, body);
    rt_string_unref(body);
}

static void sse_plain_server_thread(int port) {
    void *server = rt_tcp_server_listen(port);
    if (!server) {
        api_server_failed = true;
        api_server_ready = true;
        return;
    }

    api_server_failed = false;
    api_server_ready = true;

    void *client = rt_tcp_server_accept_for(server, 5000);
    if (!client) {
        rt_tcp_server_close(server);
        return;
    }

    read_http_request_headers(client);
    send_text(client,
              "HTTP/1.1 200 OK\r\n"
              "Content-Type: text/event-stream\r\n"
              "Connection: close\r\n"
              "\r\n"
              "event: greet\r\n"
              "id: 42\r\n"
              "data: hello\r\n"
              "\r\n");

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    rt_tcp_close(client);
    rt_tcp_server_close(server);
}

/// @brief Responds with a caller-chosen header block and closes (for
///        validation-strictness probes).
static std::string sse_bad_header_response;

static void sse_bad_header_server_thread(int port) {
    void *server = rt_tcp_server_listen(port);
    if (!server) {
        api_server_failed = true;
        api_server_ready = true;
        return;
    }
    api_server_failed = false;
    api_server_ready = true;

    void *client = rt_tcp_server_accept_for(server, 5000);
    if (client) {
        read_http_request_headers(client);
        send_text(client, sse_bad_header_response.c_str());
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        rt_tcp_close(client);
    }
    rt_tcp_server_close(server);
}

/// @brief Response bytes and post-send hold interval for focused SSE parser probes.
static std::string sse_scripted_response;
static int sse_scripted_hold_ms = 100;

/// @brief Wait until the active loopback fixture publishes readiness.
static void wait_for_server();

/// @brief Serve one caller-scripted SSE/HTTP response on a loopback connection.
/// @details The helper accepts exactly one client, drains its request head,
///          writes the configured bytes verbatim, optionally keeps the socket
///          open, and then closes both accepted and listening handles.
/// @param port Loopback port selected by the test.
static void sse_scripted_server_thread(int port) {
    void *server = rt_tcp_server_listen(port);
    if (!server) {
        api_server_failed = true;
        api_server_ready = true;
        return;
    }
    api_server_failed = false;
    api_server_ready = true;
    void *client = rt_tcp_server_accept_for(server, 5000);
    if (client) {
        read_http_request_headers(client);
        send_text(client, sse_scripted_response.c_str());
        if (sse_scripted_hold_ms > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(sse_scripted_hold_ms));
        rt_tcp_close(client);
    }
    rt_tcp_server_close(server);
}

/// @brief Assert that one malformed SSE HTTP response is rejected during Connect.
/// @details Each invocation owns a fresh loopback listener so parser state and
///          socket shutdown from an earlier rejection cannot affect the next
///          case.
/// @param response Complete malformed response bytes.
/// @param label Human-readable assertion label.
/// @param diagnostic_fragment Expected trap substring, or NULL to accept any trap.
static void expect_sse_connect_rejection(const char *response,
                                         const char *label,
                                         const char *diagnostic_fragment) {
    int port = (int)rt_netutils_get_free_port();
    if (port <= 0) {
        printf("  SKIPPED: %s (local bind unavailable)\n", label);
        return;
    }
    api_server_ready = false;
    api_server_failed = false;
    sse_scripted_response = response ? response : "";
    sse_scripted_hold_ms = 50;
    std::thread server(sse_scripted_server_thread, port);
    wait_for_server();
    if (api_server_failed) {
        server.join();
        printf("  SKIPPED: %s (local bind unavailable)\n", label);
        return;
    }
    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/events", port);
    EXPECT_TRAP(rt_sse_connect(rt_const_cstr(url)));
    bool rejected = g_last_trap != nullptr &&
                    (!diagnostic_fragment || strstr(g_last_trap, diagnostic_fragment) != nullptr);
    test_result(label, rejected);
    server.join();
}

/// @brief Two events (typed then untyped), a quiet gap, then close — for
///        dispatch-type reset and Result-shaped receive probes.
static void sse_two_event_server_thread(int port) {
    void *server = rt_tcp_server_listen(port);
    if (!server) {
        api_server_failed = true;
        api_server_ready = true;
        return;
    }
    api_server_failed = false;
    api_server_ready = true;

    void *client = rt_tcp_server_accept_for(server, 5000);
    if (client) {
        read_http_request_headers(client);
        send_text(client,
                  "HTTP/1.1 200 OK\r\n"
                  "Content-Type: text/event-stream\r\n"
                  "Connection: close\r\n"
                  "\r\n"
                  "retry: 50\r\n"
                  "event: greet\r\n"
                  "data: hello\r\n"
                  "\r\n"
                  "data: second\r\n"
                  "\r\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        rt_tcp_close(client);
    }
    rt_tcp_server_close(server);
}

/// @brief Sends a partial `data:` line, pauses past the client's timed
///        receive, then completes the event (VDOC-151 lossless-timeout probe).
static void sse_partial_then_complete_server_thread(int port) {
    void *server = rt_tcp_server_listen(port);
    if (!server) {
        api_server_failed = true;
        api_server_ready = true;
        return;
    }

    api_server_failed = false;
    api_server_ready = true;

    void *client = rt_tcp_server_accept_for(server, 5000);
    if (!client) {
        rt_tcp_server_close(server);
        return;
    }

    read_http_request_headers(client);
    send_text(client,
              "HTTP/1.1 200 OK\r\n"
              "Content-Type: text/event-stream\r\n"
              "Connection: close\r\n"
              "\r\n"
              "data: par"); // deliberately unterminated
    std::this_thread::sleep_for(std::chrono::milliseconds(900));
    send_text(client, "tial\r\n\r\n");

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    rt_tcp_close(client);
    rt_tcp_server_close(server);
}

static void sse_chunked_server_thread(int port) {
    void *server = rt_tcp_server_listen(port);
    if (!server) {
        api_server_failed = true;
        api_server_ready = true;
        return;
    }

    api_server_failed = false;
    api_server_ready = true;

    void *client = rt_tcp_server_accept_for(server, 5000);
    if (!client) {
        rt_tcp_server_close(server);
        return;
    }

    read_http_request_headers(client);
    send_text(client,
              "HTTP/1.1 200 OK\r\n"
              "Content-Type: text/event-stream\r\n"
              "Transfer-Encoding: chunked\r\n"
              "Connection: close\r\n"
              "\r\n");
    // HTTP chunked encoding: each chunk is <hex-size>\r\n<data>\r\n
    // Chunk 1: 0x17 = 23 bytes of SSE event metadata + blank-line delimiter
    // Chunk 2: 0x18 = 24 bytes of SSE data fields + blank-line delimiter
    // Chunk 0: terminates the stream
    send_text(client, "17\r\nevent: multi\r\nid: 7\r\n\r\n\r\n");
    send_text(client, "18\r\ndata: one\r\ndata: two\r\n\r\n\r\n");
    send_text(client, "0\r\n\r\n");

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    rt_tcp_close(client);
    rt_tcp_server_close(server);
}

static void sse_resume_server_thread(int port) {
    void *server = rt_tcp_server_listen(port);
    if (!server) {
        api_server_failed = true;
        api_server_ready = true;
        return;
    }

    api_server_failed = false;
    api_server_ready = true;
    sse_resume_header.clear();

    void *client = rt_tcp_server_accept_for(server, 5000);
    if (!client) {
        rt_tcp_server_close(server);
        return;
    }
    read_http_request_headers(client);
    send_text(client,
              "HTTP/1.1 200 OK\r\n"
              "Content-Type: text/event-stream\r\n"
              "Connection: close\r\n"
              "\r\n"
              "retry: 1\r\n"
              "id: 1\r\n"
              "data: first\r\n"
              "\r\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    rt_tcp_close(client);

    client = rt_tcp_server_accept_for(server, 5000);
    if (!client) {
        rt_tcp_server_close(server);
        return;
    }
    sse_resume_header = read_http_header_value(client, "Last-Event-ID");
    send_text(client,
              "HTTP/1.1 200 OK\r\n"
              "Content-Type: text/event-stream\r\n"
              "Connection: close\r\n"
              "\r\n"
              "id: 2\r\n"
              "data: second\r\n"
              "\r\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    rt_tcp_close(client);
    rt_tcp_server_close(server);
}

/// @brief Serve MAIL/RCPT/DATA/QUIT on an already-handshaken SMTP connection.
/// @details Each DATA line is appended to @ref smtp_captured_message with a
///          normalized LF separator. The fixture deliberately reads through
///          the terminating dot so it detects missing dot transparency and
///          message truncation. Protocol assertions remain local to the server
///          thread and make an unexpected client command fail the test binary.
/// @param client Connected runtime TCP object owned by the caller.
/// @param rcpt_response Exact response to the single RCPT TO command.
static void smtp_capture_message_session(void *client, const char *rcpt_response) {
    rt_string line = rt_tcp_recv_line(client);
    const char *cstr = rt_string_cstr(line);
    assert(cstr && strncmp(cstr, "MAIL FROM:", 10) == 0);
    rt_string_unref(line);
    send_text(client, "250 OK\r\n");

    line = rt_tcp_recv_line(client);
    cstr = rt_string_cstr(line);
    assert(cstr && strncmp(cstr, "RCPT TO:", 8) == 0);
    rt_string_unref(line);
    send_text(client, rcpt_response);

    line = rt_tcp_recv_line(client);
    cstr = rt_string_cstr(line);
    assert(cstr && strcmp(cstr, "DATA") == 0);
    rt_string_unref(line);
    send_text(client, "354 End data with <CR><LF>.<CR><LF>\r\n");

    while (true) {
        line = rt_tcp_recv_line(client);
        cstr = rt_string_cstr(line);
        if (cstr && strcmp(cstr, ".") == 0) {
            rt_string_unref(line);
            break;
        }
        smtp_captured_message += cstr ? cstr : "";
        smtp_captured_message += '\n';
        rt_string_unref(line);
    }

    send_text(client, "250 Queued\r\n");
    line = rt_tcp_recv_line(client);
    cstr = rt_string_cstr(line);
    assert(cstr && strcmp(cstr, "QUIT") == 0);
    rt_string_unref(line);
    send_text(client, "221 Bye\r\n");
}

/// @brief Run one configurable loopback SMTP session.
/// @param port Reserved loopback port.
/// @param ehlo_response Exact EHLO response bytes.
/// @param rcpt_response Exact RCPT response bytes.
/// @param capture_message Nonzero to continue through DATA and QUIT.
static void smtp_server_thread(int port,
                               const char *ehlo_response,
                               const char *rcpt_response,
                               bool capture_message) {
    void *server = rt_tcp_server_listen(port);
    if (!server) {
        api_server_failed = true;
        api_server_ready = true;
        return;
    }

    api_server_failed = false;
    api_server_ready = true;

    void *client = rt_tcp_server_accept_for(server, 5000);
    if (!client) {
        rt_tcp_server_close(server);
        return;
    }

    smtp_captured_message.clear();
    send_text(client, "220 localhost ESMTP ready\r\n");

    rt_string line = rt_tcp_recv_line(client);
    const char *cstr = rt_string_cstr(line);
    assert(cstr && strncmp(cstr, "EHLO ", 5) == 0);
    rt_string_unref(line);
    send_text(client, ehlo_response);

    if (!capture_message) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        rt_tcp_close(client);
        rt_tcp_server_close(server);
        return;
    }

    smtp_capture_message_session(client, rcpt_response);

    rt_tcp_close(client);
    rt_tcp_server_close(server);
}

static void smtp_plain_server_thread(int port) {
    smtp_server_thread(port, "250-localhost\r\n250 SIZE 1024\r\n", "250 OK\r\n", true);
}

static void smtp_no_starttls_server_thread(int port) {
    smtp_server_thread(port, "250-localhost\r\n250 AUTH LOGIN\r\n", "250 OK\r\n", false);
}

static void smtp_forwarding_rcpt_server_thread(int port) {
    smtp_server_thread(
        port, "250-localhost\r\n250 SIZE 1024\r\n", "251 User not local; will forward\r\n", true);
}

/// @brief Emit one configurable greeting and optional EHLO response.
/// @details Used to exercise strict reply framing without relying on a
///          production SMTP daemon. When @p ehlo_response is empty, the fixture
///          sends only the greeting and waits briefly for the client to reject it.
/// @param port Reserved loopback port.
/// @param greeting Exact greeting bytes, including any deliberately malformed delimiter.
/// @param ehlo_response Optional exact response after one EHLO command.
static void smtp_reply_script_server_thread(int port,
                                            std::string greeting,
                                            std::string ehlo_response) {
    void *server = rt_tcp_server_listen(port);
    if (!server) {
        api_server_failed = true;
        api_server_ready = true;
        return;
    }
    api_server_failed = false;
    api_server_ready = true;
    void *client = rt_tcp_server_accept_for(server, 5000);
    if (client) {
        send_text(client, greeting.c_str());
        if (!ehlo_response.empty()) {
            rt_string line = rt_tcp_recv_line(client);
            const char *text = rt_string_cstr(line);
            assert(text && strncmp(text, "EHLO ", 5) == 0);
            rt_string_unref(line);
            send_text(client, ehlo_response.c_str());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(75));
        rt_tcp_close(client);
    }
    rt_tcp_server_close(server);
}

/// @brief Reject EHLO with a permitted legacy code, then complete via HELO.
/// @details This verifies the compatibility fallback remains available for
///          unauthenticated plaintext sessions while strict EHLO requirements
///          still apply when STARTTLS or AUTH is requested.
/// @param port Reserved loopback port.
static void smtp_helo_fallback_server_thread(int port) {
    void *server = rt_tcp_server_listen(port);
    if (!server) {
        api_server_failed = true;
        api_server_ready = true;
        return;
    }
    api_server_failed = false;
    api_server_ready = true;
    void *client = rt_tcp_server_accept_for(server, 5000);
    if (client) {
        smtp_captured_message.clear();
        send_text(client, "220 localhost ready\r\n");
        rt_string line = rt_tcp_recv_line(client);
        const char *text = rt_string_cstr(line);
        assert(text && strncmp(text, "EHLO ", 5) == 0);
        rt_string_unref(line);
        send_text(client, "500 EHLO unavailable\r\n");
        line = rt_tcp_recv_line(client);
        text = rt_string_cstr(line);
        assert(text && strncmp(text, "HELO ", 5) == 0);
        rt_string_unref(line);
        send_text(client, "250 localhost\r\n");
        smtp_capture_message_session(client, "250 OK\r\n");
        rt_tcp_close(client);
    }
    rt_tcp_server_close(server);
}

/// @brief Stall the first greeting, then service a reconnect on the same listener.
/// @details The first connection remains open long enough that only the
///          client's cancellation shutdown can promptly unblock its receive.
///          The second connection proves cancellation does not poison later
///          serialized sends or leave stale transport ownership.
/// @param port Reserved loopback port.
static void smtp_cancel_reconnect_server_thread(int port) {
    void *server = rt_tcp_server_listen(port);
    if (!server) {
        api_server_failed = true;
        api_server_ready = true;
        return;
    }
    api_server_failed = false;
    api_server_ready = true;
    void *first = rt_tcp_server_accept_for(server, 5000);
    if (!first) {
        rt_tcp_server_close(server);
        return;
    }
    smtp_fixture_client_accepted = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(750));
    rt_tcp_close(first);

    void *second = rt_tcp_server_accept_for(server, 5000);
    if (second) {
        smtp_captured_message.clear();
        send_text(second, "220 localhost ready\r\n");
        rt_string line = rt_tcp_recv_line(second);
        rt_string_unref(line);
        send_text(second, "250-localhost\r\n250 SIZE 1000000\r\n");
        smtp_capture_message_session(second, "250 OK\r\n");
        rt_tcp_close(second);
    }
    rt_tcp_server_close(server);
}

/// @brief Reach STARTTLS, consume the first ClientHello byte, then stall.
/// @details Consuming one TLS byte proves the SMTP client entered its TLS-owned
///          receive path before the test requests cancellation. The incomplete
///          record remains open long enough to distinguish bounded polling from
///          waiting for the fixture's eventual close.
/// @param port Reserved loopback port.
static void smtp_stalled_starttls_server_thread(int port) {
    void *server = rt_tcp_server_listen(port);
    if (!server) {
        api_server_failed = true;
        api_server_ready = true;
        return;
    }
    api_server_failed = false;
    api_server_ready = true;
    void *client = rt_tcp_server_accept_for(server, 5000);
    if (client) {
        send_text(client, "220 localhost ESMTP ready\r\n");
        rt_string line = rt_tcp_recv_line(client);
        const char *text = rt_string_cstr(line);
        assert(text && strncmp(text, "EHLO ", 5) == 0);
        rt_string_unref(line);
        send_text(client, "250-localhost\r\n250 STARTTLS\r\n");
        line = rt_tcp_recv_line(client);
        text = rt_string_cstr(line);
        assert(text && strcmp(text, "STARTTLS") == 0);
        rt_string_unref(line);
        send_text(client, "220 Ready to start TLS\r\n");

        rt_tcp_set_recv_timeout(client, 5000);
        void *hello_byte = rt_tcp_recv(client, 1);
        smtp_fixture_tls_client_hello = hello_byte && rt_bytes_len(hello_byte) == 1;
        if (hello_byte && rt_obj_release_check0(hello_byte))
            rt_obj_free(hello_byte);
        std::this_thread::sleep_for(std::chrono::milliseconds(750));
        rt_tcp_close(client);
    }
    rt_tcp_server_close(server);
}

/// @brief Service two complete SMTP connections sequentially on one listener.
/// @details Two caller threads target the same client object. A correct
///          operation mutex allows each fresh connection to complete without
///          overwriting the other operation's transport or parser state.
/// @param port Reserved loopback port.
static void smtp_two_session_server_thread(int port) {
    void *server = rt_tcp_server_listen(port);
    if (!server) {
        api_server_failed = true;
        api_server_ready = true;
        return;
    }
    api_server_failed = false;
    api_server_ready = true;
    smtp_captured_message.clear();
    for (int session = 0; session < 2; session++) {
        void *client = rt_tcp_server_accept_for(server, 5000);
        if (!client)
            break;
        send_text(client, "220 localhost ready\r\n");
        rt_string line = rt_tcp_recv_line(client);
        rt_string_unref(line);
        send_text(client, "250-localhost\r\n250 SIZE 1000000\r\n");
        smtp_capture_message_session(client, "250 OK\r\n");
        smtp_captured_message += "--session--\n";
        rt_tcp_close(client);
    }
    rt_tcp_server_close(server);
}

/// @brief Advertises STARTTLS, accepts the upgrade command, then answers the
///        TLS ClientHello with garbage so the handshake fails (VDOC-156).
static void smtp_broken_starttls_server_thread(int port) {
    void *server = rt_tcp_server_listen(port);
    if (!server) {
        api_server_failed = true;
        api_server_ready = true;
        return;
    }
    api_server_failed = false;
    api_server_ready = true;

    void *client = rt_tcp_server_accept_for(server, 5000);
    if (client) {
        send_text(client, "220 localhost ESMTP ready\r\n");
        rt_string line = rt_tcp_recv_line(client); // EHLO
        rt_string_unref(line);
        send_text(client, "250-localhost\r\n250 STARTTLS\r\n");
        line = rt_tcp_recv_line(client); // STARTTLS
        rt_string_unref(line);
        send_text(client, "220 Ready to start TLS\r\n");
        // Not a TLS ServerHello: the client handshake must fail cleanly.
        send_text(client, "this is definitely not TLS\r\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        rt_tcp_close(client);
    }
    rt_tcp_server_close(server);
}

/// @brief STARTTLS handshake failure must fail the send cleanly with a single
///        descriptor close and leave the client reusable (VDOC-156).
static void test_smtp_starttls_handshake_failure_is_clean() {
    printf("\nTesting SmtpClient STARTTLS handshake-failure cleanup:\n");

    const int port = (int)rt_netutils_get_free_port();
    if (port <= 0) {
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }
    api_server_ready = false;
    api_server_failed = false;
    std::thread server(smtp_broken_starttls_server_thread, port);
    while (!api_server_ready)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (api_server_failed) {
        server.join();
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    void *client = rt_smtp_new(rt_const_cstr("127.0.0.1"), port);
    rt_smtp_set_tls(client, 1);
    void *res = rt_smtp_send_result(client,
                                    rt_const_cstr("a@example.com"),
                                    rt_const_cstr("b@example.com"),
                                    rt_const_cstr("s"),
                                    rt_const_cstr("b"));
    test_result("Handshake failure surfaces as Err", rt_result_is_err(res) == 1);
    rt_string err = rt_result_unwrap_err_str(res);
    test_result("Error names the TLS handshake", strstr(rt_string_cstr(err), "TLS") != nullptr);
    server.join();

    // The client object remains usable after the failed upgrade (no dangling
    // transport state): a follow-up send fails cleanly too instead of
    // crashing on a double-closed descriptor.
    void *res2 = rt_smtp_send_result(client,
                                     rt_const_cstr("a@example.com"),
                                     rt_const_cstr("b@example.com"),
                                     rt_const_cstr("s"),
                                     rt_const_cstr("b"));
    test_result("Client survives for a follow-up attempt", rt_result_is_err(res2) == 1);
    if (res && rt_obj_release_check0(res))
        rt_obj_free(res);
    if (res2 && rt_obj_release_check0(res2))
        rt_obj_free(res2);
    if (client && rt_obj_release_check0(client))
        rt_obj_free(client);
}

static void http_cookie_scope_server_thread(int port) {
    void *server = rt_tcp_server_listen(port);
    if (!server) {
        api_server_failed = true;
        api_server_ready = true;
        return;
    }

    api_server_failed = false;
    api_server_ready = true;
    http_cookie_headers[0].clear();
    http_cookie_headers[1].clear();
    http_cookie_headers[2].clear();

    for (int i = 0; i < 3; i++) {
        void *client = rt_tcp_server_accept_for(server, 5000);
        if (!client)
            break;

        rt_string request_line = rt_tcp_recv_line(client);
        rt_string_unref(request_line);
        http_cookie_headers[i] = read_http_cookie_header(client);

        if (i == 0) {
            send_text(client,
                      "HTTP/1.1 200 OK\r\n"
                      "Content-Length: 2\r\n"
                      "Set-Cookie: appToken=alpha; Path=/app\r\n"
                      "Set-Cookie: rootToken=beta; Path=/\r\n"
                      "Set-Cookie: secureToken=secret; Path=/app; Secure\r\n"
                      "\r\nok");
        } else if (i == 1) {
            send_text(client, "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\napp");
        } else {
            send_text(client, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nother");
        }

        rt_tcp_close(client);
    }

    rt_tcp_server_close(server);
}

static void http_keepalive_server_thread(int port) {
    void *server = rt_tcp_server_listen(port);
    if (!server) {
        api_server_failed = true;
        api_server_ready = true;
        return;
    }

    api_server_failed = false;
    api_server_ready = true;
    http_keepalive_accept_count = 0;
    http_keepalive_reused = false;
    http_keepalive_connection_headers[0].clear();
    http_keepalive_connection_headers[1].clear();

    void *client = rt_tcp_server_accept_for(server, 5000);
    if (!client) {
        rt_tcp_server_close(server);
        return;
    }

    http_keepalive_accept_count = 1;
    rt_string request_line = rt_tcp_recv_line(client);
    rt_string_unref(request_line);
    http_keepalive_connection_headers[0] = read_http_header_value(client, "Connection");
    send_text(client,
              "HTTP/1.1 200 OK\r\n"
              "Content-Length: 2\r\n"
              "Connection: keep-alive\r\n"
              "\r\n"
              "ok");

    void *second_client = rt_tcp_server_accept_for(server, 750);
    if (second_client) {
        http_keepalive_accept_count = 2;
        request_line = rt_tcp_recv_line(second_client);
        rt_string_unref(request_line);
        http_keepalive_connection_headers[1] = read_http_header_value(second_client, "Connection");
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
    http_keepalive_connection_headers[1] = read_http_header_value(client, "Connection");
    http_keepalive_reused = true;
    send_text(client,
              "HTTP/1.1 200 OK\r\n"
              "Content-Length: 2\r\n"
              "Connection: close\r\n"
              "\r\n"
              "ok");
    rt_tcp_close(client);
    rt_tcp_server_close(server);
}

static void http_informational_server_thread(int port,
                                             std::atomic<bool> *ready,
                                             std::atomic<bool> *failed) {
    void *server = rt_tcp_server_listen(port);
    if (!server) {
        if (failed)
            *failed = true;
        if (ready)
            *ready = true;
        return;
    }

    if (failed)
        *failed = false;
    if (ready)
        *ready = true;

    void *client = rt_tcp_server_accept_for(server, 5000);
    if (client) {
        rt_string request_line = rt_tcp_recv_line(client);
        rt_string_unref(request_line);
        read_http_request_headers(client);
        send_text(client,
                  "HTTP/1.1 103 Early Hints\r\n"
                  "Link: </style.css>; rel=preload; as=style\r\n"
                  "\r\n"
                  "HTTP/1.1 200 OK\r\n"
                  "Content-Length: 5\r\n"
                  "Connection: close\r\n"
                  "\r\n"
                  "hello");
        rt_tcp_close(client);
    }

    rt_tcp_server_close(server);
}

static void wait_for_server() {
    while (!api_server_ready)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

static void test_sse_plain_event() {
    printf("\nTesting SseClient plain event stream:\n");

    const int port = (int)rt_netutils_get_free_port();
    if (port <= 0) {
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    api_server_ready = false;
    api_server_failed = false;
    std::thread server(sse_plain_server_thread, port);
    wait_for_server();
    if (api_server_failed) {
        server.join();
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    char url_buf[128];
    snprintf(url_buf, sizeof(url_buf), "http://127.0.0.1:%d/events", port);
    void *sse = rt_sse_connect(rt_const_cstr(url_buf));
    test_result("SSE connection opens", sse != nullptr && rt_sse_is_open(sse) == 1);

    rt_string data = rt_sse_recv(sse);
    test_result("SSE plain event payload matches", strcmp(rt_string_cstr(data), "hello") == 0);

    rt_string event_type = rt_sse_last_event_type(sse);
    rt_string event_id = rt_sse_last_event_id(sse);
    test_result("SSE plain event type captured", strcmp(rt_string_cstr(event_type), "greet") == 0);
    test_result("SSE plain event id captured", strcmp(rt_string_cstr(event_id), "42") == 0);

    rt_sse_close(sse);
    server.join();
}

static void test_sse_chunked_event() {
    printf("\nTesting SseClient chunked event stream:\n");

    const int port = (int)rt_netutils_get_free_port();
    if (port <= 0) {
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    api_server_ready = false;
    api_server_failed = false;
    std::thread server(sse_chunked_server_thread, port);
    wait_for_server();
    if (api_server_failed) {
        server.join();
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    char url_buf[128];
    snprintf(url_buf, sizeof(url_buf), "http://127.0.0.1:%d/chunked", port);
    void *sse = rt_sse_connect(rt_const_cstr(url_buf));
    test_result("SSE chunked connection opens", sse != nullptr && rt_sse_is_open(sse) == 1);

    rt_string data = rt_sse_recv_for(sse, 2000);
    test_result("SSE chunked payload matches", strcmp(rt_string_cstr(data), "one\ntwo") == 0);

    rt_string event_type = rt_sse_last_event_type(sse);
    rt_string event_id = rt_sse_last_event_id(sse);
    test_result("SSE chunked event type captured",
                strcmp(rt_string_cstr(event_type), "multi") == 0);
    test_result("SSE chunked event id captured", strcmp(rt_string_cstr(event_id), "7") == 0);

    rt_sse_close(sse);
    server.join();
}

/// @brief WssServer constructor validates credential handles (VDOC-161).
static void test_wss_server_credential_validation() {
    printf("\nTesting WssServer credential-path validation:\n");
    EXPECT_TRAP(rt_wss_server_new(9443, nullptr, rt_const_cstr("key.pem")));
    test_result("NULL certificate handle traps",
                g_last_trap && strstr(g_last_trap, "certificate") != nullptr);
    rt_string nul_path = rt_string_from_bytes("cert.pem\0.bak", 13);
    EXPECT_TRAP(rt_wss_server_new(9443, nul_path, rt_const_cstr("key.pem")));
    test_result("Embedded-NUL certificate path traps",
                g_last_trap && strstr(g_last_trap, "certificate") != nullptr);
    rt_string_unref(nul_path);
}

/// @brief Validation strictness and dispatch-state fixes (VDOC-152): exact
///        media/transfer codings, per-event type reset, Result-shaped receive.
static void test_sse_validation_and_dispatch_state() {
    printf("\nTesting SseClient validation and dispatch state:\n");

    // Round 1: text/event-streaming is NOT text/event-stream.
    int port = (int)rt_netutils_get_free_port();
    if (port <= 0) {
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }
    api_server_ready = false;
    api_server_failed = false;
    sse_bad_header_response = "HTTP/1.1 200 OK\r\n"
                              "Content-Type: text/event-streaming\r\n"
                              "Connection: close\r\n"
                              "\r\n";
    std::thread bad_type_server(sse_bad_header_server_thread, port);
    wait_for_server();
    if (api_server_failed) {
        bad_type_server.join();
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }
    char url_buf[128];
    snprintf(url_buf, sizeof(url_buf), "http://127.0.0.1:%d/bad", port);
    EXPECT_TRAP(rt_sse_connect(rt_const_cstr(url_buf)));
    test_result("text/event-streaming is rejected",
                g_last_trap && strstr(g_last_trap, "text/event-stream") != nullptr);
    bad_type_server.join();

    // Round 2: unsupported Transfer-Encoding is rejected.
    port = (int)rt_netutils_get_free_port();
    api_server_ready = false;
    api_server_failed = false;
    sse_bad_header_response = "HTTP/1.1 200 OK\r\n"
                              "Content-Type: text/event-stream\r\n"
                              "Transfer-Encoding: gzip, chunked\r\n"
                              "Connection: close\r\n"
                              "\r\n";
    std::thread bad_te_server(sse_bad_header_server_thread, port);
    wait_for_server();
    snprintf(url_buf, sizeof(url_buf), "http://127.0.0.1:%d/badte", port);
    EXPECT_TRAP(rt_sse_connect(rt_const_cstr(url_buf)));
    test_result("Unsupported Transfer-Encoding is rejected",
                g_last_trap && strstr(g_last_trap, "Transfer-Encoding") != nullptr);
    bad_te_server.join();

    // Round 3: dispatch-type reset + Result-shaped receive outcomes.
    port = (int)rt_netutils_get_free_port();
    api_server_ready = false;
    api_server_failed = false;
    std::thread stream_server(sse_two_event_server_thread, port);
    wait_for_server();
    snprintf(url_buf, sizeof(url_buf), "http://127.0.0.1:%d/events", port);
    void *sse = rt_sse_connect(rt_const_cstr(url_buf));
    test_result("SSE connection opens", sse != nullptr && rt_sse_is_open(sse) == 1);

    void *r1 = rt_sse_recv_for_result(sse, 2000);
    test_result("First event arrives as Ok", rt_result_is_ok(r1) == 1);
    rt_string t1 = rt_sse_last_event_type(sse);
    test_result("First event type is greet", strcmp(rt_string_cstr(t1), "greet") == 0);
    rt_string_unref(t1);

    void *r2 = rt_sse_recv_for_result(sse, 2000);
    test_result("Second event arrives as Ok", rt_result_is_ok(r2) == 1);
    rt_string d2 = rt_result_unwrap_str(r2);
    test_result("Second event data matches", strcmp(rt_string_cstr(d2), "second") == 0);
    rt_string t2 = rt_sse_last_event_type(sse);
    test_result("Untyped event resets the dispatch type", rt_string_cstr(t2)[0] == '\0');
    rt_string_unref(t2);

    // Quiet stream: a timed receive reports timeout, not an empty event.
    void *r3 = rt_sse_recv_for_result(sse, 300);
    test_result("Quiet stream reports Err timeout", rt_result_is_err(r3) == 1);

    // After the server closes (retry: 50 keeps the reconnect attempt fast),
    // the outcome is a closed-stream error, distinct from timeout.
    void *r4 = rt_sse_recv_for_result(sse, 3000);
    test_result("Stream end reports Err", rt_result_is_err(r4) == 1);
    rt_string err4 = rt_result_unwrap_err_str(r4);
    test_result("Stream-end error names the closed stream",
                strstr(rt_string_cstr(err4), "closed") != nullptr);
    rt_string_unref(err4);

    rt_sse_close(sse);
    stream_server.join();
}

#include "RTHighLevelNetworkSseStrictTests.inc"

/// @brief Verify cancellation-safe Close and serialized concurrent receives.
/// @details The first fixture leaves a blocking receive without event bytes;
///          Close must wake it via shutdown without freeing TLS/TCP state under
///          the worker. The second fixture starts two receive callers at once
///          and proves each consumes one complete event without parser races.
static void test_sse_close_and_receive_serialization() {
    printf("\nTesting SseClient cancellation and receive serialization:\n");

    int port = (int)rt_netutils_get_free_port();
    if (port <= 0) {
        printf("  SKIPPED: SSE cancellation probes (local bind unavailable)\n");
        return;
    }
    api_server_ready = false;
    api_server_failed = false;
    sse_scripted_response = "HTTP/1.1 200 OK\r\n"
                            "Content-Type: text/event-stream\r\n\r\n";
    sse_scripted_hold_ms = 1000;
    std::thread idle_server(sse_scripted_server_thread, port);
    wait_for_server();
    if (api_server_failed) {
        idle_server.join();
        printf("  SKIPPED: SSE cancellation probes (local bind unavailable)\n");
        return;
    }
    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/idle", port);
    void *sse = rt_sse_connect(rt_const_cstr(url));
    rt_string blocked_result = nullptr;
    std::atomic<bool> receive_returned{false};
    std::thread receiver([&]() {
        blocked_result = rt_sse_recv(sse);
        receive_returned.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    test_result("Blocking receive does not expose cancellation polls",
                !receive_returned.load(std::memory_order_acquire));
    auto close_started = std::chrono::steady_clock::now();
    rt_sse_close(sse);
    receiver.join();
    auto close_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - close_started)
                             .count();
    test_result("Close interrupts a blocking receive promptly", close_elapsed < 700);
    test_result("Interrupted receive returns the legacy empty sentinel",
                blocked_result && rt_str_len(blocked_result) == 0);
    rt_string_unref(blocked_result);
    idle_server.join();

    port = (int)rt_netutils_get_free_port();
    api_server_ready = false;
    api_server_failed = false;
    sse_scripted_response = "HTTP/1.1 200 OK\r\n"
                            "Content-Type: text/event-stream\r\n\r\n"
                            "data: one\r\n\r\n"
                            "data: two\r\n\r\n";
    sse_scripted_hold_ms = 200;
    std::thread two_event_server(sse_scripted_server_thread, port);
    wait_for_server();
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/two", port);
    sse = rt_sse_connect(rt_const_cstr(url));
    rt_string received[2] = {nullptr, nullptr};
    std::thread first([&]() { received[0] = rt_sse_recv_for(sse, 2000); });
    std::thread second([&]() { received[1] = rt_sse_recv_for(sse, 2000); });
    first.join();
    second.join();
    std::string first_value = received[0] ? rt_string_cstr(received[0]) : "";
    std::string second_value = received[1] ? rt_string_cstr(received[1]) : "";
    bool complete_pair = (first_value == "one" && second_value == "two") ||
                         (first_value == "two" && second_value == "one");
    test_result("Concurrent receives serialize into two complete events", complete_pair);
    rt_string_unref(received[0]);
    rt_string_unref(received[1]);
    rt_sse_close(sse);
    two_event_server.join();
}

/// @brief RecvFor is a real event deadline and a timeout is lossless
///        (VDOC-151): the partial line survives and the next receive
///        delivers the complete event.
static void test_sse_timed_recv_is_lossless() {
    printf("\nTesting SseClient lossless timed receive:\n");

    const int port = (int)rt_netutils_get_free_port();
    if (port <= 0) {
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    api_server_ready = false;
    api_server_failed = false;
    std::thread server(sse_partial_then_complete_server_thread, port);
    wait_for_server();
    if (api_server_failed) {
        server.join();
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    char url_buf[128];
    snprintf(url_buf, sizeof(url_buf), "http://127.0.0.1:%d/slow", port);
    void *sse = rt_sse_connect(rt_const_cstr(url_buf));
    test_result("SSE connection opens", sse != nullptr && rt_sse_is_open(sse) == 1);

    // The server sent only "data: par" — this timed receive must expire
    // within its budget and must NOT deliver the fragment as an event.
    auto start = std::chrono::steady_clock::now();
    rt_string first = rt_sse_recv_for(sse, 400);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();
    test_result("Timed receive returns empty on timeout", rt_string_cstr(first)[0] == '\0');
    test_result("Timed receive honors its event deadline", elapsed < 800);
    test_result("Connection survives the timeout", rt_sse_is_open(sse) == 1);
    rt_string_unref(first);

    // The completed line must resume from the preserved fragment.
    rt_string second = rt_sse_recv_for(sse, 3000);
    test_result("Next receive delivers the complete event",
                strcmp(rt_string_cstr(second), "partial") == 0);
    rt_string_unref(second);

    rt_sse_close(sse);
    server.join();
}

static void test_sse_resume_after_disconnect() {
    printf("\nTesting SseClient reconnect and resume:\n");

    const int port = (int)rt_netutils_get_free_port();
    if (port <= 0) {
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    api_server_ready = false;
    api_server_failed = false;
    std::thread server(sse_resume_server_thread, port);
    wait_for_server();
    if (api_server_failed) {
        server.join();
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    char url_buf[128];
    snprintf(url_buf, sizeof(url_buf), "http://127.0.0.1:%d/resume", port);
    void *sse = rt_sse_connect(rt_const_cstr(url_buf));
    test_result("SSE resume connection opens", sse != nullptr && rt_sse_is_open(sse) == 1);

    rt_string data1 = rt_sse_recv(sse);
    test_result("SSE first event payload matches", strcmp(rt_string_cstr(data1), "first") == 0);
    rt_string first_id = rt_sse_last_event_id(sse);
    test_result("SSE first event id captured", strcmp(rt_string_cstr(first_id), "1") == 0);

    rt_string data2 = rt_sse_recv(sse);
    test_result("SSE reconnect receives next event", strcmp(rt_string_cstr(data2), "second") == 0);
    rt_string second_id = rt_sse_last_event_id(sse);
    test_result("SSE second event id captured", strcmp(rt_string_cstr(second_id), "2") == 0);
    test_result("SSE reconnect sent Last-Event-ID header", sse_resume_header == "1");

    rt_sse_close(sse);
    server.join();
}

static void test_sse_redirect_to_stream() {
    printf("\nTesting SseClient redirect handling:\n");

    const int target_port = (int)rt_netutils_get_free_port();
    const int redirect_port = get_bindable_local_port();
    if (target_port <= 0 || redirect_port <= 0) {
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    api_server_ready = false;
    api_server_failed = false;
    std::thread target_server(sse_plain_server_thread, target_port);
    wait_for_server();
    if (api_server_failed) {
        target_server.join();
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    void *redirect_server = rt_http_server_new(redirect_port);
    rt_http_server_get(redirect_server, rt_const_cstr("/redirect"), rt_const_cstr("redirect"));
    rt_http_server_bind_handler(
        redirect_server, rt_const_cstr("redirect"), (void *)&http_redirect_source_handler);
    rt_http_server_start(redirect_server);

    const bool redirect_ready = wait_for_condition(
        [&]() {
            return rt_http_server_is_running(redirect_server) == 1 &&
                   rt_http_server_port(redirect_server) > 0;
        },
        2000);
    if (!redirect_ready) {
        rt_http_server_stop(redirect_server);
        target_server.join();
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    char location_buf[128];
    snprintf(location_buf, sizeof(location_buf), "http://127.0.0.1:%d/events", target_port);
    redirect_source_location = location_buf;

    char url_buf[128];
    snprintf(url_buf,
             sizeof(url_buf),
             "http://127.0.0.1:%lld/redirect",
             (long long)rt_http_server_port(redirect_server));
    void *sse = rt_sse_connect(rt_const_cstr(url_buf));
    test_result("SSE redirected connection opens", sse != nullptr && rt_sse_is_open(sse) == 1);

    rt_string data = rt_sse_recv(sse);
    test_result("SSE redirected payload matches", strcmp(rt_string_cstr(data), "hello") == 0);
    rt_string event_type = rt_sse_last_event_type(sse);
    test_result("SSE redirected event type captured",
                strcmp(rt_string_cstr(event_type), "greet") == 0);

    rt_sse_close(sse);
    rt_http_server_stop(redirect_server);
    target_server.join();
}

/// @brief SmtpClient error-model fixes (VDOC-155): constructor validation and
///        trap-free Result sends.
static void test_smtp_validation_and_result_model() {
    printf("\nTesting SmtpClient validation and Result model:\n");

    // Constructor rejects empty and NUL-truncating hosts.
    EXPECT_TRAP(rt_smtp_new(rt_const_cstr(""), 25));
    test_result("Empty host is rejected",
                g_last_trap && strstr(g_last_trap, "invalid host") != nullptr);
    rt_string nul_host = rt_string_from_bytes("smtp.example.com\0evil", 21);
    EXPECT_TRAP(rt_smtp_new(nul_host, 25));
    test_result("Embedded-NUL host is rejected",
                g_last_trap && strstr(g_last_trap, "invalid host") != nullptr);
    rt_string_unref(nul_host);

    // SendResult converts transport traps (connection refused) into ErrStr.
    const int closed_port = (int)rt_netutils_get_free_port();
    if (closed_port <= 0) {
        printf("  SKIPPED: could not allocate local port\n");
        return;
    }
    void *client = rt_smtp_new(rt_const_cstr("127.0.0.1"), closed_port);
    void *res = rt_smtp_send_result(client,
                                    rt_const_cstr("a@example.com"),
                                    rt_const_cstr("b@example.com"),
                                    rt_const_cstr("subject"),
                                    rt_const_cstr("body"));
    test_result("Transport failure surfaces as Err (no trap)", rt_result_is_err(res) == 1);
    rt_string err = rt_result_unwrap_err_str(res);
    test_result("Err message is non-empty", rt_string_cstr(err)[0] != '\0');

    // NUL-truncating message fields fail cleanly instead of sending a prefix.
    rt_string nul_subject = rt_string_from_bytes("hi\0jacked", 9);
    void *res2 = rt_smtp_send_result(client,
                                     rt_const_cstr("a@example.com"),
                                     rt_const_cstr("b@example.com"),
                                     nul_subject,
                                     rt_const_cstr("body"));
    test_result("NUL-truncating field surfaces as Err", rt_result_is_err(res2) == 1);
    rt_string err2 = rt_result_unwrap_err_str(res2);
    test_result("NUL rejection names the cause", strstr(rt_string_cstr(err2), "NUL") != nullptr);
    rt_string_unref(nul_subject);
    if (res && rt_obj_release_check0(res))
        rt_obj_free(res);
    if (res2 && rt_obj_release_check0(res2))
        rt_obj_free(res2);
    if (client && rt_obj_release_check0(client))
        rt_obj_free(client);
}

/// @brief Reject malformed SMTP reply delimiters, codes, and line lengths.
/// @details Each case runs on a fresh loopback listener so parser rejection
///          cannot leave unread bytes that affect the next case. Result APIs
///          must preserve the precise strict-framing diagnostic without
///          leaking or trapping through the public boundary.
static void test_smtp_strict_reply_framing() {
    printf("\nTesting SmtpClient strict reply framing:\n");

    struct reply_case_t {
        const char *name;
        std::string greeting;
        std::string ehlo_response;
        const char *expected_error;
    };

    std::vector<reply_case_t> cases;
    cases.push_back({"Bare-LF greeting is rejected", "220 localhost ready\n", "", "bare LF"});
    cases.push_back(
        {"Malformed reply separator is rejected", "220Xlocalhost ready\r\n", "", "malformed"});
    cases.push_back({"Multiline reply cannot change code",
                     "220 localhost ready\r\n",
                     "250-first line\r\n251 final line\r\n",
                     "code changed"});
    cases.push_back({"Overlong reply line is rejected",
                     std::string("220 ") + std::string(507u, 'x') + "\r\n",
                     "",
                     "too long"});

    for (const reply_case_t &test_case : cases) {
        const int port = (int)rt_netutils_get_free_port();
        if (port <= 0) {
            printf("  SKIPPED: local bind unavailable in this environment\n");
            return;
        }
        api_server_ready = false;
        api_server_failed = false;
        std::thread server(
            smtp_reply_script_server_thread, port, test_case.greeting, test_case.ehlo_response);
        wait_for_server();
        if (api_server_failed) {
            server.join();
            printf("  SKIPPED: local bind unavailable in this environment\n");
            return;
        }

        void *client = rt_smtp_new(rt_const_cstr("127.0.0.1"), port);
        void *result = rt_smtp_send_result(client,
                                           rt_const_cstr("sender@example.com"),
                                           rt_const_cstr("dest@example.com"),
                                           rt_const_cstr("strict"),
                                           rt_const_cstr("body"));
        server.join();
        rt_string error = rt_result_unwrap_err_str(result);
        bool rejected = result && rt_result_is_err(result) == 1 && error &&
                        strstr(rt_string_cstr(error), test_case.expected_error) != nullptr;
        test_result(test_case.name, rejected);
        if (result && rt_obj_release_check0(result))
            rt_obj_free(result);
        if (client && rt_obj_release_check0(client))
            rt_obj_free(client);
    }
}

/// @brief Preserve RFC-compatible HELO fallback for feature-free plaintext sessions.
/// @details Only the explicitly permitted 500/502/504 EHLO failures may enter
///          this path. The test completes DATA to prove parser state is aligned
///          after the legacy handshake.
static void test_smtp_plain_helo_fallback() {
    printf("\nTesting SmtpClient plaintext HELO fallback:\n");
    const int port = (int)rt_netutils_get_free_port();
    if (port <= 0) {
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }
    api_server_ready = false;
    api_server_failed = false;
    std::thread server(smtp_helo_fallback_server_thread, port);
    wait_for_server();
    if (api_server_failed) {
        server.join();
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }
    void *client = rt_smtp_new(rt_const_cstr("127.0.0.1"), port);
    int8_t ok = rt_smtp_send(client,
                             rt_const_cstr("sender@example.com"),
                             rt_const_cstr("dest@example.com"),
                             rt_const_cstr("legacy"),
                             rt_const_cstr("fallback body"));
    server.join();
    test_result("Plain send falls back from EHLO to HELO", ok == 1);
    test_result("HELO fallback remains aligned through DATA",
                smtp_captured_message.find("fallback body") != std::string::npos);
    if (client && rt_obj_release_check0(client))
        rt_obj_free(client);
}

static void test_smtp_plain_send_sanitizes_and_dot_stuffs() {
    printf("\nTesting SmtpClient plain send path:\n");

    const int port = (int)rt_netutils_get_free_port();
    if (port <= 0) {
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    api_server_ready = false;
    api_server_failed = false;
    std::thread server(smtp_plain_server_thread, port);
    wait_for_server();
    if (api_server_failed) {
        server.join();
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    std::string body = ".leading line\rsecond line\r\n.third line\n";
    for (int line = 0; line < 3000; line++) {
        body += line % 17 == 0 ? ".stream-line-" : "stream-line-";
        body += std::to_string(line);
        body += line % 3 == 0 ? "\r" : (line % 3 == 1 ? "\n" : "\r\n");
    }
    body += "tail-without-newline";
    rt_string body_string = rt_string_from_bytes(body.data(), body.size());

    void *smtp = rt_smtp_new(rt_const_cstr("127.0.0.1"), port);
    void *send_result = rt_smtp_send_result(smtp,
                                            rt_const_cstr("sender@example.com"),
                                            rt_const_cstr("dest@example.com"),
                                            rt_const_cstr("Hello\r\nInjected: yes"),
                                            body_string);
    rt_string_unref(body_string);

    test_result("SMTP SendResult succeeds", rt_result_is_ok(send_result) == 1);
    rt_string success_error = rt_smtp_last_error(smtp);
    test_result("SMTP last error is empty", strcmp(rt_string_cstr(success_error), "") == 0);
    rt_str_release_maybe(success_error);
    if (send_result && rt_obj_release_check0(send_result))
        rt_obj_free(send_result);

    server.join();

    test_result("SMTP captured From header",
                smtp_captured_message.find("From: sender@example.com") != std::string::npos);
    test_result("SMTP captured To header",
                smtp_captured_message.find("To: dest@example.com") != std::string::npos);
    test_result("SMTP subject was sanitized",
                smtp_captured_message.find("Subject: Hello  Injected: yes") != std::string::npos);
    test_result("SMTP content type header present",
                smtp_captured_message.find("Content-Type: text/plain; charset=utf-8") !=
                    std::string::npos);
    test_result("SMTP body was dot-stuffed",
                smtp_captured_message.find("..leading line") != std::string::npos);
    test_result("SMTP body preserved following line",
                smtp_captured_message.find("second line") != std::string::npos);
    test_result("SMTP streams beyond one staging buffer",
                smtp_captured_message.find("..stream-line-2992") != std::string::npos);
    test_result("SMTP appends CRLF before the terminator",
                smtp_captured_message.find("tail-without-newline\n") != std::string::npos);
    if (smtp && rt_obj_release_check0(smtp))
        rt_obj_free(smtp);
}

/// @brief Verify concurrent Close interrupts a stalled greeting and permits reconnect.
/// @details The server intentionally withholds its first greeting for 750 ms.
///          Close must complete the send before that delay expires, return an
///          error Result, and leave the same client usable for a second full session.
static void test_smtp_close_interrupts_and_reconnects() {
    printf("\nTesting SmtpClient cancellation and reconnect:\n");
    const int port = (int)rt_netutils_get_free_port();
    if (port <= 0) {
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }
    api_server_ready = false;
    api_server_failed = false;
    smtp_fixture_client_accepted = false;
    std::thread server(smtp_cancel_reconnect_server_thread, port);
    wait_for_server();
    if (api_server_failed) {
        server.join();
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    void *client = rt_smtp_new(rt_const_cstr("127.0.0.1"), port);
    void *first_result = nullptr;
    std::thread sender([&]() {
        first_result = rt_smtp_send_result(client,
                                           rt_const_cstr("sender@example.com"),
                                           rt_const_cstr("dest@example.com"),
                                           rt_const_cstr("cancel"),
                                           rt_const_cstr("first"));
    });
    test_result("Stalled SMTP connection is accepted",
                wait_for_condition([]() { return smtp_fixture_client_accepted.load(); }, 2000));
    const auto cancel_start = std::chrono::steady_clock::now();
    rt_smtp_close(client);
    sender.join();
    const auto cancel_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - cancel_start)
                               .count();
    test_result("Close interrupts the stalled greeting promptly", cancel_ms < 600);
    test_result("Cancelled SendResult returns Err",
                first_result && rt_result_is_err(first_result) == 1);

    void *second_result = rt_smtp_send_result(client,
                                              rt_const_cstr("sender@example.com"),
                                              rt_const_cstr("dest@example.com"),
                                              rt_const_cstr("reconnect"),
                                              rt_const_cstr("second succeeds"));
    server.join();
    test_result("A send after cancellation reconnects cleanly",
                second_result && rt_result_is_ok(second_result) == 1);
    test_result("Reconnected session reaches DATA",
                smtp_captured_message.find("second succeeds") != std::string::npos);
    if (first_result && rt_obj_release_check0(first_result))
        rt_obj_free(first_result);
    if (second_result && rt_obj_release_check0(second_result))
        rt_obj_free(second_result);
    if (client && rt_obj_release_check0(client))
        rt_obj_free(client);
}

/// @brief Verify Close also interrupts a stalled STARTTLS handshake promptly.
/// @details The fixture consumes one ClientHello byte and withholds the rest of
///          the TLS record. This exercises cancellation-aware TLS record polling,
///          not merely the plaintext SMTP greeting path.
static void test_smtp_close_interrupts_starttls_handshake() {
    printf("\nTesting SmtpClient STARTTLS cancellation:\n");
    const int port = (int)rt_netutils_get_free_port();
    if (port <= 0) {
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }
    api_server_ready = false;
    api_server_failed = false;
    smtp_fixture_tls_client_hello = false;
    std::thread server(smtp_stalled_starttls_server_thread, port);
    wait_for_server();
    if (api_server_failed) {
        server.join();
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    void *client = rt_smtp_new(rt_const_cstr("127.0.0.1"), port);
    rt_smtp_set_tls(client, 1);
    void *send_result = nullptr;
    std::thread sender([&]() {
        send_result = rt_smtp_send_result(client,
                                          rt_const_cstr("sender@example.com"),
                                          rt_const_cstr("dest@example.com"),
                                          rt_const_cstr("cancel TLS"),
                                          rt_const_cstr("stalled handshake"));
    });
    test_result("SMTP client enters the STARTTLS handshake",
                wait_for_condition([]() { return smtp_fixture_tls_client_hello.load(); }, 2000));
    const auto cancel_start = std::chrono::steady_clock::now();
    rt_smtp_close(client);
    sender.join();
    const auto cancel_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - cancel_start)
                               .count();
    test_result("Close interrupts the stalled STARTTLS handshake promptly", cancel_ms < 600);
    test_result("Cancelled STARTTLS SendResult returns Err",
                send_result && rt_result_is_err(send_result) == 1);
    server.join();

    if (send_result && rt_obj_release_check0(send_result))
        rt_obj_free(send_result);
    if (client && rt_obj_release_check0(client))
        rt_obj_free(client);
}

/// @brief Verify two send callers serialize complete independent SMTP sessions.
/// @details Both threads share one managed client. Successful delivery of both
///          distinct subjects proves transport, reply-buffer, and LastError
///          state are not concurrently overwritten.
static void test_smtp_concurrent_sends_serialize() {
    printf("\nTesting SmtpClient concurrent send serialization:\n");
    const int port = (int)rt_netutils_get_free_port();
    if (port <= 0) {
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }
    api_server_ready = false;
    api_server_failed = false;
    std::thread server(smtp_two_session_server_thread, port);
    wait_for_server();
    if (api_server_failed) {
        server.join();
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    void *client = rt_smtp_new(rt_const_cstr("127.0.0.1"), port);
    std::atomic<int> first_ok{0};
    std::atomic<int> second_ok{0};
    std::thread first([&]() {
        first_ok = rt_smtp_send(client,
                                rt_const_cstr("one@example.com"),
                                rt_const_cstr("dest@example.com"),
                                rt_const_cstr("subject-one"),
                                rt_const_cstr("body-one"));
    });
    std::thread second([&]() {
        second_ok = rt_smtp_send(client,
                                 rt_const_cstr("two@example.com"),
                                 rt_const_cstr("dest@example.com"),
                                 rt_const_cstr("subject-two"),
                                 rt_const_cstr("body-two"));
    });
    first.join();
    second.join();
    server.join();
    test_result("Both concurrent send callers succeed", first_ok == 1 && second_ok == 1);
    test_result("First serialized message remains complete",
                smtp_captured_message.find("Subject: subject-one") != std::string::npos &&
                    smtp_captured_message.find("body-one") != std::string::npos);
    test_result("Second serialized message remains complete",
                smtp_captured_message.find("Subject: subject-two") != std::string::npos &&
                    smtp_captured_message.find("body-two") != std::string::npos);
    if (client && rt_obj_release_check0(client))
        rt_obj_free(client);
}

static void test_smtp_requires_starttls_capability() {
    printf("\nTesting SmtpClient STARTTLS capability enforcement:\n");

    const int port = (int)rt_netutils_get_free_port();
    if (port <= 0) {
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    api_server_ready = false;
    api_server_failed = false;
    std::thread server(smtp_no_starttls_server_thread, port);
    wait_for_server();
    if (api_server_failed) {
        server.join();
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    void *smtp = rt_smtp_new(rt_const_cstr("127.0.0.1"), port);
    rt_smtp_set_tls(smtp, 1);
    void *send_result = rt_smtp_send_result(smtp,
                                            rt_const_cstr("sender@example.com"),
                                            rt_const_cstr("dest@example.com"),
                                            rt_const_cstr("Hello"),
                                            rt_const_cstr("Body"));
    rt_string error = rt_smtp_last_error(smtp);
    rt_string result_error = rt_result_unwrap_err_str(send_result);

    test_result("SMTP SendResult rejects missing STARTTLS capability",
                rt_result_is_err(send_result) == 1);
    test_result("SMTP error mentions STARTTLS",
                strstr(rt_string_cstr(error), "STARTTLS") != nullptr);
    test_result("SMTP Result error mentions STARTTLS",
                strstr(rt_string_cstr(result_error), "STARTTLS") != nullptr);
    rt_str_release_maybe(error);
    if (send_result && rt_obj_release_check0(send_result))
        rt_obj_free(send_result);

    server.join();
}

static void test_smtp_accepts_forwarding_recipient_codes() {
    printf("\nTesting SmtpClient forwarding recipient handling:\n");

    const int port = (int)rt_netutils_get_free_port();
    if (port <= 0) {
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    api_server_ready = false;
    api_server_failed = false;
    std::thread server(smtp_forwarding_rcpt_server_thread, port);
    wait_for_server();
    if (api_server_failed) {
        server.join();
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    smtp_captured_message.clear();
    void *smtp = rt_smtp_new(rt_const_cstr("127.0.0.1"), port);
    int8_t ok = rt_smtp_send(smtp,
                             rt_const_cstr("sender@example.com"),
                             rt_const_cstr("dest@example.com"),
                             rt_const_cstr("Forwarded"),
                             rt_const_cstr("Accepted via 251"));

    test_result("SMTP send accepts 251 recipient forwarding", ok == 1);
    server.join();
    test_result("SMTP forwarded message still enters DATA path",
                smtp_captured_message.find("Accepted via 251") != std::string::npos);
}

static void test_http_client_cookie_scope() {
    printf("\nTesting HttpClient cookie scope:\n");

    const int port = (int)rt_netutils_get_free_port();
    if (port <= 0) {
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    api_server_ready = false;
    api_server_failed = false;
    std::thread server(http_cookie_scope_server_thread, port);
    wait_for_server();
    if (api_server_failed) {
        server.join();
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    char url_buf[128];
    void *client = rt_http_client_new();

    snprintf(url_buf, sizeof(url_buf), "http://127.0.0.1:%d/set", port);
    void *res1 = rt_http_client_get(client, rt_const_cstr(url_buf));
    test_result("HttpClient initial cookie response succeeds", rt_http_res_status(res1) == 200);
    void *cookies = rt_http_client_get_cookies(client, rt_const_cstr("127.0.0.1"));
    test_result("HttpClient rejects secure cookies set over HTTP",
                rt_map_has(cookies, rt_const_cstr("secureToken")) == 0);
    if (cookies && rt_obj_release_check0(cookies))
        rt_obj_free(cookies);

    snprintf(url_buf, sizeof(url_buf), "http://127.0.0.1:%d/app/data", port);
    void *res2 = rt_http_client_get(client, rt_const_cstr(url_buf));
    test_result("HttpClient matching-path request succeeds", rt_http_res_status(res2) == 200);

    snprintf(url_buf, sizeof(url_buf), "http://127.0.0.1:%d/other", port);
    void *res3 = rt_http_client_get(client, rt_const_cstr(url_buf));
    test_result("HttpClient non-matching-path request succeeds", rt_http_res_status(res3) == 200);

    server.join();

    test_result("Path cookie sent on matching path",
                http_cookie_headers[1].find("appToken=alpha") != std::string::npos);
    test_result("Root cookie sent on matching path",
                http_cookie_headers[1].find("rootToken=beta") != std::string::npos);
    test_result("Secure cookie withheld on plain HTTP",
                http_cookie_headers[1].find("secureToken=secret") == std::string::npos);
    test_result("Path cookie withheld outside its path",
                http_cookie_headers[2].find("appToken=alpha") == std::string::npos);
    test_result("Root cookie still sent outside the path",
                http_cookie_headers[2].find("rootToken=beta") != std::string::npos);
}

static void test_http_client_manual_cookie_is_host_only() {
    printf("\nTesting HttpClient manual cookie host scoping:\n");

    const int port = (int)rt_netutils_get_free_port();
    if (port <= 0) {
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    api_server_ready = false;
    api_server_failed = false;
    std::thread server(http_cookie_scope_server_thread, port);
    wait_for_server();
    if (api_server_failed) {
        server.join();
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    char url_buf[128];
    void *client = rt_http_client_new();
    rt_http_client_set_cookie(client,
                              rt_const_cstr("localhost"),
                              rt_const_cstr("manualToken"),
                              rt_const_cstr("hostonly"));

    snprintf(url_buf, sizeof(url_buf), "http://localhost:%d/set", port);
    void *res1 = rt_http_client_get(client, rt_const_cstr(url_buf));
    test_result("HttpClient localhost cookie request succeeds", rt_http_res_status(res1) == 200);

    snprintf(url_buf, sizeof(url_buf), "http://127.0.0.1:%d/app/data", port);
    void *res2 = rt_http_client_get(client, rt_const_cstr(url_buf));
    test_result("HttpClient IP request succeeds", rt_http_res_status(res2) == 200);

    snprintf(url_buf, sizeof(url_buf), "http://localhost:%d/other", port);
    void *res3 = rt_http_client_get(client, rt_const_cstr(url_buf));
    test_result("HttpClient second localhost request succeeds", rt_http_res_status(res3) == 200);

    server.join();

    test_result("Manual cookie sent to matching host",
                http_cookie_headers[0].find("manualToken=hostonly") != std::string::npos);
    test_result("Manual cookie withheld from different host spelling",
                http_cookie_headers[1].find("manualToken=hostonly") == std::string::npos);
    test_result("Host-only response cookies withheld from different host spelling",
                http_cookie_headers[1].find("rootToken=beta") == std::string::npos);
    test_result("Manual cookie still sent to original host",
                http_cookie_headers[2].find("manualToken=hostonly") != std::string::npos);
}

/// @brief Manual cookie-jar safety rules (VDOC-153): response-grade
///        validation, host-only scope, case-sensitive names, empty values,
///        and explicit deletion.
static void test_http_client_cookie_jar_rules() {
    printf("\nTesting HttpClient cookie jar safety rules:\n");

    void *client = rt_http_client_new();

    // The finding's supercookie probe: a cookie set for "com" must never be
    // visible to example.com (host-only storage), and header-syntax
    // injection is rejected outright.
    rt_http_client_set_cookie(
        client, rt_const_cstr("com"), rt_const_cstr("super"), rt_const_cstr("cookie"));
    void *super_scope = rt_http_client_get_cookies(client, rt_const_cstr("example.com"));
    test_result("No supercookie: 'com' cookie invisible to example.com",
                rt_map_len(super_scope) == 0);
    EXPECT_TRAP(rt_http_client_set_cookie(
        client, rt_const_cstr("example.com"), rt_const_cstr("bad;name"), rt_const_cstr("v")));
    test_result("Separator in cookie name traps",
                g_last_trap && strstr(g_last_trap, "name or value") != nullptr);
    EXPECT_TRAP(rt_http_client_set_cookie(
        client, rt_const_cstr("example.com"), rt_const_cstr("n"), rt_const_cstr("a;b")));
    test_result("Semicolon in cookie value traps",
                g_last_trap && strstr(g_last_trap, "name or value") != nullptr);

    // Manual cookies are host-only: not visible to subdomains.
    rt_http_client_set_cookie(
        client, rt_const_cstr("example.com"), rt_const_cstr("SID"), rt_const_cstr("one"));
    void *sub = rt_http_client_get_cookies(client, rt_const_cstr("sub.example.com"));
    test_result("Manual cookie is host-only", rt_map_len(sub) == 0);
    void *exact = rt_http_client_get_cookies(client, rt_const_cstr("example.com"));
    test_result("Manual cookie visible on the exact host", rt_map_len(exact) == 1);

    // Cookie names are case-sensitive: SID and sid coexist.
    rt_http_client_set_cookie(
        client, rt_const_cstr("example.com"), rt_const_cstr("sid"), rt_const_cstr("two"));
    void *both = rt_http_client_get_cookies(client, rt_const_cstr("example.com"));
    test_result("Case-distinct cookie names coexist", rt_map_len(both) == 2);

    // Empty values are stored, and deletion is explicit.
    rt_http_client_set_cookie(
        client, rt_const_cstr("example.com"), rt_const_cstr("empty"), rt_const_cstr(""));
    void *with_empty = rt_http_client_get_cookies(client, rt_const_cstr("example.com"));
    test_result("Empty-valued cookie is stored", rt_map_len(with_empty) == 3);
    rt_http_client_delete_cookie(client, rt_const_cstr("example.com"), rt_const_cstr("SID"));
    rt_http_client_delete_cookie(client, rt_const_cstr("example.com"), rt_const_cstr("empty"));
    void *after_delete = rt_http_client_get_cookies(client, rt_const_cstr("example.com"));
    test_result("DeleteCookie removes exactly the named cookies", rt_map_len(after_delete) == 1);
}

static void test_http_client_cross_origin_redirect_strips_sensitive_headers() {
    printf("\nTesting HttpClient cross-origin redirect credential stripping:\n");

    const int target_port = get_bindable_local_port();
    const int redirect_port = get_bindable_local_port();
    if (target_port <= 0 || redirect_port <= 0) {
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    void *target_server = rt_http_server_new(target_port);
    void *redirect_server = rt_http_server_new(redirect_port);
    rt_http_server_get(target_server, rt_const_cstr("/final"), rt_const_cstr("target"));
    rt_http_server_bind_handler(
        target_server, rt_const_cstr("target"), (void *)&http_redirect_target_handler);
    rt_http_server_get(redirect_server, rt_const_cstr("/redirect"), rt_const_cstr("redirect"));
    rt_http_server_bind_handler(
        redirect_server, rt_const_cstr("redirect"), (void *)&http_redirect_source_handler);

    rt_http_server_start(target_server);
    rt_http_server_start(redirect_server);

    const bool ready = wait_for_condition(
        [&]() {
            return rt_http_server_is_running(target_server) == 1 &&
                   rt_http_server_is_running(redirect_server) == 1 &&
                   rt_http_server_port(target_server) > 0 &&
                   rt_http_server_port(redirect_server) > 0;
        },
        2000);
    if (!ready) {
        rt_http_server_stop(redirect_server);
        rt_http_server_stop(target_server);
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    redirect_target_authorization_header.clear();
    redirect_target_api_key_header.clear();

    char location_buf[128];
    snprintf(location_buf,
             sizeof(location_buf),
             "http://127.0.0.1:%lld/final",
             (long long)rt_http_server_port(target_server));
    redirect_source_location = location_buf;

    char url_buf[128];
    snprintf(url_buf,
             sizeof(url_buf),
             "http://127.0.0.1:%lld/redirect",
             (long long)rt_http_server_port(redirect_server));

    void *client = rt_http_client_new();
    rt_http_client_set_header(
        client, rt_const_cstr("Authorization"), rt_const_cstr("Bearer secret"));
    rt_http_client_set_header(client, rt_const_cstr("X-API-Key"), rt_const_cstr("top-secret"));

    void *res = rt_http_client_get(client, rt_const_cstr(url_buf));
    test_result("HttpClient follows cross-origin redirect", rt_http_res_status(res) == 200);
    rt_http_server_stop(redirect_server);
    rt_http_server_stop(target_server);

    test_result("HttpClient strips Authorization on cross-origin redirect",
                redirect_target_authorization_header.empty());
    test_result("HttpClient strips API key headers on cross-origin redirect",
                redirect_target_api_key_header.empty());
}

static void test_http_client_consumes_informational_responses() {
    printf("\nTesting HttpClient informational response handling:\n");

    const int port = (int)rt_netutils_get_free_port();
    if (port <= 0) {
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    std::atomic<bool> ready{false};
    std::atomic<bool> failed{false};
    std::thread server(http_informational_server_thread, port, &ready, &failed);
    if (!wait_for_condition([&]() { return ready.load(); }, 1000) || failed.load()) {
        server.join();
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    char url_buf[128];
    snprintf(url_buf, sizeof(url_buf), "http://127.0.0.1:%d/hints", port);
    void *client = rt_http_client_new();
    void *res = rt_http_client_get(client, rt_const_cstr(url_buf));
    test_result("HttpClient returns the final response after 103", rt_http_res_status(res) == 200);
    rt_string body = rt_http_res_body_str(res);
    test_result("HttpClient preserves the final response body",
                strcmp(rt_string_cstr(body), "hello") == 0);
    rt_string_unref(body);

    server.join();
}

static void test_http_client_keepalive_reuse() {
    printf("\nTesting HttpClient keep-alive reuse:\n");

    const int port = (int)rt_netutils_get_free_port();
    if (port <= 0) {
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    api_server_ready = false;
    api_server_failed = false;
    std::thread server(http_keepalive_server_thread, port);
    wait_for_server();
    if (api_server_failed) {
        server.join();
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    char url_buf[128];
    snprintf(url_buf, sizeof(url_buf), "http://127.0.0.1:%d/reuse", port);

    void *client = rt_http_client_new();
    test_result("HttpClient keep-alive defaults on", rt_http_client_get_keep_alive(client) == 1);

    void *res1 = rt_http_client_get(client, rt_const_cstr(url_buf));
    test_result("HttpClient first keep-alive request succeeds", rt_http_res_status(res1) == 200);

    void *res2 = rt_http_client_get(client, rt_const_cstr(url_buf));
    test_result("HttpClient second keep-alive request succeeds", rt_http_res_status(res2) == 200);

    server.join();

    test_result("HttpClient reused a single accepted socket", http_keepalive_reused.load());
    test_result("HttpClient keep-alive did not require a second accept",
                http_keepalive_accept_count.load() == 1);
    test_result("HttpClient first request advertises keep-alive",
                http_keepalive_connection_headers[0].find("keep-alive") != std::string::npos);
    test_result("HttpClient second request advertises keep-alive",
                http_keepalive_connection_headers[1].find("keep-alive") != std::string::npos);
}

// ── Router concurrency (VDOC-143) ──────────────────────────────────────────

/// @brief Concurrent Add and Match on one router must be safe: Add publishes
///        under a write lock while Match holds a read lock.
static void test_router_concurrent_add_and_match() {
    printf("\nTesting HttpRouter concurrent add/match:\n");

    void *router = rt_http_router_new();
    rt_http_router_get(router, rt_const_cstr("/warm/:id"));

    std::atomic<bool> stop{false};
    std::atomic<long> matches{0};
    auto matcher = [&]() {
        while (!stop.load()) {
            void *m = rt_http_router_match(router, rt_const_cstr("GET"), rt_const_cstr("/warm/7"));
            assert(m != nullptr);
            if (rt_obj_release_check0(m))
                rt_obj_free(m);
            matches.fetch_add(1);
        }
    };
    std::thread t1(matcher);
    std::thread t2(matcher);

    const int kRoutes = 2000;
    for (int i = 0; i < kRoutes; i++) {
        char pat[64];
        int n = snprintf(pat, sizeof(pat), "/route%d/:x", i);
        rt_string pattern = rt_string_from_bytes(pat, (size_t)n);
        rt_http_router_get(router, pattern);
        rt_string_unref(pattern);
    }

    stop = true;
    t1.join();
    t2.join();

    test_result("Concurrent adds all registered", rt_http_router_count(router) == kRoutes + 1);
    test_result("Matcher threads made progress during registration", matches.load() > 0);
    if (rt_obj_release_check0(router))
        rt_obj_free(router);
}

// ── Header canonicalization (VDOC-139) ─────────────────────────────────────

static std::string http_header_capture_raw;

/// @brief Accept one client, capture the raw request head (through the blank
///        line) into `http_header_capture_raw`, and answer 200/close.
static void http_header_capture_server_thread(int port) {
    void *server = rt_tcp_server_listen(port);
    if (!server) {
        api_server_failed = true;
        api_server_ready = true;
        return;
    }

    api_server_failed = false;
    api_server_ready = true;
    http_header_capture_raw.clear();

    void *client = rt_tcp_server_accept_for(server, 5000);
    if (!client) {
        rt_tcp_server_close(server);
        return;
    }

    std::string raw;
    while (raw.size() < 16384) {
        rt_string ch = rt_tcp_recv_str(client, 1);
        if (!ch)
            break;
        const char *c = rt_string_cstr(ch);
        if (!c || !*c) {
            rt_string_unref(ch);
            break;
        }
        raw.push_back(c[0]);
        rt_string_unref(ch);
        if (raw.size() >= 4 && raw.compare(raw.size() - 4, 4, "\r\n\r\n") == 0)
            break;
    }
    http_header_capture_raw = raw;

    send_text(client,
              "HTTP/1.1 200 OK\r\n"
              "Content-Length: 2\r\n"
              "Connection: close\r\n"
              "\r\n"
              "ok");
    rt_tcp_close(client);
    rt_tcp_server_close(server);
}

/// @brief Count case-insensitive occurrences of a header name at field starts.
static int count_header_fields_ci(const std::string &raw, const char *name) {
    std::string lowered(raw);
    for (char &c : lowered)
        c = (char)tolower((unsigned char)c);
    std::string needle = std::string("\r\n") + name + ":";
    for (char &c : needle)
        c = (char)tolower((unsigned char)c);
    int count = 0;
    size_t pos = 0;
    while ((pos = lowered.find(needle, pos)) != std::string::npos) {
        count++;
        pos += needle.size();
    }
    return count;
}

/// @brief `SetHeader` must replace case-insensitively while `AddHeader`
///        appends; RestClient defaults and `ClearAuth` must be
///        case-insensitive (VDOC-139).
static void test_header_case_insensitive_configuration() {
    printf("\nTesting case-insensitive header configuration:\n");

    // Round 1: HttpReq SetHeader replace vs AddHeader append.
    int port = (int)rt_netutils_get_free_port();
    if (port <= 0) {
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }
    api_server_ready = false;
    api_server_failed = false;
    std::thread server(http_header_capture_server_thread, port);
    wait_for_server();
    if (api_server_failed) {
        server.join();
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    char url_buf[128];
    snprintf(url_buf, sizeof(url_buf), "http://127.0.0.1:%d/headers", port);
    void *req = rt_http_req_new(rt_const_cstr("GET"), rt_const_cstr(url_buf));
    rt_http_req_set_header(req, rt_const_cstr("X-Test"), rt_const_cstr("first"));
    rt_http_req_set_header(req, rt_const_cstr("x-test"), rt_const_cstr("second"));
    rt_http_req_add_header(req, rt_const_cstr("X-Multi"), rt_const_cstr("1"));
    rt_http_req_add_header(req, rt_const_cstr("x-multi"), rt_const_cstr("2"));
    void *res = rt_http_req_send(req);
    test_result("Header request succeeds", rt_http_res_status(res) == 200);
    server.join();

    test_result("SetHeader replaces across casings",
                count_header_fields_ci(http_header_capture_raw, "X-Test") == 1);
    test_result("SetHeader keeps the last value",
                http_header_capture_raw.find("second") != std::string::npos &&
                    http_header_capture_raw.find("first") == std::string::npos);
    test_result("AddHeader appends repeated fields",
                count_header_fields_ci(http_header_capture_raw, "X-Multi") == 2);

    // Round 2: RestClient mixed-case defaults and ClearAuth.
    port = (int)rt_netutils_get_free_port();
    if (port <= 0) {
        printf("  SKIPPED: local bind unavailable for RestClient round\n");
        return;
    }
    api_server_ready = false;
    api_server_failed = false;
    std::thread server2(http_header_capture_server_thread, port);
    wait_for_server();
    if (api_server_failed) {
        server2.join();
        printf("  SKIPPED: local bind unavailable for RestClient round\n");
        return;
    }

    char base_buf[128];
    snprintf(base_buf, sizeof(base_buf), "http://127.0.0.1:%d", port);
    void *rest = rt_restclient_new(rt_const_cstr(base_buf));
    rt_restclient_set_header(rest, rt_const_cstr("Authorization"), rt_const_cstr("Bearer one"));
    rt_restclient_set_header(rest, rt_const_cstr("authorization"), rt_const_cstr("Bearer two"));
    rt_restclient_clear_auth(rest);
    rt_restclient_set_header(rest, rt_const_cstr("X-Keep"), rt_const_cstr("yes"));
    void *rest_res = rt_restclient_get(rest, rt_const_cstr("/auth"));
    test_result("RestClient request succeeds",
                rest_res != nullptr && rt_http_res_status(rest_res) == 200);
    server2.join();

    test_result("ClearAuth removes every Authorization casing",
                count_header_fields_ci(http_header_capture_raw, "Authorization") == 0);
    test_result("Unrelated defaults survive ClearAuth",
                count_header_fields_ci(http_header_capture_raw, "X-Keep") == 1);
}

/// @brief Standalone `HttpReq.SetKeepAlive(true)` must reuse a pooled socket
///        via the process-wide default connection pool (VDOC-138).
static void test_standalone_httpreq_keepalive_reuse() {
    printf("\nTesting standalone HttpReq keep-alive reuse:\n");

    const int port = (int)rt_netutils_get_free_port();
    if (port <= 0) {
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    api_server_ready = false;
    api_server_failed = false;
    std::thread server(http_keepalive_server_thread, port);
    wait_for_server();
    if (api_server_failed) {
        server.join();
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    char url_buf[128];
    snprintf(url_buf, sizeof(url_buf), "http://127.0.0.1:%d/standalone-reuse", port);

    void *req1 = rt_http_req_new(rt_const_cstr("GET"), rt_const_cstr(url_buf));
    rt_http_req_set_keep_alive(req1, 1);
    void *res1 = rt_http_req_send(req1);
    test_result("Standalone first keep-alive request succeeds", rt_http_res_status(res1) == 200);

    void *req2 = rt_http_req_new(rt_const_cstr("GET"), rt_const_cstr(url_buf));
    rt_http_req_set_keep_alive(req2, 1);
    void *res2 = rt_http_req_send(req2);
    test_result("Standalone second keep-alive request succeeds", rt_http_res_status(res2) == 200);

    server.join();

    test_result("Standalone requests reused a single accepted socket",
                http_keepalive_reused.load());
    test_result("Standalone keep-alive did not require a second accept",
                http_keepalive_accept_count.load() == 1);
}

static void test_http_server_keepalive_response() {
    printf("\nTesting HttpServer keep-alive responses:\n");

    const int port = get_bindable_local_port();
    if (port <= 0) {
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    void *server = rt_http_server_new(port);
    rt_http_server_get(server, rt_const_cstr("/ping"), rt_const_cstr("ping"));
    rt_http_server_bind_handler(
        server, rt_const_cstr("ping"), (void *)&http_server_keepalive_handler);
    rt_http_server_start(server);
    if (!wait_for_condition([&]() { return rt_http_server_is_running(server) == 1; }, 1000)) {
        rt_http_server_stop(server);
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    void *client = rt_tcp_connect(rt_const_cstr("127.0.0.1"), port);

    send_text(client,
              "GET /ping HTTP/1.1\r\n"
              "Host: 127.0.0.1\r\n"
              "Connection: keep-alive\r\n"
              "\r\n");
    rt_string status1 = rt_tcp_recv_line(client);
    test_result("HttpServer first response status is 200",
                strcmp(rt_string_cstr(status1), "HTTP/1.1 200 OK") == 0);
    rt_string_unref(status1);
    std::string connection1 = read_http_header_value(client, "Connection");
    rt_string body1_bytes = nullptr;
    void *body1 = rt_tcp_recv_exact(client, 2);
    body1_bytes = rt_bytes_to_str(body1);
    test_result("HttpServer first response body matches",
                strcmp(rt_string_cstr(body1_bytes), "ok") == 0);
    rt_string_unref(body1_bytes);

    send_text(client,
              "GET /ping HTTP/1.1\r\n"
              "Host: 127.0.0.1\r\n"
              "Connection: close\r\n"
              "\r\n");
    rt_string status2 = rt_tcp_recv_line(client);
    test_result("HttpServer second response status is 200",
                strcmp(rt_string_cstr(status2), "HTTP/1.1 200 OK") == 0);
    rt_string_unref(status2);
    std::string connection2 = read_http_header_value(client, "Connection");
    void *body2 = rt_tcp_recv_exact(client, 2);
    rt_string body2_str = rt_bytes_to_str(body2);
    test_result("HttpServer second response body matches",
                strcmp(rt_string_cstr(body2_str), "ok") == 0);
    rt_string_unref(body2_str);

    rt_http_server_stop(server);
    rt_tcp_close(client);

    test_result("HttpServer honors keep-alive on the first response",
                connection1.find("keep-alive") != std::string::npos);
    test_result("HttpServer closes when the request asks for close",
                connection2.find("close") != std::string::npos);
}

static void test_http_server_pipelined_keepalive_requests() {
    printf("\nTesting HttpServer pipelined keep-alive requests:\n");

    const int port = get_bindable_local_port();
    if (port <= 0) {
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    void *server = rt_http_server_new(port);
    rt_http_server_get(server, rt_const_cstr("/ping"), rt_const_cstr("ping"));
    rt_http_server_bind_handler(
        server, rt_const_cstr("ping"), (void *)&http_server_keepalive_handler);
    rt_http_server_start(server);
    if (!wait_for_condition([&]() { return rt_http_server_is_running(server) == 1; }, 1000)) {
        rt_http_server_stop(server);
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    void *client = rt_tcp_connect(rt_const_cstr("127.0.0.1"), port);
    send_text(client,
              "GET /ping HTTP/1.1\r\n"
              "Host: 127.0.0.1\r\n"
              "Connection: keep-alive\r\n"
              "\r\n"
              "GET /ping HTTP/1.1\r\n"
              "Host: 127.0.0.1\r\n"
              "Connection: close\r\n"
              "\r\n");

    rt_string status1 = rt_tcp_recv_line(client);
    test_result("HttpServer pipelined first response status is 200",
                strcmp(rt_string_cstr(status1), "HTTP/1.1 200 OK") == 0);
    rt_string_unref(status1);
    std::string connection1 = read_http_header_value(client, "Connection");
    void *body1 = rt_tcp_recv_exact(client, 2);
    rt_string body1_str = rt_bytes_to_str(body1);
    test_result("HttpServer pipelined first response body matches",
                strcmp(rt_string_cstr(body1_str), "ok") == 0);
    rt_string_unref(body1_str);

    rt_string status2 = rt_tcp_recv_line(client);
    test_result("HttpServer pipelined second response status is 200",
                strcmp(rt_string_cstr(status2), "HTTP/1.1 200 OK") == 0);
    rt_string_unref(status2);
    std::string connection2 = read_http_header_value(client, "Connection");
    void *body2 = rt_tcp_recv_exact(client, 2);
    rt_string body2_str = rt_bytes_to_str(body2);
    test_result("HttpServer pipelined second response body matches",
                strcmp(rt_string_cstr(body2_str), "ok") == 0);
    rt_string_unref(body2_str);

    rt_http_server_stop(server);
    rt_tcp_close(client);

    test_result("HttpServer pipelined first response keeps connection alive",
                connection1.find("keep-alive") != std::string::npos);
    test_result("HttpServer pipelined second response closes connection",
                connection2.find("close") != std::string::npos);
}

static void test_https_server_roundtrip() {
    printf("\nTesting HttpsServer round-trip and TLS keep-alive:\n");

    const int port = get_bindable_local_port();
    if (port <= 0) {
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    temp_tls_files_t tls_files = create_temp_tls_files();
    if (!tls_files.valid) {
        printf("  SKIPPED: could not write temporary TLS fixture files\n");
        return;
    }

    void *server = rt_https_server_new(port,
                                       rt_const_cstr(tls_files.cert_path.c_str()),
                                       rt_const_cstr(tls_files.key_path.c_str()));
    rt_https_server_get(server, rt_const_cstr("/secure"), rt_const_cstr("secure"));
    rt_https_server_bind_handler(
        server, rt_const_cstr("secure"), (void *)&https_server_secure_handler);
    rt_https_server_start(server);
    if (!wait_for_condition([&]() { return rt_https_server_is_running(server) == 1; }, 1000)) {
        rt_https_server_stop(server);
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    test_result("HttpsServer reports running", rt_https_server_is_running(server) == 1);

    char url_buf[128];
    snprintf(url_buf, sizeof(url_buf), "https://127.0.0.1:%d/secure", port);

    void *req = rt_http_req_new(rt_const_cstr("GET"), rt_const_cstr(url_buf));
    rt_http_req_set_tls_verify(req, 0);
    rt_http_req_set_keep_alive(req, 1);
    rt_http_req_set_force_http1(req, 1); // HTTP/1.1-specific assertion below; don't negotiate h2.
    void *res = rt_http_req_send(req);
    test_result("HttpsServer request status is 200", rt_http_res_status(res) == 200);
    rt_string body = rt_http_res_body_str(res);
    test_result("HttpsServer request body matches", strcmp(rt_string_cstr(body), "secure-ok") == 0);
    rt_string_unref(body);
    rt_string conn = rt_http_res_header(res, rt_const_cstr("Connection"));
    test_result("HttpsServer high-level client sees keep-alive",
                strstr(rt_string_cstr(conn), "keep-alive") != nullptr);
    rt_string_unref(conn);

    rt_tls_config_t bad_sni_config;
    rt_tls_config_init(&bad_sni_config);
    bad_sni_config.hostname = "wrong.example.com";
    bad_sni_config.alpn_protocol = "http/1.1";
    bad_sni_config.verify_cert = 0;
    bad_sni_config.timeout_ms = 5000;
    rt_tls_session_t *bad_sni_tls = rt_tls_connect("127.0.0.1", (uint16_t)port, &bad_sni_config);
    const char *bad_sni_error = rt_tls_last_error();
    test_result("HttpsServer rejects mismatched SNI", bad_sni_tls == nullptr);
    test_result("HttpsServer reports the failed TLS handshake",
                bad_sni_error != nullptr && *bad_sni_error != '\0');
    if (bad_sni_tls)
        rt_tls_close(bad_sni_tls);

    rt_tls_session_t *tls = connect_local_tls_server_with_retries(port);
    if (!tls) {
        fprintf(stderr,
                "Raw TLS connect failed after bad SNI test: %s\n",
                rt_tls_last_error() ? rt_tls_last_error() : "(null)");
    }
    test_result("Raw TLS client connects to HttpsServer", tls != nullptr);
    if (!tls) {
        rt_https_server_stop(server);
        return;
    }

    const char *request1 = "GET /secure HTTP/1.1\r\n"
                           "Host: 127.0.0.1\r\n"
                           "Connection: keep-alive\r\n"
                           "\r\n";
    test_result("HttpsServer accepts first HTTPS request",
                tls_send_all(tls, request1, strlen(request1)));
    const std::string status1 = tls_recv_line(tls);
    test_result("HttpsServer first raw status is 200", status1 == "HTTP/1.1 200 OK");
    const auto headers1 = tls_read_http_headers(tls);
    const std::string connection1 = tls_find_http_header(headers1, "Connection");
    const std::string length1 = tls_find_http_header(headers1, "Content-Length");
    const std::string body1 = tls_recv_string(tls, (size_t)strtoull(length1.c_str(), nullptr, 10));
    test_result("HttpsServer first raw body matches", body1 == "secure-ok");

    const char *request2 = "GET /secure HTTP/1.1\r\n"
                           "Host: 127.0.0.1\r\n"
                           "Connection: close\r\n"
                           "\r\n";
    test_result("HttpsServer accepts second HTTPS request",
                tls_send_all(tls, request2, strlen(request2)));
    const std::string status2 = tls_recv_line(tls);
    test_result("HttpsServer second raw status is 200", status2 == "HTTP/1.1 200 OK");
    const auto headers2 = tls_read_http_headers(tls);
    const std::string connection2 = tls_find_http_header(headers2, "Connection");
    const std::string length2 = tls_find_http_header(headers2, "Content-Length");
    const std::string body2 = tls_recv_string(tls, (size_t)strtoull(length2.c_str(), nullptr, 10));
    test_result("HttpsServer second raw body matches", body2 == "secure-ok");
    test_result("HttpsServer keeps the first TLS response alive",
                connection1.find("keep-alive") != std::string::npos);
    test_result("HttpsServer closes the second TLS response",
                connection2.find("close") != std::string::npos);

    rt_tls_close(tls);
    rt_https_server_stop(server);
}

static void test_https_server_http2_roundtrip() {
    printf("\nTesting HttpsServer HTTP/2 ALPN round-trip:\n");

    const int port = get_bindable_local_port();
    if (port <= 0) {
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    temp_tls_files_t tls_files = create_temp_tls_files();
    if (!tls_files.valid) {
        printf("  SKIPPED: could not write temporary TLS fixture files\n");
        return;
    }

    void *server = rt_https_server_new(port,
                                       rt_const_cstr(tls_files.cert_path.c_str()),
                                       rt_const_cstr(tls_files.key_path.c_str()));
    rt_https_server_get(server, rt_const_cstr("/secure"), rt_const_cstr("secure"));
    rt_https_server_get(server, rt_const_cstr("/empty"), rt_const_cstr("empty"));
    rt_https_server_bind_handler(
        server, rt_const_cstr("secure"), (void *)&https_server_secure_handler);
    rt_https_server_bind_handler(
        server, rt_const_cstr("empty"), (void *)&https_server_no_content_handler);
    rt_https_server_start(server);
    if (!wait_for_condition([&]() { return rt_https_server_is_running(server) == 1; }, 1000)) {
        rt_https_server_stop(server);
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    rt_tls_session_t *tls = connect_local_tls_server_with_alpn(port, "h2,http/1.1");
    if (!tls) {
        fprintf(stderr,
                "HTTP/2 TLS connect failed: %s\n",
                rt_tls_last_error() ? rt_tls_last_error() : "(null)");
    }
    test_result("HTTP/2 TLS client connects to HttpsServer", tls != nullptr);
    if (!tls) {
        rt_https_server_stop(server);
        return;
    }

    test_result("HttpsServer negotiates h2 over ALPN",
                std::strcmp(rt_tls_get_negotiated_alpn(tls), "h2") == 0);

    rt_http2_io_t io{tls, tls_http2_read_cb, tls_http2_write_cb};
    rt_http2_conn_t *h2 = rt_http2_client_new(&io);
    test_result("HTTP/2 client transport allocates", h2 != nullptr);
    if (!h2) {
        rt_tls_close(tls);
        rt_https_server_stop(server);
        return;
    }

    rt_http2_response_t first{};
    rt_http2_response_t second{};
    bool ok =
        rt_http2_client_roundtrip(
            h2, "GET", "https", "127.0.0.1", "/secure", nullptr, nullptr, 0, 1024, &first) == 1 &&
        rt_http2_client_roundtrip(
            h2, "GET", "https", "127.0.0.1", "/empty", nullptr, nullptr, 0, 1024, &second) == 1;
    test_result("HTTP/2 client completes two requests on one TLS connection", ok);
    test_result("HTTP/2 first response status is 200", first.status == 200);
    test_result("HTTP/2 second response status is 204", second.status == 204);
    test_result("HTTP/2 first response body matches",
                first.body_len == 9 && std::memcmp(first.body, "secure-ok", 9) == 0);
    test_result("HTTP/2 status suppression removes the handler body", second.body_len == 0);
    const char *normalized_header = rt_http2_header_get(first.headers, "x-mixed-case");
    bool emitted_lowercase = false;
    bool emitted_uppercase = false;
    for (const rt_http2_header_t *header = first.headers; header; header = header->next) {
        emitted_lowercase = emitted_lowercase || std::strcmp(header->name, "x-mixed-case") == 0;
        emitted_uppercase = emitted_uppercase || std::strcmp(header->name, "X-Mixed-Case") == 0;
    }
    test_result("HTTP/2 response header names are normalized to lowercase",
                normalized_header && std::strcmp(normalized_header, "normalized") == 0 &&
                    emitted_lowercase && !emitted_uppercase);
    const char *empty_length = rt_http2_header_get(second.headers, "content-length");
    test_result("HTTP/2 suppressed body advertises an exact zero length",
                empty_length && std::strcmp(empty_length, "0") == 0);
    test_result("HTTP/2 second stream id advances", first.stream_id == 1 && second.stream_id == 3);

    rt_http2_response_free(&first);
    rt_http2_response_free(&second);
    rt_http2_conn_free(h2);
    rt_tls_close(tls);
    rt_https_server_stop(server);
}

static void test_https_server_rsa_roundtrip_with_verification() {
    printf("\nTesting HttpsServer RSA round-trip with native verification:\n");

    const int port = get_bindable_local_port();
    if (port <= 0) {
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    temp_tls_files_t tls_files = create_temp_tls_files_with_contents(
        LOCALHOST_RSA_LEAF_CERT_PEM, LOCALHOST_RSA_LEAF_KEY_PEM, LOCALHOST_RSA_ROOT_CERT_PEM);
    if (!tls_files.valid || tls_files.ca_path.empty()) {
        printf("  SKIPPED: could not write temporary RSA TLS fixture files\n");
        return;
    }

    void *server = rt_https_server_new(port,
                                       rt_const_cstr(tls_files.cert_path.c_str()),
                                       rt_const_cstr(tls_files.key_path.c_str()));
    rt_https_server_get(server, rt_const_cstr("/secure"), rt_const_cstr("secure"));
    rt_https_server_bind_handler(
        server, rt_const_cstr("secure"), (void *)&https_server_secure_handler);
    rt_https_server_start(server);
    if (!wait_for_condition([&]() { return rt_https_server_is_running(server) == 1; }, 1000)) {
        rt_https_server_stop(server);
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    test_result("HttpsServer RSA fixture reports running", rt_https_server_is_running(server) == 1);

    rt_tls_session_t *tls = connect_local_tls_server_verified(
        port, "127.0.0.1", "localhost", tls_files.ca_path.c_str());
    if (!tls) {
        fprintf(stderr,
                "RSA verified TLS connect failed: %s\n",
                rt_tls_last_error() ? rt_tls_last_error() : "(null)");
    }
    test_result("RSA HttpsServer verifies against the custom trust bundle", tls != nullptr);
    if (!tls) {
        rt_https_server_stop(server);
        return;
    }

    const char *request = "GET /secure HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "Connection: close\r\n"
                          "\r\n";
    test_result("RSA HttpsServer accepts the verified HTTPS request",
                tls_send_all(tls, request, strlen(request)));
    const std::string status = tls_recv_line(tls);
    test_result("RSA HttpsServer response status is 200", status == "HTTP/1.1 200 OK");
    const auto headers = tls_read_http_headers(tls);
    const std::string length = tls_find_http_header(headers, "Content-Length");
    const std::string body = tls_recv_string(tls, (size_t)strtoull(length.c_str(), nullptr, 10));
    test_result("RSA HttpsServer verified response body matches", body == "secure-ok");

    rt_tls_close(tls);
    rt_https_server_stop(server);
}

// ── Plain WsServer frame servicing (VDOC-148) ──────────────────────────────

/// @brief Send one small masked client frame with explicit FIN state.
/// @param tcp Connected runtime TCP handle after WebSocket upgrade.
/// @param fin True for the final fragment.
/// @param opcode RFC 6455 frame opcode.
/// @param payload Exact borrowed payload bytes.
/// @param len Payload length, limited to the control-frame-compatible short form.
/// @return True after the runtime accepted the complete frame bytes.
static bool tcp_send_ws_client_frame_ex(
    void *tcp, bool fin, uint8_t opcode, const uint8_t *payload, size_t len) {
    if (!tcp || len > 125)
        return false;
    uint8_t frame[2 + 4 + 125];
    frame[0] = (uint8_t)((fin ? 0x80 : 0x00) | opcode);
    frame[1] = (uint8_t)(0x80 | len);
    const uint8_t mask[4] = {0x11, 0x22, 0x33, 0x44};
    memcpy(frame + 2, mask, 4);
    for (size_t i = 0; i < len; i++)
        frame[6 + i] = (uint8_t)(payload[i] ^ mask[i % 4]);
    rt_tcp_send_all_raw(tcp, frame, (int64_t)(6 + len));
    return true;
}

/// @brief Send one final masked client frame over a runtime TCP handle.
static bool tcp_send_ws_client_frame(void *tcp,
                                     uint8_t opcode,
                                     const uint8_t *payload,
                                     size_t len) {
    return tcp_send_ws_client_frame_ex(tcp, true, opcode, payload, len);
}

/// @brief Close and release one caller-owned runtime TCP test handle.
static void release_runtime_tcp(void *tcp) {
    if (!tcp)
        return;
    rt_tcp_close(tcp);
    if (rt_obj_release_check0(tcp))
        rt_obj_free(tcp);
}

/// @brief Receive one (small, unmasked) server frame from a raw TCP handle.
static bool tcp_recv_ws_frame(void *tcp, uint8_t *opcode, std::vector<uint8_t> *payload) {
    void *hdr = rt_tcp_recv_exact(tcp, 2);
    if (!hdr || rt_bytes_len(hdr) != 2)
        return false;
    *opcode = (uint8_t)(rt_bytes_get(hdr, 0) & 0x0F);
    size_t len = (size_t)(rt_bytes_get(hdr, 1) & 0x7F);
    if (rt_obj_release_check0(hdr))
        rt_obj_free(hdr);
    payload->clear();
    if (len > 0) {
        void *data = rt_tcp_recv_exact(tcp, (int64_t)len);
        if (!data || (size_t)rt_bytes_len(data) != len)
            return false;
        for (size_t i = 0; i < len; i++)
            payload->push_back((uint8_t)rt_bytes_get(data, i));
        if (rt_obj_release_check0(data))
            rt_obj_free(data);
    }
    return true;
}

/// @brief Connect and complete a canonical plain WebSocket upgrade.
/// @param port Loopback WsServer port.
/// @return Caller-owned upgraded runtime TCP handle, or NULL on failure.
static void *connect_plain_ws_client(int port) {
    void *tcp = rt_tcp_connect(rt_const_cstr("127.0.0.1"), port);
    if (!tcp)
        return nullptr;
    char request[512];
    snprintf(request,
             sizeof(request),
             "GET /chat HTTP/1.1\r\n"
             "Host: 127.0.0.1:%d\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
             "Sec-WebSocket-Version: 13\r\n"
             "\r\n",
             port);
    rt_tcp_send_all_raw(tcp, request, (int64_t)strlen(request));
    rt_string status = rt_tcp_recv_line(tcp);
    bool upgraded =
        status && strcmp(rt_string_cstr(status), "HTTP/1.1 101 Switching Protocols") == 0;
    rt_string_unref(status);
    if (!upgraded) {
        release_runtime_tcp(tcp);
        return nullptr;
    }
    for (;;) {
        rt_string line = rt_tcp_recv_line(tcp);
        bool done = line && rt_str_len(line) == 0;
        rt_string_unref(line);
        if (done)
            break;
    }
    return tcp;
}

/// @brief Submit a malformed upgrade and observe connection close/timeout as rejection.
/// @details The helper installs the test binary's expected-trap recovery only
///          around the one-byte receive. A mistaken 101 response produces a
///          byte and therefore returns false rather than being misclassified.
/// @param port Loopback WsServer port.
/// @param request Exact malformed HTTP bytes.
/// @return True when no response byte is delivered before close/timeout.
static bool plain_ws_handshake_is_rejected(int port, const std::string &request) {
    void *tcp = rt_tcp_connect(rt_const_cstr("127.0.0.1"), port);
    if (!tcp)
        return false;
    rt_tcp_set_recv_timeout(tcp, 1500);
    rt_tcp_send_all_raw(tcp, request.data(), (int64_t)request.size());

    bool rejected = false;
    g_trap_expected = true;
    g_last_trap = nullptr;
    if (setjmp(g_trap_jmp) == 0) {
        void *byte = rt_tcp_recv_exact(tcp, 1);
        if (byte && rt_obj_release_check0(byte))
            rt_obj_free(byte);
    } else {
        rejected = true;
    }
    g_trap_expected = false;
    release_runtime_tcp(tcp);
    return rejected;
}

/// @brief The plain WsServer must service inbound frames exactly like the WSS
///        server: answer PING, echo CLOSE, and clear the client count.
static void test_ws_server_services_client_frames() {
    printf("\nTesting plain WsServer frame servicing:\n");

    const int port = get_bindable_local_port();
    if (port <= 0) {
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    void *server = rt_ws_server_new(port);
    rt_ws_server_start(server);
    if (!wait_for_condition([&]() { return rt_ws_server_is_running(server) == 1; }, 1000)) {
        rt_ws_server_stop(server);
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    void *tcp = rt_tcp_connect(rt_const_cstr("127.0.0.1"), port);
    test_result("Raw TCP client connects to WsServer", tcp != nullptr);

    const char *ws_key = "dGhlIHNhbXBsZSBub25jZQ==";
    char request[512];
    snprintf(request,
             sizeof(request),
             "GET /chat HTTP/1.1\r\n"
             "Host: 127.0.0.1:%d\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Key: %s\r\n"
             "Sec-WebSocket-Version: 13\r\n"
             "\r\n",
             port,
             ws_key);
    rt_tcp_send_all_raw(tcp, request, (int64_t)strlen(request));

    rt_string status = rt_tcp_recv_line(tcp);
    test_result("WsServer returns 101 Switching Protocols",
                strcmp(rt_string_cstr(status), "HTTP/1.1 101 Switching Protocols") == 0);
    rt_string_unref(status);
    // Drain the remaining response headers.
    for (;;) {
        rt_string line = rt_tcp_recv_line(tcp);
        bool done = rt_string_cstr(line)[0] == '\0';
        rt_string_unref(line);
        if (done)
            break;
    }

    test_result("WsServer tracks the connected client",
                wait_for_condition([&]() { return rt_ws_server_client_count(server) == 1; }, 2000));

    // The differentiator: the background server must answer PING with PONG.
    const uint8_t ping_payload[2] = {'h', 'i'};
    test_result("Client PING sent", tcp_send_ws_client_frame(tcp, 0x09, ping_payload, 2));
    uint8_t opcode = 0;
    std::vector<uint8_t> payload;
    test_result("WsServer answers PING", tcp_recv_ws_frame(tcp, &opcode, &payload));
    test_result("WsServer PONG opcode", opcode == 0x0A);
    test_result("WsServer PONG echoes the payload",
                payload.size() == 2 && payload[0] == 'h' && payload[1] == 'i');

    // Broadcasts still reach the serviced client.
    rt_ws_server_broadcast(server, rt_const_cstr("news"));
    payload.clear();
    test_result("WsServer broadcast text frame received",
                tcp_recv_ws_frame(tcp, &opcode, &payload));
    test_result("WsServer broadcast opcode", opcode == 0x01);
    test_result("WsServer broadcast payload matches",
                std::string(payload.begin(), payload.end()) == "news");

    // Text broadcasts carry the full runtime byte length: embedded NUL is
    // data, not a terminator (VDOC-161).
    rt_string nul_msg = rt_string_from_bytes("a\0b", 3);
    rt_ws_server_broadcast(server, nul_msg);
    rt_string_unref(nul_msg);
    payload.clear();
    test_result("NUL-bearing broadcast received", tcp_recv_ws_frame(tcp, &opcode, &payload));
    test_result("NUL-bearing broadcast keeps its full byte length",
                payload.size() == 3 && payload[0] == 'a' && payload[1] == '\0' &&
                    payload[2] == 'b');

    // Orderly close: server echoes the close frame and drops the client slot.
    const uint8_t close_payload[2] = {0x03, 0xE8};
    test_result("Client CLOSE sent", tcp_send_ws_client_frame(tcp, 0x08, close_payload, 2));
    payload.clear();
    test_result("WsServer echoes CLOSE", tcp_recv_ws_frame(tcp, &opcode, &payload));
    test_result("WsServer CLOSE opcode", opcode == 0x08);
    test_result("WsServer clears the client count after close",
                wait_for_condition([&]() { return rt_ws_server_client_count(server) == 0; }, 2000));

    rt_tcp_close(tcp);
    if (rt_obj_release_check0(tcp))
        rt_obj_free(tcp);
    rt_ws_server_stop(server);
}

/// @brief Port 0 requests an ephemeral port; `Port` reports the bound value
///        after Start (VDOC-150), matching TcpServer/HttpServer behavior.
static void test_ws_server_ephemeral_port() {
    printf("\nTesting WsServer ephemeral port zero:\n");

    void *server = rt_ws_server_new(0);
    test_result("WsServer.New(0) is accepted", server != nullptr);
    rt_ws_server_start(server);
    if (!wait_for_condition([&]() { return rt_ws_server_is_running(server) == 1; }, 1000)) {
        rt_ws_server_stop(server);
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }
    int64_t bound = rt_ws_server_port(server);
    test_result("Port reports the OS-assigned value after Start", bound >= 1 && bound <= 65535);

    // The reported port is genuinely reachable.
    void *tcp = rt_tcp_connect(rt_const_cstr("127.0.0.1"), bound);
    test_result("Reported ephemeral port accepts a connection", tcp != nullptr);
    rt_tcp_close(tcp);
    if (rt_obj_release_check0(tcp))
        rt_obj_free(tcp);
    rt_ws_server_stop(server);
}

/// @brief Handshake parsing is bounded (VDOC-149): a peer streaming endless
///        header lines is rejected instead of fed to an unbounded parser.
static void test_ws_server_bounded_handshake() {
    printf("\nTesting WsServer bounded handshake parsing:\n");

    const int port = get_bindable_local_port();
    if (port <= 0) {
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    void *server = rt_ws_server_new(port);
    rt_ws_server_start(server);
    if (!wait_for_condition([&]() { return rt_ws_server_is_running(server) == 1; }, 1000)) {
        rt_ws_server_stop(server);
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    void *tcp = rt_tcp_connect(rt_const_cstr("127.0.0.1"), port);
    test_result("Raw TCP client connects", tcp != nullptr);

    char request[256];
    snprintf(request, sizeof(request), "GET /chat HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n", port);
    std::string flooded_request(request);
    for (int i = 0; i < 150; i++) {
        char junk[64];
        int n = snprintf(junk, sizeof(junk), "X-Filler-%d: value\r\n", i);
        flooded_request.append(junk, (size_t)n);
    }
    // Submit the flood in one write. The server is expected to close as soon
    // as it reaches its header limit; issuing one write per line races that
    // correct close and can turn the later client writes into a send trap.
    rt_tcp_send_all_raw(tcp, flooded_request.data(), (int64_t)flooded_request.size());

    // The flooded connection must never become a registered client, and the
    // server must stay healthy enough to upgrade a well-formed client next.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    test_result("Header flood never registers a client", rt_ws_server_client_count(server) == 0);
    rt_tcp_close(tcp);
    if (rt_obj_release_check0(tcp))
        rt_obj_free(tcp);

    void *good = rt_tcp_connect(rt_const_cstr("127.0.0.1"), port);
    const char *ws_key = "dGhlIHNhbXBsZSBub25jZQ==";
    char upgrade[512];
    snprintf(upgrade,
             sizeof(upgrade),
             "GET /chat HTTP/1.1\r\n"
             "Host: 127.0.0.1:%d\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Key: %s\r\n"
             "Sec-WebSocket-Version: 13\r\n"
             "\r\n",
             port,
             ws_key);
    rt_tcp_send_all_raw(good, upgrade, (int64_t)strlen(upgrade));
    rt_string status = rt_tcp_recv_line(good);
    test_result("Server still upgrades a well-formed client after the flood",
                strcmp(rt_string_cstr(status), "HTTP/1.1 101 Switching Protocols") == 0);
    rt_string_unref(status);

    rt_tcp_close(good);
    if (rt_obj_release_check0(good))
        rt_obj_free(good);
    rt_ws_server_stop(server);
}

/// @brief Exercise strict, byte-bounded plain WebSocket upgrade parsing.
/// @details Duplicate security singletons, obsolete folding, non-canonical
///          nonce encoding, request bodies, and bare-LF line endings must all
///          close without producing a 101 response. A canonical request after
///          the rejection sequence proves the listener remains usable.
static void test_ws_server_rejects_ambiguous_handshakes() {
    printf("\nTesting strict WsServer handshake parsing:\n");
    const int port = get_bindable_local_port();
    if (port <= 0) {
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    void *server = rt_ws_server_new(port);
    rt_ws_server_start(server);
    if (!wait_for_condition([&]() { return rt_ws_server_is_running(server) == 1; }, 1000)) {
        rt_ws_server_stop(server);
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    const std::string host = "Host: 127.0.0.1:" + std::to_string(port) + "\r\n";
    const std::string prefix =
        "GET /chat HTTP/1.1\r\n" + host + "Upgrade: websocket\r\nConnection: Upgrade\r\n";
    const std::string suffix = "Sec-WebSocket-Version: 13\r\n\r\n";
    test_result("Duplicate WebSocket key is rejected",
                plain_ws_handshake_is_rejected(
                    port,
                    prefix + "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n" +
                        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n" + suffix));
    test_result(
        "Obsolete folded header is rejected",
        plain_ws_handshake_is_rejected(port,
                                       prefix + "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n" +
                                           "\tX-Folded: value\r\n" + suffix));
    test_result("Non-canonical 16-byte nonce encoding is rejected",
                plain_ws_handshake_is_rejected(
                    port, prefix + "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZR==\r\n" + suffix));
    test_result(
        "Body-bearing upgrade is rejected",
        plain_ws_handshake_is_rejected(port,
                                       prefix + "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n" +
                                           "Content-Length: 1\r\n" + suffix + "x"));

    std::string bare_lf = "GET /chat HTTP/1.1\nHost: 127.0.0.1:" + std::to_string(port) +
                          "\nUpgrade: websocket\nConnection: Upgrade\n"
                          "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\n"
                          "Sec-WebSocket-Version: 13\n\n";
    test_result("Bare-LF upgrade is rejected", plain_ws_handshake_is_rejected(port, bare_lf));

    void *good = connect_plain_ws_client(port);
    test_result("Canonical upgrade still succeeds after strict rejections", good != nullptr);
    release_runtime_tcp(good);
    rt_ws_server_stop(server);
    if (rt_obj_release_check0(server))
        rt_obj_free(server);
}

/// @brief Validate strict frame handling and fragmented UTF-8 state.
/// @details A multibyte scalar split across frames remains valid, while an
///          invalid continuation closes with 1007. Forbidden 64-bit lengths,
///          reserved close codes, and unmasked client frames close with 1002.
static void test_ws_server_strict_frames_and_fragmentation() {
    printf("\nTesting strict WsServer framing and fragmented UTF-8:\n");
    const int port = get_bindable_local_port();
    if (port <= 0) {
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }
    void *server = rt_ws_server_new(port);
    rt_ws_server_start(server);
    if (!wait_for_condition([&]() { return rt_ws_server_is_running(server) == 1; }, 1000)) {
        rt_ws_server_stop(server);
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    uint8_t opcode = 0;
    std::vector<uint8_t> payload;
    void *tcp = connect_plain_ws_client(port);
    test_result("Frame-test client upgrades", tcp != nullptr);
    const uint8_t euro_head[] = {0xE2};
    const uint8_t euro_tail[] = {0x82, 0xAC};
    test_result("First UTF-8 fragment is sent",
                tcp_send_ws_client_frame_ex(tcp, false, 0x01, euro_head, sizeof(euro_head)));
    test_result("Final UTF-8 continuation is sent",
                tcp_send_ws_client_frame_ex(tcp, true, 0x00, euro_tail, sizeof(euro_tail)));
    const uint8_t ping[] = {'o', 'k'};
    tcp_send_ws_client_frame(tcp, 0x09, ping, sizeof(ping));
    test_result("Split UTF-8 scalar keeps connection alive",
                tcp_recv_ws_frame(tcp, &opcode, &payload) && opcode == 0x0A &&
                    payload == std::vector<uint8_t>({'o', 'k'}));

    const uint8_t invalid_tail[] = {'A'};
    tcp_send_ws_client_frame_ex(tcp, false, 0x01, euro_head, sizeof(euro_head));
    tcp_send_ws_client_frame_ex(tcp, true, 0x00, invalid_tail, sizeof(invalid_tail));
    test_result("Invalid fragmented UTF-8 closes with 1007",
                tcp_recv_ws_frame(tcp, &opcode, &payload) && opcode == 0x08 &&
                    payload.size() == 2 && payload[0] == 0x03 && payload[1] == 0xEF);
    release_runtime_tcp(tcp);

    tcp = connect_plain_ws_client(port);
    const uint8_t forbidden_length[] = {0x82, 0xFF, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    rt_tcp_send_all_raw(tcp, forbidden_length, sizeof(forbidden_length));
    test_result("High-bit 64-bit frame length closes with 1002",
                tcp_recv_ws_frame(tcp, &opcode, &payload) && opcode == 0x08 &&
                    payload.size() == 2 && payload[0] == 0x03 && payload[1] == 0xEA);
    release_runtime_tcp(tcp);

    tcp = connect_plain_ws_client(port);
    const uint8_t forbidden_close[] = {0x03, 0xED}; // 1005 is wire-forbidden.
    tcp_send_ws_client_frame(tcp, 0x08, forbidden_close, sizeof(forbidden_close));
    test_result("Forbidden close status closes with 1002",
                tcp_recv_ws_frame(tcp, &opcode, &payload) && opcode == 0x08 &&
                    payload.size() == 2 && payload[0] == 0x03 && payload[1] == 0xEA);
    release_runtime_tcp(tcp);

    tcp = connect_plain_ws_client(port);
    const uint8_t unmasked_ping[] = {0x89, 0x00};
    rt_tcp_send_all_raw(tcp, unmasked_ping, sizeof(unmasked_ping));
    test_result("Unmasked client frame closes with 1002",
                tcp_recv_ws_frame(tcp, &opcode, &payload) && opcode == 0x08 &&
                    payload.size() == 2 && payload[0] == 0x03 && payload[1] == 0xEA);
    release_runtime_tcp(tcp);

    rt_ws_server_stop(server);
    if (rt_obj_release_check0(server))
        rt_obj_free(server);
}

/// @brief Prove Stop interrupts queued and handshaking WS/WSS sockets promptly.
/// @details The TLS case connects raw TCP but sends no ClientHello, exercising
///          the detached-descriptor handoff window rather than an active TLS
///          session. Both stops must join their pools well below handshake timeout.
static void test_websocket_server_stop_interrupts_pending_handshakes() {
    printf("\nTesting WebSocket server pending-handshake shutdown:\n");
    const int plain_port = get_bindable_local_port();
    if (plain_port <= 0) {
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }
    void *plain = rt_ws_server_new(plain_port);
    rt_ws_server_start(plain);
    void *plain_tcp = rt_tcp_connect(rt_const_cstr("127.0.0.1"), plain_port);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto started = std::chrono::steady_clock::now();
    rt_ws_server_stop(plain);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    test_result("WsServer Stop interrupts an incomplete HTTP upgrade", elapsed.count() < 2000);
    release_runtime_tcp(plain_tcp);
    if (rt_obj_release_check0(plain))
        rt_obj_free(plain);

    const int secure_port = get_bindable_local_port();
    temp_tls_files_t tls_files = create_temp_tls_files();
    if (secure_port <= 0 || !tls_files.valid) {
        printf("  SKIPPED: WSS fixture unavailable\n");
        return;
    }
    void *secure = rt_wss_server_new(secure_port,
                                     rt_const_cstr(tls_files.cert_path.c_str()),
                                     rt_const_cstr(tls_files.key_path.c_str()));
    test_result("WssServer uses its stable managed class id",
                secure && rt_obj_class_id(secure) == RT_WSS_SERVER_CLASS_ID);
    rt_wss_server_start(secure);
    void *raw_tls = rt_tcp_connect(rt_const_cstr("127.0.0.1"), secure_port);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    started = std::chrono::steady_clock::now();
    rt_wss_server_stop(secure);
    elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    test_result("WssServer Stop interrupts an incomplete TLS handshake", elapsed.count() < 2000);
    release_runtime_tcp(raw_tls);
    if (rt_obj_release_check0(secure))
        rt_obj_free(secure);
}

static void test_wss_server_broadcast() {
    printf("\nTesting WssServer TLS upgrade and broadcast:\n");

    const int port = get_bindable_local_port();
    if (port <= 0) {
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    temp_tls_files_t tls_files = create_temp_tls_files();
    if (!tls_files.valid) {
        printf("  SKIPPED: could not write temporary TLS fixture files\n");
        return;
    }

    void *server = rt_wss_server_new(port,
                                     rt_const_cstr(tls_files.cert_path.c_str()),
                                     rt_const_cstr(tls_files.key_path.c_str()));
    rt_wss_server_start(server);
    if (!wait_for_condition([&]() { return rt_wss_server_is_running(server) == 1; }, 1000)) {
        rt_wss_server_stop(server);
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    test_result("WssServer reports running", rt_wss_server_is_running(server) == 1);

    rt_tls_session_t *tls = connect_local_tls_server(port);
    if (!tls) {
        fprintf(stderr,
                "Raw TLS connect to WssServer failed: %s\n",
                rt_tls_last_error() ? rt_tls_last_error() : "(null)");
    }
    test_result("Raw TLS client connects to WssServer", tls != nullptr);
    if (!tls) {
        rt_wss_server_stop(server);
        return;
    }

    const char *ws_key = "dGhlIHNhbXBsZSBub25jZQ==";
    char request[512];
    snprintf(request,
             sizeof(request),
             "GET /chat HTTP/1.1\r\n"
             "Host: 127.0.0.1:%d\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Key: %s\r\n"
             "Sec-WebSocket-Version: 13\r\n"
             "\r\n",
             port,
             ws_key);
    test_result("WssServer accepts TLS WebSocket upgrade request",
                tls_send_all(tls, request, strlen(request)));
    const std::string status = tls_recv_line(tls);
    test_result("WssServer returns 101 Switching Protocols",
                status == "HTTP/1.1 101 Switching Protocols");
    const auto headers = tls_read_http_headers(tls);
    const std::string accept = tls_find_http_header(headers, "Sec-WebSocket-Accept");
    char *expected_accept = rt_ws_compute_accept_key(ws_key);
    test_result("WssServer returns correct Sec-WebSocket-Accept",
                expected_accept != nullptr && accept == expected_accept);
    free(expected_accept);

    test_result(
        "WssServer tracks the connected client",
        wait_for_condition([&]() { return rt_wss_server_client_count(server) == 1; }, 1000));

    rt_wss_server_broadcast(server, rt_const_cstr("broadcast"));
    uint8_t opcode = 0;
    std::vector<uint8_t> payload;
    test_result("WssServer sends a text frame", tls_recv_ws_frame(tls, &opcode, &payload));
    test_result("WssServer text frame opcode is correct", opcode == 0x01);
    test_result("WssServer text frame payload matches",
                std::string(reinterpret_cast<const char *>(payload.data()), payload.size()) ==
                    "broadcast");

    void *binary = rt_bytes_from_str(rt_const_cstr("bin"));
    rt_wss_server_broadcast_bytes(server, binary);
    payload.clear();
    test_result("WssServer sends a binary frame", tls_recv_ws_frame(tls, &opcode, &payload));
    test_result("WssServer binary frame opcode is correct", opcode == 0x02);
    test_result("WssServer binary frame payload matches",
                std::string(reinterpret_cast<const char *>(payload.data()), payload.size()) ==
                    "bin");

    test_result("WssServer accepts a client close frame", tls_send_ws_client_close(tls));
    payload.clear();
    test_result("WssServer replies with a close frame", tls_recv_ws_frame(tls, &opcode, &payload));
    test_result("WssServer close frame opcode is correct", opcode == 0x08);

    rt_tls_close(tls);
    test_result(
        "WssServer removes the closed client",
        wait_for_condition([&]() { return rt_wss_server_client_count(server) == 0; }, 1000));
    rt_wss_server_stop(server);
}

static void test_wss_server_rejects_invalid_sec_websocket_key() {
    printf("\nTesting WssServer rejects malformed handshake keys:\n");

    const int port = get_bindable_local_port();
    if (port <= 0) {
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    temp_tls_files_t tls_files = create_temp_tls_files();
    if (!tls_files.valid) {
        printf("  SKIPPED: could not write temporary TLS fixture files\n");
        return;
    }

    void *server = rt_wss_server_new(port,
                                     rt_const_cstr(tls_files.cert_path.c_str()),
                                     rt_const_cstr(tls_files.key_path.c_str()));
    rt_wss_server_start(server);
    if (!wait_for_condition([&]() { return rt_wss_server_is_running(server) == 1; }, 1000)) {
        rt_wss_server_stop(server);
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    rt_tls_session_t *tls = connect_local_tls_server(port);
    test_result("Raw TLS client connects to WssServer", tls != nullptr);
    if (!tls) {
        rt_wss_server_stop(server);
        return;
    }

    char request[512];
    snprintf(request,
             sizeof(request),
             "GET /chat HTTP/1.1\r\n"
             "Host: 127.0.0.1:%d\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Key: bad\r\n"
             "Sec-WebSocket-Version: 13\r\n"
             "\r\n",
             port);
    test_result("Malformed WSS handshake request is sent",
                tls_send_all(tls, request, strlen(request)));

    const std::string status = tls_recv_line(tls);
    test_result("WssServer does not upgrade malformed handshake", status.empty());
    test_result("WssServer keeps zero connected clients",
                wait_for_condition([&]() { return rt_wss_server_client_count(server) == 0; }, 250));

    rt_tls_close(tls);
    rt_wss_server_stop(server);
}

static void test_wss_server_rejects_invalid_host_header() {
    printf("\nTesting WssServer rejects malformed Host headers:\n");

    const int port = get_bindable_local_port();
    if (port <= 0) {
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    temp_tls_files_t tls_files = create_temp_tls_files();
    if (!tls_files.valid) {
        printf("  SKIPPED: could not write temporary TLS fixture files\n");
        return;
    }

    void *server = rt_wss_server_new(port,
                                     rt_const_cstr(tls_files.cert_path.c_str()),
                                     rt_const_cstr(tls_files.key_path.c_str()));
    rt_wss_server_start(server);
    if (!wait_for_condition([&]() { return rt_wss_server_is_running(server) == 1; }, 1000)) {
        rt_wss_server_stop(server);
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    rt_tls_session_t *tls = connect_local_tls_server(port);
    test_result("Raw TLS client connects to WssServer", tls != nullptr);
    if (!tls) {
        rt_wss_server_stop(server);
        return;
    }

    const char *request = "GET /chat HTTP/1.1\r\n"
                          "Host: ::1\r\n"
                          "Upgrade: websocket\r\n"
                          "Connection: Upgrade\r\n"
                          "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                          "Sec-WebSocket-Version: 13\r\n"
                          "\r\n";
    test_result("Malformed Host handshake request is sent",
                tls_send_all(tls, request, strlen(request)));

    const std::string status = tls_recv_line(tls);
    test_result("WssServer does not upgrade malformed Host handshake", status.empty());
    test_result("WssServer keeps zero connected clients",
                wait_for_condition([&]() { return rt_wss_server_client_count(server) == 0; }, 250));

    rt_tls_close(tls);
    rt_wss_server_stop(server);
}

static void test_wss_server_subprotocol_negotiation() {
    printf("\nTesting WssServer subprotocol negotiation:\n");

    const int port = get_bindable_local_port();
    if (port <= 0) {
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    temp_tls_files_t tls_files = create_temp_tls_files();
    if (!tls_files.valid) {
        printf("  SKIPPED: could not write temporary TLS fixture files\n");
        return;
    }

    void *server = rt_wss_server_new(port,
                                     rt_const_cstr(tls_files.cert_path.c_str()),
                                     rt_const_cstr(tls_files.key_path.c_str()));
    rt_wss_server_set_subprotocol(server, rt_const_cstr("chat.v1"));
    rt_wss_server_start(server);
    if (!wait_for_condition([&]() { return rt_wss_server_is_running(server) == 1; }, 1000)) {
        rt_wss_server_stop(server);
        printf("  SKIPPED: local bind unavailable in this environment\n");
        return;
    }

    rt_tls_session_t *tls = connect_local_tls_server(port);
    test_result("Raw TLS client connects to WssServer", tls != nullptr);
    if (!tls) {
        rt_wss_server_stop(server);
        return;
    }

    char request[768];
    snprintf(request,
             sizeof(request),
             "GET /chat HTTP/1.1\r\n"
             "Host: 127.0.0.1:%d\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
             "Sec-WebSocket-Version: 13\r\n"
             "Sec-WebSocket-Protocol: other, chat.v1\r\n"
             "\r\n",
             port);
    test_result("Matching subprotocol handshake request is sent",
                tls_send_all(tls, request, strlen(request)));
    const std::string status = tls_recv_line(tls);
    test_result("WssServer upgrades matching subprotocol handshake",
                status == "HTTP/1.1 101 Switching Protocols");
    const auto headers = tls_read_http_headers(tls);
    test_result("WssServer returns negotiated subprotocol",
                tls_find_http_header(headers, "Sec-WebSocket-Protocol") == "chat.v1");
    rt_tls_close(tls);
    test_result(
        "WssServer removes the negotiated client",
        wait_for_condition([&]() { return rt_wss_server_client_count(server) == 0; }, 1000));

    tls = connect_local_tls_server(port);
    test_result("Second raw TLS client connects to WssServer", tls != nullptr);
    if (!tls) {
        rt_wss_server_stop(server);
        return;
    }

    snprintf(request,
             sizeof(request),
             "GET /chat HTTP/1.1\r\n"
             "Host: 127.0.0.1:%d\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
             "Sec-WebSocket-Version: 13\r\n"
             "Sec-WebSocket-Protocol: other\r\n"
             "\r\n",
             port);
    test_result("Mismatched subprotocol handshake request is sent",
                tls_send_all(tls, request, strlen(request)));
    test_result("WssServer rejects missing required subprotocol", tls_recv_line(tls).empty());
    test_result("WssServer keeps zero connected clients after rejection",
                wait_for_condition([&]() { return rt_wss_server_client_count(server) == 0; }, 250));

    rt_tls_close(tls);
    rt_wss_server_stop(server);
}

int main() {
    test_sse_plain_event();
    test_sse_timed_recv_is_lossless();
    test_sse_validation_and_dispatch_state();
    test_sse_strict_framing_and_event_lines();
    test_sse_close_and_receive_serialization();
    test_wss_server_credential_validation();
    test_sse_chunked_event();
    test_sse_resume_after_disconnect();
    test_sse_redirect_to_stream();
    test_http_client_cookie_scope();
    test_http_client_manual_cookie_is_host_only();
    test_http_client_cookie_jar_rules();
    test_http_client_cross_origin_redirect_strips_sensitive_headers();
    test_http_client_consumes_informational_responses();
    test_http_client_keepalive_reuse();
    test_standalone_httpreq_keepalive_reuse();
    test_header_case_insensitive_configuration();
    test_router_concurrent_add_and_match();
    test_http_server_keepalive_response();
    test_http_server_pipelined_keepalive_requests();
    test_https_server_roundtrip();
    test_https_server_http2_roundtrip();
    test_https_server_rsa_roundtrip_with_verification();
    test_ws_server_services_client_frames();
    test_ws_server_bounded_handshake();
    test_ws_server_rejects_ambiguous_handshakes();
    test_ws_server_strict_frames_and_fragmentation();
    test_websocket_server_stop_interrupts_pending_handshakes();
    test_ws_server_ephemeral_port();
    test_wss_server_broadcast();
    test_wss_server_rejects_invalid_sec_websocket_key();
    test_wss_server_rejects_invalid_host_header();
    test_wss_server_subprotocol_negotiation();
    test_smtp_validation_and_result_model();
    test_smtp_strict_reply_framing();
    test_smtp_plain_helo_fallback();
    test_smtp_starttls_handshake_failure_is_clean();
    test_smtp_plain_send_sanitizes_and_dot_stuffs();
    test_smtp_close_interrupts_and_reconnects();
    test_smtp_close_interrupts_starttls_handshake();
    test_smtp_concurrent_sends_serialize();
    test_smtp_requires_starttls_capability();
    test_smtp_accepts_forwarding_recipient_codes();

    printf("\nAll high-level network tests passed.\n");
    return 0;
}
