---
status: draft
audience: contributors
last-verified: 2026-08-13
---

# ADR 0246: OpenGL Shadow Atlas — Close GAP-8 Without a Per-Context Capability

## Status

Accepted; **partially implemented** (2026-08-13).

Landed — the sampler-budget half, which was the stated blocker:

- All shadow slots now live in one `GL_TEXTURE_2D_ARRAY` depth texture sampled
  through `uShadowArray` (`sampler2DArrayShadow`) on unit 4, replacing
  `uShadowTex0..3` on units 4-7. `shadowTexelSize` and `sampleShadowCmp` lost
  their four-way branches entirely.
- The BRDF LUT took a dedicated unit 5, so terrain-splat draws no longer evict
  it and fall back to the analytic environment BRDF. Units 6-7 are spare.
- `glTexImage3D` and `glFramebufferTextureLayer` joined the loader.
- Allocation is grow-only: a slot requesting a smaller map reuses the existing
  larger array rather than reallocating and discarding every rendered layer.

Verified: cross-backend conformance is byte-identical to before the refactor
(3.032 / 2.250, both `RESULT: ok`) and the `walk_min` golden passes, so cascade
slots 0-3 render and sample correctly through the array.

Two further defects were found and fixed while chasing the atlas, both of which
stand on their own:

- **`gl_restore_framebuffer_state` could poison the GL error queue.** It restored
  the captured draw/read buffer unconditionally, so a snapshot pairing an FBO
  binding with `GL_BACK` replayed `glDrawBuffer(GL_BACK)` onto an FBO —
  `GL_INVALID_OPERATION`, once per shadow slot per frame. Buffer tokens are now
  range-checked against the framebuffer they are applied to
  (`gl_buffer_token_valid_for`). This is a latent bug in a helper used across the
  whole backend, not something the atlas introduced; the atlas merely called it
  in a state that exposed it.
- **Cube faces took the orthographic path.** `sampleShadowMapCascade` gated the
  perspective divide on `projectionType == 1`, but `VGFX3D_SHADOW_PROJECTION_CUBE`
  (2) is equally a perspective frustum — each face is a 90° projection — so cube
  UVs never landed on the map. Both sites now test `projectionType != 0`.

**Still not landed — `shadow_atlas_slots` remains 0, so GAP-8 is still open.**
The storage half now demonstrably works: layered FBOs report
`GL_FRAMEBUFFER_COMPLETE`, a six-face omni light renders slots 0-6, the completed
count climbs 0→7 as each slot finishes, and the GL error queue stays clean end to
end. But `test_canvas3d_point_shadows.zia` still reports no darkening on OpenGL:
the geometry rasterizes into the layers and the shader resolves a face, yet the
depth comparison reads fully lit. The remaining fault is somewhere between the
per-face `uShadowVP` matrices and the face lookup, and is not diagnosed.

The flag stays off deliberately: advertising an atlas whose shadows silently read
as fully lit would hand Canvas3D slot indices that produce no shadow at all —
exactly the silent-breakage class this ADR exists to close. Flip it to 1 only once
the point-shadow probe passes.

## Context

Point/omni-directional shadows do not work on Linux. OpenGL is the only backend
with `shadow_atlas_slots = 0` (`vgfx3d_backend.h:662`), which has two effects:

- `rt_canvas3d_render_pass.inc:604-605` caps the frame's shadow budget at
  `VGFX3D_CSM_SLOTS` (4) instead of `VGFX3D_MAX_SHADOW_LIGHTS` (12).
- `rt_canvas3d_overlay.c:1659-1661` withholds
  `RT_CANVAS3D_BACKEND_CAP_SHADOW_POINT`, so
  `Canvas3D.BackendSupports("shadow-point")` reports false.

D3D11 and Metal both set the flag. D3D11 stores the eight non-cascade slots as a
static 4×2 tile grid in one depth texture bound at `t17`
(`VGFX3D_D3D11_SHADOW_ATLAS_COLUMNS/ROWS` = 4/2), keyed by
`slot - VGFX3D_CSM_SLOTS`, and selects a cube face from
`VGFX3D_SHADOW_PROJECTION_CUBE` in the shader.

**The blocker is the fragment sampler budget, not the shadow code.** OpenGL 3.3
guarantees only 16 fragment texture image units, and the main PBR shader assigns
all 16 (`vgfx3d_backend_opengl.c:83-96`):

| unit | 0-3 | 4-7 | 8 | 9-11 | 12 | 13 | 14 | 15 |
|---|---|---|---|---|---|---|---|---|
| | diffuse / normal / spec / emissive | shadow 0-3 | splat control | splat layers 0-2 | **splat layer 3 ⟷ BRDF LUT (aliased)** | env map | metallic-roughness | AO |

There is no free unit for an atlas. The aliasing at unit 12 is a live quality
bug in its own right: `GL_TU_BRDF_LUT` is defined as `GL_TU_SPLAT_LAYER0 + 3`,
so a terrain-splat draw evicts the real BRDF LUT and the shader silently
substitutes an analytic environment-BRDF approximation
(`vgfx3d_backend_opengl_shaders.inc:755-763`). D3D11 always samples the true
table from a dedicated `t18`.

The obvious escape — probe `GL_MAX_TEXTURE_IMAGE_UNITS` (real drivers report 32)
and use a wider layout when available — cannot express itself through the
current contract. `shadow_atlas_slots` is a static `int8_t` on the vtable, one
value for the whole backend, but the answer would depend on the device. Making
it truthful would require a per-context query, i.e. a cross-layer contract
change, which is what forced this ADR.

