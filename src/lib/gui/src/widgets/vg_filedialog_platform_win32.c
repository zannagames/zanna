//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file vg_filedialog_platform_win32.c
/// @brief Implements the Win32 filesystem adapter for the file dialog.
///
/// @details Paths remain UTF-8 at the widget boundary and are converted to
/// UTF-16 for wide-character Win32 APIs. Directory enumeration uses
/// `FindFirstFileW()` and `FindNextFileW()`, closes every search handle before
/// return, and derives hidden state from `FILE_ATTRIBUTE_HIDDEN`.
///
/// All returned buffers and entry arrays use malloc-compatible storage and
/// transfer ownership to the caller. System-drive initialization is guarded by
/// `INIT_ONCE` for process-wide thread safety.
///
/// @see vg_filedialog_platform.h
/// @see vg_filedialog.c
//
//===----------------------------------------------------------------------===//

#include "vg_filedialog_platform.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

/// @brief Duplicate a C string using malloc-owned storage.
/// @details Avoids mixing CRT-specific duplicate helpers with widget code.
///          NULL input returns NULL.
/// @param text Source string to duplicate.
/// @return Newly allocated copy, or NULL on allocation failure.
static char *vg_filedialog_platform_strdup(const char *text) {
    if (!text)
        return NULL;
    size_t len = strlen(text);
    char *copy = (char *)malloc(len + 1u);
    if (!copy)
        return NULL;
    memcpy(copy, text, len + 1u);
    return copy;
}

/// @brief Convert a UTF-8 path string to a heap-owned UTF-16 Windows string.
/// @details The file dialog stores paths as UTF-8 internally.  Win32 filesystem
///          APIs are called through their wide-character variants so non-ACP
///          paths work reliably.  NULL input returns NULL.
/// @param text UTF-8 string to convert.
/// @return Wide string allocated with malloc, or NULL on conversion/allocation failure.
static wchar_t *vg_filedialog_platform_utf8_to_wide(const char *text) {
    if (!text)
        return NULL;
    int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (needed <= 0)
        return NULL;
    wchar_t *wide = (wchar_t *)malloc((size_t)needed * sizeof(wchar_t));
    if (!wide)
        return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, needed) != needed) {
        free(wide);
        return NULL;
    }
    return wide;
}

/// @brief Convert a UTF-16 Windows string to a heap-owned UTF-8 string.
/// @param text Wide string to convert.
/// @return UTF-8 string allocated with malloc, or NULL on conversion/allocation failure.
static char *vg_filedialog_platform_wide_to_utf8(const wchar_t *text) {
    if (!text)
        return NULL;
    int needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, NULL, 0, NULL, NULL);
    if (needed <= 0)
        return NULL;
    char *utf8 = (char *)malloc((size_t)needed);
    if (!utf8)
        return NULL;
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, utf8, needed, NULL, NULL) !=
        needed) {
        free(utf8);
        return NULL;
    }
    return utf8;
}

/// @brief Reads a Windows environment variable into an owned wide string.
///
/// @details The helper retries when the value grows between size discovery and
/// retrieval, with a bounded attempt count to avoid unending churn.
///
/// @param name Non-empty wide environment-variable name.
/// @return Owned null-terminated value, or null when absent, unstable, invalid,
///         or allocation fails.
static wchar_t *vg_filedialog_platform_environment_wide(const wchar_t *name) {
    DWORD capacity;
    if (!name || !*name)
        return NULL;
    capacity = GetEnvironmentVariableW(name, NULL, 0);
    if (capacity == 0)
        return NULL;
    for (int attempt = 0; attempt < 8; attempt++) {
        if ((size_t)capacity > SIZE_MAX / sizeof(wchar_t))
            return NULL;
        wchar_t *value = (wchar_t *)malloc((size_t)capacity * sizeof(wchar_t));
        if (!value)
            return NULL;
        DWORD length = GetEnvironmentVariableW(name, value, capacity);
        if (length > 0 && length < capacity) {
            value[length] = L'\0';
            return value;
        }
        free(value);
        if (length == 0 || length == UINT32_MAX)
            return NULL;
        capacity = length + 1u;
    }
    return NULL;
}

