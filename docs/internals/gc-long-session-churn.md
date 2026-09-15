---
status: complete
audience: contributors
last-verified: 2026-09-14
---

# GC lookup degradation during long sessions

Investigated after Legacy Baseball's 3D broadcast was reported jerky when left
running for approximately two hours.

## Reproduced engine defect

The tracked-object hash table in `src/runtime/core/rt_gc.c` used tombstones for
deletion, but grew/rehashed only when **live** entries reached the load limit.
Neither ordinary untracking nor payload relocation removed accumulated
tombstones. A stable live population could therefore eventually occupy every
slot with either a live entry or a tombstone.

An unsuccessful `find_entry` then scanned the entire table. This affected more
than cycle collection: registering a new tracked object first checked whether
it was already present, and ordinary heap/object destruction called
`rt_gc_untrack` even for payloads that were never tracked. Per-frame allocation
and cleanup could consequently become progressively more expensive without an
increase in live scene objects. The table's earlier peak population determined
how expensive these scans became; there was no fixed two-hour threshold.

The regression retains 8,192 distinct managed allocations to prevent allocator
address reuse from masking the defect. It keeps one object tracked and tracks
then untracks the remaining 8,191. Before the fix, the 64-slot table required
64 probes for an unsuccessful lookup despite containing only one live object.
The test failed. After the fix, the worst unsuccessful lookup takes two probes
and the test passes. These are structural checks, independent of machine speed.

## Fix and scope

Deletion now repairs the linear probe chain by shifting an entry into the hole
only if that entry's circular search path crosses it. The final hole becomes
empty. Both untracking and payload relocation use this operation. Moving the
whole entry preserves traversal callbacks, trial counts, colors, promotion
counts, and shutdown-finalizer epochs.

The operation allocates no memory and remains under the existing GC lock and
mutator barrier. Capacity still depends on the live population; historical
churn no longer extends lookup chains. No runtime ABI, IL rule, configuration,
feature switch, or new error message is needed. Legacy Baseball must be rebuilt
because its native executable links the runtime.

`RTGCChurnTests.c` also exercises a collision chain spanning the last and first
table slots, removal from its head and middle, metadata preservation, repeated
payload relocation, and complete cleanup. It compiles the production collector
into the existing GC hash test, allowing inspection without production counters
or new public diagnostics. Existing GC tests cover cycle reclamation, automatic
collection, weak references, arrays, finalization, and VM/native execution.

## Validation limits

This reproduces and fixes an engine defect capable of causing the reported
long-session degradation. It does not measure the original two-hour session or
establish its exact onset. The canvas clock uses integer microsecond deltas,
and the reviewed game asset caches and Metal frame-resource paths already have
bounded lifetimes; those paths were left unchanged.

## Local validation, 2026-09-13

- The added regression failed with the original collector (64 probes) and
  passed with the fix (2 probes); all 1,319 hash-test assertions passed.
- `./scripts/build_zanna_mac.sh` completed the clean compilation, all 2,201
  default tests, platform policy lint, and runtime surface audit. The script
  then stopped at the additional `native_smoke_chess_ai_arm64` test with
  `rt_list_get: index out of bounds` (a large, varying index; count 20).
  An isolated copy of the runtime archive containing the **original** collector
  reproduced that failure. The remaining Crackman/Studio host smoke tests were
  run separately and passed. The canonical script therefore did not reach its
  install stage; this is not a claim of an entirely green host smoke run.
- `./baseball/scripts/build_baseball.sh` rebuilt `baseball/bin/baseball` with
  the fix. Its `--auto-season` output matched the prior executable byte for byte.
- The existing `watch3d_liveperf.zia` was built natively with the balanced
  profile. Each run used Metal, a 1920x1080 output, the same camera/pitch
  workload, adaptive scaling off, a 12-second warm-up, and 120 measured seconds.
  An alternate archive containing the original collector supplied the control;
  the source compiler and all other runtime archives were held constant.

| Run, in execution order | Frames | Cadence p95 | Hitches outside cuts |
| --- | ---: | ---: | ---: |
| Fixed collector, first run | 5,673 | 39.553 ms | 122 |
| Original collector | 6,356 | 26.611 ms | 5 |
| Fixed collector, repeat | 6,720 | 24.522 ms | 1 |

All three runs reported zero dropped work. The repeat met the existing 25 ms
p95 and three-hitch frame budgets; startup still exceeded the separate 10-second
budget in every run (approximately 22 seconds). The first fixed run overlapped
diagnostic compilation work, so these runs are **not** a controlled FPS speedup
claim. The repeat checks steady rendering after compilation and the host smoke
tests finished. The structural churn regression, rather than these short runs,
establishes removal of the accumulated-lookup failure. A two-hour visual soak
has not been performed.
