---
status: active
audience: contributors
last-verified: 2026-07-26
---

# CODEMAP: BASIC Frontend

The BASIC frontend (`src/frontends/basic/`) compiles Zanna BASIC source to IL.

## Overview

- **Total source files**: 280 (.hpp/.cpp/.def/.inc)
- **Subdirectories**: ast/, builtins/, constfold/, detail/, diag/, lower/, lower/builtins/, lower/common/, lower/detail/, lower/oop/, lowerer/, passes/, print/, sem/, types/

## Core Infrastructure

| File                          | Purpose                                                |
|-------------------------------|--------------------------------------------------------|
| `BasicCompiler.cpp`           | Main compiler pipeline: parse → fold → analyze → lower |
| `BasicCompiler.hpp`           | Compiler class declarations                            |
| `BasicDiagnosticMessages.hpp` | Diagnostic message templates                           |
| `BasicSymbolQuery.cpp`        | Symbol lookup and query implementation                 |
| `BasicSymbolQuery.hpp`        | Symbol lookup and query utilities                      |
| `BasicTypes.hpp`              | BASIC type definitions and mappings                    |
| `Diag.cpp`                    | Diagnostic infrastructure implementation               |
| `Diag.hpp`                    | Diagnostic infrastructure                              |
| `DiagnosticCodes.hpp`         | Error and warning code definitions                     |
| `DiagnosticEmitter.cpp`       | Diagnostic output implementation                       |
| `DiagnosticEmitter.hpp`       | Diagnostic output and formatting                       |
| `Options.cpp`                 | Compiler options implementation                        |
| `Options.hpp`                 | Compiler options and configuration                     |
| `RuntimeNames.hpp`            | Runtime function name constants                        |

## Lexer and Parser

| File                           | Purpose                                   |
|--------------------------------|-------------------------------------------|
| `Lexer.cpp`                    | Tokenizer implementation                  |
| `Lexer.hpp`                    | Tokenizer for BASIC source                |
| `Parser.cpp`                   | Recursive-descent parser core             |
| `Parser.hpp`                   | Parser class declarations                 |
| `Parser_Expr.cpp`              | Expression parsing                        |
| `Parser_Stmt.cpp`              | Statement parsing dispatch                |
| `Parser_Stmt_Control.cpp`      | Control flow parsing (IF, FOR, WHILE)     |
| `Parser_Stmt_ControlHelpers.hpp`| Control flow parsing helpers             |
| `Parser_Stmt_Core.cpp`         | Core statement parsing (LET, PRINT, etc.) |
| `Parser_Stmt_If.cpp`           | IF/THEN/ELSE parsing                      |
| `Parser_Stmt_IO.cpp`           | I/O statement parsing                     |
| `Parser_Stmt_Jump.cpp`         | Jump statement parsing (GOTO, GOSUB)      |
| `Parser_Stmt_Loop.cpp`         | Loop statement parsing                    |
| `Parser_Stmt_OOP.cpp`          | OOP construct parsing                     |
| `Parser_Stmt_Runtime.cpp`      | Runtime statement parsing                 |
| `Parser_Stmt_Select.cpp`       | SELECT CASE parsing                       |
| `Parser_Token.cpp`             | Token handling implementation             |
| `Parser_Token.hpp`             | Token handling utilities                  |
| `Token.cpp`                    | Token implementation                      |
| `Token.hpp`                    | Token types and representation            |
| `TokenKinds.def`               | Macro-based token kind enumeration        |

## Symbol Tables

| File                          | Purpose                                   |
|-------------------------------|-------------------------------------------|
| `ProcedureSymbolTracker.cpp`  | Procedure symbol tracking impl            |
| `ProcedureSymbolTracker.hpp`  | Procedure symbol tracking                 |
| `ProcRegistry.cpp`            | Procedure registration implementation     |
| `ProcRegistry.hpp`            | Procedure registration and lookup         |
| `StringTable.cpp`             | String literal interning implementation   |
| `StringTable.hpp`             | String literal interning table            |
| `SymbolTable.cpp`             | Variable and procedure symbol table impl  |
| `SymbolTable.hpp`             | Variable and procedure symbol table       |

## AST (`ast/`)

