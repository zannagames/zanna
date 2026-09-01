---
status: active
audience: public
last-verified: 2026-09-01
---

# Zanna BASIC — Tutorial

Learn Zanna BASIC by example. For a complete reference, see **[BASIC Reference](../languages/basic-reference.md)**.

> **What is Zanna BASIC?**
> A compact, modernized BASIC designed for clarity: `LET` for assignment, clean arrays, short-circuit booleans,
> lightweight objects, and straightforward console/file I/O. It runs on Zanna's VM and can be lowered to native code.

---

## Table of Contents

1. [First Steps: Printing and Expressions](#1-first-steps-printing-and-expressions)
2. [Variables and Types](#2-variables-and-types)
3. [Control Flow](#3-control-flow)
4. [Procedures and Functions](#4-procedures-and-functions)
5. [Objects](#5-objects)
6. [Organizing Code with Namespaces](#6-organizing-code-with-namespaces)
7. [Console and File I/O](#7-console-and-file-io)
8. [Error Handling](#8-error-handling)
9. [Example Projects](#9-example-projects)
10. [Where to Go Next](#10-where-to-go-next)

---

## 1. First Steps: Printing and Expressions

```basic
PRINT "Hello, world!"
PRINT 1 + 2 * 3       ' 7
PRINT "A"; "B"        ' "AB" (no newline)
```

**Key points:**

- Use `:` to put multiple statements on one line
- `'` starts a single-line comment
- `;` in PRINT suppresses the newline between values

---

## 2. Variables and Types

### Assignment with LET

The `LET` keyword is optional; this tutorial includes it for clarity. Variables can be declared implicitly by assigning,
or explicitly with `DIM` to pin a type.

```basic
LET I = 42        ' with LET
J = 7             ' LET is optional
DIM Flag AS BOOLEAN
LET Flag = TRUE
```

### Arrays

Arrays **require** `DIM` and are zero-based:

```basic
DIM A(3)              ' indices 0..3 — the bound is inclusive
LET A(0) = 42
LET A(1) = 100
PRINT A(0)            ' 42
PRINT UBOUND(A)       ' 3
```

Arrays can store integers, strings, or object references. Object arrays are
declared with a class type and accept object values; `LBOUND` and `UBOUND` work
the same way as they do for numeric and string arrays.

```basic
CLASS Sprite
END CLASS

DIM Sprites(4) AS Sprite
DIM S AS Sprite
LET S = NEW Sprite()
LET Sprites(0) = S
PRINT UBOUND(Sprites)
```

### Type Suffixes

| Suffix | Type    |
|--------|---------|
| `!`    | Float   |
| `#`    | Float   |
| `$`    | String  |
| `%`    | Integer |
| `&`    | Integer |

```basic
LET S$ = "hello"
LET F# = 3.14
LET I% = 42
```

---

## 3. Control Flow

### IF ... THEN ... ELSE

```basic
IF N = 0 THEN
  PRINT "zero"
ELSEIF N < 0 THEN
  PRINT "negative"
ELSE
  PRINT "positive"
END IF
```

### FOR ... NEXT

```basic
FOR I = 1 TO 5
  PRINT I
NEXT

' With STEP
FOR I = 10 TO 1 STEP -2
  PRINT I
NEXT
```

### DO ... LOOP

```basic
' Condition at start
DO WHILE X < 10
  PRINT X
  LET X = X + 1
LOOP

' Condition at end
LET I = 3
DO
  LET I = I - 1
  PRINT I
LOOP UNTIL I = 0
```

### WHILE ... WEND

```basic
LET X = 0
WHILE X < 3
  PRINT X
  LET X = X + 1
WEND
```

### Short-Circuit Operators

Use `ANDALSO` and `ORELSE` for short-circuit evaluation:

```basic
IF A <> 0 ANDALSO (B / A) > 2 THEN
  PRINT "Safe division"
END IF

IF X = 0 ORELSE Y / X > 1 THEN
  PRINT "Conditional evaluation"
END IF
```

> **Note:** `AND` and `OR` always evaluate both operands. Use `ANDALSO` and `ORELSE` to avoid errors like division by
> zero.

---

## 4. Procedures and Functions

### Subroutines (SUB)

Subroutines are called as statements. Parentheses are recommended and required when passing
arguments. For zero-argument SUBs, the parser also accepts the legacy form without
parentheses in statement position.

```basic
SUB GREET(NAME$)
  PRINT "Hello, "; NAME$
END SUB

SUB BANNER()
  PRINT "*****"
END SUB

GREET("Ada")          ' Statement call with parentheses (required when passing args)
BANNER()              ' Zero-arg call with parentheses
BANNER                ' Legacy: zero-arg call without parentheses (statement form)
```

### Functions (FUNCTION)

Functions return values and are used in expressions:

```basic
FUNCTION SQUARE(N)
  RETURN N * N
END FUNCTION

LET X = SQUARE(9)     ' X = 81
PRINT SQUARE(5)       ' 25
```

### Exporting Functions

Use `EXPORT` before `FUNCTION` or `SUB` to make a procedure visible to other
modules in a mixed-language project:

```basic
EXPORT FUNCTION Factorial(n AS LONG) AS LONG
    IF n <= 1 THEN
        Factorial = 1
    ELSE
        Factorial = n * Factorial(n - 1)
    END IF
END FUNCTION
```

### Importing Foreign Functions

Use `DECLARE FOREIGN` to declare a function defined in another module (e.g., a
Zia library). The declaration has no body:

```basic
DECLARE FOREIGN FUNCTION ZiaHelper(n AS LONG) AS LONG
DECLARE FOREIGN SUB InitSystem()

PRINT ZiaHelper(42)
InitSystem()
```

For full details on mixed-language projects, see the
[Cross-Language Interop Guide](../languages/interop.md).

---

## 5. Objects

Zanna BASIC supports lightweight object-oriented programming with classes, methods, constructors, and destructors.

Array fields in classes
-----------------------

Fields may be scalars or arrays. Declare array fields with dimensions inside `CLASS`; access elements with
`obj.field(index)` similarly to regular arrays. When an array field declares a fixed extent, the constructor initializes
it to that length; otherwise, assign an array handle at runtime before use.

```basic
CLASS Board
  DIM cells(4) AS INTEGER   ' array field
END CLASS

DIM b AS Board
LET b = NEW Board()               ' cells() allocated length 4 by constructor
LET b.cells(0) = 1
LET b.cells(1) = 2
PRINT b.cells(0) + b.cells(1)     ' 3
```

Notes:

- Single-dimension array field access is supported using `obj.field(i)`. Multi-dimension field access will be added;
  non-member arrays already support multiple indices.
- String array fields are supported; assignment manages string lifetimes under the hood.

### Basic Class

```basic
CLASS Counter
  X AS INTEGER

  SUB NEW()
    LET ME.X = 0
  END SUB

  SUB INCREMENT()
    LET ME.X = ME.X + 1
  END SUB

  FUNCTION VALUE()
    RETURN ME.X
  END FUNCTION
END CLASS

DIM C AS Counter
LET C = NEW Counter()
C.INCREMENT()
C.INCREMENT()
PRINT C.VALUE()        ' 2
DELETE C
```

**Key points:**

- `ME` refers to the current instance (like `this` in C++ or `self` in Python)
- Inside a class method, unqualified method calls implicitly target `ME`. For example, `Increment()` is equivalent to
  `ME.Increment()`.
- `NEW` is a special constructor method
- `DELETE` frees the object (calls `DESTRUCTOR` if defined)

Common mistakes and tips:

- Missing parentheses when passing arguments will be rejected with a clear diagnostic.
- For readability and consistency, prefer parentheses even for zero-argument calls.
- Avoid naming a field and a method with the same (case-insensitive) name when the field is an array. Array-field access
  uses the same `obj.field(...)` syntax as method calls; the compiler resolves `obj.field(i)` as array access only when
  the field is declared as an array. Non-array fields with the same name as methods are treated as methods in calls (
  e.g., `obj.Value()`).

### Destructors

```basic
CLASS FileHandler
  FH AS INTEGER

  SUB NEW(FILENAME$)
    OPEN FILENAME$ FOR OUTPUT AS #1
    LET ME.FH = 1
  END SUB

  DESTRUCTOR
    IF ME.FH <> 0 THEN CLOSE #ME.FH
  END DESTRUCTOR
END CLASS
```

**Note:** Destructors are automatically called when an object is deleted or goes out of scope.

---

## 6. Organizing Code with Namespaces

Namespaces help you organize classes and avoid name collisions in larger programs.

### Declaring Namespaces

Use `NAMESPACE` blocks to group related types. Dotted paths create nested namespaces:

```basic
NAMESPACE Graphics.Rendering
  CLASS Renderer
    WIDTH AS I64
    HEIGHT AS I64
  END CLASS
END NAMESPACE

NAMESPACE Graphics.UI
  CLASS Button
    LABEL AS STR
  END CLASS
END NAMESPACE
```

**Fully-qualified names:**

```basic
DIM R AS Graphics.Rendering.Renderer
DIM B AS Graphics.UI.Button
```

> **Namespaced classes are currently limited.** A class declared inside a
> `NAMESPACE` gets no implicit default constructor (`NEW Ns.C()` fails with
> `unknown callee @NS.C.__ctor` unless the class declares an explicit
> `SUB NEW()`), and its fields are not visible from inside or outside the class
> (`E_PROP_NO_SUCH_PROPERTY`). Method-only namespaced classes with an explicit
> constructor do work. See [defect audit](../audit_09012026.md) #17 and #21.

### Using the USING Directive

Import types from a namespace for unqualified references:

```basic
USING Graphics.Rendering

DIM R AS Renderer          REM Unqualified via USING
```

**Placement rules:**

- `USING` must appear at file scope
- `USING` must come before `NAMESPACE`, `CLASS`, or `INTERFACE` declarations
- Each file's `USING` directives are file-scoped

### Creating Namespace Aliases

Create shorthand names for long namespace paths:

```basic
USING GR = Graphics.Rendering
USING UI = Graphics.UI

DIM RENDERER AS GR.Renderer
DIM BUTTON AS UI.Button
```

### Type Resolution Precedence

When you reference a type without qualification, the compiler searches in order:

1. Current namespace
2. Parent namespaces (walking up the hierarchy)
3. Imported namespaces via `USING` (in declaration order)
4. Global namespace

### Handling Ambiguity

If multiple imported namespaces contain the same type name:

```basic
USING Collections
USING Utilities

REM If both define "List":
DIM MYLIST AS List          REM Error: ambiguous reference

REM Fix 1: Use fully-qualified name
DIM MYLIST AS Collections.List

REM Fix 2: Use alias
USING Coll = Collections
DIM MYLIST AS Coll.List
```

### Case Insensitivity

All namespace and type names are case-insensitive:

```basic
NAMESPACE MyLib
  CLASS Helper
  END CLASS
END NAMESPACE

REM All equivalent:
DIM H1 AS MyLib.Helper
DIM H2 AS MYLIB.HELPER
DIM H3 AS mylib.helper
```

---

## 7. Console and File I/O

### Console Input/Output

```basic
INPUT "What is your name? ", NAME$
PRINT "Hello, "; NAME$

' LINE INPUT reads an entire line
LINE INPUT "Enter a line: ", LINE$
PRINT "You entered: "; LINE$
```

### File Operations

```basic
OPEN "output.txt" FOR OUTPUT AS #1
PRINT #1, "Hello, file!"
PRINT #1, "Line 2"
CLOSE #1

DIM L$
OPEN "output.txt" FOR INPUT AS #2
WHILE NOT EOF(#2)
  LINE INPUT #2, L$
  PRINT L$
WEND
CLOSE #2
```

**File modes:**

- `INPUT` — Read from file
- `OUTPUT` — Write to file (overwrites)
- `APPEND` — Write to file (appends)
- `BINARY` — Binary read/write

---

## 8. Error Handling

### ON ERROR GOTO

```basic
ON ERROR GOTO ErrHandler
OPEN "missing.txt" FOR INPUT AS #1
PRINT "File opened successfully"
CLOSE #1
END

ErrHandler:
PRINT "Error: Could not open file"
END
```

**Resume options:**

- `RESUME` — Retry the statement that caused the error
- `RESUME NEXT` — Continue with the next statement
- `RESUME <label>` — Resume execution at a specific line label

> **`RESUME` is not implemented yet.** Every form lowers to a `trap`, so control
> does not return to the protected code — the handler runs and the program ends.
> Have the handler do the recovery work itself for now. See
> [defect audit #22](../audit_09012026.md).

To stop execution from a handler use `END` (or fall through). Zanna BASIC does **not** treat
`RESUME 0` as "end the program"; it parses as a `RESUME <label>` jump to label `0`, which simply
fails to lower if no such label exists.

---

## 9. Example Projects

### Example A: Number Guessing Game

```basic
RANDOMIZE TIMER()
LET SECRET = INT(RND() * 100) + 1
LET ATTEMPTS = 0

DO
  INPUT "Guess a number (1-100): ", GUESS
  LET ATTEMPTS = ATTEMPTS + 1

  IF GUESS = SECRET THEN
    PRINT "Correct! You won in "; ATTEMPTS; " attempts!"
    EXIT DO
  ELSEIF GUESS < SECRET THEN
    PRINT "Higher!"
  ELSE
    PRINT "Lower!"
  END IF
LOOP
```

### Example B: File Copy Utility

```basic
LINE INPUT "Source file: ", SOURCE$
LINE INPUT "Destination file: ", DEST$

ON ERROR GOTO CopyErr

DIM LINE$
OPEN SOURCE$ FOR INPUT AS #1
OPEN DEST$ FOR OUTPUT AS #2

WHILE NOT EOF(#1)
  LINE INPUT #1, LINE$
  PRINT #2, LINE$
WEND

CLOSE #1
CLOSE #2
PRINT "File copied successfully!"
END

CopyErr:
PRINT "Error: Could not copy file"
END
```

---

## 10. Where to Go Next

**Learn More:**

- **[BASIC Reference](../languages/basic-reference.md)** — Complete language reference
- **[IL Guide](../il/il-guide.md)** — Understanding the intermediate language

**Explore Examples:**

- Browse `examples/zbasic/` for more sample programs
- Check `src/tests/golden/basic/` for test cases

**Advanced Topics:**

- Object-oriented patterns: builders, resource guards with `DESTRUCTOR`
- Namespace organization strategies for large projects
- Error handling best practices
- [Cross-Language Interop](../languages/interop.md) — Mixed BASIC + Zia projects
