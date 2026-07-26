//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/io/rt_asset.c
// Purpose: Runtime asset manager implementation. Provides transparent loading
//          from embedded ZPAK blobs, mounted .zpak pack files, and filesystem.
//
// Key invariants:
//   - Initialization is idempotent (safe to call multiple times).
//   - Resolution order: embedded → mounted packs (LIFO) → filesystem.
//   - Registry locking covers source selection only; I/O, decompression, and
//     managed-object allocation run against retained snapshots outside it.
//   - Pack auto-discovery scans exe directory for *.zpak files on init.
//   - Type dispatch in Assets.Load() is based on file extension.
//   - All returned objects are GC-managed.
//
// Ownership/Lifetime:
//   - Embedded blob pointer is borrowed (lives in .rodata, never freed).
//   - Mounted pack handles hold one registry reference until unmount.
//   - Active loads retain their selected archive through read/decompression.
//   - Data buffers from zpak_read_entry are freed after creating GC objects.
//
// Links: src/runtime/io/rt_asset.h,
//        src/runtime/io/rt_asset_decode.c,
//        src/runtime/io/rt_zpak_reader.h,
//        src/runtime/io/rt_path_exe.c
//
//===----------------------------------------------------------------------===//
/**
 * @file
 * @brief Implements layered embedded, ZPAK, and filesystem asset resolution.
 * @details Serializes registry initialization and publication, discovers
 * adjacent packs, normalizes logical names, snapshots and retains selected
 * sources outside the lock, dispatches recognized formats to typed decoders,
 * and supports concurrent mount, unmount, query, and load operations.
 */

#include "rt_asset.h"
#include "rt_file_path.h"
#include "rt_internal.h"
#include "rt_platform.h"
#include "rt_seq.h"
#include "rt_zpak_reader.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if RT_PLATFORM_WINDOWS
#include <wchar.h>
#include <windows.h>

/** Signature of the optional Win32 ordinal string comparison entry point. */
typedef int(WINAPI *asset_compare_string_ordinal_fn)(LPCWCH, int, LPCWCH, int, BOOL);

/** One-time initialization token for resolving the optional ordinal comparator. */
static INIT_ONCE g_asset_compare_once = INIT_ONCE_STATIC_INIT;
/** Dynamically resolved `CompareStringOrdinal` entry point, or NULL for fallback comparison. */
static asset_compare_string_ordinal_fn g_asset_compare_string_ordinal = NULL;

/// @brief Resolve CompareStringOrdinal without extending the native import table.
/// @param once Windows one-time initialization token.
/// @param param Unused callback parameter.
/// @param context Unused callback result slot.
/// @return `TRUE`; an unavailable symbol is recorded as a supported fallback
/// condition rather than initialization failure.
static BOOL CALLBACK asset_resolve_compare_ordinal(PINIT_ONCE once, PVOID param, PVOID *context) {
    (void)once;
    (void)param;
    (void)context;
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (kernel32) {
        g_asset_compare_string_ordinal = (asset_compare_string_ordinal_fn)(void *)GetProcAddress(
            kernel32, "CompareStringOrdinal");
    }
    return TRUE;
}

/// @brief Compare two NUL-terminated paths with Windows ordinal case folding.
/// @param left First UTF-16 path.
/// @param right Second UTF-16 path.
/// @return `CSTR_EQUAL` when the paths compare equal ignoring case; otherwise
/// a non-equal value.
static int asset_compare_path_ordinal(const wchar_t *left, const wchar_t *right) {
    (void)InitOnceExecuteOnce(&g_asset_compare_once, asset_resolve_compare_ordinal, NULL, NULL);
    if (!g_asset_compare_string_ordinal)
        return wcscmp(left, right) == 0 ? CSTR_EQUAL : 0;
    return g_asset_compare_string_ordinal(left, -1, right, -1, TRUE);
}

/// @brief Convert a NUL-terminated UTF-16 string to strict UTF-8.
/// @param wide UTF-16 input string.
/// @return Heap-allocated UTF-8 string owned by the caller, or `NULL` for
/// invalid input, conversion failure, or allocation failure.
static char *asset_wide_to_utf8_dup(const wchar_t *wide) {
    if (!wide)
        return NULL;
    int needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, NULL, 0, NULL, NULL);
    if (needed <= 0)
        return NULL;
    char *utf8 = (char *)malloc((size_t)needed);
    if (!utf8)
        return NULL;
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, utf8, needed, NULL, NULL) <=
        0) {
        free(utf8);
        return NULL;
    }
    return utf8;
}

/// @brief Resolve one UTF-16 path through the Unicode Win32 full-path API.
/// @details Retries when the process current directory changes between the
///          sizing and conversion calls and grows the required buffer.
/// @param path Non-empty UTF-16 path to resolve.
/// @return Heap-allocated full path owned by the caller, or `NULL` on failure.
static wchar_t *asset_full_path_wide_dup(const wchar_t *path) {
    if (!path || !*path)
        return NULL;
    DWORD capacity = GetFullPathNameW(path, 0, NULL, NULL);
    while (capacity > 0) {
        if ((size_t)capacity > SIZE_MAX / sizeof(wchar_t))
            return NULL;
        wchar_t *full = (wchar_t *)malloc((size_t)capacity * sizeof(wchar_t));
        if (!full)
            return NULL;
        DWORD length = GetFullPathNameW(path, capacity, full, NULL);
        if (length == 0) {
            free(full);
            return NULL;
        }
        if (length < capacity)
            return full;
        free(full);
        if (length == MAXDWORD)
            return NULL;
        capacity = length + 1;
    }
    return NULL;
}

