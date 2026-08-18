---
status: active
audience: public
last-verified: 2026-08-17
---

# Zanna Compiler Platform — Release Notes

> **Development Status:** Pre-Alpha. Zanna is under active development and not ready for production use.

## Version 0.2.3 — Pre-Alpha (2026-03-25)

### What this release is about

A major hardening, tooling, and infrastructure cycle. Five themes:

- **3D Graphics Engine (new).** A 28-class zero-dependency C runtime with four pluggable backends (Metal / D3D11 / OpenGL / software rasterizer): scene graph with frustum culling, skeletal animation + morph targets + blend trees, Physics3DBody + Physics3DWorld + Trigger3D, particles, terrain, water, post-FX (bloom / vignette / color grading), cubemap skybox, instance batching, FBX loader, NavMesh3D + Path3D, render-to-texture, Character3D controller.
- **Native assembler + linker (new).** `zanna build` goes source → executable with zero external tools. Binary encoders for x86-64 (49 opcode encodings) and AArch64 (70+ opcodes), object-file writers for ELF / Mach-O / PE/COFF, archive reader (GNU ar / BSD ar / COFF), symbol resolution with archive demand-pull, section merging, relocation application, executable writers for all three formats. DWARF v5 `.debug_line` encoding, Identical Code Folding, branch trampolines for out-of-range relocations, Mach-O two-level namespace (fixes macOS ARM64 PAC pointer-auth traps), ad-hoc code signing. 11 demos build and run through the native pipeline.
- **AArch64 performance overhaul.** Post-RA list scheduler with latency-based priority, pre-regalloc register coalescer (`MovRR` / `FMovRR` elimination via live-interval interference, ~270 LOC), MIR loop-invariant constant hoisting, cross-block store-load forwarding, division/modulo strength reduction (`SDIV` by power-of-2: 22→4 cycles; `UREM` by power-of-2: 26→1 cycle). Full O2 pipeline restored — 10 IL optimizer passes were missing from native codegen. **24-87% benchmark improvements** on Apple M4 Max.
- **Interactive REPL + Language Server.** `zanna repl` / `zia` / `vbasic` with tab completion, persistent history, meta-commands (`.il`, `.time`, `.load`, `.save`, …), piped input, Windows support; built on the TUI framework with zero external deps. New dual-protocol `zia-server` speaks both MCP (for AI assistants) and LSP (for editors), ships with VS Code extension auto-discovery.
- **Comprehensive safety audits.** Multi-phase hardening touching every layer: VM execution loop, x86-64 and AArch64 codegen, runtime resource lifecycle, capacity-overflow guards across 16 collection files, file operations, allocation-failure paths, type system, network/TLS security (14K-line audit + 10 new classes), 3D graphics, and TSan-verified concurrency.

Cross-cutting: 3 new IL optimizer passes (EH optimization, loop rotation, reassociation), enum types in Zia and BASIC with match exhaustiveness, async/await, codebase reorganization (demos consolidated under `examples/`, devdocs merged into `docs/`, 900+ doc fixes).

### By the Numbers

| Metric | v0.2.2 | v0.2.3 | Delta |
|---|---|---|---|
| Commits | — | 100 | +100 |
| Source files | 2,288 | 2,671 | +383 |
| Production SLOC | ~310K | ~348K | +38K |
| Test count | 1,261 | 1,351 | +90 |
| Runtime classes | 226 | 272 | +46 |
| IL optimizer passes | 35 | 38 | +3 |

Lines added / removed: ~279K / ~323K — net removal because ~1,100 stale files were deleted (obsolete test fixtures, dead demos, devdocs, zia-review).

---

### 3D Graphics Engine (new)

- **Core architecture.** Scene graph with frustum culling and node hierarchy (`SceneGraph`, `SceneNode`), transform system with parent-child propagation (`Transform3D`), material system with PBR properties + environment mapping + emissive surfaces (`Material3D`), camera system with shake / follow / smooth interpolation (`Camera3D`), lights (point / directional / spot, `Light3D`).
- **Rendering.** Four backends: Metal (macOS), D3D11 (Windows), OpenGL (Linux), software rasterizer (fallback). Multi-pass pipeline with alpha blending + depth sorting. Render-to-texture (`RenderTarget3D`). Post-processing effects (bloom, vignette, color grading, screen-space) in `PostFX3D`. Skybox with cubemap (`CubeMap3D`). Instance batching (`InstanceBatch3D`). Decals (`Decal3D`), terrain (`Terrain3D`), water (`Water3D`).
- **Animation + physics.** Skeletal animation with bone hierarchies and blend trees (`Skeleton3D`, `Animation3D`, `AnimBlend3D`, `AnimPlayer3D`). Morph targets for facial/mesh deformation (`MorphTarget3D`). `Character3D` combining skeletal animation + physics. 3D physics with rigid bodies and trigger volumes (`Physics3DBody`, `Physics3DWorld`, `Trigger3D`).
- **Navigation + loading.** `NavMesh3D` for AI pathfinding, `Path3D` waypoint-based movement, FBX loader for meshes + skeletons + animations, `RayHit3D` raycasting, `Particles3D`.
- **Post-audit hardening.** Canvas3D depth-buffer bounds validation + initialization guards; Mesh3D vertex/index bounds checking on construction; SceneGraph node cleanup + dangling-reference prevention; software rasterizer triangle clipping + edge-case guards; Pixels module additional bounds checks for direct pixel operations.

