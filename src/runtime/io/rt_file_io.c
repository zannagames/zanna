//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/io/rt_file_io.c
// Purpose: Provides the low-level file I/O layer used by the BASIC runtime.
//          Manages descriptor lifetimes, translates errno codes into the
//          runtime's structured RtError diagnostics, and implements line reads
//          and binary writes for both POSIX and Windows platforms.
//
// Key invariants:
//   - All RtFile handles are fully initialised before being returned to callers.
//   - errno is always translated into an Err_* enumerator; raw errno never escapes.
//   - Line reads handle CR, LF, and CRLF endings consistently across platforms.
//   - Close consumes the descriptor before calling the host, preventing stale-descriptor reuse.
//   - POSIX APIs are used on Unix/macOS; MSVC CRT equivalents on Windows.
//
// Ownership/Lifetime:
//   - RtFile handles are owned by the channel table managed in rt_file.c.
//   - Callers must not retain raw file descriptors beyond the owning RtFile.
//
// Links: src/runtime/io/rt_file.h (public declarations, channel table, and RtFile type),
//        src/runtime/io/rt_file_path.h (mode string helpers)
//
//===----------------------------------------------------------------------===//

#include "rt_file.h"

#include "rt_file_path.h"
#include "rt_internal.h"
#include "rt_platform.h"
#include "rt_string_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef EOVERFLOW
#define EOVERFLOW ERANGE
#endif

#if RT_PLATFORM_WINDOWS
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <wchar.h>
// Windows doesn't define ssize_t, so we provide it
#if !defined(ssize_t)
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#endif
// Map POSIX names to Windows CRT equivalents
#define open _open
#define close _close
#define read _read
#define write _write
#define lseek _lseeki64
#ifndef O_NOINHERIT
#define O_NOINHERIT _O_NOINHERIT
#endif
// Windows file permission flags
#define S_IRUSR _S_IREAD
#define S_IWUSR _S_IWRITE
#define S_IRGRP 0
#define S_IWGRP 0
#define S_IROTH 0
#define S_IWOTH 0
// Windows doesn't have these errno values
#ifndef EISDIR
#define EISDIR ENOENT
#endif
#ifndef ENOTDIR
#define ENOTDIR ENOENT
#endif
#ifndef ENOSPC
#define ENOSPC EIO
#endif
#ifndef EFBIG
#define EFBIG EIO
#endif
#ifndef EPIPE
#define EPIPE EIO
#endif
#ifndef EAGAIN
#define EAGAIN EIO
#endif
// mode_t for Windows (off_t is defined in sys/types.h as _off_t)
typedef unsigned short mode_t;
#else
// POSIX systems
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#define RT_FILE_MAX_LINE_BYTES (16u * 1024u * 1024u)

/// @brief Clamp errno values into the 32-bit range stored by @ref RtError.
/// @details Ensures large positive or negative errno values fit into the
///          runtime's signed 32-bit field without overflow.
/// @param err Errno value produced by the OS, promoted to a wide type.
/// @return Clamped errno suitable for storage in @ref RtError::code.
static int32_t rt_file_clamp_errno(long err) {
    if (err > INT32_MAX)
        return INT32_MAX;
    if (err < INT32_MIN)
        return INT32_MIN;
    return (int32_t)err;
}

/// @brief Map raw errno codes to runtime error kinds.
/// @details Converts common errno values into more specific runtime error
///          enumerations while falling back to @p fallback when no specialised
///          mapping exists.
/// @param err Errno value.
/// @param fallback Error kind to use when no mapping exists.
/// @return Runtime error kind describing the failure.
static enum Err rt_file_err_from_errno(int err, enum Err fallback) {
    if (err == 0)
        return fallback;
    switch (err) {
        case ENOENT:
            return Err_FileNotFound;
        case EINVAL:
        case EISDIR:
        case ENOTDIR:
            // Operation invalid for current state or object kind
            return Err_InvalidOperation;
        case ENOSPC: // No space left on device
        case EFBIG:  // File too large
        case EIO:
        case EPIPE:
        case EAGAIN:
        case EACCES:
        case EPERM:
        case EBADF:
#ifdef EMFILE
        case EMFILE: // Per-process file descriptor limit
#endif
#ifdef ENFILE
        case ENFILE: // System-wide file descriptor limit
#endif
            // Permission, transient, or device/storage I/O errors
            return Err_IOError;
        default:
            return fallback;
    }
}

