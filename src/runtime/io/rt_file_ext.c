//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/io/rt_file_ext.c
// Purpose: High-level file helpers backing the Zanna.IO.File static methods.
//          Implements ReadAllText, WriteAllText, ReadAllBytes, WriteAllBytes,
//          ReadAllLines, AppendAllText, Copy, Move, Delete, Exists, GetSize,
//          and related operations by bridging OOP-style calls to the runtime
//          file and string utilities.
//
// Key invariants:
//   - ReadAllText/ReadAllBytes size regular files up front and read their complete contents.
//   - WriteAllText/WriteAllBytes/WriteLines replace files atomically.
//   - CompareExchangeAllText serializes cooperating processes and rejects stale snapshots.
//   - Replacing an existing regular file preserves its permission mode.
//   - Exists returns false for directories; use Dir.Exists for those.
//   - IdentityKey is opaque, alias-stable, and empty for non-regular entries.
//   - Copy does not overwrite the destination unless explicitly requested.
//   - All functions handle both POSIX and Windows file APIs transparently.
//   - Internal bytes layout is accessed directly to avoid per-byte overhead.
//
// Ownership/Lifetime:
//   - Returned Bytes and Seq values are fresh runtime-managed objects; text reads return a
//     runtime-managed string reference and may use the shared empty string.
//   - Input strings are borrowed; this module does not retain string references.
//
// Links: src/runtime/io/rt_file_ext.h (public API),
//        src/runtime/io/rt_file.h (low-level RtFile handle and channel table),
//        src/runtime/io/rt_file_path.h (mode string conversion),
//        docs/adr/0282-opaque-regular-file-identity-keys.md
//
//===----------------------------------------------------------------------===//
/**
 * @file
 * @brief Implements high-level whole-file and regular-file runtime operations.
 * @details Performs bounded complete reads, exact text and Bytes handling,
 * newline splitting and line serialization, serialized append, identity and
 * metadata queries, copy/move/delete, permission-preserving durable sidecar
 * replacement, and strict cross-platform UTF-8 path adaptation.
 */

#include "rt_bytes.h"
#include "rt_file.h"

#include "network/rt_entropy_platform.h"
#include "rt_file_path.h"
#include "rt_internal.h"
#include "rt_io_class_ids.h"
#include "rt_object.h"
#include "rt_seq.h"
#include "rt_string.h"

#include "rt_platform.h"

#if RT_PLATFORM_WINDOWS
#ifndef _CRT_RAND_S
#define _CRT_RAND_S
#endif
#endif

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#if !RT_PLATFORM_WINDOWS
#include <sys/file.h>
#include <unistd.h>
#endif

// Darwin hides flock(2) behind its extension namespace when the runtime's
// global POSIX feature level is selected. This adapter still uses the stable
// libc ABI while keeping platform detection centralized in rt_platform.h.
#if RT_PLATFORM_MACOS && !defined(LOCK_EX)
#define LOCK_EX 0x02
#define LOCK_NB 0x04
#define LOCK_UN 0x08
extern int flock(int, int);
#endif

#if RT_PLATFORM_WINDOWS
#include <io.h>
#include <process.h>
#include <sys/utime.h>
#include <wchar.h>
#include <windows.h>
#else
#include <utime.h>
#endif

#if RT_PLATFORM_WINDOWS
/** Platform metadata record with 64-bit file-size support. */
typedef struct _stat64 rt_fileext_stat_t;
/** Platform metadata query used for already-open descriptors. */
#define rt_fileext_fstat _fstat64
#else
/** Platform-native POSIX metadata record. */
typedef struct stat rt_fileext_stat_t;
/** Platform metadata query used for already-open descriptors. */
#define rt_fileext_fstat fstat
#endif

/** Binary-open flag selected from the host CRT, or zero when unnecessary. */
#if defined(O_BINARY)
#define RT_FILE_O_BINARY O_BINARY
#elif defined(_O_BINARY)
#define RT_FILE_O_BINARY _O_BINARY
#else
#define RT_FILE_O_BINARY 0
#endif

/** Conservative fallback used for fixed local path buffers when absent. */
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/// @copydoc rt_trap_set_recovery()
void rt_trap_set_recovery(jmp_buf *buf);
/// @copydoc rt_trap_clear_recovery()
void rt_trap_clear_recovery(void);
/// @copydoc rt_trap_get_error()
const char *rt_trap_get_error(void);

/// @brief Release one owned reference and destroy the object when its count reaches zero.
/// @details This cleanup helper is used on trap-recovery paths that temporarily own runtime
///          containers. A null pointer is accepted and ignored.
/// @param obj Runtime object whose owned reference should be released.
static void rt_fileext_release_object(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Managed operating-system lock held for a FileLease object's lifetime.
typedef struct rt_file_lease_impl {
    int closed; ///< Nonzero after explicit release or finalization.
#if RT_PLATFORM_WINDOWS
    HANDLE handle;      ///< Open lock-file handle owning the byte-range lock.
    OVERLAPPED overlap; ///< Stable lock/unlock range descriptor.
#else
    int fd; ///< Open descriptor owning the advisory flock.
#endif
} rt_file_lease_impl;

/// @brief Validate and unwrap a managed FileLease handle.
static rt_file_lease_impl *rt_file_lease_checked(void *handle) {
    if (!rt_obj_is_instance(handle, RT_FILE_LEASE_CLASS_ID, sizeof(rt_file_lease_impl)))
        return NULL;
    return (rt_file_lease_impl *)handle;
}

/// @brief Release native lease state without deleting the persistent marker.
static void rt_file_lease_close(rt_file_lease_impl *lease) {
    if (!lease || lease->closed)
        return;
#if RT_PLATFORM_WINDOWS
    if (lease->handle != INVALID_HANDLE_VALUE) {
        (void)UnlockFileEx(lease->handle, 0, MAXDWORD, MAXDWORD, &lease->overlap);
        (void)CloseHandle(lease->handle);
        lease->handle = INVALID_HANDLE_VALUE;
    }
#else
    if (lease->fd >= 0) {
        (void)flock(lease->fd, LOCK_UN);
        (void)close(lease->fd);
        lease->fd = -1;
    }
#endif
    lease->closed = 1;
}

/// @brief GC finalizer for an unreleased FileLease.
static void rt_file_lease_finalize(void *object) {
    rt_file_lease_close((rt_file_lease_impl *)object);
}

/// @brief Preserve the active trap message in a caller-provided buffer.
/// @details Copies the runtime's current error when it is nonempty; otherwise copies @p fallback.
///          The result is always formatted through `snprintf` and therefore null-terminated when
///          @p buffer_size is nonzero.
/// @param[out] buffer Destination for the preserved diagnostic.
/// @param buffer_size Capacity of @p buffer in bytes.
/// @param fallback Message to use when no active runtime error is available.
static void rt_fileext_save_trap_error(char *buffer, size_t buffer_size, const char *fallback) {
    const char *err = rt_trap_get_error();
    snprintf(buffer, buffer_size, "%s", err && err[0] ? err : fallback);
}

/// @brief Produce a nonce component for an atomic-write sidecar name.
/// @details Mixes @p attempt into secure platform entropy. If entropy acquisition fails, the
///          attempt number alone supplies a deterministic nonzero fallback for attempts after
///          zero; callers reject a zero result and abandon that candidate.
/// @param attempt Collision-retry index to mix into the high bits.
/// @return The generated nonce, or zero when no usable value could be produced.
static uint64_t rt_fileext_random_u64(unsigned attempt) {
    uint64_t value = 0;
    if (rt_entropy_platform_random_u64(&value) != 0)
        value = (uint64_t)attempt << 48;
    else
        value ^= (uint64_t)attempt << 48;
    return value;
}

/// @brief Build a temp-file path sidecar to `path` in the same parent directory.
/// @details Combines the parent directory prefix, `prefix`, PID, random entropy,
///          and `attempt` counter to produce a collision-resistant name. Returns a
///          heap-allocated string; caller must free. Returns NULL on alloc failure.
/// @param path Destination path whose parent directory should contain the sidecar.
/// @param prefix Filename prefix to place before the PID and nonce.
/// @param attempt Collision-retry index incorporated into the candidate name.
/// @return Newly allocated candidate path, or NULL on overflow, allocation, or nonce failure.
static char *rt_fileext_make_parent_temp_path(const char *path,
                                              const char *prefix,
                                              unsigned attempt) {
    size_t path_len = strlen(path);
    size_t parent_len = 0;
    for (size_t i = 0; i < path_len; ++i) {
#if RT_PLATFORM_WINDOWS
        if (path[i] == '/' || path[i] == '\\')
#else
        if (path[i] == '/')
#endif
            parent_len = i + 1;
    }

    char nonce[17];
    uint64_t random_value = rt_fileext_random_u64(attempt);
    if (random_value == 0)
        return NULL;
    snprintf(nonce, sizeof(nonce), "%016llx", (unsigned long long)random_value);
    size_t prefix_len = strlen(prefix);
    size_t nonce_len = strlen(nonce);
    if (parent_len > SIZE_MAX - prefix_len - nonce_len - 48)
        return NULL;
    size_t cap = parent_len + prefix_len + nonce_len + 48;
    char *tmp = (char *)malloc(cap);
    if (!tmp)
        return NULL;
    if (parent_len >= cap) {
        free(tmp);
        return NULL;
    }
    if (parent_len > 0)
        memcpy(tmp, path, parent_len);
#if RT_PLATFORM_WINDOWS
    unsigned long pid = (unsigned long)_getpid();
#else
    unsigned long pid = (unsigned long)getpid();
#endif
    int written =
        snprintf(tmp + parent_len, cap - parent_len, "%s%lu.%s.%u", prefix, pid, nonce, attempt);
    if (written < 0 || (size_t)written >= cap - parent_len) {
        free(tmp);
        return NULL;
    }
    return tmp;
}

// =============================================================================
// Platform shims
// Each cross-platform helper wraps the OS-specific primitive with a uniform
// signature: open / stat / unlink / utime work on UTF-8 paths regardless of
// platform (Win32 detours through `rt_file_path_utf8_to_wide` so emoji in
// filenames work). All functions below use these shims rather than calling
// open/stat/etc. directly. The two #if/#else blocks deliberately duplicate
// rt_fileext_write_all_fd / _make_temp_path / _replace_utf8 — Win32 uses
// MoveFileExW for atomic-replace; POSIX uses rename(2) which is already atomic.
// =============================================================================

// On Windows, _read/_write take unsigned int; on POSIX they take size_t.
// These wrappers suppress C4267 truncation warnings on MSVC.
#if RT_PLATFORM_WINDOWS
/// @brief POSIX-compatible `read` wrapper that clamps `count` to UINT_MAX to suppress MSVC C4267.
/// @param fd CRT file descriptor to read.
/// @param[out] buf Destination buffer.
/// @param count Maximum requested byte count.
/// @return Number of bytes read, zero at end of file, or -1 on error with `errno` set.
static inline ssize_t rt_posix_read(int fd, void *buf, size_t count) {
    unsigned int chunk = count > (size_t)UINT_MAX ? UINT_MAX : (unsigned int)count;
    return read(fd, buf, chunk);
}

/// @brief POSIX-compatible `write` wrapper that clamps `count` to UINT_MAX to suppress MSVC C4267.
/// @param fd CRT file descriptor to write.
/// @param buf Bytes to write.
/// @param count Requested byte count.
/// @return Number of bytes written, or -1 on error with `errno` set.
static inline ssize_t rt_posix_write(int fd, const void *buf, size_t count) {
    unsigned int chunk = count > (size_t)UINT_MAX ? UINT_MAX : (unsigned int)count;
    return write(fd, buf, chunk);
}
#else
#define rt_posix_read read
#define rt_posix_write write
#endif

#if RT_PLATFORM_WINDOWS
/// @brief Open a file at a UTF-8 path via `_wopen` (Windows), converting through wide-char.
/// @details Adds `_O_NOINHERIT` so child processes do not inherit the returned descriptor.
/// @param path Null-terminated UTF-8 filesystem path.
/// @param flags CRT open flags.
/// @param pmode Creation permissions used when @p flags contains `O_CREAT`.
/// @return Open file descriptor, or -1 on conversion/open failure.
static int rt_fileext_open(const char *path, int flags, int pmode) {
    wchar_t *wide = rt_file_path_utf8_to_wide(path);
    if (!wide)
        return -1;
    int fd = _wopen(wide, flags | _O_NOINHERIT, pmode);
    free(wide);
    return fd;
}

/// @brief Acquire an exclusive whole-file lock for a Windows append descriptor.
/// @details Serializes appends opened through separate CRT descriptors because
///          `_O_APPEND` does not make the seek-and-write sequence atomic on Windows.
/// @param fd Open CRT descriptor whose underlying handle should be locked.
/// @param[out] lock_state Receives the overlapped state required for unlocking.
/// @return 1 when the lock was acquired; otherwise 0.
static int rt_fileext_lock_append(int fd, OVERLAPPED *lock_state) {
    intptr_t raw_handle = _get_osfhandle(fd);
    if (raw_handle == -1)
        return 0;
    memset(lock_state, 0, sizeof(*lock_state));
    return LockFileEx(
               (HANDLE)raw_handle, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, lock_state)
               ? 1
               : 0;
}

/// @brief Release a whole-file append lock previously acquired on @p fd.
/// @param fd CRT descriptor associated with the lock.
/// @param lock_state Overlapped state initialized by rt_fileext_lock_append().
/// @return 1 when the lock was released; otherwise 0.
static int rt_fileext_unlock_append(int fd, OVERLAPPED *lock_state) {
    intptr_t raw_handle = _get_osfhandle(fd);
    if (raw_handle == -1)
        return 0;
    return UnlockFileEx((HANDLE)raw_handle, 0, MAXDWORD, MAXDWORD, lock_state) ? 1 : 0;
}

/// @brief Stat a file at a UTF-8 path via the 64-bit-size `_wstat64` variant (Windows).
/// @param path Null-terminated UTF-8 filesystem path.
/// @param[out] st Receives the file metadata.
/// @return Zero on success, or -1 on conversion/stat failure.
static int rt_fileext_stat_path(const char *path, rt_fileext_stat_t *st) {
    wchar_t *wide = rt_file_path_utf8_to_wide(path);
    if (!wide)
        return -1;
    int rc = _wstat64(wide, st);
    free(wide);
    return rc;
}

/// @brief Delete a file at a UTF-8 path via `_wunlink` (Windows), converting through wide-char.
/// @param path Null-terminated UTF-8 path to remove.
/// @return Zero on success, or -1 on conversion/removal failure.
static int rt_fileext_unlink(const char *path) {
    wchar_t *wide = rt_file_path_utf8_to_wide(path);
    if (!wide)
        return -1;
    int rc = _wunlink(wide);
    free(wide);
    return rc;
}

/// @brief Set file access/modification times at a UTF-8 path via `_wutime` (Windows).
/// @param path Null-terminated UTF-8 path to update.
/// @param times Requested access and modification times, or NULL to use the current time.
/// @return Zero on success, or -1 on conversion/update failure.
static int rt_fileext_utime(const char *path, struct _utimbuf *times) {
    wchar_t *wide = rt_file_path_utf8_to_wide(path);
    if (!wide)
        return -1;
    int rc = _wutime(wide, times);
    free(wide);
    return rc;
}

/// @brief Write an entire byte span, retrying interruptions and short writes.
/// @details A zero-byte write is treated as failure to avoid an infinite loop.
/// @param fd Open descriptor to receive the bytes.
/// @param data Byte span to write; may be NULL only when @p len is zero.
/// @param len Number of bytes to write.
/// @return 1 after all bytes are written; otherwise 0 with `errno` left by the failing write.
static int rt_fileext_write_all_fd(int fd, const uint8_t *data, size_t len) {
    size_t written = 0;
    while (written < len) {
        ssize_t n = rt_posix_write(fd, data + written, len - written);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return 0;
        }
        if (n == 0)
            return 0;
        written += (size_t)n;
    }
    return 1;
}

