---
status: active
audience: contributors
last-verified: 2026-07-26
---

# IL Optimization Passes

## Analysis Caching & Instrumentation

- `AnalysisManager` caches module/function analyses and drops entries after each pass based on `PreservedAnalyses`.
  Module passes must mark any preserved function analyses; otherwise function caches are cleared alongside module
  caches.
- Convenience helpers exist for common function analyses: `preserveBasicAA()`, `preserveCFG()`, `preserveDominators()`,
  `preserveLiveness()`, and `preserveLoopInfo()`.
- Enable per-pass statistics with `PassManager::setReportPassStatistics(true)` plus `setInstrumentationStream(...)` to
  receive lines like `[pass licm] bb 6 -> 6, inst 42 -> 40, analyses M:0 F:2, time 1500us`.
- Statistics track IR size (basic blocks and instructions), analysis recomputations, and wall-clock duration per pass to
  highlight redundant work. When per-pass verification is enabled, statistics report verifier time separately from the
  transform time.
- The `zanna il-opt` CLI exposes the same instrumentation with `--pass-stats`. Use `--bisect-pipeline` with a named
  pipeline to run each prefix on a fresh copy of the input and print the resulting block/instruction count; this is the
  fastest way to isolate verifier failures introduced by a specific optimization pass.

## Post-Dominator Tree Analysis (`"post-dominators"`)

- Computes the **post-dominator tree** for each function: block X post-dominates block Y iff every path from Y to
  any function exit passes through X.
- **Algorithm**: Cooper–Harvey–Kennedy iterative dataflow on the reversed CFG. Exit blocks (those with no CFG
  successors) are the roots; a virtual exit node represented by `nullptr` connects them all.
- **Registration**: Available as analysis `"post-dominators"` in `PassManager` — query via
  `analysis.getFunctionResult<PostDomTree>("post-dominators", fn)`.
- **API**: `postDominates(A, B)` — true iff A is an ancestor of B in the post-dominator tree; `immediatePostDominator(B)` —
  immediate parent in the tree (`nullptr` for exit blocks, indicating the virtual exit).
- **Multiple exits**: When a function has two paths to separate exit blocks with no common block, the entry's
  immediate post-dominator is `nullptr` (virtual exit) — no concrete block post-dominates the entry.
- **Implementation**: `src/il/analysis/Dominators.hpp/.cpp`
- **Tests**: `src/tests/analysis/PostDominatorsTests.cpp` — linear chain, diamond (if/else join), and
  multiple-exit cases.

## BasicAA (Alias/ModRef)

- Classifies pointers by base object: allocas vs parameters (including `noalias`), globals (via `AddrOf`/`GAddr`
  opcodes), const strings, and null.
- Follows constant-offset `gep` chains to build base+offset summaries; compares offsets with access sizes to prove
  disjoint struct/array fields when both sides have known widths.
- Distinguishes address spaces: stack vs global are `NoAlias`; different globals are `NoAlias`; null aliases only
  null. `noalias` parameters are disambiguated from other pointer parameters, but not from arbitrary globals or stack
  slots.
- `typeSizeBytes` exposes conservative byte widths (i1/i16/i32/i64/f64/ptr/str/resume_tok/error) for passes to thread
  sizes into `alias(...)`.
- ModRef uses a priority cascade for call effect classification:
  1. Module-level attributes on locally defined functions are authoritative after verifier validation
  2. Runtime signature table is authoritative for known external helpers
  3. Import and extern attributes are used for declarations without runtime metadata
  4. Unknown callees remain conservative; raw instruction-level `CallAttr` flags are not trusted without verified
     runtime, function, import, or extern metadata
  `pure` -> `NoModRef`, `readonly` -> `Ref`, everything else -> `ModRef`.
- Runtime signature lookup uses the generated runtime table directly for known helpers and treats unknown callees as
  `ModRef`.
- Runtime signature metadata also records string/reference ownership effects and which parameters are runtime `obj`
  slots. Helpers such as `rt_str_concat` consume their string arguments and return an owned result; retain/release
  helpers are modeled explicitly so optimizers do not reorder or eliminate calls across ownership boundaries.
- `const_str` materializes an owned runtime string handle. It is modeled as a memory read with side effects, so
  optimizers can distinguish it from writes while still avoiding hoists or merges that would duplicate ownership.

## SimplifyCFG

The **SimplifyCFG** pass tidies the control-flow graph before and after SSA promotion:

- Canonicalizes block parameters and the arguments supplied by branches.
- Eliminates forwarding blocks that merely branch to their successor, but only when any ignored instructions are proven
  non-trapping as well as side-effect-free.
- Folds trivial `cbr` instructions when their condition is constant or both edges converge.
- Merges blocks that have a single predecessor with their unique successor when it preserves semantics.
- Prunes blocks that have become unreachable.
- Reachability treats `eh.push` handler labels as CFG edges so landing pads referenced only through EH metadata are not
  pruned.

