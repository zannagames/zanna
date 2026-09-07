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

Everything from B1 onward is open.

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

- **B1.** `foldComputeIntoTarget` (`aarch64/peephole/CopyPropDCE.cpp`) and `tryMaddFusion` (`aarch64/peephole/MemoryOpt.cpp`) treat an unconditional block end as "register dead" without consulting `carriedExitRegs`; `tryFoldImmThenMove`/`tryTbzTbnzFusion` do consult it. Masked today only because the end-of-block spill store is still present when they run. Add the `carriedExitRegs` parameter and a shared `blockExitLive(block, target)` helper.
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

---

## Verification

- Every Phase 1 item: unit test that fails before the fix and passes after (`ctest --test-dir build -R <test> --output-on-failure`).
- Full local gate before each report: `./scripts/build_zanna_unix.sh` (no skip flags), `./scripts/lint_platform_policy.sh`, `./scripts/run_cross_platform_smoke.sh`.
- Backend-specific: `ctest -L codegen`, `ctest -L golden`, `src/tests/e2e/differential_vm_native.cmake` corpus for both arches, `scripts/native_opt_diff.sh` on every example (`-O0` vs `-O2`), demo games via `./scripts/build_demos.sh`.
- Phase 3: `ZANNA_LOCAL_RA=1` vs default must produce identical program output across the whole corpus; `ZANNA_CODEGEN_STATS=1` load/store counts recorded before/after in `docs/internals/backend.md`.
- Docs: update `docs/internals/backend.md` (pass order, new kill switches `ZANNA_VERIFY_MIR`, `ZANNA_LOCAL_RA`, removed stages) and add a short ADR-style note only if the frame-index operand (B7) changes MIR dump format goldens.
