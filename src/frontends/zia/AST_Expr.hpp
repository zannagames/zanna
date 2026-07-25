//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/zia/AST_Expr.hpp
// Purpose: Define Zia expression nodes, operators, literals, calls, and pattern
//          matching expression data.
// Key invariants:
//   * Expr::kind identifies the concrete node used for visitor dispatch.
//   * Owning expression/type children are represented by unique_ptr aliases.
//   * Semantic metadata is populated after parsing without changing tree
//     ownership.
// Ownership: Expression nodes own their syntactic children; borrowed semantic
//            links are documented at their individual fields.
// References: docs/languages/zia-reference.md, docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//
///
/// @file
/// @brief Expression nodes for the Zia AST.
///
/// @details Defines all expression AST nodes produced by the Zia parser.
/// Expressions are the core of computation — they evaluate to values and can
/// be nested arbitrarily deep. This includes literals (integer, float, string,
/// bool), binary and unary operators, function calls, method calls, field
/// access, array/map indexing, object construction (`new`), lambda expressions,
/// string interpolation, range expressions, and control-flow expressions
/// (if-else expressions, match expressions, block expressions).
///
/// The parser uses precedence climbing to handle operator precedence and
/// associativity. Each expression node stores its source location for error
/// reporting and a resolved type slot that the semantic analyzer fills in.
///
/// During lowering, each expression kind maps to IL instructions: literals
/// become constants, operators become arithmetic/comparison ops, calls become
/// IL call instructions with argument marshalling, and field access becomes
/// pointer arithmetic with load instructions.
///
/// @invariant Every Expr has a valid `kind` field matching its concrete type.
/// @invariant Source locations are non-null for all user-written expressions.
///
/// Ownership/Lifetime: Owned by their parent expression or statement via
/// ExprPtr (std::unique_ptr<Expr>). Forms a tree, not a DAG.
///
/// @see AST_Types.hpp — type annotation nodes used in casts and generics.
/// @see Sema.hpp — performs type checking and fills in resolved types.
/// @see Lowerer.hpp — translates expression nodes into IL instructions.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "frontends/zia/AST_Types.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace il::frontends::zia {
//===----------------------------------------------------------------------===//
/// @name Expression Nodes
/// @brief AST nodes representing expressions that compute values.
/// @details Expressions are the core of computation in Zia. They can
/// be nested arbitrarily deep and include operators, function calls, field
/// access, and control flow constructs that return values.
/// @{
//===----------------------------------------------------------------------===//

/// @brief Enumerates all kinds of expression nodes.
/// @details Used for runtime type identification when processing expressions.
/// The enum is grouped by category for clarity:
/// - Literals: constant values
/// - Names: identifier references
/// - Operations: computations with operators
/// - Construction: creating new values/objects
/// - Control flow: expressions with branching
enum class ExprKind {
    // =========================================================================
    /// @name Literal Expressions
    /// @brief Constant values embedded directly in source code.
    /// @{
    // =========================================================================

    /// @brief 64-bit signed integer literal: `42`, `0xFF`, `0b1010`.
    /// @see IntLiteralExpr
    IntLiteral,

    /// @brief 64-bit floating-point literal: `3.14`, `1e-5`.
    /// @see NumberLiteralExpr
    NumberLiteral,

    /// @brief String literal: `"hello"`, including interpolated strings.
    /// @see StringLiteralExpr
    StringLiteral,

    /// @brief Boolean literal: `true` or `false`.
    /// @see BoolLiteralExpr
    BoolLiteral,

    /// @brief Null literal: `null`.
    /// @see NullLiteralExpr
    NullLiteral,

    /// @brief Unit literal: `()` - the singleton unit value.
    /// @see UnitLiteralExpr
    UnitLiteral,

    /// @}
    // =========================================================================
    /// @name Name Expressions
    /// @brief References to named entities.
    /// @{
    // =========================================================================

    /// @brief Identifier reference: `foo`, `myVariable`.
    /// @see IdentExpr
    Ident,

    /// @brief Self reference within a method: `self`.
    /// @see SelfExpr
    SelfExpr,

    /// @brief Parent class reference: `super`.
    /// @see SuperExprNode
    SuperExpr,

    /// @}
    // =========================================================================
    /// @name Operator Expressions
    /// @brief Expressions involving operators.
    /// @{
    // =========================================================================

    /// @brief Binary operation: `a + b`, `x && y`, `i = 5`.
    /// @see BinaryExpr
    Binary,

    /// @brief Unary operation: `-a`, `!b`, `~c`.
    /// @see UnaryExpr
    Unary,

    /// @brief Ternary conditional: `a ? b : c`.
    /// @see TernaryExpr
    Ternary,

