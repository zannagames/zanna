---
status: active
audience: public
last-verified: 2026-07-26
---

# Zanna BASIC Namespaces — Reference

This document provides a complete reference for the namespace feature in Zanna BASIC (Track A implementation). For
tutorial-style examples, see [the BASIC tutorial](../tutorials/basic-tutorial.md). For grammar details,
see [basic-grammar.md](basic-grammar.md).

## Overview

Namespaces organize types (classes and interfaces) into hierarchical groups, preventing name collisions and improving
code clarity. The namespace system includes:

- **NAMESPACE declarations** — `NAMESPACE A.B ... END NAMESPACE` (dotted segments form a nested hierarchy)
- **USING directives** — Import namespaces for unqualified type references (simple and aliased forms)
- **Type resolution** — Automatic search through current namespace → parent namespaces → USING imports → global
- **Case-insensitive** — All segments and identifiers are matched ignoring case (`A.B.F` ≡ `a.b.f`)
- **Nine diagnostic error codes** (E_NS_001 through E_NS_009) for comprehensive error handling

### Basic Examples

1) Flattened namespace call (t01)

```basic
NAMESPACE A
  SUB F: PRINT "ok": END SUB
END NAMESPACE

A.F()   ' qualified call
```

Output:

```text
ok
```

2) Nested namespace call (t02)

```basic
NAMESPACE A.B
  SUB F: PRINT "ok": END SUB
END NAMESPACE

A.B.F() ' fully-qualified call
```

Output:

```text
ok
```

3) Parent-walk resolution within nested namespace (t03)

```basic
NAMESPACE A.B
  SUB F: PRINT "ok": END SUB
  SUB Main
    F()   ' unqualified; resolves to A.B.F via parent-walk
  END SUB
END NAMESPACE

A.B.Main()
```

Output:

```text
ok
```

4) Cross-file usage with USING directive

File 1 defines:

```basic
NAMESPACE Lib
  SUB Ping: PRINT "pong": END SUB
END NAMESPACE
```

File 2 imports and calls:

```basic
USING Lib

Lib.Ping()   ' qualified call works
Ping()       ' unqualified call works via USING
```

Both calls succeed. Without the USING directive, only the qualified call (`Lib.Ping()`) would work.

### Notes on Case-Insensitivity

- Identifiers and each segment of qualified names are compared case-insensitively
- Canonicalization uses ASCII lowercase for resolution and duplicate checks
- Examples (all equivalent): `A.B.F`, `a.b.f`, `A.b.f`, `a.B.F`

## NAMESPACE Declaration

### Syntax

```basic
NAMESPACE A.B.C
  CLASS MyClass
    ...
  END CLASS

  INTERFACE MyInterface
    ...
  END INTERFACE
END NAMESPACE
```

### Rules

1. **Dotted paths**: `NAMESPACE A.B.C` creates a three-level hierarchy (A::B::C)
2. **Merged namespaces**: Multiple `NAMESPACE` blocks with the same path contribute to the same namespace
3. **Nested declarations**: Namespaces can contain CLASS, INTERFACE, and nested NAMESPACE blocks
4. **Case insensitive**: All namespace identifiers follow BASIC case-insensitive semantics
5. **Global scope**: NAMESPACE blocks must appear at file scope (not inside other blocks)

### Examples

```basic
NAMESPACE Collections
  CLASS List
    DIM size AS I64
  END CLASS
END NAMESPACE

NAMESPACE Collections
  CLASS Dictionary
    DIM count AS I64
  END CLASS
END NAMESPACE

REM Both List and Dictionary belong to Collections namespace
```

### Qualified Names

Types declared in namespaces have fully-qualified names:

```basic
NAMESPACE Graphics.Rendering
  CLASS Camera
  END CLASS
END NAMESPACE

REM Fully-qualified reference:
DIM cam AS Graphics.Rendering.Camera
```

## USING Directive

### Syntax

The USING directive has two forms:

1. **Simple import**: `USING NamespacePath`
2. **Aliased import**: `USING Alias = NamespacePath`

```basic
USING Collections
USING Graphics.Rendering
USING GFX = Graphics.Rendering
```

### Placement Rules

**CRITICAL**: USING directives must satisfy strict placement constraints:

1. **File scope only**: Cannot appear inside NAMESPACE, CLASS, or INTERFACE blocks
2. **Before all declarations**: Must appear before any NAMESPACE, CLASS, or INTERFACE declarations
3. **File-scoped effect**: Each file's USING directives do not affect other compilation units

```basic
REM ✓ CORRECT: USING comes first
USING Collections

NAMESPACE App
  CLASS MyApp
  END CLASS
END NAMESPACE

REM ✗ WRONG: USING after NAMESPACE
NAMESPACE App
  CLASS MyApp
  END CLASS
END NAMESPACE

USING Collections    REM Error: E_NS_005
```

### Semantics

**Simple form** (`USING Collections`):

- Makes all types from the namespace available for unqualified lookup
- Multiple USING directives accumulate
- Order matters: earlier imports checked first during resolution

**Alias form** (`USING Coll = Collections`):

- Creates a shorthand alias for the namespace path
- Alias can be used in type references: `Coll.List`
- Aliases must be unique within a file

### Examples

```basic
USING Collections
USING Utils.Helpers
USING DB = Application.Database

NAMESPACE Collections
  CLASS List
  END CLASS
END NAMESPACE

NAMESPACE Application.Database
  CLASS Connection
  END CLASS
END NAMESPACE

REM Unqualified via USING Collections
DIM myList AS List

REM Via alias
DIM conn AS DB.Connection
```

## Type Resolution

