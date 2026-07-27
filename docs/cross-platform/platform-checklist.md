---
status: active
audience: contributors
last-verified: 2026-07-26
---

# Cross-Platform Developer Checklist

When adding or modifying platform-sensitive functionality in Zanna, use this
reference to identify every file that must be touched. The codebase targets
**Windows (x86-64)**, **macOS (ARM64 / Apple Silicon)**, and **Linux (x86-64 and AArch64)**.

For a user-facing reference of runtime behavioral differences across platforms, see
[platform-differences.md](platform-differences.md).

---

## Platform Abstraction Layer

Zanna now uses two shared platform/capability layers:

| Layer | Intended Users | Entry Point |
|-------|----------------|-------------|
| Runtime C platform macros | Runtime C code and low-level platform adapters | `src/runtime/rt_platform.h` |
| Generated C++/tool/test capabilities | Codegen, tools, tests, REPL, common utilities | `src/common/PlatformCapabilities.hpp` |

### Runtime C Platform Macros

| Macro | Detects | Header |
|-------|---------|--------|
| `RT_PLATFORM_WINDOWS` | `_WIN32 \|\| _WIN64` | `src/runtime/rt_platform.h` |
| `RT_PLATFORM_MACOS` | `__APPLE__` | `src/runtime/rt_platform.h` |
| `RT_PLATFORM_LINUX` | `__linux__` | `src/runtime/rt_platform.h` |
| `RT_COMPILER_MSVC` | MSVC (not Clang-CL) | `src/runtime/rt_platform.h` |
| `RT_COMPILER_GCC_LIKE` | GCC or Clang | `src/runtime/rt_platform.h` |

`rt_platform.h` also provides cross-platform shims for TLS
(`RT_THREAD_LOCAL`), weak symbols, atomics (MSVC intrinsics vs
`__atomic_*` builtins), and POSIX compatibility mappings on Windows
(`mkdir`, `unlink`, `ssize_t`, `S_ISDIR`, etc.).

### C++ Capability Header

`src/common/PlatformCapabilities.hpp` includes the generated
`zanna/platform/Capabilities.hpp` header from the build tree. Use it in normal
C++ code instead of introducing new raw `_WIN32` / `__APPLE__` /
`__linux__` checks.

Important rule:

- raw host/compiler macros belong only in platform adapter files, build probes,
  or other low-level OS/toolchain boundaries
- shared code should use the repo’s capability headers and explicit capability
  names
- `scripts/lint_platform_policy.sh --strict` enforces this boundary
- existing raw-macro debt outside approved adapters is capped by
  `scripts/platform_policy_migration_baseline.txt`; when a file is migrated,
  lower or remove its baseline row

---

## Category Checklists

### 1. Filesystem

**When adding filesystem functionality, you must also update:**

| File | Reason |
|------|--------|
| `src/runtime/io/rt_file_io.c` | File open/read/write/seek — Windows uses `_open`/`_close`/`_read`/`_write`/`_lseeki64`; POSIX uses native APIs. Permission macros differ (`_S_IREAD` vs `S_IRUSR`). Missing errno values shimmed on Windows. |
| `src/runtime/io/rt_archive_fs.c` + `rt_archive_internal.h` | Approved archive filesystem adapter — UTF-8 path opening, exact reads, atomic write/rename, directory creation, and symlink/reparse-point rejection. |
| `src/runtime/io/rt_dir.c` | Directory listing/creation/removal — Windows uses `FindFirstFileA`/`FindNextFileA`/`_mkdir`/`_rmdir`; POSIX uses `opendir`/`readdir`/`mkdir`/`rmdir`. Path separator: `\\` on Windows, `/` on Unix. |
| `src/runtime/io/rt_watcher.c` | Filesystem watching — three backends selected with `RT_PLATFORM_*`: **Linux** (inotify + poll), **macOS** (kqueue + kevent), **Windows** (ReadDirectoryChangesW + overlapped I/O). |
| `src/runtime/rt_platform.h` | Path separator macro, POSIX compat shims for Windows, `mode_t` typedef. |

**[GAP]** Windows directory paths limited to `MAX_PATH` (260 chars); no Unicode long-path support.

