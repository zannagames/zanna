//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/io/rt_file_stdio.h
// Purpose: Open UTF-8 paths as non-inheritable stdio streams, durably close
//          them, and atomically commit adjacent temporary replacements.
//
// Key invariants:
//   - Supported stdio modes are the exact binary spellings rb, wb, ab, r+b/rb+, w+b/wb+, and
//     a+b/ab+.
//   - Every opened descriptor is marked non-inheritable through the native creation flag or
//     close-on-exec fallback.
//   - Windows descriptors use explicit sharing policy: normal opens deny writers and replacement
//     sidecars deny every concurrent open.
//   - Replacement sidecars use exclusive creation in the destination's directory, and commit
//     uses the platform's atomic replace primitive.
//   - Durable close attempts flush, sync, and close even after an earlier failure and preserves
//     the first error.
//   - All filesystem paths are UTF-8; Windows calls strict UTF-8/wide conversion helpers.
//
// Ownership/Lifetime:
//   - Successful open helpers return FILE streams that callers must close.
//   - Temporary-path construction returns malloc-owned storage that callers free after commit or
//     cleanup.
//   - Path and mode inputs are borrowed only for the duration of each call.
//   - No native descriptor remains owned after a failed stdio conversion.
//
// Links: src/runtime/io/rt_file_path.h (Windows path conversion),
//        src/runtime/io/rt_file_ext.c (higher-level atomic file operations),
//        src/runtime/graphics/2d/rt_pixels_io.c,
//        src/runtime/graphics/3d/scene/rt_scene3d_vscn_save.c
// Cross-platform touchpoints: Windows uses wide CRT paths and `_commit`;
//                             POSIX uses close-on-exec descriptors and `fsync`.
//
//===----------------------------------------------------------------------===//
/**
 * @file
 * @brief Defines shared inline UTF-8 stdio and atomic-replacement helpers.
 * @details Adapts supported binary modes to non-inheritable native
 * descriptors, constructs and exclusively opens adjacent sidecars, flushes
 * file data, commits replacements atomically, and supplies UTF-8 deletion and
 * process-identification utilities.
 */
#pragma once

#include "rt_file_path.h"
#include "rt_platform.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if RT_PLATFORM_WINDOWS
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <share.h>
#include <sys/stat.h>
#include <wchar.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#ifndef O_RDONLY
#define O_RDONLY _O_RDONLY
#endif
#ifndef O_WRONLY
#define O_WRONLY _O_WRONLY
#endif
#ifndef O_RDWR
#define O_RDWR _O_RDWR
#endif
#ifndef O_CREAT
#define O_CREAT _O_CREAT
#endif
#ifndef O_TRUNC
#define O_TRUNC _O_TRUNC
#endif
#ifndef O_EXCL
#define O_EXCL _O_EXCL
#endif
#ifndef O_APPEND
#define O_APPEND _O_APPEND
#endif
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#if RT_PLATFORM_WINDOWS
/// @brief Map a captured Win32 filesystem failure to deterministic CRT errno.
/// @param error Error returned by GetLastError immediately after a failed call.
/// @return Closest portable errno value used by runtime file APIs.
static inline int rt_file_stdio_win32_error_to_errno(DWORD error) {
    switch (error) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            return ENOENT;
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
            return EACCES;
        case ERROR_ALREADY_EXISTS:
        case ERROR_FILE_EXISTS:
            return EEXIST;
        case ERROR_DISK_FULL:
        case ERROR_HANDLE_DISK_FULL:
            return ENOSPC;
        case ERROR_NOT_SAME_DEVICE:
            return EXDEV;
        case ERROR_FILENAME_EXCED_RANGE:
            return ENAMETOOLONG;
        case ERROR_INVALID_NAME:
        case ERROR_INVALID_PARAMETER:
            return EINVAL;
        case ERROR_WRITE_PROTECT:
            return EROFS;
        default:
            return EIO;
    }
}
#endif

/// @brief Return a small process-local identifier for temporary file names.
/// @details The value is not used for security. It only reduces collision
///          probability before the following exclusive-create open enforces
///          uniqueness. Windows uses `_getpid`; POSIX uses `getpid`.
/// @return Current process id truncated to an unsigned long.
static inline unsigned long rt_file_stdio_process_id(void) {
#if RT_PLATFORM_WINDOWS
    return (unsigned long)_getpid();
#else
    return (unsigned long)getpid();
#endif
}

