---
status: active
audience: public
last-verified: 2026-08-31
---

# Zanna Arithmetic Semantics Reference

This document specifies the exact arithmetic semantics that Zanna guarantees across
all execution layers (VM, AArch64 native, x86-64 native) and both frontends
(Zia, BASIC). These guarantees are tested by the conformance test suite in
`src/tests/conformance/`.

## Shared Implementation Contract

The IL VM and BytecodeVM consume the shared scalar semantic kernel in
`src/il/semantics/ScalarOps.hpp` for checked integer arithmetic, division and
remainder traps, shift masking, `idx.chk` normalization, narrowing conversions,
and checked FP-to-integer casts. Bytecode-specific width decoding and trap
translation live in `src/bytecode/BytecodeSemantics.hpp`.

Bytecode format version 3 carries explicit width metadata for checked scalar
operations whose IL semantics depend on result type: `.ovf` arithmetic,
checked signed division and remainder, `idx.chk`, and checked FP-to-integer
casts. The compiler emits that metadata from the IL instruction type, and the
loader validates the compact width fields before a module can execute.

The structural guard `scripts/check_vm_semantics_duplication.sh` and the
conformance target `test_vm_bytecode_scalar_semantics` are intended to catch
future divergence between the tree-walking VM, bytecode switch dispatch, and
bytecode threaded dispatch.

## Integer Arithmetic

