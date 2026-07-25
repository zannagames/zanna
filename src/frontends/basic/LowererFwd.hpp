//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file LowererFwd.hpp
/// @brief Forward declarations for the BASIC lowering subsystem.
/// @details Provides lightweight type declarations so clients can reference
///          lowering types without including the full lowering headers. This
///          reduces compile-time dependencies and helps avoid circular includes.
///
/// Usage:
/// @code
/// #include "frontends/basic/LowererFwd.hpp"
/// void someFunction(il::frontends::basic::Lowerer &lowerer);
/// @endcode
//
//===----------------------------------------------------------------------===//
#pragma once

namespace il::frontends::basic {

/// @brief Entry point for lowering BASIC source into IL.
class Lowerer;

/// @brief Base helper for lowering BASIC statements.
class StatementLowerer;
/// @brief Helper for lowering IO-related BASIC statements.
class IoStatementLowerer;
/// @brief Helper for lowering control-flow BASIC statements.
class ControlStatementLowerer;
/// @brief Helper for lowering runtime-facing BASIC statements.
class RuntimeStatementLowerer;

/// @brief Visitor used to lower expressions into IL instructions.
class LowererExprVisitor;
/// @brief Visitor used to lower statements into IL instructions.
class LowererStmtVisitor;

/// @brief Pipeline stage that lowers an entire program.
struct ProgramLowering;
/// @brief Pipeline stage that lowers a single procedure.
struct ProcedureLowering;
/// @brief Pipeline stage that lowers statement sequences.
struct StatementLowering;

/// @brief Helper for lowering logical expressions and short-circuiting.
struct LogicalExprLowering;
/// @brief Helper for lowering numeric expressions.
struct NumericExprLowering;
/// @brief Helper for lowering builtin function calls and operators.
struct BuiltinExprLowering;

/// @brief Lowering helper for SELECT CASE constructs.
class SelectCaseLowering;

/// @brief Diagnostic emitter interface used by the lowering pipeline.
class DiagnosticEmitter;

/// @brief Semantic analyzer interface consumed by lowerers.
class SemanticAnalyzer;

/// @brief Context shared across OOP lowering helpers.
struct OopLoweringContext;
/// @brief Helper that emits IL for object-oriented constructs.
class OopEmitHelper;

/// @brief IL emission helper used by lowering helpers.
class Emit;
/// @brief Builder for runtime helper calls during lowering.
class RuntimeCallBuilder;
/// @brief Engine for inserting implicit type coercions.
class TypeCoercionEngine;

/// @brief Policy that governs overflow behavior in numeric lowering.
enum class OverflowPolicy;
/// @brief Policy describing signedness expectations in numeric lowering.
enum class Signedness;

namespace builtins {
/// @brief Context object used when lowering builtin functions.
class LowerCtx;
} // namespace builtins

namespace lower {
/// @brief IL emitter abstraction for lowering submodules.
class Emitter;
/// @brief Context object for lowering builtin operations.
class BuiltinLowerContext;

namespace common {
/// @brief Shared lowering utilities used by multiple frontend components.
class CommonLowering;
} // namespace common

namespace detail {
/// @brief Helper that scans expressions for type constraints.
class ExprTypeScanner;
/// @brief Helper that detects runtime feature needs during lowering.
class RuntimeNeedsScanner;

/// @brief Modular helper for lowering expressions.
class ExprLoweringHelper;
/// @brief Modular helper for lowering control-flow statements.
class ControlLoweringHelper;
/// @brief Modular helper for lowering object-oriented constructs.
class OopLoweringHelper;
/// @brief Modular helper for lowering runtime-specific constructs.
class RuntimeLoweringHelper;
} // namespace detail

} // namespace lower

} // namespace il::frontends::basic
