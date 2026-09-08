---
status: proposed
audience: contributors
last-verified: 2026-09-07
---

# Backend Codegen Review and Level-Up Plan (September 2026)

Deep review of `src/codegen/{aarch64,x86_64,common}`: register allocators, coalescers, IL lowering,
legalization, ISel, schedulers, every post-RA peephole, frame and call lowering, and both emitters.
Findings are ranked by impact; the plan is phased so the VM-vs-native differential oracle stays green
at every step.

## Status

Phase 1 items **A1**, **A2**, **A3**, and the interim form of **A4** are implemented (each with a
regression test that fails before the fix):

- `src/codegen/common/PreRAForwardCopy.hpp` — counts every use of the copy destination before
  forwarding (`test_codegen_preregalloc_opt`).
- `src/codegen/x86_64/OperandRoles.{hpp,cpp}` — `implicitDefMask` / `implicitUseMask`, consumed by
  `peephole/MemoryOpt.cpp`, `peephole/MovFolding.cpp`, `peephole/DCE.cpp` (`test_x86_peephole`).
- `src/codegen/aarch64/AsmEmitter.cpp` — wide-immediate expansions pick a scratch that is not an
  operand (`test_emit_aarch64_mir_bitwise`).
- `src/codegen/aarch64/passes/SchedulerPass.cpp` — `mayClobberEmitScratch` covers the wide-immediate
  ALU / compare / FP-constant forms (`test_aarch64_scheduler`).

Running the full gate on Linux/clang 18 also surfaced and fixed four pre-existing issues outside
the original scope:

- `src/codegen/common/linker/RelocApplier.cpp` — locally-resolved `R_X86_64_GOTPCRELX` /
  `REX_GOTPCRELX` sites were only relaxable in the `mov` form; clang 18 emits `cmp foo@GOTPCREL(%rip)`
  for pointer comparisons. All psABI B.2 forms (mov, call, jmp, test, group-1 ALU) are now relaxed
  (`test_linker_reloc_applier`).
- `src/codegen/common/linker/DynStubGen.cpp` / `NativeLinker.cpp` — plain `R_X86_64_GOTPCREL`
  (non-relaxable, e.g. `pushq foo@GOTPCREL(%rip)`) to a symbol defined in the link had no GOT slot.
  The linker now synthesizes link-time `__gotl_<name>` slots in `.got.zanna_local`
  (`test_linker_p1_hardening`).
- `src/CMakeLists.txt` — on ELF/lld the `--whole-archive` group for `zia` came after the ordinary
  `fe_basic` link item, producing duplicate-symbol link errors; the group is now declared first.
- `src/runtime/graphics/3d/backend/vgfx3d_backend_opengl_shaders.inc` (a chunk over the ISO C99
  4095-byte literal limit under `-Werror=overlength-strings`) and
  `src/tests/runtime/RTCanvas3DCoordsContractTests.cpp` (`isfinite` from a C `.inc` inside C++).

Phase 2.1 (one instruction-effects model per backend) is implemented:

- `src/codegen/aarch64/InstrEffects.{hpp,cpp}` — `effectsOf(const MInstr&, const TargetInfo&)`
  (explicit roles from `ra::operandRoles` plus call/return ABI registers, NZCV, memory class,
  jump-table and emit-time scratch clobbers), `callClobberSet`, and the shared opcode predicates.
  `peephole/PeepholeCommon.cpp`, `peephole/CopyPropDCE.cpp`, `peephole/LoopOpt.cpp`,
  `passes/SchedulerPass.cpp`, and `PreRegAllocOpt.cpp` consume it; their private role tables are
  deleted (`test_aarch64_instr_effects`, which also asserts `classifyOperand == ra::operandRoles`
  on real MIR).
- `src/codegen/x86_64/OperandRoles.{hpp,cpp}` — `effectsOf` on top of the Phase 1 implicit masks;
  `ra/Allocator.cpp::collectPhysicalClobbers`, `Scheduler.cpp`, and `ISel.cpp` consume it. The
  `JUMPTABLE` dispatch scratch (R10/R11) is now an implicit definition (`test_x86_peephole`).

Phase 2.4 (MIR verifier) is implemented:

- `src/codegen/{aarch64,x86_64}/MirVerify.{hpp,cpp}` — `verifyMir(fn, stage, target, diags)`
  with cumulative per-stage rule sets (structure at every stage; no virtual registers, frame and
  stack offsets inside the finalized frame, callee-saved coverage, reserved scratch never live
  across an implicit clobber, ABI-only entry live-in after RA; encodable immediates after pseudo
  expansion on AArch64). Register facts come from the Phase 2.1 effects model and the CFG from the
  allocator's now-exported `ra::classifyControlFlow`.
- `src/codegen/common/PassManager.hpp` — post-pass hook (`setPostPassHook`) that both pipelines
  install when `ZANNA_VERIFY_MIR=1` or `--verify-mir` (`zanna codegen arm64|x64`) is given; a
  violation is a `V-CG-MIR-*` error that stops the pipeline.
- Tests: `test_aarch64_mir_verify` / `test_x86_mir_verify` (one failing-MIR case per rule plus the
  pipeline at -O0/-O1/-O2 with verification on), and the AArch64 shared-corpus and VM-vs-native
  property tests now pass `--verify-mir`. Calibration on this host: zero violations across the
  shared IL corpus, every `examples/` program, and the demo games / 3D showcases (chess, crackman,
  paint at ~40–60K IL lines each) on both backends at -O0 and -O2.
- First verifier finding: on x86-64 `trap.from_err` lowered to a bare `call rt_trap_raise_error`
  with no terminator after it (the inline trap emitters all append `ud2`), so the block fell off
  its end in the MIR CFG. `Lowering.Mem.cpp::emitCall` now appends `UD2` after any call to a
  no-return runtime helper; the symbol set moved to `codegen/common/NoReturnSymbols.hpp` so both
  backends share it (`test_x86_backend_regressions`).

Phase 2.2 (A4, `ExpandPseudosPass`) is implemented:

- `src/codegen/aarch64/passes/ExpandPseudosPass.{hpp,cpp}` — last MIR pass at every level;
  rewrites every emit-time pseudo form (wide `AddRI/SubRI/AddsRI/SubsRI/AndRI/OrrRI/EorRI/CmpRI`,
  non-FP8 `FMovRI`, `AddFpImm` and frame/base/pair/SP accesses outside the encodable range) into
  explicit MIR with the historical scratch preference, skipping any reserved scratch that is an
  operand or live later in the block. `InstrEffects` gained the shared encodability predicates
  (`isEncodableLdStOffset` is width-aware, so positive scaled offsets no longer count as pseudo
  forms). Both emitters now reject pseudo forms (`rejectUnexpanded`) and their private scratch
  selection (`pickWideImmScratch`, `chooseGprScratch`, `resolveBaseOffset`,
  `encodeLargeOffsetLdSt`) is deleted. The verifier's `PostExpand` stage runs after the pass.
- Tests: `test_aarch64_expand_pseudos` (one case per form, scratch-live-across, pair split, SP
  store, encodable forms untouched, pipeline at -O0/-O1/-O2 leaves no pseudo form),
  `test_emit_aarch64_mir_bitwise` (emitters reject unexpanded forms). The shared corpus, every
  example, and the demo games verify clean under `PostExpand` on this host.
- Measurement that motivates a Phase 3 item: in `chess` at -O2, 12,447 of 113,289 emitted
  instructions are the `mov x9,#off; add x9,x29,x9` prefixes of frame accesses beyond ±256
  bytes (every spill/reload in a frame larger than 256 bytes costs three instructions).

Phase 2.3 (one `MirCfg` per backend, `blockExitLive`, B1) is implemented:

- `src/codegen/{aarch64,x86_64}/MirCfg.{hpp,cpp}` — snapshot CFG built from `ra::classifyControlFlow`
  through `common/ra/CfgExtract.hpp` (which now also reports per-block fallthrough), with
  predecessors, `hasEdge`, `fallsThrough`, `exitsDirectlyTo` (AArch64), lazily computed dominators
  (`common/ra/Dominators.hpp`, the former `aarch64/peephole/Dominators.cpp` moved there), dominance-
  proven `backEdges`, `naturalLoop`, and `loopDepths`. Consumers: both `ra::LivenessAnalysis`, both
  `MirVerify`, AArch64 `removeDeadInstructionsCFG`, `forwardSinglePredPhiLoads`,
  `coalesceJoinPhiLoads`, `forwardLayoutSuccessorStoreLoad`, `hoistLoopConstants`,
  `eliminateLoopPhiSpills`, and the x86-64 `traceBlockLayout`/`moveColdBlocks`. The five private
  builders they used are deleted; the drifts they had (LoopOpt dropped the fallthrough edge after a
  trailing `Tbz`/`Tbnz` and added one after a `JumpTable`/no-return call; `Peephole.cpp` added a
  fallthrough after a `JumpTable` and never recorded jump-table targets as predecessors;
  `LoopOpt` bounded natural loops by layout position) are gone with them.
- `blockExitLive(fn, bi, target)` (`aarch64/MirCfg.hpp`) = `carriedExitRegs` ∪ SP/FP/LR ∪ (return
  registers when the block leaves the function, otherwise callee-saved GPR/FPR — pinned slots live
  there; at a return the epilogue restores them, so a value left in one is dead). (Superseded in
  Phase 3 C4 by the solved-liveness form below.)
  `foldComputeIntoTarget` and `tryMaddFusion` (B1) now take the ABI and this set and scan with the
  effects model, so a call's argument registers and a return's result registers are reads, a call's
  caller-saved clobbers are writes, and a value reaching the block end is dead only if it is not
  exit-live. The CFG-aware DCE seeds function exits from the same helper and adds `carriedExitRegs`
  to every block's live-out.
- Generated-code check against the Phase 2.2 compiler (chess, crackman, paint, openworld_slice at
  -O0/-O2 on both targets): x86-64 identical, AArch64 -O0 identical, AArch64 -O2 net −11/−11/−9
  instructions (chess/crackman/paint). Two effects: (a) the CFG-aware DCE no longer seeds returning
  blocks with the callee-saved set (the epilogue restores them), so dead `mov xN, #0`
  materializations into callee-saved registers and the save/restore pairs they forced disappear;
  (b) `foldComputeIntoTarget` declines a fold whose ALU destination is an argument register read
  by the following one-argument call (five sites in chess, +1 `mov` each). The effects model reads
  every argument register at a call because `Bl` carries no arity; a call-site argument mask on
  the MIR call is Phase 3 item 16.
- Tests: `test_aarch64_mir_cfg` (the four disagreement shapes, direct-exit edges, dominators/back
  edges/natural loops/loop depth, `blockExitLive` for returning, branching, trapping, and
  fall-through blocks), `test_codegen_cfg_extract` (x86-64 `MirCfg` shapes), and B1 cases in
  `test_codegen_arm64_peephole_subpasses` (carried/return/call-argument registers block the fold
  and the fusion).

Phase 2.5 (differential coverage) is implemented:

- `src/tests/e2e/differential_opt_levels.cmake` and the `codegen_optdiff` label: every shared-corpus
  program (success and trap) and the deterministic `examples/il/` programs are built natively at
  -O0 and -O2 with the MIR verifier on and byte-compared (stdout and exit code). The VM-vs-native
  gate and this gate are registered by one function for both architectures: the AArch64
  registration is unchanged, and x86-64 hosts that link native code now get `differential_x64_*`
  and `optdiff_x64_*` — until now the x86-64 backend had no program-level oracle in the gate.
- `src/tests/common/ILKernelGenerator.{hpp,cpp}` (seeded IL text generator for the kernel shapes:
  checked-arithmetic chains, `idx.chk` reused across trap branches, div/rem by constants,
  `switch.i32` dispatch, select diamonds, leaf calls, phi-cycle inner loops, bit mixing) and
  `common/ILKernelDiff.hpp` (parse → verify → VM → native -O0 → native -O2, exit codes compared).
  `test_differential_il_kernels` runs a fixed seed range in the gate on the host backend;
  `fuzz_il_native_diff` (`ZANNA_ENABLE_FUZZ=ON`) is the unbounded libFuzzer form.
