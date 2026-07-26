//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/io/rt_tempfile.c
// Purpose: Temporary-path and temporary-file helpers backing the static
//          Zanna.IO.TempFile API. Creates names in the system temporary
//          directory using the shared platform entropy adapter.
//
// Key invariants:
//   - Temporary file names use the runtime's cross-platform entropy helper and
//     exclusive creation where an actual file is requested.
//   - Files are created in the platform temp directory (GetTempPath on Windows,
//     $TMPDIR or /tmp on Unix).
//   - Generated IDs are hex-encoded for filesystem compatibility.
//   - All functions trap on allocation failure or file creation errors.
//
// Ownership/Lifetime:
//   - Each call returns a fresh runtime String containing a path.
//   - There is no TempFile handle or finalizer; callers remove created files.
//
// Links: src/runtime/io/rt_tempfile.h (public API),
//        src/runtime/io/rt_dir.h (used to resolve the temporary directory),
//        src/runtime/io/rt_path.h (path join for constructing temp file names)
//
//===----------------------------------------------------------------------===//
/**
 * @file
 * @brief Implements entropy-backed temporary paths and exclusive creation.
 * @details Validates filename fragments, obtains 128 bits of platform entropy,
 * resolves safe usable temp roots, builds portable candidate names, retries
 * collisions, and creates non-inheritable files or owner-only directories
 * without replacing existing filesystem entries.
 */

#include "rt_tempfile.h"

#include "network/rt_entropy_platform.h"
#include "rt_dir.h"
#include "rt_file_path.h"
#include "rt_path.h"
#include "rt_platform.h"
#include "rt_string.h"

#include "rt_trap.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#include <windows.h>
#ifndef _WINDOWS_
#error "windows.h must be included before wincrypt.h"
#endif
#include <wincrypt.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

//=============================================================================
// Internal Helpers
//=============================================================================

/// @brief Validate and unwrap a path-fragment string.
/// @details Traps on NULL, negative length, embedded NUL, or platform/path
///          separator characters. Unlike the old helper, failure is reported
///          explicitly so callers can return immediately when a trap handler
///          recovers.
/// @param fragment Runtime string to validate as a filename fragment.
/// @param what Trap diagnostic.
/// @param out_cstr Receives the borrowed C string on success.
/// @return 1 when valid; 0 after trapping.
static int tempfile_require_fragment(rt_string fragment, const char *what, const char **out_cstr) {
    const char *cstr = rt_string_cstr(fragment);
    if (out_cstr)
        *out_cstr = "";
    if (!cstr) {
        rt_trap(what);
        return 0;
    }
    int64_t len = rt_str_len(fragment);
    if (len < 0) {
        rt_trap(what);
        return 0;
    }
    for (int64_t i = 0; i < len; ++i) {
        if (cstr[i] == '\0' || cstr[i] == '/' || cstr[i] == '\\' || cstr[i] == ':') {
            rt_trap(what);
            return 0;
        }
    }
    if (out_cstr)
        *out_cstr = cstr;
    return 1;
}

/// @brief Generate a unique identifier using OS-provided entropy (S-21).
/// @details Fails closed when platform entropy is unavailable so temporary path names are never
///          derived from predictable process IDs, timestamps, stack addresses, or counters. The
///          caller supplies room for the 32 lowercase hexadecimal digits plus
///          the terminating NUL.
/// @param buffer Destination for the encoded 128-bit identifier.
/// @param size Capacity of @p buffer in bytes; expected to be at least 33.
/// @return 1 when @p buffer was filled with a hexadecimal identifier, 0 on entropy failure.
static int generate_unique_id(char *buffer, size_t size) {
    uint64_t rnd_hi = 0;
    uint64_t rnd_lo = 0;

    if (rt_entropy_platform_random_u64(&rnd_hi) != 0 ||
        rt_entropy_platform_random_u64(&rnd_lo) != 0) {
        rt_trap("TempFile: failed to obtain secure randomness");
        return 0;
    }

    snprintf(
        buffer, size, "%016llx%016llx", (unsigned long long)rnd_hi, (unsigned long long)rnd_lo);
    return 1;
}

#if defined(_WIN32)
/** Signature shared by Windows temporary/current-directory query functions. */
typedef DWORD(WINAPI *tempfile_path_query_fn)(DWORD, LPWSTR);

