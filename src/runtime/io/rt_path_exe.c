//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/io/rt_path_exe.c
// Purpose: Cross-platform detection of the running executable's directory.
//          Provides Path.ExeDir() for Zia/BASIC and a C helper for the asset
//          manager's auto-discovery of .zpak pack files.
//
// Key invariants:
//   - Returns the directory containing the executable, not the exe path itself.
//   - macOS: uses _NSGetExecutablePath + realpath + dirname.
//   - Windows: uses a growing GetModuleFileNameW buffer + strict UTF-8 conversion.
//   - Linux: uses readlink("/proc/self/exe") + dirname.
//   - Windows, macOS, and Linux size or grow probe buffers with explicit truncation handling.
//     The runtime wrapper falls back to "." after probe errors.
//   - Returned C strings are malloc'd; caller must free.
//   - Returned runtime strings are GC-managed.
//
// Ownership/Lifetime:
//   - rt_path_exe_dir_cstr() returns a malloc'd string (caller frees).
//   - rt_path_exe_dir_str() returns a GC-managed runtime string.
//
// Links: src/runtime/io/rt_asset.c (consumer),
//        src/runtime/io/rt_path.c and src/runtime/io/rt_path.h (path API)
//
//===----------------------------------------------------------------------===//

#include "rt_string.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <dlfcn.h>
#include <limits.h>
#elif defined(__linux__)
#include <limits.h>
#include <unistd.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

// ─── dirname helper ─────────────────────────────────────────────────────────

/// @brief Strip the last path component from a path string (in-place).
/// @details Recognizes both slash spellings because Windows paths may contain either. When no
///          separator exists, replaces the input with `"."`.
/// @param[in,out] path Mutable null-terminated string changed to its directory prefix; NULL is
///                     ignored.
static void strip_filename(char *path) {
    if (!path)
        return;
    size_t len = strlen(path);
    // Walk backwards to find the last separator.
    while (len > 0) {
        len--;
        if (path[len] == '/' || path[len] == '\\') {
            path[len] = '\0';
            return;
        }
    }
    // No separator found — return "." for current directory.
    path[0] = '.';
    path[1] = '\0';
}

// ─── rt_path_exe_dir_cstr ───────────────────────────────────────────────────

/// @brief Get the directory of the running executable as a C string.
/// @details Uses native wide paths on Windows, dynamically resolved `_NSGetExecutablePath` plus
///          `realpath` on macOS, and the `/proc/self/exe` symlink on Linux. Probe buffers grow
///          until the full executable path fits, subject to a defensive upper bound.
/// @return Malloc-owned directory string for the caller to free; NULL on supported-platform probe,
///         conversion, or allocation failure. Unknown platforms return an allocated `"."`.
char *rt_path_exe_dir_cstr(void) {
#if defined(_WIN32)
    // Windows: GetModuleFileNameW into a growing buffer so long paths and paths
    // not representable in the ANSI code page are handled, with explicit
    // truncation detection (VDOC-185). When the return equals the buffer size
    // the path was truncated (GetLastError() == ERROR_INSUFFICIENT_BUFFER).
    DWORD cap = 256;
    for (;;) {
        wchar_t *wbuf = (wchar_t *)malloc((size_t)cap * sizeof(wchar_t));
        if (!wbuf)
            return NULL;
        DWORD len = GetModuleFileNameW(NULL, wbuf, cap);
        if (len == 0) {
            free(wbuf);
            return NULL;
        }
        if (len >= cap) {
            free(wbuf);
            if (cap > (DWORD)0x40000000u)
                return NULL; // refuse to grow without bound
            cap *= 2;
            continue;
        }
        wbuf[len] = L'\0';
        int u8_len =
            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wbuf, -1, NULL, 0, NULL, NULL);
        if (u8_len <= 0) {
            free(wbuf);
            return NULL;
        }
        char *u8 = (char *)malloc((size_t)u8_len);
        if (!u8) {
            free(wbuf);
            return NULL;
        }
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wbuf, -1, u8, u8_len, NULL, NULL) !=
            u8_len) {
            free(u8);
            free(wbuf);
            return NULL;
        }
        free(wbuf);
        strip_filename(u8);
        return u8;
    }

#elif defined(__APPLE__)
    // macOS: _NSGetExecutablePath via dlsym (avoids native linker binding
    // issues). It reports the required size when the buffer is too small, so we
    // size dynamically instead of falling back to "." on long paths (VDOC-185).
    {
        typedef int (*nsget_fn)(char *, uint32_t *);
        void *handle = dlopen(NULL, RTLD_LAZY);
        if (!handle)
            return NULL;
        nsget_fn fn = (nsget_fn)dlsym(handle, "_NSGetExecutablePath");
        if (!fn) {
            dlclose(handle);
            return NULL;
        }

        uint32_t size = 0;
        fn(NULL, &size); // query required size (returns -1, sets size)
        if (size == 0)
            size = PATH_MAX;
        char *raw = (char *)malloc(size);
        if (!raw) {
            dlclose(handle);
            return NULL;
        }
        if (fn(raw, &size) != 0) {
            free(raw);
            dlclose(handle);
            return NULL;
        }
        dlclose(handle);

        char *resolved = realpath(raw, NULL); // NULL => malloc'd result
        free(raw);
        if (!resolved)
            return NULL;
        strip_filename(resolved);
        return resolved; // realpath's buffer is malloc'd; caller frees
    }

#elif defined(__linux__)
    // Linux: readlink("/proc/self/exe") into a growing buffer with truncation
    // detection — a return equal to the buffer size may be truncated, so grow
    // and retry (VDOC-185).
    {
        size_t cap = 256;
        for (;;) {
            char *buf = (char *)malloc(cap);
            if (!buf)
                return NULL;
            ssize_t len = readlink("/proc/self/exe", buf, cap);
            if (len <= 0) {
                free(buf);
                return NULL;
            }
            if ((size_t)len == cap) {
                free(buf);
                if (cap > (size_t)0x40000000u)
                    return NULL; // refuse to grow without bound
                cap *= 2;
                continue;
            }
            buf[len] = '\0';
            strip_filename(buf);
            return buf;
        }
    }

#else
    // Unknown platform fallback.
    return strdup(".");
#endif
}

// ─── rt_path_exe_dir_str (runtime API) ──────────────────────────────────────

/// @brief Zanna.IO.Path.ExeDir() — returns directory of the running executable.
/// @details Copies and frees the native C-string result. A failed native probe is converted to the
///          current-directory fallback.
/// @return Caller-owned runtime string containing the executable directory, or `"."` when
///         detection fails.
rt_string rt_path_exe_dir_str(void) {
    char *dir = rt_path_exe_dir_cstr();
    if (!dir)
        return rt_string_from_bytes(".", 1);

    rt_string result = rt_string_from_bytes(dir, strlen(dir));
    free(dir);
    return result;
}
