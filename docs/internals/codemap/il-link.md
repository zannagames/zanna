---
status: active
audience: contributors
last-verified: 2026-07-26
---

# CODEMAP: IL Link

Cross-language module linker (`src/il/link/`) that merges multiple IL modules into a single module, resolving export/import linkage and generating boolean conversion thunks.

## Overview

- **Total source files**: 4 (.hpp/.cpp)
- **Namespace**: `il::link`
- **CMake target**: `il_link`

## Module Linker

| File               | Purpose                                                     |
|--------------------|-------------------------------------------------------------|
| `ModuleLinker.hpp` | Declares `linkModules()` and `LinkResult` struct            |
| `ModuleLinker.cpp` | Implements the multi-module merge algorithm                 |

### `LinkResult` (`ModuleLinker.hpp`)

Result of linking multiple IL modules.

**Fields:**
- `module` — `il::core::Module`: the merged module (valid only when `errors` is empty)
- `errors` — `std::vector<std::string>`: diagnostic messages (empty on success)

**Methods:**
- `succeeded() const` — `bool`: returns true if `errors` is empty

### `linkModules()` (`ModuleLinker.hpp`)

```cpp
LinkResult linkModules(std::vector<il::core::Module> modules);
```

Merges multiple IL modules into a single module. The algorithm:

1. **Entry detection**: Exactly one module must contain a function named `main`
2. **Import resolution**: Each `Import`-linkage function is matched to an `Export` or `Internal` function by name in another module
3. **Name collision handling**: `Internal` functions from non-entry modules get a disambiguating prefix (`m1$`, `m2$`, etc.)
4. **Extern deduplication**: Shared runtime extern declarations are merged; signature mismatches are link errors
5. **Global merging**: String constants are deduplicated by value
6. **Init ordering**: Module init functions (`__zia_iface_init`, `__mod_init$oop`, etc.) are injected into the merged `main`
7. **Verification**: `Import` stubs are dropped from the merged module

**Invariants:**
- Exactly one input module may contain `main`
- Every `Import` must resolve to a matching definition
- Extern signatures must agree across modules

## Boolean Interop Thunks

| File               | Purpose                                                    |
|--------------------|------------------------------------------------------------|
| `InteropThunks.hpp`| Declares `generateBooleanThunks()` and `ThunkInfo` struct  |
| `InteropThunks.cpp`| Implements boolean mismatch detection and thunk generation |

### `ThunkInfo` (`InteropThunks.hpp`)

Information about a generated boolean conversion thunk.

**Fields:**
- `thunkName` — `std::string`: name of the thunk function (e.g., `"isReady$bool_thunk"`)
- `targetName` — `std::string`: name of the original target function
- `thunk` — `il::core::Function`: the generated thunk function body

### `generateBooleanThunks()` (`InteropThunks.hpp`)

```cpp
std::vector<ThunkInfo> generateBooleanThunks(const il::core::Module &importModule,
                                             const il::core::Module &exportModule);
```

Scans for boolean type mismatches between Import and Export function pairs. When found, generates wrapper functions that perform the conversion:

| Direction     | Conversion    | Opcode   | Semantics                                   |
|---------------|---------------|----------|---------------------------------------------|
| `i1` → `i64`  | Zero-extend   | `Zext1`  | `true` (1) → `1`, `false` (0) → `0`       |
| `i64` → `i1`  | Compare ≠ 0   | `ICmpNe` | Any non-zero → `true`, `0` → `false`       |

**Design decisions:**
- Uses `Zext1` (not sign-extend), so Zia's `true` maps to `1`, not `-1`
- Uses `ICmpNe` (not `Trunc`), so BASIC's `-1` AND `1` AND any non-zero all map to `i1=1`

## Dependencies

- `il::core` — Module, Function, BasicBlock, Instr, Linkage, Type, Value, Opcode
- Used by: `src/tools/zanna/cmd_run.cpp` (mixed-language compilation pipeline)

## Related

- [Cross-Language Interop Guide](../../languages/interop.md)
- [ADR-0003: IL Linkage and Module Linking](../../adr/0003-il-linkage-and-module-linking.md)
- [IL Core: Linkage](il-core.md#linkage)
