//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/io/rt_archive_fs.c
// Purpose: Filesystem and atomic-write adapter for the archive runtime. Holds
//          the Win32/POSIX file I/O primitives (exact reads, temp-file create +
//          rename, directory creation, symlink rejection) used by archive
//          reading/extraction. Split out of rt_archive.c.
//
// Key invariants:
//   - Approved io/ platform-adapter layer: raw _WIN32 branching is permitted.
//   - Atomic writes go through a temp file + rename and fsync the parent dir.
//   - Path components are checked for reparse points / symlinks so extraction
//     cannot escape the destination root.
//
// Ownership/Lifetime:
//   - Borrows caller-owned paths/handles; Bytes-producing helpers return fresh
//     GC objects owned by the caller. Closes any handle/fd it opens.
//
// Links: src/runtime/io/rt_archive.c (core/ZIP/API),
//        src/runtime/io/rt_archive_internal.h (shared contract)
//
//===----------------------------------------------------------------------===//
/**
 * @file
 * @brief Implements native filesystem and atomic-write services for archives.
 * @details Provides reader-writer locks, exact bounded reads, secure
 * extraction beneath verified roots, adjacent temporary-file creation,
 * durable replacement, UTF-8 path adaptation, and symlink or reparse-point
 * rejection in the approved platform boundary.
 */

#include "rt_archive.h"
#include "rt_archive_internal.h"

#include "network/rt_entropy_platform.h"
#include "rt_box.h"
#include "rt_bytes.h"
#include "rt_dir.h"
#include "rt_file_path.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_string.h"

#ifdef _WIN32
#ifndef _CRT_RAND_S
#define _CRT_RAND_S
#endif
#endif

#include <errno.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#define PATH_SEP '\\'
#else
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#define PATH_SEP '/'
#endif

/// @copydoc rt_trap_set_recovery()
void rt_trap_set_recovery(jmp_buf *buf);
/// @copydoc rt_trap_clear_recovery()
void rt_trap_clear_recovery(void);
/// @copydoc rt_trap_get_error()
const char *rt_trap_get_error(void);

// Trivial Bytes accessors — defined per-TU as static inline (not shared via the
// internal header) to avoid generic-name link collisions and rtgen header scan.
/// @brief Direct pointer to the raw byte buffer of a Bytes GC object.
/// @param obj Runtime Bytes handle.
/// @return Borrowed pointer to the object's byte storage.
static inline uint8_t *bytes_data(void *obj) {
    return rt_bytes_data(obj);
}

/// @brief Byte count of a Bytes GC object.
/// @param obj Runtime Bytes handle.
/// @return Signed byte length reported by the Bytes API.
static inline int64_t bytes_len(void *obj) {
    return rt_bytes_len(obj);
}

//=============================================================================
// Filesystem / atomic-write helpers
//=============================================================================

/// @brief Allocate and initialize a native archive reader-writer lock.
/// @details The implementation stays in this approved platform-adapter unit:
///          SRWLOCK on Windows and pthread_rwlock_t on POSIX. The native lock
///          creates no managed object or GC edge.
/// @return Opaque initialized lock, or NULL on allocation/initialization failure.
void *archive_rwlock_create(void) {
#ifdef _WIN32
    SRWLOCK *lock = (SRWLOCK *)malloc(sizeof(*lock));
    if (!lock)
        return NULL;
    InitializeSRWLock(lock);
    return lock;
#else
    pthread_rwlock_t *lock = (pthread_rwlock_t *)malloc(sizeof(*lock));
    if (!lock)
        return NULL;
    if (pthread_rwlock_init(lock, NULL) != 0) {
        free(lock);
        return NULL;
    }
    return lock;
#endif
}

/// @brief Destroy and free an archive reader-writer lock after archive quiescence.
/// @details Finalization follows the last archive release, so a busy lock is an
///          invariant violation rather than a recoverable runtime condition.
/// @param lock Opaque lock returned by @ref archive_rwlock_create; NULL is a no-op.
void archive_rwlock_destroy(void *lock) {
    if (!lock)
        return;
#ifndef _WIN32
    if (pthread_rwlock_destroy((pthread_rwlock_t *)lock) != 0)
        rt_abort("Archive: failed to destroy reader-writer lock");
#endif
    free(lock);
}

/// @brief Acquire shared access to immutable/read-only archive state.
/// @param lock Opaque archive lock; NULL indicates runtime corruption.
void archive_rwlock_read_enter(void *lock) {
    if (!lock)
        rt_abort("Archive: missing reader-writer lock");
#ifdef _WIN32
    AcquireSRWLockShared((SRWLOCK *)lock);
#else
    if (pthread_rwlock_rdlock((pthread_rwlock_t *)lock) != 0)
        rt_abort("Archive: failed to acquire read lock");
#endif
}

/// @brief Release shared archive access.
/// @param lock Opaque archive lock currently held for reading.
void archive_rwlock_read_exit(void *lock) {
    if (!lock)
        rt_abort("Archive: missing reader-writer lock");
#ifdef _WIN32
    ReleaseSRWLockShared((SRWLOCK *)lock);
#else
    if (pthread_rwlock_unlock((pthread_rwlock_t *)lock) != 0)
        rt_abort("Archive: failed to release read lock");
#endif
}

