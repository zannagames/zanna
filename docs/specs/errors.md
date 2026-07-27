---
status: active
last-verified: 2026-07-26
audience: public
---

# Global Error and Trap Semantics

This document specifies the cross-layer error and trap model used by ZANNA. It covers
checked IL instructions, runtime bridges, handler control flow, and BASIC surface
syntax lowering rules. The material here is normative for the VM, the BASIC front end,
and any future languages that target the IL trap mechanism.

## Trap Kinds

The VM recognizes the following trap kinds. They are defined and shared between
checked IL operations, the runtime C ABI, and BASIC surface semantics. Numeric
values match the `TrapKind` enum in `src/vm/Trap.hpp`.

| Kind               | Value | Description                                                      |
|--------------------|-------|------------------------------------------------------------------|
| `Bounds`           | 4     | Bounds check failure (arrays, strings).                          |
| `DivideByZero`     | 0     | Integer divide/remainder with a zero divisor.                    |
| `DomainError`      | 3     | Invalid mathematical domain (e.g., sqrt of negative).            |
| `EOF`              | 6     | End-of-file reached while input still expected.                  |
| `FileNotFound`     | 5     | File system open on a path that does not exist.                  |
| `InvalidCast`      | 2     | Invalid numeric or pointer cast.                                 |
| `InvalidOperation` | 8     | Operation outside the allowed state machine.                     |
| `IOError`          | 7     | Other I/O failure (permissions, device errors).                  |
| `Overflow`         | 1     | Checked arithmetic overflow (e.g., `INT64_MIN / -1`, `i64` abs). |
| `RuntimeError`     | 9     | Catch-all for unexpected runtime failures.                       |
| `Interrupt`        | 10    | Program interrupted by Ctrl-C or `requestInterrupt()`.           |
| `NetworkError`     | 11    | Network I/O failure (connection, DNS, TLS, etc.).                |

## IL Error-Handling Primitives

The IL exposes structured primitives for raising and handling traps:

- `trap` raises a trap (plain raise).
- `trap.from_err <type>, <code>` raises a trap with the given type and code.
- `trap.kind` reads the current trap kind, or the kind stored in an optional `Error` operand (produces a value, does not
  raise).
- `trap.err` constructs an Error record from the current trap state (produces a value, does not raise).
- `eh.push ^handler` activates the handler block referenced by label.
- `eh.pop` removes the most recently pushed handler.
- `resume.same %tok`, `resume.next %tok`, and `resume.label %tok, ^L` resume
  execution after a trap using the supplied resume token.

Handlers receive their parameters in SSA form and must not forge resume tokens.
`ResumeTok` is a handler-provenance capability, not an ordinary pointer-like
value. A token is produced only by EH dispatch into the selected handler. It may
be forwarded unchanged through block parameters for typed-catch or rethrow helper
blocks, but every path to a `resume.*` must carry an active token that originated
from a dispatched handler. The token may be consumed only by `resume.*`; calls,
stores, returns, arithmetic, and other ordinary value uses are invalid. Because
`resume.label` consumes the token before entering its target, its destination
must not be a handler block.

## Trap Flow and Handler Control

Handlers are installed with `eh.push ^handler` and removed with `eh.pop`. When a
trap is raised, the VM unwinds stack frames until it finds the most recent
handler. Control then jumps into the handler block with two parameters:
`(%err: Error, %tok: ResumeTok)`.

### Trap Dispatch Sequence

```text
[Trap raised]
      |
      v
[Unwind frames] -- no handler --> [Terminate with diagnostic]
      |
      v
[Nearest eh.push]
      |
      v
[Enter handler block with (%err, %tok)]
```

The `Error` record contains `{ kind:i32, code:i32, ip:u64, line:i32 }` where
`line` is `-1` when no source line is available. The resume token is opaque and
must be passed unchanged to `resume.*` instructions. Verifiers must reject
handler entry by ordinary control-flow edges unless the edge forwards the
currently active resume token into the destination block's `ResumeTok`
parameter.

### Resume Modes

Handlers may resume execution in three different ways. Each is shown as a
control-flow schematic; `%tok` is the resume token received by the handler.

#### `resume.same %tok`

```text
[Handler]
    |
    | resume.same %tok
    v
[Faulting instruction re-executes]
```

Use this when the handler has corrected the underlying issue (for example, by
setting a divisor to a non-zero default) and wants to retry the failing
instruction.

#### `resume.next %tok`

```text
[Handler]
    |
    | resume.next %tok
    v
[Instruction immediately after the fault]
```

Choose this mode when the handler wants to skip the faulting instruction but
continue within the same block (mirrors BASIC `RESUME NEXT`).

#### `resume.label %tok, ^L`

```text
[Handler]
    |
    | resume.label %tok, ^L
    v
[Branch to label/block L]
```

This mode performs a non-local branch to another block in the current function.
It matches BASIC `RESUME <label>`.

A handler may also decide not to resume; in that case it can `trap` again or
simply fall off the end (which behaves like re-raising the same trap).

