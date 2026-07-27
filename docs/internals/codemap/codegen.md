---
status: active
audience: contributors
last-verified: 2026-07-26
---

# CODEMAP: Codegen

## Status

- AArch64: Validated end‑to‑end on Apple Silicon across all demo games.
- x86_64: Validated on Windows with full codegen test suite passing. Supports System V AMD64 and Windows x64 ABIs.

Native code generation backends for x86_64 and AArch64. Current backend
behavior is documented in [backend.md](../backend.md) and the architecture-
specific specs [x86_64.md](../../specs/x86_64.md) and
[aarch64.md](../../specs/aarch64.md).

## Common Utilities (`src/codegen/common/`)

| File                       | Purpose                                             |
|----------------------------|-----------------------------------------------------|
| `CallArgLayout.hpp/cpp`    | Shared call argument classification (SysV/Win64)    |
| `PreRAForwardCopy.hpp`     | Shared pre-RA identity/single-use copy forwarding   |
| `CallLoweringPlan.hpp`     | Call lowering plan data structure                    |
| `Diagnostics.hpp/cpp`      | Codegen-specific diagnostic reporting               |
| `FrameLayout.hpp`          | Stack frame layout data structure                   |
| `FrameLayoutUtils.hpp`     | Frame lowering utility functions                    |
| `ICE.hpp`                  | Internal compiler error reporting                   |
| `LabelUtil.hpp`            | Deterministic label generation for blocks and jumps |
| `LinkerSupport.hpp/cpp`    | Platform linker invocation and object file support  |
| `NativeEHLowering.hpp/cpp` | Structured EH to runtime call lowering              |
| `PassManager.hpp`          | Codegen pass manager interface                      |
| `PeepholeCopyProp.hpp`     | Shared peephole copy propagation patterns           |
| `PeepholeDCE.hpp`          | Shared peephole dead code elimination patterns      |
| `PeepholeUtil.hpp`         | Shared peephole utility functions                   |
| `RuntimeComponents.hpp`    | Runtime component descriptors for codegen           |
| `TargetInfoBase.hpp`       | Base class for target-specific information          |
| `AArch64RelocUtil.hpp`     | Shared AArch64 relocation helper utilities          |

### Register Allocation Utilities (`src/codegen/common/ra/`)

| File                  | Purpose                                         |
|-----------------------|-------------------------------------------------|
| `ArchTraits.hpp`      | Architecture trait definitions for register alloc |
| `CfgExtract.hpp`      | Shared MIR successor extraction for RA liveness  |
| `DataflowLiveness.hpp`| Dataflow-based liveness analysis                |

### Object File Writers (`src/codegen/common/objfile/`)

| File                       | Purpose                                        |
|----------------------------|------------------------------------------------|
| `CodeSection.hpp`          | Code section data structure (symbols, relocs)  |
| `CoffWriter.hpp/cpp`       | COFF (.obj) writer for Windows                 |
| `DebugLineTable.hpp/cpp`   | DWARF debug line table builder                 |
| `ElfWriter.hpp/cpp`        | ELF (.o) writer for Linux                      |
| `MachOWriter.hpp/cpp`      | Mach-O (.o) writer for macOS                   |
| `ObjectFileWriter.hpp/cpp` | Multi-format object file writer dispatcher     |
| `ObjFileWriterUtil.hpp`    | Shared utility functions for object writers    |
| `Relocation.hpp`           | Relocation entry data structures               |
| `StringTable.hpp/cpp`      | String table builder for object files          |
| `SymbolTable.hpp/cpp`      | Symbol table builder for object files          |
| `RelocationValidation.hpp` | Relocation range/target validation helpers     |

### Native Linker (`src/codegen/common/linker/`)