/// @brief Populate an @ref RtError structure with an error code.
/// @details Safely handles null output pointers and clamps errno before storage.
/// @param out_err Destination error pointer.
/// @param kind Runtime error classification.
/// @param err Errno value associated with the failure.
static void rt_file_set_error(RtError *out_err, enum Err kind, int err) {
    if (!out_err)
        return;
    out_err->kind = kind;
    out_err->code = rt_file_clamp_errno(err);
}

/// @brief Reset an error structure to the success sentinel.
/// @details Writes @ref RT_ERROR_NONE when the output pointer is non-null.
/// @param out_err Destination error pointer.
static void rt_file_set_ok(RtError *out_err) {
    if (out_err)
        *out_err = RT_ERROR_NONE;
}

/// @brief Determine whether an `int64_t` offset fits within the host's @ref off_t range.
/// @details Uses POSIX-provided limits when available and falls back to width-based
///          calculations otherwise.  The helper avoids undefined behaviour from
///          narrowing casts by performing bounds checks prior to conversion.
/// @param offset Proposed byte offset for @ref lseek.
/// @return `true` when @p offset is representable as @ref off_t; otherwise `false`.
static bool rt_file_offset_in_range(int64_t offset) {
#if RT_PLATFORM_WINDOWS
    // The adapter calls _lseeki64 directly. Windows' off_t remains 32-bit even
    // in 64-bit builds, so using its range here would incorrectly reject every
    // valid sparse/large-file offset above 2 GiB.
    (void)offset;
    return true;
#elif defined(OFF_MAX) && defined(OFF_MIN)
    if (sizeof(off_t) > sizeof(int64_t))
        return true;
    if (offset < (int64_t)OFF_MIN || offset > (int64_t)OFF_MAX)
        return false;
    return true;
#else
    const int bits = (int)(sizeof(off_t) * CHAR_BIT);
    const int int64_bits = (int)(sizeof(int64_t) * CHAR_BIT);
    if (bits >= int64_bits)
        return true;
    const int shift = bits - 1;
    if (shift <= 0)
        return offset == 0;
    const int64_t M = (INT64_MAX >> (63 - shift));
    const int64_t max = M;
    const int64_t min = -M - 1;
    return offset >= min && offset <= max;
#endif
}

/// @brief Validate that a file handle contains an open descriptor.
/// @details Ensures @p file is non-null and the descriptor is valid, populating
///          @p out_err on failure.
/// @param file File handle to inspect.
/// @param out_err Optional error destination.
/// @return `true` when the descriptor is usable; otherwise `false`.
static bool rt_file_check_fd(const RtFile *file, RtError *out_err) {
    if (!file) {
        rt_file_set_error(out_err, Err_InvalidOperation, 0);
        return false;
    }
    if (file->fd < 0) {
        rt_file_set_error(out_err, Err_IOError, EBADF);
        return false;
    }
    return true;
}