---

### 2. Threading

**When adding threading functionality, you must also update:**

| File | Reason |
|------|--------|
| `src/runtime/threads/rt_threads_common.c`, `rt_threads_win.c`, `rt_threads_posix.c` | Shared SafeThread logic plus split native thread create/join/sync backends. Windows uses `CreateThread`/`CRITICAL_SECTION`/`CONDITION_VARIABLE`; POSIX uses `pthread_create`/`pthread_mutex_t`/`pthread_cond_t`. |
| `src/runtime/threads/rt_future.c`, `rt_parallel.c`, `rt_parallel_ops.c`, `rt_parallel_internal.h` | Higher-level thread primitives and data-parallel combinators use `RT_PLATFORM_*` branches for their native sync objects while sharing common task and error propagation logic. |
| `src/runtime/rt_platform.h` | TLS macro (`__declspec(thread)` vs `_Thread_local` vs `__thread`), atomic operations (MSVC `_InterlockedExchange*` vs GCC `__atomic_*`), memory barriers. |
| `src/runtime/core/rt_atomic_compat.h` | GCC/Clang `__atomic_*` builtin compatibility layer for MSVC — maps to Interlocked intrinsics via C11 `_Generic` dispatch. |
| `src/runtime/core/rt_context.c` | Per-thread context storage via `RT_THREAD_LOCAL`. |

---

### 3. Networking

**When adding networking functionality, you must also update:**

| File | Reason |
|------|--------|
| `src/runtime/network/rt_socket_platform.h` + `rt_socket_platform_win.c` / `rt_socket_platform_posix.c` | Approved socket adapter — native socket type, WinSock startup/cleanup, close/error helpers, SIGPIPE policy, non-blocking mode, socket timeouts, readiness waits, and queued-byte queries. |
| `src/runtime/network/rt_entropy_platform.h` + `rt_entropy_platform_win.c` / `rt_entropy_platform_posix.c` | Approved entropy adapter for crypto and DRBG seeding — BCrypt on Windows, `arc4random_buf` on macOS, `getrandom` then `/dev/urandom` on Linux/POSIX. |
| `src/runtime/network/rt_network.c`, `rt_tls.c`, `rt_netutils.c`, `rt_network_http.c` | Shared networking logic should call the socket/entropy adapters and use `RT_PLATFORM_*` only for non-adapter capability decisions. |
| `src/runtime/network/rt_tls_verify_win.c` / `rt_tls_verify_posix.c` | Certificate verification backend split — CryptoAPI trust store on Windows, native/common verifier path elsewhere. |
| `src/runtime/CMakeLists.txt` | Selects socket, entropy, and TLS verification backend sources per platform. |
| `CMakeLists.txt` | Feature flag `ZANNA_ENABLE_NETWORK` and `ZANNA_ENABLE_TLS`. |

---

### 4. Process / IPC

**When adding process or IPC functionality, you must also update:**

| File | Reason |
|------|--------|
| `src/runtime/system/rt_exec.c` | Runtime process execution — direct argv spawning on Windows/POSIX, shell mode only when explicitly requested. |
| `src/runtime/system/rt_machine.c` | System info — three separate implementations: Windows (`GetSystemInfo`, `GlobalMemoryStatusEx`, `GetComputerNameA`), macOS (`sysctl`, `host_statistics64`), Linux (`sysconf`, `sysinfo`). |
| `src/vm/VM.cpp` | Ctrl-C / interrupt handling — Windows: `SetConsoleCtrlHandler` + `windowsCtrlHandler`. POSIX: `sigaction(SIGINT)` + `posixSigintHandler` with `SA_RESTART`. |
| `src/tests/common/PosixCompat.h` | Test compatibility layer — Windows stubs for `fork()` (returns -1), `waitpid()`, `pipe()` → `_pipe()`, `usleep()` → `Sleep()`, `mkstemp()` → `_mktemp_s()`. Uses `SKIP_TEST_NO_FORK()` macro. |
| `src/common/RunProcess.cpp` | Tool/common subprocess launcher — native argv-based spawning with separate stdout/stderr capture; shell mode must be explicit via `run_shell_command()`. |

