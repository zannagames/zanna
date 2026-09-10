---
status: accepted
audience: contributors
last-verified: 2026-09-09
---

# ADR 0348: Perspective shadow receivers get slope-scaled and normal-offset bias

## Context

Every backend's shadow sampler (Metal, D3D11, OpenGL, software) applies a
normal offset of ~1.5 shadow texels and a slope-scaled compare bias, but the
texel size and depth scale that drive both were derived only for orthographic
(directional) maps. For perspective maps — spot lights and cube faces — both
stayed zero on the GPU backends, so the normal offset was a no-op and the
slope term multiplied by zero; only the constant `SetShadowBias` remained. The
software backend applied an orthographic texel size to perspective maps and a
slope term multiplied into the base bias, so a small base bias disabled slope
handling there too.

Legacy Baseball's night rig is entirely spot lights. Its constant bias had to
fall to 0.000005 to recover any contact shadow, and the retained captures show
no roof occlusion inside the dugout, no contact shadows under players in the
closed-roof venue, and striping on the roof edge. `quality.zia` documented the
opposite assumption ("the backend also normal-offsets receivers and applies a
world-texel slope bias").

## Decision

Derive the receiver-space scale from the shadow VP rows for every projection
type, with one shared host helper `vgfx3d_shadow_receiver_scale` (row-major
VP, projection type, receiver position, texel size in NDC) that the software
backend calls and whose formulas the three GPU samplers reproduce:

- Orthographic: `texelWorld = 2 · texel / |row0|`, `depthScale = 0.5 · |row2|`
  (unchanged).
- Perspective and cube: with `w = row3 · p + t3` the receiver's distance along
  the light axis and `z = row2 · p + t2`, `texelWorld = 2 · texel · w / |row0|`;
  the axial component `a = (row2 · row3) / |row3|²` gives `z = a·w + b`, so the
  stored depth changes by `0.5 · |b| / w²` per world unit along the axis with
  `b = z − a·w`. A receiver behind the light (`w ≤ 0.0001`) is lit.

The bias formula itself is unchanged on the GPU
(`base + slopeFactor · min(slope, 8) · texelWorld · depthScale`) and the normal
offset `N · texelWorld · 1.5` now applies to perspective receivers. The
software backend keeps its map-gradient slope (already stored depth per texel)
but adds it — scaled by the canvas slope-bias knob and capped at the GPU's
slope-8 equivalent `8 · texelWorld · depthScale` — instead of multiplying it
into the base bias. No public runtime API, registry, IL, dependency or
serialized format changes. Existing software snapshot hashes that include spot
shadows move once and are rebaselined in the same change.

## Validation

Host tests: the helper against an analytic perspective projection
(`texelWorld = 2·d·tan(fov/2)/res`, `depthScale = f·n/((f−n)·d²)`) and a
finite difference of the projected stored depth; orthographic, invalid type,
non-finite input and a receiver behind the light. Software render: a spot-lit
plane renders clean at a 1e-5 base bias with the production and the maximum
slope knob while the sphere's shadow stays darker than the lit plane, and a
test-only seam proves the context carries the knob (the knob's magnitude is
not observable in that 45-degree fixture, where the normal offset already
covers the receiver; the existing snapshot stays byte-identical). Native
Metal evidence in Legacy Baseball (`analysis/plan109/shadow-*`): contact
shadows under players and dugout roof occlusion at the production bias,
roof-edge striping absent at 1:1. D3D11/OpenGL carry the same source change
and remain unverified on hardware from this Mac.