/// @brief Double a dynamically-allocated line buffer while checking overflow.
/// @details Reallocates the caller-owned buffer, ensuring the new capacity
///          accommodates the requested length and reporting structured errors on
///          failure.  The helper releases the old buffer on error to avoid
///          leaks.
/// @param buffer Pointer to the caller's heap buffer pointer.
/// @param cap Pointer to the current capacity in bytes.
/// @param len Number of bytes currently stored in the buffer.
/// @param out_err Receives error details when resizing fails.
/// @return `true` when the buffer was grown successfully; otherwise `false`.
static bool rt_file_line_buffer_grow(char **buffer, size_t *cap, size_t len, RtError *out_err) {
    if (!buffer || !*buffer || !cap) {
        rt_file_set_error(out_err, Err_InvalidOperation, 0);
        return false;
    }

    if (len == SIZE_MAX) {
        free(*buffer);
        *buffer = NULL;
        rt_file_set_error(out_err, Err_RuntimeError, ERANGE);
        return false;
    }

    if (*cap > SIZE_MAX / 2) {
        free(*buffer);
        *buffer = NULL;
        rt_file_set_error(out_err, Err_RuntimeError, ERANGE);
        return false;
    }

    size_t new_cap = (*cap) * 2;
    if (new_cap <= len) {
        free(*buffer);
        *buffer = NULL;
        rt_file_set_error(out_err, Err_RuntimeError, ERANGE);
        return false;
    }

    char *nbuf = (char *)realloc(*buffer, new_cap);
    if (!nbuf) {
        free(*buffer);
        *buffer = NULL;
        rt_file_set_error(out_err, Err_RuntimeError, ENOMEM);
        return false;
    }

    *buffer = nbuf;
    *cap = new_cap;
    return true;
}

/// @brief Append a byte span to the in-progress line buffer.
/// @details Enforces @ref RT_FILE_MAX_LINE_BYTES, grows the buffer until the
///          requested span plus a trailing terminator fits, and copies the
///          bytes into place. Used by both the seekable chunked path and the
///          byte-at-a-time fallback.
/// @param buffer Pointer to the line buffer pointer.
/// @param cap Current buffer capacity.
/// @param len Current logical byte length; updated on success.
/// @param data Bytes to append.
/// @param data_len Number of bytes in @p data.
/// @param out_err Receives structured error details on failure.
/// @return `true` when the bytes were appended; otherwise `false`.
static bool rt_file_line_append(
    char **buffer, size_t *cap, size_t *len, const char *data, size_t data_len, RtError *out_err) {
    if (data_len == 0)
        return true;
    if (!buffer || !*buffer || !cap || !len || !data) {
        rt_file_set_error(out_err, Err_InvalidOperation, 0);
        return false;
    }
    if (data_len > RT_FILE_MAX_LINE_BYTES || *len > RT_FILE_MAX_LINE_BYTES - data_len) {
        rt_file_set_error(out_err, Err_RuntimeError, EOVERFLOW);
        return false;
    }
    if (data_len > SIZE_MAX - *len - 1) {
        rt_file_set_error(out_err, Err_RuntimeError, ERANGE);
        return false;
    }
    size_t needed = *len + data_len + 1;
    while (needed > *cap) {
        if (!rt_file_line_buffer_grow(buffer, cap, *len, out_err))
            return false;
    }
    memcpy(*buffer + *len, data, data_len);
    *len += data_len;
    return true;
}

/// @brief Test hook exposing the line buffer growth guard for regression coverage.
/// @param buffer Buffer pointer owned by the caller.
/// @param cap Current capacity in bytes.
/// @param len Logical payload length stored in @p buffer.
/// @param out_err Receives error details when growth fails.
/// @return True on successful growth; false when allocation or overflow prevents resizing.
bool rt_file_line_buffer_try_grow_for_test(char **buffer,
                                           size_t *cap,
                                           size_t len,
                                           RtError *out_err) {
    return rt_file_line_buffer_grow(buffer, cap, len, out_err);
}

/// @brief Initialise a file handle to the closed state.
/// @details Sets the descriptor sentinel to -1 so subsequent operations can
///          detect whether the handle has been opened.
/// @param file Handle to initialise.
void rt_file_init(RtFile *file) {
    if (!file)
        return;
    file->fd = -1;
}