- First run of the kernel gate on x86-64 Linux: 18 of the first 48 seeds disagreed, in three
  classes, all fixed with a regression test each:
  - **IL `reassociate` rewrote multi-use values.** Its use counter only walked instruction
    operands, so a temporary passed to a successor's block parameter looked single-use and became
    an internal node of a flattened tree (`%m = and %s, M; %y = and %m, 8191; br next(%m)` turned
    `%m` into `M & 8191`). Branch arguments now count as uses (`test_il_reassociate`
    `BranchArgumentsCountAsUses`). 14 seeds.
  - **x86-64 `urem`/`srem` by a magic constant** formed `quotient * divisor` in RAX while the
    destination virtual register was defined by the following dividend copy; the allocator does
    not treat an explicitly named allocatable register as occupied between its write and its read
    and could hand the destination RAX, producing `sub rax, rax` (remainder 0). Visible at every
    level once the global pinning tier changed the free pool. The product now lives in the reserved
    scratch r11 (`test_x86_backend_regressions` `RemainderByMagicKeepsProductInReservedScratch`).
    2 seeds.
  - **Range-analysis narrowing budget.** CheckOpt demotes `iadd.ovf i, 1` to `add` from the
    loop-guard bound the whole-function range analysis proves; the verifier re-proves it with the
    same analysis. Narrowing after widening carried a recovered bound one CFG edge per sweep and
    was capped at two sweeps, so once `inline-o2` split the caller block into a chain of
    continuation blocks the proof was out of reach and the optimized module failed verification
    (`native-O2` exit 1). The budget now follows the block count with the same early exit
    (`test_il_int_range_analysis` `RecoveredLoopBoundReachesUsesManyBlocksPastHeader`,
    `VerifierAcceptsDemotedAddManyBlocksPastHeader`). 4 seeds.
  After the fixes the first 400 seeds agree on VM, native -O0, and native -O2.

Phase 3 (function-wide register allocation, C1) is complete on both backends; the results
table is in `docs/internals/backend.md` ("Codegen statistics baseline") and the model is ADR 0339.
Steps, each gate-green with the default pipeline unchanged unless stated:

- **C0 — metrics.** `aarch64/passes/CodegenStatsPass` and `x86_64/passes/CodegenStatsPass` print
  one `[codegen-stats]` line per function and module at every -O level when `ZANNA_CODEGEN_STATS`
  is set (instructions, loads/stores, frame loads/stores, offset prefixes, spill slots, frame
  bytes, callee-saved count); `scripts/codegen_stats.sh` builds the TSV for the demos and the IL
  benchmarks and diffs it against `docs/internals/codegen_stats_baseline.tsv`.
- **C1 — parallel copies.** `common/ra/ParallelCopy.hpp`: allocator-independent sequentialisation of
  a `(dst, src)` location bundle (dependency order, identity drop, cycle break through a scratch,
  mem-to-mem through a temp). x86-64 `Coalescer::lower` runs on it with byte-identical output.
- **C2 — `ParallelCopy` opcode.** The AArch64 pseudo (`dst0, src0, dst1, src1, …`, roles even=def
  / odd=use) with verifier rules `PCOPY` (shape; never survives RA) and `SCRATCH-EXIT` (reserved
  scratch never live out of a block); the emitters, encoder, expander, peepholes, and scheduler
  reject or skip it.
- **C3 — edge-copy lowering mode.** `AArch64Module::edgeCopyLowering` (off by default): block
  parameters are virtual registers, branch arguments one `ParallelCopy` per edge (inline for `br`,
  in the existing split block for `cbr`/`switch`), cross-block temporaries keep their vreg, blocks
  lower in an order where every definition precedes its uses. The shared corpus lowers and verifies
  in that mode (`test_aarch64_lowering_edge_copies`).
- **C4 — physical liveness for the post-RA passes.** `aarch64/PhysLiveness.{hpp,cpp}`
  (`computePhysLiveness`, moved out of the verifier) and `blockExitLive(fn, bi, target, liveness)`
  = solved live-out ∪ `carriedExitRegs` ∪ SP/FP/LR ∪ (return registers at a function exit). The
  twelve `carriedExitRegs` consumers (`tryFoldConsecutiveMoves`, `tryFoldImmThenMove`,
  `tryTbzTbnzFusion`, `tryCsetBranchFusion`, the three division rewrites, both DCE variants, the
  per-block and post-schedule drivers) take a `const PhysRegSet *exitLive` instead; each peephole
  stage solves liveness once on its input shape, and the phi-join forwarders and loop passes no
  longer publish carried metadata (`markCarriedExitReg` is gone). Under real liveness a callee-saved
  register is exit-live only when a successor reads it, so the conservative "every callee-saved
  register is live inside the function" seed is gone as well. Tests: `test_aarch64_phys_liveness`
  (edge read, kill, call clobber/argument, return, loop back edge, diamond, determinism, the exit
  seed, and the property *carried ⊆ solved live-out* over the allocated shared corpus),
  `test_aarch64_mir_cfg` and `test_codegen_arm64_peephole_subpasses` re-derived on successor reads
  instead of hand-set carried sets. Generated code at AArch64 -O2 (`scripts/codegen_stats.sh`
  against the Phase 3 baseline): chess 105,089 → 104,752 instructions, crackman 56,684 → 56,569,
  paint 55,230 → 55,127 (dead constant materializations and pinned-slot address computations into
  callee-saved registers, which the old seed kept alive, are gone); frame traffic, offset prefixes,
  and spill slots unchanged. openworld_slice 6,274 → 6,280: three `mov x5, x0; mov x0, x5` pairs
  before a return in blocks with a mid-block trap branch survive, because the trap call's effects
  read every argument register (`Bl` carries no arity) and the block-granular live-out includes
  the trap edge; a call-site argument mask (item 16) recovers them.

