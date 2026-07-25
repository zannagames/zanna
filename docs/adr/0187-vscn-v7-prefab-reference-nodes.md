---
status: active
audience: contributors
last-verified: 2026-07-24
---

# ADR 0187: Add VSCN v7 Prefab Reference Nodes

## Status

Accepted (2026-07-24)

## Context

Every VSCN import today is a copy: Studio's Import Asset grafts a deep copy
of the source scene, so ten placed lampposts are ten full duplicates, and an
edit to the source model reaches no placed copy. A Unity-competitive editor
needs prefab-style instancing: place a scene by reference, keep placements in
sync with the source, and store only the reference plus per-placement
overrides. VSCN v6 has no representation for "this subtree comes from another
file"; that is a format change and therefore an ADR-gated version bump.

## Decision

### Format (VSCN v7)

- A node may carry `"prefab": "<portable path>"` — resolved relative to the
  referencing file's directory (`ResolveAssetPath` rules; absolute paths are
  rejected at authoring time but tolerated at load with a diagnostic).
- Serialization writes a prefab node's own identity only: name, local TRS,
  visibility, typed metadata, and the `prefab` path. Its grafted children are
  **never serialized** into the referencing file. Other node payloads (mesh,
  material, light, camera, LOD) are not part of the v1 override surface and
  are not written on prefab nodes.
- Loading a prefab node loads the referenced scene and grafts deep copies of
  its root's children beneath the node, marking every grafted node
  **instance content** (a runtime flag, not a serialized member).
- Files declaring `"version": 7` load in current runtimes only; v5/v6 files
  load unchanged. The serializer writes version 7 only when at least one
  prefab node exists, so scenes without prefabs remain v6 byte-compatible.

Safety bounds, all diagnosed rather than trapped:

- **Cycles**: a load-path stack rejects self/circular references; the
  offending node becomes a placeholder.
- **Depth/fan-out**: nested prefab depth is capped at 8 and total grafted
  nodes per load share the existing scene node budgets; exceeding either
  produces placeholders.
- **Missing/invalid sources**: the node loads as an empty placeholder that
  **retains its `prefab` path**, gains a diagnostic, and round-trips
  byte-identically — a broken reference is never silently dropped.

One implementation in the VSCN loader serves every consumer:
`SceneGraph.Load`, `SceneAsset`, async asset handles, and world streaming all
resolve references identically.

### Editor workflow

- **Import as Instance** (beside the existing copy-import) creates a prefab
  node referencing the chosen `.scene3d`/`.vscn` and grafts its content.
- Instance content is **locked**: hierarchy rows show an instance badge,
  selection resolves to the owning prefab node (select-as-unit), and content
  nodes reject edits with a truthful message. The prefab node itself supports
  the full override surface: transform, name, visibility, metadata,
  components, duplication, deletion, reparenting.
- **Open Prefab Source** opens the referenced file as its own document.
  External changes to a referenced file refresh open instances through the
  existing external-change polling, preserving overrides.
- **Reload Instances** re-grafts on demand; a placeholder node offers
  Re-link (choose a new source) and Unpack (convert the instance to a plain
  editable copy, one transaction, removing the reference).

### Runtime surface

`SceneNode.PrefabPath` (read-only string, empty for ordinary nodes) and
`SceneNode.IsInstanceContent` (read-only bool) are registered so games and
tools can distinguish instances without parsing JSON.

## Consequences

- Repeated set-dressing stores one reference instead of N copies; source
  edits propagate on reload. File sizes for instance-heavy scenes drop by the
  duplicated content.
- The v1 override surface is deliberately narrow (transform, name,
  visibility, metadata). Per-descendant property overrides are recorded as an
  explicit deferral, not attempted.
- Runtime tests must pin: v7 round-trip byte stability, override-only
  serialization, cycle/depth/missing placeholders with retained references,
  budget behavior, v6 files staying v6, and a golden v7 fixture. A Studio
  probe pins the instance workflow (locked content, select-as-unit, unpack,
  re-link, external refresh).

## Alternatives Considered

- **Editor-only references (expand on save).** Rejected: saving expanded
  copies forfeits the entire benefit — files stay huge and source edits still
  reach nothing.
- **Full per-field override trees (Unity-complete prefabs).** Rejected for
  v1: the override diff/apply model is a large system; the narrow surface
  covers placement workflows now and can grow compatibly.
- **A separate `.prefab3d` container format.** Rejected: any scene is already
  a valid prefab source; a second format would split tooling for no
  representational gain.