    /// @brief Function/method call: `f(x, y)`, `obj.method(arg)`.
    /// @see CallExpr
    Call,

    /// @brief Array/collection indexing: `arr[i]`, `map[key]`.
    /// @see IndexExpr
    Index,

    /// @brief Field access: `obj.field`.
    /// @see FieldExpr
    Field,

    /// @brief Safe optional chain: `obj?.field` - returns null if obj is null.
    /// @see OptionalChainExpr
    OptionalChain,

    /// @brief Null coalescing: `a ?? b` - returns b if a is null.
    /// @see CoalesceExpr
    Coalesce,

    /// @brief Type check: `x is T` - tests if x is of type T.
    /// @see IsExpr
    Is,

    /// @brief Type cast: `x as T` - casts x to type T.
    /// @see AsExpr
    As,

    /// @brief Range expression: `a..b` or `a..=b`.
    /// @see RangeExpr
    Range,

    /// @brief Try/propagate expression: `expr?` - propagates null/error.
    /// @details Used with Result and Optional types to short-circuit
    /// on error or null, returning early from the enclosing function.
    /// @see TryExpr
    Try,

    /// @brief Force-unwrap expression: `expr!` - asserts non-null, traps if null.
    /// @details Converts an Optional[T] to T. If the value is null at runtime,
    /// the program traps (aborts). Use when you have already guarded against null
    /// or are certain the value is non-null.
    /// @see ForceUnwrapExpr
    ForceUnwrap,

    /// @brief Await expression: `await future` — suspends until Future resolves.
    /// @see AwaitExpr
    Await,

    /// @}
    // =========================================================================
    /// @name Construction Expressions
    /// @brief Expressions that create new values or objects.
    /// @{
    // =========================================================================

    /// @brief Object instantiation: `new Foo(args)`.
    /// @see NewExpr
    New,

    /// @brief Struct-literal initialization for struct types: `Point { x = 3, y = 4 }`.
    /// @see StructLiteralExpr
    StructLiteral,

    /// @brief Anonymous function: `(x: Integer) => x + 1`.
    /// @see LambdaExpr
    Lambda,

    /// @brief List literal: `[1, 2, 3]`.
    /// @see ListLiteralExpr
    ListLiteral,

    /// @brief Map literal: `{"a": 1, "b": 2}`.
    /// @see MapLiteralExpr
    MapLiteral,

    /// @brief Set literal: `{1, 2, 3}` (when not a map).
    /// @see SetLiteralExpr
    SetLiteral,

    /// @brief Tuple literal: `(1, "hello", true)`.
    /// @see TupleExpr
    Tuple,

    /// @brief Tuple element access: `tuple.0`, `tuple.1`.
    /// @see TupleIndexExpr
    TupleIndex,

    /// @}
    // =========================================================================
    /// @name Control Flow Expressions
    /// @brief Expressions with branching that return values.
    /// @{
    // =========================================================================

    /// @brief Conditional expression: `if (c) a else b`.
    /// @details Unlike if-statements, if-expressions require an else branch
    /// and evaluate to a value.
    /// @see IfExpr
    If,

    /// @brief Pattern matching expression: `match x { ... }`.
    /// @see MatchExpr
    Match,

    /// @brief Block expression: `{ stmts; expr }`.
    /// @details A block with a trailing expression evaluates to that expression.
    /// @see BlockExpr
    Block,

    /// @}
};

/// @brief Base class for all expression nodes.
/// @details Expressions compute values and can be composed arbitrarily.
/// Each expression has a source location and a kind for identification.
///
/// ## Type Resolution
/// During semantic analysis, each expression is assigned a ZannaType
/// indicating the type of value it produces. This is stored in the
/// Sema's expression type map, not in the AST node itself.
///
/// ## Subclass Categories
/// - Literals: IntLiteralExpr, NumberLiteralExpr, StringLiteralExpr, etc.
/// - Names: IdentExpr, SelfExpr, SuperExprNode
/// - Operators: BinaryExpr, UnaryExpr, TernaryExpr
/// - Access: FieldExpr, IndexExpr, CallExpr
/// - Construction: NewExpr, LambdaExpr, ListLiteralExpr
/// - Control: IfExpr, MatchExpr, BlockExpr
///
/// @invariant `kind` correctly identifies the concrete subclass type.
struct Expr {
    /// @brief Identifies the concrete expression kind for downcasting.
    ExprKind kind;

    /// @brief Source location of this expression.
    SourceLoc loc;

    /// @brief Construct an expression with kind and location.
    /// @param k The specific expression kind.
    /// @param l Source location of the expression.
    Expr(ExprKind k, SourceLoc l) : kind(k), loc(l) {}

    /// @brief Virtual destructor for proper polymorphic cleanup.
    virtual ~Expr() = default;
};

