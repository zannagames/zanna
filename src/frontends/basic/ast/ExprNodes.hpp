//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/ast/ExprNodes.hpp
// Purpose: Defines the complete BASIC expression hierarchy, immutable/mutable
//          visitor contracts, node discriminators, and owned child payloads.
// Key invariants:
//   - Expr::Kind is fixed by each concrete constructor and agrees with its type.
//   - Every expression retains a source location for diagnostics.
//   - Child ExprPtr values uniquely own their expression subtrees.
//   - accept dispatches to the visitor overload matching the concrete node.
// Ownership/Lifetime:
//   - Expression trees use unique ownership; visitors borrow nodes only for the
//     duration of each callback.
//   - Strings, qualified-name segments, and argument vectors are node-owned.
// Links: src/frontends/basic/ast/NodeFwd.hpp,
//        src/frontends/basic/AST.cpp,
//        src/frontends/basic/AstWalker.hpp,
//        src/frontends/basic/Parser_Expr.cpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "frontends/basic/ast/NodeFwd.hpp"
#include "support/source_location.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

/// @file
/// @brief Defines BASIC expression AST nodes and double-dispatch visitors.

namespace il::frontends::basic {

/// @brief Qualified identifier with dotted segments and source location.
struct QualifiedName {
    std::vector<std::string> segments; ///< Dotted path in declaration order.
    il::support::SourceLoc loc{};      ///< Source location of the first segment.
};

/// @brief Visitor interface for BASIC expressions.
/// @details Implementations provide one callback for every concrete expression
///          type. Nodes are passed as immutable borrowed references.
struct ExprVisitor {
    /// @brief Enables polymorphic destruction of immutable visitors.
    virtual ~ExprVisitor() = default;

    /// @brief Visits a borrowed integer literal node.
    virtual void visit(const IntExpr &) = 0;
    /// @brief Visits a borrowed floating literal node.
    virtual void visit(const FloatExpr &) = 0;
    /// @brief Visits a borrowed string literal node.
    virtual void visit(const StringExpr &) = 0;
    /// @brief Visits a borrowed Boolean literal node.
    virtual void visit(const BoolExpr &) = 0;
    /// @brief Visits a borrowed scalar variable-reference node.
    virtual void visit(const VarExpr &) = 0;
    /// @brief Visits a borrowed array element-access node.
    virtual void visit(const ArrayExpr &) = 0;
    /// @brief Visits a borrowed unary-operation node.
    virtual void visit(const UnaryExpr &) = 0;
    /// @brief Visits a borrowed binary-operation node.
    virtual void visit(const BinaryExpr &) = 0;
    /// @brief Visits a borrowed built-in call node.
    virtual void visit(const BuiltinCallExpr &) = 0;
    /// @brief Visits a borrowed LBOUND query node.
    virtual void visit(const LBoundExpr &) = 0;
    /// @brief Visits a borrowed UBOUND query node.
    virtual void visit(const UBoundExpr &) = 0;
    /// @brief Visits a borrowed user procedure-call node.
    virtual void visit(const CallExpr &) = 0;
    /// @brief Visits a borrowed object-allocation node.
    virtual void visit(const NewExpr &) = 0;
    /// @brief Visits a borrowed implicit-receiver node.
    virtual void visit(const MeExpr &) = 0;
    /// @brief Visits a borrowed object member-access node.
    virtual void visit(const MemberAccessExpr &) = 0;
    /// @brief Visits a borrowed instance method-call node.
    virtual void visit(const MethodCallExpr &) = 0;
    /// @brief Visits a borrowed runtime type-test node.
    virtual void visit(const IsExpr &) = 0;
    /// @brief Visits a borrowed type-ascription or cast node.
    virtual void visit(const AsExpr &) = 0;
    /// @brief Visits a borrowed procedure-address node.
    virtual void visit(const AddressOfExpr &) = 0;
};

/// @brief Visitor interface for mutable BASIC expressions.
/// @details Implementations may rewrite node payloads while unique subtree
///          ownership remains with the surrounding AST.
struct MutExprVisitor {
    /// @brief Enables polymorphic destruction of mutable visitors.
    virtual ~MutExprVisitor() = default;