/// @brief Build the `.zanna-tmp.<pid>.<nonce>.<attempt>` sidecar path used by atomic writes.
/// @param path Destination path beside which the sidecar will be created.
/// @param attempt Collision-retry index incorporated into the name.
/// @return Newly allocated candidate path, or NULL when it cannot be constructed.
static char *rt_fileext_make_temp_path(const char *path, unsigned attempt) {
    return rt_fileext_make_parent_temp_path(path, ".zanna-tmp.", attempt);
}

/// @brief Atomically replace `dst` with `src` using MoveFileExW (Windows).
/// @details Uses MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH for crash-safe overwrite.
/// @param src UTF-8 path of the staged source file.
/// @param dst UTF-8 destination path to replace.
/// @return 1 on success; otherwise 0.
static int rt_fileext_replace_utf8(const char *src, const char *dst) {
    wchar_t *wsrc = rt_file_path_utf8_to_wide(src);
    wchar_t *wdst = rt_file_path_utf8_to_wide(dst);
    if (!wsrc || !wdst) {
        free(wsrc);
        free(wdst);
        return 0;
    }
    BOOL ok = MoveFileExW(wsrc, wdst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    free(wsrc);
    free(wdst);
    return ok ? 1 : 0;
}
#else
/// @brief Open a POSIX path while preventing descriptor inheritance across `exec`.
/// @details Uses `O_CLOEXEC` when available and otherwise applies `FD_CLOEXEC` after opening.
/// @param path Null-terminated filesystem path.
/// @param flags POSIX open flags.
/// @param pmode Creation permissions used when @p flags contains `O_CREAT`.
/// @return Open file descriptor, or -1 on failure with `errno` set.
static int rt_fileext_open(const char *path, int flags, int pmode) {
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int fd = open(path, flags, pmode);
#if defined(FD_CLOEXEC) && !defined(O_CLOEXEC)
    if (fd >= 0) {
        int fd_flags = fcntl(fd, F_GETFD);
        if (fd_flags >= 0)
            (void)fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);
    }
#endif
    return fd;
}

#define rt_fileext_stat_path stat
#define rt_fileext_unlink unlink
#define rt_fileext_utime utime

/// @brief Write an entire byte span, retrying interruptions and short writes.
/// @details A zero-byte write is treated as failure to avoid an infinite loop.
/// @param fd Open descriptor to receive the bytes.
/// @param data Byte span to write; may be NULL only when @p len is zero.
/// @param len Number of bytes to write.
/// @return 1 after all bytes are written; otherwise 0 with `errno` left by the failing write.
static int rt_fileext_write_all_fd(int fd, const uint8_t *data, size_t len) {
    size_t written = 0;
    while (written < len) {
        ssize_t n = rt_posix_write(fd, data + written, len - written);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return 0;
        }
        if (n == 0)
            return 0;
        written += (size_t)n;
    }
    return 1;
}

/// @brief Build the `.zanna-tmp.<pid>.<nonce>.<attempt>` sidecar path used by atomic writes.
/// @param path Destination path beside which the sidecar will be created.
/// @param attempt Collision-retry index incorporated into the name.
/// @return Newly allocated candidate path, or NULL when it cannot be constructed.
static char *rt_fileext_make_temp_path(const char *path, unsigned attempt) {
    return rt_fileext_make_parent_temp_path(path, ".zanna-tmp.", attempt);
}

/// @brief Atomically replace `dst` with `src` using `rename(2)` (POSIX).
/// @details `rename` is atomic within a single filesystem by POSIX guarantee.
/// @param src Path of the staged source file.
/// @param dst Destination path to replace.
/// @return 1 on success; otherwise 0 with `errno` set by `rename`.
static int rt_fileext_replace_utf8(const char *src, const char *dst) {
    return rename(src, dst) == 0 ? 1 : 0;
}
#endif

/// @brief Move the staged temp file to its final destination.
///
/// When `replace=1`, clobbers any existing destination (used by
/// `WriteAllText`/`WriteAllBytes` where the user expects overwrite).
/// When `replace=0`, refuses to overwrite (used by `Move` and similar
/// no-clobber operations): Windows uses `MoveFileExW` without
/// REPLACE_EXISTING; POSIX uses `link()` + `unlink()` since `rename`
/// overwrites unconditionally. The link/unlink dance atomically
/// reserves the new name and cleans up the source, rolling back on
/// failure.
/// @param src Path of the staged source file.
/// @param dst Final destination path.
/// @param replace Nonzero to replace an existing destination; zero to require a new name.
/// @return 1 when the source was committed to @p dst; otherwise 0.
static int rt_fileext_commit_utf8(const char *src, const char *dst, int replace) {
    if (replace)
        return rt_fileext_replace_utf8(src, dst);
#if RT_PLATFORM_WINDOWS
    wchar_t *wsrc = rt_file_path_utf8_to_wide(src);
    wchar_t *wdst = rt_file_path_utf8_to_wide(dst);
    if (!wsrc || !wdst) {
        free(wsrc);
        free(wdst);
        return 0;
    }
    BOOL ok = MoveFileExW(wsrc, wdst, MOVEFILE_WRITE_THROUGH);
    free(wsrc);
    free(wdst);
    return ok ? 1 : 0;
#else
    if (link(src, dst) != 0)
        return 0;
    if (unlink(src) != 0) {
        int saved = errno;
        (void)unlink(dst);
        errno = saved;
        return 0;
    }
    return 1;
#endif
}

/// @brief Open an exclusive temp file beside `path` for atomic-write staging.
///
/// Tries up to 128 distinct `.zanna-tmp.<pid>.<attempt>` sidecar names
/// until O_EXCL succeeds. O_NOFOLLOW (when available) prevents a
/// symlink attack that would redirect the write elsewhere. On success,
/// writes the chosen temp path into `*out_tmp` for the caller to rename
/// (or unlink on error). Returns the open fd, or -1 on failure with
/// errno preserved.
/// @param path Final destination path used to choose the sidecar's parent directory.
/// @param binary Nonzero to request the platform's binary descriptor mode.
/// @param[out] out_tmp Receives the allocated sidecar path on success; may be NULL if the caller
///                     does not need the name.
/// @return Open exclusive descriptor on success, or -1 after exhaustion or an OS error.
static int rt_fileext_open_temp_utf8(const char *path, int binary, char **out_tmp) {
    if (out_tmp)
        *out_tmp = NULL;
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        char *tmp = rt_fileext_make_temp_path(path, attempt);
        if (!tmp) {
            errno = ENOMEM;
            return -1;
        }

        int flags = O_WRONLY | O_CREAT | O_EXCL | (binary ? RT_FILE_O_BINARY : 0);
#if defined(O_NOFOLLOW)
        flags |= O_NOFOLLOW;
#endif
        int fd = rt_fileext_open(tmp, flags, 0666);
        if (fd >= 0) {
            if (out_tmp)
                *out_tmp = tmp;
            else
                free(tmp);
            return fd;
        }
        int err = errno;
        free(tmp);
        if (err != EEXIST)
            return -1;
    }
    errno = EEXIST;
    return -1;
}

