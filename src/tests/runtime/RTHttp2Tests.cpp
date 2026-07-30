//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTHttp2Tests.cpp
// Purpose: In-memory HTTP/2 transport coverage for the internal runtime.
// Key invariants:
//   - Callback byte counts never exceed the requested transport span.
//   - Frame, HPACK, flow-control, and stream validation reject malformed input.
//   - One connection can complete sequential request/response streams.
// Ownership/Lifetime:
//   - Each test frees connection, request, response, and header-list ownership.
//   - In-memory pipe buffers are owned by their enclosing test scope.
// Links: src/runtime/network/rt_http2.c,
//        src/runtime/network/rt_http2.h
//
//===----------------------------------------------------------------------===//

#include "rt_http2.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static void test_result(const char *name, bool passed) {
    printf("  %s: %s\n", name, passed ? "PASS" : "FAIL");
    assert(passed);
}

struct pipe_buffer_t {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<uint8_t> bytes;
    bool closed = false;
};

struct pipe_endpoint_t {
    pipe_buffer_t *incoming = nullptr;
    pipe_buffer_t *outgoing = nullptr;
};

static long pipe_read_cb(void *ctx, uint8_t *buf, size_t len) {
    auto *ep = static_cast<pipe_endpoint_t *>(ctx);
    std::unique_lock<std::mutex> lock(ep->incoming->mutex);
    ep->incoming->cv.wait(lock,
                          [&]() { return ep->incoming->closed || !ep->incoming->bytes.empty(); });
    if (ep->incoming->bytes.empty())
        return 0;
    const size_t n = std::min(len, ep->incoming->bytes.size());
    std::memcpy(buf, ep->incoming->bytes.data(), n);
    ep->incoming->bytes.erase(ep->incoming->bytes.begin(), ep->incoming->bytes.begin() + (long)n);
    return (long)n;
}

static int pipe_write_cb(void *ctx, const uint8_t *buf, size_t len) {
    auto *ep = static_cast<pipe_endpoint_t *>(ctx);
    {
        std::lock_guard<std::mutex> lock(ep->outgoing->mutex);
        ep->outgoing->bytes.insert(ep->outgoing->bytes.end(), buf, buf + len);
    }
    ep->outgoing->cv.notify_all();
    return 1;
}

/// @brief Deliberately violate the read callback's maximum-count contract.
/// @param ctx Unused callback context.
/// @param buf Unused destination.
/// @param len Requested maximum byte count.
/// @return One byte more than requested.
static long oversized_read_cb(void *ctx, uint8_t *buf, size_t len) {
    (void)ctx;
    (void)buf;
    return static_cast<long>(len + 1u);
}

/// @brief Claim success without writing bytes to exercise defensive zeroing.
/// @param ctx Unused callback context.
/// @param buf Intentionally untouched destination.
/// @param len Requested byte count.
/// @return The requested count without initializing @p buf.
static long unwritten_read_cb(void *ctx, uint8_t *buf, size_t len) {
    (void)ctx;
    (void)buf;
    return static_cast<long>(len);
}

/// @brief Accept a complete write without storing it.
/// @return Nonzero, as required by the HTTP/2 write callback contract.
static int sink_write_cb(void *, const uint8_t *, size_t) {
    return 1;
}

/// @brief Validate constructor callbacks and defensive read-count handling.
static void test_http2_callback_contract() {
    printf("\nTesting HTTP/2 callback contract:\n");
    rt_http2_io_t missing_read{nullptr, nullptr, sink_write_cb};
    rt_http2_io_t missing_write{nullptr, oversized_read_cb, nullptr};
    test_result("HTTP/2 rejects a missing read callback",
                rt_http2_server_new(&missing_read) == nullptr);
    test_result("HTTP/2 rejects a missing write callback",
                rt_http2_server_new(&missing_write) == nullptr);

    rt_http2_request_t request{};
    rt_http2_io_t oversized_io{nullptr, oversized_read_cb, sink_write_cb};
    rt_http2_conn_t *oversized = rt_http2_server_new(&oversized_io);
    bool oversized_rejected =
        oversized && rt_http2_server_receive_request(oversized, 1024u, &request) == 0;
    test_result("HTTP/2 rejects callback counts above the requested span", oversized_rejected);
    rt_http2_request_free(&request);
    rt_http2_conn_free(oversized);

    rt_http2_io_t unwritten_io{nullptr, unwritten_read_cb, sink_write_cb};
    rt_http2_conn_t *unwritten = rt_http2_server_new(&unwritten_io);
    bool unwritten_rejected =
        unwritten && rt_http2_server_receive_request(unwritten, 1024u, &request) == 0;
    test_result("HTTP/2 rejects unwritten callback bytes deterministically", unwritten_rejected);
    rt_http2_request_free(&request);
    rt_http2_conn_free(unwritten);
}

