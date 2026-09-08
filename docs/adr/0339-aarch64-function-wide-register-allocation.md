# ADR 0339: AArch64 function-wide register allocation

Status: Implemented (default since Phase 3 C6 of the backend codegen review; the block-local
path was deleted in Phase 3 C7; x86-64 adopted the same model through the shared core in C8)

## Context

The AArch64 backend allocated registers one block at a time. Lowering gave every IL temporary
that crossed a block boundary a frame slot, block parameters had one slot each written by
`PhiStoreGPR`/`PhiStoreFPR` edges and reloaded at block entry, and the allocator re-adopted a
single layout predecessor's exit registers while storing every live-out value at the block end so
memory stayed authoritative. About 1,900 lines of post-RA memory forwarding
(`forwardSinglePredPhiLoads`, `coalesceJoinPhiLoads`, `eliminateLoopPhiSpills`,
`forwardLayoutSuccessorStoreLoad`, slot pinning) undid part of the traffic. Measured on `chess` at
`-O2`: 105,089 instructions, of which 12,381 were `mov xS,#off; add xS,x29,xS` prefixes for frame
accesses beyond the encodable range, 21,394 frame loads and stores, 9,977 spill slots, an
89 KB frame. Every block-crossing value paid a store and a reload per block.

The post-RA passes could not reason about liveness across blocks: the allocator published
`carriedExitRegs`, an ad-hoc set that only the block-local scheme could produce, and every
rewrite that dropped a definition reaching a block end consulted it.

## Decision

Register allocation on AArch64 is function-wide, in two phases over an interval model with holes.

- **Lowering** (the only mode since C7): block parameters are
  virtual registers, every branch argument list is one `ParallelCopy dst0, src0, …` pseudo
  (inline for `br`, in the existing split block for `cbr`/`switch`), cross-block temporaries keep
  their virtual register, and blocks are lowered in reverse post-order. No frame slot comes from
  lowering except allocas, the switch scrutinee, and the `rt_arr_obj_get` round trip. `PhiStore*`
  is gone from the MIR contract; the MIR dump changes accordingly.
- **Intervals** (`ra/LiveIntervals`): blocks numbered in reverse post-order over `MirCfg`;
  instruction *i* of block *b* reads at `base[b] + 2i`, writes at `base[b] + 2i + 1`, and each
  block owns an exit position. A backward walk per block seeded from the CFG liveness solution
  builds one sorted, merged range list per virtual register; the same walk over `effectsOf()`
  seeded from the solved physical liveness builds fixed ranges for physical registers (an
  explicit write until its last read, call clobbers as points, ABI inputs from the entry, the
  return registers read by `ret`). Weights are `Σ (uses + defs) · 10^loopDepth`; hints come from
  moves with a physical side and from parallel-copy pairs.
- **Assignment** (`ra/GlobalAllocator`): whole-interval linear scan in `(start, id)` order; hint,
  then the class pool with callee-saved registers first for call-crossing intervals; on conflict
  the register whose conflicting occupants are lightest is taken and they are evicted unless the
  new interval is lighter, in which case it spills. Spilling is total. A value live across
  `rt_native_eh_push` or the `setjmp` that follows it is always memory-homed (EH-1), and no reload
  is cached across instructions, so none survives a call or branch (EH-2).
- **Slots**: spilled values sorted by weight share first-fit slots when their range lists do not
  intersect; the hottest slot is allocated nearest x29.
- **Rewrite**: operands are replaced; a spilled value is reloaded before every use into a register
  free at the instruction (pool first, reserved x9/x16/x17 or v16/v17 last, under the existing
  arity bound; a definition-only operand may reuse a use's temporary) and stored after every
  definition; each `ParallelCopy` is sequentialised by the shared `common/ra/ParallelCopy.hpp`
  (cycles through a free pool register or a fresh slot, mem-to-mem through x17/v17); identity
  moves are dropped; the frame is finalised and the callee-saved registers touched are published.
- **Post-RA passes** read the solved physical liveness (`PhysLiveness.hpp`,
  `blockExitLive(fn, bi, target, liveness)`); no pass publishes or consumes carried-register
  metadata. The phi-slot peephole stages do not run on this path.
- **Switch**: during C6 `ZANNA_LOCAL_RA=1` (or `PipelineOptions::localRegAlloc`) selected the
  whole old path for bisecting; C7 deleted the path and the switch. `ZANNA_NO_GLOBAL_RA` now
  applies to the x86-64 backend only.

## Acceptance

- Every allocated function passes the PostRA verifier (no virtual register, no `ParallelCopy`,
  frame offsets inside the frame, callee-saved writes covered, reserved scratch never live across
  an implicit clobber nor out of a block, ABI-only entry live-in).
- `test_regalloc_aarch64_oracle`: a MIR interpreter executes seeded random edge-copy functions
  (arithmetic, slot round trips, calls that clobber every caller-saved register, diamonds, nested
  counted loops carrying up to 40 values in both classes) before and after allocation; the results
  agree for every seed.
- `test_aarch64_live_intervals` and `test_regalloc_aarch64_global` pin the position convention,
  the fixed ranges, hints, weights, and the allocator's shapes (loop parameter in one register with
  no frame access, call-crossing value callee-saved, marshalled argument and ABI live-in out of the
  pool, pressure spills with shared slots, swap in three moves, EH values memory-homed, mem-to-mem
  through x17, FPR rules, determinism, 500 live values).
- The shared corpus, `examples/il`, the benchmarks, and the demos compile and verify at `-O0` and
  `-O2`; the differential labels (VM vs native, `-O0` vs `-O2`, seeded IL kernels including the
  `eh-catch` and `phi-cycle-loop` shapes) pass on AArch64 hosts.
- Against the Phase 3 baseline (`docs/internals/codegen_stats_baseline.tsv`): chess `-O2`
  105,089 → 55,537 instructions, 21,394 → 4,946 frame accesses, 12,381 → 64 offset prefixes,
  9,977 → 200 spill slots; every benchmark loses all frame traffic; no program's instruction count
  rises. On x86-64 (C8, same core) chess `-O2` 88,966 → 75,513 instructions, 12,332 → 8,031
  frame accesses, 2,344 → 833 spill slots; the after-table is
  `docs/internals/codegen_stats_phase3.tsv`.
