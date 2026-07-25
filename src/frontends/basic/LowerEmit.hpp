//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/LowerEmit.hpp
// Purpose: Declares IR emission helpers and lowering routines for BASIC.
// Key invariants: Control-flow labels remain deterministic via BlockNamer.
// Ownership/Lifetime: Operates on Lowerer state without owning AST or module.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/**
 * @file LowerEmit.hpp
 * @brief Provides the legacy class-body declaration fragment for BASIC lowering.
 *
 * The fragment groups procedure, statement, expression, and low-level IL
 * emission members by access level. It is intended for textual inclusion inside
 * a Lowerer class definition rather than as a standalone declaration header.
 */

#pragma once

#include <cstdint>

private:
/// @brief Scan the program for SUB/FUNCTION declarations and register their signatures.
/// @details Pre-populates the procedure signature table so that forward references
///          to FUNCTION and SUB names can be resolved during lowering.
/// @param prog The parsed BASIC program containing all top-level declarations.
void collectProcedureSignatures(const Program &prog);

/// @brief Discover and register all variables used in the program.
/// @details Walks the entire AST to find variable definitions and references,
///          populating the local variable map used during IL generation.
/// @param prog The parsed BASIC program to scan for variable usage.
void collectVars(const Program &prog);

/// @brief Discover variables in a list of statements (used for nested scopes).
/// @details Null statement pointers are ignored while the remaining AST nodes
///          contribute their referenced and declared symbols.
/// @param stmts Vector of statement pointers to scan for variable usage.
void collectVars(const std::vector<const Stmt *> &stmts);

/// @brief Lower a FUNCTION declaration into an IL function definition.
/// @details Generates the IL function header, parameter slots, body, and
///          return-value handling for a BASIC FUNCTION block.
/// @param decl The FUNCTION declaration AST node.
void lowerFunctionDecl(const FunctionDecl &decl);

/// @brief Lower a SUB declaration into an IL function definition.
/// @details Generates the IL function header, parameter slots, and body
///          for a BASIC SUB block (void-returning procedure).
/// @param decl The SUB declaration AST node.
void lowerSubDecl(const SubDecl &decl);

public:
/// @brief Configuration shared by FUNCTION and SUB lowering.
struct ProcedureConfig {
    Type retType{Type(Type::Kind::Void)};  ///< IL return type for the procedure.
    std::function<void()> postCollect;     ///< Hook after variable discovery.
    std::function<void()> emitEmptyBody;   ///< Emit return path for empty bodies.
    std::function<void()> emitFinalReturn; ///< Emit return in the synthetic exit block.
};

private:
/// @brief Lower shared procedure scaffolding for FUNCTION/SUB declarations.
/// @param name Source-level procedure name used for signature lookup and mangling.
/// @param params Ordered formal parameters to materialize.
/// @param body Owned AST statements comprising the procedure body.
/// @param config Return type and stage-specific emission callbacks.
void lowerProcedure(const std::string &name,
                    const std::vector<Param> &params,
                    const std::vector<StmtPtr> &body,
                    const ProcedureConfig &config);
/// @brief Stack-allocate parameters and seed local map.
/// @param params Ordered formal parameters whose incoming values are materialized.
void materializeParams(const std::vector<Param> &params);
/// @brief Reset procedure-level lowering state between FUNCTION/SUB bodies.
/// @details Clears the local variable map, block namer counters, and any
///          accumulated deferred-release lists so the next procedure starts clean.
/// @post Procedure-local symbols, control-flow state, and deferred temporaries
///       no longer refer to the preceding procedure.
void resetLoweringState();

/// @brief Lower a single BASIC statement into IL instructions.
/// @details Dispatches on the statement kind (LET, PRINT, IF, WHILE, FOR,
///          DIM, INPUT, GOTO, etc.) and emits the corresponding IL.
/// @param stmt The statement AST node to lower.
void lowerStmt(const Stmt &stmt);