~~**[GAP]** Windows has no `fork()`.~~ **Resolved:** Tests use `ProcessIsolation` (CreateProcess self-relaunch + Job Object on Windows, fork on POSIX). See `src/tests/common/ProcessIsolation.hpp`.

---

### 5. Native Codegen

**When adding codegen functionality, you must also update:**

| File | Reason |
|------|--------|
| `src/codegen/x86_64/CodegenPipeline.cpp` | Compiler invocation — Windows: `clang`, `.exe` extension, direct return code. Unix: `cc`, `.out` extension, `WIFEXITED`/`WEXITSTATUS` from `<sys/wait.h>`. |
| `src/codegen/x86_64/Backend.cpp` | Symbol naming — Linux requires different relocation patterns than macOS Mach-O. |
| `src/codegen/x86_64/AsmEmitter.cpp` | Assembly emission — Mach-O symbol prefixes (`_` on macOS), ELF on Linux. |
| `src/codegen/x86_64/FrameLowering.cpp` | Stack frame layout — SysV AMD64 ABI (all Unix). Windows x86-64 ABI differs in shadow space and red zone. |
| `src/codegen/x86_64/CallLowering.cpp` | Calling convention — SysV: rdi/rsi/rdx/rcx/r8/r9 for integer args. Win64: rcx/rdx/r8/r9 (different order, shadow space). |
| `src/codegen/aarch64/FrameBuilder.cpp` | AArch64 AAPCS64 stack frame — 16-byte SP alignment, x29/x30 pair, callee-saved x19-x28/d8-d15. |
| `src/codegen/aarch64/RodataPool.cpp` | Mach-O rodata pool handling (macOS-specific addressing). |
| `src/codegen/common/LinkerSupport.cpp` | Archive naming (`.lib` vs `lib*.a`), framework flags on macOS, X11 on Linux, user32/gdi32 on Windows. |

~~**[GAP]** x86-64 codegen currently uses SysV ABI only.~~ **Resolved:** Both SysV and Win64 ABIs are implemented. `hostTarget()` selects the correct ABI automatically.

---

### 6. Graphics / Input

**When adding graphics or input functionality, you must also update:**

| File | Reason |
|------|--------|
| `src/lib/graphics/src/vgfx_platform_macos.m` | macOS backend — Cocoa: NSWindow, NSView, CGImage, mach_absolute_time. Objective-C. |
| `src/lib/graphics/src/vgfx_platform_linux.c` | Linux backend — X11: XOpenDisplay, XCreateWindow, XImage, XPutImage, KeySym. |
| `src/lib/graphics/src/vgfx_platform_win32.c` | Windows backend — Win32 GDI: HWND, HDC, DIB sections, BitBlt, CreateWindowEx, VK_* keycodes. |
| `src/lib/graphics/CMakeLists.txt` | Platform source selection and library linking: `-framework Cocoa` (macOS), X11 (Linux), `user32`/`gdi32` (Windows). FATAL_ERROR on unknown platform. |
| `src/lib/gui/CMakeLists.txt` | GUI additions — macOS: `vg_filedialog_native.m` (native file dialogs), `-framework UniformTypeIdentifiers`, `-framework ImageIO` for image loading. Linux: `-lm`, `_GNU_SOURCE`. |
| `src/runtime/core/rt_term.c` | Terminal input — Windows: `<conio.h>`, `_kbhit()`, `_getch()`, `ENABLE_VIRTUAL_TERMINAL_PROCESSING`. POSIX: `<termios.h>`, `select()` with zero timeout, raw mode caching. |
| `src/repl/ReplLineEditor.cpp` | REPL terminal width — Windows: `GetConsoleScreenBufferInfo()`. POSIX: `ioctl(TIOCGWINSZ)`. Raw I/O: Windows `WriteConsoleA()` vs POSIX `::write()`. |

Linux graphics availability is now controlled by `ZANNA_GRAPHICS_MODE=AUTO|REQUIRE|OFF`. Missing X11 in `REQUIRE` mode fails configure; `AUTO` reports the disabled feature explicitly in the capability summary.

---

### 7. Audio

**When adding audio functionality, you must also update:**