### Safety Notes

SimplifyCFG deliberately skips exception-handling sensitive blocks so that landing pads and dispatch regions keep their
required structure.

### Execution Order

Canonical pipelines run this pass early and again after major simplification points. The `rehab-mem2reg` pipeline also
places `SimplifyCFG` around **Mem2Reg** so SSA-promotion edge arguments can be validated in isolation before cleanup.

## Mem2Reg

Promotes alloca/store/load patterns to pure SSA values:

- **Non-entry alloca promotion**: Promotion is not restricted to entry-block allocas. Any alloca whose
  defining block dominates all of its load/store uses is eligible for promotion. This eliminates the common pattern
  of loop variables allocated in non-entry blocks staying as load/store overhead.
- **Address-taken conservatism**: Allocas forwarded directly as branch arguments are treated as address-taken so
  promotion does not erase pointer escape information.
- **SROA (Scalar Replacement of Aggregates)**: Splits struct/record allocas into per-field allocas before promotion,
  allowing fields accessed independently to be promoted individually.
- **Sealed SSA construction**: Uses the sealed-block SSA algorithm, with deterministic alloca promotion order and sorted
  completion of incomplete block parameters.
- **Edge repair**: After promotion, mem2reg repairs branch arguments for every block parameter it introduced so partially
  populated edge argument vectors do not escape into the verifier or serializer. A literal `null` branch argument is
  preserved as data rather than treated as an unfilled repair slot.
- **Duplicate-edge safety**: CFG successor/predecessor caches preserve parallel edges, and mem2reg writes promoted
  values to the exact predecessor edge index. This matters for `cbr`/`switch` sites that target the same block more than
  once with different branch-argument bundles.
- **Lazy dominator tree**: The dominator tree is computed only when non-entry allocas with cross-block uses are
  encountered, keeping the common (entry-block-only) case fast.
- **Batched use rewriting**: Load replacements are collected and applied in one function-local rewrite pass instead of
  repeatedly walking the IR for each promoted load.
- **Function parallelism**: Independent functions are promoted concurrently in the module pass; aggregate promotion
  statistics remain deterministic.
- **Shared CFG context**: The module CFG is built once for the promotion pass and reused across functions. SROA rewrites
  memory instructions but does not alter control-flow edges, so the cached CFG remains valid while avoiding repeated
  whole-module CFG construction.
- **EH conservatism**: Functions containing structured exception-handling opcodes are skipped until mem2reg has an
  exception-aware CFG. This preserves handler/resume semantics while still allowing the rest of O1/O2 to run.
- **Tests**: `test_il_mem2reg_nonentry` — single-block alloca in non-entry block, dominating non-entry alloca with
  conditional CFG, default value before first store, deterministic loop-header parameter/edge repair, and EH skip
  coverage.

## Inline

