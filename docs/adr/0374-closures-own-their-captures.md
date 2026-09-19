---
status: accepted
audience: contributors
last-verified: 2026-09-18
---

# ADR 0374: Closures Own Their Captures

## Status

Accepted (2026-09-18). Changes how the Zia frontend represents function values
(lambdas and `&function` references) and their memory ownership. The IL, the
runtime C ABI and the closure call convention are unchanged.

## Context

A Zia function value is a closure record `[code pointer, environment pointer]`;
a call loads both and passes the environment as the callee's first argument.
Lambdas built the record and their environment with `rt_alloc`, plain
`malloc` memory that nothing ever freed, and copied captured strings and
objects into the environment without taking a reference. When the function
that created a lambda returned, its scope released those values, so any lambda
that outlived its creator — a returned lambda, a handler stored in a field or a
list — read freed memory. A lambda capturing a local list reported another
list's length after the memory was reused; native builds trapped with
`List: invalid List object` (Legacy Baseball ledger ZB-67).

Function-typed values were also invisible to ownership: `needsRelease` did not
cover function types, while the boxed value-type and class-layout flatteners
(`collectManagedSlots`) already treated function-typed fields as object
references, so boxing a struct holding a function value asked the runtime to
retain `malloc` memory. `&function` references wrapped as Zia function values
(the defect-audit #26 fix, ZB-64) had the same allocation.

## Decision

- **A closure is a reference-counted runtime object.** Lambdas and wrapped
  `&function` references allocate with `rt_obj_new_i64` (the allocation path
  of class instances). The payload keeps the call convention — code pointer at
  offset 0, environment pointer at offset 8 — and stores the captured values
  inline after those two words; the environment pointer points at them (null
  when nothing is captured).
- **The closure owns its captures.** Creating a closure retains every captured
  string and managed object (`self` included, when a method's lambda names it
  or one of its members). Inside the lambda, captures and managed parameters
  are copied into slots that own a reference each, and every exit — the body's
  end or a `return` — releases them through the function exit sequence.
- **Destruction through the class-destructor hook.** A lambda with managed
  captures gets its own class id (from the same counter as classes) and a
  synthesized `<lambda>.__dtor(self)` that releases the captures;
  `__zia_dtor_dispatch` routes a dying closure there exactly like a class
  instance (ADR 0313). A closure without managed captures, and every wrapped
  `&function` reference, uses class id 0 and needs no destructor.
- **Cycle collection.** A capturing closure's captured objects are registered
  as strong slots in `__zia_layout_init` (ADR 0315), so a cycle through a
  closure — an object holding a lambda that captures the object — is
  reclaimed by `Zanna.Runtime.GC.Collect()`.
- **Function values are managed references.** `needsRelease` covers function
  types, so locals, fields, globals, collections, arguments and returns of
  function type follow the ordinary retain/release rules.
- Runtime callbacks are unaffected: an `&function` passed directly to a runtime
  API is still the raw code address the runtime calls.

## Consequences

- Returned and stored lambdas are memory-safe, and a closure's captures are
  released when the closure dies; closures no longer leak.
- Each closure creation is one runtime object allocation (previously one or
  two `malloc`s). Capturing a value costs one retain at creation and one per
  call (the lambda's own slot), balanced by releases.
- Regression coverage: `src/tests/fixtures/zia_runtime/63_closure_ownership.zia`
  (captured list, string and `self` outliving their creators, 200 stored
  closures, a capture freed with its closure, a closure cycle surviving
  reference counting and reclaimed by the collector), run on the VM and at
  native -O1/-O2.
- `docs/memory-management.md` describes the rule and no longer lists closure
  environments as untraversed.