/// @brief Join a UTF-16 directory and leaf with one Windows separator.
/// @param dir NUL-terminated directory path.
/// @param leaf NUL-terminated leaf name.
/// @return Heap-allocated joined path, or `NULL` on overflow/allocation
/// failure.
static wchar_t *asset_win_join_wide(const wchar_t *dir, const wchar_t *leaf) {
    size_t dir_len = wcslen(dir);
    size_t leaf_len = wcslen(leaf);
    int needs_sep = dir_len > 0 && dir[dir_len - 1] != L'\\' && dir[dir_len - 1] != L'/';
    if (dir_len > (SIZE_MAX / sizeof(wchar_t)) - leaf_len - (needs_sep ? 1U : 0U) - 1U)
        return NULL;
    wchar_t *path =
        (wchar_t *)malloc((dir_len + (needs_sep ? 1U : 0U) + leaf_len + 1U) * sizeof(wchar_t));
    if (!path)
        return NULL;
    memcpy(path, dir, dir_len * sizeof(wchar_t));
    size_t pos = dir_len;
    if (needs_sep)
        path[pos++] = L'\\';
    memcpy(path + pos, leaf, leaf_len * sizeof(wchar_t));
    path[pos + leaf_len] = L'\0';
    return path;
}
#else
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

// ─── External declarations ──────────────────────────────────────────────────

#include "rt_trap.h"
/// @copydoc rt_string_from_bytes()
extern rt_string rt_string_from_bytes(const char *data, size_t len);
/// @copydoc rt_string_cstr()
extern const char *rt_string_cstr(rt_string s);
/// @copydoc rt_string_len()
extern size_t rt_string_len(rt_string s);
/// @copydoc rt_const_cstr()
extern rt_string rt_const_cstr(const char *s);
/// @copydoc rt_bytes_from_raw()
extern void *rt_bytes_from_raw(const uint8_t *data, size_t len);

// Type-dispatched decoder (rt_asset_decode.c)
/// @copydoc rt_asset_decode_typed()
extern void *rt_asset_decode_typed(const char *name, const uint8_t *data, size_t size);
/// @copydoc rt_asset_extension_is_typed()
extern int rt_asset_extension_is_typed(const char *name);

// Exe directory detection
/// @copydoc rt_path_exe_dir_cstr()
extern char *rt_path_exe_dir_cstr(void);

/// @brief Duplicate a path in canonical/full form when the platform can resolve it.
/// @details POSIX uses `realpath`; Windows uses `GetFullPathNameW`. If the
/// platform resolver fails, the original non-empty path is duplicated so
/// callers still receive stable owned storage.
/// @param path Non-empty UTF-8 path.
/// @return Heap-allocated path owned by the caller, or `NULL` for invalid
/// input or allocation/conversion failure.
static char *asset_canonical_path_dup(const char *path) {
    if (!path || !*path)
        return NULL;
#if RT_PLATFORM_WINDOWS
    wchar_t *wide = rt_file_path_utf8_to_wide(path);
    if (!wide)
        return NULL;
    wchar_t *full_wide = asset_full_path_wide_dup(wide);
    free(wide);
    if (!full_wide)
        return strdup(path);
    char *full = asset_wide_to_utf8_dup(full_wide);
    free(full_wide);
    return full;
#else
    char *resolved = realpath(path, NULL);
    if (resolved)
        return resolved;
    return strdup(path);
#endif
}

/// @brief Case-sensitive (POSIX) or case-insensitive (Windows) path comparison.
/// @param a First canonical or candidate UTF-8 path.
/// @param b Second canonical or candidate UTF-8 path.
/// @return 1 when the platform-appropriate comparison considers the paths
/// equal; otherwise 0.
static int asset_path_equal(const char *a, const char *b) {
    if (!a || !b)
        return 0;
#if RT_PLATFORM_WINDOWS
    wchar_t *wide_a = rt_file_path_utf8_to_wide(a);
    wchar_t *wide_b = rt_file_path_utf8_to_wide(b);
    if (!wide_a || !wide_b) {
        free(wide_a);
        free(wide_b);
        return 0;
    }
    int result = asset_compare_path_ordinal(wide_a, wide_b);
    free(wide_a);
    free(wide_b);
    return result == CSTR_EQUAL;
#else
    return strcmp(a, b) == 0;
#endif
}

// ─── Weak default for embedded asset blob ───────────────────────────────────
// These are overridden by the stronger definitions in the generated asset .o
// file when assets are embedded via `embed` directives in zanna.project.
// When no assets are embedded, these defaults ensure clean linking.

#if RT_PLATFORM_WINDOWS
/** Link-time-overridable empty embedded-asset payload for Windows builds. */
__declspec(selectany) const unsigned char zanna_asset_blob[1] = {0};
/** Link-time-overridable byte size corresponding to @ref zanna_asset_blob. */
__declspec(selectany) const unsigned long long zanna_asset_blob_size = 0;
#else
/** Weak empty embedded-asset payload replaced when a project embeds assets. */
__attribute__((weak)) const unsigned char zanna_asset_blob[1] = {0};
/** Weak byte size corresponding to @ref zanna_asset_blob. */
__attribute__((weak)) const unsigned long long zanna_asset_blob_size = 0;
#endif

// ─── Constants ──────────────────────────────────────────────────────────────

/** Maximum number of dynamically mounted filesystem ZPAK archives. */
#define RT_ASSET_MAX_PACKS 32

/// @brief Asset-registry initialization states guarded by @ref g_asset_lock.
typedef enum {
    ASSET_INIT_UNINITIALIZED = 0, ///< No thread has claimed initialization.
    ASSET_INIT_IN_PROGRESS = 1,   ///< One thread is building an unpublished staging registry.
    ASSET_INIT_COMPLETE = 2,      ///< The complete registry was atomically published.
} asset_init_state_t;

// ─── Global state ───────────────────────────────────────────────────────────

/// @brief Singleton asset manager state — all fields guarded by g_asset_lock.
static struct {
    zpak_archive_t *embedded;                  ///< Embedded .rodata ZPAK blob, or NULL.
    zpak_archive_t *packs[RT_ASSET_MAX_PACKS]; ///< Mounted pack file archives (LIFO).
    char *pack_paths[RT_ASSET_MAX_PACKS];      ///< Canonical paths for each mounted pack.
    int pack_count;                            ///< Number of currently mounted packs.
    asset_init_state_t init_state;             ///< Serialized initialization/publication state.
} g_asset_mgr;