| File                   | Purpose                                         |
|------------------------|-------------------------------------------------|
| `AST.cpp`              | Main AST implementation                         |
| `AST.hpp`              | Main AST definitions and visitor accept methods |
| `ast/DeclNodes.hpp`    | Declaration nodes                               |
| `ast/ExprNodes.hpp`    | Expression node definitions                     |
| `ast/NodeFwd.hpp`      | Forward declarations                            |
| `ast/StmtBase.hpp`     | Statement base classes                          |
| `ast/StmtControl.hpp`  | Control flow statement nodes                    |
| `ast/StmtDecl.hpp`     | Declaration statement nodes                     |
| `ast/StmtExpr.hpp`     | Expression statement nodes                      |
| `ast/StmtNodes.hpp`    | Statement node definitions                      |
| `ast/StmtNodesAll.hpp` | Aggregated statement includes                   |
| `AstPrint_Expr.cpp`    | Expression printing                             |
| `AstPrint_Stmt.cpp`    | Statement printing                              |
| `AstPrinter.cpp`       | AST pretty-printing implementation              |
| `AstPrinter.hpp`       | AST pretty-printing                             |
| `ASTUtils.hpp`         | AST utility functions                           |
| `AstWalker.hpp`        | AST traversal utilities                         |
| `AstWalkerUtils.cpp`   | AST walker helper implementation                |
| `AstWalkerUtils.hpp`   | AST walker helper functions                     |

## Semantic Analysis (`sem/`)

| File                                   | Purpose                              |
|----------------------------------------|--------------------------------------|
| `SemanticAnalyzer.cpp`                 | Main semantic analyzer impl          |
| `SemanticAnalyzer.hpp`                 | Main semantic analyzer               |
| `SemanticAnalyzer_Builtins.cpp`        | Builtin function analysis            |
| `SemanticAnalyzer_Exprs.cpp`           | Expression analysis                  |
| `SemanticAnalyzer_Internal.hpp`        | Internal analyzer helpers            |
| `SemanticAnalyzer_Namespace.cpp`       | Namespace resolution                 |
| `SemanticAnalyzer_Procs.cpp`           | Procedure analysis                   |
| `SemanticAnalyzer_Stmts.cpp`           | Statement analysis dispatch          |
| `SemanticAnalyzer_Stmts_Control.cpp`   | Control flow analysis impl           |
| `SemanticAnalyzer_Stmts_Control.hpp`   | Control flow analysis                |
| `SemanticAnalyzer_Stmts_IO.cpp`        | I/O statement analysis impl          |
| `SemanticAnalyzer_Stmts_IO.hpp`        | I/O statement analysis               |
| `SemanticAnalyzer_Stmts_Runtime.cpp`   | Runtime statement analysis impl      |
| `SemanticAnalyzer_Stmts_Runtime.hpp`   | Runtime statement analysis           |
| `SemanticAnalyzer_Stmts_Shared.cpp`    | Shared statement analysis impl       |
| `SemanticAnalyzer_Stmts_Shared.hpp`    | Shared statement analysis            |
| `SemanticDiagnostics.cpp`              | Semantic error reporting impl        |
| `SemanticDiagnostics.hpp`              | Semantic error reporting             |
| `SemanticDiagUtil.hpp`                 | Semantic diagnostic utilities        |
| `sem/Check_Common.hpp`                 | Common semantic check utilities      |
| `sem/Check_Expr_Array.cpp`             | Array expression semantic checks     |
| `sem/Check_Expr_Binary.cpp`            | Binary expression semantic checks    |
| `sem/Check_Expr_Call.cpp`              | Call expression semantic checks      |
| `sem/Check_Expr_Unary.cpp`             | Unary expression semantic checks     |
| `sem/Check_Expr_Var.cpp`               | Variable expression semantic checks  |
| `sem/Check_If.cpp`                     | IF statement checks                  |
| `sem/Check_Jumps.cpp`                  | Jump statement checks                |
| `sem/Check_Loops.cpp`                  | Loop statement checks                |
| `sem/Check_Select.cpp`                 | SELECT CASE checks                   |
| `sem/Check_SelectDetail.hpp`           | SELECT CASE check details            |
| `sem/NamespaceRegistry.cpp`            | Namespace management impl            |
| `sem/NamespaceRegistry.hpp`            | Namespace management                 |
| `sem/OverloadResolution.cpp`           | Method overload resolution impl      |
| `sem/OverloadResolution.hpp`           | Method overload resolution           |
| `sem/RegistryBuilder.cpp`              | Registry construction impl           |
| `sem/RegistryBuilder.hpp`              | Registry construction                |
| `sem/RuntimeMethodIndex.cpp`           | Runtime method lookup impl           |
| `sem/RuntimeMethodIndex.hpp`           | Runtime method lookup                |
| `sem/RuntimePropertyIndex.cpp`         | Runtime property lookup impl         |
| `sem/RuntimePropertyIndex.hpp`         | Runtime property lookup              |
| `sem/TypeRegistry.cpp`                 | Type registration implementation     |
| `sem/TypeRegistry.hpp`                 | Type registration and lookup         |
| `sem/TypeResolver.cpp`                 | Type resolution implementation       |
| `sem/TypeResolver.hpp`                 | Type resolution                      |
| `sem/UsingContext.cpp`                 | USING directive implementation       |
| `sem/UsingContext.hpp`                 | USING directive handling             |

