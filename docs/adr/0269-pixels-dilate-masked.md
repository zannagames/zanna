---
status: active
audience: contributors
last-verified: 2026-08-18
---

# ADR 0269: Pixels.DilateMasked — UV-Atlas Gutter Dilation

## Status

Accepted (2026-08-18).

## Context

Character atlases (Meshy-class exports) pack many small UV islands over
a dark background with narrow gutters. GPU minification samples the mip
chain, and mip generation is a plain box filter with no UV awareness:
every island edge averages against the gutter background, so distant or
half-height characters render with dark (or neighbor-island-colored)
speckle fringing heads, hands, and uniform edges. The artifact is
independent of the runtime team-tint ops — untinted actors (the umpire)
show it identically — and survives full-resolution bakes, because the
bleed happens in the runtime mip chain, not in a bake-time resample.

The classic fix is gutter dilation: flood island border colors outward
into the background before mips are generated. The engine had no pixel
op for it, and interpreted per-texel loops are not viable for 2048²
atlases under the VM (the same reason `RecolorMasked` /
`TintLuminanceMasked` / `TintMaskedNeutral` are native ops).

## Decision

New native op `Zanna.Graphics.Pixels.DilateMasked(mask, passes)`
(`rt_pixels_dilate_masked(pixels, mask, passes)`):

1. **Coverage-driven.** `mask` (same dimensions as the receiver;
   mismatch is a no-op) marks covered texels: any non-zero RGB, the
   same convention as `TintMaskedNeutral`. Callers rasterize coverage
   with `Mesh3D.RasterizeUvMaskY` over the mesh's full Y range (union
   across LOD meshes).
2. **Uniform growth.** Each pass gives every uncovered texel with at
   least one covered 8-neighbor the average RGBA of those covered
   neighbors, then marks it covered — double-buffered so growth is one
   ring per pass. `passes` is the gutter width in texels, clamped to
   [0, 256]; the loop exits early once nothing grows.
3. **In-place, both buffers.** The receiver is dilated in place and the
   mask records the grown coverage (callers may reuse it for a second
   map sharing the same UV layout).

## Consequences

- One more Pixels ABI entry (`void(obj,obj,i64)`); registered in
  `defs/api/graphics2d.def` + `defs/classes/localization.def`, covered
  by `RTPixelsTests.cpp` (`test_dilate_masked_gutter_fill`).
- Cost is `passes × width × height` with a small constant; a 24-pass
  dilation of a 2048² atlas is a one-time load cost per material.
- Deep-mip island-to-island bleed (two islands closer than the mip
  footprint) is out of scope; dilation only guarantees the background
  no longer contaminates island borders.
