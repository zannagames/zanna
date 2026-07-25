---
status: active
audience: contributors
last-verified: 2026-07-24
---

# ADR 0185: Author Colliders and Gameplay Overlays on Scene Nodes

## Status

Accepted (2026-07-24)

## Context

`Collider3D` exists only as a physics-world object attached to bodies at
spawn time; VSCN does not persist colliders, and no editor surface authors
them. Games like Ashfall treat almost every placed mesh as both visual and
collider, plus a rich set of positional gameplay data (patrol routes, cover
points, spawn markers) that the 2D program made visible (ADR 0179 routes and
halos) but the 3D viewport still renders as anonymous origin markers.

Typed node metadata (VSCN v6) plus project component schemas (ADR 0178) can
carry all of this without a format change, exactly as the 2D `light`
component convention did — the missing pieces are an inspector surface and
truthful viewport overlays.

## Decision

### Collider convention

A node's collider is described by typed metadata:

- `collider.kind` — enum: `box`, `sphere`, `capsule`, `mesh-bounds`.
- Dimensions by kind: box `collider.halfX/halfY/halfZ`; sphere
  `collider.radius`; capsule `collider.radius` + `collider.height`.
  `mesh-bounds` derives the box from the node's mesh bounds at consumption
  time and authors no dimensions.
- `collider.trigger` — bool (overlap-only sensor), default false.
- Dimensions are node-local; the node's world transform applies at
  consumption, matching how meshes already scale.

A **Collider inspector group** authors these fields with kind-aware inputs
through the canonical single-transaction metadata path, and a `collider`
component in project schemas enables batch **Add Missing** across selections.
The consumption contract is documented for game adapters: derive a
`Collider3D` (or equivalent body shape) at spawn from the metadata, using
`SceneNode` world transforms and mesh bounds for `mesh-bounds`. The editor
never instantiates physics.

### Gameplay overlays

Workspace-only viewport overlays through the shared projection (ADR 0183),
each honoring the overlay toggles:

- **Collider wireframes**: box edges, sphere three-circle wire, capsule
  profile, in a distinct color; trigger colliders render dashed-style
  (alternating segments).
- **Route polylines**: a node whose project component is `route` draws an
  ordered polyline through its direct children (the 2D convention of ADR
  0179 lifted to 3D), highlighted when the route or a waypoint is selected;
  at most 256 drawn waypoints per route.
- **Marker badges**: nodes carrying recognized gameplay components keep their
  existing origin markers but gain a compact type badge in the hierarchy and
  a per-kind marker tint in the viewport, so spawns, covers, and gates are
  distinguishable at a glance.

## Consequences

- Ashfall's entire physical and tactical layer (colliding geometry, covers,
  flanks, routes) becomes visible and editable in Studio without touching the
  VSCN format.
- Adapters get one documented derivation rule instead of per-game guesswork;
  `mesh-bounds` makes the common "collider equals the mesh" case free.
- A probe must pin: kind-aware field validation and one-transaction edits,
  wireframe projection in both viewport modes, route polyline order following
  sibling order, badge/tint neutrality (no scene mutation), and toggle
  behavior.

## Alternatives Considered

- **A typed `SceneNode.Collider` component with VSCN v7 persistence.**
  Rejected for now: metadata already round-trips, needs no format change, and
  keeps collider semantics game-owned; a typed component can be promoted
  later without breaking the metadata convention.
- **Deriving colliders implicitly from every mesh.** Rejected: visual-only
  dressing is common (Ashfall's non-colliding caps and skirts); implicit
  colliders would make the editor assert physics the game never creates.
- **Editor-side physics preview.** Rejected: simulation belongs to the game;
  the editor's job is truthful geometry display.