    /// @brief Visits a borrowed mutable integer literal node.
    virtual void visit(IntExpr &) = 0;
    /// @brief Visits a borrowed mutable floating literal node.
    virtual void visit(FloatExpr &) = 0;
    /// @brief Visits a borrowed mutable string literal node.
    virtual void visit(StringExpr &) = 0;
    /// @brief Visits a borrowed mutable Boolean literal node.
    virtual void visit(BoolExpr &) = 0;
    /// @brief Visits a borrowed mutable variable-reference node.
    virtual void visit(VarExpr &) = 0;
    /// @brief Visits a borrowed mutable array-access node.
    virtual void visit(ArrayExpr &) = 0;
    /// @brief Visits a borrowed mutable unary-operation node.
    virtual void visit(UnaryExpr &) = 0;
    /// @brief Visits a borrowed mutable binary-operation node.
    virtual void visit(BinaryExpr &) = 0;
    /// @brief Visits a borrowed mutable built-in-call node.
    virtual void visit(BuiltinCallExpr &) = 0;
    /// @brief Visits a borrowed mutable LBOUND query node.
    virtual void visit(LBoundExpr &) = 0;
    /// @brief Visits a borrowed mutable UBOUND query node.
    virtual void visit(UBoundExpr &) = 0;
    /// @brief Visits a borrowed mutable user-call node.
    virtual void visit(CallExpr &) = 0;
    /// @brief Visits a borrowed mutable allocation node.
    virtual void visit(NewExpr &) = 0;
    /// @brief Visits a borrowed mutable ME-reference node.
    virtual void visit(MeExpr &) = 0;
    /// @brief Visits a borrowed mutable member-access node.
    virtual void visit(MemberAccessExpr &) = 0;
    /// @brief Visits a borrowed mutable method-call node.
    virtual void visit(MethodCallExpr &) = 0;
    /// @brief Visits a borrowed mutable IS-expression node.
    virtual void visit(IsExpr &) = 0;
    /// @brief Visits a borrowed mutable AS-expression node.
    virtual void visit(AsExpr &) = 0;
    /// @brief Visits a borrowed mutable ADDRESSOF-expression node.
    virtual void visit(AddressOfExpr &) = 0;
};

/// @brief Base class for all BASIC expressions.
struct Expr {
    /// @brief Discriminator identifying the concrete expression subclass.
    enum class Kind {
        Int,
        Float,
        String,
        Bool,
        Var,
        Array,
        LBound,
        UBound,
        Unary,
        Binary,
        BuiltinCall,
        Call,
        New,
        Me,
        MemberAccess,
        MethodCall,
        Is,
        As,
        AddressOf,
    };

    /// Source location at which the expression begins.
    il::support::SourceLoc loc{};

    /// @brief Enables polymorphic destruction of owned expression nodes.
    virtual ~Expr() = default;

    /// @brief Retrieve the discriminator for this expression.
    /// @return Concrete kind fixed during construction.
    [[nodiscard]] constexpr Kind kind() const noexcept {
        return kind_;
    }

    /// @brief Accept a visitor to process this expression.
    /// @param visitor Immutable visitor receiving the concrete callback.
    virtual void accept(ExprVisitor &visitor) const = 0;

    /// @brief Accept a mutable visitor to process this expression.
    /// @param visitor Mutable visitor receiving the concrete callback.
    virtual void accept(MutExprVisitor &visitor) = 0;

  protected:
    /// @brief Construct expression with specified kind.
    /// @param k Concrete discriminator retained for the node's lifetime.
    constexpr explicit Expr(Kind k) noexcept : kind_(k) {}

  private:
    /// Immutable-by-interface concrete node discriminator.
    Kind kind_;
};

/// @brief Signed integer literal expression.
struct IntExpr : Expr {
    /// @brief Creates an integer node with @ref Kind::Int and value zero.
    IntExpr() : Expr(Kind::Int) {}

