---
status: active
audience: contributors
last-verified: 2026-08-31
---

# ADR 0308: Deterministic Parallel Light Baking and Two-Phase Commit-Queue Teardown

## Status

Accepted (2026-08-31)

## Context

The CPU `LightBaker3D` and `LightProbeGrid3D` paths performed every radiance
sample serially. Their work is naturally independent, but the previous random
stream advanced across samples, so a direct parallel loop would make output
depend on scheduling and worker count. The baker also queried every copied
light for every shading point even when most lights had finite local bounds.

The internal Graphics3D worker-to-main-thread commit queue had a separate
lifetime ambiguity. Its `Free` entry point closed and immediately reclaimed
the wrapper. A producer that still held the raw internal handle could begin an
enqueue while teardown freed that handle. The underlying concurrent queue's
close operation correctly resolves enqueue-versus-close ownership, but it
cannot make reclamation safe while callers are still executing through the
outer pointer.

These changes add a Graphics3D-to-threads dependency and one internal C symbol,
both of which require an architecture decision record under the repository
policy.

## Decision

1. `LightBaker3D` derives each lightmap path seed from immutable triangle,
   texel, and sample coordinates. A baker-owned, lazily created `Threadpool`
   evaluates disjoint sample ranges. Results are reduced on the caller in
   sample-index order, so worker completion order and worker count cannot alter
   floating-point accumulation order. `BakeStep` retains its fixed 1,024-path
   work budget.
2. Probe grids partition their probe index range across the same baker-owned
   pool. Each task writes disjoint probe-major coefficient and validity ranges;
   the deterministic breadth-first invalid-probe fill remains serial after the
   barrier. Calls made from a thread-pool worker use the serial path to avoid
   nested-pool waits.
3. The baker owns and shuts down its pool during finalization. Pool creation
   failure and single-core hosts transparently retain the serial implementation.
   No third-party dependency is introduced; the existing runtime thread-pool
   and platform abstractions are used.
4. Finite local bake lights are indexed in a deterministic AABB hierarchy;
   directional and effectively unbounded lights remain in a compact global
   list. Shading queries traverse only overlapping light nodes.
5. The internal C ABI gains `rt_g3d_commit_queue_close(void *)`. Concurrent
   teardown is explicitly two-phase: close while producers may still hold the
   handle, stop or join those producers, then call `Free`. Close is idempotent,
   preserves queued ownership, and causes later enqueues to fail without
   transferring their payload. `Free` still closes defensively for callers
   that have no concurrent producers.
6. The close helper is intentionally not registered as a language runtime
   method. It is an internal native coordination primitive, so generated
   runtime manifests and language documentation do not change.

## Consequences

- Large sample counts and probe grids use available CPU cores while retaining
  reproducible atlases and SH coefficients.
- A baker carries a small persistent worker-pool cost only after a parallel
  workload is encountered; finalization joins and releases it.
- Commit-queue owners must make producer lifetime explicit. Reclamation racing
  a raw pointer remains invalid by contract, while the supported close/join/free
  sequence is race-free and testable.
- The light baker now depends on `rt_parallel.h` and `rt_threadpool.h`, matching
  existing dependencies used by the software rasterizer, PostFX, physics, and
  cubemap preprocessing.

## Tests

- `test_rt_lightbaker3d` verifies bounded incremental work, repeated-bake
  determinism, parallel multi-bounce sampling, parallel probe baking, finite
  local-light behavior, and scenes containing more than sixteen lights.
- `test_rt_g3d_commit_queue` closes a queue while a producer is active, joins
  that producer, verifies post-close enqueue rejection and payload ownership,
  and only then frees the queue.
- `scripts/lint_platform_policy.sh` verifies that the new dependency uses the
  runtime platform/thread abstractions without raw platform conditionals.

## References

- [ADR 0088](0088-baked-gi-lightmaps-and-probes.md)
- `src/runtime/graphics/3d/render/rt_lightbaker3d.c`
- `src/runtime/graphics/3d/rt_g3d_commit_queue.c`
- `src/runtime/threads/rt_threadpool.h`