- Enhanced cost model considers multiple factors beyond raw instruction count:
  - Base instruction/block budgets (defaults: `instrThreshold = 80` instructions, `blockBudget = 1` block,
    `maxInlineDepth = 3`). The default `inline` pass remains single-block for O1 stability.
  - The O2 pipeline uses `inline-o2`, which raises the instruction threshold to 120, permits up to four blocks, caps
    module growth at 4000 instructions, and requires multi-block callees to have a single return continuation.
  - Constant argument bonus: each constant arg reduces effective cost, enabling more inlining when optimization
    opportunities exist
  - Nested call penalty: functions with many internal calls incur code growth penalty
  - Single-use function bonus: functions called only once get priority (can be DCE'd after)
  - Tiny function bonus: very small functions (≤8 instructions) inline more aggressively
  - Total code growth tracking: limits module-wide instruction expansion
- Inline depth capped at 3 to allow multi-level utility-function chains to collapse; skips EH-sensitive opcodes and
  recursive calls.
- `inline-o2` runs to a bounded fixpoint so nested helper chains exposed by earlier inlining can collapse in one O2
  pipeline invocation.
- Callee entry-block parameters are mapped to call operands by exact id when possible and by position/type otherwise.
  This supports textual IL where the function prototype and entry block use different SSA names for the same ABI
  parameters.
- Rewrites calls by cloning the callee CFG, threading branch arguments for block parameters, and branching returns to a
  continuation block at the call site.

## Benchmark Regression Coverage

- `test_il_backend_benchmark_regressions` checks the two benchmark-discovered O2 hazards in pure IL:
  owned string work must stay inside consuming loops unless it is folded into an equivalent owned literal, checked
  unsigned division/remainder by a power of two must reduce to `lshr`/`and`, signed power-of-two remainder must avoid
  `srem`, and O2 inlining must handle prototype/entry parameter name mismatches.
- `test_codegen_arm64_benchmark_regressions` runs reduced native ARM64 versions of the same cases under `-O2` on ARM64
  hosts. The string test returns the accumulated concatenated length; the unsigned-division loop returns a fixed checksum.

## Real-World Runtime Fast Paths

The O2 pipeline includes four targeted passes that reduce runtime-helper overhead exposed by real applications:

- `devirt` rewrites `call.indirect` through a constant `gaddr` function pointer into a direct `call`. This gives inline,
  SCCP, GVN, and backend call lowering a named callee instead of an opaque indirect call.
- `runtime-fastpath` rewrites generic object retain/release helpers to `rt_obj_retain_known` and
  `rt_obj_release_known_check0` when the object value is proven to come from a runtime object allocation helper. The
  known helpers skip string-handle/raw-pointer discrimination while preserving NULL-safe retain/release semantics.
- `array-fastpath` rewrites numeric `rt_arr_i32/i64/f64_get` and `_set` calls to separate `_fast` ABI helpers when an
  `idx.chk` or equivalent single-predecessor bounds branch dominates the access. The checked helpers remain the default
  for O0 and unproven accesses. The bytecode backend recognizes these `_fast` helpers and emits direct array load/store
  opcodes for i32, i64, and f64 arrays.
- `ownership-opt` removes local retain/release pairs for strings and numeric arrays when only pure, non-owning
  instructions sit between them. Calls with ownership effects or memory writes block the rewrite.

These passes run after O2 inlining/check simplification and before the final peephole/GVN/DSE cleanup window so their
rewrites expose direct calls, fewer runtime branches, and less reference-counting traffic to later optimizers.

### Thresholds

Defaults live in `InlineCostConfig` (`src/il/transform/Inline.hpp`); `inline-o2` overrides three of them.

| Parameter | `inline` (O1) | `inline-o2` (O2) | Rationale |
|-----------|---------------|------------------|-----------|
| `instrThreshold` | 80 | 120 | Captures medium-sized helpers without unbounded growth |
| `blockBudget` | 1 | 4 | O1 stays single-block; a larger O1 budget regressed zannastudio and chess |
| `maxInlineDepth` | 3 | 3 | Deep enough for multi-level utility-function chains |
| `maxCodeGrowth` | 2000 | 4000 | Bounds total module expansion |

## LateCleanup

- End-of-pipeline polish pass: runs up to 4 iterations of `SimplifyCFG` (aggressive) followed by DCE, stopping early
  once no CFG or size changes are observed.
- DCE deletes dead loads, stores, and allocas only after proving the removed memory operation cannot trap; potentially
  null, out-of-bounds, or dynamically sized allocations are preserved even when their result is unused.
- Tracks IL size (blocks/instructions) before/after each iteration via an optional `LateCleanupStats` sink so pipelines
  can see how much cleanup occurred.
- Keeps work bounded; if nothing changes on an iteration, the pass exits without further scans.

## Parallel Function Passes

- `PassManager::enableParallelFunctionPasses(true)` is the default for audited function-local passes. Call it with
  `false` when debugging a pass that has not been marked parallel-safe.
- Module passes remain sequential unless they implement their own internal function parallelism, as `mem2reg` and SCCP
  do for large modules.
- Analysis caching uses coarse locking inside `AnalysisManager` to keep computations and invalidations thread-safe.
- Pass-level instrumentation is emitted in pipeline order. Function scheduling inside a parallel pass is intentionally
  unspecified, but the resulting IR should remain deterministic.

## Differential Testing

- `src/tests/unit/test_il_opt_equivalence.cpp` randomly builds small IL modules (ints/floats with `br`, `cbr`, `switch.i32`)
  using `IRBuilder`. The generator constrains constants to avoid UB (no unchecked divides; `idx.chk` uses in-range
  operands).
- Each generated module is verified, cloned, and executed on the VM before and after `O0`, `O1`, and `O2` pipelines via
  `il::transform::PassManager`. Return values and trap outcomes must match.
- Execution runs in a forked child so unexpected traps or aborts do not bring down the test harness; discrepancies print
  the seed plus the IL text for repro.
- Seeds default to a fixed constant for stability; override with `ZANNA_OPT_EQ_SEED=<u64>` to fuzz locally.

## Peephole

The peephole pass applies 61 pattern-based algebraic simplifications plus a small set of procedural rewrites:

- **Bitwise identities**: `x & -1 = x`, `x | 0 = x`, `x ^ 0 = x`, `x & 0 = 0`, `x ^ x = 0`, `x | x = x`
- **Division identities** (on div-by-zero–checked variants `SDivChk0`, `UDivChk0`, `SRemChk0`, `URemChk0`):
  `x / 1 = x`, `x % 1 = 0`
- **Intentionally skipped**: `0 / x` and `0 % x` for checked division/remainder, because the divisor must
  still be evaluated for divide-by-zero traps; `x + 0.0`, because IEEE signed-zero semantics can make the
  replacement observable.
- **Float arithmetic identities**: `x * 1.0 = x`, `x / 1.0 = x`, `x - 0.0 = x`
- **Integer arithmetic identities** (on overflow-checked variants `IAddOvf`, `ISubOvf`, `IMulOvf`): `x + 0 = x`,
  `x * 1 = x`, `x - 0 = x`, `x * 0 = 0`, `x - x = 0`
- **Reflexive comparisons**: `x == x = true`, `x < x = false`, etc. for integer, signed, and unsigned comparisons
  (ICmpEq/Ne, SCmpLT/LE/GT/GE, UCmpLT/LE/GT/GE). Float reflexive comparisons are intentionally skipped because NaN
  makes `fcmp.* %x, %x` non-reflexive under IEEE-754 semantics.
- **Shift identities**: `x << 0 = x`, `x >> 0 = x`, `0 << y = 0`, `0 >> y = 0`
- **Unsigned power-of-two division/remainder**: `udiv`/`urem` by constant powers of two become `lshr`/`and`.
- **Signed power-of-two division/remainder**: `sdiv`/`srem` by positive powers of two expand to sign-bias shift/mask
  sequences so optimized IL can avoid native signed division while preserving truncate-toward-zero semantics.
- **Owned literal concat folding**: when `rt_str_concat` or `Zanna.String.Concat` consumes two `const_str` results that
  have no other uses in the block, the pass replaces the whole sequence with one owned `const_str` for the combined
  literal and removes the consumed literal materializations.

The pass also simplifies CBr terminators when the branch condition is a comparison of two constants:
- Float constant comparisons fold the branch to an unconditional jump
- Integer constant comparisons (signed and unsigned) fold the branch to an unconditional jump

The pass is table-driven (`kRules` in `src/il/transform/Peephole.hpp`), so new rules do not require changes to the core
logic. Operand-forwarding rewrites are kept within the defining block after the definition, so the replacement value is
available at every rewritten use. Floating constant identity matches compare signed zero exactly, so `fsub x, +0.0 -> x`
is allowed while `fsub x, -0.0` is left intact. Full `peephole` is part of the canonical O1/O2 pipelines.

## ConstFold

Constant folding evaluates pure operations at compile time:

- **Checked integer arithmetic**: `iadd.ovf`, `isub.ovf`, `imul.ovf`, `sdiv.chk0`, `udiv.chk0`, `srem.chk0`,
  `urem.chk0` (folded when both operands are constants and the result does not trap)
- **Floating arithmetic**: `fadd`, `fsub`, `fmul`, `fdiv`
- **Bitwise**: `and`, `or`, `xor`, `shl`, `ashr`, `lshr`
- **Comparisons**: all signed, unsigned, and float comparison opcodes
- **Intrinsics**: `abs`, `ceil`, `clamp`, `cos`, `exp`, `floor`, `log`, `max`, `min`, `pow`, `sgn`, `sin`, `sqrt`, `tan`
- **Type conversions**: int/float casts with constant operands

`rt_sgn_f64(NaN)` folds to `NaN`, preserving the runtime's IEEE behavior instead of collapsing unordered input to zero.

## SCCP (Sparse Conditional Constant Propagation)

Propagates constants through the IL using sparse conditional evaluation:

- Identifies executable regions of the CFG and evaluates instructions whose operands become constant
- Folds conditional branches with known conditions; rewrites block parameters (SSA phi nodes)
- Floating arithmetic propagates the IL's defined IEEE-754 infinities, NaNs, and signed zeros
- Trapping operations (SDivChk0, UDivChk0) are never folded when the divisor is zero to preserve trap semantics

## DSE (Dead Store Elimination)

Three-level dead store elimination:

1. **Intra-block DSE** (`runDSE`): Backward scan within each basic block finds stores that are overwritten before being
   read. Uses BasicAA for alias disambiguation and is conservative about calls that may modify or reference memory.
   Uses `size_t` loop counters for safe unsigned backward iteration. A store is removed only when the target pointer is
   proven dereferenceable, so DSE does not erase a possible null or bounds trap.

2. **Cross-block DSE** (`runCrossBlockDSE`): Recursive path analysis identifies stores to non-escaping allocas that are
   provably dead because they are overwritten on all paths before being read. Uses escape analysis for safety, treats
   unknown cycles as live, and skips functions that still contain structured EH opcodes.
   **Limitation**: Conservatively treats any `ModRef` call as a read barrier, even for non-escaping allocas.
   Escape analysis follows `gep` and block-parameter forwarding so branch arguments cannot hide captured stack slots.

3. **MemorySSA-based DSE** (`runMemorySSADSE`): Uses the `MemorySSA` analysis (see below) to find additional dead
   stores that the BFS misses. Key precision improvement: calls are **not** treated as read barriers for non-escaping
   allocas because non-escaping stack memory is inaccessible to external calls. This eliminates stores in functions
   that call runtime helpers between writes — the dominant pattern in Zia-lowered loops.
   The non-escaping set follows `gep` chains and branch-argument propagation before treating calls as transparent.
   Same-block loads are linked to the nearest aliasing store, so independent stack slots do not share a single coarse
   reaching def. EH functions are skipped until MemorySSA models exceptional successors.
   - Registered as `"memory-ssa"` function analysis in `PassManager`
   - Runs after `runDSE` within the `"dse"` pipeline pass
   - Tests: `src/tests/analysis/MemorySSATests.cpp` — call-barrier precision, live-store preservation,
     simple cross-block elimination, escaping alloca correctness, def/use node assignment

## LoopUnroll

Fully unrolls small constant-bound loops to reduce iteration overhead:

- Identifies counted loops with known trip counts from comparison patterns
- Configurable thresholds: max trip count (default 8), max loop size (50 instructions)
- Handles single-latch, single-exit loops with proper SSA value threading
- Clones only non-trapping, side-effect-free loop bodies with value renaming through unrolled iterations
- Removes original loop blocks after unrolling

## JumpThreading (in SimplifyCFG)

Threads jumps through blocks with predictable branch conditions:

- When a predecessor passes a constant value that determines a conditional branch outcome, redirects the predecessor
  to bypass the intermediate block and jump directly to the known successor
- Eliminates unnecessary control flow and can enable further simplifications
- Runs in aggressive mode alongside other SimplifyCFG transformations
- Duplicate-edge aware: when one predecessor has multiple edges to the same intermediate block, threading reads the
  branch arguments from the specific edge being rewritten instead of the first matching label.
- Null-safe: target block lookups guard against missing labels to avoid crashes on malformed CFG

## CheckOpt

- Removes redundant safety checks (bounds, div/rem-by-zero, narrowing casts) when a dominating equivalent check already
  executed on all incoming paths. Dominance is tracked with a scoped map so sibling blocks never incorrectly share
  availability.
- **Constant-operand elimination (SCCP integration)**: When SCCP's `rewriteConstants()` has inlined operands as literal
  `ConstInt` values before CheckOpt runs, the pass evaluates check conditions statically at compile time:
  - `IdxChk(index, lo, hi)` — eliminated when all three are ConstInt and `lo <= index < hi`
  - `SDivChk0/UDivChk0/SRemChk0/URemChk0(lhs, divisor)` — demoted to the corresponding plain div/rem opcode when the
    divisor is a non-zero ConstInt and the signed `MIN / -1` trap case is impossible. `SRemChk0 x, -1` also requires
    proof that `x` is not `INT64_MIN`, because the plain signed-remainder opcode may lower through a trapping native
    divide path. The result temp is preserved.
  - **Type safety**: ConstInt values type as I64 in the verifier. When the check result type is narrower (e.g. I32 for
    IdxChk) and the result has live uses, constant elimination is skipped and the dominance-based check still applies.
    This prevents type-mismatch verifier errors when the check result is used as a narrower-typed discriminant.
- Guard-based `isub.ovf x, K` demotion requires a proof that covers the relevant overflow side. A lower-bound guard can
  demote non-negative `K`; negative constants require an upper-bound proof and remain checked. The guarded target must be
  reached only by the proving predecessor when the rewrite runs, so later CFG passes do not inherit a one-edge proof as
  an all-path fact.
- Range-backed overflow demotion uses comparison-edge and branch-argument facts to demote
  `iadd.ovf`/`isub.ovf`/`imul.ovf` only when the verifier can independently reconstruct the same proof. This primarily
  targets rotated counted-loop increments and arithmetic exposed by inlining/peephole rewrites.
- Signed power-of-two division expansions use a sign-bias add. CheckOpt recognizes
  `x + ((x >> 63) & mask)` as overflow-safe and demotes that `iadd.ovf` to `add`; the verifier accepts only that exact
  dominating local pattern.
- The verifier still rejects plain unchecked signed arithmetic by default. It accepts demoted `add`/`sub`/`mul` only
  when its local range reconstruction proves the operation cannot overflow; unguarded plain arithmetic remains invalid
  frontend IL.
- Hoists loop-invariant checks from loop headers to preheaders when the loop is EH-insensitive and operands are
  invariant, preserving trap behaviour.
- Safety rules: a check is eliminated only if the dominating check block dominates the use-site block; hoisting is
  restricted to loop headers with canonical preheaders. EH-sensitive opcodes (resume/eh push/pop) keep loops ineligible.
- Tests: `src/tests/unit/il/transform/checkopt_redundancy.cpp` covers nested-loop redundancies, non-dominating siblings (no
  elimination), constant in-bounds elimination, checked div/rem demotion, and trap-preservation cases.

## EarlyCSE

Dominator-tree-scoped common subexpression elimination:

- **Scope model**: Maintains a stack of expression hash tables (one per domtree level) during a pre-order DFS
  over the dominator tree. An expression computed in block A is visible in every block A dominates because A's
  table remains on the stack while its dominated subtree is processed.
- **Cross-block CSE**: Redundant pure expressions in dominated successors are eliminated — not just within the same
  block. For example, `add a, b` in an entry block eliminates a commuted `add b, a` in its only successor.
- **Sibling isolation**: When the DFS leaves a block, its table is popped, so expressions in one branch of a
  conditional cannot eliminate the same expression in a sibling branch (which would be incorrect).
- **Commutative normalization**: Uses the same `ValueKey` canonical form as GVN so `add a,b` and `add b,a`
  share a single table entry.
- **Pure ops only**: Only instructions passing `makeValueKey` are eligible (no memory ops, calls, or terminators).
- **Textual ordering guard**: Each available expression is stored as `AvailableExpr{value, block}`. Before
  replacing, `isTextuallyAvailable()` verifies the defining block appears at or before the use block in the
  function's block list. This prevents replacements where a dominator is textually later (e.g., entry→late→update
  where late dominates update but appears after it), which would create an illegal use-before-def in the IL.