static void close_pipe(pipe_buffer_t &pipe) {
    {
        std::lock_guard<std::mutex> lock(pipe.mutex);
        pipe.closed = true;
    }
    pipe.cv.notify_all();
}

struct raw_frame_t {
    uint8_t type = 0;
    uint8_t flags = 0;
    uint32_t stream_id = 0;
    std::vector<uint8_t> payload;
};

static constexpr uint8_t kFrameData = 0x0;
static constexpr uint8_t kFrameHeaders = 0x1;
static constexpr uint8_t kFramePriority = 0x2;
static constexpr uint8_t kFrameRstStream = 0x3;
static constexpr uint8_t kFrameSettings = 0x4;
static constexpr uint8_t kFrameUnknownExtension = 0x0b;
static constexpr uint8_t kFlagEndStream = 0x1;
static constexpr uint8_t kFlagAck = 0x1;
static constexpr uint8_t kFlagEndHeaders = 0x4;
static constexpr uint16_t kSettingEnablePush = 0x2;
static constexpr uint32_t kH2RefusedStream = 0x7;
static constexpr char kClientPreface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

static void pipe_push(pipe_buffer_t &pipe, const uint8_t *buf, size_t len) {
    {
        std::lock_guard<std::mutex> lock(pipe.mutex);
        pipe.bytes.insert(pipe.bytes.end(), buf, buf + len);
    }
    pipe.cv.notify_all();
}

static bool pipe_pop_exact(pipe_buffer_t &pipe, uint8_t *buf, size_t len, int timeout_ms = 2000) {
    std::unique_lock<std::mutex> lock(pipe.mutex);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (pipe.bytes.size() < len && !pipe.closed) {
        if (pipe.cv.wait_until(lock, deadline) == std::cv_status::timeout &&
            pipe.bytes.size() < len)
            return false;
    }
    if (pipe.bytes.size() < len)
        return false;
    std::memcpy(buf, pipe.bytes.data(), len);
    pipe.bytes.erase(pipe.bytes.begin(), pipe.bytes.begin() + len);
    return true;
}

static uint32_t read_u24(const uint8_t *src) {
    return (static_cast<uint32_t>(src[0]) << 16) | (static_cast<uint32_t>(src[1]) << 8) |
           static_cast<uint32_t>(src[2]);
}

static uint32_t read_u32(const uint8_t *src) {
    return (static_cast<uint32_t>(src[0]) << 24) | (static_cast<uint32_t>(src[1]) << 16) |
           (static_cast<uint32_t>(src[2]) << 8) | static_cast<uint32_t>(src[3]);
}

static void append_u24(std::vector<uint8_t> &out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xffu));
    out.push_back(static_cast<uint8_t>(value & 0xffu));
}

static void append_u16(std::vector<uint8_t> &out, uint16_t value) {
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xffu));
    out.push_back(static_cast<uint8_t>(value & 0xffu));
}

static void append_u32(std::vector<uint8_t> &out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xffu));
    out.push_back(static_cast<uint8_t>(value & 0xffu));
}

static bool read_frame(pipe_buffer_t &pipe, raw_frame_t *out, int timeout_ms = 2000) {
    uint8_t header[9];
    if (!out || !pipe_pop_exact(pipe, header, sizeof(header), timeout_ms))
        return false;
    out->type = header[3];
    out->flags = header[4];
    out->stream_id = read_u32(header + 5) & 0x7fffffffu;
    out->payload.resize(read_u24(header));
    if (!out->payload.empty() &&
        !pipe_pop_exact(pipe, out->payload.data(), out->payload.size(), timeout_ms))
        return false;
    return true;
}

static void write_frame(pipe_buffer_t &pipe,
                        uint8_t type,
                        uint8_t flags,
                        uint32_t stream_id,
                        const std::vector<uint8_t> &payload) {
    std::vector<uint8_t> frame;
    frame.reserve(9 + payload.size());
    append_u24(frame, static_cast<uint32_t>(payload.size()));
    frame.push_back(type);
    frame.push_back(flags);
    append_u32(frame, stream_id & 0x7fffffffu);
    frame.insert(frame.end(), payload.begin(), payload.end());
    pipe_push(pipe, frame.data(), frame.size());
}

static void send_client_preface(pipe_buffer_t &pipe) {
    pipe_push(pipe, reinterpret_cast<const uint8_t *>(kClientPreface), sizeof(kClientPreface) - 1);
}