/// @brief Acquire exclusive access to mutable archive writer state.
/// @param lock Opaque archive lock; NULL indicates runtime corruption.
void archive_rwlock_write_enter(void *lock) {
    if (!lock)
        rt_abort("Archive: missing reader-writer lock");
#ifdef _WIN32
    AcquireSRWLockExclusive((SRWLOCK *)lock);
#else
    if (pthread_rwlock_wrlock((pthread_rwlock_t *)lock) != 0)
        rt_abort("Archive: failed to acquire write lock");
#endif
}

/// @brief Release exclusive archive writer access.
/// @param lock Opaque archive lock currently held for writing.
void archive_rwlock_write_exit(void *lock) {
    if (!lock)
        rt_abort("Archive: missing reader-writer lock");
#ifdef _WIN32
    ReleaseSRWLockExclusive((SRWLOCK *)lock);
#else
    if (pthread_rwlock_unlock((pthread_rwlock_t *)lock) != 0)
        rt_abort("Archive: failed to release write lock");
#endif
}

#ifdef _WIN32
/// @brief Open a UTF-8 path on Windows via the wide-string CreateFileW API.
///
/// Translates the UTF-8 input to UTF-16 with the long-path-aware helper
/// from `rt_file_path`, calls CreateFileW, and frees the wide buffer.
/// Returns INVALID_HANDLE_VALUE on conversion failure or when the
/// underlying open fails — the caller is expected to check.
///
/// @param cpath        UTF-8 file path.
/// @param access       Win32 access flags (e.g., GENERIC_READ).
/// @param share        Win32 share mode.
/// @param create_disp  Win32 creation disposition (OPEN_EXISTING, CREATE_ALWAYS, ...).
/// @return Open Windows file handle, or INVALID_HANDLE_VALUE on failure.
HANDLE archive_open_win_path(const char *cpath, DWORD access, DWORD share, DWORD create_disp) {
    wchar_t *wide = rt_file_path_utf8_to_wide(cpath);
    if (!wide)
        return INVALID_HANDLE_VALUE;
    HANDLE h = CreateFileW(wide, access, share, NULL, create_disp, FILE_ATTRIBUTE_NORMAL, NULL);
    free(wide);
    return h;
}

/// @brief Read exactly `total` bytes from a Windows handle or trap.
///
/// Loops over `ReadFile` until the requested count is satisfied,
/// chunking each call to fit within DWORD_MAX. A short read that
/// reports zero bytes is treated as EOF and triggers the supplied
/// trap message — the archive parser must read complete records.
///
/// @param h        Open Windows file handle (positioned by caller).
/// @param dst      Destination buffer of at least `total` bytes.
/// @param total    Total number of bytes to read.
/// @param trap_msg Trap message used on read failure / premature EOF.
/// @return 1 after reading exactly @p total bytes, or 0 after raising a trap.
static int archive_read_exact_win(HANDLE h, uint8_t *dst, size_t total, const char *trap_msg) {
    size_t read_total = 0;
    while (read_total < total) {
        DWORD chunk = 0;
        size_t remaining = total - read_total;
        DWORD want = remaining > (size_t)UINT32_MAX ? (DWORD)UINT32_MAX : (DWORD)remaining;
        if (!ReadFile(h, dst + read_total, want, &chunk, NULL) || chunk == 0) {
            rt_trap(trap_msg);
            return 0;
        }
        read_total += (size_t)chunk;
    }
    return 1;
}

/// @brief Read a complete Windows file payload and close its handle.
/// @details On any read trap, captures the diagnostic, closes @p h, frees
/// @p dst, and re-raises. Successful completion closes the handle but leaves
/// @p dst owned by the caller.
/// @param h Open Windows handle positioned at the first requested byte.
/// @param dst Heap buffer of at least @p total bytes; freed on failure.
/// @param total Exact number of bytes to read.
/// @param trap_msg Fallback diagnostic for read failure.
/// @return 1 on success, or 0 after cleanup and trap propagation.
int archive_read_exact_win_or_free(HANDLE h, uint8_t *dst, size_t total, const char *trap_msg) {
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        archive_save_trap_error(saved_error, sizeof(saved_error), trap_msg);
        rt_trap_clear_recovery();
        CloseHandle(h);
        free(dst);
        rt_trap(saved_error);
        return 0;
    }

    if (!archive_read_exact_win(h, dst, total, trap_msg)) {
        rt_trap_clear_recovery();
        CloseHandle(h);
        free(dst);
        return 0;
    }
    rt_trap_clear_recovery();
    CloseHandle(h);
    return 1;
}

/// @brief Fill a Bytes object from a Windows handle with transactional cleanup.
/// @details Always closes @p h. Failure also releases @p bytes before the
/// captured read diagnostic is raised again.
/// @param h Open Windows handle positioned at the first requested byte.
/// @param bytes Temporary Bytes object receiving the file contents.
/// @param total Exact number of bytes to read into the object.
/// @param trap_msg Fallback diagnostic for read failure.
/// @return 1 on success, or 0 after cleanup and trap propagation.
int archive_read_exact_win_or_release_object(HANDLE h,
                                             void *bytes,
                                             size_t total,
                                             const char *trap_msg) {
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        archive_save_trap_error(saved_error, sizeof(saved_error), trap_msg);
        rt_trap_clear_recovery();
        CloseHandle(h);
        archive_release_temp_object(bytes);
        rt_trap(saved_error);
        return 0;
    }

    if (!bytes || !archive_read_exact_win(h, bytes_data(bytes), total, trap_msg)) {
        rt_trap_clear_recovery();
        CloseHandle(h);
        archive_release_temp_object(bytes);
        return 0;
    }
    rt_trap_clear_recovery();
    CloseHandle(h);
    return 1;
}