- **Signature**: `bool runEarlyCSE(Module &M, Function &F)` — module reference needed for CFGContext construction.
- **Tests**: `test_il_earlycse_domtree` — cross-block elimination, sibling-branch non-elimination, textually-unsafe cross-block CSE rejection.

## ValueKey (CSE/GVN Support)

Expression identity keys used by EarlyCSE and GVN:

- Commutative operations with well-defined value semantics (checked integer overflow ops, And, Or, Xor, ICmpEq/Ne,
  FAdd, FMul, FCmpEQ/NE) have operands normalized by a deterministic ranking function. Plain signed Add/Sub/Mul are
  excluded because overflow policy is not encoded in the key.
- Ranking caches computed values to avoid redundant tuple construction
- `isSafeCSEOpcode` whitelist restricts CSE to pure, non-trapping operations with no memory effects
- `makeValueKey` rejects terminators, side-effecting, and memory operations

## CallEffects

Unified API for querying call instruction side effects:

- Runtime helper metadata overrides call-site hints for known helpers; the verifier rejects contradictory call
  attributes on known callees.
- Function, import, and extern effect attributes feed the same classifier used by BasicAA, DCE, and LICM. This keeps
  locally defined functions and foreign declarations consistent instead of requiring each pass to duplicate lookup
  rules.
