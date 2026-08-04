---
status: active
audience: contributors
last-verified: 2026-08-03
---

# ADR 0236: Studio 3D Authoring Round Three

## Status

Accepted (2026-08-03)

## Context

ADR 0234 closed the prefab/placement/play-input holes; this round lands the
remaining editor-parity items from the 2026-08 review: multi-node layout
commands, a group pivot policy, import settings, a first animation surface,
and baked-navmesh visualization.

## Decisions

### Arrange (align/distribute) and arrow-key nudges

An always-direct **Arrange…** menu offers Align X/Y/Z to Primary and
Distribute X/Y/Z Evenly. `ArrangeSelectedNodes(op)` mirrors the 2D editor's
command: align anchors on the primary selection, distribution keeps both
extrema and derives every intermediate from them (`scene_layout_3d`, the
float twin of `scene_layout_2d`). Exact-or-reject: all selected nodes must
share one parent (local coordinates are only comparable within one frame)
and instance content refuses. One commit; no-ops create no history.
Arrow keys nudge the selection along world X/Z (PageUp/Down for Y,
Shift ×10) through the same deterministic Move command path as gizmo drags,
one history entry per keypress.

### Group pivot policy

A **Pivot: Own/Primary/Center** cycle button selects the pivot for
multi-selection world-space Rotate/Scale: each node's own origin (the
long-standing default), the primary node's pivot (satellites orbit the
anchor), or the selection centroid (computed from captured start-of-edit
world positions so mid-edit motion cannot skew it). Shared-pivot scale uses
the primary's captured world scale so one common factor applies about the
shared point. Local-space group edits keep per-node semantics — orbiting in
mixed parent frames is world-space math by definition. Workspace-only
state; `TrySetWorldMatrix`'s exact-or-reject shear policy is unchanged.

### Import settings v1

Two view-options controls apply inside the import transaction, before
spawn placement measures bounds: a uniform **Import scale** factor
(multiplies every merged root's scale and position, preserving multi-root
layout) and a **Z-up import** conversion (roots rotate −90° about X;
positions map (x, y, z) → (x, z, −y)). Defaults (1.0, off) are exact
no-ops.

### Animation surface v1

An **Animation** inspector group appears while the scene carries baked
clips: prev/next clip cycling with a name/duration label, and a Play/Stop
preview that binds one `NodeAnimator3D` to the selected node's subtree and
advances it a fixed step per pump. Playback is workspace-only — Stop (or
losing the clips on a document switch) reloads the canonical document, so
preview motion can never reach VSCN bytes or history. Skeletal
`Animation3D` clips enumerate by name but refuse the workspace preview with
an explicit message (they are `AnimController3D`-driven; preview them in
embedded Play). A timeline/keyframe editor remains future work.

### Navmesh overlay

A **Navmesh** view option imports the scene's baked `.vnavmsh` sidecar and
draws it through `NavMesh3D.DebugDraw` inside the render frame; an
in-editor re-bake refreshes an active overlay in place. Missing sidecars
report "bake first" instead of failing silently.

### Bake threading (assessed, deferred)

The lightmap bake already amortizes across pump ticks (`PumpBakeSteps`);
the nav bake is a single synchronous call. Moving it off the UI pump needs
a runtime-level incremental bake API — runtime 3D surfaces assert
main-thread affinity, so a worker-thread bake from Studio is not an option.
Deferred until the runtime offers chunked nav baking.

## Consequences

- Probes: `scene_editor_3d_probe` part 4 gains `VerifyGroupPivotAndArrange`
  (align anchor semantics, distribution extrema, mixed-parent refusal,
  all three pivot policies, and full undo-stack restitution — the serial
  fixture's next scenario begins with Undo of its predecessor's commit);
  `scene_dnd_probe` gains adjusted-import and clip-enumeration/refusal
  cases; `scene_bake_environment_probe` gains the overlay toggle round trip.
- New shared module `scene_layout_3d.zia`; new contract slot
  `ArrangeSelectedNodes` (06).