### Resolution Algorithm

When resolving an unqualified type reference (e.g., `MyClass`), the compiler searches in this order:

1. **Current namespace**: Types declared in the same namespace as the reference
2. **Parent namespaces**: Walk up the namespace hierarchy to the root
3. **USING imports**: Search imported namespaces in declaration order
4. **Global namespace**: Types declared at global scope

**Fully-qualified references** (e.g., `A.B.MyClass`) bypass this search and resolve directly.

### Examples

```basic
USING Foundation

NAMESPACE Foundation
  CLASS Entity
  END CLASS
END NAMESPACE

NAMESPACE App.Domain
  CLASS Service
    REM Resolution order:
    REM 1. App.Domain (current)
    REM 2. App (parent)
    REM 3. Global root (parent)
    REM 4. Foundation (via USING)
    DIM entity AS Entity    REM Found via USING Foundation
  END CLASS
END NAMESPACE
```

### Case Insensitivity

All namespace and type lookups are case-insensitive:

```basic
NAMESPACE MyLib
  CLASS Helper
  END CLASS
END NAMESPACE

REM All equivalent:
DIM h1 AS MyLib.Helper
DIM h2 AS MYLIB.HELPER
DIM h3 AS mylib.helper
DIM h4 AS MyLib.HELPER
```

## Reserved Namespaces

The `Zanna` root namespace is **reserved** for future built-in libraries (Track B). User code cannot:

- Declare namespaces under `Zanna` (e.g., `NAMESPACE Zanna.MyLib`)
- Use `USING Zanna` (the root itself)

This ensures future compatibility with ZannaLib.

```basic
REM ✗ WRONG: Reserved namespace
NAMESPACE Zanna.MyLib    REM Error: E_NS_009
END NAMESPACE

REM ✗ WRONG: Reserved root
USING Zanna              REM Error: E_NS_009
```

## Diagnostics Reference

The namespace system defines nine error codes covering all failure modes.

| Code     | Category  | Description                       |
|----------|-----------|-----------------------------------|
| E_NS_001 | Not Found | Namespace does not exist          |
| E_NS_002 | Not Found | Type not found in namespace       |
| E_NS_003 | Ambiguity | Type reference is ambiguous       |
| E_NS_004 | Duplicate | Alias already defined             |
| E_NS_005 | Placement | USING after declaration           |
| E_NS_006 | Structure | Nested USING not allowed          |
| E_NS_007 | Reserved  | Global type shadowed by namespace |
| E_NS_008 | Placement | USING inside namespace block      |
| E_NS_009 | Reserved  | Zanna namespace reserved          |

### E_NS_001: Namespace Not Found

**Cause**: USING directive references a namespace that doesn't exist.

```basic
USING NonExistent    REM Error: E_NS_001

END
```

**Message**: `namespace not found: 'NONEXISTENT'`

**Fix**: Ensure the namespace is declared before using it (possibly in another file).

---

### E_NS_002: Type Not Found in Namespace

**Cause**: Fully-qualified reference to a type that doesn't exist in the specified namespace.

```basic
NAMESPACE Collections
  CLASS List
  END CLASS
END NAMESPACE

DIM d AS Collections.Dictionary    REM Error: E_NS_002
```

**Message**: `type 'DICTIONARY' not found in namespace 'COLLECTIONS'`

**Fix**: Check spelling or declare the missing type.

---

### E_NS_003: Ambiguous Type Reference

**Cause**: Multiple imported namespaces contain types with the same name.

```basic
USING A
USING B

NAMESPACE A
  CLASS Thing
    DIM x AS I64
  END CLASS
END NAMESPACE

NAMESPACE B
  CLASS Thing
    DIM y AS I64
  END CLASS
END NAMESPACE

CLASS MyClass : Thing    REM Error: E_NS_003
  DIM z AS I64
END CLASS
END
```

**Message**: `ambiguous type reference 'THING' (candidates: A.THING, B.THING)`

**Candidate list is sorted alphabetically for stability.**

**Fixes**:

1. Use fully-qualified name:

```basic
CLASS MyClass : A.Thing
```

2. Use namespace alias:

```basic
USING AliasA = A
CLASS MyClass : AliasA.Thing
```

---

### E_NS_004: Duplicate Alias

**Cause**: Same alias defined twice in the same file.

```basic
USING Coll = Collections
USING Coll = Utilities    REM Error: E_NS_004
```

**Message**: `duplicate alias 'COLL'`

**Fix**: Use unique aliases for different namespaces.

**Note**: This error is difficult to demonstrate in single-file tests due to ordering constraints (USING must come
before namespace declarations, but checking namespace existence happens before checking duplicate aliases).

---

### E_NS_005: USING After Declaration

**Cause**: USING directive appears after a NAMESPACE, CLASS, or INTERFACE declaration.

```basic
NAMESPACE App
  CLASS MyApp
  END CLASS
END NAMESPACE

USING Collections    REM Error: E_NS_005

END
```

**Message**: `USING must appear before all declarations`

**Fix**: Move all USING directives to the top of the file.

---

### E_NS_006: Nested USING Not Allowed

**Cause**: USING directive inside a NAMESPACE or CLASS block.

```basic
NAMESPACE App
  USING Collections    REM Error: E_NS_006
END NAMESPACE
```

**Message**: `USING cannot be nested inside namespace or class blocks`

**Fix**: Move USING to file scope.

---

### E_NS_007: Global Type Shadowed

**Cause**: A namespace type shadows a global type (reserved for future use).

**Note**: This diagnostic is defined but not actively enforced in Track A.

---

### E_NS_008: USING Inside Namespace Block

