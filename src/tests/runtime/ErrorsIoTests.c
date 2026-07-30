//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/ErrorsIoTests.c
// Purpose: Validate runtime file helpers return structured errors on failure paths.
//
// Key invariants:
//   - Missing files map to Err_FileNotFound and end-of-file maps to Err_EOF.
//   - Operating-system failures surface structured runtime errors rather than raw errno values.
//   - A close attempt consumes the descriptor even when the host close operation reports failure.
//
// Ownership/Lifetime:
//   - Tests exercise stack-owned RtFile values and close every successfully opened descriptor.
//   - Runtime strings returned by line reads are explicitly released by the test.
//
// Links: src/runtime/io/rt_file_io.c, src/runtime/io/rt_file.h, docs/specs/errors.md
//
//===----------------------------------------------------------------------===//

#define _XOPEN_SOURCE 700

#include "zanna/runtime/rt.h"

#include "rt_platform.h"
#include "tests/common/PosixCompat.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void ensure_missing_open_sets_file_not_found(void) {
    char path[] = "/tmp/zanna_io_missingXXXXXX";
    int fd = mkstemp(path);
    if (fd >= 0) {
        close(fd);
        unlink(path);
    }

    RtFile file;
    rt_file_init(&file);
    RtError err = {Err_None, 0};
    int8_t ok = rt_file_open(&file, path, "r", RT_F_UNSPECIFIED, &err);
    assert(!ok);
    assert(err.kind == Err_FileNotFound);
    assert(err.code != 0);
}

static void ensure_read_byte_reports_eof(void) {
    char path[] = "/tmp/zanna_io_emptyXXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);

    RtFile file;
    rt_file_init(&file);
    RtError err = {Err_None, 0};
    int8_t ok = rt_file_open(&file, path, "r", RT_F_UNSPECIFIED, &err);
    assert(ok);

    uint8_t value = 0xFF;
    err.kind = Err_RuntimeError;
    err.code = -1;
    ok = rt_file_read_byte(&file, &value, &err);
    assert(!ok);
    assert(err.kind == Err_EOF);
    assert(err.code == 0);

    ok = rt_file_close(&file, &err);
    assert(ok);
    unlink(path);
}

static void ensure_read_line_reports_eof(void) {
    char path[] = "/tmp/zanna_io_lineXXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);

    RtFile file;
    rt_file_init(&file);
    RtError err = {Err_None, 0};
    int8_t ok = rt_file_open(&file, path, "r", RT_F_UNSPECIFIED, &err);
    assert(ok);

    rt_string line = NULL;
    err.kind = Err_RuntimeError;
    err.code = -1;
    ok = rt_file_read_line(&file, &line, &err);
    assert(!ok);
    assert(err.kind == Err_EOF);
    assert(err.code == 0);
    assert(line == NULL);

    ok = rt_file_close(&file, &err);
    assert(ok);
    unlink(path);
}

static void ensure_read_line_trims_crlf(void) {
    char path[] = "/tmp/zanna_io_crlfXXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);

    const char payload[] = "hello world\r\n";
    ssize_t written = write(fd, payload, sizeof(payload) - 1);
    assert(written == (ssize_t)(sizeof(payload) - 1));
    int rc = close(fd);
    assert(rc == 0);

    RtFile file;
    rt_file_init(&file);
    RtError err = {Err_None, 0};
    int8_t ok = rt_file_open(&file, path, "r", RT_F_UNSPECIFIED, &err);
    assert(ok);

    rt_string line = NULL;
    ok = rt_file_read_line(&file, &line, &err);
    assert(ok);
    assert(line != NULL);
    const char *cstr = rt_string_cstr(line);
    assert(strcmp(cstr, "hello world") == 0);
    assert(rt_str_len(line) == (int64_t)strlen("hello world"));

    rt_string_unref(line);

    ok = rt_file_close(&file, &err);
    assert(ok);
    unlink(path);
}

static void ensure_invalid_handle_surfaces_ioerror(void) {
    RtFile file;
    rt_file_init(&file);
    file.fd = -1;

    RtError err = {Err_None, 0};
    int8_t ok = rt_file_seek(&file, 0, SEEK_SET, &err);
    assert(!ok);
    assert(err.kind == Err_IOError);
    assert(err.code != 0);
}

