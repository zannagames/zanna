---
status: active
audience: contributors
last-verified: 2026-09-05
---

# ADR 0329: Reconsider Provisional SCCP Traps

## Status

Accepted. Records the optimizer correction and its completed regression tests.

## Context

Sparse conditional constant propagation can initially see a loop-carried
divisor as zero, then merge another executable edge and make it overdefined.
The old solver permanently marked the computational block trapping on the
first observation. It subsequently ignored operand updates in that block,
including updates to its induction-variable calculation. Constant substitution
could therefore freeze a finite loop's index and remove its exit. A nested
mean-pixel calculation reproduced the failure before native lowering.

The [IL reference](../il/il-guide.md#reference) already defines checked
arithmetic and runtime traps. This decision repairs propagation under those
semantics; it changes no opcode, verifier rule, grammar, runtime C ABI,
serialization format, or cross-layer dependency.

## Decision

1. `SCCPSolver::blockTraps_` records the earliest currently trapping `Instr*`
   in each block, or null. A computational trap is provisional while the
   operand lattice is still changing.
2. Instructions before and at that position remain eligible for re-evaluation.
   Only the suffix beyond the current trap is suppressed. Earlier definitions
   can themselves depend on a newly activated predecessor.
3. Instruction order comes from per-block lexical ordinals collected in
   `initialiseStates`. `BasicBlock::instructions` is a `StableList`; stable
   identity does not imply address ordering or contiguous allocation.
4. If the formerly trapping computation no longer folds to a definite trap,
   clear the marker and queue the block again. The suppressed suffix and its
   outgoing edges must be visited even when they do not directly use the
   changing operand. Any later definite trap becomes the new earliest marker.
5. Explicit terminating traps/resumes retain their existing behavior. A
   genuinely trapping literal computation remains in the emitted program;
   this correction does not speculate through it or suppress its runtime error.
6. Solver maps borrow instructions owned by the function. Identity and ordinal
   maps remain valid through solving/substitution, before CFG cleanup mutates
   the instruction lists. The additional map uses linear space in the number
   of instructions and introduces no external dependency or persistent state.
7. All frontends and native targets use the same IL transformation. No renderer,
   platform, graphics-enabled build, or native-backend special case is added.

## Consequences

- Loop inputs may become overdefined without freezing a provisional trap's
  dependent calculations. Optimized native code retains the finite loop exit.
- Existing modules, source programs, and runtime artifacts require no migration.
  Previously miscompiled native executables must be rebuilt.
- Unit coverage includes changing divisors/remainders, a preceding definition
  feeding checked overflow, multiple potential traps, and genuine literal traps.
- `sccp_transient_trap.zia` executes the reduced pixel kernel in the VM and
  native O0/O1/O2 lanes with finite timeouts, checking results before printing
  success. These tests are registered for macOS, Windows, and Linux; registration
  does not substitute for actual execution on each host.
- The macOS canonical selected suite and additional slow cross-layer arithmetic
  and memory/EH conformance tests pass. Actual Windows/Linux runs remain pending.

## Alternatives Considered

- Permanently suppressing the entire block is unsound before the operand
  lattice reaches its fixed point.
- Reconsidering only the trap ignores earlier definitions whose values change;
  the checked-overflow regression demonstrates this case.
- Comparing instruction addresses assumes storage ordering that `StableList`
  does not promise; an expanded regression caught this intermediate mistake.
- Removing or hoisting division in the triggering application hides the defect
  and leaves other finite programs vulnerable to the same miscompilation.
- Disabling SCCP or all arithmetic-trap folding globally sacrifices valid
  optimizations without repairing the solver's reachability model.
