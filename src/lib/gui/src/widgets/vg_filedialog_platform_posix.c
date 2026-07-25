//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file vg_filedialog_platform_posix.c
/// @brief Implements the POSIX filesystem adapter for the file dialog.
///
/// @details Directory enumeration uses `opendir()`, `readdir()`, and `lstat()`
/// without retaining directory handles after return. Hidden entries are
/// identified by a leading dot. All returned paths, names, and entry arrays use
/// malloc-compatible storage and transfer ownership to the caller.
///
/// @see vg_filedialog_platform.h
/// @see vg_filedialog.c
//
//===----------------------------------------------------------------------===//

#include "vg_filedialog_platform.h"

#include <dirent.h>
#include <errno.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/// @brief Duplicate a C string using malloc-owned storage.
/// @details Avoids relying on non-standard strdup availability in strict C
///          modes. NULL input returns NULL.
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
        size_t new_cap = *capacity_io == 0u ? 64u : *capacity_io * 2u;
        if (*capacity_io > SIZE_MAX / (2u * sizeof(**entries_io)))
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

/// @brief Return the POSIX filesystem root used as the file dialog fallback.
/// @return Static "/" string; caller must not free it.
const char *vg_filedialog_platform_root_path(void) {
    return "/";
}

/// @brief Test whether a character is the POSIX path separator.
/// @param ch Character to classify.
/// @return true when @p ch is '/'.
bool vg_filedialog_platform_is_separator(char ch) {
    return ch == '/';
}

/// @brief Join two POSIX path components with a single separator when needed.
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
        result[offset++] = '/';
    memcpy(result + offset, file, file_len);
    result[total] = '\0';
    return result;
}

/// @brief Resolve the current user's home directory.
/// @details Uses HOME first, then getpwuid(getuid()), and finally falls back
///          to the filesystem root.
/// @return Newly allocated path string, or NULL on allocation failure.
char *vg_filedialog_platform_home_dir(void) {
    const char *home = getenv("HOME");
    if (home)
        return vg_filedialog_platform_strdup(home);

    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_dir)
        return vg_filedialog_platform_strdup(pw->pw_dir);

    return vg_filedialog_platform_strdup("/");
}

/// @brief Return a newly allocated parent directory path.
/// @details Trims trailing separators before finding the parent. Root remains
///          root; relative single-component paths return ".".
/// @param path Input path.
/// @return Newly allocated parent path, or NULL on allocation failure.
char *vg_filedialog_platform_parent_dir(const char *path) {
    if (!path || !*path)
        return vg_filedialog_platform_strdup("/");

    char *result = vg_filedialog_platform_strdup(path);
    if (!result)
        return NULL;

    size_t len = strlen(result);
    while (len > 1u && result[len - 1u] == '/')
        result[--len] = '\0';

    char *last_slash = strrchr(result, '/');
    if (last_slash == result) {
        result[1] = '\0';
    } else if (last_slash) {
        *last_slash = '\0';
    } else {
        free(result);
        return vg_filedialog_platform_strdup(".");
    }

    return result;
}

/// @brief Test whether a path is absolute on POSIX.
/// @param path Path string to inspect.
/// @return true when @p path begins with '/'.
bool vg_filedialog_platform_is_absolute_path(const char *path) {
    return path && path[0] == '/';
}

/// @brief Test whether a path exists and is a directory.
/// @param path Path string to stat.
/// @return true when stat() succeeds and reports a directory.
bool vg_filedialog_platform_path_is_dir(const char *path) {
    if (!path)
        return false;
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/// @brief Enumerate a POSIX directory into malloc-owned entry records.
/// @details Skips "." and ".."; each returned name/full_path string and the
///          returned array must be released with vg_filedialog_platform_free_entries().
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

    DIR *dir = opendir(path);
    if (!dir)
        return false;

    vg_filedialog_platform_entry_t *entries = NULL;
    size_t count = 0u;
    size_t capacity = 0u;

    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        char *full_path = vg_filedialog_platform_join_path(path, entry->d_name);
        if (!full_path)
            goto fail;

        struct stat st;
        if (lstat(full_path, &st) != 0) {
            int saved_errno = errno;
            free(full_path);
            if (saved_errno == ENOENT)
                continue;
            goto fail;
        }

        vg_filedialog_platform_entry_t out;
        memset(&out, 0, sizeof(out));
        out.name = vg_filedialog_platform_strdup(entry->d_name);
        out.full_path = full_path;
        out.is_directory = S_ISDIR(st.st_mode);
        out.is_hidden = entry->d_name[0] == '.';
        out.size = S_ISREG(st.st_mode) && st.st_size > 0 ? (uint64_t)st.st_size : 0u;
        out.modified_time = (int64_t)st.st_mtime;
        if (!out.name || !vg_filedialog_platform_append_entry(&entries, &count, &capacity, out)) {
            free(out.name);
            free(out.full_path);
            goto fail;
        }
    }

    closedir(dir);
    *entries_out = entries;
    *count_out = count;
    return true;

fail:
    closedir(dir);
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