/// @brief Append one entry to a growing directory-entry array.
/// @details Takes ownership of the strings stored in @p entry only when the
///          append succeeds. On allocation failure the caller still owns them.
/// @param entries_io Entry array pointer to grow.
/// @param count_io Current entry count, incremented on success.
/// @param capacity_io Current array capacity, updated on growth.
/// @param entry Entry value to append.
/// @return Non-zero on success, zero on allocation failure.
static bool vg_filedialog_platform_append_entry(vg_filedialog_platform_entry_t **entries_io,
                                                size_t *count_io,
                                                size_t *capacity_io,
                                                vg_filedialog_platform_entry_t entry) {
    if (*count_io >= *capacity_io) {
        size_t new_cap;
        if (*capacity_io == 0u) {
            new_cap = 64u;
        } else {
            if (*capacity_io > SIZE_MAX / 2u)
                return false;
            new_cap = *capacity_io * 2u;
        }
        if (new_cap > SIZE_MAX / sizeof(**entries_io))
            return false;
        vg_filedialog_platform_entry_t *grown =
            (vg_filedialog_platform_entry_t *)realloc(*entries_io, new_cap * sizeof(**entries_io));
        if (!grown)
            return false;
        *entries_io = grown;
        *capacity_io = new_cap;
    }
    (*entries_io)[(*count_io)++] = entry;
    return true;
}

static INIT_ONCE g_vg_filedialog_root_once = INIT_ONCE_STATIC_INIT;
static char g_vg_filedialog_root[] = "C:\\";

/// @brief Initializes the process-wide fallback root from the Windows directory.
///
/// @param once Win32 one-time initialization token; otherwise unused.
/// @param parameter Caller parameter; unused.
/// @param context Optional context receiver; unused.
/// @return `TRUE` so `InitOnceExecuteOnce()` records successful initialization.
static BOOL CALLBACK vg_filedialog_platform_initialize_root(PINIT_ONCE once,
                                                            PVOID parameter,
                                                            PVOID *context) {
    wchar_t windows_path[MAX_PATH];
    (void)once;
    (void)parameter;
    (void)context;
    DWORD length =
        GetWindowsDirectoryW(windows_path, (UINT)(sizeof(windows_path) / sizeof(wchar_t)));
    if (length >= 2 && length < (DWORD)(sizeof(windows_path) / sizeof(wchar_t)) &&
        windows_path[1] == L':' &&
        ((windows_path[0] >= L'A' && windows_path[0] <= L'Z') ||
         (windows_path[0] >= L'a' && windows_path[0] <= L'z'))) {
        wchar_t drive = windows_path[0];
        if (drive >= L'a' && drive <= L'z')
            drive -= L'a' - L'A';
        g_vg_filedialog_root[0] = (char)drive;
    }
    return TRUE;
}

/// @brief Return the Windows system-drive root used as the dialog fallback.
/// @return Process-lifetime static root string; caller must not free it.
const char *vg_filedialog_platform_root_path(void) {
    (void)InitOnceExecuteOnce(
        &g_vg_filedialog_root_once, vg_filedialog_platform_initialize_root, NULL, NULL);
    return g_vg_filedialog_root;
}

/// @brief Test whether a character is a Windows path separator.
/// @param ch Character to classify.
/// @return true when @p ch is '/' or '\\'.
bool vg_filedialog_platform_is_separator(char ch) {
    return ch == '/' || ch == '\\';
}

/// @brief Join two Windows path components with a single backslash when needed.
/// @param dir Directory component.
/// @param file Child name component.
/// @return Newly allocated joined path, or NULL on invalid input/allocation failure.
char *vg_filedialog_platform_join_path(const char *dir, const char *file) {
    if (!dir || !file)
        return NULL;
    size_t dir_len = strlen(dir);
    size_t file_len = strlen(file);
    size_t sep_len =
        dir_len > 0u && !vg_filedialog_platform_is_separator(dir[dir_len - 1]) ? 1u : 0u;
    if (dir_len > SIZE_MAX - sep_len || dir_len + sep_len > SIZE_MAX - file_len ||
        dir_len + sep_len + file_len > SIZE_MAX - 1u) {
        return NULL;
    }

    size_t total = dir_len + sep_len + file_len;
    char *result = (char *)malloc(total + 1u);
    if (!result)
        return NULL;
    memcpy(result, dir, dir_len);
    size_t offset = dir_len;
    if (sep_len)
        result[offset++] = '\\';
    memcpy(result + offset, file, file_len);
    result[total] = '\0';
    return result;
}