/// @brief Fsync the directory containing `path` so a rename is crash-durable.
///
/// On POSIX, the rename itself hits the filesystem journal but the
/// parent directory's updated dirent doesn't necessarily reach disk
/// until its inode is fsync'd. Opens the parent with O_DIRECTORY
/// (where supported) to avoid accidentally sync'ing a regular file.
/// On Windows this is a no-op because `MoveFileExW | WRITE_THROUGH`
/// already handles durability.
/// @param path Destination path whose parent directory should be synchronized.
/// @return 1 on success, including the Windows no-op; otherwise 0.
static int rt_fileext_sync_parent_dir(const char *path) {
#if RT_PLATFORM_WINDOWS
    (void)path;
    return 1;
#else
    const char *last = strrchr(path, '/');
    char stack_buf[PATH_MAX];
    const char *parent = ".";
    char *heap_buf = NULL;

    if (last) {
        size_t len = (size_t)(last - path);
        if (len == 0) {
            parent = "/";
        } else if (len < sizeof(stack_buf)) {
            memcpy(stack_buf, path, len);
            stack_buf[len] = '\0';
            parent = stack_buf;
        } else {
            heap_buf = (char *)malloc(len + 1);
            if (!heap_buf)
                return 0;
            memcpy(heap_buf, path, len);
            heap_buf[len] = '\0';
            parent = heap_buf;
        }
    }

    int flags = O_RDONLY;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
    int fd = rt_fileext_open(parent, flags, 0);
    free(heap_buf);
    if (fd < 0)
        return 0;
    int ok = fsync(fd) == 0 ? 1 : 0;
    if (close(fd) != 0)
        ok = 0;
    return ok;
#endif
}

/// @brief Close `fd` and trap with `context` message if `close(2)` fails.
/// @param fd Open descriptor to close.
/// @param context Diagnostic passed to rt_trap() if closing fails.
static void rt_fileext_close_or_trap(int fd, const char *context) {
    if (close(fd) != 0)
        rt_trap(context);
}

/// @brief Copy access and modification timestamps from a stat snapshot to a path.
/// @param path Destination file to update.
/// @param src_st Source metadata containing `st_atime` and `st_mtime`.
/// @return 1 when both arguments are valid and the timestamp update succeeds; otherwise 0.
static int rt_fileext_apply_timestamps(const char *path, const rt_fileext_stat_t *src_st) {
    if (!path || !src_st)
        return 0;
#if RT_PLATFORM_WINDOWS
    struct _utimbuf times;
#else
    struct utimbuf times;
#endif
    times.actime = src_st->st_atime;
    times.modtime = src_st->st_mtime;
    return rt_fileext_utime(path, &times) == 0 ? 1 : 0;
}

/// @brief Apply preserved permission bits to an open staging file.
/// @details POSIX copies the low permission and special-mode bits with `fchmod`. Windows returns
///          success here because mode preservation is applied by path after closing the file.
/// @param fd Open staging-file descriptor.
/// @param src_st Metadata snapshot supplying the original permission mode.
/// @return 1 on success; otherwise 0.
static int rt_fileext_apply_mode_to_open_file(int fd, const rt_fileext_stat_t *src_st) {
    if (!src_st)
        return 0;
#if RT_PLATFORM_WINDOWS
    (void)fd;
    return 1;
#else
    return fchmod(fd, src_st->st_mode & 07777) == 0 ? 1 : 0;
#endif
}

/// @brief Apply preserved permission bits to a staging or committed file by path.
/// @details Windows preserves its supported read/write attributes; POSIX copies permission and
///          special-mode bits with `chmod`.
/// @param path Destination path to update.
/// @param src_st Metadata snapshot supplying the original permission mode.
/// @return 1 on success; otherwise 0.
static int rt_fileext_apply_mode_to_path(const char *path, const rt_fileext_stat_t *src_st) {
    if (!path || !src_st)
        return 0;
#if RT_PLATFORM_WINDOWS
    wchar_t *wide = rt_file_path_utf8_to_wide(path);
    if (!wide)
        return 0;
    int rc = _wchmod(wide, src_st->st_mode & (_S_IREAD | _S_IWRITE));
    free(wide);
    return rc == 0 ? 1 : 0;
#else
    return chmod(path, src_st->st_mode & 07777) == 0 ? 1 : 0;
#endif
}

/// @brief Test whether two paths refer to the same on-disk inode/file.
///
/// Used by `Copy` to short-circuit self-copies (which would truncate
/// the file when the temp-rename step overwrote the source). On POSIX
/// compares (dev, ino); on Windows compares
/// (volume, fileIndexHigh, fileIndexLow). Returns 0 if either path
/// can't be stat'd — treating inaccessible paths as distinct so the
/// copy attempt can fail with a clearer error.
/// @param src_path First existing path to compare.
/// @param dst_path Second existing path to compare.
/// @return 1 when both paths resolve to the same filesystem object; otherwise 0.
static int rt_fileext_same_existing_file(const char *src_path, const char *dst_path) {
#if RT_PLATFORM_WINDOWS
    wchar_t *wsrc = rt_file_path_utf8_to_wide(src_path);
    wchar_t *wdst = rt_file_path_utf8_to_wide(dst_path);
    if (!wsrc || !wdst) {
        free(wsrc);
        free(wdst);
        return 0;
    }

    HANDLE src = CreateFileW(wsrc,
                             0,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             NULL,
                             OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL,
                             NULL);
    HANDLE dst = CreateFileW(wdst,
                             0,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             NULL,
                             OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL,
                             NULL);
    free(wsrc);
    free(wdst);
    if (src == INVALID_HANDLE_VALUE) {
        if (dst != INVALID_HANDLE_VALUE)
            CloseHandle(dst);
        return 0;
    }
    if (dst == INVALID_HANDLE_VALUE) {
        CloseHandle(src);
        return 0;
    }

    BY_HANDLE_FILE_INFORMATION si;
    BY_HANDLE_FILE_INFORMATION di;
    int same = 0;
    if (GetFileInformationByHandle(src, &si) && GetFileInformationByHandle(dst, &di)) {
        same = si.dwVolumeSerialNumber == di.dwVolumeSerialNumber &&
               si.nFileIndexHigh == di.nFileIndexHigh && si.nFileIndexLow == di.nFileIndexLow;
    }
    CloseHandle(src);
    CloseHandle(dst);
    return same;
#else
    rt_fileext_stat_t src_st;
    rt_fileext_stat_t dst_st;
    if (stat(src_path, &src_st) != 0)
        return 0;
    if (stat(dst_path, &dst_st) != 0)
        return 0;
    return src_st.st_dev == dst_st.st_dev && src_st.st_ino == dst_st.st_ino;
#endif
}

/// @brief Return 1 if `mode` (from stat) indicates a regular file; 0 otherwise.
/// @param mode Platform stat mode bits to classify.
/// @return 1 for a regular file; otherwise 0.
static int rt_fileext_is_regular_mode(int mode) {
#if RT_PLATFORM_WINDOWS
    return (mode & _S_IFREG) != 0;
#else
    return S_ISREG(mode) ? 1 : 0;
#endif
}

/// @brief Snapshot the permission metadata of an existing regular file.
/// @details Atomic writers create a fresh sidecar file before renaming it over the destination.
///          Without this snapshot, that replacement would silently exchange the destination's
///          permission mode for the process-default mode selected by `open`. A missing path or a
///          path that is not a regular file is deliberately treated as having no metadata to
///          preserve; the later open/replace operation remains responsible for reporting any
///          actual I/O error.
/// @param path UTF-8 destination path to inspect.
/// @param[out] out_st Receives the destination's stat record when this function returns 1.
/// @return 1 when @p path names an existing regular file and @p out_st was populated; otherwise
///         0.
static int rt_fileext_snapshot_replaced_file(const char *path, rt_fileext_stat_t *out_st) {
    if (!path || !out_st)
        return 0;
    if (rt_fileext_stat_path(path, out_st) != 0)
        return 0;
    return rt_fileext_is_regular_mode(out_st->st_mode);
}

/// @brief **Atomic-write to disk:** write to an exclusive temp sidecar, fsync (POSIX) or
/// _commit (Win32), close, atomically rename over the destination, and fsync the parent
/// directory on POSIX so the name replacement is crash-durable. When the destination is an
/// existing regular file, its permission mode is copied to the sidecar before replacement.
/// @param path UTF-8 destination path to create or replace.
/// @param data Byte span to write; may be NULL only when @p len is zero.
/// @param len Number of bytes in @p data.
/// @param binary Nonzero to open the Windows staging descriptor in binary mode.
/// @param replace Nonzero to replace the destination; zero to require a new name at commit.
/// @return 1 after the durable commit completes; otherwise 0 after removing the sidecar when
///         possible.
static int rt_fileext_write_atomic_utf8(
    const char *path, const uint8_t *data, size_t len, int binary, int replace) {
    rt_fileext_stat_t replaced_st;
    int preserve_mode = replace && rt_fileext_snapshot_replaced_file(path, &replaced_st);

    char *tmp = NULL;
    int fd = rt_fileext_open_temp_utf8(path, binary, &tmp);
    if (fd < 0)
        return 0;

    int ok = rt_fileext_write_all_fd(fd, data, len);
#if RT_PLATFORM_WINDOWS
    if (ok && _commit(fd) != 0)
        ok = 0;
#else
    if (ok && preserve_mode && !rt_fileext_apply_mode_to_open_file(fd, &replaced_st))
        ok = 0;
    if (ok && fsync(fd) != 0)
        ok = 0;
#endif
    if (close(fd) != 0)
        ok = 0;
#if RT_PLATFORM_WINDOWS
    if (ok && preserve_mode && !rt_fileext_apply_mode_to_path(tmp, &replaced_st))
        ok = 0;
#endif
    if (ok)
        ok = rt_fileext_commit_utf8(tmp, path, replace);
    if (ok)
        ok = rt_fileext_sync_parent_dir(path);
    if (!ok)
        (void)rt_fileext_unlink(tmp);
    free(tmp);
    return ok;
}

/// @brief State for one cooperative whole-file compare/exchange lock.
typedef struct rt_fileext_path_lock {
#if RT_PLATFORM_WINDOWS
    HANDLE handle;
    OVERLAPPED overlapped;
#else
    int fd;
#endif
} rt_fileext_path_lock;

