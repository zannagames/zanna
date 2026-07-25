---
status: active
audience: contributors
last-verified: 2026-07-24
---

# ADR 0184: Author Scene Camera Nodes with Look-Through and Preview

## Status

Accepted (2026-07-24)

## Context

`SceneNode.Camera` already attaches a `Camera3D` to a node and VSCN already
persists node cameras — glTF/FBX imports carry them today. But Studio cannot
create, edit, or even see a camera node: games that want an authored intro
vantage or cutscene framing must hand-write it. The light-authoring pattern
(ADR 0172) solved the identical problem for `SceneNode.Light`: independent
clone-safe drafts, one-transaction assignment, viewport markers.

## Decision

**Add Camera** joins the 3D editor's Add menu. It creates a node carrying an
independent `Camera3D` (never a shared imported instance — the light rule: an
edit constructs a replacement before assignment, so shared imported cameras
are never mutated in place).

A single-node **Camera inspector** authors the retained fields: projection
(perspective/orthographic), vertical fov or ortho size, near and far planes.
Every accepted edit is one canonical VSCN history transaction with exact
rollback and no-op awareness. Aspect ratio is not authored: it is
display-derived at use time, exactly as games already treat it.

Viewport representation, all workspace-only:

- Camera nodes draw a camera marker plus a **frustum wireframe** (near/far
  rectangles and edge lines for perspective; the box for ortho), projected
  through the shared viewport camera per ADR 0183, honoring the camera
  overlay toggle. Markers make meshless camera nodes selectable exactly as
  light markers do.
- **Look Through Camera** drives the editor viewport from the selected
  authored camera (pose from the node's world transform, projection from the
  authored fields). Editing continues normally; leaving look-through restores
  the prior editor pose. Fly input while looking through does not modify the
  authored node.
- A **preview inset** renders the selected camera's actual view through a
  second windowless Canvas3D into a corner picture-in-picture, bounded to at
  most 256 pixels on the long edge and refreshed at most every N editor
  frames with damage awareness. The inset is presentation only and can be
  toggled off.

Multi-node camera editing is explicitly out: the inspector states single-node
semantics like the light inspector did in its v1.

## Consequences

- Authored vantages (Ashfall's per-mission intro camera) become scene data
  with undo, visible frusta, and a truthful preview.
- Zero format work: persistence rides the existing VSCN camera tables.
- The preview inset reuses the windowless-Canvas3D machinery (ADR 0168) and
  its budgets; a probe must pin frustum-overlay projection, one-transaction
  edits with rollback, look-through pose isolation, inset bounds/refresh
  discipline, and that none of it dirties the scene.

## Alternatives Considered

- **Camera settings as node metadata instead of `SceneNode.Camera`.**
  Rejected: the runtime component exists and persists; metadata would fork
  the representation games already read.
- **Full-viewport play-through instead of an inset.** Rejected: Run Scene
  owns "see it in the game"; the inset answers "what does this camera frame"
  without leaving the editing context.
- **Authoring aspect ratio.** Rejected: aspect follows the output surface at
  runtime; baking it into scenes would make authored framing lie on other
  displays.
