---
status: accepted
audience: contributors
last-verified: 2026-09-14
---

# ADR 0359: BASIC Managed Values and Non-Null Strings

## Status

Accepted. BASIC lowering adopts the managed-value convention ADR 0147 defines for Zia, and a
BASIC `STRING` is never null. Refines the `rt_arr_str_get` runtime contract.

## Context

ADR 0147 made reference ownership explicit between Zia lowering and the native backends:

- a managed slot owns one reference;
- managed parameters are retained into owning slots and released on every exit;
- returns transfer one reference.

Native code has no register teardown, so any reference the IL does not release explicitly lives
forever. BASIC predated that convention and broke it in several ways.

**Leaks measured in native loops of one million iterations:**

| Pattern | Growth |
|---|---|
| String locals in a SUB called in a loop | about 320 MB |
| String `FUNCTION` results assigned through the function name | about 320 MB |
| `NEW` objects passed straight to a call or stored in an array element | about 115 MB |
| Temporary results of `UCASE$`, `LEFT$` and every other string built-in | not released |
| Array element reads (`rt_arr_str_get` and `rt_arr_obj_get` return retained references) | not released |
| `FOR EACH` element bindings | not released |

**Use after free.** String and object parameters were stored raw. Assigning to a parameter
released the caller's value, so the caller's variable held a freed handle: `invalid string
handle` for strings, or a destructor that ran on the caller's object.

**Null strings.** Several kinds of STRING storage started as null:

- module variables read from a procedure;
- `STATIC` locals;
- `STATIC` fields;
- instance fields;
- array elements.

`rt_str_eq(NULL, "")` is false and `LEFT$(NULL, 1)` traps, so `IF name$ = "" THEN` was false for
a STRING that was never assigned, although `LEN` reported 0.

## Decision

### Slots and temporaries

- A STRING or object slot owns exactly one reference: variables, parameters, fields, the
  function-name result slot, a `USING` resource variable, and a `FOR EACH` element variable.
- Assignment moves a temporary queued for statement cleanup into the slot. It retains any other
  value. The displaced value is released.
- These results are owned temporaries, released at the statement boundary unless an assignment,
  a `USING`, or a `RETURN` takes them:
  - runtime call results, as their catalog row declares (ADR 0314), including every string
    built-in;
  - `NEW` results (the creation reference, or the runtime constructor's owned result);
  - string and object array element reads.
- A `FOR EACH` iteration releases the previous element before binding the next.

### Procedures

- A BYVAL STRING or object parameter is retained into its slot at entry. BYREF parameters use
  the caller's storage and are not retained. Array parameters stay borrowed (BUG-105).
- Every exit releases the procedure's STRING and object slots, parameters included:
  - the exit block;
  - `RETURN` with a value;
  - a bare `RETURN` in a SUB, which now branches to the exit block;
  - `EXIT`;
  - method, constructor, and destructor epilogues;
  - the `@main` epilogue.
- A FUNCTION returning through its name moves the slot's reference to the caller with
  `load` followed by a raw pointer null store, which ADR 0147 defines as a move.

### STRING is never null

- Constructors store `""` in every scalar STRING field of the class layout, inherited fields
  included.
- The lowerer records every STRING module-variable key it addresses: cross-procedure globals,
  `STATIC` locals (`PROC.NAME`), and `STATIC` fields (`Class.__static.FIELD`, also seeded from
  the OOP index). `@main` stores `""` in each before `__mod_init$oop` runs. The module-variable
  runtime stays null-initialized because Zia's `String?` globals must start null.
- `rt_arr_str_get` returns the immortal empty string for a slot that was never written. That
  covers new arrays and `REDIM` growth. String arrays have no other runtime client, and the file
  already stated the invariant that empty slots read as empty strings. The C signature is
  unchanged.

## Consequences

- The one-million-iteration native probes above now stay at about 1.8 MB resident, and
  destructors run when the last reference drops (for example right after `Take(NEW Box())`).
- Assigning to a parameter no longer corrupts the caller's value on any backend.
- IL grows by one load and one release per STRING or object local at each procedure exit, plus
  one retain per such parameter at entry. The IL goldens were regenerated.
- **VM divergence (not addressed here).** Both VMs give each string register its own reference
  and make `store str` retain the incoming value and release the current one. The IL both
  frontends emit follows the native explicit convention instead. Some patterns keep one extra
  reference in the VMs:
  - an explicit retain followed by a store (a borrowed copy such as `t = u`);
  - a parameter retain;
  - a string held only by a slot at exit.

  BASIC's move of owned temporaries avoids this for the common `t = <expression>` case, and Zia
  shows the same growth for borrowed copies. Aligning the VMs' string ownership with the native
  convention is separate work.

Tests:

- `basic_runtime_test_basic_managed_ownership` and its native twins at the default level and
  `-O0`. The fixture observes lifetimes through `Zanna.Memory.WeakRef`.
- `test_basic_lowerer_string_assignment` (exit release) and `test_rt_array_str` (unset elements
  read as `""`).
- The regenerated `basic_to_il` goldens.