/// @brief Allocate a Bytes object while protecting an already-open Windows handle.
/// @details If allocation fails or traps, closes @p h before propagating the
/// diagnostic. On success the handle remains open for the caller to read into
/// the returned object.
/// @param h Open Windows handle to close only on allocation failure.
/// @param len Requested Bytes length.
/// @param fallback Diagnostic used if allocation traps without a message.
/// @return Fresh Bytes handle on success, or `NULL` after cleanup on failure.
void *archive_bytes_new_win_or_close(HANDLE h, int64_t len, const char *fallback) {
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        archive_save_trap_error(saved_error, sizeof(saved_error), fallback);
        rt_trap_clear_recovery();
        CloseHandle(h);
        rt_trap(saved_error);
        return NULL;
    }

    void *data = rt_bytes_new(len);
    if (!data) {
        rt_trap_clear_recovery();
        CloseHandle(h);
        return NULL;
    }
    rt_trap_clear_recovery();
    return data;
}

/// @brief Attempt to write exactly `total` bytes to a Windows handle.
///
/// Mirror of `archive_read_exact_win` for the write path. Loops over
/// WriteFile, chunking by DWORD_MAX, and traps on any short write or
/// failure. It deliberately does not trap while @p h is live: callers first
/// close the handle and unlink the sidecar, then raise @p trap_msg.
/// @param h Open Windows handle positioned for output.
/// @param src Source buffer containing @p total bytes.
/// @param total Exact number of bytes to write.
/// @param trap_msg Reserved caller diagnostic; cleanup code raises it after
/// this helper reports failure.
/// @return 1 after all bytes are written; otherwise 0.
static int archive_write_exact_win(HANDLE h,
                                   const uint8_t *src,
                                   size_t total,
                                   const char *trap_msg) {
    (void)trap_msg;
    size_t written_total = 0;
    while (written_total < total) {
        DWORD chunk = 0;
        size_t remaining = total - written_total;
        DWORD want = remaining > (size_t)UINT32_MAX ? (DWORD)UINT32_MAX : (DWORD)remaining;
        if (!WriteFile(h, src + written_total, want, &chunk, NULL) || chunk == 0)
            return 0;
        written_total += (size_t)chunk;
    }
    return 1;
}
#else
/// @brief Open a POSIX path with close-on-exec semantics when available.
/// @param path NUL-terminated filesystem path.
/// @param flags POSIX `open` flags; `O_CLOEXEC` may be added.
/// @param mode Creation mode used when required by @p flags.
/// @return Open file descriptor, or -1 with `errno` set.
int archive_open_posix(const char *path, int flags, mode_t mode) {
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int fd = open(path, flags, mode);
#if defined(FD_CLOEXEC) && !defined(O_CLOEXEC)
    if (fd >= 0) {
        int fd_flags = fcntl(fd, F_GETFD);
        if (fd_flags >= 0)
            (void)fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);
    }
#endif
    return fd;
}

/// @brief Read exactly `total` bytes from a POSIX fd or trap.
///
/// Loops over `read(2)` retrying EINTR. A zero return is treated as
/// EOF and triggers the trap so partial archive reads cannot succeed
/// silently. The fd is left positioned just after the last byte read.
///
/// @param fd Open descriptor positioned at the first requested byte.
/// @param dst Destination buffer of at least @p total bytes.
/// @param total Exact byte count to read.
/// @param trap_msg Diagnostic raised on error or premature EOF.
/// @return 1 after reading all bytes, or 0 after raising a trap.
static int archive_read_exact_posix(int fd, uint8_t *dst, size_t total, const char *trap_msg) {
    size_t read_total = 0;
    while (read_total < total) {
        ssize_t n = read(fd, dst + read_total, total - read_total);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            rt_trap(trap_msg);
            return 0;
        }
        if (n == 0) {
            rt_trap(trap_msg);
            return 0;
        }
        read_total += (size_t)n;
    }
    return 1;
}

/// @brief Read a complete POSIX file payload and close its descriptor.
/// @details On any read trap, captures the diagnostic, closes @p fd, frees
/// @p dst, and re-raises. Success closes the descriptor but leaves @p dst
/// owned by the caller.
/// @param fd Open descriptor positioned at the first requested byte.
/// @param dst Heap buffer of at least @p total bytes; freed on failure.
/// @param total Exact number of bytes to read.
/// @param trap_msg Fallback diagnostic for read failure.
/// @return 1 on success, or 0 after cleanup and trap propagation.
int archive_read_exact_posix_or_free(int fd, uint8_t *dst, size_t total, const char *trap_msg) {
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        archive_save_trap_error(saved_error, sizeof(saved_error), trap_msg);
        rt_trap_clear_recovery();
        close(fd);
        free(dst);
        rt_trap(saved_error);
        return 0;
    }

    if (!archive_read_exact_posix(fd, dst, total, trap_msg)) {
        rt_trap_clear_recovery();
        close(fd);
        free(dst);
        return 0;
    }
    rt_trap_clear_recovery();
    close(fd);
    return 1;
}

