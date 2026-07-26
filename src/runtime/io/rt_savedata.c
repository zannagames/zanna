//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/io/rt_savedata.c
// Purpose: Cross-platform game save/load persistence. Key-value pairs stored
//   as JSON in platform-appropriate directories:
//     macOS:   ~/Library/Application Support/Zanna/<game>/save.json
//     Linux:   ~/.local/share/zanna/<game>/save.json
//     Windows: %APPDATA%\Zanna\<game>\save.json
//
// Key invariants:
//   - Keys are unique; last-write wins.
//   - Save writes atomically through an exclusive temp file and replace.
//   - Load replaces all in-memory data with file contents; missing file is empty.
//   - Keys and string values must be well-formed UTF-8; keys are nonempty and NUL-free.
//   - JSON persistence accepts only signed 64-bit integer and string values.
//
// Ownership/Lifetime:
//   - SaveData is GC-managed; finalizer frees all entries and path strings.
//   - Entries retain runtime-string references for keys and string values; removal/finalization
//     releases them. Game-name and file-path C strings are separately malloc-owned.
//
// Links: src/runtime/io/rt_savedata.h (public API),
//        src/runtime/io/rt_dir.h (directory creation),
//        src/runtime/core/rt_string_builder.h (JSON output),
//        src/runtime/text/rt_json_stream.h (JSON token input)
//
//===----------------------------------------------------------------------===//
/**
 * @file
 * @brief Implements typed JSON save-data storage and durable persistence.
 * @details Validates identifiers and UTF-8, retains typed key/value entries,
 * serializes exact signed integers and escaped strings, parses replacement
 * state transactionally, computes platform data locations, and commits files
 * through synchronized adjacent sidecars with rollback-safe cleanup.
 */

#include "rt_savedata.h"
#include "../rt_platform.h"
#include "network/rt_entropy_platform.h"
#include "rt_dir.h"
#include "rt_file_path.h"
#include "rt_io_class_ids.h"
#include "rt_object.h"
#include "rt_string.h"
#include "rt_string_builder.h"

#if RT_PLATFORM_WINDOWS
#ifndef _CRT_RAND_S
#define _CRT_RAND_S
#endif
#endif

#include <errno.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if RT_PLATFORM_WINDOWS
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "rt_trap.h"

/// @copydoc rt_trap_set_recovery()
void rt_trap_set_recovery(jmp_buf *buf);
/// @copydoc rt_trap_clear_recovery()
void rt_trap_clear_recovery(void);
/// @copydoc rt_trap_get_error()
const char *rt_trap_get_error(void);

/* JSON stream parser (from rt_json_stream.h / text module) */
/// @copydoc rt_json_stream_new()
extern void *rt_json_stream_new(rt_string json);
/// @copydoc rt_json_stream_next()
extern int64_t rt_json_stream_next(void *parser);
/// @copydoc rt_json_stream_string_value()
extern rt_string rt_json_stream_string_value(void *parser);
/// @copydoc rt_json_stream_number_text()
extern rt_string rt_json_stream_number_text(void *parser);

/* Token types */
/** @name JSON token identifiers consumed by the SaveData subset parser
 * @{ */
#define TOK_OBJECT_START 1
#define TOK_OBJECT_END 2
#define TOK_KEY 5
#define TOK_STRING 6
#define TOK_NUMBER 7
#define TOK_END 11
/** @} */

//=========================================================================
// Internal Data Structures
//=========================================================================

/// @brief Tag distinguishing the stored value variant of a SaveEntry.
typedef enum { SAVE_INT = 0, SAVE_STR = 1 } SaveEntryType;

/// @brief Singly-linked node holding one key-value pair in the save store.
typedef struct SaveEntry {
    rt_string key;          ///< Interned key string (heap-allocated, owned by this entry).
    int64_t key_len;        ///< Byte length of `key`, cached to avoid repeated strlen calls.
    SaveEntryType type;     ///< Discriminant selecting between `int_val` and `str_val`.
    int64_t int_val;        ///< Integer payload; valid when `type == SAVE_INT`.
    rt_string str_val;      ///< String payload; valid when `type == SAVE_STR` (heap-allocated).
    struct SaveEntry *next; ///< Next entry in the singly-linked bucket chain, or NULL.
} SaveEntry;

/// @brief Internal save-data store: game identity, on-disk path, and entry list.
typedef struct {
    char *game_name;    ///< Validated game name (alphanumeric, dash, underscore, ≤64 chars).
    char *file_path;    ///< Absolute path to the save JSON file.
    SaveEntry *entries; ///< Head of the singly-linked entry list.
} rt_savedata_impl;

/// @brief Preserve the active trap diagnostic for cleanup and rethrow.
/// @param[out] buffer Destination for the copied message.
/// @param buffer_size Capacity of @p buffer in bytes.
/// @param fallback Message used when the trap subsystem has no nonempty text.
static void savedata_save_trap_error(char *buffer, size_t buffer_size, const char *fallback) {
    const char *err = rt_trap_get_error();
    snprintf(buffer, buffer_size, "%s", err && err[0] ? err : fallback);
}

/// @brief Transactionally retain a key/value string pair.
/// @details A local recovery boundary releases the first reference if retaining the second traps,
///          then rethrows the preserved diagnostic so callers never receive partial ownership.
/// @param key Borrowed key string to retain.
/// @param value Borrowed value string to retain.
/// @param[out] out_key Receives the retained key reference.
/// @param[out] out_value Receives the retained value reference.
static void savedata_retain_pair_or_trap(rt_string key,
                                         rt_string value,
                                         rt_string *out_key,
                                         rt_string *out_value) {
    volatile int key_retained = 0;
    volatile int value_retained = 0;

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        savedata_save_trap_error(
            saved_error, sizeof(saved_error), "SaveData: string retain failed");
        rt_trap_clear_recovery();
        if (value_retained)
            rt_string_unref(value);
        if (key_retained)
            rt_string_unref(key);
        rt_trap(saved_error);
        return;
    }

    *out_key = rt_string_ref(key);
    key_retained = 1;
    *out_value = rt_string_ref(value);
    value_retained = 1;
    rt_trap_clear_recovery();
}

/// @brief Validate and unwrap an opaque SaveData receiver.
/// @param obj Borrowed runtime receiver.
/// @param context Trap diagnostic for an invalid class identifier; may be NULL.
/// @return SaveData implementation payload; invalid receivers trap.
static rt_savedata_impl *savedata_require(void *obj, const char *context) {
    if (!obj || rt_obj_class_id(obj) != RT_SAVEDATA_CLASS_ID)
        rt_trap(context ? context : "SaveData: invalid handle");
    return (rt_savedata_impl *)obj;
}

/// @brief Validate a byte span as shortest-form Unicode UTF-8.
/// @details Rejects truncated sequences, invalid continuations, overlong encodings, UTF-16
///          surrogate code points, and scalar values above U+10FFFF.
/// @param data Byte span to validate.
/// @param len Number of bytes in @p data.
/// @return 1 when the complete span is well-formed UTF-8; otherwise 0.
static int savedata_is_valid_utf8(const char *data, size_t len) {
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)data[i];
        if (c < 0x80) {
            i++;
        } else if (c >= 0xC2 && c <= 0xDF) {
            if (i + 1 >= len)
                return 0;
            unsigned char c1 = (unsigned char)data[i + 1];
            if (c1 < 0x80 || c1 > 0xBF)
                return 0;
            i += 2;
        } else if (c == 0xE0) {
            if (i + 2 >= len)
                return 0;
            unsigned char c1 = (unsigned char)data[i + 1];
            unsigned char c2 = (unsigned char)data[i + 2];
            if (c1 < 0xA0 || c1 > 0xBF || c2 < 0x80 || c2 > 0xBF)
                return 0;
            i += 3;
        } else if ((c >= 0xE1 && c <= 0xEC) || (c >= 0xEE && c <= 0xEF)) {
            if (i + 2 >= len)
                return 0;
            unsigned char c1 = (unsigned char)data[i + 1];
            unsigned char c2 = (unsigned char)data[i + 2];
            if (c1 < 0x80 || c1 > 0xBF || c2 < 0x80 || c2 > 0xBF)
                return 0;
            i += 3;
        } else if (c == 0xED) {
            if (i + 2 >= len)
                return 0;
            unsigned char c1 = (unsigned char)data[i + 1];
            unsigned char c2 = (unsigned char)data[i + 2];
            if (c1 < 0x80 || c1 > 0x9F || c2 < 0x80 || c2 > 0xBF)
                return 0;
            i += 3;
        } else if (c == 0xF0) {
            if (i + 3 >= len)
                return 0;
            unsigned char c1 = (unsigned char)data[i + 1];
            unsigned char c2 = (unsigned char)data[i + 2];
            unsigned char c3 = (unsigned char)data[i + 3];
            if (c1 < 0x90 || c1 > 0xBF || c2 < 0x80 || c2 > 0xBF || c3 < 0x80 || c3 > 0xBF)
                return 0;
            i += 4;
        } else if (c >= 0xF1 && c <= 0xF3) {
            if (i + 3 >= len)
                return 0;
            unsigned char c1 = (unsigned char)data[i + 1];
            unsigned char c2 = (unsigned char)data[i + 2];
            unsigned char c3 = (unsigned char)data[i + 3];
            if (c1 < 0x80 || c1 > 0xBF || c2 < 0x80 || c2 > 0xBF || c3 < 0x80 || c3 > 0xBF)
                return 0;
            i += 4;
        } else if (c == 0xF4) {
            if (i + 3 >= len)
                return 0;
            unsigned char c1 = (unsigned char)data[i + 1];
            unsigned char c2 = (unsigned char)data[i + 2];
            unsigned char c3 = (unsigned char)data[i + 3];
            if (c1 < 0x80 || c1 > 0x8F || c2 < 0x80 || c2 > 0xBF || c3 < 0x80 || c3 > 0xBF)
                return 0;
            i += 4;
        } else {
            return 0;
        }
    }
    return 1;
}

