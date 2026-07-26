//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
// File: src/frontends/basic/lower/Scan_ExprTypes.cpp
// Purpose: Implements expression-type inference for BASIC scan passes.
// Key invariants: Produces expression classifications without mutating runtime
//                 flags or emitting IL.
// Ownership/Lifetime: Operates on Lowerer state without owning AST or module.
// Links: docs/internals/codemap.md, docs/tutorials/basic-tutorial.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements side-effect-free scan-time expression classification.

#include "frontends/basic/AstWalker.hpp"
#include "frontends/basic/BuiltinRegistry.hpp"
#include "frontends/basic/Lowerer.hpp"
#include "frontends/basic/TypeSuffix.hpp"
#include "frontends/basic/sem/RuntimeMethodIndex.hpp"
#include "il/runtime/RuntimeClassNames.hpp"
#include "il/runtime/classes/RuntimeClasses.hpp"
#include "zanna/il/Module.hpp"
#include <cassert>
#include <optional>
#include <vector>

namespace il::frontends::basic::lower {
/// @brief Determine the resulting type for a builtin call without lowering.
///
/// @param lowerer Lowerer supplying symbol/type caches.
/// @param expr Builtin call expression to analyse.
/// @return Classification describing the builtin's result type.
Lowerer::ExprType scanBuiltinExprTypes(Lowerer &lowerer, const BuiltinCallExpr &expr);

namespace detail {

/// @brief Translate AST-level type annotations to Lowerer expression kinds.
///
/// @param ty AST type enumerator recorded on declarations.
/// @return Lowerer expression classification matching @p ty.
Lowerer::ExprType exprTypeFromAstType(Type ty) {
    switch (ty) {
        case Type::Str:
            return Lowerer::ExprType::Str;
        case Type::F64:
            return Lowerer::ExprType::F64;
        case Type::Bool:
            return Lowerer::ExprType::Bool;
        case Type::I64:
        default:
            return Lowerer::ExprType::I64;
    }
}

/// @brief Converts a lowering expression category to semantic BasicType.
/// @param ty Scan-time expression category.
/// @return Corresponding semantic type; I64 maps to BasicType::Int.
BasicType basicTypeFromExprType(Lowerer::ExprType ty) {
    switch (ty) {
        case Lowerer::ExprType::F64:
            return BasicType::Float;
        case Lowerer::ExprType::Str:
            return BasicType::String;
        case Lowerer::ExprType::Bool:
            return BasicType::Bool;
        case Lowerer::ExprType::Obj:
            return BasicType::Object;
        case Lowerer::ExprType::I64:
        default:
            return BasicType::Int;
    }
}

/// @brief Converts a semantic BasicType to the lowerer's scan category.
/// @param ty Semantic type returned by runtime method lookup.
/// @return Corresponding expression category; void, unknown, and integer types
///         conservatively map to I64.
Lowerer::ExprType exprTypeFromBasicType(BasicType ty) {
    switch (ty) {
        case BasicType::Float:
            return Lowerer::ExprType::F64;
        case BasicType::String:
            return Lowerer::ExprType::Str;
        case BasicType::Bool:
            return Lowerer::ExprType::Bool;
        case BasicType::Object:
            return Lowerer::ExprType::Obj;
        case BasicType::Int:
        case BasicType::Void:
        case BasicType::Unknown:
        default:
            return Lowerer::ExprType::I64;
    }
}

/// @brief AST walker that infers expression types during the scan phase.
///
/// @details The scanner pushes inferred @ref Lowerer::ExprType values onto a
///          private stack while traversing the AST.  It cooperates with the
///          lowerer to resolve symbols without mutating IR generation state.
class ExprTypeScanner final : public BasicAstWalker<ExprTypeScanner> {
  public:
    /// @brief Construct a scanner bound to the owning lowering context.
    ///
    /// @param lowerer Reference to the BASIC lowerer providing symbol data.
    explicit ExprTypeScanner(Lowerer &lowerer) noexcept : lowerer_(lowerer) {}

    using ExprType = Lowerer::ExprType;

    /// @brief Evaluate @p expr and return its inferred type classification.
    ///
    /// @param expr Expression to analyse.
    /// @return Expression category such as integer, floating, or string.
    ExprType evaluateExpr(const Expr &expr) {
        StackScope scope(*this);
        expr.accept(*this);
        return pop();
    }