**Cause**: USING directive appears inside a NAMESPACE block.

```basic
NAMESPACE App
  USING Collections    REM Error: E_NS_008

  CLASS MyClass
  END CLASS
END NAMESPACE

END
```

**Message**: `USING inside NAMESPACE block not allowed`

**Fix**: Move USING to file scope before all NAMESPACE declarations.

---

### E_NS_009: Reserved Zanna Namespace

**Cause**: User code attempts to declare or use the reserved `Zanna` root namespace.

```basic
REM Example 1: Declaration
NAMESPACE Zanna.MyLib    REM Error: E_NS_009
  CLASS Helper
  END CLASS
END NAMESPACE

REM Example 2: USING
USING Zanna              REM Error: E_NS_009
```

**Message**: `namespace 'ZANNA' is reserved for built-in libraries`

**Fix**: Use a different root namespace for user code.

---

## Common Pitfalls and Solutions

### Pitfall 1: Ambiguous References

**Problem**: Importing multiple namespaces that define the same type name.

```basic
USING Collections
USING Utilities

REM Both define "List"
DIM myList AS List    REM Error: E_NS_003
```

**Solutions**:

1. **Fully-qualify the reference**:

```basic
DIM myList AS Collections.List
```

2. **Use namespace aliases**:

```basic
USING Coll = Collections
USING Util = Utilities
DIM myList AS Coll.List
```

3. **Remove unnecessary USING directives**:

```basic
USING Collections    REM Only import what you need
DIM myList AS List
```

---

### Pitfall 2: Wrong USING Placement

**Problem**: Placing USING after declarations.

```basic
NAMESPACE App
END NAMESPACE

USING Collections    REM Error: E_NS_005
```

**Solution**: Always put USING directives at the top of the file:

```basic
USING Collections

NAMESPACE App
END NAMESPACE
```

---

### Pitfall 3: USING Inside Namespace

**Problem**: Attempting to scope USING to a specific namespace.

```basic
NAMESPACE App
  USING Collections    REM Error: E_NS_008
END NAMESPACE
```

**Solution**: USING is always file-scoped. Put it at file scope:

```basic
USING Collections

NAMESPACE App
  REM Collections is now available
END NAMESPACE
```

---

### Pitfall 4: Reserved Zanna Namespace

**Problem**: Trying to use `Zanna` for user code.

```basic
NAMESPACE Zanna.MyApp    REM Error: E_NS_009
```

**Solution**: Choose a different root namespace:

```basic
NAMESPACE MyCompany.MyApp
```

---

### Pitfall 5: Case Mismatches (Not an Error)

**Not a problem**: BASIC is case-insensitive, so these are all valid:

```basic
NAMESPACE MyLib
  CLASS Helper
  END CLASS
END NAMESPACE

DIM h1 AS MyLib.Helper     REM OK
DIM h2 AS MYLIB.HELPER     REM OK (same as above)
DIM h3 AS mylib.helper     REM OK (same as above)
```

This is intentional BASIC behavior, not a pitfall.

---

## Multi-File Compilation

### File-Scoped USING

Each file's USING directives are **file-scoped** and do not affect other files:

**file1.bas**:

```basic
USING Collections

NAMESPACE App
  CLASS MyApp : List    REM OK: List available via USING
  END CLASS
END NAMESPACE
```

**file2.bas**:

```basic
NAMESPACE App
  CLASS OtherApp : Collections.List    REM Must fully-qualify: no USING here
  END CLASS
END NAMESPACE
```

### Namespace Merging Across Files

Multiple files can contribute to the same namespace:

**file1.bas**:

```basic
NAMESPACE Collections
  CLASS List
  END CLASS
END NAMESPACE
```

**file2.bas**:

```basic
NAMESPACE Collections
  CLASS Dictionary
  END CLASS
END NAMESPACE
```

Both `List` and `Dictionary` belong to `Collections` namespace after linking.

---

## Best Practices

1. **Use namespaces for organization**: Group related types together
2. **Avoid deep nesting**: Keep namespace hierarchies shallow (2-3 levels)
3. **Choose descriptive names**: `Graphics.Rendering` is better than `Gfx.Rnd`
4. **Use aliases for long paths**: `USING GFX = Graphics.Rendering.Advanced`
5. **Minimize USING imports**: Only import what you need to reduce ambiguity
6. **Prefer qualification over USING**: For rarely-used types, use fully-qualified names
7. **Document public namespaces**: Add comments explaining the purpose of each namespace
8. **Reserve root namespaces**: Choose a company or project root (e.g., `MyCompany.*`)

---

## Zanna.* Runtime Namespace (Implemented)

ZannaLib exposes runtime functions and types under the reserved `Zanna.*` root namespace. This
canonical namespace organization is now implemented and available in IL and BASIC code.

### Runtime Functions by Namespace

#### Zanna.Terminal

Console I/O operations:

- `Zanna.Terminal.PrintStr(str)->void` — Print string
- `Zanna.Terminal.PrintI64(i64)->void` — Print integer
- `Zanna.Terminal.PrintF64(f64)->void` — Print double
- `Zanna.Terminal.TryReadLine()->obj<Zanna.Option>` — Read line from console (`None` on EOF)
- `Zanna.Terminal.ReadLineResult()->obj<Zanna.Result>` — Read line from console (`Err` on EOF)
- `Zanna.Terminal.ReadLine()->str` — Compatibility read line API; prefer `TryReadLine()` or `ReadLineResult()` for EOF handling

#### Zanna.String

String manipulation:

- `Zanna.String.Concat(str,str)->str` — Concatenate strings
- `Zanna.String.FromI16(i16)->str` — Convert int16 to string
- `Zanna.String.FromI32(i32)->str` — Convert int32 to string
- `Zanna.String.FromSingle(f64)->str` — Convert float to string (formats with single precision)
- `Zanna.String.get_Length(str)->i64` — String length
- `Zanna.String.Mid(str,i64)->str` — Substring from position to end
- `Zanna.String.MidLen(str,i64,i64)->str` — Substring with start and length
- `Zanna.String.SplitFields(str)->seq<str>` — Split into fields
- `Zanna.Text.StringBuilder.New()->obj` — Create StringBuilder

#### Zanna.Core.Convert

Type conversion:

- `Zanna.Core.Convert.ToDouble(str)->f64` — String to double (throws on error; accepts `NaN`, `Inf`, and `-Inf`)
- `Zanna.Core.Convert.ToInt64(str)->i64` — String to int (throws on error)
- `Zanna.Core.Convert.NumToInt(f64)->i64` — Truncate/clamp double to int (`NaN` becomes `0`)
- `Zanna.Core.Convert.ToStringDouble(f64)->str` — Convert double to round-trip decimal string, including `NaN`, `Inf`, and `-Inf`
- `Zanna.Core.Convert.ToStringInt(i64)->str` — Convert int64 to string

`ToString_Double` and `ToString_Int` remain available as compatibility aliases.

#### Zanna.Core.Parse

Type parsing with explicit error handling:

- `Zanna.Core.Parse.TryInt(str)->obj<Zanna.Option>` — Try to parse integer
- `Zanna.Core.Parse.TryDouble(str)->obj<Zanna.Option>` — Try to parse double
- `Zanna.Core.Parse.TryBool(str)->obj<Zanna.Option>` — Try to parse boolean
- `Zanna.Core.Parse.IntOr(str,i64)->i64` — Parse integer or default
- `Zanna.Core.Parse.DoubleOr(str,f64)->f64` — Parse double or default
- `Zanna.Core.Parse.BoolOr(str,i1)->i1` — Parse boolean or default
- `Zanna.Core.Parse.IsInt(str)->i1` — Validate integer text
- `Zanna.Core.Parse.IsNum(str)->i1` — Validate numeric text
- `Zanna.Core.Parse.IntRadix(str,i64,i64)->i64` — Parse radix 2-36 integer or default; `+` and `-` are accepted only for decimal, prefixes are rejected

Numeric parsing accepts explicit `NaN`, `Inf`, `+Inf`, and `-Inf` spellings.
Use `TryDouble` and `DoubleOr` for double parsing; the former `TryNum` / `NumOr`
spellings were retired by the public-surface standardization and are no longer
available.

#### Zanna.Core.Diagnostics

Error and diagnostic utilities:

- `Zanna.Core.Diagnostics.Trap(str)->void` — Trigger runtime trap; control bytes in managed-string messages are escaped in diagnostics

### Runtime Classes (Zanna.*)

Canonical runtime classes are exposed under the `Zanna.*` root and are used directly by the BASIC frontend. These are
first‑class and tested:

#### Zanna.Core

- `Zanna.Core.Object` — Base class for all objects
    - Methods:
        - `Equals(OBJECT other) -> BOOL` — reference equality by default
        - `HashCode() -> I64` — process‑consistent hash derived from the object pointer
        - `IsNull() -> BOOL` — returns true if the object reference is null
        - `IsNull(obj) -> BOOL` — static null-safe test for any object reference
        - `ToString() -> STRING` — default returns the class qualified name
        - `TypeId() -> I64` — returns the compile-time type identifier
        - `TypeName() -> STRING` — returns the fully-qualified class name
- `Zanna.String` — Managed string type (BASIC `STRING` is an alias)
    - Properties: `Length -> I64`, `IsEmpty -> BOOL`
    - Methods:
        - `Substring(I64 start, I64 length) -> STRING` — Extract substring
        - `Concat(STRING other) -> STRING` — Concatenate strings
        - `Left(I64 count) -> STRING` — First N characters
        - `Right(I64 count) -> STRING` — Last N characters
        - `Mid(I64 start) -> STRING` — Substring from position to end
        - `MidLen(I64 start, I64 length) -> STRING` — Substring with length
        - `Trim() -> STRING` — Remove leading/trailing whitespace
        - `TrimStart() -> STRING` — Remove leading whitespace
        - `TrimEnd() -> STRING` — Remove trailing whitespace
        - `ToUpper() -> STRING` — Convert to uppercase
        - `ToLower() -> STRING` — Convert to lowercase
        - `IndexOf(STRING needle) -> I64` — Find first occurrence (1-based, 0 if not found)
        - `IndexOfFrom(I64 start, STRING needle) -> I64` — Find from position
        - `Chr(I64 code) -> STRING` — Character from ASCII code (static)
        - `Asc() -> I64` — ASCII code of first character

#### Zanna.Text

- `Zanna.Text.StringBuilder` — Mutable string builder
    - Ctor: `NEW()`
    - Properties: `Length -> I64`, `Capacity -> I64`
    - Methods: `Append(STRING) -> OBJECT` (returns the builder for chaining), `Clear() -> VOID`, `ToString() -> STRING`

#### Zanna.IO

- `Zanna.IO.File` — File operations class (static utility)
    - Methods (static): `Exists(STRING) -> BOOL`, `ReadAllText(STRING) -> STRING`,
      `WriteAllText(STRING,STRING) -> VOID`, `Delete(STRING) -> VOID`

#### Zanna.Collections