### Native toolchain (assembler + linker)

- **Assembler (MIR → `.o`).** Binary encoders for x86-64 (49 opcode encodings, REX/ModR/M/SIB, RIP-relative relocations) and AArch64 (70+ opcodes, branch resolution, FP literal materialization). Object-file writers for ELF (x86_64 + AArch64), Mach-O (x86_64 + AArch64), PE/COFF (Windows). Per-function text sections for dead stripping. `--native-asm` / `--system-asm` CLI flags.
- **Linker (`.o` + archives → executable).** Archive reader (GNU ar, BSD ar, COFF). Object readers for ELF / Mach-O / COFF with unified `ObjFile` API. Symbol resolution with weak/strong precedence + archive demand-pull. Section merging with ObjC metadata preservation + page-aligned layout. Relocation application for x86-64 and AArch64 (PC-rel, absolute, ADRP/PageOff12). Executable writers for Mach-O (dyld bind opcodes, GOT generation), ELF (program headers, dynamic section), PE (import tables, DOS stub). Native ad-hoc code signing with `CS_LINKER_SIGNED` (arm64 macOS). `--native-link` / `--system-link` CLI flags.
- **Optimizations.** Mark-and-sweep dead section stripping (`DeadStripPass`) — ~53% binary size reduction. Cross-module string deduplication (promotes duplicate LOCAL rodata to shared GLOBAL symbols). Segment-level VA packing. Debug/metadata section filtering.
- **Debug info + advanced features.** DWARF v5 `.debug_line` via `DebugLineTable` (maps machine instructions back to source file / line / column). Non-alloc debug section handling across all three writers (ELF `SHT_PROGBITS` without `SHF_ALLOC`, Mach-O `__DWARF` segment with `vmaddr=0`, PE `IMAGE_SCN_MEM_DISCARDABLE`). Identical Code Folding (ICF) merges functions with byte-identical `.text` + matching relocations. Branch trampoline generation for out-of-range relocations (±128MB AArch64, ±2GB x86-64) auto-inserts veneer stubs.
- **Mach-O two-level namespace.** `MH_TWOLEVEL` flag replaces flat namespace — required for macOS ARM64 because CoreAnimation PAC pointer authentication traps fire ~1 second after window creation under flat namespace. Per-symbol dylib ordinal assignment via `BIND_OPCODE_SET_DYLIB_ORDINAL_IMM`/`ULEB`. Ordinal-`-2` fallback for `OBJC_CLASS_$` / `OBJC_METACLASS_$` symbols whose defining framework can't be determined by prefix. Leading-underscore strip before prefix matching.
- **Assembler/linker layer review (15 items).** Reverse-index map for `findOutputLocation` (O(S×C) → O(1)). Named relocation constants (`RelocConstants.hpp`). Shared `ObjFileWriterUtil.hpp` deduplicating `appendLE` / `alignUp` / `padTo`. Centralized Mach-O name mangling (`NameMangling.hpp`). Extracted relocation classification (`RelocClassify.hpp`, 189 LOC). Immediate-range validation asserts. Dead-strip stats reporting. Shared segment layout utilities. `MachOExeWriter` split 1,029 → 670 LOC into `MachOCodeSign` + `MachOBindRebase`. Data-driven framework detection (`kFrameworkRules[]` table). 13 assembler tests, 6 string-dedup tests, 11 debug-line tests, 9 debug-section tests, 10 ELF-exe-writer tests, 7 linker integration tests.

### IL optimizer

- **EH optimization (new pass).** Removes redundant `eh.push` / `eh.pop` pairs when the protected region contains no potentially-throwing instructions. Dead handler blocks cleaned up by subsequent DCE/SimplifyCFG. Registered after check-opt in O2.
- **Loop rotation (new pass).** Converts while-style loops into do-while form by duplicating the header condition into the latch and inserting a guard for the initial check. Eliminates one branch per iteration, improves LICM/unrolling. Conservative: single-latch, single-exit loops with pure headers.
- **Reassociation (new pass).** Canonicalizes operand order for commutative+associative integer ops (Add/Mul/And/Or/Xor). Placed before EarlyCSE to expose more CSE opportunities.
- **Pass improvements.** EarlyCSE fixpoint iteration (max 4 passes). LICM allows non-escaping alloca loads past modifying calls via `BasicAA::isNonEscapingAlloca()`. `ValueKey` adds `ConstStr` + `GAddr` to safe-CSE opcodes. Verifier replaces reachability check with full Cooper-Harvey-Kennedy dominator computation. SCCP treats block params as SSA φ-nodes merging only from executable predecessors. Inliner uses CallGraph SCC analysis (Tarjan) to block mutually-recursive inlining; cost model `maxCodeGrowth=2000`; integrated into O1 pipeline. SimplifyCFG converts hard asserts to soft early-returns for fuzz-test compatibility. CheckOpt removes the incorrect overflow-to-plain demotion (verifier requires `.ovf` variants). DCE fixes `predEdges` map invalidation after instruction removal.
- **Full O2 pipeline restoration.** Restored 10 missing passes to native codegen: `loop-simplify`, `loop-rotate`, `indvars`, `loop-unroll`, `check-opt`, `eh-opt`, `sibling-recursion`, `constfold`, `licm`, `reassociate`. The codegen pipeline was running a stripped-down O1 optimizer despite `-O2` requests. x86-64 codegen O2 gating threshold also fixed.
- **Alloca escape verification.** New Pass 4 in `FunctionVerifier` warns when a `ret` instruction directly returns an alloca-derived pointer, catching stack-pointer escapes at verification time.

