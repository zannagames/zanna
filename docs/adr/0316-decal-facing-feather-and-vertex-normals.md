---
status: active
audience: contributors
last-verified: 2026-09-02
---

# ADR 0316: Feathered Decal Facing and Mesh3D.VertexNormal

## Status

Accepted (2026-09-02) — amends ADR 0312.

## Context

ADR 0312's projected decal accepted a fragment only when the interpolated bind
normal pointed against the projector (`facing < 0`). Legacy Baseball's back
number sits on a torso whose spine leans 8-12 degrees forward in bind and whose
lumbar curls under the belt; on the coarse batter mesh (5 k vertices, about
thirty on the back inside the box) the interpolated facing crossed zero inside
whole triangles, so the bottom of the digits vanished along a straight cut —
the owner's "the batter's number is not rendered completely, especially towards
the bottom". The game could not even measure the reject: `Mesh3D` exposed
vertex positions (ADR 0210) but not the normals the shaders test.

Measured on the batter (`probes/number_decal_probe.zia`, 2026-09-02): the
true back surface behind the spine carries INWARD normals over most of its
height (per 2 cm band, outward/inward: 0/5, 0/1, 4/0, 2/3, 0/6, 6/0), with the
winding mirrored to match, so the lighting — which flips the normal on
back-facing triangles of cull-off geometry (ZB-21) — never revealed it. The
decal's facing test read the raw bind normal and rejected every such triangle.
The fielder bake is all-outward, which is why the pitcher's number was whole.

## Decision

1. The decal facing test uses the OUTWARD normal, exactly as the lighting
   does: on a back-facing fragment of cull-off geometry the interpolated
   facing is negated before the test (Metal `[[front_facing]]`, D3D11
   `SV_IsFrontFace`, OpenGL `gl_FrontFacing`, software `normal_sign`).
2. The facing gate is FEATHERED, identically on all four backends: fragments
   with `facing < 0.5` enter the decal path and the blend weight is
   multiplied by `1 - smoothstep(0.25, 0.5, facing)`. Surfaces up to about 75
   degrees from the projector take the decal in full, it fades out over the
   next 15 degrees, and the far side of a torso (`facing ~ +1`) stays clean.
   The `< 0` hard cut is gone.
3. `Mesh3D.VertexNormal(i)` returns one vertex's authored (bind-space) normal
   as an owned `Vec3` (NULL/null for an invalid receiver or index), the twin
   of `VertexPosition`, so a game can compute the decal's `(s, t, d, facing)`
   for every vertex with the engine's own rows before placing a projector.

## Consequences

- Manifest hash re-pinned (`test_graphics3d_runtime_manifest`); the software
  rasterizer's decal output changes only where a normal tilts past
  perpendicular to the projector.
- Baseball's `probes/number_decal_probe.zia` measures every role's back
  coverage per vertical decile of the box on LOD 0-2 and gates the bottom.
- Decal placement should still be authored in the surface's own frame (the
  bone's bind axes); the feather is a safety net, not a substitute.
