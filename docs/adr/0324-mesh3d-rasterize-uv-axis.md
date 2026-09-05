---
status: active
audience: contributors
last-verified: 2026-09-04
---

# ADR 0324: Mesh3D.RasterizeUvAxis

## Status

Accepted (2026-09-04)

## Context

ADR 0273's amendment added `Mesh3D.RasterizeUvHeight`: every covered texel
of a mesh's UV atlas receives its barycentric bind-pose Y as a luminance
ramp, so a region classifier can cut a character's atlas into garment bands
texel-exactly. Legacy Baseball's uniform repaint (plans 62/63) classifies the
actor atlases with it.

Height alone cannot separate a T-posed arm from the torso beside it: the
forearm and the jersey chest share the same Y band, and the only thing
keeping skin out of the jersey mask was a per-texel colour test that
desaturated, shadowed skin passes. The owner's screenshots showed a batter's
forearm painted club green. The bake's belt line likewise sits at a height
the classifier could only guess at.

## Decision

1. **Runtime API.** `Zanna.Graphics3D.Mesh3D.RasterizeUvAxis(pixels, axis,
   lo, hi)` (C: `rt_mesh3d_rasterize_uv_axis(obj, pixels, axis, lo, hi)`)
   generalises the height op: `axis` 0/1/2 selects bind-pose X/Y/Z, `[lo,
   hi]` maps to luminance 1..255, 0 stays "uncovered". Same conservative
   half-texel coverage, same per-triangle clamp, last-win overlap. An axis
   outside 0..2 is ignored, never trapped.
2. `RasterizeUvHeight` is now the axis-1 form of the same implementation
   (byte-identical output; pinned by the unit test).
3. No new state, no platform code, no behavioural change for existing
   callers.

## Consequences

- Graphics3D manifest hash and function/method counts re-pinned (one
  function, one method). Docs regenerated.
- Legacy Baseball's mask generator (v4) rasterises a lateral map and treats
  every jersey-band texel beyond the measured sleeve end as skin regardless
  of colour; it also completes the height map over LOD-only coverage and
  measures the belt line from the bake instead of a flat constant.