#if RT_PLATFORM_WINDOWS
/** One-time token used to construct the Windows registry lock. */
static INIT_ONCE g_asset_lock_once = INIT_ONCE_STATIC_INIT;
/** Windows critical section guarding @ref g_asset_mgr. */
static CRITICAL_SECTION g_asset_lock;
/** Condition signaled after registry initialization publishes or rolls back. */
static CONDITION_VARIABLE g_asset_init_condition = CONDITION_VARIABLE_INIT;

/// @brief `InitOnce` callback that initializes the asset manager critical section.
/// @param once Windows one-time initialization token.
/// @param parameter Unused callback parameter.
/// @param context Unused callback result slot.
/// @return `TRUE` after initializing the process-lifetime critical section.
static BOOL CALLBACK asset_init_lock(PINIT_ONCE once, PVOID parameter, PVOID *context) {
    (void)once;
    (void)parameter;
    (void)context;
    InitializeCriticalSection(&g_asset_lock);
    return TRUE;
}

/// @brief Acquire the asset-manager lock (Windows CRITICAL_SECTION, lazy-initialized).
static void asset_lock(void) {
    if (!InitOnceExecuteOnce(&g_asset_lock_once, asset_init_lock, NULL, NULL))
        rt_abort("Assets: failed to initialize registry lock");
    EnterCriticalSection(&g_asset_lock);
}

/// @brief Release the asset-manager lock.
static void asset_unlock(void) {
    LeaveCriticalSection(&g_asset_lock);
}
#else
/** POSIX mutex guarding @ref g_asset_mgr and initialization transitions. */
static pthread_mutex_t g_asset_lock = PTHREAD_MUTEX_INITIALIZER;
/** POSIX condition signaled after registry initialization completes or rolls back. */
static pthread_cond_t g_asset_init_condition = PTHREAD_COND_INITIALIZER;

/// @brief Acquire the asset-manager lock (POSIX mutex).
static void asset_lock(void) {
    if (pthread_mutex_lock(&g_asset_lock) != 0)
        rt_abort("Assets: failed to acquire registry lock");
}

/// @brief Release the asset-manager lock.
static void asset_unlock(void) {
    if (pthread_mutex_unlock(&g_asset_lock) != 0)
        rt_abort("Assets: failed to release registry lock");
}
#endif

/// @brief Wait with the asset lock held until staging initialization finishes.
/// @details The wait atomically releases and reacquires the registry lock. Any
///          native condition-variable failure is treated as unrecoverable
///          because returning would expose a registry whose publication state
///          is unknown to the caller.
static void asset_wait_for_initialization_locked(void) {
    while (g_asset_mgr.init_state == ASSET_INIT_IN_PROGRESS) {
#if RT_PLATFORM_WINDOWS
        if (!SleepConditionVariableCS(&g_asset_init_condition, &g_asset_lock, INFINITE))
            rt_abort("Assets: initialization wait failed");
#else
        if (pthread_cond_wait(&g_asset_init_condition, &g_asset_lock) != 0)
            rt_abort("Assets: initialization wait failed");
#endif
    }
}

/// @brief Wake every thread waiting for a complete asset-registry publication.
/// @details The caller holds the asset lock and has already stored the complete
///          state, so awakened waiters observe all registry fields together.
static void asset_notify_initialization_locked(void) {
#if RT_PLATFORM_WINDOWS
    WakeAllConditionVariable(&g_asset_init_condition);
#else
    if (pthread_cond_broadcast(&g_asset_init_condition) != 0)
        rt_abort("Assets: initialization notification failed");
#endif
}

// ─── Helpers ────────────────────────────────────────────────────────────────

/// @brief Borrow a null-terminated view of an asset name from an `rt_string`.
///
/// Returns NULL if `name` is empty or contains an embedded null byte
/// (which would produce a silent truncation on strcmp-based lookups).
///
/// @param name Zanna string containing the asset name.
/// @return Borrowed C string pointer, or NULL if the name is invalid.
static const char *asset_name_cstr(rt_string name) {
    const uint8_t *data = NULL;
    size_t len = rt_file_string_view((const ZannaString *)name, &data);
    if (!data)
        return NULL;
    if (memchr(data, '\0', len) != NULL)
        return NULL;
    return (const char *)data;
}

/// @brief Borrow an asset lookup name with an optional URI scheme removed.
/// @param name Runtime asset-name string.
/// @return Borrowed pointer after stripping a leading `asset://`, or `NULL`
/// when the runtime string cannot be used as a C string.
static const char *asset_logical_name_cstr(rt_string name) {
    const char *cname = asset_name_cstr(name);
    if (!cname)
        return NULL;
    if (strncmp(cname, "asset://", 8) == 0)
        return cname + 8;
    return cname;
}

/// @brief Classify slash characters accepted in asset paths.
/// @param ch Character to inspect.
/// @return 1 for `/` or `\\`; otherwise 0.
static int asset_is_separator(char ch) {
    return ch == '/' || ch == '\\';
}

/// @brief Test whether a path contains either supported separator.
/// @param path NUL-terminated path; `NULL` is accepted.
/// @return 1 when any separator is present; otherwise 0.
static int asset_path_has_separator(const char *path) {
    if (!path)
        return 0;
    for (const char *p = path; *p; ++p) {
        if (asset_is_separator(*p))
            return 1;
    }
    return 0;
}

/// @brief Borrow the final component of a slash-agnostic path.
/// @param path NUL-terminated path.
/// @return Pointer within @p path immediately after its last separator, or an
/// empty static string when @p path is `NULL`.
static const char *asset_path_basename(const char *path) {
    if (!path)
        return "";
    const char *base = path;
    for (const char *p = path; *p; ++p) {
        if (asset_is_separator(*p))
            base = p + 1;
    }
    return base;
}

/// @brief Validate a logical asset name for pack and filesystem lookup.
/// @details Rejects empty names, absolute paths, drive prefixes, colons, empty
/// components, and `.` or `..` components. Both slash directions delimit
/// components on every platform.
/// @param name NUL-terminated logical name after scheme removal.
/// @return 1 when the name is safe and relative; otherwise 0.
static int asset_name_is_safe(const char *name) {
    if (!name || name[0] == '\0')
        return 0;
    if (asset_is_separator(name[0]))
        return 0;
    if (name[0] && name[1] == ':')
        return 0;

    const char *segment = name;
    for (const char *p = name;; ++p) {
        if (*p == ':')
            return 0;
        if (*p == '\0' || asset_is_separator(*p)) {
            size_t len = (size_t)(p - segment);
            if (len == 0 || (len == 1 && segment[0] == '.') ||
                (len == 2 && segment[0] == '.' && segment[1] == '.')) {
                return 0;
            }
            if (*p == '\0')
                break;
            segment = p + 1;
        }
    }
    return 1;
}

