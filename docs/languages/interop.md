---
status: active
audience: public
last-verified: 2026-07-26
---

# Cross-Language Interop Guide

Zanna supports mixed-language projects where Zia and BASIC source files coexist
in a single project. One language provides the entry point (`main`/`start`),
while the other provides library functions that are called across the language
boundary.

This guide covers the syntax, build configuration, type compatibility, and
boolean conversion rules for cross-language interop.

---

## Overview

Both Zia and BASIC compile to the same IL (Intermediate Language) and share the
same runtime. Cross-language interop works by:

1. **Exporting** functions from one module (making them visible to other modules)
2. **Importing** functions into another module (declaring them without a body)
3. **Linking** the modules at the IL level before execution
4. **Generating boolean thunks** automatically when boolean representations differ

The VM and native backends see a single merged module — no multi-module loading
is needed.

---

## Zia Syntax

### Exporting a Function

Use `expose` before `func` at module scope to mark a function as exported:

```zia
// mathlib.zia
expose func factorial(n: Integer) -> Integer {
    if n <= 1 {
        return 1
    }
    return n * factorial(n - 1)
}
```

### Importing a Foreign Function

Use `foreign func` to declare a function defined in another module. Foreign
function declarations have no body:

```zia
// main.zia
foreign func Factorial(n: Integer) -> Integer

func start() {
    let result = Factorial(10)
    // result = 3628800
}
```

---

## BASIC Syntax

### Exporting a Function

Use the `EXPORT` keyword before `FUNCTION` or `SUB`:

```basic
' mathlib.bas
EXPORT FUNCTION Factorial(n AS LONG) AS LONG
    IF n <= 1 THEN
        Factorial = 1
    ELSE
        Factorial = n * Factorial(n - 1)
    END IF
END FUNCTION
```

### Importing a Foreign Function

Use `DECLARE FOREIGN` before `FUNCTION` or `SUB`. The declaration has no body
(no `END FUNCTION`):

```basic
' main.bas
DECLARE FOREIGN FUNCTION ZiaHelper(n AS LONG) AS LONG

PRINT ZiaHelper(42)
```

### Importing a Foreign Subroutine

```basic
DECLARE FOREIGN SUB InitSystem()

InitSystem()
```

---

## Project Configuration

Mixed-language projects require a `zanna.project` manifest with `lang mixed` and
an `entry` directive specifying which file provides the entry point.

### Manifest Format

```text
project MyMixedApp
lang mixed
entry main.zia
```

Or with a BASIC entry point:

```text
project MyMixedApp
lang mixed
entry main.bas
```

### Directory Layout

```text
my-project/
  zanna.project
  main.zia          <-- entry point (has start())
  mathlib.bas       <-- library (exports functions)
```

### How It Works

1. The `entry` file determines which language provides `main`
2. All `.zia` files are compiled together as the Zia module
3. All `.bas` files are compiled together as the BASIC module
4. The IL linker merges both modules into a single module
5. The merged module is verified and executed normally

---

## Symbol Names

Write the symbol the same way on both sides — `Factorial` in Zia and
`Factorial` in BASIC — and it resolves in either direction.

Zanna BASIC is case-insensitive and canonicalizes identifiers to upper case, so
a BASIC module emits `@FACTORIAL` where Zia emits `@Factorial`. The linker
resolves that difference: when an import does not match a definition exactly, it
retries ignoring case and binds a unique match
([ADR 0268](../adr/0268-cross-language-symbol-resolution.md)).

Two definitions differing only by case are reported as an ambiguous import
rather than resolved arbitrarily, so a case-sensitive frontend cannot rely on
case alone to distinguish two symbols that cross a module boundary.

---

## Type Compatibility

Zia and BASIC share the same IL type system. Most types are directly compatible:

| Zia Type   | BASIC Type     | IL Type | Compatible? |
|-----------|----------------|---------|-------------|
| `Integer` | `LONG`         | `i64`   | Yes         |
| `Number`  | `DOUBLE`       | `f64`   | Yes         |
| `String`  | `STRING`       | `str`   | Yes         |
| `Boolean` | `BOOLEAN`      | See below | Bridged  |

