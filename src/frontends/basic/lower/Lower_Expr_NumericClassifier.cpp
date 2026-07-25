//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
// File: src/frontends/basic/lower/Lower_Expr_NumericClassifier.cpp
// Purpose: Implements the numeric type classification helper for BASIC
//          expression lowering. Determines the result type of numeric
//          expressions according to BASIC type promotion rules.
// Key invariants: Classification follows QBasic/GW-BASIC type promotion rules.
// Ownership/Lifetime: Operates on borrowed Lowerer reference.
// Links: docs/internals/codemap.md, docs/tutorials/basic-tutorial.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements BASIC numeric-category inference used during IL lowering.
/// @details The visitor combines literal suffixes, semantic and symbol-table
///          types, operator promotion, builtin contracts, and procedure
///          signatures. Expressions without a meaningful numeric category use
///          Long as the conservative lowering fallback.

#include "frontends/basic/Lowerer.hpp"
#include "frontends/basic/SemanticAnalyzer.hpp"
#include "frontends/basic/TypeSuffix.hpp"

#include <limits>

namespace il::frontends::basic {

using NumericType = TypeRules::NumericType;
using AstType = ::il::frontends::basic::Type;
using IlType = il::core::Type;

/// @brief Visitor that classifies BASIC expressions into numeric type categories.
/// @details Walks an expression tree to determine its resulting numeric type,
///          following QBasic/GW-BASIC type promotion rules. This is used by
///          lowering to select the appropriate IL operations and coercions.
class NumericTypeClassifier final : public ExprVisitor {
  public:
    /// @brief Creates a classifier borrowing a lowerer for type lookups.
    /// @param lowerer Lowerer supplying semantic, symbol, and signature data.
    explicit NumericTypeClassifier(Lowerer &lowerer) noexcept : lowerer_(lowerer) {}

    /// @brief Returns the category assigned by the most recent visit.
    /// @return Inferred numeric category, initially Long.
    NumericType result() const noexcept {
        return result_;
    }

    /// @brief Classifies an integer literal from its suffix and magnitude.
    /// @param i Integer literal to inspect.
    void visit(const IntExpr &i) override {
        switch (i.suffix) {
            case IntExpr::Suffix::Integer:
                result_ = NumericType::Integer;
                return;
            case IntExpr::Suffix::Long:
                result_ = NumericType::Long;
                return;
            case IntExpr::Suffix::None:
                break;
        }

        const long long value = i.value;
        if (value >= std::numeric_limits<int16_t>::min() &&
            value <= std::numeric_limits<int16_t>::max()) {
            result_ = NumericType::Integer;
        } else {
            result_ = NumericType::Long;
        }
    }

    /// @brief Classifies a floating literal from its BASIC suffix.
    /// @param f Floating-point literal to inspect.
    void visit(const FloatExpr &f) override {
        result_ =
            (f.suffix == FloatExpr::Suffix::Single) ? NumericType::Single : NumericType::Double;
    }

    /// @brief Assigns the nonnumeric string fallback category.
    /// The visited node is borrowed and otherwise ignored.
    void visit(const StringExpr &) override {
        result_ = NumericType::Double;
    }

    /// @brief Classifies a boolean literal using BASIC's Integer category.
    /// The visited node is borrowed and otherwise ignored.
    void visit(const BoolExpr &) override {
        result_ = NumericType::Integer;
    }