- **C5 — the function-wide allocator (opt-in).** `aarch64/ra/LiveIntervals.{hpp,cpp}` (positions
  in reverse post-order, range lists with holes from the CFG liveness solution, fixed physical
  ranges from the effects model, weights, hints) and `aarch64/ra/GlobalAllocator.{hpp,cpp}`
  (whole-interval linear scan with weight-based eviction and spill-everywhere, shared first-fit
  spill slots hottest-first, the rewrite with per-use reloads, per-definition stores, temporaries
  free at the instruction, `ParallelCopy` lowering through the shared sequentializer, EH-1/EH-2).
  `ZANNA_GLOBAL_RA=1` / `PipelineOptions::globalRegAlloc` selects the edge-copy lowering and this
  allocator for the module; `RegAllocPass` dispatches on `AArch64Module::edgeCopyLowering` and the
  peephole skips its phi-slot stages on that path; the pre-RA move coalescer (a layout-order hull)
  is not run on it — hints do that work. Tests: `test_aarch64_live_intervals`,
  `test_regalloc_aarch64_global`, the shared-corpus global lane in `test_aarch64_lowering_edge_copies`,
  and `test_regalloc_aarch64_oracle`, a MIR interpreter over seeded random edge-copy functions
  (straight-line arithmetic, slot round trips, calls that clobber every caller-saved register,
  diamonds, nested counted loops carrying up to 40 values in both classes) that must compute the
  same result before and after allocation — this is the execution oracle for the allocator on
  hosts without native AArch64 execution; 6,000 seeds agree. Every shared-corpus program, every
  `examples/il` program and benchmark, and the four demos compile and verify in this mode at -O0
  and -O2. Measurements (`ZANNA_GLOBAL_RA=1 scripts/codegen_stats.sh --baseline`, AArch64,
  against the Phase 3 baseline):

  | program | -O2 instructions | -O2 frame loads+stores | -O2 offset prefixes | -O2 spill slots | -O0 instructions |
  |---|---|---|---|---|---|
  | chess | 105,089 → 55,537 (−47%) | 21,394 → 4,946 | 12,381 → 64 | 9,977 → 200 | 89,607 → 74,321 |
  | crackman | 56,684 → 36,567 (−36%) | 9,870 → 2,914 | 3,289 → 0 | 4,450 → 43 | 57,055 → 46,988 |
  | paint | 55,230 → 40,293 (−27%) | 8,730 → 3,000 | 1,897 → 0 | 3,898 → 49 | 57,350 → 49,576 |
  | openworld_slice | 6,274 → 4,896 (−22%) | 578 → 364 | 27 → 0 | 258 → 0 | 7,060 → 5,861 |

  Every one of the 16 IL benchmarks loses all of its frame traffic and spill slots at both
  levels (instructions −15% to −52% at -O2); the shared corpus goes 1,276 → 968 instructions at
  -O2 and 2,019 → 1,475 at -O0. The kernel generator gained the `eh-catch` shape (an `eh.push`
  around a division that traps every fourth trip, state carried through entry allocas, the
  handler resuming into a recovery block that postdominates the pushing block; the phi-cycle
  shape already existed), so the seeded differential exercises native EH on every host backend.
  Still open before the flip (C6): the differential gates with `ZANNA_GLOBAL_RA=1` on an AArch64
  host.

- **C6 — the flip.** The function-wide allocator is the AArch64 default at every `-O` level;
  `ZANNA_LOCAL_RA=1` / `PipelineOptions::localRegAlloc` brings the whole retired path back
  (frame-slot lowering, block-local allocator with its `ZANNA_NO_GLOBAL_RA` toggle, phi-slot
  peephole stages) for bisecting until C7. ADR 0339 records the decision. The seven old-shape
  tests (`test_aarch64_cross_block_reload`, `test_codegen_arm64_cross_block_phi_spill`,
  `test_codegen_arm64_spill_fpr`, `test_aarch64_frame_spill_reuse`, `test_regalloc_aarch64_linear`,
  `test_aarch64_phi_coalescer`, `test_aarch64_global_liveness`) pin the old path explicitly so it
  stays covered until it is deleted; every other backend test runs the new default, and the whole
  codegen/golden/differential label set passes with and without `ZANNA_LOCAL_RA=1` on this
  x86-64 host. `scripts/native_opt_diff.sh` documents the switch. Not yet run: the AArch64
  program-level oracle (VM vs native, `-O0` vs `-O2`, the seeded kernels) on an AArch64 host — the
  allocator's execution evidence on this host is the MIR interpreter oracle.

- **C7 — the block-local path is gone.** One commit deletes the frame-slot lowering mode
  (`LivenessAnalysis`, `analyzeCrossBlockLiveness`, the def-site stores and block-entry reloads,
  the phi slots), the `PhiStoreGPR`/`PhiStoreFPR` opcodes and their eleven consumers, the
  block-local allocator (`ra/Allocator`, `ra/VState`, `ra/InstrBuilders`, `RegAllocLinear`), the
  pre-RA `Coalescer`, the FrameBuilder block-epoch slot reuse, `MBasicBlock::carriedExitRegs`
  with the verifier's `CARRY` rule and `carriedExitRegSet`, the phi-slot peephole stages
  (`forwardSinglePredPhiLoads`, `coalesceJoinPhiLoads`, `forwardLayoutSuccessorStoreLoad`,
  `eliminateLoopPhiSpills` and their join helpers, ~1,900 lines) with their `ZANNA_NO_PH_*`
  switches, the `ZANNA_LOCAL_RA` / `PipelineOptions::localRegAlloc` /
  `AArch64Module::edgeCopyLowering` fork, and the seven old-shape tests; `ZANNA_NO_GLOBAL_RA`
  now belongs to x86-64 only. `blockExitLive` is the solved live-out plus SP/FP/LR plus the
  return registers at a function exit, nothing else. The `MBasicBlock` initialisers, the
  pool-exhaustion diagnostic test (now a reserved-scratch exhaustion: three spilled sources plus
  an explicit x9 destination), the encoder coverage count, and the ARM-host loop-phi test
  (loop-carried parameters never touch the frame; no `ldp`, no `[x29, #-…]`) were re-derived
  on the remaining path. Net −2,650 lines.

- **C8a — shared assignment core, x86-64 physical liveness.** The position/range model and
  the whole-interval linear scan (hints, callee-saved preference across calls, weight-based
  eviction, fixed physical ranges) plus the first-fit slot sharing moved into
  `common/ra/IntervalAssign.hpp` (`RangeList`, `IntervalInfo`, `RegisterFile`,
  `IntervalAssigner`); the AArch64 allocator builds `IntervalInfo`s from its intervals and reads
  the assignment back, byte-for-byte identical output on the four demos at `-O0` and `-O2`.
  x86-64 gained `PhysLiveness.{hpp,cpp}` (`computePhysLiveness`, moved out of the verifier, and
  `blockExitLive` = solved live-out ∪ RSP/RBP ∪ return registers at a function exit); the
  block-local DCE and the move-chain folder seed from it instead of "every allocatable register
  is live at every exit" (`test_x86_phys_liveness`).