    /// @brief Skip builtin arguments because specialised rules handle them.
    /// The borrowed call node is handled manually by @ref after.
    /// @return False to prevent recursive descent.
    bool shouldVisitChildren(const BuiltinCallExpr &) {
        return false;
    }

    /// @brief Skip procedure call children to avoid double counting side effects.
    /// The borrowed call node's arguments are consumed manually by @ref after.
    /// @return False so the walker avoids visiting argument subtrees automatically.
    bool shouldVisitChildren(const CallExpr &) {
        return false;
    }

    /// @brief Skip constructor arguments because custom logic consumes them.
    /// The borrowed construction node is handled manually by @ref after.
    /// @return False to defer traversal to helper routines.
    bool shouldVisitChildren(const NewExpr &) {
        return false;
    }

    /// @brief Skip member access children; the base is handled manually.
    /// The borrowed member node is handled manually by @ref after.
    /// @return False to avoid automatic recursion.
    bool shouldVisitChildren(const MemberAccessExpr &) {
        return false;
    }

    /// @brief Skip method call arguments because explicit handling is required.
    /// The borrowed method-call node is handled manually by @ref after.
    /// @return False to maintain manual traversal order.
    bool shouldVisitChildren(const MethodCallExpr &) {
        return false;
    }

    /// @brief Classify integer literals as 64-bit integers.
    /// The borrowed literal value is not needed for this classification.
    void after(const IntExpr &) {
        push(ExprType::I64);
    }

    /// @brief Classify floating literals as 64-bit floats.
    /// The borrowed literal value is not needed for this classification.
    void after(const FloatExpr &) {
        push(ExprType::F64);
    }

    /// @brief Classify string literals as strings.
    /// The borrowed literal value is not needed for this classification.
    void after(const StringExpr &) {
        push(ExprType::Str);
    }

    /// @brief Treat boolean literals as integer flags (historical BASIC rule).
    /// The borrowed literal value is not needed for this classification.
    void after(const BoolExpr &) {
        push(ExprType::I64);
    }

    /// @brief Resolve variable references using known symbol metadata.
    ///
    /// @param expr Variable reference to analyse.
    void after(const VarExpr &expr) {
        ExprType result = ExprType::I64;
        if (const auto *info = lowerer_.findSymbol(expr.name)) {
            if (info->hasType)
                result = exprTypeFromAstType(info->type);
            else
                result = exprTypeFromAstType(inferAstTypeFromName(expr.name));
        } else {
            result = exprTypeFromAstType(inferAstTypeFromName(expr.name));
        }
        push(result);
    }

    /// @brief Infer array element access and consume optional index types.
    ///
    /// @param expr Array expression possibly containing an index.
    void after(const ArrayExpr &expr) {
        // BUG-091 fix: Handle multi-dimensional arrays by discarding all index expressions
        // For backwards compatibility, check deprecated 'index' field first
        if (expr.index != nullptr) {
            (void)pop();
        }
        // Handle multi-dimensional arrays via 'indices' vector
        else if (!expr.indices.empty()) {
            for (size_t i = 0; i < expr.indices.size(); ++i) {
                (void)pop();
            }
        }
        push(ExprType::I64);
    }

    /// @brief Treat LBOUND queries as integer expressions.
    void after(const LBoundExpr &) {
        push(ExprType::I64);
    }

    /// @brief Treat UBOUND queries as integer expressions.
    void after(const UBoundExpr &) {
        push(ExprType::I64);
    }

    /// @brief Propagate operand classification through unary operators.
    void after(const UnaryExpr &) {
        ExprType operand = pop();
        push(operand);
    }

    /// @brief Combine operand types for binary operations.
    ///
    /// @param expr Binary expression describing the operator.
    void after(const BinaryExpr &expr) {
        ExprType rhs = pop();
        ExprType lhs = pop();
        push(combineBinary(expr, lhs, rhs));
    }

    /// @brief Delegate builtin classification to the shared helper.
    ///
    /// @param expr Builtin call expression.
    void after(const BuiltinCallExpr &expr) {
        push(::il::frontends::basic::lower::scanBuiltinExprTypes(lowerer_, expr));
    }

