//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/io/rt_asset_decode.c
// Purpose: Extension-based type dispatch for the asset manager. Decodes raw
//          bytes into Pixels or Sound runtime objects using the appropriate
//          in-memory or file-backed format decoder.
//
// Key invariants:
//   - JPEG, PNG, GIF, and Sound decoding use in-memory entry points.
//   - BMP currently spills to an exclusive temporary file for its path-based loader.
//   - Extension matching is case-insensitive.
//   - Returns NULL for both unknown extensions and failed recognized decodes;
//     rt_asset_extension_is_typed() lets the caller distinguish those cases so
//     corrupt typed assets never silently become Bytes.
//
// Ownership/Lifetime:
//   - Input data buffer is borrowed (not freed).
//   - Returned objects are GC-managed.
//   - Temp files are cleaned up after use.
//
// Links: src/runtime/io/rt_asset.c,
//        src/runtime/graphics/2d/rt_pixels_io.c,
//        src/runtime/audio/rt_audio.c
//
//===----------------------------------------------------------------------===//
/**
 * @file
 * @brief Implements extension-driven decoding of resolved asset bytes.
 * @details Routes supported image and audio formats to bounded in-memory
 * decoders, constructs managed Pixels objects from raw RGBA results, and uses
 * a private temporary directory only for legacy path-based BMP decoding while
 * recovering traps and cleaning every intermediate resource.
 */

#ifdef _WIN32
#ifndef _CRT_RAND_S
#define _CRT_RAND_S
#endif
#endif

#include "rt_platform.h"
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <io.h>
#include <process.h>
#include <wchar.h>
#include <windows.h>
#else
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
/// @copydoc mkdtemp()
extern char *mkdtemp(char *);
#endif

#include "network/rt_entropy_platform.h"
#include "rt_file_path.h"
#include "rt_pixels_internal.h"
#include "rt_string.h"

// ─── External declarations ──────────────────────────────────────────────────

// Image decoders (file-based)
/// @copydoc rt_pixels_load_png()
extern void *rt_pixels_load_png(void *path);
/// @copydoc rt_pixels_load_bmp()
extern void *rt_pixels_load_bmp(void *path);
/// @copydoc rt_pixels_load_gif()
extern void *rt_pixels_load_gif(void *path);
/// @copydoc rt_pixels_load()
extern void *rt_pixels_load(void *path);

// Image decoder (buffer-based)
/// @copydoc rt_jpeg_decode_buffer()
extern void *rt_jpeg_decode_buffer(const uint8_t *data, size_t len);
/// @copydoc rt_png_decode_buffer_rgba32()
extern int rt_png_decode_buffer_rgba32(const uint8_t *data,
                                       size_t len,
                                       uint32_t **out_pixels,
                                       int64_t *out_width,
                                       int64_t *out_height);
/// @copydoc rt_gif_decode_memory_first_rgba32()
extern int rt_gif_decode_memory_first_rgba32(
    const uint8_t *data, size_t len, uint32_t **out_pixels, int *out_width, int *out_height);

// Audio decoder (buffer-based)
/// @copydoc rt_sound_load_mem()
extern void *rt_sound_load_mem(const void *data, int64_t size);

// Runtime string helpers
/// @copydoc rt_string_from_bytes()
extern rt_string rt_string_from_bytes(const char *data, size_t len);
/// @copydoc rt_trap()
extern void rt_trap(const char *msg);
/// @copydoc rt_trap_set_recovery()
extern void rt_trap_set_recovery(jmp_buf *buf);
/// @copydoc rt_trap_clear_recovery()
extern void rt_trap_clear_recovery(void);
/// @copydoc rt_trap_get_error()
extern const char *rt_trap_get_error(void);

/// @brief Select a bounded, path-safe suffix for a temporary decode file.
/// @param ext Candidate extension beginning with `.`.
/// @return Borrowed @p ext when it is at most 32 bytes and contains no path
/// separator or colon; otherwise the static suffix `.tmp`.
static const char *asset_temp_suffix(const char *ext) {
    if (!ext || ext[0] != '.')
        return ".tmp";
    size_t len = strlen(ext);
    if (len == 0 || len > 32)
        return ".tmp";
    for (size_t i = 0; i < len; ++i) {
        if (ext[i] == '/' || ext[i] == '\\' || ext[i] == ':')
            return ".tmp";
    }
    return ext;
}