/// @brief Build the stable directory lock path for one compare/exchange destination.
/// @details One fixed-length lock per directory avoids leaving a sidecar for every edited file and
///          also avoids exceeding a near-limit destination component. Serializing independent
///          compare/exchanges in one directory is an intentional conservative tradeoff. POSIX
///          includes the effective user ID so an owner-only lock in a shared temp directory cannot
///          deny this API to other users.
/// @param path Null-terminated destination path.
/// @return Allocated adjacent lock path, or NULL on overflow/allocation failure.
static char *rt_fileext_make_lock_path(const char *path) {
    if (!path)
        return NULL;
    size_t path_len = strlen(path);
    size_t parent_len = 0;
    for (size_t i = 0; i < path_len; ++i) {
#if RT_PLATFORM_WINDOWS
        if (path[i] == '/' || path[i] == '\\')
#else
        if (path[i] == '/')
#endif
            parent_len = i + 1;
    }

    char basename[64];
#if RT_PLATFORM_WINDOWS
    int basename_len = snprintf(basename, sizeof(basename), ".zanna-cas.lock");
#else
    int basename_len =
        snprintf(basename, sizeof(basename), ".zanna-cas.%llu.lock", (unsigned long long)geteuid());
#endif
    if (basename_len < 0 || (size_t)basename_len >= sizeof(basename) ||
        parent_len > SIZE_MAX - (size_t)basename_len - 1)
        return NULL;
    size_t cap = parent_len + (size_t)basename_len + 1;
    char *lock_path = (char *)malloc(cap);
    if (!lock_path)
        return NULL;
    if (parent_len > 0)
        memcpy(lock_path, path, parent_len);
    memcpy(lock_path + parent_len, basename, (size_t)basename_len + 1);
    return lock_path;
}

/// @brief Acquire the stable whole-file compare/exchange lock beside @p path.
/// @details Lock files intentionally persist so a waiter can never hold an unlinked object while
///          another process creates and locks a replacement. Existing links/reparse points and
///          non-regular entries are rejected.
/// @param path Destination whose lock should be acquired.
/// @param[out] lock Receives the platform lock state.
/// @return 1 on success; otherwise 0 with no owned handle remaining.
static int rt_fileext_path_lock_acquire(const char *path, rt_fileext_path_lock *lock) {
    if (!path || !lock)
        return 0;
    char *lock_path = rt_fileext_make_lock_path(path);
    if (!lock_path)
        return 0;
#if RT_PLATFORM_WINDOWS
    lock->handle = INVALID_HANDLE_VALUE;
    memset(&lock->overlapped, 0, sizeof(lock->overlapped));
    wchar_t *wide = rt_file_path_utf8_to_wide(lock_path);
    free(lock_path);
    if (!wide)
        return 0;
    HANDLE handle = CreateFileW(wide,
                                GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                NULL,
                                OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                                NULL);
    free(wide);
    if (handle == INVALID_HANDLE_VALUE)
        return 0;
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(handle, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        CloseHandle(handle);
        return 0;
    }
    if (!LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &lock->overlapped)) {
        CloseHandle(handle);
        return 0;
    }
    lock->handle = handle;
    return 1;
#else
    lock->fd = -1;
    int flags = O_RDWR | O_CREAT;
#if defined(O_NOFOLLOW)
    flags |= O_NOFOLLOW;
#endif
    int fd = rt_fileext_open(lock_path, flags, 0600);
    free(lock_path);
    if (fd < 0)
        return 0;
    rt_fileext_stat_t st;
    if (rt_fileext_fstat(fd, &st) != 0 || !rt_fileext_is_regular_mode(st.st_mode)) {
        close(fd);
        return 0;
    }
    struct flock request;
    memset(&request, 0, sizeof(request));
    request.l_type = F_WRLCK;
    request.l_whence = SEEK_SET;
    int rc;
    do {
        rc = fcntl(fd, F_SETLKW, &request);
    } while (rc != 0 && errno == EINTR);
    if (rc != 0) {
        close(fd);
        return 0;
    }
    lock->fd = fd;
    return 1;
#endif
}

/// @brief Release a lock acquired by rt_fileext_path_lock_acquire().
/// @param lock Owned platform lock state.
/// @return 1 when unlock and handle close both succeeded; otherwise 0.
static int rt_fileext_path_lock_release(rt_fileext_path_lock *lock) {
    if (!lock)
        return 0;
#if RT_PLATFORM_WINDOWS
    if (lock->handle == INVALID_HANDLE_VALUE)
        return 0;
    int ok = UnlockFileEx(lock->handle, 0, MAXDWORD, MAXDWORD, &lock->overlapped) ? 1 : 0;
    if (!CloseHandle(lock->handle))
        ok = 0;
    lock->handle = INVALID_HANDLE_VALUE;
    return ok;
#else
    if (lock->fd < 0)
        return 0;
    struct flock request;
    memset(&request, 0, sizeof(request));
    request.l_type = F_UNLCK;
    request.l_whence = SEEK_SET;
    int ok = fcntl(lock->fd, F_SETLK, &request) == 0 ? 1 : 0;
    if (close(lock->fd) != 0)
        ok = 0;
    lock->fd = -1;
    return ok;
#endif
}

/// @brief Compare one regular file with a byte snapshot without allocating its full contents.
/// @details A missing path matches an empty snapshot. Concurrent truncation/growth is treated as
///          mismatch; invalid/non-regular paths and read/close failures report an I/O error.
/// @param path Destination path to inspect while its cooperative lock is held.
/// @param expected Expected byte span; may be NULL only when @p expected_len is zero.
/// @param expected_len Expected byte count.
/// @return 1 for an exact match, 0 for mismatch, or -1 for an I/O/type error.
static int rt_fileext_compare_path_bytes(const char *path,
                                         const uint8_t *expected,
                                         size_t expected_len) {
    int fd = rt_fileext_open(path, O_RDONLY | RT_FILE_O_BINARY, 0);
    if (fd < 0)
        return errno == ENOENT ? (expected_len == 0 ? 1 : 0) : -1;

    rt_fileext_stat_t st;
    int result = 1;
    if (rt_fileext_fstat(fd, &st) != 0 || !rt_fileext_is_regular_mode(st.st_mode)) {
        result = -1;
    } else if (st.st_size < 0 || (uint64_t)st.st_size != (uint64_t)expected_len) {
        result = 0;
    }

    uint8_t buffer[16384];
    size_t offset = 0;
    while (result == 1 && offset < expected_len) {
        size_t wanted = expected_len - offset;
        if (wanted > sizeof(buffer))
            wanted = sizeof(buffer);
        ssize_t count;
        do {
            count = rt_posix_read(fd, buffer, wanted);
        } while (count < 0 && errno == EINTR);
        if (count < 0) {
            result = -1;
        } else if (count == 0 || memcmp(buffer, expected + offset, (size_t)count) != 0) {
            result = 0;
        } else {
            offset += (size_t)count;
        }
    }
    if (result == 1) {
        uint8_t extra = 0;
        ssize_t count;
        do {
            count = rt_posix_read(fd, &extra, 1);
        } while (count < 0 && errno == EINTR);
        if (count < 0)
            result = -1;
        else if (count != 0)
            result = 0;
    }
    if (close(fd) != 0)
        result = -1;
    return result;
}

/// @brief Validate a path argument: trap with `context` on NULL/empty input.
/// @details Used as the first line of every public file operation to give specific error messages.
/// @param path Runtime string containing the path.
/// @param context Trap message to emit when conversion fails.
/// @return Null-terminated host path on success; never returns on failure (traps).
static const char *rt_io_file_require_path(rt_string path, const char *context) {
    const char *cpath = NULL;
    if (!rt_file_path_from_vstr(path, &cpath) || !cpath || *cpath == '\0')
        rt_trap(context);
    return cpath;
}

/// @brief Test whether a runtime path names an existing regular file.
/// @details This non-trapping predicate returns false for invalid paths, missing or inaccessible
///          entries, directories, and other non-regular filesystem objects.
/// @param path Runtime string containing the path to inspect.
/// @return 1 when @p path can be statted as a regular file; otherwise 0.
int64_t rt_io_file_exists(rt_string path) {
    const char *cpath = NULL;
    if (!rt_file_path_from_vstr(path, &cpath) || !cpath)
        return 0;
    rt_fileext_stat_t st;
    if (rt_fileext_stat_path(cpath, &st) == 0) {
#ifdef _WIN32
        return (st.st_mode & _S_IFREG) != 0;
#else
        return S_ISREG(st.st_mode);
#endif
    }
    return 0;
}

/// @brief Test whether two runtime paths resolve to the same existing file.
/// @details The predicate is intentionally non-trapping: missing, malformed,
///          inaccessible, and non-regular paths simply compare unequal. The
///          underlying identity comparison follows symlinks and compares
///          volume/file IDs on Windows or device/inode pairs on POSIX.
/// @param left First runtime path to compare.
/// @param right Second runtime path to compare.
/// @return 1 when both valid paths resolve to the same existing file; otherwise 0.
int64_t rt_file_same(rt_string left, rt_string right) {
    const char *left_path = NULL;
    const char *right_path = NULL;
    if (!rt_file_path_from_vstr(left, &left_path) || !left_path || *left_path == '\0' ||
        !rt_file_path_from_vstr(right, &right_path) || !right_path || *right_path == '\0')
        return 0;
    rt_fileext_stat_t left_stat;
    rt_fileext_stat_t right_stat;
    if (rt_fileext_stat_path(left_path, &left_stat) != 0 ||
        rt_fileext_stat_path(right_path, &right_stat) != 0 ||
        !rt_fileext_is_regular_mode(left_stat.st_mode) ||
        !rt_fileext_is_regular_mode(right_stat.st_mode))
        return 0;
    return rt_fileext_same_existing_file(left_path, right_path) ? 1 : 0;
}

/// @brief Return an opaque stable key for one existing regular file.
/// @details Windows keys contain the volume serial and 64-bit file index obtained
///          from an open handle. POSIX keys contain the device/inode pair from
///          `stat`. The textual representation is deliberately undocumented and
///          callers must use it only as an equality/grouping key.
/// @param path Runtime path whose followed regular-file identity should be read.
/// @return Fresh nonempty key on success, or the shared empty string on failure.
rt_string rt_file_identity_key(rt_string path) {
    const char *cpath = NULL;
    if (!rt_file_path_from_vstr(path, &cpath) || !cpath || *cpath == '\0')
        return rt_str_empty();

    rt_fileext_stat_t st;
    if (rt_fileext_stat_path(cpath, &st) != 0 || !rt_fileext_is_regular_mode(st.st_mode))
        return rt_str_empty();

    char key[96];
#if RT_PLATFORM_WINDOWS
    wchar_t *wide = rt_file_path_utf8_to_wide(cpath);
    if (!wide)
        return rt_str_empty();
    HANDLE handle = CreateFileW(wide,
                                FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                NULL,
                                OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL,
                                NULL);
    free(wide);
    if (handle == INVALID_HANDLE_VALUE)
        return rt_str_empty();
    BY_HANDLE_FILE_INFORMATION info;
    BOOL queried = GetFileInformationByHandle(handle, &info);
    CloseHandle(handle);
    if (!queried)
        return rt_str_empty();
    int written = snprintf(key,
                           sizeof(key),
                           "w:%08" PRIx32 ":%08" PRIx32 "%08" PRIx32,
                           (uint32_t)info.dwVolumeSerialNumber,
                           (uint32_t)info.nFileIndexHigh,
                           (uint32_t)info.nFileIndexLow);
#else
    int written = snprintf(
        key, sizeof(key), "p:%" PRIxMAX ":%" PRIxMAX, (uintmax_t)st.st_dev, (uintmax_t)st.st_ino);
#endif
    if (written <= 0 || (size_t)written >= sizeof(key))
        return rt_str_empty();
    return rt_string_from_bytes(key, (size_t)written);
}