/// @brief Fill a Bytes object from a POSIX descriptor with transactional cleanup.
/// @details Always closes @p fd. Failure also releases @p bytes before the
/// captured read diagnostic is raised again.
/// @param fd Open descriptor positioned at the first requested byte.
/// @param bytes Temporary Bytes object receiving the file contents.
/// @param total Exact number of bytes to read into the object.
/// @param trap_msg Fallback diagnostic for read failure.
/// @return 1 on success, or 0 after cleanup and trap propagation.
int archive_read_exact_posix_or_release_object(int fd,
                                               void *bytes,
                                               size_t total,
                                               const char *trap_msg) {
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        archive_save_trap_error(saved_error, sizeof(saved_error), trap_msg);
        rt_trap_clear_recovery();
        close(fd);
        archive_release_temp_object(bytes);
        rt_trap(saved_error);
        return 0;
    }

    if (!bytes || !archive_read_exact_posix(fd, bytes_data(bytes), total, trap_msg)) {
        rt_trap_clear_recovery();
        close(fd);
        archive_release_temp_object(bytes);
        return 0;
    }
    rt_trap_clear_recovery();
    close(fd);
    return 1;
}

/// @brief Allocate a Bytes object while protecting an already-open POSIX descriptor.
/// @details If allocation fails or traps, closes @p fd before propagating the
/// diagnostic. On success the descriptor remains open for the caller to read
/// into the returned object.
/// @param fd Open descriptor to close only on allocation failure.
/// @param len Requested Bytes length.
/// @param fallback Diagnostic used if allocation traps without a message.
/// @return Fresh Bytes handle on success, or `NULL` after cleanup on failure.
void *archive_bytes_new_posix_or_close(int fd, int64_t len, const char *fallback) {
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        archive_save_trap_error(saved_error, sizeof(saved_error), fallback);
        rt_trap_clear_recovery();
        close(fd);
        rt_trap(saved_error);
        return NULL;
    }

    void *data = rt_bytes_new(len);
    if (!data) {
        rt_trap_clear_recovery();
        close(fd);
        return NULL;
    }
    rt_trap_clear_recovery();
    return data;
}

/// @brief Attempt to write exactly `total` bytes to a POSIX fd.
///
/// Mirror of `archive_read_exact_posix` for writes. Retries EINTR,
/// reports error or unexpected zero-byte writes (the kernel never returns
/// zero on a regular file write unless the disk is full). It deliberately
/// does not trap while @p fd is live, allowing callers to close and unlink
/// their sidecar before raising @p trap_msg.
/// @param fd Open descriptor positioned for output.
/// @param src Source buffer containing @p total bytes.
/// @param total Exact number of bytes to write.
/// @param trap_msg Reserved caller diagnostic; cleanup code raises it after
/// this helper reports failure.
/// @return 1 after all bytes are written; otherwise 0.
static int archive_write_exact_posix(int fd,
                                     const uint8_t *src,
                                     size_t total,
                                     const char *trap_msg) {
    (void)trap_msg;
    size_t written_total = 0;
    while (written_total < total) {
        ssize_t n = write(fd, src + written_total, total - written_total);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return 0;
        }
        if (n == 0)
            return 0;
        written_total += (size_t)n;
    }
    return 1;
}
#endif

/// @brief Read 64 bits from platform secure randomness for archive temp names.
/// @details Returns failure instead of falling back to pid/time-derived values; those values are
///          collision context only and are not suitable entropy for exclusive temp names.
/// @param out Receives the random value on success.
/// @return 1 on success, 0 when OS entropy is unavailable.
static int archive_random_u64(uint64_t *out) {
    if (!out)
        return 0;
    if (rt_entropy_platform_random_u64(out) != 0)
        return 0;
    return 1;
}

/// @brief Build a unique temporary file path adjacent to `path`.
///
/// Constructs a sidecar name in the same directory as `path` using secure
/// random entropy plus process ID and `attempt` as collision context.
/// The caller must `free()` the returned string. Returns NULL on allocation
/// failure or path length overflow.
///
/// @param path    Base path whose parent directory receives the temp file.
/// @param attempt Iteration counter to differentiate multiple tries.
/// @return Heap-allocated temp path, or NULL on failure.
char *archive_make_temp_path(const char *path, unsigned attempt) {
    size_t path_len = strlen(path);
    size_t parent_len = 0;
    for (size_t i = 0; i < path_len; ++i) {
#ifdef _WIN32
        if (path[i] == '/' || path[i] == '\\')
#else
        if (path[i] == '/')
#endif
            parent_len = i + 1;
    }

    char nonce[17];
    uint64_t random_value = 0;
    if (!archive_random_u64(&random_value)) {
        rt_trap("Archive: failed to obtain secure randomness");
        return NULL;
    }
    snprintf(nonce, sizeof(nonce), "%016llx", (unsigned long long)random_value);
    size_t nonce_len = strlen(nonce);
    if (parent_len > SIZE_MAX - nonce_len - 64)
        return NULL;
    size_t cap = parent_len + nonce_len + 64;
    if (parent_len >= cap)
        return NULL;
    char *tmp = (char *)malloc(cap);
    if (!tmp)
        return NULL;
    if (parent_len > 0)
        memcpy(tmp, path, parent_len);
#ifdef _WIN32
    unsigned long pid = (unsigned long)GetCurrentProcessId();
#else
    unsigned long pid = (unsigned long)getpid();
#endif
    int written = snprintf(
        tmp + parent_len, cap - parent_len, ".zanna-archive-tmp.%lu.%s.%u", pid, nonce, attempt);
    if (written < 0 || (size_t)written >= cap - parent_len) {
        free(tmp);
        return NULL;
    }
    return tmp;
}

