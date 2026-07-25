//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This file provides type-safe utilities for checking and casting BASIC AST
// nodes, replacing dynamic_cast with efficient O(1) discriminator-based lookups.
//
// AST Node Type System:
// BASIC AST nodes use a discriminator-based type system where each node carries
// a Kind enum value that identifies its concrete type. This enables efficient
// type checking and casting without RTTI overhead.
//
// Key Utilities:
// - is<T>(node): Check if a node is of type T (O(1) discriminator check)
// - cast<T>(node): Unchecked cast to T when the caller already knows the kind
// - as<T>(node): Attempt to cast, returning nullptr on mismatch
//
// These utilities mirror LLVM's casting infrastructure and provide:
// - Type safety: Compile-time type checking for AST traversal code
// - Performance: O(1) discriminator checks instead of dynamic_cast overhead
// - Debugging: Assertions catch incorrect casts during development
//
// Example Usage:
//   if (isa<IfStmt>(stmt)) {
//     auto* ifStmt = cast<IfStmt>(stmt);
//     // Process if statement
//   }
//
//   if (auto* binExpr = dyn_cast<BinaryExpr>(expr)) {
//     // Process binary expression
//   }
//
// Integration:
// - Used by: Parser for AST node classification
// - Used by: SemanticAnalyzer for type-specific validation
// - Used by: Lowerer for node type dispatch
// - Used by: AST traversal and visitor patterns
//
// Design Notes:
// - All type checks rely on the Kind discriminator accurately reflecting the
//   concrete node type
// - Utilities do not own nodes; they merely provide safe access
// - Header-only implementation for zero-cost abstraction
// - Compatible with std::unique_ptr and raw pointer access patterns
//
//===----------------------------------------------------------------------===//

/**
 * @file ASTUtils.hpp
 * @brief Defines discriminator-based BASIC AST casts and construction helpers.
 *
 * Expression and statement traits map concrete node types to their enum
 * discriminators. The header also provides lightweight literal factories and
 * qualified-name extraction used when recognizing runtime classes.
 */

#pragma once

#include "frontends/basic/StringUtils.hpp"
#include "frontends/basic/ast/ExprNodes.hpp"
#include "frontends/basic/ast/StmtBase.hpp"

#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace il::frontends::basic {

//===----------------------------------------------------------------------===//
// Expression utilities
//===----------------------------------------------------------------------===//

/// @brief Type-to-Kind mapping for expression nodes.
/// @details Specialized below for each expression type accepted by is() and
///          as(). Instantiating the unspecialized primary template is invalid.
/// @tparam T Concrete expression node type.
template <typename T> struct ExprKindTraits;

/// Generate one expression type-to-discriminator trait specialization.
#define EXPR_KIND_TRAIT(Type, KindValue)                                                           \
    template <> struct ExprKindTraits<Type> {                                                      \
        static constexpr Expr::Kind kind = Expr::Kind::KindValue;                                  \
    };

EXPR_KIND_TRAIT(IntExpr, Int)
EXPR_KIND_TRAIT(FloatExpr, Float)
EXPR_KIND_TRAIT(StringExpr, String)
EXPR_KIND_TRAIT(BoolExpr, Bool)
EXPR_KIND_TRAIT(VarExpr, Var)
EXPR_KIND_TRAIT(ArrayExpr, Array)
EXPR_KIND_TRAIT(LBoundExpr, LBound)
EXPR_KIND_TRAIT(UBoundExpr, UBound)
EXPR_KIND_TRAIT(UnaryExpr, Unary)
EXPR_KIND_TRAIT(BinaryExpr, Binary)
EXPR_KIND_TRAIT(BuiltinCallExpr, BuiltinCall)
EXPR_KIND_TRAIT(CallExpr, Call)
EXPR_KIND_TRAIT(NewExpr, New)
EXPR_KIND_TRAIT(MeExpr, Me)
EXPR_KIND_TRAIT(MemberAccessExpr, MemberAccess)
EXPR_KIND_TRAIT(MethodCallExpr, MethodCall)
EXPR_KIND_TRAIT(IsExpr, Is)
EXPR_KIND_TRAIT(AsExpr, As)

#undef EXPR_KIND_TRAIT

