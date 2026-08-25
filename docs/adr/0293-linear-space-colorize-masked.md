---
status: active
audience: contributors
last-verified: 2026-08-24
---

# ADR 0293: Linear-Space Variant of Pixels.ColorizeMasked

## Status

Accepted

## Context

`Zanna.Graphics.Pixels.ColorizeMasked` performs a shade-preserving recolor:
each masked texel becomes `target * (texelLuma / refLuma)`, computed entirely
on sRGB-encoded bytes. When the recolored raster is consumed as a PBR albedo,
the 3D backends linearize it (`srgb_to_linear` in the shader), so a byte-space
shade factor `s` lands in linear light as roughly `s^2.4`: every texel darker
than the reference is darkened about twice as hard as the author intended,
while the bright tail is clamped by `maxShade`. On a dark-uniform recolor
(reference luma near the region median) roughly half the region sits below the
reference, so the whole garment renders materially darker than the requested
color — the Legacy Baseball "jerseys and caps too dark" defect. Offline mask
generators grew a compensating "accent lift" for small regions (caps, trim),
which is a workaround for the wrong-space multiply, not a fix.

The existing op cannot change in place: its byte-space behavior is pinned by
golden imagery and by callers that feed non-albedo rasters (UI tiles) where
display-space math is the intended semantic.

## Decision

Add one new op with the identical signature:

```
Zanna.Graphics.Pixels.ColorizeMaskedLinear(mask, rgb, referenceLuminance,
                                           maxShade, strength)
```

Semantics match `ColorizeMasked` except WHERE the shade is applied. The shade
ratio is read identically to the byte-space op — Rec.601 luma on the encoded
bytes over `referenceLuminance` — because that ratio IS the authored shading
pattern (creases, AO) as the artist painted it. But instead of multiplying the
encoded target bytes, the target is linearized through the exact sRGB EOTF the
3D backends apply (piecewise 2.4-gamma), `target_linear * shade` is computed,
the `maxShade` clamp and `strength` blend run in linear light, and the result
re-encodes to sRGB bytes. At the reference luma both ops land exactly on the
target; below it the linear variant preserves the authored shading in LIGHT,
where the byte-space multiply followed by shader linearization compressed it
roughly quadratically. Alpha is untouched. Callers recoloring PBR albedo
regions should use the linear variant; display-referred rasters keep the
original op.

## Consequences

- Existing callers and goldens are untouched; adoption is per-call-site.
- A recolored albedo now round-trips: the on-screen color of a region whose
  texel sits at the reference luma is the lit/tonemapped rendering of the
  requested color, and darker texels darken proportionally in light, not in
  encoded bytes.
- The op pair must stay signature-identical so call sites can switch by name
  alone. Registered in `src/il/runtime/defs/api/graphics2d.def` and the
  `Pixels` class def; implemented beside `rt_pixels_colorize_masked` in
  `src/runtime/graphics/2d/rt_pixels_transform.c`; unit-covered in
  `src/tests/runtime/RTPixelsTests.cpp` (VM/native identical by construction —
  pure integer/double math on both paths).