Recent correctness notes:

- `sem/Check_Jumps.cpp` rejects top-level `RETURN` unless the main body contains a `GOSUB`; `RETURN` with a value remains procedure-only.
- `sem/Check_Loops.cpp` validates `EXIT SUB`/`EXIT FUNCTION` against the active procedure kind, checks numeric `FOR` counters/start/end/step expressions, and checks `FOR EACH` element variables against array element types.
- `SemanticAnalyzer_Stmts_Control.cpp` semantically analyzes `FINALLY` bodies and requires `USING` initializers to produce object/resource values.
- `SemanticAnalyzer_Procs.cpp` proves function-name implicit returns per control path instead of accepting a single assignment anywhere in the function.

## OOP Support

| File                              | Purpose                                  |
|-----------------------------------|------------------------------------------|
| `detail/Semantic_OOP_Internal.hpp`| Internal OOP semantic declarations       |
| `NameMangler_OOP.hpp`             | OOP name mangling                        |
| `OopIndex.cpp`                    | Class/interface metadata impl            |
| `OopIndex.hpp`                    | Class/interface metadata storage         |
| `OopLoweringContext.cpp`          | OOP lowering state impl                  |
| `OopLoweringContext.hpp`          | OOP lowering state management            |
| `Semantic_OOP.cpp`                | OOP semantic analysis impl               |
| `Semantic_OOP.hpp`                | OOP semantic analysis and index building |
| `Semantic_OOP_Builder.cpp`        | OOP builder utilities                    |
| `Semantic_OOP_Helpers.cpp`        | OOP helper functions                     |

## OOP Lowering (`lower/oop/`)

| File                                    | Purpose                            |
|-----------------------------------------|------------------------------------|
| `lower/oop/Lower_OOP_Alloc.cpp`         | Object allocation lowering         |
| `lower/oop/Lower_OOP_Emit.cpp`          | OOP IL emission                    |
| `lower/oop/Lower_OOP_Expr.cpp`          | OOP expression lowering            |
| `lower/oop/Lower_OOP_Helpers.cpp`       | OOP lowering utilities             |
| `lower/oop/Lower_OOP_Internal.hpp`      | Internal OOP lowering declarations |
| `lower/oop/Lower_OOP_MemberAccess.cpp`  | Member access lowering             |
| `lower/oop/Lower_OOP_MethodCall.cpp`    | Method call lowering               |
| `lower/oop/Lower_OOP_RuntimeHelpers.cpp`| OOP runtime helper lowering        |
| `lower/oop/Lower_OOP_RuntimeHelpers.hpp`| OOP runtime helper declarations    |
| `lower/oop/Lower_OOP_Scan.cpp`          | OOP pre-scan phase                 |
| `lower/oop/Lower_OOP_Stmt.cpp`          | OOP statement lowering             |
| `lower/oop/MethodDispatchHelpers.cpp`   | Method dispatch implementation     |
| `lower/oop/MethodDispatchHelpers.hpp`   | Method dispatch utilities          |

## IL Lowering Core

| File                        | Purpose                              |
|-----------------------------|--------------------------------------|
| `Lowerer.cpp`               | Main lowering coordinator impl       |
| `Lowerer.hpp`               | Main lowering coordinator            |
| `LowererContext.hpp`        | Lowering context and state           |
| `LowererFwd.hpp`            | Forward declarations                 |
| `LowererRuntimeHelpers.hpp` | Runtime helper declarations          |
| `LowererTypes.hpp`          | Lowerer type definitions             |
| `LoweringContext.cpp`       | Extended lowering context impl       |
| `LoweringContext.hpp`       | Extended lowering context            |
| `LoweringPipeline.hpp`      | Lowering pipeline definition         |
| `lowerer/LowererDetailAccess.hpp` | DetailAccess inner-class accessor: narrow public surface for Lowerer internals |
| `lowerer/LowererRuntimeRequirements.hpp` | Runtime requirement declarations for the Lowerer |

