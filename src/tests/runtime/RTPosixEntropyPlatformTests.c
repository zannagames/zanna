//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTPosixEntropyPlatformTests.c
// Purpose: Fault-injection coverage for the POSIX entropy adapter.
//
// Key invariants:
//   - Linux fallback preserves bytes already returned by getrandom().
//   - Sandbox-denied getrandom() calls fall back to /dev/urandom.
//   - Fallback descriptors are close-on-exec and reads never exceed SSIZE_MAX.
//   - Source failures retain their errno across descriptor cleanup.
//
// Ownership/Lifetime:
//   - Mock descriptors have no operating-system ownership.
//
// Links: src/runtime/network/rt_entropy_platform_posix.c
//
//===----------------------------------------------------------------------===//

// glibc emits fortified inline definitions of read() and open() when
// _FORTIFY_SOURCE is active, which GCC turns on by default at -O1 and above on
// several distributions. Those inline definitions collide with the
// `#define read mock_read` interposition below, so opt this translation unit
// out before any system header is pulled in.
#undef _FORTIFY_SOURCE
#define _FORTIFY_SOURCE 0

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

enum mock_mode {
    MOCK_PARTIAL_ENOSYS,
    MOCK_DENIED,
    MOCK_FATAL_GETRANDOM,
    MOCK_READ_FAILURE,
    MOCK_READ_EOF,
    MOCK_SUCCESS,
};

static enum mock_mode g_mode;
static int g_getrandom_calls;
static int g_open_calls;
static int g_read_calls;
static int g_close_calls;
static int g_fcntl_calls;
static size_t g_largest_read;
static int g_close_errno;

static ssize_t mock_getrandom(void *buf, size_t len, unsigned int flags);
static int mock_open(const char *path, int flags, ...);
static ssize_t mock_read(int fd, void *buf, size_t count);
static int mock_close(int fd);
static int mock_fcntl(int fd, int command, ...);

#define getrandom mock_getrandom
#define open mock_open
#define read mock_read
#define close mock_close
#define fcntl mock_fcntl
#undef O_CLOEXEC
#undef SSIZE_MAX
#define SSIZE_MAX 4
#include "../../runtime/network/rt_entropy_platform_posix.c"
#undef fcntl
#undef close
#undef read
#undef open
#undef getrandom

static void reset_mocks(enum mock_mode mode) {
    g_mode = mode;
    g_getrandom_calls = 0;
    g_open_calls = 0;
    g_read_calls = 0;
    g_close_calls = 0;
    g_fcntl_calls = 0;
    g_largest_read = 0;
    g_close_errno = 0;
}

static ssize_t mock_getrandom(void *buf, size_t len, unsigned int flags) {
    (void)flags;
    g_getrandom_calls++;
    if (g_mode == MOCK_PARTIAL_ENOSYS && g_getrandom_calls == 1) {
        assert(len >= 3);
        memset(buf, 0xA1, 3);
        return 3;
    }
    if (g_mode == MOCK_FATAL_GETRANDOM) {
        errno = EIO;
        return -1;
    }
    errno = g_mode == MOCK_DENIED ? EPERM : ENOSYS;
    return -1;
}

static int mock_open(const char *path, int flags, ...) {
    assert(strcmp(path, "/dev/urandom") == 0);
    assert(flags == O_RDONLY);
    g_open_calls++;
    return 71;
}

static ssize_t mock_read(int fd, void *buf, size_t count) {
    assert(fd == 71);
    assert(count <= 4);
    g_read_calls++;
    if (count > g_largest_read)
        g_largest_read = count;
    if (g_mode == MOCK_READ_FAILURE) {
        errno = EIO;
        return -1;
    }
    if (g_mode == MOCK_READ_EOF)
        return 0;
    memset(buf, 0xB2, count);
    return (ssize_t)count;
}

static int mock_close(int fd) {
    assert(fd == 71);
    g_close_calls++;
    if (g_close_errno != 0) {
        errno = g_close_errno;
        return -1;
    }
    errno = EBADF;
    return 0;
}

static int mock_fcntl(int fd, int command, ...) {
    assert(fd == 71);
    g_fcntl_calls++;
    if (command == F_GETFD)
        return 0;
    assert(command == F_SETFD);
    va_list args;
    va_start(args, command);
    int flags = va_arg(args, int);
    va_end(args);
    assert((flags & FD_CLOEXEC) != 0);
    return 0;
}

static void test_partial_fallback_and_bounded_reads(void) {
    uint8_t bytes[10] = {0};
    reset_mocks(MOCK_PARTIAL_ENOSYS);
    assert(rt_entropy_platform_random_bytes(bytes, sizeof(bytes)) == 0);
    assert(bytes[0] == 0xA1 && bytes[1] == 0xA1 && bytes[2] == 0xA1);
    for (size_t index = 3; index < sizeof(bytes); ++index)
        assert(bytes[index] == 0xB2);
    assert(g_open_calls == 1);
    assert(g_read_calls == 2);
    assert(g_largest_read == 4);
    assert(g_fcntl_calls == 2);
    assert(g_close_calls == 1);
}

static void test_sandbox_denial_falls_back(void) {
    uint8_t byte = 0;
    reset_mocks(MOCK_DENIED);
    assert(rt_entropy_platform_random_bytes(&byte, 1) == 0);
    assert(byte == 0xB2);
    assert(g_open_calls == 1);
}

static void test_fatal_getrandom_does_not_fallback(void) {
    uint8_t byte = 0;
    reset_mocks(MOCK_FATAL_GETRANDOM);
    assert(rt_entropy_platform_random_bytes(&byte, 1) == -1);
    assert(errno == EIO);
    assert(g_open_calls == 0);
}

static void test_read_error_survives_close(void) {
    uint8_t byte = 0;
    reset_mocks(MOCK_READ_FAILURE);
    assert(rt_entropy_platform_random_bytes(&byte, 1) == -1);
    assert(errno == EIO);
    assert(g_close_calls == 1);
}

static void test_eof_and_close_errors_are_reported(void) {
    uint8_t byte = 0;
    reset_mocks(MOCK_READ_EOF);
    assert(rt_entropy_platform_random_bytes(&byte, 1) == -1);
    assert(errno == EIO);

    reset_mocks(MOCK_SUCCESS);
    g_close_errno = EINTR;
    assert(rt_entropy_platform_random_bytes(&byte, 1) == -1);
    assert(errno == EINTR);
}

int main(void) {
    test_partial_fallback_and_bounded_reads();
    test_sandbox_denial_falls_back();
    test_fatal_getrandom_does_not_fallback();
    test_read_error_survives_close();
    test_eof_and_close_errors_are_reported();
    return 0;
}
