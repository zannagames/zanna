---
status: active
audience: contributors
last-verified: 2026-09-14
---

# ADR 0361: Release a Private Console When a Windows Graphical Program Opens Its Window

## Status

Accepted (2026-09-14)

## Context

Zanna's PE writer emits every native executable with the console subsystem
(`IMAGE_SUBSYSTEM_WINDOWS_CUI`), so command-line programs and graphical programs
share one binary format. Windows gives a console-subsystem process started from
Explorer, a Start-menu shortcut, or an installer a console of its own. On Windows 11
that console is a Windows Terminal window. For a graphical program such as Legacy
Baseball or Zanna Studio, that window sits beside the program's own window for the
whole session, shows nothing useful, and ends the program when the user closes it.
Desktop launches on macOS and Linux open no terminal.

Programs started from a terminal must keep printing to it, and output that a caller
redirected to a file or pipe must keep reaching its destination.

`FreeConsole` closes the console handles the process started with, by value. It
does this even after those handles have been replaced in the standard handle slots,
and measurements showed that duplicates and the replacement handles themselves stay
open. Two unsafe outcomes follow:

- **Calling `FreeConsole` alone** leaves the std slots and CRT descriptors 0-2 holding
  closed values. In a measured run, Windows handed all three values to the next 64
  `CreateFileW` calls, so a later `printf` or `WriteFile(GetStdHandle(...))` could
  write into an unrelated file, such as a save file.
- **Replacing a stream with `_dup2`** closes the console handle and then opens a new
  handle. Windows sometimes recycles the just-freed value for that handle, and
  `FreeConsole` then closes the replacement.

## Decision

`vgfx_platform_init_window` (the Win32 adapter behind every Canvas, Canvas3D and GUI
window) calls `vgfx_win32_release_private_console()` from
`src/lib/graphics/src/vgfx_platform_win32_console.c` once the window is shown. The
call is skipped when `ZANNA_GFX_HIDE_WINDOWS` keeps the window hidden.

`int vgfx_win32_release_private_console(void)`:

1. Returns 0 unless `GetConsoleProcessList` reports this process as the console's
   only client. That covers a process with no console and a console shared with a
   shell or parent.
2. Flushes `stdout` and `stderr`.
3. For each of `STD_INPUT_HANDLE`, `STD_OUTPUT_HANDLE` and `STD_ERROR_HANDLE` that
   holds a console handle (`GetFileType` is `FILE_TYPE_CHAR` and `GetConsoleMode`
   succeeds), opens a `NUL` handle. All of these are opened while every console
   handle is still open, so none can take a value `FreeConsole` will close.
4. For each of those streams, if CRT descriptor 0, 1 or 2 holds the same console
   handle, calls `_close` on the descriptor and then `_open_osfhandle` on the `NUL`
   handle. `_open_osfhandle` takes the lowest free descriptor, which is the one just
   closed, and opens no handle. It then points the std slot at the same `NUL` handle
   with `SetStdHandle`. Streams that are not console handles (files, pipes, the
   `NUL` device) are untouched.
5. Calls `FreeConsole` and returns 1 on success. No handle is opened between step 4
   retiring the console handles and this call. If any step cannot complete without
   opening a handle, the function returns 0 and keeps the console.

The native Windows import planner maps `FreeConsole`, `GetConsoleProcessList`,
`GetFileType` and `SetStdHandle` to `kernel32.dll`, and the dynamic-symbol policy
lists them as Windows-only. `_close`, `_dup2` and `_open_osfhandle` already map to
the Universal CRT.

## Consequences

- A graphical program started from the desktop ends up with only its own window.
  The console is visible from process start until that window appears.
- Terminal launches, hidden-window automation (CTest's `requires_display` and
  `graphics3d` labels set `ZANNA_GFX_HIDE_WINDOWS`), and redirected output behave
  as before. A child started by the runtime's `Process` API has pipes for all three
  streams and a windowless console of its own. That console is released, and its
  piped output still reaches the parent.
- Standard output from a desktop-launched graphical program goes to `NUL`. Before
  this change it went to a console that closed when the program exited.
- If another thread opens a handle during the few calls between step 4 and
  `FreeConsole`, Windows may give it a retired console handle value that
  `FreeConsole` then closes. That window is a handful of system calls, once, when
  the first window opens.
- Executables keep the console subsystem, so every command-line behavior of
  `zanna build` output is unchanged. IL, verifier rules, the runtime C ABI and
  serialized formats are unchanged.

## Alternatives Considered

- **Emit the GUI subsystem for graphical programs.** No console would ever flash.
  However, `cmd` and PowerShell do not wait for GUI-subsystem programs, their
  output is invisible in a console unless the program calls `AttachConsole`, and
  console input would be shared with the shell. It also needs new project and
  linker configuration. This remains an option for packaged games, as a separate
  decision.
- **Hide the console window** (`ShowWindow(GetConsoleWindow(), SW_HIDE)`). Rejected:
  under Windows Terminal the console window is a pseudo-window. Hiding it is either
  ignored or hides a terminal window that may host unrelated tabs.
- **`FreeConsole` alone, or `_dup2` replacement streams.** Rejected for the handle
  hazards described above.

## Validation

`test_win32_console` (`src/lib/graphics/tests`, Windows) re-launches itself in
windowless-console roles:

- As the only client of its console (repeated 25 times, because handle values are
  recycled nondeterministically), the child releases it. Every std slot and CRT
  descriptor 0-2 then holds an open non-console handle, Win32 and CRT writes
  succeed, none of 64 newly opened handles aliases a standard stream, and a second
  call is a no-op.
- As one of two clients of a console, the child keeps it and leaves its handles
  unchanged.
- With standard output redirected to a file, the child releases the console, and
  both Win32 and CRT writes still land in the file.

Results on the reference machine:

| Implementation under test | Failing runs |
|---------------------------|--------------|
| This implementation | 0 of 30 |
| `_dup2` per stream | 10 of 10 |
| Stub that never releases | fails |
| `FreeConsole` alone | fails |

`test_platform_import_planners` pins the four `kernel32.dll` imports and their
Windows-only scope.

A native 2D Canvas program was started with `Start-Process`, the way Explorer starts
programs. Before this change a Windows Terminal window appeared beside the program's
window. After it, only the program's window appeared. Started from a shell, or with
its output redirected, the same program still printed the line it writes after its
window opens.