/// @brief Resolve the current user's Windows home directory.
/// @details Uses USERPROFILE first, then HOMEDRIVE + HOMEPATH, and finally
///          falls back to "C:\\".
/// @return Newly allocated path string, or NULL on allocation failure.
char *vg_filedialog_platform_home_dir(void) {
    wchar_t *userprofile_w = vg_filedialog_platform_environment_wide(L"USERPROFILE");
    if (userprofile_w) {
        DWORD attributes = GetFileAttributesW(userprofile_w);
        char *userprofile =
            attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0
                ? vg_filedialog_platform_wide_to_utf8(userprofile_w)
                : NULL;
        free(userprofile_w);
        if (userprofile)
            return userprofile;
    }

    wchar_t *homedrive_w = vg_filedialog_platform_environment_wide(L"HOMEDRIVE");
    wchar_t *homepath_w = vg_filedialog_platform_environment_wide(L"HOMEPATH");
    char *homedrive = vg_filedialog_platform_wide_to_utf8(homedrive_w);
    char *homepath = vg_filedialog_platform_wide_to_utf8(homepath_w);
    char *result = NULL;
    free(homedrive_w);
    free(homepath_w);
    if (homedrive && homepath) {
        size_t drive_len = strlen(homedrive);
        size_t path_len = strlen(homepath);
        if (drive_len <= SIZE_MAX - path_len && drive_len + path_len <= SIZE_MAX - 1u) {
            result = (char *)malloc(drive_len + path_len + 1u);
            if (result) {
                memcpy(result, homedrive, drive_len);
                memcpy(result + drive_len, homepath, path_len + 1u);
                if (!vg_filedialog_platform_is_absolute_path(result) ||
                    !vg_filedialog_platform_path_is_dir(result)) {
                    free(result);
                    result = NULL;
                }
            }
        }
    }
    free(homedrive);
    free(homepath);
    if (result)
        return result;

    return vg_filedialog_platform_strdup(vg_filedialog_platform_root_path());
}

/// @brief Return the non-trimmable prefix length of a rooted Windows path.
/// @details Handles drive roots, ordinary UNC share roots, extended drive
///          roots, and extended UNC share roots. A zero result means the input
///          has no recognized absolute root.
/// @param path NUL-terminated path whose absolute-root prefix is measured.
/// @return Number of bytes that form the indivisible root, or zero for a null,
///         relative, or unrecognized path.
static size_t vg_filedialog_platform_root_length(const char *path) {
    size_t length;
    size_t cursor;
    int extended_unc = 0;

    if (!path)
        return 0u;
    length = strlen(path);
    if (length >= 3u && isalpha((unsigned char)path[0]) && path[1] == ':' &&
        vg_filedialog_platform_is_separator(path[2])) {
        return 3u;
    }
    if (length >= 7u && vg_filedialog_platform_is_separator(path[0]) &&
        vg_filedialog_platform_is_separator(path[1]) && path[2] == '?' &&
        vg_filedialog_platform_is_separator(path[3]) && isalpha((unsigned char)path[4]) &&
        path[5] == ':' && vg_filedialog_platform_is_separator(path[6])) {
        return 7u;
    }
    if (length < 5u || !vg_filedialog_platform_is_separator(path[0]) ||
        !vg_filedialog_platform_is_separator(path[1])) {
        return 0u;
    }
    cursor = 2u;
    if (length >= 8u && path[2] == '?' && vg_filedialog_platform_is_separator(path[3]) &&
        (path[4] == 'U' || path[4] == 'u') && (path[5] == 'N' || path[5] == 'n') &&
        (path[6] == 'C' || path[6] == 'c') && vg_filedialog_platform_is_separator(path[7])) {
        cursor = 8u;
        extended_unc = 1;
    }
    while (cursor < length && !vg_filedialog_platform_is_separator(path[cursor]))
        cursor++;
    if (cursor == (extended_unc ? 8u : 2u) || cursor >= length)
        return 0u;
    while (cursor < length && vg_filedialog_platform_is_separator(path[cursor]))
        cursor++;
    {
        size_t share_start = cursor;
        while (cursor < length && !vg_filedialog_platform_is_separator(path[cursor]))
            cursor++;
        if (cursor == share_start)
            return 0u;
    }
    return cursor;
}

/// @brief Return a newly allocated parent directory path.
/// @details Trims trailing separators before finding the parent. Drive roots
///          remain rooted; relative single-component paths return ".".
/// @param path Input path.
/// @return Newly allocated parent path, or NULL on allocation failure.
char *vg_filedialog_platform_parent_dir(const char *path) {
    if (!path || !*path)
        return vg_filedialog_platform_strdup("C:\\");

    char *result = vg_filedialog_platform_strdup(path);
    if (!result)
        return NULL;

    size_t len = strlen(result);
    const size_t root_len = vg_filedialog_platform_root_length(result);
    while (len > (root_len ? root_len : 1u) &&
           vg_filedialog_platform_is_separator(result[len - 1u]))
        result[--len] = '\0';

    if (root_len && len <= root_len) {
        result[root_len] = '\0';
        return result;
    }

    char *last_slash = strrchr(result, '\\');
    char *last_forward = strrchr(result, '/');
    if (!last_slash || (last_forward && last_forward > last_slash))
        last_slash = last_forward;

    if (root_len && last_slash && (size_t)(last_slash - result) < root_len) {
        result[root_len] = '\0';
    } else if (last_slash == result) {
        result[1] = '\0';
    } else if (last_slash == result + 2 && result[1] == ':') {
        result[3] = '\0';
    } else if (last_slash) {
        *last_slash = '\0';
    } else {
        free(result);
        return vg_filedialog_platform_strdup(".");
    }

    return result;
}

