//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file Sema_Expr.cpp
/// @brief Expression analysis dispatcher and literal analysis for the Zia
///        semantic analyzer.
/// @details Centralizes expression-kind dispatch, result caching, literal singleton types,
///          identifier resolution, and `self` validation. Complex expression families are
///          implemented in the companion `Sema_Expr_*` translation units.
///
//===----------------------------------------------------------------------===//

#include "frontends/zia/Sema.hpp"

#include <functional>

namespace il::frontends::zia {

//=============================================================================
// Expression Analysis Dispatcher
//=============================================================================

/// @brief Main entry point for expression analysis.
/// @param expr The expression AST node to analyze.
/// @return The resolved semantic type for the expression.
/// @details Dispatches to specific analysis methods based on expression kind.
///          Caches the result in exprTypes_ for later retrieval.
TypeRef Sema::analyzeExpr(Expr *expr) {
    if (!expr)
        return types::unknown();

    TypeRef result;

    switch (expr->kind) {
        case ExprKind::IntLiteral:
            result = analyzeIntLiteral(static_cast<IntLiteralExpr *>(expr));
            break;
        case ExprKind::NumberLiteral:
            result = analyzeNumberLiteral(static_cast<NumberLiteralExpr *>(expr));
            break;
        case ExprKind::StringLiteral:
            result = analyzeStringLiteral(static_cast<StringLiteralExpr *>(expr));
            break;
        case ExprKind::BoolLiteral:
            result = analyzeBoolLiteral(static_cast<BoolLiteralExpr *>(expr));
            break;
        case ExprKind::NullLiteral:
            result = analyzeNullLiteral(static_cast<NullLiteralExpr *>(expr));
            break;
        case ExprKind::UnitLiteral:
            result = analyzeUnitLiteral(static_cast<UnitLiteralExpr *>(expr));
            break;
        case ExprKind::Ident:
            result = analyzeIdent(static_cast<IdentExpr *>(expr));
            break;
        case ExprKind::SelfExpr:
            result = analyzeSelf(static_cast<SelfExpr *>(expr));
            break;
        case ExprKind::Binary:
            result = analyzeBinary(static_cast<BinaryExpr *>(expr));
            break;
        case ExprKind::Unary:
            result = analyzeUnary(static_cast<UnaryExpr *>(expr));
            break;
        case ExprKind::Ternary:
            result = analyzeTernary(static_cast<TernaryExpr *>(expr));
            break;
        case ExprKind::If:
            result = analyzeIfExpr(static_cast<IfExpr *>(expr));
            break;
        case ExprKind::StructLiteral:
            result = analyzeStructLiteral(static_cast<StructLiteralExpr *>(expr));
            break;
        case ExprKind::Call:
            result = analyzeCall(static_cast<CallExpr *>(expr));
            break;
        case ExprKind::Index:
            result = analyzeIndex(static_cast<IndexExpr *>(expr));
            break;
        case ExprKind::Field:
            result = analyzeField(static_cast<FieldExpr *>(expr));
            break;
        case ExprKind::OptionalChain:
            result = analyzeOptionalChain(static_cast<OptionalChainExpr *>(expr));
            break;
        case ExprKind::ForceUnwrap:
            result = analyzeForceUnwrap(static_cast<ForceUnwrapExpr *>(expr));
            break;
        case ExprKind::Coalesce:
            result = analyzeCoalesce(static_cast<CoalesceExpr *>(expr));
            break;
        case ExprKind::Try:
            result = analyzeTry(static_cast<TryExpr *>(expr));
            break;
        case ExprKind::Is:
            result = analyzeIs(static_cast<IsExpr *>(expr));
            break;
        case ExprKind::As:
            result = analyzeAs(static_cast<AsExpr *>(expr));
            break;
        case ExprKind::Range:
            result = analyzeRange(static_cast<RangeExpr *>(expr));
            break;
        case ExprKind::New:
            result = analyzeNew(static_cast<NewExpr *>(expr));
            break;
        case ExprKind::Lambda:
            result = analyzeLambda(static_cast<LambdaExpr *>(expr));
            break;
        case ExprKind::Match:
            result = analyzeMatchExpr(static_cast<MatchExpr *>(expr));
            break;
        case ExprKind::ListLiteral:
            result = analyzeListLiteral(static_cast<ListLiteralExpr *>(expr));
            break;
        case ExprKind::MapLiteral:
            result = analyzeMapLiteral(static_cast<MapLiteralExpr *>(expr));
            break;
        case ExprKind::SetLiteral:
            result = analyzeSetLiteral(static_cast<SetLiteralExpr *>(expr));
            break;
        case ExprKind::Tuple:
            result = analyzeTuple(static_cast<TupleExpr *>(expr));
            break;
        case ExprKind::TupleIndex:
            result = analyzeTupleIndex(static_cast<TupleIndexExpr *>(expr));
            break;
        case ExprKind::Block:
            result = analyzeBlockExpr(static_cast<BlockExpr *>(expr));
            break;
        case ExprKind::Await: {
            auto *awaitExpr = static_cast<AwaitExpr *>(expr);
            TypeRef operandType = analyzeExpr(awaitExpr->operand.get());
            TypeRef awaitedType = operandType;
            if (awaitedType && awaitedType->kind == TypeKindSem::Optional &&
                awaitedType->innerType())
                awaitedType = awaitedType->innerType();

            if (awaitedType && awaitedType->kind != TypeKindSem::Any &&
                awaitedType->kind != TypeKindSem::Unknown &&
                !(awaitedType->kind == TypeKindSem::Ptr &&
                  awaitedType->name == "Zanna.Threads.Future")) {
                error(expr->loc, "`await` expects Zanna.Threads.Future");
            }

            result = types::unknown();
            if (awaitedType && awaitedType->kind == TypeKindSem::Ptr &&
                awaitedType->name == "Zanna.Threads.Future" && !awaitedType->typeArgs.empty() &&
                awaitedType->typeArgs[0]) {
                result = awaitedType->typeArgs[0];
                break;
            }
            if (awaitedType && awaitedType->kind == TypeKindSem::Ptr &&
                awaitedType->name == "Zanna.Threads.Future") {
                error(expr->loc, "`await` requires a typed Zanna.Threads.Future[T]");
                break;
            }
            if (auto *call = dynamic_cast<CallExpr *>(awaitExpr->operand.get())) {
                if (FunctionDecl *asyncDecl = resolvedFunctionDecl(call);
                    asyncDecl && asyncDecl->isAsync) {
                    result = asyncDecl->returnType ? resolveTypeNode(asyncDecl->returnType.get())
                                                   : types::voidType();
                    break;
                }

                auto findAsyncDecl = [&](const std::string &name) -> FunctionDecl * {
                    if (FunctionDecl *decl = getFunctionDecl(name); decl && decl->isAsync)
                        return decl;
                    for (FunctionDecl *decl : getFunctionOverloads(name)) {
                        if (decl && decl->isAsync)
                            return decl;
                    }
                    return nullptr;
                };

                FunctionDecl *asyncDecl = nullptr;

                std::string calleeName = resolvedFunctionCallee(call);
                if (!calleeName.empty()) {
                    asyncDecl = findAsyncDecl(calleeName);
                }

                if (!asyncDecl) {
                    if (auto *ident = dynamic_cast<IdentExpr *>(call->callee.get())) {
                        if (Symbol *sym = lookupSymbol(ident->name);
                            sym && sym->kind == Symbol::Kind::Function && sym->decl)
                            asyncDecl = static_cast<FunctionDecl *>(sym->decl);

                        if (asyncDecl && !asyncDecl->isAsync)
                            asyncDecl = nullptr;

                        if (!asyncDecl)
                            asyncDecl = findAsyncDecl(ident->name);
                    } else if (auto *field = dynamic_cast<FieldExpr *>(call->callee.get())) {
                        std::function<bool(Expr *, std::string &)> buildName =
                            [&](Expr *node, std::string &out) -> bool {
                            if (auto *name = dynamic_cast<IdentExpr *>(node)) {
                                out = name->name;
                                return true;
                            }
                            if (auto *nested = dynamic_cast<FieldExpr *>(node)) {
                                if (!buildName(nested->base.get(), out))
                                    return false;
                                out += ".";
                                out += nested->field;
                                return true;
                            }
                            return false;
                        };

                        std::string dottedName;
                        if (buildName(field, dottedName)) {
                            if (Symbol *sym = lookupSymbol(dottedName);
                                sym && sym->kind == Symbol::Kind::Function && sym->decl)
                                asyncDecl = static_cast<FunctionDecl *>(sym->decl);

                            if (asyncDecl && !asyncDecl->isAsync)
                                asyncDecl = nullptr;

                            if (!asyncDecl)
                                asyncDecl = findAsyncDecl(dottedName);
                        }
                    }
                }

                if (asyncDecl && asyncDecl->isAsync)
                    result = asyncDecl->returnType ? resolveTypeNode(asyncDecl->returnType.get())
                                                   : types::voidType();
            }
            break;
        } break;
        default:
            result = types::unknown();
            break;
    }

    if (!result)
        result = types::unknown();
    exprTypes_[expr] = result;
    return result;
}

//=============================================================================
// Literal Analysis
//=============================================================================

/// @brief Analyze an integer literal expression.
/// @param expr Integer literal node; its value does not affect the semantic primitive type.
/// @return The Integer type singleton.
TypeRef Sema::analyzeIntLiteral(IntLiteralExpr * /*expr*/) {
    return types::integer();
}

/// @brief Analyze a floating-point number literal expression.
/// @param expr Number literal node; its value does not affect the semantic primitive type.
/// @return The Number type singleton.
TypeRef Sema::analyzeNumberLiteral(NumberLiteralExpr * /*expr*/) {
    return types::number();
}

/// @brief Analyze a string literal expression.
/// @param expr String literal node; its contents do not affect the semantic primitive type.
/// @return The String type singleton.
TypeRef Sema::analyzeStringLiteral(StringLiteralExpr * /*expr*/) {
    return types::string();
}

/// @brief Analyze a boolean literal expression (true/false).
/// @param expr Boolean literal node.
/// @return The Boolean type singleton.
TypeRef Sema::analyzeBoolLiteral(BoolLiteralExpr * /*expr*/) {
    return types::boolean();
}

/// @brief Analyze a null literal expression.
/// @param expr Null literal node.
/// @return Optional[Unknown] type; actual type determined by context.
TypeRef Sema::analyzeNullLiteral(NullLiteralExpr * /*expr*/) {
    // null is Optional[Unknown] - needs context to determine actual type
    return types::optional(types::unknown());
}

/// @brief Analyze a unit literal expression ().
/// @param expr Unit literal node.
/// @return The Unit type singleton.
TypeRef Sema::analyzeUnitLiteral(UnitLiteralExpr * /*expr*/) {
    return types::unit();
}

/// @brief Analyze an identifier expression.
/// @param expr The identifier expression node.
/// @return The type bound to the identifier, or Unknown if undefined.
/// @details Looks up the identifier in the symbol table and imported symbols.
///          For imported runtime classes, returns a module-like type.
TypeRef Sema::analyzeIdent(IdentExpr *expr) {
    std::string lookupName = expr->name;
    Symbol *sym = lookupAccessibleSymbol(lookupName, expr->loc);
    if (!sym && expr->loc.file_id != 0 && lookupName.find('.') == std::string::npos) {
        std::string scopedName = fileScopedDeclName(expr->loc.file_id, lookupName);
        if (scopedName != lookupName) {
            sym = lookupAccessibleSymbol(scopedName, expr->loc);
            if (sym)
                lookupName = std::move(scopedName);
        }
    }

    if (sym)
        resolvedIdentNames_[expr] = lookupName;

    if (sym && sym->kind == Symbol::Kind::Function && hasOverloadedFunctionName(lookupName)) {
        error(expr->loc,
              "Function '" + expr->name +
                  "' is overloaded and must be used in a call expression to resolve a specific "
                  "overload");
        return types::unknown();
    }
    if (!sym) {
        std::string scopedName = expr->loc.file_id != 0 && expr->name.find('.') == std::string::npos
                                     ? fileScopedDeclName(expr->loc.file_id, expr->name)
                                     : expr->name;
        if (hasOverloadedFunctionName(expr->name) ||
            (scopedName != expr->name && hasOverloadedFunctionName(scopedName))) {
            error(expr->loc,
                  "Function '" + expr->name +
                      "' is overloaded and must be used in a call expression to resolve a specific "
                      "overload");
            return types::unknown();
        }
        if (hasModuleExports(expr->name, expr->loc))
            return types::module(expr->name);

        if (currentSelfType_ && expr->name.find('.') == std::string::npos) {
            if (auto fieldOwner = findFieldOwner(currentSelfType_->name, expr->name)) {
                const std::string fieldKey = *fieldOwner + "." + expr->name;
                auto fieldVisIt = memberVisibility_.find(fieldKey);
                const bool isInsideDeclaringType =
                    currentSelfType_->name == *fieldOwner ||
                    types::isSubclassOf(currentSelfType_->name, *fieldOwner);
                if (fieldVisIt != memberVisibility_.end() &&
                    fieldVisIt->second == Visibility::Private && !isInsideDeclaringType) {
                    error(expr->loc,
                          "Cannot access private member '" + expr->name + "' of type '" +
                              *fieldOwner + "'");
                    return types::unknown();
                }

                TypeRef fieldType = getFieldType(currentSelfType_->name, expr->name);
                if (fieldType)
                    return fieldType;
            }
        }

        // Check if this is an imported symbol from a bound namespace
        auto importIt = importedSymbols_.find(expr->name);
        if (importIt != importedSymbols_.end()) {
            const std::string &fullName = importIt->second;
            if (fullName.rfind("Zanna.", 0) == 0) {
                Symbol *fnSym = lookupSymbol(fullName);
                if (fnSym && fnSym->kind == Symbol::Kind::Function && fnSym->isExtern) {
                    if (fnSym->type && fnSym->type->kind == TypeKindSem::Function &&
                        fnSym->type->paramTypes().empty()) {
                        autoEvalGetters_[expr] = fullName;
                        return normalizeRuntimeSurfaceType(fnSym->type->returnType());
                    }
                    return fnSym->type;
                }
                // For imported runtime classes, return a module-like type so that
                // field access (e.g., Canvas.New) can be resolved
                return types::module(fullName);
            }
        }

        errorUndefined(expr->loc, expr->name);
        return types::unknown();
    }

    // Mark symbol as used for W001 (unused variable) detection
    sym->used = true;

    // For variables and parameters, respect flow-sensitive type narrowing
    // (e.g., after `if x != null`, x is narrowed from T? to T)
    if (sym->kind == Symbol::Kind::Variable || sym->kind == Symbol::Kind::Parameter) {
        // Warn if variable used before initialization. Module-level globals
        // are exempt by construction: registration marks them initialized
        // (explicitly or default-initialized), and re-deriving their state
        // from the current scope chain misresolves when two modules declare
        // same-named globals, producing false positives in both modules.
        const bool moduleGlobal = sym->decl && sym->decl->kind == DeclKind::GlobalVar;
        if (sym->kind == Symbol::Kind::Variable && !moduleGlobal &&
            !isInitialized(expr->name)) {
            warn(WarningCode::W015_UninitializedVariable,
                 expr->loc,
                 "Variable '" + expr->name + "' may be used before initialization");
        }

        TypeRef narrowed = lookupVarType(expr->name);
        if (narrowed)
            return narrowed;
    }

    return sym->type;
}

/// @brief Analyze a 'self' expression.
/// @param expr The self expression node.
/// @return The type of 'self' in the current method context.
/// @details Emits error if used outside a method body.
TypeRef Sema::analyzeSelf(SelfExpr *expr) {
    if (!currentSelfType_) {
        error(expr->loc, "'self' can only be used inside a method");
        return types::unknown();
    }
    return currentSelfType_;
}

} // namespace il::frontends::zia