    /// Literal 64-bit numeric value parsed from the source.
    std::int64_t value{0};
    /// Optional BASIC suffix enforcing INTEGER or LONG semantics.
    enum class Suffix {
        /// No explicit integer literal suffix.
        None,
        /// INTEGER suffix spelling.
        Integer,
        /// LONG suffix spelling.
        Long,
    } suffix{Suffix::None};

    /// @brief Dispatches this literal to an immutable visitor.
    /// @param visitor Visitor receiving @ref ExprVisitor::visit.
    void accept(ExprVisitor &visitor) const override;

    /// @brief Dispatches this literal to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutExprVisitor::visit.
    void accept(MutExprVisitor &visitor) override;
};

/// @brief Floating-point literal expression.
struct FloatExpr : Expr {
    /// @brief Creates a floating node with @ref Kind::Float and value zero.
    FloatExpr() : Expr(Kind::Float) {}

    /// Literal double-precision value parsed from the source.
    double value{0.0};
    /// Optional BASIC suffix distinguishing SINGLE from DOUBLE.
    enum class Suffix {
        /// No explicit floating literal suffix.
        None,
        /// SINGLE suffix spelling.
        Single,
        /// DOUBLE suffix spelling.
        Double,
    } suffix{Suffix::None};

    /// @brief Dispatches this literal to an immutable visitor.
    /// @param visitor Visitor receiving @ref ExprVisitor::visit.
    void accept(ExprVisitor &visitor) const override;

    /// @brief Dispatches this literal to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutExprVisitor::visit.
    void accept(MutExprVisitor &visitor) override;
};

/// @brief String literal expression.
struct StringExpr : Expr {
    /// @brief Creates an empty string node with @ref Kind::String.
    StringExpr() : Expr(Kind::String) {}

    /// Owned UTF-8 string contents without surrounding quotes.
    std::string value;

    /// @brief Dispatches this literal to an immutable visitor.
    /// @param visitor Visitor receiving @ref ExprVisitor::visit.
    void accept(ExprVisitor &visitor) const override;

    /// @brief Dispatches this literal to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutExprVisitor::visit.
    void accept(MutExprVisitor &visitor) override;
};

/// @brief Boolean literal expression.
struct BoolExpr : Expr {
    /// @brief Creates a false Boolean node with @ref Kind::Bool.
    BoolExpr() : Expr(Kind::Bool) {}

    /// Literal boolean value parsed from the source.
    bool value{false};

    /// @brief Dispatches this literal to an immutable visitor.
    /// @param visitor Visitor receiving @ref ExprVisitor::visit.
    void accept(ExprVisitor &visitor) const override;

    /// @brief Dispatches this literal to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutExprVisitor::visit.
    void accept(MutExprVisitor &visitor) override;
};

/// @brief Reference to a scalar variable.
struct VarExpr : Expr {
    /// @brief Creates an unnamed variable node with @ref Kind::Var.
    VarExpr() : Expr(Kind::Var) {}

    /// Variable name including optional type suffix.
    std::string name;

    /// @brief Dispatches this reference to an immutable visitor.
    /// @param visitor Visitor receiving @ref ExprVisitor::visit.
    void accept(ExprVisitor &visitor) const override;

    /// @brief Dispatches this reference to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutExprVisitor::visit.
    void accept(MutExprVisitor &visitor) override;
};

/// @brief Array element access A(i) or A(i,j) for multi-dimensional arrays.
struct ArrayExpr : Expr {
    /// @brief Creates an empty array-access node with @ref Kind::Array.
    ArrayExpr() : Expr(Kind::Array) {}

    /// Name of the array variable being indexed.
    std::string name;

    /// Zero-based index expression for single-dimensional arrays; owned and non-null.
    /// @deprecated Use @ref indices for multi-dimensional support.
    ExprPtr index;

    /// Index expressions for multi-dimensional arrays (owned).
    /// For single-dimensional arrays, this contains one element (backward compatible).
    std::vector<ExprPtr> indices;