#ifdef _WIN32
/// @brief Convert a NUL-terminated UTF-16 path to strict UTF-8.
/// @param wide UTF-16 input string.
/// @return Heap-allocated UTF-8 string owned by the caller, or `NULL` for
/// invalid input, conversion failure, or allocation failure.
static char *asset_decode_wide_to_utf8_dup(const wchar_t *wide) {
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
#endif

/// @brief Best-effort removal of a temporary file.
/// @param path NUL-terminated UTF-8 path previously created by this unit.
static void asset_remove_temp_path(const char *path) {
#ifdef _WIN32
    wchar_t *wide = rt_file_path_utf8_to_wide(path);
    if (wide) {
        DeleteFileW(wide);
        free(wide);
    }
#else
    remove(path);
#endif
}

/// @brief Best-effort removal of an empty temporary directory.
/// @param path NUL-terminated UTF-8 directory path previously created by this
/// unit.
static void asset_remove_temp_dir(const char *path) {
#ifdef _WIN32
    wchar_t *wide = rt_file_path_utf8_to_wide(path);
    if (wide) {
        RemoveDirectoryW(wide);
        free(wide);
    }
#else
    rmdir(path);
#endif
}

/// @brief Remove and free a temporary asset file and its private directory.
/// @details The file is removed before the directory. Pointers are nulled after
///          free so callers can safely use this helper from normal and trap
///          recovery paths without double cleanup.
/// @param tmppath In/out heap path to the temporary file.
/// @param tmpdir_path In/out heap path to the temporary directory.
static void asset_cleanup_tempfile(char **tmppath, char **tmpdir_path) {
    if (tmppath && *tmppath) {
        asset_remove_temp_path(*tmppath);
        free(*tmppath);
        *tmppath = NULL;
    }
    if (tmpdir_path && *tmpdir_path) {
        asset_remove_temp_dir(*tmpdir_path);
        free(*tmpdir_path);
        *tmpdir_path = NULL;
    }
}

/// @brief Join a temporary directory and leaf with the native separator.
/// @param dir NUL-terminated directory path.
/// @param leaf NUL-terminated leaf name.
/// @return Heap-allocated joined path owned by the caller, or `NULL` for
/// invalid input, size overflow, or allocation failure.
static char *asset_join_temp_path(const char *dir, const char *leaf) {
    if (!dir || !leaf)
        return NULL;
    size_t dir_len = strlen(dir);
    size_t leaf_len = strlen(leaf);
    int needs_sep = dir_len > 0 && dir[dir_len - 1] != '/' && dir[dir_len - 1] != '\\';
    if (dir_len > SIZE_MAX - leaf_len - (needs_sep ? 1U : 0U) - 1U)
        return NULL;
    char *path = (char *)malloc(dir_len + (needs_sep ? 1U : 0U) + leaf_len + 1U);
    if (!path)
        return NULL;
    memcpy(path, dir, dir_len);
    size_t pos = dir_len;
    if (needs_sep)
#ifdef _WIN32
        path[pos++] = '\\';
#else
        path[pos++] = '/';
#endif
    memcpy(path + pos, leaf, leaf_len);
    path[pos + leaf_len] = '\0';
    return path;
}

#ifndef _WIN32
/// @brief Return the POSIX temporary-directory base without linking the system runtime module.
/// @details Mirrors `rt_machine_temp`'s environment lookup order while keeping asset decode
///          usable in reduced native-runtime link sets that do not include `rt_machine_temp`.
/// @return Borrowed process-environment pointer, or the static `/tmp` fallback.
static const char *asset_posix_temp_base(void) {
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp)
        tmp = getenv("TMP");
    if (!tmp || !*tmp)
        tmp = getenv("TEMP");
    if (!tmp || !*tmp)
        tmp = "/tmp";
    struct stat st;
    if (tmp[0] != '/' || stat(tmp, &st) != 0 || !S_ISDIR(st.st_mode) ||
        access(tmp, W_OK | X_OK) != 0 ||
        ((st.st_mode & S_IWOTH) != 0 && (st.st_mode & S_ISVTX) == 0)) {
        tmp = "/tmp";
    }
    return tmp;
}
#endif

