---
status: active
audience: public
last-verified: 2026-08-17
---

# Functional Types

> Runtime wrappers for pre-evaluated values, optional values, and explicit success or failure.

**Part of the [Zanna Runtime Library](README.md)**

## Contents

- [Zanna.Functional.Lazy](#zannafunctionallazy)
- [Zanna.Option](#zannaoption)
- [Zanna.Result](#zannaresult)

These registry classes have no public constructor. Call their factory methods on the class, then
use properties and operations on the returned instance. The flat runtime names also accept the
instance as an explicit first argument, so both `value.UnwrapI64()` and
`Zanna.Option.UnwrapI64(value)` are supported call shapes.

`Zanna.Option` and `Zanna.Result` are dynamically typed runtime-library objects. They are distinct
from Zia's type-system features [`T?`](../languages/zia-reference.md#optional-types) and
[`Result[T]`](../languages/zia-reference.md#result-values).

---

## Zanna.Functional.Lazy

`Zanna.Functional.Lazy` is a runtime wrapper that caches a deferred object value. `New(supplier)`
defers evaluation: the zero-argument supplier is invoked once on the first `Get`/`Force` and its
object result is cached. The `Of`, `OfStr`, and `OfI64` factories instead create **pre-evaluated**
wrappers around values you already have, so their `IsEvaluated` is `true` immediately.

For on-demand element generation and deferred transformation pipelines, use
[`Zanna.Functional.LazySeq`](collections/functional.md#zannafunctionallazyseq).

### Factory Methods

| Method          | Signature          | Description                                              |
|-----------------|--------------------|----------------------------------------------------------|
| `New(supplier)` | `Lazy(Callback)`   | Defer a zero-argument, object-returning supplier; it runs once on first access |
| `Of(value)`     | `Lazy(Object)`     | Wrap a generic object as already evaluated               |
| `OfStr(value)`  | `Lazy(String)`     | Wrap a string as already evaluated                       |
| `OfI64(value)`  | `Lazy(Integer)`    | Wrap an integer as already evaluated                     |

### Instance Members

| Member          | Signature/Object type | Description                                                  |
|-----------------|-----------------------|--------------------------------------------------------------|
| `IsEvaluated`   | `Boolean` property    | Whether the wrapper's cached value is initialized            |
| `Get()`         | `Object()`            | Return the generic-object payload                             |
| `GetStr()`      | `String()`            | Return the string payload; returns `""` for a mismatched type |
| `GetI64()`      | `Integer()`           | Return the integer payload; returns `0` for a mismatched type |
| `Force()`       | `Void()`              | Ensure evaluation; runs a pending `New` supplier, no-op for `Of*` values |
| `Map(callback)` | `Lazy(Callback)`      | Immediately map a generic-object payload into a new Lazy      |
| `AndThen(callback)` | `Lazy(Callback)`  | Immediately pass a generic-object payload to a Lazy-producing callback |

`Map` and `AndThen` force the source before invoking their callback. `Map` wraps the callback's
object result in a new pre-evaluated Lazy; `AndThen` returns the callback's Lazy result directly.
They are not deferred transformations. Both callback operations are for generic object payloads,
not the string or integer variants.

Always match the factory and accessor: `New`/`Of` with `Get`, `OfStr` with `GetStr`, or `OfI64`
with `GetI64`. A `New` supplier must return an object; use `Of*` for strings and integers.

### Zia Example

```zia
module LazyDemo;

bind Zanna.Terminal;
bind Zanna.Functional.Lazy as Lazy;
bind Zanna.Collections.Seq as Seq;

func makeSeq() -> Zanna.Collections.Seq {
    Say("computing...");
    var s = new Zanna.Collections.Seq();
    s.Push("built");
    return s;
}

func start() {
    var deferred = Lazy.New(makeSeq);
    SayBool(deferred.get_IsEvaluated()); // false — supplier not run yet

    var seq = deferred.Get();            // prints "computing..." once
    SayBool(deferred.get_IsEvaluated()); // true
    deferred.Get();                      // cached; supplier not re-run

    var value = Lazy.OfI64(42);

    // Of* factories are already evaluated.
    SayBool(value.get_IsEvaluated()); // true
    SayInt(value.GetI64());           // 42

    value.Force();
    SayBool(value.get_IsEvaluated()); // still true
}
```

### BASIC Example

```basic
DIM value AS OBJECT = Zanna.Functional.Lazy.OfI64(42)

PRINT "IsEvaluated: "; value.IsEvaluated  ' Output: 1
PRINT "Value: "; Zanna.Functional.Lazy.GetI64(value) ' Output: 42

DIM textValue AS OBJECT = Zanna.Functional.Lazy.OfStr("hello")
PRINT textValue.GetStr() ' Output: hello

value.Force() ' Already evaluated; no visible change
```

---

## Zanna.Option

`Zanna.Option` represents either `Some`, which is present even when its generic object payload is
null, or `None`, which has no payload. Use the factory that matches the stored value's runtime type
and the corresponding typed accessor.

### Factory Methods

| Method           | Signature          | Description                                  |
|------------------|--------------------|----------------------------------------------|
| `Some(value)`    | `Object(Object)`   | Create Some with a generic object payload    |
| `SomeStr(value)` | `Object(String)`   | Create Some with a string payload            |
| `SomeI64(value)` | `Object(Integer)`  | Create Some with an integer payload           |
| `SomeI1(value)`  | `Object(Boolean)`  | Create Some with a normalized boolean payload |
| `SomeF64(value)` | `Object(Double)`   | Create Some with a double payload             |
| `None()`         | `Object()`         | Create an empty Option                        |

### Properties

| Property | Type                  | Description                         |
|----------|-----------------------|-------------------------------------|
| `IsSome` | `Boolean` (read-only) | True when the Option is Some        |
| `IsNone` | `Boolean` (read-only) | True when the Option is None        |

For a valid Option, `IsSome` and `IsNone` are complementary. At the C runtime boundary, a null
Option handle is treated as None by these predicates; language code should create absence with
`None()`.

### Unwrap Methods

| Method        | Signature    | Required factory | Failure behavior                     |
|---------------|--------------|------------------|--------------------------------------|
| `Unwrap()`    | `Object()`   | `Some`           | Traps on None or a non-object payload |
| `UnwrapStr()` | `String()`   | `SomeStr`        | Traps on None or a mismatched payload |
| `UnwrapI64()` | `Integer()`  | `SomeI64`/`SomeI1` | Traps on None or a mismatched payload |
| `UnwrapI1()`  | `Boolean()`  | `SomeI1`/`SomeI64` | Traps on None or a mismatched payload |
| `UnwrapF64()` | `Double()`   | `SomeF64`        | Traps on None or a mismatched payload |

`SomeI1` uses the integer storage variant and normalizes its input to `0` or `1`.

### Fallback Methods

| Method                   | Signature            | Description                                      |
|--------------------------|----------------------|--------------------------------------------------|
| `UnwrapOr(default)`      | `Object(Object)`     | Generic object value, or the default             |
| `UnwrapOrStr(default)`   | `String(String)`     | String value, or the default                     |
| `UnwrapOrI64(default)`   | `Integer(Integer)`   | Integer value, or the default                    |
| `UnwrapOrI1(default)`    | `Boolean(Boolean)`   | Normalized boolean value, or normalized default  |
| `UnwrapOrF64(default)`   | `Double(Double)`     | Double value, or the default                     |

Fallback methods do not trap. They return the default for None, a null handle, or a payload-type
mismatch. The generic `UnwrapOr` is for `Some`, not the typed `SomeStr`/`SomeI64`/`SomeI1`/`SomeF64`
factories.

### Object Access and Conversion

| Method         | Signature          | Description                                                   |
|----------------|--------------------|---------------------------------------------------------------|
| `Expect(msg)`  | `Object(String)`   | Generic-object value, or an invalid-operation trap containing `msg` |
| `OkOr(err)`    | `Result(Object)`   | Some becomes Ok; None becomes Err with an object error         |
| `OkOrStr(err)` | `Result(String)`   | Some becomes Ok; None becomes Err with a string error          |

`Expect` and the generic `Unwrap` accept only object payloads: on None they trap with
`DomainError`, and for a typed `SomeStr`/`SomeI64`/`SomeI1`/`SomeF64` payload they trap
with an explanatory message — use the matching `UnwrapStr`/`UnwrapI64`/`UnwrapI1`/`UnwrapF64`
accessor instead. For a non-trapping read, use `UnwrapOr` and its typed variants. `OkOr`
and `OkOrStr` preserve a Some payload's object, string, integer, boolean-storage, or double
variant when producing Ok.

### Combinators and Utilities

| Method              | Signature          | Description                                                     |
|---------------------|--------------------|-----------------------------------------------------------------|
| `Map(callback)`     | `Object(Object)`   | Map a generic Some object; None remains None                     |
| `AndThen(callback)` | `Object(Object)`   | Pass a generic Some object to an Option-producing callback      |
| `OrElse(callback)`  | `Object(Object)`   | Keep Some; call a zero-argument Option-producing callback for None |
| `Filter(predicate)` | `Object(Object)`   | Keep a generic Some object only when the predicate is true      |
| `Equals(other)`     | `Boolean(Object)`  | Compare two Options                                             |
| `ToString()`        | `String()`         | Render the variant and payload                                  |

The callback-based `Map`, `AndThen`, and `Filter` operations support only generic object payloads.
For typed string, integer, boolean, and double Options, unwrap, transform, and re-wrap explicitly.
In particular, `Map` and `AndThen` leave a typed Some unchanged, while `Filter` returns None for a
typed Some. With a null callback, `Map`, `AndThen`, and `Filter` return None; `OrElse` returns the
existing Some or, for None, a fresh None.

`Equals` compares variants and payload types. Strings compare by content, numeric payloads by
value, and generic objects by identity; all None instances compare equal. `ToString` produces
`None`, `Some(42)`, `Some(3.5)`, or `Some("text")`. Boolean payloads render as `Some(0)` or
`Some(1)`, and generic objects render with their pointer address. Rendering uses a 256-byte buffer,
so long string representations may be truncated; embedded string quotes are not escaped.

### Zia Example

```zia
module OptionDemo;

bind Zanna.Terminal;
bind Zanna.Option as Option;
bind Zanna.Result as Result;

func start() {
    var some = Option.SomeI64(42);
    SayBool(some.IsSome);             // true
    SayInt(some.UnwrapI64());         // 42
    Say(some.ToString());             // Some(42)

    var flag = Option.SomeI1(true);
    SayBool(flag.UnwrapI1());         // true

    var none = Option.None();
    SayBool(none.IsNone);             // true
    SayInt(none.UnwrapOrI64(99));     // 99

    var failed = none.OkOrStr("value was missing");
    SayBool(failed.IsErr);            // true
}
```

### BASIC Example

```basic
DIM some AS OBJECT = Zanna.Option.SomeI64(42)

PRINT "IsSome: "; some.IsSome       ' Output: 1
PRINT "IsNone: "; some.IsNone       ' Output: 0
PRINT "Value: "; some.UnwrapI64()   ' Output: 42
PRINT "Text: "; some.ToString()     ' Output: Some(42)

DIM none AS OBJECT = Zanna.Option.None()
PRINT "Default: "; none.UnwrapOrI64(99) ' Output: 99

DIM textValue AS OBJECT = Zanna.Option.SomeStr("hello")
PRINT textValue.UnwrapStr() ' Output: hello

DIM failed AS OBJECT = none.OkOrStr("value was missing")
PRINT "IsErr: "; failed.IsErr       ' Output: 1
```

---

## Zanna.Result

`Zanna.Result` represents either an `Ok` success payload or an `Err` failure payload. It can carry
a generic object or string on either side; the public registry additionally provides integer and
double factories for Ok values.

### Factory Methods

| Method          | Signature          | Description                              |
|-----------------|--------------------|------------------------------------------|
| `Ok(value)`     | `Object(Object)`   | Create Ok with a generic object payload  |
| `OkStr(value)`  | `Object(String)`   | Create Ok with a string payload          |
| `OkI64(value)`  | `Object(Integer)`  | Create Ok with an integer payload        |
| `OkF64(value)`  | `Object(Double)`   | Create Ok with a double payload          |
| `Err(value)`    | `Object(Object)`   | Create Err with a generic object payload |
| `ErrStr(value)` | `Object(String)`   | Create Err with a string payload         |

### Properties

| Property | Type                  | Description                         |
|----------|-----------------------|-------------------------------------|
| `IsOk`   | `Boolean` (read-only) | True when the Result is Ok          |
| `IsErr`  | `Boolean` (read-only) | True when the Result is Err         |

`IsOk` and `IsErr` are complementary for a valid Result. A null C runtime handle is neither Ok nor
Err.

### Success Accessors

| Method        | Signature    | Required factory | Failure behavior                     |
|---------------|--------------|------------------|--------------------------------------|
| `Unwrap()`    | `Object()`   | `Ok`             | Traps on Err or a non-object payload  |
| `UnwrapStr()` | `String()`   | `OkStr`          | Traps on Err or a mismatched payload  |
| `UnwrapI64()` | `Integer()`  | `OkI64`          | Traps on Err or a mismatched payload  |
| `UnwrapF64()` | `Double()`   | `OkF64`          | Traps on Err or a mismatched payload  |

### Fallback Accessors

| Method                   | Signature            | Description                                  |
|--------------------------|----------------------|----------------------------------------------|
| `UnwrapOr(default)`      | `Object(Object)`     | Generic Ok object, or the default            |
| `UnwrapOrStr(default)`   | `String(String)`     | Ok string, or the default                    |
| `UnwrapOrI64(default)`   | `Integer(Integer)`   | Ok integer, or the default                   |
| `UnwrapOrF64(default)`   | `Double(Double)`     | Ok double, or the default                    |

Fallback accessors return the default for Err, a null handle, or a payload-type mismatch. The
generic `UnwrapOr` is for `Ok`, not the typed Ok factories.

### Error and Object Accessors

| Method           | Signature          | Description                                                   |
|------------------|--------------------|---------------------------------------------------------------|
| `UnwrapErr()`    | `Object()`         | Generic Err object; traps on Ok                                |
| `UnwrapErrStr()` | `String()`         | Err string; traps on Ok or a mismatched payload                |
| `Expect(msg)`    | `Object(String)`   | Generic Ok object, or an invalid-operation trap containing `msg` |
| `ExpectErr(msg)` | `Object(String)`   | Generic Err object, or an invalid-operation trap containing `msg` |

`Unwrap`, `UnwrapErr`, `Expect`, and `ExpectErr` are object-payload operations: called on a typed
string, integer, or double payload they trap with an explanatory message instead of exposing the
stored bits as a pointer. Use the matching typed unwrap method for string, integer, or double
values.

### Combinators and Utilities

| Method                 | Signature          | Description                                                   |
|------------------------|--------------------|---------------------------------------------------------------|
| `Map(callback)`        | `Object(Object)`   | Map a generic Ok object; Err passes through                   |
| `MapErr(callback)`     | `Object(Object)`   | Map a generic Err object; Ok passes through                   |
| `AndThen(callback)`    | `Object(Object)`   | Pass a generic Ok object to a Result-producing callback       |
| `OrElse(callback)`     | `Object(Object)`   | Pass a generic Err object to a recovery callback              |
| `Equals(other)`        | `Boolean(Object)`  | Compare two Results                                           |
| `ToString()`           | `String()`         | Render the variant and payload                                |

All four callback combinators operate only on generic object payloads. A string, integer, or
double payload passes through unchanged, so typed values must be unwrapped, transformed, and
re-wrapped explicitly.

`Equals` requires the same variant and payload type. Strings compare by content, numeric payloads
by value, and generic objects by identity. `ToString` produces forms such as `Ok(42)`,
`Ok("text")`, and `Err("oops")`; generic objects render with their pointer address. A null handle
renders as `Result(null)`. Rendering uses a 256-byte buffer, so long string representations may be
truncated; embedded string quotes are not escaped.

### Zia Example

```zia
module ResultDemo;

bind Zanna.Terminal;
bind Zanna.Result as Result;

func start() {
    var ok = Result.OkI64(42);
    SayBool(ok.IsOk);                 // true
    SayInt(ok.UnwrapI64());           // 42
    Say(ok.ToString());               // Ok(42)

    var err = Result.ErrStr("oops");
    SayBool(err.IsErr);               // true
    Say(err.UnwrapErrStr());          // oops
    Say(err.ToString());              // Err("oops")
    SayInt(err.UnwrapOrI64(0));       // 0
}
```

### BASIC Example

```basic
DIM ok AS OBJECT = Zanna.Result.OkI64(42)

PRINT "IsOk: "; ok.IsOk           ' Output: 1
PRINT "IsErr: "; ok.IsErr         ' Output: 0
PRINT "Value: "; ok.UnwrapI64()   ' Output: 42
PRINT "Text: "; ok.ToString()     ' Output: Ok(42)

DIM err AS OBJECT = Zanna.Result.ErrStr("oops")

PRINT "IsOk: "; err.IsOk          ' Output: 0
PRINT "IsErr: "; err.IsErr        ' Output: 1
PRINT "Error: "; err.UnwrapErrStr() ' Output: oops
PRINT "Text: "; err.ToString()    ' Output: Err("oops")
PRINT "Default: "; err.UnwrapOrI64(0) ' Output: 0

DIM textResult AS OBJECT = Zanna.Result.OkStr("success")
PRINT textResult.UnwrapStr()       ' Output: success

DIM numberResult AS OBJECT = Zanna.Result.OkF64(3.14)
PRINT numberResult.UnwrapF64()     ' Output: 3.14
```

### Error-Handling Pattern

```basic
' Pseudocode: SomeOperationThatMayFail is application-specific.
DIM result AS OBJECT = SomeOperationThatMayFail()

IF result.IsOk THEN
    DIM value AS INTEGER = result.UnwrapI64()
    PRINT "Success: "; value
ELSE
    DIM message AS STRING = result.UnwrapErrStr()
    PRINT "Error: "; message
END IF

DIM safeValue AS INTEGER = result.UnwrapOrI64(-1)
```

### Result vs Option

| Feature           | Result                         | Option                    |
|-------------------|--------------------------------|---------------------------|
| Present/success   | `Ok(value)`                    | `Some(value)`             |
| Failure/absence   | `Err(error)` with error data   | `None` without error data |
| Typical use       | An operation may fail          | A value may be absent     |
| Option conversion | --                             | `OkOr` / `OkOrStr`        |

---

## See Also

- [Generated Lazy API](../generated/runtime/functional.md#zanna-functional-lazy)
- [Generated Option API](../generated/runtime/option.md#zanna-option)
- [Generated Result API](../generated/runtime/result.md#zanna-result)
- [Functional Collections](collections/functional.md) - `LazySeq` and collection operations
- [Core Types](core.md) - `Object`, `Box`, and `String`
- [Diagnostics](diagnostics.md) - traps and assertion helpers