/// @brief Atomically rename `src` to `dst`, replacing any existing file.
///
/// On Windows, an existing regular destination is replaced with
/// `ReplaceFileW`, preserving its identity metadata, ACLs, compression,
/// encryption, and named streams; a new destination uses `MoveFileExW`.
/// Directory and reparse-point destinations are rejected. POSIX uses
/// `rename(2)`. Each operation stays on the sidecar's filesystem.
///
/// @param src UTF-8 source path.
/// @param dst UTF-8 destination path.
/// @return 1 on success, 0 on failure.
static int archive_replace_utf8(const char *src, const char *dst) {
#ifdef _WIN32
    wchar_t *wsrc = rt_file_path_utf8_to_wide(src);
    wchar_t *wdst = rt_file_path_utf8_to_wide(dst);
    if (!wsrc || !wdst) {
        free(wsrc);
        free(wdst);
        return 0;
    }
    DWORD attributes = GetFileAttributesW(wdst);
    BOOL ok = FALSE;
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        if ((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0) {
            ok = ReplaceFileW(wdst, wsrc, NULL, 0, NULL, NULL);
        }
    } else {
        DWORD attr_error = GetLastError();
        if (attr_error == ERROR_FILE_NOT_FOUND || attr_error == ERROR_PATH_NOT_FOUND)
            ok = MoveFileExW(wsrc, wdst, MOVEFILE_WRITE_THROUGH);
    }
    free(wsrc);
    free(wdst);
    return ok ? 1 : 0;
#else
    return rename(src, dst) == 0 ? 1 : 0;
#endif
}

/// @brief Delete a file at a UTF-8 path, ignoring errors.
///
/// Cross-platform wrapper: `DeleteFileW` on Windows (after wide
/// conversion), `unlink(2)` on POSIX. Used for temp-file cleanup
/// where best-effort removal is sufficient.
///
/// @param path UTF-8 path of the file to delete.
void archive_unlink_utf8(const char *path) {
#ifdef _WIN32
    wchar_t *wide = rt_file_path_utf8_to_wide(path);
    if (wide) {
        (void)DeleteFileW(wide);
        free(wide);
    }
#else
    (void)unlink(path);
#endif
}

/// @brief fsync the directory that contains `path` (POSIX only).
///
/// Required on Linux/macOS to guarantee the directory entry for a
/// newly renamed file survives a crash. No-op on Windows (the
/// `MOVEFILE_WRITE_THROUGH` flag handles this there). Returns 1 on
/// success, 0 on any error.
///
/// @param path UTF-8 path of the newly placed file.
/// @return 1 if the parent directory was fsynced successfully, 0 otherwise.
static int archive_sync_parent_dir(const char *path) {
#ifdef _WIN32
    (void)path;
    return 1;
#else
    const char *last = strrchr(path, '/');
    const char *parent = ".";
    char *owned = NULL;
    if (last) {
        size_t len = (size_t)(last - path);
        if (len == 0) {
            parent = "/";
        } else {
            owned = (char *)malloc(len + 1);
            if (!owned)
                return 0;
            memcpy(owned, path, len);
            owned[len] = '\0';
            parent = owned;
        }
    }
    int flags = O_RDONLY;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
    int fd = archive_open_posix(parent, flags, 0);
    free(owned);
    if (fd < 0)
        return 0;
    int ok = fsync(fd) == 0 ? 1 : 0;
    if (close(fd) != 0)
        ok = 0;
    return ok;
#endif
}

/// @brief Atomically replace a file at a UTF-8 path with `total` bytes.
///
/// Cross-platform helper used to flush the assembled write buffer
/// during `Finish` and to drop extracted entries onto disk during
/// `Extract`/`ExtractAll`. Writes an exclusive temp sidecar first,
/// flushes it, then replaces the destination so readers never observe
/// a partially-written archive or extracted file. POSIX replacements copy
/// an existing regular destination's permission bits onto the sidecar before
/// rename; newly created files retain the historical 0644-before-umask mode.
///
/// @param cpath    UTF-8 destination file path.
/// @param src      Source byte buffer.
/// @param total    Number of bytes to write.
/// @param trap_msg Trap message used on any failure.
/// @return 1 after the durable atomic replacement succeeds; otherwise 0
/// after raising @p trap_msg.
int archive_write_file_all_utf8(const char *cpath,
                                const uint8_t *src,
                                size_t total,
                                const char *trap_msg) {
#ifdef _WIN32
    char *tmp = NULL;
    HANDLE h = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        tmp = archive_make_temp_path(cpath, attempt);
        if (!tmp) {
            rt_trap(trap_msg);
            return 0;
        }
        h = archive_open_win_path(tmp, GENERIC_WRITE, 0, CREATE_NEW);
        if (h != INVALID_HANDLE_VALUE)
            break;
        free(tmp);
        tmp = NULL;
        DWORD err = GetLastError();
        if (err != ERROR_FILE_EXISTS && err != ERROR_ALREADY_EXISTS)
            break;
    }
    if (h == INVALID_HANDLE_VALUE || !tmp) {
        free(tmp);
        rt_trap(trap_msg);
        return 0;
    }
    if (!archive_write_exact_win(h, src, total, trap_msg)) {
        CloseHandle(h);
        archive_unlink_utf8(tmp);
        free(tmp);
        rt_trap(trap_msg);
        return 0;
    }
    int ok = FlushFileBuffers(h) ? 1 : 0;
    if (!CloseHandle(h))
        ok = 0;
    if (ok)
        ok = archive_replace_utf8(tmp, cpath);
    if (!ok) {
        archive_unlink_utf8(tmp);
        free(tmp);
        rt_trap(trap_msg);
        return 0;
    }
    free(tmp);