/// @brief 64-bit signed integer literal: `42`, `0xFF`, `0b1010`.
/// @details Represents compile-time integer constants. The lexer handles
/// decimal, hexadecimal (0x), and binary (0b) formats.
///
/// ## Examples
/// - `42` - Decimal integer
/// - `0xFF` - Hexadecimal (255 in decimal)
/// - `0b1010` - Binary (10 in decimal)
/// - `-123` - Negative integer (actually a unary minus on 123)
struct IntLiteralExpr : Expr {
    /// @brief The integer value.
    int64_t value;

    /// @brief Construct an integer literal.
    /// @param l Source location.
    /// @param v The integer value.
    IntLiteralExpr(SourceLoc l, int64_t v) : Expr(ExprKind::IntLiteral, l), value(v) {}
};

/// @brief 64-bit floating-point literal: `3.14`, `1e-5`.
/// @details Represents compile-time floating-point constants.
/// Scientific notation with optional exponent is supported.
///
/// ## Examples
/// - `3.14159` - Simple decimal
/// - `1e10` - Scientific notation (1 × 10^10)
/// - `2.5e-3` - Scientific with negative exponent (0.0025)
struct NumberLiteralExpr : Expr {
    /// @brief The floating-point value.
    double value;

    /// @brief Construct a number literal.
    /// @param l Source location.
    /// @param v The floating-point value.
    NumberLiteralExpr(SourceLoc l, double v) : Expr(ExprKind::NumberLiteral, l), value(v) {}
};

/// @brief String literal: `"hello"`, with interpolation support.
/// @details Represents string constants. Strings support:
/// - Escape sequences: `\n`, `\t`, `\\`, `\"`, `\$`
/// - Interpolation: `"Hello ${name}!"` embeds expressions
///
/// ## String Interpolation
/// Interpolated strings are desugared during parsing into a series of
/// string concatenation operations. This node represents the final
/// resolved string value after interpolation processing.
struct StringLiteralExpr : Expr {
    /// @brief The string value with escapes processed.
    std::string value;

    /// @brief Construct a string literal.
    /// @param l Source location.
    /// @param v The string value.
    StringLiteralExpr(SourceLoc l, std::string v)
        : Expr(ExprKind::StringLiteral, l), value(std::move(v)) {}
};

/// @brief Boolean literal: `true` or `false`.
/// @details Represents the two boolean constant values.
struct BoolLiteralExpr : Expr {
    /// @brief The boolean value.
    bool value;

    /// @brief Construct a boolean literal.
    /// @param l Source location.
    /// @param v The boolean value.
    BoolLiteralExpr(SourceLoc l, bool v) : Expr(ExprKind::BoolLiteral, l), value(v) {}
};

/// @brief Null literal: `null`.
/// @details Represents the absence of a value for optional types.
/// Only valid where an optional type is expected.
struct NullLiteralExpr : Expr {
    /// @brief Construct a null literal.
    /// @param l Source location.
    NullLiteralExpr(SourceLoc l) : Expr(ExprKind::NullLiteral, l) {}
};

/// @brief Unit literal: `()`.
/// @details Represents the singleton unit value, similar to void but
/// with an actual value. Used with Result[Unit] for operations that
/// succeed but return no meaningful data.
struct UnitLiteralExpr : Expr {
    /// @brief Construct a unit literal.
    /// @param l Source location.
    UnitLiteralExpr(SourceLoc l) : Expr(ExprKind::UnitLiteral, l) {}
};

/// @brief Identifier expression: `foo`, `myVariable`.
/// @details References a named symbol: variable, parameter, function, or type.
/// The semantic analyzer resolves the name to its definition.
struct IdentExpr : Expr {
    /// @brief The identifier name.
    std::string name;

    /// @brief Construct an identifier expression.
    /// @param l Source location.
    /// @param n The identifier name.
    IdentExpr(SourceLoc l, std::string n) : Expr(ExprKind::Ident, l), name(std::move(n)) {}
};

/// @brief Self reference within methods: `self`.
/// @details References the current object instance within a method.
/// Only valid inside method bodies of value or class types.
struct SelfExpr : Expr {
    /// @brief Construct a self expression.
    /// @param l Source location.
    SelfExpr(SourceLoc l) : Expr(ExprKind::SelfExpr, l) {}
};

/// @brief Parent class reference: `super`.
/// @details References the parent class for calling overridden methods
/// or accessing inherited members. Only valid in class types that
/// extend another class.
struct SuperExprNode : Expr {
    /// @brief Construct a super expression.
    /// @param l Source location.
    SuperExprNode(SourceLoc l) : Expr(ExprKind::SuperExpr, l) {}
};

