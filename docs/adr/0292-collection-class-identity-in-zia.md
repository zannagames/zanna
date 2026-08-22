---
status: active
audience: contributors
last-verified: 2026-08-22
---

# ADR 0292: Enforce Runtime Collection Class Identity in Zia

## Status

Accepted

## Context

`Zanna.Collections.*` classes are distinct runtime classes with distinct heap
class ids (`src/runtime/collections/rt_collection_ids.h`), and every receiver
check is an exact class-id comparison — `rt_obj_is_instance` performs no
hierarchy walk. A mismatched handle therefore cannot be type-erased away; it
becomes a trap such as `Seq: invalid Seq object`, and the default handler
`rt_abort` prints that bare message and calls `_Exit(1)`. To a user this reads
as an unexplained crash, attributed to whichever accessor happened to touch the
handle rather than to the call that supplied it.

Three separate holes let a wrong collection reach a receiver:

1. **Assignability.** Every runtime class is modelled as a *named* `Ptr`. The
   rule written for the type-erased *unnamed* `Ptr` accepted any reference type,
   so a `Seq` parameter silently accepted a `List`, a `Map`, or any class.
2. **Static receiver form.** `Zanna.Collections.Seq.*` declared its receiver as
   `obj`, and runtime extern calls skipped argument checking entirely, so
   `Seq.get_Count(anything)` type-checked.
3. **`as` casts.** `Any` could not be narrowed at all, which is why `Any`
   parameters proliferated; and narrowing to a runtime class emitted no check,
   so a cast was a blind assertion.

A live instance of hole 1 shipped in Studio: the Save-All preflight reports
written files as a `Collections.List`, which reached a `Seq.get_Count` consumer
and aborted Studio on every Build and Build-and-Run with a project open.

## Decision

Collection class identity is enforced by the Zia frontend, and narrowing casts
are verified at runtime.

- **Assignability** (`src/frontends/zia/Types.cpp`): a `Zanna.Collections.*`
  target accepts only the same collection class. Non-collection runtime classes
  keep the historical permissive rule, because Zia does not model the runtime
  GUI class hierarchy and a `FloatingPanel` really is a `Widget`.
- **Receiver typing** (`src/il/runtime/defs/api/collections.def`): the 42
  `Zanna.Collections.Seq.*` entries that take a receiver declare it `seq<obj>`
  instead of `obj`; `PushAll` declares its second parameter `seq<obj>`; and
  `New`, `NewSized`, and `WithCapacity` return
  `obj<Zanna.Collections.Seq>` instead of bare `obj`. `seq<…>` and `obj` both
  lower to IL `Ptr` and both are managed-object parameter tokens, so the emitted
  IL, the verifier rules, and the runtime C ABI are unchanged; only the declared
  Zia-visible type changes.
- **Extern argument checking** (`src/frontends/zia/Sema_Expr_Call.cpp`): a
  runtime extern call written in explicit-receiver form is type-checked when its
  argument count equals the declared parameter count. The implicit-receiver
  method form, where surface and symbol arity differ by one, is untouched, and
  arity itself is still not policed for externs.
- **`Any` narrowing** (`src/frontends/zia/Sema_Expr_Advanced.cpp`): `Any` may be
  narrowed with an explicit `as` cast. Implicit assignment from `Any` remains an
  error.
- **Checked casts** (`rt_cast_runtime_class`, wired in the Zia lowerer): casting
  to a runtime collection class verifies the heap class id. Null narrows to
  null so nullable runtime handles keep their `== null` guards; a mismatch traps
  naming both the expected and the actual class. The name-to-id bridge lives in
  `il::runtime::runtimeCollectionClassId` because frontends may not include
  runtime headers.

## Consequences

- A wrong collection is a compile error at both the parameter boundary and the
  static receiver form, instead of a process abort at an unrelated accessor.
- `Any` stays legal where a value is genuinely dynamic, but crossing back to a
  concrete type is explicit, greppable, and now verified at runtime.
- Trap text names both classes (`Cast: expected Zanna.Collections.Seq, got
  Zanna.Collections.Map`) rather than the accessor that tripped over the handle.
- Sources that passed an untyped `obj` into the static receiver form must type
  the declaration or narrow with `as`. Studio required 91 parameter slots and
  67 declarations to be typed; `baseball` needed none.
- Only `Seq` receivers are tightened. The other collection families keep `obj`
  receivers, so their static forms stay unchecked; the assignability and cast
  rules already cover them, and widening the receiver change is a mechanical
  follow-up.

## Alternatives Considered

Tightening assignability for *all* named runtime classes rejects real code,
because Zia has no model of runtime class inheritance and `Result.Unwrap()`
does not resolve to its payload type; that needs a hierarchy in the registry
first. Making `as` a null-returning cast rather than a trapping one would have
made a failed narrowing indistinguishable from a legitimate null, since runtime
collection handles are nullable. Leaving the receiver as `obj` and relying only
on parameter typing would have left the static form — the spelling used
throughout Studio and `baseball` — unchecked.
