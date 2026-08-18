---
status: active
audience: contributors
last-verified: 2026-08-17
---

# Wayland Hardening Audit

This audit tracks the native Wayland implementation defined by
[ADR 0139](adr/0139-native-wayland-backend-and-linux-runtime-selection.md). It is a
living engineering checklist, not a replacement for the ADR or the public graphics API.

## Current support

| Area | Implementation | Automated coverage |
|---|---|---|
| Runtime selection | `AUTO` prefers Wayland and falls back to X11; explicit `WAYLAND` never falls back | Live AUTO preference, CSD, and X11 fallback tests |
| Core windowing | Stable xdg-shell toplevel, configure/ack, title, resize, maximize, fullscreen, minimize request, close | Live shell, SHM, public lifecycle, maximize/restore tests |
| Software presentation | Two release-tracked `wl_shm` buffers and compositor frame callbacks | Live two-frame SHM test |
| OpenGL | Dynamically loaded EGL and `libwayland-egl`, OpenGL 3.3 renderer integration | Live EGL creation and presentation test |
| Input | Pointer, keyboard/XKB, keyboard repeat, touch, relative pointer and constraints | Translation/repeat policy tests plus public live lifecycle |
| Text input | text-input-v3 preedit, commit, deletion, surrounding text and cursor rectangle | Bounded and invalid-offset surrounding-text tests |
| Data transfer | UTF-8 clipboard, bounded nonblocking pipes, URI-list file drop, DND copy negotiation | Clipboard lifecycle and URI parsing tests |
| Scaling | Output hotplug, integer scale, core preferred buffer scale, viewporter and fractional-scale-v1 | Integer/fractional policy and live capability tests |
| Decorations | xdg-decoration negotiation and built-in subsurface CSD fallback | Live server/CSD-forced AUTO tests |
| Desktop integration | xdg-activation, themed cursors, portal preferences, AT-SPI projection | Loader/capability tests and broader Linux platform tests |

The product has no link-time Wayland, xkbcommon, cursor, EGL, or libwayland-egl
dependency. Stable client entry points are loaded at runtime and the protocol metadata used by
Zanna is checked into the repository.

## Hardened in the 2026-07-23 pass

- The display event loop now waits for `POLLOUT` after a nonblocking flush fills the socket and
  treats display error/hangup descriptors as failures instead of successful idle polls.
- Capability queries now report successfully bound per-window clipboard, file-drop, composition,
  fractional-scale, and decoration objects rather than merely advertised globals.
- `wl_keyboard.enter` synchronizes the complete pressed-key snapshot, preventing stale key state
  after focus transitions.
- text-input-v3 clamps negative, oversized, and mid-codepoint cursor/anchor offsets before bounded
  surrounding-text copies.
- URI drops reject remote file authorities and percent-decoded NUL bytes.
- DND v3 negotiates the copy action before finishing an accepted drop.
- Core-v6 preferred buffer scale is honored, while fractional-scale-v1 remains authoritative when
  available.
- Every public cursor enum has a primary Xcursor theme mapping.
- EGL loader publication uses acquire/release atomics, removing a concurrent first-use data race.

## Remaining work

### Priority 0: failure and interoperability coverage

- Add a repository-controlled headless compositor fixture so CI can exercise compositor
  disconnect, resize storms, output hotplug, integer/fractional transitions, keyboard repeat,
  composition, cursor entry, relative pointer lock/unlock, DND, and EGL teardown without relying on
  a developer desktop.
- Run the matrix on Weston, Mutter, KWin, and one wlroots compositor. The current machine provides
  a live compositor, but that is not a substitute for the ADR's compositor matrix.
- Add protocol-error capture to live tests so a request rejected asynchronously by the compositor
  fails at the originating operation rather than at a later pump.

### Priority 1: behavior completeness

- Scale built-in client decorations and cursor themes for integer and fractional HiDPI. The main
  content surface scales correctly, but the fallback decoration and cursor assets are still
  allocated at scale 1.
- Add animated Xcursor frame scheduling. The current cursor backend presents the first image.
- Make process-global clipboard and cursor routing robust across multiple simultaneous Wayland
  windows, including destruction of the focused/most-recent window.
- Exercise and document multiple-seat policy. The current connection binds one advertised seat.
- Improve `wl_pointer` axis-frame aggregation and value120/discrete wheel handling while avoiding
  duplicate legacy-axis events.
- Make synchronous clipboard reads dispatch-safe for providers that require additional Wayland
  progress, or add an asynchronous public transfer path.

### Priority 2: performance

- Track software framebuffer damage and convert only changed RGBA regions to SHM XRGB instead of
  converting the full frame on every commit.
- Reuse appropriately sized SHM pools across resize bursts rather than destroying and recreating
  both buffers for every configure.
- Reduce CSD memory bandwidth: the below-content frame currently allocates and clears the entire
  content-sized rectangle even though only borders and title controls are visible.
- Evaluate sharing a `wl_display` across windows. The current per-window connection simplifies
  ownership and dispatch but increases compositor connections, registries, output proxies, and
  optional-manager objects.

## Validation commands

```sh
ctest --test-dir build -R 'test_(wayland_loader|linux_auto_)' --output-on-failure
ctest --test-dir build -R test_wayland_egl --output-on-failure
ctest --test-dir build -L graphics3d --output-on-failure
./scripts/lint_platform_policy.sh
./scripts/run_cross_platform_smoke.sh --build-dir build
./scripts/build_demos_linux.sh --run
./scripts/build_zanna_linux.sh
```