    /// Resolved array extents from semantic analysis (BUG-020 fix).
    /// @details Stored during semantic analysis so the lowerer can compute correct
    ///          flattened indices for multi-dimensional arrays even after procedure
    ///          scope cleanup erases the temporary ArrayMetadata entries.
    std::vector<long long> resolvedExtents;

    /// @brief Dispatches this array access to an immutable visitor.
    /// @param visitor Visitor receiving @ref ExprVisitor::visit.
    void accept(ExprVisitor &visitor) const override;

    /// @brief Dispatches this array access to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutExprVisitor::visit.
    void accept(MutExprVisitor &visitor) override;
};

/// @brief Query the logical lower bound of an array.
struct LBoundExpr : Expr {
    /// @brief Creates an empty lower-bound query with @ref Kind::LBound.
    LBoundExpr() : Expr(Kind::LBound) {}

    /// Name of the array operand queried for its lower bound.
    std::string name;

    /// Optional one-based dimension expression from `LBOUND(array, dim)`.
    ExprPtr dimension;

    /// Zero-based dimension resolved by semantic analysis when known.
    std::optional<std::size_t> resolvedDimension;

    /// @brief Dispatches this query to an immutable visitor.
    /// @param visitor Visitor receiving @ref ExprVisitor::visit.
    void accept(ExprVisitor &visitor) const override;

    /// @brief Dispatches this query to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutExprVisitor::visit.
    void accept(MutExprVisitor &visitor) override;
};

/// @brief Query the logical upper bound of an array.
struct UBoundExpr : Expr {
    /// @brief Creates an empty upper-bound query with @ref Kind::UBound.
    UBoundExpr() : Expr(Kind::UBound) {}

    /// Name of the array operand queried for its upper bound.
    std::string name;

    /// Optional one-based dimension expression from `UBOUND(array, dim)`.
    ExprPtr dimension;

    /// Zero-based dimension resolved by semantic analysis when known.
    std::optional<std::size_t> resolvedDimension;

    /// Inclusive upper bound resolved from array metadata when available.
    std::optional<long long> resolvedUpperBound;

    /// @brief Dispatches this query to an immutable visitor.
    /// @param visitor Visitor receiving @ref ExprVisitor::visit.
    void accept(ExprVisitor &visitor) const override;

    /// @brief Dispatches this query to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutExprVisitor::visit.
    void accept(MutExprVisitor &visitor) override;
};

/// @brief Unary expression (e.g., NOT, unary plus/minus).
struct UnaryExpr : Expr {
    /// @brief Creates a unary node with @ref Kind::Unary.
    UnaryExpr() : Expr(Kind::Unary) {}

    /// Unary operator applied to @ref expr.
    enum class Op {
        /// BASIC NOT.
        LogicalNot,
        /// Unary plus.
        Plus,
        /// Unary minus.
        Negate,
    } op{};

    /// Operand expression; owned and non-null.
    ExprPtr expr;

    /// @brief Dispatches this unary operation to an immutable visitor.
    /// @param visitor Visitor receiving @ref ExprVisitor::visit.
    void accept(ExprVisitor &visitor) const override;

    /// @brief Dispatches this unary operation to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutExprVisitor::visit.
    void accept(MutExprVisitor &visitor) override;
};

/// @brief Binary expression combining two operands.
struct BinaryExpr : Expr {
    /// @brief Creates a binary node with @ref Kind::Binary.
    BinaryExpr() : Expr(Kind::Binary) {}

    /// Binary operator applied to @ref lhs and @ref rhs.
    enum class Op {
        /// Addition or string concatenation.
        Add,
        /// Subtraction.
        Sub,
        /// Multiplication.
        Mul,
        /// Floating division.
        Div,
        /// Exponentiation.
        Pow,
        /// Integer division.
        IDiv,
        /// Integer remainder.
        Mod,
        /// Equality comparison.
        Eq,
        /// Inequality comparison.
        Ne,
        /// Less-than comparison.
        Lt,
        /// Less-than-or-equal comparison.
        Le,
        /// Greater-than comparison.
        Gt,
        /// Greater-than-or-equal comparison.
        Ge,
        /// Short-circuit logical conjunction.
        LogicalAndShort,
        /// Short-circuit logical disjunction.
        LogicalOrShort,
        /// Eager logical/bitwise conjunction.
        LogicalAnd,
        /// Eager logical/bitwise disjunction.
        LogicalOr,
    } op{};