/// @brief Check if an expression is of a specific type.
/// @details Performs O(1) kind comparison instead of RTTI-based dynamic_cast.
/// @tparam T Target expression type (e.g., IntExpr, VarExpr).
/// @param expr Expression to check.
/// @return True if expr is of type T.
///
/// @note This replaces patterns like: `dynamic_cast<const IntExpr*>(&expr) != nullptr`
/// @example
/// ```cpp
/// if (is<IntExpr>(expr)) {
///     const auto& intExpr = as<IntExpr>(expr);
///     // use intExpr.value
/// }
/// ```
template <typename T> [[nodiscard]] constexpr bool is(const Expr &expr) noexcept {
    static_assert(std::is_base_of_v<Expr, T>, "T must be derived from Expr");
    return expr.kind() == ExprKindTraits<T>::kind;
}

/// @brief Safely cast an expression to a specific type.
/// @details Returns a pointer to the derived type if the kind matches, nullptr otherwise.
///          Performs O(1) kind check instead of RTTI-based dynamic_cast.
/// @tparam T Const-qualified target expression type.
/// @param expr Expression to cast.
/// @return Pointer to T if kind matches, nullptr otherwise.
///
/// @note This replaces: `dynamic_cast<const IntExpr*>(&expr)`
/// @example
/// ```cpp
/// if (const auto* intExpr = as<const IntExpr>(expr)) {
///     // use intExpr->value
/// }
/// ```
template <typename T> [[nodiscard]] constexpr T *as(const Expr &expr) noexcept {
    using BaseT = std::remove_cv_t<std::remove_pointer_t<T>>;
    static_assert(std::is_base_of_v<Expr, BaseT>, "T must be derived from Expr");
    static_assert(std::is_const_v<T> || std::is_pointer_v<T>,
                  "T must be const or pointer to const");

    if (expr.kind() == ExprKindTraits<BaseT>::kind) {
        return static_cast<T *>(&const_cast<Expr &>(const_cast<Expr &>(expr)));
    }
    return nullptr;
}

/// @brief Safely cast a mutable expression to a specific type.
/// @details Returns a pointer to the derived type if the kind matches, nullptr otherwise.
/// @tparam T Target expression type (non-const).
/// @param expr Expression to cast.
/// @return Pointer to T if kind matches, nullptr otherwise.
template <typename T> [[nodiscard]] constexpr T *as(Expr &expr) noexcept {
    using BaseT = std::remove_cv_t<std::remove_pointer_t<T>>;
    static_assert(std::is_base_of_v<Expr, BaseT>, "T must be derived from Expr");

    if (expr.kind() == ExprKindTraits<BaseT>::kind) {
        return static_cast<T *>(&expr);
    }
    return nullptr;
}

/// @brief Unchecked cast to a specific expression type.
/// @details Performs static_cast without kind verification. Use only when kind is
///          guaranteed to match (e.g., after is<T>() check).
/// @tparam T Target expression type.
/// @param expr Expression to cast.
/// @return Reference to T (undefined behavior if kind mismatches).
///
/// @warning Only use after verifying kind with is<T>(). Undefined behavior otherwise.
/// @example
/// ```cpp
/// if (is<IntExpr>(expr)) {
///     const auto& intExpr = cast<IntExpr>(expr);  // Safe: kind checked above
/// }
/// ```
template <typename T> [[nodiscard]] constexpr T &cast(Expr &expr) noexcept {
    static_assert(std::is_base_of_v<Expr, T>, "T must be derived from Expr");
    return static_cast<T &>(expr);
}

/// @brief Unchecked cast to a specific expression type (const version).
/// @tparam T Concrete expression type.
/// @param expr Expression known to have `ExprKindTraits<T>::kind`.
/// @return Const reference to @p expr viewed as T.
/// @warning A mismatched dynamic type causes undefined behavior; verify with
///          is<T>() or equivalent control flow first.
template <typename T> [[nodiscard]] constexpr const T &cast(const Expr &expr) noexcept {
    static_assert(std::is_base_of_v<Expr, T>, "T must be derived from Expr");
    return static_cast<const T &>(expr);
}

//===----------------------------------------------------------------------===//
// Statement utilities
//===----------------------------------------------------------------------===//

