---
status: active
audience: contributors
last-verified: 2026-07-26
---

# AArch64 (arm64) Backend — Status

This document captures the current state of the AArch64 backend, recent bug
fixes, and known limitations. It is kept developer-focused with concrete source
references and test cases.

## Executive Summary

### Current Status (May 2026)

- **End-to-end validated on Apple Silicon**: All demo games compile and run natively
- **Core pipeline mature**: MIR layer, instruction selection, register allocation (with coalescer and protected-use eviction), frame lowering, peephole optimization (6 sub-passes), post-RA scheduler, linker integration
- **Immediate utils**: Extracted `A64ImmediateUtils.hpp` for consistent immediate encoding
- **Binary encoder**: Direct object code emission (bypassing assembler text)
- **Large-module compile path**: `CodegenPipeline` accepts an already-verified in-memory IL module from `zanna build`,
  avoiding a textual IL round trip for project builds.
- **Parallel backend passes**: IL-to-MIR lowering and register allocation run per function concurrently while preserving
  deterministic function order in the module.
- **Fastpaths**: Arithmetic and call fastpath optimizations for common patterns
- **Register allocator hardening**: Protected-use sets prevent source-operand eviction during def allocation; FPR load/store classification; operandRoles fix for immediate-ALU instructions; clean FPR spill slot reuse across calls; dead vreg early release
- **Emergency spill reloads**: Reserved scratch registers cover no-free-register reload cases instead of letting `takeGPR` / `takeFPR` abort mid-instruction.
- **Cross-target output plumbing**: `--target-darwin`, `--target-linux`, and `--target-windows` now drive the native object format, linker platform, and system-assembler target instead of silently falling back to the host
- **Trap ABI fixes**: plain `trap` still marshals `x0 = NULL`, `idx.chk` raises structured bounds errors through `rt_trap_raise_error`, checked div/rem and overflow traps call `rt_trap_div0` / `rt_trap_ovf`, and `trap.from_err` marshals its code into `x0` before calling `rt_trap_raise_error`
- **Checked FP casts**: `cast.fp_to_si.rte.chk`, `cast.fp_to_ui.rte.chk`, and `fptosi` now lower with explicit NaN/range checks before `FCvtZS` / `FCvtZU`, reporting `InvalidCast` for NaN/invalid unsigned inputs and `Overflow` for range failures. The bounds honor `i1`, `i16`, `i32`, and `i64` result widths where applicable.
- **FP compare NaN semantics**: all ordered `fcmp_*` predicates mask primitive AArch64 `FCMP` conditions with `vc`; `fcmp_ne` is unordered-true via `ne || vs`; `fcmp_ord` / `fcmp_uno` materialize `vc` / `vs` directly. Conditional branches over FP compares use the same materialized boolean path.
- **Memory addressing**: general `load` / `store` now preserve optional immediate displacements instead of dropping them for base-register addressing.
- **Type-safe phi edges**: phi-edge copies reject GPR/FPR class mismatches instead of silently inserting numeric conversions.
- **Strict address lowering**: unsupported `addr_of` operands now fall through as unsupported instead of reporting success without defining the result.
- **Width-sensitive checks**: annotated sub-width checked arithmetic and `idx.chk` sign-extend operands to the annotated width before checking. Checked narrowing compares the widened result and traps on overflow.
- **Runtime error access**: `ErrGetMsg` lowers to `rt_throw_msg_get`, matching the VM/native EH contract.
- **Control-flow correctness**: `switch.i32` edge arguments travel through phi spill slots via dedicated edge blocks, and larger switches lower as balanced decision trees instead of pure compare chains
- **Call ABI hardening**: direct calls to module-defined variadic callees (`...`) honor target-specific variadic placement: Darwin spills anonymous variadic args to the stack, while Linux and Windows continue through the normal register banks. Malformed direct-call IL is diagnosed instead of being silently dropped.
- **Emitter parity**: text assembly and the binary encoder now gate BTI / PACIASP / AUTIASP on target policy; branch-target identification and return-address signing are emitted by default on Darwin and skipped for Linux/Windows objects. Text emission keeps internal branch targets assembler-local under Darwin `.subsections_via_symbols` so conditional trap branches assemble with Apple `as`. `AdrPage` / `AddPageOff` render Mach-O `@PAGE` / `@PAGEOFF` on Darwin and ELF/COFF `:lo12:` syntax on Linux/Windows.
- **Object metadata**: binary rodata references use exact section-offset relocations, rodata assembly sections are target-specific (`__TEXT,__const`, `.rodata`, `.rdata`), string escapes use fixed-width octal for nonprintable bytes, and Windows ARM64 COFF emission records `.pdata` / `.xdata` unwind metadata for framed functions.
- **Strength reduction**: signed and unsigned division by arbitrary constants now lower through magic-multiply sequences; `UmulhRRR` is part of the MIR and binary encoder surface
- **Peephole hardening**: optimized builds now run block layout before peephole cleanup, then schedule, then run a
  final peephole cleanup. The peephole pass validates that no virtual registers remain, branch targets still name MIR
  blocks, and non-terminators do not follow terminators.