/// @brief Read an entire regular file into a runtime string.
/// @details Sizes the allocation from `fstat`, then reads exactly that many bytes. No encoding
///          validation or newline translation is performed. A size change that causes premature
///          EOF, an oversized/non-regular file, or any open, read, close, or allocation failure
///          traps. An empty file returns the shared empty runtime string.
/// @param path Runtime string containing the file path.
/// @return Runtime string containing the exact file bytes; control-flow fallback values after a
///         trap are not successful results.
rt_string rt_io_file_read_all_text(rt_string path) {
    const char *cpath =
        rt_io_file_require_path(path, "Zanna.IO.File.ReadAllText: invalid file path");

    int fd = rt_fileext_open(cpath, O_RDONLY | RT_FILE_O_BINARY, 0);
    if (fd < 0) {
        rt_trap("Zanna.IO.File.ReadAllText: failed to open file");
        return rt_str_empty();
    }

    rt_fileext_stat_t st;
    if (rt_fileext_fstat(fd, &st) != 0) {
        close(fd);
        rt_trap("Zanna.IO.File.ReadAllText: failed to stat file");
        return rt_str_empty();
    }
    if (!rt_fileext_is_regular_mode(st.st_mode)) {
        close(fd);
        rt_trap("Zanna.IO.File.ReadAllText: path is not a regular file");
        return rt_str_empty();
    }
    if (st.st_size < 0 || (uint64_t)st.st_size > (uint64_t)SIZE_MAX) {
        close(fd);
        rt_trap("Zanna.IO.File.ReadAllText: file too large");
        return rt_str_empty();
    }
    size_t size = (st.st_size > 0) ? (size_t)st.st_size : 0;
    // Handle empty files
    if (size == 0) {
        rt_fileext_close_or_trap(fd, "Zanna.IO.File.ReadAllText: failed to close file");
        return rt_str_empty();
    }

    char *buf = (char *)malloc(size);
    if (!buf) {
        close(fd);
        rt_trap("Zanna.IO.File.ReadAllText: allocation failed");
        return rt_str_empty();
    }

    size_t off = 0;
    while (off < size) {
        ssize_t n = rt_posix_read(fd, buf + off, size - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            free(buf);
            close(fd);
            rt_trap("Zanna.IO.File.ReadAllText: failed to read file");
            return rt_str_empty();
        }
        if (n == 0) {
            free(buf);
            close(fd);
            rt_trap("Zanna.IO.File.ReadAllText: file changed while reading");
            return rt_str_empty();
        }
        off += (size_t)n;
    }
    if (close(fd) != 0) {
        free(buf);
        rt_trap("Zanna.IO.File.ReadAllText: failed to close file");
        return rt_str_empty();
    }
    rt_string s = rt_string_from_bytes(buf, size);
    free(buf);
    if (!s) {
        rt_trap("Zanna.IO.File.ReadAllText: allocation failed");
        return rt_str_empty();
    }
    return s;
}

/// @brief Read complete text from one descriptor under a strict byte ceiling.
/// @details The descriptor is statted before allocation. After reading the
///          statted size, one additional byte is probed so observed growth is
///          rejected rather than returned as a partial file snapshot.
/// @param path Runtime string containing the file path.
/// @param max_bytes Maximum accepted descriptor size; negative values trap.
/// @return Runtime string containing the exact stable file bytes.
rt_string rt_io_file_read_all_text_bounded(rt_string path, int64_t max_bytes) {
    const char *cpath =
        rt_io_file_require_path(path, "Zanna.IO.File.ReadAllTextBounded: invalid file path");
    if (max_bytes < 0) {
        rt_trap("Zanna.IO.File.ReadAllTextBounded: maxBytes must be nonnegative");
        return rt_str_empty();
    }

    int fd = rt_fileext_open(cpath, O_RDONLY | RT_FILE_O_BINARY, 0);
    if (fd < 0) {
        rt_trap("Zanna.IO.File.ReadAllTextBounded: failed to open file");
        return rt_str_empty();
    }

    rt_fileext_stat_t st;
    if (rt_fileext_fstat(fd, &st) != 0) {
        close(fd);
        rt_trap("Zanna.IO.File.ReadAllTextBounded: failed to stat file");
        return rt_str_empty();
    }
    if (!rt_fileext_is_regular_mode(st.st_mode)) {
        close(fd);
        rt_trap("Zanna.IO.File.ReadAllTextBounded: path is not a regular file");
        return rt_str_empty();
    }
    if (st.st_size < 0 || (uint64_t)st.st_size > (uint64_t)SIZE_MAX ||
        (uint64_t)st.st_size > (uint64_t)max_bytes) {
        close(fd);
        rt_trap("Zanna.IO.File.ReadAllTextBounded: file exceeds maxBytes");
        return rt_str_empty();
    }

    size_t size = (size_t)st.st_size;
    char *buf = size > 0 ? (char *)malloc(size) : NULL;
    if (size > 0 && !buf) {
        close(fd);
        rt_trap("Zanna.IO.File.ReadAllTextBounded: allocation failed");
        return rt_str_empty();
    }

    size_t off = 0;
    while (off < size) {
        ssize_t n = rt_posix_read(fd, buf + off, size - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            free(buf);
            close(fd);
            rt_trap("Zanna.IO.File.ReadAllTextBounded: failed to read file");
            return rt_str_empty();
        }
        if (n == 0) {
            free(buf);
            close(fd);
            rt_trap("Zanna.IO.File.ReadAllTextBounded: file changed while reading");
            return rt_str_empty();
        }
        off += (size_t)n;
    }

    uint8_t extra = 0;
    ssize_t trailing = -1;
    do {
        trailing = rt_posix_read(fd, &extra, 1);
    } while (trailing < 0 && errno == EINTR);
    if (trailing != 0) {
        free(buf);
        close(fd);
        rt_trap(trailing > 0 ? "Zanna.IO.File.ReadAllTextBounded: file changed while reading"
                             : "Zanna.IO.File.ReadAllTextBounded: failed to read file");
        return rt_str_empty();
    }
    if (close(fd) != 0) {
        free(buf);
        rt_trap("Zanna.IO.File.ReadAllTextBounded: failed to close file");
        return rt_str_empty();
    }
    if (size == 0)
        return rt_str_empty();

    rt_string result = rt_string_from_bytes(buf, size);
    free(buf);
    if (!result) {
        rt_trap("Zanna.IO.File.ReadAllTextBounded: allocation failed");
        return rt_str_empty();
    }
    return result;
}

/// What: Write @p contents to @p path, replacing the file atomically.
/// Why:  Complement read_all_text with a simple write primitive.
/// How:  Writes an exclusive temp sidecar, flushes it, then replaces the destination.
/// @brief Atomically write `contents` (UTF-8) to `path`, replacing any existing file. Uses
/// `_write_atomic_utf8` so an interrupted write can never corrupt the destination — readers
/// see either the old file or the new file, never a partial write.
/// @details The runtime string's bytes are written verbatim without encoding validation. When
///          replacing a regular file, its supported permission mode is preserved. Invalid
///          arguments and any staging, flush, rename, or directory-sync failure trap.
/// @param path Runtime string containing the destination path.
/// @param contents Runtime string whose complete byte sequence should be stored.
void rt_io_file_write_all_text(rt_string path, rt_string contents) {
    const char *cpath =
        rt_io_file_require_path(path, "Zanna.IO.File.WriteAllText: invalid file path");

    const uint8_t *data = NULL;
    size_t len = rt_file_string_require_view(
        contents, &data, "Zanna.IO.File.WriteAllText: invalid contents");
    if (!rt_fileext_write_atomic_utf8(cpath, data, len, 1, 1))
        rt_trap("Zanna.IO.File.WriteAllText: failed to write file");
}

/// @brief Durably create a text file without replacing a racing destination.
/// @param path Runtime string containing the new destination path.
/// @param contents Runtime string whose bytes are written verbatim.
void rt_io_file_write_all_text_new(rt_string path, rt_string contents) {
    const char *cpath =
        rt_io_file_require_path(path, "Zanna.IO.File.WriteAllTextNew: invalid file path");

    const uint8_t *data = NULL;
    size_t len = rt_file_string_require_view(
        contents, &data, "Zanna.IO.File.WriteAllTextNew: invalid contents");
    if (!rt_fileext_write_atomic_utf8(cpath, data, len, 1, 0))
        rt_trap("Zanna.IO.File.WriteAllTextNew: destination exists or write failed");
}

/// @brief Cooperatively compare and exchange the complete bytes of one text file.
/// @details A stable adjacent OS lock serializes callers using this primitive. The destination is
///          streamed against @p expected while locked and is durably replaced with @p desired only
///          on an exact match. Missing and empty destinations intentionally share the empty
///          expected representation. Ordinary unconditional writers do not join this protocol.
/// @param path Runtime string containing the destination path.
/// @param expected Runtime string containing the complete expected bytes.
/// @param desired Runtime string containing the complete replacement bytes.
/// @return 1 after committing @p desired; 0 for a content mismatch.
int64_t rt_io_file_compare_exchange_all_text(rt_string path,
                                             rt_string expected,
                                             rt_string desired) {
    const char *cpath =
        rt_io_file_require_path(path, "Zanna.IO.File.CompareExchangeAllText: invalid file path");
    const uint8_t *expected_data = NULL;
    size_t expected_len = rt_file_string_require_view(
        expected, &expected_data, "Zanna.IO.File.CompareExchangeAllText: invalid expected text");
    const uint8_t *desired_data = NULL;
    size_t desired_len = rt_file_string_require_view(
        desired, &desired_data, "Zanna.IO.File.CompareExchangeAllText: invalid desired text");

    rt_fileext_path_lock lock;
    if (!rt_fileext_path_lock_acquire(cpath, &lock)) {
        rt_trap("Zanna.IO.File.CompareExchangeAllText: failed to acquire lock");
        return 0;
    }
    int matches = rt_fileext_compare_path_bytes(cpath, expected_data, expected_len);
    if (matches <= 0) {
        int released = rt_fileext_path_lock_release(&lock);
        if (matches < 0) {
            rt_trap("Zanna.IO.File.CompareExchangeAllText: failed to read destination");
            return 0;
        }
        if (!released) {
            rt_trap("Zanna.IO.File.CompareExchangeAllText: failed to release lock");
            return 0;
        }
        return 0;
    }

    int written = rt_fileext_write_atomic_utf8(cpath, desired_data, desired_len, 1, 1);
    int released = rt_fileext_path_lock_release(&lock);
    if (!written) {
        rt_trap("Zanna.IO.File.CompareExchangeAllText: failed to write destination");
        return 0;
    }
    if (!released) {
        rt_trap("Zanna.IO.File.CompareExchangeAllText: failed to release lock");
        return 0;
    }
    return 1;
}