/// @brief Delete a file at a UTF-8 path.
/// @details Windows converts @p path to a wide path and calls `_wunlink`.
///          POSIX calls `unlink`. The function does not treat missing files
///          specially; callers can inspect `errno` or the Windows CRT errno
///          mapping when a non-zero result is returned.
/// @param path UTF-8 path to delete.
/// @return 0 on success, non-zero on invalid input or deletion failure.
static inline int rt_file_stdio_unlink_utf8(const char *path) {
    if (!path) {
        errno = EINVAL;
        return -1;
    }
#if RT_PLATFORM_WINDOWS
    wchar_t *wide_path = rt_file_path_utf8_to_wide(path);
    if (!wide_path) {
        errno = EINVAL;
        return -1;
    }
    int rc = _wunlink(wide_path);
    free(wide_path);
    return rc;
#else
    return unlink(path);
#endif
}

/// @brief Atomically replace a UTF-8 destination path with a UTF-8 source path.
/// @details Windows uses `MoveFileExW` with `MOVEFILE_REPLACE_EXISTING` and
///          `MOVEFILE_WRITE_THROUGH`. POSIX uses `rename`, which atomically
///          replaces an existing destination when both paths are on the same
///          filesystem. The temporary source should be created in the same
///          directory as the destination when atomicity is required.
/// @param src_utf8 Existing source path.
/// @param dst_utf8 Destination path to replace.
/// @return 1 on success, 0 on invalid input or replace failure.
static inline int rt_file_stdio_replace_utf8(const char *src_utf8, const char *dst_utf8) {
    if (!src_utf8 || !dst_utf8) {
        errno = EINVAL;
        return 0;
    }
#if RT_PLATFORM_WINDOWS
    wchar_t *wide_src = rt_file_path_utf8_to_wide(src_utf8);
    wchar_t *wide_dst = rt_file_path_utf8_to_wide(dst_utf8);
    if (!wide_src || !wide_dst) {
        free(wide_src);
        free(wide_dst);
        errno = EINVAL;
        return 0;
    }
    int ok =
        MoveFileExW(wide_src, wide_dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    DWORD replace_error = ok ? ERROR_SUCCESS : GetLastError();
    free(wide_src);
    free(wide_dst);
    if (!ok)
        errno = rt_file_stdio_win32_error_to_errno(replace_error);
    return ok ? 1 : 0;
#else
    return rename(src_utf8, dst_utf8) == 0 ? 1 : 0;
#endif
}

/// @brief Build a same-directory temporary path for replacing @p dst_path.
/// @details The returned path appends `.tmp.<pid>.<attempt>` to the full
///          destination path. It is intended to be opened with exclusive-create
///          semantics by rt_file_stdio_open_temp_for_replace_utf8, so collisions
///          are harmless and cause another candidate to be tried.
/// @param dst_path Destination path that will later be replaced.
/// @param attempt Candidate number used to vary the suffix.
/// @return Malloc-owned temporary path, or NULL on invalid input, overflow, or allocation failure.
static inline char *rt_file_stdio_temp_replace_path_utf8(const char *dst_path, unsigned attempt) {
    if (!dst_path || !*dst_path) {
        errno = EINVAL;
        return NULL;
    }
    char suffix[64];
    int suffix_len =
        snprintf(suffix, sizeof(suffix), ".tmp.%lu.%u", rt_file_stdio_process_id(), attempt);
    if (suffix_len <= 0 || (size_t)suffix_len >= sizeof(suffix)) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    size_t dst_len = strlen(dst_path);
    if (dst_len > SIZE_MAX - (size_t)suffix_len - 1u) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    char *tmp = (char *)malloc(dst_len + (size_t)suffix_len + 1u);
    if (!tmp)
        return NULL;
    memcpy(tmp, dst_path, dst_len);
    memcpy(tmp + dst_len, suffix, (size_t)suffix_len + 1u);
    return tmp;
}

/// @brief Open a same-directory temporary file for atomically replacing a UTF-8 destination path.
/// @details Tries a bounded sequence of `.tmp.<pid>.<attempt>` candidate names
///          beside @p dst_path, opening each with exclusive-create semantics so
///          an existing file is never truncated. The returned FILE* is opened
///          in binary write mode and must be closed by the caller before
///          committing with rt_file_stdio_replace_utf8. On success @p out_tmp_path
///          receives a malloc-owned path that the caller must free. On failure
///          it is set to NULL.
/// @param dst_path Destination path that will eventually be replaced.
/// @param out_tmp_path Receives the malloc-owned temporary path.
/// @return Open FILE* on success; NULL on invalid input, collisions after all attempts, or I/O
/// error.
static inline FILE *rt_file_stdio_open_temp_for_replace_utf8(const char *dst_path,
                                                             char **out_tmp_path) {
    if (out_tmp_path)
        *out_tmp_path = NULL;
    if (!dst_path || !out_tmp_path) {
        errno = EINVAL;
        return NULL;
    }

    for (unsigned attempt = 0; attempt < 256u; ++attempt) {
        char *tmp_path = rt_file_stdio_temp_replace_path_utf8(dst_path, attempt);
        if (!tmp_path)
            return NULL;
        int flags = O_WRONLY | O_CREAT | O_EXCL | O_TRUNC;
#if defined(O_BINARY)
        flags |= O_BINARY;
#elif defined(_O_BINARY)
        flags |= _O_BINARY;
#endif
#if RT_PLATFORM_WINDOWS
        flags |= _O_NOINHERIT;
        wchar_t *wide_path = rt_file_path_utf8_to_wide(tmp_path);
        if (!wide_path) {
            free(tmp_path);
            errno = EINVAL;
            return NULL;
        }
        int fd = -1;
        errno_t open_error = _wsopen_s(&fd, wide_path, flags, _SH_DENYRW, _S_IREAD | _S_IWRITE);
        free(wide_path);
        if (open_error != 0) {
            errno = (int)open_error;
            fd = -1;
        }
#else
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
        int fd = open(tmp_path, flags, (mode_t)0666);
#endif
        if (fd < 0) {
            if (errno == EEXIST) {
                free(tmp_path);
                continue;
            }
            free(tmp_path);
            return NULL;
        }
#if !RT_PLATFORM_WINDOWS && defined(FD_CLOEXEC) && !defined(O_CLOEXEC)
        int fd_flags = fcntl(fd, F_GETFD);
        if (fd_flags >= 0)
            (void)fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);
#endif
#if RT_PLATFORM_WINDOWS
        FILE *fp = _fdopen(fd, "wb");
#else
        FILE *fp = fdopen(fd, "wb");
#endif
        if (!fp) {
            int fdopen_error = errno ? errno : EIO;
#if RT_PLATFORM_WINDOWS
            (void)_close(fd);
#else
            (void)close(fd);
#endif
            (void)rt_file_stdio_unlink_utf8(tmp_path);
            free(tmp_path);
            errno = fdopen_error;
            return NULL;
        }
        *out_tmp_path = tmp_path;
        return fp;
    }
    errno = EEXIST;
    return NULL;
}

