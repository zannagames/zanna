//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/zia/AST_Stmt.hpp
// Purpose: Define executable Zia statement nodes and block/control-flow
//          containers used inside function and method bodies.
// Key invariants:
//   * Stmt::kind matches the concrete statement node.
//   * Block and branch nodes own their nested statements and expressions.
//   * Source ranges retain both starting and, where available, ending locations.
// Ownership: Statement nodes own syntactic children through unique_ptr aliases
//            and vectors of those aliases.
// References: docs/languages/zia-reference.md, docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//
///
/// @file
/// @brief Statement nodes for the Zia AST.
///
/// @details Defines all statement AST nodes produced by the Zia parser.
/// Statements perform actions but do not produce values (unlike expressions).
/// They include control flow (if/else, while, for, match), variable
/// declarations (var/let), jumps (return, break, continue), blocks,
/// assignments, and expression statements. Each statement node carries a
/// source location for error reporting and a StmtKind tag for downcasting.
///
/// Statement nodes are created by the Parser and consumed by the Sema
/// (semantic analyzer) for type checking, then by the Lowerer for IL
/// generation. The Lowerer translates each statement kind into the
/// corresponding IL instructions (branches, stores, calls, etc.).
///
/// @invariant Every Stmt has a valid `kind` field matching its concrete type.
/// @invariant Source locations are non-null for all user-written statements.
///
/// Ownership/Lifetime: Owned by their parent node (block, function body,
/// or module) via StmtPtr (std::unique_ptr<Stmt>).
///
/// @see AST_Expr.hpp — expression nodes that statements may contain.
/// @see Parser.hpp — creates statement nodes during parsing.
/// @see Lowerer.hpp — translates statement nodes into IL instructions.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "frontends/zia/AST_Expr.hpp"
#include <optional>
#include <string>
#include <vector>

namespace il::frontends::zia {
//===----------------------------------------------------------------------===//
/// @name Statement Nodes
/// @brief AST nodes representing statements that perform actions.
/// @details Statements execute actions but don't produce values (unlike
/// expressions). They include control flow, declarations, and jumps.
/// @{
//===----------------------------------------------------------------------===//

/// @brief Enumerates all kinds of statement nodes.
/// @details Used for runtime type identification when processing statements.
enum class StmtKind {
    /// @brief Block of statements: `{ stmt1; stmt2; }`.
    /// @see BlockStmt
    Block,

    /// @brief Expression used as statement: `f();`.
    /// @see ExprStmt
    Expr,

    /// @brief Variable declaration: `var x = 1;`.
    /// @see VarStmt
    Var,

    /// @brief Conditional statement: `if (c) { ... }`.
    /// @see IfStmt
    If,

    /// @brief While loop: `while (c) { ... }`.
    /// @see WhileStmt
    While,

    /// @brief C-style for loop: `for (init; cond; update) { ... }`.
    /// @see ForStmt
    For,

    /// @brief For-in loop: `for (x in collection) { ... }`.
    /// @see ForInStmt
    ForIn,

    /// @brief Return from function: `return expr;`.
    /// @see ReturnStmt
    Return,

    /// @brief Break out of loop: `break;`.
    /// @see BreakStmt
    Break,

    /// @brief Continue to next iteration: `continue;`.
    /// @see ContinueStmt
    Continue,

    /// @brief Cleanup action registered for scope exit: `defer cleanup();`.
    /// @see DeferStmt
    Defer,

    /// @brief Guard statement: `guard (c) else { return; }`.
    /// @see GuardStmt
    Guard,

    /// @brief Pattern matching statement: `match x { ... }`.
    /// @see MatchStmt
    Match,

    /// @brief Try/catch/finally statement.
    /// @see TryStmt
    Try,

    /// @brief Throw statement: `throw expr;`.
    /// @see ThrowStmt
    Throw,
};

/// @brief Base class for all statement nodes.
/// @details Statements perform actions and may contain nested statements
/// and expressions. Unlike expressions, statements don't produce values.
///
/// @invariant `kind` correctly identifies the concrete subclass type.
struct Stmt {
    /// @brief Identifies the concrete statement kind for downcasting.
    StmtKind kind;