- **Target-aware DCE**: AArch64 peephole DCE can use `TargetInfo` for CFG-level physical-register liveness, including
  callee-saved FPRs and call-clobber/argument registers.
- **Loop cleanup**: loop phi spill/reload cleanup iterates to a bounded fixed point and re-runs cross-block dead
  frame-store cleanup after each successful iteration. Loop-phi spill elimination now accepts only natural loop
  back-edges whose header dominates the latch, so layout-created backward branches to if/else joins keep their
  phi-slot stores intact.
- **Dead spill-store safety**: whole-function FP-relative dead-store cleanup is limited to compiler-created spill
  slots recorded in `MFunction::frame.spills`; addressable stack locals/allocas are preserved because they can be
  observed through derived base-register loads even when no direct `LdrRegFpImm` remains.
- **Join forwarding safety**: cross-block phi-load forwarding tracks the latest store for each frame offset, even when
  that store cannot be forwarded because its source register is clobbered before the edge. This prevents stale older
  stores from replacing join-entry reloads.
- **Scheduler alias safety**: post-RA scheduling only disambiguates memory through explicit FP/SP-derived stack
  addresses. Heap/object/list accesses through base registers are conservatively treated as may-alias, including
  pointers loaded from different frame slots, so object updates cannot be reordered ahead of later reads through an
  aliased register.
- **117+ codegen test files**

## Source File Map

### Target description and ABI

| File | Purpose |
|------|---------|
| `src/codegen/aarch64/TargetAArch64.hpp`/`.cpp` | `PhysReg` enum (X0–X30, SP, V0–V31), `TargetInfo` struct, `darwinTarget()` / `linuxTarget()` / `windowsTarget()` singletons, `isGPR()`, `isFPR()`, `regName()` |

`TargetInfo` contents:

- `darwinTarget()`, `linuxTarget()`, and `windowsTarget()` return `const TargetInfo &` singletons with:
    - Caller/callee-saved sets (AAPCS64), arg register orders (X0–X7, V0–V7), return regs (X0, V0), stack alignment (16)
- `TargetInfo::abiFormat` drives symbol mangling and assembler/object-file dialect (Mach-O, ELF, COFF)
- `TargetInfo::usesStackVariadicTail()` is true for Darwin and false for Linux/Windows, matching platform vararg ABIs.
- `isGPR`, `isFPR` classify physical registers
- `regName` renders canonical string names (e.g., `"x0"`, `"v15"`)

### Machine IR

| File | Purpose |
|------|---------|
| `src/codegen/aarch64/MachineIR.hpp`/`.cpp` | `MOpcode` enum, `MReg`, `MOperand`, `MInstr`, `MBasicBlock`, `MFunction` |
| `src/codegen/aarch64/MachineIR.hpp`/`.cpp` | `MFunction::savedGPRs` — callee-saved GPR list for prologue/epilogue |
| `src/codegen/aarch64/generated/OpcodeDispatch.inc` | Generated opcode dispatch table |

MIR opcode categories:

- Integer arithmetic: `AddRRR`, `AddRI`, `SubRRR`, `SubRI`, `MulRRR`, `SDivRRR`, `UDivRRR`, `MSubRRRR`, `MAddRRRR`
- Checked arithmetic: `AddOvfRRR`, `AddOvfRI`, `SubOvfRRR`, `SubOvfRI`, `MulOvfRRR`
- Integer bitwise: `AndRRR`, `AndRI`, `EorRRR`, `EorRI`, `OrrRRR`, `OrrRI`, `TstRR`
- Integer shift: `AsrRI`, `AsrvRRR`, `LslRI`, `LslvRRR`, `LsrRI`, `LsrvRRR`
- Integer compare/select: `CmpRI`, `CmpRR`, `Csel`, `Cset`
- Moves: `MovRI`, `MovRR`
- Branches: `BCond`, `Bl`, `Blr`, `Br`, `Cbz`, `Cbnz`, `Ret`
- Address computation: `AddPageOff`, `AdrPage`
- Floating-point: `FAddRRR`, `FCmpRR`, `FCvtZS`, `FCvtZU`, `FDivRRR`, `FMovGR`, `FMovRI`, `FMovRR`,
  `FMulRRR`, `FRintN`, `FSubRRR`, `SCvtF`, `UCvtF`
- Memory (FP-relative): `AddFpImm`, `LdrFprFpImm`, `LdrRegFpImm`, `LdpFprFpImm`, `LdpRegFpImm`,
  `StpFprFpImm`, `StpRegFpImm`, `StrFprFpImm`, `StrRegFpImm`
- Memory (base+offset): `LdrFprBaseImm`, `LdrRegBaseImm`, `StrFprBaseImm`, `StrRegBaseImm`
- Stack pointer: `AddSpImm`, `StrFprSpImm`, `StrRegSpImm`, `SubSpImm`

