---
status: active
audience: contributors
last-verified: 2026-07-26
---

# CODEMAP: Support & Common

Shared support and infrastructure used across the toolchain.

Directories: `src/support/`, `src/common/`, `src/parse/`, `src/pass/`.

## Overview

- **Total source files**: 33 (.hpp/.cpp)
  - support/: 21 files
  - common/: 10 files
  - parse/: 1 file
  - pass/: 1 file

## Source Management (`src/support/`)

| File                  | Purpose                                         |
|-----------------------|-------------------------------------------------|
| `source_location.cpp` | Source location implementation                  |
| `source_location.hpp` | Source location value type (file, line, column) |
| `source_manager.cpp`  | Source file registration implementation         |
| `source_manager.hpp`  | Source file registration and path normalization |

## Diagnostics (`src/support/`)

| File                | Purpose                                             |
|---------------------|-----------------------------------------------------|
| `diag_capture.cpp`  | Diagnostic buffer capture implementation            |
| `diag_capture.hpp`  | Diagnostic buffer capture for tests                 |
| `diag_catalog.cpp`  | Diagnostic-code catalog lookup implementation       |
| `diag_catalog.def`  | Central diagnostic-code catalog (X-macro entries)   |
| `diag_catalog.hpp`  | Diagnostic-code catalog for zanna explain           |
| `diag_expected.cpp` | Expected/diagnostic wrapper implementation          |
| `diag_expected.hpp` | Expected/diagnostic wrapper for result-style errors |
| `diagnostics.cpp`   | Diagnostic engine implementation                    |
| `diagnostics.hpp`   | Diagnostic engine: collect, count, print messages   |

## Memory (`src/support/`)

| File        | Purpose                           |
|-------------|-----------------------------------|
| `arena.cpp` | Bump-pointer arena implementation |
| `arena.hpp` | Bump-pointer arena allocator      |

## String Interning (`src/support/`)

| File                  | Purpose                               |
|-----------------------|---------------------------------------|
| `string_interner.cpp` | String deduplication implementation   |
| `string_interner.hpp` | String deduplication and symbol table |
| `symbol.cpp`          | Interned symbol implementation        |
| `symbol.hpp`          | Interned symbol identifier type       |

## Configuration (`src/support/`)

| File              | Purpose                                 |
|-------------------|-----------------------------------------|
| `alignment.hpp`   | Alignment utilities                     |
| `options.hpp`     | Global compile-time options and toggles |
| `small_vector.hpp`| Small-buffer-optimized vector type      |

## Common Utilities (`src/common/`)

| File                 | Purpose                                                |
|----------------------|--------------------------------------------------------|
| `IntegerHelpers.hpp` | Integer helpers (width/signedness, overflow policies)  |
| `Mangle.cpp`         | Name mangling implementation                           |
| `Mangle.hpp`         | Name mangling helpers used by frontends/codegen        |
| `PlatformCapabilities.hpp` | Shared compile-time platform capability flags (preferred over raw `_WIN32`/`__APPLE__`/`__linux__` checks) |
| `RunProcess.cpp`     | Test helper to spawn subprocesses implementation       |
| `RunProcess.hpp`     | Test helper to spawn subprocesses with env/dir control |

## Parsing Helpers (`src/parse/`)

| File         | Purpose                                                             |
|--------------|---------------------------------------------------------------------|
| `Cursor.cpp` | Source cursor utilities; C header at `include/zanna/parse/Cursor.h` |

## Pass Framework (`src/pass/`)

| File              | Purpose                                                                         |
|-------------------|---------------------------------------------------------------------------------|
| `PassManager.cpp` | Generic pass manager facade; public API at `include/zanna/pass/PassManager.hpp` |