/// @brief Verify empty writes still validate descriptor lifetime.
/// @details A null data pointer remains valid for an empty write on an open
///          descriptor, but the same operation on a closed handle must report
///          the stale handle rather than silently succeeding.
static void ensure_zero_length_write_validates_handle(void) {
    RtFile file;
    rt_file_init(&file);

    RtError err = RT_ERROR_NONE;
    int8_t ok = rt_file_write(&file, NULL, 0, &err);
    assert(!ok);
    assert(err.kind == Err_IOError);
    assert(err.code != 0);

    char path[] = "/tmp/zanna_io_zero_writeXXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(close(fd) == 0);

    ok = rt_file_open(&file, path, "w", RT_F_UNSPECIFIED, &err);
    assert(ok);
    ok = rt_file_write(&file, NULL, 0, &err);
    assert(ok);
    assert(err.kind == Err_None);
    assert(rt_file_close(&file, &err));
    assert(unlink(path) == 0);
}

static void ensure_seek_out_of_range_reports_invalid_operation(void) {
#if (defined(OFF_MAX) && (OFF_MAX < INT64_MAX)) || (defined(OFF_MIN) && (OFF_MIN > INT64_MIN))
    char path[] = "/tmp/zanna_io_seek_rangeXXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    int rc = close(fd);
    assert(rc == 0);

    RtFile file;
    rt_file_init(&file);
    RtError err = {Err_None, 0};
    int8_t ok = rt_file_open(&file, path, "r", RT_F_UNSPECIFIED, &err);
    assert(ok);

#if defined(OFF_MAX) && (OFF_MAX < INT64_MAX)
    const int64_t overflow_offset = (int64_t)OFF_MAX + 1;
#else
    const int64_t overflow_offset = (int64_t)OFF_MIN - 1;
#endif
    err.kind = Err_None;
    err.code = 0;
    ok = rt_file_seek(&file, overflow_offset, SEEK_SET, &err);
    assert(!ok);
    assert(err.kind == Err_InvalidOperation);
    assert(err.code == ERANGE);

    ok = rt_file_close(&file, &err);
    assert(ok);
    unlink(path);
#else
    // No representable int64_t value falls outside off_t's range on this platform.
#endif
}

/// @brief Verify a failed host close cannot leave an RtFile alias to a reusable descriptor.
/// @details The test closes the descriptor externally to force `rt_file_close` through its EBADF
///          path, then opens a second descriptor that reuses the same integer. A correct close
///          implementation invalidates the RtFile before calling the host, so a subsequent
///          idempotent close cannot accidentally close the unrelated replacement descriptor.
static void ensure_close_failure_consumes_descriptor(void) {
    char path[] = "/tmp/zanna_io_close_failureXXXXXX";
    int seed_fd = mkstemp(path);
    assert(seed_fd >= 0);
    assert(close(seed_fd) == 0);

    RtFile file;
    rt_file_init(&file);
    RtError err = RT_ERROR_NONE;
    int8_t ok = rt_file_open(&file, path, "r", RT_F_UNSPECIFIED, &err);
    assert(ok);

    int stale_fd = file.fd;
    assert(close(stale_fd) == 0);
    ok = rt_file_close(&file, &err);
    assert(!ok);
    assert(err.kind == Err_IOError);
    assert(file.fd == -1);

    int replacement_fd = open(path, O_RDONLY);
    assert(replacement_fd >= 0);
    ok = rt_file_close(&file, &err);
    assert(ok);
#if RT_PLATFORM_WINDOWS
    assert(_get_osfhandle(replacement_fd) != -1);
#else
    assert(fcntl(replacement_fd, F_GETFD) != -1);
#endif

    assert(close(replacement_fd) == 0);
    assert(unlink(path) == 0);
}

/// @brief Execute all IO error-path unit checks.
int main(void) {
#if RT_PLATFORM_WINDOWS
    // Skip on Windows: test uses /tmp paths not available on Windows
    printf("Test skipped: POSIX temp paths not available on Windows\n");
    return 0;
#endif
    ensure_missing_open_sets_file_not_found();
    ensure_read_byte_reports_eof();
    ensure_read_line_reports_eof();
    ensure_read_line_trims_crlf();
    ensure_invalid_handle_surfaces_ioerror();
    ensure_zero_length_write_validates_handle();
    ensure_seek_out_of_range_reports_invalid_operation();
    ensure_close_failure_consumes_descriptor();
    return 0;
}