/// @brief Type-to-Kind mapping for statement nodes.
/// @details Specialized below for each statement type accepted by is() and
///          as(). Instantiating the unspecialized primary template is invalid.
/// @tparam T Concrete statement node type.
template <typename T> struct StmtKindTraits;

/// Generate one statement type-to-discriminator trait specialization.
#define STMT_KIND_TRAIT(Type, KindValue)                                                           \
    template <> struct StmtKindTraits<Type> {                                                      \
        static constexpr Stmt::Kind kind = Stmt::Kind::KindValue;                                  \
    };

// Forward declare statement types
struct LabelStmt;
struct PrintStmt;
struct PrintChStmt;
struct BeepStmt;
struct CallStmt;
struct ClsStmt;
struct ColorStmt;
struct SleepStmt;
struct LocateStmt;
struct CursorStmt;
struct AltScreenStmt;
struct LetStmt;
struct DimStmt;
struct ReDimStmt;
struct RandomizeStmt;
struct IfStmt;
struct SelectCaseStmt;
struct TryCatchStmt;
struct WhileStmt;
struct DoStmt;
struct ForStmt;
struct ForEachStmt;
struct NextStmt;
struct ExitStmt;
struct GotoStmt;
struct GosubStmt;
struct OpenStmt;
struct CloseStmt;
struct SeekStmt;
struct OnErrorGoto;
struct Resume;
struct EndStmt;
struct InputStmt;
struct InputChStmt;
struct LineInputChStmt;
struct ReturnStmt;
struct FunctionDecl;
struct SubDecl;
struct StmtList;
struct DeleteStmt;
struct ConstructorDecl;
struct DestructorDecl;
struct MethodDecl;
struct PropertyDecl;
struct ClassDecl;
struct TypeDecl;
struct InterfaceDecl;
struct NamespaceDecl;

STMT_KIND_TRAIT(LabelStmt, Label)
STMT_KIND_TRAIT(PrintStmt, Print)
STMT_KIND_TRAIT(PrintChStmt, PrintCh)
STMT_KIND_TRAIT(BeepStmt, Beep)
STMT_KIND_TRAIT(CallStmt, Call)
STMT_KIND_TRAIT(ClsStmt, Cls)
STMT_KIND_TRAIT(ColorStmt, Color)
STMT_KIND_TRAIT(SleepStmt, Sleep)
STMT_KIND_TRAIT(LocateStmt, Locate)
STMT_KIND_TRAIT(CursorStmt, Cursor)
STMT_KIND_TRAIT(AltScreenStmt, AltScreen)
STMT_KIND_TRAIT(LetStmt, Let)
STMT_KIND_TRAIT(DimStmt, Dim)
STMT_KIND_TRAIT(ReDimStmt, ReDim)
STMT_KIND_TRAIT(RandomizeStmt, Randomize)
STMT_KIND_TRAIT(IfStmt, If)
STMT_KIND_TRAIT(SelectCaseStmt, SelectCase)
STMT_KIND_TRAIT(TryCatchStmt, TryCatch)
STMT_KIND_TRAIT(WhileStmt, While)
STMT_KIND_TRAIT(DoStmt, Do)
STMT_KIND_TRAIT(ForStmt, For)
STMT_KIND_TRAIT(ForEachStmt, ForEach)
STMT_KIND_TRAIT(NextStmt, Next)
STMT_KIND_TRAIT(ExitStmt, Exit)
STMT_KIND_TRAIT(GotoStmt, Goto)
STMT_KIND_TRAIT(GosubStmt, Gosub)
STMT_KIND_TRAIT(OpenStmt, Open)
STMT_KIND_TRAIT(CloseStmt, Close)
STMT_KIND_TRAIT(SeekStmt, Seek)
STMT_KIND_TRAIT(OnErrorGoto, OnErrorGoto)
STMT_KIND_TRAIT(Resume, Resume)
STMT_KIND_TRAIT(EndStmt, End)
STMT_KIND_TRAIT(InputStmt, Input)
STMT_KIND_TRAIT(InputChStmt, InputCh)
STMT_KIND_TRAIT(LineInputChStmt, LineInputCh)
STMT_KIND_TRAIT(ReturnStmt, Return)
STMT_KIND_TRAIT(FunctionDecl, FunctionDecl)
STMT_KIND_TRAIT(SubDecl, SubDecl)
STMT_KIND_TRAIT(StmtList, StmtList)
STMT_KIND_TRAIT(DeleteStmt, Delete)
STMT_KIND_TRAIT(ConstructorDecl, ConstructorDecl)
STMT_KIND_TRAIT(DestructorDecl, DestructorDecl)
STMT_KIND_TRAIT(MethodDecl, MethodDecl)
STMT_KIND_TRAIT(PropertyDecl, PropertyDecl)
STMT_KIND_TRAIT(ClassDecl, ClassDecl)
STMT_KIND_TRAIT(TypeDecl, TypeDecl)
STMT_KIND_TRAIT(InterfaceDecl, InterfaceDecl)
STMT_KIND_TRAIT(NamespaceDecl, NamespaceDecl)