/// @brief Require a nonempty UTF-8 key without embedded NUL bytes.
/// @param key Borrowed runtime string candidate.
/// @param[out] len_out Optional destination for the validated byte length.
/// @param context Trap diagnostic for invalid input.
/// @return Borrowed key byte pointer on success; invalid input traps.
static const char *savedata_require_key(rt_string key, size_t *len_out, const char *context) {
    const char *kcstr = rt_string_cstr(key);
    int64_t klen_i64 = key ? rt_str_len(key) : -1;
    if (!kcstr || klen_i64 <= 0) {
        rt_trap(context);
        if (len_out)
            *len_out = 0;
        return "";
    }
    size_t klen = (size_t)klen_i64;
    if (memchr(kcstr, '\0', klen) || !savedata_is_valid_utf8(kcstr, klen)) {
        rt_trap(context);
        if (len_out)
            *len_out = 0;
        return "";
    }
    if (len_out)
        *len_out = klen;
    return kcstr;
}

/// @brief Non-trapping validation for a SaveData key.
/// @param key Borrowed runtime string candidate.
/// @param[out] len_out Optional destination for the validated byte length.
/// @return 1 for a nonempty well-formed UTF-8 key without embedded NUL; otherwise 0.
static int savedata_is_valid_key(rt_string key, size_t *len_out) {
    const char *kcstr = rt_string_cstr(key);
    int64_t klen_i64 = key ? rt_str_len(key) : -1;
    if (!kcstr || klen_i64 <= 0)
        return 0;
    size_t klen = (size_t)klen_i64;
    if (memchr(kcstr, '\0', klen) || !savedata_is_valid_utf8(kcstr, klen))
        return 0;
    if (len_out)
        *len_out = klen;
    return 1;
}

/// @brief Require a runtime string containing well-formed UTF-8.
/// @param value Borrowed runtime string candidate; empty strings are valid.
/// @param context Trap diagnostic for invalid input.
static void savedata_require_string_value(rt_string value, const char *context) {
    const char *data = rt_string_cstr(value);
    int64_t len_i64 = value ? rt_str_len(value) : -1;
    if (!data || len_i64 < 0) {
        rt_trap(context);
        return;
    }
    if (!savedata_is_valid_utf8(data, (size_t)len_i64)) {
        rt_trap(context);
        return;
    }
}

/// @brief Non-trapping validation for a string-valued save entry.
/// @param value Borrowed runtime string candidate.
/// @return 1 for a valid runtime string containing well-formed UTF-8; otherwise 0.
static int savedata_is_valid_string_value(rt_string value) {
    const char *data = rt_string_cstr(value);
    int64_t len_i64 = value ? rt_str_len(value) : -1;
    return data && len_i64 >= 0 && savedata_is_valid_utf8(data, (size_t)len_i64);
}

//=========================================================================
// Internal Helpers
//=========================================================================

/// @brief Linear-search the entry list for a key by raw bytes + length.
///
/// Uses `memcmp` on the stored key bytes and cached validated length rather than
/// repeating NUL-terminated scans. The list is
/// singly-linked in insertion order; lookups are O(n). Save files are
/// typically small (a handful to low hundreds of keys), so a hash
/// table isn't worth the complexity and memory overhead.
/// @param sd SaveData store to search.
/// @param key Validated key bytes.
/// @param key_len Number of bytes in @p key.
/// @return Matching entry node, or NULL when absent/invalid.
static SaveEntry *find_entry(rt_savedata_impl *sd, const char *key, size_t key_len) {
    if (!sd || !key)
        return NULL;

    SaveEntry *e = sd->entries;
    while (e) {
        if (e->key && e->key_len == (int64_t)key_len &&
            memcmp(rt_string_cstr(e->key), key, key_len) == 0)
            return e;
        e = e->next;
    }
    return NULL;
}

/// @brief Release all resources owned by a single entry node and free it.
/// @param e Owned entry node to destroy; NULL is ignored.
static void free_entry(SaveEntry *e) {
    if (e) {
        if (e->key)
            rt_string_unref(e->key);
        if (e->str_val)
            rt_string_unref(e->str_val);
        free(e);
    }
}

/// @brief Convert a JSON number token to `int64_t` exactly.
///
/// SaveData persists Integer values as decimal JSON numbers. Parsing through
/// double loses precision past 2^53, so load validates and converts directly
/// from the original token text. Fractions, exponents, and out-of-range values
/// are rejected rather than rounded.
/// @param text Borrowed runtime string containing the raw JSON number token.
/// @param[out] out Receives the exact signed value on success.
/// @return 1 for a canonical in-range JSON integer; otherwise 0.
static int savedata_number_text_to_i64(rt_string text, int64_t *out) {
    if (!text || !out)
        return 0;

    const char *data = rt_string_cstr(text);
    int64_t raw_len = rt_str_len(text);
    if (!data || raw_len <= 0)
        return 0;

    size_t len = (size_t)raw_len;
    size_t i = 0;
    int negative = 0;
    if (data[i] == '-') {
        negative = 1;
        i++;
        if (i == len)
            return 0;
    }

    uint64_t limit = negative ? UINT64_C(9223372036854775808) : UINT64_C(9223372036854775807);
    uint64_t acc = 0;

    if (data[i] == '0') {
        i++;
    } else if (data[i] >= '1' && data[i] <= '9') {
        while (i < len && data[i] >= '0' && data[i] <= '9') {
            uint64_t digit = (uint64_t)(data[i] - '0');
            if (acc > (limit - digit) / UINT64_C(10))
                return 0;
            acc = acc * UINT64_C(10) + digit;
            i++;
        }
    } else {
        return 0;
    }

    if (i != len)
        return 0;

    if (negative) {
        if (acc == UINT64_C(9223372036854775808)) {
            *out = INT64_MIN;
        } else {
            *out = -(int64_t)acc;
        }
    } else {
        *out = (int64_t)acc;
    }
    return 1;
}

/// @brief Walk the entry list, freeing every node and setting the head pointer to NULL.
/// @param sd SaveData store whose owned entries should be cleared.
static void free_all_entries(rt_savedata_impl *sd) {
    SaveEntry *e = sd->entries;
    while (e) {
        SaveEntry *next = e->next;
        free_entry(e);
        e = next;
    }
    sd->entries = NULL;
}

/// @brief Test whether a native path spelling is absolute.
/// @param path Null-terminated native path.
/// @return 1 for POSIX-rooted, Windows drive-rooted, or Windows UNC paths; otherwise 0.
static int savedata_path_is_abs(const char *path) {
    if (!path || path[0] == '\0')
        return 0;
#if RT_PLATFORM_WINDOWS
    if ((path[0] == '\\' && path[1] == '\\') || (path[0] == '/' && path[1] == '/'))
        return 1;
    return ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
           path[1] == ':' && (path[2] == '\\' || path[2] == '/');
#else
    return path[0] == '/';
#endif
}