## Mapping Tables

### Checked IL Instruction → Trap Kind

| Instruction              | Condition                                              | Trap Kind                    |
|--------------------------|--------------------------------------------------------|------------------------------|
| `cast.fp_to_si.rte.chk`  | NaN or value outside signed integer range              | `Overflow`                   |
| `cast.fp_to_ui.rte.chk`  | NaN or value outside unsigned integer range            | `Overflow`                   |
| `cast.si_narrow.chk`     | Value outside target signed range                      | `Overflow`                   |
| `cast.ui_narrow.chk`     | Value outside target unsigned range                    | `Overflow`                   |
| `iadd.ovf`               | Signed addition overflows                              | `Overflow`                   |
| `idx.chk`                | Index outside `[lo, hi)` range                         | `Bounds`                     |
| `imul.ovf`               | Signed multiplication overflows                        | `Overflow`                   |
| `isub.ovf`               | Signed subtraction overflows                           | `Overflow`                   |
| `sdiv.chk0`              | Divisor is `0`, or `INT64_MIN / -1`                    | `DivideByZero` or `Overflow` |
| `srem.chk0`              | Divisor is `0`                                         | `DivideByZero`               |
| `udiv.chk0`              | Divisor is `0`                                         | `DivideByZero`               |
| `urem.chk0`              | Divisor is `0`                                         | `DivideByZero`               |

### Runtime `Err` Code → Trap Kind

The `Err` enum is defined in `src/runtime/core/rt_error.h`. Numeric values are listed for reference.

| Runtime `Err`                         | Value | Trap Kind          |
|---------------------------------------|-------|--------------------|
| `Err_Bounds`                          | 7     | `Bounds`           |
| `Err_DomainError`                     | 6     | `DomainError`      |
| `Err_EOF`                             | 2     | `EOF`              |
| `Err_FileNotFound`                    | 1     | `FileNotFound`     |
| `Err_InvalidCast`                     | 5     | `InvalidCast`      |
| `Err_InvalidOperation`                | 8     | `InvalidOperation` |
| `Err_IOError`                         | 3     | `IOError`          |
| `Err_Overflow`                        | 4     | `Overflow`         |
| `Err_RuntimeError` (any other non-zero) | 9   | `RuntimeError`     |

The C runtime reports success or failure and fills an `Err` out-parameter. VM
glue converts that code into the listed trap kind and raises `trap.err` with the
complete `Error` payload.

## BASIC ↔ IL Handler Mapping

The BASIC `ON ERROR` directive installs or clears a handler for the current
procedure. Each variant lowers to explicit `eh.push`/`eh.pop` and `resume.*`
operations.

### Installing a Handler

BASIC:

```basic
ON ERROR GOTO handler
...
handler:
```

Lowered IL (excerpt):

```il
entry:
  eh.push ^handler
  br ^body

body:
  ... instructions ...
  eh.pop
  ret

handler(%err: Error, %tok: ResumeTok):
  ...
```

### Clearing the Handler

BASIC `ON ERROR GOTO 0` lowers to a simple `eh.pop` in the current function to
remove the active handler.

### Resume Variants

| BASIC Statement | IL Equivalent               |
|-----------------|-----------------------------|
| `RESUME`        | `resume.same %tok`          |
| `RESUME NEXT`   | `resume.next %tok`          |
| `RESUME label`  | `resume.label %tok, ^label` |

The `%tok` value is always the resume token received by the handler block. Hand
crafted IL must not forge resume tokens.

## Worked Examples

### Divide-by-zero

IL:

```il
  %q = sdiv.chk0 %numerator, %denominator
```

- If `%denominator` is `0`, the VM raises `DivideByZero`.
- A handler can set `%denominator` to `1`, then `resume.same %tok` to re-run the
  division safely.

### Out-of-bounds index

IL:

```il
  %item = idx.chk %array, %index
```

- If `%index` is outside `[0, len)`, a `Bounds` trap is raised.
- A BASIC program with `ON ERROR` can `RESUME label` that clamps the index.

### Opening a missing file

Runtime call from BASIC:

```il
  %ok = call @rt_file_open(%handle, %path, %err_out)
  brnz %ok, ^continue, ^trap

trap:
  %err = load Error, %err_out
  trap.err %err
```

If the runtime stores `Err::FileNotFound` in `%err_out`, the VM glue maps it to
the `FileNotFound` trap kind and enters the handler. A BASIC handler may print a
message and `RESUME NEXT` to continue without the file.

## Unhandled Trap Diagnostics

If no `eh.push` is active when a trap occurs, the VM terminates with a deterministic
multi-field diagnostic:

```text
Trap: <Kind>
Function: <function name>
IL: <function>#<block>#<instruction index>
Source line: <line number or -1>
```

For example, an unhandled divide-by-zero might report:

```text
Trap: DivideByZero
Function: @main
IL: @main#L2#7
Source line: 42
```

The `Source line` field is `-1` when the IL instruction is not annotated with a
line number.