| File | Reason |
|------|--------|
| `src/lib/audio/src/vaud_platform_macos.m` | macOS backend — AudioQueue: AudioQueueRef, AudioStreamBasicDescription. Objective-C. |
| `src/lib/audio/src/vaud_platform_linux.c` | Linux backend — ALSA: snd_pcm_t, snd_pcm_writei, pthread for audio thread. |
| `src/lib/audio/src/vaud_platform_win32.c` | Windows backend — WASAPI: IMMDevice, IAudioClient, IAudioRenderClient, COM initialization. |
| `src/lib/audio/CMakeLists.txt` | Platform source selection: `-framework AudioToolbox` (macOS), ALSA (Linux), `ole32` (Windows). FATAL_ERROR on unknown platform. |
| `src/runtime/audio/rt_audio.c` | Runtime bridge — delegates to ZannaAUD. Gated by `ZANNA_ENABLE_AUDIO`; stubs provided when disabled. |

Linux audio availability is now controlled by `ZANNA_AUDIO_MODE=AUTO|REQUIRE|OFF`. Missing ALSA in `REQUIRE` mode fails configure; `AUTO` reports the disabled feature explicitly in the capability summary.

---

### 8. Time / Clocks

**When adding time or timer functionality, you must also update:**

| File | Reason |
|------|--------|
| `src/runtime/core/rt_time.c` | Sleep and monotonic clocks — Windows: `Sleep()` (10-15ms resolution), `QueryPerformanceCounter()` with `GetTickCount64()` fallback. POSIX: `nanosleep()` with EINTR retry, `clock_gettime(CLOCK_MONOTONIC)` with `CLOCK_REALTIME` fallback. |
| `src/runtime/rt_platform.h` | Windows time helpers: `rt_windows_time_ms()`, `rt_windows_time_us()`, `rt_windows_sleep_ms()`. Thread-safe `rt_localtime_r()` / `rt_gmtime_r()` / `rt_strtok_r()` wrappers. |

---

### 9. Installer / Packaging

**When adding packaging or installation functionality, you must also update:**

| File | Reason |
|------|--------|
| `CMakeLists.txt`, `src/CMakeLists.txt` | Installed toolchain layout, exported targets, generated package config, public runtime archives, man pages, docs, and staged ship-set completeness all flow from the install rules. |
| `src/tools/zanna/cmd_install_package.cpp` | Canonical CLI for packaging the staged Zanna toolchain. Any new format or verification rule should surface here. |
| `src/tools/common/packaging/ToolchainInstallManifest.*` | Shared manifest and install-path mapping for Windows/macOS/Linux installers. This is the single source of truth for staged file selection. |
| `src/tools/common/packaging/WindowsPackageBuilder.*`, `MacOSPackageBuilder.*`, `LinuxPackageBuilder.*` | Platform-specific installer writers and payload layout policy. Keep staged-relative layout stable across all three. |
| `src/tools/common/packaging/PkgVerify.*` | Structural verification for produced installer artifacts. Extend this rather than adding format-specific one-off verification scripts. |
| `scripts/build_zanna_unix.sh`, `scripts/build_zanna_mac.sh`, `scripts/build_zanna_linux.sh`, `scripts/build_zanna_win.ps1`, `scripts/build_installer.sh`, `scripts/build_installer.ps1` | Canonical build/test/install and toolchain-packaging entry points. Shared env vars: `ZANNA_BUILD_DIR`, `ZANNA_BUILD_TYPE`, `ZANNA_FAST_DEBUG`, `ZANNA_JOBS`, `ZANNA_CTEST_JOBS`, `ZANNA_RUN_SLOW_TESTS`, `ZANNA_SKIP_INSTALL`, `ZANNA_SKIP_LINT`, `ZANNA_SKIP_AUDIT`, `ZANNA_SKIP_SMOKE`, `ZANNA_CMAKE_GENERATOR`, `ZANNA_EXTRA_CMAKE_ARGS`. |

---

### 10. Build System / Compiler

**When adding build configuration, you must also update:**

