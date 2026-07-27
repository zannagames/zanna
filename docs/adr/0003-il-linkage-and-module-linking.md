---
status: active
audience: contributors
last-verified: 2026-06-27
---

# ADR 0003: IL Linkage and Module Linking

## Status

Accepted; implemented and verified against source/tests on 2026-06-27.

## Context

Zanna has two frontend languages (Zia and BASIC) that compile to the same IL and
share the same runtime. This ADR records the linkage annotations and module
linking algorithm used for cross-language interop.

### Pre-Implementation Limitations

- `il::core::Function` has no linkage or visibility metadata
- Both frontends emit a function named `main`, causing collisions if merged
- `il::core::Module` is self-contained with no import/export mechanism
- The VM and codegen backends process exactly one module
- Boolean representation differs: Zia uses `i1` (0/1), BASIC uses `i64` (-1/0)

## Decision

### 1. Add `Linkage` enum to IL core types

A new `Linkage` enum with three values:
- `Internal` (default) -- visible only within the defining module
- `Export` -- defined here, visible to other modules
- `Import` -- declared here, defined in another module (no body)

Added as a field to `Function` and `Global`, defaulting to `Internal` for full
backwards compatibility.

### 2. IL text syntax

```llvm
func export @name(...) -> type { ... }   // Export linkage
func import @name(...) -> type           // Import linkage (no body)
func @name(...) -> type { ... }          // Internal (default, backwards compat)
```

The keywords `export` and `import` appear between `func` and the function name.
Omitting the keyword implies `Internal`.

### 3. Module linker

A new `il::link` subsystem merges multiple IL modules into one:
- Exactly one module provides `main` (the entry module)
- Import declarations are resolved by name and signature against matching exports
  or a definition in the entry module
- Internal functions from different modules get disambiguating prefixes
- Externs are deduplicated; signature mismatches are link errors
- Non-entry functions carrying the explicit `[module_init]` attribute are
  called before user entry code. Names are never interpreted as initializer
  metadata.
- Boolean type mismatches (i1 vs i64) are bridged via auto-generated thunks

The linker runs the same validation path for single-module and multi-module
links. Duplicate exports are rejected, unresolved imports are errors even in a
single input module, and import/export signatures must match exactly unless the
only differences are supported boolean representation conversions. Rename maps
are scoped to the module that owns the reference, so same-named internal helpers
in different modules do not rewrite each other's calls. Global-name collisions
are likewise rewritten inside the owning module before merge.

Global imports follow the same resolution model: an import resolves to a
matching exported global or a definition in the entry module, its primitive
type and const qualification must match, and the declaration stub is omitted
from the linked module. Unresolved or duplicate global exports are link errors.
The final function/global/extern symbol namespace is checked for ambiguity.

### 4. Verifier changes

Import-linkage functions are valid call targets (like externs but with
IL-level type information). They must have no body (empty blocks vector).

## Consequences

- Existing single-language projects are unaffected (all defaults to Internal)
- Existing `.il` files parse correctly (no linkage keyword = Internal)
- The VM and codegen backends require no changes (they see a single merged module)
- Frontends gain `expose`/`foreign` (Zia) and `EXPORT`/`DECLARE FOREIGN` (BASIC)
- Mixed-language projects become possible via the project manifest

## Implementation Status

Verified on 2026-06-27:

- `src/il/core/Linkage.hpp` defines `Internal`, `Export`, and `Import`; `Function`
  and `Global` default to `Internal`.
- `src/il/io/FunctionParser_Prototype.cpp`, `src/il/io/ModuleParser.cpp`, and
  `src/il/io/Serializer.cpp` parse and print function/global linkage.
- `src/il/verify/FunctionVerifier.cpp` and `src/il/verify/GlobalVerifier.cpp`
  validate import-linkage declarations.
- `src/il/link/ModuleLinker.cpp` resolves imports, rejects duplicate exports and
  unresolved imports, prefixes internal collisions, deduplicates externs, rewrites
  function/global references, and injects non-entry module init calls.
- `src/il/link/InteropThunks.cpp` generates `i1`/`i64` boolean conversion thunks.
- `Function::moduleInitializer` round-trips as `[module_init]`; the linker uses
  only that marker when injecting startup calls.
- `src/tools/zanna/cmd_run.cpp` links entry and library project modules for mixed
  projects before running the optimizer/VM path.
- Zia lowers `foreign` functions to `Import` and public/exposed functions to
  `Export`; BASIC lowers `DECLARE FOREIGN` to `Import` and `EXPORT` to `Export`.
- Focused tests passed: `test_il_linkage_roundtrip`, `test_il_module_linker`,
  `test_il_interop_integration`, and `test_il_interop_thunks`.
