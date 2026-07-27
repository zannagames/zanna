---
status: active
audience: contributors
last-verified: 2026-07-26
---

# CODEMAP: IL Analysis

Static analysis utilities (`src/il/analysis/`) for IL programs.

## Overview

- **Total source files**: 12 (.hpp/.cpp) — `Dominators.hpp/.cpp` includes both forward and post-dominator trees; `MemorySSA.hpp/.cpp` provides memory def-use chains for precise DSE

## Control Flow

| File           | Purpose                                                     |
|----------------|-------------------------------------------------------------|
| `CFG.cpp`      | CFG queries implementation                                                                       |
| `CFG.hpp`      | CFG queries: successors, predecessors, post-order, reverse post-order, topological order, acyclicity; CFGContext caching layer |
| `CallGraph.cpp`| Direct-call graph implementation with Tarjan's SCC algorithm                                     |
| `CallGraph.hpp`| Direct-call graph construction and queries; SCCs in reverse topo order; `isRecursive()` query    |

## Dominance

| File             | Purpose                                                                      |
|------------------|------------------------------------------------------------------------------|
| `Dominators.cpp` | Forward and post-dominator tree implementations                              |
| `Dominators.hpp` | `DomTree` / `computeDominatorTree()` and `PostDomTree` / `computePostDominatorTree()` |

### DomTree (`"dominators"`)

- **Algorithm**: Cooper–Harvey–Kennedy iterative dataflow on RPO; `idom[entry] = nullptr`
- **Queries**: `dominates(A, B)`, `immediateDominator(B)`
- **Children map**: populated for efficient tree walk (used by EarlyCSE, CheckOpt, Mem2Reg)
- **Tests**: `src/tests/analysis/DominatorsTests.cpp`

### PostDomTree (`"post-dominators"`)

- **Definition**: Block X post-dominates block Y iff every path from Y to any exit passes through X
- **Algorithm**: CHK iterative dataflow on reversed CFG; exit blocks initialised with `ipostdom = nullptr` (virtual exit)
- **Virtual exit**: represented by `nullptr` in the `ipostdom` map; exit blocks and, in the multiple-exit case,
  the entry block can have `ipostdom = nullptr`
- **Queries**: `postDominates(A, B)`, `immediatePostDominator(B)`
- **Registration**: Available as `"post-dominators"` in `PassManager::analyses()`
- **Tests**: `src/tests/analysis/PostDominatorsTests.cpp` — linear chain, diamond, multiple exits

## Alias Analysis

| File           | Purpose                                                                     |
|----------------|-----------------------------------------------------------------------------|
| `BasicAA.hpp`  | SSA-based alias analysis with alloca/param/global tracking and ModRef queries |

### BasicAA Details

- **Alias queries**: Classifies pointer pairs as NoAlias/MayAlias/MustAlias using SSA def-chain analysis
- **Base tracking**: Follows GEP/AddrOf/GAddr chains up to depth 8 to identify allocation bases
- **Runtime effects**: Queries the generated runtime signature table first, then the small helper-effect fallback; no process-wide mutable analysis cache is used.
- **ModRef queries**: Priority cascade — module functions authoritative, runtime signatures as fallback
- **Tests**: `src/tests/analysis/BasicAATests.cpp`, `src/tests/unit/il/transform/test_opt_review_basicaa.cpp`

## Memory SSA

| File              | Purpose                                                                     |
|-------------------|-----------------------------------------------------------------------------|
| `MemorySSA.hpp`   | MemorySSA data structures: `MemoryAccess` (Def/Use/Phi/LiveOnEntry), `MemorySSA` result with dead-store query |
| `MemorySSA.cpp`   | `computeMemorySSA()`: RPO-order forward dataflow building def-use chains; dead-store detection with precise call handling |

### MemorySSA Details

- **Purpose**: Provides memory def-use chains for DSE to eliminate dead stores more precisely than conservative BFS
- **Key precision**: Calls are **transparent** for non-escaping allocas — external calls cannot access non-escaping
  stack memory, so they are not treated as read barriers (the main limitation of `runCrossBlockDSE`)
- **Access kinds**: `LiveOnEntry` (id=0, root), `Def` (stores/modifying calls), `Use` (loads/reading calls), `Phi` (join points)
- **Construction**: Three-phase — escape analysis → RPO forward dataflow (def-use link building) → dead-store detection
- **Dead-store rule**: A store is removable only when every path to exit either overwrites the same location first or exits without a read; loop/path state is tracked conservatively to avoid conflating live and killed paths.
- **Dead store**: A `Store` at `(block, instrIdx)` is dead when all forward paths kill or exit without any load reading it
- **`isDeadStore(block, idx)`**: O(1) predicate queried by `runMemorySSADSE` in `DSE.cpp`
- **Registration**: Available as `"memory-ssa"` in `PassManager::analyses()`
- **Tests**: `src/tests/analysis/MemorySSATests.cpp` — call-barrier precision (key improvement), live-store preservation,
  simple cross-block, escaping alloca safety, Def/Use node assignment