#undef STMT_KIND_TRAIT

/// @brief Check if a statement is of a specific type.
/// @details Compares the node's stored statement discriminator in constant time.
/// @tparam T Target statement type.
/// @param stmt Statement to check.
/// @return True if stmt is of type T.
template <typename T> [[nodiscard]] constexpr bool is(const Stmt &stmt) noexcept {
    static_assert(std::is_base_of_v<Stmt, T>, "T must be derived from Stmt");
    return stmt.stmtKind() == StmtKindTraits<T>::kind;
}

/// @brief Safely cast a statement to a specific type (const version).
/// @tparam T Target statement type; callers should use a const-qualified type
///           to preserve @p stmt's constness.
/// @param stmt Statement whose discriminator is checked.
/// @return Pointer to @p stmt as T when its kind matches, otherwise `nullptr`.
/// @warning The implementation permits a non-const T and internally removes
///          constness. Passing such a T and mutating through the result is
///          invalid when the original object is const.
template <typename T> [[nodiscard]] constexpr T *as(const Stmt &stmt) noexcept {
    using BaseT = std::remove_cv_t<std::remove_pointer_t<T>>;
    static_assert(std::is_base_of_v<Stmt, BaseT>, "T must be derived from Stmt");

    if (stmt.stmtKind() == StmtKindTraits<BaseT>::kind) {
        return static_cast<T *>(&const_cast<Stmt &>(const_cast<Stmt &>(stmt)));
    }
    return nullptr;
}

/// @brief Safely cast a statement to a specific type (non-const version).
/// @tparam T Target concrete statement type.
/// @param stmt Mutable statement whose discriminator is checked.
/// @return Pointer to @p stmt as T when its kind matches, otherwise `nullptr`.
template <typename T> [[nodiscard]] constexpr T *as(Stmt &stmt) noexcept {
    using BaseT = std::remove_cv_t<std::remove_pointer_t<T>>;
    static_assert(std::is_base_of_v<Stmt, BaseT>, "T must be derived from Stmt");

    if (stmt.stmtKind() == StmtKindTraits<BaseT>::kind) {
        return static_cast<T *>(&stmt);
    }
    return nullptr;
}

/// @brief Unchecked cast to a specific statement type.
/// @tparam T Concrete statement type.
/// @param stmt Mutable statement known to have `StmtKindTraits<T>::kind`.
/// @return Mutable reference to @p stmt viewed as T.
/// @warning A mismatched dynamic type causes undefined behavior; verify with
///          is<T>() or equivalent control flow first.
template <typename T> [[nodiscard]] constexpr T &cast(Stmt &stmt) noexcept {
    static_assert(std::is_base_of_v<Stmt, T>, "T must be derived from Stmt");
    return static_cast<T &>(stmt);
}

/// @brief Unchecked cast to a specific statement type (const version).
/// @tparam T Concrete statement type.
/// @param stmt Statement known to have `StmtKindTraits<T>::kind`.
/// @return Const reference to @p stmt viewed as T.
/// @warning A mismatched dynamic type causes undefined behavior; verify with
///          is<T>() or equivalent control flow first.
template <typename T> [[nodiscard]] constexpr const T &cast(const Stmt &stmt) noexcept {
    static_assert(std::is_base_of_v<Stmt, T>, "T must be derived from Stmt");
    return static_cast<const T &>(stmt);
}

//===----------------------------------------------------------------------===//
// AST Factory Helpers
//===----------------------------------------------------------------------===//

