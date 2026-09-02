# ADR 0315: Cycle collection of Zia class instances

- Status: Accepted
- Date: 2026-09-02
- Scope: Zia frontend (`Lowerer`), runtime object allocation (`rt_object`), cycle collector (`rt_gc`), new runtime module `rt_class_layout`
- Related: ADR 0313 (class destructor hook), ADR 0314 (declared result ownership), `docs/memory-management.md`

## Context

ADR 0313 made every Zia object release its fields when its reference count
reaches zero. Reference counting cannot reclaim a strong cycle: a
`parent <-> child` pair, a doubly linked list, a node that refers to itself, or
a node held by a list that the node itself owns. The cycle collector already
existed for reference-bearing arrays and runtime containers (`rt_gc_track`,
trial deletion), but **class instances were never registered with it**: the
collector had no way to enumerate an instance's outgoing strong edges, so a
cycle between two plain objects lived forever unless one edge was declared
`weak`. `docs/memory-management.md` recorded this as its first known
unsoundness.

## Decision

### Per-class strong-slot maps, registered at program entry

New runtime pair `src/runtime/oop/rt_class_layout.[ch]`:

```c
void  rt_obj_class_layout_begin(int64_t class_id, int64_t slot_count);
void  rt_obj_class_layout_add_slot(int64_t class_id, int64_t offset, int64_t kind); /* kind = RT_CLASS_SLOT_OBJ */
int8_t rt_obj_class_layout_has_ref_slots(int64_t class_id);
const rt_class_layout_t *rt_obj_class_layout_get(int64_t class_id);
```

A dense table indexed by class id (ids are `1..N`), grown under a spin lock,
immutable after the entry prologue. Only strong object slots exist: strings
and weak handles are never members of a cycle and stay out of the map. The
two registration functions are IL-visible descriptor rows (`void(i64,i64)`,
`void(i64,i64,i64)`, manual lowering) and runtime-surface internal symbols.

Rejected: extending `rt_class_info` (per-`RtContext` and sealable while the
collector is process-global) and per-instance registration in the style of
`rt_box_value_type` (O(fields) per allocation).

### The compiler emits the maps

`Lowerer::emitClassLayoutInit` emits `__zia_layout_init` unconditionally (a
`ret void` stub when no class has a strong slot), batched into bounded helpers
exactly like the itable init. For every class with a positive id it walks the
fields (inherited first), skips `weak` fields, flattens inline aggregates
through the shared `collectManagedSlots` (the same flattener boxed value
types use) and keeps object-kind slots only. The entry prologue calls it right
after `rt_obj_set_class_dtor_hook`, before any object can be allocated.

### Instances are tracked from their first allocation

`rt_obj_new_i64` registers the payload with the collector
(`rt_gc_track_class_instance`, non-trapping) when `class_id > 0` and the
class has at least one strong slot; on failure the not-yet-escaped payload is
freed and the allocation traps, mirroring reference-bearing arrays in
`rt_heap_alloc`. Classes with only value, string or weak fields are never
tracked and pay nothing. The collector traverses an instance with
`gc_zia_object_traverse`, which visits every non-null slot from the class's
map; untracked children (value-only objects, runtime handles) are reported
and ignored by the lookups, so cycles that pass through a `List[Node]` or a
`Map` still collect.

### Reclaim rule

In `gc_finalize_unreachable`, a garbage member that is a compiled class
instance runs the class destructor hook (`__zia_dtor_dispatch`) under the
same release suppression a finalizer gets: releases whose target is another
member of the garbage set are no-ops through `rt_heap_release_impl`'s
sentinel (the collector owns those counts); releases to outside objects take
the normal path. The member is marked `class_dtor_ran` and
`gc_free_finalized_unreachable` skips the traverse-release for it, so every
outgoing edge is dropped **exactly once** on both death paths (refcount and
collector). Strings and weak handles are released by the destructor only.
Resurrection and trapping destructors follow the existing finalizer contract.

### Policy: explicit only

`g_gc_threshold` stays 0 and `rt_gc_safepoint` is unchanged. Programs call
`Zanna.Runtime.GC.Collect()` at their own boundaries (Legacy Baseball collects
in `Watch3DStage.destroy()` and reports the pass at each game end). A C
runtime function never runs user destructors on its own.

### Parity

`rt_obj_new_i64` is the single allocation path for compiled objects; the
layout calls are ordinary descriptor rows executed by both VMs and native
code; the destructor runs through the already-bridged hook (ADR 0313).

## Limitations

- Closure environments (class id 0) and runtime-internal objects (negative
  ids) are not traversed; a cycle that passes only through them still leaks.
- Instances of classes whose only reference fields are strings or `weak` are
  not tracked, by design.
- A destructor or finalizer that resurrects a member of a cycle after other
  members' destructors already ran leaves those members with released fields;
  resurrection from inside a collected cycle is unsupported (same class of
  hazard the finalizer contract already documents).
- Collection is explicit; a program that never calls `GC.Collect()` keeps its
  cycles until exit.
- The layout map flattens inline aggregates (structs, fixed arrays, tuples)
  to their reference slots, while the synthesized destructor still releases a
  field as declared; a reference nested inside an inline aggregate field is
  traversed by the collector but not released by the destructor. No Zia
  program in the tree declares one; the destructor flattener is a follow-up.

## Consequences

- Strong cycles between Zia objects are reclaimable with one explicit call;
  `weak` is no longer required for correctness, only for immediacy.
- Allocation of a class with strong slots pays one hash-table insert; frame
  budgets in Legacy Baseball's probes are unchanged within noise (see the
  plan 89 ledger).
- The runtime C ABI gains the four `rt_obj_class_layout_*` functions (two of
  them IL-visible descriptor rows) and `rt_gc_track_class_instance`.

## Tests

- `test_rt_class_layout`: layout register/query/redeclare; instance tracked
  only when slots exist; a two-member cycle with an external child collected
  with the destructor hook run once per member and the external child freed
  once; a self cycle; a live instance survives a pass; refcount death untracks.
- `test_zia_destructors` (`ClassLayoutInitRegistersStrongSlots`):
  `__zia_layout_init` exists, is called from the entry point, and registers
  strong slots only (inherited included; weak/String/Integer excluded).
- `zia_runtime_test_class_cycle_gc` (VM) and `native_run_zia_class_cycle_gc`
  (native): ring, doubly linked list, self reference, cycle through a
  `List[Node]`, weak variant; tracked count back at baseline, weak references
  dead, `deinit` counts exact, value-only class never tracked.
- Legacy Baseball: `watch3d_live_probe` prints the game-end collection.