    /// @brief Classifies a variable using semantic type, symbol type, suffix,
    ///        and name-based inference in descending precedence.
    /// @param var Variable reference to resolve.
    void visit(const VarExpr &var) override {
        // BUG-019 fix: Check semantic analysis first for CONST float types
        AstType effectiveType = AstType::I64;
        bool hasEffectiveType = false;

        if (lowerer_.semanticAnalyzer()) {
            if (auto semaType = lowerer_.semanticAnalyzer()->lookupVarType(std::string{var.name})) {
                using SemaType = SemanticAnalyzer::Type;
                switch (*semaType) {
                    case SemaType::Float:
                        effectiveType = AstType::F64;
                        hasEffectiveType = true;
                        break;
                    case SemaType::Int:
                        effectiveType = AstType::I64;
                        hasEffectiveType = true;
                        break;
                    default:
                        break;
                }
            }
        }

        if (const auto *info = lowerer_.findSymbol(var.name)) {
            if (info->hasType && !hasEffectiveType) {
                effectiveType = info->type;
                hasEffectiveType = true;
            }
        }

        if (hasEffectiveType) {
            if (effectiveType == AstType::F64) {
                if (!var.name.empty()) {
                    switch (var.name.back()) {
                        case '!':
                            result_ = NumericType::Single;
                            return;
                        case '#':
                            result_ = NumericType::Double;
                            return;
                        default:
                            break;
                    }
                }
                result_ = NumericType::Double;
                return;
            }
            if (!var.name.empty()) {
                switch (var.name.back()) {
                    case '%':
                        result_ = NumericType::Integer;
                        return;
                    case '&':
                        result_ = NumericType::Long;
                        return;
                    default:
                        break;
                }
            }
            result_ = NumericType::Long;
            return;
        }

        if (!var.name.empty()) {
            switch (var.name.back()) {
                case '!':
                    result_ = NumericType::Single;
                    return;
                case '#':
                    result_ = NumericType::Double;
                    return;
                case '%':
                    result_ = NumericType::Integer;
                    return;
                case '&':
                    result_ = NumericType::Long;
                    return;
                default:
                    break;
            }
        }

        AstType astTy = inferAstTypeFromName(var.name);
        result_ = (astTy == AstType::F64) ? NumericType::Double : NumericType::Long;
    }

    /// @brief Assigns the pointer-like array-expression fallback category.
    /// The visited node is borrowed and otherwise ignored.
    void visit(const ArrayExpr &) override {
        result_ = NumericType::Long;
    }

    /// @brief Propagates a unary expression's operand category.
    /// @param un Unary expression whose child is classified recursively.
    void visit(const UnaryExpr &un) override {
        if (!un.expr) {
            result_ = NumericType::Long;
            return;
        }
        result_ = lowerer_.classifyNumericType(*un.expr);
    }

    /// @brief Applies BASIC's operator-specific binary promotion rules.
    /// @param bin Binary expression whose operands are classified recursively.
    void visit(const BinaryExpr &bin) override {
        if (!bin.lhs || !bin.rhs) {
            result_ = NumericType::Long;
            return;
        }

        NumericType lhsTy = lowerer_.classifyNumericType(*bin.lhs);
        NumericType rhsTy = lowerer_.classifyNumericType(*bin.rhs);

        switch (bin.op) {
            case BinaryExpr::Op::Add:
                result_ = TypeRules::resultType('+', lhsTy, rhsTy);
                return;
            case BinaryExpr::Op::Sub:
                result_ = TypeRules::resultType('-', lhsTy, rhsTy);
                return;
            case BinaryExpr::Op::Mul:
                result_ = TypeRules::resultType('*', lhsTy, rhsTy);
                return;
            case BinaryExpr::Op::Div:
                result_ = TypeRules::resultType('/', lhsTy, rhsTy);
                return;
            case BinaryExpr::Op::IDiv:
                result_ = TypeRules::resultType('\\', lhsTy, rhsTy);
                return;
            case BinaryExpr::Op::Mod:
                result_ = TypeRules::resultType("MOD", lhsTy, rhsTy);
                return;
            case BinaryExpr::Op::Pow:
                result_ = TypeRules::resultType('^', lhsTy, rhsTy);
                return;
            default:
                result_ = NumericType::Long;
                return;
        }
    }

    /// @brief Classifies a builtin call from its declared numeric semantics.
    /// @param call Builtin call whose kind and, for STR$, first argument are
    ///        inspected.
    void visit(const BuiltinCallExpr &call) override {
        switch (call.builtin) {
            case BuiltinCallExpr::Builtin::Cint:
                result_ = NumericType::Integer;
                return;
            case BuiltinCallExpr::Builtin::Clng:
                result_ = NumericType::Long;
                return;
            case BuiltinCallExpr::Builtin::Csng:
                result_ = NumericType::Single;
                return;
            case BuiltinCallExpr::Builtin::Cdbl:
                result_ = NumericType::Double;
                return;
            // BUG-OOP-016 fix: Int, Fix, Floor, Ceil, Abs return integers
            case BuiltinCallExpr::Builtin::Int:
            case BuiltinCallExpr::Builtin::Fix:
            case BuiltinCallExpr::Builtin::Floor:
            case BuiltinCallExpr::Builtin::Ceil:
            case BuiltinCallExpr::Builtin::Abs:
                result_ = NumericType::Long;
                return;
            case BuiltinCallExpr::Builtin::Round:
            case BuiltinCallExpr::Builtin::Sqr:
            case BuiltinCallExpr::Builtin::Sin:
            case BuiltinCallExpr::Builtin::Cos:
            case BuiltinCallExpr::Builtin::Pow:
            case BuiltinCallExpr::Builtin::Rnd:
            case BuiltinCallExpr::Builtin::Val:
                result_ = NumericType::Double;
                return;
            case BuiltinCallExpr::Builtin::Str:
                if (!call.args.empty() && call.args[0]) {
                    result_ = lowerer_.classifyNumericType(*call.args[0]);
                } else {
                    result_ = NumericType::Long;
                }
                return;
            default:
                result_ = NumericType::Double;
                return;
        }
    }