static void hpack_encode_int(std::vector<uint8_t> &out,
                             uint32_t value,
                             uint8_t prefix_bits,
                             uint8_t first_byte_base) {
    const uint32_t max_prefix = (1u << prefix_bits) - 1u;
    if (value < max_prefix) {
        out.push_back(static_cast<uint8_t>(first_byte_base | value));
        return;
    }
    out.push_back(static_cast<uint8_t>(first_byte_base | max_prefix));
    value -= max_prefix;
    while (value >= 128u) {
        out.push_back(static_cast<uint8_t>((value & 0x7fu) | 0x80u));
        value >>= 7;
    }
    out.push_back(static_cast<uint8_t>(value));
}

static void hpack_encode_string(std::vector<uint8_t> &out, const std::string &value) {
    hpack_encode_int(out, static_cast<uint32_t>(value.size()), 7, 0x00);
    out.insert(out.end(), value.begin(), value.end());
}

static void hpack_encode_literal(std::vector<uint8_t> &out,
                                 const char *name,
                                 const std::string &value) {
    hpack_encode_int(out, 0, 4, 0x00);
    hpack_encode_string(out, name ? name : "");
    hpack_encode_string(out, value);
}

static void hpack_encode_dynamic_table_size_update(std::vector<uint8_t> &out, uint32_t value) {
    hpack_encode_int(out, value, 5, 0x20);
}

static std::vector<uint8_t> make_request_header_block(const char *method,
                                                      const char *authority,
                                                      const char *path) {
    std::vector<uint8_t> block;
    hpack_encode_literal(block, ":method", method ? method : "GET");
    hpack_encode_literal(block, ":scheme", "https");
    hpack_encode_literal(block, ":authority", authority ? authority : "example.test");
    hpack_encode_literal(block, ":path", path ? path : "/");
    return block;
}

static std::vector<uint8_t> make_response_header_block(int status,
                                                       const char *name,
                                                       const char *value) {
    std::vector<uint8_t> block;
    hpack_encode_literal(block, ":status", std::to_string(status));
    if (name && value)
        hpack_encode_literal(block, name, value);
    return block;
}

static std::vector<uint8_t> make_trailer_block(const char *name, const char *value) {
    std::vector<uint8_t> block;
    hpack_encode_literal(block, name ? name : "x-trailer", value ? value : "yes");
    return block;
}

static void test_http2_roundtrip_basic() {
    printf("\nTesting HTTP/2 in-memory round-trip:\n");

    pipe_buffer_t c2s;
    pipe_buffer_t s2c;
    pipe_endpoint_t client_ep{&s2c, &c2s};
    pipe_endpoint_t server_ep{&c2s, &s2c};
    rt_http2_io_t client_io{&client_ep, pipe_read_cb, pipe_write_cb};
    rt_http2_io_t server_io{&server_ep, pipe_read_cb, pipe_write_cb};
    rt_http2_conn_t *client = rt_http2_client_new(&client_io);
    rt_http2_conn_t *server = rt_http2_server_new(&server_io);
    assert(client != nullptr);
    assert(server != nullptr);

    std::thread server_thread([&]() {
        rt_http2_request_t req{};
        bool ok = rt_http2_server_receive_request(server, 1024, &req) == 1;
        test_result("HTTP/2 server receives request", ok);
        test_result("HTTP/2 request stream id is 1", req.stream_id == 1);
        test_result("HTTP/2 request method is POST", std::strcmp(req.method, "POST") == 0);
        test_result("HTTP/2 request scheme is https", std::strcmp(req.scheme, "https") == 0);
        test_result("HTTP/2 request authority preserved",
                    std::strcmp(req.authority, "example.test") == 0);
        test_result("HTTP/2 request path preserved", std::strcmp(req.path, "/upload?q=1") == 0);
        test_result("HTTP/2 custom header lower-cased",
                    std::strcmp(rt_http2_header_get(req.headers, "x-test"), "yes") == 0);
        test_result("HTTP/2 request body preserved",
                    req.body_len == 5 && std::memcmp(req.body, "hello", 5) == 0);

        rt_http2_header_t *headers = nullptr;
        ok = rt_http2_header_append_copy(&headers, "content-type", "text/plain") == 1 &&
             rt_http2_server_send_response(
                 server, req.stream_id, 201, headers, (const uint8_t *)"world", 5) == 1;
        test_result("HTTP/2 server sends response", ok);

        rt_http2_headers_free(headers);
        rt_http2_request_free(&req);
    });

    rt_http2_header_t *req_headers = nullptr;
    rt_http2_response_t res{};
    bool ok = rt_http2_header_append_copy(&req_headers, "X-Test", "yes") == 1 &&
              rt_http2_client_roundtrip(client,
                                        "POST",
                                        "https",
                                        "example.test",
                                        "/upload?q=1",
                                        req_headers,
                                        (const uint8_t *)"hello",
                                        5,
                                        1024,
                                        &res) == 1;
    test_result("HTTP/2 client completes round-trip", ok);
    test_result("HTTP/2 response stream id is 1", res.stream_id == 1);
    test_result("HTTP/2 response status is 201", res.status == 201);
    test_result("HTTP/2 response header preserved",
                std::strcmp(rt_http2_header_get(res.headers, "content-type"), "text/plain") == 0);
    test_result("HTTP/2 response body preserved",
                res.body_len == 5 && std::memcmp(res.body, "world", 5) == 0);

    server_thread.join();
    close_pipe(c2s);
    close_pipe(s2c);
    rt_http2_headers_free(req_headers);
    rt_http2_response_free(&res);
    rt_http2_conn_free(client);
    rt_http2_conn_free(server);
}

