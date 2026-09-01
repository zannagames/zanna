---
status: active
audience: contributors
last-verified: 2026-09-01
---

# Linux Platform Implementation

Zanna's Linux support is split across small native adapters rather than hidden behind a third-party
portability library. The principal adapters are native Wayland and X11/GLX graphics, ALSA audio, inotify file
watching, Linux/POSIX pseudo-terminals, Linux machine-information probes, and ELF native linking.

## Process-global library state

The X11 backend initializes Xlib threading with `pthread_once` before opening its first display.
Fully constructed windows are published to clipboard and cursor services only after all native
resources have been created. Global cursor and clipboard operations retain the backend lifecycle
lock while using a selected window, so destruction cannot invalidate the selection concurrently.

GLX and XInput2 are loaded once with local, immediate symbol binding. Their shared-object handles
remain loaded for the process lifetime: unloading either library while Xlib or a driver retains
callbacks into it is unsafe. XInput extension opcodes remain display-local because different X
server connections are not required to assign the same opcode.

`ZANNA_GRAPHICS_BACKEND` is a **CMake configure option** (`-DZANNA_GRAPHICS_BACKEND=...`), not a
runtime environment variable. Its accepted values are `AUTO` (the default), `NATIVE`, `WAYLAND`,
`X11`, and `HEADLESS`. `HEADLESS` selects the dependency-free in-memory framebuffer backend while
keeping the public graphics surface enabled. `WAYLAND` and Linux `NATIVE` select native Wayland;
`X11` selects Xlib. `AUTO` namespaces both adapters into one archive and chooses at **run time**:
it prefers a usable compositor when `WAYLAND_DISPLAY` is set, and falls back to X11 when `DISPLAY`
is usable. The first successful
window fixes the adapter for the process. If X11 development files are absent, `AUTO` remains a
valid Wayland-only build. A failed desktop connection never silently becomes an invisible headless
application. See [ADR 0139](adr/0139-native-wayland-backend-and-linux-runtime-selection.md).

## Native Wayland capability status

The Wayland adapter has no build-time Wayland or xkbcommon dependency. It resolves the stable
client ABI, xkbcommon, and cursor-theme ABI dynamically and carries the protocol metadata it uses
in the repository. Its required compositor globals are `wl_compositor`, `wl_shm`, `wl_seat`, and
stable `xdg_wm_base`.

The currently implemented path includes xdg-toplevel creation and state, release-tracked
double-buffered SHM presentation paced by compositor frame callbacks, pointer/keyboard/touch seat
input, layout-aware keyboard text and repeat, clipboard text, URI-list drag and drop, text-input-v3
composition, themed cursors, output hotplug and integer scaling, viewporter/fractional scaling,
server-side decoration negotiation, a built-in subsurface client-decoration fallback, and native
relative mouse through relative-pointer-v1 plus pointer-constraints-v1. The fallback keeps the
application surface and framebuffer content-only while supplying move, edge/corner resize,
minimize, maximize/restore, and close controls. Each extension is optional: missing extensions
reduce only the associated capability, while missing required globals fail window creation with a
stable Wayland diagnostic. A compositor requesting client decorations must advertise the core
`wl_subcompositor` global so Zanna can provide the fallback frame.

Wayland intentionally does not implement global window positioning, focus stealing, or cursor
warping. Position reads return the documented unavailable result, and raw relative mode reports
unsupported unless both relative-pointer and pointer-constraints are present. User-authorized
foreground requests use `xdg_activation_v1` and a recent seat serial.

Canvas3D uses dynamically loaded EGL and `libwayland-egl` for native OpenGL presentation. The
renderer shares its shader/resource implementation with GLX while context creation, make-current,
swap, resize, and teardown are binding-neutral. Wayland EGL swaps use interval zero because Zanna
owns dispatch on the shared `wl_display`; the compositor still schedules scanout, while explicit
tearing control would require a separate optional protocol. If EGL, OpenGL 3.3, or required driver
limits are unavailable, Canvas3D falls back to its software renderer. Runtime `AUTO` selection is
covered by live Wayland-preference and failed-Wayland-to-X11 tests.