    /// @brief Use stored procedure signatures to classify call expressions.
    ///
    /// @param expr Procedure call expression.
    void after(const CallExpr &expr) {
        for (const auto &arg : expr.args) {
            if (!arg)
                continue;
            consumeExpr(*arg);
        }
        if (const auto *sig = lowerer_.findProcSignature(expr.callee)) {
            using K = il::core::Type::Kind;
            switch (sig->retType.kind) {
                case K::F64:
                    push(ExprType::F64);
                    break;
                case K::Ptr:
                case K::I1:
                case K::I16:
                case K::I32:
                case K::I64:
                    push(ExprType::I64);
                    break;
                case K::Str:
                    push(ExprType::Str);
                    break;
                case K::Void:
                default:
                    push(ExprType::I64);
                    break;
            }
        } else {
            push(ExprType::I64);
        }
    }

    /// @brief Classify object construction expressions as integer handles.
    ///
    /// @param expr New expression containing constructor arguments.
    void after(const NewExpr &expr) {
        for (const auto &arg : expr.args) {
            if (!arg)
                continue;
            consumeExpr(*arg);
        }
        push(ExprType::Obj);
    }

    /// @brief Treat ME references as object handles to the current object.
    void after(const MeExpr &) {
        push(ExprType::Obj);
    }

    /// @brief Resolve member access result types from cached class layouts.
    ///
    /// @param expr Member access expression being evaluated.
    void after(const MemberAccessExpr &expr) {
        ExprType result = ExprType::I64;
        if (expr.base) {
            consumeExpr(*expr.base);
            std::string className = lowerer_.resolveObjectClass(*expr.base);
            // BUG-011 fix: Use findClassLayout() instead of direct lookup to handle
            // case-insensitive class names and qualified/unqualified name variants.
            if (const auto *layout = lowerer_.findClassLayout(className)) {
                if (const auto *field = layout->findField(expr.member))
                    result = exprTypeFromAstType(field->type);
            }
        }
        push(result);
    }

    /// @brief Treat method calls as integer-returning after consuming arguments.
    ///
    /// @param expr Method call expression encountered during scanning.
    void after(const MethodCallExpr &expr) {
        ExprType baseType = ExprType::I64;
        if (expr.base)
            baseType = consumeExpr(*expr.base);

        std::vector<BasicType> runtimeArgTypes;
        runtimeArgTypes.reserve(expr.args.size());
        for (const auto &arg : expr.args) {
            if (!arg) {
                runtimeArgTypes.push_back(BasicType::Unknown);
                continue;
            }
            runtimeArgTypes.push_back(basicTypeFromExprType(consumeExpr(*arg)));
        }

        ExprType result = ExprType::I64;
        if (expr.base) {
            std::string className = lowerer_.resolveObjectClass(*expr.base);
            std::string runtimeClass =
                className.empty() ? std::string{} : lowerer_.qualify(className);
            if (runtimeClass.empty() && baseType == ExprType::Str)
                runtimeClass = std::string(il::runtime::RTCLASS_STRING);

            if (!runtimeClass.empty() && il::runtime::findRuntimeClassByQName(runtimeClass)) {
                if (auto info =
                        runtimeMethodIndex().find(runtimeClass, expr.method, runtimeArgTypes))
                    result = exprTypeFromBasicType(info->ret);
            }

            if (result == ExprType::I64) {
                if (!lowerer_.findMethodReturnClassName(className, expr.method, expr.args.size())
                         .empty()) {
                    result = ExprType::Obj;
                } else if (auto retTy = lowerer_.findMethodReturnType(className, expr.method)) {
                    result = exprTypeFromAstType(*retTy);
                }
            }
        }
        push(result);
    }

  private:
    struct StackScope {
        /// @brief Reference to the owning walker used for validation.
        ExprTypeScanner &walker;
        /// @brief Depth snapshot captured on scope entry.
        std::size_t depth;

        /// @brief Record current stack depth to enforce balance at scope exit.
        explicit StackScope(ExprTypeScanner &w) : walker(w), depth(w.exprStack_.size()) {}

        /// @brief Assert that the stack depth matches the entry snapshot.
        ~StackScope() {
            assert(walker.exprStack_.size() == depth && "expression stack imbalance");
        }
    };

    /// @brief Push a classification onto the evaluation stack.
    ///
    /// @param ty Expression classification to record.
    void push(ExprType ty) {
        exprStack_.push_back(ty);
    }

