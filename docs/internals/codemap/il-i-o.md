---
status: active
audience: contributors
last-verified: 2026-08-17
---

# CODEMAP: IL I/O

Text parsing and serialization (`src/il/io/`) for IL modules.

## Overview

- **Total source files**: 30 (.hpp/.cpp) across `src/il/io/` (22 files) and `src/il/internal/io/` (8 internal headers)

## Parser Core

| File                | Purpose                                              |
|---------------------|------------------------------------------------------|
| `Parser.cpp`        | Top-level parser implementation                      |
| `Parser.hpp`        | Top-level parser facade and entry point              |
| `ParserState.cpp`   | Mutable parser context constructor                   |
| `ParserUtil.cpp`    | Lexical helpers: trim, tokenize, parse literals      |
| `ModuleParser.cpp`  | Module-level directives: il, extern, global, func    |
| `TypeParser.cpp`    | Type mnemonic to Type object translation             |

## Function Parsing

| File                           | Purpose                                      |
|--------------------------------|----------------------------------------------|
| `FunctionParser.cpp`           | Function body parsing: headers, blocks       |
| `FunctionParser_Body.cpp`      | Function body block and instruction parsing  |
| `FunctionParser_Prototype.cpp` | Function prototype/signature parsing         |
| `InstrParser.cpp`              | Individual instruction line parsing          |

## Operand Parsing

| File                          | Purpose                                          |
|-------------------------------|--------------------------------------------------|
| `OperandParse_Const.cpp`      | Constant literal operand parsing                 |
| `OperandParse_Label.cpp`      | Branch label operand parsing                     |
| `OperandParse_Type.cpp`       | Type literal operand parsing                     |
| `OperandParse_Value.cpp`      | Value operand parsing dispatcher                 |
| `OperandParse_ValueDetail.cpp`| Token-to-Value decoding                          |
| `OperandParser.cpp`           | Legacy monolithic operand parser                 |

## Serializer

| File              | Purpose                                         |
|-------------------|-------------------------------------------------|
| `FormatUtils.cpp` | Locale-independent integer/float formatting     |
| `Serializer.cpp`  | Module to text emission implementation          |
| `Serializer.hpp`  | Module to text emission with canonical ordering |
| `StringEscape.cpp`| String escape/unescape implementation           |
| `StringEscape.hpp`| String escape/unescape for IL literals          |

## Internal Headers (`src/il/internal/io/`)

These headers are consumed by the `src/il/io/` translation units and are not part of the public API.

| File                                      | Purpose                                                     |
|-------------------------------------------|-------------------------------------------------------------|
| `FunctionParser.hpp`                      | Function parser declarations                                |
| `FunctionParser_Internal.hpp`             | Internal function parser helpers                            |
| `InstrParser.hpp`                         | Instruction parser declarations                             |
| `ModuleParser.hpp`                        | Module parser declarations                                  |
| `OperandParser.hpp`                       | Operand parser declarations                                 |
| `ParserState.hpp`                         | Parser state: module, function, block refs, SSA bookkeeping |
| `ParserUtil.hpp`                          | Lexical helper declarations                                 |
| `TypeParser.hpp`                          | Type parser declarations                                    |
