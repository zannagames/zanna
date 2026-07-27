---
status: active
audience: public
last-verified: 2026-07-26
---

# Numeric Semantics

This document is the single source of truth for implemented numeric behaviour
across the BASIC front end, IL constant-folder, and the runtime/VM. All language
layers must agree with these rules.

The current BASIC implementation preserves legacy surface names for type
spelling, suffixes, diagnostics, and conversion functions, but it stores numeric
values in two IL categories: `i64` for integral values and `f64` for floating
values. Narrow BASIC storage (`i16`, `i32`, `f32`) is not a current guarantee.

## Primitive Types and Ranges

| BASIC spelling | Internal representation | Range / current storage contract |
|----------------|-------------------------|----------------------------------|
| `INTEGER`, `INT` | signed 64-bit (`i64`) | −9,223,372,036,854,775,808 … 9,223,372,036,854,775,807 |
| `LONG` | signed 64-bit (`i64`) | Same storage range as `INTEGER`; the spelling remains available for diagnostics and legacy source compatibility. |
| `SINGLE` | IEEE-754 binary64 (`f64`) | Stored as `double` values. `CSNG` explicitly rounds through binary32 and widens the result back to `f64`. |
| `DOUBLE` | IEEE-754 binary64 (`f64`) | Stored as `double` values. Operations may produce NaN/Inf where specified below. |

Semantic promotions still use the surface lattice `INTEGER < LONG < SINGLE <
DOUBLE` for diagnostics and result-type spelling. Lowering then maps integral
results to `i64` and floating results to `f64`. Integer literals without a
suffix start as `INTEGER`; `%` and `&` both lower to `i64`. Floating literals
default to `DOUBLE`; `!` and `#` both lower to `f64`.

## Operator Result Types

* `+`, `-`, `*` follow the semantic promotion lattice, then lower integer
  results to checked `i64` arithmetic and floating results to `f64` arithmetic.
* `/` always performs floating-point division in `f64`. The semantic result is
  `DOUBLE` for integral operands or when either operand is `DOUBLE`; otherwise
  it is `SINGLE`.