/// @brief Copy a path as an absolute native path string.
/// @details Windows delegates normalization to `GetFullPathNameW`; POSIX preserves already
///          absolute spellings or prefixes a dynamically sized current working directory.
/// @param path Nonempty null-terminated path.
/// @return Malloc-owned absolute path, or NULL on conversion, overflow, probe, or allocation
///         failure.
static char *savedata_absolute_dup(const char *path) {
    if (!path || path[0] == '\0')
        return NULL;
#if RT_PLATFORM_WINDOWS
    wchar_t *wide = rt_file_path_utf8_to_wide(path);
    if (!wide)
        return NULL;
    DWORD needed = GetFullPathNameW(wide, 0, NULL, NULL);
    if (needed == 0) {
        free(wide);
        return NULL;
    }
    wchar_t *full = (wchar_t *)malloc(((size_t)needed + 1) * sizeof(wchar_t));
    if (!full) {
        free(wide);
        return NULL;
    }
    DWORD written = GetFullPathNameW(wide, needed + 1, full, NULL);
    free(wide);
    if (written == 0 || written > needed) {
        free(full);
        return NULL;
    }
    rt_string s = rt_file_path_wide_to_string(full);
    free(full);
    if (!s)
        return NULL;
    const char *cs = rt_string_cstr(s);
    int64_t len = rt_str_len(s);
    char *copy = NULL;
    if (cs && len >= 0) {
        copy = (char *)malloc((size_t)len + 1);
        if (copy) {
            memcpy(copy, cs, (size_t)len);
            copy[(size_t)len] = '\0';
        }
    }
    rt_string_unref(s);
    return copy;
#else
    if (savedata_path_is_abs(path))
        return strdup(path);
    size_t cap = 256;
    char *cwd = NULL;
    for (;;) {
        char *next = (char *)realloc(cwd, cap);
        if (!next) {
            free(cwd);
            return NULL;
        }
        cwd = next;
        if (getcwd(cwd, cap))
            break;
        if (errno != ERANGE) {
            free(cwd);
            return NULL;
        }
        if (cap > SIZE_MAX / 2) {
            free(cwd);
            return NULL;
        }
        cap *= 2;
    }
    size_t cwd_len = strlen(cwd);
    size_t path_len = strlen(path);
    int needs_sep = cwd_len > 0 && cwd[cwd_len - 1] != '/';
    if (cwd_len > SIZE_MAX - path_len - (size_t)needs_sep - 1) {
        free(cwd);
        return NULL;
    }
    char *out = (char *)malloc(cwd_len + (size_t)needs_sep + path_len + 1);
    if (!out) {
        free(cwd);
        return NULL;
    }
    memcpy(out, cwd, cwd_len);
    size_t pos = cwd_len;
    if (needs_sep)
        out[pos++] = '/';
    memcpy(out + pos, path, path_len);
    out[pos + path_len] = '\0';
    free(cwd);
    return out;
#endif
}

#if RT_PLATFORM_WINDOWS
/// @brief Snapshot a Windows environment variable and return strict UTF-8.
/// @details `getenv` exposes process-ACP bytes and a borrowed buffer that can be
///          invalidated by concurrent environment changes. This helper retries
///          the native UTF-16 sizing race and always returns owned storage.
/// @param name Null-terminated wide environment-variable name.
/// @return Malloc-owned strict UTF-8 value, or NULL when missing, empty, unstable, invalid, or
///         allocation fails.
static char *savedata_windows_env_utf8(const wchar_t *name) {
    if (!name)
        return NULL;
    DWORD capacity = GetEnvironmentVariableW(name, NULL, 0);
    if (capacity == 0)
        return NULL;

    for (int attempt = 0; attempt < 8; ++attempt) {
        if ((size_t)capacity > SIZE_MAX / sizeof(wchar_t) || capacity > (DWORD)INT_MAX)
            return NULL;
        wchar_t *wide = (wchar_t *)malloc((size_t)capacity * sizeof(wchar_t));
        if (!wide)
            return NULL;
        DWORD length = GetEnvironmentVariableW(name, wide, capacity);
        if (length == 0) {
            free(wide);
            return NULL;
        }
        if (length >= capacity) {
            free(wide);
            if (length == MAXDWORD)
                return NULL;
            capacity = length + (length == capacity ? 1u : 0u);
            continue;
        }

        int required =
            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, NULL, 0, NULL, NULL);
        if (required <= 1) {
            free(wide);
            return NULL;
        }
        char *utf8 = (char *)malloc((size_t)required);
        if (!utf8) {
            free(wide);
            return NULL;
        }
        int written = WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, utf8, required, NULL, NULL);
        free(wide);
        if (written != required) {
            free(utf8);
            return NULL;
        }
        return utf8;
    }
    return NULL;
}
#endif

#if RT_PLATFORM_WINDOWS
/// @brief Concatenate four optional path fragments with checked size arithmetic.
/// @param a First fragment; NULL contributes an empty string.
/// @param b Second fragment; NULL contributes an empty string.
/// @param c Third fragment; NULL contributes an empty string.
/// @param d Fourth fragment; NULL contributes an empty string.
/// @return Malloc-owned concatenation, or NULL on size overflow/allocation failure.
static char *savedata_concat4(const char *a, const char *b, const char *c, const char *d) {
    const char *parts[4] = {a ? a : "", b ? b : "", c ? c : "", d ? d : ""};
    size_t lengths[4] = {0};
    size_t total = 0;
    for (size_t i = 0; i < 4; ++i) {
        lengths[i] = strlen(parts[i]);
        if (lengths[i] > SIZE_MAX - total - 1u)
            return NULL;
        total += lengths[i];
    }
    char *result = (char *)malloc(total + 1u);
    if (!result)
        return NULL;
    size_t offset = 0;
    for (size_t i = 0; i < 4; ++i) {
        memcpy(result + offset, parts[i], lengths[i]);
        offset += lengths[i];
    }
    result[offset] = '\0';
    return result;
}
#endif

/// @brief Resolve the user's home directory as a fresh heap-allocated C string.
///
/// Windows tries `USERPROFILE` first, falls back to concatenating
/// `HOMEDRIVE`+`HOMEPATH`, then to `"."`. POSIX tries `HOME`, then
/// falls back to `getpwuid(getuid())`, then `"."`. Always returns
/// an owned string so the caller can `free` unconditionally.
/// @return Malloc-owned absolute home/fallback directory, or NULL when even fallback resolution
///         fails.
static char *get_home_dir(void) {
#if RT_PLATFORM_WINDOWS
    char *home = savedata_windows_env_utf8(L"USERPROFILE");
    if (home) {
        char *absolute = savedata_absolute_dup(home);
        free(home);
        if (absolute)
            return absolute;
    }
    char *drive = savedata_windows_env_utf8(L"HOMEDRIVE");
    char *path = savedata_windows_env_utf8(L"HOMEPATH");
    if (drive && path) {
        char *combined = savedata_concat4(drive, path, NULL, NULL);
        char *absolute = combined ? savedata_absolute_dup(combined) : NULL;
        free(combined);
        free(drive);
        free(path);
        if (absolute)
            return absolute;
    } else {
        free(drive);
        free(path);
    }
    return savedata_absolute_dup(".");
#else
    const char *home = getenv("HOME");
    if (home && *home)
        return savedata_absolute_dup(home);
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_dir && pw->pw_dir[0] != '\0')
        return savedata_absolute_dup(pw->pw_dir);
    return savedata_absolute_dup(".");
#endif
}

/// @brief Duplicate the parent-directory portion of `file_path` as a C string.
/// @details Scans backwards for the last path separator and returns a heap-allocated
///          copy of everything before it.  Returns NULL if the path has no separator
///          (i.e. a bare filename with no directory component).
/// @param file_path Null-terminated file path to split.
/// @return Malloc-owned parent path, or NULL for invalid/bare paths or allocation failure.
static char *savedata_parent_dir_dup(const char *file_path) {
    if (!file_path)
        return NULL;
    const char *last_sep = NULL;
    for (const char *p = file_path; *p; ++p) {
        if (*p == '/' || *p == '\\')
            last_sep = p;
    }
    if (!last_sep)
        return NULL;
    size_t len = (size_t)(last_sep - file_path);
#if RT_PLATFORM_WINDOWS
    if (len == 2 && file_path[1] == ':')
        len = 3;
    else if (len == 0)
        len = 1;
#else
    if (len == 0)
        len = 1;
#endif
    char *dir = (char *)malloc(len + 1);
    if (!dir)
        return NULL;
    memcpy(dir, file_path, len);
    dir[len] = '\0';
    return dir;
}

