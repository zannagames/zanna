---
status: active
audience: contributors
last-verified: 2026-09-05
---

# ADR 0328: Lossless clustered-light capacity fallback

## Status

Accepted for implementation (2026-09-05).

Implemented on 2026-09-05. Host validation is recorded below; native Windows/
Linux execution and the overall Baseball visual/performance acceptance remain
separate from this implementation status.

## Objective and evidence

Baseball Plan 95's native Metal night workload exhausts the shared 8,192-entry
light-index pool across 3,456 froxels. The current prefix builder silently
discards later lists. A six-second diagnostic records 69,704,192 cumulative
truncated entries and zero other renderer drops. The pre-optimization binary
also reproduces the failure. Good frame time does not excuse missing lighting.

## Scope, toggle and configuration

Apply to CPU table construction/validation, software reference selection and
both Phong/PBR shaders on Metal, OpenGL and D3D11. No new backend, dependency,
IL change, exposure compensation or application-specific light removal.
No opt-in toggle: capacity must not change lighting correctness. Existing
`ClusteredLighting` and `ClusterLightBudget` controls retain their enabled
state and 8..64 budget clamp. The budget bounds compact-list work, not lighting
fidelity. A cluster exceeding either its budget or shared-pool capacity uses
the existing full flattened-light loop, including its ordinary attenuation,
area/volume terms, material response and shadow semantics.

## Exact representation

Keep the existing table layout, 16-bit offsets/indices and portable buffer
sizes. In `offsets[cluster]`, bit `0x8000` marks full-light fallback. Bits
`0x7fff` hold the original prefix offset. Always mask **both** range endpoints;
the next cluster's fallback flag is not part of the current list's end offset.
The terminal boundary has no flag. A flagged cluster stores no compact
entries: its masked start equals its masked end. Nonflagged clusters store
their complete ordered local list, or an empty list when none contributes.
Reserve lists all-or-nothing, deterministically in existing cluster order.
Globals occur exactly once: fallback selects light indices `0..light_count-1`;
compact selection keeps the globals-first prefix plus local indices.

`VGFX3D_CLUSTER_FALLBACK_FLAG = 0x8000u` and
`VGFX3D_CLUSTER_OFFSET_MASK = 0x7fffu` are internal representation constants.
The existing internal `overflow_count` records local index demand routed to
fallback (the full size of omitted lists); it no longer represents lost light.
No table header or GPU constant-buffer size changes.

## Public diagnostic surface

Add read-only `Canvas3D.ClusterFallbackEntryCount -> i64`, C entry point
`rt_canvas3d_get_cluster_fallback_entry_count(void *)`. It is a saturating
lifetime count of local cluster-list entries served by full-light fallback,
incremented once when a revision's table is built, not once per draw/cache hit.
It is capacity pressure, not rejected geometry or missing lighting. Invalid
canvas returns zero. Graphics-disabled stub returns zero through the existing
neutral diagnostic path.

`ClusterOverflowCount` continues to mean actual lost cluster-light entries;
the new lossless builder contributes zero to it. It must not alias the new
fallback counter. Applications can observe fallback cost without falsely
certifying dropped light as successful work. Existing loss counters and
`DroppedLightCount` (forward array limit) remain unchanged.

## Errors and failure behavior

No new trap/message. Invalid revision, count, depth, offset/index bounds,
flagged terminal offset or flagged nonempty list causes table validation to
return false and the backend to use its existing flat-light fallback. Masked
offsets remain monotonic, first offset is zero and final offset is at most
8,192. The existing allocation-failure flat fallback remains valid. Counter
addition saturates at `INT64_MAX`; no signed overflow or wrap.

## Tests and acceptance

- Given three unbounded local lights, pool exhaustion marks whole clusters
  for fallback; every cluster selects all three lights and output is byte-
  deterministic. This regression fails on the truncating builder.
- Given more local lights than the minimum per-cluster budget, every affected
  cluster falls back with no partial list and no missing/doubled global light.
- Given ordinary bounded lights, non-overflow table bytes and selection stay
  unchanged. At a boundary beside a fallback cluster, masking preserves the
  preceding compact list's exact range.
- Given malformed flags/offsets or a stale revision, shared and D3D11 validators
  reject safely. Software selection/rendering and each GPU shader decode both
  paths identically for Phong and PBR; shader compilation must succeed.
- Repeated draws of a cached revision do not multiply fallback telemetry;
  overflow totals stay zero; counter saturation and disabled stubs are covered.
- Run the canonical engine build/tests, platform lint/local cross-platform
  smoke and real native night workload. Keep the full workload's unchanged
  performance/drop budgets and review actual night lighting. Actual Windows/
  Linux backend execution remains separately required, not implied by host
  source tests.

## Verification record (2026-09-05)

The new complete-list regression fails against the old builder with 726
incomplete clusters (322/323 Canvas3D cases pass). With the implementation,
the canonical macOS build's 2,023-test run passes in 388.52 seconds; the existing
audio-unavailable case is skipped in this audio-enabled configuration. Runtime
surface audit (8,038 functions, 539 classes, 9,462 header declarations), platform
policy lint and local platform smoke pass. Logs:
`/tmp/zanna-venue95.ockKeK/cluster-lossless-red-test01.log` and
`/tmp/zanna-venue95.ockKeK/cluster-lossless-full-build02.log`.

Baseball's isolated real-Metal probe passes all eight full-image comparisons in
both source and balanced-profile native execution: Phong/PBR, compact/pool-
overflow depths, per-cluster overflow, and a directional global prefix.
Each comparison has zero changed pixels and a nonempty three-channel reference.
See `baseball/analysis/venue95/cluster-lighting/README.md` for retained evidence
and exact hashes. Shared/D3D11 validator and assembled HLSL guardrail tests pass;
native D3D11/GL shader execution is not available on this Metal-only host.
Graphics-disabled stub execution is not claimed by the enabled host build.

The short native stadium night smoke reports zero actual dropped work and
61,197,534 lifetime entries served through fallback. It was run concurrently
with other verification and is not performance evidence. The full stadium's
day/night image gate still fails six daylight bands, so no art-quality or
release acceptance follows from isolated lighting parity.

The subsequent clean 120-second native night workload passes the unchanged
budgets: startup 6.964 seconds, cadence p95 23.088 ms, no >50 ms hitches, no
actual dropped work, and 501,362,658 lifetime entries served through fallback.
Geometry/population counts are unchanged. Evidence:
`baseball/analysis/venue95/liveperf/native-balanced-night-lossless-09.*`.

## Alternatives considered

Raising the shared pool alone exceeds portable small uniform-buffer limits
before covering worst-case demand. Increasing only `ClusterLightBudget`
does not expand that pool. Whole-table fallback is correct but discards useful
compact lists. Removing lights, suppressing diagnostics or brightening
exposure preserves the defect. A different mask/list-compression representation
may improve performance later, but must preserve this lossless contract.

## References

`rt_canvas3d_clusters.c`, `vgfx3d_backend_utils.c`,
`vgfx3d_backend_d3d11_shared.c`, the three GPU shader includes,
`vgfx3d_backend_sw_raster.inc`, ADRs 0070 and 0233,
`baseball/analysis/venue95/liveperf/README.md`.
