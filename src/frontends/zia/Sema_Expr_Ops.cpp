//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file Sema_Expr_Ops.cpp
/// @brief Operator expression analysis (binary, unary, ternary) and common
///        type computation for the Zia semantic analyzer.
/// @details Validates assignment targets and mutability, applies short-circuit narrowing,
///          dispatches operator-family checks, records property setters and definite assignment,
///          and computes branch/literal common types.
///
//===----------------------------------------------------------------------===//

#include "frontends/zia/Sema.hpp"

namespace il::frontends::zia {

namespace {

/// @brief True if @p expr is a valid assignment LHS (identifier, field
///        access, or index expression).
/// @param expr Candidate left-hand expression.
/// @return True for identifier, field, and index nodes.
bool isAssignableTarget(const Expr *expr) {
    if (!expr)
        return false;

    switch (expr->kind) {
        case ExprKind::Ident:
        case ExprKind::Field:
        case ExprKind::Index:
            return true;
        default:
            return false;
    }
}

/// @brief True if @p field is a read-only built-in property of @p baseType
///        (e.g. List/Map/Set Length/Count) that may not be assigned to.
/// @param baseType Receiver type.
/// @param field Member spelling.
/// @return True for recognized collection/string count properties.
bool isReadOnlyBuiltinProperty(TypeRef baseType, const std::string &field) {
    if (!baseType)
        return false;
    if (baseType->kind == TypeKindSem::List || baseType->kind == TypeKindSem::Map ||
        baseType->kind == TypeKindSem::Set) {
        return field == "Length" || field == "length" || field == "Len" || field == "Count" ||
               field == "count" || field == "size";
    }
    if (baseType->kind == TypeKindSem::String)
        return field == "Length" || field == "length";
    return false;
}

/// @brief Produce a concise receiver name for built-in-property diagnostics.
/// @param type Receiver type.
/// @return Canonical collection/string name, semantic display spelling, or `value` for null.
std::string builtinTypeName(TypeRef type) {
    if (!type)
        return "value";
    switch (type->kind) {
        case TypeKindSem::List:
            return "List";
        case TypeKindSem::Map:
            return "Map";
        case TypeKindSem::Set:
            return "Set";
        case TypeKindSem::String:
            return "String";
        default:
            return type->toDisplayString();
    }
}

} // namespace

/// @brief Analyze a binary expression (e.g., a + b, x == y).
/// @param expr The binary expression node.
/// @return The result type of the operation.
/// @details Handles arithmetic, comparison, logical, bitwise, and assignment operators.
///          Performs type checking and widening for numeric operations.
TypeRef Sema::analyzeBinary(BinaryExpr *expr) {
    TypeRef leftType = nullptr;
    TypeRef rightType = nullptr;

    if (expr->op == BinaryOp::Assign) {
        if (!isAssignableTarget(expr->left.get())) {
            error(expr->left ? expr->left->loc : expr->loc,
                  "Assignment target must be a variable, field, or index expression");
            analyzeExpr(expr->right.get());
            return types::unknown();
        }

        rightType = analyzeExpr(expr->right.get());
        if (rightType && rightType->kind == TypeKindSem::Unit) {
            error(expr->right->loc,
                  "Unit literal cannot be assigned; use null for optional values or omit the "
                  "value in a void context");
            rightType = types::unknown();
        }

        if (auto *fieldExpr = dynamic_cast<FieldExpr *>(expr->left.get())) {
            TypeRef baseType = analyzeExpr(fieldExpr->base.get());
            if (baseType && baseType->kind == TypeKindSem::Optional && baseType->innerType()) {
                error(fieldExpr->loc,
                      "Cannot assign member '" + fieldExpr->field + "' on Optional type '" +
                          baseType->toDisplayString() +
                          "' without null check; use force unwrap before assignment");
                return types::unknown();
            }

            if (isReadOnlyBuiltinProperty(baseType, fieldExpr->field)) {
                error(expr->loc,
                      "Cannot assign to read-only property '" + fieldExpr->field + "' on " +
                          builtinTypeName(baseType));
                leftType = types::integer();
                exprTypes_[fieldExpr] = leftType;
            }

            /// @brief Records the assignment type of a write-only property target.
            /// @param targetType Setter parameter type.
            auto recordWriteOnlyTargetType = [&](TypeRef targetType) {
                leftType = targetType ? targetType : types::unknown();
                exprTypes_[fieldExpr] = leftType;
            };

            bool handledWriteOnlyProperty = false;
            if (!leftType && baseType &&
                (baseType->kind == TypeKindSem::Class || baseType->kind == TypeKindSem::Struct)) {
                if (const PropertyDecl *prop =
                        propertyDeclForLowering(baseType->name, fieldExpr->field, nullptr)) {
                    handledWriteOnlyProperty = !prop->getterBody && prop->setterBody;
                    if (handledWriteOnlyProperty) {
                        recordWriteOnlyTargetType(prop->type ? resolveTypeNode(prop->type.get())
                                                             : types::unknown());
                    }
                }
            }

            if (!leftType && !handledWriteOnlyProperty && baseType &&
                baseType->kind == TypeKindSem::Ptr && !baseType->name.empty()) {
                std::string setterName = baseType->name + ".set_" + fieldExpr->field;
                if (Symbol *setter = lookupSymbol(setterName);
                    setter && setter->kind == Symbol::Kind::Function && setter->type &&
                    setter->type->kind == TypeKindSem::Function) {
                    auto params = setter->type->paramTypes();
                    TypeRef setterValueType = nullptr;
                    if (params.size() >= 2 && params[1]) {
                        setterValueType = params[1];
                    } else if (params.size() == 1 && params[0]) {
                        setterValueType = params[0];
                    }
                    if (setterValueType) {
                        handledWriteOnlyProperty = true;
                        recordWriteOnlyTargetType(setterValueType);
                    }
                }
            }

            if (!leftType && !handledWriteOnlyProperty)
                leftType = analyzeExpr(expr->left.get());
        } else {
            leftType = analyzeExpr(expr->left.get());
        }
    } else {
        leftType = analyzeExpr(expr->left.get());
        if (expr->op == BinaryOp::And || expr->op == BinaryOp::Or) {
            std::string nullCheckVar;
            bool isNotNull = false;
            TypeRef checkedNullType = nullptr;
            bool appliedNarrowing = false;

            if (tryExtractNullCheck(expr->left.get(), nullCheckVar, isNotNull, &checkedNullType)) {
                TypeRef varType = checkedNullType ? checkedNullType : lookupVarType(nullCheckVar);
                if (varType && varType->kind == TypeKindSem::Optional && varType->innerType()) {
                    bool rhsSeesNonNull = (expr->op == BinaryOp::And) ? isNotNull : !isNotNull;
                    if (rhsSeesNonNull) {
                        pushNarrowingScope();
                        narrowType(nullCheckVar, varType->innerType());
                        appliedNarrowing = true;
                    }
                }
            }

            rightType = analyzeExpr(expr->right.get());
            if (appliedNarrowing)
                popNarrowingScope();
        } else {
            rightType = analyzeExpr(expr->right.get());
        }
    }

    switch (expr->op) {
        case BinaryOp::Add:
        case BinaryOp::Sub:
        case BinaryOp::Mul:
        case BinaryOp::Div:
        case BinaryOp::Mod:
            return checkArithmeticBinary(expr, leftType, rightType);

        case BinaryOp::Eq:
        case BinaryOp::Ne:
        case BinaryOp::Lt:
        case BinaryOp::Le:
        case BinaryOp::Gt:
        case BinaryOp::Ge:
            return checkComparisonBinary(expr, leftType, rightType);

        case BinaryOp::And:
        case BinaryOp::Or:
            return checkLogicalBinary(expr, leftType, rightType);

        case BinaryOp::Shl:
        case BinaryOp::Shr:
        case BinaryOp::BitAnd:
        case BinaryOp::BitOr:
        case BinaryOp::BitXor:
            return checkBitwiseBinary(expr, leftType, rightType);

        case BinaryOp::Assign:
            return recordBinaryAssignment(expr, leftType, rightType);
    }

    return types::unknown();
}

/// @brief Validate arithmetic or concatenation operands and compute their result type.
/// @param expr Binary expression supplying the operator and diagnostic locations.
/// @param leftType Analyzed left operand type.
/// @param rightType Analyzed right operand type.
/// @return String for concatenation, widened numeric type for arithmetic, or Unknown on error.
/// @details Literal zero divisors emit W010 before numeric result typing.
TypeRef Sema::checkArithmeticBinary(BinaryExpr *expr, TypeRef leftType, TypeRef rightType) {
    if (!leftType || !rightType || leftType->kind == TypeKindSem::Unknown ||
        rightType->kind == TypeKindSem::Unknown)
        return types::unknown();

    if (leftType->kind == TypeKindSem::String && expr->op == BinaryOp::Add) {
        if (rightType->kind == TypeKindSem::Any) {
            error(expr->right->loc,
                  "Cannot concatenate String with Any; convert the value to String explicitly");
        }
        return types::string();
    }
    if (rightType->kind == TypeKindSem::String && expr->op == BinaryOp::Add) {
        if (leftType->kind == TypeKindSem::Any) {
            error(expr->left->loc,
                  "Cannot concatenate Any with String; convert the value to String explicitly");
        }
        return types::string();
    }

    // W010: Division by zero — check for literal zero divisor.
    if ((expr->op == BinaryOp::Div || expr->op == BinaryOp::Mod) && leftType->isNumeric() &&
        rightType->isNumeric()) {
        if (expr->right->kind == ExprKind::IntLiteral) {
            auto *lit = static_cast<IntLiteralExpr *>(expr->right.get());
            if (lit->value == 0)
                warn(WarningCode::W010_DivisionByZero, expr->right->loc, "Division by zero");
        } else if (expr->right->kind == ExprKind::NumberLiteral) {
            auto *lit = static_cast<NumberLiteralExpr *>(expr->right.get());
            if (lit->value == 0.0)
                warn(WarningCode::W010_DivisionByZero, expr->right->loc, "Division by zero");
        }
    }

    if (leftType->isNumeric() && rightType->isNumeric()) {
        if (leftType->kind == TypeKindSem::Number || rightType->kind == TypeKindSem::Number)
            return types::number();
        return types::integer();
    }
    error(expr->loc, "Invalid operands for arithmetic operation");
    return types::unknown();
}

/// @brief Validate equality or relational operands.
/// @param expr Comparison expression.
/// @param leftType Analyzed left operand type.
/// @param rightType Analyzed right operand type.
/// @return Boolean in all cases so semantic analysis can continue after diagnostics.
/// @details Emits warning diagnostics for floating equality and redundant Boolean literals, and
///          accounts for declared optional surfaces when comparing against null.
TypeRef Sema::checkComparisonBinary(BinaryExpr *expr, TypeRef leftType, TypeRef rightType) {
    if (!leftType || !rightType)
        return types::boolean();

    // W005: Float equality — comparing floats with == or != is unreliable.
    if ((expr->op == BinaryOp::Eq || expr->op == BinaryOp::Ne) &&
        leftType->kind == TypeKindSem::Number && rightType->kind == TypeKindSem::Number) {
        warn(WarningCode::W005_FloatEquality,
             expr->loc,
             "Comparing floating-point values with " +
                 std::string(expr->op == BinaryOp::Eq ? "==" : "!=") +
                 " is unreliable; consider using an epsilon threshold");
    }

    // W011: Redundant bool comparison (e.g., `flag == true`, `b != false`).
    if ((expr->op == BinaryOp::Eq || expr->op == BinaryOp::Ne) &&
        (leftType->kind == TypeKindSem::Boolean || rightType->kind == TypeKindSem::Boolean)) {
        bool leftIsBoolLit = (expr->left->kind == ExprKind::BoolLiteral);
        bool rightIsBoolLit = (expr->right->kind == ExprKind::BoolLiteral);
        if (leftIsBoolLit || rightIsBoolLit) {
            warn(WarningCode::W011_RedundantBoolComparison,
                 expr->loc,
                 "Redundant comparison with Boolean literal; use the expression directly");
        }
    }

    if (leftType->kind != TypeKindSem::Unknown && rightType->kind != TypeKindSem::Unknown &&
        leftType->kind != TypeKindSem::Error && rightType->kind != TypeKindSem::Error) {
        const bool equality = expr->op == BinaryOp::Eq || expr->op == BinaryOp::Ne;
        if (equality) {
            TypeRef compareLeft = declaredOptionalSurfaceType(expr->left.get(), leftType);
            TypeRef compareRight = declaredOptionalSurfaceType(expr->right.get(), rightType);
            bool compatible = compareLeft->kind == TypeKindSem::Any ||
                              compareRight->kind == TypeKindSem::Any ||
                              compareLeft->isAssignableFrom(*compareRight) ||
                              compareRight->isAssignableFrom(*compareLeft);
            if (!compatible) {
                const TypeRef leftInner = compareLeft->innerType();
                const TypeRef rightInner = compareRight->innerType();
                const bool leftIsNull = compareLeft->kind == TypeKindSem::Optional && leftInner &&
                                        leftInner->kind == TypeKindSem::Unknown;
                const bool rightIsNull = compareRight->kind == TypeKindSem::Optional &&
                                         rightInner && rightInner->kind == TypeKindSem::Unknown;
                if (leftIsNull != rightIsNull) {
                    TypeRef nonNullType = leftIsNull ? compareRight : compareLeft;
                    error(expr->loc,
                          "Cannot compare non-nullable " + nonNullType->toDisplayString() +
                              " with null; use an Optional type if this value can be absent");
                } else {
                    error(expr->loc,
                          "Cannot compare " + compareLeft->toDisplayString() + " with " +
                              compareRight->toDisplayString());
                }
            }
        } else {
            bool compatible =
                (leftType->isNumeric() && rightType->isNumeric()) ||
                (leftType->kind == TypeKindSem::String && rightType->kind == TypeKindSem::String);
            if (!compatible) {
                error(expr->loc, "Relational comparison requires numeric operands or two Strings");
            }
        }
    }
    return types::boolean();
}

/// @brief Require Boolean operands for a logical binary operator.
/// @param expr Logical expression.
/// @param leftType Analyzed left operand type.
/// @param rightType Analyzed right operand type.
/// @return Boolean, including recovery paths for unknown operands.
TypeRef Sema::checkLogicalBinary(BinaryExpr *expr, TypeRef leftType, TypeRef rightType) {
    if (!leftType || !rightType)
        return types::boolean();
    if (leftType->kind == TypeKindSem::Unknown || rightType->kind == TypeKindSem::Unknown)
        return types::boolean();
    if (leftType->kind != TypeKindSem::Boolean || rightType->kind != TypeKindSem::Boolean)
        error(expr->loc, "Logical operators require Boolean operands");
    return types::boolean();
}

/// @brief Validate integral operands for shifts and bitwise operators.
/// @param expr Bitwise expression.
/// @param leftType Analyzed left operand type.
/// @param rightType Analyzed right operand type.
/// @return Byte for two-byte non-shift operations, Integer otherwise, or Unknown for unresolved
///         operands.
TypeRef Sema::checkBitwiseBinary(BinaryExpr *expr, TypeRef leftType, TypeRef rightType) {
    if (!leftType || !rightType)
        return types::unknown();
    if (leftType->kind == TypeKindSem::Unknown || rightType->kind == TypeKindSem::Unknown)
        return types::unknown();

    // W017/W018 are opt-in (`-Wall`) rather than default-on: ordinary integer bit
    // manipulation (flags, masks, hashing) is the common, correct use of `^`/`&`,
    // so warning on every occurrence by default is noise. They remain useful for
    // catching operator confusion when a project opts into the full warning set.
    if (expr->op == BinaryOp::BitXor) {
        warn(WarningCode::W017_XorConfusion,
             expr->loc,
             "'^' is bitwise XOR in Zia; use Math.Pow() for exponentiation");
    }
    if (expr->op == BinaryOp::BitAnd) {
        warn(WarningCode::W018_BitwiseAndConfusion,
             expr->loc,
             "'&' is bitwise AND in Zia; use '+' for string concatenation");
    }
    if (!leftType->isIntegral() || !rightType->isIntegral())
        error(expr->loc, "Bitwise operators require integral operands");
    if (leftType->kind == TypeKindSem::Byte && rightType->kind == TypeKindSem::Byte &&
        expr->op != BinaryOp::Shl && expr->op != BinaryOp::Shr)
        return types::byte();
    return types::integer();
}

/// @brief Validate an assignment and update semantic write metadata.
/// @param expr Assignment expression.
/// @param leftType Initially analyzed target type.
/// @param rightType Analyzed source type.
/// @return The target expression type.
/// @details Enforces final-field and property mutability, records generated/runtime setters,
///          validates indexed writes, updates definite initialization, and refreshes narrowing
///          after assigning a known non-null value.
TypeRef Sema::recordBinaryAssignment(BinaryExpr *expr, TypeRef leftType, TypeRef rightType) {
    // W009: Self-assignment (e.g., `x = x`).
    if (expr->left->kind == ExprKind::Ident && expr->right->kind == ExprKind::Ident) {
        auto *lhs = static_cast<IdentExpr *>(expr->left.get());
        auto *rhs = static_cast<IdentExpr *>(expr->right.get());
        if (lhs->name == rhs->name) {
            warn(WarningCode::W009_SelfAssignment,
                 expr->loc,
                 "Self-assignment of '" + lhs->name + "' has no effect");
        }
    }

    // Assignment - LHS must be assignable, types must be compatible.
    // Use the original (non-narrowed) declared type for the check, since
    // guard-clause narrowing may have refined the variable's effective type
    // (e.g., Page? narrowed to Page), but reassignment should still accept
    // the original type.
    {
        if (auto *fieldExpr = dynamic_cast<FieldExpr *>(expr->left.get())) {
            TypeRef baseType = typeOf(fieldExpr->base.get());
            if (baseType && baseType->kind == TypeKindSem::Optional && baseType->innerType())
                baseType = baseType->innerType();

            if (baseType &&
                (baseType->kind == TypeKindSem::Class || baseType->kind == TypeKindSem::Struct ||
                 baseType->kind == TypeKindSem::Module)) {
                std::string ownerName = baseType->name;
                if (baseType->kind != TypeKindSem::Module) {
                    if (auto fieldOwner = findFieldOwner(baseType->name, fieldExpr->field))
                        ownerName = *fieldOwner;
                }
                std::string memberKey = ownerName + "." + fieldExpr->field;
                bool assigningDuringInit = currentSelfType_ &&
                                           currentSelfType_->name == ownerName && currentMethod_ &&
                                           currentMethod_->name == "init";
                if (finalFields_.contains(memberKey) && !assigningDuringInit) {
                    error(expr->loc, "Cannot assign to final field '" + fieldExpr->field + "'");
                }
            }

            if (baseType &&
                (baseType->kind == TypeKindSem::Class || baseType->kind == TypeKindSem::Struct)) {
                std::string declaringOwner;
                if (const PropertyDecl *prop = propertyDeclForLowering(
                        baseType->name, fieldExpr->field, &declaringOwner)) {
                    if (!prop->setterBody) {
                        error(expr->loc,
                              "Property '" + fieldExpr->field + "' of type '" + declaringOwner +
                                  "' is read-only");
                    } else {
                        resolvedFieldSetters_[fieldExpr] = declaringOwner + ".set_" + prop->name;
                    }
                }
            }

            // Resolve runtime class property setters (e.g., ctrl.VelocityY = value).
            // Getters are resolved in Sema_Expr_Advanced; setters need the same
            // symbol-table lookup here on the assignment path.
            if (baseType && resolvedFieldSetters_.find(fieldExpr) == resolvedFieldSetters_.end()) {
                std::string setterName = baseType->name + ".set_" + fieldExpr->field;
                Symbol *setter = lookupSymbol(setterName);
                if (setter && setter->kind == Symbol::Kind::Function) {
                    resolvedFieldSetters_[fieldExpr] = setterName;
                    if (setter->type && setter->type->kind == TypeKindSem::Function) {
                        auto params = setter->type->paramTypes();
                        // Setter signature: (self, value) — struct type is params[1].
                        if (params.size() >= 2 && params[1]) {
                            exprTypes_[fieldExpr] = params[1];
                        } else if (params.size() == 1 && params[0]) {
                            // Static setter: (value) — no self.
                            exprTypes_[fieldExpr] = params[0];
                        }
                    }
                }
            }
        }

        TypeRef assignTarget = leftType;
        if (expr->left->kind == ExprKind::Ident) {
            auto *lhsIdent = static_cast<IdentExpr *>(expr->left.get());
            Symbol *sym = currentScope_->lookup(lhsIdent->name);
            bool assigningFinalFieldDuringInit = sym && sym->kind == Symbol::Kind::Field &&
                                                 currentSelfType_ && currentMethod_ &&
                                                 currentMethod_->name == "init";
            if (sym && sym->isFinal && !assigningFinalFieldDuringInit) {
                if (sym->kind == Symbol::Kind::Field) {
                    error(expr->loc, "Cannot assign to final field '" + lhsIdent->name + "'");
                } else {
                    error(expr->loc, "Cannot reassign final variable '" + lhsIdent->name + "'");
                }
            }
            if (sym && sym->type)
                assignTarget = sym->type;
        } else if (auto *fieldExpr = dynamic_cast<FieldExpr *>(expr->left.get())) {
            auto resolvedIt = exprTypes_.find(fieldExpr);
            if (resolvedIt != exprTypes_.end() && resolvedIt->second) {
                assignTarget = resolvedIt->second;
            }
        } else if (auto *indexExpr = dynamic_cast<IndexExpr *>(expr->left.get())) {
            TypeRef baseType = typeOf(indexExpr->base.get());
            if (baseType && baseType->kind == TypeKindSem::String) {
                error(expr->loc, "Cannot assign through a String index");
            } else if (baseType && baseType->kind != TypeKindSem::Unknown &&
                       baseType->kind != TypeKindSem::List && baseType->kind != TypeKindSem::Map &&
                       baseType->kind != TypeKindSem::FixedArray) {
                error(expr->loc, "Indexed assignment requires a List, Map, or fixed-size array");
            }
        }
        if (assignTarget && expr->right->kind == ExprKind::MapLiteral) {
            auto *mapLiteral = static_cast<MapLiteralExpr *>(expr->right.get());
            if (mapLiteral->entries.empty() && (assignTarget->kind == TypeKindSem::Set ||
                                                assignTarget->kind == TypeKindSem::Map)) {
                exprTypes_[expr->right.get()] = assignTarget;
                rightType = assignTarget;
            }
        }
        if (assignTarget && rightType && assignTarget->kind != TypeKindSem::Unknown &&
            rightType->kind != TypeKindSem::Unknown && assignTarget->kind != TypeKindSem::Error &&
            rightType->kind != TypeKindSem::Error && !assignTarget->isAssignableFrom(*rightType)) {
            errorTypeMismatch(expr->loc, assignTarget, rightType);
        }
    }

    // Track initialization for definite-assignment analysis. Also clear any
    // narrowing on the variable since the new value may have a different type.
    if (expr->left->kind == ExprKind::Ident) {
        auto *ident = static_cast<IdentExpr *>(expr->left.get());
        markInitialized(ident->name);
        if (!narrowedTypes_.empty()) {
            for (auto &scope : narrowedTypes_)
                scope.erase(ident->name);
        }

        // Re-establish narrowing when assigning a definite non-null value to
        // an Optional[T] variable.
        Symbol *sym = currentScope_->lookup(ident->name);
        if (sym && sym->type && sym->type->kind == TypeKindSem::Optional && rightType &&
            rightType->kind != TypeKindSem::Optional) {
            if (TypeRef inner = sym->type->innerType();
                inner && inner->isAssignableFrom(*rightType)) {
                narrowType(ident->name, inner);
            }
        }
    }
    return leftType;
}

/// @brief Analyze a unary expression (e.g., -x, !flag, ~bits).
/// @param expr The unary expression node.
/// @return The result type of the operation.
/// @details Handles negation, logical not, bitwise not, and address-of operators.
TypeRef Sema::analyzeUnary(UnaryExpr *expr) {
    TypeRef operandType = analyzeExpr(expr->operand.get());

    switch (expr->op) {
        case UnaryOp::Neg:
            if (!operandType->isNumeric()) {
                error(expr->loc, "Negation requires numeric operand");
            }
            return operandType;

        case UnaryOp::Not:
            if (operandType->kind != TypeKindSem::Boolean) {
                error(expr->loc, "Logical not requires Boolean operand");
            }
            return types::boolean();

        case UnaryOp::BitNot:
            if (!operandType->isIntegral()) {
                error(expr->loc, "Bitwise not requires integral operand");
            }
            return types::integer();

        case UnaryOp::AddressOf: {
            // Function-reference operator for managed callbacks: &funcName
            // The operand must be an identifier referring to a function
            auto *ident = dynamic_cast<IdentExpr *>(expr->operand.get());
            if (!ident) {
                error(expr->loc, "Function reference operator '&' requires a function name");
                return types::unknown();
            }

            Symbol *sym = lookupSymbol(ident->name);
            if (!sym) {
                error(expr->loc, "Unknown identifier '" + ident->name + "'");
                return types::unknown();
            }

            if (sym->kind != Symbol::Kind::Function && sym->kind != Symbol::Kind::Method) {
                error(expr->loc, "Function reference operator '&' requires a function name");
                return types::unknown();
            }

            // Return the function's type (which is already a function type)
            // This allows assignment to function-typed variables
            return sym->type;
        }
    }

    return types::unknown();
}

/// @brief Analyze a ternary conditional expression (cond ? then : else).
/// @param expr The ternary expression node.
/// @return The common type of the then and else branches.
/// @details Validates condition is Boolean and finds common type of branches.
TypeRef Sema::analyzeTernary(TernaryExpr *expr) {
    TypeRef condType = analyzeExpr(expr->condition.get());
    if (condType && condType->kind != TypeKindSem::Unknown &&
        condType->kind != TypeKindSem::Boolean) {
        error(expr->condition->loc, "Condition must be Boolean");
    }

    TypeRef thenType = analyzeExpr(expr->thenExpr.get());
    TypeRef elseType = analyzeExpr(expr->elseExpr.get());

    TypeRef resultType = commonType(thenType, elseType);
    if (resultType && resultType->kind != TypeKindSem::Unknown)
        return resultType;

    error(expr->loc, "Incompatible types in ternary expression");
    return types::unknown();
}

/// @brief Analyze an if-expression (`if cond { thenExpr } else { elseExpr }`).
/// @param expr The if-expression AST node.
/// @return The common type of the then and else branches.
TypeRef Sema::analyzeIfExpr(IfExpr *expr) {
    TypeRef condType = analyzeExpr(expr->condition.get());
    if (condType && condType->kind != TypeKindSem::Boolean &&
        condType->kind != TypeKindSem::Unknown) {
        error(expr->condition->loc, "Condition must be Boolean");
    }

    TypeRef thenType = analyzeExpr(expr->thenBranch.get());
    TypeRef elseType = analyzeExpr(expr->elseBranch.get());

    TypeRef resultType = commonType(thenType, elseType);
    if (resultType && resultType->kind != TypeKindSem::Unknown)
        return resultType;

    // If one branch is unknown, return the other — avoids spurious errors on null branches
    if (thenType && thenType->kind != TypeKindSem::Unknown)
        return thenType;
    if (elseType && elseType->kind != TypeKindSem::Unknown)
        return elseType;

    error(expr->loc, "Incompatible types in if-expression");
    return types::unknown();
}

/// @brief Compute the common type of two types for type unification.
/// @param lhs The first type.
/// @param rhs The second type.
/// @return The most general type compatible with both, or Unknown if incompatible.
/// @details Handles numeric widening, optional lifting, and subtype relationships.
TypeRef Sema::commonType(TypeRef lhs, TypeRef rhs) {
    if (!lhs && !rhs)
        return types::unknown();
    if (!lhs)
        return rhs;
    if (!rhs)
        return lhs;
    if (lhs->kind == TypeKindSem::Unknown)
        return rhs;
    if (rhs->kind == TypeKindSem::Unknown)
        return lhs;

    if (lhs->kind == TypeKindSem::Optional || rhs->kind == TypeKindSem::Optional) {
        TypeRef innerL = lhs->kind == TypeKindSem::Optional ? lhs->innerType() : lhs;
        TypeRef innerR = rhs->kind == TypeKindSem::Optional ? rhs->innerType() : rhs;
        TypeRef inner = commonType(innerL, innerR);
        return types::optional(inner ? inner : types::unknown());
    }

    if (lhs->isNumeric() && rhs->isNumeric()) {
        if (lhs->kind == TypeKindSem::Number || rhs->kind == TypeKindSem::Number)
            return types::number();
        if (lhs->kind == TypeKindSem::Integer || rhs->kind == TypeKindSem::Integer)
            return types::integer();
        return types::byte();
    }

    if (lhs->isAssignableFrom(*rhs))
        return lhs;
    if (rhs->isAssignableFrom(*lhs))
        return rhs;

    return types::unknown();
}

} // namespace il::frontends::zia