/// @brief Return nonzero when a separator-terminated path is a Windows volume root.
/// @details Covers drive, UNC, extended drive, extended UNC, and volume-GUID roots so
///          normalization never turns `\\?\C:\` into the drive-relative `\\?\C:`.
/// @param path Native path to classify; separators may be slash or backslash.
/// @param length Number of wide characters before the terminating NUL.
/// @return 1 when the complete path denotes a recognized root, otherwise 0.
static int tempfile_windows_path_is_root(const wchar_t *path, size_t length) {
    size_t start = 0;
    size_t required_components = 0;
    size_t components = 0;
    int in_component = 0;
    if (!path || length == 0 || (path[length - 1] != L'\\' && path[length - 1] != L'/'))
        return 0;
    if (length == 1)
        return 1;
    if (length == 3 && path[1] == L':')
        return 1;
    if (length >= 8 && path[0] == L'\\' && path[1] == L'\\' && path[2] == L'?' &&
        path[3] == L'\\' && (path[4] == L'U' || path[4] == L'u') &&
        (path[5] == L'N' || path[5] == L'n') && (path[6] == L'C' || path[6] == L'c') &&
        (path[7] == L'\\' || path[7] == L'/')) {
        start = 8;
        required_components = 2;
    } else if (length >= 4 && path[0] == L'\\' && path[1] == L'\\' && path[2] == L'?' &&
               path[3] == L'\\') {
        start = 4;
        required_components = 1;
    } else if (length >= 2 && path[0] == L'\\' && path[1] == L'\\') {
        start = 2;
        required_components = 2;
    } else {
        return 0;
    }
    for (size_t i = start; i + 1u < length; ++i) {
        if (path[i] == L'\\' || path[i] == L'/') {
            if (in_component)
                components++;
            in_component = 0;
        } else {
            in_component = 1;
        }
    }
    if (in_component)
        components++;
    return components == required_components;
}

/// @brief Query, normalize, and validate one native Windows directory provider.
/// @details The provider is retried with a larger heap buffer when necessary.
///          Non-root trailing separators are removed, and the resulting path
///          must name an existing directory before it is converted to a
///          runtime UTF-8 string.
/// @param query Windows path-provider function such as GetTempPathW,
///              GetTempPath2W, or GetCurrentDirectoryW.
/// @return Fresh runtime string for a usable directory, or NULL when the
///         provider fails, returns invalid data, or allocation/conversion fails.
static rt_string tempfile_windows_directory_from_query(tempfile_path_query_fn query) {
    if (!query)
        return NULL;
    DWORD capacity = query(0, NULL);
    for (int attempt = 0; capacity > 0 && attempt < 8; ++attempt) {
        if (capacity == MAXDWORD || (size_t)capacity + 1u > SIZE_MAX / sizeof(wchar_t))
            return NULL;
        capacity++;
        wchar_t *buffer = (wchar_t *)malloc((size_t)capacity * sizeof(wchar_t));
        if (!buffer)
            return NULL;
        DWORD length = query(capacity, buffer);
        if (length == 0) {
            free(buffer);
            return NULL;
        }
        if (length >= capacity) {
            free(buffer);
            capacity = length;
            continue;
        }

        while (length > 1 && (buffer[length - 1] == L'\\' || buffer[length - 1] == L'/') &&
               !tempfile_windows_path_is_root(buffer, (size_t)length)) {
            buffer[--length] = L'\0';
        }
        DWORD attributes = GetFileAttributesW(buffer);
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            free(buffer);
            return NULL;
        }
        rt_string result = rt_file_path_wide_to_string(buffer);
        free(buffer);
        if (result && rt_str_len(result) > 0)
            return result;
        if (result)
            rt_string_unref(result);
        return NULL;
    }
    return NULL;
}
#endif

#if !defined(_WIN32)
/// @brief Validate a POSIX temporary directory candidate.
/// @details A usable candidate must be absolute and name an existing
///          directory. The historical `TempFile.Dir` contract preserves `/`
///          exactly when TMPDIR points at the filesystem root, so root is
///          accepted for the path-reporting API even though creating files
///          there may later fail for unprivileged callers. Other directories
///          must be searchable and writable, and world-writable directories
///          must carry the sticky bit to avoid unsafe shared-temp semantics.
/// @param path Candidate path from the process environment.
/// @return 1 when the candidate can be returned by @ref rt_tempfile_dir, 0 when
///         the caller should fall back to `/tmp`.
static int tempfile_dir_is_usable(const char *path) {
    if (!path || path[0] != '/')
        return 0;
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
        return 0;
    if (path[1] == '\0')
        return 1;
    if (access(path, W_OK | X_OK) != 0)
        return 0;
    if ((st.st_mode & S_IWOTH) != 0 && (st.st_mode & S_ISVTX) == 0)
        return 0;
    return 1;
}
#endif