    /// @brief Pop and return the most recent classification.
    ///
    /// @return Last computed classification.
    ExprType pop() {
        assert(!exprStack_.empty());
        ExprType ty = exprStack_.back();
        exprStack_.pop_back();
        return ty;
    }

    /// @brief Conditionally discard the most recent classification.
    ///
    /// @param condition When true the top of the stack is removed.
    void discardIf(bool condition) {
        if (condition)
            (void)pop();
    }

    /// @brief Evaluate a child expression and return its classification.
    ///
    /// @param expr Expression subtree to consume.
    /// @return Computed classification of @p expr.
    ExprType consumeExpr(const Expr &expr) {
        expr.accept(*this);
        return pop();
    }

    /// @brief Combine operand classifications following BASIC promotion rules.
    ///
    /// @param expr Binary expression providing operator context.
    /// @param lhs Left-hand operand classification.
    /// @param rhs Right-hand operand classification.
    /// @return Resulting classification after promotions.
    ExprType combineBinary(const BinaryExpr &expr, ExprType lhs, ExprType rhs) {
        using Op = BinaryExpr::Op;
        if (expr.op == Op::Pow)
            return ExprType::F64;
        if (expr.op == Op::Add && lhs == ExprType::Str && rhs == ExprType::Str)
            return ExprType::Str;
        if (expr.op == Op::Eq || expr.op == Op::Ne)
            return ExprType::Bool;
        if (expr.op == Op::LogicalAndShort || expr.op == Op::LogicalOrShort)
            return ExprType::Bool;
        if (expr.op == Op::LogicalAnd || expr.op == Op::LogicalOr)
            return ExprType::I64;
        if (lhs == ExprType::F64 || rhs == ExprType::F64)
            return ExprType::F64;
        return ExprType::I64;
    }

    /// Borrowed lowering context supplying symbol, layout, and signature data.
    Lowerer &lowerer_;
    /// LIFO classifications produced by child visits.
    std::vector<ExprType> exprStack_{};
};

} // namespace detail

/// @brief Classify a standalone expression using the scan-time inference walker.
///
/// @param lowerer Lowerer providing symbol tables and inference caches.
/// @param expr Expression to classify.
/// @return Expression classification recorded by the scan.
Lowerer::ExprType scanExprTypes(Lowerer &lowerer, const Expr &expr) {
    detail::ExprTypeScanner scanner(lowerer);
    return scanner.evaluateExpr(expr);
}

/// @brief Determine builtin expression result types by consulting scan rules.
///
/// @param lowerer Lowerer providing rule lookup and expression scanner access.
/// @param expr Builtin call to analyse.
/// @return Resulting expression classification.
Lowerer::ExprType scanBuiltinExprTypes(Lowerer &lowerer, const BuiltinCallExpr &expr) {
    const auto &rule = getBuiltinScanRule(expr.builtin);
    std::vector<std::optional<Lowerer::ExprType>> argTypes(expr.args.size());

    /// @brief Scans one present builtin argument into the type cache.
    /// @param idx Argument index to scan.
    auto scanArg = [&](std::size_t idx) {
        if (idx >= expr.args.size())
            return;
        const auto &arg = expr.args[idx];
        if (!arg)
            return;
        detail::ExprTypeScanner scanner(lowerer);
        argTypes[idx] = scanner.evaluateExpr(*arg);
    };

    if (rule.traversal == BuiltinScanRule::ArgTraversal::All) {
        for (std::size_t i = 0; i < expr.args.size(); ++i)
            scanArg(i);
    } else {
        for (std::size_t idx : rule.explicitArgs)
            scanArg(idx);
    }

    /// @brief Retrieves a cached builtin argument type.
    /// @param idx Argument index to query.
    /// @return Cached type, or `std::nullopt` when absent or out of range.
    auto argType = [&](std::size_t idx) -> std::optional<Lowerer::ExprType> {
        if (idx >= argTypes.size())
            return std::nullopt;
        return argTypes[idx];
    };

    Lowerer::ExprType result = rule.result.type;
    if (rule.result.kind == BuiltinScanRule::ResultSpec::Kind::FromArg) {
        if (auto ty = argType(rule.result.argIndex))
            result = *ty;
    }
    return result;
}

} // namespace il::frontends::basic::lower