/// @brief Build a Pixels object from malloc-owned raw RGBA32 pixels.
/// @details The decoder helpers return row-major 0xRRGGBBAA pixels in a
///          malloc-owned array. This helper copies that array into a runtime
///          Pixels object, marks the buffer generation changed, releases the
///          temporary raw array, and returns the GC-managed Pixels handle.
/// @param raw_pixels Malloc-owned raw pixel array; consumed by this helper.
/// @param width Image width in pixels.
/// @param height Image height in pixels.
/// @return New Pixels object, or NULL on invalid dimensions/allocation failure.
static void *asset_pixels_from_raw_rgba32(uint32_t *raw_pixels, int64_t width, int64_t height) {
    if (!raw_pixels || width <= 0 || height <= 0) {
        free(raw_pixels);
        return NULL;
    }
    if (width > INT64_MAX / height || (uint64_t)(width * height) > SIZE_MAX / sizeof(uint32_t)) {
        free(raw_pixels);
        return NULL;
    }
    void *pixels_obj = rt_pixels_new(width, height);
    if (!pixels_obj) {
        free(raw_pixels);
        return NULL;
    }
    rt_pixels_impl *pixels = (rt_pixels_impl *)pixels_obj;
    size_t bytes = (size_t)(width * height) * sizeof(uint32_t);
    memcpy(pixels->data, raw_pixels, bytes);
    pixels->generation++;
    pixels->alpha_scan_valid = 0;
    free(raw_pixels);
    return pixels_obj;
}

/// @brief Decode PNG bytes directly into a Pixels object.
/// @param data PNG file bytes.
/// @param size Number of bytes in @p data.
/// @return New Pixels object on success, NULL on decode failure.
static void *asset_decode_png_memory(const uint8_t *data, size_t size) {
    uint32_t *raw_pixels = NULL;
    int64_t width = 0;
    int64_t height = 0;
    if (!rt_png_decode_buffer_rgba32(data, size, &raw_pixels, &width, &height))
        return NULL;
    return asset_pixels_from_raw_rgba32(raw_pixels, width, height);
}

/// @brief Decode the first GIF frame directly into a Pixels object.
/// @param data GIF file bytes.
/// @param size Number of bytes in @p data.
/// @return New Pixels object on success, NULL on decode failure.
static void *asset_decode_gif_memory(const uint8_t *data, size_t size) {
    uint32_t *raw_pixels = NULL;
    int width = 0;
    int height = 0;
    if (!rt_gif_decode_memory_first_rgba32(data, size, &raw_pixels, &width, &height))
        return NULL;
    return asset_pixels_from_raw_rgba32(raw_pixels, (int64_t)width, (int64_t)height);
}

// ─── Helpers ────────────────────────────────────────────────────────────────

/// @brief Return 1 if `name`'s extension matches `ext` (case-insensitive), 0 otherwise.
/// @param name NUL-terminated asset name whose final suffix is inspected.
/// @param ext NUL-terminated extension including its leading dot.
/// @return 1 for a case-insensitive match; otherwise 0.
static int iext(const char *name, const char *ext) {
    const char *dot = strrchr(name, '.');
    if (!dot)
        return 0;
#ifdef _WIN32
    return _stricmp(dot, ext) == 0;
#else
    return strcasecmp(dot, ext) == 0;
#endif
}