    /// @brief Source location of this statement.
    SourceLoc loc;

    /// @brief Construct a statement with kind and location.
    /// @param k The specific statement kind.
    /// @param l Source location.
    Stmt(StmtKind k, SourceLoc l) : kind(k), loc(l) {}

    /// @brief Virtual destructor for proper polymorphic cleanup.
    virtual ~Stmt() = default;
};

/// @brief Block statement: `{ stmt1; stmt2; }`.
/// @details Groups multiple statements into a single compound statement.
/// Creates a new scope for local variables.
struct BlockStmt : Stmt {
    /// @brief The statements within this block.
    std::vector<StmtPtr> statements;

    /// @brief Location of the closing brace when this block came from braces.
    SourceLoc endLoc{};

    /// @brief Construct a block statement.
    /// @param l Source location.
    /// @param s The statements.
    BlockStmt(SourceLoc l, std::vector<StmtPtr> s)
        : Stmt(StmtKind::Block, l), statements(std::move(s)) {}

    /// @brief Construct a block statement with an explicit closing location.
    /// @param l Source location.
    /// @param end Closing brace or synthetic end location.
    /// @param s The statements.
    BlockStmt(SourceLoc l, SourceLoc end, std::vector<StmtPtr> s)
        : Stmt(StmtKind::Block, l), statements(std::move(s)), endLoc(end) {}
};

/// @brief Block expression: `{ stmts; expr }`.
/// @details A block that evaluates to a value. The last expression in
/// the block is the block's value. Creates a new scope.
///
/// ## Example
/// ```
/// var x = {
///     var temp = compute();
///     process(temp);
///     temp * 2  // This is the block's value
/// };
/// ```
struct BlockExpr : Expr {
    /// @brief The statements executed before the final expression.
    std::vector<StmtPtr> statements;

    /// @brief The final expression whose value becomes the block's value.
    ExprPtr value;

    /// @brief Construct a block expression.
    /// @param l Source location.
    /// @param s The statements.
    /// @param v The final value expression.
    BlockExpr(SourceLoc l, std::vector<StmtPtr> s, ExprPtr v)
        : Expr(ExprKind::Block, l), statements(std::move(s)), value(std::move(v)) {}
};

/// @brief Expression statement: `f();`, `x = 5;`.
/// @details Evaluates an expression for its side effects, discarding the value.
struct ExprStmt : Stmt {
    /// @brief The expression to evaluate.
    ExprPtr expr;

    /// @brief Construct an expression statement.
    /// @param l Source location.
    /// @param e The expression.
    ExprStmt(SourceLoc l, ExprPtr e) : Stmt(StmtKind::Expr, l), expr(std::move(e)) {}
};

/// @brief Variable declaration statement: `var x = 1;` or `final x = 1;`.
/// @details Introduces a new local variable with optional type and initializer.
/// Variables declared with `final` cannot be reassigned after initialization.
///
/// ## Examples
/// - `var x = 1;` - Mutable integer (type inferred)
/// - `var x: Integer = 1;` - Mutable integer (explicit type)
/// - `final PI = 3.14159;` - Immutable constant
struct VarStmt : Stmt {
    /// @brief The variable name.
    std::string name;

    /// @brief True when this declaration destructures a tuple.
    bool isTupleDestructure = false;

    /// @brief The second variable name for legacy two-element tuple destructuring consumers.
    std::string secondName;

    /// @brief Variable names for tuple destructuring, in tuple element order.
    std::vector<std::string> tupleNames;

    /// @brief The declared type (nullptr = inferred from initializer).
    TypePtr type;

    /// @brief The second declared type for legacy two-element tuple destructuring consumers.
    TypePtr secondType;

    /// @brief Declared types for tuple destructuring, in tuple element order.
    std::vector<TypePtr> tupleTypes;