| File                       | Purpose                                           |
|----------------------------|---------------------------------------------------|
| `AlignUtil.hpp`            | Alignment calculation utilities                   |
| `ArchiveReader.hpp/cpp`    | COFF/ELF archive (.lib/.a) reader                 |
| `BranchTrampoline.hpp/cpp` | AArch64 branch trampoline insertion               |
| `CoffReader.cpp`           | COFF object file reader                           |
| `DeadStripPass.hpp/cpp`    | Unreferenced section removal                      |
| `DynStubGen.hpp/cpp`       | Dynamic symbol stub generation                    |
| `ElfExeWriter.hpp/cpp`     | ELF executable writer                             |
| `ElfReader.cpp`            | ELF object file reader                            |
| `ExeWriterUtil.hpp`        | Shared executable writer utilities                |
| `ICF.hpp/cpp`              | Identical Code Folding                            |
| `LinkTypes.hpp`            | Linker type definitions and structures            |
| `MachOBindRebase.hpp/cpp`  | Mach-O bind/rebase opcode emission                |
| `MachOCodeSign.hpp/cpp`    | Mach-O ad-hoc code signing                        |
| `MachOExeWriter.hpp/cpp`   | Mach-O executable writer                          |
| `MachOReader.cpp`          | Mach-O object file reader                         |
| `NameMangling.hpp`         | Platform-aware symbol name mangling               |
| `NativeLinker.hpp/cpp`     | Top-level native linker driver                    |
| `ObjFileReader.hpp/cpp`    | Multi-format object file reader dispatcher        |
| `PeExeWriter.hpp/cpp`      | PE32+ executable writer for Windows               |
| `RelocApplier.hpp/cpp`     | Relocation patching with range checking           |
| `RelocClassify.hpp`        | Relocation classification utilities               |
| `RelocConstants.hpp`       | Relocation type constants                         |
| `SectionMerger.hpp/cpp`    | Cross-object section merging and VA assignment    |
| `StringDedup.hpp/cpp`      | String deduplication for merged sections          |
| `SymbolResolver.hpp/cpp`   | Symbol resolution across objects and archives     |
| `DynamicSymbolPolicy.hpp`  | Dynamic symbol export/import policy               |
| `PlatformImportPlanner.hpp`| Platform-agnostic dynamic-import planning interface |
| `LinuxImportPlanner.cpp`   | Linux (ELF) dynamic import planning               |
| `MacImportPlanner.cpp`     | macOS (Mach-O) dynamic import planning            |
| `WindowsImportPlanner.cpp` | Windows (PE) dynamic import planning              |

## AArch64 Backend (`src/codegen/aarch64/`)

Targeting AAPCS64 (Apple Silicon, Linux ARM64).

### Core Infrastructure

| File                    | Purpose                                |
|-------------------------|----------------------------------------|
| `FrameBuilder.hpp/cpp`  | Stack frame construction               |
| `FramePlan.hpp`         | Stack frame layout planning            |
| `LoweringContext.hpp`   | Lowering pass context and state        |
| `MachineIR.hpp/cpp`     | Machine IR data structures             |
| `OpcodeMappings.hpp`    | IL opcode to MIR opcode mapping        |
| `TargetAArch64.hpp/cpp` | Target description and ABI information |
| `MOpcodeDef.inc`        | X-macro MIR opcode definitions         |
| `Noreturn.hpp`          | No-return call annotation helpers      |

### Lowering

| File                        | Purpose                            |
|-----------------------------|------------------------------------|
| `InstrLowering.hpp/cpp`     | Per-instruction lowering rules     |
| `LowerILToMIR.hpp/cpp`      | Main IL to MIR lowering driver     |
| `OpcodeDispatch.hpp/cpp`    | Opcode-based dispatch for lowering |
| `TerminatorLowering.hpp/cpp`| Block terminator lowering          |
| `FpCompareLowering.hpp`     | Floating-point comparison lowering |

### Pipeline & Infrastructure

| File                          | Purpose                                |
|-------------------------------|----------------------------------------|
| `CodegenPipeline.hpp/cpp`     | End-to-end AArch64 compilation pipeline|
| `A64ImmediateUtils.hpp`       | Immediate encoding/decoding helpers    |
| `LivenessAnalysis.hpp/cpp`    | CFG-level liveness analysis            |
| `LowerOvf.hpp/cpp`            | Overflow-checked operation lowering    |
| `FrameCodegen.hpp`            | Frame code generation helpers          |
| `RegAllocLinear.hpp/cpp`      | Legacy linear scan (pre-ra/ refactor)  |

### Register Allocation (`ra/`)

| File                       | Purpose                                      |
|----------------------------|----------------------------------------------|
| `ra/Allocator.hpp/cpp`     | Linear scan register allocator with protected-use eviction |
| `ra/Liveness.hpp/cpp`      | Liveness analysis for register alloc         |
| `ra/InstrBuilders.hpp`     | MIR instruction builder helpers              |
| `ra/OpcodeClassify.hpp`    | Opcode classification (call, terminator, mem)|
| `ra/OperandRoles.hpp/cpp`  | Per-operand use/def role classification      |
| `ra/RegClassify.hpp`       | Register class classification                |
| `ra/RegPools.hpp/cpp`      | Physical register pool management            |
| `ra/VState.hpp`            | Virtual register state tracking              |

### Optimization

