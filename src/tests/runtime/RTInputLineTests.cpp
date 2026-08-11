//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/runtime/RTInputLineTests.cpp
// Purpose: Ensure rt_input_line handles lines longer than the initial buffer and EOF-terminated
// Key invariants: To be documented.
// Ownership/Lifetime: To be documented.
// Links: docs/internals/architecture.md
//
//===----------------------------------------------------------------------===//

#include "rt_internal.h"
#include "tests/common/PosixCompat.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

static rt_string read_line(const std::string &data) {
    int fds[2];
    assert(pipe(fds) == 0);
#ifdef _WIN32
    if (!data.empty()) {
        const int written =
            posix_write(fds[1], data.data(), static_cast<unsigned int>(data.size()));
        assert(written == static_cast<int>(data.size()));
    }
    posix_close(fds[1]);
#else
    if (!data.empty()) {
        const ssize_t written = write(fds[1], data.data(), data.size());
        assert(written == static_cast<ssize_t>(data.size()));
    }
    close(fds[1]);
#endif

    // Save original stdin fd before replacing
#ifdef _WIN32
    int saved_stdin = posix_dup(0);
#else
    int saved_stdin = dup(0);
#endif
    assert(saved_stdin >= 0);

#ifdef _WIN32
    posix_dup2(fds[0], 0);
    posix_close(fds[0]);
#else
    dup2(fds[0], 0);
    close(fds[0]);
#endif

    // Clear any buffered state and error flags on stdin
    clearerr(stdin);
    // Discard any buffered input by seeking (no-op on pipes but clears buffer)
    fflush(stdin);

    rt_string result = rt_input_line();

    // Restore original stdin
#ifdef _WIN32
    posix_dup2(saved_stdin, 0);
    posix_close(saved_stdin);
#else
    dup2(saved_stdin, 0);
    close(saved_stdin);
#endif
    clearerr(stdin);

    return result;
}

static void feed_and_check(size_t len, bool with_newline) {
    std::string input(len, 'x');
    std::string data = with_newline ? input + "\n" : input;
    rt_string s = read_line(data);
    assert(s);
    assert(rt_str_len(s) == (int64_t)input.size());
    assert(std::memcmp(s->data, input.data(), input.size()) == 0);
}

static void feed_crlf_and_check(size_t len) {
    std::string input(len, 'x');
    std::string data = input + "\r\n";
    rt_string s = read_line(data);
    assert(s);
    assert(rt_str_len(s) == (int64_t)input.size());
    assert(std::memcmp(s->data, input.data(), input.size()) == 0);
    assert(std::memchr(s->data, '\r', input.size()) == nullptr);
}

static void feed_empty_newline_returns_empty_string() {
    rt_string s = read_line("\n");
    assert(s);
    assert(rt_str_len(s) == 0);
    assert(s->data[0] == '\0');
}

int main() {
#ifdef _WIN32
    // On Windows, dup2() redirection of stdin doesn't synchronize properly with
    // the C runtime's FILE* stdin stream, making pipe-based stdin tests unreliable.
    // Skip this test on Windows since the underlying rt_input_line functionality
    // is tested through integration tests.
    return 0;
#else
    feed_and_check(1500, true);
    feed_and_check(1500, false);
    feed_crlf_and_check(16);
    feed_empty_newline_returns_empty_string();
    return 0;
#endif
}