/// @brief Binary operators for BinaryExpr.
/// @details Organized by category: arithmetic, comparison, logical, bitwise.
enum class BinaryOp {
    // Arithmetic operators
    Add, ///< Addition: `a + b`
    Sub, ///< Subtraction: `a - b`
    Mul, ///< Multiplication: `a * b`
    Div, ///< Division: `a / b`
    Mod, ///< Modulo: `a % b`

    // Comparison operators
    Eq, ///< Equality: `a == b`
    Ne, ///< Inequality: `a != b`
    Lt, ///< Less than: `a < b`
    Le, ///< Less or equal: `a <= b`
    Gt, ///< Greater than: `a > b`
    Ge, ///< Greater or equal: `a >= b`

    // Logical operators
    And, ///< Logical AND: `a && b` (short-circuiting)
    Or,  ///< Logical OR: `a || b` (short-circuiting)

    // Bitwise operators
    BitAnd, ///< Bitwise AND: `a & b`
    BitOr,  ///< Bitwise OR: `a | b`
    BitXor, ///< Bitwise XOR: `a ^ b`
    Shl,    ///< Left shift: `a << b`
    Shr,    ///< Right shift: `a >> b`

    // Assignment
    Assign, ///< Assignment: `a = b`
};

/// @brief Binary operation expression: `a + b`, `x && y`, `i = 5`.
/// @details Represents operations with two operands. The operator determines
/// the semantics: arithmetic, comparison, logical, bitwise, or assignment.
///
/// ## Precedence
/// Binary expressions are parsed with precedence climbing:
/// 1. Multiplicative: `*`, `/`, `%`
/// 2. Additive: `+`, `-`
/// 3. Comparison: `<`, `>`, `<=`, `>=`
/// 4. Equality: `==`, `!=`
/// 5. Logical AND: `&&`
/// 6. Logical OR: `||`
///
/// ## Short-Circuit Evaluation
/// Logical AND and OR use short-circuit evaluation:
/// - `a && b`: b is only evaluated if a is true
/// - `a || b`: b is only evaluated if a is false
struct BinaryExpr : Expr {
    /// @brief The binary operator.
    BinaryOp op;

    /// @brief The left operand.
    ExprPtr left;

    /// @brief The right operand.
    ExprPtr right;

    /// @brief Construct a binary expression.
    /// @param l Source location.
    /// @param o The operator.
    /// @param lhs Left operand.
    /// @param rhs Right operand.
    BinaryExpr(SourceLoc l, BinaryOp o, ExprPtr lhs, ExprPtr rhs)
        : Expr(ExprKind::Binary, l), op(o), left(std::move(lhs)), right(std::move(rhs)) {}
};

/// @brief Unary operators for UnaryExpr.
enum class UnaryOp {
    Neg,       ///< Arithmetic negation: `-a`
    Not,       ///< Logical NOT: `!a`
    BitNot,    ///< Bitwise NOT: `~a`
    AddressOf, ///< Address-of / function reference: `&func`
};

/// @brief Unary operation expression: `-a`, `!b`, `~c`.
/// @details Represents operations with a single operand.
struct UnaryExpr : Expr {
    /// @brief The unary operator.
    UnaryOp op;

    /// @brief The operand.
    ExprPtr operand;

    /// @brief Construct a unary expression.
    /// @param l Source location.
    /// @param o The operator.
    /// @param e The operand.
    UnaryExpr(SourceLoc l, UnaryOp o, ExprPtr e)
        : Expr(ExprKind::Unary, l), op(o), operand(std::move(e)) {}
};

/// @brief Ternary conditional expression: `a ? b : c`.
/// @details Evaluates condition, then returns thenExpr if true, elseExpr if false.
/// Both branches must have compatible types.
struct TernaryExpr : Expr {
    /// @brief The condition to test.
    ExprPtr condition;

    /// @brief Expression to evaluate if condition is true.
    ExprPtr thenExpr;

    /// @brief Expression to evaluate if condition is false.
    ExprPtr elseExpr;

    /// @brief Construct a ternary expression.
    /// @param l Source location.
    /// @param c Condition.
    /// @param t Then branch.
    /// @param e Else branch.
    TernaryExpr(SourceLoc l, ExprPtr c, ExprPtr t, ExprPtr e)
        : Expr(ExprKind::Ternary, l), condition(std::move(c)), thenExpr(std::move(t)),
          elseExpr(std::move(e)) {}
};

/// @brief Named or positional argument in a function call.
/// @details Zia supports both positional and named arguments.
/// Named arguments improve readability for functions with many parameters.
///
/// ## Examples
/// - `f(1, 2)` - Two positional arguments
/// - `f(x: 1, y: 2)` - Two named arguments
/// - `f(1, y: 2)` - Mixed positional and named
struct CallArg {
    /// @brief The argument name if using named syntax, nullopt for positional.
    std::optional<std::string> name;

