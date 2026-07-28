---
status: active
audience: contributors
last-verified: 2026-07-27
---

# ADR 0211: Author Terrain as a Canonical Scene Mesh

## Status

Accepted (2026-07-27)

## Context

Zanna Studio needs direct terrain creation and sculpting in the 3D scene
editor. The terrain visible while authoring must be the same surface a game
loads from the saved scene: a separate editor preview, heightmap sidecar, or
project-only environment stand-in can drift from the shipped geometry and
make placement, lighting, collision conventions, and screenshots misleading.

The runtime has a standalone `Terrain3D` value with chunking, LOD, holes, and
splatting, but a VSCN `SceneNode` persists an attached `Mesh3D`, material,
transform, light, camera, and typed metadata. It does not persist a
`Terrain3D` attachment. Adding that attachment would require a new scene-format
and runtime ABI design, while Studio needs a useful exact terrain workflow now.

An arbitrary imported mesh cannot safely be treated as a heightfield merely
because its vertex and triangle counts happen to resemble a grid. Destructive
sculpting therefore needs an explicit, typed, versioned convention whose mesh
topology can be validated before editing.

## Decision

Studio terrain is an ordinary VSCN `SceneNode` whose `Mesh3D` is the canonical
source of truth. The node carries this exact typed metadata:

| Key | Kind | Value |
|-----|------|-------|
| `terrain.kind` | string | `heightfield-mesh` |
| `terrain.version` | integer | `1` |
| `terrain.columns` | integer | X sample count |
| `terrain.rows` | integer | Z sample count |
| `terrain.spacing` | float | local distance between samples |

Version 1 uses these rules:

- The grid contains 9 through 65 samples on each axis. A newly created terrain
  is 33 by 33 samples with spacing 1.0.
- Vertices are row-major in centered node-local XZ space. Their Y coordinate is
  the authored height, bounded to -1024 through 1024.
- Every cell contains two upward-wound triangles in the fixed `(a,d,b)` and
  `(a,c,d)` order, with UVs spanning the complete grid and normals recalculated
  after a rebuild.
- A node is sculptable only when every metadata key has the exact kind and
  version, the sample and spacing values are in range, and vertex and triangle
  counts exactly match the declared grid. Unrelated or incomplete meshes remain
  protected.
- Regenerate Flat may replace the canonical mesh using new bounded sample
  counts and spacing. Flatten All preserves topology and assigns one bounded
  height to every sample. Each accepted action is one ordinary scene history
  transaction.

The viewport tool supports Raise, Lower, Smooth, and Flatten brushes. Radius,
strength, and flatten level are node-local values. Smooth reads an immutable
input height list so results do not depend on vertex traversal order. Holding
Shift temporarily swaps Raise and Lower.

A sculpt press captures the pointer and the original mesh. Dabs update the live
mesh for immediate shaded feedback, while evenly spaced path resampling avoids
gaps and a fixed per-frame dab budget bounds work. Release serializes exactly
one VSCN history entry when heights changed. Escape restores the original mesh
identity and creates no history. Brush mode, radius, and tool activation are
workspace presentation state rather than scene metadata.

No runtime C ABI or VSCN format revision is added by this decision. Games load
and draw the same ordinary scene mesh, material, transform, and typed metadata
that Studio edits. A future optimized `Terrain3D` conversion must be explicit
and must define how the canonical mesh and its richer runtime representation
stay synchronized.

## Consequences

- Studio and game scene loading share exact terrain vertices, triangles,
  normals, material, transform, and metadata; there is no preview sidecar to
  diverge.
- Existing SceneGraph rendering, precise raycasts, surface placement, save,
  recovery, undo, clipboard, and import behavior work without a new runtime
  attachment.
- VSCN snapshots contain the complete bounded mesh, so dense terrain consumes
  more document and undo-history space than a compact heightmap.
- The 65-by-65 ceiling keeps synchronous viewport rebuilds predictable but is
  not a large-world streaming solution.
- Runtime `Terrain3D` splat layers, holes, chunk LOD, and streaming are not yet
  visually authored by this tool. Games may consume the canonical mesh
  directly or perform a deliberate project-specific conversion.

## Alternatives considered

- **Persist `Terrain3D` directly on `SceneNode`.** This can eventually expose
  runtime splatting, holes, and LOD, but requires a versioned scene attachment,
  C ABI, serializer, clone, import, renderer, and compatibility design beyond
  this bounded Studio milestone.
- **Keep a heightmap or metadata recipe beside an editor preview mesh.**
  Rejected because two persisted representations can disagree about the
  surface the game actually uses.
- **Treat any regular-looking imported mesh as terrain.** Rejected because
  count-only inference can destructively reorder or flatten unrelated assets.
- **Add general mutable vertex setters to `Mesh3D`.** Rejected for this
  workflow because rebuilding a bounded immutable draft gives exact
  cancellation and history ownership without exposing partially mutated
  renderer storage.