#else
    char *tmp = NULL;
    int fd = -1;
    mode_t target_mode = 0644;
    struct stat existing;
    if (lstat(cpath, &existing) == 0 && S_ISREG(existing.st_mode))
        target_mode = existing.st_mode & 07777;
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        tmp = archive_make_temp_path(cpath, attempt);
        if (!tmp) {
            rt_trap(trap_msg);
            return 0;
        }
        int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
        fd = archive_open_posix(tmp, flags, target_mode);
        if (fd >= 0)
            break;
        int err = errno;
        free(tmp);
        tmp = NULL;
        if (err != EEXIST)
            break;
    }
    if (fd < 0 || !tmp) {
        free(tmp);
        rt_trap(trap_msg);
        return 0;
    }
    if (fchmod(fd, target_mode) != 0) {
        close(fd);
        archive_unlink_utf8(tmp);
        free(tmp);
        rt_trap(trap_msg);
        return 0;
    }
    if (!archive_write_exact_posix(fd, src, total, trap_msg)) {
        close(fd);
        archive_unlink_utf8(tmp);
        free(tmp);
        rt_trap(trap_msg);
        return 0;
    }
    int ok = fsync(fd) == 0 ? 1 : 0;
    if (close(fd) != 0)
        ok = 0;
    if (ok)
        ok = archive_replace_utf8(tmp, cpath);
    if (ok)
        ok = archive_sync_parent_dir(cpath);
    if (!ok) {
        archive_unlink_utf8(tmp);
        free(tmp);
        rt_trap(trap_msg);
        return 0;
    }
    free(tmp);
#endif
    return 1;
}

/// @brief Write the contents of an `rt_bytes` handle to a UTF-8 path.
///
/// Thin adapter over `archive_write_file_all_utf8` that unwraps the
/// bytes handle. Traps if the path is empty or NULL.
///
/// @param cpath UTF-8 destination path.
/// @param data  Source `rt_bytes` handle.
void archive_write_bytes_to_path(const char *cpath, void *data) {
    if (!cpath || *cpath == '\0') {
        rt_trap("Archive: invalid destination path");
        return;
    }
    if (!data) {
        rt_trap("Archive: NULL data");
        return;
    }

    const uint8_t *src = bytes_data(data);
    size_t total = (size_t)bytes_len(data);
    (void)archive_write_file_all_utf8(
        cpath, src, total, "Archive: failed to write destination file");
}

#if !defined(_WIN32)
/// @brief Open a child path relative to a directory descriptor.
/// @param parent_fd Directory descriptor used as the `openat` base.
/// @param name Relative child name.
/// @param flags POSIX open flags; close-on-exec may be added.
/// @param mode Creation mode used when required by @p flags.
/// @return Open descriptor, or -1 with `errno` set.
static int archive_openat_posix(int parent_fd, const char *name, int flags, mode_t mode) {
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int fd = openat(parent_fd, name, flags, mode);
#if defined(FD_CLOEXEC) && !defined(O_CLOEXEC)
    if (fd >= 0) {
        int fd_flags = fcntl(fd, F_GETFD);
        if (fd_flags >= 0)
            (void)fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);
    }
#endif
    return fd;
}

/// @brief Duplicate a POSIX descriptor with close-on-exec semantics.
/// @param fd Descriptor to duplicate.
/// @return Independent descriptor on success, or -1 on failure.
static int archive_dup_fd_posix(int fd) {
    int dup_fd = -1;
#ifdef F_DUPFD_CLOEXEC
    dup_fd = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    if (dup_fd >= 0)
        return dup_fd;
#endif
    dup_fd = dup(fd);
#ifdef FD_CLOEXEC
    if (dup_fd >= 0) {
        int flags = fcntl(dup_fd, F_GETFD);
        if (flags >= 0)
            (void)fcntl(dup_fd, F_SETFD, flags | FD_CLOEXEC);
    }
#endif
    return dup_fd;
}

/// @brief Open one verified, non-symlink directory component.
/// @details Optionally creates the component, checks it without following
/// symlinks, then opens and revalidates the resulting descriptor.
/// @param parent_fd Descriptor for the trusted parent directory.
/// @param name Single non-empty child component.
/// @param create Nonzero to create a missing directory.
/// @return Open child-directory descriptor, or -1 after raising a trap.
static int archive_open_child_dir_posix(int parent_fd, const char *name, int create) {
    if (!name || *name == '\0') {
        rt_trap("Archive: invalid directory entry");
        return -1;
    }
    if (create && mkdirat(parent_fd, name, 0755) != 0 && errno != EEXIST) {
        rt_trap("Archive: failed to create directory");
        return -1;
    }

    struct stat st;
    if (fstatat(parent_fd, name, &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISDIR(st.st_mode)) {
        rt_trap("Archive: refusing to extract through symlink");
        return -1;
    }

    int flags = O_RDONLY;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = archive_openat_posix(parent_fd, name, flags, 0);
    if (fd < 0) {
        rt_trap("Archive: failed to open destination directory");
        return -1;
    }
    if (fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode)) {
        close(fd);
        rt_trap("Archive: destination component is not a directory");
        return -1;
    }
    return fd;
}