### O1 correctness + compilation performance

All 11 demos now compile at O1; 9 of 11 run correctly at native `-O1`.

- **Inliner.** Escaped value analysis resolves actual return types from the module's function lookup table instead of the Void instruction type (fixes paint-app O1 crash). Temp-name uniquification appends `_il{id}` to cloned block params + instruction results to prevent caller/callee scope collisions. `blockBudget=1` limits multi-block inlining until multi-block continuation value threading is hardened.
- **DCE.** Alloca observation split into two passes (collect then observe) to prevent late-defined allocas from overwriting earlier observation marks. Observation checks for store-value operands, return operands, branch arguments. Entry-block param protection via positional ABI prefix preservation with `funcParamId` fast-path.
- **SimplifyCFG.** Entry-block param protection (same as DCE) in `canonicalizeParamsAndArgs`. Verification hooks removed from hot path — debug-only `Verifier::verify()` was verifying the entire module per function per iteration (up to 40 full-module verifications per SimplifyCFG invocation). **27× speedup** for medium modules (zannaide: 4 min → 8.5 s). Large modules (>100K instructions) auto-skip IL optimization entirely to dodge O(n²) behavior; AArch64 codegen peephole still runs. Reduces sqldb compilation from 8+ min to 11 s.
- **BranchVerifier.** Void-typed branch arguments (from DCE-orphaned definitions) are compatible with any param type — register allocator tracks liveness independently of IL types.
- **AArch64 LoopOpt disabled.** The `hoistLoopConstants` peephole was identifying non-loop control flow (if/else merges, function exits) as loops and removing MovRI instructions from mutually exclusive code paths (black ghosts in pacman, crashes in paint).
- **IL peephole removed from O1.** Its global `replaceAll` created cross-block SSA violations when DCE's block-param compaction ran afterward. SCCP already handles constant folding; the AArch64 codegen peephole handles machine-level optimizations.
- **LICM readonly-call safety.** Conservative guard prevents hoisting readonly calls when the loop contains mutating memory operations.

### AArch64 backend

- **Post-RA instruction scheduler.** List scheduler running after register allocation with latency-based priority ordering + anti-dependency tracking. Reorders instructions within basic blocks to improve pipeline utilization while respecting data dependencies + register constraints.
- **Register coalescer (~270 LOC).** Pre-regalloc `MovRR` / `FMovRR` elimination via live-interval interference analysis. Integrated before the register allocator.
- **MIR loop-invariant constant hoisting.** Hoists `MovRI` from loop bodies into preheaders when the register is callee-saved (x19-x28) and defined only by `MovRI` with the same immediate throughout the loop. Natural loop body computation via reverse-reachability BFS from the latch through predecessors (handles non-contiguous loop bodies correctly).
- **Cross-block store-load forwarding.** Forwards stores to loads across basic-block boundaries when the layout predecessor has a single successor and they access the same FP-relative offset. Includes reachability verification.
- **Division/modulo strength reduction.** `SDIV` by power-of-2: sign-corrected arithmetic shift (22→4 cycles). `SDIV` by arbitrary constant: magic-number multiply-high (22→6-8 cycles). `UREM` by power-of-2: AND mask (26→1 cycle). `SREM` by power-of-2: sign-corrected AND+SUB (26→5 cycles).
- **Benchmark results (Apple M4 Max, native -O2 vs baseline):** `branch_stress` 180→24 ms (-87%), `redundant_stress` 315→104 ms (-67%), `mixed_stress` 83→30 ms (-63%), `udiv_stress` 180→74 ms (-59%), `inline_stress` 144→79 ms (-46%), `call_stress` 45→34 ms (-24%), `arith_stress` 140→119 ms (-15%).
- **Additional peephole optimizations.** CSET+branch fusion, dead FP-store elimination, leaf-function register preference (low-numbered caller-saved registers), logical immediate emission (proper AArch64 encoding for AND/ORR/EOR immediates), dead overflow DCE, new `SmulhRRR` MIR opcode for proper signed multiply-high overflow detection.
- **Peephole decomposition.** Monolithic 2,750-line `Peephole.cpp` split into 6 focused sub-passes under `peephole/`: `IdclassElim`, `StrengthReduce`, `CopyPropDCE`, `BranchOpt`, `MemoryOpt`, `LoopOpt`. Shared peephole templates (`PeepholeDCE.hpp`, `PeepholeCopyProp.hpp`) parameterized on target traits are used by both backends.
- **PassManager integration.** Existing pass infrastructure wired into AArch64 `CodegenPipeline`, replacing monolithic per-function loop. Brings ARM64 architecture in line with x86-64.
- **Full EH opcode support.** `EhPush`, `EhPop`, `EhEntry`, `TrapDiv`, `TrapOvf`, `TrapIdx`, `TrapNull`, `TrapCast`, `ErrGetMsg`, `ErrGetCode`, `Resume`.
- **Fixes.** Dominator `intersect()` runtime guard for release builds. `AsmEmitter` `resolveBaseOffset()` helper extracted from 4 duplicated base+offset load/store functions. Loop-body BFS over-inclusion (crawling backwards past the header for BASIC two-header for-loop patterns) caused SIGBUS crashes. SLF reachability verification for cross-block store-load forwarding. Fast-path param stores for >8-argument functions in `lowerCallWithArgs` (was leaving uninitialized stack reads). AArch64 `MOVN` for simple negative immediates instead of multi-instruction `MOVZ`/`MOVK`. Schedule hash-maps replaced with `vector<optional<>>` for O(1) cache-friendly lookup. `CopyPropDCE` `MovRR` / `FMovRR` logic parameterized (90% duplication eliminated). Shared prologue/epilogue iteration (`FrameCodegen.hpp`) eliminates callee-saved register save/restore duplication between AsmEmitter and BinaryEncoder. Division handlers parameterized (4 near-identical → shared `lowerDivisionChk`). `OpcodeDispatch` refactored with handler table replacing 1K-line switch. AsmEmitter consolidated with `emit2Op` / `emit3Op` primitives.