| File                      | Purpose                              |
|---------------------------|--------------------------------------|
| `Coalescer.hpp/cpp`       | Pre-RA register coalescer            |
| `PreRegAllocOpt.hpp/cpp`  | Pre-register-allocation optimization |
| `Peephole.hpp/cpp`        | Top-level peephole dispatcher        |
| `peephole/BranchOpt.hpp/cpp`       | Branch optimization sub-pass        |
| `peephole/CopyPropDCE.hpp/cpp`     | Copy propagation and DCE sub-pass   |
| `peephole/IdentityElim.hpp/cpp`    | Identity instruction elimination    |
| `peephole/LoopOpt.hpp/cpp`         | Loop-level peephole optimization    |
| `peephole/Dominators.hpp/cpp`      | Dominator-tree analysis for peephole|
| `peephole/MemoryOpt.hpp/cpp`       | Memory access optimization          |
| `peephole/PeepholeCommon.hpp/cpp`  | Shared peephole utilities           |
| `peephole/StrengthReduce.hpp/cpp`  | Strength reduction sub-pass         |

### Pipeline Passes (`passes/`)

| File                               | Purpose                                  |
|------------------------------------|------------------------------------------|
| `passes/PassManager.hpp`           | Pass orchestration and sequencing        |
| `passes/LoweringPass.hpp/cpp`      | IL to MIR lowering pass                  |
| `passes/LegalizePass.hpp/cpp`      | Pre-RA legalization and pseudo expansion |
| `passes/PreRegAllocOptPass.hpp/cpp`| Pre-register-allocation optimization pass|
| `passes/RegAllocPass.hpp/cpp`      | Register allocation pass                 |
| `passes/SchedulerPass.hpp/cpp`     | Instruction scheduling pass              |
| `passes/PeepholePass.hpp/cpp`      | Peephole optimization pass               |
| `passes/BlockLayoutPass.hpp/cpp`   | Basic-block layout pass                  |
| `passes/EmitPass.hpp/cpp`          | Assembly text emission pass              |
| `passes/BinaryEmitPass.hpp/cpp`    | Native binary object emission pass       |

### Binary Encoder (`binenc/`)

| File                              | Purpose                                   |
|-----------------------------------|-------------------------------------------|
| `binenc/A64BinaryEncoder.hpp/cpp` | AArch64 native binary instruction encoder |
| `binenc/A64Encoding.hpp`          | AArch64 encoding tables and format defs   |

### Emission

| File                   | Purpose                                      |
|------------------------|----------------------------------------------|
| `AsmEmitter.hpp/cpp`   | ARM assembly emission                        |
| `RodataPool.hpp/cpp`   | Read-only data pool (constants, strings)     |

### Fast Paths (`fastpaths/`)

| File                       | Purpose                        |
|----------------------------|--------------------------------|
| `FastPaths.hpp/cpp`        | Fast path dispatch             |
| `FastPaths_Arithmetic.cpp` | Arithmetic operation fast paths|
| `FastPaths_Call.cpp`       | Call lowering fast paths       |
| `FastPaths_Cast.cpp`       | Type cast fast paths           |
| `FastPaths_Memory.cpp`     | Memory operation fast paths    |
| `FastPaths_Return.cpp`     | Return instruction fast paths  |
| `FastPathsInternal.hpp`    | Internal fast path utilities   |

## x86_64 Backend (`src/codegen/x86_64/`)

Targeting System V AMD64 ABI (Linux) and Windows x64 ABI. macOS x86-64 is not a supported target;
macOS support is Apple Silicon/ARM64 only.

### Driver & Pipeline

| File                     | Purpose                                    |
|--------------------------|--------------------------------------------|
| `Backend.hpp/cpp`        | High-level backend facade                  |
| `CodegenPipeline.hpp/cpp`| End-to-end compilation pipeline            |

### Target & ABI

| File                    | Purpose                                   |
|-------------------------|-------------------------------------------|
| `CallLowering.hpp/cpp`  | Calling convention implementation         |
| `FrameLowering.hpp/cpp` | Stack frame layout and prologue/epilogue  |
| `TargetX64.hpp/cpp`     | Target description, SysV and Win64 ABIs   |

### Machine IR

| File               | Purpose                                          |
|--------------------|--------------------------------------------------|
| `MachineIR.hpp/cpp`| Machine IR data structures (MInstr, MBasicBlock) |

### IL Lowering