/// @brief Traverse a normalized relative directory path from a trusted root.
/// @param root_fd Descriptor for the trusted extraction root.
/// @param path Forward-slash-separated relative directory path; empty selects
/// the root itself.
/// @param create Nonzero to create missing components.
/// @return Open descriptor for the final directory, or -1 after a trap.
static int archive_open_dir_path_posix(int root_fd, const char *path, int create) {
    int current = archive_dup_fd_posix(root_fd);
    if (current < 0) {
        rt_trap("Archive: failed to open destination directory");
        return -1;
    }
    if (!path || *path == '\0')
        return current;

    char *copy = strdup(path);
    if (!copy) {
        close(current);
        rt_trap("Archive: memory allocation failed");
        return -1;
    }
    size_t len = strlen(copy);
    while (len > 0 && copy[len - 1] == '/')
        copy[--len] = '\0';

    char *segment = copy;
    while (*segment) {
        char *slash = strchr(segment, '/');
        if (slash)
            *slash = '\0';
        int next = archive_open_child_dir_posix(current, segment, create);
        close(current);
        if (next < 0) {
            free(copy);
            return -1;
        }
        current = next;
        if (!slash)
            break;
        segment = slash + 1;
    }

    free(copy);
    return current;
}

/// @brief Create and verify every component of a relative directory path.
/// @param root_fd Descriptor for the trusted extraction root.
/// @param path Normalized relative path to create.
void archive_make_dirs_posix_at(int root_fd, const char *path) {
    int fd = archive_open_dir_path_posix(root_fd, path, 1);
    if (fd >= 0)
        close(fd);
}

/// @brief Open the verified parent of an archive file entry.
/// @details Creates missing parent directories and returns the final path
/// component separately so callers can use descriptor-relative file APIs.
/// @param root_fd Descriptor for the trusted extraction root.
/// @param name Normalized relative file-entry name.
/// @param out_leaf Receives a heap-allocated leaf name owned by the caller.
/// @return Open parent-directory descriptor, or -1 after raising a trap.
int archive_open_parent_for_file_posix(int root_fd, const char *name, char **out_leaf) {
    if (!name || !out_leaf) {
        rt_trap("Archive: invalid file entry");
        return -1;
    }
    *out_leaf = NULL;
    char *copy = strdup(name);
    if (!copy) {
        rt_trap("Archive: memory allocation failed");
        return -1;
    }
    char *last = strrchr(copy, '/');
    char *leaf_src = copy;
    int parent_fd = -1;
    if (last) {
        *last = '\0';
        leaf_src = last + 1;
        parent_fd = archive_open_dir_path_posix(root_fd, copy, 1);
    } else {
        parent_fd = archive_dup_fd_posix(root_fd);
    }
    if (parent_fd < 0) {
        free(copy);
        rt_trap("Archive: failed to open destination directory");
        return -1;
    }
    if (!leaf_src || *leaf_src == '\0') {
        close(parent_fd);
        free(copy);
        rt_trap("Archive: invalid file entry");
        return -1;
    }
    char *leaf = strdup(leaf_src);
    free(copy);
    if (!leaf) {
        close(parent_fd);
        rt_trap("Archive: memory allocation failed");
        return -1;
    }
    *out_leaf = leaf;
    return parent_fd;
}

/// @brief Atomically write Bytes beneath a verified directory descriptor.
/// @details Creates an exclusive randomized sidecar, preserves an existing
/// regular file's permission bits, flushes it, renames it over @p leaf, and
/// synchronizes the parent directory.
/// @param parent_fd Descriptor for the already verified destination parent.
/// @param leaf Single destination filename relative to @p parent_fd.
/// @param data Runtime Bytes handle containing the output payload.
void archive_write_bytes_to_dirfd_posix(int parent_fd, const char *leaf, void *data) {
    if (parent_fd < 0 || !leaf || !data) {
        rt_trap("Archive: failed to write destination file");
        return;
    }
    const uint8_t *src = bytes_data(data);
    size_t total = (size_t)bytes_len(data);
    char tmp_name[128];
    int fd = -1;
    mode_t target_mode = 0644;
    struct stat existing;
    if (fstatat(parent_fd, leaf, &existing, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(existing.st_mode))
        target_mode = existing.st_mode & 07777;
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        uint64_t random_value = 0;
        if (!archive_random_u64(&random_value)) {
            rt_trap("Archive: failed to obtain secure randomness");
            return;
        }
        snprintf(tmp_name,
                 sizeof(tmp_name),
                 ".zanna-archive-extract-tmp.%lu.%016llx.%u",
                 (unsigned long)getpid(),
                 (unsigned long long)random_value,
                 attempt);
        int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
        fd = archive_openat_posix(parent_fd, tmp_name, flags, target_mode);
        if (fd >= 0)
            break;
        if (errno != EEXIST) {
            rt_trap("Archive: failed to write destination file");
            return;
        }
    }
    if (fd < 0) {
        rt_trap("Archive: failed to write destination file");
        return;
    }

    if (fchmod(fd, target_mode) != 0) {
        close(fd);
        (void)unlinkat(parent_fd, tmp_name, 0);
        rt_trap("Archive: failed to write destination file");
        return;
    }

    if (!archive_write_exact_posix(fd, src, total, "Archive: failed to write destination file")) {
        close(fd);
        (void)unlinkat(parent_fd, tmp_name, 0);
        rt_trap("Archive: failed to write destination file");
        return;
    }
    int ok = fsync(fd) == 0 ? 1 : 0;
    if (close(fd) != 0)
        ok = 0;
    if (ok && renameat(parent_fd, tmp_name, parent_fd, leaf) != 0)
        ok = 0;
    if (ok && fsync(parent_fd) != 0)
        ok = 0;
    if (!ok) {
        (void)unlinkat(parent_fd, tmp_name, 0);
        rt_trap("Archive: failed to write destination file");
        return;
    }
}

