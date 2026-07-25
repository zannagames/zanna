---
status: active
audience: contributors
last-verified: 2026-07-24
---

# ADR 0182: Adopt Canonical .scene2d and .scene3d Extensions

## Status

Accepted (2026-07-24)

## Context

Zanna's two scene formats grew their extensions historically: 2D scene
documents use `.scene` (with `.level` as an accepted twin) and 3D scenes use
`.vscn`, a name inherited from an internal codename era. None of the three
says what the file is. As the scene editors become the primary authoring
surface for both dimensions, the extensions should state the format and the
dimension at a glance, and the pair should be visibly symmetric.

Extension knowledge is concentrated in a small set of dispatch points: the 3D
model-loader extension dispatch and its async-cache accepted list, streaming
proxy naming, the `zanna asset` CLI text, and Studio's
`services/file_utils.zia` kind detection plus per-kind default extension
(which drives New and Save As). The 2D `SceneDocument` loader never gates on
extension at all.

## Decision

The canonical extensions become **`.scene2d`** for 2D scene documents and
**`.scene3d`** for 3D (VSCN) scenes.

`.scene`, `.level`, and `.vscn` remain **accepted legacy aliases
indefinitely**. Nothing that opens, loads, imports, streams, bakes, or
migrates a scene may drop support for the legacy extensions; they continue to
be detected everywhere the canonical extensions are.

Canonical-forward behavior:

- Studio's kind detection maps `.scene2d` to the 2D editor and `.scene3d` to
  the 3D editor; the per-kind default extension used by New and Save As
  becomes the canonical one, and save-hint messages list canonical first with
  legacy alternatives named.
- The 3D model dispatch (`SceneAsset`/async handles/`Assets3D`) accepts
  `.scene3d` wherever it accepts `.vscn`; streaming cell manifests may name
  either, and proxy naming (`*_proxy` bake outputs) preserves whichever
  suffix the cell uses.
- First-party exporters and examples emit canonical extensions.
  `xenoscape-scenes` renames its region files to `.scene2d`;
  `examples/3d/openworld_slice` deliberately stays on `.vscn` as the living
  legacy-compatibility proof and says so in its README.
- Documentation states "canonical `.scene2d`/`.scene3d`; `.scene`, `.level`,
  and `.vscn` remain accepted" wherever extensions are described. The format
  names ("scene document JSON schema v1", "VSCN v6/v7") do not change; only
  the canonical file naming does.

## Consequences

- File listings, editors, and tooling read unambiguously; the 2D/3D pair is
  symmetric and self-describing.
- No repository, project, or user file breaks: legacy extensions load
  forever, and the schema-migration assistant, Run Scene, asset browsers, and
  component schemas treat both spellings identically.
- One new runtime test pins `.scene3d` acceptance through the blocking and
  async model-load paths; Studio probes pin the new New/Save As defaults.
- A repo-wide re-grep for stale extension references gates the change.

## Alternatives Considered

- **Hard rename with no legacy acceptance.** Rejected: it would break every
  existing project, fixture, and manifest for a purely cosmetic gain.
- **Keep `.vscn` and only add `.scene2d`.** Rejected: the asymmetry is the
  problem; `.vscn` is the least self-describing of the three.
- **Version the extension (`.scene3d7`).** Rejected: the version lives inside
  the file where load-time validation reads it; encoding it in the name would
  force renames on every format bump.
