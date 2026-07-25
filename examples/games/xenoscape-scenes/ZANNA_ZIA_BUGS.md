# Zanna/Zia Bugs Found During Xenoscape Overhaul

This list is limited to Zanna/Zia compiler, runtime, or tooling issues noticed
while working on Xenoscape. Gameplay/data bugs are tracked in the code changes
and validation probe instead.

## Confirmed

1. **Native-linked macOS aggregate-return miscompile** — **RESOLVED 2026-07-07**
   - Evidence: `physics.zia` documents that `MoveResult` must remain a heap
     class because the native-linked macOS demo binary currently miscompiles
     aggregate returns.
   - Impact: Zia code that should naturally return a lightweight record/struct
     has to allocate or reuse a class instance instead.
   - Workaround in Xenoscape: `PhysicsHelper` owns one reusable `MoveResult`
     and mutates it in place.
   - Resolution: the residual breakage was two frontend lowering bugs, not a
     codegen return-classification bug. (a) `lowerNewStruct` allocated struct
     stack storage without zero-initializing it, so `init`'s managed-field
     stores released stack garbage — the VM masked this by zero-filling
     allocas, native crashed ("invalid string handle" / SEGV). (b) The
     `Zanna.Core.Box.ValueTypeAddField` retain flag lowered as `i64` where the
     runtime ABI declares `i1`, so any struct with a String/object field
     failed IL verification. Both fixed (`Lowerer_Expr_Complex.cpp`,
     `Lowerer_Emit.cpp`); pinned by `tests/zia_runtime/51_struct_return_abi.zia`
     (VM + native -O0/-O2, covers the exact MoveResult shape, HFA pairs/quads,
     >16-byte indirect returns, method returns, nested structs, String fields,
     chained returns). `MoveResult` may collapse to a struct now; the reusable
     instance also serves as a zero-alloc optimization, so it stays.

## Tooling Candidates

1. **Ambiguous imported symbol warning for common names**
   - Evidence: the targeted Xenoscape build emits `warning[V3001]` because
     `Zanna.Math.Lerp` conflicts with `Zanna.Graphics.Color.Lerp` when both
     modules are bound.
   - Impact: broad module binds make common helper names fragile and noisy.
   - Follow-up: prefer or add clearer namespace-qualified/aliased import
     patterns so demos can bind graphics and math APIs without warning churn.

2. **No obvious discard pattern for intentional count loops** — **RESOLVED**
   - Evidence: the build emits `warning[W001]` for range-loop variables such as
     `for i in 0..MAX_ENEMIES` when the variable is only used to repeat work.
   - Impact: intentional fixed-count loops produce warning noise unless the
     code is rewritten as a manual `while`.
   - Resolution: `for _ in 0..N` is the supported discard idiom — sema skips
     `_` in the W001 unused-variable check and `_` is reusable across loops in
     the same scope (verified 2026-07-07). Now documented in
     `docs/book/part1-foundations/05-repetition.md` ("Discarding the Loop
     Variable").