    /// @brief The argument value expression.
    ExprPtr value;
};

/// @brief Function/method call expression: `f(x, y)`, `obj.method(arg)`.
/// @details Represents invocation of a callable with arguments.
/// The callee can be:
/// - An identifier (function name)
/// - A field expression (method call)
/// - Any expression evaluating to a callable type (lambda)
struct CallExpr : Expr {
    /// @brief The expression being called.
    ExprPtr callee;

    /// @brief The arguments passed to the call.
    std::vector<CallArg> args;

    /// @brief Construct a call expression.
    /// @param l Source location.
    /// @param c The callee expression.
    /// @param a The argument list.
    CallExpr(SourceLoc l, ExprPtr c, std::vector<CallArg> a)
        : Expr(ExprKind::Call, l), callee(std::move(c)), args(std::move(a)) {}
};

/// @brief Array/collection indexing expression: `arr[i]`, `map[key]`.
/// @details Accesses an element from a collection by index or key.
/// Works with List (integer index), Map (key lookup), and String (character).
struct IndexExpr : Expr {
    /// @brief The collection being indexed.
    ExprPtr base;

    /// @brief The index or key expression.
    ExprPtr index;

    /// @brief Construct an index expression.
    /// @param l Source location.
    /// @param b The base collection.
    /// @param i The index/key.
    IndexExpr(SourceLoc l, ExprPtr b, ExprPtr i)
        : Expr(ExprKind::Index, l), base(std::move(b)), index(std::move(i)) {}
};

/// @brief Field access expression: `obj.field`.
/// @details Accesses a field or property from a value or class type.
/// Also used for accessing static members and module-level items.
struct FieldExpr : Expr {
    /// @brief The object expression.
    ExprPtr base;

    /// @brief The field name being accessed.
    std::string field;

    /// @brief Construct a field expression.
    /// @param l Source location.
    /// @param b The base object.
    /// @param f The field name.
    FieldExpr(SourceLoc l, ExprPtr b, std::string f)
        : Expr(ExprKind::Field, l), base(std::move(b)), field(std::move(f)) {}
};

/// @brief Safe optional chain expression: `obj?.field`.
/// @details Safely accesses a field from an optional type. If the base
/// is null, the entire expression evaluates to null instead of crashing.
///
/// ## Example
/// ```
/// var user: User? = getUser();
/// var name = user?.name;  // String? - null if user is null
/// ```
struct OptionalChainExpr : Expr {
    /// @brief The optional object expression.
    ExprPtr base;

    /// @brief The field to access if base is not null.
    std::string field;

    /// @brief Construct an optional chain expression.
    /// @param l Source location.
    /// @param b The optional base.
    /// @param f The field name.
    OptionalChainExpr(SourceLoc l, ExprPtr b, std::string f)
        : Expr(ExprKind::OptionalChain, l), base(std::move(b)), field(std::move(f)) {}
};

/// @brief Null coalescing expression: `a ?? b`.
/// @details Returns the left operand if it's not null, otherwise returns
/// the right operand. The right operand is only evaluated if needed.
///
/// ## Example
/// ```
/// var name = user?.name ?? "Anonymous";
/// ```
struct CoalesceExpr : Expr {
    /// @brief The primary value (may be null).
    ExprPtr left;

    /// @brief The fallback value if left is null.
    ExprPtr right;

    /// @brief Construct a coalesce expression.
    /// @param l Source location.
    /// @param lhs Primary value.
    /// @param rhs Fallback value.
    CoalesceExpr(SourceLoc l, ExprPtr lhs, ExprPtr rhs)
        : Expr(ExprKind::Coalesce, l), left(std::move(lhs)), right(std::move(rhs)) {}
};

/// @brief Type check expression: `x is T`.
/// @details Tests at runtime whether a value is of a specific type.
/// Returns true if x is of type T, false otherwise.
///
/// ## Usage
/// Used with class types to check for subtypes before casting:
/// ```
/// if (animal is Dog) {
///     var dog = animal as Dog;
///     dog.bark();
/// }
/// ```
struct IsExpr : Expr {
    /// @brief The value to check.
    ExprPtr value;

    /// @brief The type to test against.
    TypePtr type;

    /// @brief Construct an is-expression.
    /// @param l Source location.
    /// @param v The value to check.
    /// @param t The type to test.
    IsExpr(SourceLoc l, ExprPtr v, TypePtr t)
        : Expr(ExprKind::Is, l), value(std::move(v)), type(std::move(t)) {}
};

