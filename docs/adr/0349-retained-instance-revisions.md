---
status: accepted
audience: contributors
last-verified: 2026-09-09
---

# ADR 0349: Retained instance batches track what changed

## Context

`InstanceBatch3D` holds authoritative double matrices plus current/previous
snapshots for motion vectors, a float submit mirror, per-instance culling
bounds (ADR 0347) and visibility scratch. Every `Canvas3D.DrawInstanced`
re-sanitized all N matrices, copied all N doubles into the frame snapshot,
rebuilt all N previous floats, frustum-tested all N instances and, in the
canvas, counting-sorted the visible instances into spatial cells and looked
each one up in the canvas motion-history map. Legacy Baseball's crowd is
~9,150 spectators with two parked posture slots each and a cap slot, so a
stationary bowl cost several milliseconds of CPU per frame in the wide view
(plan 109 L1 ledger: ~10 ms of scene-bracket CPU outside the pass timers).
A comparison-based reuse trial (plan 108) was rejected because it still
touched every matrix.

## Decision

1. **Dirty ranges.** `Set` widens an inclusive dirty slot range;
   add/remove/clear/reallocation/state repair mark every slot. Sanitization
   covers only the dirty range. At a new frame the snapshot copy covers the
   union of the last two frames' ranges (after the swap the target buffer is
   two frames old), and the previous-frame float mirror is patched over the
   previous frame's range instead of being rebuilt. Structural or
   uninitialized states copy everything. Same-frame draws never snapshot, as
   before. Motion history stays exact: the snapshot equals the live matrices
   after every frame and the previous snapshot equals the prior frame's.
2. **Batch-owned motion history.** When a batch supplies previous matrices,
   the canvas no longer walks its per-instance motion-history map for that
   draw; the map remains the source for draws that do not supply history.
3. **Retained 3D cells.** A batch above the split threshold keeps a 3D grid
   (the existing 256-unit spacing, at most four cells per axis, so parked
   instances far below the bowl fall into their own cells) with per-cell
   member lists and world AABBs maintained from the dirty range. Culling
   tests cells first: an outside cell skips its members, an inside cell
   accepts them, a partial cell tests members through the ADR 0347 bounds
   cache. Visible instances are submitted per cell, so the canvas' per-frame
   counting sort no longer runs for these batches.
4. **`InstanceBatch3D.RetainedBytes: i64`** (read-only) reports the batch's
   retained CPU storage — matrices, snapshots, mirrors, scratch, bounds and
   cells — so resource accounting (plan 109 L5) measures rather than
   estimates. Registry delta: one function, one property; disabled-graphics
   stub returns zero.

No shader, IL, dependency or serialized format changes. Invalid dirty
ranges are repaired to "all"; allocation failure of cell storage falls back
to the per-instance path for that draw.

## Validation

Unit: partial snapshots over four frames including a two-frame-old buffer,
subframe mutation, swap-remove and clear (`test_instance_batch_dirty_ranges_keep_motion_history_exact`);
cell partition equals a brute-force per-instance cull over random frusta and
layouts including parked instances; the motion map is untouched for
history-owning batches; `RetainedBytes` accounting; the existing
sanitize/repair/bounds-cache robustness tests unchanged. Native: Legacy
Baseball's L1 protocol before/after (wide-view median CPU, frame p95) with
matched images.
