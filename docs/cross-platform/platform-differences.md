---
status: active
audience: public
last-verified: 2026-07-26
---

# Platform Behavioral Differences

This document describes the intentional and incidental behavioral differences in the Zanna compiler and runtime across **Windows (x86-64)**, **macOS (ARM64 / Apple Silicon)**, and **Linux (x86-64 and AArch64)**. It is intended for developers and advanced users who need to understand what Zanna does differently per platform at runtime.

For a contributor-oriented checklist of which source files to modify when adding platform-sensitive code, see [platform-checklist.md](platform-checklist.md).

---

## Summary Table

| Feature Area | Windows | macOS | Linux |
|---|---|---|---|
| Core Runtime (VM, IL, Zia) | ✅ Full | ✅ Full | ✅ Full |
| Terminal I/O | ✅ Full | ✅ Full | ✅ Full |
| Filesystem I/O | ⚠️ Partial [1] | ✅ Full | ✅ Full |
| File Watching | ✅ Full | ✅ Full | ✅ Full |
| Networking (TCP/UDP/HTTP) | ✅ Full | ✅ Full | ✅ Full |
| TLS/SSL | ✅ Full (Schannel + in-tree handshake) | ✅ Full (in-tree TLS/X.509 runtime) | ✅ Full (in-tree TLS/X.509 runtime) |
| Threading | ✅ Full | ✅ Full | ✅ Full |
| Graphics | ✅ Full (Win32 GDI) | ✅ Full (Cocoa) | ✅ Full (Wayland + X11) [2] |
| Audio | ✅ Full (WASAPI) | ✅ Full (AudioQueue) | ⚠️ Partial [3] |
| Game Controllers | ✅ Full (XInput) | ⚠️ Partial [4] (IOKit HID) | ✅ Full (evdev) |
| Native Codegen (x86-64) | ✅ Full | ❌ Not supported [5] | ✅ Full |
| Native Codegen (AArch64) | ⚠️ Partial [6] | ✅ Full | ✅ Full |
| Process Execution | ✅ Full | ✅ Full | ✅ Full |

**Footnotes:**

1. Windows directory operations are limited to `MAX_PATH` (260 characters). No Unicode long-path support.
2. The default Linux build (`ZANNA_GRAPHICS_BACKEND=AUTO`) always includes the native Wayland adapter, which resolves the Wayland, xkbcommon, cursor, and EGL ABIs at runtime and needs no development packages. The X11 fallback adapter is added only when `find_package(X11)` succeeds at configure time; without it, `AUTO` is a valid Wayland-only build.
3. Linux audio requires ALSA development headers (`libasound2-dev` / `alsa-lib-devel`) at configure time. Without them CMake reports `ZannaAUD: disabled` and the audio library is omitted; `-DZANNA_AUDIO_MODE=REQUIRE` turns that into a configure error instead.
4. macOS gamepad vibration is a no-op. The generic IOKit HID interface exposes no force-feedback output reports, so rumble would require vendor-specific extensions. Buttons, axes, and hot-plug detection are fully supported.
5. macOS is supported on Apple Silicon (ARM64) only. Intel (x86-64) macOS is not a supported target: the native linker does not implement Mach-O x86-64 dynamic imports, and the x86-64 native-link backend is not built on macOS hosts.
6. The AArch64 Windows PE/COFF target is defined but not exercised in CI.

---

## 1. Runtime Library (Zanna.\* Namespace)

The Zanna runtime presents a uniform API across platforms. Underneath, each module dispatches to platform-native implementations. The subsections below document where behavior diverges.

### 1.1 Terminal I/O

The `Zanna.Terminal` module (`Say`, `Print`, `ReadKey`, etc.) is functionally equivalent across platforms, but the underlying implementation differs.

| Aspect | Windows | macOS / Linux |
|--------|---------|---------------|
| Key input | `_kbhit()` / `_getch()` via `<conio.h>` | `termios` raw mode + `select()` with zero timeout |
| ANSI escape support | Enabled at startup via `SetConsoleMode(ENABLE_VIRTUAL_TERMINAL_PROCESSING)` | Native — no setup required |
| Terminal width query | `GetConsoleScreenBufferInfo()` | `ioctl(TIOCGWINSZ)` |
| Blocking key wait | `WaitForSingleObject()` with timeout on stdin | `select()` with timeout on fd 0 |
| Beep | Optional `Beep()` WinAPI (controlled by `ZANNA_BEEP_WINAPI` env var) | Terminal bell character (`\a`) |