#if RT_PLATFORM_WINDOWS
/// @brief Read a non-directory, non-reparse Windows file into native memory.
/// @param path UTF-8 filesystem path.
/// @param out_size Receives the exact file size on success and zero on failure;
/// may be `NULL`.
/// @return Heap buffer owned by the caller, including for an empty file, or
/// `NULL` on validation, I/O, size, or allocation failure.
static uint8_t *asset_read_regular_file(const char *path, size_t *out_size) {
    if (out_size)
        *out_size = 0;
    wchar_t *wide = rt_file_path_utf8_to_wide(path);
    if (!wide)
        return NULL;
    HANDLE h = CreateFileW(wide,
                           GENERIC_READ,
                           FILE_SHARE_READ,
                           NULL,
                           OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN |
                               FILE_FLAG_OPEN_REPARSE_POINT,
                           NULL);
    free(wide);
    if (h == INVALID_HANDLE_VALUE)
        return NULL;
    BY_HANDLE_FILE_INFORMATION info;
    LARGE_INTEGER size;
    if (!GetFileInformationByHandle(h, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
        !GetFileSizeEx(h, &size) || size.QuadPart < 0 || (uint64_t)size.QuadPart > SIZE_MAX) {
        CloseHandle(h);
        return NULL;
    }
    size_t len = (size_t)size.QuadPart;
    uint8_t *buf = (uint8_t *)malloc(len > 0 ? len : 1);
    if (!buf) {
        CloseHandle(h);
        return NULL;
    }
    size_t off = 0;
    while (off < len) {
        DWORD chunk = (len - off) > (size_t)UINT32_MAX ? UINT32_MAX : (DWORD)(len - off);
        DWORD got = 0;
        if (!ReadFile(h, buf + off, chunk, &got, NULL) || got == 0) {
            free(buf);
            CloseHandle(h);
            return NULL;
        }
        off += (size_t)got;
    }
    if (!CloseHandle(h)) {
        free(buf);
        return NULL;
    }
    if (out_size)
        *out_size = len;
    return buf;
}

/// @brief Stat a regular file at a UTF-8 path and return its size (Windows).
///
/// Returns 0 for missing files, directories, and reparse points.
/// Writes the file size in bytes to `*out_size` when non-NULL.
///
/// @param path     UTF-8 file path.
/// @param out_size Out-parameter for size in bytes (may be NULL).
/// @return 1 if the path is a regular readable file, 0 otherwise.
static int asset_regular_file_size(const char *path, uint64_t *out_size) {
    if (out_size)
        *out_size = 0;
    wchar_t *wide = rt_file_path_utf8_to_wide(path);
    if (!wide)
        return 0;
    WIN32_FILE_ATTRIBUTE_DATA attrs;
    BOOL ok = GetFileAttributesExW(wide, GetFileExInfoStandard, &attrs);
    free(wide);
    if (!ok)
        return 0;
    if ((attrs.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        return 0;
    if ((attrs.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        return 0;
    if (out_size) {
        *out_size = ((uint64_t)attrs.nFileSizeHigh << 32) | (uint64_t)attrs.nFileSizeLow;
    }
    return 1;
}
#else
/// @brief Read a non-symlink POSIX regular file into native memory.
/// @param path Filesystem path passed to `lstat` and `open`.
/// @param out_size Receives the exact file size on success and zero on failure;
/// may be `NULL`.
/// @return Heap buffer owned by the caller, including for an empty file, or
/// `NULL` on validation, I/O, size, or allocation failure.
static uint8_t *asset_read_regular_file(const char *path, size_t *out_size) {
    if (out_size)
        *out_size = 0;
    struct stat lst;
    if (lstat(path, &lst) != 0 || !S_ISREG(lst.st_mode))
        return NULL;
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = open(path, flags);
    if (fd < 0)
        return NULL;
#if defined(FD_CLOEXEC) && !defined(O_CLOEXEC)
    int fd_flags = fcntl(fd, F_GETFD);
    if (fd_flags >= 0)
        (void)fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);
#endif
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uint64_t)st.st_size > (uint64_t)SIZE_MAX) {
        close(fd);
        return NULL;
    }
    size_t len = (size_t)st.st_size;
    uint8_t *buf = (uint8_t *)malloc(len > 0 ? len : 1);
    if (!buf) {
        close(fd);
        return NULL;
    }
    size_t off = 0;
    while (off < len) {
        ssize_t n = read(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            free(buf);
            close(fd);
            return NULL;
        }
        if (n == 0) {
            free(buf);
            close(fd);
            return NULL;
        }
        off += (size_t)n;
    }
    if (close(fd) != 0) {
        free(buf);
        return NULL;
    }
    if (out_size)
        *out_size = len;
    return buf;
}

/// @brief Stat a regular file at a UTF-8 path and return its size (POSIX).
///
/// Returns 0 for missing files, directories, and special files.
/// Writes the file size in bytes to `*out_size` when non-NULL.
///
/// @param path     UTF-8 file path.
/// @param out_size Out-parameter for size in bytes (may be NULL).
/// @return 1 if the path is a regular readable file, 0 otherwise.
static int asset_regular_file_size(const char *path, uint64_t *out_size) {
    if (out_size)
        *out_size = 0;
    struct stat st;
    if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return 0;
    if (st.st_size < 0)
        return 0;
    if (out_size)
        *out_size = (uint64_t)st.st_size;
    return 1;
}
#endif

/// @brief Retained packed source selected from the asset registry.
typedef struct {
    zpak_archive_t *archive;   ///< Retained archive, released by the reader.
    const zpak_entry_t *entry; ///< Entry owned by @ref archive.
} asset_packed_source_t;

/// @brief Select and retain the highest-priority packed source for an asset.
/// @details The caller must hold the asset-manager lock. A successful result
///          remains valid after unlocking because the archive reference owns
///          both its table of contents and entry names.
/// @param name Safe logical asset name.
/// @param out_source Receives the retained archive and borrowed entry.
/// @return Non-zero when a packed entry was found and retained.
static int asset_find_packed_source_locked(const char *name, asset_packed_source_t *out_source) {
    if (!name || !out_source)
        return 0;
    out_source->archive = NULL;
    out_source->entry = NULL;

    if (g_asset_mgr.embedded) {
        const zpak_entry_t *entry = zpak_find(g_asset_mgr.embedded, name);
        if (entry && zpak_retain(g_asset_mgr.embedded)) {
            out_source->archive = g_asset_mgr.embedded;
            out_source->entry = entry;
            return 1;
        }
    }

    for (int i = g_asset_mgr.pack_count - 1; i >= 0; --i) {
        zpak_archive_t *archive = g_asset_mgr.packs[i];
        if (!archive)
            continue;
        const zpak_entry_t *entry = zpak_find(archive, name);
        if (entry && zpak_retain(archive)) {
            out_source->archive = archive;
            out_source->entry = entry;
            return 1;
        }
    }
    return 0;
}

/// @brief Read a retained packed source with trap-safe archive cleanup.
/// @details Decompression may trap on corrupt input. The recovery boundary
///          releases the archive snapshot before propagating the same message,
///          preventing both a registry deadlock and a leaked archive reference.
/// @param source Retained source returned by
///        @ref asset_find_packed_source_locked.
/// @param out_size Receives the uncompressed byte count on success.
/// @return Caller-owned byte buffer, or NULL when reading fails.
static uint8_t *asset_read_packed_source(asset_packed_source_t source, size_t *out_size) {
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[512];
        const char *error = rt_trap_get_error();
        snprintf(saved_error,
                 sizeof(saved_error),
                 "%s",
                 error && error[0] ? error : "Assets.Load: packed asset read failed");
        rt_trap_clear_recovery();
        zpak_close(source.archive);
        rt_trap(saved_error);
        return NULL;
    }

    uint8_t *data = zpak_read_entry(source.archive, source.entry, out_size);
    rt_trap_clear_recovery();
    zpak_close(source.archive);
    return data;
}

/// @brief Resolve an asset name across the layered asset sources.
///
/// Lookup order (first-hit wins):
///   1. The embedded ZPAK baked into the executable's .rodata — always
///      tried first so shipped binaries stay self-contained.
///   2. Mounted packs, iterated in *reverse* mount order — later
///      mounts shadow earlier ones, letting mod/DLC loads override
///      the base game's assets cleanly.
///   3. The filesystem, relative to the current working directory —
///      dev-mode convenience that lets you drop a file in-place
///      without rebuilding a ZPAK.
/// Returns a heap-allocated buffer (caller frees) or NULL if not found
/// or on any read error.
/// @param name Validated logical asset name.
/// @param out_size Receives the decoded or file byte count on success.
/// @return Heap-allocated payload owned by the caller, or `NULL` when no source
/// succeeds.
static uint8_t *asset_find_data(const char *name, size_t *out_size) {
    asset_packed_source_t source;
    asset_lock();
    int found_packed = asset_find_packed_source_locked(name, &source);
    asset_unlock();
    if (found_packed)
        return asset_read_packed_source(source, out_size);

    // 3. Filesystem fallback (CWD-relative)
    uint8_t *loose = asset_read_regular_file(name, out_size);
    if (loose)
        return loose;

    return NULL;
}

/// @brief Private, unpublished registry assembled during lazy initialization.
/// @details Native I/O populates this structure without holding the live
///          registry lock. Once complete, ownership of every field moves into
///          @ref g_asset_mgr in one short locked publication.
typedef struct {
    zpak_archive_t *embedded;                  ///< Parsed embedded archive, or NULL.
    zpak_archive_t *packs[RT_ASSET_MAX_PACKS]; ///< Owned discovered pack archives.
    char *pack_paths[RT_ASSET_MAX_PACKS];      ///< Owned canonical paths for @ref packs.
    int pack_count;                            ///< Number of initialized pack/path pairs.
} asset_init_stage_t;

/// @brief Add an opened pack to an initialization stage transactionally.
/// @details Canonicalizes and copies @p path, rejects duplicates, and enforces
///          the fixed pack ceiling. Ownership of @p archive always transfers:
///          it is stored on success and closed on any rejection or allocation
///          failure.
/// @param stage Unpublished initialization registry being assembled.
/// @param path Native UTF-8 path used to open @p archive.
/// @param archive Owned open archive that this helper consumes.
static void asset_stage_add_pack(asset_init_stage_t *stage,
                                 const char *path,
                                 zpak_archive_t *archive) {
    if (!archive)
        return;
    if (!stage || !path || stage->pack_count >= RT_ASSET_MAX_PACKS) {
        zpak_close(archive);
        return;
    }

    char *canonical = asset_canonical_path_dup(path);
    if (!canonical) {
        zpak_close(archive);
        return;
    }
    for (int i = 0; i < stage->pack_count; ++i) {
        if (asset_path_equal(stage->pack_paths[i], canonical)) {
            free(canonical);
            zpak_close(archive);
            return;
        }
    }

    stage->packs[stage->pack_count] = archive;
    stage->pack_paths[stage->pack_count] = canonical;
    stage->pack_count++;
}

/// @brief Scan one directory for regular `*.zpak` files into a staging registry.
/// @details Directory enumeration, canonicalization, file opening, and complete
///          ZPAK validation all occur without the live asset-manager lock. Bad,
///          symlinked/reparse, duplicate, and excess packs are skipped.
/// @param dir Native UTF-8 directory path to enumerate; NULL is a no-op.
/// @param stage Unpublished initialization registry that owns accepted packs.
static void discover_packs(const char *dir, asset_init_stage_t *stage) {
    if (!dir || !stage)
        return;

#if RT_PLATFORM_WINDOWS
    wchar_t *wdir = rt_file_path_utf8_to_wide(dir);
    if (!wdir)
        return;
    wchar_t *pattern = asset_win_join_wide(wdir, L"*.zpak");
    if (!pattern) {
        free(wdir);
        return;
    }
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(pattern, &fd);
    free(pattern);
    if (hFind == INVALID_HANDLE_VALUE) {
        free(wdir);
        return;
    }
    do {
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
            continue;
        wchar_t *wpath = asset_win_join_wide(wdir, fd.cFileName);
        char *path = asset_wide_to_utf8_dup(wpath);
        free(wpath);
        if (!path)
            continue;
        if (stage->pack_count < RT_ASSET_MAX_PACKS) {
            zpak_archive_t *archive = zpak_open_file_no_follow(path);
            asset_stage_add_pack(stage, path, archive);
        }
        free(path);
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);
    free(wdir);
#else
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        size_t nlen = strlen(entry->d_name);
        if (nlen < 5)
            continue;
        if (strcmp(entry->d_name + nlen - 4, ".zpak") != 0)
            continue;

        size_t dir_len = strlen(dir);
        int needs_sep = dir_len > 0 && dir[dir_len - 1] != '/';
        if (dir_len > SIZE_MAX - nlen - (size_t)needs_sep - 1)
            continue;
        char *path = (char *)malloc(dir_len + (size_t)needs_sep + nlen + 1);
        if (!path)
            continue;
        memcpy(path, dir, dir_len);
        size_t pos = dir_len;
        if (needs_sep)
            path[pos++] = '/';
        memcpy(path + pos, entry->d_name, nlen);
        path[pos + nlen] = '\0';

        // Verify it's a regular file
        uint64_t pack_size = 0;
        if (!asset_regular_file_size(path, &pack_size)) {
            free(path);
            continue;
        }

        if (stage->pack_count < RT_ASSET_MAX_PACKS) {
            zpak_archive_t *archive = zpak_open_file_no_follow(path);
            asset_stage_add_pack(stage, path, archive);
        }
        free(path);
    }
    closedir(d);
#endif
}

