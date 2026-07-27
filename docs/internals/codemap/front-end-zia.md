---
status: active
audience: contributors
last-verified: 2026-07-26
---

# CODEMAP: Zia Frontend

The Zia frontend (`src/frontends/zia/`) compiles Zia source to IL.

Zia is Zanna's native language with classes, structs, generics, lambdas, and imports.

## Overview

- **Total source files**: 89 (.hpp/.cpp)

## Core Infrastructure

| File               | Purpose                                       |
|--------------------|-----------------------------------------------|
| `Compiler.cpp`     | Main compiler pipeline implementation         |
| `Compiler.hpp`     | Main compiler pipeline orchestration          |
| `Options.hpp`      | Compiler options (optimization levels)        |
| `RuntimeNames.hpp` | Runtime function name constants (~1400 funcs) |

## Lexer and Parser

| File                | Purpose                                   |
|---------------------|-------------------------------------------|
| `Lexer.cpp`         | Tokenizer implementation                  |
| `Lexer.hpp`         | Tokenizer for Zia source                  |
| `Parser.hpp`        | Recursive-descent parser core             |
| `Parser_Decl.cpp`   | Declaration parsing (func, class, struct) |
| `Parser_Expr.cpp`   | Expression parsing                        |
| `Parser_Expr_Pattern.cpp` | Pattern expression parsing (match arms) |
| `Parser_Expr_Primary.cpp` | Primary expression parsing              |
| `Parser_Stmt.cpp`   | Statement parsing                         |
| `Parser_Tokens.cpp` | Token handling utilities                  |
| `Parser_Type.cpp`   | Type declaration parsing                  |
| `Token.hpp`         | Token types and representation            |

## AST

| File            | Purpose                      |
|-----------------|------------------------------|
| `AST.hpp`       | Main AST include             |
| `AST_Decl.hpp`  | Declaration node definitions |
| `AST_Expr.hpp`  | Expression node definitions  |
| `AST_Fwd.hpp`   | Forward declarations         |
| `AST_Stmt.hpp`  | Statement node definitions   |
| `AST_Types.hpp` | Type representation nodes    |

## Types

| File         | Purpose                                                          |
|--------------|------------------------------------------------------------------|
| `Types.cpp`  | Type system implementation                                       |
| `Types.hpp`  | Type system (primitives, classes, collections, optionals, funcs) |

## Semantic Analysis

| File                     | Purpose                                                           |
|--------------------------|-------------------------------------------------------------------|
| `Sema.cpp`               | Main semantic analyzer implementation                             |
| `Sema.hpp`               | Main semantic analyzer and symbol table                           |
| `Sema_Decl.cpp`          | Declaration analysis                                              |
| `Sema_Expr.cpp`          | Expression type checking                                          |
| `Sema_Expr_Advanced.cpp` | Advanced expression analysis (index, field, optional chain, etc.) |
| `Sema_Expr_Call.cpp`     | Call expression analysis and collection method resolution         |
| `Sema_Expr_Ops.cpp`      | Operator expression analysis (binary, unary, ternary)             |
| `Sema_Generics.cpp`      | Generic type and function support                                 |
| `Sema_Runtime.cpp`       | Runtime function registration                                     |
| `Sema_Stmt.cpp`          | Statement analysis                                                |
| `Sema_TypeResolution.cpp`| Type resolution and closure capture collection                    |

Recent correctness notes:

- `Sema_Stmt.cpp` analyzes unreachable statements after reporting the first unreachable warning, so later semantic errors are still surfaced.
- `Sema_Stmt.cpp` requires guard `else` blocks to exit on every path and validates tuple `for a, b in ...` bindings against iterable pairs or 2-element tuples.
- `Sema_Expr_Advanced.cpp` rejects direct member access on `Optional[T]` unless flow narrowing, force unwrap, or `?.` is used. Null-check narrowing now supports dotted field paths such as `self.child`.
- `Sema_Expr_Call.cpp` enforces arity and element/key/value typing for List, Set, Map, and built-in String methods. `Map.keys()` and `Map.values()` return typed `Seq` values.
- Async functions are typed as `Zanna.Threads.Future[T]`, allowing `await` to recover payload type through future variables.

## Import Resolution

| File                 | Purpose                           |
|----------------------|-----------------------------------|
| `ImportResolver.cpp` | Module import resolution impl     |
| `ImportResolver.hpp` | Module import resolution          |
| `RuntimeAdapter.cpp` | Type conversion bridge impl (IL types to Zia types) |
| `RuntimeAdapter.hpp` | Type conversion utilities for RuntimeRegistry |

## Semantic Analysis Support (`sema/`)

| File                  | Purpose                                        |
|-----------------------|------------------------------------------------|
| `sema/SemaTypes.hpp`  | Symbol, ScopedSymbol, and Scope type definitions (extracted from Sema.hpp) |

## IL Lowering