#if RT_PLATFORM_WINDOWS
/// @brief Open a file at a UTF-8 path using a non-inheritable CRT fd.
/// @details Only binary read mode is accepted.
/// @param path Null-terminated UTF-8 file path.
/// @param mode Required `"rb"` stdio mode.
/// @return Caller-owned FILE stream, or NULL on invalid mode, conversion, open, or wrapping
///         failure.
static FILE *savedata_fopen_utf8(const char *path, const char *mode) {
    wchar_t *wide_path = rt_file_path_utf8_to_wide(path);
    if (!wide_path)
        return NULL;
    int flags = _O_NOINHERIT | _O_BINARY;
    if (strcmp(mode, "rb") == 0)
        flags |= _O_RDONLY;
    else {
        free(wide_path);
        errno = EINVAL;
        return NULL;
    }
    int fd = _wopen(wide_path, flags);
    free(wide_path);
    if (fd < 0)
        return NULL;
    FILE *fp = _fdopen(fd, mode);
    if (!fp)
        _close(fd);
    return fp;
}

/// @brief Delete a file at a UTF-8 path via `_wremove` (Windows).
/// @param path Null-terminated UTF-8 path to remove.
/// @return Zero on success; otherwise nonzero.
static int savedata_remove_utf8(const char *path) {
    wchar_t *wide_path = rt_file_path_utf8_to_wide(path);
    if (!wide_path)
        return -1;
    int rc = _wremove(wide_path);
    free(wide_path);
    return rc;
}

/// @brief Atomically replace `dst` with `src` using `MoveFileExW` (Windows).
/// @param src UTF-8 staged source path.
/// @param dst UTF-8 destination path to replace.
/// @return 1 on success; otherwise 0.
static int savedata_replace_utf8(const char *src, const char *dst) {
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

/// @brief Flush and sync an open file to stable storage using `_commit` (Windows).
/// @param fp Open stdio stream whose descriptor should be committed.
/// @return 1 on success; otherwise 0.
static int savedata_sync_file(FILE *fp) {
    return _commit(_fileno(fp)) == 0 ? 1 : 0;
}

/// @brief Fsync the directory containing `path` so renames are durable.
///
/// On POSIX, a successful `rename()` plus `fsync()` on the file is
/// *not* enough — the directory's own dirent must also be fsync'd so
/// a crash after the rename can't leave the old name pointing at the
/// new inode. Windows' `MoveFileExW(... | MOVEFILE_WRITE_THROUGH)`
/// already synchronizes, so this opens the parent with
/// `FILE_FLAG_BACKUP_SEMANTICS` just to call `FlushFileBuffers`; some
/// filesystems (e.g. exFAT) return INVALID_HANDLE — treated as
/// success to avoid failing on portable drives.
/// @param path Committed file path whose parent directory should be flushed.
/// @return 1 on success or an explicitly tolerated unsupported/access result; otherwise 0.
static int savedata_sync_parent_dir(const char *path) {
    char *parent = savedata_parent_dir_dup(path);
    if (!parent)
        return 1;

    wchar_t *wide_parent = rt_file_path_utf8_to_wide(parent);
    free(parent);
    if (!wide_parent)
        return 0;

    HANDLE h = CreateFileW(wide_parent,
                           FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL,
                           OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS,
                           NULL);
    free(wide_parent);
    if (h == INVALID_HANDLE_VALUE)
        return 0;

    if (FlushFileBuffers(h)) {
        CloseHandle(h);
        return 1;
    }

    DWORD err = GetLastError();
    CloseHandle(h);
    return (err == ERROR_INVALID_HANDLE || err == ERROR_ACCESS_DENIED) ? 1 : 0;
}

/// @brief Get the size of an open file in bytes using 64-bit seek (Windows).
/// @details Leaves the stream positioned at its start.
/// @param fp Open readable stream.
/// @param[out] out_size Receives the nonnegative byte size.
/// @return 1 when size and rewind succeed; otherwise 0.
static int savedata_file_size(FILE *fp, uint64_t *out_size) {
    if (_fseeki64(fp, 0, SEEK_END) != 0)
        return 0;
    __int64 size = _ftelli64(fp);
    if (size < 0)
        return 0;
    if (_fseeki64(fp, 0, SEEK_SET) != 0)
        return 0;
    *out_size = (uint64_t)size;
    return 1;
}

/// @brief Read 64 bits of OS entropy for atomic save temp-file names.
/// @details Fails instead of falling back to pid/time-derived names so atomic writes never use
///          predictable sidecar paths when secure randomness is unavailable.
/// @param out Receives the random value on success.
/// @return 1 on success, 0 on entropy failure.
static int savedata_random_u64(uint64_t *out) {
    unsigned int rand_value = 0;
    unsigned int rand_value2 = 0;
    if (rand_s(&rand_value) != 0 || rand_s(&rand_value2) != 0)
        return 0;
    *out = ((uint64_t)rand_value << 32) | rand_value2;
    return 1;
}
#else
/// @brief Open a file at a UTF-8 path using `fopen` (POSIX).
/// @details Opens a close-on-exec read-only descriptor and wraps it in @p mode.
/// @param path Null-terminated native path.
/// @param mode Binary read stdio mode used by `fdopen`.
/// @return Caller-owned FILE stream, or NULL on open/wrapping failure.
static FILE *savedata_fopen_utf8(const char *path, const char *mode) {
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int fd = open(path, flags);
    if (fd < 0)
        return NULL;
#if defined(FD_CLOEXEC) && !defined(O_CLOEXEC)
    int fd_flags = fcntl(fd, F_GETFD);
    if (fd_flags >= 0)
        (void)fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);
#endif
    FILE *fp = fdopen(fd, mode);
    if (!fp)
        close(fd);
    return fp;
}

/// @brief Delete a file at a UTF-8 path using `remove` (POSIX).
/// @param path Null-terminated path to remove.
/// @return Zero on success; otherwise nonzero with `errno` set.
static int savedata_remove_utf8(const char *path) {
    return remove(path);
}

/// @brief Atomically replace `dst` with `src` using `rename(2)` (POSIX).
/// @param src Staged source path.
/// @param dst Destination path to replace.
/// @return 1 on success; otherwise 0.
static int savedata_replace_utf8(const char *src, const char *dst) {
    return rename(src, dst) == 0 ? 1 : 0;
}

/// @brief Flush and sync an open file to stable storage using `fsync` (POSIX).
/// @param fp Open stdio stream whose descriptor should be synchronized.
/// @return 1 on success; otherwise 0.
static int savedata_sync_file(FILE *fp) {
    return fsync(fileno(fp)) == 0 ? 1 : 0;
}

/// @brief Fsync the parent directory of `path` so rename is crash-durable (POSIX).
/// @param path Committed file path whose parent directory should be synchronized.
/// @return 1 on success; otherwise 0.
static int savedata_sync_parent_dir(const char *path) {
    char *parent = savedata_parent_dir_dup(path);
    if (!parent)
        return 0;
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int fd = open(parent, flags);
    free(parent);
    if (fd < 0)
        return 0;
    int ok = fsync(fd) == 0 ? 1 : 0;
    close(fd);
    return ok;
}

/// @brief Read 64 bits of OS entropy for atomic save temp-file names.
/// @details Fails instead of falling back to pid/time-derived names so atomic writes never use
///          predictable sidecar paths when secure randomness is unavailable.
/// @param out Receives the random value on success.
/// @return 1 on success, 0 on entropy failure.
static int savedata_random_u64(uint64_t *out) {
    return rt_entropy_platform_random_u64(out) == 0;
}

/// @brief Get the size of an open file in bytes using `fseeko`/`ftello` (POSIX).
/// @details Leaves the stream positioned at its start.
/// @param fp Open readable stream.
/// @param[out] out_size Receives the nonnegative byte size.
/// @return 1 when size and rewind succeed; otherwise 0.
static int savedata_file_size(FILE *fp, uint64_t *out_size) {
    if (fseeko(fp, 0, SEEK_END) != 0)
        return 0;
    off_t size = ftello(fp);
    if (size < 0)
        return 0;
    if (fseeko(fp, 0, SEEK_SET) != 0)
        return 0;
    *out_size = (uint64_t)size;
    return 1;
}
#endif