- **C8b — x86-64 on the function-wide allocator.** `x86_64/ra/LiveIntervals` is the interval
  model (positions in reverse post-order, range lists with holes from the CFG liveness solution,
  memory address registers as reads, fixed ranges from `effectsOf` including the implicit
  RAX/RDX/RCX effects and the call argument reads, `rt_native_eh_push`/`setjmp` positions, hints
  from `MOVrr`/`MOVSDrr`/`PX_COPY` pairs including the entry copies from the argument registers);
  `x86_64/ra/GlobalAllocator` runs the shared assigner and rewrites: operand substitution,
  reloads and stores around spilled operands into temporaries free at the instruction (pool,
  then the reserved R10/R11, then a pool register saved to a fresh slot and restored — never a
  register the instruction touches), `PX_COPY` through the shared sequentializer, per-class
  placeholder slots (`ra/SpillSlots.hpp`). Deleted: `ra/Allocator`, `ra/Coalescer`, `ra/Spiller`,
  the first/last-touch `LiveIntervals`, `RegAllocLinear.cpp`, the pin machinery
  (`common/ra/GlobalPinning.hpp` → `LoopDepths.hpp` keeps `computeLoopDepths`),
  `common/ra/ArchTraits.hpp`, `ZANNA_NO_GLOBAL_RA`, the stale `il_codegen_x86_64` CMake target,
  and the tests that pinned the block-local shapes (`test_x86_global_ra`,
  `test_codegen_x86_64_spiller`, `test_codegen_x86_64_coalescer`, `test_ra_victim_selection`);
  `test_codegen_x86_64_allocator`, `test_codegen_x86_64_live_intervals`,
  `test_regalloc_consistency`, and four `test_x86_backend_regressions` cases were re-derived.
  The native run of `42_try_catch_promises` caught a latent bug in the shared core: a value
  read by a longjmp handler has a hole over the protected body (no CFG edge models the
  longjmp), so a body-local value could share and overwrite its slot. EH-crossing values now
  keep private slots on both backends (`test_interval_assign`). Two hand-built test modules
  (`test_regalloc_stress`, `test_cf_stress`) spelled their branch targets with raw IL block
  names where the IL bridge emits `.L_<function>_<block>`; the MIR control-flow graph resolves
  labels exactly, so those loops were invisible to liveness and the block-local allocator had
  only masked it by spilling everything across blocks. Lowering now canonicalises a raw block
  name on `br`/`cbr`/`switch_i32` to the block's MIR label, and the library entry points
  (`emitModuleToAssembly`, `emitFunctionToAssembly`) run the MIR verifier after every stage
  when `ZANNA_VERIFY_MIR` is set, as the pipeline already did. `test_abi_probe` and two
  `test_cf_stress` counts were re-derived (pass-through arguments need no move; an empty
  block is threaded away; the last switch case may fall through under `jne`). The first
  results table showed x86-64 spilling in kernels that have no register pressure
  (`udiv_stress` -O2: 0 → 19 frame accesses): every checked operation branches to the shared
  overflow trap block, whose `call rt_trap_ovf` read every argument register under the
  arity-less effects model, so RDI..R9 and RAX were live around every loop that can trap and
  the pool shrank to RBX/R12..R15. Item 16 landed for x86-64 in its minimal form: a `CALL`
  carries `MInstr::callArgMask`, `lowerCall` records exactly the registers it marshalled (plus
  RAX for a SysV vararg call), the trap and startup calls record theirs by hand, and `effectsOf`
  reads only those (a mask-less `CALL` keeps the old every-register reading). An in-block
  variant tried first ("a call reads an argument register only if its block wrote it") was
  unsound: the marshalling sequence can be split by a guard branch
  (`native_run_sccp_transient_trap_O1`), so the mask is recorded at the one place that knows
  the arity. AArch64 `Bl`/`Blr` are unchanged (its pool is wide enough that the trap-block
  pollution costs nothing measurable; the same mask is the natural follow-up).

- **C9 — docs and results.** `backend.md` carries the after-table for both targets
  (`codegen_stats_phase3.tsv` has every column), the register-allocation section describes
  the shared model, and the kill-switch list no longer names `ZANNA_NO_GLOBAL_RA` or
  `ZANNA_LOCAL_RA`. Across the 20 programs at `-O2`: AArch64 224,277 → 137,951 instructions and
  40,670 → 11,224 frame accesses; x86-64 224,706 → 190,872 instructions and 24,434 → 14,736
  frame accesses; no program's instruction count rises except `mixed_stress` `-O0` by two.
  The AArch64 execution gates (differential, `-O0` vs `-O2`, seeded kernels) and the ARM-host
  ctests were last run on an ARM host at C6; C7, C8 and the argument-mask change were verified
  here by the asm-only AArch64 lanes (shared corpus, verifier, byte-identical demo assembly
  through C8b) and need one run on an ARM host.

Everything from B2 onward is open; of the Phase 3 follow-ups, 12 is moot (intervals end at
last use by construction), 13 is partly done (the entry copy is one `PX_COPY`; call arguments
are still one move each), 16 is done for x86-64 and open for AArch64, and 14/15 are open.

## Context

A deep read of `src/codegen/{aarch64,x86_64,common}` (allocators, coalescers, lowering, legalization, ISel, schedulers, all post-RA peepholes, frame/call lowering, emitters) found:

- **Two reachable miscompile bugs** (one shared pre-RA pass, one x86-64 post-RA pass) plus a real text-emitter/allocator scratch-register collision.
- **A family of latent hazards** with the same root cause: side effects that are invisible in MIR operands (implicit RAX/RDX defs, emit-time scratch writes, block-exit register carries) are modeled by *some* passes and not others. Each backend has 4-5 hand-rolled CFG/liveness reasoners that disagree at the edges.
- **The dominant performance gap is structural**: both register allocators are block-local. Every value crossing a block boundary goes through a frame slot unless a single-pred fallthrough carry or a pinning heuristic rescues it; loop headers reload every live-in on every iteration. A large, risky post-RA memory-forwarding stack exists only to undo that (and has been the source of ZB-29/30/31 and the plan-88 bisect).