/// @brief Try to acquire a nonblocking, process-lifetime lease on a lock file.
/// @details The persistent file is deliberately not unlinked on release: a
///          scanner can open that same stable inode and determine whether its
///          former owner is still alive. Links, directories, and special files
///          are rejected before publication of a managed handle.
void *rt_file_lease_try_acquire(rt_string path) {
    const char *cpath =
        rt_io_file_require_path(path, "Zanna.IO.FileLease.TryAcquire: invalid file path");

    rt_file_lease_impl *lease = (rt_file_lease_impl *)rt_obj_new_i64(
        RT_FILE_LEASE_CLASS_ID, (int64_t)sizeof(rt_file_lease_impl));
    if (!lease) {
        rt_trap("Zanna.IO.FileLease.TryAcquire: allocation failed");
        return NULL;
    }
    memset(lease, 0, sizeof(*lease));
    lease->closed = 1;
#if RT_PLATFORM_WINDOWS
    lease->handle = INVALID_HANDLE_VALUE;
    memset(&lease->overlap, 0, sizeof(lease->overlap));
#else
    lease->fd = -1;
#endif
    rt_obj_set_finalizer(lease, rt_file_lease_finalize);

#if RT_PLATFORM_WINDOWS
    wchar_t *wide = rt_file_path_utf8_to_wide(cpath);
    if (!wide) {
        rt_fileext_release_object(lease);
        return NULL;
    }
    HANDLE handle = CreateFileW(wide,
                                GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                NULL,
                                OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                                NULL);
    free(wide);
    if (handle == INVALID_HANDLE_VALUE) {
        rt_fileext_release_object(lease);
        return NULL;
    }
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(handle, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
        !LockFileEx(handle,
                    LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                    0,
                    MAXDWORD,
                    MAXDWORD,
                    &lease->overlap)) {
        (void)CloseHandle(handle);
        rt_fileext_release_object(lease);
        return NULL;
    }
    lease->handle = handle;
#else
    int flags = O_RDWR | O_CREAT;
#if defined(O_CLOEXEC)
    flags |= O_CLOEXEC;
#endif
#if defined(O_NOFOLLOW)
    flags |= O_NOFOLLOW;
#endif
    int fd = rt_fileext_open(cpath, flags, 0600);
    if (fd < 0) {
        rt_fileext_release_object(lease);
        return NULL;
    }
    rt_fileext_stat_t st;
    if (rt_fileext_fstat(fd, &st) != 0 || !rt_fileext_is_regular_mode(st.st_mode) ||
        st.st_nlink != 1) {
        (void)close(fd);
        rt_fileext_release_object(lease);
        return NULL;
    }
    int lock_result;
    do {
        lock_result = flock(fd, LOCK_EX | LOCK_NB);
    } while (lock_result != 0 && errno == EINTR);
    if (lock_result != 0) {
        (void)close(fd);
        rt_fileext_release_object(lease);
        return NULL;
    }
    lease->fd = fd;
#endif
    lease->closed = 0;
    return lease;
}

/// @brief Return whether a managed FileLease still owns its native lock.
int64_t rt_file_lease_is_valid(void *handle) {
    rt_file_lease_impl *lease = rt_file_lease_checked(handle);
    if (!lease || lease->closed)
        return 0;
#if RT_PLATFORM_WINDOWS
    return lease->handle != INVALID_HANDLE_VALUE ? 1 : 0;
#else
    return lease->fd >= 0 ? 1 : 0;
#endif
}

/// @brief Explicitly release a FileLease; finalization is an equivalent fallback.
void rt_file_lease_release(void *handle) {
    rt_file_lease_impl *lease = rt_file_lease_checked(handle);
    if (!lease) {
        rt_trap("Zanna.IO.FileLease.Release: invalid lease");
        return;
    }
    rt_file_lease_close(lease);
}

/// What: Append @p text and a newline to @p path (creating it when missing).
/// Why:  Provide a convenient "append line" helper for Zanna.IO.File.
/// How:  Opens with O_APPEND and writes the UTF-8 bytes plus '\n' as one buffer,
///       using a whole-file byte-range lock on Windows.
/// @brief Append `text` and then an LF to the end of a file, creating it if absent.
/// @details The text and its trailing LF are combined into ONE buffer and
///          emitted with a single `write()` on the `O_APPEND` fd. On Windows,
///          a whole-file `LockFileEx` range serializes the full write loop
///          because CRT `_O_APPEND` descriptors can otherwise race. On POSIX each
///          `write()` to an `O_APPEND` file appends atomically at end-of-file,
///          so the whole line lands without interleaving another appender's
///          bytes — as long as the line fits in one write (VDOC-182: this
///          replaces the previous split text-then-newline writes, which could
///          interleave between the two calls). A partial write (a line exceeding
///          the OS single-write atomicity limit, or a signal) falls back to a
///          loop that CAN interleave with concurrent writers; for guaranteed
///          line-level atomicity across processes, use an explicit lock/record
///          protocol.
/// @param path Runtime string containing the destination path.
/// @param text Runtime string to append before the line-feed byte.
void rt_io_file_append_line(rt_string path, rt_string text) {
    const char *cpath =
        rt_io_file_require_path(path, "Zanna.IO.File.AppendLine: invalid file path");

    int fd = rt_fileext_open(cpath, O_WRONLY | O_CREAT | O_APPEND | RT_FILE_O_BINARY, 0666);
    if (fd < 0) {
        rt_trap("Zanna.IO.File.AppendLine: failed to open file");
        return;
    }

    const uint8_t *data = NULL;
    size_t len = rt_file_string_require_view(text, &data, "Zanna.IO.File.AppendLine: invalid text");

    // Combine the text and its LF so the common case is a single atomic append.
    if (len > SIZE_MAX - 1) {
        (void)close(fd);
        rt_trap("Zanna.IO.File.AppendLine: text too large");
        return;
    }
    uint8_t *line = (uint8_t *)malloc(len + 1);
    if (!line) {
        (void)close(fd);
        rt_trap("Zanna.IO.File.AppendLine: memory allocation failed");
        return;
    }
    if (len > 0)
        memcpy(line, data, len);
    line[len] = '\n';

#if RT_PLATFORM_WINDOWS
    OVERLAPPED lock_state;
    if (!rt_fileext_lock_append(fd, &lock_state)) {
        free(line);
        (void)close(fd);
        rt_trap("Zanna.IO.File.AppendLine: failed to lock file");
        return;
    }
#endif

    size_t total = len + 1;
    size_t written = 0;
    int write_failed = 0;
    while (written < total) {
        ssize_t n = rt_posix_write(fd, line + written, total - written);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            write_failed = 1;
            break;
        }
        if (n == 0) {
            write_failed = 1;
            break;
        }
        written += (size_t)n;
    }
#if RT_PLATFORM_WINDOWS
    if (!rt_fileext_unlock_append(fd, &lock_state))
        write_failed = 1;
#endif
    free(line);

    if (write_failed) {
        (void)close(fd);
        rt_trap("Zanna.IO.File.AppendLine: failed to write file");
        return;
    }

    rt_fileext_close_or_trap(fd, "Zanna.IO.File.AppendLine: failed to close file");
}

/// What: Read the entire file at @p path as a Bytes object.
/// Why:  Provide binary file input for Zanna.IO.File.ReadAllBytes.
/// How:  Reads the file into a temporary buffer and copies it into a new Bytes.
/// @brief Read the entire file as raw Bytes (no text decoding). Returns a Bytes object sized to
/// match the actual file length on disk.
/// @details The file must remain at least as large as its initial `fstat` size for the duration
///          of the read. Invalid paths, non-regular or oversized files, premature EOF, and I/O
///          or allocation failures trap.
/// @param path Runtime string containing the file path.
/// @return Fresh runtime Bytes object containing the exact file contents.
void *rt_io_file_read_all_bytes(rt_string path) {
    const char *cpath =
        rt_io_file_require_path(path, "Zanna.IO.File.ReadAllBytes: invalid file path");

    int fd = rt_fileext_open(cpath, O_RDONLY | RT_FILE_O_BINARY, 0);
    if (fd < 0) {
        rt_trap("Zanna.IO.File.ReadAllBytes: failed to open file");
        return NULL;
    }

    rt_fileext_stat_t st;
    if (rt_fileext_fstat(fd, &st) != 0) {
        (void)close(fd);
        rt_trap("Zanna.IO.File.ReadAllBytes: failed to stat file");
        return NULL;
    }
    if (!rt_fileext_is_regular_mode(st.st_mode)) {
        (void)close(fd);
        rt_trap("Zanna.IO.File.ReadAllBytes: path is not a regular file");
        return NULL;
    }
    if (st.st_size < 0 || (uint64_t)st.st_size > (uint64_t)SIZE_MAX) {
        (void)close(fd);
        rt_trap("Zanna.IO.File.ReadAllBytes: file too large");
        return NULL;
    }

    size_t size = (st.st_size > 0) ? (size_t)st.st_size : 0;
    if (size == 0) {
        rt_fileext_close_or_trap(fd, "Zanna.IO.File.ReadAllBytes: failed to close file");
        return rt_bytes_new(0);
    }

    uint8_t *buf = (uint8_t *)malloc(size);
    if (!buf) {
        (void)close(fd);
        rt_trap("Zanna.IO.File.ReadAllBytes: allocation failed");
        return NULL;
    }

    size_t off = 0;
    while (off < size) {
        ssize_t n = rt_posix_read(fd, buf + off, size - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            free(buf);
            (void)close(fd);
            rt_trap("Zanna.IO.File.ReadAllBytes: failed to read file");
            return NULL;
        }
        if (n == 0) {
            free(buf);
            (void)close(fd);
            rt_trap("Zanna.IO.File.ReadAllBytes: file changed while reading");
            return NULL;
        }
        off += (size_t)n;
    }
    if (close(fd) != 0) {
        free(buf);
        rt_trap("Zanna.IO.File.ReadAllBytes: failed to close file");
        return NULL;
    }

    /* O-02: Use memcpy into the raw bytes buffer instead of per-byte rt_bytes_set */
    void *bytes = rt_bytes_new((int64_t)size);
    if (!bytes) {
        free(buf);
        rt_trap("Zanna.IO.File.ReadAllBytes: memory allocation failed");
        return NULL;
    }
    uint8_t *dst = rt_bytes_data(bytes);
    if (dst)
        memcpy(dst, buf, size);

    free(buf);
    return bytes;
}

/// What: Write an entire Bytes object to @p path, overwriting the file.
/// Why:  Provide binary file output for Zanna.IO.File.WriteAllBytes.
/// How:  Writes bytes in chunks to avoid per-byte syscalls.
/// @brief Atomically write raw Bytes to `path`. Same atomic-replace semantics as `_write_all_text`
/// but skips text encoding conversion.
/// @details A non-null Bytes object with a representable length is required. Existing regular-file
///          permissions are preserved; invalid inputs and I/O failures trap.
/// @param path Runtime string containing the destination path.
/// @param bytes Runtime Bytes object whose complete payload should be written.
void rt_io_file_write_all_bytes(rt_string path, void *bytes) {
    const char *cpath =
        rt_io_file_require_path(path, "Zanna.IO.File.WriteAllBytes: invalid file path");

    if (!bytes) {
        rt_trap("Zanna.IO.File.WriteAllBytes: null Bytes");
        return;
    }

    /* IO-H-1: use raw data pointer instead of per-byte rt_bytes_get() —
       eliminates O(n) function calls in favour of a single write() */
    int64_t len = rt_bytes_len(bytes);
    if (len < 0 || (uint64_t)len > (uint64_t)SIZE_MAX) {
        rt_trap("Zanna.IO.File.WriteAllBytes: invalid Bytes length");
        return;
    }
    const uint8_t *src = rt_bytes_data_const(bytes);
    if (len > 0 && !src) {
        rt_trap("Zanna.IO.File.WriteAllBytes: invalid Bytes data");
        return;
    }
    if (!rt_fileext_write_atomic_utf8(cpath, src, (size_t)len, 1, 1))
        rt_trap("Zanna.IO.File.WriteAllBytes: failed to write file");
}

