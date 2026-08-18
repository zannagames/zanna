---
status: active
audience: contributors
last-verified: 2026-08-17
---

# ADR 0201: Add Project-Owned 3D Lens and Atmosphere Previews

## Status

Accepted (2026-07-26)

## Context

ADRs 0198 and 0200 let a project give Studio a gameplay start pose, sky
palette, distance fog, and key/fill light rig without teaching the editor one
game's metadata vocabulary. Live comparison with Ashfall still showed three
material differences: Studio retained its 60-degree editor lens instead of the
game's 90-degree lens, PBR preview materials lacked the game's bounded sky IBL,
and the ash-floor height fog was absent. The resulting scene was technically
complete but did not resemble the frame a player sees.

Studio cannot infer these choices reliably. Executing project camera,
post-processing, or environment code would make scene inspection unbounded and
nondeterministic. Hard-coding Ashfall's constants or root keys would also turn
project policy into editor policy.

## Decision

### Component-schema version 7

`scene-components.json` version 7 extends `scenePreview3D` with:

- optional `cameraFov`, a static perspective field of view in degrees in
  `[1, 179]`;
- optional `iblIntensity` in `[0, 8]`, which requires the complete version-6
  sky palette; and
- one optional complete height-fog group:
  `heightFogProperty`, `heightFogBase`, `heightFogFalloff`,
  `heightFogDensity`, and `heightFogOpacity`.

`heightFogProperty` uses the existing portable 128-character metadata-key
contract. The scene value must be typed Boolean. Base is bounded to
`[-1,000,000, 1,000,000]`, falloff and density to `[0, 100]`, and opacity to
`[0, 1]`. Any version-7 member in an older schema, a partial height-fog group,
IBL without a complete sky palette, a wrong JSON kind, or an out-of-range
number rejects the complete schema under the existing fail-closed policy.

### Read-only viewport application

The validated lens replaces Studio's ordinary 60-degree lens for that
project's perspective scene view. Camera placement, manual projection,
rendering, overlays, and picking all derive from the same active lens so the
pixels-per-world-unit contract remains internally consistent. Orthographic
views remain unchanged.

When a generated project sky or canonical authored skybox is present, the
validated IBL value configures only the Studio render canvas. A zero value
disables preview IBL. Height fog is enabled only when the complete schema group
exists and the mapped root Boolean is present with the correct type and true;
otherwise Studio explicitly clears retained height fog.

All three settings are presentation state. They never join the canonical
`SceneGraph`, VSCN serializer, hierarchy, history, revision, dirty state,
session payload, or runtime scene. Studio still does not import or execute
project code.

## Consequences

- A project can align Studio's perspective composition and broad atmospheric
  material response with its game while retaining a bounded generic editor.
- Reusable render canvases cannot leak height fog or preview IBL between
  scenes.
- Existing version 1–6 schemas retain their 60-degree lens and current
  environment behavior.
- Studio probes must pin version gating, complete-group validation, lens
  projection, IBL/height-fog canvas state, wrong-typed root fallback, and the
  no-dirty invariant.

## Alternatives Considered

- **Infer FOV and IBL from the first camera node.** Rejected because gameplay
  cameras are often runtime-owned and authored cinematic cameras may use a
  different lens.
- **Execute the project's environment setup.** Rejected because arbitrary game
  code is not a bounded editor-preview contract.
- **Apply height fog whenever distance fog exists.** Rejected because the two
  effects have independent semantics and many scenes intentionally use only
  distance fog.
- **Bake these values into every VSCN.** Rejected because derived editor
  presentation would duplicate or change runtime-owned state.