/// @brief Adapter that lets file-based image decoders consume in-memory bytes.
///
/// Some decoders expose only a file-path entry point. To feed those loaders
/// from embedded/mounted assets, this helper spills the bytes to an exclusively
/// created temp file, calls the path-based loader, and unlinks it. The current
/// caller uses this path for BMP; JPEG, PNG, GIF, and audio decode in memory.
/// A loader trap is captured long enough to release the runtime path and remove
/// the private file/directory, then propagated with its original diagnostic.
/// @param data Borrowed encoded file bytes.
/// @param size Number of accessible bytes in @p data.
/// @param ext Extension used for the private temporary file.
/// @param loader Path-based decoder invoked with a temporary runtime string.
/// @return The loader's GC-managed result, or `NULL` when temporary-file setup,
/// writing, runtime-string creation, or decoding fails.
static void *load_via_tempfile(const uint8_t *data,
                               size_t size,
                               const char *ext,
                               void *(*loader)(void *path_str)) {
    const char *suffix = asset_temp_suffix(ext);
    char *tmppath = NULL;
    char *tmpdir_path = NULL;
    FILE *f = NULL;
#ifdef _WIN32
    DWORD need = GetTempPathW(0, NULL);
    if (need == 0)
        return NULL;
    wchar_t *wtmpdir = (wchar_t *)malloc(((size_t)need + 1) * sizeof(wchar_t));
    if (!wtmpdir)
        return NULL;
    DWORD tmpdir_len = GetTempPathW(need + 1, wtmpdir);
    if (tmpdir_len == 0 || tmpdir_len > need) {
        free(wtmpdir);
        return NULL;
    }
    char *tmpdir = asset_decode_wide_to_utf8_dup(wtmpdir);
    free(wtmpdir);
    if (!tmpdir)
        return NULL;
    HANDLE h = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 128 && h == INVALID_HANDLE_VALUE; ++attempt) {
        char dir_leaf[64];
        uint64_t random_value = 0;
        if (rt_entropy_platform_random_u64(&random_value) != 0) {
            free(tmpdir);
            free(tmppath);
            free(tmpdir_path);
            return NULL;
        }
        snprintf(dir_leaf,
                 sizeof(dir_leaf),
                 "zanna_asset_%016llx_%d",
                 (unsigned long long)random_value,
                 attempt);
        free(tmpdir_path);
        tmpdir_path = asset_join_temp_path(tmpdir, dir_leaf);
        if (!tmpdir_path)
            break;
        wchar_t *wide_dir = rt_file_path_utf8_to_wide(tmpdir_path);
        if (!wide_dir)
            continue;
        BOOL made_dir = CreateDirectoryW(wide_dir, NULL);
        DWORD dir_err = made_dir ? ERROR_SUCCESS : GetLastError();
        free(wide_dir);
        if (!made_dir) {
            if (dir_err == ERROR_ALREADY_EXISTS)
                continue;
            break;
        }
        char file_leaf[64];
        snprintf(file_leaf, sizeof(file_leaf), "asset%s", suffix);
        free(tmppath);
        tmppath = asset_join_temp_path(tmpdir_path, file_leaf);
        if (!tmppath) {
            asset_remove_temp_dir(tmpdir_path);
            break;
        }
        wchar_t *wide = rt_file_path_utf8_to_wide(tmppath);
        if (!wide) {
            asset_remove_temp_dir(tmpdir_path);
            continue;
        }
        h = CreateFileW(wide,
                        GENERIC_WRITE,
                        0,
                        NULL,
                        CREATE_NEW,
                        FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED,
                        NULL);
        free(wide);
        if (h == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
            asset_remove_temp_dir(tmpdir_path);
            free(tmpdir);
            free(tmppath);
            free(tmpdir_path);
            return NULL;
        }
        if (h == INVALID_HANDLE_VALUE)
            asset_remove_temp_dir(tmpdir_path);
    }
    free(tmpdir);
    if (h == INVALID_HANDLE_VALUE) {
        free(tmppath);
        free(tmpdir_path);
        return NULL;
    }
    int fd = _open_osfhandle((intptr_t)h, _O_BINARY | _O_NOINHERIT);
    if (fd < 0) {
        CloseHandle(h);
        asset_remove_temp_path(tmppath);
        asset_remove_temp_dir(tmpdir_path);
        free(tmppath);
        free(tmpdir_path);
        return NULL;
    }
    f = _fdopen(fd, "wb");
    if (!f) {
        _close(fd);
        asset_remove_temp_path(tmppath);
        asset_remove_temp_dir(tmpdir_path);
        free(tmppath);
        free(tmpdir_path);
        return NULL;
    }
#else
    const char *tmpdir = asset_posix_temp_base();
    tmpdir_path = asset_join_temp_path(tmpdir, "zanna_asset_XXXXXX");
    if (!tmpdir_path)
        return NULL;
    if (!mkdtemp(tmpdir_path)) {
        free(tmpdir_path);
        return NULL;
    }
    char file_leaf[64];
    snprintf(file_leaf, sizeof(file_leaf), "asset%s", suffix);
    tmppath = asset_join_temp_path(tmpdir_path, file_leaf);
    if (!tmppath) {
        asset_remove_temp_dir(tmpdir_path);
        free(tmpdir_path);
        return NULL;
    }
    int fd = -1;
#ifdef O_CLOEXEC
    fd = open(tmppath, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
#else
    fd = open(tmppath, O_WRONLY | O_CREAT | O_EXCL, 0600);
#endif
    if (fd < 0) {
        asset_remove_temp_dir(tmpdir_path);
        free(tmpdir_path);
        free(tmppath);
        return NULL;
    }
#if defined(FD_CLOEXEC) && !defined(O_CLOEXEC)
    int fd_flags = fcntl(fd, F_GETFD);
    if (fd_flags >= 0)
        (void)fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);
