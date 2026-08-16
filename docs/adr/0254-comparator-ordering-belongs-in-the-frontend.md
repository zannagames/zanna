---
status: active
audience: contributors
last-verified: 2026-08-16
---

# ADR 0254: Comparator Ordering Belongs in the Frontend, Not a Runtime Callback

## Status

Accepted (2026-08-16) — records a rejected implementation and the correct one.

## Consulted

- ADR 0239 — VM Callback Policy for the 3D Surface
- `src/runtime/collections/rt_seq_ops.c` — `rt_seq_sort_by`, an unregistered stable merge sort
- `src/runtime/collections/rt_list.c` — `list_sort_impl`, the same shape
- `src/frontends/zia/Lowerer_Expr.cpp:288-300` — `&func` lowering
- `src/frontends/common/CollectionMethodCatalog.cpp` — the language-level `List[T]` surface

## Context

**The runtime cannot order a collection of objects.** The registered surface
offers `List.Sort`, `List.SortDesc`, `Seq.Sort`, `Seq.SortDesc` — all over a
fixed total order on boxed scalars — plus `Zanna.Game.UI.HudTable.SortBy`, which
is a widget's column sort. A `List[MyClass]` has no ordering path at all.

The cost is measurable. An audit of the largest real Zanna application found two
hand-written **unstable** exchange sorts plus a 60-line hand-rolled top-1
multi-key scan. The instability is the expensive part: an unstable sort makes
tie order an artifact of the algorithm, and that artifact gets printed into
byte-stable report fixtures, so replacing the sort with *anything* — including a
better one — moves the output.

Two facts made a runtime-side fix look easy:

1. `rt_seq_sort_by(void *obj, int64_t (*cmp)(void *, void *))` already exists —
   a complete, documented, **stable** merge sort with trap-safe scratch
   handling. It was simply never registered. `list_sort_impl` is the same shape.
2. Zia's `&func` operator lowers to `Value::global(mangledName)` — an address —
   and `Zanna.Threads.Thread.Start(&handler, 0)` already passes a Zia function
   to the runtime through the `obj` calling convention.

## Decision

**Reject the runtime-callback implementation. Comparator ordering must be
lowered by the frontend, alongside the existing `List[T]` combinators.**

### Why the runtime-callback route is wrong

It was implemented and tested before being rejected, which is how the problem
surfaced: `List.SortByKey(myList, &winsKey)` type-checks, then **crashes with
SIGBUS** under `zanna run`. The address `&func` produces is a VM-level symbol,
not a machine address the C runtime can call. `Thread.Start` works because it
invokes its entry at a *thread boundary*, not re-entrantly inside a runtime
call.

ADR 0239 already settled this, for the 3D surface but on general grounds:

> Zia functions are VM closures — invoking one from inside a runtime C call
> requires a re-entrant VM trampoline. […] A general "call Zia from C
> mid-simulation" trampoline is **rejected as a public contract**: it would make
> simulation order observable and divergent between VM and native builds, break
> the batch determinism work, and turn every runtime call site into a
> re-entrancy hazard.

A comparator invoked from inside a merge sort is precisely that hazard, and it
would be *worse* than the 3D case it was rejected for: a sort calls back
O(n log n) times per call, and VM-versus-native divergence in comparison order
is exactly the determinism property the ordering work exists to protect.

### The correct implementation

Lower `sortBy` in the Zia frontend, as `List[T].map`/`filter`/`reduce`/
`firstWhere`/`any`/`all` already are (`CollectionMethodCatalog.cpp`,
`Sema_Expr_Call.cpp`). Those take lambdas today and emit VM-native code — no
runtime re-entrancy, no ABI question, and the element type `T` is preserved so
the result stays typed.

Proposed language surface, mirroring the existing combinators:

```zia
rows.sortBy((a, b) => b.wins - a.wins);   // stable
rows.sortByKey((r) => r.wins);            // ascending, stable
rows.sortByKeyDesc((r) => r.wins);
var best = rows.maxBy((r) => r.wins);     // T?, ties keep the first
var worst = rows.minBy((r) => r.wins);
```

Stability must be a **documented guarantee**, not an implementation detail: it
is the property that lets a report writer swap a hand-rolled sort for this one
without moving output, provided the comparator is total.

The frontend can still delegate the actual ordering to the existing
`rt_seq_sort_by` / `list_sort_impl` merge sorts by emitting a comparison
*thunk* it controls, rather than handing the runtime an opaque Zia address. That
keeps the sort algorithm in one place while leaving the call inside the VM.

## Consequences

- The gap stays open until the frontend work is done. Applications continue to
  hand-roll sorts; the mitigation in the interim is to make those comparators
  **total** (add an explicit tiebreaker such as an id) so that a later swap to a
  stable sort is byte-neutral.
- No runtime registry change lands from this ADR. `rt_seq_sort_by` and
  `list_sort_impl` stay unregistered rather than being exposed with a calling
  convention that cannot work.
- ADR 0239's policy is confirmed to be general, not 3D-specific. It is worth
  restating in the runtime-extension how-to, because "the runtime already takes
  function pointers" is a very easy wrong turn to take — `Thread.Start`'s
  existence actively suggests it.
- **Open question deliberately not answered here:** `Zanna.Collections.Seq.Get`
  returns an untyped `obj`, which Zia cannot narrow to a class (`as` on `Any` is
  rejected). That makes a runtime `Seq` unusable for class elements regardless
  of sorting. Zia's generic `List[T]` does not have this problem because it
  keeps `T`, which is a further argument for putting this at the language level.

## Links

- ADR 0239 — VM Callback Policy (the governing decision)
- ADR 0249–0253 — the runtime completions from the same audit that *did* land
- `baseball/plans/58-runtime-adoption.md` §Q2 — the queued migration this blocks