/// @brief Atomically attempt to create an empty file at an exact path.
/// @details Creation fails rather than replacing an existing entry. Windows
///          uses CreateFileW with CREATE_NEW and closes the non-shared handle;
///          POSIX uses `open(O_CREAT | O_EXCL, 0600)`, applies close-on-exec
///          when supported, and closes the descriptor before return.
/// @param cpath NUL-terminated UTF-8 path to create.
/// @return 1 when created, 0 for a name collision that the caller may retry,
///         or -1 after trapping on conversion, creation, or close failure.
static int tempfile_try_create_path(const char *cpath) {
#ifdef _WIN32
    wchar_t *wide_path = rt_file_path_utf8_to_wide(cpath);
    if (!wide_path) {
        rt_trap("TempFile.Create: invalid temporary file path");
        return -1;
    }
    HANDLE h = CreateFileW(wide_path,
                           GENERIC_WRITE,
                           0,
                           NULL,
                           CREATE_NEW,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED,
                           NULL);
    if (h != INVALID_HANDLE_VALUE) {
        if (!CloseHandle(h)) {
            (void)DeleteFileW(wide_path);
            free(wide_path);
            rt_trap("TempFile.Create: failed to close temporary file");
            return -1;
        }
        free(wide_path);
        return 1;
    }

    DWORD err = GetLastError();
    free(wide_path);
    if (err == ERROR_FILE_EXISTS || err == ERROR_ALREADY_EXISTS)
        return 0;

    rt_trap("TempFile.Create: failed to create temporary file");
    return -1;
#else
    int flags = O_CREAT | O_EXCL | O_WRONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int tmp_fd = open(cpath, flags, 0600);
    if (tmp_fd >= 0) {
#if defined(FD_CLOEXEC) && !defined(O_CLOEXEC)
        int fd_flags = fcntl(tmp_fd, F_GETFD);
        if (fd_flags >= 0)
            (void)fcntl(tmp_fd, F_SETFD, fd_flags | FD_CLOEXEC);
#endif
        close(tmp_fd);
        return 1;
    }

    if (errno == EEXIST)
        return 0;

    rt_trap("TempFile.Create: failed to create temporary file");
    return -1;
#endif
}

/// @brief Atomically attempt to create an empty directory at an exact path.
/// @details Windows converts the UTF-8 path and calls `_wmkdir`; POSIX uses
///          `mkdir(0700)` so only the owner receives permissions before umask
///          restrictions. Existing names are reported as retryable collisions.
/// @param cpath NUL-terminated UTF-8 path to create.
/// @return 1 when created, 0 for a name collision that the caller may retry,
///         or -1 after trapping on conversion or creation failure.
static int tempfile_try_create_dir(const char *cpath) {
#ifdef _WIN32
    wchar_t *wide_path = rt_file_path_utf8_to_wide(cpath);
    if (!wide_path) {
        rt_trap("TempFile.CreateDir: invalid temporary directory path");
        return -1;
    }
    if (_wmkdir(wide_path) == 0) {
        free(wide_path);
        return 1;
    }
    free(wide_path);
#else
    if (mkdir(cpath, 0700) == 0)
        return 1;
#endif
    if (errno == EEXIST)
        return 0;

    rt_trap("TempFile.CreateDir: failed to create temporary directory");
    return -1;
}

//=============================================================================
// Public API
//=============================================================================

/// @brief Resolve a usable platform temporary directory.
/// @details Windows prefers GetTempPath2W when available, then GetTempPathW,
///          and finally the existing process current directory; trailing
///          separators are removed except on volume roots. POSIX accepts an
///          absolute, existing, writable/searchable TMPDIR, rejecting unsafe
///          non-sticky world-writable directories, and otherwise uses `/tmp`.
/// @return Runtime string reference containing the normalized directory path;
///         Windows traps and returns the shared empty string if every provider fails.
rt_string rt_tempfile_dir(void) {
#ifdef _WIN32
    tempfile_path_query_fn query = NULL;
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (kernel32)
        query = (tempfile_path_query_fn)(void *)GetProcAddress(kernel32, "GetTempPath2W");
    rt_string result = tempfile_windows_directory_from_query(query);
    if (!result)
        result = tempfile_windows_directory_from_query(GetTempPathW);
    if (!result) {
        /* A malformed or stale environment-provided temp path must not escape
         * as a plausible but nonexistent C:\Temp fallback. The process current
         * directory is an existing absolute last resort. */
        result = tempfile_windows_directory_from_query(GetCurrentDirectoryW);
    }
    if (result)
        return result;
    rt_trap("TempFile.Dir: no usable temporary directory");
    return rt_str_empty();
#else
    const char *tmp = getenv("TMPDIR");
    if (tmp && *tmp) {
        size_t len = strlen(tmp);
        if (!tempfile_dir_is_usable(tmp))
            return rt_const_cstr("/tmp");
        // Remove trailing slash if present
        if (len > 1 && tmp[len - 1] == '/') {
            char *copy = (char *)malloc(len);
            if (!copy) {
                rt_trap("rt_tempfile: memory allocation failed");
                return rt_const_cstr("/tmp");
            }
            memcpy(copy, tmp, len - 1);
            copy[len - 1] = '\0';
            rt_string result = rt_string_from_bytes(copy, len - 1);
            free(copy);
            return result;
        }
        return rt_string_from_bytes(tmp, len);
    }
    return rt_const_cstr("/tmp");
#endif
}

