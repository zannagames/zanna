---
status: active
audience: contributors
last-verified: 2026-09-05
---

# ADR 0326: Explicit-Domain Mesh Height Loft

## Status

Accepted; implementation and validation in progress.

## Context

Authored wall modules must retain UVs, seams, edge radii and materials while
fitting independent ground and top heights at both ends. Affine scale/shear
cannot vary height across X without also shearing the ground. Per-material
bounds are not a reliable module domain (as with narrow-group arc bending).

## Decision

Add `Mesh3D.LoftHeight(xMin, xMax, yMin, yMax, bottomLeft, bottomRight,
topLeft, topRight)`, C symbol `rt_mesh3d_loft_height(obj, f64×8)`, returning void.
Every material group receives the same source rectangle. For each vertex,
`t=(x-xMin)/(xMax-xMin)`, `u=(y-yMin)/(yMax-yMin)`, and
`y'=lerp(lerp(bottomLeft,bottomRight,t), lerp(topLeft,topRight,t), u)`.
X/Z, UVs, indices, colors and unrelated attributes remain unchanged.

All parameters must be finite and fit float range. Source spans and both
destination heights must exceed 1e-9. Positions must be finite and inside the
source X/Y rectangle (relative 1e-6 tolerance for imported float conversion,
clamped only within that tolerance). Preflight validation precedes mutation;
bad parameters/vertices trap without changing valid mesh geometry. Invalid
handles are ignored. Empty valid meshes validate parameters then return.
Skinned and morph-target meshes are rejected.

Normals use the inverse-transpose analytic Jacobian; tangents use its forward
map and are orthogonalized against the transformed normal. Tangent handedness
is retained. Invalid/zero normals and tangents receive deterministic finite
fallbacks. Positive destination heights preserve triangle winding.
Authoritative double positions, geometry revision, bounds, deferred snapshots
and raycast invalidation follow the existing mesh mutation contract. Only the
supplied mesh changes; callers explicitly transform each retained LOD.

This is backend-independent runtime math with a graphics-disabled no-op stub.
It adds one registry function and one method, without IL, verifier,
serialization or existing signature changes.

## Validation

Tests cover endpoint/midpoint lofts, explicit shared domains, normal/tangent
frames, handedness, unchanged attributes/indices, bounds/revisions, invalid
spans and nonfinite/out-of-domain inputs before mutation. Registry counts,
manifest hash and generated reference are updated. Baseball independently
checks variable-height fence contact, shared endpoints and rendered output.