/// @brief Validate game_name bytes for use in file paths.
/// Rejects embedded NULs, path traversal separators, and non-alphanumeric
/// characters except - and _.
/// @param name Candidate byte span.
/// @param len Number of bytes in @p name.
/// @return 1 for 1..64 ASCII letters/digits/dash/underscore bytes; otherwise 0.
static int is_safe_game_name_bytes(const char *name, size_t len) {
    if (!name || len == 0 || len > 64)
        return 0;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

/// @brief Validate a NUL-terminated game name.
/// @param name Candidate null-terminated name.
/// @return 1 when the complete C string satisfies is_safe_game_name_bytes(); otherwise 0.
static int is_safe_game_name(const char *name) {
    return name ? is_safe_game_name_bytes(name, strlen(name)) : 0;
}

/// @brief Build the platform-appropriate save file path for `game_name`.
///
/// Locations follow each OS's convention for per-user data:
///   - Windows: `%APPDATA%\Zanna\<game>\save.json` (or `%HOME%\AppData\...`
///     if APPDATA is unset).
///   - macOS: `~/Library/Application Support/Zanna/<game>/save.json`.
///   - Linux: `~/.local/share/zanna/<game>/save.json` (XDG default).
/// Rejects unsafe game names up front — any path-traversal characters
/// trap before we compose the path. Returns a heap-allocated string
/// that the caller owns (via `free`).
/// @param game_name Validated null-terminated game identifier.
/// @return Malloc-owned absolute save-file path, or NULL after resolution/allocation failure.
static char *compute_save_path(const char *game_name) {
    if (!is_safe_game_name(game_name)) {
        rt_trap("SaveData: invalid game name (must be alphanumeric, dash, or underscore, max 64 "
                "chars)");
        return NULL;
    }
    char *path = NULL;

#if RT_PLATFORM_WINDOWS
    char *appdata_env = savedata_windows_env_utf8(L"APPDATA");
    char *appdata = appdata_env ? savedata_absolute_dup(appdata_env) : NULL;
    free(appdata_env);
    char *home = appdata ? NULL : get_home_dir();
    const char *base = appdata ? appdata : home;
    const char *middle = appdata ? "\\Zanna\\" : "\\AppData\\Roaming\\Zanna\\";
    if (base)
        path = savedata_concat4(base, middle, game_name, "\\save.json");
    free(appdata);
    free(home);
#else
    char *home = get_home_dir();
    if (!home)
        return NULL;
#if RT_PLATFORM_MACOS
    size_t needed = strlen(home) + strlen("/Library/Application Support/Zanna/") +
                    strlen(game_name) + strlen("/save.json") + 1;
    path = (char *)malloc(needed);
    if (path)
        snprintf(
            path, needed, "%s/Library/Application Support/Zanna/%s/save.json", home, game_name);
#else
    size_t needed = strlen(home) + strlen("/.local/share/zanna/") + strlen(game_name) +
                    strlen("/save.json") + 1;
    path = (char *)malloc(needed);
    if (path)
        snprintf(path, needed, "%s/.local/share/zanna/%s/save.json", home, game_name);
#endif
    free(home);
#endif

    if (path && !savedata_path_is_abs(path)) {
        char *abs = savedata_absolute_dup(path);
        free(path);
        path = abs;
    }
    return path;
}

/// @brief Create all parent directories of `file_path` if they don't exist yet.
/// @details `rt_dir_make_all` traps on permission/path failures, but SaveData.Save
///          promises a Boolean failure result. Catch the trap via the recovery hook
///          and report it as failure so operational errors reach the advertised
///          Boolean boundary rather than terminating the program (VDOC-246).
/// @param file_path Save-file path whose parent hierarchy is required.
/// @return 1 when the parent directory exists (or was created), 0 on failure.
static int ensure_parent_dir(const char *file_path) {
    char *dir = savedata_parent_dir_dup(file_path);
    if (!dir)
        return 0;
    rt_string dir_str = rt_string_from_bytes(dir, strlen(dir));
    volatile int ok = 1;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        rt_trap_clear_recovery();
        ok = 0;
    } else {
        rt_dir_make_all(dir_str);
        rt_trap_clear_recovery();
    }
    rt_string_unref(dir_str);
    free(dir);
    return ok;
}

/// @brief Insert or update an int entry at the head of the list.
///
/// If `key` already exists, releases any previous string payload and
/// rewrites the slot (type-converting string→int is intentional —
/// callers can change the stored type). Otherwise allocates a new
/// node and pushes it to the head so the most-recently-written key
/// stays cheap to look up in LIFO workloads. Returns 0 only on OOM.
/// @param[in,out] head Entry-list head pointer.
/// @param key Borrowed validated key; retained when a node is created.
/// @param value Signed value to store.
/// @return 1 after insertion/update; 0 for invalid input or node-allocation failure.
static int savedata_set_int_entry(SaveEntry **head, rt_string key, int64_t value) {
    if (!head || !key)
        return 0;

    const char *key_data = rt_string_cstr(key);
    size_t key_len = (size_t)rt_str_len(key);
    SaveEntry *e = NULL;
    for (SaveEntry *it = *head; it; it = it->next) {
        if (it->key && it->key_len == (int64_t)key_len &&
            memcmp(rt_string_cstr(it->key), key_data, key_len) == 0) {
            e = it;
            break;
        }
    }

    if (e) {
        if (e->str_val) {
            rt_string_unref(e->str_val);
            e->str_val = NULL;
        }
        e->type = SAVE_INT;
        e->int_val = value;
        return 1;
    }

    rt_string retained_key = rt_string_ref(key);
    e = (SaveEntry *)malloc(sizeof(SaveEntry));
    if (!e) {
        rt_string_unref(retained_key);
        return 0;
    }

    e->key = retained_key;
    e->key_len = (int64_t)key_len;
    e->type = SAVE_INT;
    e->int_val = value;
    e->str_val = NULL;
    e->next = *head;
    *head = e;
    return 1;
}

/// @brief Insert or update a string entry at the head of the list (same semantics as the int
/// variant).
/// @details Retains @p value (or the shared empty string for NULL) before releasing an existing
///          value, so replacement is transactional across retain traps.
/// @param[in,out] head Entry-list head pointer.
/// @param key Borrowed validated key retained when a node is created.
/// @param value Borrowed string value; NULL is stored as empty.
/// @return 1 after insertion/update; 0 for invalid input or node-allocation failure.
static int savedata_set_string_entry(SaveEntry **head, rt_string key, rt_string value) {
    if (!head || !key)
        return 0;

    const char *key_data = rt_string_cstr(key);
    size_t key_len = (size_t)rt_str_len(key);
    SaveEntry *e = NULL;
    for (SaveEntry *it = *head; it; it = it->next) {
        if (it->key && it->key_len == (int64_t)key_len &&
            memcmp(rt_string_cstr(it->key), key_data, key_len) == 0) {
            e = it;
            break;
        }
    }

    rt_string stored_value = value ? value : rt_str_empty();
    if (e) {
        rt_string retained_value = rt_string_ref(stored_value);
        if (e->str_val)
            rt_string_unref(e->str_val);
        e->type = SAVE_STR;
        e->str_val = retained_value;
        e->int_val = 0;
        return 1;
    }

    rt_string retained_key = NULL;
    rt_string retained_value = NULL;
    savedata_retain_pair_or_trap(key, stored_value, &retained_key, &retained_value);

    e = (SaveEntry *)malloc(sizeof(SaveEntry));
    if (!e) {
        rt_string_unref(retained_value);
        rt_string_unref(retained_key);
        return 0;
    }

    e->key = retained_key;
    e->key_len = (int64_t)key_len;
    e->type = SAVE_STR;
    e->int_val = 0;
    e->str_val = retained_value;
    e->next = *head;
    *head = e;
    return 1;
}

/// @brief Release a JSON stream parser object via the GC if its refcount drops to zero.
/// @param parser Owned parser object reference; NULL is ignored.
static void savedata_free_parser(void *parser) {
    if (parser && rt_obj_release_check0(parser))
        rt_obj_free(parser);
}

/// @brief Append to the SaveData JSON builder and report append failure.
/// @details SaveData builds the whole JSON payload before atomically replacing
///          the file. Any builder error must abort the save before disk I/O so
///          a truncated JSON document is never written.
/// @param sb Destination builder.
/// @param bytes Bytes to append.
/// @param len Number of bytes in @p bytes.
/// @return 1 on success, 0 when the append failed.
static int savedata_json_append_bytes(rt_string_builder *sb, const char *bytes, size_t len) {
    return rt_sb_append_bytes(sb, bytes, len) == RT_SB_OK;
}

