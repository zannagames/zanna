---
status: active
audience: contributors
last-verified: 2026-07-24
---

# ADR 0188: Add the Studio Bake and Environment Workflow

## Status

Accepted (2026-07-24)

## Context

The runtime already owns baked global illumination end to end:
`LightBaker3D` is a deterministic CPU path tracer (texels-per-unit, samples,
bounces, sky color, explicit lights, chunked `BakeStep()` with `Progress`,
`Apply()`), `LightProbeGrid3D` bakes an SH-9 grid with `.vlpg` save/load, and
the results persist without any format work: `Apply()` writes chart UV1
directly into mesh vertices (which VSCN serializes as raw vertex blobs) and
installs the atlas on per-node `Material3D` instances (whose lightmap slots
VSCN serializes). `Canvas3D` consumes skyboxes and IBL. What is missing is
entirely authoring: no Studio surface runs a bake, authors a probe grid, or
assigns an environment.

One sharp edge shapes the design: the baker writes chart UVs **into mesh
vertices**, so two nodes sharing one mesh would overwrite each other's
charts. Baking therefore requires per-node-unique meshes.

## Decision

### Bake panel

A Bake group in the 3D inspector:

- Settings: texels per unit, samples, bounces, sky color — persisted as
  scene root metadata (`bake.*`) so projects keep their bake configuration.
- **Shared-mesh preflight**: baking first scans for meshes shared by
  multiple baked nodes. If any exist, Bake refuses with the offending node
  list and offers **Make Meshes Unique for Baking** — one canonical VSCN
  history transaction that deep-copies shared meshes per node (exact
  rollback; a no-op when meshes are already unique).
- **Bake** constructs a `LightBaker3D` over the live SceneGraph, feeds every
  authored `SceneNode.Light` **in world space** (light-local values
  transformed through each node's `WorldMatrix`, matching how `Draw` treats
  node lights), then runs `BakeStep()` chunks inside the editor pump under a
  bounded per-frame time budget with live progress and Cancel. The scene
  stays fully editable-looking but bake-affecting edits are rejected with a
  truthful message while a bake is active.
- **Apply** commits the bake as **one** canonical VSCN transaction: chart
  UVs, per-node material instances with the atlas, and the `bake.*` settings
  together; Cancel or a failed apply restores the exact prior document.
  Reloading the saved scene shows the persisted lightmaps with no rebake.

### Probe grid

A probe-grid node convention (`probes.min*/max*/spacing` typed metadata,
authored through a small inspector group with a bounds wireframe overlay).
**Bake Probes** runs `LightProbeGrid3D.Bake` against the last baker and saves
`<scene>.vlpg` beside the scene file; games load it with the existing
`LightProbeGrid3D.Load`. The sidecar write is explicit and reported; it never
dirties the scene document.

### Environment

Scene-global environment lives in root-node typed metadata:

- `env.skybox` — asset path (authored through the bounded asset browser),
- `env.iblEnabled` — bool, `env.iblIntensity` — float.

The editor viewport applies the skybox/IBL live (workspace-only render
state, honoring the overlay toggles); games read the same metadata and call
`Canvas3D.SetSkybox`/`IblEnabled`/`IblIntensity` — the documented adapter
contract, consistent with ADR 0177's camera/lighting stance that application
stays game-owned.

## Consequences

- Baked GI and environment setup become one-panel workflows over runtime
  machinery that already guarantees determinism and persistence.
- The make-unique preflight makes the shared-mesh constraint visible and
  fixable in one undo step instead of a silent wrong bake; instance-economy
  scenes (shared meshes) simply opt in when they want baking.
- A probe must pin: preflight refusal + make-unique transaction, monotone
  progress with bounded per-frame stepping, cancel restoration, apply as one
  transaction whose lightmaps survive save/reload, `.vlpg` sidecar behavior,
  env metadata round-trip, and live viewport application staying
  workspace-only.

## Alternatives Considered

- **Blocking one-shot bake.** Rejected: `BakeStep` exists precisely so the
  editor can stay responsive with truthful progress and cancel.
- **Automatic silent mesh un-sharing.** Rejected: it mutates authored
  structure as a side effect of an unrelated action; an explicit undoable
  preflight keeps the author in charge.
- **A separate baked-scene output file.** Rejected: VSCN already persists
  every bake product in place; a second artifact would drift from its scene.
