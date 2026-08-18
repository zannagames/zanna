---
status: active
audience: contributors
last-verified: 2026-08-18
---

# ADR 0272: Material3D.SetTextureFilters — Public Sampler Filter Surface

## Status

Accepted (2026-08-18).

## Context

Material texture slots default to `MIP_FILTER_NONE`; only the
glTF-importer boundary (`rt_material3d_set_import_texture_slot_sampler_axes`)
could change it. Pixels-sourced textures (procedural grass/dirt/track/wall
kits) therefore sample base-level-only on every backend even though the
backends generate mip chains — visible as distance shimmer/aliasing on any
tiled surface in motion. Only `Material3D.Anisotropy` was public.

## Decision

New method `Zanna.Graphics3D.Material3D.SetTextureFilters(i64 min, i64 mag,
i64 mip)` (`rt_material3d_set_texture_filters`): sets the sampler filters
for **every** texture slot and the aggregate fields. Values: min/mag
`0=Linear, 1=Nearest`; mip `0=None, 1=Nearest, 2=Linear` (trilinear).
Out-of-range values fall back to the slot defaults (Linear/Linear/None)
rather than trapping. Mip modes engage only when the bound texture carries
a mip chain. Per-slot granularity stays importer-only until a real need
appears.

## Consequences

- Authored materials can opt into trilinear filtering
  (`SetTextureFilters(0, 0, 2)`) — Legacy Baseball's field/wall kits do.
- ABI manifest re-reviewed together with ADR 0271 (counts/hash recorded
  there); runtime reference docs regenerated.

## Links

- `src/runtime/graphics/3d/render/rt_material3d.c`, `rt_canvas3d.h`
- `src/il/runtime/defs/graphics3d/rendering.def`
- ADR 0271 — Sharpen postfx (same landing family)