### Assembly emitter

| File | Purpose |
|------|---------|
| `src/codegen/aarch64/AsmEmitter.hpp`/`.cpp` | AT&T-compatible AArch64 text emitter |
| `src/codegen/aarch64/RodataPool.hpp`/`.cpp` | String literal pool (deduplication, unique labels) |

`AsmEmitter` capabilities:

- `emitFunction(os, fn)` — full MIR function to assembly (header, prologue, blocks, epilogue)
- `emitBlock(os, bb)` — single basic block with label
- `emitInstruction(os, mi)` — individual MIR instruction
- `emitFunctionHeader(os, name)` — `.globl` + `name:` directives
- `emitPrologue(os)` / `emitEpilogue(os)` — default `stp x29, x30, [sp, #-16]!; mov x29, sp` / `ldp x29, x30, [sp], #16; ret`
- `emitPrologue(os, plan)` / `emitEpilogue(os, plan)` — callee-saved GPR save/restore via `FramePlan`
- Integer ops: `emitMovRR`, `emitMovRI`, `emitAddRRR`, `emitSubRRR`, `emitMulRRR`, `emitAddRI`, `emitSubRI`,
  `emitAndRRR`, `emitAndRI`, `emitOrrRRR`, `emitOrrRI`, `emitEorRRR`, `emitEorRI`, `emitLslRI`, `emitLsrRI`,
  `emitAsrRI`
- Chunk-safe SP adjustment: `emitSubSp` / `emitAddSp` use 4080-byte chunks (largest 16-byte-aligned 12-bit value)
- Internal `mangleSymbol()` static function in `AsmEmitter.cpp` provides platform-correct symbol mangling (`_` prefix on Darwin)
- Unknown MIR opcodes are hard emitter errors instead of comments in emitted assembly.

### IL to MIR lowering

| File | Purpose |
|------|---------|
| `src/codegen/aarch64/LowerILToMIR.hpp`/`.cpp` | `LowerILToMIR::lowerFunction()` — top-level IL → MIR entry point |
| `src/codegen/aarch64/InstrLowering.hpp`/`.cpp` | Per-opcode lowering handlers (`materializeValueToVReg`, arithmetic, FP, OOP) |
| `src/codegen/aarch64/TerminatorLowering.hpp`/`.cpp` | Branch/return lowering; CBr entry-block restriction (correctness guard) |
| `src/codegen/aarch64/LoweringContext.hpp` | Per-function mutable state threaded through all lowering handlers |
| `src/codegen/aarch64/OpcodeMappings.hpp` | IL opcode → MIR opcode tables |
| `src/codegen/aarch64/OpcodeDispatch.hpp`/`.cpp` | Dispatch infrastructure for per-opcode lowering |

### Fast paths

| File | Purpose |
|------|---------|
| `src/codegen/aarch64/FastPaths.hpp`/`.cpp` | Fast-path dispatcher for common single-block patterns |
| `src/codegen/aarch64/fastpaths/FastPaths_Arithmetic.cpp` | Arithmetic fast paths (register-register and register-immediate; shifts excluded from commutative swap; sub-width checked arithmetic and checked chains fall back to generic lowering) |
| `src/codegen/aarch64/fastpaths/FastPaths_Call.cpp` | Call fast paths (argument marshalling, stack args); transactional fallback and multi-stack-arg calls route through the generalized lowering path |
| `src/codegen/aarch64/fastpaths/FastPaths_Cast.cpp` | Cast fast paths (narrowing overflow checks) |
| `src/codegen/aarch64/fastpaths/FastPaths_Memory.cpp` | Memory operation fast paths |
| `src/codegen/aarch64/fastpaths/FastPaths_Return.cpp` | Return fast paths |
| `src/codegen/aarch64/fastpaths/FastPathsInternal.hpp` | Shared internal helpers for all fast-path files |

### Register allocation and liveness

| File | Purpose |
|------|---------|
| `src/codegen/aarch64/ra/Allocator.hpp`/`.cpp` | Linear-scan register allocator with protected-use eviction |
| `src/codegen/aarch64/ra/Liveness.hpp`/`.cpp` | Live variable analysis |
| `src/codegen/aarch64/ra/RegPools.hpp`/`.cpp` | Physical register pools (GPR/FPR) |
| `src/codegen/aarch64/ra/OperandRoles.hpp`/`.cpp` | Per-operand use/def role classification |
| `src/codegen/aarch64/ra/OpcodeClassify.hpp` | Opcode classification (call, terminator, mem load/store) |
| `src/codegen/aarch64/ra/InstrBuilders.hpp` | MIR instruction builder helpers for spill/reload |
| `src/codegen/aarch64/ra/RegClassify.hpp` | Register class classification |
| `src/codegen/aarch64/ra/VState.hpp` | Virtual register state tracking |
| `src/codegen/aarch64/Coalescer.hpp`/`.cpp` | Pre-RA register coalescer (~270 LOC) |
| `src/codegen/aarch64/LivenessAnalysis.hpp`/`.cpp` | CFG-level liveness analysis |