### The Boolean Bridge

Zia and BASIC represent booleans differently:

- **Zia**: `Boolean` compiles to `i1` (0 = false, 1 = true)
- **BASIC**: `BOOLEAN` compiles to `i64` (-1 = true, 0 = false)

When the linker detects a boolean mismatch between an export and its
corresponding import, it **automatically generates a conversion thunk**:

| Direction | Conversion | Opcode | Semantics |
|-----------|-----------|--------|-----------|
| `i1` -> `i64` | Zero-extend | `Zext1` | `true` (1) -> `1`, `false` (0) -> `0` |
| `i64` -> `i1` | Compare != 0 | `ICmpNe` | Any non-zero -> `true`, `0` -> `false` |

This means:
- BASIC's `-1` (true) correctly maps to Zia's `true`
- BASIC's `1` or any non-zero value also maps to Zia's `true`
- Zia's `true` maps to BASIC's `1` (not `-1`)

**Important**: Zia's `true` maps to `1`, not `-1`. If your BASIC code uses
bitwise `AND` on boolean values expecting `-1`, the interop result may differ.
Use comparison operators (`<>`, `=`) rather than bitwise operators on
cross-language boolean values.

---

## IL Linkage Model

At the IL level, functions and globals have a `Linkage` attribute:

| Linkage    | Meaning | Has Body? |
|-----------|---------|-----------|
| `Internal` | Module-private (default) | Yes |
| `Export`   | Defined here, visible to other modules | Yes |
| `Import`   | Declared here, defined elsewhere | No |

### IL Text Syntax

```text
func export @calculateScore(i64, i64) -> i64 { ... }
func import @BasicHelper(i64) -> i64
func @internalHelper(i64) -> i64 { ... }
```

The linkage keyword appears between `func` and the function name. Omitting it
defaults to `Internal` (backwards compatible with all existing `.il` files).

### Linking Algorithm

The IL module linker (`il::link::linkModules`) merges multiple modules:

1. **Entry detection**: Exactly one module must contain a function named `main`
2. **Import resolution**: Each `Import` function is matched to an `Export` or
   `Internal` function by name in another module
3. **Name collision handling**: `Internal` functions with the same name in
   different modules get a disambiguating prefix (`m1$`, `m2$`, etc.)
4. **Extern deduplication**: Shared runtime extern declarations are merged;
   signature mismatches are link errors
5. **Global merging**: String constants are deduplicated by value
6. **Init ordering**: Module init functions (`__zia_iface_init`,
   `__mod_init$oop`, etc.) are injected into the merged `main`
7. **Verification**: The merged module is verified before execution

---

## Limitations

- **Single entry point**: Only one module may provide `main` / `start()`
- **No circular imports**: Module A cannot import from B while B imports from A
  (the linker processes modules sequentially)
- **No class/value type sharing**: Object types cannot currently be shared
  across language boundaries. Use primitive types (`Integer`, `Number`, `String`)
  for cross-language function signatures
- **Boolean arithmetic caveat**: Zia's `true` zero-extends to `1`, not BASIC's
  `-1`. Code that relies on `true AND mask` patterns may see different results

---

## Example: Zia Main + BASIC Library

### `zanna.project`

```text
project FactorialDemo
lang mixed
entry main.zia
```

### `main.zia`

```zia
bind Zanna.Terminal;

foreign func Factorial(n: Integer) -> Integer

func start() {
    let result = Factorial(10)
    Say(toString(result))
}
```

### `mathlib.bas`

```basic
EXPORT FUNCTION Factorial(n AS LONG) AS LONG
    IF n <= 1 THEN
        Factorial = 1
    ELSE
        Factorial = n * Factorial(n - 1)
    END IF
END FUNCTION
```

### Running

```sh
zanna run .
# Output: 3628800
```

---

## See Also

- [ADR-0003: IL Linkage and Module Linking](../adr/0003-il-linkage-and-module-linking.md)
- [IL Guide: Functions and Calls](../il/il-guide.md)
- [Architecture Overview](../internals/architecture.md)