/// @brief Type cast expression: `x as T`.
/// @details Casts a value to a specific type. The cast may be:
/// - Checked: For class types, throws if the cast fails
/// - Unchecked: For struct types, assumes the programmer knows the type
///
/// ## Example
/// ```
/// var dog = animal as Dog;  // Throws if not a Dog
/// ```
struct AsExpr : Expr {
    /// @brief The value to cast.
    ExprPtr value;

    /// @brief The target type.
    TypePtr type;

    /// @brief Construct an as-expression.
    /// @param l Source location.
    /// @param v The value to cast.
    /// @param t The target type.
    AsExpr(SourceLoc l, ExprPtr v, TypePtr t)
        : Expr(ExprKind::As, l), value(std::move(v)), type(std::move(t)) {}
};

/// @brief Range expression: `a..b` or `a..=b`.
/// @details Creates a range of values from start to end.
/// - `a..b`: Exclusive range [a, b)
/// - `a..=b`: Inclusive range [a, b]
///
/// ## Usage
/// Primarily used in for-in loops:
/// ```
/// for (i in 0..10) { ... }     // 0 to 9
/// for (i in 0..=10) { ... }    // 0 to 10
/// ```
struct RangeExpr : Expr {
    /// @brief The start of the range.
    ExprPtr start;

    /// @brief The end of the range.
    ExprPtr end;

    /// @brief True for inclusive (`..=`), false for exclusive (`..`).
    bool inclusive;

    /// @brief Construct a range expression.
    /// @param l Source location.
    /// @param s Start of range.
    /// @param e End of range.
    /// @param incl True if inclusive.
    RangeExpr(SourceLoc l, ExprPtr s, ExprPtr e, bool incl)
        : Expr(ExprKind::Range, l), start(std::move(s)), end(std::move(e)), inclusive(incl) {}
};

/// @brief Try/propagate expression: `expr?`.
/// @details Used with Optional and Result types to propagate null/error
/// to the enclosing function. If the expression is null/error, the
/// enclosing function returns early with the same null/error.
///
/// ## Example
/// ```
/// func getUsername(): String? {
///     var user = getUser()?;  // Returns null if getUser() returns null
///     return user.name;
/// }
/// ```
struct TryExpr : Expr {
    /// @brief The expression to try (must be Optional or Result type).
    ExprPtr operand;

    /// @brief Construct a try expression.
    /// @param l Source location.
    /// @param e The operand expression.
    TryExpr(SourceLoc l, ExprPtr e) : Expr(ExprKind::Try, l), operand(std::move(e)) {}
};

/// @brief Force-unwrap expression: `expr!`.
/// @details Converts an Optional[T] to T. If the value is null at runtime,
/// the program traps (aborts). Use when you have already guarded against null
/// or are certain the value is non-null.
///
/// ## Example
/// ```
/// var page = pool.fetchPage(id)!;  // Traps if null
/// ```
struct ForceUnwrapExpr : Expr {
    /// @brief The expression to force-unwrap (must be Optional type).
    ExprPtr operand;

    /// @brief Construct a force-unwrap expression.
    /// @param l Source location.
    /// @param e The operand expression.
    ForceUnwrapExpr(SourceLoc l, ExprPtr e)
        : Expr(ExprKind::ForceUnwrap, l), operand(std::move(e)) {}
};

/// @brief Await expression: `await future`.
/// @details Suspends the current async function until the given Future resolves
/// to a value. The result type is the inner type of the Future.
///
/// ## Example
/// ```
/// let result = await Async.Run(fetchData, url)
/// ```
struct AwaitExpr : Expr {
    /// @brief The future expression to await.
    ExprPtr operand;

    /// @brief Construct an await expression.
    /// @param l Source location.
    /// @param e The future operand expression.
    AwaitExpr(SourceLoc l, ExprPtr e) : Expr(ExprKind::Await, l), operand(std::move(e)) {}
};

/// @brief Object instantiation expression: `new Foo(args)`.
/// @details Creates a new instance of an class type by invoking its
/// constructor. Class types are reference types with identity.
///
/// ## Example
/// ```
/// var player = new Player("Alice", 100);
/// ```
struct NewExpr : Expr {
    /// @brief The type to instantiate.
    TypePtr type;

    /// @brief Constructor arguments.
    std::vector<CallArg> args;

    /// @brief Construct a new expression.
    /// @param l Source location.
    /// @param t The type to create.
    /// @param a Constructor arguments.
    NewExpr(SourceLoc l, TypePtr t, std::vector<CallArg> a)
        : Expr(ExprKind::New, l), type(std::move(t)), args(std::move(a)) {}
};

/// @brief Lambda parameter specification.
/// @details Represents one parameter of a lambda expression.
/// Lambda parameters always carry an explicit type annotation.
struct LambdaParam {
    /// @brief Parameter name.
    std::string name;

