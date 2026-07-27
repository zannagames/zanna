---
status: active
audience: contributors
last-verified: 2026-07-26
---

# BASIC Frontend Grammar Notes

This document records small grammar extensions supported by the BASIC frontend so contributors know what to expect when
reading or writing tests.

## Namespaces

Namespace grammar and semantics (NAMESPACE declarations, USING directives,
type-resolution precedence, reserved namespaces, and the E_NS_* diagnostics)
are specified in the [BASIC Namespace Reference](basic-namespaces.md).

## Access Modifiers (CLASS members)

- Scope and default:
    - Inside `CLASS … END CLASS`, a single `PUBLIC` or `PRIVATE` keyword applies to the next member only (single‑use
      prefix).
    - If no access modifier is present, the default is `PUBLIC`.

- Supported member forms affected by access:
    - Field declarations: `DIM v AS TYPE` or `v AS TYPE`.
    - Methods and constructors/destructors: `SUB … END SUB`, `FUNCTION … END FUNCTION`, `DESTRUCTOR … END DESTRUCTOR`.

- Enforcement:
    - `PRIVATE` members may only be accessed from within the declaring class.
    - The frontend enforces this during lowering and emits a diagnostic when violated.

## Classes, inheritance, and method modifiers

- Declaration:

  CLASS B : A
  [PUBLIC|PRIVATE] [VIRTUAL] [OVERRIDE] [ABSTRACT] [FINAL] SUB M(...)
  ... optional body ...
  END SUB
  END CLASS

    - Single inheritance only: `CLASS B : A` declares `B` with base `A`. The base may be qualified, e.g. `Namespace.A`.
    - Parser note: Some documentation may refer to `INHERITS A` as a descriptive synonym for readability. The BASIC
      frontend syntax is `:` for inheritance.
    - Method modifiers are per‑member (single‑use) prefixes in any order:
        - `VIRTUAL`: introduces a vtable slot for dispatch.
        - `OVERRIDE`: marks an override of a base virtual method; signature must match.
        - `ABSTRACT`: forbids a body; the method must be implemented by a non‑abstract derived class.
        - `FINAL`: disallows further overrides in derived classes.
    - Constructors are declared as `SUB NEW(...)` and cannot carry the above modifiers.

## Destructors and Disposal

### Class destructors

- Instance destructor (no params/return):

  ```basic
  DESTRUCTOR
    ' body
  END DESTRUCTOR
  ```

- Static destructor (no params):

  ```basic
  STATIC DESTRUCTOR
    ' body
  END DESTRUCTOR
  ```

Constraints:

- At most one instance destructor per class.
- At most one static destructor per class.
- No access modifiers on destructors.
- Destructors are not allowed in `INTERFACE` declarations.

### DELETE statement

- Explicit disposal of an object reference:

  ```basic
  DELETE <expr>
  ```

`<expr>` must evaluate to an object handle; deleting `NULL` is a no‑op. The keyword is `DELETE`
(parser handler `Parser::parseDeleteStatement` in `Parser_Stmt_OOP.cpp`); there is no `DISPOSE`
keyword in the BASIC frontend.

- Base‑qualified call:

    - `BASE.M(...)` calls the base implementation directly, bypassing virtual dispatch. Parsing treats `BASE` as a
      special receiver token; lowering substitutes the current instance (`ME`) as the first argument when emitting the
      direct call.

## Structured Error Handling (TRY/CATCH)

### TRY ... CATCH ... END TRY

Lexed keywords: `TRY`, `CATCH`, `FINALLY`, `END`.

```basic
TRY
  ' protected statements
CATCH [errVar]
  ' handler (errVar is optional i64 variable holding error code)
END TRY
```

- `CATCH` without a variable is valid; use `ERR()` inside the block to retrieve the code.
- `FINALLY` is supported: the finally block always executes after try/catch regardless of whether an exception occurred. The semantic analyzer visits FINALLY statements, so unresolved names and type errors there are reported. At least one of CATCH or FINALLY must be present.
- TRY/CATCH composes with `ON ERROR GOTO`: a TRY installs a handler on top of any active `ON ERROR` handler and pops it at `END TRY`.

