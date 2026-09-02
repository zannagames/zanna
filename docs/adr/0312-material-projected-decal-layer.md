---
status: active
audience: contributors
last-verified: 2026-09-01
---

# ADR 0312: Material3D Projected Decal Layer

## Status

Accepted (2026-09-01)

## Context

Legacy Baseball's jersey numbers were a separate rigid curved quad socketed to
the spine bone, standing 16-18.5 cm proud of the shirt so it never clipped
through a skinned back. Three owner reviews (plans 52, 53, 76) moved and
re-inked the plate; each still read as a sticker, because a plate can only
approximate the cloth: it casts its own shadow onto the jersey, carries no
normal/AO/roughness maps, samples without mips, and cannot follow the skinned
surface exactly. Baking the digits into the atlas would have worked but costs
one 2048² texture per uniformed actor. The owner chose a shader-level decal
layer.

## Decision

A projected decal layer on `Material3D`, implemented identically on Metal,
D3D11, OpenGL and the software rasterizer, and composited over the albedo
BEFORE lighting so the decal is lit, shadowed and normal-mapped exactly like
the surface it sits on.

- `Material3D.SetDecalMap(source)` / `HasDecalMap` — the decal texture
  (Pixels, TextureAsset3D or RenderTarget3D; backends sample the resident
  Pixels). `SetDecalOpacity(a)` / `DecalOpacity` scale the decal alpha.
- `Material3D.SetDecalProjector(ox,oy,oz, ux,uy,uz, vx,vy,vz, halfW, halfH,
  depth)` — a box in MODEL space (pre-skin / bind positions): origin on the
  surface, right and up directions (up is re-orthogonalized), half extents
  and depth along forward = right × up. Texel (0,0) sits at (-right, +up) so
  images read upright. The runtime stores three affine rows mapping a model
  position to (s, t, d) in [0,1]³ plus the forward vector.
- Vertex stage: each backend evaluates `(s, t, d)` from the vertex's
  pre-skin, post-morph position and `facing = dot(bindNormal, forward)`,
  and interpolates them. On the software backend a CPU-skinned submission
  carries the source mesh's bind vertices (`rt_mesh3d.bind_vertices_ref` →
  `vgfx3d_draw_cmd_t.bind_vertices`) so the projection stays in bind space.
- Fragment stage: when `opacity > 0`, `facing < 0`, and `(s,t,d)` lie in
  [0,1]³, the decal is sampled (clamped, sRGB-decoded under the PBR workflow)
  and `albedo = mix(albedo, decal.rgb, decal.a * opacity)`. Alpha mode,
  shadows and the shadow pass are untouched.
- The layer is runtime-only: `MakeInstance`/`Clone` copy it, VSCN never
  persists it, and it does not participate in texture streaming.

## Consequences

- Public 3D ABI grows by five functions, two properties and three methods
  (manifest re-pinned). `test_rt_material3d` covers set/get/clone and
  degenerate projectors; `test_rt_canvas3d` renders a decal on the software
  backend and checks inside/outside/back-facing texels.
- Metal binds the material block to the vertex stage (buffer 11) and the
  decal at texture 19; D3D11 binds `cb_per_material` to the VS and the decal
  at t19; OpenGL adds unit 18 and the `uDecal*` uniforms; every per-material
  constant block grew by four float4s at its end.
- Legacy Baseball replaces the number plate with a per-actor material
  instance carrying the digits as a decal projected onto the jersey back.