#endif
    f = fdopen(fd, "wb");
    if (!f) {
        close(fd);
        asset_remove_temp_path(tmppath);
        asset_remove_temp_dir(tmpdir_path);
        free(tmpdir_path);
        free(tmppath);
        return NULL;
    }
#endif

    if (size > 0 && fwrite(data, 1, size, f) != size) {
        fclose(f);
        asset_remove_temp_path(tmppath);
        asset_remove_temp_dir(tmpdir_path);
        free(tmppath);
        free(tmpdir_path);
        return NULL;
    }
    if (fclose(f) != 0) {
        asset_remove_temp_path(tmppath);
        asset_remove_temp_dir(tmpdir_path);
        free(tmppath);
        free(tmpdir_path);
        return NULL;
    }

    // Call file-based loader
    rt_string path_str = rt_string_from_bytes(tmppath, strlen(tmppath));
    if (!path_str) {
        asset_remove_temp_path(tmppath);
        asset_remove_temp_dir(tmpdir_path);
        free(tmppath);
        free(tmpdir_path);
        return NULL;
    }
    void *result = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[256];
        const char *err = rt_trap_get_error();
        snprintf(saved_error,
                 sizeof(saved_error),
                 "%s",
                 err && err[0] ? err : "Asset.Decode: file-based loader failed");
        rt_trap_clear_recovery();
        rt_string_unref(path_str);
        asset_cleanup_tempfile(&tmppath, &tmpdir_path);
        rt_trap(saved_error);
        return NULL;
    }
    result = loader((void *)path_str);
    rt_trap_clear_recovery();
    rt_string_unref(path_str);

    // Cleanup
    asset_cleanup_tempfile(&tmppath, &tmpdir_path);
    return result;
}

// ─── rt_asset_decode_typed ──────────────────────────────────────────────────

/// @brief Return 1 when `name`'s extension has a registered typed decoder
///        (image or audio), 0 otherwise. Lets `rt_asset_load` distinguish an
///        UNRECOGNIZED extension from a RECOGNIZED decoder failure (VDOC-181).
/// @param name NUL-terminated asset name used for extension dispatch.
/// @return 1 for JPEG, WAV, OGG, MP3, PNG, BMP, or GIF suffixes,
/// case-insensitively; otherwise 0.
int rt_asset_extension_is_typed(const char *name) {
    if (!name)
        return 0;
    return iext(name, ".jpg") || iext(name, ".jpeg") || iext(name, ".wav") || iext(name, ".ogg") ||
           iext(name, ".mp3") || iext(name, ".png") || iext(name, ".bmp") || iext(name, ".gif");
}

/// @brief Decode raw asset bytes according to a recognized filename extension.
/// @details JPEG, PNG, first-frame GIF, and audio decoding operate directly on
/// memory. BMP uses a private exclusive temporary file because its decoder is
/// path-based.
/// @param name NUL-terminated asset name used for extension dispatch.
/// @param data Borrowed encoded asset bytes.
/// @param size Number of accessible bytes in @p data.
/// @return Fresh GC-managed Pixels or Sound object, or `NULL` for invalid
/// input, unknown extension, or decode failure.
void *rt_asset_decode_typed(const char *name, const uint8_t *data, size_t size) {
    if (!name || !data || size == 0)
        return NULL;

    // JPEG — direct buffer API
    if (iext(name, ".jpg") || iext(name, ".jpeg"))
        return rt_jpeg_decode_buffer(data, size);

    // Audio — direct buffer API (WAV/OGG/MP3 format detection is internal)
    if (iext(name, ".wav") || iext(name, ".ogg") || iext(name, ".mp3"))
        return rt_sound_load_mem(data, (int64_t)size);

    // PNG — direct buffer API
    if (iext(name, ".png"))
        return asset_decode_png_memory(data, size);

    // BMP — via temp file
    if (iext(name, ".bmp"))
        return load_via_tempfile(data, size, ".bmp", rt_pixels_load_bmp);

    // GIF — direct buffer API (first frame)
    if (iext(name, ".gif"))
        return asset_decode_gif_memory(data, size);

    // Unknown extension — return NULL (caller will return as Bytes)
    return NULL;
}