## Decision

**Fit the atlas inside OpenGL 3.3's guaranteed 16-unit budget, and do not add a
per-context shadow capability.**

Concretely:

1. **Collapse all shadow sampling into one `sampler2DArrayShadow` at unit 4.**
   Layers `[0, VGFX3D_CSM_SLOTS)` hold the cascades; layers
   `[VGFX3D_CSM_SLOTS, VGFX3D_MAX_SHADOW_LIGHTS)` hold the general atlas slots,
   keyed exactly as D3D11 keys its tiles. `uShadowTex0..3`
   (`vgfx3d_backend_opengl_shaders.inc:289-292`) disappear.
2. **Give the BRDF LUT its own unit (5)** and delete the
   `GL_TU_BRDF_LUT == GL_TU_SPLAT_LAYER0 + 3` alias along with the analytic
   fallback branch. Units 6-7 stay free for future growth.
3. **Port the cube-face selection** from the D3D11 shader: for
   `VGFX3D_SHADOW_PROJECTION_CUBE`, pick the face from the dominant axis of the
   light-to-fragment vector and index the matching layer.
4. **Set `shadow_atlas_slots = 1` on the OpenGL backend** and leave the two
   consumers untouched.
5. **Add `glTexImage3D` (or `glTexStorage3D`) and `glFramebufferTextureLayer`**
   to the hand-rolled loader. Both are GL 3.3 core, so they resolve
   unconditionally and need no `LOADP_OPTIONAL` treatment.
6. **Relax the header comment at `vgfx3d_backend.h:789-793`,** which currently
   prescribes "tiles of one internal depth atlas (static 4x2 grid …)". Tile
   geometry is a backend-private detail; the contract should say only that slots
   at or above `VGFX3D_CSM_SLOTS` are backend-resident and cost no extra bind
   point. D3D11's grid is unchanged.

Collapsing 4 units into 1 frees 3, which is enough for the atlas and the BRDF
LUT with one to spare — so the capability is a property of the backend, not the
device, and the static flag stays honest.

### Alternatives rejected

- **Per-context capability query.** Probe `GL_MAX_TEXTURE_IMAGE_UNITS`, use a
  wide 20-unit layout when present, and report the result through a new
  per-context hook (or by moving `SHADOW_POINT` into the existing
  `get_feature_caps`, which already carries device-dependent bits like
  `HDR_SCENE` and `TAA`). Rejected because it makes a user-visible feature
  silently device-dependent and doubles the shader layouts under test, to buy
  nothing: the budget fits without it. If some future feature genuinely needs
  more than 16 units, `get_feature_caps` is the established seam and this
  decision does not block it.
- **A tiled 2D atlas mirroring D3D11 exactly.** Needs no new entry points and
  keeps the backends visually symmetric, but a PCF footprint near a tile edge
  reads neighbouring tiles, so it needs gutters and per-tile clamping. Array
  layers make the bleed structurally impossible. D3D11 already carries that
  complexity; there is no reason to import it.
- **Leave GAP-8 open and require 5-8 lights be unshadowed on Linux.** This is
  the status quo and the largest remaining user-visible platform gap.

### Constraint accepted

Array layers share one resolution, so cascades and atlas slots must agree on a
size. `canvas3d_ensure_shadow_targets(c, c->shadow_resolution)` already drives
both from a single canvas-level setting, so this matches existing behaviour; the
per-slot `ctx->shadow_width[]` / `shadow_height[]` arrays in the GL context
become uniform and can be collapsed.

## Consequences

- `Canvas3D.BackendSupports("shadow-point")` becomes true on Linux and the
  per-frame shadow budget rises from 4 to 12 slots, matching Windows and macOS.
- Terrain-splat draws stop losing the BRDF LUT, so splat materials gain correct
  specular IBL on OpenGL. This changes rendered output for those materials and
  will move any golden that contains splat terrain.
- The main PBR fragment shader changes shape (one array sampler replacing four
  2D samplers). This is the highest-regression edit in the OpenGL backend and
  should land as its own commit, separate from the sampler-budget renumbering.
- GAP-8 is retired from `docs/cross-platform/platform-differences.md:419`.
- 12 shadow FBOs are already allocated (`vgfx3d_backend_opengl.c:852`, sized
  `VGFX3D_MAX_SHADOW_LIGHTS`; the constants live at
  `rt_canvas3d_internal.h:853,858`, so the eight non-cascade slots are exactly
  D3D11's 4×2 grid) and
  `gl_shadow_begin` already accepts slots 4-11
  (`vgfx3d_backend_opengl_frame.inc`), so the render side needs re-targeting to
  layers rather than new plumbing.

## Validation

The OpenGL translation unit is `__linux__`-guarded and cannot be compiled on the
macOS dev host, which is why GAP-8 has been deferred repeatedly. This work
requires a Linux session, and on a host with X11 development headers if the GLX
path is to be covered as well as Wayland/EGL.

- `zia_graphics_conformance_cross_backend` (default lane as of 2026-08-13)
  guards against colour or resolve regressions from the shader edit.
- Add `g3d_test_canvas3d_point_shadows_opengl` mirroring the existing D3D11
  (`src/tests/CMakeLists.txt:3135`) and Metal (`:3146`) registrations — a point
  light must darken geometry behind an occluder.
- A splat-terrain scene must show the specular-IBL change, confirming the alias
  is gone rather than merely unbound.
- Slots 8-11 must be exercised, not just the first atlas layer, so a
  cube-face indexing error cannot hide.