### USING ... END USING

`USING` resource blocks require an object/resource initializer:

```basic
USING r AS Resource = NEW Resource()
  ' body
END USING
```

Scalar initializers such as `USING r AS Resource = 42` are rejected during semantic analysis.

Lowering installs a scoped exception handler for the block. On normal fallthrough, the
handler is popped and the resource is released/destroyed before execution continues. If
the body traps, the same cleanup path runs and the original exception token is resumed so
outer `TRY`/`ON ERROR` handlers still see the failure.

## Interfaces and conformance

- Declaration:

  INTERFACE I
  SUB M(a AS I64)    # abstract signatures only; no bodies
  FUNCTION F() AS I64
  END INTERFACE

    - Interface members are abstract signatures (SUB/FUNCTION) without bodies.
    - Slot assignment follows declaration order and is used for interface dispatch.

- Implementing interfaces:

  CLASS C [ : Base ] [IMPLEMENTS I1 [, I2 ...]]
  OVERRIDE SUB M(a AS I64)
  ' ... body ...
  END SUB
  END CLASS

    - A class may implement multiple interfaces via an `IMPLEMENTS` list; each declared interface method must be
      provided with a compatible signature. Missing or mismatched members trigger a conformance diagnostic.
    - Slot assignment equals the interface declaration order and is used to build per‑interface itables bound by each
      implementing class.

## Runtime type tests and casts (RTTI)

- Expressions:
    - `expr IS Type` → BOOLEAN
        - If `Type` is a class, tests whether the dynamic type of `expr` is that class or a derived class.
        - If `Type` is an interface, tests whether the dynamic type of `expr` implements that interface.
    - `expr AS Type` → pointer/reference value or NULL
        - If `Type` is a class, returns `expr` when the dynamic type is that class or a derived class; otherwise returns
          NULL.
        - If `Type` is an interface, returns `expr` when the dynamic type implements the interface; otherwise returns
          NULL.

Notes:

- The frontend lowers `IS`/`AS` to runtime helpers (type id queries and itable checks). See also the dispatch and RTTI
  details in the [OOP Semantics](basic-reference.md#oop-semantics) section of the BASIC reference.

## Static Members and Properties

### STATIC modifier placement

`STATIC` applies to the next member declaration inside a `CLASS`:

```basic
CLASS C
  STATIC value AS INTEGER      ' static field
  STATIC SUB Ping()            ' static method
  STATIC PROPERTY Count AS INTEGER
    GET: RETURN 0: END GET
  END PROPERTY
END CLASS
```

### PROPERTY blocks

```basic
PROPERTY <Name> AS <Type>
  [<AccessorAccess>] GET
    <statements>
  END GET
  [<AccessorAccess>] SET [(<Param> [AS <Type>])]
    <statements>
  END SET
END PROPERTY
```

- `<AccessorAccess>`: optional `PUBLIC` or `PRIVATE` on each accessor; defaults to the property head's access.
- `SET` parameter defaults to the property type and parameter name `value` when not given explicitly.

### Static constructor

One static constructor per class is supported using the standard constructor syntax preceded by `STATIC`:

```basic
CLASS A
  STATIC SUB NEW()
    ' one-time initialization for the class
  END SUB
END CLASS
```

The static constructor is parameterless and is invoked by the module initializer before any user code runs.

## Enum declarations

Enumerations declare named integer constants:

```basic
ENUM Color
  Red
  Green = 5
  Blue
END ENUM
```

- Members default to sequential values starting from 0.
- Explicit values may be assigned; subsequent members continue from the last explicit value.
- Enum members are accessed via the enum name: `Color.Red`, `Color.Green`.
- Enum names are registered in the OOP index and participate in name resolution.
- Keywords that collide with enum names (e.g. `Color`) are handled by `canBeUsedAsName()` in the parser.