| File                         | Purpose                              |
|------------------------------|--------------------------------------|
| `LowerDiv.cpp`               | Division/modulo lowering             |
| `LowerILToMIR.hpp/cpp`       | Main IL to MIR lowering adapter      |
| `LoweringRuleTable.hpp/cpp`  | Lowering rule table and dispatch     |
| `LoweringRules.hpp/cpp`      | Rule-based lowering infrastructure   |
| `Lowering.Arith.cpp`         | Arithmetic operation lowering        |
| `Lowering.Bitwise.cpp`       | Bitwise operation lowering           |
| `Lowering.CF.cpp`            | Control flow lowering                |
| `Lowering.EH.cpp`            | Exception handling lowering          |
| `Lowering.EmitCommon.hpp/cpp`| Shared lowering emission helpers     |
| `Lowering.Mem.cpp`           | Memory operation lowering            |

### Instruction Selection & Optimization

| File                       | Purpose                              |
|----------------------------|--------------------------------------|
| `ISel.hpp/cpp`             | Instruction selection and legalization|
| `OperandUtils.hpp`         | Operand manipulation utilities       |
| `Peephole.hpp/cpp`         | Peephole optimizations               |
| `OperandRoles.hpp/cpp`     | Per-operand use/def role classification |
| `PreRegAllocOpt.hpp/cpp`   | Pre-register-allocation optimization |
| `Scheduler.hpp/cpp`        | Instruction scheduling               |

### Peephole (`peephole/`)

| File                            | Purpose                              |
|---------------------------------|--------------------------------------|
| `peephole/ArithSimplify.hpp/cpp`| Arithmetic simplification sub-pass   |
| `peephole/BranchOpt.hpp/cpp`    | Branch optimization sub-pass         |
| `peephole/DCE.hpp/cpp`          | Dead code elimination sub-pass       |
| `peephole/MemoryOpt.hpp/cpp`    | Memory access optimization sub-pass  |
| `peephole/MovFolding.hpp/cpp`   | Move folding sub-pass                |
| `peephole/PeepholeCommon.hpp`   | Shared peephole utilities            |

### Pipeline Passes (`passes/`)

| File                      | Purpose                                  |
|---------------------------|------------------------------------------|
| `BinaryEmitPass.hpp/cpp`  | Native binary object emission pass       |
| `EmitPass.hpp/cpp`        | Assembly text emission pass              |
| `LegalizePass.hpp/cpp`    | Pre-RA legalization, pseudo expansion, runtime-entry MIR insertion |
| `LoweringPass.hpp/cpp`    | IL to MIR lowering pass                  |
| `PassManager.hpp/cpp`     | Pass orchestration and sequencing        |
| `PeepholePass.hpp/cpp`    | Peephole optimization pass               |
| `RegAllocPass.hpp/cpp`    | Register allocation pass                 |
| `PreRegAllocOptPass.hpp/cpp`| Pre-register-allocation optimization pass |
| `SchedulerPass.hpp/cpp`   | Instruction scheduling pass              |

### Register Allocation (`ra/`)

| File                    | Purpose                        |
|-------------------------|--------------------------------|
| `Allocator.hpp/cpp`     | Linear scan register allocator |
| `Coalescer.hpp/cpp`     | Copy coalescing                |
| `LiveIntervals.hpp/cpp` | Live interval computation      |
| `Liveness.hpp/cpp`      | Liveness analysis              |
| `Spiller.hpp/cpp`       | Spill code insertion           |

### Binary Encoder (`binenc/`)

| File                       | Purpose                                     |
|----------------------------|---------------------------------------------|
| `X64BinaryEncoder.hpp/cpp` | x86-64 native binary instruction encoder    |
| `X64Encoding.hpp`          | Encoding tables and format definitions      |

### Generated Tables (`generated/`)

| File                          | Purpose                                       |
|-------------------------------|-----------------------------------------------|
| `generated/EncodingTable.inc` | Generated MIR opcode → machine encoding table |
| `generated/OpFmtTable.inc`    | Generated MIR opcode operand-format table     |

### Legacy Register Allocation

| File                    | Purpose                                      |
|-------------------------|----------------------------------------------|
| `RegAllocLinear.hpp/cpp`| Original linear scan allocator (deprecated)  |

### Assembly Formatting (`asmfmt/`)

| File              | Purpose                                           |
|-------------------|---------------------------------------------------|
| `Format.hpp/cpp`  | AT&T syntax assembly formatting helpers           |

### Emission

| File                 | Purpose                                        |
|----------------------|------------------------------------------------|
| `AsmEmitter.hpp/cpp` | x86-64 assembly emission with encoding table   |

### Overflow Lowering

| File             | Purpose                                            |
|------------------|----------------------------------------------------|
| `LowerOvf.cpp`   | Overflow-checked operation lowering                |

### Miscellaneous

| File             | Purpose                                            |
|------------------|----------------------------------------------------|
| `Unsupported.hpp`| Tracking for unsupported IL opcodes                |