/// @brief Test whether a path is absolute on Windows.
/// @param path Path string to inspect.
/// @return true for drive-rooted and slash-rooted paths.
bool vg_filedialog_platform_is_absolute_path(const char *path) {
    if (!path || !*path)
        return false;
    if (strlen(path) >= 3u && isalpha((unsigned char)path[0]) && path[1] == ':' &&
        vg_filedialog_platform_is_separator(path[2]))
        return true;
    return vg_filedialog_platform_is_separator(path[0]) &&
           vg_filedialog_platform_is_separator(path[1]);
}

/// @brief Test whether a path exists and is a directory.
/// @param path Path string to stat.
/// @return true when GetFileAttributesW succeeds and reports a directory.
bool vg_filedialog_platform_path_is_dir(const char *path) {
    if (!path)
        return false;
    wchar_t *wide = vg_filedialog_platform_utf8_to_wide(path);
    if (!wide)
        return false;
    DWORD attrs = GetFileAttributesW(wide);
    free(wide);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

/// @brief Enumerate a Windows directory into malloc-owned entry records.
/// @details Uses FindFirstFileW/FindNextFileW, skips "." and "..", and returns
///          entry strings owned by the caller.
/// @param path Directory to enumerate.
/// @param entries_out Receives the allocated entry array.
/// @param count_out Receives the number of entries.
/// @return true when the directory was opened and scanned, false otherwise.
bool vg_filedialog_platform_list_directory(const char *path,
                                           vg_filedialog_platform_entry_t **entries_out,
                                           size_t *count_out) {
    if (entries_out)
        *entries_out = NULL;
    if (count_out)
        *count_out = 0u;
    if (!path || !entries_out || !count_out)
        return false;

    char *search_path = vg_filedialog_platform_join_path(path, "*");
    if (!search_path)
        return false;

    wchar_t *search_path_w = vg_filedialog_platform_utf8_to_wide(search_path);
    free(search_path);
    if (!search_path_w)
        return false;

    WIN32_FIND_DATAW find_data;
    HANDLE find = FindFirstFileW(search_path_w, &find_data);
    free(search_path_w);
    if (find == INVALID_HANDLE_VALUE)
        return false;

    vg_filedialog_platform_entry_t *entries = NULL;
    size_t count = 0u;
    size_t capacity = 0u;

    do {
        if (wcscmp(find_data.cFileName, L".") == 0 || wcscmp(find_data.cFileName, L"..") == 0)
            continue;

        char *name = vg_filedialog_platform_wide_to_utf8(find_data.cFileName);
        if (!name)
            goto fail;
        char *full_path = vg_filedialog_platform_join_path(path, name);
        if (!full_path) {
            free(name);
            goto fail;
        }

        vg_filedialog_platform_entry_t out;
        memset(&out, 0, sizeof(out));
        out.name = name;
        out.full_path = full_path;
        out.is_directory = (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        out.is_hidden = (find_data.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0;
        out.size = ((uint64_t)find_data.nFileSizeHigh << 32) | (uint64_t)find_data.nFileSizeLow;

        ULARGE_INTEGER ull;
        ull.LowPart = find_data.ftLastWriteTime.dwLowDateTime;
        ull.HighPart = find_data.ftLastWriteTime.dwHighDateTime;
        out.modified_time = ull.QuadPart >= 116444736000000000ULL
                                ? (int64_t)((ull.QuadPart - 116444736000000000ULL) / 10000000ULL)
                                : 0;

        if (!out.name || !out.full_path ||
            !vg_filedialog_platform_append_entry(&entries, &count, &capacity, out)) {
            free(out.name);
            free(out.full_path);
            goto fail;
        }
    } while (FindNextFileW(find, &find_data));

    if (GetLastError() != ERROR_NO_MORE_FILES)
        goto fail;

    FindClose(find);
    *entries_out = entries;
    *count_out = count;
    return true;

fail:
    FindClose(find);
    vg_filedialog_platform_free_entries(entries, count);
    return false;
}

/// @brief Free an entry array returned by vg_filedialog_platform_list_directory().
/// @param entries Entry array to release; NULL is accepted.
/// @param count Number of entries in @p entries.
void vg_filedialog_platform_free_entries(vg_filedialog_platform_entry_t *entries, size_t count) {
    if (!entries)
        return;
    for (size_t i = 0; i < count; i++) {
        free(entries[i].name);
        free(entries[i].full_path);
    }
    free(entries);
}