| File                          | Purpose                                                          |
|-------------------------------|------------------------------------------------------------------|
| `Lowerer.cpp`                 | Main lowering coordinator impl                                   |
| `Lowerer.hpp`                 | Main lowering coordinator and BlockManager                       |
| `LowererTypes.hpp`            | Type layout structs (FieldLayout, StructTypeInfo, ClassTypeInfo, InterfaceTypeInfo) |
| `LowererTypeLayout.hpp`       | Type layout computation and registry class                       |
| `LowererTypeLayout.cpp`       | Type layout registration implementation                          |
| `Lowerer_Decl.cpp`            | Declaration lowering (functions, types)                          |
| `Lowerer_Dispatch.cpp`        | Method dispatch lowering                                         |
| `Lowerer_Emit.cpp`            | IL emission helpers (box/unbox, GEP, etc.)                       |
| `Lowerer_Expr.cpp`            | Expression lowering main dispatch                                |
| `Lowerer_Expr_Binary.cpp`     | Binary operator lowering                                         |
| `Lowerer_Expr_Call.cpp`       | Function/method call lowering                                    |
| `Lowerer_Expr_Collections.cpp`| List/Map/Tuple literal and index lowering                        |
| `Lowerer_Expr_Complex.cpp`    | Complex expression lowering (field, new, coalesce, lambda, etc.) |
| `Lowerer_Expr_Literals.cpp`   | Literal expression lowering                                      |
| `Lowerer_Expr_Match.cpp`      | Match expression lowering                                        |
| `Lowerer_Expr_Method.cpp`     | Method call and type construction lowering                       |
| `Lowerer_Decl_Functions.cpp`  | Function declaration lowering (split from Lowerer_Decl)          |
| `Lowerer_Decl_Types.cpp`     | Type declaration lowering (split from Lowerer_Decl)              |
| `Lowerer_Expr_Lambda.cpp`    | Lambda/closure expression lowering                               |
| `Lowerer_Expr_Optional.cpp`  | Optional type expression lowering                                |
| `Lowerer_Stmt_EH.cpp`        | Exception handling statement lowering                            |
| `Lowerer_Stmt.cpp`            | Statement lowering                                               |
| `LowererSymbolTable.hpp`     | Symbol table for the lowerer                                     |
| `LowererBinaryOperatorLowerer.hpp` | Helper: lowering non-assignment binary operators           |
| `LowererBinaryOperatorLowerer.cpp` | Binary-operator lowering helper impl                       |
| `LowererCallArgumentLowerer.hpp`   | Helper: convert call/new arguments into IL argument vectors |
| `LowererCallArgumentLowerer.cpp`   | Call-argument lowering helper impl                         |
| `LowererCollectionLowerer.hpp`     | Helper: list/set/map literal, tuple, and index lowering    |
| `LowererCollectionLowerer.cpp`     | Collection lowering helper impl                            |

Recent correctness notes:

- Primitive and struct `Optional[T]` payloads use nullable boxed pointers; optional lowering performs uniform null checks instead of a separate flag/value representation.
- Map index lowering emits a `Map.Has` guard and traps on missing keys before unboxing a `Map.Get` result.
- `Set.remove()` lowers to the runtime Boolean result, matching semantic return typing.
- Inline aggregate layout is centralized in `Lowerer_Emit.cpp`: structs, tuples, and fixed arrays use semantic size/alignment helpers for field layout, tuple offsets, fixed-array strides, and nested copy/zero/store operations. Do not reintroduce pointer-sized `index * 8` assumptions for these values.

## Warnings

| File                  | Purpose                                |
|-----------------------|----------------------------------------|
| `Warnings.hpp`        | Warning system declarations            |
| `Warnings.cpp`        | Warning emission implementation        |
| `WarningSuppressions.hpp` | Warning suppression infrastructure |

## IDE / Completion

| File                  | Purpose                                |
|-----------------------|----------------------------------------|
| `ZiaCompletion.hpp`   | Code completion engine declarations    |
| `ZiaCompletion.cpp`   | Code completion engine implementation  |
| `ZiaAnalysis.hpp`     | Analysis facade for IDE integration    |
| `ZiaLocationScope.hpp`| Location-based scope tracking          |
| `ZiaLocationScope.cpp`| Location scope implementation          |
| `Sema_Completion.cpp` | Semantic analysis for completions      |
| `rt_zia_completion.cpp`| Runtime completion bridge (C API)     |
| `rt_zia_highlight.cpp`| Runtime syntax-highlighter bridge (C API for the GUI editor) |

## Debugging and Inspection

| File                | Purpose                          |
|---------------------|----------------------------------|
| `ZiaAstPrinter.hpp` | AST printer declaration          |
| `ZiaAstPrinter.cpp` | AST printer (indented tree dump) |

CLI flags for inspecting each pipeline stage (see `docs/tools/debugging.md` §8):

| Flag | Stage |
|------|-------|
| `--dump-tokens` | Token stream from Lexer |
| `--dump-ast` | AST after Parser |
| `--dump-sema-ast` | AST after Sema |
| `--dump-il` | IL after Lowerer |
| `--dump-il-passes` | IL before/after each optimizer pass |
| `--dump-il-opt` | IL after full optimization |

Programmatic equivalents are in `CompilerOptions` (`Options.hpp`).

## Key Features

- Full type inference with Unknown type propagation
- Class and Struct type support with vtables
- Generic types (List<T>, Map<K,V>)
- Lambda expressions with closures
- Pattern matching with match expressions
- Optional types with null safety
- Import system for multi-file projects