/// @brief Lower an expression and return its IL value with type information.
/// @details Dispatches on the expression kind (literal, variable, binary,
///          unary, call, array access, etc.) and returns the emitted IL value
///          along with its BASIC type classification.
/// @param expr The expression AST node to lower.
/// @return An RVal containing the emitted IL value and its inferred type.
RVal lowerExpr(const Expr &expr);
/// @brief Lower a variable reference expression.
/// @param expr Variable expression node.
/// @return Loaded value and its type.
RVal lowerVarExpr(const VarExpr &expr);
/// @brief Lower a unary expression (e.g. NOT).
/// @param expr Unary expression node.
/// @return Resulting value and type.
RVal lowerUnaryExpr(const UnaryExpr &expr);
/// @brief Lower a binary expression.
/// @param expr Binary expression node.
/// @return Resulting value and type.
RVal lowerBinaryExpr(const BinaryExpr &expr);
/// @brief Lower a boolean expression using explicit branch bodies.
/// @param cond Boolean condition selecting THEN branch.
/// @param loc Source location to attribute to control flow.
/// @param emitThen Lambda emitting THEN branch body, storing into result slot.
/// @param emitElse Lambda emitting ELSE branch body, storing into result slot.
/// @param thenLabelBase Optional label base for THEN block naming.
/// @param elseLabelBase Optional label base for ELSE block naming.
/// @param joinLabelBase Optional label base for join block naming.
/// @return Resulting value and type.
RVal lowerBoolBranchExpr(Value cond,
                         il::support::SourceLoc loc,
                         const std::function<void(Value)> &emitThen,
                         const std::function<void(Value)> &emitElse,
                         std::string_view thenLabelBase = {},
                         std::string_view elseLabelBase = {},
                         std::string_view joinLabelBase = {});
/// @brief Lower logical (`AND`/`OR`) expressions with short-circuiting.
/// @param expr Binary expression node.
/// @return Resulting value and type.
RVal lowerLogicalBinary(const BinaryExpr &expr);
/// @brief Lower integer division and modulo with divide-by-zero check.
/// @param expr Binary expression node.
/// @return Resulting value and type.
RVal lowerDivOrMod(const BinaryExpr &expr);
/// @brief Lower string concatenation and equality/inequality comparisons.
/// @param expr Binary expression node.
/// @param lhs Pre-lowered left-hand side.
/// @param rhs Pre-lowered right-hand side.
/// @return Resulting value and type.
RVal lowerStringBinary(const BinaryExpr &expr, RVal lhs, RVal rhs);
/// @brief Lower numeric arithmetic and comparisons.
/// @param expr Binary expression node.
/// @param lhs Pre-lowered left-hand side.
/// @param rhs Pre-lowered right-hand side.
/// @return Resulting value and type.
RVal lowerNumericBinary(const BinaryExpr &expr, RVal lhs, RVal rhs);
/// @brief Lower a built-in call expression.
/// @param expr Built-in call expression node.
/// @return Resulting value and type.
RVal lowerBuiltinCall(const BuiltinCallExpr &expr);

private:
// =========================================================================
// Shared argument coercion helpers
// =========================================================================

/// @brief Coerce a value to i64, inserting an implicit widening if needed.
/// @param v The value to coerce (may already be i64).
/// @param loc Source location for diagnostic reporting if the coercion fails.
/// @return An RVal containing the i64 value.
RVal coerceToI64(RVal v, il::support::SourceLoc loc);

/// @brief Coerce a value to f64, inserting an implicit widening if needed.
/// @param v The value to coerce (may already be f64, or i64 requiring sitofp).
/// @param loc Source location for diagnostic reporting if the coercion fails.
/// @return An RVal containing the f64 value.
RVal coerceToF64(RVal v, il::support::SourceLoc loc);

/// @brief Coerce a value to i1 boolean, inserting a comparison if needed.
/// @param v The value to coerce (may already be i1, or i64 requiring icmp ne 0).
/// @param loc Source location for diagnostic reporting if the coercion fails.
/// @return An RVal containing the i1 boolean value.
RVal coerceToBool(RVal v, il::support::SourceLoc loc);

