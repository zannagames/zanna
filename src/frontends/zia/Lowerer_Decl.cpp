//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file Lowerer_Decl.cpp
/// @brief Dispatches Zia declarations and pre-registers enum and `final`
///        constant values.
///
/// @details Declaration dispatch delegates IL-producing work to the focused
///          function and type lowerers. Before expression lowering, this file
///          also evaluates foldable global constants to a fixpoint so forward
///          references and enum variants are available to later declarations.
///          Namespace traversal temporarily threads the current qualified-name
///          prefix through nested declaration lists.
///
//===----------------------------------------------------------------------===//

#include "frontends/zia/Lowerer.hpp"
#include "frontends/zia/RuntimeNames.hpp"
#include "frontends/zia/ZiaLocationScope.hpp"

#include "il/core/Linkage.hpp"
#include <functional>

namespace il::frontends::zia {

using namespace runtime;

//=============================================================================
// Declaration Lowering
//=============================================================================

/// @brief Dispatch a declaration to the type-specific lowering routine for its kind.
/// @param decl Declaration to lower; null is ignored.
/// @details Functions, structs, classes, interfaces, globals, namespaces, and enums route to
///          their dedicated `lower*Decl` methods. Type aliases emit no IL (resolved in sema).
void Lowerer::lowerDecl(Decl *decl) {
    if (!decl)
        return;

    switch (decl->kind) {
        case DeclKind::Function:
            lowerFunctionDecl(*static_cast<FunctionDecl *>(decl));
            break;
        case DeclKind::Struct:
            lowerStructDecl(*static_cast<StructDecl *>(decl));
            break;
        case DeclKind::Class:
            lowerClassDecl(*static_cast<ClassDecl *>(decl));
            break;
        case DeclKind::Interface:
            lowerInterfaceDecl(*static_cast<InterfaceDecl *>(decl));
            break;
        case DeclKind::GlobalVar:
            lowerGlobalVarDecl(*static_cast<GlobalVarDecl *>(decl));
            break;
        case DeclKind::Namespace:
            lowerNamespaceDecl(*static_cast<NamespaceDecl *>(decl));
            break;
        case DeclKind::Enum:
            lowerEnumDecl(*static_cast<EnumDecl *>(decl));
            break;
        case DeclKind::TypeAlias:
            break; // Type aliases are resolved at sema time, no IL needed
        default:
            break;
    }
}

/// @brief Prefix a name with the current namespace, if any.
/// @param name Unqualified declaration name.
/// @return `namespacePrefix_.name` inside a namespace, otherwise @p name unchanged.
std::string Lowerer::qualifyName(const std::string &name) const {
    if (namespacePrefix_.empty())
        return name;
    return namespacePrefix_ + "." + name;
}

/// @brief Resolve the lowered name to use for a declaration.
/// @param decl Declaration whose canonical name is wanted.
/// @param name Fallback unqualified name.
/// @return The name sema recorded for @p decl (handling collisions/qualification), or the
///         namespace-qualified @p name when sema has no entry.
std::string Lowerer::declarationName(const Decl &decl, const std::string &name) const {
    auto it = sema_.semanticDeclNames_.find(&decl);
    if (it != sema_.semanticDeclNames_.end())
        return it->second;
    return qualifyName(name);
}

//=============================================================================
// Compile-Time Constant Folding Helper
//=============================================================================

/// @brief Try to evaluate an initializer expression to a compile-time constant.
/// @param init Initializer expression to evaluate, or null.
/// @return Folded IL constant when every required operation and reference is
///         compile-time evaluable; otherwise `std::nullopt`.
/// @details Handles literals, constant references, enum variants, unary operators,
///          and pure arithmetic/logical expressions. Returns nullopt for any
///          expression that cannot be evaluated at compile time.
/// @note Fixes BUG-FE-011: non-literal final constant initializers (such as
///       `final X = 0 - 2147483647`) were previously silently dropped, causing
///       all references to resolve to constInt(0).
std::optional<il::core::Value> Lowerer::tryFoldNumericConstant(Expr *init) {
    if (!init)
        return std::nullopt;

    /// @brief Looks up a constant by resolved or namespace-qualified name.
    /// @param name Candidate constant name.
    /// @return Constant value when registered.
    auto lookupConstant = [&](const std::string &name) -> std::optional<Value> {
        if (auto it = globalConstants_.find(name); it != globalConstants_.end())
            return it->second;

        std::string qualified = qualifyName(name);
        if (qualified != name) {
            if (auto it = globalConstants_.find(qualified); it != globalConstants_.end())
                return it->second;
        }
        return std::nullopt;
    };

    /// @brief Recursively folds a supported constant expression.
    /// @param expr Expression to evaluate.
    /// @return Folded IL constant, or `std::nullopt` when not constant.
    std::function<std::optional<Value>(Expr *)> fold = [&](Expr *expr) -> std::optional<Value> {
        if (!expr)
            return std::nullopt;

        if (auto *intLit = dynamic_cast<IntLiteralExpr *>(expr))
            return Value::constInt(intLit->value);
        if (auto *numLit = dynamic_cast<NumberLiteralExpr *>(expr))
            return Value::constFloat(numLit->value);
        if (auto *boolLit = dynamic_cast<BoolLiteralExpr *>(expr))
            return Value::constBool(boolLit->value);
        if (auto *strLit = dynamic_cast<StringLiteralExpr *>(expr))
            return Value::constStr(stringTable_.intern(strLit->value));
        if (auto *ident = dynamic_cast<IdentExpr *>(expr)) {
            std::string resolvedName = sema_.resolvedIdentifierName(ident);
            return lookupConstant(resolvedName.empty() ? ident->name : resolvedName);
        }

        if (auto *fieldExpr = dynamic_cast<FieldExpr *>(expr)) {
            std::string resolvedSymbol = sema_.resolvedFieldSymbolName(fieldExpr);
            if (!resolvedSymbol.empty()) {
                if (auto constIt = globalConstants_.find(resolvedSymbol);
                    constIt != globalConstants_.end()) {
                    return constIt->second;
                }
            }

            /// @brief Reconstructs a dotted identifier/field expression name.
            /// @param node Expression node to inspect.
            /// @return Qualified name when the expression is a pure name chain.
            std::function<std::optional<std::string>(Expr *)> buildQualifiedName =
                [&](Expr *node) -> std::optional<std::string> {
                if (auto *ident = dynamic_cast<IdentExpr *>(node))
                    return ident->name;
                if (auto *field = dynamic_cast<FieldExpr *>(node)) {
                    auto base = buildQualifiedName(field->base.get());
                    if (base)
                        return *base + "." + field->field;
                }
                return std::nullopt;
            };

            if (auto qualified = buildQualifiedName(fieldExpr)) {
                if (auto enumIt = enumVariantValues_.find(*qualified);
                    enumIt != enumVariantValues_.end()) {
                    return Value::constInt(enumIt->second);
                }
                if (auto constIt = globalConstants_.find(*qualified);
                    constIt != globalConstants_.end()) {
                    return constIt->second;
                }
            }

            return lookupConstant(fieldExpr->field);
        }

        if (auto *unary = dynamic_cast<UnaryExpr *>(expr)) {
            auto inner = fold(unary->operand.get());
            if (!inner)
                return std::nullopt;

            switch (unary->op) {
                case UnaryOp::Neg:
                    if (inner->kind == Value::Kind::ConstInt)
                        return Value::constInt(-inner->i64);
                    if (inner->kind == Value::Kind::ConstFloat)
                        return Value::constFloat(-inner->f64);
                    break;
                case UnaryOp::Not:
                    if (inner->kind == Value::Kind::ConstInt && inner->isBool)
                        return Value::constBool(inner->i64 == 0);
                    break;
                case UnaryOp::BitNot:
                    if (inner->kind == Value::Kind::ConstInt && !inner->isBool)
                        return Value::constInt(~inner->i64);
                    break;
                default:
                    break;
            }
            return std::nullopt;
        }

        if (auto *binary = dynamic_cast<BinaryExpr *>(expr)) {
            auto lhs = fold(binary->left.get());
            auto rhs = fold(binary->right.get());
            if (!lhs || !rhs)
                return std::nullopt;

            const bool lhsInt = lhs->kind == Value::Kind::ConstInt && !lhs->isBool;
            const bool rhsInt = rhs->kind == Value::Kind::ConstInt && !rhs->isBool;
            const bool lhsBool = lhs->kind == Value::Kind::ConstInt && lhs->isBool;
            const bool rhsBool = rhs->kind == Value::Kind::ConstInt && rhs->isBool;
            const bool lhsFloat = lhs->kind == Value::Kind::ConstFloat;
            const bool rhsFloat = rhs->kind == Value::Kind::ConstFloat;

            if (lhsInt && rhsInt) {
                const int64_t l = lhs->i64;
                const int64_t r = rhs->i64;
                switch (binary->op) {
                    case BinaryOp::Add:
                        return Value::constInt(l + r);
                    case BinaryOp::Sub:
                        return Value::constInt(l - r);
                    case BinaryOp::Mul:
                        return Value::constInt(l * r);
                    case BinaryOp::Div:
                        return r == 0 ? std::nullopt : std::optional<Value>(Value::constInt(l / r));
                    case BinaryOp::Mod:
                        return r == 0 ? std::nullopt : std::optional<Value>(Value::constInt(l % r));
                    case BinaryOp::Eq:
                        return Value::constBool(l == r);
                    case BinaryOp::Ne:
                        return Value::constBool(l != r);
                    case BinaryOp::Lt:
                        return Value::constBool(l < r);
                    case BinaryOp::Le:
                        return Value::constBool(l <= r);
                    case BinaryOp::Gt:
                        return Value::constBool(l > r);
                    case BinaryOp::Ge:
                        return Value::constBool(l >= r);
                    case BinaryOp::BitAnd:
                        return Value::constInt(l & r);
                    case BinaryOp::BitOr:
                        return Value::constInt(l | r);
                    case BinaryOp::BitXor:
                        return Value::constInt(l ^ r);
                    case BinaryOp::Shl:
                        return Value::constInt(l << r);
                    case BinaryOp::Shr:
                        return Value::constInt(l >> r);
                    default:
                        break;
                }
            }

            if ((lhsInt || lhsFloat) && (rhsInt || rhsFloat)) {
                const double l = lhsFloat ? lhs->f64 : static_cast<double>(lhs->i64);
                const double r = rhsFloat ? rhs->f64 : static_cast<double>(rhs->i64);
                switch (binary->op) {
                    case BinaryOp::Add:
                        return Value::constFloat(l + r);
                    case BinaryOp::Sub:
                        return Value::constFloat(l - r);
                    case BinaryOp::Mul:
                        return Value::constFloat(l * r);
                    case BinaryOp::Div:
                        return r == 0.0 ? std::nullopt
                                        : std::optional<Value>(Value::constFloat(l / r));
                    case BinaryOp::Eq:
                        return Value::constBool(l == r);
                    case BinaryOp::Ne:
                        return Value::constBool(l != r);
                    case BinaryOp::Lt:
                        return Value::constBool(l < r);
                    case BinaryOp::Le:
                        return Value::constBool(l <= r);
                    case BinaryOp::Gt:
                        return Value::constBool(l > r);
                    case BinaryOp::Ge:
                        return Value::constBool(l >= r);
                    default:
                        break;
                }
            }

            if (lhsBool && rhsBool) {
                const bool l = lhs->i64 != 0;
                const bool r = rhs->i64 != 0;
                switch (binary->op) {
                    case BinaryOp::And:
                        return Value::constBool(l && r);
                    case BinaryOp::Or:
                        return Value::constBool(l || r);
                    case BinaryOp::Eq:
                        return Value::constBool(l == r);
                    case BinaryOp::Ne:
                        return Value::constBool(l != r);
                    default:
                        break;
                }
            }
        }

        return std::nullopt;
    };

    return fold(init);
}

//=============================================================================
// Final Constant Pre-Registration
//=============================================================================

/// @brief Pre-compute the integer value of every enum variant in a declaration list.
/// @param declarations Top-level or namespace-scoped declarations to scan.
/// @details Variants are I64 constants: values auto-increment from 0 unless an explicit value
///          resets the running counter. Results are stored in @c enumVariantValues_ keyed by
///          `<EnumQualifiedName>.<Variant>`. Recurses into namespaces with prefix threading.
void Lowerer::registerAllEnumValues(std::vector<DeclPtr> &declarations) {
    for (auto &decl : declarations) {
        if (decl->kind == DeclKind::Enum) {
            auto *enumDecl = static_cast<EnumDecl *>(decl.get());
            int64_t nextValue = 0;
            std::string enumName = declarationName(*enumDecl, enumDecl->name);
            for (const auto &variant : enumDecl->variants) {
                std::string key = enumName + "." + variant.name;
                if (variant.explicitValue.has_value())
                    nextValue = *variant.explicitValue;
                enumVariantValues_[key] = nextValue;
                if (&variant == &enumDecl->variants.back() || nextValue == INT64_MAX)
                    continue;
                ++nextValue;
            }
        } else if (decl->kind == DeclKind::Namespace) {
            auto *ns = static_cast<NamespaceDecl *>(decl.get());
            std::string savedPrefix = namespacePrefix_;
            if (namespacePrefix_.empty())
                namespacePrefix_ = ns->name;
            else
                namespacePrefix_ = namespacePrefix_ + "." + ns->name;
            registerAllEnumValues(ns->declarations);
            namespacePrefix_ = savedPrefix;
        }
    }
}

/// @brief Pre-register all compile-time-foldable `final` constants before expression lowering.
/// @param declarations Top-level or namespace-scoped declarations to scan.
/// @details Collects every `final` global with an initializer (recursing namespaces), then
///          repeatedly folds them via tryFoldNumericConstant() in a fixpoint loop so finals
///          that reference other finals resolve regardless of declaration order. Non-foldable
///          finals are left for runtime initialization via lowerGlobalVarDecl().
void Lowerer::registerAllFinalConstants(std::vector<DeclPtr> &declarations) {
    struct PendingFinal {
        GlobalVarDecl *decl;
        std::string qualifiedName;
    };

    std::vector<PendingFinal> pending;
    /// @brief Recursively collects final declarations awaiting constant folding.
    /// @param decls Declaration list in the active namespace.
    std::function<void(std::vector<DeclPtr> &)> collectPending = [&](std::vector<DeclPtr> &decls) {
        for (auto &decl : decls) {
            if (decl->kind == DeclKind::GlobalVar) {
                auto *gvar = static_cast<GlobalVarDecl *>(decl.get());
                if (gvar->isFinal && gvar->initializer) {
                    pending.push_back({gvar, declarationName(*gvar, gvar->name)});
                }
                continue;
            }

            if (decl->kind != DeclKind::Namespace)
                continue;

            auto *ns = static_cast<NamespaceDecl *>(decl.get());
            std::string savedPrefix = namespacePrefix_;
            if (namespacePrefix_.empty())
                namespacePrefix_ = ns->name;
            else
                namespacePrefix_ = namespacePrefix_ + "." + ns->name;

            collectPending(ns->declarations);
            namespacePrefix_ = savedPrefix;
        }
    };

    collectPending(declarations);

    bool madeProgress = true;
    while (madeProgress) {
        madeProgress = false;

        for (const auto &entry : pending) {
            if (globalConstants_.find(entry.qualifiedName) != globalConstants_.end())
                continue;

            if (auto folded = tryFoldNumericConstant(entry.decl->initializer.get())) {
                globalConstants_[entry.qualifiedName] = *folded;
                madeProgress = true;
            }
        }
    }

    // Non-foldable finals are valid runtime-initialized immutable globals.
    // They are registered by lowerGlobalVarDecl() and initialized from
    // emitGlobalInitializers() alongside mutable globals.
}

//=============================================================================
// Namespace and Enum Declaration Lowering
//=============================================================================

/// @brief Lower all declarations nested inside a namespace.
/// @param decl Namespace declaration AST node.
/// @details Extends @c namespacePrefix_ for the duration so nested declarations lower under
///          their qualified names, then restores the previous prefix on exit.
void Lowerer::lowerNamespaceDecl(NamespaceDecl &decl) {
    ZiaLocationScope locScope(*this, decl.loc);

    // Save current namespace prefix
    std::string savedPrefix = namespacePrefix_;

    // Compute new prefix
    if (namespacePrefix_.empty())
        namespacePrefix_ = decl.name;
    else
        namespacePrefix_ = namespacePrefix_ + "." + decl.name;

    // Lower all declarations inside the namespace
    for (auto &innerDecl : decl.declarations) {
        lowerDecl(innerDecl.get());
    }

    // Restore previous prefix
    namespacePrefix_ = savedPrefix;
}

/// @brief Lower an enum declaration by registering its variant values.
/// @param decl Enum declaration AST node.
/// @details Enums emit no IL structure; each variant is an I64 constant. Values auto-increment
///          from 0 unless an explicit value resets the counter, and are stored in
///          @c enumVariantValues_ for use during expression lowering.
void Lowerer::lowerEnumDecl(EnumDecl &decl) {
    // Enums don't produce IL structures -- each variant is an I64 constant.
    // Just register variant values for later lookup during expression lowering.
    int64_t nextValue = 0;
    std::string enumName = declarationName(decl, decl.name);
    for (const auto &variant : decl.variants) {
        std::string key = enumName + "." + variant.name;
        if (variant.explicitValue.has_value())
            nextValue = *variant.explicitValue;
        enumVariantValues_[key] = nextValue;
        if (&variant == &decl.variants.back() || nextValue == INT64_MAX)
            continue;
        ++nextValue;
    }
}

} // namespace il::frontends::zia