All integer arithmetic operates on **I64** (64-bit signed two's complement).
There are no sub-width arithmetic instructions; narrowing is explicit via cast
opcodes.

### Checked (Overflow-Trapping) Arithmetic — the only legal signed forms

| Opcode | Behavior | On Overflow |
|--------|----------|-------------|
| `iadd.ovf` | Signed addition with overflow check | **Trap** |
| `isub.ovf` | Signed subtraction with overflow check | **Trap** |
| `imul.ovf` | Signed multiplication with overflow check | **Trap** |

These support sub-width type annotations (I32, I16). The overflow check uses
the type's range: `iadd.ovf : i32` traps when the result exceeds `INT32_MAX` or
falls below `INT32_MIN`.
Native x86-64 and AArch64 lowering preserve those annotations by sign-extending
operands to the annotated width before checking the computed result.

> **Plain `add`/`sub`/`mul` are verifier-rejected in frontend IL.** The opcodes
> exist in `Opcode.def`, but the IL verifier (see
> `src/il/verify/generated/SpecTables.cpp`) rejects them with messages like
> *"signed integer add must use iadd.ovf (traps on overflow)"* unless it can
> independently prove the operation cannot overflow. The optimizer demotes
> checked forms to plain ones behind exactly that proof
> ([ADR 0026](../adr/0026-range-analysis-demotion-proofs.md)), which is why
> optimized IL still verifies. Frontends must emit the `.ovf` forms; there is no
> public "wrapping arithmetic" path at the IL level for signed integers.

Zia uses these checked variants by default. `overflow-checks` is a
`zanna.project` manifest directive (default `true`), not a command-line flag; the
verifier requires `.ovf` opcodes in frontend IL regardless of its value.

### Division

| Opcode | Truncation | Div-by-zero | MIN/-1 |
|--------|-----------|-------------|--------|
| `sdiv.chk0` | Toward zero (C11) | **Trap** | **Trap** |
| `udiv.chk0` | Toward zero | **Trap** | N/A |

Source IL should use the `.chk0` forms. The verifier rejects plain `sdiv` /
`udiv` unless an optimizer has already proven the divisor non-zero; CheckOpt
uses that proof to demote checked division to the plain opcode so native
backends can omit dead trap edges.

**Division truncation direction**: `7 / -2 = -3`, `-7 / 2 = -3`.

### Remainder (Modulo)

| Opcode | Sign rule | Div-by-zero | MIN%-1 |
|--------|-----------|-------------|--------|
| `srem.chk0` | Dividend's sign | **Trap** | `0` (no trap) |
| `urem.chk0` | Unsigned | **Trap** | N/A |

Source IL should use the `.chk0` forms. As with division, the verifier accepts
plain `srem` / `urem` only when CheckOpt has preserved a non-zero constant
divisor proof by demoting from a checked opcode.

**Remainder sign rule** (C11): `-7 % 2 = -1`, `7 % -2 = 1`, `-7 % -2 = -1`.

`MIN % -1 = 0` is mathematically correct and does not trap.

### Bitwise Operations

| Opcode | Behavior |
|--------|----------|
| `and`  | Bitwise AND (I64) |
| `or`   | Bitwise OR (I64) |
| `xor`  | Bitwise XOR (I64) |

All operate on full 64-bit values.

## Shift Operations

| Opcode | Direction | Extension | Masking |
|--------|-----------|-----------|---------|
| `shl`  | Left | N/A | `shift & 63` |
| `ashr` | Right | Sign-extending (arithmetic) | `shift & 63` |
| `lshr` | Right | Zero-extending (logical) | `shift & 63` |

**Shift masking**: Shift amounts are masked to `[0, 63]`, matching x86-64
hardware behavior. `shl(1, 64)` produces `1` (shift amount becomes 0).
This is **not** undefined behavior.

**AShr sign extension**: `ashr(-1, 63)` produces `-1` (all ones).
**LShr zero extension**: `lshr(-1, 63)` produces `1`.

## Floating-Point Arithmetic

All floating-point operations use **F64** (IEEE-754 binary64, double precision).

### Basic Operations

| Opcode | Behavior |
|--------|----------|
| `fadd` | IEEE-754 addition |
| `fsub` | IEEE-754 subtraction |
| `fmul` | IEEE-754 multiplication |
| `fdiv` | IEEE-754 division |

**Rounding mode**: Round-to-nearest-even (hardware default). Not configurable.

### Special Values

| Expression | Result |
|-----------|--------|
| `x + NaN` | `NaN` (propagates) |
| `0.0 / 0.0` | `NaN` |
| `1.0 / 0.0` | `+Inf` |
| `-1.0 / 0.0` | `-Inf` |
| `Inf + Inf` | `Inf` |
| `Inf - Inf` | `NaN` |
| `Inf * 0.0` | `NaN` |

Runtime sign helpers preserve unordered input: `rt_sgn_f64(NaN)` returns `NaN`, while negative, zero, and positive
finite values return `-1.0`, `0.0`, and `1.0`.

### Floating-Point Comparisons

| Opcode | `NaN` behavior | Meaning |
|--------|---------------|---------|
| `fcmp_eq` | `false` | Ordered equal |
| `fcmp_ne` | `true` | Unordered not-equal |
| `fcmp_lt` | `false` | Ordered less-than |
| `fcmp_le` | `false` | Ordered less-or-equal |
| `fcmp_gt` | `false` | Ordered greater-than |
| `fcmp_ge` | `false` | Ordered greater-or-equal |
| `fcmp_ord` | `false` | Both operands are not NaN |
| `fcmp_uno` | `true` | At least one operand is NaN |

All ordered comparisons return `false` when either operand is NaN.
`fcmp_ne` is unordered: returns `true` when either operand is NaN.

## Type Conversions

### Integer-to-Float

| Opcode | Behavior |
|--------|----------|
| `sitofp` | Signed I64 to F64. Exact for values with <= 53 significant bits; rounded otherwise. |
| `cast.si_to_fp` | Same semantics as `sitofp`. |
| `cast.ui_to_fp` | Unsigned I64 to F64. |

### Float-to-Integer

| Opcode | Behavior | NaN | Out-of-range |
|--------|----------|-----|-------------|
| `fptosi` | Truncation toward zero | **Trap** | **Trap** |
| `cast.fp_to_si.rte.chk` | Round-to-even, then convert | **Trap** | **Trap** |
| `cast.fp_to_ui.rte.chk` | Round-to-even, then convert (unsigned) | **Trap** | **Trap** |

The VM and native backends trap `fptosi` on NaN and overflow (not UB).
The public verifier currently directs source IL toward `cast.fp_to_si.rte.chk`;
these `fptosi` rules apply to execution layers that receive the opcode.

`Zanna.Core.Convert.NumToInt` is a separate public conversion helper with
saturating semantics for compatibility with existing library code: finite inputs
truncate toward zero, `NaN` returns `0`, and values outside the signed 64-bit
range clamp to `INT64_MIN` / `INT64_MAX`. Use checked IL casts when NaN or range
violations must trap.

Checked FP-to-integer casts use the static result type as the range contract:
`i16`, `i32`, and `i64` results check against their own signed or unsigned
exclusive upper bounds. For unsigned casts, NaN and negative inputs are
`InvalidCast`; finite values outside `[0, 2^N)` are `Overflow`.

`fptosi(1.9)` produces `1` (truncation toward zero).
`fptosi(-1.9)` produces `-1` (truncation toward zero).

### Integer Narrowing

| Opcode | Behavior | Out-of-range |
|--------|----------|-------------|
| `cast.si_narrow.chk` | I64 to I32 or I16 (signed) | **Trap** |
| `cast.ui_narrow.chk` | I64 to I32 or I16 (unsigned) | **Trap** |

## Frontend Promotion Rules

### Zia

Type hierarchy: `Byte < Integer < Number`

| Expression | IL Emitted | Notes |
|-----------|-----------|-------|
| `Integer + Integer` | `iadd.ovf` | Trap on signed overflow |
| `Number + Number` | `fadd` | |
| `Integer + Number` | `sitofp` on Integer operand, then `fadd` | Implicit widening |
| `Integer / Integer` | `sdiv.chk0` | Truncation toward zero, trap on `/0` and `MIN/-1` |
| `Integer % Integer` | `srem.chk0` | Dividend sign, trap on `/0` |
| `Byte + Integer` | — | **Currently broken.** `Byte` lowers to `i32` and no implicit widening is inserted, so the module fails IL verification (`operand type mismatch: operand 1 must be i32`). `Byte + Byte` and even `"s" + byteValue` fail the same way. Convert explicitly before use. |

### BASIC

Semantic promotion lattice: `INTEGER% < LONG& < SINGLE! < DOUBLE#`.

The lattice governs diagnostics and result-type spelling only. Every integral
value lowers to `i64` and every floating value to `f64` — `%` and `&` both lower
to `i64`, `!` and `#` both lower to `f64`. Narrow BASIC storage (`i16`, `i32`,
`f32`) is not a current guarantee; see
[specs/numerics.md](../specs/numerics.md), which is the source of truth here.

| Expression | IL Emitted | Notes |
|-----------|-----------|-------|
| `A% + B%` | `iadd.ovf` | I64 result |
| `A + B` (float) | `fadd` | |
| `A \ B` (integer div) | `sdiv.chk0` | Always checked, always I64 |
| `A MOD B` | `srem.chk0` | Always checked, always I64 |
| `A / B` (float div) | `fdiv` | Both promoted to F64 |
| `TRUE` | `-1` (I64) | BASIC uses `-1` for true |

Both frontends emit the checked variants because source IL is expected to use
the plain (non-`.ovf` / non-`.chk0`) signed integer opcodes only after a
proving optimizer demotes them. The BASIC frontend
exposes an internal `OverflowPolicy::Wrap` switch (in `EmitCommon.cpp`),
but every current call site passes `OverflowPolicy::Checked`, so the wrapping
path is dead code at lowering time and would fail verification if enabled.