/// @brief Ensure the asset manager is initialized (lazy init on first use).
static void ensure_init(void) {
    rt_asset_init(NULL, 0);
}

// ─── rt_asset_init ──────────────────────────────────────────────────────────

/// @brief Initialize the asset manager with an optional embedded ZPAK blob.
/// @details Parses the embedded blob (from linked .rodata or explicit argument),
///          then auto-discovers .zpak pack files next to the executable. On macOS,
///          also scans the .app bundle's Resources directory. One caller claims
///          initialization; all parsing and filesystem I/O populate a private
///          stage outside the registry lock. The complete stage is published
///          atomically, and concurrent callers wait rather than observing a
///          partial registry. Idempotent: only the first caller's blob is used.
/// @param blob Optional borrowed pointer to a complete embedded ZPAK image.
/// @param size Byte length of @p blob; ignored when the blob is absent, too
/// short, or not representable by `size_t`.
void rt_asset_init(const uint8_t *blob, uint64_t size) {
    asset_lock();
    if (g_asset_mgr.init_state == ASSET_INIT_COMPLETE) {
        asset_unlock();
        return;
    }
    if (g_asset_mgr.init_state == ASSET_INIT_IN_PROGRESS) {
        asset_wait_for_initialization_locked();
        asset_unlock();
        return;
    }
    g_asset_mgr.init_state = ASSET_INIT_IN_PROGRESS;
    asset_unlock();

    asset_init_stage_t stage;
    memset(&stage, 0, sizeof(stage));

    // Parse embedded blob (explicit argument)
    if (blob && size >= RT_ZPAK_HEADER_SIZE && size <= (uint64_t)SIZE_MAX)
        stage.embedded = zpak_open_memory(blob, (size_t)size);

    // Auto-discover embedded blob from linked asset .o file.
    // The AssetCompiler generates a C file with zanna_asset_blob[] and
    // zanna_asset_blob_size. When linked, these override the weak defaults below.
    if (!stage.embedded) {
        if (zanna_asset_blob_size >= RT_ZPAK_HEADER_SIZE &&
            zanna_asset_blob_size <= (unsigned long long)SIZE_MAX)
            stage.embedded = zpak_open_memory(zanna_asset_blob, (size_t)zanna_asset_blob_size);
    }

    // Auto-discover .zpak packs next to executable
    char *exe_dir = rt_path_exe_dir_cstr();
    if (exe_dir) {
        discover_packs(exe_dir, &stage);

#if RT_PLATFORM_MACOS
        // Also check bundle Resources directory
        static const char resource_suffix[] = "/../Resources";
        size_t exe_len = strlen(exe_dir);
        if (exe_len <= SIZE_MAX - sizeof(resource_suffix)) {
            char *resource_dir = (char *)malloc(exe_len + sizeof(resource_suffix));
            if (resource_dir) {
                memcpy(resource_dir, exe_dir, exe_len);
                memcpy(resource_dir + exe_len, resource_suffix, sizeof(resource_suffix));
                discover_packs(resource_dir, &stage);
                free(resource_dir);
            }
        }
#endif
        free(exe_dir);
    }

    asset_lock();
    g_asset_mgr.embedded = stage.embedded;
    for (int i = 0; i < stage.pack_count; ++i) {
        g_asset_mgr.packs[i] = stage.packs[i];
        g_asset_mgr.pack_paths[i] = stage.pack_paths[i];
    }
    g_asset_mgr.pack_count = stage.pack_count;
    g_asset_mgr.init_state = ASSET_INIT_COMPLETE;
    asset_notify_initialization_locked();
    asset_unlock();
}