**User-visible difference:** On older Windows terminals (pre-Windows 10 1607), ANSI escape sequences may not render correctly. The runtime enables VT processing automatically, but third-party terminal emulators may behave differently.

### 1.2 System Information (Zanna.System.Machine)

The `Zanna.System.Machine` module returns platform-specific values for several queries.

| Property | Windows | macOS | Linux |
|----------|---------|-------|-------|
| `Machine.Os` | `"windows"` | `"macos"` | `"linux"` |
| `Machine.OsVer` | `GetVersionExA()` → e.g. `"10.0.19045"` | `sysctlbyname("kern.osproductversion")` → e.g. `"14.2.1"` | `VERSION_ID` from `/etc/os-release` → e.g. `"22.04"` |
| `Machine.Host` | `GetComputerNameA()` | `gethostname()` | `gethostname()` |
| `Machine.User` | `GetUserNameA()` with SID fallback | `getpwuid(getuid())` | `getpwuid()` with `$USER` env fallback |
| `Machine.Home` | `%USERPROFILE%` (e.g. `C:\Users\alice`) | `$HOME` or `/var/root` | `$HOME` or `getpwuid()` fallback |
| `Machine.Temp` | `GetTempPathA()` (e.g. `C:\Users\alice\AppData\Local\Temp`) | `$TMPDIR` or `/tmp` | `$TMPDIR` or `/tmp` |
| `Machine.Cores` | Processor-group-aware Win32 query | `sysctlbyname("hw.logicalcpu")` | Online CPUs constrained by active cgroup v1/v2 quota and cpuset |
| `Machine.MemTotal` | `GlobalMemoryStatusEx()` | `sysctl(HW_MEMSIZE)` | `sysinfo()` total constrained by the active cgroup memory limit |
| `Machine.MemFree` | `GlobalMemoryStatusEx().ullAvailPhys` | `host_statistics64()` free + inactive pages | `/proc/meminfo` `MemAvailable`, constrained by active cgroup usage |

**User-visible differences:**

- `OsVer` format varies significantly between platforms. Do not parse the string for version comparison — use `Machine.Os` for platform detection.
- `Home` uses `%USERPROFILE%` on Windows (typically `C:\Users\<name>`) vs `$HOME` on Unix (typically `/home/<name>` or `/Users/<name>`). The path separator in the returned string matches the platform convention.
- `Temp` returns a platform-native path. On Windows this often includes the user's AppData directory; on Unix it is typically `/tmp` unless `$TMPDIR` is set.
- Linux resolves cgroup mounts through `/proc/self/mountinfo` and combines their
  mount roots with `/proc/self/cgroup`. Relocated cgroup v2 hierarchies, nested
  container memberships, and hybrid v1 cpu/cpuset/memory mounts are therefore
  reflected in `Cores`, `MemTotal`, and `MemFree`.

### 1.3 File Watching

The `Zanna.IO.Watcher` class uses three completely separate backends, but presents a unified API.

| Aspect | Windows | macOS | Linux |
|--------|---------|-------|-------|
| Backend | `ReadDirectoryChangesW` + overlapped I/O | `kqueue` + `kevent` with `EVFILT_VNODE` | `inotify` + `poll()` |
| Event granularity | Per-file notifications via `FILE_NOTIFY_INFORMATION` | Per-directory `NOTE_WRITE` / `NOTE_DELETE` / `NOTE_RENAME` | Per-file `IN_CREATE` / `IN_DELETE` / `IN_MODIFY` / `IN_MOVED_*` |
| Filename in events | UTF-16 → UTF-8 conversion from `FILE_NOTIFY_INFORMATION.FileName` | Not provided by kqueue (directory-level only) | Provided in `inotify_event.name` |
| Cleanup | `CancelIoEx()` + `CloseHandle()` (`CancelIo` compatibility fallback) | `close(kqueue_fd)` + `close(watched_fd)` | `inotify_rm_watch()` + `close()` |