/// @brief Append a C string to the SaveData JSON builder.
/// @param sb Destination builder.
/// @param text NUL-terminated bytes to append.
/// @return 1 on success, 0 when the append failed.
static int savedata_json_append_cstr(rt_string_builder *sb, const char *text) {
    return rt_sb_append_cstr(sb, text) == RT_SB_OK;
}

/// @brief Append a raw UTF-8 string to the builder with JSON escaping.
///
/// Handles all RFC 8259 short escapes (`"` `\` `\b` `\f` `\n` `\r`
/// `\t`) and `\u00XX` for any remaining control character under 0x20.
/// Non-control bytes pass through unchanged, so valid UTF-8 is
/// preserved without re-encoding to `\u` escapes — keeping save
/// files compact and diff-friendly.
/// @param sb Destination JSON string builder.
/// @param str Valid UTF-8 bytes to escape.
/// @param len Number of bytes in @p str.
/// @return 1 on success, 0 when a builder append failed.
static int json_escape_append(rt_string_builder *sb, const char *str, size_t len) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)str[i];
        switch (c) {
            case '"':
                if (!savedata_json_append_cstr(sb, "\\\""))
                    return 0;
                break;
            case '\\':
                if (!savedata_json_append_cstr(sb, "\\\\"))
                    return 0;
                break;
            case '\b':
                if (!savedata_json_append_cstr(sb, "\\b"))
                    return 0;
                break;
            case '\f':
                if (!savedata_json_append_cstr(sb, "\\f"))
                    return 0;
                break;
            case '\n':
                if (!savedata_json_append_cstr(sb, "\\n"))
                    return 0;
                break;
            case '\r':
                if (!savedata_json_append_cstr(sb, "\\r"))
                    return 0;
                break;
            case '\t':
                if (!savedata_json_append_cstr(sb, "\\t"))
                    return 0;
                break;
            default:
                if (c < 0x20) {
                    char escaped[6] = {'\\', 'u', '0', '0', hex[(c >> 4) & 0x0F], hex[c & 0x0F]};
                    if (!savedata_json_append_bytes(sb, escaped, sizeof(escaped)))
                        return 0;
                } else {
                    char ch = (char)c;
                    if (!savedata_json_append_bytes(sb, &ch, 1))
                        return 0;
                }
                break;
        }
    }
    return 1;
}

/// @brief Atomically write `len` bytes to `path` via temp-file-and-rename.
///
/// Protects save files from torn writes and crashes mid-save:
///   1. Open `<path>.tmp.<pid>.<random>.<attempt>` with O_EXCL to avoid
///      colliding with a concurrent writer (retried up to 128 times).
///   2. Write all bytes, flush, and fsync the file so its data hits
///      stable storage.
///   3. Rename the temp onto the real path (atomic on POSIX;
///      MOVEFILE_REPLACE_EXISTING + WRITE_THROUGH on Windows).
///   4. Fsync the parent directory so the dirent update itself is
///      durable.
/// On any failure the temp file is removed so we don't leave garbage
/// alongside real saves.
/// @param path Final save-file path to create or replace.
/// @param data Complete serialized JSON payload.
/// @param len Number of bytes in @p data.
/// @return 1 after durable replacement; otherwise 0 after removing the sidecar when possible.
static int savedata_write_atomic(const char *path, const char *data, size_t len) {
    size_t path_len = strlen(path);
#if RT_PLATFORM_WINDOWS
    unsigned long pid = (unsigned long)_getpid();
#else
    unsigned long pid = (unsigned long)getpid();
#endif

    char *tmp_path = NULL;
    FILE *fp = NULL;
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        char nonce[17];
        uint64_t random_value = 0;
        if (!savedata_random_u64(&random_value)) {
            // Entropy failure is an operational failure, not a programming error:
            // return the Boolean failure Save promises instead of trapping so
            // callers branching on `if save.Save()` can recover (VDOC-246).
            return 0;
        }
        snprintf(nonce, sizeof(nonce), "%016llx", (unsigned long long)random_value);
        if (path_len > SIZE_MAX - strlen(nonce) - 48)
            return 0;
        size_t tmp_cap = path_len + strlen(nonce) + 48;
        tmp_path = (char *)malloc(tmp_cap);
        if (!tmp_path)
            return 0;
        snprintf(tmp_path, tmp_cap, "%s.tmp.%lu.%s.%u", path, pid, nonce, attempt);
#if RT_PLATFORM_WINDOWS
        wchar_t *wide_path = rt_file_path_utf8_to_wide(tmp_path);
        if (!wide_path) {
            free(tmp_path);
            return 0;
        }
        int fd = _wopen(wide_path,
                        _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY | _O_NOINHERIT,
                        _S_IREAD | _S_IWRITE);
        free(wide_path);
        if (fd >= 0)
            fp = _fdopen(fd, "wb");
        if (fd >= 0 && !fp)
            _close(fd);
#else
        int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
        int fd = open(tmp_path, flags, 0600);
        if (fd >= 0)
            fp = fdopen(fd, "wb");
        if (fd >= 0 && !fp)
            close(fd);
#endif
        if (fp)
            break;
        int err = errno;
        free(tmp_path);
        tmp_path = NULL;
        if (err != EEXIST)
            return 0;
    }
    if (!fp) {
        free(tmp_path);
        return 0;
    }

    int ok = 1;
    size_t written = 0;
    while (written < len) {
        size_t n = fwrite(data + written, 1, len - written, fp);
        if (n == 0) {
            if (ferror(fp))
                break;
            ok = 0;
            break;
        }
        written += n;
    }

    ok = ok && (written == len) ? 1 : 0;
    if (ok && fflush(fp) != 0)
        ok = 0;
    if (ok && !savedata_sync_file(fp))
        ok = 0;
    if (fclose(fp) != 0)
        ok = 0;

    if (ok)
        ok = savedata_replace_utf8(tmp_path, path);
    if (ok)
        ok = savedata_sync_parent_dir(path);

    if (!ok)
        savedata_remove_utf8(tmp_path);
    free(tmp_path);
    return ok;
}

//=========================================================================
// Finalizer
//=========================================================================

/// @brief GC finalizer: release all entries and C string allocations owned by the SaveData store.
/// @param obj SaveData implementation payload being finalized.
static void savedata_finalizer(void *obj) {
    rt_savedata_impl *sd = (rt_savedata_impl *)obj;
    free_all_entries(sd);
    free(sd->game_name);
    free(sd->file_path);
    sd->game_name = NULL;
    sd->file_path = NULL;
}

//=========================================================================
// Public API
//=========================================================================

/// @brief Construct a SaveData store keyed by `game_name`. The save file path is computed from
/// the platform's per-user data directory (`%APPDATA%/Zanna/<game>` on Win32,
/// `~/.local/share/zanna/<game>` on Linux, `~/Library/Application Support/Zanna/<game>` on
/// macOS). The name must contain 1..64 ASCII letters, digits, dash, or underscore bytes.
/// @param game_name Borrowed runtime game identifier used to derive the save path.
/// @return Fresh runtime-managed empty SaveData handle; invalid names/allocation/path resolution
///         trap or return NULL as trap-control fallback.
void *rt_savedata_new(rt_string game_name) {
    int64_t raw_name_len = game_name ? rt_str_len(game_name) : 0;
    if (!game_name || raw_name_len == 0) {
        rt_trap("SaveData.New: game name must not be empty");
        return NULL;
    }

    const char *name_data = rt_string_cstr(game_name);
    size_t name_len = (size_t)raw_name_len;
    if (!name_data || !is_safe_game_name_bytes(name_data, name_len)) {
        rt_trap("SaveData: invalid game name (must be alphanumeric, dash, or underscore, max 64 "
                "chars)");
        return NULL;
    }

    rt_savedata_impl *sd =
        (rt_savedata_impl *)rt_obj_new_i64(RT_SAVEDATA_CLASS_ID, (int64_t)sizeof(rt_savedata_impl));
    if (!sd)
        return NULL;
    rt_obj_set_finalizer(sd, savedata_finalizer);

    sd->game_name = (char *)malloc(name_len + 1);
    if (!sd->game_name) {
        if (rt_obj_release_check0(sd))
            rt_obj_free(sd);
        return NULL;
    }
    memcpy(sd->game_name, name_data, name_len);
    sd->game_name[name_len] = '\0';

    sd->file_path = compute_save_path(sd->game_name);
    sd->entries = NULL;
    if (!sd->file_path) {
        if (rt_obj_release_check0(sd))
            rt_obj_free(sd);
        return NULL;
    }

    return sd;
}