## Procedure Lowering

| File                               | Purpose                              |
|------------------------------------|--------------------------------------|
| `Lowerer_Procedure.cpp`            | Procedure lowering coordinator       |
| `Lowerer_Procedure_Context.cpp`    | Procedure context management         |
| `Lowerer_Procedure_Emit.cpp`       | Procedure emission                   |
| `Lowerer_Procedure_Signatures.cpp` | Procedure signature handling         |
| `Lowerer_Procedure_Skeleton.cpp`   | Procedure skeleton generation        |
| `Lowerer_Procedure_Variables.cpp`  | Procedure variable handling          |
| `Lowerer_Program.cpp`              | Program-level lowering               |
| `Lowerer_Statement.cpp`            | Statement lowering dispatch          |

## Expression Lowering

| File                   | Purpose                          |
|------------------------|----------------------------------|
| `LowerExpr.cpp`        | Expression lowering              |
| `LowerExprBuiltin.cpp` | Builtin expression lowering impl |
| `LowerExprBuiltin.hpp` | Builtin expression lowering      |
| `LowerExprLogical.cpp` | Logical expression lowering impl |
| `LowerExprLogical.hpp` | Logical expression lowering      |
| `LowerExprNumeric.cpp` | Numeric expression lowering impl |
| `LowerExprNumeric.hpp` | Numeric expression lowering      |

## Statement Lowering

| File                                  | Purpose                          |
|---------------------------------------|----------------------------------|
| `ControlStatementLowerer.cpp`         | Control statement lowering impl  |
| `ControlStatementLowerer.hpp`         | Control statement lowering       |
| `IoStatementLowerer.cpp`              | I/O statement lowering impl      |
| `IoStatementLowerer.hpp`              | I/O statement lowering           |
| `LowerStmt_Control.cpp`               | Control flow lowering impl       |
| `LowerStmt_Control.hpp`               | Control flow lowering            |
| `LowerStmt_Core.hpp`                  | Core statement lowering          |
| `LowerStmt_IO.cpp`                    | I/O statement lowering impl      |
| `LowerStmt_IO.hpp`                    | I/O statement lowering           |
| `LowerStmt_Runtime.cpp`               | Runtime statement lowering impl  |
| `LowerStmt_Runtime.hpp`               | Runtime statement lowering       |
| `RuntimeCallHelpers.cpp`              | Runtime call utilities impl      |
| `RuntimeCallHelpers.hpp`              | Runtime call utilities           |
| `RuntimeHelperRequests.hpp`           | Runtime helper request types     |
| `RuntimeStatementLowerer.cpp`         | Runtime statement lowering impl  |
| `RuntimeStatementLowerer.hpp`         | Runtime statement lowering       |
| `RuntimeStatementLowerer_Assign.cpp`  | Assignment lowering              |
| `RuntimeStatementLowerer_Decl.cpp`    | Declaration lowering             |
| `RuntimeStatementLowerer_Terminal.cpp`| Terminal I/O lowering            |
| `SelectCaseLowering.cpp`              | SELECT CASE lowering impl        |
| `SelectCaseLowering.hpp`              | SELECT CASE lowering             |
| `SelectCaseRange.hpp`                 | SELECT CASE range handling       |
| `SelectModel.cpp`                     | SELECT CASE model impl           |
| `SelectModel.hpp`                     | SELECT CASE model                |

## Lowering Submodules (`lower/`)

