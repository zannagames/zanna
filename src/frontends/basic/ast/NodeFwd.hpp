//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/ast/NodeFwd.hpp
// Purpose: Provides lightweight forward declarations, scalar type tags,
//          ownership aliases, and visitor-dispatch entry points for the BASIC AST.
// Key invariants:
//   - Type enumerators represent the four scalar categories shared by parser,
//     semantic analysis, and lowering.
//   - ExprPtr and StmtPtr express unique subtree ownership.
//   - Free visit overloads dispatch to the concrete node's accept method.
// Ownership/Lifetime:
//   - Pointer aliases own their nodes; visitor references are borrowed only for
//     each dispatch call.
// Links: src/frontends/basic/ast/ExprNodes.hpp,
//        src/frontends/basic/ast/StmtBase.hpp,
//        src/frontends/basic/ast/DeclNodes.hpp,
//        src/frontends/basic/AST.cpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <memory>
#include <string>

/// @file
/// @brief Forward-declares BASIC AST nodes and their common ownership API.

namespace il::frontends::basic {

/// @name Expression node declarations
/// @{
struct Expr;
struct IntExpr;
struct FloatExpr;
struct StringExpr;
struct BoolExpr;
struct VarExpr;
struct ArrayExpr;
struct UnaryExpr;
struct BinaryExpr;
struct BuiltinCallExpr;
struct LBoundExpr;
struct UBoundExpr;
struct CallExpr;
struct NewExpr;
struct MeExpr;
struct MemberAccessExpr;
struct MethodCallExpr;
struct IsExpr;
struct AsExpr;
struct AddressOfExpr;
/// @}

/// @name Expression visitor declarations
/// @{
struct ExprVisitor;
struct MutExprVisitor;
/// @}

/// @name Statement node declarations
/// @{
struct Stmt;
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
struct ConstStmt;
struct DimStmt;
struct StaticStmt;
struct SharedStmt;
struct ReDimStmt;
struct SwapStmt;
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
struct StmtList;
struct DeleteStmt;
struct ConstructorDecl;
struct DestructorDecl;
struct MethodDecl;
struct PropertyDecl;
struct ClassDecl;
struct TypeDecl;
struct EnumDecl;
struct InterfaceDecl;
struct PrintItem;
struct CaseArm;
struct NameRef;
/// @}

/// @name Statement visitor declarations
/// @{
struct StmtVisitor;
struct MutStmtVisitor;
/// @}

/// @name Program and procedure aggregate declarations
/// @{
struct Param;
struct FunctionDecl;
struct SubDecl;
struct Program;
/// @}

/// @brief BASIC primitive types mirrored by the AST.
enum class Type : std::uint8_t {
    /// Signed integer scalar represented as IL i64.
    I64,
    /// Floating scalar represented as IL f64.
    F64,
    /// Managed BASIC string value.
    Str,
    /// Boolean scalar represented as IL i1 during expression lowering.
    Bool,
};

/// @brief Unique ownership pointer for any expression subtree.
using ExprPtr = std::unique_ptr<Expr>;

/// @brief Semantic alias marking an expression pointer used as an assignable target.
using LValuePtr = ExprPtr;

/// @brief Owned source spelling used for BASIC identifiers.
using Identifier = std::string;

/// @brief Unique ownership pointer for any statement subtree.
using StmtPtr = std::unique_ptr<Stmt>;

/// @brief Procedure-declaration ownership alias; declarations are statements.
using ProcDecl = StmtPtr;

/// @brief Dispatch an expression to the matching visitor callback (double dispatch).
/// @param expr Concrete expression borrowed for the call.
/// @param visitor Immutable visitor receiving the concrete overload.
void visit(const Expr &expr, ExprVisitor &visitor);

/// @brief Dispatch a mutable expression to the matching mutable-visitor callback.
/// @param expr Mutable concrete expression borrowed for the call.
/// @param visitor Mutable visitor receiving the concrete overload.
void visit(Expr &expr, MutExprVisitor &visitor);

/// @brief Dispatch a statement to the matching visitor callback (double dispatch).
/// @param stmt Concrete statement borrowed for the call.
/// @param visitor Immutable visitor receiving the concrete overload.
void visit(const Stmt &stmt, StmtVisitor &visitor);

/// @brief Dispatch a mutable statement to the matching mutable-visitor callback.
/// @param stmt Mutable concrete statement borrowed for the call.
/// @param visitor Mutable visitor receiving the concrete overload.
void visit(Stmt &stmt, MutStmtVisitor &visitor);

} // namespace il::frontends::basic