### Frame layout

| File | Purpose |
|------|---------|
| `src/codegen/aarch64/FramePlan.hpp` | `FramePlan` struct: callee-saved GPR list, frame size |
| `src/codegen/aarch64/FrameBuilder.hpp`/`.cpp` | `FrameBuilder`: stack slot allocation, aligned slot assignment |

### Peephole optimization

| File | Purpose |
|------|---------|
| `src/codegen/aarch64/Peephole.hpp`/`.cpp` | Top-level peephole dispatcher |
| `src/codegen/aarch64/peephole/BranchOpt.hpp`/`.cpp` | CBZ/CBNZ fusion, branch inversion |
| `src/codegen/aarch64/peephole/CopyPropDCE.hpp`/`.cpp` | Copy propagation + dead code elimination |
| `src/codegen/aarch64/peephole/IdentityElim.hpp`/`.cpp` | Identity operation removal |
| `src/codegen/aarch64/peephole/LoopOpt.hpp`/`.cpp` | Loop-specific optimizations |
| `src/codegen/aarch64/peephole/MemoryOpt.hpp`/`.cpp` | LDP/STP merging |
| `src/codegen/aarch64/peephole/StrengthReduce.hpp`/`.cpp` | MADD fusion, immediate folding, signed/unsigned division-by-constant strength reduction |

### CLI driver integration

| File | Purpose |
|------|---------|
| `src/tools/zanna/cmd_codegen_arm64.hpp`/`.cpp` | `cmd_codegen_arm64(argc, argv)` — CLI entry for `zanna codegen arm64` |

Usage: `zanna codegen arm64 <input.il> -S <out.s>`

### Common library (`src/codegen/common/`)

| File | Purpose |
|------|---------|
| `src/codegen/common/Diagnostics.hpp`/`.cpp` | Diagnostic sink |
| `src/codegen/common/LabelUtil.hpp` | Assembler-safe label sanitization (hyphens → underscores) |
| `src/codegen/common/LinkerSupport.hpp`/`.cpp` | Shared linker invocation, runtime archive selection |
| `src/codegen/common/PassManager.hpp` | Generic `Pass<M>` / `PassManager<M>` templates |
| `src/codegen/common/RuntimeComponents.hpp` | Runtime component classification for selective linking |
| `src/codegen/common/TargetInfoBase.hpp` | Shared base for `TargetInfo` structs |

Note: `ArgNormalize.hpp`, `MachineIRBuilder.hpp`, and `MachineIRFormat.hpp` were removed as dead code during the
codegen review.

### Build integration

- `src/codegen/aarch64/CMakeLists.txt` builds `il_codegen_aarch64` (target, emitter, lowering, RA, peephole).
- `src/CMakeLists.txt` exposes `zanna_cmd_arm64` as a static lib; `zanna` links `zanna_cmd_arm64` and
  `il_codegen_aarch64`.
- `zanna_native_compiler` links `il_codegen_aarch64` directly so `zanna build` can call the AArch64 pipeline without
  routing through the CLI adapter.

## Backend Pipeline

The AArch64 backend uses `CodegenPipeline` to orchestrate passes. The pipeline stages are:

1. **IL input** — load and verify textual IL for `zanna codegen`, or accept an already-verified in-memory module from
   `zanna build`
2. **Rodata pool construction** (`RodataPool`) — scan globals, deduplicate string literals
3. **IL to MIR lowering** (`LowerILToMIR::lowerFunction`) — instruction selection via `InstrLowering` + `TerminatorLowering` + fast paths; functions are lowered in parallel and written back in source order
4. **Register coalescing** (`Coalescer`) — pre-RA copy elimination
5. **Register allocation** (`ra/Allocator`) — linear scan, spill/reload insertion; functions are allocated in parallel
6. **Frame finalization** (`FrameBuilder`) — stack slot layout, frame size computation
7. **Block layout** — reorder hot/fallthrough blocks before branch cleanup
8. **Peephole optimization** (`Peephole` + sub-passes) — post-RA pattern rewrites, CFG-aware DCE, branch cleanup,
   phi spill/reload cleanup, and MIR validation
9. **Post-RA scheduling** — instruction reordering for pipeline utilization; memory scheduling remains conservative
   for base-register heap/object accesses and only separates explicit FP/SP stack slots
10. **Final peephole cleanup** — removes branches/fallthroughs and dead moves exposed by scheduling
11. **Assembly emission** (`AsmEmitter::emitFunction`) — MIR → text assembly
12. **Binary encoding** (`A64BinaryEncoder`) — direct object code emission (optional); per-function text sections are
    handed to the object writer without building a duplicate aggregate text buffer
