//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/audio/src/vaud_file_stdio.h
// Purpose: Open UTF-8 audio file paths through non-inheritable native handles.
// Key invariants:
//   - Windows paths are decoded strictly as UTF-8 and opened through UTF-16.
//   - Returned Windows descriptors cannot leak into spawned child processes.
// Ownership/Lifetime:
//   - The caller owns and must fclose every returned FILE pointer.
//   - The helper retains no path storage or process-global state.
// Links: src/lib/audio/src/vaud_wav.c
//
//===----------------------------------------------------------------------===//
#pragma once

#include "vaud_config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(VAUD_PLATFORM_WINDOWS)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

/// @brief Open a UTF-8 path for binary reading.
/// @details Windows performs strict UTF-8 to UTF-16 conversion and creates a
///          non-inheritable descriptor. Other platforms pass UTF-8 directly to
///          the native filesystem through fopen.
/// @param path NUL-terminated UTF-8 path.
/// @return Caller-owned stream, or NULL on validation, conversion, or open failure.
static inline FILE *vaud_file_open_read_utf8(const char *path) {
    if (!path) {
        errno = EINVAL;
        return NULL;
    }
#if defined(VAUD_PLATFORM_WINDOWS)
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (required <= 0) {
        errno = EINVAL;
        return NULL;
    }
    wchar_t *wide_path = (wchar_t *)malloc((size_t)required * sizeof(wchar_t));
    if (!wide_path)
        return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide_path, required) !=
        required) {
        free(wide_path);
        errno = EINVAL;
        return NULL;
    }
    const int fd = _wopen(wide_path, _O_RDONLY | _O_BINARY | _O_NOINHERIT);
    free(wide_path);
    if (fd < 0)
        return NULL;
    FILE *file = _fdopen(fd, "rb");
    if (!file)
        _close(fd);
    return file;
#else
    return fopen(path, "rb");
#endif
}