    /// @brief The initializer expression (nullptr = default value).
    ExprPtr initializer;

    /// @brief True if declared with `final` (immutable).
    bool isFinal;

    /// @brief Construct a variable declaration.
    /// @param l Source location.
    /// @param n Variable name.
    /// @param t Type annotation (nullptr if inferred).
    /// @param init Initializer (nullptr for default).
    /// @param final True if immutable.
    VarStmt(SourceLoc l, std::string n, TypePtr t, ExprPtr init, bool final)
        : Stmt(StmtKind::Var, l), name(std::move(n)), type(std::move(t)),
          initializer(std::move(init)), isFinal(final) {}

    /// @brief Construct a tuple destructuring declaration.
    /// @param l Source location.
    /// @param names Declared tuple element names.
    /// @param types Optional type annotations in tuple element order.
    /// @param init Tuple-valued initializer.
    /// @param final True when the bindings are immutable.
    VarStmt(SourceLoc l,
            std::vector<std::string> names,
            std::vector<TypePtr> types,
            ExprPtr init,
            bool final)
        : Stmt(StmtKind::Var, l), isTupleDestructure(true), tupleNames(std::move(names)),
          tupleTypes(std::move(types)), initializer(std::move(init)), isFinal(final) {
        if (!tupleNames.empty())
            name = tupleNames[0];
        if (tupleNames.size() > 1)
            secondName = tupleNames[1];
    }

    /// @brief Construct a two-name tuple destructuring declaration.
    /// @param l Source location.
    /// @param first First declared name.
    /// @param firstType Optional annotation for the first element.
    /// @param second Second declared name.
    /// @param secondTypeAnnotation Optional annotation for the second element.
    /// @param init Tuple-valued initializer.
    /// @param final True when both bindings are immutable.
    VarStmt(SourceLoc l,
            std::string first,
            TypePtr firstType,
            std::string second,
            TypePtr secondTypeAnnotation,
            ExprPtr init,
            bool final)
        : Stmt(StmtKind::Var, l), name(std::move(first)), isTupleDestructure(true),
          secondName(std::move(second)), type(std::move(firstType)),
          secondType(std::move(secondTypeAnnotation)), initializer(std::move(init)),
          isFinal(final) {
        tupleNames.push_back(name);
        tupleNames.push_back(secondName);
    }
};

/// @brief Conditional if-statement: `if (c) { ... } else { ... }`.
/// @details Executes the then-branch if condition is true, else-branch otherwise.
/// Unlike if-expressions, the else-branch is optional.
struct IfStmt : Stmt {
    /// @brief The condition to test.
    ExprPtr condition;

    /// @brief The then-branch (executed if true).
    StmtPtr thenBranch;

    /// @brief The else-branch (nullptr if no else).
    StmtPtr elseBranch;

    /// @brief Construct an if-statement.
    /// @param l Source location.
    /// @param c Condition.
    /// @param t Then branch.
    /// @param e Else branch (nullptr if none).
    IfStmt(SourceLoc l, ExprPtr c, StmtPtr t, StmtPtr e)
        : Stmt(StmtKind::If, l), condition(std::move(c)), thenBranch(std::move(t)),
          elseBranch(std::move(e)) {}
};

/// @brief While loop statement: `while (c) { ... }`.
/// @details Repeatedly executes the body while condition is true.
struct WhileStmt : Stmt {
    /// @brief The loop condition.
    ExprPtr condition;

    /// @brief The loop body.
    StmtPtr body;

    /// @brief Construct a while statement.
    /// @param l Source location.
    /// @param c Condition.
    /// @param b Body.
    WhileStmt(SourceLoc l, ExprPtr c, StmtPtr b)
        : Stmt(StmtKind::While, l), condition(std::move(c)), body(std::move(b)) {}
};

/// @brief C-style for loop: `for (init; cond; update) { ... }`.
/// @details Traditional three-part for loop with initialization,
/// condition, and update expressions.
///
/// ## Example
/// ```
/// for (var i = 0; i < 10; i = i + 1) {
///     print(i);
/// }
/// ```
struct ForStmt : Stmt {
    /// @brief Initialization (VarStmt or ExprStmt).
    StmtPtr init;

