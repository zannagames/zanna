---
status: draft
audience: contributors
last-verified: 2026-08-03
---

# ADR 0237: VSCN v8 External Asset References

## Status

Proposed (design ready; not yet implemented)

## Context

VSCN v7 embeds every mesh and texture as base64 inside the scene file; only
`prefab` entries reference external files. This is the single largest gap
against Unity-class tooling: multi-megabyte scene files, the 64 MB editing
cap and 256 MB undo pinning in Studio, textures duplicated into every scene
that uses them, and re-importing an asset never updating existing
placements. The 2026-08 deep review ranked this the top strategic item.

## Decision (design)

### Format

Version 8 adds one optional lane to mesh and texture entries: a `"ref"`
string holding a path **relative to the scene file** (the prefab-path
resolution rules of ADR 0187 apply verbatim: no absolute paths, no `..`
escapes above the project root, forward slashes canonical). An entry
carries either embedded payload fields (v7 shape, unchanged) or `"ref"` —
never both; both present is a load error, not a preference.

- **Referenced formats:** exactly what `SceneAsset.LoadResult` accepts for
  meshes (glTF/GLB/OBJ/STL/FBX and `.scene3d`) and what the texture codecs
  accept (PNG/KTX2). One referenced file may satisfy many entries; the
  loader caches by resolved canonical path within one load.
- **Versioning:** the saver writes `"version": 8` only when at least one
  ref exists; a fully embedded scene stays v7 byte-compatible, so every
  existing golden and every v7 consumer is untouched until a scene opts in.
- **Failure policy:** a missing or invalid referenced file loads a
  placeholder (the magenta error material / unit error mesh), records the
  path on the scene's diagnostics list (`UnresolvedAssetCount` /
  `UnresolvedAssetPath(i)`, the prefab-diagnostics pattern), and never
  fails the whole scene. Byte-exactness guarantee: saving a scene with
  unresolved refs re-emits the refs untouched.

### Loader/saver

Load resolves refs after the node graph parses (prefab-hydration stage
ordering), with bounded IO through the existing safe-read layer and a
per-scene budget (refs count against the same 64 MB decode budget embedded
payloads use today — referencing raises the *file-size* ceiling, not the
*memory* ceiling). Save round-trips a ref entry byte-exactly; embedded
entries keep v7 serialization bit-for-bit.

### Studio workflow (phase 2)

- **Save With References…**: rewrites embedded textures/meshes that
  originated from importable files into refs beside the scene (asset paths
  recorded at import time in node metadata), shrinking the document in one
  undoable transaction.
- Import records `import.source` metadata so re-import-updates-placements
  becomes possible.
- The asset browser shows ref targets; a broken ref surfaces in the
  hierarchy with the same badge prefabs use.

### Phasing

1. Loader + saver + diagnostics + goldens (runtime only; hand-authorable).
2. Studio Save-With-References + import source tracking.
3. Re-import updates placements; dedup across scenes.

## Consequences

- The 64 MB cap stops being a level-size ceiling; undo snapshots shrink to
  reference size.
- A scene file stops being self-contained; packaging must walk refs (the
  prefab packaging walker generalizes).
- Every v7 file remains valid v8 input forever.

## Alternatives considered

- **A sidecar asset-pack file** (one binary blob beside the scene):
  rejected — it re-creates the duplication problem across scenes and adds
  a second file format for no authoring benefit.
- **Always-referenced v8** (no embedding): rejected — single-file scenes
  are genuinely useful for fixtures, goldens, and small demos, and the
  migration burden would be enormous.