| File                                    | Purpose                         |
|-----------------------------------------|---------------------------------|
| `lower/AstVisitor.hpp`                  | Lowering AST visitor            |
| `lower/BuiltinCommon.cpp`               | Common builtin lowering impl    |
| `lower/BuiltinCommon.hpp`               | Common builtin lowering         |
| `lower/Emit_Builtin.cpp`                | Builtin emission                |
| `lower/Emit_Control.cpp`                | Control flow emission           |
| `lower/Emit_Expr.cpp`                   | Expression emission             |
| `lower/Emit_OOP.cpp`                    | OOP emission                    |
| `lower/Emitter.cpp`                     | IL emitter implementation       |
| `lower/Emitter.hpp`                     | IL emitter class                |
| `lower/Lower_Expr_NumericClassifier.cpp`| Numeric type classification     |
| `lower/Lower_If.cpp`                    | IF statement lowering           |
| `lower/Lower_Loops.cpp`                 | Loop lowering                   |
| `lower/Lower_Switch.cpp`                | Switch/SELECT lowering          |
| `lower/Lower_TryCatch.cpp`              | Exception handling lowering     |
| `lower/Lowerer_Errors.cpp`              | Error lowering                  |
| `lower/Lowerer_Expr.cpp`                | Expression lowerer              |
| `lower/Lowerer_Stmt.cpp`                | Statement lowerer               |
| `lower/MemberArrayResolver.cpp`         | Member array field resolution impl |
| `lower/MemberArrayResolver.hpp`         | Member array field resolution for the lowerer |
| `lower/Scan_ExprTypes.cpp`              | Expression type scanning        |
| `lower/Scan_RuntimeNeeds.cpp`           | Runtime requirement scanning    |

## Lowering Common (`lower/common/`)

| File                              | Purpose                        |
|-----------------------------------|--------------------------------|
| `lower/common/BuiltinUtils.cpp`   | Builtin utilities impl         |
| `lower/common/BuiltinUtils.hpp`   | Builtin utilities              |
| `lower/common/CommonLowering.cpp` | Common lowering utilities impl |
| `lower/common/CommonLowering.hpp` | Common lowering utilities      |

## Lowering Builtins (`lower/builtins/`)

| File                            | Purpose                  |
|---------------------------------|--------------------------|
| `lower/builtins/Array.cpp`      | Array builtin lowering   |
| `lower/builtins/Common.cpp`     | Common builtin lowering  |
| `lower/builtins/IO.cpp`         | I/O builtin lowering     |
| `lower/builtins/Math.cpp`       | Math builtin lowering    |
| `lower/builtins/Registrars.hpp` | Builtin registrar declarations |
| `lower/builtins/String.cpp`     | String builtin lowering  |

## Lowering Detail (`lower/detail/`)

| File                                    | Purpose                        |
|-----------------------------------------|--------------------------------|
| `lower/detail/ControlLoweringHelper.cpp`| Control flow lowering helpers  |
| `lower/detail/ExprLoweringHelper.cpp`   | Expression lowering helpers    |
| `lower/detail/LowererDetail.hpp`        | Internal lowerer declarations  |
| `lower/detail/OopLoweringHelper.cpp`    | OOP lowering helpers           |
| `lower/detail/RuntimeLoweringHelper.cpp`| Runtime call lowering helpers  |

## Emission Helpers

| File             | Purpose                        |
|------------------|--------------------------------|
| `EmitCommon.cpp` | Common emission utilities impl |
| `EmitCommon.hpp` | Common emission utilities      |
| `LowerEmit.cpp`  | IL emission helpers impl       |
| `LowerEmit.hpp`  | IL emission helpers            |
| `LowerRuntime.cpp`| Runtime call lowering impl    |
| `LowerRuntime.hpp`| Runtime call lowering         |
| `LowerScan.cpp`  | Pre-lowering scan impl         |
| `LowerScan.hpp`  | Pre-lowering scan phase        |

## Builtins