- Instruction-level `CallAttr` flags are metadata assertions, not an optimizer proof by themselves; unknown callees stay
  conservative.
- `canEliminateIfUnused()` requires both `pure` and `nothrow`; `canReorderWithMemory()` gates LICM and code motion.

## IndVarSimplify

Induction variable optimization and strength reduction:

- Null-safe: guards against deleted instructions when looking up address expressions

## LoopInfo

Loop detection and nesting analysis:

- blockLabels vector contains no duplicates (important for self-loops where latch == header)
- Members hash set provides O(1) `contains()` queries
- Latch labels tracked separately from body membership

## CallGraph / SCC

Direct-call graph with strongly connected component analysis:

- **Edge building**: Scans all functions for `Call` instructions with non-empty `callee` field; records
  caller→callee edges and per-callee call counts. Indirect calls and unresolved targets are ignored.
- **SCC computation**: Tarjan's iterative SCC algorithm runs after edge collection to
  identify mutually recursive function groups. SCCs are stored in reverse topological order (callees before
  callers) in `CallGraph::sccs`. Each function name maps to its SCC index via `CallGraph::sccIndex`.
- **`isRecursive(fn)`**: Returns true when `fn` is in a multi-member SCC or has a self-edge.
- **Use cases**: Bottom-up interprocedural analysis (process leaf SCCs first); the inliner uses it to skip
  recursive SCCs accurately rather than relying on a per-edge self-loop check.
