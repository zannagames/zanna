---
status: accepted
audience: contributors
last-verified: 2026-09-09
---

# ADR 0347: Reuse prepared instance bounds during submission

## Decision

Add the internal C bridge `rt_canvas3d_queue_instanced_batch_prepared` with
current/previous matrices, optional packed minXYZ/maxXYZ bounds per original
instance, and a flag indicating matrices are already in frame render space.
Existing public runtime methods and old internal entry points retain their
contracts. InstanceBatch3D supplies exact cached bounds for the same prepared
matrices used by culling. Missing, invalid or unavailable cached bounds fall
back to normal bounds calculation.

Spatial splitting indexes bounds through the same original-index remap as
matrices. Hardware submission consumes bounds synchronously to form a copied
aggregate AABB; no queued draw borrows the bounds array. Matrices remain
immutable frame-owned snapshots. Software and precision fallback paths keep
their existing calculations. No shader, language, IL or dependency change.

Prepared bounds must be conservative for the current mesh and already use
frame render coordinates. Internal callers own this invariant; finite but
incorrect supplied bounds cannot be verified without repeating the transform.
Allocation failure in InstanceBatch3D disables reuse for that submission.

## Validation

Compare cached and ordinary aggregate bounds, require zero redundant AABB
transforms for valid prepared data, verify invalid entries fall back, and
mutate caller arrays after queuing to prove ownership. Cover split batches,
relative coordinates, changed instances, native images and live timings.