/// @brief Create an integer literal expression node.
/// @details Allocates and initializes an IntExpr with the given value and location.
///          Reduces boilerplate from 4 lines to 1 line.
/// @param value Integer value for the literal.
/// @param loc Source location for diagnostics.
/// @return Unique pointer to the newly created IntExpr.
/// @example
/// ```cpp
/// // Before:
/// auto expr = std::make_unique<IntExpr>();
/// expr->loc = loc;
/// expr->value = 42;
///
/// // After:
/// auto expr = makeIntExpr(42, loc);
/// ```
[[nodiscard]] inline ExprPtr makeIntExpr(long long value, il::support::SourceLoc loc) {
    auto expr = std::make_unique<IntExpr>();
    expr->loc = loc;
    expr->value = value;
    return expr;
}

/// @brief Create a boolean literal expression node.
/// @details Allocates and initializes a BoolExpr with the given value and location.
/// @param value Boolean value for the literal.
/// @param loc Source location for diagnostics.
/// @return Unique pointer to the newly created BoolExpr.
[[nodiscard]] inline ExprPtr makeBoolExpr(bool value, il::support::SourceLoc loc) {
    auto expr = std::make_unique<BoolExpr>();
    expr->loc = loc;
    expr->value = value;
    return expr;
}

/// @brief Create a floating-point literal expression node.
/// @details Allocates and initializes a FloatExpr with the given value and location.
/// @param value Floating-point value for the literal.
/// @param loc Source location for diagnostics.
/// @return Unique pointer to the newly created FloatExpr.
[[nodiscard]] inline ExprPtr makeFloatExpr(double value, il::support::SourceLoc loc) {
    auto expr = std::make_unique<FloatExpr>();
    expr->loc = loc;
    expr->value = value;
    return expr;
}

/// @brief Create a string literal expression node.
/// @details Allocates and initializes a StringExpr with the given value and location.
/// @param value String value for the literal.
/// @param loc Source location for diagnostics.
/// @return Unique pointer to the newly created StringExpr.
[[nodiscard]] inline ExprPtr makeStrExpr(std::string_view value, il::support::SourceLoc loc) {
    auto expr = std::make_unique<StringExpr>();
    expr->loc = loc;
    expr->value = value;
    return expr;
}

//===----------------------------------------------------------------------===//
// Runtime Class Name Utilities
//===----------------------------------------------------------------------===//

/// @brief Recursively collects qualified name segments from an expression.
/// @details Walks a chain of VarExpr or MemberAccessExpr nodes to build a list
///          of name segments. For example, `Zanna.String.get_Length` produces
///          ["Zanna", "String", "get_Length"]. Existing elements in @p out
///          are preserved; successful segments are appended in source order.
/// @param expr Expression to traverse.
/// @param out Vector to accumulate name segments.
/// @return True if the expression forms a valid qualified name, false otherwise.
inline bool collectQualifiedSegments(const Expr &expr, std::vector<std::string> &out) {
    if (auto *var = as<const VarExpr>(expr)) {
        out.push_back(var->name);
        return true;
    }
    if (auto *mem = as<const MemberAccessExpr>(expr)) {
        if (!mem->base)
            return false;
        if (!collectQualifiedSegments(*mem->base, out))
            return false;
        out.push_back(mem->member);
        return true;
    }
    return false;
}

/// @brief Extracts a runtime class qualified name from an expression.
/// @details Attempts to interpret the expression as a qualified name chain
///          (e.g., `Zanna.String`). Returns the qualified name if the chain
///          starts with "Zanna" (case-insensitive), nullopt otherwise. Original
///          segment spelling is preserved in the returned dotted name.
/// @param expr Expression to analyze.
/// @return Qualified class name (e.g., "Zanna.String") or nullopt.
[[nodiscard]] inline std::optional<std::string> runtimeClassQNameFrom(const Expr &expr) {
    std::vector<std::string> parts;
    if (!collectQualifiedSegments(expr, parts))
        return std::nullopt;
    if (parts.empty() || !string_utils::iequals(parts.front(), "Zanna"))
        return std::nullopt;
    std::string qname;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i)
            qname.push_back('.');
        qname += parts[i];
    }
    return qname;
}

} // namespace il::frontends::basic