/// @brief Generate a unique temp-file PATH (does NOT create the file). Default prefix "zanna_",
/// extension ".tmp". Use when you want to atomically open it yourself with specific flags.
/// @details Path uniqueness is probabilistic and does not reserve the name.
/// @return Fresh runtime string containing the candidate path, or an empty
///         string after entropy, validation, or allocation failure.
rt_string rt_tempfile_path(void) {
    return rt_tempfile_path_with_prefix(rt_const_cstr("zanna_"));
}

/// @brief Generate an unreserved temporary path with a custom prefix.
/// @details The generated filename uses the default `.tmp` extension.
/// @param prefix Filename fragment placed before the random identifier; path
///               separators, colon, embedded NUL, and invalid strings trap.
/// @return Fresh runtime string containing the candidate path, or an empty
///         string after failure.
rt_string rt_tempfile_path_with_prefix(rt_string prefix) {
    return rt_tempfile_path_with_ext(prefix, rt_const_cstr(".tmp"));
}

/// @brief Path generator with custom prefix AND extension. Format:
/// `{tempdir}/{prefix}{32-hex-id}{ext}`. The 32-hex random ID gives ~2^128 entropy — collision
/// chance remains negligible even for high-volume temp-path generation.
/// @details This function only constructs a path and does not reserve it.
///          Both caller fragments are restricted to portable filename text
///          without slash, backslash, colon, embedded NUL, or invalid storage.
/// @param prefix Filename fragment placed before the random identifier.
/// @param extension Filename fragment placed after the random identifier,
///                  commonly including a leading period.
/// @return Fresh runtime string containing the joined path, or an empty string
///         after entropy, validation, overflow, or allocation failure.
rt_string rt_tempfile_path_with_ext(rt_string prefix, rt_string extension) {
    char unique_id[64];
    if (!generate_unique_id(unique_id, sizeof(unique_id)))
        return rt_str_empty();

    const char *prefix_cstr = "";
    const char *ext_cstr = "";
    if (!tempfile_require_fragment(prefix, "TempFile: invalid prefix", &prefix_cstr) ||
        !tempfile_require_fragment(extension, "TempFile: invalid extension", &ext_cstr))
        return rt_str_empty();

    // Build filename: prefix + unique_id + extension
    size_t prefix_len = strlen(prefix_cstr);
    size_t unique_len = strlen(unique_id);
    size_t ext_len = strlen(ext_cstr);
    if (prefix_len > SIZE_MAX - unique_len - ext_len - 1) {
        rt_trap("TempFile: path length overflow");
        return rt_str_empty();
    }
    size_t filename_len = prefix_len + unique_len + ext_len + 1;
    char *filename = (char *)malloc(filename_len);
    if (!filename) {
        rt_trap("TempFile: memory allocation failed");
        return rt_str_empty();
    }
    snprintf(filename, filename_len, "%s%s%s", prefix_cstr, unique_id, ext_cstr);

    rt_string temp_dir = rt_tempfile_dir();
    rt_string fname_str = rt_string_from_bytes(filename, strlen(filename));
    free(filename);

    rt_string result = rt_path_join(temp_dir, fname_str);
    rt_string_unref(fname_str);
    rt_string_unref(temp_dir);

    return result;
}

/// @brief Atomically create a temp file (path + filesystem object). Default prefix "zanna_".
/// Returns the path of the freshly-created (empty) file.
/// @details The file is closed before return and remains on disk until the
///          caller removes it.
/// @return Runtime string reference naming the created file, or an empty
///         string after a trapped creation failure.
rt_string rt_tempfile_create(void) {
    return rt_tempfile_create_with_prefix(rt_const_cstr("zanna_"));
}