/// @brief Open a file using BASIC runtime semantics.
/// @details Validates arguments, translates the textual mode into POSIX flags,
///          applies owner-only default permissions for newly created files, and records structured
///          errors when the system call fails.  On success the descriptor is
///          stored in @p file.
/// @param file Handle receiving the descriptor.
/// @param path File path to open.
/// @param mode BASIC mode string (e.g., "r", "w", "a").
/// @param basic_mode BASIC OPEN mode enumerator used to refine binary and access flags, or
///                   `RT_F_UNSPECIFIED` when @p mode alone defines the operation.
/// @param out_err Optional error destination.
/// @return `true` on success; otherwise `false` with @p out_err populated.
int8_t rt_file_open(
    RtFile *file, const char *path, const char *mode, int32_t basic_mode, RtError *out_err) {
    if (!file || !path || !mode) {
        rt_file_set_error(out_err, Err_InvalidOperation, 0);
        return 0;
    }
    if (file->fd >= 0) {
        rt_file_set_error(out_err, Err_InvalidOperation, 0);
        return 0;
    }

    int flags = 0;
    if (!rt_file_mode_to_flags(mode, basic_mode, &flags)) {
        file->fd = -1;
        rt_file_set_error(out_err, Err_InvalidOperation, 0);
        return 0;
    }

    mode_t perms = S_IRUSR | S_IWUSR;
    errno = 0;
#if RT_PLATFORM_WINDOWS
    flags |= O_NOINHERIT;
    wchar_t *wide_path = rt_file_path_utf8_to_wide(path);
    if (!wide_path) {
        rt_file_set_error(out_err, Err_InvalidOperation, 0);
        file->fd = -1;
        return 0;
    }
    int fd = (flags & O_CREAT) ? _wopen(wide_path, flags, perms) : _wopen(wide_path, flags);
    free(wide_path);
#else
    int fd = (flags & O_CREAT) ? open(path, flags, perms) : open(path, flags);
#if defined(FD_CLOEXEC) && !defined(O_CLOEXEC)
    if (fd >= 0) {
        int fd_flags = fcntl(fd, F_GETFD);
        if (fd_flags >= 0)
            (void)fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);
    }
#endif
#endif
    if (fd < 0) {
        int err = errno ? errno : EIO;
        rt_file_set_error(out_err, rt_file_err_from_errno(err, Err_IOError), err);
        file->fd = -1;
        return 0;
    }

    file->fd = fd;
    rt_file_set_ok(out_err);
    return 1;
}

/// @brief Close an open runtime file handle.
/// @details Treats already-closed handles as success, otherwise closes the
///          descriptor and reports any system errors through @p out_err. The
///          descriptor is invalidated before the host close call because a close
///          failure does not safely grant the caller continued ownership: the
///          numeric descriptor may already be eligible for reuse by another open.
/// @param file Handle to close.
/// @param out_err Optional error destination.
/// @return `true` when the host close succeeds or the handle was already closed;
///         `false` when the host reports an error. In every case @p file is closed
///         from the runtime's perspective when this function returns.
int8_t rt_file_close(RtFile *file, RtError *out_err) {
    if (!file) {
        rt_file_set_error(out_err, Err_InvalidOperation, 0);
        return 0;
    }
    if (file->fd < 0) {
        rt_file_set_ok(out_err);
        return 1;
    }

    int fd = file->fd;
    file->fd = -1;
    errno = 0;
    int rc = close(fd);
    if (rc < 0) {
        int err = errno ? errno : EIO;
        rt_file_set_error(out_err, rt_file_err_from_errno(err, Err_IOError), err);
        return 0;
    }

    rt_file_set_ok(out_err);
    return 1;
}

/// @brief Read a single byte from a file descriptor.
/// @details Retries on EINTR, reports EOF via @ref Err_EOF, and surfaces other
///          failures through @p out_err.  The byte is written to @p out_byte on
///          success.
/// @param file File handle to read from.
/// @param out_byte Destination for the read byte.
/// @param out_err Optional error destination.
/// @return `true` when a byte was read; `false` on EOF or error.
int8_t rt_file_read_byte(RtFile *file, uint8_t *out_byte, RtError *out_err) {
    if (!out_byte) {
        rt_file_set_error(out_err, Err_InvalidOperation, 0);
        return 0;
    }
    if (!rt_file_check_fd(file, out_err))
        return 0;

    for (;;) {
        uint8_t byte = 0;
        errno = 0;
        ssize_t n = read(file->fd, &byte, 1);
        if (n == 1) {
            *out_byte = byte;
            rt_file_set_ok(out_err);
            return 1;
        }
        if (n == 0) {
            rt_file_set_error(out_err, Err_EOF, 0);
            return 0;
        }
        if (n < 0 && errno == EINTR)
            continue;

        int err = errno ? errno : EIO;
        rt_file_set_error(out_err, rt_file_err_from_errno(err, Err_IOError), err);
        return 0;
    }
}