- **Tests**: `test_il_callgraph_scc` — linear chain ordering, mutual recursion, self-recursion, `isRecursive`.

## GVN (Global Value Numbering)

- Assigns value numbers to expressions using dominator-tree-scoped hash tables.
- Eliminates redundant computations including loads when no intervening store exists and the load is known
  non-trapping. Loads from unknown pointer values are not removed or reused because IL load semantics include
  null and misalignment traps.
- Uses `ValueKey` canonical forms for commutative normalization.
- Available values stored as `vector<AvailableValue>` (searched most-recent-first) per expression key,
  enabling multiple definitions of the same expression across different dominator-tree paths.
- **Textual ordering guard**: Same `isTextuallyAvailable()` check as EarlyCSE — replacements are only
  applied when the defining block appears at or before the use block in the function's block list.
  Prevents use-before-def violations when a dominator appears later textually.
- Implementation: `src/il/transform/GVN.cpp`
- **Tests**: `test_GVN` — cross-block CSE, redundant load elimination, textual order guard for load elimination.

## LICM (Loop-Invariant Code Motion)

- Hoists instructions whose operands are defined outside the loop to the loop preheader.
- Requires `LoopInfo` and `Dominators` analyses.
- Full `licm` may hoist non-trapping loads and readonly calls when BasicAA proves the loop cannot modify the observed
  memory. The `licm-safe` variant remains available as an explicit diagnostic subset that never hoists memory reads.
