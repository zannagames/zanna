---
status: active
audience: contributors
last-verified: 2026-08-17
---

# CODEMAP: IL Runtime

Runtime signature metadata (`src/il/runtime/`) for C ABI helpers.

## Overview

- **Total source files**: 48 (.hpp/.cpp/.def/.inc)
- **Subdirectories**: classes/, defs/, signatures/ — the modular `@summary`/`@details`
  definition fragments introduced by [ADR 0101](../../adr/0101-modular-runtime-definitions-and-documentation.md) live in `defs/`

## Definition Files

| File                            | Purpose                                                        |
|---------------------------------|----------------------------------------------------------------|
| `runtime.def`                   | X-macro table defining all runtime functions, classes, methods, properties, and aliases |
| `RuntimeSigs.def`               | Compact signature definitions for generated code               |
| `RuntimeSurfacePolicy.inc`      | X-macro surface-policy table (frontend-visible `Zanna.*` APIs + intentionally-internal headers); consumed by audit tests |

## Signature Registry

| File                            | Purpose                                                  |
|---------------------------------|----------------------------------------------------------|
| `HelperEffects.hpp`             | Effect tags for runtime helpers (side-effects, aliasing) |
| `RuntimeClassNames.hpp`         | Canonical runtime class name constants                   |
| `RuntimeNameMap.hpp`            | Name to runtime function mapping                         |
| `RuntimeOwnership.hpp`          | Runtime reference-ownership metadata for optimizer queries |
| `RuntimeSignatureParser.cpp`    | Parse compact ABI signature spellings impl               |
| `RuntimeSignatureParser.hpp`    | Parse compact ABI signature spellings                    |
| `RuntimeSignatures.cpp`         | Main registry implementation                             |
| `RuntimeSignatures.hpp`         | Main registry mapping helper names to IL signatures      |
| `RuntimeSignatures_Handlers.cpp`| Runtime signature handler implementations                |
| `RuntimeSignatures_Handlers.hpp`| Runtime signature handler declarations                   |
| `RuntimeSignaturesData.hpp`     | Static data bundles for signature catalog                |

## Runtime Classes (`classes/`)

| File                        | Purpose                                            |
|-----------------------------|----------------------------------------------------|
| `classes/RuntimeClasses.cpp`| Runtime class metadata implementation              |
| `classes/RuntimeClasses.hpp`| Runtime class metadata (StringBuilder, List, etc.) |

## Signature Categories (`signatures/`)

| File                               | Purpose                                      |
|------------------------------------|----------------------------------------------|
| `signatures/Registry.cpp`          | Registration entry points implementation     |
| `signatures/Registry.hpp`          | Registration entry points for category files |
| `signatures/Signatures_Arrays.cpp` | Array helper signatures                      |
| `signatures/Signatures_FileIO.cpp` | File I/O helper signatures                   |
| `signatures/Signatures_Math.cpp`   | Math function signatures                     |
| `signatures/Signatures_OOP.cpp`    | OOP runtime helper signatures                |
| `signatures/Signatures_Strings.cpp`| String operation signatures                  |

The debug signature registry is append-only for unique helper names, idempotent
for duplicate identical registrations, and rejects duplicate names with
conflicting ABI or effect metadata. Consumers that need effect facts should
prefer `RuntimeSignatures.hpp`; the category registry is for debug/audit
coverage.