static void test_http2_reuses_connection_for_second_stream() {
    printf("\nTesting HTTP/2 stream reuse:\n");

    pipe_buffer_t c2s;
    pipe_buffer_t s2c;
    pipe_endpoint_t client_ep{&s2c, &c2s};
    pipe_endpoint_t server_ep{&c2s, &s2c};
    rt_http2_io_t client_io{&client_ep, pipe_read_cb, pipe_write_cb};
    rt_http2_io_t server_io{&server_ep, pipe_read_cb, pipe_write_cb};
    rt_http2_conn_t *client = rt_http2_client_new(&client_io);
    rt_http2_conn_t *server = rt_http2_server_new(&server_io);
    assert(client != nullptr);
    assert(server != nullptr);

    std::thread server_thread([&]() {
        for (int i = 0; i < 2; i++) {
            rt_http2_request_t req{};
            std::string expected_path = i == 0 ? "/one" : "/two";
            std::string reply = i == 0 ? "one" : "two";
            bool ok = rt_http2_server_receive_request(server, 1024, &req) == 1;
            test_result(i == 0 ? "HTTP/2 server receives first stream"
                               : "HTTP/2 server receives second stream",
                        ok);
            test_result(i == 0 ? "HTTP/2 first path is /one" : "HTTP/2 second path is /two",
                        expected_path == req.path);
            ok = rt_http2_server_send_response(server,
                                               req.stream_id,
                                               200,
                                               nullptr,
                                               (const uint8_t *)reply.data(),
                                               reply.size()) == 1;
            test_result(i == 0 ? "HTTP/2 server sends first response"
                               : "HTTP/2 server sends second response",
                        ok);
            rt_http2_request_free(&req);
        }
    });

    rt_http2_response_t first{};
    rt_http2_response_t second{};
    bool ok =
        rt_http2_client_roundtrip(
            client, "GET", "https", "example.test", "/one", nullptr, nullptr, 0, 1024, &first) ==
            1 &&
        rt_http2_client_roundtrip(
            client, "GET", "https", "example.test", "/two", nullptr, nullptr, 0, 1024, &second) ==
            1;
    test_result("HTTP/2 client reuses one connection for two streams", ok);
    test_result("HTTP/2 first stream id is 1", first.stream_id == 1);
    test_result("HTTP/2 second stream id is 3", second.stream_id == 3);
    test_result("HTTP/2 first response body matches",
                first.body_len == 3 && std::memcmp(first.body, "one", 3) == 0);
    test_result("HTTP/2 second response body matches",
                second.body_len == 3 && std::memcmp(second.body, "two", 3) == 0);

    server_thread.join();
    close_pipe(c2s);
    close_pipe(s2c);
    rt_http2_response_free(&first);
    rt_http2_response_free(&second);
    rt_http2_conn_free(client);
    rt_http2_conn_free(server);
}

