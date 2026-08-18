---
status: active
audience: contributors
last-verified: 2026-08-18
---

# ADR 0270: PostFX Backend Snapshot Carries the COLOR_LUT Payload

## Status

Accepted (2026-08-18).

## Context

`PostFX3D.AddColorLut(pixels, blend)` has been public registry surface since
Track E (a 256x16 strip: 16 tiles of 16x16, x = red, y = green, tile = blue;
trilinear; the chain retains the Pixels). Only the software reference chain
ever implemented it: the backend-facing `vgfx3d_postfx_snapshot_t` had no LUT
fields, `vgfx3d_postfx_fill_effect_snapshot` preserved the entry's ordered
discriminator with an empty snapshot, and no GPU backend referenced
`COLOR_LUT` at all. On Metal/OpenGL/D3D11 a LUT entry therefore encoded a
silent full-screen passthrough pass — the reviewed software capture and the
shipped live frame were different images, which is why Legacy Baseball's
grading kit (`lut_kit.zia`) was pulled from its live chains (plan 49 B2) and
the gap was ledgered as plan-59 B10.

## Decision

1. **Snapshot payload (ABI-stable append).** `vgfx3d_postfx_snapshot_t` gains
   `color_lut_enabled`, `color_lut_blend`, `color_lut_texels`
   (`const uint32_t *`, packed 0xRRGGBBAA exactly as `rt_pixels` stores it),
   `color_lut_width`, `color_lut_height`, and `color_lut_revision`
   (`rt_pixels_generation` of the retained Pixels). The pointer borrows the
   chain-retained LUT Pixels: it stays valid while the chain retains the LUT
   (released on `Clear`/destroy/replacement) and backends must never free it.
2. **Cache key contract.** Backends treat `(color_lut_texels,
   color_lut_revision)` as a texture cache key: re-upload when either
   changes, otherwise reuse the uploaded texture. Uploads byte-swizzle the
   0xRRGGBBAA words into RGBA8 staging so host endianness never leaks into
   the texture.
3. **Sanitizer.** `vgfx3d_sanitize_postfx_snapshot` clamps the blend to
   [0, 1] and force-disables any payload whose pointer is null or whose
   dimensions are not exactly 256x16. Backends additionally force-disable
   the pass when the texture upload itself fails, so the shader never grades
   toward a fallback texture.
4. **Application point.** Each backend samples the strip in its `postfx`
   fragment shader at the pass's authored chain position, replicating the
   CPU reference sampler (`apply_color_lut_cpu`) texel-for-texel: 8-corner
   trilinear over integer reads, `mix(color, lut, blend)`. Like every chain
   effect, the LUT is display-referred when authored after the tonemap —
   the position Legacy Baseball uses — and the CPU path remains the parity
   reference.
5. **Out of scope.** `AUTO_EXPOSURE` and `SUN_SHAFTS` keep their
   ordered-discriminator-only export; they remain software-reference-only.

## Consequences

- `AddColorLut` now grades identically on the software captures and all
  three GPU backends; visual gates measured on software captures are honest
  proxies for the live frame.
- No registry surface, IL opcode, grammar, or serialized format changes —
  the ABI manifest is untouched. The snapshot struct grows by appended
  fields only (existing backends memcpy whole structs).
- Tests: `test_rt_postfx3d_snapshot` pins the payload export, packing, and
  sanitizer gates; the three `test_vgfx3d_backend_*_shared` suites pin the
  tile-sampler source and the upload-failure force-disable in each backend.

## Links

- `src/runtime/graphics/3d/render/rt_postfx3d.{c,h}` — snapshot fill + CPU reference
- `src/runtime/graphics/3d/backend/vgfx3d_backend_utils.c` — shared sanitizer
- `src/runtime/graphics/3d/backend/vgfx3d_backend_metal_{draw,context}.inc`
- `src/runtime/graphics/3d/backend/vgfx3d_backend_opengl_{targets,shaders}.inc`
- `src/runtime/graphics/3d/backend/vgfx3d_backend_d3d11_{present,shaders}.inc`
- ADR 0247 — 3D display transfer contract (application point rationale)
- `baseball/plans/59-broadcast-truth-look-and-boot.md` — B10
