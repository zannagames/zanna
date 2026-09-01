---
status: active
audience: public
last-verified: 2026-09-01
---

# Zanna BASIC — Reference

Complete language reference for Zanna BASIC. This document describes **statements**, **expressions & operators**, *
*built-in functions**, **object features**, and **I/O**. For a tutorial introduction, see *
*[BASIC Tutorial](../tutorials/basic-tutorial.md)**.

---

## Key Language Features

- **Assignment**: `LET` is optional — `LET X = 2` and `X = 2` are both accepted, for scalars, strings, compound updates, and array elements
- **Function calls**: Use `Name(args)` with parentheses (required in expressions). For statement-form
  SUB calls, parentheses are required when passing arguments; the legacy paren-less form for zero-arg
  SUBs is accepted in statement position.
- **Built-ins**: Must be called with parentheses, even with zero arguments (`RND()`, `INKEY$()`)
- **Arrays**: One-dimensional, zero-based; require `DIM` or `REDIM`; elements may be integer, string, or object refs
- **Short-circuit operators**: `ANDALSO` and `ORELSE` (vs. `AND` and `OR`)
- **Functions**: Return values with `RETURN`; subroutines use `SUB`
- **Objects**: `CLASS`, methods, fields, `ME`, `NEW`, `DELETE`, optional `DESTRUCTOR`
- **Enums**: `ENUM...END ENUM` blocks with named integer constants and dot-notation access

---

## Table of Contents