static void test_http2_server_refuses_concurrent_streams_without_dropping_connection() {
    printf("\nTesting HTTP/2 concurrent stream refusal + request trailers:\n");

    pipe_buffer_t c2s;
    pipe_buffer_t s2c;
    pipe_endpoint_t server_ep{&c2s, &s2c};
    rt_http2_io_t server_io{&server_ep, pipe_read_cb, pipe_write_cb};
    rt_http2_conn_t *server = rt_http2_server_new(&server_io);
    assert(server != nullptr);

    std::thread server_thread([&]() {
        rt_http2_request_t first{};
        bool ok = rt_http2_server_receive_request(server, 1024, &first) == 1;
        test_result("HTTP/2 server completes active request despite concurrent stream", ok);
        test_result("HTTP/2 first request stays on stream 1", first.stream_id == 1);
        test_result("HTTP/2 first request path preserved", std::strcmp(first.path, "/slow") == 0);
        test_result("HTTP/2 first request body preserved",
                    first.body_len == 1 && std::memcmp(first.body, "a", 1) == 0);
        {
            const char *trailer = rt_http2_header_get(first.headers, "x-trailer");
            test_result("HTTP/2 request trailers preserved",
                        trailer && std::strcmp(trailer, "yes") == 0);
        }
        ok = rt_http2_server_send_response(
                 server, first.stream_id, 200, nullptr, (const uint8_t *)"one", 3) == 1;
        test_result("HTTP/2 server replies to first request after refusal", ok);
        rt_http2_request_free(&first);

        rt_http2_request_t second{};
        ok = rt_http2_server_receive_request(server, 1024, &second) == 1;
        test_result("HTTP/2 server remains usable after concurrent refusal", ok);
        test_result("HTTP/2 next accepted request advances to stream 5", second.stream_id == 5);
        test_result("HTTP/2 later request path preserved", std::strcmp(second.path, "/after") == 0);
        ok = rt_http2_server_send_response(
                 server, second.stream_id, 200, nullptr, (const uint8_t *)"two", 3) == 1;
        test_result("HTTP/2 server replies to later request on same connection", ok);
        rt_http2_request_free(&second);
    });

    send_client_preface(c2s);
    write_frame(c2s, kFrameSettings, 0, 0, {});

    raw_frame_t frame{};
    bool ok = read_frame(s2c, &frame);
    test_result("HTTP/2 server emits SETTINGS after preface",
                ok && frame.type == kFrameSettings && frame.stream_id == 0);

    write_frame(c2s, kFrameSettings, kFlagAck, 0, {});
    write_frame(c2s,
                kFrameHeaders,
                kFlagEndHeaders,
                1,
                make_request_header_block("POST", "example.test", "/slow"));
    write_frame(c2s,
                kFrameHeaders,
                static_cast<uint8_t>(kFlagEndHeaders | kFlagEndStream),
                3,
                make_request_header_block("GET", "example.test", "/queued"));
    write_frame(c2s, kFrameData, 0, 1, std::vector<uint8_t>{'a'});
    write_frame(c2s,
                kFrameHeaders,
                static_cast<uint8_t>(kFlagEndHeaders | kFlagEndStream),
                1,
                make_trailer_block("x-trailer", "yes"));

    bool saw_refusal = false;
    bool saw_first_body = false;
    while (!(saw_refusal && saw_first_body)) {
        ok = read_frame(s2c, &frame);
        test_result("HTTP/2 client reads refusal/response frames", ok);
        if (frame.type == kFrameRstStream && frame.stream_id == 3) {
            saw_refusal =
                frame.payload.size() == 4 && read_u32(frame.payload.data()) == kH2RefusedStream;
            test_result("HTTP/2 concurrent request is refused with RST_STREAM", saw_refusal);
        } else if (frame.type == kFrameData && frame.stream_id == 1) {
            saw_first_body =
                frame.payload.size() == 3 && std::memcmp(frame.payload.data(), "one", 3) == 0;
            test_result("HTTP/2 first response body survives concurrent refusal", saw_first_body);
        }
    }

    write_frame(c2s,
                kFrameHeaders,
                static_cast<uint8_t>(kFlagEndHeaders | kFlagEndStream),
                5,
                make_request_header_block("GET", "example.test", "/after"));

    bool saw_second_body = false;
    while (!saw_second_body) {
        ok = read_frame(s2c, &frame);
        test_result("HTTP/2 client reads later response frames", ok);
        if (frame.type == kFrameData && frame.stream_id == 5) {
            saw_second_body =
                frame.payload.size() == 3 && std::memcmp(frame.payload.data(), "two", 3) == 0;
            test_result("HTTP/2 later response body arrives on surviving connection",
                        saw_second_body);
        }
    }

    server_thread.join();
    close_pipe(c2s);
    close_pipe(s2c);
    rt_http2_conn_free(server);
}