- `Zanna.Collections.List` — Dynamic list of object references (non‑generic)
    - Ctor: `NEW()`; Properties: `Len -> I64`, `IsEmpty -> BOOL`
    - Methods: `Clear()`, `Find(OBJECT)->I64`, `First()->OBJECT`, `Flip()`, `Get(I64)->OBJECT`, `Has(OBJECT)->BOOL`, `Insert(I64,OBJECT)`, `Last()->OBJECT`, `Pop()->OBJECT`, `Push(OBJECT)`, `Remove(OBJECT)->BOOL`, `RemoveAt(I64)`, `Set(I64,OBJECT)`, `Slice(I64,I64)->List`, `Sort()`, `SortDesc()`

#### Zanna.Math

- `Zanna.Math` — Mathematical functions (static utility)
    - Methods (static):
        - `Abs(F64) -> F64` — Absolute value (float)
        - `AbsInt(I64) -> I64` — Absolute value (integer)
        - `Sqrt(F64) -> F64` — Square root
        - `Sin(F64) -> F64` — Sine (radians)
        - `Cos(F64) -> F64` — Cosine (radians)
        - `Tan(F64) -> F64` — Tangent (radians)
        - `Atan(F64) -> F64` — Arctangent (radians)
        - `Floor(F64) -> F64` — Round down
        - `Ceil(F64) -> F64` — Round up
        - `Pow(F64, F64) -> F64` — Power function
        - `Log(F64) -> F64` — Natural logarithm
        - `Exp(F64) -> F64` — Exponential (e^x)
        - `Sgn(F64) -> F64` — Sign (-1, 0, +1) for float
        - `SgnInt(I64) -> I64` — Sign (-1, 0, +1) for integer
        - `Min(F64, F64) -> F64` — Minimum of two floats
        - `Max(F64, F64) -> F64` — Maximum of two floats
        - `MinInt(I64, I64) -> I64` — Minimum of two integers
        - `MaxInt(I64, I64) -> I64` — Maximum of two integers

#### Zanna.Math.Random

- `Zanna.Math.Random` — Random number generation class (constructor seeds the RNG)
    - Methods:
        - `Next() -> F64` — Return next random number in [0, 1)
        - `NextInt(I64 n) -> I64` — Return next random integer in [0, n)
        - `Seed(I64) -> VOID` — Seed the random number generator

#### Zanna.System.Environment

- `Zanna.System.Environment` — Command-line and environment (static utility)
    - Methods (static):
        - `GetArgumentCount() -> I64` — Number of program arguments
        - `GetArgument(I64 index) -> STRING` — Get program argument by zero-based index
        - `GetCommandLine() -> STRING` — Program arguments joined as a single string

#### Zanna.Time

- `Zanna.Time` — Time and timing utilities (static utility)
    - Methods (static):
        - `Clock.NowMs() -> I64` — Monotonic milliseconds (not the Unix epoch)
        - `Clock.NowMicros() -> I64` — Monotonic microseconds
        - `Clock.Sleep(I64 ms) -> VOID` — Pause execution for milliseconds

#### Zanna.Terminal

- `Zanna.Terminal` — Terminal/console control (static utility)
    - Methods (static):
        - `Bell() -> VOID` — Sound terminal bell
        - `Clear() -> VOID` — Clear the screen
        - `GetKey() -> STRING` — Wait for and return keypress
        - `GetKeyTimeout(I64 ms) -> STRING` — Wait with timeout (empty if timeout)
        - `InKey() -> STRING` — Non-blocking key check (empty if no key)
        - `SetAltScreen(Boolean enable) -> VOID` — Switch to/from alternate screen buffer
        - `SetColor(I64 fg, I64 bg) -> VOID` — Set foreground/background colors
        - `SetCursorVisible(Boolean visible) -> VOID` — Show/hide cursor (FALSE=hide, TRUE=show)
        - `SetPosition(I64 row, I64 col) -> VOID` — Move cursor position

**Note:** Legacy `Zanna.System.*` aliases for core namespaces have been removed. Use the canonical `Zanna.*` names, except for dedicated System services such as `Zanna.System.Clipboard`.

### Canonical Runtime Names

Runtime procedures are exposed only through canonical `Zanna.*` names. Legacy `rt_*` public aliases are intentionally unsupported.

### OOP Runtime vs Procedural Helpers

The OOP `Zanna.*` classes are the preferred surface for new code. Some canonical function-style entry points (e.g.,
`Zanna.String.get_Length`, `Zanna.IO.*`) remain available and are used internally by lowering bridges. Prefer equivalent
class property/method calls where they exist.

### Examples

Object methods on user classes:

```basic
NAMESPACE App
  CLASS C
  END CLASS
END NAMESPACE

DIM o AS App.C
o = NEW App.C()
PRINT o.ToString()   ' prints "App.C"
PRINT o.Equals(o)    ' 1
```

Working with strings via Zanna.String:

```basic
DIM s AS Zanna.String
s = "  Hello World  "
PRINT s.Length            ' 15
PRINT s.IsEmpty           ' 0
PRINT s.Trim()            ' "Hello World"
PRINT s.ToUpper()         ' "  HELLO WORLD  "
PRINT s.Left(7)           ' "  Hello"
PRINT s.Right(7)          ' "World  "
PRINT s.IndexOf("World")  ' 9
PRINT s.Substring(3, 5)   ' "Hello"
```

StringBuilder for efficient text composition:

```basic
DIM sb AS Zanna.Text.StringBuilder
sb = NEW Zanna.Text.StringBuilder()
sb.Append("hello").Append(", world")
PRINT sb.ToString()
```

File I/O using Zanna.IO.File:

```basic
USING Zanna.IO
File.WriteAllText("out.txt", "data")
IF File.Exists("out.txt") THEN
  PRINT File.ReadAllText("out.txt")
  File.Delete("out.txt")
END IF
```

In‑memory collections with List:

```basic
DIM list AS Zanna.Collections.List
list = NEW Zanna.Collections.List()
list.Push(NEW App.C())
PRINT list.Length
PRINT list.Get(0).ToString()
list.Set(0, list)
list.RemoveAt(0)
list.Clear()
```

Mathematical functions with Zanna.Math:

```basic
USING Zanna
PRINT Math.Sqrt(16)          ' 4
PRINT Math.Abs(-3.14)        ' 3.14
PRINT Math.Sin(0)            ' 0
PRINT Math.Pow(2, 10)        ' 1024
PRINT Math.Min(5.0, 3.0)     ' 3
PRINT Math.MaxInt(10, 20)    ' 20
```

Random numbers with Zanna.Math.Random:

```basic
USING Zanna.Math
DIM rng AS Random
rng = NEW Random()           ' Constructor seeds the RNG
rng.Seed(12345)              ' Re-seed for reproducibility
DIM r AS DOUBLE
r = rng.Next()               ' Returns value in [0, 1)
PRINT r
```

Command-line arguments with Zanna.System.Environment:

```basic
USING Zanna
DIM argc AS INTEGER
argc = Environment.GetArgumentCount()
PRINT "Arguments: "; argc
FOR i = 0 TO argc - 1
  PRINT "  "; i; ": "; Environment.GetArgument(i)
NEXT i
```

The `zanna` tool forwards only arguments after `--` to the program, so
`GetArgument(0)` is the first user argument, not the `zanna` executable name.

Timing with Zanna.Time:

```basic
USING Zanna
DIM start AS LONG
start = Time.Clock.NowMs()
' ... do work ...
Time.Clock.Sleep(100)            ' Pause 100ms
PRINT "Elapsed: "; Time.Clock.NowMs() - start; "ms"
```

Terminal control with Zanna.Terminal:

```basic
USING Zanna
Terminal.Clear()                    ' Clear screen
Terminal.SetColor(14, 1)            ' Yellow on blue
Terminal.SetPosition(10, 20)        ' Move cursor
PRINT "Hello!"
Terminal.SetCursorVisible(FALSE)        ' Hide cursor
DIM key AS STRING
key = Terminal.ReadKeyFor(5000)     ' Wait up to 5 seconds for a key
IF key <> "" THEN PRINT "You pressed: "; key
Terminal.SetCursorVisible(TRUE)        ' Show cursor
```

#### Zanna.Graphics

The Graphics namespace provides 2D rendering, image manipulation, and game development utilities.

**Zanna.Graphics.Canvas** — Window and drawing surface:
- Ctor: `NEW(STRING title, I64 width, I64 height)`
- Properties:
    - `Width -> I64` (read-only) — Canvas width in pixels
    - `Height -> I64` (read-only) — Canvas height in pixels
    - `ShouldClose -> I64` (read-only) — 1 if window close requested
- Methods:
    - `Clear(I64 color) -> VOID` — Fill with solid color
    - `Plot(I64 x, I64 y, I64 color) -> VOID` — Set single pixel
    - `Line(I64 x1, I64 y1, I64 x2, I64 y2, I64 color) -> VOID` — Draw line
    - `Box(I64 x, I64 y, I64 w, I64 h, I64 color) -> VOID` — Filled rectangle
    - `Frame(I64 x, I64 y, I64 w, I64 h, I64 color) -> VOID` — Rectangle outline
    - `Disc(I64 cx, I64 cy, I64 r, I64 color) -> VOID` — Filled circle
    - `Ring(I64 cx, I64 cy, I64 r, I64 color) -> VOID` — Circle outline
    - `Text(I64 x, I64 y, STRING text, I64 color) -> VOID` — Draw text
    - `Blit(I64 x, I64 y, PIXELS src) -> VOID` — Draw pixels (no alpha)
    - `BlitAlpha(I64 x, I64 y, PIXELS src) -> VOID` — Draw with alpha blending
    - `BlitRegion(I64 x, I64 y, PIXELS src, I64 sx, I64 sy, I64 sw, I64 sh) -> VOID` — Draw region
    - `GradientH(I64 x, I64 y, I64 w, I64 h, I64 c1, I64 c2) -> VOID` — Horizontal gradient
    - `GradientV(I64 x, I64 y, I64 w, I64 h, I64 c1, I64 c2) -> VOID` — Vertical gradient
    - `GetWindowX() -> I64`, `GetWindowY() -> I64` — Current window position in screen coordinates
    - `SetPosition(I64 x, I64 y) -> VOID` — Move the window
    - `GetMonitorWidth() -> I64`, `GetMonitorHeight() -> I64` — Current monitor size in pixels
    - `Poll() -> I64` — Process events, return event type
    - `KeyHeld(I64 keycode) -> I64` — Check if key is held down
    - `Flip() -> VOID` — Present frame and limit FPS

**Zanna.Graphics.Color** — Color utilities (static class):
- Methods (static):
    - `Rgb(I64 r, I64 g, I64 b) -> I64` — Create color from RGB (0-255)
    - `Rgba(I64 r, I64 g, I64 b, I64 a) -> I64` — Create color with alpha
    - `FromHsl(I64 h, I64 s, I64 l) -> I64` — Create from HSL (h:0-360, s:0-100, l:0-100)
    - `GetRed(I64 color) -> I64` — Extract red component
    - `GetGreen(I64 color) -> I64` — Extract green component
    - `GetBlue(I64 color) -> I64` — Extract blue component
    - `GetAlpha(I64 color) -> I64` — Extract stored alpha byte. `Color.Rgb()` has no stored alpha, so this returns `0`; drawing APIs treat RGB colors as opaque.
    - `Lerp(I64 c1, I64 c2, I64 t) -> I64` — Linear interpolation (t:0-100)
    - `Brighten(I64 color, I64 amount) -> I64` — Increase brightness
    - `Darken(I64 color, I64 amount) -> I64` — Decrease brightness

