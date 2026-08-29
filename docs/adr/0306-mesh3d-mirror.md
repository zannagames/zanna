---
status: active
audience: contributors
last-verified: 2026-08-29
---

# ADR 0306: Mirror Skinned Meshes Across the Sagittal Plane

## Status

Accepted (2026-08-29)

## Context

ADR 0243 gave `Animation3D.Mirror` — a left/right-mirrored copy of a clip
whose bone tracks are remapped to their sagittal partners — so a right-handed
performance can play left-handed on the same skeleton. A mirrored clip alone
is not a left-handed character: a character mesh with an asymmetric prop
baked in (Legacy Baseball's fielder carries its glove in the left-hand
geometry) would swing the gloved hand. The engine had no way to produce the
mirrored mesh: `Mesh3D.Transform` reflects geometry (reversing winding and
tangent handedness for a `det < 0` matrix) but keeps every bone influence on
its original bone, `Mesh3D.SetBoneWeights` is write-only, and no backend
flips front-face winding for reflected scene nodes (a negative-scale node
would need a winding flip in Metal, D3D11, OpenGL and the software raster,
including shadow, depth and instanced passes — and the Metal path ships
un-gated because CI runs the software raster).

## Decision

Add `Zanna.Graphics3D.Mesh3D.Mirror(mesh, skeleton) -> Mesh3D`
(`rt_mesh3d_mirror`), registered beside `Transform` as a function and a
method. It returns a NEW mesh:

- positions, normals and tangents reflected across model `X = 0` through
  the `Transform` math with `diag(-1, 1, 1)` — winding reversed, tangent `w`
  negated;
- morph deltas reflected (`rt_morphtarget3d_clone_mirrored_x`: the X lane
  of every position / normal / tangent delta negated; names and weights
  copied);
- every bone influence (the four vertex slots and the 5-8 side stream)
  remapped to its sagittal partner through the mesh's palette (`bone_map`
  when present) with the ADR 0243 resolution order, promoted to a shared
  runtime helper `rt_skeleton3d_mirror_bone(skeleton, bone)`: exact-name
  side-token swap (`Left`/`Right`, `left`/`right`, `LEFT`/`RIGHT`) → humanoid
  role side flip → self.

`skeleton` may be null (the mesh's attached skeleton serves); a handle that
is not a `Skeleton3D` yields null (no trap). A mesh without bone weights
mirrors as plain geometry. LOD meshes are mirrored by the caller per level.
The source mesh is never mutated.

## Consequences

- One mirrored mesh per role serves every opposite-handed character on that
  skeleton (single-digit megabytes for a character mesh); Legacy Baseball
  can render a left-handed pitcher or first baseman with the glove on the
  right hand from the same asset.
- Uniform textures that carry text would read mirrored; Legacy Baseball's
  uniforms carry region-mask colours only (numbers are separate props).
- Public 3D surface: +1 function, +1 method (`test_graphics3d_runtime_manifest`
  re-pinned; `baseball/scripts/verify_generated_inventory.sh` fn 8009 → 8010,
  method 5763 → 5764).

## Tests

`src/tests/unit/test_rt_mesh3d_mirror.cpp`: a two-arm strip on a
Hips/LeftArm/RightArm/Tail skeleton — positions negate in X, normals follow,
both triangles reverse winding, tangent handedness flips, `LeftArm`
influences move to `RightArm`, the center and the unpaired bones self-map,
mirror∘mirror restores geometry and influences, morph deltas reflect, the
attached skeleton resolves partners when none is passed, a non-skeleton
handle yields null, and a weightless box mirrors as geometry.