static void test_http2_client_accepts_response_trailers() {
    printf("\nTesting HTTP/2 response trailers:\n");

    pipe_buffer_t c2s;
    pipe_buffer_t s2c;
    pipe_endpoint_t client_ep{&s2c, &c2s};
    rt_http2_io_t client_io{&client_ep, pipe_read_cb, pipe_write_cb};
    rt_http2_conn_t *client = rt_http2_client_new(&client_io);
    assert(client != nullptr);

    std::thread server_thread([&]() {
        raw_frame_t frame{};
        char preface[sizeof(kClientPreface) - 1];
        bool ok = pipe_pop_exact(c2s, reinterpret_cast<uint8_t *>(preface), sizeof(preface));
        test_result("HTTP/2 raw server reads client preface",
                    ok && std::memcmp(preface, kClientPreface, sizeof(preface)) == 0);

        ok = read_frame(c2s, &frame);
        test_result("HTTP/2 raw server reads client SETTINGS",
                    ok && frame.type == kFrameSettings && frame.stream_id == 0 &&
                        (frame.flags & kFlagAck) == 0);

        write_frame(s2c, kFrameSettings, 0, 0, {});

        bool saw_request = false;
        while (!saw_request) {
            ok = read_frame(c2s, &frame);
            test_result("HTTP/2 raw server reads client request frame", ok);
            if (frame.type == kFrameHeaders && frame.stream_id == 1)
                saw_request = true;
        }
        test_result("HTTP/2 raw server sees request HEADERS on stream 1", saw_request);

        write_frame(s2c,
                    kFrameHeaders,
                    kFlagEndHeaders,
                    1,
                    make_response_header_block(200, "content-type", "text/plain"));
        write_frame(s2c, kFrameData, 0, 1, std::vector<uint8_t>{'o', 'k'});
        write_frame(s2c,
                    kFrameHeaders,
                    static_cast<uint8_t>(kFlagEndHeaders | kFlagEndStream),
                    1,
                    make_trailer_block("x-trailer", "yes"));
    });

    rt_http2_response_t res{};
    bool ok =
        rt_http2_client_roundtrip(
            client, "GET", "https", "example.test", "/trailers", nullptr, nullptr, 0, 1024, &res) ==
        1;
    test_result("HTTP/2 client completes response with trailers", ok);
    test_result("HTTP/2 trailer-bearing response status is 200", res.status == 200);
    test_result("HTTP/2 trailer-bearing response body preserved",
                res.body_len == 2 && std::memcmp(res.body, "ok", 2) == 0);
    {
        const char *content_type = rt_http2_header_get(res.headers, "content-type");
        test_result("HTTP/2 regular response header preserved",
                    content_type && std::strcmp(content_type, "text/plain") == 0);
    }
    {
        const char *trailer = rt_http2_header_get(res.headers, "x-trailer");
        test_result("HTTP/2 response trailers appended to header list",
                    trailer && std::strcmp(trailer, "yes") == 0);
    }

    server_thread.join();
    close_pipe(c2s);
    close_pipe(s2c);
    rt_http2_response_free(&res);
    rt_http2_conn_free(client);
}

static void test_http2_client_ignores_unknown_stream_extension_frame() {
    printf("\nTesting HTTP/2 unknown extension frame handling:\n");

    pipe_buffer_t c2s;
    pipe_buffer_t s2c;
    pipe_endpoint_t client_ep{&s2c, &c2s};
    rt_http2_io_t client_io{&client_ep, pipe_read_cb, pipe_write_cb};
    rt_http2_conn_t *client = rt_http2_client_new(&client_io);
    assert(client != nullptr);

    std::thread server_thread([&]() {
        raw_frame_t frame{};
        char preface[sizeof(kClientPreface) - 1];
        bool ok = pipe_pop_exact(c2s, reinterpret_cast<uint8_t *>(preface), sizeof(preface));
        test_result("HTTP/2 extension test reads client preface",
                    ok && std::memcmp(preface, kClientPreface, sizeof(preface)) == 0);
        ok = read_frame(c2s, &frame);
        test_result("HTTP/2 extension test reads client SETTINGS",
                    ok && frame.type == kFrameSettings && frame.stream_id == 0);
        write_frame(s2c, kFrameSettings, 0, 0, {});
        bool saw_request = false;
        while (!saw_request) {
            ok = read_frame(c2s, &frame);
            test_result("HTTP/2 extension test reads request frame", ok);
            if (frame.type == kFrameHeaders && frame.stream_id == 1)
                saw_request = true;
        }

        write_frame(s2c, kFrameUnknownExtension, 0, 1, std::vector<uint8_t>{1, 2, 3});
        write_frame(s2c,
                    kFrameHeaders,
                    kFlagEndHeaders,
                    1,
                    make_response_header_block(200, "content-type", "text/plain"));
        write_frame(s2c, kFrameData, kFlagEndStream, 1, std::vector<uint8_t>{'o', 'k'});
    });

    rt_http2_response_t res{};
    bool ok = rt_http2_client_roundtrip(client,
                                        "GET",
                                        "https",
                                        "example.test",
                                        "/extension",
                                        nullptr,
                                        nullptr,
                                        0,
                                        1024,
                                        &res) == 1;
    test_result("HTTP/2 client ignores unknown stream extension frame", ok);
    test_result("HTTP/2 extension response body preserved",
                res.body_len == 2 && std::memcmp(res.body, "ok", 2) == 0);

    server_thread.join();
    close_pipe(c2s);
    close_pipe(s2c);
    rt_http2_response_free(&res);
    rt_http2_conn_free(client);
}