/// @brief Open and verify a POSIX extraction root without following symlinks.
/// @param cdir UTF-8 path to the destination directory.
/// @return Open root-directory descriptor, or -1 after raising a trap.
int archive_open_root_dir_posix(const char *cdir) {
    int flags = O_RDONLY;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = archive_open_posix(cdir, flags, 0);
    if (fd < 0) {
        rt_trap("Archive: failed to open destination directory");
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode)) {
        close(fd);
        rt_trap("Archive: destination is not a directory");
        return -1;
    }
    return fd;
}
#endif

/// @brief Return 1 if `c` is a path separator (`/` or `\`), 0 otherwise.
/// @param c Character to classify.
/// @return 1 for either slash direction; otherwise 0.
static int archive_is_sep(char c) {
    return c == '/' || c == '\\';
}

/// @brief Return the length of `path` with trailing separators stripped.
///
/// Leaves at least one character so that a path consisting of only
/// separators (e.g., `"/"`) is not reduced to zero length.
///
/// @param path String to trim (not modified).
/// @param len  Initial length to trim down from.
/// @return Trimmed length (>= 1).
size_t archive_trim_trailing_seps(const char *path, size_t len) {
    while (len > 1 && archive_is_sep(path[len - 1]))
        --len;
    return len;
}

/// @brief Inspect whether `path` is a symlink or reparse point.
///
/// Uses `lstat(2)` on POSIX and `GetFileAttributesW` on Windows. Does
/// not follow the link — the check is on the link itself, which is
/// exactly what the traversal guard needs.
///
/// @param path UTF-8 path to inspect.
/// @return 1 if the entry is a symlink/reparse point, 0 if it is absent or safe,
///         and -1 when the path exists but cannot be inspected.
static int archive_path_is_reparse_or_symlink(const char *path) {
#ifdef _WIN32
    wchar_t *wide = rt_file_path_utf8_to_wide(path);
    if (!wide)
        return -1;
    DWORD attrs = GetFileAttributesW(wide);
    DWORD err = GetLastError();
    free(wide);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
            return 0;
        return -1;
    }
    return (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    struct stat st;
    if (lstat(path, &st) != 0) {
        if (errno == ENOENT || errno == ENOTDIR)
            return 0;
        return -1;
    }
    return S_ISLNK(st.st_mode) ? 1 : 0;
#endif
}

/// @brief Trap if any path component (or the leaf) is a symlink / reparse point.
///
/// Walks each intermediate prefix of `path` starting after `root_len`
/// characters (the known-safe root, e.g., the extraction directory).
/// If `include_leaf` is true, the full path is also checked. This
/// defends against TOCTOU symlink-swapping attacks during extraction.
///
/// @param path         Full UTF-8 path to inspect.
/// @param root_len     Number of leading bytes that are already trusted.
/// @param include_leaf If non-zero, also check the final path component.
void archive_reject_symlink_components(const char *path, size_t root_len, int include_leaf) {
    if (!path)
        return;
    size_t len = strlen(path);
    if (root_len > len)
        root_len = len;
    root_len = archive_trim_trailing_seps(path, root_len);
    if (len > SIZE_MAX - 2) {
        rt_trap("Archive: destination path too long");
        return;
    }

    char *scratch = (char *)malloc(len + 2);
    if (!scratch) {
        rt_trap("Archive: memory allocation failed");
        return;
    }

    if (root_len > 0) {
        memcpy(scratch, path, root_len);
        scratch[root_len] = '\0';
        int link_state = archive_path_is_reparse_or_symlink(scratch);
        if (link_state != 0) {
            free(scratch);
            rt_trap(link_state > 0 ? "Archive: refusing to extract through symlink"
                                   : "Archive: unable to inspect destination path");
            return;
        }
    }

    size_t i = root_len;
    while (i < len) {
        while (i < len && archive_is_sep(path[i]))
            ++i;
        if (i >= len)
            break;
        while (i < len && !archive_is_sep(path[i]))
            ++i;
        if (i == len && !include_leaf)
            break;

        if (i > len) {
            free(scratch);
            rt_trap("Archive: destination path too long");
            return;
        }
        memcpy(scratch, path, i);
        scratch[i] = '\0';
        int link_state = archive_path_is_reparse_or_symlink(scratch);
        if (link_state != 0) {
            free(scratch);
            rt_trap(link_state > 0 ? "Archive: refusing to extract through symlink"
                                   : "Archive: unable to inspect destination path");
            return;
        }
    }

    free(scratch);
}