/// @brief Atomically create an empty temporary file with a custom prefix.
/// @details POSIX first uses `mkstemp` with a six-character replacement
///          template and closes the descriptor before returning. Windows, and
///          POSIX after `mkstemp` failure, try up to 128 independently generated
///          128-bit names with exclusive-create semantics. No created file is
///          automatically deleted.
/// @param prefix Portable filename fragment placed before the generated suffix.
/// @return Runtime string reference naming the created file, or an empty
///         string after validation or creation failure.
rt_string rt_tempfile_create_with_prefix(rt_string prefix) {
#ifndef _WIN32
    /* S-21: Use mkstemp for atomic, exclusive, unpredictable file creation on POSIX */
    const char *prefix_cstr = "";
    if (!tempfile_require_fragment(prefix, "TempFile.Create: invalid prefix", &prefix_cstr))
        return rt_str_empty();
    rt_string temp_dir = rt_tempfile_dir();
    const char *dir_cstr = rt_string_cstr(temp_dir);

    size_t dir_len = strlen(dir_cstr);
    size_t prefix_len = strlen(prefix_cstr);
    if (dir_len > SIZE_MAX - prefix_len - 8) {
        rt_string_unref(temp_dir);
        rt_trap("TempFile.Create: path length overflow");
        return rt_str_empty();
    }
    size_t tmpl_len = dir_len + 1 + prefix_len + 6 + 1;
    char *tmpl = (char *)malloc(tmpl_len);
    if (!tmpl) {
        rt_string_unref(temp_dir);
        rt_trap("TempFile.Create: memory allocation failed");
        return rt_str_empty();
    }
    snprintf(tmpl, tmpl_len, "%s/%sXXXXXX", dir_cstr, prefix_cstr);
    rt_string_unref(temp_dir);

    int fd = mkstemp(tmpl);
    if (fd >= 0) {
#ifdef FD_CLOEXEC
        int fd_flags = fcntl(fd, F_GETFD);
        if (fd_flags >= 0)
            (void)fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);
#endif
        close(fd);
        rt_string result = rt_string_from_bytes(tmpl, strlen(tmpl));
        free(tmpl);
        return result;
    }
    free(tmpl);
    /* Fall through to path-based creation on mkstemp failure */
#endif

    for (int attempt = 0; attempt < 128; attempt++) {
        rt_string path = rt_tempfile_path_with_prefix(prefix);
        const char *cpath = rt_string_cstr(path);
        int created = tempfile_try_create_path(cpath);
        if (created > 0)
            return path;
        rt_string_unref(path);
        if (created < 0)
            return rt_const_cstr("");
    }

    rt_trap("TempFile.Create: failed to generate a unique temporary file path");
    return rt_const_cstr("");
}

/// @brief Atomically create a temp DIRECTORY (not a file). Default prefix "zanna_". Mode 0700
/// on POSIX (owner-only access). The directory companion to `_create`.
/// @details The empty directory persists until the caller removes it.
/// @return Runtime string reference naming the created directory, or an empty
///         string after a trapped creation failure.
rt_string rt_tempdir_create(void) {
    return rt_tempdir_create_with_prefix(rt_const_cstr("zanna_"));
}

/// @brief Atomic temp-directory creation with custom prefix. Same retry pattern as `_create`
/// but uses `mkdir`/`_wmkdir` as the atomic primitive.
/// @details Up to 128 independently generated 128-bit names are attempted.
///          POSIX requests mode 0700, subject to the process umask.
/// @param prefix Portable filename fragment placed before the random identifier.
/// @return Runtime string reference naming the created directory, or an empty
///         string after validation or creation failure.
rt_string rt_tempdir_create_with_prefix(rt_string prefix) {
    const char *prefix_cstr = "";
    if (!tempfile_require_fragment(prefix, "TempFile.CreateDir: invalid prefix", &prefix_cstr))
        return rt_const_cstr("");
    (void)prefix_cstr;
    for (int attempt = 0; attempt < 128; attempt++) {
        rt_string result = rt_tempfile_path_with_ext(prefix, rt_const_cstr(""));
        const char *cpath = rt_string_cstr(result);
        int created = tempfile_try_create_dir(cpath);
        if (created > 0)
            return result;
        rt_string_unref(result);
        if (created < 0)
            return rt_const_cstr("");
    }

    rt_trap("TempFile.CreateDir: failed to generate a unique temporary directory path");
    return rt_const_cstr("");
}