static void test_http2_client_ignores_informational_response() {
    printf("\nTesting HTTP/2 informational response handling:\n");

    pipe_buffer_t c2s;
    pipe_buffer_t s2c;
    pipe_endpoint_t client_ep{&s2c, &c2s};
    rt_http2_io_t client_io{&client_ep, pipe_read_cb, pipe_write_cb};
    rt_http2_conn_t *client = rt_http2_client_new(&client_io);
    assert(client != nullptr);

    std::thread server_thread([&]() {
        raw_frame_t frame{};
        char preface[sizeof(kClientPreface) - 1];
        bool ok = pipe_pop_exact(c2s, reinterpret_cast<uint8_t *>(preface), sizeof(preface));
        test_result("HTTP/2 informational test reads client preface",
                    ok && std::memcmp(preface, kClientPreface, sizeof(preface)) == 0);
        ok = read_frame(c2s, &frame);
        test_result("HTTP/2 informational test reads client SETTINGS",
                    ok && frame.type == kFrameSettings && frame.stream_id == 0);
        write_frame(s2c, kFrameSettings, 0, 0, {});
        bool saw_request = false;
        while (!saw_request) {
            ok = read_frame(c2s, &frame);
            test_result("HTTP/2 informational test reads request frame", ok);
            if (frame.type == kFrameHeaders && frame.stream_id == 1)
                saw_request = true;
        }

        write_frame(s2c,
                    kFrameHeaders,
                    kFlagEndHeaders,
                    1,
                    make_response_header_block(103, "x-hint", "preload"));
        write_frame(s2c,
                    kFrameHeaders,
                    kFlagEndHeaders,
                    1,
                    make_response_header_block(200, "content-type", "text/plain"));
        write_frame(s2c, kFrameData, kFlagEndStream, 1, std::vector<uint8_t>{'d', 'o', 'n', 'e'});
    });

    rt_http2_response_t res{};
    bool ok =
        rt_http2_client_roundtrip(
            client, "GET", "https", "example.test", "/early", nullptr, nullptr, 0, 1024, &res) == 1;
    test_result("HTTP/2 client completes after informational response", ok);
    test_result("HTTP/2 final response status wins", res.status == 200);
    test_result("HTTP/2 informational headers are not stored",
                rt_http2_header_get(res.headers, "x-hint") == nullptr);
    test_result("HTTP/2 final response body preserved",
                res.body_len == 4 && std::memcmp(res.body, "done", 4) == 0);

    server_thread.join();
    close_pipe(c2s);
    close_pipe(s2c);
    rt_http2_response_free(&res);
    rt_http2_conn_free(client);
}

static void test_http2_rejects_priority_on_connection_stream() {
    printf("\nTesting HTTP/2 PRIORITY stream validation:\n");

    pipe_buffer_t c2s;
    pipe_buffer_t s2c;
    pipe_endpoint_t server_ep{&c2s, &s2c};
    rt_http2_io_t server_io{&server_ep, pipe_read_cb, pipe_write_cb};
    rt_http2_conn_t *server = rt_http2_server_new(&server_io);
    assert(server != nullptr);

    send_client_preface(c2s);
    write_frame(c2s, kFrameSettings, 0, 0, {});
    write_frame(c2s, kFramePriority, 0, 0, std::vector<uint8_t>{0, 0, 0, 0, 0});

    rt_http2_request_t req{};
    bool ok = rt_http2_server_receive_request(server, 1024, &req) == 0;
    test_result("HTTP/2 server rejects PRIORITY on stream 0", ok);
    const char *err = rt_http2_get_error(server);
    test_result("HTTP/2 PRIORITY error is reported",
                err && std::strstr(err, "PRIORITY") != nullptr);

    close_pipe(c2s);
    close_pipe(s2c);
    rt_http2_request_free(&req);
    rt_http2_conn_free(server);
}

static void test_http2_rejects_oversized_inbound_frame() {
    printf("\nTesting HTTP/2 local max frame size enforcement:\n");

    pipe_buffer_t c2s;
    pipe_buffer_t s2c;
    pipe_endpoint_t server_ep{&c2s, &s2c};
    rt_http2_io_t server_io{&server_ep, pipe_read_cb, pipe_write_cb};
    rt_http2_conn_t *server = rt_http2_server_new(&server_io);
    assert(server != nullptr);

    send_client_preface(c2s);
    write_frame(c2s, kFrameSettings, 0, 0, {});
    write_frame(c2s, kFrameUnknownExtension, 0, 1, std::vector<uint8_t>(16385, 0));

    rt_http2_request_t req{};
    bool ok = rt_http2_server_receive_request(server, 1024, &req) == 0;
    test_result("HTTP/2 server rejects frame above default local max", ok);
    const char *err = rt_http2_get_error(server);
    test_result("HTTP/2 oversized frame error is reported",
                err && std::strstr(err, "oversized frame") != nullptr);

    close_pipe(c2s);
    close_pipe(s2c);
    rt_http2_request_free(&req);
    rt_http2_conn_free(server);
}