**Zanna.Graphics.Pixels** — Software image buffer:
- Ctor: `NEW(I64 width, I64 height)`
- Properties:
    - `Width -> I64` (read-only) — Image width in pixels
    - `Height -> I64` (read-only) — Image height in pixels
- Methods:
    - `Get(I64 x, I64 y) -> I64` — Get raw `0xRRGGBBAA` pixel storage
    - `GetRgba(I64 x, I64 y) -> I64` — Get raw `0xRRGGBBAA` pixel storage
    - `GetColor(I64 x, I64 y) -> I64` — Get pixel as a `Color`-compatible value
    - `Set(I64 x, I64 y, I64 color) -> VOID` — Set raw `0xRRGGBBAA` pixel storage
    - `SetRgba(I64 x, I64 y, I64 color) -> VOID` — Set raw `0xRRGGBBAA` pixel storage
    - `SetColor(I64 x, I64 y, I64 color) -> VOID` — Set pixel from `Color.Rgb/Rgba` or Canvas RGB
    - `Fill(I64 color) -> VOID` — Fill image with raw `0xRRGGBBAA`
    - `FillRgba(I64 color) -> VOID` — Fill image with raw `0xRRGGBBAA`
    - `FillColor(I64 color) -> VOID` — Fill image from `Color.Rgb/Rgba` or Canvas RGB
    - `Clear() -> VOID` — Clear to transparent black
    - `Copy(I64 dx, I64 dy, PIXELS src, I64 sx, I64 sy, I64 sw, I64 sh) -> VOID` — Copy region
    - `Clone() -> PIXELS` — Create copy of image
    - `Scale(I64 w, I64 h) -> PIXELS` — Nearest-neighbor scale
    - `Resize(I64 w, I64 h) -> PIXELS` — Bilinear interpolation scale
    - `FlipH() -> PIXELS` — Flip horizontally
    - `FlipV() -> PIXELS` — Flip vertically
    - `RotateClockwise() -> PIXELS` — Rotate 90° clockwise
    - `RotateCounterClockwise() -> PIXELS` — Rotate 90° counter-clockwise
    - `Rotate180() -> PIXELS` — Rotate 180°
    - `Invert() -> PIXELS` — Invert RGB colors
    - `Grayscale() -> PIXELS` — Convert to grayscale
    - `Tint(I64 color) -> PIXELS` — Apply color tint (multiply blend)
    - `Blur(I64 radius) -> PIXELS` — Apply box blur (radius 1-10)
    - `SaveBmp(STRING path) -> I64` — Save as BMP file
    - `ToBytes() -> BYTES` — Get raw pixel data
- Static methods:
    - `FromBytes(I64 width, I64 height, BYTES bytes) -> PIXELS` — Create from row-major RGBA bytes
    - `Load(STRING path) -> PIXELS` — Load PNG/JPEG/BMP/GIF by extension
    - `LoadBmp(STRING path) -> PIXELS`, `LoadPng(STRING path) -> PIXELS`, `LoadJpeg(STRING path) -> PIXELS`, `LoadGif(STRING path) -> PIXELS` — Format-specific loaders

**Zanna.Graphics.Sprite** — Animated sprite for 2D games:
- Ctor: `NEW(PIXELS pixels)` — Create sprite from image
- Static methods:
    - `FromFile(STRING path) -> SPRITE` — Load a sprite from BMP, PNG, JPEG, or GIF
- Properties:
    - `X -> I64` — X position
    - `Y -> I64` — Y position
    - `Width -> I64` (read-only) — Sprite width
    - `Height -> I64` (read-only) — Sprite height
    - `ScaleX -> I64` — Horizontal scale (100 = 100%)
    - `ScaleY -> I64` — Vertical scale (100 = 100%)
    - `Rotation -> I64` — Rotation in degrees
    - `Visible -> I64` — Visibility flag (0/1)
    - `Frame -> I64` — Current animation frame
    - `FrameCount -> I64` (read-only) — Number of animation frames
- Methods:
    - `Draw(CANVAS canvas) -> VOID` — Draw sprite to canvas
    - `SetOrigin(I64 x, I64 y) -> VOID` — Set rotation/scale origin
    - `AddFrame(PIXELS pixels) -> VOID` — Add animation frame
    - `SetFrameDelay(I64 ms) -> VOID` — Set animation delay
    - `GetFrameDelayAt(I64 frame) -> I64` — Get one frame's delay
    - `SetFrameDelayAt(I64 frame, I64 ms) -> VOID` — Set one frame's delay
    - `Update() -> VOID` — Advance animation based on time
    - `Overlaps(SPRITE other) -> I64` — AABB collision check
    - `Contains(I64 px, I64 py) -> I64` — Point-in-sprite test
    - `Move(I64 dx, I64 dy) -> VOID` — Move by delta

**Zanna.Graphics2D.Tilemap** — Tile-based map rendering:
- Ctor: `NEW(I64 width, I64 height, I64 tileWidth, I64 tileHeight)` — Create tilemap (size in tiles)
- Properties:
    - `Width -> I64` (read-only) — Map width in tiles
    - `Height -> I64` (read-only) — Map height in tiles
    - `TileWidth -> I64` (read-only) — Tile width in pixels
    - `TileHeight -> I64` (read-only) — Tile height in pixels
    - `TileCount -> I64` (read-only) — Number of tiles in tileset
