//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/gui/src/vg_file_stdio.h
// Purpose: Open UTF-8 GUI asset paths through non-inheritable native handles.
// Key invariants:
//   - Windows paths are decoded strictly as UTF-8 and opened through UTF-16.
//   - Returned Windows descriptors cannot leak into spawned child processes.
// Ownership/Lifetime:
//   - The caller owns and must fclose every returned FILE pointer.
//   - The helper retains no path storage or process-global state.
// Links: src/lib/gui/src/font/vg_font.c, src/lib/gui/src/widgets/vg_image.c
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Provides a portable UTF-8 binary-read opener for GUI assets.
/// @details Windows paths are converted strictly to UTF-16 and opened with a
///          non-inheritable descriptor; other platforms delegate to `fopen`.

#pragma once

#include "../../../runtime/rt_platform.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#if RT_PLATFORM_WINDOWS
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <windows.h>
#endif

/// @brief Open a UTF-8 path for binary reading.
/// @details Windows performs strict UTF-8 to UTF-16 conversion and creates a
///          non-inheritable descriptor. Other platforms pass UTF-8 directly to
///          the native filesystem through fopen.
/// @param path NUL-terminated UTF-8 path.
/// @return Caller-owned stream, or NULL on validation, conversion, or open failure.
static inline FILE *vg_file_open_read_utf8(const char *path) {
    if (!path) {
        errno = EINVAL;
        return NULL;
    }
#if RT_PLATFORM_WINDOWS
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
    int fd = -1;
    const errno_t open_error =
        _wsopen_s(&fd, wide_path, _O_RDONLY | _O_BINARY | _O_NOINHERIT, _SH_DENYWR, 0);
    free(wide_path);
    if (open_error != 0) {
        errno = (int)open_error;
        return NULL;
    }
    FILE *file = _fdopen(fd, "rb");
    if (!file) {
        const int fdopen_error = errno ? errno : EIO;
        (void)_close(fd);
        errno = fdopen_error;
    }
    return file;
#else
    return fopen(path, "rb");
#endif
}