    /// @brief Classifies LBOUND as a Long integer result.
    /// The visited node is borrowed and otherwise ignored.
    void visit(const LBoundExpr &) override {
        result_ = NumericType::Long;
    }

    /// @brief Classifies UBOUND as a Long integer result.
    /// The visited node is borrowed and otherwise ignored.
    void visit(const UBoundExpr &) override {
        result_ = NumericType::Long;
    }

    /// @brief Classifies a procedure call from its resolved return signature.
    /// @param callExpr Call expression whose callee signature is queried.
    void visit(const CallExpr &callExpr) override {
        if (const auto *sig = lowerer_.findProcSignature(callExpr.callee)) {
            switch (sig->retType.kind) {
                case IlType::Kind::I16:
                    result_ = NumericType::Integer;
                    return;
                case IlType::Kind::I32:
                case IlType::Kind::I64:
                    result_ = NumericType::Long;
                    return;
                case IlType::Kind::F64:
                    result_ = NumericType::Double;
                    return;
                default:
                    break;
            }
        }
        result_ = NumericType::Long;
    }

    /// @brief Assigns the pointer-like object-allocation fallback category.
    /// The visited node is borrowed and otherwise ignored.
    void visit(const NewExpr &) override {
        result_ = NumericType::Long;
    }

    /// @brief Assigns the pointer-like ME-reference fallback category.
    /// The visited node is borrowed and otherwise ignored.
    void visit(const MeExpr &) override {
        result_ = NumericType::Long;
    }

    /// @brief Assigns the conservative category for member access.
    /// The visited node is borrowed and otherwise ignored.
    void visit(const MemberAccessExpr &) override {
        result_ = NumericType::Long;
    }

    /// @brief Assigns the conservative category for method calls.
    /// The visited node is borrowed and otherwise ignored.
    void visit(const MethodCallExpr &) override {
        result_ = NumericType::Long;
    }

    /// @brief Classifies an object identity test as a Long BASIC boolean.
    /// The visited node is borrowed and otherwise ignored.
    void visit(const IsExpr &) override {
        // Boolean result
        result_ = NumericType::Long;
    }

    /// @brief Propagates the value category through an AS expression.
    /// @param as Type-assertion expression whose value is classified.
    void visit(const AsExpr &as) override {
        // Classify underlying value
        if (as.value)
            result_ = lowerer_.classifyNumericType(*as.value);
        else
            result_ = NumericType::Long;
    }

    /// @brief Assigns the Long fallback category to an ADDRESSOF pointer.
    /// The visited node is borrowed and otherwise ignored.
    void visit(const AddressOfExpr &) override {
        // ADDRESSOF yields a pointer, not a numeric type. Default to Long.
        result_ = NumericType::Long;
    }

  private:
    /// Borrowed lowering context used for recursive classification and lookups.
    Lowerer &lowerer_;
    /// Category accumulated by the active visit.
    NumericType result_{NumericType::Long};
};

/// @brief Classify an expression's numeric result type.
/// @details Uses a visitor to walk the expression tree and determine what
///          numeric type the expression will produce, following BASIC type
///          promotion rules.
/// @param expr Expression to classify.
/// @return The numeric type category of the expression result.
TypeRules::NumericType Lowerer::classifyNumericType(const Expr &expr) {
    NumericTypeClassifier classifier(*this);
    expr.accept(classifier);
    return classifier.result();
}

} // namespace il::frontends::basic