    /// @brief Parameter type.
    TypePtr type;
};

/// @brief Captured variable in a closure.
/// @details Represents a variable captured from the enclosing scope.
struct CapturedVar {
    /// @brief Variable name.
    std::string name;
};

/// @brief Anonymous function expression: `(x: Integer) => x + 1`.
/// @details Creates a callable lambda that captures its environment.
/// Lambdas use typed parameters and optional return type inference.
///
/// ## Examples
/// - `(x: Integer) => x * 2` - Single parameter
/// - `(a: Integer, b: Integer) => a + b` - Multiple parameters
/// - `() => 42` - No parameters
struct LambdaExpr : Expr {
    /// @brief Lambda parameters.
    std::vector<LambdaParam> params;

    /// @brief Return type (nullptr if inferred).
    TypePtr returnType;

    /// @brief Lambda body expression.
    ExprPtr body;

    /// @brief Variables captured from enclosing scope (populated during sema).
    mutable std::vector<CapturedVar> captures;

    /// @brief Construct a lambda expression.
    /// @param l Source location.
    /// @param p Parameters.
    /// @param ret Return type (nullptr if inferred).
    /// @param b Body expression.
    LambdaExpr(SourceLoc l, std::vector<LambdaParam> p, TypePtr ret, ExprPtr b)
        : Expr(ExprKind::Lambda, l), params(std::move(p)), returnType(std::move(ret)),
          body(std::move(b)) {}
};

/// @brief List literal expression: `[1, 2, 3]`.
/// @details Creates a new List containing the given elements.
/// Element type is inferred from the elements or context.
struct ListLiteralExpr : Expr {
    /// @brief The list elements.
    std::vector<ExprPtr> elements;

    /// @brief Construct a list literal.
    /// @param l Source location.
    /// @param e The elements.
    ListLiteralExpr(SourceLoc l, std::vector<ExprPtr> e)
        : Expr(ExprKind::ListLiteral, l), elements(std::move(e)) {}
};

/// @brief Key-value entry in a map literal.
struct MapEntry {
    /// @brief The key expression.
    ExprPtr key;

    /// @brief The value expression.
    ExprPtr value;
};

/// @brief Map literal expression: `{"a": 1, "b": 2}`.
/// @details Creates a new Map with the given key-value pairs.
/// Key and struct types are inferred from the entries or context.
struct MapLiteralExpr : Expr {
    /// @brief The map entries.
    std::vector<MapEntry> entries;

    /// @brief Construct a map literal.
    /// @param l Source location.
    /// @param e The entries.
    MapLiteralExpr(SourceLoc l, std::vector<MapEntry> e)
        : Expr(ExprKind::MapLiteral, l), entries(std::move(e)) {}
};

/// @brief Set literal expression: `{1, 2, 3}`.
/// @details Creates a new Set containing the given unique elements.
/// Distinguished from map literals by lacking key-value pairs.
struct SetLiteralExpr : Expr {
    /// @brief The set elements.
    std::vector<ExprPtr> elements;

    /// @brief Construct a set literal.
    /// @param l Source location.
    /// @param e The elements.
    SetLiteralExpr(SourceLoc l, std::vector<ExprPtr> e)
        : Expr(ExprKind::SetLiteral, l), elements(std::move(e)) {}
};

/// @brief Tuple literal expression: `(1, "hello", true)`.
/// @details Creates a tuple containing multiple values of potentially
/// different types. Tuples have fixed size and element types.
///
/// ## Examples
/// - `(1, 2)` - Pair of integers
/// - `(x, "name", true)` - Mixed types
/// - `(point.x, point.y)` - From field access
struct TupleExpr : Expr {
    /// @brief The tuple elements.
    std::vector<ExprPtr> elements;

    /// @brief Construct a tuple literal.
    /// @param l Source location.
    /// @param e The elements.
    TupleExpr(SourceLoc l, std::vector<ExprPtr> e)
        : Expr(ExprKind::Tuple, l), elements(std::move(e)) {}
};

/// @brief Tuple element access expression: `tuple.0`, `tuple.1`.
/// @details Accesses an element of a tuple by its index. The index
/// must be a compile-time constant within the tuple's bounds.
///
/// ## Examples
/// - `pair.0` - First element
/// - `pair.1` - Second element
/// - `triple.2` - Third element
struct TupleIndexExpr : Expr {
    /// @brief The tuple being accessed.
    ExprPtr tuple;

    /// @brief The element index (0-based).
    size_t index;

    /// @brief Construct a tuple index expression.
    /// @param l Source location.
    /// @param t The tuple expression.
    /// @param i The element index.
    TupleIndexExpr(SourceLoc l, ExprPtr t, size_t i)
        : Expr(ExprKind::TupleIndex, l), tuple(std::move(t)), index(i) {}
};