    /// Left-hand operand expression; owned and non-null.
    ExprPtr lhs;

    /// Right-hand operand expression; owned and non-null.
    ExprPtr rhs;

    /// @brief Dispatches this binary operation to an immutable visitor.
    /// @param visitor Visitor receiving @ref ExprVisitor::visit.
    void accept(ExprVisitor &visitor) const override;

    /// @brief Dispatches this binary operation to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutExprVisitor::visit.
    void accept(MutExprVisitor &visitor) override;
};

/// @brief Call to a BASIC builtin function.
struct BuiltinCallExpr : Expr {
    /// @brief Creates a LEN call node with @ref Kind::BuiltinCall.
    BuiltinCallExpr() : Expr(Kind::BuiltinCall) {}

    /// Which builtin function to invoke.
    /// @details Enumerators cover parser-recognized intrinsic functions; their
    ///          arity and type contracts are validated by later phases.
    enum class Builtin {
        Len,
        Mid,
        Left,
        Right,
        Str,
        Val,
        Cint,
        Clng,
        Csng,
        Cdbl,
        Int,
        Fix,
        Round,
        Sqr,
        Abs,
        Floor,
        Ceil,
        Sin,
        Cos,
        Tan,
        Atn,
        Exp,
        Log,
        Sgn,
        Pow,
        Rnd,
        Instr,
        Ltrim,
        Rtrim,
        Trim,
        Ucase,
        Lcase,
        Chr,
        Asc,
        InKey,
        GetKey,
        Eof,
        Lof,
        Loc,
        Timer,
        Argc,
        ArgGet,
        Command,
        Err
    } builtin{Builtin::Len};

    /// Argument expressions passed to the builtin; owned.
    std::vector<ExprPtr> args;

    /// @brief Dispatches this built-in call to an immutable visitor.
    /// @param visitor Visitor receiving @ref ExprVisitor::visit.
    void accept(ExprVisitor &visitor) const override;

    /// @brief Dispatches this built-in call to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutExprVisitor::visit.
    void accept(MutExprVisitor &visitor) override;
};

/// @brief Call to user-defined FUNCTION or SUB.
struct CallExpr : Expr {
    /// @brief Creates an empty call node with @ref Kind::Call.
    CallExpr() : Expr(Kind::Call) {}

    /// Procedure name to invoke.
    Identifier callee;

    /// Optional qualified callee path when a dotted name was parsed.
    /// When non-empty, `callee` contains the dot-joined string as well for
    /// backward compatibility with existing passes.
    std::vector<std::string> calleeQualified;

    /// Ordered argument expressions; owned.
    std::vector<ExprPtr> args;

    /// @brief Dispatches this procedure call to an immutable visitor.
    /// @param visitor Visitor receiving @ref ExprVisitor::visit.
    void accept(ExprVisitor &visitor) const override;

    /// @brief Dispatches this procedure call to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutExprVisitor::visit.
    void accept(MutExprVisitor &visitor) override;
};

/// @brief Allocate a new instance of a class.
struct NewExpr : Expr {
    /// @brief Creates an empty allocation node with @ref Kind::New.
    NewExpr() : Expr(Kind::New) {}

    /// Name of the class type to instantiate.
    std::string className;

    /// Optional qualified class/type name segments. When non-empty, className
    /// stores the dot-joined form for compatibility with existing passes.
    std::vector<std::string> qualifiedType;

    /// Arguments passed to the constructor.
    std::vector<ExprPtr> args;

    /// @brief Dispatches this allocation to an immutable visitor.
    /// @param visitor Visitor receiving @ref ExprVisitor::visit.
    void accept(ExprVisitor &visitor) const override;

    /// @brief Dispatches this allocation to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutExprVisitor::visit.
    void accept(MutExprVisitor &visitor) override;
};

/// @brief Reference to the receiver instance inside methods.
struct MeExpr : Expr {
    /// @brief Creates an implicit receiver node with @ref Kind::Me.
    MeExpr() : Expr(Kind::Me) {}