**User-visible difference:** On macOS, kqueue reports directory-level changes without identifying the specific file. The runtime works around this where possible, but some event details may be less granular than on Windows or Linux.

All backends expose the same 64-event bounded queue and overflow contract. Native/backend event loss is reported with an unknown count (`-1`); only overflow of Zanna's own ring has an exact positive count. A terminal macOS vnode event retires the watch because kqueue follows the inode rather than a recreated pathname. Calls on one Watcher instance require external serialization on every platform.

### 1.4 Temporary Files and Entropy

| Aspect | Windows | macOS / Linux |
|--------|---------|---------------|
| Random bytes | `BCryptGenRandom()` | macOS: `arc4random_buf()`; Linux: `getrandom()` |
| Fallback entropy | None | Linux/generic POSIX: close-on-exec `/dev/urandom` |
| Temp directory | `GetTempPathA()` with trailing `\` stripped | `$TMPDIR` or `/tmp` with trailing `/` stripped |

Both paths produce cryptographically unpredictable temp filenames. The fallback entropy path is only used if the primary source fails (extremely rare).

### 1.5 Process Execution

| Aspect | Windows | macOS / Linux |
|--------|---------|---------------|
| Shell command | `system()` dispatches to `cmd.exe /c` | `system()` dispatches to `/bin/sh -c` |
| Pipe open | `_popen()` / `_pclose()` | `popen()` / `pclose()` |
| Environment get | `GetEnvironmentVariableA()` | `getenv()` |
| Environment set | `SetEnvironmentVariableA()` | Not exposed (process-local `setenv()` only) |
| `fork()` | Not available — test infra uses `CreateProcess` self-relaunch instead | Available |

**User-visible difference:** Shell commands passed to `Zanna.System.Exec.Shell()` are interpreted by `cmd.exe` on Windows and `/bin/sh` on Unix. Shell syntax (pipes, redirects, quoting rules) differs between these interpreters.

### 1.6 Networking and TLS

The socket API is functionally equivalent, but initialization and error handling differ.

| Aspect | Windows | macOS | Linux |
|--------|---------|-------|-------|
| Socket init | `WSAStartup()` required (called automatically) | None | None |
| Socket close | `closesocket()` | `close()` | `close()` |
| Error retrieval | `WSAGetLastError()` | `errno` | `errno` |
| Non-blocking mode | `ioctlsocket(FIONBIO)` | `fcntl(F_SETFL, O_NONBLOCK)` | `fcntl(F_SETFL, O_NONBLOCK)` |
| SIGPIPE suppression | Not needed (no SIGPIPE on Windows) | `SO_NOSIGPIPE` socket option | `MSG_NOSIGNAL` flag per-send |
| Would-block error | `WSAEWOULDBLOCK` | `EAGAIN` | `EAGAIN` / `EWOULDBLOCK` |

**TLS/SSL backends:**

| Platform | Backend | Certificate Store |
|----------|---------|-------------------|
| Windows | Schannel via WinCrypt API | Windows Certificate Store (`CertOpenStore`) |
| macOS | Custom built-in TLS/X.509 runtime | Configured PEM bundle override or the system PEM bundle (`/etc/ssl/cert.pem` on current macOS releases) |
| Linux | Custom built-in TLS/X.509 runtime | Configured PEM bundle override or the system PEM bundle (typically `/etc/ssl/certs/...`) |

The macOS and Linux TLS implementations are entirely custom and built-in. TLS 1.3 handshake processing, certificate-chain parsing, hostname verification, RSA-PSS CertificateVerify, and HTTPS/WSS server signing all run in-tree without OpenSSL, Security.framework, LibreSSL, mbedTLS, or runtime `dlopen()` fallbacks.

All three backends validate server certificates against a trusted root source. Windows uses the platform certificate store; macOS and Linux use a PEM trust bundle discovered from standard system locations unless a PEM bundle override is configured by the caller.

### 1.7 Game Controller Input

| Aspect | Windows | macOS | Linux |
|--------|---------|-------|-------|
| Backend | XInput API | IOKit HID Manager | evdev (`/dev/input/event*`) |
| Button mapping | `XINPUT_GAMEPAD_*` constants | HID element parsing via `IOHIDDeviceGetValue` | Raw `BTN_*` / `ABS_*` event codes |
| Vibration/Rumble | ✅ `XInputSetState()` | ❌ Not supported | ✅ `FF_RUMBLE` effect via `EVIOCSFF` |
| Hot-plug detection | Automatic via XInput polling | `IOHIDManagerRegisterDeviceMatchingCallback` | Manual `/dev/input/` scanning |

**User-visible difference:** Vibration is unavailable on macOS — the generic IOKit
HID interface exposes no force-feedback output reports, so `Zanna.Input.Pad`
rumble calls are accepted and ignored. Windows drives both motors through
`XInputSetState`; Linux uploads an `FF_RUMBLE` effect whose strong and weak
magnitudes map to the left and right motors, and devices that do not advertise
`FF_RUMBLE` are also a no-op.

### 1.8 Graphics

| Aspect | Windows | macOS | Linux |
|--------|---------|-------|-------|
| Backend | Win32 GDI (DIB sections + `BitBlt`) | Cocoa (`NSWindow` + `NSView` + `CGImage`) | Native Wayland with runtime X11 fallback when available |
| Pixel format | BGRA via DIB section | BGRA via `CGBitmapContextCreate` | BGRA buffer → `wl_shm` XRGB (Wayland) or XImage (X11) |
| Keyboard input | `VK_*` virtual key codes | `NSEvent` key codes | `XLookupKeysym()` |
| Timer source | `QueryPerformanceCounter` | `mach_absolute_time` | `clock_gettime(CLOCK_MONOTONIC)` |
| Build dependency | Built-in (`user32`, `gdi32`) | Built-in (`-framework Cocoa`) | Wayland loaded dynamically; optional X11 fallback uses development headers |

**User-visible difference:** Linux AUTO builds prefer a non-empty
`WAYLAND_DISPLAY`, then fall back to a non-empty `DISPLAY` when the X11 adapter
was built. If neither display is usable, window creation reports a platform
error rather than selecting a backend implicitly.

### 1.9 Audio

| Aspect | Windows | macOS | Linux |
|--------|---------|-------|-------|
| Backend | WASAPI via COM (`IAudioClient`) | AudioQueue (`AudioQueueRef`) | ALSA (`snd_pcm_t`) |
| Threading model | Dedicated `HANDLE` thread + `WaitForMultipleObjects` | AudioQueue callback (OS-managed thread) | Dedicated `pthread_t` + mutex/cond |
| Synchronization | `CRITICAL_SECTION` | AudioQueue internal | `pthread_mutex_t` + `pthread_cond_t` |
| Build dependency | Built-in (`ole32`) | Built-in (`-framework AudioToolbox`) | Requires ALSA development headers at configure time |

**User-visible difference:** Unlike graphics — where the Wayland adapter is always
built — Linux audio is a genuine configure-time dependency. If ALSA development
headers are absent, CMake prints `ZannaAUD: disabled`, the audio library is
omitted, and audio functions are unavailable at runtime. Configure with
`-DZANNA_AUDIO_MODE=REQUIRE` to turn a missing ALSA into a hard configure error.

### 1.10 Numeric Parsing (Locale)

Float-to-string and string-to-float conversions use locale-independent formatting/parsing to ensure deterministic behavior, but the implementation varies.

| Platform | Method |
|----------|--------|
| Windows | Per-call `_create_locale()` with `_strtod_l()` / `_vsnprintf_l()` |
| macOS | `strtod()` / `vsnprintf()` with per-thread `uselocale()` guards |
| Linux | `strtod()` / `vsnprintf()` with per-thread `uselocale()` guards |

**User-visible difference:** None — all three paths produce identical results for well-formed numeric strings. The runtime guarantees that `Zanna.Text.Fmt.Num()` and the `Zanna.Core.Parse` numeric helpers are locale-independent and round-trip consistent for finite values emitted by `Fmt.Num`.

---

## 2. Native Codegen

Zanna compiles Zia programs to native machine code via its built-in code generator. The generated code differs by target architecture and operating system.

### 2.1 x86-64: SysV vs Win64 ABI

The x86-64 backend supports two ABIs. The pipeline selects the correct ABI automatically via `hostTarget()` — Win64 on Windows, SysV on Linux. (macOS x86-64 is not a supported target; macOS support is ARM64-only.)

| Property | SysV AMD64 (Linux) | Win64 (Windows) |
|----------|---------------------------|-----------------|
| Integer argument registers | 6: RDI, RSI, RDX, RCX, R8, R9 | 4: RCX, RDX, R8, R9 |
| FP argument registers | 8: XMM0–XMM7 | 4: XMM0–XMM3 |
| Callee-saved GPRs | 6: RBX, R12–R15, RBP | 8: RBX, RBP, RDI, RSI, R12–R15 |
| Callee-saved FPRs | None | 10: XMM6–XMM15 |
| Red zone | 128 bytes below RSP | None |
| Shadow space | None | 32 bytes above return address |
| Varargs | `%AL` must hold XMM count | Not required |
| Stack alignment | 16-byte aligned before `CALL` | 16-byte aligned before `CALL` |
| Return registers | RAX (int), XMM0 (float) | RAX (int), XMM0 (float) |

**Implication:** Native compilation produces executables that follow the platform's native calling convention — Win64 on Windows, SysV on Linux. The generated code and runtime share the same ABI on each platform.

### 2.2 AArch64: Darwin vs Linux vs Windows

The AArch64 backend uses the AAPCS64 calling convention on all three platforms. The register allocation, argument passing, and stack frame layout are **identical**. Differences are limited to assembly-level directives and symbol naming.

| Property | Darwin (macOS) | Linux | Windows |
|----------|----------------|-------|---------|
| Symbol prefix | `_` (underscore) | None | None |
| `.type` directive | Not emitted | `.type name, @function` | Not emitted |
| `.size` directive | Not emitted | `.size name, .-name` | Not emitted |
| Object format | Mach-O | ELF | PE/COFF |

Register convention (all platforms): 8 integer args (X0–X7), 8 FP args (V0–V7), callee-saved X19–X28 + V8–V15, 16-byte stack alignment.

### 2.3 Object Formats and Symbol Mangling

| Aspect | macOS | Linux | Windows |
|--------|-------|-------|---------|
| Object format | Mach-O | ELF | PE/COFF |
| Read-only data section | `.section __TEXT,__const` | `.section .rodata` | `.section .rdata` |
| Text section | `.section __TEXT,__code` | `.text` | `.text` |
| C symbol prefix | `_main`, `_rt_print` | `main`, `rt_print` | `main`, `rt_print` |
| Alignment syntax | `.align 2` (power-of-2) | `.align 8` (bytes) | `.align 8` (bytes) |

### 2.4 Linker Integration

The native codegen pipeline uses the built-in native linker to produce the final executable on every supported host path. The host assembler may still be used for `.s -> .o` when explicitly requested, but final executable linking does not fall back to `cc` or `ld`.

| Aspect | Windows | macOS / Linux |
|--------|---------|---------------|
| Library naming | `zanna_rt_core.lib` | `libzanna_rt_core.a` |
| Graphics library | `zannagfx.lib` + `user32` + `gdi32` | `libzannagfx.a` + `-framework Cocoa` (macOS); on Linux `libX11.so.6` is added to the import plan only when the program actually references X11 symbols, and the Wayland adapter is resolved at runtime with no import entry. GUI image loading also imports `ImageIO.framework` on macOS |
| Audio library | `zannaaud.lib` + `ole32` | `libzannaaud.a` + `-framework AudioToolbox` (macOS) or `-lasound` (Linux) |
| Network library | `ws2_32.lib` | (system sockets, no extra lib) |
| Final link driver | Native PE writer + import metadata | Native Mach-O / ELF writers + import metadata |
| Executable naming | `.exe` extension | `.out`-style host convention |

---

## 3. Filesystem Path Handling

### Path Separators

| Platform | Separator | Macro |
|----------|-----------|-------|
| Windows | `\` (backslash) | `RT_PATH_SEPARATOR = '\\'` |
| macOS | `/` (forward slash) | `RT_PATH_SEPARATOR = '/'` |
| Linux | `/` (forward slash) | `RT_PATH_SEPARATOR = '/'` |

The runtime's path functions (`Zanna.Path.*`) normalize paths using the platform's native separator. Windows additionally recognizes forward slashes in most contexts, but returned paths always use backslashes.

### Case Sensitivity

| Platform | Default Filesystem | Behavior |
|----------|-------------------|----------|
| Windows | NTFS | Case-insensitive (case-preserving) |
| macOS | APFS / HFS+ | Case-insensitive (case-preserving) by default |
| Linux | ext4 / XFS | Case-sensitive |

**User-visible difference:** A file created as `Hello.zia` can be opened as `hello.zia` on Windows and macOS (default), but not on Linux. Write portable code by matching filename case exactly.

### Home Directory Resolution

| Platform | Source | Typical Value |
|----------|--------|---------------|
| Windows | `%USERPROFILE%` environment variable | `C:\Users\alice` |
| macOS | `$HOME` env, fallback to `/var/root` if root | `/Users/alice` |
| Linux | `$HOME` env, fallback to `getpwuid()` | `/home/alice` |

### Temporary Directory Resolution

| Platform | Source | Typical Value |
|----------|--------|---------------|
| Windows | `GetTempPathA()` | `C:\Users\alice\AppData\Local\Temp` |
| macOS | `$TMPDIR` env, fallback to `/tmp` | `/var/folders/xx/.../T` (randomized) |
| Linux | `$TMPDIR` env, fallback to `/tmp` | `/tmp` |

### Maximum Path Length

| Platform | Limit | Constant |
|----------|-------|----------|
| Windows | 260 characters | `MAX_PATH` |
| macOS | 1024 characters | `PATH_MAX` |
| Linux | 4096 characters | `PATH_MAX` |

**User-visible difference:** Windows directory operations will fail for paths exceeding 260 characters. This is a known limitation (GAP-2). macOS and Linux support significantly longer paths.

---

## 4. Threading and Synchronization

The `Zanna.Threads` module provides a uniform threading API. The underlying primitives differ by platform.

### Thread Creation and Synchronization

| Primitive | Windows | macOS / Linux |
|-----------|---------|---------------|
| Thread create | `CreateThread()` | `pthread_create()` |
| Mutex | `CRITICAL_SECTION` | `pthread_mutex_t` |
| Condition variable | `CONDITION_VARIABLE` | `pthread_cond_t` |
| Thread join signaling | `WakeAllConditionVariable()` | `pthread_cond_signal()` |
| Atomic thread ID | `InterlockedIncrement64()` | `__atomic_fetch_add()` |

### Thread-Local Storage

| Compiler | Keyword |
|----------|---------|
| MSVC | `__declspec(thread)` |
| C11-compliant (GCC/Clang) | `_Thread_local` |
| Pre-C11 GCC/Clang | `__thread` |

The runtime abstracts this via the `RT_THREAD_LOCAL` macro.

### Atomic Operations

| Operation | MSVC (Windows) | GCC / Clang (macOS / Linux) |
|-----------|---------------|----------------------------|
| Exchange | `_InterlockedExchange()` | `__atomic_exchange_n()` |
| Compare-and-swap | `_InterlockedCompareExchange()` | `__atomic_compare_exchange_n()` |
| Fetch-and-add | `_InterlockedExchangeAdd()` | `__atomic_fetch_add()` |
| Memory fence | `_mm_mfence()` (x86) / `__dmb()` (ARM64) | `__atomic_thread_fence()` |

### CPU Core Count Detection

| Platform | Method |
|----------|--------|
| Windows | `GetSystemInfo().dwNumberOfProcessors` |
| macOS | `sysctlbyname("hw.logicalcpu")` |
| Linux | `sysconf(_SC_NPROCESSORS_ONLN)` |

The parallel task pool (`Zanna.Parallel`) uses this to size its worker thread pool.

---

## 5. Timers and Clock Resolution

### Sleep

| Platform | API | Typical Resolution |
|----------|-----|-------------------|
| Windows | `Sleep(ms)` | ~10–15 ms (system timer granularity) |
| macOS | `nanosleep()` with EINTR retry | ~1 ms |
| Linux | `nanosleep()` with EINTR retry | ~1 ms (kernel `CONFIG_HZ` dependent) |

**User-visible difference:** `Zanna.Time.Sleep(1)` on Windows may sleep for up to 15 ms due to the default timer resolution. On macOS and Linux, the actual sleep duration is much closer to the requested value.

### Monotonic Clock

| Platform | Primary Source | Fallback | Precision |
|----------|---------------|----------|-----------|
| Windows | `QueryPerformanceCounter` | `GetTickCount64()` (~15 ms) | Sub-microsecond |
| macOS | `clock_gettime(CLOCK_MONOTONIC)` | `CLOCK_REALTIME` | Nanosecond |
| Linux | `clock_gettime(CLOCK_MONOTONIC)` | `CLOCK_REALTIME` | Nanosecond |

Used by `Zanna.Time.Timer()`, `Zanna.Time.ClockUs()`, and `Zanna.Stopwatch`.

### Wall-Clock Time

| Platform | API |
|----------|-----|
| Windows | `rt_windows_time_ms()` (internally `GetSystemTimeAsFileTime`) |
| macOS | `gettimeofday()` |
| Linux | `clock_gettime(CLOCK_REALTIME)` |

Used by `Zanna.DateTime.Now()`. All platforms return milliseconds since the Unix epoch. Results are consistent across platforms for the same wall-clock instant.

### Thread-Safe Time Formatting

| Function | Windows (MSVC) | macOS / Linux |
|----------|----------------|---------------|
| Local time | `localtime_s()` (args reversed) | `localtime_r()` |
| GMT time | `gmtime_s()` (args reversed) | `gmtime_r()` |
| String tokenize | `strtok_s()` | `strtok_r()` |

The runtime wraps these behind `rt_localtime_r()`, `rt_gmtime_r()`, and `rt_strtok_r()` for uniform usage.

---

## Known Gaps

These are known platform-specific limitations, tracked across the project.

| ID | Category | Description | Severity |
|----|----------|-------------|----------|
| GAP-2 | Filesystem | Windows `MAX_PATH` (260 char) limit on directory operations | Medium |
| GAP-5 | Graphics | The Linux X11 fallback adapter needs X11 development headers at configure time. `ZANNA_GRAPHICS_BACKEND=X11` fails without them; the default `AUTO` build reports the omission and continues Wayland-only | Low |
| GAP-6 | Audio | Linux audio needs ALSA development headers at configure time. `ZANNA_AUDIO_MODE=REQUIRE` fails without them; `AUTO` reports the omission and builds without audio | Low |
| GAP-7 | Input | macOS gamepads have no vibration path through the generic IOKit HID interface | Low |
| GAP-8 | Graphics3D | Point/omni-directional shadows are unavailable on Linux: the OpenGL backend is the only one without the `shadow_atlas_slots` hook, so `Canvas3D.BackendSupports("shadow-point")` reports false there (directional slots and primary CSM cascades work) | Medium |
| GAP-9 | Graphics3D | `Canvas3D.FrameGpuTimeUs` is implemented only by the D3D11 backend; it reads 0 on macOS and Linux | Low |
| GAP-10 | Graphics3D | The seven `Canvas3D.Backend*` statistics properties (`BackendDrawCalls`, `BackendDroppedDraws`, mesh-cache and streaming counters, `BackendPresentPath`) read 0 on macOS: the Metal backend does not implement `get_backend_stats` | Low |
| GAP-11 | Graphics3D | Windows-on-ARM64 defaults to the software rasterizer because several Windows-on-ARM GPU stacks crash inside the display driver during Present; opt in to the GPU with `ZANNA_3D_BACKEND=d3d11`. `Canvas3D.BackendFallback` reports only runtime fallback, not this policy default | Medium |
| GAP-12 | Graphics3D | The extended GPU-skinning path (`gpu_skinning_extras`) exists only on Metal; D3D11 and OpenGL take the reduced skinning path with no capability bit distinguishing them | Low |
| GAP-13 | Graphics3D | SSR, HDR scene color, and TAA require backend hooks the software rasterizer does not implement; the corresponding `Canvas3D` settings are silently unavailable when the portable backend is active | Low |

GAP-3 and GAP-4 are closed: Windows test infrastructure uses `CreateProcess`
self-relaunch plus a Job Object rather than `fork()`, and the x86-64 backend
implements both the System V AMD64 and Windows x64 ABIs.