/// @brief Translate a supported binary stdio mode into descriptor-open flags.
/// @details Accepts read, write, append, and update variants in the exact spellings used by
///          `fdopen`. Adds the platform binary and non-inheritance flags and selects creation
///          permissions appropriate to POSIX or the Windows CRT. Output values are written only
///          after the complete mode has been validated.
/// @param mode Null-terminated binary stdio mode string.
/// @param[out] out_flags Required destination for `open`/`_wopen` flags.
/// @param[out] out_pmode Required destination for creation permissions.
/// @return 1 when @p mode is supported and both outputs were populated; otherwise 0.
static inline int rt_file_stdio_mode_flags(const char *mode, int *out_flags, int *out_pmode) {
    if (!mode || !out_flags || !out_pmode)
        return 0;

    int flags = 0;
    int pmode = 0666;
    if (strcmp(mode, "rb") == 0) {
        flags = O_RDONLY;
    } else if (strcmp(mode, "wb") == 0) {
        flags = O_WRONLY | O_CREAT | O_TRUNC;
    } else if (strcmp(mode, "ab") == 0) {
        flags = O_WRONLY | O_CREAT | O_APPEND;
    } else if (strcmp(mode, "r+b") == 0 || strcmp(mode, "rb+") == 0) {
        flags = O_RDWR;
    } else if (strcmp(mode, "w+b") == 0 || strcmp(mode, "wb+") == 0) {
        flags = O_RDWR | O_CREAT | O_TRUNC;
    } else if (strcmp(mode, "a+b") == 0 || strcmp(mode, "ab+") == 0) {
        flags = O_RDWR | O_CREAT | O_APPEND;
    } else {
        return 0;
    }

#if defined(O_BINARY)
    flags |= O_BINARY;
#elif defined(_O_BINARY)
    flags |= _O_BINARY;
#endif
#if RT_PLATFORM_WINDOWS
    flags |= _O_NOINHERIT;
    pmode = _S_IREAD | _S_IWRITE;
#endif

    *out_flags = flags;
    *out_pmode = pmode;
    return 1;
}