// ─── rt_asset_load ──────────────────────────────────────────────────────────

/// @brief Load an asset by name with automatic type dispatch based on file extension.
/// @details Searches embedded blob → mounted packs (LIFO) → filesystem. A
///          recognized image/audio extension returns its typed object (Pixels
///          or Sound) or NULL when the bytes are malformed — it NEVER silently
///          downgrades a recognized-but-corrupt asset to Bytes, so each
///          recognized suffix has one stable result type (VDOC-181). Only
///          UNRECOGNIZED extensions return raw Bytes.
/// @param name Safe relative asset name, optionally prefixed by `asset://`.
/// @return Fresh GC-managed typed object or Bytes value, or `NULL` when the
/// name is invalid, no source exists, reading fails, or typed decoding fails.
void *rt_asset_load(rt_string name) {
    if (!name)
        return NULL;
    ensure_init();

    const char *cname = asset_logical_name_cstr(name);
    if (!asset_name_is_safe(cname))
        return NULL;
    size_t data_size = 0;
    uint8_t *data = asset_find_data(cname, &data_size);
    if (!data)
        return NULL;

    // Type dispatch by extension.
    void *result = rt_asset_decode_typed(cname, data, data_size);
    if (result) {
        free(data);
        return result;
    }

    // A recognized image/audio extension whose decode failed must NOT fall
    // back to Bytes — that would make the same suffix return two different
    // types depending on content validity (VDOC-181). Report failure instead.
    if (rt_asset_extension_is_typed(cname)) {
        free(data);
        return NULL;
    }

    // Unrecognized extension: return the raw bytes.
    result = rt_bytes_from_raw(data, data_size);
    free(data);
    return result;
}