// Forward declare BlockExpr (defined after statements)
struct BlockExpr;

/// @brief Conditional if-expression: `if (c) a else b`.
/// @details Unlike if-statements, if-expressions require an else branch
/// and evaluate to a value. Both branches must have compatible types.
///
/// ## Example
/// ```
/// var max = if (a > b) a else b;
/// ```
struct IfExpr : Expr {
    /// @brief The condition to test.
    ExprPtr condition;

    /// @brief Expression evaluated if condition is true.
    ExprPtr thenBranch;

    /// @brief Expression evaluated if condition is false (required).
    ExprPtr elseBranch;

    /// @brief Construct an if-expression.
    /// @param l Source location.
    /// @param c Condition.
    /// @param t Then branch.
    /// @param e Else branch.
    IfExpr(SourceLoc l, ExprPtr c, ExprPtr t, ExprPtr e)
        : Expr(ExprKind::If, l), condition(std::move(c)), thenBranch(std::move(t)),
          elseBranch(std::move(e)) {}
};

/// @brief Struct-literal initialization for struct types.
/// @details `Point { x = 3, y = 4 }` initializes a struct type by field name.
/// Each field may appear in any order; the lowerer reorders by declaration order.
struct StructLiteralExpr : Expr {
    /// @brief One named-field initializer.
    struct Field {
        std::string name; ///< Field name as written in source.
        ExprPtr value;    ///< Initializer expression for this field.
        SourceLoc loc;    ///< Location of this field entry.
    };

    /// @brief The struct type name (e.g., "Point").
    std::string typeName;

    /// @brief Named field initializers (in source order).
    std::vector<Field> fields;

    /// @brief Construct a struct literal with named field initializers.
    /// @param l Source location of the literal.
    /// @param name Struct type name as written.
    /// @param fs Owned field initializers in source order.
    StructLiteralExpr(SourceLoc l, std::string name, std::vector<Field> fs)
        : Expr(ExprKind::StructLiteral, l), typeName(std::move(name)), fields(std::move(fs)) {}
};

/// @brief Pattern matching arm: `Pattern => Expr`.
/// @details Represents one case in a match expression, with a pattern
/// to match against and an expression to evaluate if matched.
struct MatchArm {
    /// @brief Pattern to match against the scrutinee.
    struct Pattern {
        /// @brief The kinds of patterns supported.
        enum class Kind {
            /// @brief Wildcard pattern: `_` matches anything.
            Wildcard,

            /// @brief Literal pattern: matches a specific value.
            Literal,

            /// @brief Binding pattern: binds a name to the matched value.
            Binding,

            /// @brief Constructor pattern: matches and destructs a type.
            Constructor,

            /// @brief Tuple pattern: matches tuple structure.
            Tuple,

            /// @brief Expression pattern: evaluates expression and matches if true.
            /// Used for guard-style matching: `match (true) { x > 0 => ... }`
            Expression,

            /// @brief OR pattern: matches if any subpattern matches.
            /// Syntax: `1 | 2 | 3 => ...`
            /// Subpatterns stored in `subpatterns`. No bindings allowed.
            Or
        };

        /// @brief The pattern kind.
        Kind kind{Kind::Wildcard};

        /// @brief Name for Binding patterns, type name for Constructor.
        std::string binding;

        /// @brief Nested patterns for Constructor and Tuple.
        std::vector<Pattern> subpatterns;

        /// @brief The literal value for Literal patterns.
        ExprPtr literal;

        /// @brief Optional guard condition that must be true to match.
        ExprPtr guard;
    };

    /// @brief The pattern to match.
    Pattern pattern;

    /// @brief The expression to evaluate if pattern matches.
    ExprPtr body;
};

/// @brief Pattern matching expression: `match value { ... }`.
/// @details Matches a value against multiple patterns and evaluates
/// the body of the first matching arm.
///
/// ## Example
/// ```
/// var desc = match status {
///     0 => "idle";
///     1 => "running";
///     _ => "unknown";
/// };
/// ```
struct MatchExpr : Expr {
    /// @brief The value being matched against.
    ExprPtr scrutinee;

    /// @brief The match arms in order.
    std::vector<MatchArm> arms;

    /// @brief Construct a match expression.
    /// @param l Source location.
    /// @param s The scrutinee.
    /// @param a The match arms.
    MatchExpr(SourceLoc l, ExprPtr s, std::vector<MatchArm> a)
        : Expr(ExprKind::Match, l), scrutinee(std::move(s)), arms(std::move(a)) {}
};

/// @}


} // namespace il::frontends::zia