- Methods:
    - `SetTileset(PIXELS pixels) -> VOID` — Set tileset image
    - `SetTile(I64 x, I64 y, I64 tileIndex) -> VOID` — Set tile at position
    - `GetTile(I64 x, I64 y) -> I64` — Get tile at position
    - `Fill(I64 tileIndex) -> VOID` — Fill all tiles
    - `Clear() -> VOID` — Clear all tiles to 0
    - `FillRect(I64 x, I64 y, I64 w, I64 h, I64 tileIndex) -> VOID` — Fill region
    - `Draw(CANVAS canvas, I64 offsetX, I64 offsetY) -> VOID` — Draw entire map
    - `DrawRegion(CANVAS c, I64 ox, I64 oy, I64 vx, I64 vy, I64 vw, I64 vh) -> VOID` — Draw visible region
    - `Load(STRING path) -> TILEMAP` — Load JSON tilemap data
    - `LoadCsv(STRING path, I64 tileWidth, I64 tileHeight) -> TILEMAP` — Load a CSV tile layer
    - `SetTileAnim(I64 baseTile, I64 startTile, I64 frameCount) -> VOID` — Register sequential tile animation
    - `SetTileAnimFrame(I64 baseTile, I64 frame, I64 tileId) -> VOID` — Override one animation frame
    - `UpdateAnims(I64 deltaMs) -> VOID` — Advance tile animation state
    - `ResolveAnimTile(I64 tileId) -> I64` — Resolve current visible tile for an animated base tile
    - `ToTileX(I64 pixelX) -> I64` — Convert pixel X to tile X
    - `ToTileY(I64 pixelY) -> I64` — Convert pixel Y to tile Y
    - `ToPixelX(I64 tileX) -> I64` — Convert tile X to pixel X
    - `ToPixelY(I64 tileY) -> I64` — Convert tile Y to pixel Y

**Zanna.Graphics.Camera** — 2D viewport camera:
- Ctor: `NEW(I64 viewWidth, I64 viewHeight)` — Create camera with viewport size
- Properties:
    - `X -> I64` — Camera world X position
    - `Y -> I64` — Camera world Y position
    - `Zoom -> I64` — Zoom level (100 = 100%)
    - `Rotation -> I64` — Rotation in degrees
    - `Width -> I64` (read-only) — Viewport width
    - `Height -> I64` (read-only) — Viewport height
- Methods:
    - `Follow(I64 worldX, I64 worldY) -> VOID` — Center on position
    - `ToScreenX(I64 worldX) -> I64` — Convert world to screen X
    - `ToScreenY(I64 worldY) -> I64` — Convert world to screen Y
    - `ToWorldX(I64 screenX) -> I64` — Convert screen to world X
    - `ToWorldY(I64 screenY) -> I64` — Convert screen to world Y
    - `Move(I64 dx, I64 dy) -> VOID` — Move by delta
    - `SetBounds(I64 minX, I64 minY, I64 maxX, I64 maxY) -> VOID` — Constrain camera
    - `ClearBounds() -> VOID` — Remove constraints

Graphics example - drawing and sprites:

```basic
' Create a window
DIM canvas AS Zanna.Graphics.Canvas
canvas = NEW Zanna.Graphics.Canvas("My Game", 800, 600)

' Create colors
DIM red AS INTEGER, blue AS INTEGER
red = Zanna.Graphics.Color.Rgb(255, 0, 0)
blue = Zanna.Graphics.Color.Rgb(0, 0, 255)

' Create and configure a sprite
DIM spriteImg AS Zanna.Graphics.Pixels
spriteImg = NEW Zanna.Graphics.Pixels(32, 32)
spriteImg.FillColor(red)

DIM player AS Zanna.Graphics.Sprite
player = NEW Zanna.Graphics.Sprite(spriteImg)
player.X = 100
player.Y = 100

' Game loop
WHILE canvas.ShouldClose = 0
    canvas.Poll()

    ' Move with arrow keys
    IF canvas.KeyHeld(262) THEN player.X = player.X + 2  ' Right
    IF canvas.KeyHeld(263) THEN player.X = player.X - 2  ' Left

    ' Draw
    canvas.Clear(blue)
    player.Draw(canvas)
    canvas.Flip()
WEND
END
```

---

## Known Limitations

- **Case-sensitive qualified procedure calls**: Lowercase qualified calls like `a.b.f()` fail when the procedure is
  defined as `A.B.F()`. This is a bug; all lookups should be case-insensitive per the specification.
- **File-scoped USING only**: USING directives are not inherited across compilation units; each file must declare its
  own imports.

## Future Enhancements

The following features are planned for future releases:

- **Namespace forwarding**: Re-export types from other namespaces
- **Namespace-level visibility**: Control which types are exported from a namespace
- **Nested namespace imports**: `USING Graphics.Rendering.*` to import all types

These features are not yet implemented in Track A.

---

## Summary

Zanna BASIC namespaces provide:

- **Clarity**: Fully-qualified names show exactly where types come from
- **Collision avoidance**: Multiple libraries can define the same type names
- **Convenience**: USING directives reduce verbosity
- **Organization**: Group related types hierarchically

The system is designed to be simple, predictable, and compatible with BASIC's case-insensitive semantics.

For more examples, see the golden tests in `src/tests/golden/basic/namespace_*.bas` and the e2e tests in
`src/tests/e2e/test_namespace_e2e.cpp`.
