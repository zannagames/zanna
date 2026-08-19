---
status: active
audience: contributors
last-verified: 2026-08-18
---

# ADR 0273: Pixels.DilateOwner + Pixels.ColorizeMasked — Region-Mask Recoloring

## Status

Accepted (2026-08-18).

## Context

Team-uniform recoloring gates texture-space tints with region masks that
must cover UV-atlas gutters, because GPU mip generation averages island
edges against whatever the gutters hold. The atlas gutters are padded
with `Pixels.DilateMasked` (ADR 0269), whose growth rule AVERAGES the
covered 8-neighbors. That rule is correct for photographic fill but
structurally wrong for masks: a binary 255-vs-0 mask front decays under
integer averaging and truncates to zero within ~3-5 rings wherever two
regions share a gutter. Measured on the fielder atlas: 74.4% of gutter
texels ended up albedo-filled but mask-zero — untinted bright chains
("cracks") across every dark home jersey, and raising the pass count
made the halo worse, not better. The averaging semantics are pinned by
`test_dilate_masked_id_map_growth` and stay untouched for existing
callers.

Separately, `Pixels.RecolorMasked` writes `target * (lum / ref_lum)`
with `ref_lum` fixed to the color-class reference and the shade ratio
clamped at 1.5. Dark authored regions (the navy cap, mean luminance 13
against the class reference 29) therefore recolor to ~0.45 x target —
any club color reads near-black, which is why "the cap color cannot be
changed."

## Decision

Two new native Pixels ops, registered beside the ADR-0269 family:

1. **`Pixels.DilateOwner(mask, passes)`**
   (`rt_pixels_dilate_owner(pixels, mask, passes)`): identical contract
   to `DilateMasked` (same-dimension mask, non-zero-RGB coverage, both
   buffers updated in place) with an exact-copy value rule — each
   claimed texel copies the EXACT RGBA of its first covered 8-neighbor
   in the fixed `(dy,dx)` scan order `(-1,-1)..(1,1)`. Values never
   average, so a label/mask map grown with the same coverage as its
   atlas shares one deterministic watershed topology: every gutter
   texel's color owner and region owner coincide by construction.
   Ring-synchronous frontier lists make each ring read only the
   previous ring's coverage (claim results are order-independent) and
   keep the total cost O(texels). `passes <= 0` runs to convergence —
   the full-surface nearest-owner fill used for atlas padding.

2. **`Pixels.ColorizeMasked(mask, rgb, refLum, maxShade, strength)`**
   (`rt_pixels_colorize_masked`): `RecolorMasked`'s shade-preserving
   interior formula with the color-class gates replaced by an explicit
   coverage mask (proportionally scaled, the `TintMaskedNeutral`
   convention), an explicit reference luminance, and an explicit shade
   clamp (invalid/<= 0 falls back to the historical 1.5). A navy cap
   region colorized with `refLum 13` reaches any bright club color at
   full brightness with its shading preserved; the same op with a white
   target performs the one-time "whiten authored navy regions" atlas
   preparation. Alpha untouched.

## Amendment (2026-08-19): `Mesh3D.RasterizeUvHeight`

The band rasterizer (`RasterizeUvMaskY`, ADR 0269 family) includes every
triangle whose Y range INTERSECTS the band, so classification boundaries
derived from bands are triangle-granular (±6 cm on the actor meshes) —
the jagged hem/shoulder cuts. New op
`Zanna.Graphics3D.Mesh3D.RasterizeUvHeight(pixels, yMin, yMax)`
(`rt_mesh3d_rasterize_uv_height`): identical triangle setup and
conservative half-texel coverage, but each covered texel receives the
BARYCENTRIC-INTERPOLATED object-space Y at its center (clamped to the
triangle's own Y range), mapped `[yMin, yMax] -> luminance 1..255`
(0 = uncovered). Overlapping triangles last-win (charts are disjoint).
Callers threshold the height map per TEXEL — garment cuts become exact.
One Graphics3D ABI entry + method; the pinned graphics3d manifest is
re-pinned deliberately with this ADR as the review record.

Also added: `Zanna.Graphics.Pixels.StampNonZero(src)`
(`rt_pixels_stamp_nonzero`) — copy every src texel with non-zero RGB
over the receiver (same dims; mismatch no-op). The sparse-layer stamp:
the offline mask generator computes garment-fabric-sourced fills for the
AI bakes' pure-black occlusion holes (a DilateOwner whose fill domain is
the garment masks only) and ships them as a mostly-zero heal layer; the
runtime applies it in one native pass before padding and tinting.
Pixels 2D surface — unpinned, no manifest churn.

## Consequences

- Two more Pixels ABI entries (`void(obj,obj,i64)` and
  `void(obj,obj,i64,i64,f64,f64)`); registered in
  `defs/api/graphics2d.def` + `defs/classes/localization.def`; covered
  by `RTPixelsTests.cpp` (`test_dilate_owner_*`,
  `test_colorize_masked_*`). The 2D Pixels surface carries no pinned
  ABI manifest (ADR 0269 precedent).
- `DilateMasked` remains unchanged; callers that relied on growing a
  region mask with it should migrate to `DilateOwner` (the baseball
  registry3d does in plan 62).
- Full-fill padding via `DilateOwner(passes=0)` supersedes fixed-count
  gutter rings for atlas preparation: the entire background becomes
  nearest-island color, which is the strongest available mitigation for
  deep-mip background bleed. True island-to-island mip bleed stays out
  of scope (as in ADR 0269).