    /// @brief Dispatches this receiver reference to an immutable visitor.
    /// @param visitor Visitor receiving @ref ExprVisitor::visit.
    void accept(ExprVisitor &visitor) const override;

    /// @brief Dispatches this receiver reference to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutExprVisitor::visit.
    void accept(MutExprVisitor &visitor) override;
};

/// @brief Access a member field on an object.
struct MemberAccessExpr : Expr {
    /// @brief Creates an empty member access with @ref Kind::MemberAccess.
    MemberAccessExpr() : Expr(Kind::MemberAccess) {}

    /// Base expression evaluating to an object.
    ExprPtr base;

    /// Member field being accessed.
    std::string member;

    /// @brief Dispatches this member access to an immutable visitor.
    /// @param visitor Visitor receiving @ref ExprVisitor::visit.
    void accept(ExprVisitor &visitor) const override;

    /// @brief Dispatches this member access to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutExprVisitor::visit.
    void accept(MutExprVisitor &visitor) override;
};

/// @brief Invoke a method on an object instance.
struct MethodCallExpr : Expr {
    /// @brief Creates an empty method call with @ref Kind::MethodCall.
    MethodCallExpr() : Expr(Kind::MethodCall) {}

    /// Base expression evaluating to the receiver instance.
    ExprPtr base;

    /// Method name to invoke.
    std::string method;

    /// Arguments passed to the method call.
    std::vector<ExprPtr> args;

    /// @brief Dispatches this method call to an immutable visitor.
    /// @param visitor Visitor receiving @ref ExprVisitor::visit.
    void accept(ExprVisitor &visitor) const override;

    /// @brief Dispatches this method call to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutExprVisitor::visit.
    void accept(MutExprVisitor &visitor) override;
};

/// @brief Runtime type check expression: `value IS Type.Name`.
struct IsExpr : Expr {
    /// @brief Creates an empty runtime type test with @ref Kind::Is.
    IsExpr() : Expr(Kind::Is) {}

    /// Value being tested; owned and expected non-null after parsing.
    ExprPtr value;

    /// Dotted type name segments.
    std::vector<std::string> typeName;

    /// @brief Dispatches this type test to an immutable visitor.
    /// @param visitor Visitor receiving @ref ExprVisitor::visit.
    void accept(ExprVisitor &visitor) const override;

    /// @brief Dispatches this type test to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutExprVisitor::visit.
    void accept(MutExprVisitor &visitor) override;
};

/// @brief Type ascription/cast expression: `value AS Type.Name`.
struct AsExpr : Expr {
    /// @brief Creates an empty type ascription with @ref Kind::As.
    AsExpr() : Expr(Kind::As) {}

    /// Value being cast; owned and expected non-null after parsing.
    ExprPtr value;

    /// Dotted type name segments.
    std::vector<std::string> typeName;

    /// @brief Dispatches this cast to an immutable visitor.
    /// @param visitor Visitor receiving @ref ExprVisitor::visit.
    void accept(ExprVisitor &visitor) const override;

    /// @brief Dispatches this cast to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutExprVisitor::visit.
    void accept(MutExprVisitor &visitor) override;
};

/// @brief Expression that obtains a function pointer: `ADDRESSOF SubOrFunction`.
/// Used for threading APIs that require callback functions.
struct AddressOfExpr : Expr {
    /// @brief Creates an empty procedure-address node with @ref Kind::AddressOf.
    AddressOfExpr() : Expr(Kind::AddressOf) {}

    /// Name of the SUB or FUNCTION whose address is being taken.
    std::string targetName;

    /// @brief Dispatches this address expression to an immutable visitor.
    /// @param visitor Visitor receiving @ref ExprVisitor::visit.
    void accept(ExprVisitor &visitor) const override;

    /// @brief Dispatches this address expression to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutExprVisitor::visit.
    void accept(MutExprVisitor &visitor) override;
};

} // namespace il::frontends::basic