13. **Rodata emission** — string/FP constant pool to `.section __TEXT,__const` (macOS) or `.section .rodata` (Linux)
14. **Assembly + linking** (`LinkerSupport`) — invoke assembler/linker, link with only the runtime archives and support libraries required by the module; the selected target platform now also chooses the object format, linker platform, and system-assembler triple

Set `ZANNA_CODEGEN_STATS=1` to emit non-fatal diagnostics with peephole transformation counts and MIR
function/block/instruction, call, branch, load, and store counters.

Native assembler debug line tables are disabled by default for faster object generation and smaller native-link
executables. Use `--debug-lines` on `zanna codegen arm64` when DWARF `.debug_line` output and linked debug sections
are needed.

Before MIR lowering, `CodegenPipeline` now runs a selective IL optimization stage:

- EH-sensitive modules run an `eh-opt`-only safety pipeline instead of bypassing IL optimization entirely.
- Very large modules run a reduced CFG/SCCP/constfold/DCE pipeline instead of skipping IL optimization outright.

## Calling Convention (AAPCS64 / Darwin)

- **Integer arguments**: X0–X7 (independent sequence from FP args)
- **Floating-point arguments**: V0–V7 (D-register aliases; independent from integer arg sequence)
- **Integer return**: X0
- **Float return**: V0 (D-register)
- **Stack alignment**: 16 bytes at all call sites
- **Callee-saved GPRs**: X19–X28, X29 (FP), X30 (LR)
- **Callee-saved FPRs**: Lower 64 bits of V8–V15 (D8–D15); upper bits are caller-saved
- **Caller-saved GPRs**: X0–X15
- **Caller-saved FPRs**: V0–V7, V16–V31

For variadic callees, both runtime-declared and module-defined `...` signatures use the declared prefix as the named
argument set and force the anonymous tail onto the stack per AAPCS64.

## Debug Dump Flags

### IL-Level Dumps

Before code reaches the AArch64 backend, it passes through the IL pipeline. The following shared flags inspect those earlier stages (see `docs/tools/debugging.md` §8 for details):

| Flag | Stage |
|------|-------|
| `--dump-tokens` | Lexer token stream |
| `--dump-ast` | AST after parsing |
| `--dump-il` | IL after lowering (pre-optimization) |
| `--dump-il-passes` | IL before/after each optimization pass |
| `--dump-il-opt` | IL after full optimization pipeline |

### MIR Dump Flags

The AArch64 CLI supports flags to dump Machine IR for debugging:

| Flag | Description |
|------|-------------|
| `--dump-mir-before-ra` | Print MIR to stderr before register allocation |
| `--dump-mir-after-ra` | Print MIR to stderr after register allocation |
| `--dump-mir-full` | Print MIR both before and after register allocation |

The complete inspection pipeline from source to native is:

```text
--dump-tokens → --dump-ast → --dump-il → --dump-il-passes → --dump-il-opt → --dump-mir-* → assembly
```

**Example:**

```bash
./build/src/tools/zanna/zanna codegen arm64 /tmp/test.il -S /tmp/test.s --dump-mir-after-ra
```

**Example output:**

```text
=== MIR after RA: test_func ===
MFunction: test_func
  Block: entry
    AddRRR @x0:gpr <- @x0:gpr, @x1:gpr
    MulRRR @x0:gpr <- @x0:gpr, @x2:gpr
    Ret
```

Virtual registers (`%v0:gpr`) appear before RA; physical registers (`@x0:gpr`) appear after RA.

## Tests

96 AArch64-specific test files live in `src/tests/unit/codegen/`. Selected coverage:

| Test file | Coverage |
|-----------|---------|
| `test_target_aarch64.cpp` | Register name / ABI sanity |
| `test_emit_aarch64_minimal.cpp` | Emitter sequencing (header, prologue, add, epilogue) |
| `test_emit_aarch64_mir_minimal.cpp` | MIR → asm: prologue, add, epilogue |
| `test_emit_aarch64_mir_bitwise.cpp` | MIR AND/ORR/EOR emission |
| `test_emit_aarch64_mir_branches.cpp` | MIR conditional branch emission |
| `test_emit_aarch64_frameplan.cpp` | FramePlan odd/even callee-save pairs |
| `test_emit_aarch64_mir_frameplan.cpp` | MIR `savedGPRs` prologue/epilogue |
| `test_codegen_arm64_cli.cpp` | `ret 0` → `mov x0, #0` |
| `test_codegen_arm64_add_params.cpp` | Add two params → `add x0, x0, x1` |
| `test_codegen_arm64_add_imm.cpp` | Add/sub immediate forms |
| `test_codegen_arm64_bitwise.cpp` | AND/OR/XOR register-register and logical-immediate lowering |
| `test_codegen_arm64_call.cpp` | Simple extern call → `bl h` |
| `test_codegen_arm64_call_args.cpp` | Argument marshalling with permutations and cycles |
| `test_codegen_arm64_call_temp_args.cpp` | Non-param temporaries as call args |
| `test_codegen_arm64_callee_saved.cpp` | Callee-saved register preservation across calls |
| `test_codegen_arm64_callee_stack_params.cpp` | Reading stack-passed parameters |
| `test_codegen_arm64_casts.cpp` | Integer cast operations |
| `test_codegen_arm64_cbr.cpp` | Conditional branch patterns |
| `test_codegen_arm64_cbr_cbnz.cpp` | CBZ/CBNZ fusion peephole |
| `test_codegen_arm64_cf_if_else_phi.cpp` | If/else with phi nodes |
| `test_codegen_arm64_cf_loop_phi.cpp` | Loop with phi nodes |
| `test_codegen_arm64_division.cpp` | Signed/unsigned div/rem with zero-check traps |
| `test_codegen_arm64_exception_handling.cpp` | Exception handling paths |
| `test_codegen_arm64_fp.cpp` | Floating-point arithmetic and plain `fptosi` NaN/range checks |
| `test_codegen_arm64_fp_basic.cpp` | Basic FP operations (add/sub/mul/div) |
| `test_codegen_arm64_fp_cmp_all.cpp` | All FP comparison predicates including NaN |
| `test_codegen_arm64_gep_load_store.cpp` | GEP, load, store lowering |
| `test_codegen_arm64_icmp.cpp` | Integer compare predicates |
| `test_codegen_arm64_icmp_imm.cpp` | Integer compare with immediates |
| `test_codegen_arm64_indirect_call.cpp` | Indirect call via register (`blr`) |
| `test_codegen_arm64_large_imm.cpp` | Large immediate materialization (`movz/movk`) |
| `test_codegen_arm64_ovf.cpp` | Overflow-checked arithmetic, including annotated sub-width checks |
| `test_codegen_arm64_params_wide.cpp` | Params beyond x1 (x2/x3 normalization) |
| `test_codegen_arm64_peephole.cpp` | Peephole optimization correctness |
| `test_codegen_arm64_peephole_comprehensive.cpp` | All five peephole patterns |
| `test_codegen_arm64_ra_many_temps.cpp` | Register allocation under high pressure |
| `test_codegen_arm64_ret_param.cpp` | `ret %param0` no-op; `ret %param1` `mov x0, x1` |
| `test_codegen_arm64_rodata_literals.cpp` | String literal pool deduplication |
| `test_codegen_arm64_run_native.cpp` | Full assemble + link + execute on Apple Silicon |
| `test_codegen_arm64_select.cpp` | Select (conditional value) lowering |
| `test_codegen_arm64_shift_imm.cpp` | `lsl`/`lsr`/`asr` by immediate |
| `test_codegen_arm64_shift_reg.cpp` | Shift by register (`lslv`/`lsrv`/`asrv`) |
| `test_codegen_arm64_spill_fpr.cpp` | FPR spill/reload under register pressure |
| `test_codegen_arm64_stack_locals.cpp` | Stack local allocation and access |
| `test_codegen_arm64_switch.cpp` | Switch statement lowering |
| `test_aarch64_vararg.cpp` | User-defined variadic calls and malformed direct-call diagnostics |
| `test_aarch64_pac_bti.cpp` | Darwin-only BTI/PAC emission policy in text and binary output |
| `test_codegen_arm64_unsigned_cmp.cpp` | Unsigned comparison predicates |

All test artifacts are written under `build/test-out/arm64/` (created on demand). CMake wiring is in
`src/tests/CMakeLists.txt`.

## Quick Start Testing

### Verify Backend Works

```bash
cat > /tmp/test.il << 'EOF'
il 0.3.0
func @main() -> i64 {
entry:
  ret 15
}
EOF

./build/src/tools/zanna/zanna codegen arm64 /tmp/test.il -S /tmp/test.s
as /tmp/test.s -o /tmp/test.o
clang++ /tmp/test.o -o /tmp/test_native
/tmp/test_native
echo "Exit code: $?"  # Should print 15
```

## Critical Bugs Fixed (Historical)

### BUG 1: Incorrect Section Directive for macOS (FIXED)

- **Was**: Always emitted `.section .rodata` (ELF/Linux syntax)
- **Fix**: `AsmEmitter` now emits `.section __TEXT,__const` on macOS (detected via `#ifdef __APPLE__`)

### BUG 2: Invalid Label Generation with Negative Numbers (FIXED)

- **Was**: Labels like `L-1000000000_POSITION.INIT` generated from uninitialized IDs
- **Fix**: Label sanitization in `LabelUtil.hpp` replaces hyphens with underscores; all IDs are initialized before use

### BUG 3: Missing Rodata Pool (FIXED)

- **Was**: No deduplication of string literals, causing duplicate symbol definitions
- **Fix**: `RodataPool` class in `src/codegen/aarch64/RodataPool.hpp`/`.cpp` deduplicates via hash tracking

### BUG 4: V8–V15 in callerSavedFPR (FIXED)