/// What: Read a text file and return a Seq of lines.
/// Why:  Provide convenient line-based file input for Zanna.IO.File.ReadAllLines.
/// How:  Reads the file and splits on LF, CR, and CRLF, stripping line terminators.
/// @brief Read a file, split on LF/CR/CRLF, return a Seq of rt_strings (one per line, no
/// trailing newline). Empty trailing lines are preserved (a file ending in `\n\n` yields a
/// trailing empty string).
/// @details The returned sequence owns its string elements. Empty files produce an empty sequence.
///          Invalid paths, non-regular or oversized files, concurrent truncation, I/O failures,
///          and allocation failures trap.
/// @param path Runtime string containing the file path.
/// @return Fresh owning Seq of runtime strings with terminators removed.
void *rt_io_file_read_all_lines(rt_string path) {
    const char *cpath =
        rt_io_file_require_path(path, "Zanna.IO.File.ReadAllLines: invalid file path");

    int fd = rt_fileext_open(cpath, O_RDONLY | RT_FILE_O_BINARY, 0);
    if (fd < 0) {
        rt_trap("Zanna.IO.File.ReadAllLines: failed to open file");
        return rt_seq_new();
    }

    rt_fileext_stat_t st;
    if (rt_fileext_fstat(fd, &st) != 0) {
        (void)close(fd);
        rt_trap("Zanna.IO.File.ReadAllLines: failed to stat file");
        return rt_seq_new();
    }
    if (!rt_fileext_is_regular_mode(st.st_mode)) {
        (void)close(fd);
        rt_trap("Zanna.IO.File.ReadAllLines: path is not a regular file");
        return rt_seq_new();
    }
    if (st.st_size < 0 || (uint64_t)st.st_size > (uint64_t)SIZE_MAX) {
        (void)close(fd);
        rt_trap("Zanna.IO.File.ReadAllLines: file too large");
        return rt_seq_new();
    }

    size_t size = (st.st_size > 0) ? (size_t)st.st_size : 0;
    if (size == 0) {
        rt_fileext_close_or_trap(fd, "Zanna.IO.File.ReadAllLines: failed to close file");
        void *empty = rt_seq_new();
        if (!empty)
            return NULL;
        rt_seq_set_owns_elements(empty, 1);
        return empty;
    }

    if (size > SIZE_MAX - 3) {
        (void)close(fd);
        rt_trap("Zanna.IO.File.ReadAllLines: file too large");
        return rt_seq_new();
    }
    char *buf = (char *)malloc(size + 3);
    if (!buf) {
        (void)close(fd);
        rt_trap("Zanna.IO.File.ReadAllLines: allocation failed");
        return rt_seq_new();
    }

    size_t off = 0;
    while (off < size) {
        ssize_t n = rt_posix_read(fd, buf + off, size - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            free(buf);
            (void)close(fd);
            rt_trap("Zanna.IO.File.ReadAllLines: failed to read file");
            return rt_seq_new();
        }
        if (n == 0) {
            free(buf);
            (void)close(fd);
            rt_trap("Zanna.IO.File.ReadAllLines: file changed while reading");
            return rt_seq_new();
        }
        off += (size_t)n;
    }
    if (close(fd) != 0) {
        free(buf);
        rt_trap("Zanna.IO.File.ReadAllLines: failed to close file");
        return rt_seq_new();
    }
    if (off > size) {
        free(buf);
        rt_trap("Zanna.IO.File.ReadAllLines: file changed while reading");
        return rt_seq_new();
    }
    buf[off] = '\0';
    buf[off + 1] = '\0';
    buf[off + 2] = '\0';

    void *seq = rt_seq_new();
    if (!seq) {
        free(buf);
        return NULL;
    }
    void *volatile owned_seq = seq;
    char *volatile owned_buf = buf;
    rt_string volatile line = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        rt_fileext_save_trap_error(
            saved_error, sizeof(saved_error), "Zanna.IO.File.ReadAllLines: failed to split lines");
        rt_trap_clear_recovery();
        if (line)
            rt_string_unref((rt_string)line);
        rt_fileext_release_object((void *)owned_seq);
        free((char *)owned_buf);
        rt_trap(saved_error);
        return rt_seq_new();
    }

    rt_seq_set_owns_elements(seq, 1);
    size_t i = 0;
    while (i < off) {
        size_t start = i;
        while (i < off && buf[i] != '\n' && buf[i] != '\r')
            ++i;
        size_t end = i;

        line = (end == start) ? rt_str_empty() : rt_string_from_bytes(buf + start, end - start);
        rt_seq_push(seq, (void *)line);
        rt_string_unref((rt_string)line);
        line = NULL;

        if (i >= off)
            break;
        if (buf[i] == '\r') {
            if (i + 1 < off && buf[i + 1] == '\n')
                i += 2;
            else
                i += 1;
        } else {
            ++i; // '\n'
        }
        if (i == off) {
            line = rt_str_empty();
            rt_seq_push(seq, (void *)line);
            rt_string_unref((rt_string)line);
            line = NULL;
        }
    }

    free(buf);
    owned_buf = NULL;
    rt_trap_clear_recovery();
    return seq;
}

/// What: Delete the file at @p path.
/// Why:  Allow simple cleanup without surfacing platform-specific APIs.
/// How:  Converts to host path and calls unlink(); errors are ignored.
/// @brief Delete a file. Trap if the path is null/empty; silently succeeds if the file is missing.
/// @details Any removal failure other than `ENOENT`, including attempting to delete a directory,
///          is reported through the runtime trap mechanism.
/// @param path Runtime string containing the file path to delete.
void rt_io_file_delete(rt_string path) {
    const char *cpath = rt_io_file_require_path(path, "Zanna.IO.File.Delete: invalid file path");
    if (rt_fileext_unlink(cpath) != 0 && errno != ENOENT)
        rt_trap("Zanna.IO.File.Delete: failed to delete file");
}

/// @brief Copy a regular file through a durable staging sidecar.
///
/// Copies via a staging temp file + atomic rename so a crash mid-copy
/// never leaves a truncated destination. Path validation, same-file
/// short-circuit, regular-file check, and non-clobber policing all
/// happen up front before any write. The transfer itself uses an 8KB
/// stack buffer — large enough to amortize syscall overhead on fast
/// storage, small enough to keep stack use predictable. Permission bits
/// and access/modification timestamps are copied from the source.
/// @param src Runtime string naming the source regular file.
/// @param dst Runtime string naming the destination.
/// @param replace Nonzero to replace an existing destination; zero to fail if it exists.
static void rt_file_copy_impl(rt_string src, rt_string dst, int replace) {
    const char *src_path = rt_io_file_require_path(src, "File.Copy: invalid source path");
    const char *dst_path = rt_io_file_require_path(dst, "File.Copy: invalid destination path");

    int src_fd = rt_fileext_open(src_path, O_RDONLY | RT_FILE_O_BINARY, 0);
    if (src_fd < 0) {
        char msg[512];
        snprintf(
            msg, sizeof(msg), "File.Copy: cannot open source '%s': %s", src_path, strerror(errno));
        rt_trap(msg);
        return;
    }

    if (rt_fileext_same_existing_file(src_path, dst_path)) {
        close(src_fd);
        rt_trap("File.Copy: source and destination are the same file");
        return;
    }

    rt_fileext_stat_t src_st;
    if (rt_fileext_fstat(src_fd, &src_st) != 0 || !rt_fileext_is_regular_mode(src_st.st_mode)) {
        close(src_fd);
        rt_trap("File.Copy: source is not a regular file");
        return;
    }

    if (!replace) {
        rt_fileext_stat_t dst_st;
#if RT_PLATFORM_WINDOWS
        if (rt_fileext_stat_path(dst_path, &dst_st) == 0) {
#else
        if (lstat(dst_path, &dst_st) == 0) {
#endif
            close(src_fd);
            rt_trap("File.Copy: destination already exists");
            return;
        }
    }

    char *tmp_path = NULL;
    int dst_fd = rt_fileext_open_temp_utf8(dst_path, 1, &tmp_path);
    if (dst_fd < 0) {
        close(src_fd);
        char msg[512];
        snprintf(msg,
                 sizeof(msg),
                 "File.Copy: cannot create temporary destination for '%s': %s",
                 dst_path,
                 strerror(errno));
        rt_trap(msg);
        return;
    }

    char buf[8192];
    for (;;) {
        ssize_t n = rt_posix_read(src_fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            close(src_fd);
            close(dst_fd);
            if (tmp_path)
                (void)rt_fileext_unlink(tmp_path);
            free(tmp_path);
            rt_trap("File.Copy: read error");
            return;
        }
        if (n == 0)
            break;
        size_t written = 0;
        while (written < (size_t)n) {
            ssize_t w = rt_posix_write(dst_fd, buf + written, (size_t)n - written);
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                close(src_fd);
                close(dst_fd);
                if (tmp_path)
                    (void)rt_fileext_unlink(tmp_path);
                free(tmp_path);
                rt_trap("File.Copy: write error (disk full or I/O error)");
                return;
            }
            if (w == 0) {
                close(src_fd);
                close(dst_fd);
                if (tmp_path)
                    (void)rt_fileext_unlink(tmp_path);
                free(tmp_path);
                rt_trap("File.Copy: write error (zero-byte write)");
                return;
            }
            written += (size_t)w;
        }
    }

    int ok = 1;
    if (!rt_fileext_apply_mode_to_open_file(dst_fd, &src_st))
        ok = 0;
#if RT_PLATFORM_WINDOWS
    if (_commit(dst_fd) != 0)
        ok = 0;
#else
    if (fsync(dst_fd) != 0)
        ok = 0;
#endif
    if (close(src_fd) != 0)
        ok = 0;
    if (close(dst_fd) != 0)
        ok = 0;
    if (ok)
        ok = rt_fileext_commit_utf8(tmp_path, dst_path, replace);
    if (ok)
        ok = rt_fileext_apply_mode_to_path(dst_path, &src_st);
    if (ok)
        ok = rt_fileext_apply_timestamps(dst_path, &src_st);
    if (ok)
        ok = rt_fileext_sync_parent_dir(dst_path);
    if (!ok) {
        if (tmp_path)
            (void)rt_fileext_unlink(tmp_path);
        free(tmp_path);
        rt_trap("File.Copy: failed to commit destination");
        return;
    }
    free(tmp_path);
}

/// What: Copy a file from @p src to @p dst.
/// Why:  Allow file duplication without platform-specific APIs.
/// How:  Reads src file and writes to dst file.
/// @brief Copy file `src` to `dst`. Streams in chunks to avoid loading the whole file into RAM
/// (important for large files). Traps if the destination already exists.
/// @details The source must be a regular file distinct from @p dst. The committed copy preserves
///          source permission bits and access/modification timestamps.
/// @param src Runtime string naming the source file.
/// @param dst Runtime string naming a destination that must not already exist.
void rt_file_copy(rt_string src, rt_string dst) {
    rt_file_copy_impl(src, dst, 0);
}

