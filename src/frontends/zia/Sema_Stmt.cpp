//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file Sema_Stmt.cpp
/// @brief Statement analysis for the Zia semantic analyzer.
/// @details Dispatches statement kinds, manages lexical and loop/catch context, validates local
///          declarations and control flow, propagates optional narrowing, tracks definite
///          initialization, and checks pattern-match coverage.
///
//===----------------------------------------------------------------------===//

#include "frontends/zia/Sema.hpp"

#include <string_view>

namespace il::frontends::zia {

namespace {

/// @brief True if @p type may not appear as a statement-level value
///        (Void, Never, or Module).
/// @param type Semantic type to classify.
/// @return True when the type cannot be stored in a local binding.
bool isForbiddenValueType(TypeRef type) {
    return type && (type->kind == TypeKindSem::Void || type->kind == TypeKindSem::Never ||
                    type->kind == TypeKindSem::Module);
}

/// @brief Display name for a forbidden value type, for the diagnostic message.
/// @param type Semantic type, which may be null.
/// @return Semantic spelling, or `unknown` for a null type.
std::string forbiddenValueTypeName(TypeRef type) {
    return type ? type->toString() : "unknown";
}

/// @brief True if @p type is a typed runtime sequence/collection pointer
///        (Seq/Queue/Stack/…) that supports element-typed for-iteration.
/// @param type Candidate iterable type.
/// @return True for a supported named runtime container with element metadata.
bool isTypedRuntimeSequence(TypeRef type) {
    return type && type->kind == TypeKindSem::Ptr && type->elementType() &&
           (type->name == "Zanna.Collections.Seq" || type->name == "Zanna.Collections.Queue" ||
            type->name == "Zanna.Collections.Stack" || type->name == "Zanna.Collections.Deque" ||
            type->name == "Zanna.Collections.List" || type->name == "Zanna.Collections.Ring" ||
            type->name == "Zanna.Collections.Heap");
}

/// @brief Return true when @p typeName is a runtime trap/catch type name accepted by Zia.
/// @details The names mirror the user-facing runtime error names exposed by `rt_error.h`;
///          `Error` is the language-level catch-all alias.
/// @param typeName Catch-clause type spelling.
/// @return True when the spelling is a supported runtime error kind or catch-all.
bool isKnownCatchTypeName(const std::string &typeName) {
    static constexpr std::string_view kKnownCatchTypes[] = {
        "DivideByZero",
        "Overflow",
        "InvalidCast",
        "DomainError",
        "Bounds",
        "FileNotFound",
        "EOF",
        "IOError",
        "InvalidOperation",
        "RuntimeError",
        "Interrupt",
        "NetworkError",
        "Error",
    };
    for (std::string_view name : kKnownCatchTypes) {
        if (typeName == name)
            return true;
    }
    return false;
}

} // namespace

//=============================================================================
// Statement Analysis
//=============================================================================

/// @brief Analyze a statement and dispatch to its kind-specific checker.
/// @param stmt Statement node; null is ignored for recovery.
/// @details Handles expression-result warnings, loop-control legality, deferred statements,
///          catch scopes and ordering, and throw/rethrow context in addition to delegated kinds.
void Sema::analyzeStmt(Stmt *stmt) {
    if (!stmt)
        return;

    switch (stmt->kind) {
        case StmtKind::Block:
            analyzeBlockStmt(static_cast<BlockStmt *>(stmt));
            break;
        case StmtKind::Expr: {
            auto *exprStmt = static_cast<ExprStmt *>(stmt);
            TypeRef resultType = analyzeExpr(exprStmt->expr.get());

            // W014: Unused result — call returning non-void value discarded
            if (exprStmt->expr->kind == ExprKind::Call && resultType &&
                resultType->kind != TypeKindSem::Void && resultType->kind != TypeKindSem::Unknown) {
                warn(WarningCode::W014_UnusedResult,
                     exprStmt->loc,
                     "Result of function call is unused");
            }
            break;
        }
        case StmtKind::Var:
            analyzeVarStmt(static_cast<VarStmt *>(stmt));
            break;
        case StmtKind::If:
            analyzeIfStmt(static_cast<IfStmt *>(stmt));
            break;
        case StmtKind::While:
            analyzeWhileStmt(static_cast<WhileStmt *>(stmt));
            break;
        case StmtKind::For:
            analyzeForStmt(static_cast<ForStmt *>(stmt));
            break;
        case StmtKind::ForIn:
            analyzeForInStmt(static_cast<ForInStmt *>(stmt));
            break;
        case StmtKind::Return:
            analyzeReturnStmt(static_cast<ReturnStmt *>(stmt));
            break;
        case StmtKind::Break:
        case StmtKind::Continue:
            if (loopDepth_ == 0) {
                error(stmt->loc,
                      stmt->kind == StmtKind::Break ? "break used outside of loop"
                                                    : "continue used outside of loop");
            }
            break;
        case StmtKind::Defer: {
            auto *deferStmt = static_cast<DeferStmt *>(stmt);
            analyzeStmt(deferStmt->action.get());
            break;
        }
        case StmtKind::Guard:
            analyzeGuardStmt(static_cast<GuardStmt *>(stmt));
            break;
        case StmtKind::Match:
            analyzeMatchStmt(static_cast<MatchStmt *>(stmt));
            break;
        case StmtKind::Try: {
            auto *tryStmt = static_cast<TryStmt *>(stmt);
            if (tryStmt->tryBody)
                analyzeStmt(tryStmt->tryBody.get());

            /// @brief Validates one catch-clause error type name.
            /// @param typeName Declared catch type, or empty for catch-all.
            auto validateCatchType = [&](const std::string &typeName) {
                if (typeName.empty())
                    return;
                if (!isKnownCatchTypeName(typeName)) {
                    error(tryStmt->loc, "unknown error type '" + typeName + "' in catch clause");
                }
            };

            for (size_t i = 0; i < tryStmt->catches.size(); ++i) {
                auto &catchClause = tryStmt->catches[i];
                validateCatchType(catchClause.typeName);
                if ((catchClause.typeName.empty() || catchClause.typeName == "Error") &&
                    i + 1 < tryStmt->catches.size()) {
                    error(catchClause.loc,
                          "catch-all clause must be the last catch clause in a try statement");
                }

                pushScope(catchClause.loc.isValid() ? catchClause.loc : tryStmt->loc);
                if (!catchClause.var.empty()) {
                    Symbol sym;
                    sym.kind = Symbol::Kind::Variable;
                    sym.name = catchClause.var;
                    sym.type = types::error();
                    sym.isFinal = true;
                    defineSymbol(catchClause.var, sym, catchClause.loc);
                    markInitialized(catchClause.var);
                }
                catchDepth_++;
                analyzeStmt(catchClause.body.get());
                catchDepth_--;
                popScope(catchClause.body ? scopeEndForStmt(catchClause.body.get())
                                          : catchClause.loc);
            }
            if (tryStmt->finallyBody)
                analyzeStmt(tryStmt->finallyBody.get());
            break;
        }
        case StmtKind::Throw: {
            auto *throwStmt = static_cast<ThrowStmt *>(stmt);
            if (throwStmt->value) {
                analyzeExpr(throwStmt->value.get());
            } else if (catchDepth_ == 0) {
                error(stmt->loc, "bare throw may only be used inside a catch clause");
            }
            break;
        }
    }
}

/// @brief Check if a statement unconditionally terminates (return/break/continue/throw).
/// @param s Statement tree to inspect.
/// @return True when every reachable path through the recognized construct terminates.
static bool stmtTerminates(const Stmt *s) {
    if (!s)
        return false;
    if (s->kind == StmtKind::Return || s->kind == StmtKind::Break ||
        s->kind == StmtKind::Continue || s->kind == StmtKind::Throw)
        return true;
    if (s->kind == StmtKind::Block) {
        auto *block = static_cast<const BlockStmt *>(s);
        if (!block->statements.empty())
            return stmtTerminates(block->statements.back().get());
    }
    if (s->kind == StmtKind::If) {
        auto *ifStmt = static_cast<const IfStmt *>(s);
        return ifStmt->elseBranch && stmtTerminates(ifStmt->thenBranch.get()) &&
               stmtTerminates(ifStmt->elseBranch.get());
    }
    if (s->kind == StmtKind::Try) {
        auto *tryStmt = static_cast<const TryStmt *>(s);
        if (tryStmt->finallyBody && stmtTerminates(tryStmt->finallyBody.get()))
            return true;
        bool tryExits = stmtTerminates(tryStmt->tryBody.get());
        if (!tryStmt->catches.empty()) {
            for (const auto &catchClause : tryStmt->catches) {
                if (!stmtTerminates(catchClause.body.get()))
                    return false;
            }
            return tryExits;
        }
        return tryExits;
    }
    return false;
}

/// @brief Analyze a lexical statement block.
/// @param stmt Block statement.
/// @details Creates a scope, reports unreachable statements, and persists non-null narrowing
///          implied by terminating `if` or `guard` clauses across later statements in the block.
void Sema::analyzeBlockStmt(BlockStmt *stmt) {
    pushScope(stmt->loc);
    bool afterTerminator = false;
    bool warnedUnreachable = false;
    int guardNarrowings = 0;
    /// @brief Persists non-null narrowing implied by a terminating statement.
    /// @param condition Condition whose null comparison is inspected.
    /// @param conditionHoldsAfterStmt Whether the condition is true on fallthrough.
    auto persistOptionalNullCheckNarrowing = [&](Expr *condition, bool conditionHoldsAfterStmt) {
        std::string nullCheckVar;
        bool isNotNull = false;
        TypeRef checkedType = nullptr;
        if (!tryExtractNullCheck(condition, nullCheckVar, isNotNull, &checkedType))
            return;

        TypeRef varType = checkedType ? checkedType : lookupVarType(nullCheckVar);
        if (!varType || varType->kind != TypeKindSem::Optional)
            return;

        // If the condition holds after the statement, `x != null` narrows to T.
        // If the condition does not hold after the statement, `x == null` having
        // exited implies `x != null` on the fallthrough path.
        bool isNonNullAfterStmt = conditionHoldsAfterStmt ? isNotNull : !isNotNull;
        if (!isNonNullAfterStmt || !varType->innerType())
            return;

        pushNarrowingScope();
        narrowType(nullCheckVar, varType->innerType());
        guardNarrowings++;
    };

    for (auto &s : stmt->statements) {
        // W002: Unreachable code after return/break/continue
        if (afterTerminator) {
            if (!warnedUnreachable) {
                warn(WarningCode::W002_UnreachableCode,
                     s->loc,
                     "Unreachable code after return/break/continue");
                warnedUnreachable = true;
            }
        }

        analyzeStmt(s.get());

        if (s->kind == StmtKind::Return || s->kind == StmtKind::Break ||
            s->kind == StmtKind::Continue || s->kind == StmtKind::Throw) {
            afterTerminator = true;
        }

        // Guard-clause narrowing: if (x == null) return; → x is non-null after
        if (s->kind == StmtKind::If) {
            auto *ifStmt = static_cast<IfStmt *>(s.get());
            if (!ifStmt->elseBranch && stmtTerminates(ifStmt->thenBranch.get())) {
                persistOptionalNullCheckNarrowing(ifStmt->condition.get(),
                                                  /*conditionHoldsAfterStmt=*/false);
            }
        } else if (s->kind == StmtKind::Guard) {
            auto *guardStmt = static_cast<GuardStmt *>(s.get());
            if (stmtTerminates(guardStmt->elseBlock.get())) {
                persistOptionalNullCheckNarrowing(guardStmt->condition.get(),
                                                  /*conditionHoldsAfterStmt=*/true);
            }
        }
    }
    for (int i = 0; i < guardNarrowings; i++)
        popNarrowingScope();
    popScope(scopeEndForStmt(stmt));
}

/// @brief Analyze a local variable or tuple-destructuring declaration.
/// @param stmt Variable statement.
/// @details Resolves annotations and initializers, supplies target types to lambdas and empty
///          collections, validates Byte literals and shadowing, defines bindings, and updates
///          definite initialization and optional narrowing.
void Sema::analyzeVarStmt(VarStmt *stmt) {
    if (stmt->isTupleDestructure) {
        if (!stmt->initializer) {
            error(stmt->loc, "Tuple destructuring requires an initializer");
            return;
        }

        std::vector<std::string> names = stmt->tupleNames;
        if (names.empty()) {
            names.push_back(stmt->name);
            if (!stmt->secondName.empty())
                names.push_back(stmt->secondName);
        }

        TypeRef initType = analyzeExpr(stmt->initializer.get());
        if (!initType || initType->kind != TypeKindSem::Tuple) {
            error(stmt->initializer->loc, "Tuple destructuring requires a tuple initializer");
            return;
        }

        const auto &elements = initType->tupleElementTypes();
        if (elements.size() != names.size()) {
            error(stmt->initializer->loc, "Tuple destructuring arity mismatch");
            return;
        }

        std::vector<TypeRef> bindingTypes;
        bindingTypes.reserve(elements.size());
        for (size_t i = 0; i < elements.size(); ++i) {
            TypeNode *annotation = nullptr;
            if (!stmt->tupleTypes.empty() && i < stmt->tupleTypes.size()) {
                annotation = stmt->tupleTypes[i].get();
            } else if (i == 0) {
                annotation = stmt->type.get();
            } else if (i == 1) {
                annotation = stmt->secondType.get();
            }

            TypeRef bindingType = annotation ? resolveTypeNode(annotation) : elements[i];
            if (bindingType && elements[i] && !bindingType->isAssignableFrom(*elements[i]))
                errorTypeMismatch(stmt->loc, bindingType, elements[i]);
            bindingTypes.push_back(bindingType ? bindingType : types::unknown());
        }

        /// @brief Defines one destructured tuple loop binding.
        /// @param name Binding identifier.
        /// @param type Inferred or annotated binding type.
        auto defineTupleBinding = [&](const std::string &name, TypeRef type) {
            if (currentScope_ && currentScope_->parent()) {
                Symbol *existing = currentScope_->parent()->lookup(name);
                if (existing && (existing->kind == Symbol::Kind::Variable ||
                                 existing->kind == Symbol::Kind::Parameter)) {
                    warn(WarningCode::W004_VariableShadowing,
                         stmt->loc,
                         "Variable '" + name + "' shadows a variable in an outer scope");
                }
            }

            Symbol sym;
            sym.kind = Symbol::Kind::Variable;
            sym.name = name;
            sym.type = type ? type : types::unknown();
            sym.isFinal = stmt->isFinal;
            defineSymbol(name, sym, stmt->loc);
            markInitialized(name);
        };

        for (size_t i = 0; i < names.size(); ++i)
            defineTupleBinding(names[i], bindingTypes[i]);
        return;
    }

    if (stmt->isFinal && !stmt->initializer) {
        error(stmt->loc, "'final' declarations require an initializer");
        return;
    }

    TypeRef declaredType = stmt->type ? resolveTypeNode(stmt->type.get()) : nullptr;
    if (isForbiddenValueType(declaredType)) {
        error(stmt->loc,
              "Type '" + forbiddenValueTypeName(declaredType) +
                  "' cannot be used for a local value");
        declaredType = types::unknown();
    }
    // Give a lambda initializer the declared function type so it can infer
    // omitted parameter types (target-typed lambdas). Cleared afterward so it
    // never leaks to a non-lambda initializer.
    if (stmt->initializer && declaredType && declaredType->kind == TypeKindSem::Function)
        lambdaTypeHint_ = declaredType;
    TypeRef initType = stmt->initializer ? analyzeExpr(stmt->initializer.get()) : nullptr;
    lambdaTypeHint_ = nullptr;
    if (declaredType && declaredType->kind == TypeKindSem::Set && stmt->initializer &&
        stmt->initializer->kind == ExprKind::MapLiteral) {
        auto *mapLiteral = static_cast<MapLiteralExpr *>(stmt->initializer.get());
        if (mapLiteral->entries.empty()) {
            exprTypes_[stmt->initializer.get()] = declaredType;
            initType = declaredType;
        }
    }
    if (initType && initType->kind == TypeKindSem::Unit) {
        error(stmt->initializer->loc,
              "Unit literal cannot be stored; use null for optional values or omit the value in a "
              "void context");
        initType = types::unknown();
    }

    TypeRef varType;
    if (declaredType && initType) {
        // BUG-VL-001: Allow integer literals in Byte range (0-255) to be assigned to Byte
        if (declaredType->kind == TypeKindSem::Byte && initType->kind == TypeKindSem::Integer) {
            if (stmt->initializer->kind == ExprKind::IntLiteral) {
                auto *lit = static_cast<IntLiteralExpr *>(stmt->initializer.get());
                if (lit->value >= 0 && lit->value <= 255) {
                    initType = types::byte(); // Treat as Byte literal
                }
            }
        }

        // W003: Implicit narrowing (Number assigned to Integer variable)
        if (declaredType->kind == TypeKindSem::Integer && initType->kind == TypeKindSem::Number) {
            warn(WarningCode::W003_ImplicitNarrowing,
                 stmt->loc,
                 "Implicit narrowing from Number to Integer in initialization of '" + stmt->name +
                     "'");
        }

        // Both declared and inferred - check compatibility
        if (!declaredType->isAssignableFrom(*initType)) {
            errorTypeMismatch(stmt->loc, declaredType, initType);
        }
        varType = declaredType;
    } else if (declaredType) {
        varType = declaredType;
    } else if (initType) {
        bool inferredFromNullLiteral =
            stmt->initializer && stmt->initializer->kind == ExprKind::NullLiteral && initType &&
            initType->kind == TypeKindSem::Optional && initType->innerType() &&
            initType->innerType()->kind == TypeKindSem::Unknown;
        if (inferredFromNullLiteral) {
            error(stmt->loc,
                  "Cannot infer type from null initializer; add an explicit type annotation "
                  "such as 'String?', 'MyType', or 'GUI.Font'");
            varType = types::unknown();
        } else {
            varType = initType;
        }
    } else {
        error(stmt->loc, "Cannot infer type without initializer");
        varType = types::unknown();
    }

    // W004: Variable shadowing — check if name shadows a variable in parent scope
    if (currentScope_ && currentScope_->parent()) {
        Symbol *existing = currentScope_->parent()->lookup(stmt->name);
        if (existing && (existing->kind == Symbol::Kind::Variable ||
                         existing->kind == Symbol::Kind::Parameter)) {
            warn(WarningCode::W004_VariableShadowing,
                 stmt->loc,
                 "Variable '" + stmt->name + "' shadows a variable in an outer scope");
        }
    }

    Symbol sym;
    sym.kind = Symbol::Kind::Variable;
    sym.name = stmt->name;
    sym.type = varType;
    sym.isFinal = stmt->isFinal;
    defineSymbol(stmt->name, sym, stmt->loc);

    // Fixed arrays are zero-initialized aggregate storage. Scalar declarations
    // without explicit initializers keep the existing W015 definite-init warning.
    bool defaultInitializedAggregate =
        declaredType && declaredType->kind == TypeKindSem::FixedArray && !stmt->initializer;
    if (stmt->initializer || defaultInitializedAggregate) {
        markInitialized(stmt->name);
    }

    if (stmt->initializer) {
        // A non-null initializer narrows Optional[T] to T until a later assignment
        // clears or replaces that flow fact.
        if (varType && varType->kind == TypeKindSem::Optional && initType &&
            initType->kind != TypeKindSem::Optional) {
            if (TypeRef inner = varType->innerType(); inner && inner->isAssignableFrom(*initType)) {
                narrowType(stmt->name, inner);
            }
        }
    }
}

/// @brief Analyze an if statement with branch-sensitive narrowing and initialization state.
/// @param stmt If statement.
/// @details Validates the Boolean condition, emits condition/body warnings, narrows null-checked
///          values in the applicable branch, and intersects definite initialization across a
///          complete if/else.
void Sema::analyzeIfStmt(IfStmt *stmt) {
    // W007: Assignment in condition (e.g., `if (x = 5)`)
    if (stmt->condition->kind == ExprKind::Binary) {
        auto *binary = static_cast<BinaryExpr *>(stmt->condition.get());
        if (binary->op == BinaryOp::Assign) {
            warn(WarningCode::W007_AssignmentInCondition,
                 stmt->condition->loc,
                 "Assignment in condition; did you mean '=='?");
        }
    }

    TypeRef condType = analyzeExpr(stmt->condition.get());
    if (condType && condType->kind != TypeKindSem::Unknown &&
        condType->kind != TypeKindSem::Boolean) {
        error(stmt->condition->loc, "Condition must be Boolean");
    }

    // W013: Empty if body
    if (stmt->thenBranch && stmt->thenBranch->kind == StmtKind::Block) {
        auto *block = static_cast<BlockStmt *>(stmt->thenBranch.get());
        if (block->statements.empty()) {
            warn(WarningCode::W013_EmptyBody,
                 stmt->loc,
                 "Empty if-body; consider removing or adding a comment");
        }
    }

    // W013: Empty else body
    if (stmt->elseBranch && stmt->elseBranch->kind == StmtKind::Block) {
        auto *block = static_cast<BlockStmt *>(stmt->elseBranch.get());
        if (block->statements.empty()) {
            warn(WarningCode::W013_EmptyBody,
                 stmt->elseBranch->loc,
                 "Empty else-body; consider removing or adding a comment");
        }
    }

    // Check for null check pattern for type narrowing
    std::string nullCheckVar;
    bool isNotNull = false;
    TypeRef checkedNullType = nullptr;
    bool hasNullCheck =
        tryExtractNullCheck(stmt->condition.get(), nullCheckVar, isNotNull, &checkedNullType);

    TypeRef narrowedType = nullptr;
    if (hasNullCheck) {
        // Look up the variable's current type
        TypeRef varType = checkedNullType ? checkedNullType : lookupVarType(nullCheckVar);
        if (varType && varType->kind == TypeKindSem::Optional) {
            // Get the inner (non-optional) type
            narrowedType = varType->innerType();
        }
    }

    // Save initialization state before branches for definite-assignment analysis
    auto preIfState = saveInitState();

    // Analyze then-branch with narrowing if condition is "x != null"
    if (hasNullCheck && isNotNull && narrowedType) {
        pushNarrowingScope();
        narrowType(nullCheckVar, narrowedType);
        analyzeStmt(stmt->thenBranch.get());
        popNarrowingScope();
    } else {
        analyzeStmt(stmt->thenBranch.get());
    }

    auto thenState = saveInitState();

    // Analyze else-branch with narrowing if condition is "x == null"
    if (stmt->elseBranch) {
        // Restore pre-if state before analyzing else-branch
        initializedVars_ = preIfState;

        if (hasNullCheck && !isNotNull && narrowedType) {
            // In else branch of "x == null", x is not null
            pushNarrowingScope();
            narrowType(nullCheckVar, narrowedType);
            analyzeStmt(stmt->elseBranch.get());
            popNarrowingScope();
        } else {
            analyzeStmt(stmt->elseBranch.get());
        }

        auto elseState = saveInitState();

        // After if/else: only keep variables initialized in BOTH branches
        intersectInitState(thenState, elseState);
    } else {
        // No else branch — conservatively restore pre-if state
        // (the then-branch may not execute)
        initializedVars_ = preIfState;
    }
}

/// @brief Analyze a while loop.
/// @param stmt While statement.
/// @details Checks assignment-in-condition and empty-body warnings, requires a Boolean condition,
///          and establishes loop context for break/continue validation.
void Sema::analyzeWhileStmt(WhileStmt *stmt) {
    // W007: Assignment in condition
    if (stmt->condition->kind == ExprKind::Binary) {
        auto *binary = static_cast<BinaryExpr *>(stmt->condition.get());
        if (binary->op == BinaryOp::Assign) {
            warn(WarningCode::W007_AssignmentInCondition,
                 stmt->condition->loc,
                 "Assignment in while-condition; did you mean '=='?");
        }
    }

    TypeRef condType = analyzeExpr(stmt->condition.get());
    if (condType && condType->kind != TypeKindSem::Unknown &&
        condType->kind != TypeKindSem::Boolean) {
        error(stmt->condition->loc, "Condition must be Boolean");
    }

    // W006: Empty loop body
    if (stmt->body && stmt->body->kind == StmtKind::Block) {
        auto *block = static_cast<BlockStmt *>(stmt->body.get());
        if (block->statements.empty()) {
            warn(WarningCode::W006_EmptyLoopBody, stmt->loc, "Empty while-loop body");
        }
    }

    loopDepth_++;
    analyzeStmt(stmt->body.get());
    loopDepth_--;
}

/// @brief Analyze a C-style for loop in its own lexical scope.
/// @param stmt For statement.
/// @details Processes initializer, optional Boolean condition, body, and update in execution order
///          while establishing loop context and checking empty-body warnings.
void Sema::analyzeForStmt(ForStmt *stmt) {
    pushScope(stmt->loc);
    if (stmt->init)
        analyzeStmt(stmt->init.get());
    if (stmt->condition) {
        TypeRef condType = analyzeExpr(stmt->condition.get());
        if (condType && condType->kind != TypeKindSem::Unknown &&
            condType->kind != TypeKindSem::Boolean) {
            error(stmt->condition->loc, "Condition must be Boolean");
        }
    }

    // W006: Empty loop body
    if (stmt->body && stmt->body->kind == StmtKind::Block) {
        auto *block = static_cast<BlockStmt *>(stmt->body.get());
        if (block->statements.empty()) {
            warn(WarningCode::W006_EmptyLoopBody, stmt->loc, "Empty for-loop body");
        }
    }

    loopDepth_++;
    analyzeStmt(stmt->body.get());
    if (stmt->update)
        analyzeExpr(stmt->update.get());
    loopDepth_--;
    popScope(stmt->body ? scopeEndForStmt(stmt->body.get()) : stmt->loc);
}

/// @brief Analyze a for-in loop and infer its iteration binding types.
/// @param stmt For-in statement.
/// @details Supports language collections, strings, ranges, typed runtime sequences, map key/value
///          pairs, index/element tuple binding, and explicitly annotated loop variables.
void Sema::analyzeForInStmt(ForInStmt *stmt) {
    pushScope(stmt->loc);

    TypeRef iterableType = analyzeExpr(stmt->iterable.get());

    // Determine element types from iterable
    TypeRef elementType = types::unknown();
    TypeRef secondType = types::unknown();

    if (!iterableType || iterableType->kind == TypeKindSem::Unknown) {
        elementType = types::unknown();
    } else if (iterableType->kind == TypeKindSem::List || iterableType->kind == TypeKindSem::Set) {
        elementType = iterableType->elementType();
    } else if (iterableType->kind == TypeKindSem::String) {
        // Iterating a String yields one-character Strings, mirroring `str[i]`.
        elementType = types::string();
    } else if (iterableType->kind == TypeKindSem::Map) {
        elementType = iterableType->keyType() ? iterableType->keyType() : types::string();
        secondType = iterableType->valueType();
    } else if (stmt->iterable && stmt->iterable->kind == ExprKind::Range) {
        elementType = types::integer();
    } else if (iterableType->kind == TypeKindSem::Ptr &&
               iterableType->name == "Zanna.Collections.Seq" && !iterableType->typeArgs.empty()) {
        // Typed seq (e.g. from Str.Split, Dir.FilesSeq) — element type is typeArgs[0]
        elementType = iterableType->typeArgs[0];
    } else if (isTypedRuntimeSequence(iterableType)) {
        elementType = iterableType->elementType();
    } else {
        error(stmt->iterable->loc, "Expression is not iterable");
    }

    if (stmt->isTuple) {
        if (iterableType && iterableType->kind == TypeKindSem::Map) {
            // Map iteration binds (key, value)
        } else if (iterableType && (iterableType->kind == TypeKindSem::List ||
                                    iterableType->kind == TypeKindSem::Set ||
                                    iterableType->kind == TypeKindSem::String ||
                                    isTypedRuntimeSequence(iterableType))) {
            // List/Set/String iteration with tuple binding: (index, element)
            secondType = elementType;       // Element goes to second variable
            elementType = types::integer(); // Index goes to first variable
        } else if (iterableType && iterableType->kind == TypeKindSem::Tuple) {
            const auto &elements = iterableType->tupleElementTypes();
            if (elements.size() == 2) {
                elementType = elements[0];
                secondType = elements[1];
            } else {
                error(stmt->loc,
                      "Tuple binding requires an iterable pair or a 2-element Tuple, got " +
                          std::to_string(elements.size()) + " elements");
            }
        } else {
            error(stmt->loc, "Tuple binding requires Map, List, Set, or Tuple elements");
        }
    }

    if (stmt->variableType) {
        TypeRef explicitType = resolveTypeNode(stmt->variableType.get());
        if (explicitType && elementType && elementType->kind != TypeKindSem::Unknown &&
            !explicitType->isAssignableFrom(*elementType)) {
            error(stmt->loc, "Loop variable type does not match iterable element type");
        }
        elementType = explicitType ? explicitType : types::unknown();
    }

    if (stmt->isTuple && stmt->secondVariableType) {
        TypeRef explicitType = resolveTypeNode(stmt->secondVariableType.get());
        if (explicitType && secondType && secondType->kind != TypeKindSem::Unknown &&
            !explicitType->isAssignableFrom(*secondType)) {
            error(stmt->loc, "Loop variable type does not match iterable element type");
        }
        secondType = explicitType ? explicitType : types::unknown();
    }

    Symbol sym;
    sym.kind = Symbol::Kind::Variable;
    sym.name = stmt->variable;
    sym.type = elementType ? elementType : types::unknown();
    sym.isFinal = true;
    defineSymbol(stmt->variable, sym, stmt->loc);
    markInitialized(stmt->variable);

    if (stmt->isTuple) {
        Symbol secondSym;
        secondSym.kind = Symbol::Kind::Variable;
        secondSym.name = stmt->secondVariable;
        secondSym.type = secondType ? secondType : types::unknown();
        secondSym.isFinal = true;
        defineSymbol(stmt->secondVariable, secondSym, stmt->loc);
        markInitialized(stmt->secondVariable);
    }

    loopDepth_++;
    analyzeStmt(stmt->body.get());
    loopDepth_--;
    popScope(stmt->body ? scopeEndForStmt(stmt->body.get()) : stmt->loc);
}

/// @brief Analyze a return statement against the current callable result type.
/// @param stmt Return statement.
/// @details Contextualizes empty Map/Set literals, permits Unit only for void-like returns, and
///          retains the supported Number-to-Integer return narrowing.
void Sema::analyzeReturnStmt(ReturnStmt *stmt) {
    if (!expectedReturnType_) {
        if (stmt->value)
            analyzeExpr(stmt->value.get());
        error(stmt->loc, "Return statement can only be used inside a function or method");
        return;
    }

    if (stmt->value) {
        TypeRef valueType = analyzeExpr(stmt->value.get());
        if (expectedReturnType_ && stmt->value->kind == ExprKind::MapLiteral) {
            auto *mapLiteral = static_cast<MapLiteralExpr *>(stmt->value.get());
            if (mapLiteral->entries.empty() && (expectedReturnType_->kind == TypeKindSem::Set ||
                                                expectedReturnType_->kind == TypeKindSem::Map)) {
                exprTypes_[stmt->value.get()] = expectedReturnType_;
                valueType = expectedReturnType_;
            }
        }
        if (valueType && valueType->kind == TypeKindSem::Unit) {
            if (expectedReturnType_ && (expectedReturnType_->kind == TypeKindSem::Void ||
                                        expectedReturnType_->kind == TypeKindSem::Unit))
                return;
            error(stmt->value->loc,
                  "Unit literal cannot be returned from a non-void function; use null for "
                  "optional values");
            valueType = types::unknown();
        }
        if (expectedReturnType_ && valueType && valueType->kind != TypeKindSem::Unknown &&
            !expectedReturnType_->isAssignableFrom(*valueType)) {
            // Allow implicit Number -> Integer conversion in return statements
            // This enables returning Floor/Ceil/Round/Trunc results from Integer functions
            bool allowedNarrowing = (expectedReturnType_->kind == TypeKindSem::Integer &&
                                     valueType->kind == TypeKindSem::Number);
            if (!allowedNarrowing) {
                errorTypeMismatch(stmt->value->loc, expectedReturnType_, valueType);
            }
        }
    } else {
        // No value - must be void return
        if (expectedReturnType_ && expectedReturnType_->kind != TypeKindSem::Void) {
            error(stmt->loc, "Expected return value");
        }
    }
}

/// @brief Analyze a guard statement.
/// @param stmt Guard statement containing a condition and else block.
/// @details Requires a Boolean condition and verifies that every path through the else block exits
///          the current scope.
void Sema::analyzeGuardStmt(GuardStmt *stmt) {
    TypeRef condType = analyzeExpr(stmt->condition.get());
    if (condType->kind != TypeKindSem::Boolean) {
        error(stmt->condition->loc, "Condition must be Boolean");
    }

    analyzeStmt(stmt->elseBlock.get());
    if (!stmtAlwaysExits(stmt->elseBlock.get())) {
        error(stmt->loc, "Guard else block must exit the scope");
    }
}

/// @brief Analyze a statement-form pattern match.
/// @param stmt Match statement.
/// @details Creates arm-local binding scopes, validates Boolean guards, restores initialization
///          state after each arm, and diagnoses non-exhaustive Boolean, enum, integer, Optional,
///          and Result matches.
void Sema::analyzeMatchStmt(MatchStmt *stmt) {
    TypeRef scrutineeType = analyzeExpr(stmt->scrutinee.get());
    scrutineeType = declaredOptionalSurfaceType(stmt->scrutinee.get(), scrutineeType);
    exprTypes_[stmt->scrutinee.get()] = scrutineeType;

    MatchCoverage coverage;
    for (auto &arm : stmt->arms) {
        std::unordered_map<std::string, TypeRef> bindings;
        auto preArmState = saveInitState();
        pushScope(stmt->loc);

        analyzeMatchPattern(arm.pattern, scrutineeType, coverage, bindings);

        for (const auto &binding : bindings) {
            Symbol sym;
            sym.kind = Symbol::Kind::Variable;
            sym.name = binding.first;
            sym.type = binding.second;
            sym.isFinal = true;
            defineSymbol(binding.first, sym, stmt->loc);
            markInitialized(binding.first);
        }

        if (arm.pattern.guard) {
            TypeRef guardType = analyzeExpr(arm.pattern.guard.get());
            if (guardType && guardType->kind != TypeKindSem::Unknown &&
                guardType->kind != TypeKindSem::Boolean) {
                error(arm.pattern.guard->loc, "Match guard must be Boolean");
            }
        }

        analyzeExpr(arm.body.get());
        popScope(arm.body ? arm.body->loc : stmt->loc);
        initializedVars_ = std::move(preArmState);
    }

    if (!coverage.hasIrrefutable) {
        if (scrutineeType && scrutineeType->kind == TypeKindSem::Boolean) {
            if (coverage.coveredBooleans.size() < 2) {
                error(stmt->loc,
                      "Non-exhaustive patterns: match on Boolean must cover both true "
                      "and false, or use a wildcard (_)");
            }
        } else if (scrutineeType && scrutineeType->kind == TypeKindSem::Enum) {
            auto it = enumDecls_.find(scrutineeType->name);
            if (it != enumDecls_.end()) {
                size_t totalVariants = it->second->variants.size();
                if (coverage.coveredEnumVariants.size() < totalVariants) {
                    std::string missing;
                    for (const auto &v : it->second->variants) {
                        if (coverage.coveredEnumVariants.find(v.name) ==
                            coverage.coveredEnumVariants.end()) {
                            if (!missing.empty())
                                missing += ", ";
                            missing += scrutineeType->name + "." + v.name;
                        }
                    }
                    error(stmt->loc, "Non-exhaustive patterns: missing variants " + missing);
                }
            }
        } else if (scrutineeType && scrutineeType->isIntegral()) {
            error(stmt->loc,
                  "Non-exhaustive patterns: match on Integer requires a wildcard (_) or "
                  "else case to be exhaustive");
        } else if (scrutineeType && scrutineeType->kind == TypeKindSem::Optional) {
            if (!(coverage.coversNull && coverage.coversSome)) {
                error(stmt->loc,
                      "Non-exhaustive patterns: match on optional type should use a "
                      "wildcard (_) or handle all cases");
            }
        } else if (scrutineeType && scrutineeType->kind == TypeKindSem::Result) {
            if (!(coverage.coversResultOk && coverage.coversResultErr)) {
                error(stmt->loc,
                      "Non-exhaustive patterns: match on Result should handle Ok and Err or use "
                      "a wildcard (_)");
            }
        }
    }
}

/// @brief Determine whether every path through a statement exits the current control-flow region.
/// @param stmt Statement tree to inspect.
/// @return True for direct terminators, terminating block tails, complete terminating if/else
///         statements, or try statements whose required paths terminate.
bool Sema::stmtAlwaysExits(Stmt *stmt) {
    if (!stmt)
        return false;

    switch (stmt->kind) {
        case StmtKind::Return:
        case StmtKind::Break:
        case StmtKind::Continue:
        case StmtKind::Throw:
            return true;

        case StmtKind::Block: {
            auto *block = static_cast<BlockStmt *>(stmt);
            if (block->statements.empty())
                return false;
            return stmtAlwaysExits(block->statements.back().get());
        }

        case StmtKind::If: {
            auto *ifStmt = static_cast<IfStmt *>(stmt);
            if (!ifStmt->elseBranch)
                return false;
            return stmtAlwaysExits(ifStmt->thenBranch.get()) &&
                   stmtAlwaysExits(ifStmt->elseBranch.get());
        }

        case StmtKind::Try: {
            auto *tryStmt = static_cast<TryStmt *>(stmt);
            if (tryStmt->finallyBody && stmtAlwaysExits(tryStmt->finallyBody.get()))
                return true;
            bool tryExits = stmtAlwaysExits(tryStmt->tryBody.get());
            if (!tryStmt->catches.empty()) {
                for (const auto &catchClause : tryStmt->catches) {
                    if (!stmtAlwaysExits(catchClause.body.get()))
                        return false;
                }
                return tryExits;
            }
            return tryExits;
        }

        default:
            return false;
    }
}

} // namespace il::frontends::zia