/// @brief Assert that a value is already i64 and return it, or coerce if possible.
/// @param v The value that should be i64.
/// @param loc Source location for diagnostic reporting.
/// @return An RVal containing the i64 value.
RVal ensureI64(RVal v, il::support::SourceLoc loc);

/// @brief Assert that a value is already f64 and return it, or coerce if possible.
/// @param v The value that should be f64.
/// @param loc Source location for diagnostic reporting.
/// @return An RVal containing the f64 value.
RVal ensureF64(RVal v, il::support::SourceLoc loc);

/// @brief Lower a LET assignment statement (variable = expression).
/// @param stmt The LET statement AST node.
void lowerLet(const LetStmt &stmt);

/// @brief Lower a PRINT statement, emitting calls to terminal print routines.
/// @details Handles semicolon-separated values, TAB positioning, and the
///          trailing newline (unless a semicolon suppresses it).
/// @param stmt The PRINT statement AST node.
void lowerPrint(const PrintStmt &stmt);

/// @brief Lower a compound statement list by iterating and lowering each child.
/// @param stmt The statement list AST node containing child statements.
void lowerStmtList(const StmtList &stmt);

/// @brief Lower a RETURN statement from a FUNCTION or GOSUB context.
/// @details For FUNCTION returns, evaluates the return expression and emits
///          a ret instruction. For GOSUB returns, emits a branch to the
///          saved return address.
/// @param stmt The RETURN statement AST node.
void lowerReturn(const ReturnStmt &stmt);
/// @brief Emit blocks for an IF/ELSEIF chain.
/// @param conds Number of conditions (IF + ELSEIFs).
/// @return Indices for test/then blocks and ELSE/exit blocks.
IfBlocks emitIfBlocks(size_t conds);
/// @brief Evaluate @p cond and branch to @p thenBlk or @p falseBlk.
/// @param cond Condition expression to lower.
/// @param testBlk Block in which condition evaluation begins.
/// @param thenBlk Destination selected by a true condition.
/// @param falseBlk Destination selected by a false condition.
/// @param loc Source location attached to emitted control-flow instructions.
void lowerIfCondition(const Expr &cond,
                      BasicBlock *testBlk,
                      BasicBlock *thenBlk,
                      BasicBlock *falseBlk,
                      il::support::SourceLoc loc);
/// @brief Lower a boolean expression directly into a conditional branch.
/// @param expr Boolean-valued expression to lower.
/// @param trueBlk Destination selected by a true result.
/// @param falseBlk Destination selected by a false result.
/// @param loc Source location attached to emitted instructions.
void lowerCondBranch(const Expr &expr,
                     BasicBlock *trueBlk,
                     BasicBlock *falseBlk,
                     il::support::SourceLoc loc);
/// @brief Lower a THEN/ELSE branch and link to exit.
/// @param stmt Optional branch body; null denotes an empty branch.
/// @param thenBlk Block receiving the branch body.
/// @param exitIdx Stable index of the join block in the active function.
/// @param loc Source location attached to emitted branch instructions.
/// @return True if branch falls through to exit block.
bool lowerIfBranch(const Stmt *stmt,
                   BasicBlock *thenBlk,
                   size_t exitIdx,
                   il::support::SourceLoc loc);
/// @brief Lower an IF/ELSEIF/ELSE statement chain into conditional branches.
/// @param stmt The IF statement AST node.
void lowerIf(const IfStmt &stmt);

/// @brief Lower a WHILE loop into a condition-test/body/backedge structure.
/// @param stmt The WHILE statement AST node.
void lowerWhile(const WhileStmt &stmt);

/// @brief Lower a DO/LOOP statement into pre-test or post-test loop structure.
/// @param stmt The DO statement AST node.
void lowerDo(const DoStmt &stmt);

/// @brief Lower the body of a loop, handling NEXT and EXIT within.
/// @param body The list of statements forming the loop body.
void lowerLoopBody(const std::vector<StmtPtr> &body);