### x86-64 backend

- **Peephole decomposition.** Monolithic 1,470-line `Peephole.cpp` split into 4 focused sub-passes under `peephole/`: `ArithSimplify` (MOV-zero→XOR, CMP-zero→TEST, strength reduction), `MovFolding` (redundant MOV elimination), `DCE` (dead code elimination with implicit register tracking), `BranchOpt` (branch optimization + cold-block reordering). Peephole iteration bounded by `kMaxIterations=100`.
- **CFG-aware register allocation liveness.** Replaced the conservative "unconditional spill" hack (which force-spilled ALL cross-block vregs) with proper backward dataflow liveness analysis. New `LivenessAnalysis` class computes per-block `liveIn` / `liveOut` using standard fixed-point iteration — only vregs that are *truly* live across block boundaries get spill slots.
- **Shared dataflow solver.** Backward dataflow liveness extracted into a shared template (`common/ra/DataflowLiveness.hpp`) used by both backends. AArch64 allocator's inline liveness code extracted into a separate `Liveness` class (matching x86-64's pattern); both delegate to the shared solver. Shared `buildPredecessors()` utility.
- **Shared linker utilities.** Common encoding utilities (`ExeWriterUtil.hpp`) shared between Mach-O and PE executable writers: `writeLE16/32/64`, `writeBE32/64`, `writeULEB128`, `writePad`, `padTo`, `resolveMainAddress()`. ObjC dynamic-stub generation extracted from the native linker into `DynStubGen.hpp/cpp`.
- **Fixes.** EFLAGS clobber: MOV-zero→XOR peephole guarded against EFLAGS clobber between TEST and CMOV (was silent miscompilation for `select` with `falseVal=0`). Block arguments: `EdgeArg` extended with `argValues` to carry full ILValue data for constant block args; constants materialized into fresh vregs via MOVri / MOVSDmr before PX_COPY. Peephole DCE: implicit register liveness for CQO (reads RAX) and IDIV/DIV (reads RAX+RDX) — DCE was incorrectly eliminating dividend loads. Deterministic labels: three static `uint32_t` counters replaced with per-function `nextLocalLabelId()`, reset to 0 per function for output determinism. Parallel-move resolution: entry-parameter MOV loop replaced with `PX_COPY` pseudo-instruction for topological sort + cycle-breaking (fixes crash in multi-param class init calls). GNU-stack marking: `.note.GNU-stack` section added to ELF output for non-executable stack. PE/COFF directives: `.rdata` section and ELF `.type @function` in AsmEmitter. PIE support: `-pie` flag for Linux linker. `LoweringRuleTable.hpp` declarations moved to `.cpp` for faster incremental compile times.

### VM

- **Execution loop hardening (8 fixes).** Convert assert-only guards to runtime traps in branch target lookup, switch index, and EH handlers. Null guard for `prepareTrap` handler. `fr.func->name` null checks in trap diagnostics. Defensive operand checks for trap/EH ops.
- **Virtual dispatch lowering.** O(N) if-else chain replaced with `SwitchI32` for multi-implementation method dispatch.
- **Match-expression lowering.** `SwitchI32` fast path for integer-only match arms without guards, falling back to generic lowering otherwise.
- **ThreadsRuntime async execution.** Improved for async function handling, supporting `Future.Get` continuation semantics for `await` expressions.
- **GC epoch tagging.** Per-entry survival counter skips promoted objects in trial-deletion, with periodic full scans to catch new cycles. Reduces GC overhead for long-lived objects.

### REPL + language server