Goal: fix the bugs with fail-first tests, then remove the bug *class* via shared infrastructure and a MIR verifier, then close the performance gap with a global allocator, staged so the VM-vs-native differential oracle stays green at every step.

No IL opcode, grammar, verifier-rule, or runtime C ABI changes are needed (no ADR required). CLAUDE.md rules apply: build scripts only, full Zanna headers, no agents writing code, no commits by Claude.

---

## Findings

### A. Bugs to fix now (write the failing test first)

**A1. Shared pre-RA copy forwarder drops uses after an in-block conditional branch** — `src/codegen/common/PreRAForwardCopy.hpp` `findSingleDirectUse`.
After the first use of `dst` is recorded, hitting a non-call boundary (`BCond`, `Cbz`, `Tbz`, `JumpTable` on AArch64) `break`s and returns the site, so a second use later in the block is never seen. The copy is erased and the later use reads an undefined vreg.
Reachable on AArch64 at -O1+: vreg→vreg `MovRR` copies come from `idx.chk` with lo=0 (`src/codegen/aarch64/InstrLowering.cpp:1836`), `gep base, 0` (`:1119`, `:2632`), same-width casts (`:2360`); in-block `BCond` comes from `lowerOverflowOps` (`LowerOvf.cpp`, runs in `LegalizePass` *before* `PreRegAllocOptPass`), `emitSubWidthCheckedBinary`, and idx/null checks. Pattern: `i = idx.chk(...)`; `a[i]` load; `s = iadd.ovf ...` (→ `b.vs`); `b[i]` load.
x86-64 is protected only because `splitInternalLabelBlocks` makes `JCC` a block terminator.
Fix: count *all* uses of `dst` in the block (or continue scanning past non-call boundaries and bail on any additional use); require exactly one use total. Test in `src/tests/unit/codegen/test_codegen_preregalloc_opt.cpp` for both backends: `MovRR v2,v1; AddRRR v3,v2,v1; BCond vs L; AddRRR v4,v2,v1` → 0 forwarded.

**A2. x86-64 store→load forwarder ignores implicit RAX/RDX defs** — `src/codegen/x86_64/peephole/MemoryOpt.cpp` `forwardFrameStoreLoads` / `eraseStoresClobberedBy` → `definesOperandReg` (explicit def operands only).
`CQO`, `IDIVrm`, `DIVrm`, `MULr`, `IMULr` write RDX/RAX implicitly and are not memory barriers. The allocator spills a value living in RDX before `CQO` (`collectPhysicalClobbers` in `ra/Allocator.cpp`), the division clobbers RDX, a later reload of the slot in the same block is rewritten to `mov r, rdx` → wrong value. `MovFolding.cpp::defRegMask` already models these; MemoryOpt does not.
Fix: add `implicitDefs(MOpcode)`/`implicitUses(MOpcode)` to `src/codegen/x86_64/OperandRoles.{hpp,cpp}` and use them in MemoryOpt (both functions), MovFolding, DCE (`getDefReg` returns only the first explicit def), Scheduler. Test in `src/tests/unit/codegen/test_x86_peephole.cpp` next to the existing `forwardFrameStoreLoads` tests (~line 975): `MOVrm [rbp-16],rdx; CQO; IDIVrm rcx; MOVmr rbx,[rbp-16]` → 0 forwarded.

**A3. AArch64 text emitter clobbers operands with unguarded scratch** — `src/codegen/aarch64/AsmEmitter.cpp` `emitAddRI/emitSubRI/emitAndRI/emitOrrRI/emitEorRI` (`:489-559`, always `kScratchGPR`=x9), `emitCmpRI` (`:606`, x16), `AddsRI/SubsRI` (`:1574`, x16), `FMovRI` fallback (`:1164`, x16).
The allocator hands out x9/x16/x17 as emergency reload registers for the *same* instruction (`ra/Allocator.cpp` `handleSpilledOperand` → `chooseEmergencyScratch`), fast paths keep values in x9 across several instructions (`fastpaths/FastPaths_Arithmetic.cpp:97`, `FastPaths_Cast.cpp:128`), and `StrengthReduce.cpp` uses x9/x16 as temps. `mov x9,#imm; add dst, x9, x9` is then wrong. The binary encoder instead *throws* for un-legalized AddRI immediates (`A64BinaryEncoder.cpp:2215`) and uses `chooseGprScratch(rn)` for CmpRI — the two emitters diverge.
Fix: every scratch pick in `AsmEmitter.cpp` goes through the existing `chooseGprScratch({dst, lhs, ...})` (`:673`); make the binary encoder's AddRI/SubRI/AndRI/etc. fallback identical (or make both throw and rely on A4). Test: `test_emit_aarch64_mir_*`: `AddRI x9, x9, #0x123456` and `CmpRI x16, #0x123456` must not use the operand register as temp; assert text and binary paths agree on a corpus (`test_codegen_arm64_native_asm.cpp` pattern).

**A4. Emit-time scratch writes are invisible to post-RA passes** — `src/codegen/aarch64/passes/SchedulerPass.cpp` `mayClobberEmitScratch` (`:497-548`) lists only large-offset loads/stores and `AddFpImm`; it omits the AddRI/SubRI/AndRI/OrrRI/EorRI/CmpRI/AddsRI/SubsRI/FMovRI big-immediate forms from A3. `propagateCopies`, `removeDeadInstructions*`, `forwardStoreLoads`, `tryMaddFusion` model none of them.
Fix (preferred, removes the class): a post-RA `ExpandPseudosPass` on AArch64 that rewrites every emit-time multi-instruction form into explicit MIR (`MovRI xS,#imm; AddRRR dst,lhs,xS`, `MovRI xS,#off; AddRRR xS,base,xS; LdrRegBaseImm …`), run right after `RegAllocPass` and before BlockLayout/Peephole/Scheduler, choosing xS via `chooseGprScratch` against the instruction's operands. Then the emitters assert the immediate is encodable. Interim: one shared `emitScratchClobbers(const MInstr&)` in `peephole/PeepholeCommon.hpp` used by the scheduler and every peephole.