/// @brief Lower a FOR/NEXT loop with start, end, and step values.
/// @details Dispatches to lowerForConstStep or lowerForVarStep depending
///          on whether the STEP expression is a compile-time constant.
/// @param stmt The FOR statement AST node.
void lowerFor(const ForStmt &stmt);

/// @brief Lower a FOR loop with a compile-time constant STEP value.
/// @details Uses the sign of stepConst to determine whether the loop counts
///          up or down, generating an appropriate comparison (LE vs GE).
/// @param stmt The FOR statement AST node.
/// @param slot The stack slot holding the loop counter variable.
/// @param end The pre-computed end-value RVal.
/// @param step The pre-computed step-value RVal.
/// @param stepConst The compile-time constant step value.
void lowerForConstStep(const ForStmt &stmt, Value slot, RVal end, RVal step, int64_t stepConst);

/// @brief Lower a FOR loop with a runtime-variable STEP value.
/// @details Emits a runtime sign check on the step value to select between
///          ascending (LE) and descending (GE) loop termination conditions.
/// @param stmt The FOR statement AST node.
/// @param slot The stack slot holding the loop counter variable.
/// @param end The pre-computed end-value RVal.
/// @param step The pre-computed step-value RVal.
void lowerForVarStep(const ForStmt &stmt, Value slot, RVal end, RVal step);

/// @brief Allocate the basic blocks needed for a FOR loop structure.
/// @param varStep True if the step is runtime-variable (needs additional blocks).
/// @return A ForBlocks struct containing the header, body, step, and exit blocks.
ForBlocks setupForBlocks(bool varStep);

/// @brief Emit the increment/decrement of the FOR loop counter by the step value.
/// @param slot The stack slot holding the loop counter.
/// @param step The step value to add to the counter.
void emitForStep(Value slot, Value step);

/// @brief Lower a NEXT statement that closes a FOR loop.
/// @param stmt The NEXT statement AST node.
void lowerNext(const NextStmt &stmt);

/// @brief Lower an EXIT statement (EXIT FOR, EXIT WHILE, EXIT DO, etc.).
/// @details Emits an unconditional branch to the exit block of the
///          matching enclosing loop.
/// @param stmt The EXIT statement AST node.
void lowerExit(const ExitStmt &stmt);

/// @brief Lower a GOTO statement to an unconditional branch to a label block.
/// @param stmt The GOTO statement AST node.
void lowerGoto(const GotoStmt &stmt);

/// @brief Lower an ON ERROR GOTO statement for structured error handling.
/// @param stmt The ON ERROR GOTO statement AST node.
void lowerOnErrorGoto(const OnErrorGoto &stmt);

/// @brief Lower a RESUME statement inside an error handler.
/// @param stmt The RESUME statement AST node.
void lowerResume(const Resume &stmt);

/// @brief Lower an END statement that terminates program execution.
/// @param stmt The END statement AST node.
void lowerEnd(const EndStmt &stmt);

/// @brief Lower an INPUT statement, emitting calls to terminal input routines.
/// @details Handles optional prompt strings and multiple variable targets
///          separated by commas.
/// @param stmt The INPUT statement AST node.
void lowerInput(const InputStmt &stmt);

/// @brief Lower a DIM statement to allocate arrays or declare typed variables.
/// @details Emits runtime array allocation calls with the declared dimensions
///          and element type.
/// @param stmt The DIM statement AST node.
void lowerDim(const DimStmt &stmt);

/// @brief Lower a RANDOMIZE statement to seed the random number generator.
/// @param stmt The RANDOMIZE statement AST node.
void lowerRandomize(const RandomizeStmt &stmt);

/// @brief Lower a TRY/CATCH statement into exception-handling IR.
/// @details Emits the try body with an error handler push, the catch handler
///          block, and links them with the error-handling infrastructure.
/// @param stmt The TRY/CATCH statement AST node.
void lowerTryCatch(const TryCatchStmt &stmt);

