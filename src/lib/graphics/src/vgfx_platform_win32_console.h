//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/graphics/src/vgfx_platform_win32_console.h
// Purpose: Detach a graphical Win32 process from a console that exists only for
//          it, keeping its standard streams safe to use afterwards.
// Key invariants:
//   - A console shared with any other process (a terminal launch) is never touched.
//   - Console-backed standard streams move to the NUL device before the console is
//     released, so no closed handle value stays in a std slot or CRT descriptor 0-2.
//   - Redirected standard streams (files, pipes) are left exactly as they are.
// Ownership/Lifetime:
//   - The NUL handles installed in the std slots stay open for the process lifetime.
// Links: src/lib/graphics/src/vgfx_platform_win32.c,
//        docs/adr/0361-release-private-console-for-windows-graphical-programs.md
//
//===----------------------------------------------------------------------===//

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Release the console when this process is its only client.
/// @details Windows gives a console-subsystem executable started from Explorer, a
///          shortcut, or an installer a console of its own; on Windows 11 that is a
///          Windows Terminal window. Nothing reads it once the program has a window.
///          Each standard stream that refers to that console is first pointed at the
///          NUL device, both in its std slot and in its CRT descriptor, because
///          FreeConsole closes the console handles the process started with by value
///          and Windows recycles freed values for the next handles it opens.
/// @return 1 when the console was released; 0 when the process has no console, the
///         console has other clients, or a stream could not be moved (the console
///         is then kept).
int vgfx_win32_release_private_console(void);

#ifdef __cplusplus
}
#endif