### B. Latent hazards (cheap fixes + tests)

- **B1.** (fixed in Phase 2.3) `foldComputeIntoTarget` (`aarch64/peephole/CopyPropDCE.cpp`) and `tryMaddFusion` (`aarch64/peephole/MemoryOpt.cpp`) treat an unconditional block end as "register dead" without consulting `carriedExitRegs`; `tryFoldImmThenMove`/`tryTbzTbnzFusion` do consult it. Masked today only because the end-of-block spill store is still present when they run. Add the `carriedExitRegs` parameter and a shared `blockExitLive(block, target)` helper.
- **B2.** `eliminateDeadFpStores` and `forwardStoreLoads` (AArch64) key on exact offsets and ignore sub-word `Ldr8/16/32RegFpImm` / `Str8/16/32RegFpImm`: `str x0,[fp,#-16]; ldr w1,[fp,#-16]; str x2,[fp,#-16]` deletes the first store. Use byte-range overlap for every FpImm width (extend `fpStoreRange` to loads and sub-word forms).
- **B3.** x86 `ra/Coalescer.cpp::lower` leaves `dstState.hasPhys/cachedInBlock` set after a Mem-dest PX_COPY (stale register). Unreachable under SSA dominance; invalidate + assert.
- **B4.** x86 `LowerOvf.cpp` 3-operand form `mov dest,lhs; op dest,rhs` assumes `dest != rhs`. Assert (or swap for commutative ops).
- **B5.** AArch64 emits nothing after `bl rt_trap_*` (no-return set in `Noreturn.hpp`); the runtime's `vm_trap` hook "may return" (`src/runtime/core/rt_io.c` `rt_trap_dispatch`). x86 emits `UD2`. Add a `Brk` MOpcode (`MOpcodeDef.inc`, both emitters, operand roles, classifiers) and emit it after every no-return call in `TerminatorLowering`/`LowerOvf`/`LowerDiv`-equivalents.
- **B6.** `assignPinnedSlots` (`aarch64/ra/Allocator.cpp`) records `SlotStats::fpr` from the *last* access; a slot touched by both classes pins one class and leaves the other's accesses hitting stale memory. Track `mixedClass` and disqualify.
- **B7.** x86 frame placeholders (`FrameLowering.cpp::decodeFrameSlotPlaceholder`) collide with real `[rbp-N]` displacements; strict mode throws on any N%8≠0 (e.g., after `ISel::foldLeaIntoMem` folds a +4 GEP off an alloca). Replace with an explicit `OpFrameIndex` operand kind in `MachineIR.hpp` (resolved in `assignSpillSlots`), so no arithmetic on placeholders is possible.
- **B8.** Pool exhaustion ICEs: AArch64 `assignNewPhysReg`/`RegPools::takeGPR` and x86 `takeRegister` throw when every resident is protected. Add a unit test with max-arity instructions under full pressure (`test_ra_victim_selection.cpp` / `test_codegen_arm64_ra_many_temps.cpp`) and fall back to reserved scratch instead of throwing.

### C. Structural / performance

- **C1. Block-local allocation (both backends).** AArch64: `LivenessAnalysis::analyzeCrossBlockLiveness` spills every cross-block IL temp to a slot at lowering time; the allocator (`ra/Allocator.cpp`) is per-block with single-pred exit-state re-adoption and slot pinning. x86: same model with `canCarryIntoNextBlock` + `crossBlockSpillVRegs_` + `assignPinnedGlobals`. Loop headers always reload; loops containing calls get no pinning/coalescing at all (`assignPinnedSlots`, `eliminateLoopPhiSpills`, `coalesceJoinPhiLoads`). Roughly 2,500 lines of post-RA memory forwarding exist to compensate.
- **C2.** AArch64 never releases a register at a value's last use (`materialize` only under pressure) → extra spills at calls.
- **C3.** x86 `CallLowering.cpp` routes every GPR argument through R11 (2 movs/arg) and reserves R10+R11 permanently (12 allocatable GPRs).
- **C4.** x86 post-RA DCE seeds *all* allocatable registers live for any block with successors (`Peephole.cpp:181` `blockMayTransferControl`) → DCE only in return blocks. AArch64 already has the CFG-aware `removeDeadInstructionsCFG`.
- **C5.** Compile time: `aarch64/Coalescer.cpp::coalesceClass` recomputes intervals and restarts after every merge; `ISel::foldLeaIntoMem` calls `countVirtualRegisterUsesInFunction` per memory operand; `FrameBuilder::findLatestSpillSlot` is a reverse linear scan per query.
- **C6.** Five CFG builders per backend with different edge rules (RA `CfgExtract`, `CopyPropDCE::buildSuccessors`, `Peephole::buildPredecessorMap`, `LoopOpt` preds, `Dominators`).

---

## Plan

### Phase 1 — Fail-first regression tests and bug fixes (small, independent commits)

1. **A1** `PreRAForwardCopy.hpp`: pre-scan the block for the total use count of `dst`; forward only when it is exactly 1 and that use precedes any boundary/call. Tests for both traits.
2. **A2** `x86_64/OperandRoles.{hpp,cpp}`: add `implicitDefs`/`implicitUses` (CQO, IDIVrm, DIVrm, MULr, IMULr, CALL, SHL/SHR/SARrc→RCX). Consume in `peephole/MemoryOpt.cpp`, `MovFolding.cpp` (replace local switch), `DCE.cpp` (`getDefReg` → mask), `Scheduler.cpp`. Test above.
3. **A3** `aarch64/AsmEmitter.cpp`: all scratch selection through `chooseGprScratch({operands})`; `A64BinaryEncoder.cpp` AddRI/SubRI/logical fallback made identical to the text path. Emitter-agreement test.
4. **B1, B2, B3, B4, B6, B8** as listed, each with a unit test in the matching `test_codegen_arm64_peephole_subpasses.cpp` / `test_x86_peephole.cpp` / `test_regalloc_aarch64_linear.cpp` file.
5. **B5** `Brk` opcode + emission after no-return calls (both emitters, `OperandRoles.cpp`, `OpcodeClassify.hpp`, `Noreturn.hpp` callers). Golden updates via `./scripts/update_goldens.sh`.

