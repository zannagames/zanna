---
status: active
audience: contributors
last-verified: 2026-09-02
---

# ADR 0317: Application Icon

## Status

Accepted (2026-09-02)

## Context

A game built with `zanna build` is a bare executable: on macOS a raw Mach-O
draws the generic Unix-executable icon in Finder and the Dock, on Windows the
window class only loads a sibling `<exe>.ico`, and on X11 the runtime set only
`_NET_WM_ICON_NAME` (a string). `zanna package` already generates `.icns` /
`.ico` / hicolor PNGs from `package-icon` and writes an `.app` bundle, an
installer and a desktop entry, but a running program had no way to present an
icon of its own, and nothing bridged a bare binary to its packaged identity.
Legacy Baseball shipped with a generic icon (plan 90).

## Decision

1. **Runtime API.** `Zanna.Graphics.Canvas.SetIcon(pixels)` and
   `Zanna.Graphics3D.Canvas3D.SetIcon(pixels)` (window-owning canvases only)
   hand a `Pixels` image (at most 1024 x 1024, `0xRRGGBBAA` words) to
   `vgfx_set_icon(window, rgba, w, h)`. Platform behaviour:
   - macOS: `[NSApp setApplicationIconImage:]`, the Dock icon.
   - Win32: a 32-bit BGRA DIB icon installed as the window's big and small
     icon (`WM_SETICON`); the window class keeps the sibling `.ico` default.
   - X11: the EWMH `_NET_WM_ICON` property (CARDINAL, format 32).
   - Wayland: no icon protocol; ignored (see 3).
   - Mock: ignored.
   Invalid or oversized sources are ignored, never trapped.
2. **Adjacent icon on macOS.** Mirroring the Win32 `<exe>.ico` convention, a
   bare macOS executable installs `<exe>.icns` or `<exe>.png` from beside the
   binary as the Dock icon when its first window opens. A bundled app already
   names its icon in Info.plist.
3. **Wayland app id.** The toplevel app id is `org.zanna.<executable basename>`
   (sanitized) instead of the fixed `org.zanna.app`, so a packaged game's
   desktop entry, and with it its icon, can be matched by the compositor.

## Consequences

- Manifest hash and counts re-pinned (`Canvas3D.SetIcon`); `Canvas.SetIcon` is
  a 2D row. Docs regenerated.
- `zanna build` still emits no `RT_ICON` resource in the PE payload: the
  Windows Explorer icon of a bare `.exe` remains generic until the packager's
  resource builder is shared with the linker (follow-up; the installer and
  shortcuts already carry the icon, and the window shows it at runtime).
- Legacy Baseball: `tools/make_app_icon.zia` authors `media/images/app_icon.png`
  from the boot backdrop lockup, `zanna.project` declares `package-icon` and
  the assets, `scripts/package_baseball.sh` produces the `.app` / installer /
  `.deb`, and the shell calls `Canvas.SetIcon` at boot.