/// @brief Open a UTF-8 path as a non-inheritable binary stdio stream.
/// @details Converts the path to UTF-16 on Windows, opens a descriptor with flags from
///          rt_file_stdio_mode_flags(), and wraps it with `fdopen`. If stream construction fails,
///          the descriptor is closed before returning. Invalid input sets `errno` to `EINVAL`;
///          other failures preserve the native open/stdio error.
/// @param path Null-terminated UTF-8 filesystem path.
/// @param mode Supported binary stdio mode string.
/// @return Caller-owned open FILE stream, or NULL on validation, conversion, open, or wrapping
///         failure.
static inline FILE *rt_file_stdio_open_utf8(const char *path, const char *mode) {
    if (!path || !mode) {
        errno = EINVAL;
        return NULL;
    }

    int flags = 0;
    int pmode = 0;
    if (!rt_file_stdio_mode_flags(mode, &flags, &pmode)) {
        errno = EINVAL;
        return NULL;
    }

#if RT_PLATFORM_WINDOWS
    wchar_t *wide_path = rt_file_path_utf8_to_wide(path);
    if (!wide_path) {
        errno = EINVAL;
        return NULL;
    }
    int fd = -1;
    int share_mode = (flags & (O_WRONLY | O_RDWR)) == 0 ? _SH_DENYWR : _SH_DENYRW;
    errno_t open_error = _wsopen_s(&fd, wide_path, flags, share_mode, pmode);
    free(wide_path);
    if (open_error != 0) {
        errno = (int)open_error;
        return NULL;
    }
    FILE *fp = _fdopen(fd, mode);
    if (!fp) {
        int fdopen_error = errno ? errno : EIO;
        (void)_close(fd);
        errno = fdopen_error;
    }
    return fp;
#else
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int fd = open(path, flags, (mode_t)pmode);
    if (fd < 0)
        return NULL;
#if defined(FD_CLOEXEC) && !defined(O_CLOEXEC)
    int fd_flags = fcntl(fd, F_GETFD);
    if (fd_flags >= 0)
        (void)fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);
#endif
    FILE *fp = fdopen(fd, mode);
    if (!fp) {
        int fdopen_error = errno ? errno : EIO;
        (void)close(fd);
        errno = fdopen_error;
    }
    return fp;
#endif
}

/// @brief Flush, synchronize, and close a writable stdio stream.
/// @details Every step is attempted even after an earlier failure. Windows
///          commits the descriptor with `_commit`; POSIX uses `fsync`. The
///          first meaningful errno is restored after close so cleanup cannot
///          hide the authoritative write or durability failure.
/// @param fp Caller-owned writable stream, consumed on every non-null call.
/// @return 1 only when flush, durable sync, and close all succeed; otherwise 0.
static inline int rt_file_stdio_flush_sync_close(FILE *fp) {
    if (!fp) {
        errno = EINVAL;
        return 0;
    }
    int first_error = 0;
#if RT_PLATFORM_WINDOWS
    int fd = _fileno(fp);
#else
    int fd = fileno(fp);
#endif
    if (fd < 0)
        first_error = errno ? errno : EBADF;
    if (fflush(fp) != 0 && first_error == 0)
        first_error = errno ? errno : EIO;
    if (fd >= 0) {
#if RT_PLATFORM_WINDOWS
        if (_commit(fd) != 0 && first_error == 0)
#else
        if (fsync(fd) != 0 && first_error == 0)
#endif
            first_error = errno ? errno : EIO;
    }
    if (fclose(fp) != 0 && first_error == 0)
        first_error = errno ? errno : EIO;
    if (first_error != 0) {
        errno = first_error;
        return 0;
    }
    return 1;
}

/// @brief Seek a stdio stream with a signed 64-bit byte offset.
/// @details Centralizes the platform-specific large-file API used by runtime
///          modules that still parse through `FILE *`. Windows uses `_fseeki64`;
///          POSIX platforms use `fseeko`. Callers pass the usual `SEEK_SET`,
///          `SEEK_CUR`, or `SEEK_END` origin and receive the underlying API's
///          zero-on-success result.
/// @param fp     Open stream to reposition.
/// @param offset Signed byte offset interpreted relative to @p origin.
/// @param origin Standard stdio seek origin.
/// @return 0 on success; non-zero on invalid input or seek failure.
static inline int rt_file_stdio_seek64(FILE *fp, int64_t offset, int origin) {
    if (!fp)
        return -1;
#if RT_PLATFORM_WINDOWS
    return _fseeki64(fp, offset, origin);
#else
    return fseeko(fp, (off_t)offset, origin);
#endif
}

/// @brief Report the current stdio stream offset as a signed 64-bit value.
/// @details Pairs with rt_file_stdio_seek64 so callers do not need to carry
///          their own `_ftelli64`/`ftello` preprocessor branches. Returns -1
///          when @p fp is NULL or when the underlying API reports failure.
/// @param fp Open stream to query.
/// @return Current byte offset from the beginning of the stream, or -1 on failure.
static inline int64_t rt_file_stdio_tell64(FILE *fp) {
    if (!fp)
        return -1;
#if RT_PLATFORM_WINDOWS
    return (int64_t)_ftelli64(fp);
#else
    return (int64_t)ftello(fp);
#endif
}
