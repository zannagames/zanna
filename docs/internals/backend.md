---
status: active
audience: contributors
last-verified: 2026-07-26
---

# Zanna Backend — Native Code Generation

Comprehensive guide to the Zanna native backends (x86-64 and AArch64), which compile Zanna IL programs to executable
machine code. This document covers the backend's design philosophy, compilation pipeline, code generation strategies,
and source code organization.

> Status
>
> - AArch64: Validated end-to-end on Apple Silicon across all demo games. Register coalescer, post-RA scheduler, peephole optimizer.
> - x86_64: Implemented with System V (Linux) and Windows x64 ABI support. Validated on Windows with all codegen tests passing. macOS x86-64 is not a supported target — macOS support is Apple Silicon/ARM64 only.

---

## Table of Contents

1. [Overview](#overview)
2. [Architecture & Design Philosophy](#architecture--design-philosophy)
3. [Compilation Pipeline](#compilation-pipeline)
4. [Machine IR (MIR)](#machine-ir-mir)
5. [IL Lowering](#il-lowering)
6. [Instruction Selection](#instruction-selection)
7. [Register Allocation](#register-allocation)
8. [Frame Lowering](#frame-lowering)
9. [Assembly Emission](#assembly-emission)
10. [Calling Convention](#calling-convention)
11. [AArch64 Backend](#aarch64-backend)
12. [Source Code Guide](#source-code-guide)

---

## Overview

### What is the Zanna Backend?

The Zanna backend is a **native code generator** that translates Zanna IL (Intermediate Language) programs into
executable machine code for x86-64 and AArch64. It implements the final compilation stage in the Zanna toolchain:

```text
Source → Frontend → IL → Backend → Assembly → Executable
```

### Key Characteristics

| Feature           | Description                                              |
|-------------------|----------------------------------------------------------|
| **Target**        | x86-64 (AMD64) and AArch64 (ARM64) architectures         |
| **ABI**           | System V AMD64 (Linux) and Windows x64                   |
| **Output**        | Text assembly, native relocatable objects, and executables |
| **Strategy**      | SSA-based with linear scan register allocation           |
| **Pipeline**      | Multi-pass: Lowering → Selection → Allocation → Emission |
| **Validation**    | x86_64 validated on Windows; AArch64 validated on Apple Silicon |

Native builds from `zanna build` keep frontend/project IL optimization and backend optimization separate. The driver
hands the verified, already-optimized IL module directly to the backend, tells the backend to skip its own IL
optimization pass, and still forwards the selected `O0`/`O1`/`O2` level to MIR/codegen passes such as pre-regalloc
cleanup, block layout, scheduling, and peephole optimization.

### Current Implementation Priorities

The current implementation prioritizes:

1. **Correctness**: Deterministic, verifiable code generation
2. **Clarity**: Educational implementation demonstrating compiler techniques
3. **Completeness**: Full IL opcode coverage for basic programs
4. **Simplicity**: Clean architecture for target-specific maintenance

---

## Architecture & Design Philosophy

### Core Principles

The backend design emphasizes:

1. **Modularity**: Clean separation between lowering, allocation, and emission
2. **SSA Preservation**: Virtual registers map directly to IL SSA values
3. **Multi-Pass**: Each pass has a single, well-defined responsibility
4. **Testability**: Intermediate representations are inspectable and verifiable
5. **Determinism**: Identical inputs produce identical outputs

### High-Level Architecture

```text
┌─────────────────────────────────────────────────────────┐
│                    Backend Pipeline                      │
├─────────────────────────────────────────────────────────┤
│                                                          │
│  IL Module                                              │
│      ↓                                                  │
│  ┌──────────────┐                                       │
│  │  IL Lowering │ → Machine IR (Virtual Regs)          │
│  └──────────────┘                                       │
│      ↓                                                  │
│  ┌──────────────┐                                       │
│  │ Instruction  │ → Legalized Machine IR                │
│  │  Selection   │                                       │
│  └──────────────┘                                       │
│      ↓                                                  │
│  ┌──────────────┐                                       │
│  │   Register   │ → Physical Register Assignment        │
│  │  Allocation  │                                       │
│  └──────────────┘                                       │
│      ↓                                                  │
│  ┌──────────────┐                                       │
│  │    Frame     │ → Stack Frame Layout                  │
│  │   Lowering   │                                       │
│  └──────────────┘                                       │
│      ↓                                                  │
│  ┌──────────────┐                                       │
│  │   Assembly   │ → AT&T Syntax Output                  │
│  │   Emission   │                                       │
│  └──────────────┘                                       │
│      ↓                                                  │
│  Assembly Text                                          │
│                                                          │
└─────────────────────────────────────────────────────────┘
```

---

## Compilation Pipeline

### Pipeline Stages

The backend implements a **sequential multi-pass pipeline**:

```cpp
// High-level pipeline flow (x86-64)
ILModule → LoweringPass → LegalizePass → PreRegAllocOptPass → RegAllocPass → SchedulerPass → PeepholePass → EmitPass → Assembly

// AArch64
ILModule → LoweringPass → LegalizePass → PreRegAllocOptPass → RegAllocPass → BlockLayoutPass → PeepholePass → SchedulerPass → PeepholePass(post-schedule) → ExpandPseudosPass → EmitPass / BinaryEmitPass
```

Each pass operates on a shared `Module` structure that threads state through the pipeline.
On AArch64, `ExpandPseudosPass` is the last MIR pass at every optimization level: it rewrites
every form the emitters used to expand through a hidden scratch register (wide ALU/compare
immediates, non-FP8 `FMovRI`, frame/base/pair/SP-relative accesses outside the encodable range)
into explicit MIR (`MovRI xS,#imm; op dst,lhs,xS`, `MovRI xS,#off; AddRRR xS,x29,xS; op rt,[xS,#0]`,
pair splits), choosing a reserved scratch (x9/x16/x17) that is neither an operand nor live at that
point. It runs after the peepholes and scheduler so the frame-slot forwarders still match the
compact forms; until it runs, `InstrEffects::effectsOf` reports the scratch set as implicit
definitions of every such pseudo form. Both emitters reject any pseudo form that reaches them.
`LegalizePass` is a real backend stage on both native backends: x86-64 lowers adapter IL to MIR and expands early
machine pseudos; AArch64 expands overflow pseudos, inserts the `main` runtime-context calls into MIR, and refreshes
leaf metadata before register allocation.

At O1 and above, both native backends run a conservative `PreRegAllocOptPass` after legalization and before register
allocation. The pass removes identity register copies and forwards single-use virtual-to-virtual register copies within
the same basic block when the source is not clobbered before the use and the destination has no later use after a call
boundary. It deliberately does not forward physical ABI sources such as `x0`/`rax` return registers, because those are
not modeled as durable pre-RA live ranges and may be reused by register allocation before the forwarded use.

### PassManager

The `PassManager` orchestrates pass execution:

```cpp
class PassManager {
    void addPass(std::unique_ptr<Pass> pass);
    bool run(Module& module, Diagnostics& diags);
};
```

**Execution model:**

- Passes run sequentially in registration order
- Failure in any pass short-circuits the pipeline
- Diagnostics accumulate throughout execution
- Each pass reports success/failure via return value
- `ZANNA_CODEGEN_STATS=1` enables non-fatal diagnostics with backend peephole transformation counts and MIR size/memory
- Triage kill switches (bisection aids; never consulted at `-O0`): `ZANNA_NO_PRE_RA_OPT`,
  `ZANNA_NO_BLOCK_LAYOUT`, `ZANNA_NO_PEEPHOLE`, `ZANNA_NO_SCHEDULER`,
  `ZANNA_NO_POST_SCHED_PEEPHOLE` skip one AArch64 pipeline stage each
  (`CodegenPipeline.cpp`), and `ZANNA_NO_PH_{REORDER,LOOPHOIST,PERBLOCK,DCE_CFG,FPSTORES,STORELOAD_FWD,PHI_LOADS,PHI_SPILLS,BRANCH}`
  skip one sub-stage of the full peephole (`Peephole.cpp`). Together with the older
  `ZANNA_NO_ADDR_FOLDS`, `ZANNA_NO_GLOBAL_RA`, `ZANNA_NO_JUMP_TABLES`, `ZANNA_NO_IF_CONVERT`,
  `ZANNA_NO_ABI_COPYFWD`, `ZANNA_NO_LOAD_FUSE`, `ZANNA_NO_RETAIN_ELIDE` they let a miscompile
  be bisected against a program-level oracle (VM vs native output) without rebuilding the
  compiler. Set the variable to any value, e.g. `ZANNA_NO_PEEPHOLE=1 zanna build …`.
- **MIR verifier** (`ZANNA_VERIFY_MIR=1`, or `--verify-mir` on `zanna codegen arm64|x64`): the
  pass manager's post-pass hook runs `verifyMir` (`src/codegen/aarch64/MirVerify.hpp`,
  `src/codegen/x86_64/MirVerify.hpp`) on every function after every backend pass. Rules are
  cumulative by pipeline stage: structural rules everywhere (branch labels resolve, no
  instruction after a terminator, the last block ends in one, one register class per virtual
  register, well-formed carried-exit metadata); after register allocation no virtual registers,
  frame- and stack-relative offsets inside the finalized frame, callee-saved writes covered by the
  save list, reserved scratch (x9/x16/x17/v16/v17, R10/R11) never live across an instruction that
  clobbers it implicitly, and an entry live-in set restricted to ABI inputs; after pseudo
  expansion (AArch64) every immediate directly encodable. Violations are `V-CG-MIR-*` error
  diagnostics that stop the pipeline. The register facts come from the shared effects model
  (`InstrEffects.hpp` on AArch64, `OperandRoles.hpp::effectsOf` on x86-64) that every post-RA
  pass consumes, so the verifier and the passes cannot disagree. Unit tests that drive a pipeline
  (`test_aarch64_mir_verify`, `test_x86_mir_verify`, the AArch64 shared-corpus and VM-vs-native
  property tests) run it unconditionally.
- **One CFG per backend** (`src/codegen/aarch64/MirCfg.hpp`, `src/codegen/x86_64/MirCfg.hpp`):
  `MirCfg` is a snapshot of a function's edges built from the allocator's branch classifier
  (`ra::classifyControlFlow`) through the shared extractor (`common/ra/CfgExtract.hpp`), with
  predecessors, per-block fallthrough flags, lazily computed dominators
  (`common/ra/Dominators.hpp`), dominance-proven back edges, natural loops, and loop depths. The
  register allocator's liveness, the verifier, the AArch64 CFG-aware DCE, the phi-join
  forwarding/coalescing passes, the loop passes, and the x86-64 layout passes all read it; no pass
  keeps a private terminator scan, so a mid-block branch, a no-return call, a jump table, or a
  trailing conditional branch means the same thing everywhere. On AArch64 `blockExitLive(fn, bi,
  target)` is the exit-liveness seed for post-RA block-local rewrites: the allocator's
  `carriedExitRegs` plus SP/FP/LR, plus the return registers of a block that leaves the function
  or, for every other exit, the callee-saved registers (which hold pinned frame slots and values
  carried across blocks; at a return the epilogue restores them). `foldComputeIntoTarget` and
  `tryMaddFusion` consult it (and the effects model, so a call's argument registers and a
  return's result registers count as reads) before declaring a register dead at the block end.
- `ZANNA_IL_OPT_KEEP_FUNCS=<file>` (IL optimizer, `PassManager::runPipeline`): the file lists
  one IL function name per line; every function *not* listed is restored to its pre-pipeline
  body after the named pipeline runs (functions, externs, and globals the pipeline removed
  are re-added). With a program-level oracle this bisects "which optimized function changes
  the output" in log₂(N) builds — plan 80 found ZB-30/ZB-31 this way. Keep-none must
  reproduce the unoptimized result and keep-all the optimized one before trusting a run.
- Native executables carry a full local symbol table by default (Mach-O `LC_SYMTAB` `N_SECT`
  entries, ELF `.symtab`/`.strtab`), so `sample`, Instruments, and `perf` attribute samples to
  Zia and runtime functions by name; `zanna build --strip-symbols` writes only the entry
  point and imports. The loader never reads these entries, so stripping changes nothing at
  run time.
  mix counters
- AArch64 post-RA join coalescing handles acyclic joins and true loop headers only when the natural loop is call-free.
  Loop headers with call-containing bodies stay on stack-backed phi slots until the pass has full liveness proof for
  every rewritten physical register through complex application update loops. The loop-phi spill pass uses the same
  call-free natural-loop restriction.

### Module State

Mutable state threaded through passes:

```cpp
struct Module {
    il::core::Module il;                    // Original IL module
    std::optional<ILModule> lowered;        // Adapter module (MIR)
    bool legalised;                         // Post-selection flag
    // PreRegAllocOptPass runs here at O1+ while MIR still uses virtual registers.
    bool registersAllocated;                // Post-allocation flag
    std::optional<CodegenResult> codegenResult; // Final assembly
};
```

### Diagnostic System

```cpp
class Diagnostics {
    void error(std::string message);        // Record fatal error
    void warning(std::string message);      // Record non-fatal warning
    bool hasErrors() const;
    void flush(std::ostream& err) const;
};
```

---

## Machine IR (MIR)

### Design Philosophy

Machine IR (MIR) is the **backend's internal representation**, positioned between high-level IL and final assembly:

- **IL**: High-level, platform-independent SSA
- **MIR**: Low-level, x86-64-specific, virtual registers
- **Assembly**: Final textual output

MIR provides:

- **SSA form**: Virtual registers assigned once
- **Target-specific opcodes**: x86-64 instruction semantics
- **Flexible operands**: Registers, immediates, memory, labels
- **Allocation-ready**: Virtual regs map to physical regs

### Virtual Registers

```cpp
struct VReg {
    uint16_t id;             // Unique within function
    RegClass cls;            // GPR or XMM
};
```

Virtual registers are:

- Numbered sequentially starting from 1
- Classified as GPR (general-purpose) or XMM (floating-point)
- Mapped to IL SSA values during lowering
- Allocated to physical registers later

### Operand Types

MIR supports five operand kinds:

```cpp
// Register operand
struct OpReg {
    bool isPhys;         // Virtual or physical?
    RegClass cls;        // GPR or XMM
    uint16_t idOrPhys;   // VReg ID or PhysReg enum
};

// Immediate operand
struct OpImm {
    int64_t val;
};

// Memory operand
struct OpMem {
    OpReg base;          // Base register
    OpReg index;         // Optional index register
    uint8_t scale;       // 1, 2, 4, or 8
    int32_t disp;        // Displacement
    bool hasIndex;
};

// Label operand
struct OpLabel {
    std::string name;
};

// RIP-relative label
struct OpRipLabel {
    std::string name;
};

using Operand = std::variant<OpReg, OpImm, OpMem, OpLabel, OpRipLabel>;
```

### Machine Instructions

```cpp
struct MInstr {
    MOpcode opcode;                  // Instruction opcode
    std::vector<Operand> operands;   // Operands in emission order
};
```

**Supported opcodes:**

- **Moves**: `MOVrr`, `MOVri`, `LEA`, `CMOVNErr`
- **Arithmetic**: `ADDrr/ri`, `SUBrr`, `IMULrr`, `DIVS64rr`, `REMS64rr`
- **Bitwise**: `ANDrr/ri`, `ORrr/ri`, `XORrr/ri`, `SHLri/rc`, `SHRri/rc`, `SARri/rc`
- **Comparison**: `CMPrr/ri`, `TESTrr`, `SETcc`, `UCOMIS`
- **Control**: `JMP`, `JCC`, `CALL`, `RET`, `LABEL`
- **Floating-point**: `FADD`, `FSUB`, `FMUL`, `FDIV`, `CVTSI2SD`, `CVTTSD2SI`
- **Special**: `PX_COPY` (parallel copy pseudo), `UD2` (trap)

### Basic Blocks

```cpp
struct MBasicBlock {
    std::string label;              // Block label
    std::vector<MInstr> instructions;
};
```

Blocks are:

- Labeled for control flow
- Contain ordered instruction sequences
- Terminated implicitly (no explicit terminator in MIR)

### Functions

```cpp
struct MFunction {
    std::string name;               // Function symbol
    std::vector<MBasicBlock> blocks;
    FunctionMetadata metadata;      // Vararg flag, etc.
    size_t localLabelCounter;       // Unique label generation
};
```

---

## IL Lowering

### LowerILToMIR Class

The `LowerILToMIR` adapter converts IL to Machine IR:

```cpp
class LowerILToMIR {
    MFunction lower(const ILFunction& func);
    const std::vector<CallLoweringPlan>& callPlans() const;
};
```

**Key responsibilities:**

1. Map IL SSA values to MIR virtual registers
2. Translate IL opcodes to MIR instruction sequences
3. Materialize block parameters as `PX_COPY` pseudo-instructions
4. Record call lowering plans for later processing

### Lowering Rules

IL instructions are lowered via **pattern-matching rules**:

```cpp
struct LoweringRule {
    bool (*match)(const ILInstr&);              // Match predicate
    void (*emit)(const ILInstr&, MIRBuilder&);  // Code emitter
    const char* name;                           // Debug name
};
```

**Rule selection:**

```cpp
const LoweringRule* rule = zanna_select_rule(ilInstr);
if (rule) {
    rule->emit(ilInstr, builder);
}
```

**Categories:**

- **Arithmetic** (`Lowering.Arith.cpp`): `iadd.ovf`, `isub.ovf`, `imul.ovf`, `sdiv.chk0`, `udiv.chk0`, `srem.chk0`,
  `urem.chk0`, `fadd`, `fsub`, `fmul`, `fdiv`
- **Bitwise** (`Lowering.Bitwise.cpp`): `and`, `or`, `xor`, `shl`, `shr`
- **Control Flow** (`Lowering.CF.cpp`): `br`, `cbr`, `ret`, `switch`
- **Memory** (`Lowering.Mem.cpp`): `load`, `store`, `alloca`, `gep`
- **Exception Handling** (`Lowering.EH.cpp`): runtime trap/error bridges plus
  handler markers; `resume.same` and `resume.next` remain unsupported in native codegen

### MIRBuilder

Helper class for emitting MIR during lowering:

```cpp
class MIRBuilder {
    VReg ensureVReg(int id, ILValue::Kind kind);  // IL value → VReg
    VReg makeTempVReg(RegClass cls);              // Allocate temp
    Operand makeOperandForValue(const ILValue&);  // Create operand
    void append(MInstr instr);                    // Emit instruction
    void recordCallPlan(CallLoweringPlan plan);   // Record call
};
```

**Example lowering (IL `add`):**

```cpp
// IL: %3 = add %1, %2
void emitAdd(const ILInstr& instr, MIRBuilder& b) {
    VReg lhs = b.ensureVReg(instr.ops[0].id, ILValue::I64);
    VReg rhs = b.ensureVReg(instr.ops[1].id, ILValue::I64);
    VReg dest = b.ensureVReg(instr.resultId, ILValue::I64);

    // MOV dest, lhs
    b.append(MInstr::make(MOpcode::MOVrr, {
        makeVRegOperand(GPR, dest.id),
        makeVRegOperand(GPR, lhs.id)
    }));

    // ADD dest, rhs
    b.append(MInstr::make(MOpcode::ADDrr, {
        makeVRegOperand(GPR, dest.id),
        makeVRegOperand(GPR, rhs.id)
    }));
}
```

### Block Parameter Lowering

IL block parameters are lowered to **parallel copy** (`PX_COPY`) pseudo-instructions:

```cpp
// IL edge: br label %target(%arg1, %arg2)
// MIR:
PX_COPY %p1, %arg1
PX_COPY %p2, %arg2
JMP target
```

The parallel copy ensures SSA semantics are preserved across control flow edges.

---

## Instruction Selection

### ISel Class

The instruction selector legalizes MIR for x86-64:

```cpp
class ISel {
    void lowerArithmetic(MFunction& func) const;
    void lowerCompareAndBranch(MFunction& func) const;
    void lowerSelect(MFunction& func) const;
};
```

### Legalization Tasks

**1. Immediate Operand Constraints**

x86-64 restricts immediate sizes:

- Memory operands: 32-bit immediates only
- Register operands: 64-bit immediates allowed for `MOVri`

**2. Compare + Branch Fusion**

Fuse compare/test instructions with conditional branches:

```text
CMPrr %a, %b
SETcc %tmp
TESTrr %tmp, %tmp
JCC label
```

→

```text
CMPrr %a, %b
JCC label
```

**3. Boolean Materialization**

IL `i1` values are materialized using `SETcc` + `MOVZXrr8`:

```text
CMPrr %a, %b
SETcc %result8      # Set byte based on condition
MOVZXrr8 %result, %result8   # Zero-extend byte result to 64-bit
```

`MOVZXrr32` is the separate 32-bit-write zero-extension case and encodes as `movl`.

**4. Conditional Moves**

Select-like patterns are lowered to `CMOVcc`:

```text
CMPrr %cond, 0
CMOVNErr %dest, %true_val  # Move if not equal (cond != 0)
```

**5. LEA Folding**

Single-use `LEA` instructions are folded into memory operands:

```text
LEA %tmp, [%base + disp]
MOV %dest, [%tmp]
```

→

```text
MOV %dest, [%base + disp]
```

---

## Register Allocation

### Linear Scan Algorithm

The backend uses **linear scan register allocation**:

```cpp
class LinearScanAllocator {
    AllocationResult run();
};
```

**Algorithm overview:**

1. **Compute live intervals** for each virtual register
2. **Walk instructions** in block order
3. **Expire old intervals** and release physical registers
4. **Allocate registers** or **spill to stack**
5. **Insert spill code** (loads/stores)
6. **Resolve parallel copies** by coalescing or emitting moves

**Performance optimizations:**

- **Active sets** use `std::unordered_set<uint16_t>` for O(1) insert/remove operations
- **Caller-saved register lookup** uses precomputed `std::bitset<32>` for O(1) membership checks
- **Deterministic allocation** via sorted free-register pools

### Register Classes

Two independent register classes:

```cpp
enum class RegClass {
    GPR,  // General-purpose: RAX, RBX, RCX, RDX, RSI, RDI, R8-R15, RBP, RSP
    XMM   // Floating-point: XMM0-XMM15
};
```

**SysV AMD64 ABI (Linux):**

- **Caller-saved GPR**: `RAX`, `RDI`, `RSI`, `RDX`, `RCX`, `R8`-`R11` (9 registers)
- **Callee-saved GPR**: `RBX`, `R12`-`R15`, `RBP` (6 registers, allocated with save/restore)
- **Caller-saved XMM**: `XMM0`-`XMM15` (16 registers)
- **Callee-saved XMM**: none

**Windows x64 ABI:**

- **Caller-saved GPR**: `RAX`, `RCX`, `RDX`, `R8`-`R11` (7 registers)
- **Callee-saved GPR**: `RBX`, `RBP`, `RDI`, `RSI`, `R12`-`R15` (8 registers)
- **Caller-saved XMM**: `XMM0`-`XMM5` (6 registers)
- **Callee-saved XMM**: `XMM6`-`XMM15` (10 registers)

**Reserved registers:**

- `RSP`: Stack pointer (never allocated)

### Live Interval Analysis

```cpp
class LiveIntervals {
    struct Interval {
        size_t start;  // First definition
        size_t end;    // Last use
    };

    std::unordered_map<uint16_t, Interval> intervals_;
};
```

Intervals track the lifetime of each virtual register within a function.

### Spilling

When no free registers are available:

1. **Select victim**: Virtual register with furthest end point
2. **Allocate stack slot**: 8-byte aligned slot in spill area
3. **Insert spill store**: Before victim's definition
4. **Insert reload**: Before each use of victim

**Spill code example:**

```text
# Before allocation
ADDrr %v1, %v2

# After spilling %v1
MOV [rbp - 8], %rax    # Spill
ADDrr %rax, %rdx       # Use RAX instead of %v1
MOV %rax, [rbp - 8]    # Reload (if needed later)
```

### Coalescing

The `Coalescer` attempts to eliminate `PX_COPY` instructions:

```cpp
class Coalescer {
    bool tryCoalesce(MInstr& copy,
                     std::unordered_map<uint16_t, VirtualAllocation>& states);
};
```

**Coalescing conditions:**

- Source and destination are both virtual registers
- Destination has not been allocated yet
- No interference in live ranges

**When successful:**

```text
PX_COPY %v2, %v1  → (eliminated, %v2 uses same phys reg as %v1)
```

### Allocation Result

```cpp
struct AllocationResult {
    std::unordered_map<uint16_t, PhysReg> vregToPhys;  // Assignments
    std::vector<SpillSlot> spillSlots;                 // Spill slots
    std::vector<PhysReg> usedCalleeSaved;              // Callee-saved used
};
```

---

## Frame Lowering

### Stack Frame Layout

System V AMD64 stack frame (grows downward):

```text
Higher addresses
┌────────────────────┐
│  Return address    │  [rbp + 8]
├────────────────────┤
│  Saved RBP         │  [rbp]      ← Frame pointer
├────────────────────┤
│  Callee-saved regs │  [rbp - 8 * N]
├────────────────────┤
│  GPR spill area    │  [rbp - ...]
├────────────────────┤
│  XMM spill area    │  [rbp - ...]
├────────────────────┤
│  Outgoing args     │  [rbp - ...]
└────────────────────┘  ← Stack pointer
Lower addresses
```

### FrameInfo

Summarizes frame requirements:

```cpp
struct FrameInfo {
    int spillAreaGPR;                // GPR spill bytes
    int spillAreaXMM;                // XMM spill bytes
    int outgoingArgArea;             // Call argument space
    int frameSize;                   // Total frame size
    std::vector<PhysReg> usedCalleeSaved;  // Regs to save
};
```

### Prologue Emission

```asm
# Function entry
push   %rbp
mov    %rsp, %rbp
sub    $frameSize, %rsp   # Allocate stack space

# Save callee-saved registers
push   %rbx
push   %r12
# ... (for each used callee-saved reg)
```

### Epilogue Emission

```asm
# Restore callee-saved registers
pop    %r12
pop    %rbx
# ...

# Function exit
leave                      # mov %rbp, %rsp; pop %rbp
ret
```

### Spill Slot Assignment

```cpp
void assignSpillSlots(MFunction& func, FrameInfo& frame);
```

Replaces abstract spill slots with concrete stack offsets:

```text
# Before
MOV [SPILL_GPR(0)], %rax

# After
MOV [%rbp - 16], %rax
```

---

## Assembly Emission

### AsmEmitter Class

Translates MIR to AT&T syntax assembly:

```cpp
class AsmEmitter {
    void emitFunction(std::ostream& os, const MFunction& func) const;
    void emitRoData(std::ostream& os) const;
};
```

### Encoding Table

Instructions are matched against an **encoding specification table**:

```cpp
struct EncodingRow {
    MOpcode opcode;            // MIR opcode
    std::string_view mnemonic; // Assembly mnemonic
    EncodingForm form;         // Operand pattern
    OperandOrder order;        // Emission order
    OperandPattern pattern;    // Expected operands
    EncodingFlag flags;        // REX.W, ModRM, etc.
};
```

**Example encoding:**

```cpp
{MOpcode::ADDrr, "addq", EncodingForm::RegReg,
 OperandOrder::R_R, {Reg, Reg}, EncodingFlag::REXW}
```

### Operand Formatting

AT&T syntax rules:

- **Registers**: `%rax`, `%xmm0`
- **Immediates**: `$42`
- **Memory**: `displacement(%base, %index, scale)`
- **Labels**: `symbol` or `symbol(%rip)` for RIP-relative

**Examples:**

```asm
movq   %rax, %rbx           # Register to register
movq   $42, %rax            # Immediate to register
movq   (%rdi), %rax         # Memory to register
leaq   8(%rsp), %rax        # Address calculation
movsd  .LC0(%rip), %xmm0    # RIP-relative load
```

### RoData Pool

String and floating-point literals are pooled:

```cpp
class RoDataPool {
    int addStringLiteral(std::string bytes);
    int addF64Literal(double value);
    std::string stringLabel(int index) const;  // ".LC0"
    std::string f64Label(int index) const;      // ".LF0"
    void emit(std::ostream& os) const;
};
```

**Emitted rodata section:**

```asm
    .section .rodata
.LC0:
    .string "Hello, world!"
.LF0:
    .quad 0x400921fb54442d18  # 3.14159265358979323846
```

---

## Calling Convention

The backend supports both **System V AMD64** (Linux) and **Windows x64** calling conventions.
The appropriate ABI is selected automatically based on the host platform. macOS x86-64 is not a
supported target; on macOS the backend targets AArch64 only.

### System V AMD64 ABI

Used on Linux and other Unix-like x86-64 systems:

**Integer/Pointer Arguments:**

1. `RDI`
2. `RSI`
3. `RDX`
4. `RCX`
5. `R8`
6. `R9`
7. Stack (right-to-left)

**Floating-Point Arguments:**

1. `XMM0`
2. `XMM1`
3. `XMM2`
4. `XMM3`
5. `XMM4`
6. `XMM5`
7. `XMM6`
8. `XMM7`
9. Stack

**Return Values:**

- Integer/pointer: `RAX`
- Floating-point: `XMM0`

**Caller/Callee-Saved:**

- **Caller-saved**: `RAX`, `RCX`, `RDX`, `RSI`, `RDI`, `R8`-`R11`, `XMM0`-`XMM15`
- **Callee-saved**: `RBX`, `RBP`, `R12`-`R15`

### Windows x64 ABI

Used on Windows:

**Integer/Pointer Arguments:**

1. `RCX`
2. `RDX`
3. `R8`
4. `R9`
5. Stack (right-to-left)

**Floating-Point Arguments:**

1. `XMM0`
2. `XMM1`
3. `XMM2`
4. `XMM3`
5. Stack

**Return Values:**

- Integer/pointer: `RAX`
- Floating-point: `XMM0`

**Caller/Callee-Saved:**

- **Caller-saved**: `RAX`, `RCX`, `RDX`, `R8`-`R11`, `XMM0`-`XMM5`
- **Callee-saved**: `RBX`, `RBP`, `RDI`, `RSI`, `R12`-`R15`, `XMM6`-`XMM15`

**Shadow Space:**

Windows x64 requires 32 bytes of shadow space before each call for register argument spilling.

### Call Lowering

```cpp
struct CallLoweringPlan {
    std::string calleeLabel;   // Function to call
    std::vector<CallArg> args; // Arguments
    bool returnsF64;           // Return type
    bool isVarArg;            // Vararg flag
};

void lowerCall(MBasicBlock& block, size_t insertIdx,
               const CallLoweringPlan& plan, FrameInfo& frame);
```

**Call sequence:**

1. **Compute argument layout** (register vs. stack)
2. **Emit argument moves** to physical registers
3. **Update stack pointer** if stack arguments present
4. **Emit CALL** instruction
5. **Capture return value** from `RAX` or `XMM0`
6. **Restore stack pointer**

**Example:**

```asm
# Call foo(a, b, c) where a,b,c are in %v1,%v2,%v3
movq   %v1, %rdi       # First arg
movq   %v2, %rsi       # Second arg
movq   %v3, %rdx       # Third arg
call   foo
movq   %rax, %v_result # Capture return value
```

---

## AArch64 Backend

### Overview

The AArch64 backend targets 64-bit ARM processors (Apple Silicon, ARM servers). It shares design principles with the
x86-64 backend but is tailored for the ARM instruction set and AAPCS64 calling convention.

### Key Characteristics

| Feature      | Description                                    |
|--------------|------------------------------------------------|
| **Target**   | AArch64 (ARM64) architecture                   |
| **ABI**      | AAPCS64 (ARM Procedure Call Standard)          |
| **Output**   | ARM assembly (GAS-compatible)                  |
| **Strategy** | SSA-based with linear scan register allocation |
| **Status**   | Functional for core operations                 |

### Supported Features

- Array and object operations
- Bitwise operations (and, or, xor, shifts)
- Comparisons and conditional branches
- Floating-point arithmetic (fadd, fsub, fmul, fdiv)
- Function calls (direct)
- Integer arithmetic (iadd.ovf, isub.ovf, imul.ovf, sdiv.chk0, udiv.chk0, srem.chk0, urem.chk0)
- Local variables (FP-relative addressing)
- Switch statements

### AArch64 Register Usage

**Callee-saved:** X19-X28
**Caller-saved:** X9-X15
**Float Arguments:** V0-V7 (D registers)
**Frame Pointer:** X29 (FP)
**Integer Arguments:** X0-X7
**Link Register:** X30 (LR)
**Return Values:** X0 (integer), V0 (float)
**Stack Pointer:** SP

### Source Files

```text
src/codegen/aarch64/
├── AsmEmitter.hpp/cpp         # ARM assembly emission
├── FastPaths.hpp/cpp          # Fast-path instruction selection
├── FrameBuilder.hpp/cpp       # Stack frame construction
├── FramePlan.hpp              # Frame layout planning
├── InstrLowering.hpp/cpp      # Individual instruction lowering
├── LivenessAnalysis.hpp/cpp   # Live variable analysis
├── LowerILToMIR.hpp/cpp       # IL → MIR lowering driver
├── LoweringContext.hpp        # Context for lowering pass
├── MachineIR.hpp/cpp          # Machine IR structures
├── OpcodeDispatch.hpp/cpp     # Opcode-specific dispatch
├── OpcodeMappings.hpp         # IL opcode to MIR mapping tables
├── Peephole.hpp/cpp           # Peephole optimizations
├── RegAllocLinear.hpp/cpp     # Linear scan allocator
├── RodataPool.hpp/cpp         # Read-only data management
├── TargetAArch64.hpp/cpp      # Target description
├── TerminatorLowering.hpp/cpp # Branch/call/ret lowering
├── fastpaths/                 # Fast-path implementations
└── generated/                 # Generated opcode/format tables (EncodingTable.inc, OpFmtTable.inc)
```

### Usage

```bash
# Generate ARM assembly from IL
zanna codegen arm64 program.il -S program.s

# Assemble and link (macOS, simple/monolithic runtime)
as program.s -o program.o
clang++ program.o build/src/runtime/libzanna_runtime.a -o program

# Recommended: let zanna link the native executable (auto-selects required runtime components)
zanna codegen arm64 program.il -run-native
```

---

## Source Code Guide

### Directory Structure

```text
src/codegen/
├── common/                        # Shared utilities
│   ├── Diagnostics.hpp/cpp        # Codegen diagnostic reporting
│   ├── LabelUtil.hpp              # Label sanitization helpers
│   ├── LinkerSupport.hpp/cpp      # Runtime symbol resolution for linking
│   ├── PassManager.hpp            # Abstract pass manager interface
│   ├── RuntimeComponents.hpp      # Runtime component dependency tracking
│   └── TargetInfoBase.hpp         # Base template for target register info
│
├── aarch64/                       # ARM64 backend (see above)
│
└── x86_64/                        # x86-64 backend
    ├── AsmEmitter.hpp/cpp         # Assembly emission
    ├── Backend.hpp/cpp            # High-level facade and pipeline orchestration
    ├── CallLowering.hpp/cpp       # Calling convention
    ├── CodegenPipeline.hpp/cpp    # End-to-end pipeline
    ├── FrameLowering.hpp/cpp      # Stack frame layout
    ├── ISel.hpp/cpp               # Instruction selection
    ├── LowerDiv.cpp               # Division/modulo lowering
    ├── LowerILToMIR.hpp/cpp       # IL → MIR lowering
    ├── LowerOvf.cpp               # Overflow-checked arithmetic lowering
    ├── Lowering.Arith.cpp         # Arithmetic ops lowering
    ├── Lowering.Bitwise.cpp       # Bitwise ops lowering
    ├── Lowering.CF.cpp            # Control flow lowering
    ├── Lowering.EH.cpp            # Exception handling lowering
    ├── Lowering.EmitCommon.*      # Shared lowering helpers
    ├── Lowering.Mem.cpp           # Memory ops lowering
    ├── LoweringRules.hpp/cpp      # Lowering rule registry
    ├── LoweringRuleTable.hpp/cpp  # Rule table generator
    ├── MachineIR.hpp/cpp          # MIR data structures
    ├── OperandUtils.hpp           # Operand utilities
    ├── Peephole.hpp/cpp           # Peephole optimization
    ├── RegAllocLinear.hpp/cpp     # Register allocation (top-level driver)
    ├── TargetX64.hpp/cpp          # x86-64 target description
    ├── Unsupported.hpp            # Unsupported opcode tracking
    ├── asmfmt/Format.hpp/cpp      # Formatting helpers
    ├── generated/                 # Generated opcode/format tables (EncodingTable.inc, OpFmtTable.inc)
    ├── passes/                    # Pipeline passes
    │   ├── BinaryEmitPass.hpp/cpp # Native object emission pass
    │   ├── EmitPass.hpp/cpp       # Assembly emission pass
    │   ├── LegalizePass.hpp/cpp   # Instruction selection pass
    │   ├── LoweringPass.hpp/cpp   # IL lowering pass
    │   ├── PassManager.hpp/cpp    # Pass orchestration
    │   ├── PeepholePass.hpp/cpp   # Post-RA MIR optimization pass
    │   ├── RegAllocPass.hpp/cpp   # Register allocation pass
    │   └── SchedulerPass.hpp/cpp  # Post-RA scheduling pass
    └── ra/                        # Register allocation internals
        ├── Allocator.hpp/cpp      # Linear scan allocator
        ├── Coalescer.hpp/cpp      # Copy coalescing
        ├── LiveIntervals.hpp/cpp  # Live interval analysis
        └── Spiller.hpp/cpp        # Spill code insertion
```

### Key Files by Functionality

**Core Infrastructure:**

- `Backend.hpp`, `Backend.cpp` — High-level API
- `CodegenPipeline.hpp` — End-to-end compilation
- `MachineIR.hpp` — MIR data structures
- `TargetX64.hpp` — x86-64 register/ABI definitions

**Lowering:**

- `LowerILToMIR.hpp` — IL to MIR adapter
- `LoweringRules.hpp` — Rule-based lowering
- `Lowering.*.cpp` — Lowering implementations by category

**Legalization:**

- `ISel.hpp` — Instruction selection and legalization
- `LowerDiv.cpp` — Division/remainder lowering
- `LowerOvf.cpp` — Overflow-checked arithmetic lowering
- `Peephole.hpp` — Simple peephole optimizations

**Register Allocation:**

- `ra/Allocator.hpp` — Linear scan algorithm
- `ra/Coalescer.hpp` — Copy coalescing
- `ra/LiveIntervals.hpp` — Liveness analysis
- `ra/Spiller.hpp` — Spill code insertion

**ABI & Frame:**

- `CallLowering.hpp` — System V call lowering
- `FrameLowering.hpp` — Stack frame construction

**Emission:**

- `AsmEmitter.hpp` — AT&T assembly output
- `asmfmt/Format.hpp` — Operand formatting

**Passes:**

- `passes/PassManager.hpp` — Pass orchestration
- `passes/*Pass.hpp` — Individual pipeline passes

---

## Best Practices

### For Backend Developers

1. **Lowering Rules**: Keep rules simple and focused on one opcode family
2. **MIR Design**: Add new operand types conservatively
3. **Register Allocation**: Maintain deterministic allocation order
4. **Testing**: Add golden assembly tests for each new feature
5. **Documentation**: Document ABI deviations and assumptions

### For IL Generators

1. **SSA Form**: Ensure proper SSA before backend entry
2. **Type Safety**: Match IL types to backend expectations
3. **ABI Awareness**: Understand calling convention requirements
4. **Testing**: Verify assembly output for correctness

### For Embedders

1. **Configuration**: Use `CodegenOptions` for future compatibility
2. **Error Handling**: Check `CodegenResult.errors` for diagnostics
3. **Assembly Integration**: Link generated assembly with system linker
4. **Toolchain**: Ensure GAS-compatible assembler is available

---

## Further Reading

**Zanna Documentation:**

- **[IL Guide](../il/il-guide.md)** — IL specification and semantics
- **[IL Reference](../il/il-guide.md#reference)** — Complete opcode catalog
- **[VM Architecture](vm.md)** — VM execution model

**Developer Documentation:**

- [x86-64 Backend](../specs/x86_64.md) — Additional x86-64 backend notes
- [AArch64 Backend](../specs/aarch64.md) — ARM64 backend notes
- [Architecture](architecture.md) — Overall system architecture
- [Object Layout](../specs/object-layout.md) — ABI specification details

**External References:**

- **System V AMD64 ABI
  **: [https://refspecs.linuxfoundation.org/elf/x86_64-abi-0.99.pdf](https://refspecs.linuxfoundation.org/elf/x86_64-abi-0.99.pdf)
- **Intel x86-64 Manual
  **: [https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)
- **SSA Book**: "SSA-based Compiler Design" by Rastello & Bouchez Tichadou

**Source Code:**

- `src/codegen/x86_64/` — Backend implementation
- `src/tests/codegen/` — Backend tests