| File | Reason |
|------|--------|
| `CMakeLists.txt` | Compiler flags — MSVC: `/FS`, `/utf-8`, `/W4`, `/permissive-`. GCC/Clang: `-Wall -Wextra -Wpedantic`, sanitizers, LTO. Symbol visibility (`-fvisibility=hidden`) on non-Windows. LLD linker gated on `IL_USE_LLD`. Apple-specific: suppress ld64 warnings, ARM64 cross-compilation flags. |
| `src/runtime/CMakeLists.txt` | Feature definitions — `_POSIX_C_SOURCE=200809L` on non-Windows, `_DEFAULT_SOURCE` on Linux (not macOS), `_GNU_SOURCE` on Linux. MSVC link optimization `/OPT:REF /OPT:ICF`. macOS frameworks: IOKit, CoreFoundation, Security. Runtime component archives are exported for generated manifest consumers. |
| `src/tests/CMakeLists.txt` | Test gating — ARM64 tests enabled only on `arm64\|aarch64\|ARM64` processors. Windows tests add `WinDialogSuppress.c` to suppress crash dialogs. |
| `scripts/lint_platform_policy.sh` | Advisory/strict lint for raw host macro usage outside approved adapter files plus required touchpoint notes for high-risk chokepoints. |
| `scripts/run_cross_platform_smoke.sh` | Host-capability smoke slice: core smoke tests, disabled-surface coverage, planner smoke, and display-bound probes when a display is available. |
| `scripts/audit_runtime_surface.sh` | Runtime surface audit plus disabled graphics/audio link-surface checks. |

---

## Platform Gap Summary

| ID | Category | Gap Description | Severity |
|----|----------|----------------|----------|
| GAP-2 | Filesystem | Windows `MAX_PATH` (260 char) limit on directory operations | Medium |
| ~~GAP-3~~ | ~~Process~~ | ~~Resolved: Windows uses CreateProcess self-relaunch + Job Object~~ | ~~Resolved~~ |
| ~~GAP-4~~ | ~~Codegen~~ | ~~Resolved: Both SysV and Win64 ABIs now implemented~~ | ~~Resolved~~ |
| GAP-5 | Graphics | Linux requires X11 dev headers; configure now fails in `REQUIRE` mode and reports explicitly in `AUTO` mode | Low |
| GAP-6 | Audio | Linux requires ALSA dev headers; configure now fails in `REQUIRE` mode and reports explicitly in `AUTO` mode | Low |

---

## Quick-Reference: Platform Backend Files

For features that require a per-platform implementation file, follow this
pattern (used by graphics and audio):

```text
src/lib/<feature>/
  CMakeLists.txt          # if(APPLE) / elseif(UNIX) / elseif(WIN32) / else FATAL_ERROR
  src/
    <feature>_common.c    # Shared logic
    <feature>_platform_macos.m    # Cocoa / CoreAudio (Objective-C)
    <feature>_platform_linux.c    # X11 / ALSA
    <feature>_platform_win32.c    # Win32 GDI / WASAPI
  include/
    <feature>.h           # Public API (platform-agnostic)
```

For features that use platform branches within a single runtime file, use
`rt_platform.h` macros rather than raw host macros:

```text
#if RT_PLATFORM_WINDOWS
    // Windows implementation
#elif RT_PLATFORM_MACOS
    // macOS-specific (kqueue, mach_*, etc.)
#else
    // POSIX fallback (Linux)
#endif
```

---

## Build Dependencies by Platform

### macOS
- Xcode (Apple Clang + Objective-C)
- Frameworks: Cocoa, AudioToolbox, IOKit, CoreFoundation, Security, UniformTypeIdentifiers

### Linux (Debian/Ubuntu)
- `build-essential`, `cmake`, `clang` or `gcc`
- `libx11-dev` (graphics)
- `libasound2-dev` (audio)
- No OpenSSL/libssl dependency is required for TLS

### Linux (RedHat/Fedora)
- `gcc`, `cmake`, `make`
- `libX11-devel` (graphics)
- `alsa-lib-devel` (audio)
- No OpenSSL dependency is required for TLS

### Windows
- MSVC or Clang-CL
- CMake
- Linked at build: `ws2_32`, `Xinput9_1_0`, `ole32`, `user32`, `gdi32`
