---
status: active
audience: contributors
last-verified: 2026-08-17
---

# ADR 0224: Bounded Viewport Presentation Budget and Zero-Allocation Readback

- Status: Accepted
- Date: 2026-07-29
- Deciders: Zanna Studio maintainers
- Tags: zannastudio, graphics3d, gui, performance

## Context

Every interactive 3D viewport frame performed two full-frame transient
allocations and two per-pixel conversion loops: the render target read
back into a fresh `Pixels` (RGBA8 → packed u32), and the widget upload
unpacked it again (u32 → RGBA8) through another malloc/free pair. The
editor already bounds interactive resolution (reduced-res frames upscale
to the logical size), but the readback pipeline itself was allocation-
and conversion-heavy, and the upcoming play pane (P5) and GPU compositing
(P6) both need a leaner presentation seam.

## Decision

- **Retained readback buffer.** The 3D editor keeps one
  `viewportReadbackPixels` sized to the render target and reuses it every
  frame: post-FX frames land through
  `Canvas3D.TryCopyScreenshotFinalTo`, plain frames through
  `RenderTarget3D.CopyTo`. Steady-size interactive frames make zero
  transient allocations; a size change reallocates once. Editor overlays
  keep drawing onto the published pixels; the next frame's full-buffer
  copy makes that safe by construction.
- **Direct widget presentation seam.** New runtime method
  `GUI.Image.TrySetFromRenderTarget(target)` (one def function + method)
  row-copies the target's synced RGBA8 color mirror straight into the
  image widget's retained buffer via two narrow seams:
  `rt_rendertarget3d_try_read_rgba` (non-trapping, exact-size, straight
  `memcpy` rows — no packing pass) and
  `vg_image_borrow_writable_pixels`/`vg_image_commit_borrowed_pixels`
  (grow-only widget buffer with commit-after-write semantics). The
  editor viewport cannot use it yet — overlays composite on CPU pixels
  until ADR 0230 moves them into the frame — but the P5 play pane
  presents pure game frames through it, and the graphics-disabled build
  keeps a truthful always-0 stub.
- **Budget truthfulness.** The reduced-resolution interactive budget
  (already present) stays; the stats line now reports a `read` segment
  (milliseconds spent in readback) alongside the per-pass CPU times so
  the P6 compositing work has honest before/after numbers.

## Deferred (recorded)

The GPU-parity half of this program phase — D3D11 headless device
creation (`win == NULL` → `D3D11CreateDevice` HARDWARE→WARP without a
swapchain) and the EGL surfaceless/pbuffer + GLX-pbuffer headless
adapters — requires real Windows and Linux hardware to verify and is
deferred to a platform-verification session. macOS Metal headless
contexts already work (the backend-adaptive constructor contract test
covers them), and the readback pipeline above is backend-independent, so
the play pane and compositing phases proceed on macOS; the Win/Linux
headless legs slot in without further ABI or def changes.

## Consequences

- Interactive orbit/fly no longer allocates per frame on the readback
  path; the `read` stat quantifies what remains for P6 to eliminate.
- `TrySetFromRenderTarget` gives shared-memory and play-pane consumers a
  single-copy presentation path with no trapping edge cases.
- GUI ABI manifest re-baselined for the one new function/method.

## Links

- `docs/adr/0204-project-owned-3d-post-processing-previews.md` (ScreenshotFinal boundary)
- `docs/adr/0222-schema-v19-typed-scene-settings.md`
- `src/runtime/graphics/3d/render/rt_rendertarget3d.c`
- `src/lib/gui/src/widgets/vg_image.c`