- **Was**: V8–V15 listed in both caller-saved and callee-saved FPR sets; register allocator saved and also treated as clobbered across calls
- **Fix**: Removed V8–V15 from `callerSavedFPR` in `TargetAArch64.cpp`

### BUG 5: Shift commutativity in fast paths (FIXED)

- **Was**: `FastPaths_Arithmetic.cpp` and `FastPaths_Call.cpp::computeTempTo` treated shifts as commutative, swapping operands for `shl`/`lshr`/`ashr`
- **Fix**: Shifts excluded from the commutative operand swap path in both files

### BUG 6: X19 fallback without occupancy check (FIXED)

- **Was**: `RegAllocLinear::takeGPR()` silently returned X19 when all allocatable registers were occupied, without checking if X19 was already in use
- **Fix**: Replaced silent fallback with `assert` to catch register conflicts

### BUG 7: emplace_back invalidates block references (FIXED)

- **Was**: `InstrLowering.cpp` called `emplace_back` on the blocks vector while holding references to earlier blocks (use-after-reallocation UB in 5 functions: `lowerSRemChk0`, `lowerSDivChk0`, `lowerUDivChk0`, `lowerURemChk0`, `lowerBoundsCheck`)
- **Fix**: Moved trap block creation to after all uses of the `out` reference

### BUG 8: Shared GPR/FPR parameter index (FIXED)

- **Was**: `materializeValueToVReg` used the overall parameter position to index both GPR and FPR arg arrays; for `f(i64, f64, i64)` the second i64 loaded from X2 instead of X1
- **Fix**: Count same-class parameters preceding the target to compute the correct register index

### BUG 9: Hardcoded `_rt_*` symbol mangling (FIXED)

- **Was**: Runtime symbols emitted with hardcoded `_rt_` prefix; incorrect on macOS where C symbols need `__rt_`
- **Fix**: `AsmEmitter.cpp` uses an internal `mangleSymbol()` static function for platform-correct symbol name generation

### BUG 10: SP chunk size 4095 not 16-byte aligned (FIXED)

- **Was**: `emitSubSp`/`emitAddSp` used 4095-byte chunks; AArch64 hardware requires SP 16-byte aligned at all times
- **Fix**: Changed chunk size to 4080 (largest multiple of 16 fitting in 12-bit immediate)

### BUG 11: `emitFMovRI` ostream format flags not restored (FIXED)

- **Was**: `std::fixed` applied to ostream but never restored, corrupting all subsequent FP emission
- **Fix**: Save and restore `std::ostream` format flags around FP immediate materialization

### BUG 12: `switch.i32` edge values were copied on untaken cases (FIXED)

- **Was**: case-edge argument copies ran before each `b.eq`, so untaken edges could overwrite the values seen by the taken successor
- **Fix**: `TerminatorLowering` now lowers `switch.i32` through dedicated edge blocks and phi spill-slot stores, matching the `br` / `cbr` path

### BUG 13: `trap.from_err` did not reliably marshal its argument into `x0` (FIXED)

- **Was**: nontrivial `trap.from_err` paths could reach `rt_trap_raise_error` without a guaranteed ABI move into `x0`
- **Fix**: lowering now routes `trap.from_err` through the normal argument-marshalling helper path, and regression tests cover both constant and non-entry block-parameter inputs

### BUG 14: checked trap helpers used the generic `rt_trap` ABI (FIXED)

- **Was**: overflow, divide-by-zero, and bare trap paths relied on `rt_trap` in places where the runtime expects dedicated no-arg helpers or an explicit `x0 = NULL`
- **Fix**: overflow and checked div/rem now lower to `rt_trap_ovf` / `rt_trap_div0`, while plain trap and `idx.chk` set `x0` to null before calling `rt_trap`

### BUG 15: target selection stopped at instruction selection (FIXED)

- **Was**: native object writing, system assembly, and native linking still followed the host platform even when `--target-linux` or `--target-windows` was selected
- **Fix**: `CodegenPipeline` now threads the selected target platform through object writer selection, native linker selection, and system-assembler target arguments

### BUG 16: text and binary emitters diverged on BTI/PAC hardening (FIXED)

- **Was**: the binary encoder emitted `bti c`, `paciasp`, and `autiasp`, but the text emitter omitted them
- **Fix**: `AsmEmitter` now emits the same hardening sequence, and dedicated tests keep the two paths aligned

### BUG 17: non-Darwin objects inherited Mach-O compact-unwind metadata (FIXED)

- **Was**: `A64BinaryEncoder` recorded compact-unwind entries even for ELF and COFF output modes
- **Fix**: unwind entry emission is now gated by `ABIFormat::Darwin`

### BUG 18: unsigned division by arbitrary constants was not strength-reduced (FIXED)

- **Was**: only a subset of signed constant-division rewrites existed, and unsigned non-power-of-two division still fell back to `udiv`
- **Fix**: the peephole pass now emits magic-multiply sequences using `UmulhRRR`, with focused unit coverage for signed and unsigned cases