/// @brief Move a regular file, optionally replacing the destination.
/// @details First attempts a same-filesystem rename. On a cross-device error, copies through an
///          atomic staging file with source metadata preserved, then removes the source. Invalid
///          paths, non-regular or identical operands, destination-policy violations, and any
///          rename/copy/removal failure trap.
/// @param src Runtime string naming the source file.
/// @param dst Runtime string naming the destination.
/// @param replace Nonzero to replace an existing destination; zero to require it to be absent.
static void rt_file_move_impl(rt_string src, rt_string dst, int replace) {
    const char *src_path = rt_io_file_require_path(src, "Zanna.IO.File.Move: invalid source path");
    const char *dst_path =
        rt_io_file_require_path(dst, "Zanna.IO.File.Move: invalid destination path");

    rt_fileext_stat_t src_st;
    if (rt_fileext_stat_path(src_path, &src_st) != 0 ||
        !rt_fileext_is_regular_mode(src_st.st_mode)) {
        rt_trap("File.Move: source is not a regular file");
        return;
    }

    if (rt_fileext_same_existing_file(src_path, dst_path)) {
        rt_trap("File.Move: source and destination are the same file");
        return;
    }

    if (rt_fileext_commit_utf8(src_path, dst_path, replace))
        return;

#if RT_PLATFORM_WINDOWS
    DWORD move_err = GetLastError();
    if (move_err != ERROR_NOT_SAME_DEVICE) {
        rt_trap("File.Move: failed to move file");
        return;
    }
#else
    if (errno != EXDEV) {
        rt_trap("File.Move: failed to move file");
        return;
    }
#endif

    rt_file_copy_impl(src, dst, replace);
    if (rt_fileext_unlink(src_path) != 0)
        rt_trap("File.Move: failed to remove source after cross-device copy");
}

/// @brief Move file `src` to `dst` without replacing an existing destination.
/// @param src Runtime string naming the source regular file.
/// @param dst Runtime string naming a destination that must not already exist.
void rt_file_move(rt_string src, rt_string dst) {
    rt_file_move_impl(src, dst, 0);
}

/// @brief Move file `src` to `dst`, replacing any existing destination.
/// @param src Runtime string naming the source regular file.
/// @param dst Runtime string naming the destination to create or replace.
void rt_file_move_over(rt_string src, rt_string dst) {
    rt_file_move_impl(src, dst, 1);
}

/// What: Get the size of a file in bytes.
/// Why:  Allow querying file size without opening the file.
/// How:  Uses stat() to get file size.
/// @brief Return the size of `path` in bytes (via `stat`). -1 on missing/non-regular path.
/// @details This query is non-trapping and follows the platform `stat` behavior for symlinks.
/// @param path Runtime string containing the path to inspect.
/// @return Nonnegative regular-file size, or -1 for an invalid, inaccessible, missing, or
///         non-regular path.
int64_t rt_file_size(rt_string path) {
    const char *cpath = NULL;
    if (!rt_file_path_from_vstr(path, &cpath) || !cpath)
        return -1;

    rt_fileext_stat_t st;
    if (rt_fileext_stat_path(cpath, &st) != 0)
        return -1;
    if (!rt_fileext_is_regular_mode(st.st_mode))
        return -1;

    return (int64_t)st.st_size;
}

/// What: Read entire file as a Bytes object.
/// Why:  Support binary file reading.
/// How:  Opens file, reads all bytes, returns Bytes object.
/// @brief Alias for `rt_io_file_read_all_bytes` — reads the whole file as a Bytes object.
/// @param path Runtime string containing the file path.
/// @return Fresh runtime Bytes object containing the complete file.
void *rt_file_read_bytes(rt_string path) {
    return rt_io_file_read_all_bytes(path);
}

/// What: Write a Bytes object to a file.
/// Why:  Support binary file writing.
/// How:  Opens file, writes all bytes from Bytes object using chunked writes.
/// @brief Alias for `rt_io_file_write_all_bytes` — atomically write Bytes to disk.
/// @param path Runtime string containing the destination path.
/// @param bytes Runtime Bytes object to write.
void rt_file_write_bytes(rt_string path, void *bytes) {
    rt_io_file_write_all_bytes(path, bytes);
}

/// What: Read entire file as a sequence of lines.
/// Why:  Support line-by-line text file reading.
/// How:  Reads file, splits by newlines, returns Seq of strings.
/// @brief Alias for `rt_io_file_read_all_lines` — read the file split into lines.
/// @param path Runtime string containing the file path.
/// @return Fresh owning Seq of runtime strings with line terminators removed.
void *rt_file_read_lines(rt_string path) {
    return rt_io_file_read_all_lines(path);
}

/// What: Write a sequence of strings to a file as lines.
/// Why:  Support line-by-line text file writing.
/// How:  Writes each string followed by newline.
/// @brief Atomically write a Seq of strings as lines (joined with LF). Each element becomes one
/// line; trailing newline is added to the final line so future appends concatenate correctly.
/// @details Every element is validated before the sidecar is opened. An empty sequence writes an
///          empty file. Existing regular-file permissions are preserved; invalid elements and
///          I/O failures trap.
/// @param path Runtime string containing the destination path.
/// @param lines Non-null runtime Seq whose elements must all be strings.
void rt_file_write_lines(rt_string path, void *lines) {
    const char *cpath =
        rt_io_file_require_path(path, "Zanna.IO.File.WriteLines: invalid file path");

    rt_fileext_stat_t replaced_st;
    int preserve_mode = rt_fileext_snapshot_replaced_file(cpath, &replaced_st);

    if (!lines) {
        rt_trap("Zanna.IO.File.WriteLines: null lines");
        return;
    }

    int64_t count = rt_seq_len(lines);
    for (int64_t i = 0; i < count; i++) {
        const uint8_t *unused = NULL;
        (void)rt_file_string_require_view(
            (rt_string)rt_seq_get(lines, i), &unused, "Zanna.IO.File.WriteLines: invalid line");
    }

    char *tmp_path = NULL;
    int fd = rt_fileext_open_temp_utf8(cpath, 1, &tmp_path);
    if (fd < 0) {
        rt_trap("Zanna.IO.File.WriteLines: failed to open file");
        return;
    }

    int ok = 1;
    for (int64_t i = 0; i < count; i++) {
        rt_string line = (rt_string)rt_seq_get(lines, i);
        const uint8_t *data = NULL;
        size_t len =
            rt_file_string_require_view(line, &data, "Zanna.IO.File.WriteLines: invalid line");
        if (!rt_fileext_write_all_fd(fd, data, len)) {
            ok = 0;
            break;
        }
        // Write newline
        char nl = '\n';
        if (!rt_fileext_write_all_fd(fd, (const uint8_t *)&nl, 1)) {
            ok = 0;
            break;
        }
    }

#if RT_PLATFORM_WINDOWS
    if (ok && _commit(fd) != 0)
        ok = 0;
#else
    if (ok && preserve_mode && !rt_fileext_apply_mode_to_open_file(fd, &replaced_st))
        ok = 0;
    if (ok && fsync(fd) != 0)
        ok = 0;
#endif
    if (close(fd) != 0)
        ok = 0;
#if RT_PLATFORM_WINDOWS
    if (ok && preserve_mode && !rt_fileext_apply_mode_to_path(tmp_path, &replaced_st))
        ok = 0;
#endif
    if (ok)
        ok = rt_fileext_replace_utf8(tmp_path, cpath);
    if (ok)
        ok = rt_fileext_sync_parent_dir(cpath);
    if (!ok) {
        (void)rt_fileext_unlink(tmp_path);
        free(tmp_path);
        rt_trap("Zanna.IO.File.WriteLines: failed to write file");
        return;
    }
    free(tmp_path);
}

/// @brief Alias for `rt_file_write_lines` using the Zanna.IO.File.WriteAllLines spelling.
/// @param path Runtime string containing the destination path.
/// @param lines Non-null runtime Seq whose elements must all be strings.
void rt_io_file_write_all_lines(rt_string path, void *lines) {
    rt_file_write_lines(path, lines);
}

/// What: Append text to an existing file.
/// Why:  Support appending without reading+writing entire file.
/// How:  Opens file with O_APPEND and writes text.
/// @brief Append `text` (no newline added) to the end of a file. Like `_append_line` but doesn't
/// add a trailing LF — useful for binary-style appends.
/// @details Creates the file when absent and writes the runtime string's bytes verbatim. Invalid
///          inputs and open, write, or close failures trap.
/// @param path Runtime string containing the destination path.
/// @param text Runtime string whose bytes should be appended.
void rt_file_append(rt_string path, rt_string text) {
    const char *cpath = rt_io_file_require_path(path, "Zanna.IO.File.Append: invalid file path");

    int fd = rt_fileext_open(cpath, O_WRONLY | O_CREAT | O_APPEND | RT_FILE_O_BINARY, 0666);
    if (fd < 0) {
        rt_trap("Zanna.IO.File.Append: failed to open file");
        return;
    }

    const uint8_t *data = NULL;
    size_t len = rt_file_string_require_view(text, &data, "Zanna.IO.File.Append: invalid text");
    if (!rt_fileext_write_all_fd(fd, data, len)) {
        close(fd);
        rt_trap("Zanna.IO.File.Append: failed to write file");
        return;
    }

    rt_fileext_close_or_trap(fd, "Zanna.IO.File.Append: failed to close file");
}

/// What: Get file modification time as Unix timestamp.
/// Why:  Support querying when a file was last modified.
/// How:  Uses stat() to get mtime.
/// @brief Return the file's mtime as Unix epoch seconds (`stat.st_mtime`). -1 if missing.
/// @details This query is non-trapping and rejects directories and other non-regular objects.
/// @param path Runtime string containing the path to inspect.
/// @return Modification time in Unix epoch seconds, or -1 for an invalid, inaccessible, missing,
///         or non-regular path.
int64_t rt_file_modified(rt_string path) {
    const char *cpath = NULL;
    if (!rt_file_path_from_vstr(path, &cpath) || !cpath)
        return -1;

    rt_fileext_stat_t st;
    if (rt_fileext_stat_path(cpath, &st) != 0)
        return -1;
    if (!rt_fileext_is_regular_mode(st.st_mode))
        return -1;

    return (int64_t)st.st_mtime;
}

/// What: Create file or update modification time.
/// Why:  Support "touch" semantics from Unix.
/// How:  Creates file if not exists, updates mtime if exists.
/// @brief Update the file's mtime+atime to "now" (`utime(NULL)`). Creates an empty file if it
/// doesn't exist. Mirrors the Unix `touch` command.
/// @details Existing non-regular paths are rejected. Invalid paths and timestamp, creation, or
///          close failures trap.
/// @param path Runtime string containing the regular-file path to update or create.
void rt_file_touch(rt_string path) {
    const char *cpath = rt_io_file_require_path(path, "Zanna.IO.File.Touch: invalid file path");

    // Touch is a FILE operation: reject an existing directory rather than
    // mutating its timestamps, matching the rest of the File surface which
    // requires a regular-file operand (VDOC-183).
    rt_fileext_stat_t existing;
    if (rt_fileext_stat_path(cpath, &existing) == 0 &&
        !rt_fileext_is_regular_mode(existing.st_mode)) {
        rt_trap("Zanna.IO.File.Touch: path is not a regular file");
        return;
    }

    // Try to update mtime (works if file exists)
    if (rt_fileext_utime(cpath, NULL) == 0)
        return;
    if (errno != ENOENT) {
        rt_trap("Zanna.IO.File.Touch: failed to update file time");
        return;
    }

    // File doesn't exist, create it
    int fd = rt_fileext_open(cpath, O_WRONLY | O_CREAT | RT_FILE_O_BINARY, 0666);
    if (fd < 0) {
        rt_trap("Zanna.IO.File.Touch: failed to create file");
        return;
    }
    rt_fileext_close_or_trap(fd, "Zanna.IO.File.Touch: failed to close file");
}
