---
status: active
audience: contributors
last-verified: 2026-07-26
---

# Zanna Memory Management

> **Status**: Active reference. Known gaps are listed under
> [Known Unsoundness](#known-unsoundness).
>
> **Audience**: Runtime developers, language users, future contributors.

Zanna uses a **hybrid memory model**: atomic reference counting as the primary
lifetime mechanism, a slab pool allocator for small strings, and an opt-in
cycle-detecting garbage collector for breaking reference cycles. There is no
generational collector, no arena allocator, and no tracing GC. The design
prioritises determinism and low latency over throughput.

---

## Architecture Overview

```text
┌───────────────────────────────────────────────────────────────┐
│                     Zanna Program                             │
│  (Zia / BASIC source → IL → VM or Native)                    │
└──────────────────────────┬────────────────────────────────────┘
                           │ calls
┌──────────────────────────▼────────────────────────────────────┐
│                   Unified Heap API                            │
│   rt_heap_alloc · rt_heap_retain · rt_heap_release            │
│   rt_obj_new_i64 · rt_string_from_bytes                       │
│                                                               │
│   ┌─────────────────────┐  ┌───────────────────────────────┐  │
│   │  Pool Allocator     │  │  System Allocator             │  │
│   │  (strings ≤ 512B)   │  │  (arrays, objects, large str) │  │
│   │  Spinlock freelists │  │  malloc / calloc / free       │  │
│   │  4 size classes      │  │                               │  │
│   └─────────────────────┘  └───────────────────────────────┘  │
│                                                               │
│   ┌─────────────────────────────────────────────────────────┐ │
│   │  Cycle-Detecting GC (opt-in)                            │ │
│   │  rt_gc_track · rt_gc_collect · rt_weakref_*             │ │
│   └─────────────────────────────────────────────────────────┘ │
└───────────────────────────────────────────────────────────────┘
```

Every heap object — string, array, or runtime object — is allocated through
`rt_heap_alloc()` and prefixed by a common header:

```text
Memory layout:
┌──────────────────────────┬─────────────────────────┐
│     rt_heap_hdr_t        │       Payload            │
│  (metadata + refcount)   │  (string/array/object)   │
└──────────────────────────┴─────────────────────────┘
^                          ^
header address             payload pointer (returned to caller)
```

**Key source files:**

| File | Role |
|------|------|
| `src/runtime/core/rt_heap.h` | Header struct, allocation API |
| `src/runtime/core/rt_heap.c` | Allocation, retain/release, realloc |
| `src/runtime/core/rt_pool.h` / `.c` | Slab pool allocator |
| `src/runtime/core/rt_gc.h` / `.c` | Cycle GC and weak references |
| `src/runtime/oop/rt_object.h` / `.c` | Object allocation, finalizers, resurrection |
| `src/runtime/core/rt_string.h` / `rt_string_ops.c` | String refcounting and operations |
| `src/runtime/core/rt_memory.c` | General-purpose allocation shim (`rt_alloc`) |

---

## Heap Object Header

Every heap-allocated runtime value is preceded by `rt_heap_hdr_t`:

```c
typedef struct rt_heap_hdr {
    uint32_t magic;                // 0x52504956 ('VIPR') — corruption sentinel
    uint16_t kind;                 // RT_HEAP_STRING(1) | RT_HEAP_ARRAY(2) | RT_HEAP_OBJECT(3)
    uint16_t elem_kind;            // RT_ELEM_NONE(0) | I32(1) | I64(2) | F64(3) | U8(4) | STR(5) | BOX(6) | OBJ(7)
    uint32_t flags;                // bit0 = disposed, bit1 = pool-allocated
    size_t   refcnt;               // Atomic reference count; 1 at creation; SIZE_MAX-1 = immortal
    size_t   len;                  // Current logical length (elements)
    size_t   cap;                  // Allocated capacity (elements)
    size_t   alloc_size;           // Total allocation bytes (header + payload)
    int64_t  class_id;             // Runtime class ID (objects only)
    rt_heap_finalizer_t finalizer; // Optional cleanup callback (objects only)
} rt_heap_hdr_t;
```

**Invariants:**
- `magic == 0x52504956` — validated through live heap-registry checks before public heap helpers touch the header
- `refcnt == 1` on fresh allocation; caller owns the initial reference
- `refcnt >= SIZE_MAX - 1` = immortal (never freed; used for string literals and immutable runtime-owned handles)
- `len <= cap` maintained by all mutating operations
- Payload address = `(uint8_t*)header + sizeof(rt_heap_hdr_t)`
- Header address = `(uint8_t*)payload - sizeof(rt_heap_hdr_t)`

---

## Reference Counting

### Core API

| Function | Behaviour |
|----------|-----------|
| `rt_heap_retain(payload)` | CAS-based atomic increment. No-op for NULL or immortal payloads. Traps on zero/overflow before the count can collide with the immortal sentinel. |
| `rt_heap_release(payload)` | CAS-based atomic decrement. Frees (pool or system) when count reaches zero; double release and attempts to release immortal payloads trap without underflow. |
| `rt_heap_release_deferred(payload)` | Decrement without freeing at zero. Caller must later call `rt_heap_free_zero_ref`. |
| `rt_heap_free_zero_ref(payload)` | Free only if refcount is already zero. No-op otherwise. |
| `rt_heap_get_info(payload, out)` | Copies scalar metadata while the live-allocation registry is locked. Use this for borrowed or untrusted handle validation; the result does not expose an internal header address. |
| `rt_heap_realloc(payload, ...)` | Moves and invalidates the original allocation only when its exact refcount is one. Shared or immortal payloads trap and remain unchanged. |
| `rt_memory_retain(payload)` | Public `Zanna.Runtime.Unsafe.Retain` wrapper; validates live object, array, or string handles before retaining and traps on raw string payloads or unsupported heap kinds. |
| `rt_memory_release(payload)` | Public `Zanna.Runtime.Unsafe.Release` wrapper; releases through managed object/string/array paths and runs finalizers or element cleanup at zero. |
| `rt_memory_retain_str(str)` | Public `Zanna.Runtime.Unsafe.RetainStr` wrapper; validates and retains a runtime string. |
| `rt_memory_release_str(str)` | Public `Zanna.Runtime.Unsafe.ReleaseStr` wrapper; validates and releases a runtime string. |

Public heap helpers reject non-runtime and already-freed payloads before header
access. That keeps stale pointers on the trap path instead of relying on
debug-only assertions or undefined behaviour. Code that merely inspects a
borrowed handle must use `rt_heap_get_info()` rather than retaining the raw
header returned by `rt_heap_try_get_header()`; the latter is reserved for a
caller that already owns a strong reference or the collector's exclusive graph
scope.

### Memory Ordering

The refcount protocol follows the standard release-acquire pattern used by
`std::shared_ptr`:

- **Retain**: `__ATOMIC_RELAXED` — visibility is sufficient; no ordering constraint.
- **Release (decrement)**: `__ATOMIC_RELEASE` — ensures all writes to the object
  happen-before the decrement is visible to other threads.
- **Release (at zero)**: `__atomic_thread_fence(__ATOMIC_ACQUIRE)` — ensures the
  releasing thread sees all prior writes before freeing.

### String Refcounting

Strings have wrapper functions that dispatch to the heap API:

- `rt_string_ref(s)` → `rt_heap_retain` (returns same pointer for chaining)
- `rt_string_unref(s)` → `rt_heap_release`
- `rt_str_retain_maybe(s)` / `rt_str_release_maybe(s)` — NULL-safe variants

**Immortal strings**: Literal strings created via `rt_str_from_lit()` or
`rt_const_cstr()` have `refcnt >= RT_HEAP_IMMORTAL_REFCNT` (`SIZE_MAX - 1`) and are never freed. The
retain/release fast path checks for this and short-circuits.

**String interning**: `rt_string_intern(s)` inserts into a global hash table
(FNV-1a, open-addressing, 5/8 load factor). Interned strings are effectively
immortal — retained by the intern table until `rt_string_intern_drain()` is
called (currently only used in tests).

### Object Refcounting

Objects use a two-step release pattern that allows finalizer interleaving:

```text
1. rt_obj_release_check0(obj)  → decrements refcount (deferred)
                                  returns 1 if count reached zero
2. [caller runs custom cleanup]
3. rt_obj_free(obj)            → verifies refcount is zero, invokes finalizer,
                                  then rt_heap_free_zero_ref
```

This split exists because finalizers may need the object's allocation to remain
valid (e.g., to read fields during cleanup).
Calling `rt_obj_free()` on a still-retained object traps; callers must not use it
as a combined release/free helper.

### Consume Semantics

Some operations consume their operands — they release the input references
and return a new reference:

- `rt_str_concat(a, b)` releases both `a` and `b`, returns a new string
  (or reuses `a` in-place if uniquely owned with sufficient capacity)

### Debug Tracing

Define `ZANNA_RC_DEBUG` to enable stderr logging of every retain/release with
the payload address and resulting refcount.

---

## Pool Allocator

The pool reduces `malloc`/`free` overhead for small, frequently-allocated objects
(primarily short strings).

### Size Classes

| Class | Block Size | Blocks/Slab |
|-------|-----------|-------------|
| `RT_POOL_64` | 64 bytes | 64 |
| `RT_POOL_128` | 128 bytes | 64 |
| `RT_POOL_256` | 256 bytes | 64 |
| `RT_POOL_512` | 512 bytes | 64 |

Allocations larger than 512 bytes fall through to `malloc`/`free`.

### Design

- **Spinlock-protected freelists** keep intrusive links private until a block is
  removed. This avoids reading a candidate block after another thread has
  returned it to caller-owned memory.
- **Slab allocation**: when a freelist is empty, a new slab of 64 blocks is
  allocated from the system, all blocks are pushed onto the freelist, and one is
  returned.
- **Blocks are zeroed before allocation** so recycled caller data is never
  exposed without also clearing the same block redundantly on free.
- Heap strings, arrays, and objects whose complete allocation fits a size class
  may use the pool. Reallocation moves pooled allocations when necessary.

### Lifetime

- Freed blocks stay on the freelist for reuse during normal execution.
- `rt_pool_shutdown()` is used by runtime teardown and may also be called
  explicitly. It reclaims only size classes with no outstanding allocations;
  live classes remain valid until their last block is released and a later
  shutdown retries reclamation.
- `rt_pool_stats(class_idx, &allocated, &free)` provides per-class diagnostics.

---

## Cycle-Detecting Garbage Collector

### Purpose

Reference counting cannot reclaim cycles (A → B → A). The cycle GC supplements
refcounting by detecting and breaking unreachable reference cycles among
explicitly registered objects.

### Algorithm: Trial-Deletion Mark-Sweep

The collector runs synchronously in four phases:

```text
Phase 1 — Initialize
  For each tracked object: trial_rc = 1, color = white

Phase 2 — Trial Decrement
  For each tracked object, call its traverse function.
  For each child that is also tracked: decrement child's trial_rc.
  After this phase, objects with trial_rc <= 0 are only referenced
  by other tracked objects (candidate cycle members).

Phase 3 — Scan (Mark Reachable)
  Objects with trial_rc > 0 have external references → mark black.
  Promoted black roots and trial_rc > 0 roots are both scanned.
  Recursively mark all children reachable from black objects.

Phase 4 — Collect
  White (unmarked) objects are unreachable cycle members.
  Untrack them, invoke finalizers, release outgoing references, clear
  weak references for objects that were not resurrected, then free.
```

The `trial_rc = 1` assumption means the algorithm assumes exactly one external
reference per tracked object. After trial decrements from tracked children,
objects that still have `trial_rc > 0` are provably reachable. This is
conservative — it may not detect all cycles in one pass if objects have multiple
external references — but it is always safe.

### Registration

Objects must be **explicitly registered** for cycle collection. Reference-bearing
heap arrays (`RT_ELEM_OBJ` and `RT_ELEM_BOX`) are registered transactionally by
`rt_heap_alloc()` before their payload is returned:

```c
rt_gc_track(obj, traverse_fn);   // Register as potentially cyclic
rt_gc_untrack(obj);              // Remove before manual free
rt_gc_is_tracked(obj);           // Query tracking status
```

`rt_gc_track` accepts heap objects and reference-bearing arrays. Passing a string,
primitive array, stale pointer,
or non-runtime payload traps instead of registering a value the collector cannot
traverse or reclaim safely.

The `traverse_fn` callback must enumerate all strong references held by the
object by calling `visitor(child, ctx)` for each one.

### Triggering

By default the collector does not run automatically. It can be triggered explicitly, or by setting an allocation threshold via `rt_gc_set_threshold(n)` (default 0 = disabled; negative values are clamped to 0). Crossing the threshold records coalescing allocation debt; it does not collect from inside `rt_heap_alloc()`. The VM services that debt after a native runtime call has completed, and native integrations can call `rt_gc_safepoint()` at an equivalent fully initialized boundary. At program shutdown, `rt_gc_run_all_finalizers()` uses an allocation-free epoch walk to run finalizers for currently tracked objects without performing a cycle-collection pass.

```c
int64_t freed = rt_gc_collect();  // Run one collection pass
rt_gc_set_threshold(1000);        // Request collection after 1000 allocations
rt_gc_safepoint();                // Service coalesced debt at a safe boundary
```

Exposed to Zanna programs as `Zanna.Runtime.GC.Collect()`, `SetThreshold(n)`,
and `GetThreshold()`.

### Thread Safety

The tracked-object table and weak-reference registry are protected by a global
mutex (`pthread_mutex_t` on Unix, `CRITICAL_SECTION` on Windows). A separate
managed-graph reader/writer barrier coordinates those tables with object
reference counts and container slots. Retain/release and structural mutators
enter a shared scope; a collection holds the exclusive scope while it snapshots,
traverses, and performs trial deletion. Other runtime threads can mutate the
graph concurrently with one another, but pause at this barrier during a
synchronous pass.

Finalizers and traversal callbacks run without the bookkeeping mutex to avoid
callback deadlocks. Collection traversal still owns the exclusive graph scope,
so a callback never enumerates storage that another mutator is resizing or
freeing. Shared scopes nest per thread, and collection requested from inside a
mutator is deferred to the next safe boundary instead of attempting an unsafe
lock upgrade. An active collection flag makes reentrant `rt_gc_collect()` calls
return 0 while a pass is still reclaiming objects.
If a trap occurs during collection after the active flag is set, temporary
collector state is released, retained snapshot entries are balanced, and the
active-collection flag is cleared before the trap is re-raised so later
collection passes can run. If a finalizer resurrects any member of an
unreachable garbage set, the collector restores the entire set's refcounts and
tracking entries instead of freeing only part of the object graph.

### Statistics

| Function | Returns |
|----------|---------|
| `rt_gc_tracked_count()` | Number of currently tracked objects |
| `rt_gc_total_collected()` | Cumulative objects freed by cycle collection |
| `rt_gc_pass_count()` | Number of `rt_gc_collect()` invocations |

`rt_gc_total_collected()` saturates at `INT64_MAX` rather than wrapping.
`rt_gc_shutdown()` resets the tracked-object table, weak-reference registry, and
public statistics so isolated runtime tests and embedders can restart from a
clean GC state.

### Limitations

- **Linear scan**: `find_entry()` is O(N) over the tracked-object array. Will
  degrade with thousands of tracked objects.
- **No automatic triggering by default**: cycles accumulate silently unless the
  program calls `GC.Collect()` or enables a positive threshold.
- **Reentrant collect is ignored**: `rt_gc_collect()` returns 0 if called while
  a collection pass is already active on the same process.
- **Synchronous**: stop-the-world; no concurrent or incremental mode.

---

## Weak References and Finalizers

### Zeroing Weak References

```c
rt_weakref *rt_weakref_new(target);     // Create (does NOT retain target)
void       *rt_weakref_get(ref);        // Dereference (NULL if target freed)
int8_t      rt_weakref_alive(ref);      // Check if target alive
void        rt_weakref_reset(ref, target); // Retarget an existing weak ref
void        rt_weakref_free(ref);       // Destroy handle (does not affect target)
```

Weak targets must be live runtime handles: `NULL`, heap objects, arrays, or
runtime strings. Raw foreign pointers are rejected so the weak-reference
registry never tracks memory it cannot zero safely.

The public runtime surface is `Zanna.Memory.WeakRef`. `New(target)` returns an
owned weak-reference object. Static-style calls (`Get(ref)`, `IsAlive(ref)`,
`Reset(ref, target)`, `Free(ref)`) and instance-style calls (`ref.Get()`,
`ref.IsAlive()`, `ref.Reset(target)`, `ref.Free()`) are both supported. `Get`
returns an owned strong reference to the current target or `NULL`; `Free`
consumes only the weak-reference object and never retains or releases the target.
Generic `Zanna.Runtime.Unsafe.Release(ref)` is also safe: weak-reference objects detach
from the registry in their finalizer before their storage is freed.

When a target object, array, or runtime string is freed,
`rt_gc_clear_weak_refs(target)` automatically nullifies all weak references
pointing to it. This is called from object, array, and string final-release paths
and from the GC collector after the object's finalizer has run and did not
resurrect the object. Weak references remain valid when resurrection succeeds.
Clearing a target also detaches the weak-reference chain links, so a cleared weak
reference can be safely reset to another target.

Object fields can also use the lightweight helpers:

```c
rt_weak_store(&slot, target);
void *target = rt_weak_load(&slot); // NULL once the target is freed
```

Implementation: hash table of per-target weak reference chains (64 buckets),
protected by the GC mutex.

### Finalizers

```c
rt_obj_set_finalizer(obj, fn);  // Install callback (one per object, replaces previous)
```

- Invoked from `rt_obj_free()` when refcount has already reached zero, and from
  cycle collection when a tracked cycle is reclaimed.
- Only works for `RT_HEAP_OBJECT` kind (not strings or arrays).
- Runs **before** the heap storage is freed.
- Weak references are cleared after the finalizer only when the object is still
  dead. A resurrecting finalizer preserves existing weak references.
- During cycle collection, outgoing references owned by unreachable objects are
  released after finalization while edges to other objects in the same garbage set
  are skipped.
- If any finalizer in an unreachable cycle resurrects an object, the whole
  garbage set is restored and re-tracked to avoid dangling references between
  surviving and collected cycle members. Non-resurrecting members whose
  finalizers ran during the aborted reclaim keep their finalizers installed so
  they still release resources when they are actually freed later.
- Finalizer traps during collection or shutdown finalizer sweeps re-raise the
  original trap after snapshot retains are balanced.
- Finalizer traps during direct `Zanna.Runtime.Unsafe.Release()` /
  `rt_obj_free()` also re-raise the original trap. If the object did not
  resurrect, the zero-ref payload is still untracked, weak refs are cleared, and
  heap storage is freed.
- Managed array cleanup clears each slot before releasing it. If an element
  finalizer traps, cleanup continues with later elements, frees the zero-ref
  array, and then re-raises the first trap.
- Calling `rt_gc_collect()` from a finalizer is safe but returns 0 while another
  collection pass is already active.

### Object Resurrection

```c
rt_obj_resurrect(obj);  // Set refcount from 0 → 1 (inside finalizer only)
```

Allows finalizers to prevent deallocation by resetting the refcount. After
`rt_obj_resurrect()`, `rt_heap_free_zero_ref()` observes a non-zero refcount and
skips deallocation. The caller must re-install the finalizer before returning the
object to users.

`Zanna.Runtime.Unsafe.Release()` reports this resurrected refcount to callers. A
finalizer that calls `rt_obj_resurrect()` changes the return value from the
transient zero to the restored live count. The compatibility
`Zanna.Runtime.Unsafe.Release()` entry point reports the same value.

**Use case**: Vec2/Vec3 thread-local pool recycling. When the pool has space, the
finalizer resurrects the object and pushes it back to the LIFO pool for reuse
without `malloc`/`free` overhead.

---

## Per-Type Ownership Reference

| Type | Allocation | Ownership | Element Management | Notes |
|------|-----------|-----------|-------------------|-------|
| **Strings** | Pool (≤512B) or malloc | Refcounted | N/A | Immortal literals, interning, `rt_str_concat` consumes both operands |
| **Lists** | malloc | Refcounted | Auto retain/release | `rt_list_i64` for unboxed ints (no per-element refcounting) |
| **Arrays** | malloc | Refcounted | Kind-specific retain/release | Object arrays use `RT_ELEM_OBJ`; string arrays use `RT_ELEM_STR`; `RT_ELEM_NONE` means no managed elements |
| **Sequences** | malloc | Refcounted | Borrowed by default; optional retained mode | Caller-managed by default; collection snapshots/conversions retain elements |
| **Maps** | malloc | Refcounted | Keys copied, values retained | String-keyed |
| **LazySeq** | malloc | **Manual destroy** | On-demand generation | Not refcounted; requires `rt_lazyseq_destroy()` |
| **Boxed values** | malloc | Refcounted | Type-tagged (I64/F64/I1/STR) | Runtime class id `Zanna.Core.Box`; unbox does not consume the box; box helpers validate class id, heap kind, and payload size |
| **Objects** | malloc | Refcounted | Optional finalizer | Optional GC tracking for cycles |
| **Vec2/Vec3** | Thread-local pool (cap 32) | Refcounted + resurrection | Immutable values | Pool recycling via finalizer |
| **Files** | Stack (`RtFile`) | Caller-owned | POSIX fd | Manual close or finalizer-based cleanup |
| **Network** | malloc | Refcounted | N/A | Manual `close()` available |
| **GUI Widgets** | malloc | Refcounted (vgfx) | Widget tree | Managed by GUI framework |
| **LRU Cache** | malloc | Refcounted | Values are retained while cached | Finalizer frees internal nodes and bucket array |
| **WeakMap** | malloc | Refcounted | Keys retained, values **weak** | Values may be collected independently |

### Strings

- Created via `rt_string_from_bytes(bytes, len)` — heap-backed, refcount=1
- Created via `rt_const_cstr(literal)` — immortal wrapper, never freed
- Pool-allocated when total size (header + payload) ≤ 512 bytes
- `rt_string_intern(s)` returns the canonical pointer; enables O(1) pointer
  equality. The intern table retains its own reference.
- `rt_str_concat(a, b)` **consumes both operands** (releases a and b). May
  append in-place if `a` is uniquely owned with sufficient capacity.
- UTF-8 encoded, null-terminated. Byte-based indexing (not codepoint).

### Lists

- Elements are retained on store (`push`, `set`), released on removal
  (`remove_at`, `clear`, destructor).
- The list itself is refcounted.
- `rt_list_i64` is a typed variant for unboxed int64 elements with no
  per-element refcounting — separate `len`/`cap` via `rt_heap_set_len`.
- Backing array grows geometrically (doubling).

### Sequences (Seq)

- The container itself is refcounted.
- Plain `rt_seq_new()` sequences borrow elements by default. Placing
  refcounted objects into a borrowed Seq without retaining them remains unsafe
  if the Seq can outlive those elements.
- Runtime collection snapshots and conversions can enable retained-element
  mode before inserting values. Owned Seq instances retain on push/insert/set
  and release on clear/finalize. `Slice`, `Clone`, `Take`, `Drop`, `Keep`,
  `Reject`, `TakeWhile`, and `Iterator.ToSeq` preserve retained ownership when
  their source owns elements.

### LazySeq

- **Not refcounted** — caller owns the handle.
- Must be destroyed with `rt_lazyseq_destroy()`.
- Zanna has no `using`/`Dispose` pattern, so this requires manual lifecycle
  management in user code.
- Lazy sequences can be infinite; collector operations (`ToSeq`, `Count`) may
  not terminate.

### Mixed Ownership in Caches

- **LRU Cache**: GC object containing a `malloc`'d bucket array and node chain.
  Values are retained while cached and released on overwrite, eviction, removal,
  clear, or finalization. A capacity of `0` means unbounded cache growth; it
  disables automatic LRU eviction.
- **WeakMap**: GC object with `malloc`'d hash table. Keys are retained; values
  are weak runtime-managed objects or strings. `Get` promotes a live weak value
  to a retained reference and returns `NULL` after the value has been collected.

---

## Compiler Lifetime Emission

This section documents what each compiler frontend does (and doesn't do) about
object lifetimes. This is the most important section for understanding the
soundness of Zanna programs.

### BASIC Frontend

The BASIC lowerer (`src/frontends/basic/lower/Emitter.cpp`) emits explicit
retain/release calls:

- **Strings**: `deferReleaseStr(v)` queues a temporary for cleanup.
  `releaseDeferredTemps()` emits `rt_str_release_maybe` at scope boundaries.
- **Objects**: `deferReleaseObj(v, className)` queues objects.
  `rt_obj_release_check0` + `rt_obj_free` emitted at scope boundaries.
- **Arrays**: `emitArrayRelease()` emits type-specific release calls
  (`rt_arr_str_release`, `rt_arr_obj_release`, `rt_arr_f64_release`,
  `rt_arr_i64_release`) at function exit.
- **Array stores**: retain new value, release old value (`emitArrayStore`).
- **Object assignment**: `rt_obj_retain_maybe` on new value, `rt_obj_release_check0`
  + `rt_obj_free` on old value.

**Result**: BASIC programs have well-managed lifetimes. Temporaries are tracked
and released at scope boundaries. Arrays and objects are cleaned up at function
exit.

### Zia Frontend

Zia lowering makes ownership explicit in the emitted IL, following the managed-value
convention established by
[ADR 0147](adr/0147-managed-reference-lowering-and-native-retain-elision.md):

- **Managed local slots** own exactly one reference to their non-null contents, for
  both mutable and immutable bindings. Initialization and assignment move a deferred
  owned temporary when one exists and otherwise retain a borrowed value; assignment
  releases the displaced value.
- **Managed parameters** are borrowed at entry. The callee retains each into its
  owning slot (`rt_obj_retain_maybe`) and releases that slot on every exit.
- **Managed returns** transfer exactly one reference. The caller moves it into
  another owner, passes it to a consuming operation, or schedules one
  statement-boundary release.
- **Lexical cleanup** releases every managed slot introduced by a scope, including
  shadowed bindings. `break`, `continue`, and `return` release the iteration and
  lexical owners they exit.
- **Conditional edges** release temporaries created on only one edge before leaving
  it; ternary, value-`if`, coalesce, and optional expressions merge managed results
  through an owning slot.
- Runtime calls annotated `consumedArgMask` / `returnsOwned` in
  `RuntimeOwnershipEffects` override the borrow-by-default convention.

Object cleanup lowers to `rt_obj_release_check0` plus a conditional destroy block;
string temporaries lower to `rt_str_release_maybe`. You can see both directly:

```sh
zanna build program.zia -o /dev/stdout | grep -E 'retain|release'
```

**Result**: ordinary Zia programs balance their own allocations. Reference cycles
still need the cycle collector (see [Known Unsoundness](#known-unsoundness)).

### VM (Runner.cpp)

The VM has minimal explicit lifecycle management — only one `rt_string_unref`
call (for command-line argument passing). The VM relies on runtime functions to
manage their own internal retain/release.

---

## Known Unsoundness

Severity-ordered list of memory management gaps:

### 1. HIGH: No Automatic GC Triggering

The cycle collector only runs when explicitly called via
`Zanna.Runtime.GC.Collect()`. Programs that create cyclic object graphs (e.g.,
doubly-linked lists, parent-child class references) without calling
`GC.Collect()` will leak those cycles indefinitely.

### 2. HIGH: Borrowed Seq Elements Require Explicit Lifetime Management

Plain `rt_seq_new()` uses borrowed-element mode. This still creates two hazards
when callers insert refcounted values without retaining them:
- **Dangling references**: an element may be freed while still referenced by the
  Seq.
- **Leaks**: callers that retain before insertion must release those references
  when they are no longer needed.

Collection APIs that return snapshots, such as `Map.Values`, `Set.Items`, and
the `ToSeq` conversion helpers, use retained-element mode so the returned Seq
keeps its values alive independently of the source collection.

### 3. MEDIUM: LazySeq Requires Manual Destroy

LazySeq handles are not refcounted and require explicit `destroy()` calls.
Zanna has no `using`/`Dispose` language-level pattern, making it easy to
forget cleanup.

### 4. MEDIUM: Pool Memory Never Returned to OS

The slab allocator retains all allocated slabs for the process lifetime.
For long-running processes that create many short strings early, this memory
remains allocated even if never used again. `rt_pool_shutdown()` is called
at process exit via the `atexit` handler (see §Shutdown Cleanup below) but
not during normal execution.

### 5. Shutdown Cleanup

An `atexit` handler (`rt_global_shutdown` in `rt_heap.c`) is registered on first
heap allocation. It runs the following cleanup in order:

1. `rt_gc_run_all_finalizers()` — runs finalizers on all GC-tracked objects
   (flushes files, closes sockets, releases audio/GPU handles)
2. `rt_audio_shutdown()` — destroys the audio device (idempotent no-op if
   audio was never initialized or already shut down)
3. `rt_legacy_context_shutdown()` — closes BASIC file channels, releases
   argument storage and type registry held by the static legacy context
4. `rt_string_intern_drain()` — frees the interned string table
5. `rt_gc_shutdown()` — frees the GC tracking hash table and weak-ref buckets
6. `rt_pool_shutdown()` — frees all pool slabs

This runs on normal non-Windows runtime `exit()` paths. Windows runtime builds
skip CRT `atexit` registration because the same archive is used by native PE
binaries that enter through Zanna's CRT-less startup shim; those builds rely on
process teardown, and `rt_env_exit()` uses `ExitProcess` there for the same
reason. The stack-overflow handler uses `_exit(1)` and intentionally bypasses
cleanup (stack is blown; running arbitrary code is unsafe).

### 6. LOW: Interned Strings Are Immortal

The intern table retains strings forever during normal execution. Programs
that intern many unique strings will see monotonically growing memory.
`rt_string_intern_drain()` is called at process exit via the `atexit`
handler.

### 7. LOW: GC Tracking Uses Linear Scan

`find_entry()` in `rt_gc.c` is O(N) over the tracked-object array. This will
degrade with thousands of tracked objects.

---

## Quick Reference

### Allocation & Lifetime API

| Operation | API Call | Notes |
|-----------|---------|-------|
| Allocate string | `rt_string_from_bytes(bytes, len)` | refcount=1; pool if ≤512B |
| Allocate object | `rt_obj_new_i64(class_id, size)` | refcount=1 |
| Retain | `rt_heap_retain(p)`, `rt_memory_retain(p)`, or `rt_string_ref(s)` | Atomic increment; immortal values are unchanged |
| Release | `rt_heap_release(p)`, `rt_memory_release(p)`, or `rt_string_unref(s)` | Frees at zero; public memory release reports any finalizer resurrection count |
| Deferred release | `rt_heap_release_deferred(p)` then `rt_heap_free_zero_ref(p)` | Two-step pattern |
| Set finalizer | `rt_obj_set_finalizer(obj, fn)` | Objects only; one per object. ValueType managed-field registration preserves and chains an existing finalizer. |
| Resurrect | `rt_obj_resurrect(obj)` | Inside finalizer only; 0→1 |
| Create weak ref | `rt_weakref_new(target)` | Does NOT retain target |
| Read weak ref | `rt_weakref_get(ref)` | Returns NULL if target freed |
| Track for GC | `rt_gc_track(obj, traverse_fn)` | Required for cycle detection |
| Collect cycles | `rt_gc_collect()` | Explicit and synchronous; returns count freed |
| Service allocation debt | `rt_gc_safepoint()` | Runs at most one coalesced automatic pass outside allocator construction |
| Intern string | `rt_string_intern(s)` | Returns canonical pointer |
| Mark disposed | `rt_heap_mark_disposed(p)` | Debug aid; atomic flag |
| Pool stats | `rt_pool_stats(class, &alloc, &free)` | Per-class diagnostics |

### Ownership Rules

1. Every `rt_heap_alloc` / `rt_obj_new_i64` / `rt_string_from_bytes` returns
   ownership (refcount=1). The caller is responsible for releasing.
2. `rt_heap_retain` / `rt_string_ref` to share ownership.
3. `rt_heap_release` / `rt_string_unref` to relinquish ownership.
4. The last release frees the object.
5. Immortal strings (literals, interned) skip the retain/release cycle entirely.
6. **List, Map, Set, Deque, and most specialized collection values** are
   auto-managed by their containers (retained on store, released on removal).
7. **Plain Seq, Stack, Queue, and Ring elements** are borrowed by default.
   Runtime conversion and snapshot helpers enable retained-element mode where
   needed, except Ring itself remains a borrowed circular buffer.
8. The cycle GC only helps objects explicitly registered via `rt_gc_track`.
9. `rt_str_concat` consumes both operands — do not use `a` or `b` after calling.

---

## Glossary

| Term | Definition |
|------|-----------|
| **Immortal string** | A string with `refcnt >= RT_HEAP_IMMORTAL_REFCNT` (`SIZE_MAX - 1`); never freed. Created by `rt_str_from_lit()` or `rt_const_cstr()`. |
| **Pool-allocated** | Memory sourced from the slab allocator. Identified by `RT_HEAP_FLAG_POOLED` (bit 1) in the header flags. |
| **Trial deletion** | The algorithm used by the cycle GC: temporarily decrement refcounts to identify objects only reachable through cycles. |
| **Object resurrection** | Re-arming an object's refcount from 0→1 inside a finalizer, preventing deallocation. Used for pool recycling. |
| **Deferred release** | Decrementing the refcount without immediately freeing, allowing cleanup code to run while the allocation remains valid. |
| **Tagged pointer** | A pointer with metadata (version counter) packed in unused upper bits. The pool retains tagged-pointer helpers for experimentation, but its freelists use a per-size-class spinlock instead. |
| **Consume semantics** | A function that releases its operand references and returns a new reference. Callers must not use the operands after the call. |