/// @brief Read a line of text terminated by `\n` from a file descriptor.
/// @details Allocates a growable buffer, strips trailing carriage returns,
///          handles EOF, and wraps the result in a runtime string object.
///          Structured errors are produced when allocation fails or the
///          descriptor encounters an I/O error.
/// @param file File handle to read from.
/// @param out_line Receives the allocated runtime string on success.
/// @param out_err Optional error destination.
/// @return `true` on success; `false` on EOF or error.
int8_t rt_file_read_line(RtFile *file, rt_string *out_line, RtError *out_err) {
    if (out_line)
        *out_line = NULL;
    if (!out_line) {
        rt_file_set_error(out_err, Err_InvalidOperation, 0);
        return 0;
    }
    if (!rt_file_check_fd(file, out_err))
        return 0;

    size_t cap = 128;
    size_t len = 0;
    char *buffer = (char *)malloc(cap);
    if (!buffer) {
        rt_file_set_error(out_err, Err_RuntimeError, ENOMEM);
        return false;
    }

    bool ok = false;
    rt_string s = NULL;

    int64_t seek_pos = lseek(file->fd, 0, SEEK_CUR);
    if (seek_pos >= 0) {
        for (;;) {
            char chunk[4096];
            errno = 0;
            ssize_t n = read(file->fd, chunk, sizeof(chunk));
            if (n > 0) {
                size_t bytes_read = (size_t)n;
                size_t append_len = bytes_read;
                int found_newline = 0;
                for (size_t i = 0; i < bytes_read; ++i) {
                    if (chunk[i] == '\n') {
                        append_len = i;
                        found_newline = 1;
                        break;
                    }
                }
                if (!rt_file_line_append(&buffer, &cap, &len, chunk, append_len, out_err))
                    goto cleanup;
                if (found_newline) {
                    size_t consumed = append_len + 1;
                    if (seek_pos > INT64_MAX - (int64_t)consumed) {
                        rt_file_set_error(out_err, Err_RuntimeError, EOVERFLOW);
                        goto cleanup;
                    }
                    int64_t target = seek_pos + (int64_t)consumed;
                    errno = 0;
                    if (lseek(file->fd, target, SEEK_SET) < 0) {
                        int err = errno ? errno : EIO;
                        rt_file_set_error(out_err, rt_file_err_from_errno(err, Err_IOError), err);
                        goto cleanup;
                    }
                    break;
                }
                if (seek_pos > INT64_MAX - (int64_t)bytes_read) {
                    rt_file_set_error(out_err, Err_RuntimeError, EOVERFLOW);
                    goto cleanup;
                }
                seek_pos += (int64_t)bytes_read;
                continue;
            }
            if (n == 0) {
                if (len == 0) {
                    rt_file_set_error(out_err, Err_EOF, 0);
                    goto cleanup;
                }
                break;
            }
            if (n < 0 && errno == EINTR)
                continue;

            int err = errno ? errno : EIO;
            rt_file_set_error(out_err, rt_file_err_from_errno(err, Err_IOError), err);
            goto cleanup;
        }
    } else {
        for (;;) {
            char ch = 0;
            errno = 0;
            ssize_t n = read(file->fd, &ch, 1);
            if (n == 1) {
                if (ch == '\n')
                    break;
                if (!rt_file_line_append(&buffer, &cap, &len, &ch, 1, out_err))
                    goto cleanup;
                continue;
            }
            if (n == 0) {
                if (len == 0) {
                    rt_file_set_error(out_err, Err_EOF, 0);
                    goto cleanup;
                }
                break;
            }
            if (n < 0 && errno == EINTR)
                continue;

            int err = errno ? errno : EIO;
            rt_file_set_error(out_err, rt_file_err_from_errno(err, Err_IOError), err);
            goto cleanup;
        }
    }

    if (len > 0 && buffer[len - 1] == '\r')
        --len;

    if (len + 1 > cap) {
        char *nbuf = (char *)realloc(buffer, len + 1);
        if (!nbuf) {
            rt_file_set_error(out_err, Err_RuntimeError, ENOMEM);
            goto cleanup;
        }
        buffer = nbuf;
    }
    buffer[len] = '\0';

    s = rt_string_from_bytes(buffer, len);
    if (!s) {
        rt_file_set_error(out_err, Err_RuntimeError, ENOMEM);
        goto cleanup;
    }

    *out_line = s;
    rt_file_set_ok(out_err);
    ok = true;

cleanup:
    if (!ok) {
        if (s) {
            rt_string_unref(s);
            s = NULL;
        }
    }
    if (buffer) {
        free(buffer);
        buffer = NULL;
    }
    return ok;
}

