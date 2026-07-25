---
status: active
audience: contributors
last-verified: 2026-07-25
---

# ADR 0190: In-Memory Scene3D Text Serialization

## Status

Accepted (2026-07-25)

## Context

Zanna Studio's 3D scene editor keeps `Document.content` as the canonical VSCN
text and re-derives it after every accepted edit. Because the runtime exposes
only file-based endpoints (`SceneGraph.Save(path)`, `SceneGraph.Load(path)`,
`SceneAsset.LoadResult(path)`), the editor round-trips every commit through a
staged temporary file: serialize to a dot-prefixed sibling file, read the bytes
back, delete the file — and the mirror trick again on every undo, redo, and
document activation (ADR 0187 forced the sibling staging so relative prefab
references resolve). A VSCN document may legally reach 64 MB (embedded texture
maps), so each gizmo commit can cost two full disk passes plus directory churn
beside the user's scene, and crash timing can leak stale dot-files.

Internally the serializer already builds the complete document in one memory
buffer before writing, and the loader already has a buffer entry point
(`rt_scene3d_load_from_memory`, used by streaming) whose `filepath` parameter
is used only for diagnostics and prefab base-directory resolution. The missing
pieces are purely public API surface, which is ADR-gated.

## Decision

### Runtime surface (additive, no format change)

- `Zanna.Graphics3D.SceneGraph.SaveToText() -> String` — serialize the live
  graph to canonical VSCN text entirely in memory. Returns the exact bytes
  `Save(path)` would write (same version-selection rules). Returns the empty
  string on serialization failure or when the text would exceed the existing
  256 MB VSCN file budget — the same bound `Save(path)` enforces — matching
  the 2D `SceneDocument.ToJson` failure convention. No filesystem access.
  Studio separately keeps enforcing its own stricter 64 MB editing limit on
  the returned text, exactly as its staged read did.
- `Zanna.Graphics3D.SceneAsset.LoadTextResult(virtualPath, text) -> Result` —
  load VSCN text as a SceneAsset without touching the filesystem for the
  document itself. `virtualPath` supplies (a) the extension route, which must
  be `.scene3d`/`.vscn` — other extensions produce `Err`, never a trap — and
  (b) the base directory against which relative prefab references (ADR 0187)
  and diagnostics resolve; referenced prefab files themselves still load from
  disk with the existing cycle/depth/fan-out guards. Null arguments trap like
  the sibling loaders; recoverable failures return `Err(message)` through the
  shared asset-diagnostic channel.

Implementation is refactor-only around existing code: the body of
`rt_scene3d_save` splits into a build-text helper shared with
`rt_scene3d_save_text`, and `rt_model3d_load_impl` accepts an optional
preloaded VSCN text buffer exactly as it already accepts preloaded glTF/FBX
buffers, feeding `rt_scene3d_load_from_memory`.

### Editor contract

Studio's 3D editor replaces both staged temp-file paths:

- `CommitScene` uses `SaveToText`; a failed or over-budget serialization rolls
  back exactly as a failed staged write does today.
- Document (re)loads use `SceneAsset.LoadTextResult(documentPath, content)`;
  untitled documents pass their display name, accepting that relative prefab
  references cannot resolve until the document is saved (unchanged behavior —
  the temp file lived in the same directory only for saved documents).

Editor history additionally becomes byte-budgeted: undo/redo stacks retain at
most 100 snapshots as before, and now also drop oldest entries beyond an
aggregate 256 MB text budget so 64 MB documents cannot pin ~6.4 GB.

## Consequences

- One gizmo commit costs one in-memory serialization — no disk writes, reads,
  or sibling directory churn; no stale dot-file leakage on crash.
- Prefab resolution semantics are unchanged and now explicit: the virtual
  path names the resolution base instead of a staged file's accident of
  location.
- The text APIs are additive; existing file endpoints, VSCN versioning, and
  byte-exact output are untouched, so goldens and parity probes must not
  change.
- Streaming's private buffer loader gains a public, tested contract, which
  future work (asset servers, collaborative editing) can reuse.

## Tests

- Runtime unit: `SaveToText` equals the bytes `Save(path)` writes for scenes
  covering v2/v5/v6/v7 features; `LoadTextResult` round-trips `SaveToText`
  (including a prefab reference resolved beside the virtual path) and returns
  `Err` for wrong extensions and malformed text.
- Studio probes: existing 3D editor probes stay green with no temp files
  created during commits (dot-file staging removed); undo byte-budget probe.