// =========================================================================
// IL emission helpers — low-level instruction constructors
// =========================================================================

/// @brief Return the IL type used for BASIC boolean values (i1).
/// @return The IL type descriptor representing a one-bit boolean.
IlType ilBoolTy();

/// @brief Emit a boolean constant as an IL i1 value.
/// @param v The boolean constant (true or false).
/// @return The emitted IL constant value.
IlValue emitBoolConst(bool v);

/// @brief Emit a boolean value computed from two branch bodies.
/// @details Creates a diamond control-flow pattern with a stack slot, where
///          emitThen stores into the slot on the true path and emitElse stores
///          on the false path, then loads the result at the join point.
/// @param emitThen Lambda that stores the "true" result into the provided slot.
/// @param emitElse Lambda that stores the "false" result into the provided slot.
/// @param thenLabelBase Optional label prefix for the then-block.
/// @param elseLabelBase Optional label prefix for the else-block.
/// @param joinLabelBase Optional label prefix for the join-block.
/// @return The loaded boolean value at the join point.
IlValue emitBoolFromBranches(const std::function<void(Value)> &emitThen,
                             const std::function<void(Value)> &emitElse,
                             std::string_view thenLabelBase = "bool_then",
                             std::string_view elseLabelBase = "bool_else",
                             std::string_view joinLabelBase = "bool_join");

/// @brief Emit a stack allocation of the given size in bytes.
/// @param bytes Number of bytes to allocate on the stack frame.
/// @return A pointer value referencing the allocated stack slot.
Value emitAlloca(int bytes);

/// @brief Emit a load instruction from a memory address.
/// @param ty The IL type of the value being loaded.
/// @param addr The pointer value to load from.
/// @return The loaded value.
Value emitLoad(Type ty, Value addr);

/// @brief Emit a store instruction writing a value to a memory address.
/// @param ty The IL type of the value being stored.
/// @param addr The pointer value to store into.
/// @param val The value to write.
void emitStore(Type ty, Value addr, Value val);

/// @brief Emit a binary arithmetic or comparison instruction.
/// @param op The IL opcode for the binary operation (add, sub, mul, etc.).
/// @param ty The IL type of the operands and result.
/// @param lhs The left-hand operand.
/// @param rhs The right-hand operand.
/// @return The result value of the binary operation.
Value emitBinary(Opcode op, Type ty, Value lhs, Value rhs);

/// @brief Emit a unary instruction (e.g. negation, bitwise NOT).
/// @param op The IL opcode for the unary operation.
/// @param ty The IL type of the operand and result.
/// @param val The operand value.
/// @return The result value of the unary operation.
Value emitUnary(Opcode op, Type ty, Value val);

/// @brief Emit a 64-bit integer constant.
/// @param v The integer constant value.
/// @return The emitted IL constant value.
Value emitConstI64(std::int64_t v);

/// @brief Emit a zero-extension from i1 to i64.
/// @details Used to convert boolean comparison results (i1) into BASIC
///          integer-width values. A true bit becomes 1; callers requiring
///          BASIC's canonical -1 representation must normalize separately.
/// @param val The i1 value to extend.
/// @return The zero-extended i64 value.
Value emitZext1ToI64(Value val);

/// @brief Emit an integer subtraction (lhs - rhs).
/// @param lhs The left-hand operand.
/// @param rhs The right-hand operand.
/// @return The difference as an i64 value.
Value emitISub(Value lhs, Value rhs);

/// @brief Convert a BASIC logical value (where any nonzero = true) to canonical i64.
/// @details BASIC uses -1 for TRUE and 0 for FALSE in logical operations.
///          This helper normalizes arbitrary nonzero values.
/// @param b1 The input i64 value to normalize.
/// @return The canonical BASIC logical value (-1 or 0).
Value emitBasicLogicalI64(Value b1);

/// @brief Emit an unconditional branch to a target basic block.
/// @param target The basic block to branch to.
void emitBr(BasicBlock *target);