| File                          | Purpose                        |
|-------------------------------|--------------------------------|
| `BuiltinRegistry.cpp`         | Builtin function registry impl |
| `BuiltinRegistry.hpp`         | Builtin function registry      |
| `builtins/MathBuiltins.cpp`   | Math builtin implementations   |
| `builtins/MathBuiltins.hpp`   | Math builtin declarations      |
| `builtins/StringBuiltins.cpp` | String builtin implementations |
| `builtins/StringBuiltins.hpp` | String builtin declarations    |
| `Intrinsics.cpp`              | Intrinsic function impl        |
| `Intrinsics.hpp`              | Intrinsic function definitions |
| `builtin_registry.inc`        | Auto-generated builtin X-macro table (rows expand via the includer's `BUILTIN` macro) |

## Constant Folding (`constfold/`)

| File                          | Purpose                           |
|-------------------------------|-----------------------------------|
| `ConstFolder.cpp`             | Constant folding coordinator impl |
| `ConstFolder.hpp`             | Constant folding coordinator      |
| `constfold/ConstantUtils.hpp` | Constant utilities                |
| `constfold/Dispatch.cpp`      | Fold dispatch implementation      |
| `constfold/Dispatch.hpp`      | Fold dispatch logic               |
| `constfold/FoldArith.cpp`     | Arithmetic folding                |
| `constfold/FoldBuiltins.cpp`  | Builtin function folding          |
| `constfold/FoldCasts.cpp`     | Cast folding                      |
| `constfold/FoldCompare.cpp`   | Comparison folding                |
| `constfold/FoldLogical.cpp`   | Logical folding                   |
| `constfold/FoldStrings.cpp`   | String folding                    |
| `constfold/Value.hpp`         | Compile-time value representation |

## Types

| File                    | Purpose                           |
|-------------------------|-----------------------------------|
| `ILTypeUtils.cpp`       | IL type utilities impl            |
| `ILTypeUtils.hpp`       | IL type utilities                 |
| `NumericRules.hpp`      | Numeric type rules                |
| `TypeCoercionEngine.cpp`| Unified type coercion engine impl (all numeric conversions route here) |
| `TypeCoercionEngine.hpp`| Unified type coercion engine (float/int/bool conversions)             |
| `TypeRules.cpp`         | Type checking rules impl          |
| `TypeRules.hpp`         | Type checking rules               |
| `TypeSuffix.cpp`        | Type suffix handling impl         |
| `TypeSuffix.hpp`        | Type suffix handling (%, $, etc.) |
| `types/TypeMapping.cpp` | BASIC to IL type mapping impl     |
| `types/TypeMapping.hpp` | BASIC to IL type mapping          |

## Diagnostics (`diag/`)

| File                 | Purpose                              |
|----------------------|--------------------------------------|
| `diag/BasicDiag.cpp` | BASIC-specific diagnostics           |

## Passes (`passes/`)

| File                        | Purpose                      |
|-----------------------------|------------------------------|
| `passes/CollectProcs.cpp`   | Procedure collection impl    |
| `passes/CollectProcs.hpp`   | Procedure collection pass    |

## Print Utilities (`print/`)

| File                          | Purpose                        |
|-------------------------------|--------------------------------|
| `print/Print_Stmt_Common.hpp` | Common statement print utils   |
| `print/Print_Stmt_Control.cpp`| Control statement printing     |
| `print/Print_Stmt_Decl.cpp`   | Declaration statement printing |
| `print/Print_Stmt_IO.cpp`     | I/O statement printing         |
| `print/Print_Stmt_Jump.cpp`   | Jump statement printing        |

## IDE Tooling

| File                  | Purpose                                                       |
|-----------------------|---------------------------------------------------------------|
| `BasicAnalysis.cpp`   | Partial-compilation API implementation for IDE tooling        |
| `BasicAnalysis.hpp`   | Partial-compilation API for BASIC IDE tooling (completion, hover) |
| `BasicCompletion.cpp` | Code-completion engine implementation                         |
| `BasicCompletion.hpp` | Code-completion engine for the Zanna BASIC language           |

## Debugging and Inspection

CLI flags for inspecting each pipeline stage (see `docs/tools/debugging.md` §8):

| Flag | Stage |
|------|-------|
| `--dump-tokens` | Token stream from Lexer |
| `--dump-ast` | AST after Parser (uses `AstPrinter`) |
| `--dump-il` | IL after Lowerer |
| `--dump-il-passes` | IL before/after each optimizer pass |
| `--dump-il-opt` | IL after full optimization |

Note: `--dump-sema-ast` is Zia-only. BASIC does not have a post-sema AST dump.

Programmatic equivalents are in `BasicCompilerOptions` (`BasicCompiler.hpp`).

## Utilities

| File                     | Purpose                      |
|--------------------------|------------------------------|
| `IdentifierUtil.hpp`     | Identifier utilities         |
| `LineUtils.hpp`          | Line number utilities        |
| `LocationScope.cpp`      | Source location scoping impl |
| `LocationScope.hpp`      | Source location scoping      |
| `NameMangler.cpp`        | Name mangling impl           |
| `NameMangler.hpp`        | Name mangling                |
| `ScopeTracker.hpp`       | Scope tracking               |
| `StatementSequencer.cpp` | Statement sequencing impl    |
| `StatementSequencer.hpp` | Statement sequencing         |
| `StringUtils.hpp`        | String utilities             |