The OpenGL material pipeline supports the GL 3.3 minimum of 16 fragment texture units. Common
unlit draws use a specialized program containing texture transforms, terrain splats, emissive and
alpha/soft-particle inputs, fog, skinning/morph inputs, and motion output without activating the
larger PBR sampler set. Streamed `Pixels` textures preserve Zanna's top-left row and UV convention;
the OpenGL upload does not apply a second vertical inversion. The live EGL test plus the
`graphics3d` label exercise context creation, textured pixel output, view-model depth isolation,
terrain, render targets, post-processing, and the broader renderer contracts.

`vgfx_get_window_capabilities()` reports native behavior per live window. Wayland does not claim
global position or cursor-warp support; composition, fractional scaling, server-side decorations,
relative input, and activation reflect the compositor globals actually bound by the window.

Desktop appearance and accessibility preferences use the Settings portal when GIO is available,
with a 250 ms bounded call and conventional GTK/Qt environment overrides. Native Wayland never
opens X11 for these queries. The adapter recognizes the portal color-scheme and contrast settings,
plus the desktop-interface animation preference used for reduced motion. Linux AT-SPI projection
now establishes a dynamically loaded GIO connection to the dedicated accessibility bus, exports
the required Application/Accessible root interfaces, and completes the registry `Socket.Embed`
handshake on a private main context. Visible semantic widgets are exported under stable object
paths with parent/child traversal, role and state mapping, Component bounds/hit testing, readable
Action metadata, Unicode Text content/selections and boundary/granularity queries, and Value ranges.
The `/org/a11y/atspi/cache` object supports bulk tree discovery. Action, caret, and Value mutations
cross a bounded request queue and execute on the GUI thread. Widget notifications emit native
visible-data events, and explicit live-region announcements emit AT-SPI `Announcement` events.
Tree mutations are coalesced until layout/render synchronization so setup code does not reconnect
once per widget. Text glyph extents are currently estimated from widget geometry because the
software text renderer does not retain a shaped glyph-to-character map.

The ALSA backend does not replace `snd_lib_error_set_handler`. That handler is process-global and
ALSA provides no operation for retrieving and restoring an embedding application's previous
handler. Applications that want to redirect ALSA diagnostics should install their handler before
creating Zanna audio contexts.

## File watcher behavior

Linux watchers drain nonblocking inotify data until `EAGAIN`. Create, delete, move, write-close,
content-modification, and attribute changes are translated into the portable watcher event set.
Malformed records, native queue overflow, unmount, a removed watch, and descriptor error/hangup
conditions emit an overflow event requesting a caller rescan. Terminal conditions also make
`IsWatching` false; already queued events remain readable after that transition.

Watcher creation never changes the process-wide `RLIMIT_NOFILE`. Resource exhaustion leaves the
watcher inactive so the caller can report the condition or use periodic rescans. Operators should
configure descriptor and inotify limits outside the process when workloads require more watches.

## Machine information in containers

Linux CPU reporting caps the online host CPU count with cgroup v2 `cpu.max` and
`cpuset.cpus.effective` constraints when available. Total and available memory are similarly capped
by `memory.max` and `memory.current`. Missing, malformed, or unlimited cgroup controls fall back to
the host probes. Arithmetic saturates rather than wrapping when kernel counters exceed `int64`.

Account lookup uses `getpwuid_r`. Empty environment variables are treated as absent, and temporary
directory overrides must name an existing absolute directory.

## Pseudo-terminal lifecycle

On Linux the PTY startup-status pipe is created atomically with `O_CLOEXEC`. Every controlling-TTY,
window-size, and standard-stream setup operation is checked in the child and reported to the parent
before `exec`. Status messages handle interruption and partial transfer. PTY destruction signals the
child session so descendants do not survive merely because the immediate shell spawned them.

## Validation

Run targeted Linux platform tests after an incremental build:

```sh
ctest --test-dir build -R 'test_rt_(watcher|machine|open_load_result_apis)' --output-on-failure
ctest --test-dir build -R 'test_vaud_(audit_fixes|core_fixes)' --output-on-failure
ctest --test-dir build -R 'test_(window|pixels|input)' --output-on-failure
ctest --test-dir build -R 'test_(wayland_loader|linux_auto_)' --output-on-failure
ctest --test-dir build-wayland -R 'test_wayland_(backend|egl)' --output-on-failure
ctest --test-dir build -L graphics3d --output-on-failure
ctest --test-dir build -R linux_headless_graphics_smoke --output-on-failure
./scripts/lint_platform_policy.sh
```

Before proposing changes, run the complete Linux build and test pipeline with
`./scripts/build_zanna_linux.sh`.
