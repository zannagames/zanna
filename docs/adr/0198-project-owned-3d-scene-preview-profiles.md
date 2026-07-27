---
status: active
audience: contributors
last-verified: 2026-07-26
---

# ADR 0198: Add Project-Owned 3D Scene Preview Profiles

## Status

Accepted (2026-07-26)

## Context

The 3D scene editor can render canonical VSCN meshes, materials, cameras, and
lights, but a game often keeps its initial player view and environment values
in typed scene-root metadata. Studio cannot infer whether `af.startZ`,
`level.spawn.z`, or another project key is the gameplay anchor. It likewise
cannot know which metadata triples describe ambient light, a clear color, or
distance fog.

Fitting every node and enabling every gameplay overlay is a poor fallback for
large production scenes: authored geometry becomes distant, collider outlines
cover shaded art, and the first view bears little resemblance to the game.
Executing project code inside Studio would make authoring nondeterministic and
unsafe. A per-user mapping would make the same scene look different for each
collaborator.

## Decision

### Component-schema version 4

`scene-components.json` version 4 adds one optional top-level
`scenePreview3D` object. Its string members name typed float metadata keys on
the VSCN root:

- `startXProperty`, `startYProperty`, `startZProperty`, and optional
  `startYawProperty`;
- `ambientRProperty`, `ambientGProperty`, `ambientBProperty`;
- `clearRProperty`, `clearGProperty`, `clearBProperty`;
- `fogRProperty`, `fogGProperty`, `fogBProperty`,
  `fogNearProperty`, and `fogFarProperty`.

The start, ambient, clear, and fog groups are each all-or-none. Every supplied
key must be a portable metadata identifier of at most 128 characters.
`startYawProperty` requires the complete start triple. An optional numeric
`startEyeHeight` between -1,000 and 1,000 requires the complete start and yaw
mapping. It declares that the start is a gameplay foot anchor: Studio places
the initial camera at that height and looks along the mapped gameplay yaw.
Without it, the start triple remains an orbit target. Optional boolean
`showGrid`, `showMarkers`, `showLights`, `showCameras`, `showColliders`, and
`showRoutes` members set first-open overlay defaults. Missing booleans retain
Studio defaults.

Wrong-version, partial, malformed, duplicate, or oversized data rejects the
complete component schema, matching the existing fail-closed policy.

### Read-only viewport application

On the first open of a document without saved workspace state, Studio uses the
mapped start triple as its view target, or as the camera foot anchor when an
eye height is declared. Eye-anchored views convert gameplay yaw into Studio's
orbit convention and look forward at the existing bounded scale. Target-style
views use the mapped yaw directly and keep a useful bounded authoring pitch.
The declared overlay defaults apply only to that new workspace state;
subsequent tab and session restores preserve the user's exact choices.

For every shaded render, complete and correctly typed ambient, clear, and fog
groups configure the existing `Canvas3D` viewport. Missing or wrong-typed scene
metadata falls back to Studio's ordinary editor lighting and clear color.
Metadata values are clamped to the runtime's valid color and distance ranges.
The profile never writes VSCN metadata, scene bytes, history, revision, or
dirty state.

## Consequences

- Large game scenes can open at a meaningful gameplay location with their
  project-owned atmosphere instead of as a collider-covered overview.
- The mapping travels with source control and applies identically on macOS,
  Windows, and Linux without loading game code or new dependencies.
- Existing version 1–3 component schemas and scenes without a profile retain
  their current behavior.
- Studio probes must pin schema validation, wrong-typed metadata fallback,
  first-open camera/overlay defaults, render-state application, session
  preservation, and the no-dirty invariant.

## Alternatives Considered

- **Recognize Ashfall's `af.*` and `lenv.*` keys directly.** Rejected because
  project vocabulary must not leak into the general editor.
- **Copy editor-specific metadata into every VSCN file.** Rejected because the
  project schema is the single shared mapping and avoids duplicating paths.
- **Execute a game's environment setup in Studio.** Rejected because arbitrary
  startup code, side effects, timing, and runtime state are not a deterministic
  authoring contract.
- **Always hide all overlays and frame the origin.** Rejected because marker,
  route, and start conventions legitimately differ between projects.