static void test_http2_rejects_invalid_hpack_header_blocks() {
    printf("\nTesting HTTP/2 HPACK header block validation:\n");

    {
        pipe_buffer_t c2s;
        pipe_buffer_t s2c;
        pipe_endpoint_t server_ep{&c2s, &s2c};
        rt_http2_io_t server_io{&server_ep, pipe_read_cb, pipe_write_cb};
        rt_http2_conn_t *server = rt_http2_server_new(&server_io);
        assert(server != nullptr);

        std::vector<uint8_t> block;
        hpack_encode_literal(block, ":method", "GET");
        hpack_encode_dynamic_table_size_update(block, 0);
        hpack_encode_literal(block, ":scheme", "https");
        hpack_encode_literal(block, ":authority", "example.test");
        hpack_encode_literal(block, ":path", "/");

        send_client_preface(c2s);
        write_frame(c2s, kFrameSettings, 0, 0, {});
        write_frame(
            c2s, kFrameHeaders, static_cast<uint8_t>(kFlagEndHeaders | kFlagEndStream), 1, block);

        rt_http2_request_t req{};
        bool ok = rt_http2_server_receive_request(server, 1024, &req) == 0;
        test_result("HTTP/2 server rejects mid-block dynamic table size update", ok);

        close_pipe(c2s);
        close_pipe(s2c);
        rt_http2_request_free(&req);
        rt_http2_conn_free(server);
    }

    {
        pipe_buffer_t c2s;
        pipe_buffer_t s2c;
        pipe_endpoint_t server_ep{&c2s, &s2c};
        rt_http2_io_t server_io{&server_ep, pipe_read_cb, pipe_write_cb};
        rt_http2_conn_t *server = rt_http2_server_new(&server_io);
        assert(server != nullptr);

        std::vector<uint8_t> block = make_request_header_block("GET", "example.test", "/nul");
        hpack_encode_literal(block, "x-bad", std::string("ok\0bad", 6));

        send_client_preface(c2s);
        write_frame(c2s, kFrameSettings, 0, 0, {});
        write_frame(
            c2s, kFrameHeaders, static_cast<uint8_t>(kFlagEndHeaders | kFlagEndStream), 1, block);

        rt_http2_request_t req{};
        bool ok = rt_http2_server_receive_request(server, 1024, &req) == 0;
        test_result("HTTP/2 server rejects embedded NUL in decoded header value", ok);

        close_pipe(c2s);
        close_pipe(s2c);
        rt_http2_request_free(&req);
        rt_http2_conn_free(server);
    }
}

static void test_http2_rejects_invalid_settings_value() {
    printf("\nTesting HTTP/2 invalid SETTINGS rejection:\n");

    pipe_buffer_t c2s;
    pipe_buffer_t s2c;
    pipe_endpoint_t server_ep{&c2s, &s2c};
    rt_http2_io_t server_io{&server_ep, pipe_read_cb, pipe_write_cb};
    rt_http2_conn_t *server = rt_http2_server_new(&server_io);
    assert(server != nullptr);

    send_client_preface(c2s);
    std::vector<uint8_t> payload;
    append_u16(payload, kSettingEnablePush);
    append_u32(payload, 2);
    write_frame(c2s, kFrameSettings, 0, 0, payload);

    rt_http2_request_t req{};
    bool ok = rt_http2_server_receive_request(server, 1024, &req) == 0;
    test_result("HTTP/2 server rejects invalid ENABLE_PUSH setting", ok);
    const char *err = rt_http2_get_error(server);
    test_result("HTTP/2 invalid SETTINGS error is reported",
                err && std::strstr(err, "SETTINGS") != nullptr);

    close_pipe(c2s);
    close_pipe(s2c);
    rt_http2_request_free(&req);
    rt_http2_conn_free(server);
}

int main() {
    test_http2_callback_contract();
    test_http2_roundtrip_basic();
    test_http2_reuses_connection_for_second_stream();
    test_http2_server_refuses_concurrent_streams_without_dropping_connection();
    test_http2_client_accepts_response_trailers();
    test_http2_client_ignores_unknown_stream_extension_frame();
    test_http2_client_ignores_informational_response();
    test_http2_rejects_priority_on_connection_stream();
    test_http2_rejects_oversized_inbound_frame();
    test_http2_rejects_invalid_hpack_header_blocks();
    test_http2_rejects_invalid_settings_value();
    printf("\nAll HTTP/2 tests passed.\n");
    return 0;
}