* `\` performs integer division on `INTEGER` or `LONG` inputs in `i64`. The
  semantic result adopts the promoted integer rank.
* `MOD` matches the rank and sign rules of its inputs; the result keeps the sign of
  the dividend (left operand).
* `^` (power) is computed in `f64`. The result is `DOUBLE`. A `DomainError` trap
  occurs when the base is negative and the exponent is non-integral. If the result
  magnitude overflows to ±∞ or becomes NaN, an `Overflow` trap is raised.

### Explicit MOD Examples

* `-3 MOD 2 = -1`
* `3 MOD -2 = 1`

## Division and Remainder Traps

* `/` never traps; it follows IEEE rules (NaN/Inf propagate).
* `\` truncates the quotient toward zero. It traps with `DivideByZero` when the
  divisor is zero and with `Overflow` when the mathematically exact quotient would
  fall outside the `i64` target range (for example, `-9_223_372_036_854_775_808 \ -1`).
* `MOD` is defined as `r = a − trunc(a / b) * b` with the sign of `a`. It traps
  with `DivideByZero` when `b = 0`.

## Rounding and Conversion Functions

All conversions that round from floating point use round-to-nearest, ties-to-even
("banker’s rounding"). Unless otherwise stated, traps are reported as `Overflow`.

| Function   | Description |
|------------|-------------|
| `CDBL(x)`  | Convert to `DOUBLE` (`f64`). Always succeeds for finite inputs. |
| `CINT(x)`  | Round-to-nearest-even, validate against the classic 16-bit `INTEGER` range −32,768…32,767, then return the value in `i64` storage. Example: `CINT(2.5) = 2`, `CINT(3.5) = 4`. |
| `CLNG(x)`  | Round-to-nearest-even, validate against the classic 32-bit `LONG` range −2,147,483,648…2,147,483,647, then return the value in `i64` storage. |
| `CSNG(x)`  | Convert through IEEE-754 binary32. Trap if the rounded binary32 result would be non-finite, then return the widened value in `f64` storage. |
| `FIX(x)`   | Truncate toward zero. Works on any numeric rank and returns a floating result for floating inputs. |
| `INT(x)`   | Floor: greatest integral value <= `x`. Works on any numeric rank. |
| `ROUND(x)` | Round-to-nearest-even. Result rank follows argument rank. |

Additional examples:

* `INT(-1.5) = -2`
* `FIX(-1.5) = -1`

## Power Operator Edge Cases

`^` is evaluated via `pow` in `f64`. Inputs are widened to `DOUBLE` before the
operation. The evaluator must detect:

* **Negative base with fractional exponent** → `DomainError`.
* **Non-finite results** → `Overflow` trap.
* **Subnormal inputs** are permitted; the result is rounded to `DOUBLE` normally.

## VAL Function

`VAL` parses ASCII/UTF‑8 according to the grammar below (whitespace `ws` is
`[ \t\r\n]*`).

```text
VALInput ::= ws Number? (Trailing*)
Number   ::= Sign? Digits ('.' Digits?)? Exponent?
Sign     ::= '+' | '-'
Digits   ::= [0-9]+
Exponent ::= ('E' | 'e') Sign? Digits
Trailing ::= any non-numeric character (parsing stops before it)
```

Rules:

* Leading and trailing whitespace is ignored.
* Parsing stops at the first non-numeric character after the number; the rest of
  the string is ignored.
* If the first non-whitespace character is not part of a number, the result is 0.
* The resulting value is produced as `DOUBLE`. Overflow to a non-finite value
  traps with `Overflow`.

## STR$ Formatting

`STR$` prints numeric values using invariant decimal formatting:

* Integers use the minimal decimal representation with a leading `-` for negatives
  (no extra spaces or `+`).
* Values formatted through the `SINGLE` surface helper round through binary32
  and use `printf("%.9g")` semantics.
* `DOUBLE` values round-trip using `printf("%.17g")` semantics.

These guarantees ensure `VAL(STR$(x))` yields `x` for finite numbers.

## Checked IL Operations

The IL exposes checked variants that must be used to enforce BASIC semantics.
Each trap type propagates to the VM/runtime as a terminating error with the
indicated condition.

| IL op                   | Description                                            | Trap                                                      |
|-------------------------|--------------------------------------------------------|-----------------------------------------------------------|
| `cast.fp_to_si.rte.chk` | Float-to-signed-integer cast (round to even).          | `Overflow` on NaN or out-of-range.                        |
| `cast.fp_to_ui.rte.chk` | Float-to-unsigned-integer cast (round to even).        | `Overflow` on NaN or out-of-range.                        |
| `cast.si_narrow.chk`    | Signed narrowing cast.                                 | `Overflow` when the result is out of range.               |
| `cast.ui_narrow.chk`    | Unsigned narrowing cast.                               | `Overflow` when the result is out of range.               |
| `iadd.ovf`              | Signed integer addition with overflow detection.       | `Overflow`                                                |
| `imul.ovf`              | Signed integer multiplication with overflow detection. | `Overflow`                                                |
| `isub.ovf`              | Signed integer subtraction with overflow detection.    | `Overflow`                                                |
| `sdiv.chk0`             | Signed integer division.                               | `DivideByZero` on zero divisor; `Overflow` on `MIN / -1`. |
| `srem.chk0`             | Signed remainder.                                      | `DivideByZero` on zero divisor.                           |

> **Note (2026-04-09):** The plain `add`/`sub`/`mul`/`sdiv`/`udiv`/`srem`/`urem` opcodes still
> exist in `Opcode.def` for legacy lowering paths but are **rejected by the IL verifier** for
> signed integer types (see `src/il/verify/generated/SpecTables.cpp`). Frontends must always emit
> the checked variants listed above; the doc's earlier wording allowing unchecked use "when the
> result is statically proven in range" is obsolete.

## BASIC ↔ IL Lowering Table

| BASIC construct                          | Operand ranks                | IL lowering |
|------------------------------------------|------------------------------|-------------|
| `a + b`, `a - b`, `a * b` (both integer) | promote to `LONG` as needed  | `iadd.ovf`, `isub.ovf`, `imul.ovf` on `i64` values. |
| `a + b`, `a - b`, `a * b` (any floating) | promote to `SINGLE`/`DOUBLE` | `fadd`, `fsub`, `fmul` after widening to `f64`. |
| `a / b`                                  | integer or float             | `fdiv` in `f64`. |
| `a \ b`                                  | integer ranks                | `sdiv.chk0` on `i64`. |
| `a ^ b`                                  | any numeric                  | Call runtime helper `@rt_pow_f64_chkdom(a', b')` where inputs are widened to `f64`; helper enforces `DomainError`/`Overflow`. |
| `a MOD b`                                | integer ranks                | `srem.chk0` on `i64`. |
| `CDBL(x)`                                | any numeric                  | Ensure `f64`, returning `DOUBLE`. |
| `CINT(x)`                                | any numeric                  | Runtime call `@rt_cint_from_double(x', &ok)` returning `i64` storage or trapping through the lowering bridge when `ok` is false. |
| `CLNG(x)`                                | any numeric                  | Runtime call `@rt_clng_from_double(x', &ok)` returning `i64` storage or trapping through the lowering bridge when `ok` is false. |
| `CSNG(x)`                                | any numeric                  | Runtime call `@rt_csng_from_double(x', &ok)` returning `f64` storage or trapping through the lowering bridge when `ok` is false. |
| `FIX(x)`                                 | any numeric                  | Runtime call `@rt_fix_trunc(x')` implementing truncate-toward-zero for floating inputs. |
| `INT(x)`                                 | any numeric                  | For integers: no-op. For floats: floor in `f64`, returning the original semantic rank. |
| `ROUND(x)`                               | any numeric                  | Runtime call `@rt_round_even(x', ndigits)`. |
| `STR$(x)`                                | any numeric                  | Runtime call producing a `str`. |
| `VAL(s$)`                                | string                       | Runtime call producing `DOUBLE` (`f64`) or trapping on overflow. |

The ABI must ensure traps propagate as described and that the
round-to-nearest-even rule is observed for every conversion.