/// @brief Emit a conditional branch based on a boolean condition.
/// @param cond The i1 condition value.
/// @param t The basic block to branch to if cond is true.
/// @param f The basic block to branch to if cond is false.
void emitCBr(Value cond, BasicBlock *t, BasicBlock *f);

/// @brief Emit a direct function call that returns a value.
/// @param ty The IL return type of the called function.
/// @param callee The name of the function to call.
/// @param args The argument values to pass.
/// @return The return value of the call.
Value emitCallRet(Type ty, const std::string &callee, const std::vector<Value> &args);

/// @brief Emit a direct function call that returns void.
/// @param callee The name of the function to call.
/// @param args The argument values to pass.
void emitCall(const std::string &callee, const std::vector<Value> &args);

/// @brief Emit an indirect (function-pointer) call that returns a value.
/// @param ty The IL return type of the called function.
/// @param callee The function pointer value to call through.
/// @param args The argument values to pass.
/// @return The return value of the call.
Value emitCallIndirectRet(Type ty, Value callee, const std::vector<Value> &args);

/// @brief Emit an indirect (function-pointer) call that returns void.
/// @param callee The function pointer value to call through.
/// @param args The argument values to pass.
void emitCallIndirect(Value callee, const std::vector<Value> &args);

/// @brief Emit a reference to a string constant in the global data section.
/// @param globalName The name of the global string constant.
/// @return A pointer value referencing the string data.
Value emitConstStr(const std::string &globalName);

/// @brief Emit a trap instruction that terminates execution with an error.
void emitTrap();

/// @brief Push an error handler onto the exception-handling stack.
/// @param handler The basic block to branch to on error.
void emitEhPush(BasicBlock *handler);

/// @brief Pop the top error handler from the exception-handling stack.
void emitEhPop();

/// @brief Pop all error handlers before a return statement.
/// @details Ensures the EH stack is balanced when returning from a procedure
///          that installed error handlers.
void emitEhPopForReturn();

/// @brief Clear the currently active ON ERROR GOTO handler.
void clearActiveErrorHandler();

/// @brief Get or create the basic block for an error handler targeting a line label.
/// @param targetLine The line number label that ON ERROR GOTO targets.
/// @return Pointer to the error handler basic block.
BasicBlock *ensureErrorHandlerBlock(int targetLine);

/// @brief Emit a return instruction with a value.
/// @param v The value to return from the current function.
void emitRet(Value v);

/// @brief Emit a void return instruction.
void emitRetVoid();

/// @brief Get or create a unique global label name for a string literal.
/// @details Deduplicates identical string constants, returning the same label
///          for the same string content.
/// @param s The string literal content.
/// @return The global label name assigned to this string.
std::string getStringLabel(const std::string &s);

/// @brief Generate the next unique temporary ID for block naming.
/// @return A monotonically increasing unsigned integer.
unsigned nextTempId();

/// @brief Lower an array element access expression into pointer arithmetic.
/// @details Evaluates the array base and index expressions, performs bounds
///          checking if enabled, and computes the element address.
/// @param expr The array access expression AST node.
/// @param kind Whether this is a read (Get) or write (Set) access.
/// @return An ArrayAccess struct with the computed address and element type.
ArrayAccess lowerArrayAccess(const ArrayExpr &expr, ArrayAccessKind kind);

/// @brief Current expression lowering depth for recursion guard.
unsigned exprLowerDepth_{0};
/// @brief Current statement lowering depth for recursion guard.
unsigned stmtLowerDepth_{0};
/// @brief Maximum allowed lowering recursion depth.
static constexpr unsigned kMaxLowerDepth = 512;

/// @brief Emit the entire BASIC program as an IL module.
/// @details This is the top-level entry point for lowering: it processes all
///          top-level statements, SUB/FUNCTION declarations, and DIM arrays,
///          generating the complete IL module.
/// @param prog The parsed BASIC program AST.
void emitProgram(const Program &prog);
