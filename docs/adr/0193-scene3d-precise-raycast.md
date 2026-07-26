---
status: active
audience: contributors
last-verified: 2026-07-25
---

# ADR 0193: Triangle-Accurate Scene Raycast Queries

## Status

Accepted (2026-07-25)

## Context

`SceneGraph.RaycastNodes` intersects the ray against each visible mesh
node's **world AABB** and returns the node with the smallest box entry
distance. That is the right cost model for gameplay-scale broad-phase
queries, but it is wrong as an editor picking primitive: clicking through
the visible gap of a torus, an archway, or any concave silhouette selects
the near node whose empty box corner the ray crosses, not the mesh the
user can actually see behind it. Zanna Studio's viewport picking
(`ViewportNodeAt`) is built on this query, so wrong-node selection is a
routine editor experience on real scenes.

The narrow-phase machinery already exists and is battle-tested at the
mesh level: `rt_raycast3d.c` implements Möller–Trumbore triangle
intersection behind a retained, geometry-revision-keyed BVH
(`Mesh3D.IntersectRay` → `rt_ray3d_intersect_mesh`), including
ray-into-object-space conversion through the model matrix, a
world-space exact linear fallback for singular transforms, and
`RayHit3D` results carrying distance, point, normal, and triangle index.
What is missing is the scene-level composition: walk the graph, prefilter
by world AABB, and run the per-mesh BVH query for surviving candidates.
Scene-level public API is ADR-gated, hence this document.

## Decision

### Runtime surface (additive)

Three sibling queries join `RaycastNodes` on
`Zanna.Graphics3D.SceneGraph`, sharing its argument contract (Vec3
origin, Vec3 direction normalized internally, non-negative finite
`maxDistance` where non-finite input means the scene maximum):

- `RaycastNodesPrecise(origin, direction, maxDistance) -> SceneNode` —
  the visible mesh node containing the **nearest intersected triangle**,
  or `null` when no triangle is hit. Nodes whose world AABB the ray
  misses are never narrow-phase tested; the AABB entry distance also
  prunes candidates that cannot beat the best triangle hit found so far.
- `RaycastNodesPreciseAll(origin, direction, maxDistance) -> Seq` — every
  visible mesh node with at least one triangle hit within range, sorted
  nearest-first (ties keep stable traversal order). This is the
  overlap-cycling primitive (editor Alt-click).
- `RaycastPreciseHit(origin, direction, maxDistance) -> RayHit3D` — the
  nearest triangle hit itself (world-space point, normal, distance,
  triangle index), or `null`. This feeds spawn-at-cursor and future
  surface/vertex snapping without a second query API.

Semantics shared by all three:

- Only `visible` nodes with mesh payloads participate — identical
  visibility semantics to `RaycastNodes`. Invisible subtrees are pruned
  wholesale during traversal (children of invisible nodes are skipped),
  matching the AABB query's stack-walk behavior.
- The spatial index, when enabled, supplies candidates from the ray's
  swept bounds exactly as `RaycastNodes` does; results are
  order-normalized so indexed and non-indexed traversals return the
  same nodes (sorting makes this observable only through `...All`).
- Distances are Euclidean world units along the normalized direction.
- Singular node world matrices fall back to the exact world-space
  linear triangle sweep already implemented for `Mesh3D.IntersectRay`;
  they never silently drop a node.

### Internal composition (no new geometry code)

`rt_raycast3d.c` refactors its private mesh-query context to carry a raw
`const double model[16]` instead of a boxed Mat4 and exports one internal
raw entry point (declared in `rt_canvas3d_internal.h`, not public API):
prepared world ray + `rt_mesh3d *` + raw model matrix in, hit
distance/triangle/world point out, zero allocations. The scene queries in
`rt_scene3d_query.c` reuse their existing traversal skeletons and call
this raw helper for candidates that survive the AABB prefilter. The boxed
`Mesh3D.IntersectRay` keeps byte-identical behavior — it now routes
through the same core with its transform's matrix pointer.

### Editor adoption (Zanna Studio)

`ViewportNodeAt` switches to `RaycastNodesPrecise`; Alt-click cycles
through `RaycastNodesPreciseAll` in nearest-first order; marquee
selection and hover feedback build on screen-projected node bounds
(workspace-only state, no scene mutation). Studio behavior itself is not
ADR-gated; it is recorded here only to fix the intended consumer.

## Consequences

- Editor clicks select what the pixel under the cursor shows, including
  through gaps and around concave silhouettes.
- Worst-case cost is bounded by the AABB prefilter times per-mesh BVH
  descent; the retained BVH is keyed to the mesh geometry revision, so
  repeated picks on a static scene do no rebuild work. The AABB-only
  `RaycastNodes` remains available where broad-phase cost is the point.
- `...All` allocates one Seq per call; callers that only need the
  nearest node should prefer `RaycastNodesPrecise`.
- New public surface requires the graphics3d ABI manifest and
  qualified-surface contract re-baselines and generated-docs
  regeneration; this ADR is the required record.

## Alternatives considered

- **Ray-march the depth buffer / GPU ID pass.** Pixel-exact against the
  rendered frame but couples picking to the render backend and frame
  cadence; unavailable headless. Triangle raycast is deterministic and
  backend-free. A GPU ID pass may later complement this for silhouette
  outlines (deferred; recorded in Studio status).
- **Return hit metadata from the node query (tuple).** The runtime
  surface has no tuple convention; `RayHit3D` already models hit data,
  so a sibling query returning it keeps every getter reusable.
- **Editor-side triangle testing via `Mesh3D.IntersectRay` per node.**
  Functionally equivalent but walks the graph in Zia, re-reading node
  matrices through the boxed API per node per click; the scene-level
  query keeps picking O(candidates) in C with zero boxing.
