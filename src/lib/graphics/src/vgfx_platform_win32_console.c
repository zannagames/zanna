//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/graphics/src/vgfx_platform_win32_console.c
// Purpose: Release a console window that exists only for a graphical Win32 process.
// Key invariants:
//   - Only a console whose sole client is this process is released.
//   - FreeConsole closes the console handles the process started with by value,
//     even after they leave the std slots. Every replacement handle is therefore
//     opened while those values are still taken, and nothing opens a handle
//     between retiring them and FreeConsole.
// Ownership/Lifetime:
//   - A replacement NUL handle is owned by its CRT descriptor, or by the std slot
//     alone (never closed) when the descriptor did not mirror the console handle.
// Links: src/lib/graphics/src/vgfx_platform_win32_console.h,
//        src/lib/graphics/tests/test_win32_console.c
//
//===----------------------------------------------------------------------===//

#include "vgfx_platform_win32_console.h"

#include <windows.h>

#include <fcntl.h>
#include <io.h>
#include <stdint.h>
#include <stdio.h>

/// @brief Whether @p handle refers to a console input or screen buffer.
/// @param handle Handle from a std slot; may be NULL or INVALID_HANDLE_VALUE.
/// @return Nonzero for a live console handle.
static int win32_console_is_console_handle(HANDLE handle) {
    DWORD mode = 0;
    return handle != NULL && handle != INVALID_HANDLE_VALUE &&
           GetFileType(handle) == FILE_TYPE_CHAR && GetConsoleMode(handle, &mode);
}

/// @brief Open the NUL device for standard input (@p reading) or output.
/// @return A new handle, or NULL on failure.
static HANDLE win32_console_open_nul(int reading) {
    HANDLE handle = CreateFileW(L"NUL",
                                reading ? GENERIC_READ : GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                NULL,
                                OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL,
                                NULL);
    return handle == INVALID_HANDLE_VALUE ? NULL : handle;
}

int vgfx_win32_release_private_console(void) {
    static const DWORD slots[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    DWORD clients[2];
    if (GetConsoleProcessList(clients, 2) != 1)
        return 0;
    (void)fflush(stdout);
    (void)fflush(stderr);

    HANDLE console[3] = {NULL, NULL, NULL};
    HANDLE nul[3] = {NULL, NULL, NULL};
    for (int d = 0; d < 3; ++d) {
        HANDLE handle = GetStdHandle(slots[d]);
        if (!win32_console_is_console_handle(handle))
            continue;
        console[d] = handle;
        nul[d] = win32_console_open_nul(d == 0);
        if (!nul[d]) {
            for (int opened = 0; opened < d; ++opened) {
                if (nul[opened])
                    CloseHandle(nul[opened]);
            }
            return 0;
        }
    }

    // _close frees descriptor d and its console handle; _open_osfhandle takes the
    // lowest free descriptor, which is d again, and opens no new handle.
    int keep_console = 0;
    for (int d = 0; d < 3; ++d) {
        if (!console[d])
            continue;
        if ((HANDLE)_get_osfhandle(d) == console[d]) {
            (void)_close(d);
            int descriptor = _open_osfhandle((intptr_t)nul[d], d == 0 ? _O_RDONLY : _O_WRONLY);
            if (descriptor < 0) {
                keep_console = 1;
            } else if (descriptor != d) {
                // Unreachable while descriptors 0-2 stay open, but never leave
                // d closed: _dup2 opens a handle, so the console must stay.
                (void)_dup2(descriptor, d);
                (void)_close(descriptor);
                nul[d] = (HANDLE)_get_osfhandle(d);
                keep_console = 1;
            }
        }
        (void)SetStdHandle(slots[d], nul[d]);
    }
    if (keep_console)
        return 0;
    return FreeConsole() ? 1 : 0;
}