// ─── rt_asset_load_bytes ────────────────────────────────────────────────────

/// @brief Load an asset as raw Bytes (no type dispatch, always returns Bytes or NULL).
/// @param name Safe relative asset name, optionally prefixed by `asset://`.
/// @return Fresh GC-managed Bytes object, or `NULL` when the name is invalid,
/// absent, unreadable, or cannot be allocated.
void *rt_asset_load_bytes(rt_string name) {
    if (!name)
        return NULL;
    ensure_init();

    const char *cname = asset_logical_name_cstr(name);
    if (!asset_name_is_safe(cname))
        return NULL;
    size_t data_size = 0;
    uint8_t *data = asset_find_data(cname, &data_size);
    if (!data)
        return NULL;

    void *result = rt_bytes_from_raw(data, data_size);
    free(data);
    return result;
}

/// @brief Load an asset into a malloc-owned raw byte buffer.
/// @param name Safe relative asset name, optionally prefixed by `asset://`.
/// @param out_size Receives the payload length on success and zero on failure;
/// may be `NULL`.
/// @return Heap buffer owned by the caller and released with `free`, or `NULL`
/// when the asset cannot be resolved.
uint8_t *rt_asset_load_raw(rt_string name, size_t *out_size) {
    if (out_size)
        *out_size = 0;
    if (!name)
        return NULL;
    ensure_init();

    const char *cname = asset_logical_name_cstr(name);
    if (!asset_name_is_safe(cname))
        return NULL;

    uint8_t *data = asset_find_data(cname, out_size);
    return data;
}

// ─── rt_asset_exists ────────────────────────────────────────────────────────

/// @brief Check whether an asset exists in any source (embedded, packs, or filesystem).
/// @param name Safe relative asset name, optionally prefixed by `asset://`.
/// @return 1 when a packed entry or regular loose file exists; otherwise 0.
int64_t rt_asset_exists(rt_string name) {
    if (!name)
        return 0;
    ensure_init();

    const char *cname = asset_logical_name_cstr(name);
    if (!asset_name_is_safe(cname))
        return 0;

    asset_lock();
    // Check embedded
    if (g_asset_mgr.embedded && zpak_find(g_asset_mgr.embedded, cname)) {
        asset_unlock();
        return 1;
    }

    // Check mounted packs
    for (int i = g_asset_mgr.pack_count - 1; i >= 0; --i) {
        if (g_asset_mgr.packs[i] && zpak_find(g_asset_mgr.packs[i], cname)) {
            asset_unlock();
            return 1;
        }
    }

    asset_unlock();
    return asset_regular_file_size(cname, NULL) ? 1 : 0;
}

// ─── rt_asset_size ──────────────────────────────────────────────────────────

/// @brief Get the byte size of an asset without loading it.
/// @param name Safe relative asset name, optionally prefixed by `asset://`.
/// @return Uncompressed pack-entry size or loose-file size, or -1 when the
/// name is invalid, absent, or too large for `int64_t`.
int64_t rt_asset_size(rt_string name) {
    if (!name)
        return -1;
    ensure_init();

    const char *cname = asset_logical_name_cstr(name);
    if (!asset_name_is_safe(cname))
        return -1;

    asset_lock();
    // Check embedded
    if (g_asset_mgr.embedded) {
        const zpak_entry_t *e = zpak_find(g_asset_mgr.embedded, cname);
        if (e) {
            asset_unlock();
            return (int64_t)e->data_size;
        }
    }

    // Check mounted packs
    for (int i = g_asset_mgr.pack_count - 1; i >= 0; --i) {
        if (!g_asset_mgr.packs[i])
            continue;
        const zpak_entry_t *e = zpak_find(g_asset_mgr.packs[i], cname);
        if (e) {
            asset_unlock();
            return (int64_t)e->data_size;
        }
    }

    asset_unlock();
    uint64_t fs_size = 0;
    if (asset_regular_file_size(cname, &fs_size) && fs_size <= INT64_MAX)
        return (int64_t)fs_size;
    return -1;
}

// ─── rt_asset_list ──────────────────────────────────────────────────────────

/// @brief Snapshot retained archive references in resolution-list order.
/// @param archives Fixed-capacity output array.
/// @param capacity Number of entries available in @p archives.
/// @return Number of retained archives written to the output array.
static size_t asset_snapshot_archives(zpak_archive_t **archives, size_t capacity) {
    if (!archives || capacity == 0)
        return 0;
    size_t count = 0;
    asset_lock();
    if (g_asset_mgr.embedded && count < capacity && zpak_retain(g_asset_mgr.embedded))
        archives[count++] = g_asset_mgr.embedded;
    for (int i = 0; i < g_asset_mgr.pack_count && count < capacity; ++i) {
        if (g_asset_mgr.packs[i] && zpak_retain(g_asset_mgr.packs[i]))
            archives[count++] = g_asset_mgr.packs[i];
    }
    asset_unlock();
    return count;
}