- **Interactive REPL.** `zanna repl` / `zia` (no args) / `vbasic` (no args). Built on the TUI framework's `TerminalSession` / `InputDecoder` for raw-mode terminal I/O with zero external dependencies. Expression auto-print with type-aware coloring (Bool / Int / Num / String / Object). Variable + function persistence across inputs via statement replay. Entity + value + interface definitions. Multi-line input via bracket depth (Zia) and block-keyword tracking (BASIC). Tab completion via CompletionEngine (Zia) and keyword matching (BASIC). Meta-commands: `.help`, `.quit`, `.clear`, `.vars`, `.funcs`, `.binds`, `.type`, `.il`, `.time`, `.load`, `.save`. Persistent history (`~/.zanna/repl_history_{lang}`). Double Ctrl-C exit. Non-interactive piped input. Windows support via `_pipe` / `_dup` / `_dup2` / `_read` stdout capture. 86 unit tests (51 Zia + 35 BASIC).
- **`zia-server` (dual MCP/LSP).** JSON value type + recursive-descent parser/emitter (zero deps). MCP transport (newline-delimited) with 11 tool definitions for AI assistants. LSP transport (Content-Length framed) with diagnostics, completions, hover, document symbols. JSON-RPC 2.0 request/response handling. `CompilerBridge` facade wraps Zia compiler APIs (including class-field hover). VS Code extension with auto-discovery of `zia-server` binary.

### Zia frontend

- **Enum types.** `enum Color { Red, Green = 5, Blue }` with explicit/auto-increment values, `expose` visibility, match exhaustiveness checking, variant access via dot notation. Variants lowered as I64 constants. 16 Zia + 8 BASIC enum tests.
- **Match OR patterns.** `match x { 1 | 2 | 3 => … }` via new `Pattern::Kind::Or` lowered as a waterfall of test blocks — each subpattern's success jumps to the arm body; failure falls through to the next.
- **Typed catch with list shorthand.** Exception handlers can now specify a type for the caught error, with list-literal shorthand syntax.
- **Say/Print auto-dispatch.** `Say(42)`, `Say(3.14)`, `Say(true)` work without explicit `.ToString()` via typed runtime variants (`SayInt`, `SayNum`, `SayBool`, `PrintBool`).
- **Async/await.** New `async` / `await` keywords with AST nodes, parser support, semantic checking, and lowering to `Future.Get` runtime calls.
- **`.Len` → `.Length` rename.** Collection `.Len` property renamed to `.Length` across List / Map / Set for spec consistency; `.Len` retained as alias for back-compat.
- **Bible audit remediation.** Three missing platform features: `Http.Put()` / `PutBytes()` / `Delete()` / `DeleteBytes()` completing the REST verb set; range iteration `.rev()` + `.step(n)` for `for i in range` loops; 10 named `Color` constants (`RED` through `ORANGE`) as static properties. Plus 900+ documentation fixes across the Bible reference manual.
- **Fixes.** Static property resolution — the sema module-field handler now tries the `get_` prefix convention when resolving static properties through alias bindings (previously `Color.Red` errored with "Module has no exported symbol 'RED'"). P0 `BUG-EH-001`: Exception handler param types corrected (Ptr/I64 → Error/ResumeTok). P0 `BUG-OPT-001`: `Optional<String>` mapping corrected (Str → Ptr for nullable boxing). P1 `BUG-MATCH-002`: String match return type (I64 → I1). P1 `BUG-MATCH-003`: Boolean match with `Zext1` before `ICmpEq`. P1 `BUG-MATCH-001`: Negative literal patterns in match arms. P2 `BUG-VAL-001`: Value-type String field init/copy (raw ptr store + retain). P2 `BUG-OPT-002`: Nested-coalescing inner type derivation. P2 `BUG-IL-001`: Save/restore `IRBuilder` temp counter around lambda lowering. P2 `BUG-IL-002`: Type annotations for Dynamic result types in IL serializer; `call.indirect` parser/serializer support. Entity init overload resolution now matches parameter count + types against the call site (inherited init no longer fails "no init overload matching"). `override expose func` on class methods works for method dispatch. Deinit binding propagation — destructor blocks can access class fields through `self` bindings. Destructor dispatch lowering of `__dtor_TypeName` through inheritance chains. Guard-clause narrowing: assignment type checking uses original declared type; narrowing cleared on reassignment; force-unwrap allows reference/nullable after narrowing. `Optional<String>` maps to IL Str type (not Ptr) matching runtime externs. Match exhaustiveness checking (W019). Escape analysis in BasicAA (non-escaping allocas vs Param → NoAlias). Import deduplication in `ImportResolver`.
- **Language audit findings.** 6 confirmed fixed (enum variant values, string instance methods, async/await runtime, deinit destructor field bindings, BASIC `EXPORT` keyword, entity property getter/setter resolution). 7 open bugs filed as `ZIA-BUG-001` / `-003` / `-004` / `-005` / `-006` / `-007` + `BASIC-BUG-001`.

### BASIC frontend

- **Enums.** `ENUM...END ENUM` blocks with explicit + negative values, keyword-as-name collision handling. Variants lowered as I64 constants.
- **Fixes.** Runtime property setter calls on runtime classes (sema symbol lookup). Member-array field handling consolidated (4 bugs) into `MemberArrayResolver`. BASIC Lexer migrated to `LexerBase` CRTP (shared cursor management with Zia). `require*()` methods extracted to `lowerer/LowererRuntimeRequirements.hpp`.

### Runtime