**Language Core**
- [Statements A–Z](#statements-az)
- [Expressions & Operators](#expressions--operators)
- [Built-in Functions](#built-in-functions)

**Object-Oriented Programming**
- [OOP Semantics](#oop-semantics) — Inheritance, interfaces, properties, destructors
- [Runtime Classes (Zanna.*)](#runtime-classes-zanna)
- [Runtime Classes Usage Examples](#runtime-classes-usage-examples)

**Modules & Namespaces**
- [ZannaLib & Namespaces](#zannalib--namespaces)
- [Reserved Root](#reserved-root)
- [Namespaces & USING](#namespaces--using)

**Quick Reference**
- [Keyword Index](#keyword-index)

---

## Statements A–Z

### '

Comment. A leading apostrophe starts a comment.

```basic
' Single-line comment
PRINT "Hello"
```

### CLASS / NEW / DELETE

Defines classes with scalar or array fields; construct with NEW, optionally free with DELETE. Array fields may be
declared with dimensions and accessed using `obj.field(index)`.

```basic
CLASS Counter
  X AS INTEGER
  SUB NEW()
   LET ME.X = 0
  END SUB
  SUB INC()
   LET ME.X = ME.X + 1
  END SUB
END CLASS

DIM c AS Counter
LET c = NEW Counter()
c.INC()
PRINT c.X
DELETE c
```

Array fields:

```basic
CLASS Buffer
  DIM data(8) AS INTEGER
END CLASS

DIM b AS Buffer
LET b = NEW Buffer()      ' data() allocated length 8 by constructor
LET b.data(0) = 42
PRINT b.data(0)
```

Notes:

- When an array field includes dimensions in the class definition, the constructor allocates the array to the specified
  length.
- String array fields are supported; element loads/stores retain/release strings automatically.

### BEEP

Emits a beep or bell sound.

```basic
BEEP
PRINT "Alert!"
```

### CLS

Clears the screen and moves the cursor home (1,1). No-op when stdout is not a TTY.

```basic
CLS
PRINT "Clean screen"
```

### CLOSE

Closes an open file number.

```basic
OPEN "out.txt" FOR OUTPUT AS #1
PRINT #1, "Hello"
CLOSE #1
```

### COLOR

Sets terminal foreground and background colors. Uses values 0-7 for normal colors, 8-15 for bright colors, or -1 to
leave that channel unchanged. `COLOR -1, -1` is a no-op (it leaves both channels unchanged); the runtime does not
provide a built-in "reset to defaults" form.

```basic
COLOR 15, 1   ' Bright white on blue
PRINT "Colored text"
COLOR 7, -1   ' Switch foreground to white, leave background as-is
```

### DESTRUCTOR

Optional destructor called on DELETE and finalization. Declared without parentheses.

```basic
CLASS WithFile
  FH AS INTEGER
  SUB NEW()
    OPEN "out.txt" FOR OUTPUT AS #1
  END SUB
  DESTRUCTOR
    CLOSE #1
  END DESTRUCTOR
END CLASS
```

### DIM

Declares a variable or array. Required for arrays; optional for scalars (to pin type).

```basic
DIM A(5)           ' array 0..5 (upper bound is inclusive)
DIM Flag AS BOOLEAN
```

Object arrays use a class type in the `AS` clause. Element assignments must be
object values, and `LBOUND`/`UBOUND` accept object arrays just like numeric and
string arrays.

```basic
CLASS Sprite
END CLASS

DIM Sprites(4) AS Sprite
DIM S AS Sprite
LET S = NEW Sprite()
LET Sprites(0) = S
PRINT LBOUND(Sprites)
PRINT UBOUND(Sprites)
```

### DO ... LOOP

Loop with condition at start or end (DO WHILE/UNTIL … LOOP or DO … LOOP UNTIL/WHILE).

```basic
LET I = 3
DO
  LET I = I - 1
LOOP UNTIL I = 0
```

### END

Terminates program execution immediately.

```basic
PRINT "before end"
END
PRINT "this never prints"
```

### ENUM

Declares a named set of integer constants. Variants are auto-numbered from 0, or may specify explicit values.

```basic
ENUM Direction
  NORTH           ' 0
  SOUTH           ' 1
  EAST            ' 2
  WEST            ' 3
END ENUM

LET d = Direction.NORTH
```

Explicit and mixed values:

```basic
ENUM Priority
  LOW             ' 0
  MEDIUM = 5      ' 5
  HIGH            ' 6
  CRITICAL        ' 7
END ENUM
```

Negative values are supported (`BACKWARD = -1`). Duplicate variant names produce a compile error.

### EXIT

Exits the nearest loop (EXIT FOR / EXIT DO).

```basic
FOR I = 1 TO 10
  IF I = 3 THEN EXIT FOR
  PRINT I
NEXT
```

### FOR ... NEXT

Counted loop; optional STEP.

```basic
FOR I = 1 TO 5
  PRINT I
NEXT
```

### GOSUB

Subroutine call to a line label; returns with RETURN (statement-form).

```basic
GOSUB MySub
PRINT "back"
END
MySub:
PRINT "in subroutine"
RETURN
```

---

## Runtime Classes (Zanna.*)

Runtime classes are built-in types provided by the Zanna runtime and exposed under the `Zanna.*` namespace. They behave
like objects with properties and methods but are implemented by canonical runtime extern functions. The receiver (
instance) is passed as the first argument when lowering to the runtime.

- Properties/methods lower to canonical `Zanna.*` externs with the receiver as arg0.
- Behavior and traps match the underlying runtime helpers.
- BASIC `STRING` is an alias of `Zanna.String`.
- Optional `NEW` is available when a constructor helper is defined.

### Example: `Zanna.String`

```basic
DIM s AS Zanna.String
LET s = "hello"
PRINT s.Length              ' prints 5
PRINT s.Substring(1, 3)     ' zero-based BYTE start, length → "ell"
PRINT s.Mid(2)              ' 1-based CODEPOINT start → "ello" (suffix from char 2)
```

Lowering equivalence (receiver as first argument):

- `s.Length` → `Zanna.String.get_Length(s)`
- `s.Substring(i, n)` → `Zanna.String.Substring(s, i, n)` — `i` is a 0-based byte offset.
- `s.Mid(i)` → `Zanna.String.Mid(s, i)` — `i` is a **1-based codepoint** position (matches BASIC `MID$`).
- `NEW Zanna.String(x)` is rejected: `Zanna.String` has no constructor
  (`error[E_RUNTIME_CLASS_NO_CTOR]`). Assign a string literal or expression directly.

Null and bounds:

- Accessing a property/method on a null receiver traps.
- `Substring` clamps negative starts to 0 and over-long lengths to the available bytes; zero-length
  results return the empty string. (See `rt_str_substr` in `src/runtime/core/rt_string_ops.c`.)
- `Mid` traps when `start < 1` (the legacy BASIC `MID$` semantics).

BASIC `STRING` alias:

```basic
DIM s AS STRING
LET s = "abcd"
PRINT s.Length   ' same as Zanna.String
```

### `Zanna.String` API

Properties:

- `Length: i64` → `Zanna.String.get_Length(string)`
- `IsEmpty: i1` → `Zanna.String.get_IsEmpty(string)`

Methods:

- `Substring(i64 start, i64 length) -> string` → `Zanna.String.Substring(string, i64, i64)` —
  `start` is a 0-based byte offset.
- `Mid(i64 start) -> string` → `Zanna.String.Mid(string, i64)` — `start` is a **1-based codepoint**
  index; suffix from `start` to end of string.
- `Concat(string other) -> string` → `Zanna.String.Concat(string, string)`

### `Zanna.Collections.List` (non-generic)

Canonical, non-generic list that stores object references (opaque `obj`). Type safety is enforced by user code; the
runtime does not check element types.

Properties:

- `Len: i64` → `Zanna.Collections.List.get_Count(obj)`

Methods:

- `Push(obj value) -> void` → `Zanna.Collections.List.Push(obj,obj)`
- `Clear() -> void` → `Zanna.Collections.List.Clear(obj)`
- `RemoveAt(i64 index) -> void` → `Zanna.Collections.List.RemoveAt(obj,i64)`
- `Get(i64 index) -> obj` → `Zanna.Collections.List.Get(obj,i64)`
- `Set(i64 index, obj value) -> void` → `Zanna.Collections.List.Set(obj,i64,obj)`

Semantics:

- Indexes are zero-based. Negative or out-of-range indexes trap at runtime (bounds error).
- `Clear()` resets `Len` to 0.
- Elements are stored as opaque references; no automatic type conversions are performed.

### GOTO

Unconditional jump to a label.

```basic
GOTO Skip
PRINT "skipped"
Skip:
PRINT "landed"
```

### IF ... THEN

Conditional execution; optional ELSEIF / ELSE; terminated by END IF.

```basic
IF X = 0 THEN
  PRINT "zero"
ELSEIF X < 0 THEN
  PRINT "negative"
ELSE
  PRINT "positive"
END IF
```

### INPUT

Reads tokens from standard input into variables.

```basic
INPUT "Name? ", N$
PRINT "Hello, "; N$
```

### LET

Assignment to a variable, array element, or field. Required for assignment statements.

```basic
LET X = 2
DIM A(3)
LET A(0) = X
```

### LINE INPUT

Reads an entire line into a string variable.

```basic
LINE INPUT "Line? ", L$
PRINT "You typed: "; L$
```

### LOCATE

Moves the terminal cursor to a 1-based row and column position. No-op when stdout is not a TTY.

```basic
CLS
LOCATE 10, 20
PRINT "Centered"
```

### ON ERROR GOTO

Installs an error handler at a label.

```basic
ON ERROR GOTO ErrHandler
OPEN "missing.txt" FOR INPUT AS #1
PRINT "opened"
END
ErrHandler:
PRINT "failed to open"
END
```

### TRY … CATCH

Structured error handling that scopes a handler to a lexical block.

Syntax:

```basic
TRY
  ' protected statements
CATCH [errVar]
  ' handler statements
END TRY
```

Behavior:

- Errors raised in the TRY body transfer to the CATCH block.
- After the CATCH block finishes, execution continues after `END TRY`.
- `errVar` (optional) is a local INTEGER (i64) visible only within the CATCH block.
- Inside CATCH, `ERR()` returns the error code (same meaning as in `ON ERROR` handlers).
- Nested TRY/CATCH is allowed; handlers stack in last-in–first-out order.
- Interaction with `ON ERROR GOTO`: a TRY installs a handler on top of any existing handler and pops it at `END TRY` (
  the outer `ON ERROR` handler remains active).

Examples

Divide by zero is caught and control resumes after the block:

```basic
TRY
  LET Z = 0
  LET X = 1 / Z
  PRINT "nope"
CATCH e
  PRINT "caught "; STR$(ERR())
END TRY
PRINT "after"
```

Output:

```text
caught 0
after
```

Nested TRY where the inner handler fires and does not leak outward:

```basic
TRY
  TRY
   OPEN "missing.txt" FOR INPUT AS #1
   PRINT "opened"
  CATCH
   PRINT "inner"
  END TRY
  PRINT "outer-body"
CATCH
PRINT "outer"
END TRY
```

Output:

```text
inner
outer-body
```

### OPEN

Opens a file and assigns it a file number (#).

```basic
OPEN "out.txt" FOR OUTPUT AS #1
PRINT #1, "Hello"
CLOSE #1
```

### PRINT

Writes to the console. ';' suppresses newline; ',' aligns to columns.

```basic
PRINT "Hello, world"
PRINT "A"; "B"      ' prints AB (no newline between)
PRINT "A", "B"      ' prints in columns
```

### PRINT #

Writes to a file using PRINT formatting.

```basic
OPEN "log.txt" FOR OUTPUT AS #1
PRINT #1, "Started"
CLOSE #1
```

### RANDOMIZE

Seeds the random number generator with a given value or current time.

```basic
RANDOMIZE 12345    ' Use specific seed
PRINT RND()        ' Reproducible sequence
```

```basic
RANDOMIZE TIMER    ' Seed from current time
PRINT RND()        ' Different each run
```

### REDIM

Resizes an existing array (contents may be reinitialized).

```basic
DIM A(2)           ' 0..1
REDIM A(10)        ' 0..9
```

### RESUME

Resumes execution after an error. Forms:

- `RESUME` — retry the statement that caused the error.
- `RESUME NEXT` — continue at the statement after the one that errored.
- `RESUME <label>` — jump to a numeric or named line label.

There is no special "end the program" form; use `END` from inside the handler instead.

> **Not implemented.** All three forms currently lower to a bare `trap`
> instruction, so the handler runs and the program then ends instead of
> resuming. Write handlers that finish the work themselves (or call `END`)
> rather than relying on `RESUME`. See
> [defect audit #22](../audit_09012026.md).

```basic
ErrHandler:
PRINT "failed to open"
RESUME NEXT
```

### RETURN

Returns from a FUNCTION to its caller, or from a GOSUB subroutine to its call site.

```basic
FUNCTION F(N)
  IF N < 0 THEN RETURN -1
  RETURN N * 2
END FUNCTION
```

```basic
GOSUB MySub
PRINT "back"
END
MySub:
PRINT "in subroutine"
RETURN   ' RETURN with no value in GOSUB context
```

### SEEK

Sets or queries the file position for a file number.

```basic
OPEN "data.bin" FOR BINARY AS #1
SEEK #1, 0        ' go to start
CLOSE #1
```

### SELECT CASE

Multi-way branch on a value; range and relational cases supported.

```basic
SELECT CASE N
CASE < 0: PRINT "neg"
CASE 0:   PRINT "zero"
CASE 1 TO 9: PRINT "small"
CASE ELSE: PRINT "big"
END SELECT
```

### SUB / FUNCTION

Declares procedures and functions. Functions return a value via RETURN.

```basic
SUB HELLO(S$)
  PRINT "Hello, "; S$
END SUB

FUNCTION SQUARE(N)
  RETURN N * N
END FUNCTION

HELLO("Ada")            ' statement call (parentheses required)
LET X = SQUARE(9)       ' function in expression
```

### WHILE ... WEND

Loop while a condition is true.

```basic
LET I = 0
WHILE I < 3
  PRINT I
  LET I = I + 1
WEND
```

### WRITE #

Writes comma-delimited data with quotes, to a file number.

```basic
OPEN "out.csv" FOR OUTPUT AS #1
WRITE #1, 1, "two", 3.0
CLOSE #1
```

## Expressions & operators

Numeric literals may be decimal, hexadecimal, or binary. Hexadecimal integers
accept classic BASIC `&H2A` and C-style `0x2A` prefixes; binary integers accept
`&B101010` and `0b101010`. Hex and binary literals are integer literals and may
use the `%` or `&` integer suffixes. Floating literals use decimal notation with
optional exponent and `!` or `#` suffixes.

- Arithmetic: `+ - *`, `/` (floating-point division), `\` (integer division), `MOD`
- Comparison: `= <> < <= > >=`
- Logical words: `NOT`, `AND`, `OR`
- Short-circuit booleans: **`ANDALSO`**, **`ORELSE`**
- String concatenation: `+`

**Precedence (high → low)**: unary (`NOT`), `* / \ MOD`, `+ -`, comparisons, `ANDALSO/ORELSE`, `AND/OR`.

`NOT`, `AND`, and `OR` are eager BASIC logical-word operators:
- On boolean values, they behave like ordinary boolean logic.
- On integer values, `NOT` is bitwise complement and `AND`/`OR` are bitwise operations.
- `ANDALSO` and `ORELSE` are the boolean-only short-circuit forms.

## Built-in functions

The following built-ins are available. Use them in expressions (e.g., `LET X = ABS(-3)`).

| Name    | Args | Returns |
|---------|------|---------|
| ABS     | 1    | Numeric |
| ASC     | 1    | Integer |
| ATN     | 1    | Float   |
| CDBL    | 1    | Float   |
| CEIL    | 1    | Numeric |
| CHR$    | 1    | String  |
| CINT    | 1    | Integer |
| CLNG    | 1    | Integer |
| COS     | 1    | Float   |
| CSNG    | 1    | Float   |
| EOF     | 1    | Integer |
| ERR     | 0    | Integer |
| EXP     | 1    | Float   |
| FIX     | 1    | Numeric |
| FLOOR   | 1    | Numeric |
| GETKEY$ | 0    | String  |
| INKEY$  | 0    | String  |
| INSTR   | 2–3  | Integer |
| INT     | 1    | Numeric |
| LCASE$  | 1    | String  |
| LEFT$   | 2    | String  |
| LEN     | 1    | Integer |
| LOC     | 1    | Integer |
| LOF     | 1    | Integer |
| LOG     | 1    | Float   |
| LTRIM$  | 1    | String  |
| MID$    | 2–3  | String  |
| POW     | 2    | Numeric |
| RIGHT$  | 2    | String  |
| RND     | 0    | Float   |
| ROUND   | 1–2  | Numeric |
| RTRIM$  | 1    | String  |
| SGN     | 1    | Integer |
| SIN     | 1    | Float   |
| SQR     | 1    | Float   |
| STR$    | 1    | String  |
| TAN     | 1    | Float   |
| TIMER   | 0    | Integer |
| TRIM$   | 1    | String  |
| UCASE$  | 1    | String  |
| VAL     | 1    | Float   |

### Built-in examples

**Numeric**

```basic
PRINT ABS(-3)         ' 3
PRINT SQR(9)          ' 3
PRINT INT(3.9)        ' 3
PRINT FIX(-3.9)       ' -3
PRINT ROUND(2.6)      ' 3
PRINT FLOOR(2.6)      ' 2
PRINT CEIL(2.1)       ' 3
PRINT POW(2, 10)      ' 1024
PRINT SIN(0), COS(0)  ' 0  1
PRINT TAN(0)          ' 0
PRINT ATN(1) * 4      ' 3.14159... (pi)
PRINT EXP(1)          ' 2.71828... (e)
PRINT LOG(2.71828)    ' ~1  (natural log)
PRINT SGN(-5)         ' -1
PRINT SGN(0)          ' 0
PRINT TIMER()         ' monotonic milliseconds (not seconds, and not time-of-day)
PRINT ERR()           ' current error code (0 = none)
```

**String**

```basic
PRINT LEN("abc")            ' 3
PRINT LEFT$("hello", 2)     ' "he"
PRINT RIGHT$("hello", 3)    ' "llo"
PRINT MID$("hello", 2, 2)   ' "el"
PRINT INSTR("banana", "na") ' 3
PRINT LTRIM$("  hi")        ' "hi"
PRINT RTRIM$("hi  ")        ' "hi"
PRINT TRIM$("  hi  ")       ' "hi"
PRINT UCASE$("hi")          ' "HI"
PRINT LCASE$("HI")         ' "hi"
PRINT CHR$(65)             ' "A"
PRINT ASC("A")             ' 65
```

**Conversion & random**

```basic
PRINT CINT(3.9)             ' 4
PRINT CLNG(3.9)             ' 4
PRINT CSNG(3.5)             ' 3.5
PRINT CDBL(3.5)             ' 3.5
PRINT VAL("42")             ' 42
PRINT STR$(42)              ' "42" (no leading space)
PRINT RND()                 ' 0 <= x < 1
```

**Keyboard**

```basic
LET K$ = INKEY$()
IF K$ = "" THEN K$ = GETKEY$()
PRINT "Key: "; K$
```

**File query**

```basic
OPEN "in.txt" FOR INPUT AS #1
PRINT EOF(#1)         ' 0 until end of file
PRINT LOF(#1)         ' file length in bytes
PRINT LOC(#1)         ' current byte position
CLOSE #1
```

## ZannaLib & Namespaces

ZannaLib exposes procedures and types under the reserved `Zanna.*` root namespace. You can call
procedures fully qualified, or import a namespace with `USING`.

- Fully qualified:

```basic
Zanna.Terminal.PrintI64(42)
```

- With USING (same line using `:`):

```basic
USING Zanna.Terminal : PrintI64(42)
```

- Or as two lines:

```basic
USING Zanna.Terminal
PrintI64(42)
```

### Runtime Procedures

All runtime procedures are available under canonical `Zanna.*` namespace names. Legacy `rt_*` public aliases are intentionally unsupported. Signatures shown as `Name(params)->result`.

#### Zanna.Terminal

Console I/O operations:

- `Zanna.Terminal.PrintI64(i64)->void` — Print an integer to console
- `Zanna.Terminal.PrintF64(f64)->void` — Print a floating-point number to console
- `Zanna.Terminal.PrintStr(str)->void` — Print a string to console
- `Zanna.Terminal.TryReadLine()->obj<Zanna.Option>` — Read a line from console input (`None` on EOF)
- `Zanna.Terminal.ReadLineResult()->obj<Zanna.Result>` — Read a line from console input (`Err` on EOF)
- `Zanna.Terminal.ReadLine()->str` — Compatibility read line API; prefer `TryReadLine()` or `ReadLineResult()` for EOF handling

#### Zanna.String

String manipulation:

- `Zanna.String.get_Length(str)->i64` — Get string length
- `Zanna.String.Mid(str, i64, i64)->str` — Extract substring (start, length)
- `Zanna.String.Concat(str, str)->str` — Concatenate two strings
- `Zanna.String.SplitFields(str)->seq<str>` — Split string into fields
- `Zanna.String.FromI16(i16)->str` — Convert int16 to string
- `Zanna.String.FromI32(i32)->str` — Convert int32 to string
- `Zanna.String.FromSingle(f64)->str` — Convert float to string (formats with single precision)
- `Zanna.Text.StringBuilder.New()->obj` — Create a new StringBuilder instance

#### Zanna.Core.Convert

Type conversion:

- `Zanna.Core.Convert.ToInt64(str)->i64` — Convert string to integer (throws on error)
- `Zanna.Core.Convert.ToDouble(str)->f64` — Convert string to double (throws on error; accepts `NaN`, `Inf`, and `-Inf`)
- `Zanna.Core.Convert.NumToInt(f64)->i64` — Truncate finite double to integer; NaN becomes 0 and out-of-range values clamp
- `Zanna.Core.Convert.ToStringInt(i64)->str` — Convert integer to string
- `Zanna.Core.Convert.ToStringDouble(f64)->str` — Convert double to round-trip decimal string, including `NaN`, `Inf`, and `-Inf`

`ToString_Int` and `ToString_Double` remain available as compatibility aliases.

#### Zanna.Core.Parse

Type parsing (with explicit error handling):

- `Zanna.Core.Parse.TryInt(str)->obj<Zanna.Option>` — Try to parse integer
- `Zanna.Core.Parse.TryDouble(str)->obj<Zanna.Option>` — Try to parse double
- `Zanna.Core.Parse.TryBool(str)->obj<Zanna.Option>` — Try to parse boolean
- `Zanna.Core.Parse.IntOr(str, i64)->i64` — Parse integer or return default
- `Zanna.Core.Parse.DoubleOr(str, f64)->f64` — Parse double or return default
- `Zanna.Core.Parse.BoolOr(str, i1)->i1` — Parse boolean or return default
- `Zanna.Core.Parse.IsInt(str)->i1` — Validate integer text
- `Zanna.Core.Parse.IsNum(str)->i1` — Validate numeric text
- `Zanna.Core.Parse.IntRadix(str, i64, i64)->i64` — Parse radix 2-36 integer or return default; `+` and `-` are accepted only for decimal, prefixes are rejected

Numeric parsing accepts explicit `NaN`, `Inf`, `+Inf`, and `-Inf` spellings.
Use `TryDouble` and `DoubleOr` for double parsing; the former `TryNum` / `NumOr`
spellings were retired by the public-surface standardization and are no longer
available.

#### Zanna.Diagnostics

Error and diagnostic utilities:

- `Zanna.Core.Diagnostics.Trap(str)->void` — Trigger a runtime trap with message

### Runtime Types

ZannaLib classes are recognized under `Zanna.*`. These namespaced runtime types are known to the compiler for
declarations and construction. Their method surfaces are being exposed progressively.

#### Zanna

Core types:

- `Zanna.Core.Object` — Base class for all objects
- `Zanna.String` — Managed string type

#### Zanna.Text

Text processing types:

- `Zanna.Text.StringBuilder` — Mutable string builder (can be constructed with NEW)

#### Zanna.IO

I/O types:

- `Zanna.IO.File` — File operations class

#### Zanna.Collections

Collection types:

- `Zanna.Collections.List` — Dynamic list container

### Examples

Using runtime procedures:

```basic
USING Zanna.Terminal
USING Zanna.String

PrintStr("Length: ")
PrintI64(Len("hello"))
PrintStr(Concat("hello", " world"))
```

Using runtime types:

```basic
DIM sb AS Zanna.Text.StringBuilder
LET sb = NEW Zanna.Text.StringBuilder()
```

### Runtime Name Policy

Runtime APIs are canonical-only. Use the `Zanna.*` names listed above; legacy `rt_*` public aliases are not part of the runtime surface.

## Reserved Root

The `Zanna` root namespace is reserved for ZannaLib. User code may not declare symbols under `Zanna` (for
example, `NAMESPACE Zanna.Tools` is an error). You may import and call `Zanna.*` library procedures, but define your own
symbols under a different root.

## Namespaces & USING

**IMPORTANT**: USING directives are **file-scoped only** and must appear at the top of the file before any declarations.
USING directives cannot appear inside NAMESPACE, CLASS, or INTERFACE blocks.

### Correct Usage

```basic
' ✓ CORRECT: USING at file scope, before declarations
USING Zanna.Terminal

NAMESPACE App
  SUB Main()
    ' Use imported namespace without qualification
    PrintI64(99)
    ' Or fully qualified (always works)
    Zanna.Terminal.PrintI64(99)
  END SUB
END NAMESPACE

App.Main()
```

### Incorrect Usage

```basic
' ✗ WRONG: USING after NAMESPACE declaration (Error: E_NS_005)
NAMESPACE App
  SUB Main()
  END SUB
END NAMESPACE

USING Zanna.Terminal  ' Error: USING must appear before all declarations
```

```basic
' ✗ WRONG: USING inside NAMESPACE block
NAMESPACE App
  USING Zanna.Terminal  ' accepted by default, but the import does not work
  SUB Main()
  END SUB
END NAMESPACE
```

`E_NS_008` is only raised when runtime namespaces are disabled
(`--no-runtime-namespaces`). In the default configuration the directive is
accepted, has no effect, and the resulting module fails IL verification with
`unknown callee`. Keep `USING` at file scope. See
[defect audit #18](../audit_09012026.md).

### USING Rules

1. **File scope only**: USING must be at file scope, not inside any block
2. **Before declarations**: USING must appear before any NAMESPACE, CLASS, or INTERFACE declarations
3. **File-scoped effect**: Each file's USING directives do not affect other compilation units
4. **Two forms**:
    - Simple: `USING Zanna.Terminal` (imports all from namespace)
    - Aliased: `USING VC = Zanna.Terminal` (creates shorthand alias). The alias
      currently resolves **type** references only (`DIM w AS VC.Widget`,
      `NEW VC.Widget()`); an alias-qualified *call* such as `VC.PrintI64(7)`
      fails with `B1001: unknown variable 'VC'`. See
      [defect audit #19](../audit_09012026.md).

For complete namespace documentation, see [Namespace Reference](basic-namespaces.md).

## Keyword index

(All keywords are case-insensitive.)

### A

- `ABS`
- `ABSTRACT`
- `ADDFILE`
- `ADDRESSOF`
- `ALTSCREEN`
- `AND`
- `ANDALSO`
- `APPEND`
- `AS`

### B

- `BASE`
- `BEEP`
- `BINARY`
- `BOOLEAN`
- `BYREF`
- `BYVAL`

### C

- `CASE`
- `CATCH`
- `CEIL`
- `CLASS`
- `CLOSE`
- `CLS`
- `COLOR`
- `CONST`
- `COS`
- `CURSOR`

### D

- `DECLARE`
- `DELETE`
- `DESTRUCTOR`
- `DIM`
- `DO`

### E

- `EACH`
- `ELSE`
- `ELSEIF`
- `END`
- `ENUM`
- `EOF`
- `ERROR`
- `EXIT`
- `EXPORT`

### F

- `FALSE`
- `FINAL`
- `FINALLY`
- `FLOOR`
- `FOR`
- `FOREIGN`
- `FUNCTION`

### G

- `GET`
- `GOSUB`
- `GOTO`

### I

- `IF`
- `IMPLEMENTS`
- `IN`
- `INPUT`
- `INTERFACE`
- `IS`

### L

- `LBOUND`
- `LET`
- `LINE`
- `LOC`
- `LOCATE`
- `LOF`
- `LOOP`

### M

- `ME`
- `MOD`

### N

- `NAMESPACE`
- `NEW`
- `NEXT`
- `NOT`
- `NOTHING`

### O

- `OFF`
- `ON`
- `OPEN`
- `OR`
- `ORELSE`
- `OUTPUT`
- `OVERRIDE`

### P

- `POW`
- `PRESERVE`
- `PRINT`
- `PRIVATE`
- `PROPERTY`
- `PUBLIC`

### R

- `RANDOM`
- `RANDOMIZE`
- `REDIM`
- `RESUME`
- `RETURN`
- `RND`

### S

- `SEEK`
- `SELECT`
- `SET`
- `SHARED`
- `SIN`
- `SLEEP`
- `SQR`
- `STATIC`
- `STEP`
- `SUB`
- `SWAP`

### T

- `THEN`
- `TO`
- `TRUE`
- `TRY`
- `TYPE`

### U

- `UBOUND`
- `UNTIL`
- `USING`

### V

- `VIRTUAL`

### W

- `WEND`
- `WHILE`
- `WRITE`

### TRY/CATCH and ON ERROR Interop

TRY/CATCH composes with legacy `ON ERROR GOTO` as a nested handler:

- Precedence: a TRY installs a fresh error handler on top of any active `ON ERROR GOTO` handler.
- Restoration: when the TRY region finishes without error, the TRY handler is popped and the prior `ON ERROR GOTO`
  handler remains in effect.
- Catch resumption: the CATCH body terminates by resuming to the first block after the TRY via a resume token. A
  `RESUME` inside a CATCH is allowed but typically unnecessary because the compiler emits a `resume.label` to the join
  point.

Example:

```basic
ON ERROR GOTO Outer
TRY
  ' protected code
CATCH err
  ' handle inner error (err is i64 here)
END TRY
' Outer ON ERROR handler still active
END

Outer:
RESUME NEXT
```

Semantics:

- Exceptions raised in the TRY body are caught by the TRY handler.
- After `END TRY`, the previously active `ON ERROR GOTO Outer` handler continues to apply.

## Runtime Classes Usage Examples

Runtime-backed classes expose an object surface (properties, methods, constructors) that lower to canonical extern
functions provided by the runtime. Two families are currently available:

- Zanna.String (aliased in BASIC as STRING)
- Zanna.Text.StringBuilder

These object members are functional equivalents of the procedural helpers under Zanna.String.* and Zanna.Text.*. The
compiler lowers property and method calls to the corresponding extern with the receiver as argument 0. BASIC class
instances can call Object base methods such as `ToString`, `Equals`, and `HashCode`; those calls lower through the
canonical `Zanna.Core.Object` runtime class.

Examples:

```basic
DIM s AS STRING                 ' STRING is an alias of Zanna.String
LET s = "hello"
Zanna.Terminal.PrintI64(s.Length)
Zanna.Terminal.PrintStr(s.Substring(2, 3))  ' zero-based start, length 3

DIM sb AS Zanna.Text.StringBuilder
LET sb = NEW Zanna.Text.StringBuilder()
' Depending on your build, APPEND may be a reserved keyword; use the procedural form below if needed.
' sb.Append("X")
' or equivalently (procedural): sb = Zanna.Text.StringBuilder.Append(sb, "X")
Zanna.Terminal.PrintI64(sb.Length)
Zanna.Terminal.PrintStr(sb.ToString())
```

Conventions and semantics:

- Properties and methods lower to canonical externs with the receiver as arg0.
    - Examples: s.Length → call @Zanna.String.get_Length(s);
      s.Substring(i,n) → call @Zanna.String.Substring(s,i,n);
      s.Mid(i) → call @Zanna.String.Mid(s,i);
      sb.ToString() → call @Zanna.Text.StringBuilder.ToString(sb)
- Object base method calls use `Zanna.Core.Object.*`; the legacy `Zanna.Object.*` spelling is not emitted.
- STRING alias: The BASIC type STRING is the same nominal runtime class as Zanna.String.
- Index base: Substring uses a 0-based byte offset; Mid/MidLen use a 1-based UTF-8 codepoint offset.
- Null receivers trap: Accessing a property or method on a null object raises a runtime trap that can be caught with
  TRY/CATCH.
- Procedural equivalence: For every object member there is a procedural helper under Zanna.String.* or Zanna.Text.*
  with the receiver passed explicitly as the first argument. Use these forms where convenient or when a member name
  collides with a BASIC keyword (e.g., APPEND).

Cross-reference: See [ZannaLib & Namespaces](#zannalib--namespaces) for procedural helpers under
Zanna.String.* and Zanna.Text.*.

---

## OOP Semantics

### Inheritance

Single inheritance only (`CLASS B : A`). Namespaces may qualify base names, e.g. `CLASS B : Foo.Bar.A`.

### Method modifiers

- `VIRTUAL`: Declares a new virtual method. A vtable slot is introduced if the name does not already exist in a base.
- `OVERRIDE`: Reuses the slot of the closest base virtual with the same name. Signature must match exactly.
- `ABSTRACT`: Method has no body; must be implemented in a non-abstract descendant.
  Combine with `VIRTUAL` (`VIRTUAL ABSTRACT`) — `ABSTRACT` alone introduces no
  vtable slot, so a descendant's `OVERRIDE` fails with
  `B2104: cannot override non-virtual`. Instantiating an abstract class is
  rejected with `B2106`.
- `FINAL`: Prevents further overrides in descendants.
- `PUBLIC|PRIVATE`: Access control enforced at call/field-access sites.

Constructors (`SUB NEW`) may not be marked `VIRTUAL`, `OVERRIDE`, `ABSTRACT`, or `FINAL`.

### Slot assignment

Slot numbering is base-first, stable, and append-only: a class inherits its base's vtable verbatim, each new `VIRTUAL`
appends one entry, and `OVERRIDE` keeps the base slot number. This guarantees deterministic ABI layout.

### Virtual dispatch

- Virtual calls dispatch through the vtable using the receiver's dynamic type.
- Non-virtual methods use direct calls.
- `BASE.M(...)` is a direct call to the immediate base class implementation.

### Interfaces and RTTI

Interfaces are nominal: the name identifies the contract. A class lists one or more interface names on its declaration.

```basic
INTERFACE I
  SUB Speak()
  FUNCTION F() AS I64
END INTERFACE

CLASS A IMPLEMENTS I
  OVERRIDE SUB Speak(): END SUB
  OVERRIDE FUNCTION F() AS I64: RETURN 0: END FUNCTION
END CLASS
```

Each interface assigns slot indices to members in declaration order. A class that implements multiple interfaces has one
itable per interface. Dispatch materializes the callee pointer from the itable and emits `call.indirect`.

RTTI operators:
- `expr IS Class` — true iff the dynamic type equals or derives from the class.
- `expr IS Interface` — true iff the dynamic type implements the interface.
- `expr AS Class` — returns the object when IS succeeds; otherwise `NOTHING`.
- `expr AS Interface` — returns the object when IS succeeds; otherwise `NOTHING`.
  Test the result with `Zanna.Core.Object.RefEquals(x, NOTHING)`.

### Static members

- `STATIC SUB NEW()` is intended to run once per class during module
  initialization before user code.
- Static fields are intended to lower to module-scope globals, with reads/writes
  independent of any instance.
- Static methods do not receive `ME`.

> **Static members are largely non-functional today.** A `STATIC` field fails to
> compile (`global has malformed name @C::VALUE`); `STATIC SUB NEW()` never runs;
> `STATIC DESTRUCTOR` never runs; a static method resolves only through an
> instance receiver (`c.Ping()`, not `C.Ping()`); and referencing `ME` inside a
> `STATIC SUB` is accepted rather than rejected. See
> [defect audit](../audit_09012026.md) entries 1, 2, 3, 9, and 16.

### Properties

A `PROPERTY` defines up to two accessors (`GET` and `SET`). Accessor-level access modifiers are supported.

Property sugar:
- `x.Name` → `get_Name(x)` ; `x.Name = v` → `set_Name(x, v)` (instance)
- `C.Name` → `get_Name()` ; `C.Name = v` → `set_Name(v)` (static)

### Destructors and disposal

- Chaining order: derived body runs first, then base, continuing up the chain.
- One instance destructor per class; no parameters or return value.
- `STATIC DESTRUCTOR` is intended to run at program shutdown in class
  declaration order, but currently never runs — see the note under
  [Static members](#static-members).
- `DELETE expr` invokes the derived→base destructor chain and releases storage when retain count drops to zero.
- Deleting `NOTHING` (or an unassigned object variable) is a no-op; double-delete
  traps in debug builds. (The BASIC keyword is `DELETE`; there is no `DISPOSE`
  keyword.) Note that `NOTHING` can only be *assigned* — comparing against it
  requires `Zanna.Core.Object.RefEquals(obj, NOTHING)`.

### Overload resolution

Conversions: only widening numeric (INTEGER→DOUBLE) are permitted implicitly. For each viable candidate, parameters are
scored positionally: exact match (+2), widening numeric (+1), otherwise not viable. The best total score wins; ties are
ambiguous. Property accessors (`get_*/set_*`) participate as ordinary candidates.