### Phase 2 — Remove the bug class: shared effects table, one CFG, MIR verifier

6. **Implicit-effects table per backend** (`aarch64/ra/OperandRoles.cpp`, `x86_64/OperandRoles.cpp`): `InstrEffects effectsOf(const MInstr&, const TargetInfo&)` = explicit uses/defs + implicit regs + flags + memory kind + emit-scratch clobbers (until A4's expand pass lands). Every RA clobber scan, scheduler, DCE, copy-prop, store-load forwarder consumes only this.
7. **A4 `ExpandPseudosPass` (AArch64)**: explicit scratch materialization post-RA; emitters assert encodability. Delete `mayClobberEmitScratch`.
8. **One `MirCfg` utility** in `src/codegen/common/ra/CfgExtract.hpp` (already backend-neutral) with predecessor/dominator/loop-depth helpers; replace the four local builders on AArch64 and the x86 equivalents. Add `blockExitLive()` on top of it (B1).
9. **`MirVerifier`** (`src/codegen/{aarch64,x86_64}/MirVerify.{hpp,cpp}`): after lowering (SSA-ish: single def per vreg per block, operand roles classified for every opcode — `operandRoles` already throws), after RA (no vregs; every phys use reaches a def, ABI live-in, or `carriedExitRegs`; reserved scratch never live across an instruction that may clobber it), after each peephole stage (terminator placement, frame offsets inside `frame.totalBytes`, spill-slot lifetimes). Enabled by `ZANNA_VERIFY_MIR=1`, always on in unit tests and in `test_diff_vm_native*`. Reuses the existing kill-switch style (`backendStageDisabled`).
10. **Differential coverage**: promote `scripts/native_opt_diff.sh` into a ctest label (`codegen_optdiff`: `-O0` vs `-O2` native on `examples/` + `src/tests/codegen/aarch64/test_shared_il_corpus.cpp` corpus); add a fuzz harness under `src/tests/fuzz/` that generates small IL kernels (checked arithmetic, idx.chk, loops with calls, phi cycles, div/rem by constants) and compares VM vs native at `-O2`.

### Phase 3 — Performance

11. **Global register allocation** (the big one; AArch64 first, x86 second, old allocator kept behind `ZANNA_LOCAL_RA=1` for bisect):
    - Lowering stops routing cross-block temps through frame slots (`analyzeCrossBlockLiveness` + `PhiStore*` → edge parallel copies like x86's `buildEdgeCopyBlock` in `x86_64/LowerILToMIR.cpp:779`; split critical edges).
    - Function-wide liveness (`common/ra/DataflowLiveness.hpp` already exists) → live ranges with holes; linear scan over the whole function with interval splitting at calls (callee-saved preference for ranges crossing calls, as `nextUseAfterCall` does locally today); spill-everywhere with slot sharing keyed by interval interference (generalize `FrameBuilder::ensureSpillWithReuse` to function-wide indices, dropping the per-block epoch rule).
    - Parallel-copy resolution on edges (port `x86_64/ra/Coalescer.cpp::lower`, which already breaks cycles); delete `PhiStore*`, `carriedExitRegs`, `restoreFromPredecessor`, `assignPinnedSlots`, and the loop-phi / join-phi forwarding stages in `Peephole.cpp` (`forwardSinglePredPhiLoads`, `coalesceJoinPhiLoads`, `eliminateLoopPhiSpills`, `forwardLayoutSuccessorStoreLoad`) once the differential gate is green without them.
    - Success metric: instructions and loads/stores per function from `ZANNA_CODEGEN_STATS=1` on the demo games and `test_codegen_arm64_benchmark_regressions.cpp`; VM-vs-native differential and `run_cross_platform_smoke.sh` green.
12. **C2** release at last use in `allocateInstruction` (uses `usePositions*_` + `isLiveOut`) — cheap, do before 11.
13. **C3** x86 argument marshalling as one `PX_COPY` per call (reuse `Coalescer::lower`), free R11 for allocation (keep R10 for cycle breaking or use `XCHG`).
14. **C4** port `removeDeadInstructionsCFG` to x86 on top of `MirCfg`.
15. **C5** precompute vreg use/def counts once per function for `foldLeaIntoMem`/`runAddressingFolds`; make `coalesceClass` incremental (update intervals on merge instead of restart); index spill slots by vreg in `FrameBuilder`.
16. **Call-site argument masks.** `Bl`/`Blr` (and x86 `CALL`) carry no arity, so `effectsOf` reads every argument register at every call. *x86-64 done in Phase 3 C8b (`MInstr::callArgMask`); AArch64 open.* Record the integer/FP argument-register counts on the MIR call at lowering and read only those: it restores the five `foldComputeIntoTarget` folds Phase 2.3 declines in `chess` (ALU result in an argument register the next one-argument call does not read), lets DCE drop dead argument-register writes before calls, and removes false scheduler dependencies. Measured on `chess` -O2: 12,447 frame-access prefixes (Phase 2.2) dwarf this, so it goes after global RA.

---

## Verification

- Every Phase 1 item: unit test that fails before the fix and passes after (`ctest --test-dir build -R <test> --output-on-failure`).
- Full local gate before each report: `./scripts/build_zanna_unix.sh` (no skip flags), `./scripts/lint_platform_policy.sh`, `./scripts/run_cross_platform_smoke.sh`.
- Backend-specific: `ctest -L codegen`, `ctest -L golden`, `src/tests/e2e/differential_vm_native.cmake` corpus for both arches, `scripts/native_opt_diff.sh` on every example (`-O0` vs `-O2`), demo games via `./scripts/build_demos.sh`.
- Phase 3: `ZANNA_LOCAL_RA=1` vs default must produce identical program output across the whole corpus; `ZANNA_CODEGEN_STATS=1` load/store counts recorded before/after in `docs/internals/backend.md`.
- Docs: update `docs/internals/backend.md` (pass order, new kill switches `ZANNA_VERIFY_MIR`, `ZANNA_LOCAL_RA`, removed stages) and add a short ADR-style note only if the frame-index operand (B7) changes MIR dump format goldens.