    /// @brief Loop condition.
    ExprPtr condition;

    /// @brief Update expression (executed after each iteration).
    ExprPtr update;

    /// @brief Loop body.
    StmtPtr body;

    /// @brief Construct a for statement.
    /// @param l Source location.
    /// @param i Initialization.
    /// @param c Condition.
    /// @param u Update.
    /// @param b Body.
    ForStmt(SourceLoc l, StmtPtr i, ExprPtr c, ExprPtr u, StmtPtr b)
        : Stmt(StmtKind::For, l), init(std::move(i)), condition(std::move(c)), update(std::move(u)),
          body(std::move(b)) {}
};

/// @brief For-in loop statement: `for (x in collection) { ... }`.
/// @details Iterates over elements of a collection (List, Set, Range, etc.).
///
/// ## Examples
/// ```
/// for (item in myList) { ... }
/// for (i in 0..10) { ... }
/// for (key in myMap) { ... }
/// ```
struct ForInStmt : Stmt {
    /// @brief The loop variable name (bound to each element).
    std::string variable;

    /// @brief Optional explicit type for the loop variable.
    TypePtr variableType;

    /// @brief True if the loop binds a tuple (two variables).
    bool isTuple = false;

    /// @brief The second variable name for tuple bindings.
    std::string secondVariable;

    /// @brief Optional explicit type for the second tuple variable.
    TypePtr secondVariableType;

    /// @brief The collection to iterate over.
    ExprPtr iterable;

    /// @brief The loop body.
    StmtPtr body;

    /// @brief Construct a for-in statement.
    /// @param l Source location.
    /// @param v Loop variable name.
    /// @param i The iterable expression.
    /// @param b Body.
    ForInStmt(SourceLoc l, std::string v, ExprPtr i, StmtPtr b)
        : Stmt(StmtKind::ForIn, l), variable(std::move(v)), iterable(std::move(i)),
          body(std::move(b)) {}

    /// @brief Construct a tuple-binding for-in statement.
    /// @param l Source location.
    /// @param first First loop variable name.
    /// @param second Second loop variable name.
    /// @param i The iterable expression.
    /// @param b Body.
    ForInStmt(SourceLoc l, std::string first, std::string second, ExprPtr i, StmtPtr b)
        : Stmt(StmtKind::ForIn, l), variable(std::move(first)), isTuple(true),
          secondVariable(std::move(second)), iterable(std::move(i)), body(std::move(b)) {}
};

/// @brief Return statement: `return expr;`.
/// @details Returns from the current function with an optional value.
/// The struct type must match the function's return type.
struct ReturnStmt : Stmt {
    /// @brief The return value (nullptr for void/unit return).
    ExprPtr value;

    /// @brief Construct a return statement.
    /// @param l Source location.
    /// @param v Return value (nullptr for void).
    ReturnStmt(SourceLoc l, ExprPtr v) : Stmt(StmtKind::Return, l), value(std::move(v)) {}
};

/// @brief Break statement: `break;`.
/// @details Exits the innermost enclosing loop.
struct BreakStmt : Stmt {
    /// @brief Construct a break statement.
    /// @param l Source location.
    BreakStmt(SourceLoc l) : Stmt(StmtKind::Break, l) {}
};

/// @brief Continue statement: `continue;`.
/// @details Skips to the next iteration of the innermost enclosing loop.
struct ContinueStmt : Stmt {
    /// @brief Construct a continue statement.
    /// @param l Source location.
    ContinueStmt(SourceLoc l) : Stmt(StmtKind::Continue, l) {}
};

/// @brief Defer statement: `defer cleanup();` or `defer { cleanup(); }`.
/// @details Registers an action to run when the current block exits.
struct DeferStmt : Stmt {
    /// @brief The deferred action, stored as a statement or block.
    StmtPtr action;