/// @brief Release all retained archive references in a fixed snapshot.
/// @param archives Snapshot populated by @ref asset_snapshot_archives.
/// @param count Number of initialized entries in @p archives.
static void asset_release_archive_snapshot(zpak_archive_t **archives, size_t count) {
    for (size_t i = 0; i < count; ++i)
        zpak_close(archives[i]);
}

/// @brief List all available asset names from embedded and mounted sources as a sequence.
/// @details Filesystem fallback assets are not enumerated. Names retain source
/// order (embedded first, then packs in mount order), and duplicates are not
/// removed.
/// @return Fresh element-owning `seq<str>`, or `NULL` on allocation failure.
void *rt_asset_list(void) {
    ensure_init();

    zpak_archive_t *archives[RT_ASSET_MAX_PACKS + 1];
    size_t archive_count =
        asset_snapshot_archives(archives, sizeof(archives) / sizeof(archives[0]));
    void *seq = NULL;
    rt_string pending = NULL;

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[512];
        const char *error = rt_trap_get_error();
        snprintf(saved_error,
                 sizeof(saved_error),
                 "%s",
                 error && error[0] ? error : "Assets.List: allocation failed");
        rt_trap_clear_recovery();
        rt_str_release_maybe(pending);
        if (seq && rt_obj_release_check0(seq))
            rt_obj_free(seq);
        asset_release_archive_snapshot(archives, archive_count);
        rt_trap(saved_error);
        return NULL;
    }

    seq = rt_seq_new();
    if (!seq) {
        rt_trap_clear_recovery();
        asset_release_archive_snapshot(archives, archive_count);
        return NULL;
    }
    rt_seq_set_owns_elements(seq, 1);
    for (size_t archive_index = 0; archive_index < archive_count; ++archive_index) {
        zpak_archive_t *archive = archives[archive_index];
        for (uint32_t i = 0; i < archive->count; ++i) {
            const char *name = archive->entries[i].name;
            pending = rt_string_from_bytes(name, strlen(name));
            rt_seq_push(seq, (void *)pending);
            rt_string_unref(pending);
            pending = NULL;
        }
    }

    rt_trap_clear_recovery();
    asset_release_archive_snapshot(archives, archive_count);
    return seq;
}

// ─── rt_asset_mount ─────────────────────────────────────────────────────────

/// @brief Mount an additional ZPAK pack file at runtime (assets become available immediately).
/// @param path Runtime filesystem path to a valid ZPAK file.
/// @return 1 when mounted or already present, or 0 for invalid input, open or
/// canonicalization failure, or the 32-pack capacity limit.
int64_t rt_asset_mount(rt_string path) {
    if (!path)
        return 0;
    ensure_init();

    const char *cpath = NULL;
    if (!rt_file_path_from_vstr((const ZannaString *)path, &cpath) || !cpath || *cpath == '\0')
        return 0;
    zpak_archive_t *archive = zpak_open_file(cpath);
    if (!archive)
        return 0;

    char *path_copy = asset_canonical_path_dup(cpath);
    if (!path_copy) {
        zpak_close(archive);
        return 0;
    }

    asset_lock();
    if (g_asset_mgr.pack_count >= RT_ASSET_MAX_PACKS) {
        asset_unlock();
        free(path_copy);
        zpak_close(archive);
        return 0;
    }
    for (int i = 0; i < g_asset_mgr.pack_count; ++i) {
        if (g_asset_mgr.pack_paths[i] && asset_path_equal(g_asset_mgr.pack_paths[i], path_copy)) {
            asset_unlock();
            free(path_copy);
            zpak_close(archive);
            return 1;
        }
    }
    g_asset_mgr.packs[g_asset_mgr.pack_count] = archive;
    g_asset_mgr.pack_paths[g_asset_mgr.pack_count] = path_copy;
    g_asset_mgr.pack_count++;
    asset_unlock();
    return 1;
}

// ─── rt_asset_unmount ───────────────────────────────────────────────────────

/// @brief Unmount a previously-mounted ZPAK pack file.
/// @details An exact canonical path is preferred. A separator-free basename
/// is accepted only when it uniquely identifies one mounted pack. Active
/// readers remain safe through retained archive references.
/// @param path Runtime path or unique basename of the mounted pack.
/// @return 1 when a pack was removed; otherwise 0.
int64_t rt_asset_unmount(rt_string path) {
    if (!path)
        return 0;
    ensure_init();

    const char *cpath = NULL;
    if (!rt_file_path_from_vstr((const ZannaString *)path, &cpath) || !cpath || *cpath == '\0')
        return 0;
    char *search_path = asset_canonical_path_dup(cpath);
    if (!search_path)
        return 0;

    asset_lock();
    int match = -1;
    for (int i = g_asset_mgr.pack_count - 1; i >= 0; --i) {
        if (g_asset_mgr.pack_paths[i] && asset_path_equal(g_asset_mgr.pack_paths[i], search_path)) {
            match = i;
            break;
        }
    }
    if (match < 0 && !asset_path_has_separator(cpath)) {
        int basename_matches = 0;
        for (int i = g_asset_mgr.pack_count - 1; i >= 0; --i) {
            if (g_asset_mgr.pack_paths[i] &&
                asset_path_equal(asset_path_basename(g_asset_mgr.pack_paths[i]), cpath)) {
                match = i;
                basename_matches++;
            }
        }
        if (basename_matches != 1)
            match = -1;
    }
    if (match >= 0) {
        zpak_archive_t *archive = g_asset_mgr.packs[match];
        char *mounted_path = g_asset_mgr.pack_paths[match];

        // Shift remaining packs down
        for (int j = match; j < g_asset_mgr.pack_count - 1; j++) {
            g_asset_mgr.packs[j] = g_asset_mgr.packs[j + 1];
            g_asset_mgr.pack_paths[j] = g_asset_mgr.pack_paths[j + 1];
        }
        g_asset_mgr.pack_count--;
        g_asset_mgr.packs[g_asset_mgr.pack_count] = NULL;
        g_asset_mgr.pack_paths[g_asset_mgr.pack_count] = NULL;
        asset_unlock();
        zpak_close(archive);
        free(mounted_path);
        free(search_path);
        return 1;
    }

    asset_unlock();
    free(search_path);
    return 0;
}