- **Resource lifecycle (10 fixes).** GC epoch-counter overflow (`uint8` → `uint32` in `rt_gc.c`). String intern table dangling pointer on GC collect. TLS session cache stale pointer after context free. Parallel worker thread-local cleanup on join. `PkgDeflate` double-free on realloc failure. `ThreadsRuntime` detached-thread resource leak. File watcher handle leak on rewatch. Audio `waveOut` handle leak on close. Closure env pointer offset extracted to named constants with `static_assert`. GC shutdown flag guard for Windows re-init safety.
- **Capacity overflow guards (16 files).** Integer overflow guards before capacity doubling across seq / deque / map / set / bag / bimap / countmap / defaultmap / frozenmap / frozenset / intmap / multimap / sortedset / sparsearray / treemap / weakmap. Prevents undefined behavior when capacity approaches `INT64_MAX/2` or `SIZE_MAX/2`.
- **String operations (12 sites).** `strlen()` replaced with `rt_string_len_bytes()` / `rt_str_len()` in 5 case-conversion functions, 2 LIKE functions, box hash, bloomfilter, bimap/bag helpers, playlist operations.
- **File operations.** 32-bit `ftell` / `fseek` overflow on Windows → 64-bit equivalents (linereader, PNG loader). Delete corrupt PNG file on partial write failure. Check `fclose()` return in HTTP download; remove partial file on failure. Validate stream state after `Serializer::write`. Check write result in `ArWriter::finishToFile`.
- **Allocation failure handling.** Silent data-dropping replaced with `rt_trap` in `rt_list_push`, `rt_map_new`, `rt_map_set`, `rt_set_add`. Assert-only bounds checks in `rt_arr_str` / `rt_arr_obj` get/put replaced with `rt_trap` + `rt_arr_oob_panic`.
- **Type system (5 fixes).** Zia `lowerAs()` missing numeric conversion instructions. Assignment coercion for Number↔Integer fields and vars. Checked `fptosi` (`CastFpToSiRteChk`) with NaN/overflow guards. Separate `CastFpToSiRteChk` from `Fptosi` in lowering pipeline.
- **SipHash-2-4.** Replaces FNV-1a for all runtime hash maps. Uses per-process random seed from OS CSPRNG for HashDoS resistance.
- **Consistency audit (7 phases).** (1) 40+ C functions renamed to `rt_<type>_<verb>` convention across collections (List / Set / Map / Stack / Queue / Deque / Ring / Heap / Seq / Bag / Bytes), key renames `Contains`→`Has`, `Count`→`Length`, `Size`→`Length`, plus `IsEmpty` property additions and `TryPop` / `TryPeek` safe-access variants. (2) `runtime.def` aligned to C implementations with missing `RT_METHOD` / `RT_PROP` entries. (3) `Clone()` added to Stack / Queue / Set / Map; `First()` / `Last()` / `Reverse()` for Ring; `TryPopFront()` / `TryPopBack()` for Deque; legacy `Items()` names were later removed in favor of `ToSeq()`. (4) 12 `#define` constant groups converted to `typedef enum { ... } rt_xxx_t;`. (5) 7 existing enum types normalized to `typedef enum { ... } rt_xxx_t;` with `_t` suffix. (6) 15 boolean-returning functions moved from `bool`/`int` to `int8_t` (matching IL `i1`). (7) zannalib docs updated to cover all API additions from phases 1-3.
- **New APIs.** `AnimStateMachine` combined state machine + animation playback controller — maps each state to an animation clip (frame range, duration, loop flag); transitions auto-reconfigure the internal animation; surfaces `CurrentFrame`, `IsAnimFinished`, `Progress`, `JustEntered` / `JustExited` edge flags, `FramesInState`. `TextureAtlas` named-region 2D sprite-sheet atlas with grid-based auto-slicing (`LoadGrid`) and manual region definition (`Add`); integrates with SpriteBatch via `DrawAtlas` / `DrawAtlasScaled` / `DrawAtlasEx`. `Canvas.DeltaTime` + `Canvas.SetDTMax()` for frame-rate-independent physics. `ParticleEmitter.Draw()` / `DrawAt()` / `DrawToPixels()` direct rendering. `Camera.AddParallaxLayer()` / `RemoveLayer()` / `ClearLayers()` / `DrawParallax()` multi-layer parallax backgrounds. `Camera.SnapTo` for instant repositioning bypassing smooth-follow.
- **230+ bugs fixed** across runtime C sources in four batches: bounds checking, null guards, integer overflow, format-string safety, memory-leak prevention, error handling — graphics, network, collections, core. Notable: `TextCenteredScaled` swapped scale/color params (was drawing enormous near-black rectangles during level intro overlays); canvas coordinate overflow guards; pixel-bounds validation; bitmap-font character-range checks; GUI widget input handling; navmesh boundary validation; FBX loader robustness; scene-graph cleanup; keychord + action unboxing; color-hex alpha parsing; circle-fill algorithm in `rt_drawing.c`; canvas coordinate guard in `rt_canvas.c`.

### Network & TLS