- O2 uses full `licm`; `rehab-licm` remains available to validate LICM independently from the broader optimizer.
- Implementation: `src/il/transform/LICM.cpp`

## EHOpt (Exception Handling Optimization)

- Removes dead exception handlers (try/catch blocks where the try body cannot throw).
- Simplifies exception handling structure after inlining.
- Implementation: `src/il/transform/EHOpt.cpp`

## LoopRotate

- Rotates loops by duplicating the header into the preheader.
- Transforms while-loops into do-while form for better analysis by LICM and IndVarSimplify.
- Implementation: `src/il/transform/LoopRotate.cpp`

## Reassociate

- Canonicalizes operand order for commutative/associative ops (add, mul, and/or/xor): operands are
  sorted (temporaries by descending ID, then constants) so equivalent expressions match for CSE/GVN.
- Despite the pass name, it does not yet restructure or rebalance expression chains; canonical operand
  order still helps ConstFold and GVN line up constants in a single pass.
- Implementation: `src/il/transform/Reassociate.cpp`

## SiblingRecursion

- Detects sibling recursive calls (tail calls to the same function) and optimizes them.
- Reduces stack growth for mutually-recursive patterns.
- When it creates a loop backedge, any copied plain signed arithmetic is restored to checked arithmetic so earlier
  one-edge check demotions do not become unverifiable loop-header operations. Later CheckOpt runs may demote them again
  when the loop-shaped proof is visible.
- Implementation: `src/il/transform/SiblingRecursion.cpp`

## Canonical Optimization Pipelines

Every frontend and native codegen entry point calls `pm.runPipeline(module, "O1")` or
`pm.runPipeline(module, "O2")`, so the Zia frontend, the BASIC frontend, the VM, and both native backends
run identical pass sequences. The AArch64 and x86-64 pipelines keep no backend-local pass lists; after
native EH lowering they reject any residual structured EH opcodes, then run the same IL pipelines.

Pipelines are registered in `src/il/transform/PassManager.cpp`:

| Pipeline | Passes, in order |
|----------|------------------|
| `O0` | `simplify-cfg`, `dce` |
| `O1` | `simplify-cfg`, `mem2reg`, `simplify-cfg`, `sccp`, `constfold`, `peephole`, `dce`, `simplify-cfg`, `sccp`, `inline`, `peephole`, `dce`, `simplify-cfg` |
| `O2` | `simplify-cfg`, `mem2reg`, `simplify-cfg`, `loop-simplify`, `licm`, `loop-rotate`, `indvars`, `loop-unroll`, `simplify-cfg`, `reassociate`, `earlycse`, `sccp`, `check-opt`, `loop-simplify`, `licm`, `eh-opt`, `dce`, `simplify-cfg`, `sibling-recursion`, `devirt`, `inline-o2`, `simplify-cfg`, `sccp`, `constfold`, `loop-simplify`, `licm`, `loop-rotate`, `indvars`, `loop-unroll`, `reassociate`, `gvn`, `earlycse`, `check-opt`, `loop-simplify`, `licm`, `runtime-fastpath`, `array-fastpath`, `ownership-opt`, `peephole`, `check-opt`, `dce`, `simplify-cfg`, `sccp`, `constfold`, `peephole`, `gvn`, `earlycse`, `if-conv`, `dce`, `simplify-cfg`, `dse`, `dce`, `late-cleanup` |

Ordering notes for O2:

- `reassociate` runs before `earlycse`/`gvn` so canonicalized expression trees feed CSE.
- `check-opt` is followed by `loop-simplify` + `licm`: deleted or hoisted checks unlock loop-invariant
  hoisting that trapping operations previously blocked.
- `sccp` runs both before and after `inline-o2`, first to simplify callees and then to propagate
  constants through inlined code from call sites.
- A bounded second scalar group catches second-order opportunities exposed by inlining and loop work.

`mem2reg` is canonical in O1/O2 because dominance, edge-repair, non-entry alloca, and loop-reentered
allocation guards make SSA promotion verifier-clean. Full memory-hoisting `licm` is canonical in O2
because load-safety and BasicAA mod/ref guards keep hoisting alias-conservative. Full IL `peephole` is
canonical given its verifier-clean local-use, use-count, trap-preservation, and signed-zero coverage.

The pass manager verifies IR after each pass by default, including release
builds. Callers may disable this for specialised measurement, but rehab and CI
pipelines should keep per-pass verification enabled.

**Test**: `test_il_canonical_pipeline` — verifies O1/O2 contain expected passes, full peephole is canonical, and SCCP
runs. `test_il_realworld_perf_passes` covers devirtualization, array fast-path rewriting, known-object RC helpers,
ownership-pair cleanup, and runtime ownership metadata.

### Rehab Pipelines

The pass manager also registers targeted rehab pipelines for focused pass validation:

| Pipeline | Passes | Purpose |
|----------|--------|---------|
| `rehab-mem2reg` | `simplify-cfg, mem2reg, dce` | Isolate SSA promotion behavior for loop, non-entry alloca, and edge-repair cases |
| `rehab-peephole` | `peephole, dce` | Exercise IL peephole rewrites in isolation from the broader O1/O2 pipelines |
| `rehab-licm` | `loop-simplify, licm, simplify-cfg, dce` | Validate full loop-invariant code motion independently from the broader optimizer |

`zanna il-opt --pipeline` accepts these registered names directly; lowercase rehab names are not uppercased to O-level
aliases.

Further pipeline changes should include verifier-clean IR, native-vs-VM equivalence,
and representative demo/application workload runs.

## Optimization Review Test Coverage

Regression tests covering fixes from the comprehensive IL optimization review
(`src/tests/unit/il/transform/test_opt_review_*.cpp`):

| Test File | Tests | Coverage |
|-----------|-------|---------|
| `test_opt_review_basicaa.cpp` | 7 | Priority cascade, ModRef classification, alias queries |
| `test_opt_review_calleffects.cpp` | 7 | Pure/readonly/conservative classification, generated runtime metadata priority, pure+nothrow deletion gating |
| `test_opt_review_dse.cpp` | 5 | Dead store elimination, load-intervened stores, different allocas, MemorySSA exit-block stores |
| `test_opt_review_loopinfo.cpp` | 4 | Self-loop dedup, normal loop membership, block counts |
| `test_opt_review_peephole.cpp` | 25 | UCmp/FCmp constant folding in CBr, integer reflexive comparisons, trap-preserving skipped folds, signed-zero FP identities |
| `test_opt_review_sccp.cpp` | 4 | FDiv by zero preservation, normal FDiv folding |
| `test_opt_review_valuekey.cpp` | 8 | Commutative normalization, safe opcode classification |

### Optimizer Improvement Tests

| Test File | Tests | Coverage |
|-----------|-------|---------|
| `test_il_canonical_pipeline.cpp` | 9 | O1/O2 pass registration, mem2reg and full LICM promotion, full peephole promotion, removed legacy safe alias, SCCP execution via canonical pipeline |
| `test_realworld_perf_passes.cpp` | 6 | Runtime ownership metadata, object RC fast paths, numeric array fast helpers after bounds proof, fast-path invalidation across memory effects, direct-call devirtualization, retain/release pair cleanup |
| `test_il_mem2reg_nonentry.cpp` | 6 | Non-entry-block alloca promotion, domination-filtered promotion, edge repair, loop-reentered alloca guard, EH skip |
| `test_il_inline_threshold.cpp` | 3 | Default thresholds (`instrThreshold` 80 / `blockBudget` 1 / `maxInlineDepth` 3), 50-instruction inline, oversized rejection |
| `test_il_earlycse_domtree.cpp` | 3 | Cross-block CSE via domtree, sibling-branch non-elimination, textually-unsafe rejection |
| `test_il_callgraph_scc.cpp` | 4 | Linear chain SCC ordering, mutual recursion, self-recursion, isRecursive |

### SCCP ↔ CheckOpt Integration

CheckOpt statically evaluates checks whose operands SCCP's `rewriteConstants()` has already inlined as
`ConstInt` literals:

- `IdxChk(5, 0, 10)` with I64 result → eliminated (index 5 is trivially in [0, 10))
- `SDivChk0(12, 3)` with I64 result → eliminated (divisor 3 ≠ 0)
- Narrower-typed checks (I32 IdxChk result) with live uses are preserved to avoid IL verifier type mismatches
  (ConstInt is always typed I64; substituting into an I32 scrutinee position would fail verification)

| Test File | Tests | Coverage |
|-----------|-------|---------|
| `checkopt_redundancy.cpp` | 3 new | Const in-bounds IdxChk elimination, non-zero SDivChk0, OOB preservation |

### Post-Dominator Tree

`computePostDominatorTree()` lives in `src/il/analysis/Dominators.hpp/.cpp` and is registered as the
`"post-dominators"` analysis in `PassManager`. It enables control-dependent DCE and precise speculative
hoisting.

| Test File | Tests | Coverage |
|-----------|-------|---------|
| `PostDominatorsTests.cpp` | 3 | Linear chain, diamond join (merge pdoms entry), multiple exits |

### SimplifyCFG Self-Loop Handling

`mergeSinglePred` in `BlockMerging.cpp` counts self-loop edges in a block's predecessor total but records
only non-self blocks as the merge candidate. The `predecessorEdges == 1` guard therefore rejects a merge
when a block has both an external predecessor and a self-loop — merging there would rewrite the self-loop
into a back-edge to the entry block and produce an unbounded alloca-stack loop.

Covered by `test_basic_do_exit` (DO WHILE loop back-edges) and `test_il_opt_equivalence` (randomized
differential testing of the optimizer pipelines).