/// @brief Adjust the file position indicator.
/// @details Wraps @ref lseek, converting offsets to `off_t` and reporting
///          failures through structured errors.
/// @param file File handle.
/// @param offset Byte offset relative to @p origin.
/// @param origin One of `SEEK_SET`, `SEEK_CUR`, or `SEEK_END`.
/// @param out_err Optional error destination.
/// @return `true` on success; otherwise `false`.
int8_t rt_file_seek(RtFile *file, int64_t offset, int origin, RtError *out_err) {
    if (!rt_file_check_fd(file, out_err))
        return 0;

    if (!rt_file_offset_in_range(offset)) {
        rt_file_set_error(out_err, Err_InvalidOperation, ERANGE);
        return 0;
    }

    errno = 0;
    int64_t target = offset;
    int64_t pos = lseek(file->fd, target, origin);
    if (pos == -1) {
        int err = errno ? errno : EIO;
        rt_file_set_error(out_err, rt_file_err_from_errno(err, Err_IOError), err);
        return 0;
    }

    rt_file_set_ok(out_err);
    return 1;
}

/// @brief Write a byte buffer to a file descriptor, retrying short writes.
/// @details Handles zero-length writes as success, validates the input pointer,
///          and loops until all bytes are written or an error occurs.  EINTR is
///          retried automatically.
/// @param file File handle to write to.
/// @param data Buffer containing bytes to write.
/// @param len Number of bytes to write.
/// @param out_err Optional error destination.
/// @return `true` when all bytes are written; otherwise `false`.
int8_t rt_file_write(RtFile *file, const uint8_t *data, size_t len, RtError *out_err) {
    if (len == 0) {
        rt_file_set_ok(out_err);
        return 1;
    }
    if (!data) {
        rt_file_set_error(out_err, Err_InvalidOperation, 0);
        return 0;
    }
    if (!rt_file_check_fd(file, out_err))
        return 0;

    size_t written = 0;
    while (written < len) {
        errno = 0;
        size_t chunk = len - written;
#if RT_PLATFORM_WINDOWS
        // Windows _write takes unsigned int, clamp to avoid overflow
        if (chunk > UINT_MAX)
            chunk = UINT_MAX;
#endif
#if RT_PLATFORM_WINDOWS
        ssize_t n = write(file->fd, data + written, (unsigned int)chunk);
#else
        ssize_t n = write(file->fd, data + written, chunk);
#endif
        if (n < 0) {
            if (errno == EINTR)
                continue;
            int err = errno ? errno : EIO;
            rt_file_set_error(out_err, rt_file_err_from_errno(err, Err_IOError), err);
            return 0;
        }
        if (n == 0) {
            rt_file_set_error(out_err, Err_IOError, EIO);
            return 0;
        }
        written += (size_t)n;
    }

    rt_file_set_ok(out_err);
    return 1;
}