- **Security fixes (8 findings).** Native ECDSA P-256 verification for Linux TLS `CertificateVerify` (no OpenSSL dependency). WebSocket fragmentation reassembly capped at 64 MB with RFC 6455 close code 1009. TLS transcript buffer increased 8 KB → 32 KB for large cert chains. MSVC-compatible 128-bit arithmetic for ECDSA P-256 (`_addcarry_u64`, `_subborrow_u64`, `_umul128` replacing `__uint128_t`). TOML parser nesting depth limit (`kMaxTomlDepth=128`). BASIC parser `peek()` distance limit (`kMaxPeekDistance=1000`). Volatile memset for crypto key material wiping (HKDF, TLS). `atoi()` replaced with `strtol()` + range validation in WebSocket port parsing.
- **Network audit (14,427-line review).** AES-128-GCM cipher suite implementation (~300 LOC) for TLS dual cipher support. ChaCha20 counter overflow protection + Poly1305 key zeroing. TLS buffered-data check for WebSocket + HTTP keep-alive. Windows socket type fix in TLS layer. Thread-safe CA certificate DER cache with heap-allocated PEM base64. HTTP HEAD response Content-Length fix. 303 redirect method rewrite. URL parsing memory leak fixes (`key_str` unref, parse validation). `recv_line` length cap to prevent unbounded reads. `INT_MAX` socket send clamp.
- **10 new network classes.** `HttpRouter` (path pattern matching + parameter extraction); `HttpServer` (multi-client event-loop); `ConnectionPool` (reuse with idle timeout + health checks); `MultipartParser` (multipart/form-data streaming); `NetUtils` (DNS resolution, interface enumeration, port checking); `WebSocketServer` (multi-client + broadcast); `SSEClient` (Server-Sent Events with auto-reconnect); `HttpClient` (high-level with cookie jar + redirects); `SmtpClient` (SMTP with AUTH + TLS); `AsyncSocket` (non-blocking with completion callbacks).
- **Crypto additions.** HMAC-SHA256 for message authentication. HKDF (HMAC-based Key Derivation Function) for key expansion.
- **Fixes.** Cipher `rt_cipher_decrypt` didn't fall back to legacy HKDF when PBKDF2-derived key failed.

### Concurrency (TSan-verified)

- **Pool allocator.** `block->next` pointer uses atomic load/store to prevent ARM64 memory-ordering corruption in the lock-free freelist.
- **SipHash.** Non-atomic `rt_siphash_seeded_` fast-path check was causing stale seed reads on ARM64 weak memory model.
- **GC.** Non-atomic `g_shutdown_registered` could double-register `atexit`. TOCTOU race removed by eliminating the unnecessary unlock/relock gap in snapshot.
- **Init races.** `PTHREAD_MUTEX_INITIALIZER` / `InitOnceExecuteOnce` for all global init paths.
- **ARM64 barriers.** `__dmb(_ARM64_BARRIER_ISH)` in `rt_platform.h` + `rt_pool.c`.

### Graphics & GUI

- **Graphics fixes.** Present-before-events: reordered `vgfx_update` to present the frame before polling events (fixes visual glitches on rapid input during scene transitions). macOS resize alpha: opaque alpha (0xFF) on framebuffer clear during window resize (prevents transparent flicker artifacts). Linux X11: 32-bit TrueColor visual via `XMatchVisualInfo`; RGBA→BGRA swizzle in presentation buffer for correct color rendering. Linux linking: `-lX11` for graphics demos. macOS: frameworks guarded behind `#if defined(__APPLE__)` in AArch64 backend.
- **GUI.** ListBox multi-select, search/filter with keyboard, scroll-to-selection. TextInput undo/redo stack, text selection with shift+arrows, clipboard integration. Improved dock layout edge cases, floating panel cleanup. Refined widget focus + keyboard event dispatch.

### Game engine framework

Built as pure Zia libraries and C runtime additions:

- **GameBase + IScene** (Zia library). Reusable game-loop framework — `GameBase` handles canvas creation, frame pacing, DeltaTime clamping, scene management; `IScene` defines `update` / `draw` / `onEnter` / `onExit` lifecycle. Eliminates ~100 lines of boilerplate per game.
- **Screen transitions.** `transitionTo()` orchestrates fade-out → scene switch → fade-in via ScreenFX. `shake()` and `flash()` for screen effects.
- **Action presets (C runtime).** `Action.LoadPreset("platformer")` loads standard input bindings in one call: `standard_movement`, `menu_navigation`, `platformer`, `topdown`.
- **Canvas frame helpers.** `BeginFrame()` combines Poll + ShouldClose check; `SetDTMax()` auto-clamps DeltaTime; `TextCentered` / `TextRight` / `TextCenteredScaled` layout helpers.
- **SaveData.** Cross-platform key-value persistence. JSON storage in platform-appropriate directories (macOS Application Support, Linux XDG, Windows AppData).
- **DebugOverlay.** Real-time FPS/dt/watch-variable overlay with color-coded FPS, 16-frame rolling average, up to 16 custom watch entries.
- **SoundBank + Synth.** Named sound registry + procedural synth generating tones / sweeps / noise / 6 preset game SFX (jump / coin / hit / explosion / powerup / laser) without WAV files. Bhaskara I sine approximation, in-memory WAV generation.

### Tests

Roughly +90 net tests (18K lines of new coverage). Highlights:

- **Frontends.** 25 Zia parser-error tests, 62 Zia lexer tests, 75 Zia frontend tests (35 parser + 24 sema + 16 lowerer), 24 Zia/BASIC enum tests, 6 Zia runtime programs (enums, async, properties, deinit, string methods, optional narrowing), 3 async tests, 3 destructor tests, 2 static-property tests, 2 string-method tests.
- **Native toolchain.** 13 assembler, 6 string-dedup, 13 symbol-resolver, 8 relocation edge-case, 11 DWARF debug-line, 9 DWARF debug-section, 1 ICF, 1 branch-trampoline, 10 ELF exe-writer, 7 linker integration.
- **Codegen.** 357 x86-64 determinism tests (8 scenarios, 407 compilations verifying byte-identical output — repeated compilation N=100, regalloc pressure N=50, rodata pool N=50, multi-function ordering N=50, complex CFG N=50, separate construction pointer-address independence, ISel patterns N=50, static counter awareness). 9 dataflow-liveness tests, 1 encoding-validation, 1 x86-64 peephole integration, 1 DCE param-compaction, 1 alias-precision, 1 inline round-trip, 3 canonical-pipeline.
- **Runtime.** 86 REPL (51 Zia + 35 BASIC), 24 rt_map, 11 rt_pool, 5 VM equivalence, 2 crypto (HMAC-SHA256 + HKDF), 3 Pixels, 2 Canvas3D, 2 SceneGraph, 1 network integration, 7 AnimStateMachine, 3 TextureAtlas.
- **Fuzz.** 2 libFuzzer harnesses for Zia lexer + parser.

### Demos & docs

Marquee demo is the sidescroller "Nova Run" — evolved across 8 phases from a single-level tech demo into a complete 5-level platformer (Training Grounds / Crystal Caverns / Volcanic Depths / Sky Fortress / Ancient Ruins) with 5 parallax backgrounds, 7 new tile types with 3D bevels + crystal facets + lava veins, all entities redrawn using `DrawTriangle` / `DrawEllipse` / `DrawBezier` / `DrawThickLine` / `Blur` / `Tint`, 4-frame player run cycle, frame-rate-independent DeltaTime physics, gamepad support via `Action.BindPadButton`, persistent CSV high scores, 12 procedurally-generated WAV SFX, `ParticleEmitter`-based projectile trails, landing dust + wall-slide sparks + jetpack flame + ambient lava glow + crystal shimmer + speed trail + level intro splashes. Plus a new Platformer Showcase Demo (~600 LOC across 6 files demonstrating 11+ runtime APIs in one cohesive game) and a new 3D Bowling demo (Physics3D showcase). Docs: all demos consolidated under `examples/` (`apps/`, `games/`, `embedding/`, `apiaudit/`, `basic/`, `il/`, `sqldb-basic/`), ~80 trivial/redundant demo files removed, 5 directories deleted; `devdocs/` merged into `docs/` with 28 files relocated, 25 stale files deleted, 117 broken relative links fixed, `docs/README.md` rebuilt as a clean navigation hub, YAML frontmatter added to 95 files, 548 bare code blocks tagged with language identifiers, terminology standardized ("standard library" → "ZannaLib", "interpreter" → "VM"), 900+ Bible reference-manual fixes. Large-file splits for readability: `BytecodeVM.cpp` + `BytecodeVM_threaded.cpp`; `rt_graphics.c` + `rt_canvas.c` + `rt_drawing.c` + `rt_drawing_advanced.c`; `rt_network_http.c` + `rt_http_url.c`; `rt_tls.c` + `rt_tls_verify.c` + `rt_tls_internal.h`; `vg_ide_widgets.h` split into 6 focused sub-headers; `Peephole.cpp` decompositions (both backends); `RegAllocLinear.cpp` → 8 files in `ra/`; `Lowerer.hpp` foundation decomposition. Zia frontend file splits: `Parser_Expr.cpp` (1,804 LOC → 3 files), `Lowerer_Decl.cpp` (1,690 → 3), `Lowerer_Expr_Complex.cpp` (1,386 → 3). Sidescroller demo fixes: lock-free pool allocator race (reserve first slab block before sharing with freelist); `TILE_BRIDGE` missing from `isSolid()` (players fell through bridges into spike pits); `PS_HURT` state stuck permanently; turret/boss shared `etimer` corruption leaving enemies stuck in hurt state; clear nearby enemy bullets on enemy death (4-tile radius despawn).

### Breaking changes

1. **Directory restructure:** `demos/` consolidated into `examples/`. Update any hardcoded paths.
2. **`devdocs/` removed:** all developer documentation now lives under `docs/`.
3. **Runtime API renames:** 40+ C functions renamed to `rt_<type>_<verb>` convention (`Contains`→`Has`, `Count`→`Length`, `Size`→`Length`, plus `IsEmpty` property additions). `runtime.def` registrations updated accordingly.
4. **Collection `.Len` → `.Length`** across List / Map / Set. `.Len` retained as alias for back-compat.
5. **Boolean return types:** 15 runtime functions changed from `bool`/`int` to `int8_t` (matching IL `i1`). Affects C FFI callers using these functions directly.

---

### Commits

See `git log <v0.2.2-anchor>..HEAD -- .` for the full 100-commit history. Commits pair feature-add and follow-up hardening in the same subsystem (e.g. native linker → ICF + branch trampolines + two-level namespace; AArch64 scheduler → coalescer + loop hoist + strength reduction; network class set → 14K-line audit + crypto additions; Sidescroller 8-phase evolution → ParticleEmitter + SoundManager + state machine refactor).
