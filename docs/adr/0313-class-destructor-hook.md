# ADR 0313: Class destructor hook and synthesized destructors

- Status: Accepted
- Date: 2026-09-01
- Scope: Zia frontend (`Lowerer`), runtime object ABI (`rt_object`), bytecode VM and tree-walking VM bridges
- Related: ADR 0311 (depth-only shading), plan 88 (probe memory)

## Context

Legacy Baseball's shot-capture probe rebuilt a headless stage per capture and
peaked at 14.8 GB of resident memory, while the engine's own world create/destroy
cycle retains about 6 MB. Attributing the live allocations by call stack showed
whole dead stages surviving: animation controllers, meshes, software-rasterizer
contexts with their thread pools, and every list the stage owned.

Two Zia language defects caused this, and both apply to every Zia program:

1. The lowerer only emitted `<Type>.__dtor` for classes that declared `deinit`.
   A class without `deinit` released **none** of its reference fields (strings,
   objects, collections, boxed `Any`, runtime handles) when its last reference
   dropped. A ten-line program that churns objects holding a `List[Integer]`
   grew without bound on both the VM and native binaries.
2. Even for classes with `deinit`, the destructor ran only when *compiled code*
   dropped the last reference (`rt_obj_release_check0` → `__zia_dtor_dispatch`
   → `rt_obj_free`). When the **runtime** dropped the last reference (a list of
   objects being released, a map value overwritten, a boxed `Any`), the payload
   was freed by C code that knows nothing about Zia destructors, so the
   object's fields leaked and its `deinit` body never ran.

## Decision

### Synthesized destructors

`Lowerer::lowerClassDecl` synthesizes `<Type>.__dtor` for every class that
needs one: a user `deinit`, any releasable field (string, pointer, or weak,
own or inherited), or a base class that needs a destructor. Value-only classes
still get none.

A derived destructor releases only the fields the class adds and then calls
`Base.__dtor(self)`; the base releases its own fields (and runs its `deinit`
body). Previously a derived instance never ran the base body and a user-written
derived `deinit` released inherited fields itself; the chain now runs each
body and releases each field exactly once.

### Runtime class destructor hook

```c
typedef void (*rt_obj_class_dtor_hook_t)(void *obj);
void  rt_obj_set_class_dtor_hook(void *fn);   /* fn: rt_obj_class_dtor_hook_t or NULL */
void *rt_obj_get_class_dtor_hook(void);
```

`rt_obj_free` (the only zero-reference object reclaim path) invokes the hook
for every object payload whose header carries a **positive** class id, before
any per-object finalizer, inside the same trap-recovery scope finalizers use.
Runtime-internal objects use class id 0 (closure environments) or negative ids
and never reach the hook. The hook is one process-wide pointer; a program
installs it once.

The compiler emits `rt_obj_set_class_dtor_hook(@__zia_dtor_dispatch)` as the
first statement of the entry point (`start`/`main`), before interface tables
and global initializers. Compiled release sites no longer call the dispatcher
directly; `rt_obj_free` dispatches for compiled and runtime-internal releases
alike, so there is exactly one path.

`__zia_dtor_dispatch` now dispatches by binary search over the sorted class
ids (O(log n) compares). With a destructor on nearly every class the linear
chain would have cost hundreds of compares per object death.

### Executor bridges (VM and native)

- **Native binaries**: `@__zia_dtor_dispatch` is a real function address; the
  runtime calls it directly.
- **Bytecode VM**: function values are tagged module indices, not code. The
  VM's unified handler for `rt_obj_set_class_dtor_hook` resolves the tagged
  value to a `BytecodeFunction` and installs a C trampoline that runs it with
  `invokeVoidReentrant` (the same bridge `Parallel.For` and pool tasks use).
  The trampoline checks that the active module is the one that registered the
  dispatcher and that a VM is active on the calling thread; otherwise it does
  nothing (a leak, never bytecode executed off its VM).
- **Tree-walking VM**: same shape through `resolveEntryFunction` and
  `VMAccess::callFunction`.

### Follow-up

Making destruction real exposed the second half of the leak: the compiler
guessed from names whether a *runtime* call's result was owned. ADR 0314
replaces that guess with a declaration on every reference-returning
`runtime.def` row.

### Not changed

Object-to-object cycles are still not collected: class instances are not
registered with the cycle collector (only reference-bearing arrays and runtime
containers are), so a strong cycle between two plain objects lives forever
unless one edge is `weak`. That is a separate design item; this ADR fixes
acyclic ownership, which was the actual cause of the probe memory growth.

## Consequences

- Every Zia program frees what it owns. Long-running programs (games, servers,
  probes) that previously grew without bound no longer do.
- Destruction is now real: code that relied on leaked objects staying alive
  (for example, an engine handle held only by a dead object) will observe the
  release. Such cases are bugs in the program's ownership, not in this change.
- `Zanna.Runtime.Unsafe.Release` and every compiled release site emit one call
  less (`rt_obj_free` only).
- The runtime C ABI gains two functions (`rt_obj_set_class_dtor_hook`,
  `rt_obj_get_class_dtor_hook`) and one IL-visible descriptor row
  (`rt_obj_set_class_dtor_hook`, `void(ptr)`, manual lowering).

## Tests

- `test_zia_destructors`: synthesized destructor for reference fields, none
  for value-only classes, derived → base chaining (own fields only, base body
  reached), hook installation at entry and no direct dispatcher call.
- `test_rt_object_class_dtor_hook`: hook runs for positive ids before the
  finalizer, skips zero/negative ids, runs for array-element releases, stops
  when cleared.
- `zia_runtime_test_object_field_release` (VM) and
  `native_run_zia_object_field_release` (native): tracked-object count returns
  to baseline across locals, nulling, expression statements, returned values,
  externally assigned fields, objects inside lists, and a class hierarchy whose
  base `deinit` must run exactly once per derived instance.
