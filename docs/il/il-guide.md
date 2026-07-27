---
status: active
audience: public
last-verified: 2026-07-26
---

# Zanna IL — Complete Guide

Comprehensive guide to Zanna's Intermediate Language (IL), covering everything from quickstart to advanced topics. This
document consolidates the quickstart, normative reference, BASIC lowering rules, optimization passes, and worked
examples for IL v0.3.

> **Note:** This guide documents IL v0.3.0. See the version header in your IL files for compatibility.

---

## Table of Contents

1. [Quickstart](#quickstart)
2. [Normative IL Reference](#reference)
3. [BASIC-to-IL Lowering](#lowering)
4. [Optimization Passes](#passes)
5. [Worked Examples](#examples)

---

<a id="quickstart"></a>

## Quickstart

Welcome! This guide is for developers from languages like C#, Java, TypeScript, or Python who want a hands-on tour of
Zanna's intermediate language. **No prior compiler experience is required.**

### What is Zanna IL?

Zanna IL is the **"thin waist"** of the Zanna toolchain — a versioned, textual intermediate representation that
decouples frontends from backends:

- **Frontends** (Zia, BASIC, etc.) compile to IL
- **VM** executes IL deterministically
- **Verifier** ensures type safety and correctness
- **Transforms** optimize IL (SimplifyCFG, LICM, SCCP)
- **Backends** compile IL to native code

**Why a separate IL?**

- **Decoupling**: Frontends evolve independently from the VM
- **Stability**: IL version headers (`il 0.3.0`) ensure compatibility
- **Inspectability**: Textual format is easy to read and debug
- **Optimization**: Centralized place for analysis and transforms
- **Multi-language**: Multiple frontends share one runtime

### Program structure

An IL module is plain text. Its top‑level layout is:

1. **Version line** – `il 0.3.0` pins the expected IL grammar version.
2. **Extern declarations** – `extern @name(signature) -> ret [attrs?]` describes functions provided by the runtime or
   other modules.
3. **Globals** – `global const str @.msg = "hi"` defines immutable strings; scalar storage globals use forms such as
   `global i64 @counter = 0`.
4. **Functions** – `func @main() -> i64 { ... }` contains basic blocks and instructions.

Inside a function:

* Each basic block starts with a label like `entry:`. There is no fall‑through; control transfers with a terminator such
  as `ret`, `br`, or `cbr`.
* Instructions assign results to SSA registers (`%v0`, `%tmp1`). A register is defined once and used many times.
* Comments begin with `#` or `//` outside quoted strings and run to the end of the line.

These pieces mirror what a compiler would normally keep in its internal IR and make it explicit for learning and
debugging.

### Your first IL program

Create a file `first.il` with the contents:

```llvm
# Print the number 4 and exit.
il 0.3.0
extern @Zanna.Terminal.PrintI64(i64) -> void
func @main() -> i64 {
entry:
  call @Zanna.Terminal.PrintI64(4)    # runtime prints `4` with no newline
  ret 0                    # zero exit code
}
```

Run it with `zanna`:

```bash
zanna -run first.il
```

Expected output — the single byte `4`, with no trailing newline:

```text
4
```

Use `@Zanna.Terminal.SayInt` instead of `PrintI64` when you want a trailing newline.

**Line by line**

- `# Print the number 4 and exit.` – comments start with `#` or `//` and are ignored by the VM.
- `il 0.3.0` – required version header that pins the expected IL grammar version.
- `extern @Zanna.Terminal.PrintI64(i64) -> void` – declare a runtime function taking an `i64` and returning `void`.
- `func @main() -> i64 {` – define the `@main` function that returns an `i64` exit code.
- `entry:` – the initial basic block label.
- `call @Zanna.Terminal.PrintI64(4)` – invoke the extern with the literal `4`.
- `ret 0` – terminate the function and supply the process exit status.
- `}` – close the function body.

Compatibility:

- When built with `-DZANNA_RUNTIME_NS_DUAL=ON`, legacy `@rt_*` externs are accepted as aliases of `@Zanna.*`.
- New code should emit `@Zanna.*`.
- The current build default is `ZANNA_RUNTIME_NS_DUAL=ON`, so legacy `@rt_*` aliases are
  published alongside canonical names. Configure with `-DZANNA_RUNTIME_NS_DUAL=OFF` when
  legacy IL compatibility is not required.

**What just happened?** `Zanna.Terminal.PrintI64` is supplied by the runtime and prints its argument. Every function ends
with a terminator such as `ret` giving the program's exit code.

**Gotcha:** Every module must start with a version line (e.g., `il 0.3.0`).

### Values and types

IL is statically typed and uses SSA-style virtual registers (`%v0`, `%t1`, ...). Primitive types include `i1` (bool),
`i16`, `i32`, `i64`, `f64`, `ptr`, and `str`, plus specialized types `error` and `resume_tok` for exception handling.

```llvm
il 0.3.0
extern @Zanna.Terminal.PrintI64(i64) -> void
func @main() -> i64 {
entry:
  %p = alloca 8            # reserve 8 bytes on the stack
  store i64, %p, 10        # write constant 10 to memory
  %v0 = load i64, %p       # read it back
  call @Zanna.Terminal.PrintI64(%v0)  # prints 10
  ret 0
}
```

**Line by line**

- `%p = alloca 8` – allocate eight bytes of stack memory and bind its address to `%p` (type `ptr`).
- `store i64, %p, 10` – store the 64‑bit constant `10` into the memory pointed to by `%p`.
- `%v0 = load i64, %p` – load an `i64` from `%p` into `%v0`.
- `call @Zanna.Terminal.PrintI64(%v0)` – pass the loaded value to the runtime print routine.
- `ret 0` – return from `main` with exit code 0.

**What just happened?** `alloca` creates a stack slot, `store` writes to it, and `load` reads from it.

**Gotcha:** All integers are 64-bit; mixing `i64` and `f64` requires explicit casts.

Integer literals may be decimal, `0x` hexadecimal, `0b` binary, or leading-zero octal, with an optional sign and
underscore digit separators. The same parser is used for instruction operands and scalar global initializers, so values
such as `-0x2a` and `0b1010_0011` round-trip consistently.

### Locals, params, and calls

Functions declare typed parameters. Values are passed and returned explicitly.

```llvm
il 0.3.0
extern @Zanna.Terminal.PrintI64(i64) -> void
func @add(i64 %a, i64 %b) -> i64 {
entry:
  %sum = iadd.ovf %a, %b   # compute a + b (traps on overflow)
  ret %sum
}
func @main() -> i64 {
entry:
  %v0 = call @add(2, 3)    # call with constants
  call @Zanna.Terminal.PrintI64(%v0)  # prints 5
  ret 0
}
```

**Line by line**

- `func @add(i64 %a, i64 %b) -> i64` – declare a function with two `i64` parameters and an `i64` return type.
- `%sum = iadd.ovf %a, %b` – add the two parameters with overflow checking; traps on signed overflow.
- `ret %sum` – return the computed sum.
- `func @main() -> i64 { ... }` – define the entry point.
- `%v0 = call @add(2, 3)` – call `@add` with literal arguments; result stored in `%v0`.
- `call @Zanna.Terminal.PrintI64(%v0)` – print the returned value.
- `ret 0` – exit with status 0.

**What just happened?** `call` pushes arguments and receives a result. Each function has one entry block.

**Gotcha:** Arguments are immutable; use `alloca` + `store` if you need a mutable local. Signed integer arithmetic
must use the `.ovf` forms (`iadd.ovf`, `isub.ovf`, `imul.ovf`) — plain `add`/`sub`/`mul` are rejected by the verifier.

### Arithmetic and comparisons

```llvm
il 0.3.0
extern @Zanna.Terminal.PrintI64(i64) -> void
func @main() -> i64 {
entry:
  %v0 = iadd.ovf 2, 2      # 4
  %v1 = scmp_gt %v0, 3     # 1 (true)
  %v2 = zext1 %v1          # widen i1 → i64 before printing
  call @Zanna.Terminal.PrintI64(%v2)  # prints 1
  ret 0
}
```

**Line by line**

- `%v0 = iadd.ovf 2, 2` – compute the constant expression `2 + 2` with overflow checking.
- `%v1 = scmp_gt %v0, 3` – signed compare‑greater; result is `1` because 4 > 3.
- `%v2 = zext1 %v1` – zero-extend the `i1` result to `i64` so it can be passed to `PrintI64`.
- `call @Zanna.Terminal.PrintI64(%v2)` – print the widened value.
- `ret 0` – terminate `main` with success.

**What just happened?** `scmp_gt` compares signed integers and yields an `i1` (0 or 1).

**Gotcha:** Comparison results are `i1`; widen to `i64` with `zext1` before passing them to `i64`-typed callees.

### Control flow

Blocks end with a terminator. `cbr` chooses a target based on an `i1` value.

```llvm
il 0.3.0
extern @Zanna.Terminal.PrintI64(i64) -> void
func @main() -> i64 {
entry:
  %flag = scmp_gt 5, 3     # 1 means take then
  cbr %flag, then, else    # conditional branch
then:
  call @Zanna.Terminal.PrintI64(1)    # prints 1 if flag != 0
  br done                  # jump to exit
else:
  call @Zanna.Terminal.PrintI64(0)    # prints 0 otherwise
  br done
done:
  ret 0
}
```

```text
 entry ──cbr──▶ then ──▶ done
   │             │
   └──────▶ else ┘
```

**Line by line**

- `%flag = scmp_gt 5, 3` – compare constants; `%flag` holds `1`.
- `cbr %flag, then, else` – branch to `then` when `%flag` is non‑zero, otherwise `else`.
- `then:` / `else:` – block labels.
- `br done` – unconditionally jump to the block `done`.
- `ret 0` – final terminator of the `done` block.

**What just happened?** Labels (`then`, `else`, `done`) mark basic blocks. `br` is an unconditional jump.

**Gotcha:** There is no fall-through; every block must end with a terminator, and textual IL is rejected if more
instructions appear after that terminator in the same block.

### Strings and text

Strings live in globals and use `Zanna.Terminal.PrintStr` for output.

```llvm
il 0.3.0
extern @Zanna.Terminal.PrintStr(str) -> void
global const str @.msg = "hello"  # immutable global
func @main() -> i64 {
entry:
  %s = const_str @.msg     # load pointer to string
  call @Zanna.Terminal.PrintStr(%s)   # prints hello
  ret 0
}
```

**Line by line**

- `extern @Zanna.Terminal.PrintStr(str) -> void` – declare the runtime string printer.
- `global const str @.msg = "hello"` – create an immutable global string named `@.msg`.
- `%s = const_str @.msg` – get a pointer to the string constant.
- `call @Zanna.Terminal.PrintStr(%s)` – pass that pointer to the runtime for printing.
- `ret 0` – exit normally.

**What just happened?** `const_str` loads the address of a global string constant.

**Gotcha:** Strings are reference-counted; do not `alloca` them manually.

### Errors and exit codes

Returning a non-zero `i64` sets the process exit code. `trap` reports an error and aborts.

```llvm
il 0.3.0
func @main() -> i64 {
entry:
  trap                    # aborts execution
}
```

Running the above produces a non-zero exit.

**Line by line**

- `trap` – raise a runtime trap, aborting execution immediately.

`trap` aborts execution immediately with a non‑zero status; no `ret` is needed.
To trap with a specific error code and message, use `trap.err` to create an error value
and `trap.from_err` to terminate with it.

**Gotcha:** After a `trap` the VM stops; no `ret` is required.

### From high-level code to IL

A tiny BASIC program:

```basic
PRINT 2 + 2
END
```

Lowered IL:

```llvm
il 0.3.0
extern @Zanna.Terminal.PrintI64(i64) -> void
func @main() -> i64 {
entry:
  %t0 = iadd.ovf 2, 2
  call @Zanna.Terminal.PrintI64(%t0)
  ret 0
}
```

**Line by line**

- `%t0 = iadd.ovf 2, 2` – compute the arithmetic expression from the BASIC code with overflow checking.
- `call @Zanna.Terminal.PrintI64(%t0)` – print the result.
- `ret 0` – exit with success.

**What just happened?** The front end evaluated the expression, emitted an `iadd.ovf`, and called the print routine.

### Debugging IL

- `zanna -run --trace=il foo.il` prints each instruction as it executes (`--trace=src` prints source lines instead).
- `il-verify foo.il` checks structural rules without running.
- Common errors like "type mismatch" or "undefined block" point to the offending line.

### Tips & best practices

- Keep functions small and testable.
- Use meaningful block labels and value names with `@name` and `%v0` hints.
- Prefer deterministic behaviour; avoid relying on undefined order.

### Next steps

- Read the full [IL reference](#reference) for all instructions.
- Explore the `examples/` and `src/tests/golden/` directories for more programs.
- Try adding your own IL file and running it with `zanna`.

### Common mistakes

- Forgetting the version line (`il 0.3.0`).
- Missing terminators at the end of blocks.
- Mismatched types in instructions or extern calls.

Happy hacking!

<a id="reference"></a>

## Reference

### Normative scope

The current IL v0.3.0 specification builds on the design principles established in earlier versions: IL acts as the "thin waist"
between front ends and execution engines, enforces explicit control flow with one terminator per block, and keeps the
type system intentionally small (`void`, `i1`, `i16`, `i32`, `i64`, `f64`, `ptr`, `str`, plus `error` and `resume_tok`
for structured exception handling). The material below supersedes earlier
drafts (including v0.1.x) while remaining source-compatible with modules written for those versions. Numeric promotion
semantics are specified in [specs/numerics.md](../specs/numerics.md) and the unified trap/handler model is
defined in [specs/errors.md](../specs/errors.md); both documents are normative for all front ends and the
VM.

### IL Reference (v0.3.0)

> Start here: [IL Quickstart](#quickstart) for a hands-on introduction.

#### Overview

Zanna IL is the project’s "thin waist" intermediate language designed to sit between diverse front ends and back ends.
Its goals are:

* **Determinism** – VM and native back ends must produce identical observable behaviour.
* **Explicit control flow** – each basic block ends with exactly one terminator; no fallthrough.
* **Static types** – a minimal set of primitive types (`void`, `i1`, `i16`, `i32`, `i64`, `f64`, `ptr`, `str`) plus
  `error` and `resume_tok` for structured exception handling.

Execution is organized as functions consisting of labelled basic blocks. Modules may execute either under the IL virtual
machine (VM) or after lowering to native code through a C runtime. Front ends such as BASIC first lower into IL
patterns described in [BASIC lowering](#lowering).

#### Module & Function Syntax

##### Text parser resource limits

The in-memory IL model has no specification-level size ceiling, but textual IL
parsers must apply configurable resource budgets as defined by
[ADR 0111](../adr/0111-il-text-resource-limits.md). The default parser accepts
lines up to 1 MiB, up to 1,000,000 physical lines, 100,000 functions, 1,000,000
blocks, 10,000,000 instructions, and 65,535 operands or branch arguments on one
instruction. Exceeding a budget is a compile error, not a trap. Trusted tools
may explicitly raise these budgets.

Identifier fragments must not contain ASCII control bytes (`U+0000` through
`U+001F`, or `U+007F`). Quoted tokens require an unescaped closing quote.

An IL module is a set of declarations and function definitions. It starts with a version line:

```text
il 0.3.0
```

An optional `target "..."` metadata line may follow. The VM ignores it, but native back ends can use it as advisory
information.

```text
il 0.3.0
target "x86_64-sysv"
```

See [examples/il](../../examples/il) for complete programs.

Each function has the form:

```il
func @name(param_list?) -> ret_type [attrs?] {
entry:
  ...
}
```

The parameter list may end with `...` to mark a C-style variadic function:

Function definitions may carry `[nothrow]`, `[readonly]`, `[pure]`,
`[module_init]`, or a comma-separated combination before the opening brace.
The verifier checks effect promises against the body. `[module_init]` explicitly
marks a non-entry `() -> void` function for invocation by the module linker;
function names have no initializer semantics.

```il
func export @printfLike(str %fmt, ...) -> i64
```

##### Minimal Function

```text
il 0.3.0
func @main() -> i64 {
entry:
  ret 0
}
```

##### Function Linkage

Functions may have a linkage keyword between `func` and the name:

- **`Internal`** (default, no keyword) — visible only within the defining module.
- **`export`** — defined here, callable from other modules.
- **`import`** — declared here, defined in another module (no body required).

```text
func export @calculateScore(i64 %x, i64 %y) -> i64 {
entry:
  %r = imul.ovf %x, %y
  ret %r
}

func import @BasicHelper(i64 %n) -> i64
```

Import functions have no body (no `{...}` block). The IL linker resolves them
against matching export functions when linking multiple modules. Omitting the
linkage keyword defaults to `Internal`, so all existing `.il` files are
backwards compatible.

See [Cross-Language Interop Guide](../languages/interop.md) for full details on linking
multiple modules.

##### Extern Declarations

External functions are declared with `extern` and may be called like normal functions.
Externs reference runtime-provided functions (C ABI), while `import` references
functions defined in other IL modules.

```text
il 0.3.0
extern @Zanna.Terminal.PrintI64(i64) -> void
func @main() -> i64 {
entry:
  call @Zanna.Terminal.PrintI64(42)
  ret 0
}
```

##### Globals

Module-level globals bind a symbol to immutable string data or scalar storage.
String globals require a quoted initializer and are immutable. Scalar globals may
be `i1`, `i16`, `i32`, `i64`, `f64`, or `ptr`; omitted initializers default to
zero or null. `const` on scalar globals records source intent but does not change
the pointer representation exposed by `gaddr`.

```text
il 0.3.0
global const str @.msg = "hi"
global i64 @counter = 41
func @main() -> i64 {
entry:
  %s = const_str @.msg
  %p = gaddr @counter
  %v = load i64, %p
  ret %v
}
```

#### Types

| Type        | Meaning              | Alignment | Notes                                |
|-------------|----------------------|-----------|--------------------------------------|
| `void`      | no value             | —         | function return only                 |
| `i1`        | boolean              | 1         | produced by comparisons and `trunc1` |
| `i16`       | 16-bit signed int    | 2         | wrap on overflow                     |
| `i32`       | 32-bit signed int    | 4         | wrap on overflow                     |
| `i64`       | 64-bit signed int    | 8         | wrap on overflow                     |
| `f64`       | 64-bit IEEE float    | 8         | NaN/Inf propagate                    |
| `ptr`       | untyped pointer      | 8         | byte-addressed                       |
| `str`       | opaque string handle | 8         | managed by runtime                   |
| `error`      | error value          | 8         | exception handling only              |
| `resume_tok` | resume token         | 8         | exception handling only              |

The two exception-handling types also accept the PascalCase spellings `Error` and
`ResumeTok`, which is the form conventionally used in handler block-parameter
annotations (`handler ^h(%err:Error, %tok:ResumeTok):`). The printer always emits
the lower-case canonical names.

`ptr` and `str` have the same storage width but are distinct verifier types; raw extern calls and indirect-call
signatures must use the exact type declared by the callee. Known runtime helpers whose catalog signature spells a
parameter as `obj` are the only compatibility bridge: those object slots lower to `ptr` at the ABI boundary, but may
accept a managed `str` handle.

#### Constants & Literals

Integers use decimal notation (`-?[0-9]+`). Floats use decimal with optional fraction (`-?[0-9]+(\.[0-9]+)?`) and permit
`NaN`, `Inf`, and `-Inf`. Booleans `true`/`false` sugar to `i1` values `1`/`0`. Strings appear in quotes with escapes
`\"`, `\\`, `\n`, `\t`, `\xNN`. `const_null` yields a null value for pointer-like result annotations (`ptr`, `str`,
`error`, or `resume_tok`).

#### Basic Blocks

Functions contain one or more labelled blocks. Labels end in `:` and the first block is `entry`. A block may declare
parameters; each predecessor must supply matching arguments. Omitting the argument list is shorthand for passing no
values (for example, `br next` is the same as `br next()`).

```text
il 0.3.0
func @loop(i64 %n) -> i64 {
entry:
  br body(%n, 0)
body(%i:i64, %acc:i64):
  %cmp = scmp_ge %i, %n
  cbr %cmp, done(%acc), body(%i, %acc)
done(%r:i64):
  ret %r
}
```

#### Control Flow

##### `br`

Unconditional branch to a block with optional arguments.

```text
br next(%v)
```

##### `cbr`

Conditional branch on an `i1` value.

```text
cbr %cond, then, else
```

##### `switch.i32`

Multi-way branch on an `i32` scrutinee with an explicit default.

```text
switch.i32 %scrutinee, ^default, 1 -> ^case_one, 2 -> ^case_two
```

The first operand is the `i32` value to test. The first label after the operand
is the mandatory default target (e.g. `^default(args?)`; the `^` caret prefix is
optional). Subsequent entries pair a distinct 32-bit integer constant with a
branch target using `value -> label(args?)`. When no case matches, the default
label is taken. Each target may optionally supply block arguments.

##### `ret`

Return from the current function.

##### `trap`

Abort execution with an unconditional runtime trap.

```text
func @oops() -> void {
entry:
  trap
}
```

#### Instructions

Each non-terminator instruction optionally assigns to a `%temp` and produces a result. Below, `x` and `y` denote
operands.

> _Opcode table note:_ An `InstrType` sentinel means the result or operand type
> is taken from the instruction's declared type.

##### Integer Arithmetic

Front ends must emit the checked opcodes below: they trap on overflow or divide-by-zero. The
non-checking variants (`add`, `sub`, `mul`, `sdiv`, `udiv`, `srem`, `urem`) exist in the opcode
table but the verifier rejects them **unless it can prove the operation cannot trap** — no signed
overflow, no divide by zero, and no `INT64_MIN / -1`. The optimizer demotes checked forms to plain
ones where such a proof exists, and the verifier independently re-derives the proof so optimized IL
still verifies. See [ADR 0026](../adr/0026-range-analysis-demotion-proofs.md).

Rejection reads, for example:

```text
error[V-IL-VERIFY]: f:entry: %2 = add %t0 %t1: signed integer add must use iadd.ovf (traps on overflow)
```

| Instr       | Form             | Result | Notes                                                         |
|-------------|------------------|--------|---------------------------------------------------------------|
| `iadd.ovf`  | `iadd.ovf x, y`  | `i64`  | signed add, trap on signed overflow                           |
| `isub.ovf`  | `isub.ovf x, y`  | `i64`  | signed subtract, trap on signed overflow                      |
| `imul.ovf`  | `imul.ovf x, y`  | `i64`  | signed multiply, trap on signed overflow                      |
| `sdiv.chk0` | `sdiv.chk0 x, y` | `i64`  | signed divide, trap on divide-by-zero or INT64_MIN/-1         |
| `udiv.chk0` | `udiv.chk0 x, y` | `i64`  | unsigned divide, trap on divide-by-zero                       |
| `srem.chk0` | `srem.chk0 x, y` | `i64`  | signed remainder, trap on divide-by-zero (matches BASIC `MOD`) |
| `urem.chk0` | `urem.chk0 x, y` | `i64`  | unsigned remainder, trap on divide-by-zero                    |

`sdiv.chk0` and `srem.chk0` follow C semantics: the quotient is truncated toward zero and the remainder keeps the
dividend's sign. Front ends such as BASIC map `\` (integer divide) to `sdiv.chk0` and `MOD` to `srem.chk0`.

```text
il 0.3.0
func @main() -> i64 {
entry:
  %t0 = iadd.ovf 2, 3
  ret %t0
}
```

##### Bitwise and Shifts

| Instr  | Form        | Result |
|--------|-------------|--------|
| `and`  | `and x, y`  | `i64`  |
| `ashr` | `ashr x, y` | `i64`  |
| `lshr` | `lshr x, y` | `i64`  |
| `or`   | `or x, y`   | `i64`  |
| `shl`  | `shl x, y`  | `i64`  |
| `xor`  | `xor x, y`  | `i64`  |

Shift counts are masked modulo 64, matching the behaviour of x86-64 shifts.

```text
%r = xor 0b1010, 0b1100
```

##### Floating-Point Arithmetic

| Instr  | Form        | Result |
|--------|-------------|--------|
| `fadd` | `fadd x, y` | `f64`  |
| `fdiv` | `fdiv x, y` | `f64`  |
| `fmul` | `fmul x, y` | `f64`  |
| `fsub` | `fsub x, y` | `f64`  |

```text
%f = fmul 2.0, 4.0
```

##### Comparisons

| Instrs                                                           | Form           | Result                                                |
|------------------------------------------------------------------|----------------|-------------------------------------------------------|
| `icmp_eq`, `icmp_ne`                                             | `icmp_eq x, y` | `i1`                                                  |
| `scmp_lt`, `scmp_le`, `scmp_gt`, `scmp_ge`                       | `scmp_lt x, y` | `i1` signed compare                                   |
| `ucmp_lt`, `ucmp_le`, `ucmp_gt`, `ucmp_ge`                       | `ucmp_lt x, y` | `i1` unsigned compare                                 |
| `fcmp_lt`, `fcmp_le`, `fcmp_gt`, `fcmp_ge`, `fcmp_eq`, `fcmp_ne` | `fcmp_eq x, y` | `i1` (`NaN` makes `fcmp_eq` false and `fcmp_ne` true) |
| `fcmp_ord`, `fcmp_uno`                                            | `fcmp_ord x, y`| `i1` (ordered: both non-NaN; unordered: either is NaN) |

```text
%cond = scmp_lt %a, %b
```

##### Selection

| Instr    | Form                          | Result                                        |
|----------|-------------------------------|-----------------------------------------------|
| `select` | `select type, cond, tval, fval` | `type` (`tval` when `cond` is true, else `fval`) |

`select` is a pure value computation: `cond` must be `i1`, and both arms and
the result carry exactly the declared `type`. There is no short-circuiting —
both arms are ordinary SSA operands, already evaluated wherever they were
defined, so only values that were safe to compute unconditionally may flow
through a `select`. Backends lower it to conditional moves
(`csel`/`fcsel` on AArch64, `cmov` on x86-64). The `if-conv` optimizer pass
folds small branch diamonds into `select` at `-O2`
(see [ADR 0063](../adr/0063-il-select-and-if-conversion.md)).

```text
%r = select i64, %cond, %a, %b
```

##### Conversions

| Instr                   | Form                       | Result | Notes                                                           |
|-------------------------|----------------------------|--------|-----------------------------------------------------------------|
| `cast.fp_to_si.rte.chk` | `cast.fp_to_si.rte.chk x` | `i64`  | float to signed int (round-to-even, trap on overflow)           |
| `cast.fp_to_ui.rte.chk` | `cast.fp_to_ui.rte.chk x` | `i64`  | float to unsigned int (round-to-even, trap on overflow)         |
| `cast.si_narrow.chk`    | `cast.si_narrow.chk x`    | `i32` or `i16` | narrow signed int (i64→i32 or i64→i16); result type declared on register; trap on overflow |
| `cast.si_to_fp`         | `cast.si_to_fp x`          | `f64`  | signed int to float                                             |
| `cast.ui_narrow.chk`    | `cast.ui_narrow.chk x`    | `i32` or `i16` | narrow unsigned int (i64→i32 or i64→i16); result type declared on register; trap on overflow |
| `cast.ui_to_fp`         | `cast.ui_to_fp x`          | `f64`  | unsigned int to float                                           |
| `fptosi`                | `fptosi x`                 | `i64`  | internal/legacy truncating FP-to-int; verifier prefers checked RTE casts |
| `sitofp`                | `sitofp x`                 | `f64`  | signed int to float                                             |
| `trunc1`                | `trunc1 x`                 | `i1`   | truncate i64 to i1                                              |
| `zext1`                 | `zext1 x`                  | `i64`  | zero-extend i1 to i64                                           |

The `cast.*` family provides type-aware conversions with explicit overflow and rounding behavior. The `.rte` suffix
denotes round-to-even (IEEE 754 default). The `.chk` suffix indicates trap-on-overflow.

```text
%f = sitofp 42
%i = cast.fp_to_si.rte.chk 3.7
```

##### Memory Operations

| Instr        | Form                     | Result                                          |
|--------------|--------------------------|-------------------------------------------------|
| `addr_of`    | `addr_of @global`        | `ptr` (address of immutable string storage)     |
| `alloca`     | `alloca size`            | `ptr` (size < 0 traps; memory zero-initialized) |
| `const.f64`  | `const.f64 3.14`         | `f64` (load floating-point constant)            |
| `const_null` | `const_null`             | `ptr`, `str`, `error`, or `resume_tok`           |
| `const_str`  | `const_str @label`       | `str` (requires a declared string global)       |
| `gaddr`      | `gaddr @global`          | `ptr` (address of scalar module-level storage)  |
| `gep`        | `gep ptr, offs`          | `ptr` (constant alloca-derived offsets checked) |
| `idx.chk`    | `idx.chk idx, lo, hi`    | `i64` (trap if idx < lo or idx >= hi)           |
| `load`       | `load type, ptr`         | `type` (null or misaligned trap)                |
| `store`      | `store type, ptr, value` | — (null or misaligned trap)                     |

`idx.chk` performs bounds checking for array accesses, trapping if the index is outside `[lo, hi)`. It returns the normalized zero-based index `idx - lo`.

`i64`, `f64`, `ptr`, and `str` loads and stores require 8-byte alignment; misaligned or null accesses trap. Stack
allocations created by `alloca` are zero-initialized and live until the function returns. When a load/store address is
statically derived from a constant-size `alloca`, the verifier checks the full access width, not just the pointer
offset.
Constant `gep` may form a one-past-the-end pointer for address arithmetic, but that pointer is not dereferenceable.

```text
func @main() -> i64 {
entry:
  %p = alloca 8
  store i64, %p, 7
  %v = load i64, %p
  ret %v
}
```

##### Calls

| Instr           | Form                            | Result                   |
|-----------------|---------------------------------|--------------------------|
| `call`          | `call @f(%x, %y) [attrs?]`      | return type of `@f`      |
| `call.indirect` | `call.indirect [ret(params?)] %fn_ptr(%x, %y)` | annotated return type |

Direct calls use `@symbol` references. Indirect calls use a function pointer as the first operand, followed by
arguments. Pointer-based indirect calls must carry an explicit signature, for example `[i64(ptr, i64)]`; the verifier
checks argument count, argument types, variadic tails, and return type. A non-void indirect signature must bind the
result to a temp. For variadic callees declared with a trailing `...`, the verifier enforces the declared prefix and
permits only scalar, pointer, or string values after it.

Direct calls may carry bracketed semantic hints after the argument list:
`[nothrow]`, `[readonly]`, `[pure]`, or a comma-separated combination. Known
runtime helpers, extern declarations, imported prototypes, and local functions
remain authoritative; contradictory call-site attributes are rejected by the
verifier. Calls to unknown symbols cannot use these attributes.

Function definitions, imported function prototypes, and extern declarations may
also carry `[nothrow]`, `[readonly]`, and `[pure]`. The optimizer uses this
metadata for DCE, alias analysis, and LICM after verifier validation.

```text
call @f(%x, %y) [nothrow, readonly]
%result = call.indirect [i64(ptr)] %fn_ptr(%arg)
```

##### Error Handling

IL provides a structured error handling system with error values, handler stacks, and resumption points.

**Error Types:**

- `error` — Opaque error value containing kind, code, IP, and line number
- `resume_tok` — Token identifying a resumption point in the error handler stack

**Handler Stack Operations:**
| Instr | Form | Notes |
|-------|------|-------|
| `eh.entry` | `eh.entry` | Mark entry to error handler block |
| `eh.pop` | `eh.pop` | Pop error handler from stack |
| `eh.push` | `eh.push ^handler` | Push error handler block onto stack |

**Trap Operations:**
| Instr | Form | Notes |
|-------|------|-------|
| `trap` | `trap` | Unconditional trap (abort) |
| `trap.err` | `%e = trap.err %kind, %msg` | Create an error value from i32 kind + str message; returns `Error` |
| `trap.from_err` | `trap.from_err i32 7` | Terminator: trap with the given i32 trap-kind code (the `i32` type prefix is required before a constant) |
| `trap.kind` | `%k = trap.kind` or `%k = trap.kind %err` | Read the current trap kind, or the kind stored in an `Error`; returns `i64` |

**Resume Operations:**
| Instr | Form | Notes |
|-------|------|-------|
| `resume.label` | `resume.label %tok, ^label` | Resume at specific label |
| `resume.next` | `resume.next %tok` | Resume at next instruction |
| `resume.same` | `resume.same %tok` | Resume at same instruction |

**Error Value Accessors:**
| Instr | Form | Result |
|-------|------|--------|
| `err.get_code` | `err.get_code %err` | `i32` |
| `err.get_ip` | `err.get_ip %err` | `i64` |
| `err.get_kind` | `err.get_kind %err` | `i32` |
| `err.get_line` | `err.get_line %err` | `i32` |
| `err.get_msg` | `err.get_msg %err` | `str` |

**Example:**

```llvm
func @divide(i64 %a, i64 %b) -> i64 {
entry:
  eh.push ^handler
  %result = sdiv.chk0 %a, %b
  eh.pop
  ret %result
handler(%err:Error, %tok:ResumeTok):
  eh.entry
  trap
}
```

Error-handler blocks must declare `(%err:Error, %tok:ResumeTok)` parameters; the runtime passes the captured error
value and a resume token into the handler so it can inspect or resume the faulting operation.
Per [ADR 0005](../adr/0005-resume-token-provenance.md), `resume_tok` values are
handler-provenance capabilities. A token is produced only when EH dispatch enters
the selected handler. Handler continuations may forward that exact active token
through block parameters, but `resume.*` must consume a token that reached the
resume site by EH dispatch or verified forwarding. Ordinary value uses such as
calls, stores, returns, or arithmetic on `resume_tok` are invalid, and
`resume.label` may not target a handler block because it consumes the token
before transferring control.

#### Runtime ABI

The IL runtime provides helper functions used by front ends and tests. All functions use canonical `Zanna.*` namespace
names. Legacy `@rt_*` aliases are maintained for compatibility when built with `-DZANNA_RUNTIME_NS_DUAL=ON`.

##### Console I/O

| Function                   | Signature     | Notes                                  |
|----------------------------|---------------|----------------------------------------|
| `@Zanna.Terminal.PrintF64` | `f64 -> void` | write float to stdout                  |
| `@Zanna.Terminal.PrintI64` | `i64 -> void` | write integer to stdout                |
| `@Zanna.Terminal.PrintStr` | `str -> void` | write string to stdout                 |
| `@Zanna.Terminal.TryReadLine` | `-> obj<Zanna.Option>` | read line from stdin; `None` on EOF |
| `@Zanna.Terminal.ReadLineResult` | `-> obj<Zanna.Result>` | read line from stdin; `Err` on EOF |
| `@Zanna.Terminal.ReadLine` | `-> str`      | compatibility read line; prefer `TryReadLine` or `ReadLineResult` for EOF |

##### String Operations

| Function                              | Signature                | Notes                                          |
|---------------------------------------|--------------------------|------------------------------------------------|
| `@Zanna.Core.Convert.ToStringDouble`  | `f64 -> str`             | convert double to string                       |
| `@Zanna.Core.Convert.ToStringInt`     | `i64 -> str`             | convert integer to string                      |
| `@Zanna.String.Concat`                | `str × str -> str`       | concatenate strings                            |
| `@Zanna.String.get_Length`            | `str -> i64`             | length in bytes                                |
| `@Zanna.String.Mid`                   | `str × i64 -> str`       | substring from start index to end              |
| `@Zanna.String.MidLen`                | `str × i64 × i64 -> str` | substring; indices clamp; negative bounds trap |

##### Type Conversion

| Function                       | Signature    | Notes                                               |
|--------------------------------|--------------|-----------------------------------------------------|
| `@Zanna.Core.Convert.ToDouble` | `str -> f64` | convert string to double; traps on invalid numeric  |
| `@Zanna.Core.Convert.ToInt64`    | `str -> i64` | convert string to integer; traps on invalid numeric |

##### Memory Management

| Function    | Signature    | Notes                               |
|-------------|--------------|-------------------------------------|
| `@rt_alloc` | `i64 -> ptr` | allocate bytes; negative size traps |

#### Terminal & Keyboard Features

RuntimeFeature → Canonical Zanna name → C runtime symbol

- `TermCls`    → `Zanna.Terminal.Clear`       → `rt_term_cls`
- `TermColor`  → `Zanna.Terminal.SetColor`    → `rt_term_color_i32`
- `TermLocate` → `Zanna.Terminal.SetPosition` → `rt_term_locate_i32`
- `GetKey`     → `Zanna.Terminal.GetKey`      → `rt_getkey_str`
- `InKey`      → `Zanna.Terminal.InKey`       → `rt_inkey_str`

These helpers are gated by feature requests during lowering rather than being emitted unconditionally.

Strings are reference-counted by the runtime implementation. See [src/runtime/](../../src/runtime) for additional details.

#### Memory Model

IL is single-threaded by default. Pointers are plain addresses with no aliasing rules beyond the type requirements of `load`
and `store`. Memory obtained through `alloca` or the runtime follows the alignment rules above, and invalid accesses (
null or misaligned) trap deterministically.

#### Source Locations

`.loc file line col` annotates instructions with source information. It has no semantic effect.

```text
.loc 1 3 4
%v = iadd.ovf 1, 2
```

#### Verifier Rules

Operand and result type checks are now table-driven from `OpcodeInfo`. The
verifier still has bespoke handlers for `idx.chk`, calls, and the handful of ops
wired directly to runtime contracts. Passes can also observe the
`hasSideEffects` flag directly via the shared verifier table.

* First block is `entry` and every block ends with exactly one terminator.
* All referenced labels exist in the same function.
* Operand and result types match instruction signatures; fixed-result opcodes
  must use the schema result type unless a specialised cast/check strategy
  validates an instruction-declared result type.
* Calls match callee arity and types. Runtime catalog parameters spelled `obj` may accept either object pointers or
  managed string handles; raw `ptr` parameters remain distinct from `str`.
* Direct call attributes cannot contradict known runtime helper, extern, import, or local function metadata, and require
  such metadata.
* `call.indirect` requires a pointer-typed callee operand unless it names a known `globaladdr` callee; pointer callees
  require explicit `[ret(params)]` signatures, and non-void signatures require a result temp.
* `addr_of` and `const_str` require a declared string global; `gaddr` requires a declared scalar storage global.
  Generic `load`, `store`, and `gep` operands must use the materialized pointer, not a direct `@global` value.
* `load`/`store` use `ptr` operands and non-void element types. Constant alloca-derived accesses must fit within the
  allocation after considering the access type's byte width.
* `alloca` sizes are `i64`; constant operands must be non-negative. Constant `gep` offsets are signed byte offsets.
  When a constant `gep` derives from a constant-size `alloca`, the cumulative alloca-relative offset is
  range-checked; one-past-the-end is allowed only as a pointer value, not as a dereferenced load/store address.
* Temporaries are defined exactly once and every reachable cross-block use is dominated by its definition.
* Function and block parameters have unique names/ids, non-void types, and each predecessor passes matching arguments.
* Branch arguments must match each destination block's parameters and must reference defined, non-void values. Trailing empty successor bundles may be omitted.
* Handler blocks may be entered by EH dispatch, or by verified handler-continuation
  control flow that forwards the currently active `resume_tok` unchanged. A
  `resume.*` instruction requires the active token for that path, and
  `resume.label` targets must not be handler blocks.
* Returning an `alloca`-derived pointer, including through `gep` or block parameters, is invalid. Direct calls may borrow
  stack-derived pointers under the current IL ABI; stores into non-stack storage and pointer-based indirect calls are
  treated as escapes.
* Runtime ownership metadata drives retain/release checks. Explicit string, array, and object release helpers reject
  double release and dominated use-after-release, while object `DESTROY`, destructor dispatch, and `rt_obj_free` are
  allowed as finalization steps after `rt_obj_release_check0`.
* `cbr` takes an `i1` condition.

Tooling can call `Verifier::verify()` for a single primary diagnostic with related verifier failures attached as notes, or `Verifier::verifyAll()` to collect a bounded list of independent verifier failures. Function-body verification continues across independent functions, so one bad function no longer hides later broken functions in the same module. Frontends and native build paths run verification before handing IL to an optimizer, VM, bytecode compiler, or native backend.

Verifier diagnostics that did not originate from a more specific checker use the stable `V-IL-VERIFY` code. Warnings use `V-IL-WARN`.

Example diagnostics:

```text
L(%x: i64, %x: i64):
              ^ duplicate param %x

br L(1.0)
       ^ arg type mismatch: expected i64, got f64
```

#### Unknown opcodes

Modules may only contain opcodes listed in this reference. Textual IL should be rejected by the verifier or backend preflight before execution. If a bytecode module is corrupted after compilation and the VM decoder encounters an unknown opcode tag, it raises an `InvalidOperation` trap with the failing instruction source location. Diagnostics include the raw mnemonic rendered as `opcode#<value>` to make mismatches obvious even when a module was hand-written, generated by an outdated toolchain, or corrupted after bytecode emission.

The bytecode backend intentionally supports a subset of IL. `gaddr` is supported for globals present in the module and lowers to bytecode global storage; malformed or unknown globals are reported as `V-BC-*` compile diagnostics before the bytecode VM starts.

#### Text Grammar (EBNF)

```ebnf
module      ::= "il" VERSION (target_decl)? decl*
VERSION     ::= NUMBER "." NUMBER ("." NUMBER)?
target_decl ::= "target" STRING
decl        ::= extern | global | func
extern      ::= "extern" SYMBOL "(" type_list? ")" "->" type func_attrs?
global      ::= "global" linkage? ("const")? type SYMBOL ("=" ginit)?
ginit       ::= STRING | INT | FLOAT | "null" | SYMBOL
linkage     ::= "export" | "import"
func        ::= "func" linkage? SYMBOL "(" func_params? ")" "->" type func_attrs? ( "{" block+ "}" )?
func_params ::= func_param ("," func_param)* ("," "...")? | "..."
func_param  ::= type TEMP | TEMP ":" type   (* canonical: "type %name"; legacy "%name: type" also accepted *)
type_list   ::= type ("," type)*
block       ::= LABEL ("(" blk_params? ")")? ":" instr* term
blk_params  ::= blk_param ("," blk_param)*
blk_param   ::= TEMP ":" type               (* block params always use "%name: type" form *)
instr       ::= (TEMP (":" type)? "=")? op   (* type annotation on result: %name:i32 = op *)
term        ::= "ret" value? | "br" label_ref | "cbr" value "," label_ref "," label_ref | "trap" | "trap.from_err" type value | "switch.i32" value "," label_ref ("," INT "->" label_ref)* | "resume.same" value | "resume.next" value | "resume.label" value "," label_ref
label_ref   ::= ("^")? LABEL ("(" value_list? ")")?   (* "^" caret is optional; args passed to block params *)
call_attrs  ::= "[" ("nothrow" | "readonly" | "pure") ("," ("nothrow" | "readonly" | "pure"))* "]"
func_attrs  ::= "[" ("nothrow" | "readonly" | "pure" | "module_init") ("," ("nothrow" | "readonly" | "pure" | "module_init"))* "]"
ind_sig     ::= "[" type "(" (type ("," type)* ("," "...")? | "...")? ")" "]"
op          ::= "add" value "," value | "and" value "," value | "ashr" value "," value |
                "alloca" value | "addr_of" SYMBOL |
                "call" SYMBOL "(" args? ")" call_attrs? | "call.indirect" ind_sig? value "(" args? ")" |
                "cast.fp_to_si.rte.chk" value | "cast.fp_to_ui.rte.chk" value |
                "cast.si_narrow.chk" value | "cast.si_to_fp" value |
                "cast.ui_narrow.chk" value | "cast.ui_to_fp" value |
                "const.f64" FLOAT | "const_null" | "const_str" SYMBOL |
                "eh.entry" | "eh.pop" | "eh.push" label_ref |
                "err.get_code" value | "err.get_ip" value |
                "err.get_kind" value | "err.get_line" value | "err.get_msg" value |
                "fadd" value "," value | "fdiv" value "," value |
                "fcmp_eq" value "," value | "fcmp_ge" value "," value |
                "fcmp_gt" value "," value | "fcmp_le" value "," value |
                "fcmp_lt" value "," value | "fcmp_ne" value "," value |
                "fcmp_ord" value "," value | "fcmp_uno" value "," value |
                "fmul" value "," value | "fptosi" value | "fsub" value "," value |
                "gaddr" SYMBOL | "gep" value "," value |
                "iadd.ovf" value "," value | "icmp_eq" value "," value |
                "icmp_ne" value "," value | "idx.chk" value "," value "," value |
                "imul.ovf" value "," value | "isub.ovf" value "," value |
                "load" type "," value | "lshr" value "," value | "mul" value "," value |
                "or" value "," value |
                "scmp_ge" value "," value | "scmp_gt" value "," value |
                "scmp_le" value "," value | "scmp_lt" value "," value |
                "sdiv" value "," value | "sdiv.chk0" value "," value |
                "select" type "," value "," value "," value |
                "shl" value "," value | "sitofp" value |
                "srem" value "," value | "srem.chk0" value "," value |
                "store" type "," value "," value | "sub" value "," value |
                "trap.err" value "," value | "trap.kind" value? |
                "trunc1" value |
                "ucmp_ge" value "," value | "ucmp_gt" value "," value |
                "ucmp_le" value "," value | "ucmp_lt" value "," value |
                "udiv" value "," value | "udiv.chk0" value "," value |
                "urem" value "," value | "urem.chk0" value "," value |
                "xor" value "," value | "zext1" value
args        ::= value ("," value)*
value_list  ::= value ("," value)*
value       ::= TEMP | SYMBOL | literal
literal     ::= INT | FLOAT | STRING | "true" | "false" | "null"
type        ::= "void" | "i1" | "i16" | "i32" | "i64" | "f64" | "ptr" | "str"
              | "error" | "Error" | "resume_tok" | "ResumeTok"
```

#### Calling Conventions

IL itself is ABI-neutral; each native back end lowers calls to the host platform's convention.
The pipeline selects it automatically — Win64 on Windows, System V on Linux, AAPCS64 on AArch64.

System V x86-64:

* Integer and pointer arguments: `rdi`, `rsi`, `rdx`, `rcx`, `r8`, `r9`.
* Floating-point arguments: `xmm0`–`xmm7`.
* Return values: integers/pointers in `rax`, floats in `xmm0`.
* Call sites maintain 16-byte stack alignment; `i1` arguments are zero-extended to 32 bits.

Windows x64 and AArch64 differ in register assignment, callee-saved sets, shadow space, and the red
zone. See [x86-64 Backend](../specs/x86_64.md), [AArch64 Backend](../specs/aarch64.md), and the
[platform differences](../cross-platform/platform-differences.md#21-x86-64-sysv-vs-win64-abi) summary.

#### Versioning & Conformance

Modules must begin with a version header (e.g., `il 0.3.0`). A conforming implementation accepts this grammar, obeys the semantics above, and
traps on the conditions listed for each instruction. Implementations are validated against the sample suite
under [examples/il](../../examples/il).

<a id="lowering"></a>

## Lowering

### BASIC to IL Lowering Reference

**Note:** Built-in math functions use `@rt_*` helpers in lowered IL. These are registered
runtime functions that remain under the `rt_*` naming scheme; the constfold pass recognizes
these names and folds literal calls at compile time.

| BASIC        | IL runtime call              |
|--------------|------------------------------|
| `ABS(i)`     | `%r = call @rt_abs_i64(%i)`  |
| `ABS(x#)`    | `%r = call @rt_abs_f64(%x)`  |
| `CEIL(x)`    | `%r = call @rt_ceil(%x)`     |
| `F(x)`       | `%r = call @F(%x)`           |
| `FLOOR(x)`   | `%r = call @rt_floor(%x)`    |
| `S(x$, a())` | `call @S(%x, %a)`            |
| `SQR(x)`     | `%r = call @rt_sqrt(%x)`     |

Integer arguments to `SQR`, `FLOOR`, and `CEIL` are first widened to `f64`.

> **Arrays today.** BASIC array declarations and accesses lower to runtime
> helpers such as `@rt_arr_i64_new`, `@rt_arr_i64_len`, `@rt_arr_i64_get`, and
> `@rt_arr_i64_set` (or the corresponding `_str` and `_f64` variants). The
> front end emits bounds checks in IL via `idx.chk` before accessing array storage.
> Optimized O2 IL may rewrite numeric checked accesses to `@rt_arr_i32_get_fast`,
> `@rt_arr_i64_set_fast`, and the corresponding `_f64` helpers only when a
> dominating `idx.chk` or equivalent single-predecessor bounds branch proves the
> same array/index pair safe. Frontends should still emit the checked helpers;
> the optimizer owns the conversion to unchecked fast ABI calls. The bytecode
> backend lowers these `_fast` helpers to direct array opcodes instead of
> out-of-line runtime calls.

### Procedure calls

User-defined `FUNCTION` and `SUB` calls lower to direct `call` instructions.
Arguments are evaluated left-to-right and converted to the callee's expected
types:

| BASIC form                    | Lowered IL                              | Notes                                                    |
|-------------------------------|-----------------------------------------|----------------------------------------------------------|
| `F(1)` (callee expects `f64`) | `%t0 = sitofp 1`<br>`%r = call @F(%t0)` | Widen integer arguments with `sitofp` before the call.   |
| `CALL P(T$)`                  | `call @P(%T)`                           | String parameters pass runtime-managed handles directly. |
| `CALL P(A())`                 | `call @P(%A)`                           | Arrays pass by reference without copying.                |
| `CALL S()`                    | `call @S()`                             | `SUB` invocation used as a statement.                    |
| `X = F()`                     | `%x = call @F()`                        | `FUNCTION` invocation used as an expression.             |

Recursive calls lower the same way; see the [factorial example](#example-4--read-input-and-compute-factorial)
for a recursion sanity check.

### Compilation unit lowering

BASIC programs lower procedures first, then wrap remaining top-level statements
into a synthetic `@main` function:

```text
Program
├─ procs[] → @<name>
└─ main[]  → @main
```

This ordering guarantees functions are listed before `@main`.

### Procedure Definitions

| BASIC                              | IL                                                         |
|------------------------------------|------------------------------------------------------------|
| `FUNCTION f(...) ... END FUNCTION` | `func @f(<params>) -> <retTy>`<br>`entry_f`/`ret_f` blocks |
| `SUB s(...) ... END SUB`           | `func @s(<params>) -> void`<br>`entry_s`/`ret_s` blocks    |

Return type is derived from the name suffix (`$` → `str`, `#` → `f64`, none →
`i64`). Parameters lower by scalar type or as array handles.

Each BASIC `FUNCTION` or `SUB` becomes an IL function named `@<name>`. The
entry block label is deterministically `entry_<name>` and a closing block
`ret_<name>` carries the fallthrough `ret`. Scalar parameters are materialized
by allocating stack slots and storing the incoming values. Array parameters
(`i64[]` or `str[]`) are passed as pointers/handles and stored directly without
copying.

```llvm
func @F(i64 %X) -> i64 {
  entry_F:
    br ret_F
  ret_F:
    ret %X
}
```

```llvm
func @S(i64 %X) -> void {
  entry_S:
    br ret_S
  ret_S:
    ret
}
```

#### Deterministic label naming (procedures)

Blocks created while lowering a procedure use predictable labels so goldens
remain consistent. Within a procedure `proc`, block names follow these patterns:

* `entry_proc` and `ret_proc` for the entry and synthetic return blocks.
* `if_then_k_proc`, `if_else_k_proc`, `if_end_k_proc` for `IF` constructs.
* `while_head_k_proc`, `while_body_k_proc`, `while_end_k_proc` for `WHILE`.
* `do_head_k_proc`, `do_body_k_proc`, `do_end_k_proc` for `DO` / `LOOP`.
* `for_head_k_proc`, `for_body_k_proc`, `for_inc_k_proc`, `for_end_k_proc` for `FOR`.
* `exit_end_k_proc` is not used; `EXIT` statements branch directly to the
  surrounding loop's `*_end_k_proc` block.
* `call_cont_k_proc` for call continuations.

The counter `k` is monotonic **per procedure**; labels never depend on container
iteration order. Rebuilding the same source therefore yields identical label
names across runs.

Example BASIC and corresponding IL excerpt:

```basic
FUNCTION F(X)
FOR I = 0 TO 1
 WHILE X < 10
   IF X THEN CALL P
   X = X + 1
 WEND
NEXT I
F = X
RETURN
END FUNCTION
```

```il
func @F(i64 %X) -> i64 {
  entry_F:
    br for_head_0_F
  for_head_0_F:
    ...
    cbr %t0 for_body_0_F for_end_0_F
  for_body_0_F:
    br while_head_0_F
  while_head_0_F:
    ...
    cbr %t1 while_body_0_F while_end_0_F
  while_body_0_F:
    cbr %t2 if_then_0_F if_end_0_F
  if_then_0_F:
    call @P()
    br call_cont_0_F
  call_cont_0_F:
    ...
    br while_head_0_F
  if_end_0_F:
    ...
    br while_head_0_F
  while_end_0_F:
    br for_inc_0_F
  for_inc_0_F:
    ...
    br for_head_0_F
  for_end_0_F:
    br ret_F
  ret_F:
    ret %X
}
```

### Return statements

`RETURN` lowers directly to an IL `ret` terminator (bare `ret` with no operand for `SUB`, since void functions use `ret` without a value).
Once emitted, the current block is considered closed and no further statements
from the same BASIC block are lowered. This prevents generating dead
instructions after a `RETURN` and ensures each block has exactly one
terminator.

### Boolean expressions

Relational operators (`=`, `<>`, `<`, `>`, and friends) emit `i1` values that
can feed `cbr` directly. When a boolean expression produces a value that must be
reused, the front end materializes it in a temporary stack slot sized for an
`i1` and fills that slot by branching.

#### `NOT`

`NOT expr` flips the operand and stores the inverted constant into the `i1`
slot:

```llvm
  %cond = ...            ; operand lowered earlier, yields i1
  %not_slot = alloca 1   ; temporary for the result (i1)
  cbr %cond, label not_true_0, label not_false_0
not_true_0:
  store i1, %not_slot, 0
  br label not_join_0
not_false_0:
  store i1, %not_slot, 1
  br label not_join_0
not_join_0:
  %result = load i1, %not_slot
```

#### `ANDALSO`

`A ANDALSO B` only evaluates `B` when `A` is true. The slot defaults to `FALSE`
and updates with `B`'s value when the right-hand side is visited:

```llvm
  %lhs = ...               ; first operand (i1)
  %and_slot = alloca 1
  store i1, %and_slot, 0
  cbr %lhs, label and_rhs_0, label and_join_0
and_rhs_0:
  %rhs = ...               ; second operand lowered here
  store i1, %and_slot, %rhs
  br label and_join_0
and_join_0:
  %result = load i1, %and_slot
```

#### `ORELSE`

`A ORELSE B` skips `B` when `A` is true. The `TRUE` branch writes `1` and the
`FALSE` branch lowers `B` and stores that value:

```llvm
  %lhs = ...               ; first operand (i1)
  %or_slot = alloca 1
  cbr %lhs, label or_true_0, label or_rhs_0
or_true_0:
  store i1, %or_slot, 1
  br label or_join_0
or_rhs_0:
  %rhs = ...               ; second operand lowered here
  store i1, %or_slot, %rhs
  br label or_join_0
or_join_0:
  %result = load i1, %or_slot
```

<a id="passes"></a>

## Passes

### Passes overview

The optimisation passes below operate on IL after front-end lowering.

#### Optimizer Correctness Contract

IL optimizer passes must preserve verifier-valid SSA and avoid deleting or moving
operations unless the proof covers the observable behavior:

* DSE treats `MayAlias` as a barrier. A later store kills an earlier store only
  when alias analysis proves the same starting location and the later byte range
  fully covers the earlier write.
* Non-escaping alloca analysis follows `gep`-derived pointers. Passing or storing
  either an alloca or a derived pointer makes the stack slot escaping.
* LICM may hoist loads only when they are non-trapping and cannot observe loop
  writes. Pure/nothrow calls may be hoisted; readonly calls are hoisted only when
  loop memory is stable.
* Loop rotation, induction-variable simplification, and full unrolling are
  intentionally conservative. They skip cases with incomplete branch arguments,
  loop-local preheader operands, arithmetic overflow in analysis-time rewrites,
  or side-effecting loop bodies.
* Value-based CSE does not commute floating-point arithmetic. Integer arithmetic
  and equality comparisons may be normalized where IL semantics make operand
  order unobservable.

<a id="constfold"></a>

### constfold (v1)

Folds literal computations at the IL level.

#### Supported folds

**Runtime call folds** (all operands must be compile-time literals):

| BASIC / runtime call                          | Result                                 |
|-----------------------------------------------|----------------------------------------|
| `ABS(i64 lit)` (`rt_abs_i64`)                 | absolute value as `i64`               |
| `ABS(f64 lit)` (`rt_abs_f64`)                 | absolute value as `f64`               |
| `ACOS(1.0)` (`rt_acos`)                       | `0.0`                                  |
| `ASIN(0.0)` (`rt_asin`)                       | `0.0`                                  |
| `ATAN(0.0)` (`rt_atan`)                       | `0.0`                                  |
| `CEIL(f64 lit)` (`rt_ceil`)                   | `ceil` result as `f64`                |
| `CLAMP(f64, f64, f64)` (`rt_clamp_f64`)       | clamped `f64`                          |
| `CLAMP(i64, i64, i64)` (`rt_clamp_i64`)       | clamped `i64`                          |
| `COS(0.0)` (`rt_cos`)                         | `1.0`                                  |
| `EXP(0.0)` (`rt_exp`)                         | `1.0`                                  |
| `FIX(f64 lit)` (`rt_fix_trunc`)               | truncated `f64`                        |
| `FLOOR(f64 lit)` (`rt_floor`, `rt_int_floor`) | `floor` result as `f64`               |
| `LOG(1.0)` (`rt_log`)                         | `0.0`                                  |
| `MAX(f64, f64)` (`rt_max_f64`)                | larger `f64`                           |
| `MAX(i64, i64)` (`rt_max_i64`)                | larger `i64`                           |
| `MIN(f64, f64)` (`rt_min_f64`)                | smaller `f64`                          |
| `MIN(i64, i64)` (`rt_min_i64`)                | smaller `i64`                          |
| `POW(f64, i64)` *(\|exp\| ≤ 16)*             | `pow` result as `f64`                 |
| `ROUND(f64, i64)` (`rt_round_even`)           | banker's-rounded `f64`                |
| `SGN(f64 lit)` (`rt_sgn_f64`)                 | `-1.0`, `0.0`, or `1.0`              |
| `SGN(i64 lit)` (`rt_sgn_i64`)                 | `-1`, `0`, or `1`                    |
| `SIN(0.0)` (`rt_sin`)                         | `0.0`                                  |
| `SQR(f64 lit ≥ 0)` (`rt_sqrt`)               | `sqrt` result as `f64`               |
| `TAN(0.0)` (`rt_tan`)                         | `0.0`                                  |

**IL opcode folds** (constant operands only):

| Opcode                   | Condition                        | Result             |
|--------------------------|----------------------------------|--------------------|
| `cast.fp_to_si.rte.chk`  | finite `f64` literal, in range   | `i64` literal      |
| `iadd.ovf`               | both `i64` literals, no overflow | `i64` sum          |
| `imul.ovf`               | both `i64` literals, no overflow | `i64` product      |
| `isub.ovf`               | both `i64` literals, no overflow | `i64` difference   |
| `sdiv.chk0`              | both `i64`, divisor ≠ 0          | `i64` quotient     |
| `srem.chk0`              | both `i64`, divisor ≠ 0          | `i64` remainder    |
| `fadd`, `fsub`, `fmul`   | both `f64` literals              | IEEE `f64` result  |
| `fdiv`                   | both `f64` literals              | IEEE `f64` result  |
| `fcmp_ord`, `fcmp_uno`   | both `f64` literals              | `i1` literal       |

Basic IEEE arithmetic folds preserve defined special results, including `NaN`,
`Inf`, `-Inf`, and signed zero. Runtime math calls are folded only for
host-independent exact cases or deterministic sequenced arithmetic; other
libm results remain runtime calls, as specified by ADR 0114.

#### Caveats

* Trig functions fold only at the identity points listed above, not for general inputs.
* `POW` folds only when the exponent is an integer literal with `|exp| ≤ 16` and the result is finite.
* `SQR` requires a non-negative input; negative inputs are not folded (would trap at runtime).

<a id="mem2reg"></a>

### mem2reg (v3)

Promotes stack slots to SSA registers across branches and loops by introducing
block parameters and passing values along edges.

#### Scope

* Handles integer (`i64`), float (`f64`), and boolean (`i1`) slots.
* The address of the alloca must not escape (only used by `load`/`store`; direct
  branch-argument forwarding of the address is treated as address-taken).
* Alloca zero-initialization is modeled as the initial reaching value for the
  promoted slot, so load-before-store cases become explicit zero constants
  rather than ad hoc missing-definition fallbacks.

#### Algorithm (seal & rename)

1. Collect promotable allocas.
2. Walk blocks in depth-first order, maintaining the current SSA value for each
   variable per block.
3. Loads are replaced with the current value; stores update it.
4. If a block reads a variable before all predecessors are seen, create a
   placeholder block parameter. When the block becomes *sealed* (all preds
   known), resolve placeholders to real parameters with the correct incoming
   arguments.
5. Values are propagated along edges by updating predecessor terminators with
   branch arguments.
6. After processing, remove the original allocas and stores.

#### Example (diamond)

Input IL:

```llvm
il 0.3.0
func @main() -> i64 {
entry:
  %t0 = alloca 8
  %t1 = icmp_eq 0, 0
  cbr %t1, T, F
T:
  store i64, %t0, 2
  br Join
F:
  store i64, %t0, 3
  br Join
Join:
  %t2 = load i64, %t0
  ret %t2
}
```

After `mem2reg`:

```llvm
il 0.3.0
func @main() -> i64 {
entry:
  %t1 = icmp_eq 0, 0
  cbr %t1, T, F
T:
  br Join(2)
F:
  br Join(3)
Join(%a0:i64):
  ret %a0
}
```

The alloca, loads, and stores are removed. A block parameter on `Join` receives
the value from each predecessor via branch arguments.

#### Example (loop)

Input IL:

```llvm
il 0.3.0
func @main() -> i64 {
entry:
  %t0 = alloca 8
  store i64, %t0, 0
  br L1
L1:
  %t1 = load i64, %t0
  %t2 = iadd.ovf %t1, 1
  store i64, %t0, %t2
  %t3 = scmp_lt %t2, 10
  cbr %t3, L1, Exit
Exit:
  %t4 = load i64, %t0
  ret %t4
}
```

After `mem2reg`:

```llvm
il 0.3.0
func @main() -> i64 {
entry:
  br L1(0)
L1(%a0:i64):
  %t2 = iadd.ovf %a0, 1
  %t3 = scmp_lt %t2, 10
  cbr %t3, L1(%t2), Exit(%t2)
Exit(%a1:i64):
  ret %a1
}
```

The loop header `L1` has a block parameter `%a0` representing the running value,
fed by both the entry edge and the back-edge. The exit block receives the final
value via parameter `%a1`.

#### Stats

`zanna il-opt --mem2reg-stats` prints the number of promoted variables and the
removed loads/stores when the pass runs.

## Examples

### BASIC to IL Examples

The archived BASIC to IL gallery collected six small BASIC programs (≈10–20 lines each) with their fully lowered IL
modules. The examples below update that material to IL v0.3.0 while preserving the original teaching intent.

**Legacy Notation:** The examples in this section use legacy `@rt_*` function names for compatibility. These work when
the runtime is built with `-DZANNA_RUNTIME_NS_DUAL=ON` (the current default). New code should use the canonical
`@Zanna.*` names documented in the Runtime ABI section above. For example:

- `@rt_print_str` → `@Zanna.Terminal.PrintStr`
- `@rt_print_i64` → `@Zanna.Terminal.PrintI64`
- `@rt_str_len` → `@Zanna.String.get_Length`

#### Example 1 — Hello, arithmetic, and a conditional branch

**BASIC**

```basic
PRINT "HELLO"
LET X = 2 + 3
LET Y = X * 2
PRINT "READY"
PRINT Y
IF Y > 8 THEN GOTO PrintY
PRINT 4
GOTO Done
PrintY:
PRINT Y
Done:
END
```

**IL**

```llvm
il 0.3.0
extern @rt_print_str(str) -> void
extern @rt_print_i64(i64) -> void
global const str @.L0 = "HELLO"
global const str @.L1 = "READY"
func @main() -> i64 {
entry:
  %x_slot = alloca 8
  %y_slot = alloca 8
  %t0 = iadd.ovf 2, 3
  store i64, %x_slot, %t0
  %xv = load i64, %x_slot
  %t1 = imul.ovf %xv, 2
  store i64, %y_slot, %t1
  %s0 = const_str @.L0
  call @rt_print_str(%s0)
  %s1 = const_str @.L1
  call @rt_print_str(%s1)
  %yv0 = load i64, %y_slot
  call @rt_print_i64(%yv0)
  %cond = scmp_gt %yv0, 8
  cbr %cond, then80, else60
then80:
  %yv1 = load i64, %y_slot
  call @rt_print_i64(%yv1)
  br done
else60:
  call @rt_print_i64(4)
  br done
done:
  ret 0
}
```

*Notes:* locals lower to stack slots, branches are explicit, and runtime calls handle I/O.

#### Example 2 — Sum 1..10 with a WHILE loop

**BASIC**

```basic
PRINT "SUM 1..10"
LET I = 1
LET S = 0
WHILE I <= 10
LET S = S + I
LET I = I + 1
WEND
PRINT S
PRINT "DONE"
END
```

**IL**

```llvm
il 0.3.0
extern @rt_print_str(str) -> void
extern @rt_print_i64(i64) -> void
global const str @.L0 = "SUM 1..10"
global const str @.L1 = "DONE"
func @main() -> i64 {
entry:
  %i_slot = alloca 8
  %s_slot = alloca 8
  %h = const_str @.L0
  call @rt_print_str(%h)
  store i64, %i_slot, 1
  store i64, %s_slot, 0
  br loop_head
loop_head:
  %i0 = load i64, %i_slot
  %cond = scmp_le %i0, 10
  cbr %cond, loop_body, done
loop_body:
  %s0 = load i64, %s_slot
  %s1 = iadd.ovf %s0, %i0
  store i64, %s_slot, %s1
  %i1 = iadd.ovf %i0, 1
  store i64, %i_slot, %i1
  br loop_head
done:
  %s2 = load i64, %s_slot
  call @rt_print_i64(%s2)
  %d = const_str @.L1
  call @rt_print_str(%d)
  ret 0
}
```

*Notes:* the canonical while-loop lowering uses explicit head/body/done blocks.

#### Example 3 — Nested loops printing a multiplication table

**BASIC**

```basic
PRINT "TABLE 5x5"
LET N = 5
LET I = 1
WHILE I <= N
LET J = 1
WHILE J <= N
PRINT I * J
LET J = J + 1
WEND
LET I = I + 1
WEND
END
```

**IL**

```llvm
il 0.3.0
extern @rt_print_str(str) -> void
extern @rt_print_i64(i64) -> void
global const str @.L0 = "TABLE 5x5"
func @main() -> i64 {
entry:
  %n_slot = alloca 8
  %i_slot = alloca 8
  %j_slot = alloca 8
  %h = const_str @.L0
  call @rt_print_str(%h)
  store i64, %n_slot, 5
  store i64, %i_slot, 1
  br outer_head
outer_head:
  %i0 = load i64, %i_slot
  %n0 = load i64, %n_slot
  %oc = scmp_le %i0, %n0
  cbr %oc, outer_body, outer_done
outer_body:
  store i64, %j_slot, 1
  br inner_head
inner_head:
  %j0 = load i64, %j_slot
  %n1 = load i64, %n_slot
  %ic = scmp_le %j0, %n1
  cbr %ic, inner_body, inner_done
inner_body:
  %i1 = load i64, %i_slot
  %prod = imul.ovf %i1, %j0
  call @rt_print_i64(%prod)
  %j1 = iadd.ovf %j0, 1
  store i64, %j_slot, %j1
  br inner_head
inner_done:
  %i2 = iadd.ovf %i0, 1
  store i64, %i_slot, %i2
  br outer_head
outer_done:
  ret 0
}
```

*Notes:* nested control flow forms two explicit loop nests.

#### Example 4 — Read input and compute factorial

**BASIC**

```basic
PRINT "FACTORIAL"
PRINT "ENTER N:"
LET S$ = INPUT$
LET N = VAL(S$)
LET R = 1
WHILE N > 1
LET R = R * N
LET N = N - 1
WEND
PRINT R
END
```

**IL**

```llvm
il 0.3.0
extern @rt_print_str(str) -> void
extern @rt_print_i64(i64) -> void
extern @rt_input_line() -> str
extern @rt_to_int(str) -> i64
global const str @.L0 = "FACTORIAL"
global const str @.L1 = "ENTER N:"
func @main() -> i64 {
entry:
  %s_slot = alloca 8
  %n_slot = alloca 8
  %r_slot = alloca 8
  %h0 = const_str @.L0
  call @rt_print_str(%h0)
  %h1 = const_str @.L1
  call @rt_print_str(%h1)
  %line = call @rt_input_line()
  store str, %s_slot, %line
  %sval = load str, %s_slot
  %n0 = call @rt_to_int(%sval)
  store i64, %n_slot, %n0
  store i64, %r_slot, 1
  br loop_head
loop_head:
  %n1 = load i64, %n_slot
  %cond = scmp_gt %n1, 1
  cbr %cond, loop_body, done
loop_body:
  %r0 = load i64, %r_slot
  %r1 = imul.ovf %r0, %n1
  store i64, %r_slot, %r1
  %n2 = isub.ovf %n1, 1
  store i64, %n_slot, %n2
  br loop_head
done:
  %r2 = load i64, %r_slot
  call @rt_print_i64(%r2)
  ret 0
}
```

*Notes:* the runtime provides string input and numeric conversion helpers; errors surface as traps.

#### Example 5 — String concat, length, substring, equality

**BASIC**

```basic
LET A$ = "JOHN"
LET B$ = "DOE"
LET C$ = A$ + " "
LET C$ = C$ + B$
PRINT C$
LET L = LEN(C$)
PRINT L
PRINT MID$(C$, 1, 1)
IF C$ = "JOHN DOE" THEN PRINT 1 ELSE PRINT 0
END
```

**IL**

```llvm
il 0.3.0
extern @rt_print_str(str) -> void
extern @rt_print_i64(i64) -> void
extern @rt_str_len(str) -> i64
extern @rt_str_concat(str, str) -> str
extern @rt_str_substr(str, i64, i64) -> str
extern @rt_str_eq(str, str) -> i1
global const str @.L0 = "JOHN"
global const str @.L1 = "DOE"
global const str @.L2 = " "
global const str @.L3 = "JOHN DOE"
func @main() -> i64 {
entry:
  %a_slot = alloca 8
  %b_slot = alloca 8
  %c_slot = alloca 8
  %l_slot = alloca 8
  %sA = const_str @.L0
  store str, %a_slot, %sA
  %sB = const_str @.L1
  store str, %b_slot, %sB
  %sSpace = const_str @.L2
  %a0 = load str, %a_slot
  %c0 = call @rt_str_concat(%a0, %sSpace)
  store str, %c_slot, %c0
  %b0 = load str, %b_slot
  %c1 = call @rt_str_concat(%c0, %b0)
  store str, %c_slot, %c1
  call @rt_print_str(%c1)
  %len = call @rt_str_len(%c1)
  store i64, %l_slot, %len
  call @rt_print_i64(%len)
  %first = call @rt_str_substr(%c1, 0, 1)
  call @rt_print_str(%first)
  %target = const_str @.L3
  %eq = call @rt_str_eq(%c1, %target)
  cbr %eq, then1, else0
then1:
  call @rt_print_i64(1)
  br exit
else0:
  call @rt_print_i64(0)
  br exit
exit:
  ret 0
}
```

*Notes:* all string work is delegated to runtime helpers and equality returns an `i1` flag.

#### Example 6 — Heap array via `rt_alloc`, squares, and floating average

**BASIC**

```basic
LET N = 5
DIM A(N)
LET I = 0
LET SUM = 0
WHILE I < N
LET A(I) = I * I
LET SUM = SUM + A(I)
LET I = I + 1
WEND
LET AVG = SUM / N
PRINT AVG
PRINT "DONE"
END
```

**IL**

```llvm
il 0.3.0
extern @rt_alloc(i64) -> ptr
extern @rt_print_f64(f64) -> void
extern @rt_print_str(str) -> void
global const str @.L0 = "DONE"
func @main() -> i64 {
entry:
  %n_slot = alloca 8
  %i_slot = alloca 8
  %sum_slot = alloca 8
  %a_slot = alloca 8
  store i64, %n_slot, 5
  store i64, %i_slot, 0
  store i64, %sum_slot, 0
  %n0 = load i64, %n_slot
  %bytes = imul.ovf %n0, 8
  %base = call @rt_alloc(%bytes)
  store ptr, %a_slot, %base
  br loop_head
loop_head:
  %i0 = load i64, %i_slot
  %n1 = load i64, %n_slot
  %cond = scmp_lt %i0, %n1
  cbr %cond, loop_body, done
loop_body:
  %a0 = load ptr, %a_slot
  %offset = shl %i0, 3
  %elem_ptr = gep %a0, %offset
  %sq = imul.ovf %i0, %i0
  store i64, %elem_ptr, %sq
  %sum0 = load i64, %sum_slot
  %sum1 = iadd.ovf %sum0, %sq
  store i64, %sum_slot, %sum1
  %i1 = iadd.ovf %i0, 1
  store i64, %i_slot, %i1
  br loop_head
done:
  %sum2 = load i64, %sum_slot
  %n2 = load i64, %n_slot
  %fsum = sitofp %sum2
  %fn = sitofp %n2
  %avg = fdiv %fsum, %fn
  call @rt_print_f64(%avg)
  %msg = const_str @.L0
  call @rt_print_str(%msg)
  ret 0
}
```

*Notes:* heap allocation models BASIC arrays and floating arithmetic uses `sitofp`/`fdiv`.

#### How to use these examples

* Golden tests: keep each BASIC + IL pair in sync when validating lowering.
* VM vs native: run the IL through both execution modes and compare output.
* Coverage: together they exercise arithmetic, branching, loops, input, strings, heap, and floating-point operations.

Sources:

- docs/il/il-guide.md#quickstart
- docs/il/il-guide.md#reference
- docs/il/il-guide.md#reference
- docs/il/il-guide.md#lowering
- docs/il/il-guide.md#constfold
- docs/il/il-guide.md#mem2reg
- (archived: basic-to-il-examples.md — no longer available)
