---
status: active
audience: public
last-verified: 2026-08-17
---

# Core Types

> Foundational types that form the basis of all Zanna programs.

**Part of the [Zanna Runtime Library](README.md)**

## Contents

- [Zanna.Core.Box](#zannacorebox)
- [Zanna.Core.Convert](#zannacoreconvert)
- [Zanna.Core.Diagnostics](#zannacorediagnostics)
- [Zanna.Core.MessageBus](#zannacoremessagebus)
- [Zanna.Core.Object](#zannacoreobject)
- [Zanna.Core.Parse](#zannacoreparse)
- [Zanna.String](#zannastring)
- [Zanna.Text.Char](#zannatextchar)

---

## Zanna.Core.Object

Common base facade for Zanna reference types. It also recognizes built-in runtime strings and
boxes, which have content/value equality rather than the identity-only fallback used by other
objects.

**Type:** Base class (not instantiated directly)

### Instance Methods

| Method          | Signature         | Description                                    |
|-----------------|-------------------|------------------------------------------------|
| `Equals(other)` | `Boolean(Object)` | Compares this object with another for equality |
| `HashCode()`    | `Integer()`       | Returns a hash code for the object             |
| `IsNull()`      | `Boolean()`       | Returns true if this reference is null         |
| `ToString()`    | `String()`        | Returns a string representation of the object  |
| `TypeId()`      | `Integer()`       | Returns a numeric type identifier for the object's runtime type |
| `TypeName()`    | `String()`        | Returns the runtime type name of the object    |

### Class-Call Forms

| Function                             | Signature                 | Description                                               |
|--------------------------------------|---------------------------|-----------------------------------------------------------|
| `Zanna.Core.Object.IsNull(obj)`      | `Boolean(Object)`         | Null test that is safe for a null input                    |
| `Zanna.Core.Object.RefEquals(a, b)`  | `Boolean(Object, Object)` | Tests whether two references are the same instance         |

### Zia Example

> Object is the base type for all reference types. In Zia, it is implicit—most programs interact
> with concrete types rather than calling Object methods directly.

`TypeId()` returns stable built-in identifiers for strings, boxes, boxed value types, `Zanna.Option`, `Zanna.Core.MessageBus`, and `MessageBus.Callback` objects in addition to user/runtime class IDs.
Runtime string handles compare and hash by byte content through `Equals` and `HashCode`, so distinct handles with identical bytes behave as equal object keys.
Boxes compare and hash by tag and value; all other recognized objects use reference equality and a
pointer-derived hash. `RefEquals` always uses reference identity, including for strings and boxes.
`TypeName()` and `ToString()` report useful built-in names for opaque runtime objects such as boxed value types, options, message buses, and callback wrappers instead of falling back to a generic `Object` label.

### BASIC Example

```basic
DIM obj1 AS Zanna.Core.Object
DIM obj2 AS Zanna.Core.Object

IF obj1.Equals(obj2) THEN
    PRINT "Objects are equal"
END IF

PRINT obj1.ToString()
PRINT obj1.HashCode()
PRINT obj1.TypeName()   ' Runtime type name
PRINT obj1.TypeId()     ' Numeric type ID
PRINT obj1.IsNull()     ' True if null reference
PRINT Zanna.Core.Object.IsNull(obj1)  ' Static null-safe form

' Static function call - check if same instance
IF Zanna.Core.Object.RefEquals(obj1, obj2) THEN
    PRINT "Same object instance"
END IF
```

---

## Zanna.Core.Box

Boxing helpers for storing primitive values in generic collections. Boxed values are heap objects with type tags.

**Type:** Static utility class

### Methods

| Method              | Signature                 | Description                                            |
|---------------------|---------------------------|--------------------------------------------------------|
| `I64(value)`        | `Object(Integer)`         | Box an integer                                         |
| `F64(value)`        | `Object(Double)`          | Box a double                                           |
| `I1(value)`         | `Object(Boolean)`         | Box a boolean                                          |
| `Str(value)`        | `Object(String)`          | Box a string                                           |
| `ToI64(box)`        | `Integer(Object)`         | Unbox integer (traps on wrong type)                    |
| `ToF64(box)`        | `Double(Object)`          | Unbox double (traps on wrong type)                     |
| `ToI1(box)`         | `Boolean(Object)`         | Unbox boolean (traps on wrong type)                    |
| `ToStr(box)`        | `String(Object)`          | Unbox string as a retained result (traps on wrong type) |
| `ToI64Option(box)`  | `Option<Integer>(Object)` | Return `Some(integer)` or `None` on wrong type                    |
| `ToF64Option(box)`  | `Option<Double>(Object)`  | Return `Some(double)` or `None` on wrong type                     |
| `ToI1Option(box)`   | `Option<Boolean>(Object)` | Return `Some(boolean)` or `None` on wrong type                    |
| `ToStrOption(box)`  | `Option<String>(Object)`  | Return `Some(string)` or `None` on wrong type                     |
| `Type(box)`         | `Integer(Object)`         | Return type tag (0=i64, 1=f64, 2=i1, 3=str), or -1 for a non-box |
| `EqI64(box,val)`    | `Boolean(Object,Integer)` | Compare boxed value to integer                         |
| `EqF64(box,val)`    | `Boolean(Object,Double)`  | Compare boxed value to double                          |
| `EqStr(box,val)`    | `Boolean(Object,String)`  | Compare boxed value to string                          |

### Notes

- Type tags: 0 = integer, 1 = double, 2 = boolean, 3 = string.
- Unboxing with the wrong type traps with a runtime diagnostic.
- `ToStr` returns an owned/retained string; generated code releases it like other string-returning runtime calls.
- String box helpers validate non-null string handles and trap invalid foreign pointers instead of retaining or comparing arbitrary memory.
- The `To*Option` forms do not trap for type mismatch and return managed `Option` values, so Zia and BASIC never need output pointers.
- Boxed values report `Zanna.Core.Box` through `Zanna.Core.Object.TypeName` and use value equality/hash semantics for `Object.Equals` and collection lookup.
- Floating-point box hashes canonicalize `+0.0`/`-0.0` and all NaN payloads. Boxed NaN values compare equal to other boxed NaNs so collection hashing and equality stay compatible.
- `EqF64(box, value)` is the raw-value convenience comparison and follows IEEE `==`; unlike
  box-to-box equality, `EqF64(Box.F64(NaN), NaN)` is false.
- `ValueType(size)` and `ValueTypeAddField(...)` are compiler/runtime hooks. New user code should call `Zanna.Runtime.Unsafe.ValueType` and `Zanna.Runtime.Unsafe.ValueTypeAddField` only when intentionally integrating with boxed value-type payloads.
- `Zanna.Runtime.Unsafe.ValueType` is the catalog/introspection hook for boxed value-type payloads. The compiler copies the inline payload into heap storage, then registers managed object/string fields with the unsafe value-type field registration hook so boxed structs retain referenced values, participate in GC traversal, and release fields when finalized. Registering the same offset with the same field kind is idempotent and does not touch the current slot's reference count; registering the same offset with a different kind traps. When `retainNow` is true, the current slot value is validated before it is retained. If the value-type object already has a finalizer, managed-field cleanup chains it instead of replacing it.

### Zia Example

```zia
module BoxDemo;

bind Zanna.Terminal;
bind Zanna.Text.Fmt as Fmt;

func start() {
    // Box an integer (type tag 0)
    var boxed = Zanna.Core.Box.I64(42);
    Say("Type: " + Fmt.Int(Zanna.Core.Box.Type(boxed)));   // 0
    Say("Value: " + Fmt.Int(Zanna.Core.Box.ToI64(boxed)));  // 42

    // Box a string (type tag 3)
    var sbox = Zanna.Core.Box.Str("hello");
    Say("String: " + Zanna.Core.Box.ToStr(sbox));           // hello

    // Box a float (type tag 1)
    var fbox = Zanna.Core.Box.F64(3.14);
    Say("Float type: " + Fmt.Int(Zanna.Core.Box.Type(fbox)));  // 1
}
```

### BASIC Example

```basic
DIM boxed AS OBJECT
boxed = Zanna.Core.Box.I64(42)

PRINT Zanna.Core.Box.Type(boxed)         ' Output: 0
PRINT Zanna.Core.Box.ToI64(boxed)        ' Output: 42
PRINT Zanna.Core.Box.EqI64(boxed, 42)    ' Output: true
```

---

## Zanna.Core.Diagnostics

Assertion and trap utilities for program correctness checks. Failed checks raise a runtime trap with a message; an
unhandled trap terminates execution.

**Type:** Static utility class

### Methods

| Method                      | Signature                          | Description                                               |
|-----------------------------|------------------------------------|-----------------------------------------------------------|
| `Assert(cond, msg)`         | `Void(Boolean, String)`            | Trap with `msg` if `cond` is false                        |
| `AssertEq(a, b, msg)`       | `Void(Integer, Integer, String)`   | Trap if `a` and `b` are not equal                         |
| `AssertNeq(a, b, msg)`      | `Void(Integer, Integer, String)`   | Trap if `a` and `b` are equal                             |
| `AssertEqNum(a, b, msg)`    | `Void(Double, Double, String)`     | Trap unless numbers pass the runtime's `1e-9` approximate comparison |
| `AssertEqStr(a, b, msg)`    | `Void(String, String, String)`     | Trap if two strings are not equal                         |
| `AssertNull(obj, msg)`      | `Void(Object, String)`             | Trap if `obj` is not null                                 |
| `AssertNotNull(obj, msg)`   | `Void(Object, String)`             | Trap if `obj` is null                                     |
| `AssertFail(msg)`           | `Void(String)`                     | Unconditional trap with `msg`                             |
| `AssertGt(a, b, msg)`       | `Void(Integer, Integer, String)`   | Trap if `a` is not greater than `b`                       |
| `AssertLt(a, b, msg)`       | `Void(Integer, Integer, String)`   | Trap if `a` is not less than `b`                          |
| `AssertGte(a, b, msg)`      | `Void(Integer, Integer, String)`   | Trap if `a` is not greater than or equal to `b`           |
| `AssertLte(a, b, msg)`      | `Void(Integer, Integer, String)`   | Trap if `a` is not less than or equal to `b`              |
| `Trap(msg)`                 | `Void(String)`                     | Trigger a runtime trap with the given message             |

### Notes

- All assertion failures use the structured runtime trap mechanism. An unhandled trap terminates
  the program; a host, VM recovery point, or trap hook may intercept it.
- `AssertEqNum` first accepts exact equality (including equal infinities) and treats two NaNs as
  equal. Otherwise it requires absolute difference `< 1e-9` when both magnitudes are below 1, or
  relative difference `< 1e-9` for larger magnitudes.
- `Trap` unconditionally raises a trap request; prefer `AssertFail` when the intent is a named
  assertion failure.
- `Trap(msg)` accepts a managed `String` handle. Embedded NUL bytes in the message are preserved for validation and escaped in the diagnostic path.
- `AssertEqStr` compares full runtime string contents, including embedded NUL bytes, and escapes non-printable bytes in failure messages. Invalid string handles produce a trap diagnostic instead of a native crash.
- These are intended for invariant checking during development and internal consistency validation.

### Zia Example

```zia
module DiagnosticsDemo;

bind Zanna.Terminal;
bind Zanna.Core.Diagnostics as Diag;
bind Zanna.Text.Fmt as Fmt;

func divide(a: Integer, b: Integer) -> Integer {
    Diag.Assert(b != 0, "divide: divisor must be non-zero");
    return a / b;
}

func start() {
    Say(Fmt.Int(divide(10, 2)));   // 5

    var obj = Zanna.Core.Box.I64(42);
    Diag.AssertNotNull(obj, "box must not be null");

    Diag.AssertEqStr("hello", "hello", "strings must match");
    Diag.AssertGt(5, 3, "5 must be > 3");
}
```

### BASIC Example

```basic
' Basic assertions
DIM x AS INTEGER = 1
DIM name AS STRING = "Alice"
Zanna.Core.Diagnostics.Assert(x > 0, "x must be positive")
Zanna.Core.Diagnostics.AssertEqStr(name, "Alice", "unexpected name")

' Null checks
DIM obj AS OBJECT = Zanna.Core.Box.I64(1)
Zanna.Core.Diagnostics.AssertNotNull(obj, "GetSomething returned null")

' Ordering assertions
DIM score AS INTEGER = 50
Zanna.Core.Diagnostics.AssertGte(score, 0, "score out of range")
Zanna.Core.Diagnostics.AssertLte(score, 100, "score out of range")

' Unconditional trap
DIM unrecoverableError AS INTEGER = 0
IF unrecoverableError THEN
    Zanna.Core.Diagnostics.Trap("fatal: unrecoverable error in pipeline")
END IF
```

---

## Zanna.Core.Parse

Safe string parsing utilities. Methods return `Option`, validation booleans, or default values rather than trapping on bad input.

**Type:** Static utility class

### Methods

| Method                      | Signature                           | Description                                                        |
|-----------------------------|-------------------------------------|--------------------------------------------------------------------|
| `TryInt(s)`                 | `Option<Integer>(String)`           | Parse integer; returns `None` if invalid                           |
| `TryDouble(s)`              | `Option<Double>(String)`            | Parse double; returns `None` if invalid                            |
| `TryBool(s)`                | `Option<Boolean>(String)`           | Parse boolean; returns `None` if invalid                           |
| `IntOr(s, default)`         | `Integer(String, Integer)`          | Parse `s` as integer; return `default` on failure                  |
| `DoubleOr(s, default)`      | `Double(String, Double)`            | Parse `s` as double; return `default` on failure                   |
| `BoolOr(s, default)`        | `Boolean(String, Boolean)`          | Parse `s` as boolean; return `default` on failure                  |
| `IsInt(s)`                  | `Boolean(String)`                   | Return true if `s` is a valid integer (no side effects)            |
| `IsNum(s)`                  | `Boolean(String)`                   | Return true if `s` is a valid number (no side effects)             |
| `IntRadix(s, radix, default)` | `Integer(String, Integer, Integer)` | Parse `s` in the given radix (2–36); return `default` on failure |

### Notes

- `TryInt`, `TryDouble`, and `TryBool` return managed `Option` values. The lower-level C output-pointer helpers remain runtime-internal.
- Decimal integers accept an optional sign. Decimal numbers use the strict grammar
  `[+-]?([0-9]+(\.[0-9]*)?|\.[0-9]+)([eE][+-]?[0-9]+)?`; commas, underscores, hexadecimal
  prefixes, and trailing text are rejected.
- `TryBool` accepts `true`/`yes`/`1`/`on` and `false`/`no`/`0`/`off`, case-insensitively.
- Null input is treated as parse failure: `Try*` returns `None`, `Is*` returns false, and `*Or`/`IntRadix` returns the supplied default.
- `IntRadix` supports bases 2 through 36 (e.g., 16 for hex, 2 for binary). Leading `+` and `-` signs are accepted for radix 10 only; non-decimal radices parse unsigned 64-bit bit patterns so formatted hex/binary values can round-trip.
- Leading/trailing ASCII whitespace is accepted; non-whitespace trailing characters and embedded NUL bytes are rejected.
- Numeric parsing accepts signed, case-insensitive `NaN` and `Inf` spellings. Decimal overflow and
  non-finite decimal results are rejected; finite underflow to zero or a subnormal value is
  accepted. Numeric parsing is isolated to the C numeric locale, so the decimal separator is
  always `.`.

### Parse.TryDouble and Parse.TryInt Example

```zia
module ParseOptionsDemo;

bind Zanna.Core.Parse as Parse;
bind Zanna.Terminal;

func start() {
    var n = Parse.TryDouble("3.14");   // Some(3.14)
    var bad = Parse.TryDouble("abc"); // None
    if (bad.IsNone) {
        Say("Not a number");
    }

    var i = Parse.TryInt("42");       // Some(42)
    var badInt = Parse.TryInt("xyz"); // None
}
```

### Zia Example

```zia
module ParseDemo;

bind Zanna.Terminal;
bind Zanna.Core.Parse as Parse;
bind Zanna.Text.Fmt as Fmt;

func start() {
    // Safe integer parsing with default
    var n = Parse.IntOr("42", 0);
    Say("Parsed: " + Fmt.Int(n));         // 42

    var bad = Parse.IntOr("abc", -1);
    Say("Bad input: " + Fmt.Int(bad));    // -1

    // Validation without parsing
    Say(Fmt.Bool(Parse.IsInt("123")));    // true
    Say(Fmt.Bool(Parse.IsInt("12.5")));   // false

    // Hex parsing
    var hex = Parse.IntRadix("FF", 16, 0);
    Say("0xFF = " + Fmt.Int(hex));        // 255

    // Float parsing
    var f = Parse.DoubleOr("3.14", 0.0);
    Say("Float: " + Fmt.Num(f));          // 3.14
}
```

### BASIC Example

```basic
' Safe parsing with defaults
DIM userInput AS STRING = "yes"
DIM n AS INTEGER = Zanna.Core.Parse.IntOr(userInput, 0)
DIM f AS DOUBLE  = Zanna.Core.Parse.DoubleOr(userInput, 0.0)
DIM b AS INTEGER = Zanna.Core.Parse.BoolOr(userInput, 0)

' Validation before use
DIM inputStr AS STRING = "42"
IF Zanna.Core.Parse.IsInt(inputStr) THEN
    DIM value AS INTEGER = Zanna.Core.Parse.IntOr(inputStr, 0)
    PRINT "Value: "; value
ELSE
    PRINT "Not a valid integer"
END IF

' Radix parsing (hex, binary, etc.)
DIM hexVal AS INTEGER = Zanna.Core.Parse.IntRadix("1A3F", 16, 0)
PRINT "Hex 1A3F = "; hexVal    ' Output: 6719

DIM binVal AS INTEGER = Zanna.Core.Parse.IntRadix("1010", 2, 0)
PRINT "Bin 1010 = "; binVal    ' Output: 10
```

---

## Zanna.Core.Convert

Strict string conversions and scalar formatting helpers. String-to-number failures raise a runtime
trap; use [`Zanna.Core.Parse`](#zannacoreparse) when invalid user input should produce `Option` or a
default value.

**Type:** Static utility class

| Function                                  | Signature                  | Description                                |
|-------------------------------------------|----------------------------|--------------------------------------------|
| `ToStringInt(value)`                      | `String(Integer)`          | Convert an integer to decimal text         |
| `ToStringDouble(value)`                   | `String(Double)`           | Convert a double to round-trip text        |
| `ToInt64(text)`                           | `Integer(String)`          | Strictly parse a decimal integer; trap on failure |
| `ToDouble(text)`                          | `Double(String)`           | Strictly parse a decimal double or signed `NaN`/`Inf`; trap on failure |
| `NumToInt(value)`                         | `Integer(Number)`          | Truncate toward zero; map NaN to 0 and clamp overflow |

`ToString_Int` and `ToString_Double` remain available as compatibility aliases. `NumToInt(3.7)`
returns `3`; infinities and finite out-of-range values clamp to the nearest signed 64-bit endpoint.
This is distinct from `ToInt64(text)`, which parses a string.

---

## Zanna.String

String manipulation class. Zanna strings are immutable byte strings and commonly contain UTF-8.
Most positions and lengths are bytes. `Mid`/`MidLen`, `Reverse`, and the SQL-LIKE wildcards instead
advance by Unicode code points using one shared strict UTF-8 decoder: invalid lead or
continuation bytes, overlong encodings, UTF-16 surrogates, and values above U+10FFFF trap with
an invalid-UTF-8 diagnostic rather than being grouped as apparent characters. Byte-oriented
methods (`Left`, `Right`, `Substr`, indexes, ...) continue to accept arbitrary bytes.

**Type:** Instance (opaque*)

### Properties

| Property  | Type    | Description                                    |
|-----------|---------|------------------------------------------------|
| `Length`  | Integer | Returns the number of bytes in the string      |
| `IsEmpty` | Boolean | Returns true if the string has zero length     |

### Methods

| Method                       | Signature                  | Description                                                                   |
|------------------------------|----------------------------|-------------------------------------------------------------------------------|
| `Substring(start, length)`   | `String(Integer, Integer)` | Extracts bytes from zero-based offset `start`; negative values clamp to zero   |
| `Concat(other)`              | `String(String)`           | Concatenates another string and returns the result                            |
| `Left(count)`                | `String(Integer)`          | Returns the leftmost `count` bytes; a negative count traps                     |
| `Right(count)`               | `String(Integer)`          | Returns the rightmost `count` bytes; a negative count traps                    |
| `Mid(start)`                 | `String(Integer)`          | Returns Unicode code points from one-based position `start` (strictly validated); `start < 1` traps  |
| `MidLen(start, length)`      | `String(Integer, Integer)` | Returns `length` Unicode code points (strictly validated); invalid negative/range inputs trap or clamp as noted below |
| `Trim()`                     | `String()`                 | Removes leading and trailing ASCII whitespace                                 |
| `TrimStart()`                | `String()`                 | Removes leading ASCII whitespace                                              |
| `TrimEnd()`                  | `String()`                 | Removes trailing ASCII whitespace                                             |
| `ToUpper()`                  | `String()`                 | Converts ASCII `a`–`z` to uppercase; non-ASCII UTF-8 bytes are unchanged      |
| `ToLower()`                  | `String()`                 | Converts ASCII `A`–`Z` to lowercase; non-ASCII UTF-8 bytes are unchanged      |
| `IndexOf(search)`            | `Integer(String)`          | Returns the one-based byte position of `search`, or 0 if not found            |
| `IndexOfFrom(start, search)` | `Integer(Integer, String)` | Searches from a one-based byte position; returns 0 if not found                |
| `Chr(code)`                  | `String(Integer)`          | Returns a one-byte string for code 0–255; other values trap                    |
| `Asc()`                      | `Integer()`                | Returns the unsigned value of the first byte, or 0 for an empty string         |

`Substring`, `Left`, `Right`, and the search methods clamp slices/search starts to the available
byte range. `MidLen` clamps an overlong result to the end and returns empty for a zero length or a
start beyond the end. Trimming recognizes space, tab, CR, LF, vertical tab, and form feed.

### Extended Methods

**Search & Match:**

| Method               | Signature         | Description                                  |
|----------------------|-------------------|----------------------------------------------|
| `StartsWith(prefix)` | `Boolean(String)` | Returns true if string starts with prefix    |
| `EndsWith(suffix)`   | `Boolean(String)` | Returns true if string ends with suffix      |
| `Contains(needle)`   | `Boolean(String)` | Returns true if string contains needle       |
| `Count(needle)`      | `Integer(String)` | Counts non-overlapping occurrences of needle |

**Transformation:**

| Method                         | Signature                 | Description                                                     |
|--------------------------------|---------------------------|-----------------------------------------------------------------|
| `Replace(needle, replacement)` | `String(String, String)`  | Replaces all occurrences of needle with replacement             |
| `PadLeft(width, padChar)`      | `String(Integer, String)` | Pads to a byte width with a single-byte `padChar` (multibyte traps) |
| `PadRight(width, padChar)`     | `String(Integer, String)` | Pads to a byte width with a single-byte `padChar` (multibyte traps) |
| `Repeat(count)`                | `String(Integer)`         | Repeats the string; a non-positive count returns empty           |
| `Reverse()`                       | `String()`                | Reverses Unicode code points (strict UTF-8; malformed input traps)   |
| `Split(delimiter)`             | `Seq(String)`             | Splits string by delimiter into a Seq of strings                |
| `Lines()`                      | `Seq(String)`             | Splits into logical lines on `\n`, dropping a trailing `\r` (CRLF→LF); segment count matches `Split("\n")` |

**Case Conversion:**

| Method              | Signature    | Description                                                    |
|---------------------|-------------|----------------------------------------------------------------|
| `Capitalize()`      | `String()`  | Apply C-locale uppercase to the first byte; leave the rest unchanged |
| `Title()`           | `String()`  | Uppercase the first byte and the first byte after C-locale whitespace |
| `CamelCase()`       | `String()`  | Convert byte words to camelCase                                |
| `PascalCase()`      | `String()`  | Convert byte words to PascalCase                               |
| `SnakeCase()`       | `String()`  | Convert byte words to snake_case                               |
| `KebabCase()`       | `String()`  | Convert byte words to kebab-case                               |
| `ScreamingSnakeCase()`  | `String()`  | Convert byte words to SCREAMING_SNAKE_CASE                     |

**Additional Search:**

| Method                | Signature          | Description                                               |
|-----------------------|--------------------|-----------------------------------------------------------|
| `LastIndexOf(search)` | `Integer(String)`  | Returns the last one-based byte position, or 0 if not found (empty needle: `Length + 1`) |
| `RemovePrefix(prefix)`| `String(String)`   | Removes prefix if present, otherwise returns original     |
| `RemoveSuffix(suffix)`| `String(String)`   | Removes suffix if present, otherwise returns original     |
| `TrimChar(chars)`     | `String(String)`   | Removes repetitions of the first byte of `chars` from both ends |
| `Slug()`              | `String()`         | Lowercase C-locale alphanumerics and collapse other byte runs to `-` |

**String Distance:**

| Method                | Signature          | Description                                              |
|-----------------------|--------------------|----------------------------------------------------------|
| `Levenshtein(other)`  | `Integer(String)`  | Compute byte-level edit distance; allocation failure returns -1 |
| `Jaro(other)`         | `Double(String)`   | Compute byte-level Jaro similarity (0.0 to 1.0)          |
| `JaroWinkler(other)`  | `Double(String)`   | Compute byte-level Jaro plus a four-byte prefix bonus     |
| `Hamming(other)`      | `Integer(String)`  | Count differing bytes, or return -1 for unequal byte lengths |

**Pattern Matching:**

| Method           | Signature         | Description                                              |
|------------------|-------------------|----------------------------------------------------------|
| `Like(pattern)`   | `Boolean(String)` | Whole-string SQL LIKE match (`%` = any sequence, `_` = one Unicode code point) |
| `LikeIgnoreCase(pattern)` | `Boolean(String)` | C-locale byte-folded SQL LIKE match                    |

**Comparison:**

| Method             | Signature         | Description                                      |
|--------------------|-------------------|--------------------------------------------------|
| `Compare(other)`   | `Integer(String)` | Bytewise comparison, returning -1, 0, or 1                 |
| `CompareIgnoreCase(other)` | `Integer(String)` | C-locale byte-folded comparison, returning -1, 0, or 1     |

Empty needles follow one shared rule across the index family: an empty needle matches at every
position `1..Length+1`, so `IndexOf` returns 1, `IndexOfFrom` returns its clamped start, and
`LastIndexOf` returns `Length + 1` — never the 0 miss sentinel. Elsewhere, `StartsWith`,
`EndsWith`, and `Has` return true; `Count` returns 0; and `Replace` returns the original.
`Split` with an empty delimiter returns a one-element sequence containing the original string.

The five identifier-style case conversions split only on space, tab, `_`, `-`, lower-to-upper
transitions, and acronym boundaries. Digits remain in the surrounding byte word. Case
classification is ASCII-only and locale-independent (bytes above 0x7F never fold or classify
as letters). The conversions honor the byte-string contract: embedded NUL bytes are ordinary
word bytes and are preserved in the output.
`PadLeft`/`PadRight` measure width in bytes and therefore require a single-byte padding string:
multibyte padding traps (`padding must be a single byte`) instead of emitting malformed UTF-8;
an empty padding string leaves the input unchanged.

`Like` and `LikeIgnoreCase` match the whole string. A backslash quotes the next pattern byte; a final
backslash is literal. See [Pattern Matching](text/patterns.md#stringlike--stringlikeci) for the full
contract and malformed-UTF-8 limitations.

### Static Functions (Zanna.String)

| Function                                       | Signature                  | Description                                                      |
|------------------------------------------------|----------------------------|------------------------------------------------------------------|
| `Zanna.String.Equals(a, b)`                    | `Boolean(String, String)`  | Compare two strings for equality                                 |
| `Zanna.String.FromI16(value)`                  | `String(i16)`              | Format a signed 16-bit integer                                  |
| `Zanna.String.FromI32(value)`                  | `String(i32)`              | Format a signed 32-bit integer                                  |
| `Zanna.String.Join(separator, items)`          | `String(String, Seq<String>)` | Join string elements; null elements are empty and other types trap |
| `Zanna.String.SplitFields(text)`               | `Seq<String>(String)`      | Parse trimmed comma-separated fields, double quotes, and doubled quotes |
| `Zanna.String.SplitFieldsResult(text)`         | `Result(String)`           | Strict-quoting split: `Ok(Seq<String>)` or `ErrStr` on malformed quotes |

`FromI16` and `FromI32` accept ordinary integer expressions in both languages: the frontends
insert checked narrowing at the call site, so out-of-range values trap with a narrow-conversion
diagnostic instead of wrapping. `FromSingle` accepts a Double and formats it with
single-precision semantics (the value is narrowed to Float32 before formatting), identically on
the VM and native backends.

`SplitFields` trims each field with the process C character locale. Quoted fields may contain
commas and doubled `""` becomes `"`; empty and trailing fields are preserved. The parser is
deliberately lenient INPUT-style splitting: it tolerates unclosed quotes and quotes appearing
after unquoted text rather than rejecting the record. When you need strict validation, use
`SplitFieldsResult(text)`, which enforces quote structure — a quoted field must open at the
field start, close exactly once, and be followed only by whitespace before the next comma;
quotes may not appear inside unquoted text; the record may not end inside a quote — and returns
`Ok(Seq<String>)` or `ErrStr` naming the violation.

### Zia Example

```zia
module StringDemo;

bind Zanna.Terminal;
bind Zanna.String as Str;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var s = "  Hello, World!  ";

    Say("Length: " + Fmt.Int(Str.get_Length(s)));           // 17 bytes
    Say(Str.Trim(s));                                       // Hello, World!
    Say(Str.ToUpper(s));                                    // HELLO, WORLD!
    Say(Str.ToLower(s));                                    // hello, world!
    Say("IndexOf: " + Fmt.Int(Str.IndexOf(s, "World")));    // 10
    Say("Has World: " + Fmt.Bool(Str.Contains(s, "World")));     // true
    Say(Str.Replace(s, "World", "Zia"));                    // Hello, Zia!
}
```

### BASIC Example

```basic
DIM s AS STRING
s = "  Hello, World!  "

PRINT s.Length          ' Output: 17
PRINT s.Trim()          ' Output: "Hello, World!"
PRINT s.ToUpper()       ' Output: "  HELLO, WORLD!  "
PRINT s.Left(7).Trim()  ' Output: "Hello,"
PRINT s.IndexOf("World") ' Output: 10 (one-based)

DIM code AS INTEGER
code = s.Trim().Asc()   ' code = 72 (ASCII for 'H')
```

### Extended Methods Zia Example

```zia
module StringExtDemo;

bind Zanna.Terminal;
bind Zanna.String as Str;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var s = "hello world";

    // Search and match
    Say("Starts: " + Fmt.Bool(Str.StartsWith(s, "hello")));  // true
    Say("Ends: " + Fmt.Bool(Str.EndsWith(s, "world")));      // true
    Say("Has: " + Fmt.Bool(Str.Contains(s, "lo wo")));            // true
    Say("Count l: " + Fmt.Int(Str.Count(s, "l")));           // 3

    // Transformation
    Say(Str.Replace(s, "world", "zia"));    // hello zia
    Say(Str.PadLeft("42", 5, "0"));         // 00042
    Say(Str.Repeat("ab", 3));               // ababab
    Say(Str.Reverse("hello"));                 // olleh
}
```

### Extended Methods BASIC Example

```basic
DIM s AS STRING
s = "hello world"

' Search and match
PRINT s.StartsWith("hello")  ' Output: true
PRINT s.EndsWith("world")    ' Output: true
PRINT s.Contains("lo wo")         ' Output: true
PRINT s.Count("l")           ' Output: 3

' Transformation
PRINT s.Replace("world", "universe")  ' Output: "hello universe"
PRINT "42".PadLeft(5, "0")            ' Output: "00042"
PRINT "hi".PadRight(5, "_")           ' Output: "hi___"
PRINT "ab".Repeat(3)                   ' Output: "ababab"
PRINT "hello".Reverse()                   ' Output: "olleh"

' Split and join
DIM parts AS Zanna.Collections.Seq
parts = "a,b,c".Split(",")
PRINT parts.Count                         ' Output: 3
PRINT Zanna.String.Join("-", parts)   ' Output: "a-b-c"

' Comparison
PRINT "abc".Compare("abd")             ' Output: -1
PRINT "ABC".CompareIgnoreCase("abc")           ' Output: 0
```

---

## Zanna.Text.Char

Static ASCII character-classification helpers for identifier scanning (completion triggers,
word selection, tokenization). Each takes a string and classifies its **first byte**, so it drops
directly into a byte-by-byte scan; an empty string or a non-ASCII leading byte returns `false`.

| Method                     | Signature       | Description                                              |
|----------------------------|-----------------|---------------------------------------------------------|
| `IsIdentifierStart(ch)`    | `Boolean(String)` | First character can start an identifier (ASCII letter or `_`) |
| `IsIdentifierPart(ch)`     | `Boolean(String)` | First character can continue an identifier (ASCII letter, digit, or `_`) |
| `IsAlphanumeric(ch)`              | `Boolean(String)` | First character is ASCII alphanumeric (letter or digit) |

```zia
module CharDemo;

bind Zanna.Text.Char as Char;
bind Zanna.String as Str;
bind Zanna.Terminal;

func start() {
    var text = "name1";
    if Char.IsIdentifierStart(Str.MidLen(text, 1, 1)) {
        Say("identifier start");
    }
}
```

---

## Zanna.Core.MessageBus

In-process publish/subscribe message bus for decoupled communication between components. Subscribers register interest in named topics and receive published data via callbacks.

**Type:** Instance class (requires `New()`)

### Constructor

| Method  | Signature      | Description                    |
|---------|----------------|--------------------------------|
| `New()` | `MessageBus()` | Create a new empty message bus |

### Properties

| Property             | Type                  | Description                                      |
|----------------------|-----------------------|--------------------------------------------------|
| `TotalSubscriptions` | `Integer` (read-only) | Total number of active subscriptions across all topics |

### Methods

| Method                     | Signature                     | Description                                                     |
|----------------------------|-------------------------------|-----------------------------------------------------------------|
| `Callback(fn)`             | `Object(Function)`            | Wrap a frontend function reference as a managed handler |
| `Subscribe(topic, handler)` | `Integer(String, Object)`    | Subscribe a handler to a topic; returns subscription ID         |
| `Unsubscribe(id)`          | `Boolean(Integer)`           | Remove a subscription by ID; returns true if found              |
| `Publish(topic, data)`     | `Integer(String, Object)`    | Publish data to all subscribers of a topic; returns count notified |
| `SubscriberCount(topic)`   | `Integer(String)`            | Returns the number of subscribers for a topic                   |
| `Topics()`                 | `Seq<String>()`              | Returns copied names for all topics with active subscribers      |
| `ClearTopic(topic)`        | `Void(String)`               | Remove all subscribers for a specific topic                     |
| `Clear()`                  | `Void()`                     | Remove all subscriptions from all topics                        |

### Notes

- `Subscribe` returns a positive, monotonically increasing ID that can be used with `Unsubscribe`;
  null inputs return `-1`, and the bus traps rather than reusing IDs after signed 64-bit exhaustion.
- `Publish` invokes handlers synchronously in subscription order and returns the number called.
  Handler functions receive the published data as their argument.
- Publish uses a stable subscriber snapshot; unsubscribes during a handler affect later publishes,
  not the in-flight one. A handler trap aborts the remaining calls, releases the snapshot, and
  re-raises the trap.
- Publish retains managed string handles and object/array payloads for the duration of dispatch so
  one handler cannot free the payload before later handlers run. Arbitrary foreign pointers remain
  borrowed and must outlive the call.
- Subscribe accepts a managed callback returned by `Callback(&handler)` in Zia or `Callback(ADDRESSOF Handler)` in BASIC. Native callback pointers stay inside the runtime bridge.
- Topic matching is byte-length aware; topic names containing embedded NUL bytes remain distinct.
- Empty topic names are valid. `Topics()` returns an owning `Seq` of copied topic strings in
  unspecified hash-bucket order; the result remains valid after the bus is cleared or destroyed.
- `Topics()` is registered as `seq<str>`, so natural chains such as `bus.Topics().Count`
  type-check directly.
- `Unsubscribe`, `ClearTopic`, and `Clear` remove empty topic records, so a later `Topics()` call reports only active topics.
- If a handler traps during `Publish`, the in-flight snapshot is released before the trap is re-raised.
- MessageBus instances are typed runtime objects, participate in GC traversal for retained handlers,
  serialize state access with an internal lock, and retain the bus for each public operation.
  Handlers run after that lock is released, so they may publish, unsubscribe, or clear safely.

### Zia Example

```zia
module MessageBusDemo;

bind Zanna.Terminal;
bind Zanna.Core.MessageBus as MessageBus;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var bus = MessageBus.New();
    Say("Total subscriptions: " + Fmt.Int(bus.get_TotalSubscriptions())); // 0
    Say("Subscriber count for 'test': " + Fmt.Int(bus.SubscriberCount("test"))); // 0
}
```

### BASIC Example

```basic
' Create a new message bus
DIM bus AS OBJECT = Zanna.Core.MessageBus.New()

' Check initial state
PRINT "Total subscriptions: "; bus.TotalSubscriptions  ' Output: 0
PRINT "Subscribers for 'test': "; bus.SubscriberCount("test")  ' Output: 0

' Subscribe and publish use managed callbacks created with MessageBus.Callback(ADDRESSOF Handler).
```

---

## See Also

- [Text Processing](text/README.md) - `StringBuilder` for efficient string building, `Pattern` for regex
- [Utilities](utilities.md) - `Fmt` for string formatting, `Parse` for string parsing
- [Collections](collections/README.md) - `Seq` for string lists, `Map` for string-keyed data