### BUG 19: logical-immediate ORR/EOR text emission fell through (FIXED)

- **Was**: `OrrRI` and `EorRI` could appear as unknown text-emitter opcodes.
- **Fix**: `AsmEmitter` now emits `orr/eor xD, xN, #imm`, and unknown opcodes throw instead of producing commented assembly.

### BUG 20: checked casts collapsed invalid and overflow trap codes (FIXED)

- **Was**: checked FP casts used one runtime error code for all failure modes.
- **Fix**: NaN/invalid unsigned inputs raise `InvalidCast`; finite out-of-range values raise `Overflow`.

### BUG 21: fast paths bypassed width-sensitive checked arithmetic (FIXED)

- **Was**: simple arithmetic fast paths could intercept annotated sub-width checked arithmetic before generic width checks.
- **Fix**: sub-width checked arithmetic falls back to generic lowering; chained checked arithmetic no longer maps to unchecked fast-path ops.

### BUG 22: AArch64 FP compare conditions mishandled unordered inputs (FIXED)

- **Was**: raw `fcmp` condition aliases made `fcmp_eq` / `fcmp_le` true for NaN and `fcmp_ne` false for NaN.
- **Fix**: lowering combines `eq`/`ls` with `vc` for ordered predicates and ORs `ne` with `vs` for unordered not-equal.

### BUG 23: base-register memory displacements were dropped (FIXED)

- **Was**: non-frame `load` / `store` forms always emitted `[base, #0]`, ignoring optional IL displacement operands.
- **Fix**: the displacement is preserved for `Ldr*BaseImm` and `Str*BaseImm`, including when frame-local GEPs are resolved.

### BUG 24: phi-edge copies silently converted register classes (FIXED)

- **Was**: a GPR value flowing to an FPR phi, or vice versa, inserted `SCvtF` / `FCvtZS` during edge-copy lowering.
- **Fix**: the terminator lowering path now rejects class mismatches so type errors are caught instead of changing values.

### BUG 25: FP constants used the wrong MIR bit-cast opcode (FIXED)

- **Was**: constant materialization could emit `FMovRR` with a GPR source; text assembly printed an invalid FPR-to-FPR form while the binary encoder silently corrected it.
- **Fix**: lowering emits `FMovGR`, and the binary encoder rejects `FMovRR` with a GPR source.

## Peephole Optimizations Implemented

| Pattern | Description |
|---------|-------------|
| CBZ/CBNZ fusion | `cmp xN, #0; b.eq label` → `cbz xN, label` (and `b.ne` → `cbnz`). Also handles `tst xN, xN` pattern. |
| MADD fusion | `mul tmp, a, b; add dst, tmp, c` → `madd dst, a, b, c` when mul destination is dead after add. |
| LDP/STP merging | Adjacent `ldr`/`str` with consecutive FP offsets (diff 8) → `ldp`/`stp` pairs. Supports GPR and FPR. |
| Branch inversion | `b.cond .Lnext; b .Lother` (fall-through) → `b.!cond .Lother`. Supports all AArch64 condition codes. |
| Immediate folding | `AddRRR`/`SubRRR` → `AddRI`/`SubRI` when one operand is a known constant in 12-bit range (0–4095). |

New MIR opcodes added to support these patterns: `Cbnz`, `MAddRRRR`, `Csel`, `LdpRegFpImm`, `StpRegFpImm`,
`LdpFprFpImm`, `StpFprFpImm`. (`Cbz` was already present in the original opcode set.)

## Known Limitations

### Unimplemented Peephole Patterns

- **Address mode folding**: Base+offset computations could fold into addressing modes
- **Additional logical-immediate peepholes**: direct lowering handles logical immediates; post-RA folding from materialized constants into bitmask immediates is not implemented
- **Conditional select (CSEL) pattern matching**: `Csel` opcode exists; pattern matching not yet wired
- **Compare elimination**: Comparisons whose flags are set by a preceding arithmetic instruction
- **Shift-add fusion**: Shift followed by add → shifted-register form of ADD
- **Zero-extension elimination**: Redundant explicit zero-extensions after 32-bit operations

### Architectural Gaps

- **AArch64 PassManager**: `CodegenPipeline` now uses `PassManager`-based composition with Scheduler and BlockLayout passes at O1+; per-pass verification hooks remain limited
- **Darwin symbol fixup**: Uses string search-and-replace on full assembly output (fragile); needs redesign
- **Instruction scheduling**: Post-RA scheduler implemented; further scheduling opportunities remain
- **MIR verification**: No inter-pass MIR invariant checker
- **MIR-level constant folding**: Folding is done in the IL optimizer; the MIR layer has no
  independent constant folder

## References

- AArch64 Procedure Call Standard (AAPCS64)
- Apple/Darwin ABI conventions for arm64 (callee-saved V8–V15 lower 64 bits, etc.)
