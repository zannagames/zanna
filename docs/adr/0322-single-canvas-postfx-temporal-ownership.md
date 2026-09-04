---
status: accepted
audience: contributors
last-verified: 2026-09-04
---

# ADR 0322: Give PostFX Temporal State One Canvas Owner

## Context

PostFX3D stores CPU TAA history, the previous view-projection matrix,
auto-exposure adaptation, reusable image buffers, and a worker pool in the
chain object. Canvas3D previously allowed one chain to be attached to several
canvases simultaneously. Alternating those canvases then reused temporal state
from unrelated cameras and resolutions, producing ghosting and exposure jumps;
concurrent rendering could also race on the shared scratch buffers.

Changing the Canvas3D/PostFX3D attachment contract affects the runtime C API
surface and therefore requires this decision record under ADR 0006.

## Decision

A PostFX3D chain may have at most one attached Canvas3D at a time. The chain
records a weak canvas address plus that canvas's nonzero 64-bit allocation
generation. `Canvas3D.SetPostFX` refuses a second concurrent attachment,
preserves both canvases' existing bindings, and records a recoverable
`PostFX3D.LastError` explaining that the caller must detach the first canvas.

Detaching, replacing, or finalizing a canvas clears the matching weak owner and
resets temporal state. The chain can then be attached to a different canvas
with clean TAA and exposure history. Reassigning the already attached chain to
the same canvas remains a no-op.

## Consequences

- TAA, auto-exposure, previous-camera state, scratch buffers, and worker-pool
  execution cannot be contaminated by another canvas.
- Applications that want identical settings on multiple canvases must create
  one chain per canvas.
- Ownership rejection is recoverable and discoverable through the existing
  `LastError` API; it does not trap or partially mutate either binding.
- No IL opcode, grammar, verifier rule, serialized format, dependency, or
  registry signature changes.

## Alternatives Considered

- Keep a dynamic per-canvas state map inside each chain: rejected because it
  complicates chain mutation, cut invalidation, synchronization, and teardown.
- Reset history whenever the active canvas changes: rejected because
  interleaved canvases would never accumulate useful temporal history.
- Clone a chain silently at bind time: rejected because `Canvas3D.PostFX` would
  stop returning the object the caller explicitly attached.
