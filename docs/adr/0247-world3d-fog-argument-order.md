---
status: active
audience: contributors
last-verified: 2026-08-13
---

# ADR 0247: Align World3D.SetFog with Canvas3D's Argument Order; Add ClearFog

## Status

Accepted (2026-08-13)

## Consulted

- `src/il/runtime/defs/game3d/world.def` — registry surface
- `src/runtime/graphics/3d/rt_game3d_world_api.inc` — forwarding

## Context

`Canvas3D.SetFog` takes `(near, far, r, g, b)`. `Game3D.World3D.SetFog` — a
thin forwarder to the same canvas state — took `(r, g, b, near, far)`. Both
signatures are five untyped `f64`s, so nothing at compile time or runtime
distinguished them: a caller who knew the Canvas3D order and wrote
`world.SetFog(700.0, 2800.0, 0.7, 0.75, 0.8)` silently set the fog COLOR to
`(700, 2800, 0.7)` and the planes to `(0.75, 0.8)` — fog at full density from
less than a foot away, in a blown-out color. This is the probable mechanism
behind the historical "fog saturates everything" observations that kept
distance fog turned off in downstream projects.

World3D also had no way to disable fog again; `Canvas3D.ClearFog` existed
with no World3D counterpart.

## Decision

1. `World3D.SetFog(near, far, r, g, b)` — the same order as
   `Canvas3D.SetFog`, everywhere, forever. The C forwarder
   `rt_game3d_world_set_fog` now takes the aligned order and forwards
   positionally.
2. Add `World3D.ClearFog()` forwarding to `rt_canvas3d_clear_fog`.
3. `test_game3d_world_fog_order.zia` pins the forwarding order through
   canvas readback (`FogNear`/`FogFar`/`FogColor`) and the ClearFog reset.

This is a breaking change to the World3D registry surface. All five in-repo
call sites (`walk_min`, `openworld_slice`, `game3d_showcase`,
`conformance_scene`, `soak_scene`) were reordered in the same change with
identical values, so rendered output — including the committed visual
baselines — is unchanged.

## Consequences

- Fog authored against either class now means the same thing; the
  five-untyped-f64 trap is gone from the surface (the types still cannot
  catch a future misuse — the readback test is the guard).
- Out-of-repo Zia code calling `World3D.SetFog` must reorder its arguments.
  The registry inventory (`zanna --dump-runtime-api`) reflects the new
  contract from the live binary.
- No IL opcode, grammar, or verifier changes. Runtime registry surface
  change: one method signature's meaning, one new method.

## Links

- ZB-22 in the Legacy Baseball engine-bug ledger
- ADR 0246 — 3D display-transfer contract (companion visual-correctness work)