    /// @brief Construct a defer statement.
    /// @param l Source location.
    /// @param a Deferred action.
    DeferStmt(SourceLoc l, StmtPtr a) : Stmt(StmtKind::Defer, l), action(std::move(a)) {}
};

/// @brief Guard statement: `guard (c) else { return; }`.
/// @details An early-exit pattern: if condition is false, executes the
/// else-block which must exit the scope (return, break, continue, throw).
///
/// ## Example
/// ```
/// guard (user != null) else {
///     return null;
/// }
/// // user is now known to be non-null
/// ```
struct GuardStmt : Stmt {
    /// @brief The condition that must be true to continue.
    ExprPtr condition;

    /// @brief The else-block executed if condition is false (must exit scope).
    StmtPtr elseBlock;

    /// @brief Construct a guard statement.
    /// @param l Source location.
    /// @param c Condition.
    /// @param e Else block.
    GuardStmt(SourceLoc l, ExprPtr c, StmtPtr e)
        : Stmt(StmtKind::Guard, l), condition(std::move(c)), elseBlock(std::move(e)) {}
};

/// @brief Pattern matching statement: `match x { ... }`.
/// @details Statement form of pattern matching. Unlike match expressions,
/// the arms don't need to return values.
///
/// ## Example
/// ```
/// match command {
///     "quit" => return;
///     "help" => showHelp();
///     _ => processCommand(command);
/// }
/// ```
struct MatchStmt : Stmt {
    /// @brief The value being matched.
    ExprPtr scrutinee;

    /// @brief The match arms.
    std::vector<MatchArm> arms;

    /// @brief Construct a match statement.
    /// @param l Source location.
    /// @param s The scrutinee.
    /// @param a The arms.
    MatchStmt(SourceLoc l, ExprPtr s, std::vector<MatchArm> a)
        : Stmt(StmtKind::Match, l), scrutinee(std::move(s)), arms(std::move(a)) {}
};

/// @brief Try/catch/finally statement.
/// @details Implements structured exception handling.
///
/// ## Example
/// ```
/// try {
///     riskyCode();
/// } catch(e) {
///     handleError(e);
/// } finally {
///     cleanup();
/// }
/// ```
struct TryStmt : Stmt {
    struct CatchClause {
        std::string var;
        std::string typeName;
        StmtPtr body;
        SourceLoc loc;
    };

    /// @brief The try body.
    StmtPtr tryBody;

    /// @brief Ordered catch clauses.
    std::vector<CatchClause> catches;

    /// @brief Catch variable name (empty if no catch clause).
    /// @deprecated Use catches instead. Kept for older code paths during migration.
    std::string catchVar;

    /// @brief Catch error type name for typed catch (empty for catch-all).
    /// @details When non-empty, only errors matching this TrapKind are caught.
    /// Unmatched errors are re-raised to the next handler in the chain.
    /// @deprecated Use catches instead. Kept for older code paths during migration.
    std::string catchTypeName;

    /// @brief Catch body (nullptr if no catch clause).
    /// @deprecated Use catches instead. Kept for older code paths during migration.
    StmtPtr catchBody;

    /// @brief Finally body (nullptr if no finally clause).
    StmtPtr finallyBody;

    /// @brief Construct a try statement.
    /// @param l Source location.
    TryStmt(SourceLoc l) : Stmt(StmtKind::Try, l) {}
};

/// @brief Throw statement.
/// @details Raises an exception with a value expression.
///
/// ## Example
/// ```
/// throw "something went wrong";
/// ```
struct ThrowStmt : Stmt {
    /// @brief The value to throw (may be nullptr for bare `throw;`).
    ExprPtr value;

    /// @brief Construct a throw statement.
    /// @param l Source location.
    /// @param v The value to throw.
    ThrowStmt(SourceLoc l, ExprPtr v) : Stmt(StmtKind::Throw, l), value(std::move(v)) {}
};

/// @}

} // namespace il::frontends::zia