/// @brief Store an int64 under `key`. In-memory only — call `_save` to flush to disk.
/// Re-setting an existing key overwrites; type-changing (string→int) is allowed.
/// @param obj Borrowed SaveData handle.
/// @param key Borrowed nonempty, NUL-free, well-formed UTF-8 key.
/// @param value Signed 64-bit value to store.
void rt_savedata_set_int(void *obj, rt_string key, int64_t value) {
    rt_savedata_impl *sd = savedata_require(obj, "SaveData.SetInt: invalid handle");
    size_t klen = 0;
    (void)savedata_require_key(key, &klen, "SaveData.SetInt: invalid key");
    (void)klen;
    savedata_set_int_entry(&sd->entries, key, value);
}

/// @brief Store a string under `key`. In-memory only — call `_save` to persist.
/// @details Replacing an integer changes the entry type. Both key and value must be well-formed
///          UTF-8; the store retains its own references.
/// @param obj Borrowed SaveData handle.
/// @param key Borrowed nonempty, NUL-free UTF-8 key.
/// @param value Borrowed UTF-8 string value; empty is valid.
void rt_savedata_set_string(void *obj, rt_string key, rt_string value) {
    rt_savedata_impl *sd = savedata_require(obj, "SaveData.SetString: invalid handle");
    size_t klen = 0;
    (void)savedata_require_key(key, &klen, "SaveData.SetString: invalid key");
    savedata_require_string_value(value, "SaveData.SetString: invalid value");
    (void)klen;
    savedata_set_string_entry(&sd->entries, key, value);
}

/// @brief Read an int64 by `key`, returning `default_val` if missing or stored as a different type.
/// @param obj Borrowed SaveData handle; NULL yields @p default_val.
/// @param key Borrowed key to look up; NULL/invalid yields @p default_val.
/// @param default_val Value returned for absence or a string-valued entry.
/// @return Stored integer when present with integer type; otherwise @p default_val.
int64_t rt_savedata_get_int(void *obj, rt_string key, int64_t default_val) {
    if (!obj || !key)
        return default_val;
    rt_savedata_impl *sd = savedata_require(obj, "SaveData.GetInt: invalid handle");
    const char *kcstr = rt_string_cstr(key);
    if (!kcstr)
        return default_val;
    SaveEntry *e = find_entry(sd, kcstr, (size_t)rt_str_len(key));
    if (!e || e->type != SAVE_INT)
        return default_val;
    return e->int_val;
}

/// @brief Read a string by `key`, returning `default_val` (or empty) if missing/wrong type.
/// The returned string is freshly retained.
/// @param obj Borrowed SaveData handle; NULL selects the fallback.
/// @param key Borrowed key to look up; NULL/invalid selects the fallback.
/// @param default_val Borrowed fallback string; NULL selects the shared empty string.
/// @return Caller-owned reference to the stored string, @p default_val, or shared empty string.
rt_string rt_savedata_get_string(void *obj, rt_string key, rt_string default_val) {
    if (!obj || !key)
        return default_val ? rt_string_ref(default_val) : rt_str_empty();
    rt_savedata_impl *sd = savedata_require(obj, "SaveData.GetString: invalid handle");
    const char *kcstr = rt_string_cstr(key);
    if (!kcstr)
        return default_val ? rt_string_ref(default_val) : rt_str_empty();
    SaveEntry *e = find_entry(sd, kcstr, (size_t)rt_str_len(key));
    if (!e || e->type != SAVE_STR)
        return default_val ? rt_string_ref(default_val) : rt_str_empty();
    return e->str_val ? rt_string_ref(e->str_val) : rt_str_empty();
}

/// @brief Persist all in-memory entries to disk as JSON. Ensures the parent directory exists,
/// builds the JSON object via string-builder, then atomically writes via the file-IO layer.
/// Returns 1 on success, 0 on any failure.
/// @param obj Borrowed SaveData handle.
/// @return 1 after durable atomic persistence; 0 for null state or any operational/build failure.
int8_t rt_savedata_save(void *obj) {
    if (!obj)
        return 0;
    rt_savedata_impl *sd = savedata_require(obj, "SaveData.Save: invalid handle");
    if (!sd->file_path)
        return 0;

    /* Ensure parent directory exists; a permission/path failure is reported as a
       Boolean failure rather than trapping (VDOC-246). */
    if (!ensure_parent_dir(sd->file_path))
        return 0;

    /* Build JSON using string builder */
    rt_string_builder sb;
    rt_sb_init(&sb);

    if (!savedata_json_append_cstr(&sb, "{\n"))
        goto build_error;

    int first = 1;
    SaveEntry *e = sd->entries;
    while (e) {
        if (!first && !savedata_json_append_cstr(&sb, ",\n"))
            goto build_error;
        first = 0;

        if (!savedata_json_append_cstr(&sb, "  \"") ||
            !json_escape_append(&sb, rt_string_cstr(e->key), (size_t)e->key_len) ||
            !savedata_json_append_cstr(&sb, "\": "))
            goto build_error;

        if (e->type == SAVE_INT) {
            if (rt_sb_append_int(&sb, e->int_val) != RT_SB_OK)
                goto build_error;
        } else {
            if (!savedata_json_append_cstr(&sb, "\""))
                goto build_error;
            if (e->str_val) {
                int64_t raw_len = rt_str_len(e->str_val);
                if (raw_len < 0 ||
                    !json_escape_append(&sb, rt_string_cstr(e->str_val), (size_t)raw_len))
                    goto build_error;
            }
            if (!savedata_json_append_cstr(&sb, "\""))
                goto build_error;
        }

        e = e->next;
    }

    if (!savedata_json_append_cstr(&sb, "\n}\n"))
        goto build_error;

    int ok = savedata_write_atomic(sd->file_path, sb.data, sb.len);
    rt_sb_free(&sb);

    return ok ? 1 : 0;

build_error:
    rt_sb_free(&sb);
    return 0;
}

/// @brief Load a complete JSON save file and transactionally replace the in-memory entries.
/// @details Reads the file into one byte buffer, tokenizes it with `rt_json_stream_*`, and builds
///          a separate entry list. The current store is changed only after the entire top-level
///          object validates. Only integer and string values are accepted. A missing file clears
///          the store and succeeds; empty, oversized, malformed, or unreadable files fail.
/// @param obj Borrowed SaveData handle.
/// @return 1 on successful replacement or a missing-file empty state; otherwise 0.
int8_t rt_savedata_load(void *obj) {
    if (!obj)
        return 0;
    rt_savedata_impl *sd = savedata_require(obj, "SaveData.Load: invalid handle");
    if (!sd->file_path)
        return 0;

    FILE *fp = savedata_fopen_utf8(sd->file_path, "rb");
    if (!fp) {
        if (errno == ENOENT) {
            free_all_entries(sd);
            return 1;
        }
        return 0;
    }

    /* Read entire file */
    uint64_t file_size = 0;
    if (!savedata_file_size(fp, &file_size) || file_size == 0 || file_size > SIZE_MAX) {
        fclose(fp);
        return 0;
    }

    char *buf = (char *)malloc((size_t)file_size + 1);
    if (!buf) {
        fclose(fp);
        return 0;
    }
    size_t read_count = 0;
    while (read_count < (size_t)file_size) {
        size_t n = fread(buf + read_count, 1, (size_t)file_size - read_count, fp);
        if (n == 0) {
            free(buf);
            fclose(fp);
            return 0;
        }
        read_count += n;
    }
    fclose(fp);
    buf[read_count] = '\0';

    /* Parse JSON */
    rt_string json = rt_string_from_bytes(buf, read_count);
    free(buf);

    void *parser = rt_json_stream_new(json);
    if (!parser) {
        rt_string_unref(json);
        return 0;
    }

    SaveEntry *loaded_entries = NULL;
    int success = 0;

    int64_t tok = rt_json_stream_next(parser);
    if (tok != TOK_OBJECT_START)
        goto done;

    tok = rt_json_stream_next(parser);
    if (tok == TOK_OBJECT_END) {
        success = (rt_json_stream_next(parser) == TOK_END) ? 1 : 0;
        goto done;
    }

    while (tok == TOK_KEY) {
        rt_string key_str = rt_json_stream_string_value(parser);
        size_t key_len = 0;
        if (!savedata_is_valid_key(key_str, &key_len)) {
            rt_string_unref(key_str);
            goto done;
        }
        tok = rt_json_stream_next(parser);
        if (tok == TOK_NUMBER) {
            rt_string number_text = rt_json_stream_number_text(parser);
            int64_t int_val = 0;
            if (!savedata_number_text_to_i64(number_text, &int_val)) {
                rt_string_unref(number_text);
                rt_string_unref(key_str);
                goto done;
            }
            rt_string_unref(number_text);
            if (!savedata_set_int_entry(&loaded_entries, key_str, int_val)) {
                rt_string_unref(key_str);
                goto done;
            }
        } else if (tok == TOK_STRING) {
            rt_string val_str = rt_json_stream_string_value(parser);
            if (!savedata_is_valid_string_value(val_str)) {
                rt_string_unref(val_str);
                rt_string_unref(key_str);
                goto done;
            }
            if (!savedata_set_string_entry(&loaded_entries, key_str, val_str)) {
                rt_string_unref(val_str);
                rt_string_unref(key_str);
                goto done;
            }
            rt_string_unref(val_str);
        } else {
            rt_string_unref(key_str);
            goto done;
        }
        rt_string_unref(key_str);

        tok = rt_json_stream_next(parser);
        if (tok == TOK_OBJECT_END) {
            success = (rt_json_stream_next(parser) == TOK_END) ? 1 : 0;
            goto done;
        }
        if (tok != TOK_KEY)
            goto done;
    }

done:
    savedata_free_parser(parser);
    rt_string_unref(json);
    if (!success) {
        while (loaded_entries) {
            SaveEntry *next = loaded_entries->next;
            free_entry(loaded_entries);
            loaded_entries = next;
        }
        return 0;
    }

    free_all_entries(sd);
    sd->entries = loaded_entries;
    return 1;
}

