---
status: active
audience: contributors
last-verified: 2026-07-27
---

# ADR 0204: Add Project-Owned 3D Post-Processing Previews

## Status

Accepted (2026-07-27)

## Context

Studio can reproduce a project's canonical scene, transient node and
environment prefabs, gameplay camera, sky, lights, IBL, and fog without
executing project code. That still leaves a large visual mismatch for games
whose identity is established during frame finalization. Ashfall's saved scene
and Studio viewport contained the same terrain and actors, but Studio omitted
the game's ACES tonemap, bloom, color grade, vignette, and FXAA. The resulting
saturated orange editor view did not resemble the ash-white game frame.

Executing a game's renderer or importing its effect module would give project
code authority inside Studio and make previews dependent on startup state.
Hard-coding Ashfall's recipe would couple the editor to one project.
Backend-only effects such as SSAO and SSR would also make the same project
schema render differently on machines without the required GPU path.

## Decision

### Component-schema version 10

`scene-components.json` version 10 extends `scenePreview3D` with optional,
static post-processing groups:

- `tonemapMode` and `tonemapExposure`;
- `bloomThreshold`, `bloomIntensity`, and `bloomPasses`;
- `colorGradeBrightness`, `colorGradeContrast`, and
  `colorGradeSaturation`;
- `vignetteRadius` and `vignetteSoftness`;
- `fxaa`.

Every multi-value group is complete or absent. Tone-map mode is an integer in
`[0, 2]`; exposure, bloom threshold, and bloom intensity are bounded to
`[0, 16]`; bloom passes are an integer in `[0, 32]`; color-grade brightness is
in `[-1, 1]`; contrast and saturation are in `[0, 4]`; vignette radius is in
`[0, 1]`; and vignette softness is in `[0.001, 1]`. `fxaa` is an exact boolean.
Wrong-version, malformed, fractional-integer, incomplete, or out-of-range
values reject the complete component schema without publishing a partial
profile.

Studio constructs effects in the fixed portable runtime order: tonemap,
bloom, color grade, vignette, then FXAA. This order matches the project recipes
the feature targets and is independent of JSON member order. Schema v10 does
not expose SSAO, SSR, depth of field, temporal antialiasing, or motion blur;
those passes depend on depth, history, or backend capability and cannot yet
promise the same authoring preview across macOS, Windows, Linux, software, and
accelerated renderers.

### Retained viewport presentation

The 3D scene editor builds one `PostFX3D` chain from validated schema values
when the schema or retained offscreen canvas changes. The chain is attached to
the main shaded or shaded-wire viewport before canonical and transient preview
graphs draw. Pure wireframe rendering detaches it so topology remains legible;
returning to a shaded mode reuses the retained chain. Editor gizmos, selection
outlines, labels, and other 2D overlays are composed after render-target
readback and therefore remain crisp and ungraded.

The camera inset remains an unprocessed inspection surface. Post-processing is
presentation only: it never enters the canonical graph, VSCN bytes, revision,
dirty state, or undo history. Component-schema authoring preserves the raw
profile and infers version 10 whenever any post-FX member is present.

The implementation uses the existing `PostFX3D` and `Canvas3D.SetPostFX`
runtime APIs. It adds no runtime C ABI, external dependency, scene format, or
platform-specific branch.

## Consequences

- Project-owned scene previews can reproduce the same portable color pipeline
  as their games, closing a major gap between authored and played frames.
- Post-FX allocation is retained instead of repeated every render frame.
- Software probes and accelerated Studio builds consume the same validated
  effect order and values.
- Versions 1–9 and version-10 profiles without active effects preserve prior
  rendering behavior.
- Tests must pin version gating, group completeness, numeric bounds,
  structured-authoring preservation, retained effect count, real-project
  loading, and canonical-content isolation.

## Alternatives Considered

- **Execute the project's post-FX module in Studio.** Rejected because project
  code, mutable options, and renderer startup are not a safe editor contract.
- **Copy Ashfall's constants into the editor.** Rejected because visual
  conventions belong to project-owned data.
- **Expose every runtime effect immediately.** Rejected because backend-only
  and temporal passes would violate deterministic cross-platform previews.
- **Bake post-processing into preview textures or materials.** Rejected
  because screen-space tonemapping, bloom, and antialiasing cannot be
  represented faithfully as canonical material edits.
