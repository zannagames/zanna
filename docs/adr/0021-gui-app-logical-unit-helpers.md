---
status: active
audience: contributors
last-verified: 2026-06-30
---

# ADR 0021: HiDPI Logical-Unit Helpers (Zanna.GUI.App)

## Status

Accepted (runtime implemented; Zanna Studio's overlays are the intended first
consumer). Driven by the GUI runtime-additions review, recommendation **R4**
(review document retired 2026-07-15; the recommendation ids are the surviving record).

## Context

On a HiDPI display `Zanna.GUI.App.GetWidth()`/`GetHeight()` report **physical**
pixels (e.g. 2560 on a 1280-point 2× window), but floating panels and overlays are
positioned and sized in **logical** (point) units. The runtime exposes the scale
factor (`GetScale()`) but no conversion, so every consumer divides by hand.

In Zanna Studio this `physicalToLogical` helper is **duplicated across three files** —
`ui/ide_overlays.zia:717-728`, `ui/debug_breakpoint_overlay.zia:223-224`,
`ui/explorer_actions.zia:346-347` — each re-deriving the same `value <= 0 ? value :
scale <= 1.0 ? value : round(value / scale)` logic. Any Zanna GUI app that places a
panel relative to the window hits the same friction. This is a missing convenience
on an existing primitive, not application logic.

Adding runtime methods is a runtime C-ABI surface change, which requires an ADR.

## Decision

Add four methods to `Zanna.GUI.App`:

- `GetLogicalWidth() -> i64` — window width in logical units (`GetWidth() / GetScale()`).
- `GetLogicalHeight() -> i64` — window height in logical units.
- `ToLogical(physical: i64) -> i64` — convert a physical-pixel value to logical
  units using the window's current scale.
- `ToPhysical(logical: i64) -> i64` — convert a logical value to physical pixels.

Conversion semantics match the hand-rolled idiom exactly (so adoption is
behaviour-preserving):

- Non-positive values pass through unchanged.
- A scale `<= 1.0` (including a non-finite/NaN scale, and the no-window case which
  reports scale `1.0`) passes the value through unchanged — so the same code is
  correct on standard 1× displays.
- Otherwise round `physical / scale` (or `logical * scale`) to the nearest integer.

The identity `GetLogicalWidth() == ToLogical(GetWidth())` holds, and `ToLogical` /
`ToPhysical` round-trip at integer scales.

### Implementation

The conversion math is two `static inline` functions in `rt_gui.h`
(`rt_gui_dpi_to_logical` / `rt_gui_dpi_to_physical`), so the App methods and the
unit test share one definition and the arithmetic is testable headlessly without a
window. The four App methods (`rt_app_*` in `rt_gui_system.c`) are thin wrappers:
they fetch the window's scale (the same `rt_app_window_scale` `GetScale` uses) and
apply the inline. Graphics-disabled twins in the file's `#else` block return `0`
(logical size, no window) or pass the value through (scale `1.0`). No new class, no
new class id, no new file (so no `source_health` surface change).

## Consequences

- **Adoption:** the three duplicated `physicalToLogical` helpers collapse to
  `app.GetLogicalWidth()` / `app.ToLogical(...)`; the conversion lives in one place
  and is correct under any backing scale.
- **Determinism / cross-platform:** pure arithmetic over the existing scale getter;
  no new OS surface, no platform `#ifdef`.
- **No behavior risk:** purely additive; `GetWidth`/`GetHeight`/`GetScale` are
  unchanged, and the conversion reproduces the existing idiom byte-for-byte.

## Alternatives Considered

- **Leave callers to divide by `GetScale()`.** Rejected: every GUI app re-derives
  the same rounding-and-passthrough logic, and getting the 1×/non-finite edge cases
  wrong is a silent HiDPI bug.
- **A static `Dpi.ToLogical(px, scale)` taking an explicit scale.** Rejected as the
  primary surface: the caller would still fetch the app's scale and thread it
  through. The instance methods read the window's own scale, which is what every
  consumer wants; the explicit-scale form remains available internally as the shared
  inline.
- **Report logical units from `GetWidth()` directly.** Rejected: that would silently
  change an existing API's contract and break callers that correctly expect physical
  pixels (e.g. for `Canvas` blits). Adding distinct logical getters keeps both
  meanings available and unambiguous.
