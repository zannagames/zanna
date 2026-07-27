---
status: active
audience: contributors
last-verified: 2026-07-26
---

# CODEMAP: Frontend Common

Shared utilities (`src/frontends/common/`) used across all language frontends.

## Overview

- **Total source files**: 23 (.cpp/.hpp)

## Lexer Utilities

| File                    | Purpose                                      |
|-------------------------|----------------------------------------------|
| `CharUtils.hpp`         | Character classification helpers             |
| `EscapeSequences.hpp`   | Escape sequence processing (\\n, \\xNN, \\uXXXX) |
| `KeywordTable.hpp`      | Keyword lookup table infrastructure          |
| `LexerBase.hpp`         | Common lexer base class and utilities        |
| `NumberParsing.hpp`     | Numeric literal parsing                      |

## Parser Utilities

| File               | Purpose                            |
|--------------------|------------------------------------|
| `BlockManager.hpp` | Block scope management             |
| `ExprResult.hpp`   | Expression parsing result type     |
| `LoopContext.hpp`  | Loop nesting context tracking      |
| `ScopeTracker.hpp` | Scope entry/exit tracking          |

## Type Utilities

| File            | Purpose                            |
|-----------------|------------------------------------|
| `TypeUtils.hpp` | Common type manipulation utilities |

## Lowering Utilities

| File                     | Purpose                          |
|--------------------------|----------------------------------|
| `ConstantFolding.hpp`    | Compile-time constant evaluation |
| `InstructionEmitter.hpp` | Common IL instruction emission   |
| `NameMangler.hpp`        | Name mangling utilities          |

## String Utilities

| File              | Purpose                            |
|-------------------|------------------------------------|
| `StringHash.hpp`  | String hashing utilities           |
| `StringTable.hpp` | String interning for literals      |
| `StringUtils.hpp` | Common string manipulation helpers |

## Diagnostics

| File                       | Purpose                                        |
|----------------------------|------------------------------------------------|
| `DiagnosticFormatter.hpp`  | Source-line extraction, caret, severity display |
| `DiagnosticHelpers.hpp`    | Common diagnostic formatting helpers           |

## Runtime Registry

| File                  | Purpose                             |
|-----------------------|-------------------------------------|
| `RuntimeRegistry.cpp` | Runtime function registry impl      |
| `RuntimeRegistry.hpp` | Runtime function signature registry |

## General Utilities

| File         | Purpose                                          |
|--------------|--------------------------------------------------|
| `Common.hpp` | Shared type definitions and forward declarations |