/// @brief Returns 1 if `key` exists in the current entries, regardless of stored type.
/// @param obj Borrowed SaveData handle.
/// @param key Borrowed key to query.
/// @return 1 when a byte-identical key exists; otherwise 0.
int8_t rt_savedata_has_key(void *obj, rt_string key) {
    if (!obj || !key)
        return 0;
    rt_savedata_impl *sd = savedata_require(obj, "SaveData.HasKey: invalid handle");
    const char *kcstr = rt_string_cstr(key);
    if (!kcstr)
        return 0;
    return find_entry(sd, kcstr, (size_t)rt_str_len(key)) != NULL;
}

/// @brief Remove an entry by key. Returns 1 if removed, 0 if absent. In-memory only.
/// @param obj Borrowed SaveData handle.
/// @param key Borrowed key to remove.
/// @return 1 when an entry was removed; otherwise 0.
int8_t rt_savedata_remove(void *obj, rt_string key) {
    if (!obj || !key)
        return 0;
    rt_savedata_impl *sd = savedata_require(obj, "SaveData.Remove: invalid handle");
    const char *kcstr = rt_string_cstr(key);
    if (!kcstr)
        return 0;
    size_t klen = (size_t)rt_str_len(key);

    SaveEntry **pp = &sd->entries;
    while (*pp) {
        SaveEntry *e = *pp;
        if (e->key && e->key_len == (int64_t)klen &&
            memcmp(rt_string_cstr(e->key), kcstr, klen) == 0) {
            *pp = e->next;
            free_entry(e);
            return 1;
        }
        pp = &e->next;
    }
    return 0;
}

/// @brief Drop every in-memory entry (call `_save` afterwards to clear the on-disk file too).
/// @param obj Borrowed SaveData handle; NULL is ignored.
void rt_savedata_clear(void *obj) {
    if (!obj)
        return;
    free_all_entries(savedata_require(obj, "SaveData.Clear: invalid handle"));
}

/// @brief Number of entries currently in the store.
/// @param obj Borrowed SaveData handle; NULL reports zero.
/// @return Current number of unique in-memory keys.
int64_t rt_savedata_count(void *obj) {
    if (!obj)
        return 0;
    rt_savedata_impl *sd = savedata_require(obj, "SaveData.Count: invalid handle");
    int64_t count = 0;
    SaveEntry *e = sd->entries;
    while (e) {
        count++;
        e = e->next;
    }
    return count;
}

/// @brief Read the absolute path where this SaveData persists. Useful for showing the user
/// where their save lives or for backup/restore tooling.
/// @param obj Borrowed SaveData handle.
/// @return Caller-owned runtime string copy of the absolute save-file path, or the shared empty
///         string for NULL/missing state.
rt_string rt_savedata_get_path(void *obj) {
    if (!obj)
        return rt_str_empty();
    rt_savedata_impl *sd = savedata_require(obj, "SaveData.Path: invalid handle");
    if (!sd->file_path)
        return rt_str_empty();
    return rt_string_from_bytes(sd->file_path, strlen(sd->file_path));
}

//=============================================================================
// Path.DataDir — per-user writable data directory
//=============================================================================

/// @brief Zanna.IO.Path.DataDir(app) — resolve (and create) the per-user
///        writable data directory for an application.
///
/// Locations follow each OS's convention for per-user data, mirroring the
/// SaveData layout but rooted directly at the app's own folder:
///   - Windows: `%APPDATA%\<app>` (or `<home>\AppData\Roaming\<app>` when
///     APPDATA is unset).
///   - macOS: `~/Library/Application Support/<app>`.
///   - Linux/other: `$XDG_DATA_HOME/<app>` when set, else
///     `~/.local/share/<app>`.
///
/// The directory (including parents) is created on demand so callers can
/// write settings/saves immediately. Validation covers the runtime string's
/// complete stored byte length, rejecting embedded NULs and every byte outside
/// ASCII letters, digits, dash, and underscore (VDOC-199).
///
/// @param app_name Application folder name (alphanumeric/dash/underscore).
/// @return GC-managed absolute path string (no trailing separator).
rt_string rt_path_data_dir(rt_string app_name) {
    int64_t raw_name_len = app_name ? rt_str_len(app_name) : 0;
    if (!app_name || raw_name_len == 0) {
        rt_trap("Path.DataDir: app name must be non-empty");
        return NULL;
    }
    const char *name = rt_string_cstr(app_name);
    // Validate the runtime String's full stored byte length (not strlen), so
    // bytes after an embedded NUL cannot bypass the charset/64-byte checks and
    // then be silently truncated when composing the directory name (VDOC-199).
    // The safe charset excludes NUL, so a length-aware check also rejects it.
    if (!name || !is_safe_game_name_bytes(name, (size_t)raw_name_len)) {
        rt_trap("Path.DataDir: app name must be alphanumeric, dash, or underscore (max 64 chars)");
        return NULL;
    }

    char *path = NULL;

#if RT_PLATFORM_WINDOWS
    char *appdata_env = savedata_windows_env_utf8(L"APPDATA");
    char *appdata = appdata_env ? savedata_absolute_dup(appdata_env) : NULL;
    free(appdata_env);
    char *home = appdata ? NULL : get_home_dir();
    const char *base = appdata ? appdata : home;
    const char *middle = appdata ? "\\" : "\\AppData\\Roaming\\";
    if (base)
        path = savedata_concat4(base, middle, name, NULL);
    free(appdata);
    free(home);
#elif RT_PLATFORM_MACOS
    char *home = get_home_dir();
    if (home) {
        const char *middle = "/Library/Application Support/";
        size_t needed = strlen(home) + strlen(middle) + strlen(name) + 1;
        path = (char *)malloc(needed);
        if (path)
            snprintf(path, needed, "%s%s%s", home, middle, name);
        free(home);
    }
#else
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && *xdg && savedata_path_is_abs(xdg)) {
        size_t needed = strlen(xdg) + 1 + strlen(name) + 1;
        path = (char *)malloc(needed);
        if (path)
            snprintf(path, needed, "%s/%s", xdg, name);
    } else {
        char *home = get_home_dir();
        if (home) {
            const char *middle = "/.local/share/";
            size_t needed = strlen(home) + strlen(middle) + strlen(name) + 1;
            path = (char *)malloc(needed);
            if (path)
                snprintf(path, needed, "%s%s%s", home, middle, name);
            free(home);
        }
    }
#endif

    if (!path) {
        rt_trap("Path.DataDir: could not resolve a per-user data directory");
        return NULL;
    }
    if (!savedata_path_is_abs(path)) {
        char *abs = savedata_absolute_dup(path);
        free(path);
        path = abs;
        if (!path) {
            rt_trap("Path.DataDir: could not resolve a per-user data directory");
            return NULL;
        }
    }

    rt_string dir_str = rt_string_from_bytes(path, strlen(path));
    rt_dir_make_all(dir_str);
    free(path);
    return dir_str;
}
