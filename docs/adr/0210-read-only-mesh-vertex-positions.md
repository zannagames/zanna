---
status: active
audience: contributors
last-verified: 2026-07-27
---

# ADR 0210: Expose Read-Only Mesh Vertex Positions for Authoring Tools

## Status

Accepted (2026-07-27)

## Context

Zanna Studio can inspect a mesh's vertex count and perform triangle-accurate
scene raycasts, but it cannot identify the individual mesh-local vertices that
produced the rendered surface. That prevents editor interactions such as
vertex-to-vertex and vertex-to-surface snapping from using the same geometry
the game renders. Approximating vertices from node bounds is visibly wrong for
imported, concave, rotated, or non-uniformly scaled assets.

The runtime already owns the authoritative vertex streams. Mesh positions are
stored in renderer-compatible floats and may additionally retain a
double-precision position sidecar for large-world fidelity. An editor-only
private-memory cast would couple Studio to that layout, bypass safe-count
repair, and fail as soon as compact streams or storage details change.

Adding a public runtime method changes the C ABI surface and therefore requires
this ADR.

## Decision

Add the following read-only method to `Zanna.Graphics3D.Mesh3D`:

```text
VertexPosition(index: Integer) -> Zanna.Math.Vec3?
```

Its fully qualified runtime name is
`Zanna.Graphics3D.Mesh3D.VertexPosition`, with ABI signature
`obj<Zanna.Math.Vec3>(obj,i64)`.

The method has these semantics:

- `index` is zero-based and must be less than the mesh's repaired live
  `VertexCount`.
- A valid index returns a fresh runtime-managed `Vec3` containing the
  mesh-local vertex position. Callers cannot mutate mesh storage through the
  returned object, and later mesh mutations do not change an earlier result.
- The authoritative double-precision position sidecar is used when present;
  otherwise the drawable float position is widened to a double-precision
  `Vec3`.
- An invalid receiver, negative index, or out-of-range index returns `null`.
  Readback is a query and does not trap for an ordinary bounds miss.
- The graphics-disabled compatibility stub returns `null`.

The method intentionally exposes neither raw storage nor normals, UVs,
indices, topology mutation, or a whole-mesh sequence. Callers that scan
vertices must impose their own work budget appropriate to the interaction.
Zanna Studio's vertex snapping uses a fixed per-query vertex ceiling so a
single dense asset cannot stall the UI thread.

## Consequences

- Studio can project the actual vertices used by canonical and project-owned
  preview meshes, enabling exact transform-authoring gestures rather than
  bounds-based approximations.
- Imported and large-world meshes preserve the best position precision the
  runtime retains.
- Each successful read allocates or reuses one managed `Vec3`; the scalar,
  index-at-a-time API favors bounded interactive queries over unbounded bulk
  copies.
- The runtime API inventory, generated API documentation, Graphics3D guide,
  ABI-surface tests, and graphics-enabled/disabled implementations must remain
  synchronized.

## Alternatives considered

- **Expose a raw vertex pointer to Studio.** Rejected because it leaks runtime
  layout, lifetime, capacity, and precision-sidecar details across the ABI.
- **Return every vertex in a sequence.** Easier for callers, but forces an
  unbounded allocation and copy even when an editor needs only nearby
  candidates.
- **Infer snap points from world AABBs or collider corners.** Fast but not
  geometry-accurate and fails the core requirement that the scene editor show
  and manipulate what the game actually renders.
- **Add vertex metadata to `RayHit3D` only.** A triangle hit can identify the
  nearest face, but it cannot enumerate source vertices under the cursor or
  support pivot-to-vertex snapping without a second geometry query.
