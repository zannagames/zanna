---
status: active
audience: contributors
last-verified: 2026-08-17
---

# ADR 0177: Add Scene Camera and Lighting Section Contracts

## Status

Accepted (2026-07-24)

## Context

Canonical 2D scene JSON reserves `camera` and `lighting` as named rich
sections, but neither has a schema, a typed accessor, or a consumer contract.
Games therefore hardcode camera configuration and lighting atmosphere in code
even when every value is plain per-scene data — Xenoscape configures regional
darkness in `lighting.zia` and camera behavior in `camera.zia` while its scene
metadata lives elsewhere. The runtime already has the consuming classes:
`Zanna.Graphics.Camera` (bounds, deadzone, smooth-follow, zoom) and
`Zanna.Game.Lighting2D` (darkness, tint, player light, dynamic lights).

Placed point lights do not belong in a section: they are positioned scene
content that should get placement, dragging, undo, clipboard, and hierarchy
behavior for free. Only scene-global settings need section fields.

## Decision

Define version-free minimal schemas for the two sections, holding scene-global
settings only, with every field optional:

```json
"camera": {
  "mode": "follow" | "fixed",
  "minX": 0, "minY": 0, "maxX": 9600, "maxY": 1024,
  "deadzoneWidth": 96, "deadzoneHeight": 64,
  "followLerpPct": 12,
  "zoomPct": 100
}
"lighting": {
  "darkness": 160,
  "tint": 4278854912,
  "playerLightRadius": 220,
  "playerLightColor": 4294962892
}
```

Field contracts map one-to-one onto the consuming runtime APIs: camera bounds
onto `Camera.SetBounds` (all four present or the bounds fields are rejected as
a group), deadzone onto `SetDeadzone`, `followLerpPct` (1..100) onto
`SmoothFollow`, `zoomPct` (10..1000) onto `SetZoom`; `darkness` is 0..255,
colors are 32-bit ARGB integers, and `playerLightRadius` is 0..4096 pixels.
Coordinates are world pixels bounded by the existing scene coordinate limits.

`SceneDocument` gains typed accessors following the exact-kind discipline of
ADR 0158, one getter/setter pair per field plus explicit absence handling:

```zia
CameraFieldKind(key) -> String        // "int", "string", or "" when absent
CameraGetInt(key, default) -> Integer
CameraGetStr(key, default) -> String
CameraSetInt(key, value)
CameraSetStr(key, value)
CameraRemove(key)
CameraKeys() -> Seq
LightingFieldKind(key) -> String
LightingGetInt(key, default) -> Integer
LightingSetInt(key, value)
LightingRemove(key)
LightingKeys() -> Seq
```

Setters validate the known-field ranges above and reject out-of-range values
as edit-time warning no-ops. Unknown keys are accepted within the existing
property key/count limits so games may extend the sections; unknown members in
loaded files are retained and re-emitted verbatim (same preservation rule as
ADR 0176). Empty sections are omitted from JSON. Serialization is canonical:
lexicographic keys inside each section, fixed rich-section order.

Placed point lights are ordinary objects carrying a project `light` component
(ADR 0160 schema, e.g. `light.radius`, `light.color`, `light.flickerPct`),
consumed by game code through the existing typed object property APIs and
mapped onto `Lighting2D.AddLight`. The sections never enumerate lights.

Application stays game-owned. `SceneDocument` does not construct or configure
`Camera` or `Lighting2D` instances, consistent with the existing rule that
scene documents never instantiate game classes; the schema is the adapter
contract, and the mapping table above is documented in
`docs/zannalib/game/scene.md`.

Studio authors both sections through inspector groups whose accepted edits
serialize once and create one history entry each, and may draw workspace-only
overlays (camera bounds rectangle, light halos) that never change canonical
bytes.

## Consequences

- Per-scene atmosphere and camera framing become authored data with undo,
  instead of per-game code tables.
- Games adopt the sections incrementally: absent fields leave existing code
  defaults untouched.
- Point lights inherit every existing object workflow (selection, drag,
  duplicate, clipboard, components) with zero new placement UI.
- Runtime tests must pin field validation ranges, absence semantics, unknown
  member retention, and canonical ordering; a Studio probe must pin the
  inspector transaction and overlay-neutrality rules.

## Alternatives Considered

- **A `lights` array inside the `lighting` section.** Rejected: it would
  duplicate the object system (position editing, undo, copy/paste) for one
  content type and leave those lights invisible to object tooling.
- **Scene-level properties instead of sections.** Rejected: the named
  sections already exist for exactly this data, and flat `camera.*` property
  keys would collide with the game-owned property namespace.
- **Automatic application to runtime Camera/Lighting2D during
  `BuildTilemap()`.** Rejected: `BuildTilemap()` returns a render/collision
  copy; camera and lighting objects are frame-loop state owned by the game,
  and implicit configuration would hide load-order dependencies.
- **A versioned envelope per section.** Rejected: every field is optional and
  independently validated; a version number would add migration ceremony to
  sections designed for incremental adoption.
