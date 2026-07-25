//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/SemanticAnalyzer_Stmts_Runtime.cpp
// Purpose: Implements runtime/data-manipulation statement checks for the BASIC
//          semantic analyzer, covering LET/DIM/REDIM, RANDOMIZE, and CALL.
// Key invariants: Shared helpers guard loop-variable mutations and array/type
//                 tracking remains in sync with procedure scopes.
// Ownership/Lifetime: Borrowed SemanticAnalyzer state only.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/// @file SemanticAnalyzer_Stmts_Runtime.cpp
/// @brief Implements semantic validation for BASIC statements that manipulate runtime state.
/// @details These helpers cover statement calls, scalar/member/array assignment,
///          RANDOMIZE, DIM/CONST/STATIC/SHARED declarations, REDIM metadata, and
///          SWAP traversal while maintaining scoped symbol, type, object-class,
///          array, channel, and implicit-conversion bookkeeping.

#include "frontends/basic/SemanticAnalyzer_Stmts_Runtime.hpp"
#include "frontends/basic/ASTUtils.hpp"

#include "frontends/basic/BasicDiagnosticMessages.hpp"
#include "frontends/basic/Diag.hpp"
#include "frontends/basic/IdentifierUtil.hpp"
#include "frontends/basic/SemanticAnalyzer_Internal.hpp"
#include "frontends/basic/StringUtils.hpp"
#include "frontends/basic/ast/ExprNodes.hpp"

#include <climits>

namespace il::frontends::basic::semantic_analyzer_detail {

/// @brief Binds runtime statement helpers to shared analyzer state.
/// @param analyzer Analyzer borrowed by @ref StmtShared.
/// @post No analyzer stack is modified.
RuntimeStmtContext::RuntimeStmtContext(SemanticAnalyzer &analyzer) noexcept
    : StmtShared(analyzer) {}

} // namespace il::frontends::basic::semantic_analyzer_detail

namespace il::frontends::basic {

using semantic_analyzer_detail::astToSemanticType;
using semantic_analyzer_detail::RuntimeStmtContext;
using semantic_analyzer_detail::semanticTypeName;

/// @brief Validates a statement-level invocation node.
/// @details A null call is ignored. Plain calls resolve with SUB expectation,
///          though FUNCTION signatures are accepted as statements, then visit
///          and validate all arguments. Method calls perform a best-effort
///          receiver precheck: an unbound variable spelling that is indexed as
///          a class is diagnosed as an unknown qualified procedure; other
///          unresolved receivers are deferred. The full method expression is
///          then visited for runtime/OOP resolution and argument checks. Other
///          expression node kinds are ignored defensively.
/// @param stmt Mutable call statement; runtime method aliases may rewrite its
///             contained call expression.
void SemanticAnalyzer::analyzeCallStmt(CallStmt &stmt) {
    if (!stmt.call)
        return;

    if (auto *ce = as<CallExpr>(*stmt.call)) {
        // Statement calls must target SUBs (not FUNCTIONs)
        const ProcSignature *sig = resolveCallee(*ce, ProcSignature::Kind::Sub);
        checkCallArgs(*ce, sig);
        return;
    }

    if (auto *me = as<MethodCallExpr>(*stmt.call)) {
        // Best-effort analysis: visit receiver and args to trigger diagnostics.
        // BUG-037 fix: Detect undefined variables in method calls and suggest qualified call
        // syntax. BUG-120 fix: Only emit error for unknown base if the base variable looks like a
        // namespace (not a class-qualified class name). If the parser produced a MethodCallExpr, it
        // has already determined this is NOT a namespace-qualified call. Procedure-local object
        // variables may not be in symbols_ yet during semantic analysis but will be handled
        // correctly by the lowerer.
        if (me->base) {
            if (auto *varExpr = as<VarExpr>(*me->base)) {
                // Check if the base variable exists; if not, this might be a qualified call.
                // However, only emit the error if the name is a known namespace. Otherwise,
                // it's likely a local object variable that will be resolved during lowering.
                if (symbols_.find(std::string{varExpr->name}) == symbols_.end()) {
                    // BUG-OOP-032 fix: Also check if the variable can be resolved via scopes_.
                    // Local variables inside SUB/FUNCTION are mangled (e.g., "player" becomes
                    // "player_42") and stored in symbols_ with the mangled name. The scope
                    // tracker maintains the original→mangled mapping, so we must check both.
                    bool isLocalVariable = scopes_.resolve(varExpr->name).has_value();

                    // Only error if this looks like a namespace-qualified call.
                    // If it's a simple local variable name (not registered as a namespace),
                    // let the lowerer handle it - it might be an object instance.
                    bool looksLikeNamespace =
                        !isLocalVariable && oopIndex_.findClass(varExpr->name) != nullptr;
                    if (looksLikeNamespace) {
                        std::vector<std::string> segments;
                        segments.push_back(std::string{varExpr->name});
                        segments.push_back(me->method);
                        std::string qualifiedName = CanonicalizeQualified(segments);
                        std::vector<std::string> attempts;
                        diagx::ErrorUnknownProcWithTries(
                            de.emitter(), stmt.loc, qualifiedName, attempts);
                        return;
                    }
                    // Otherwise, skip the error - the lowerer will handle this case
                    // (either as an object method call or emit a more specific error).
                }
            }
        }
        // Full expression analysis: resolves the runtime/OOP method, validates
        // argument compatibility (including object-parameter checks), and reports
        // unknown methods on resolved receivers. Unresolvable receivers still fall
        // through silently, preserving the BUG-120 leniency for late-bound locals.
        visitExpr(*stmt.call);
        return;
    }

    // Unknown invocation node: nothing to analyze (defensive).
}

/// @brief Validates assignment to a simple variable or implicit-instance field.
/// @details Constants are rejected first. Assignment to the active FUNCTION
///          name records VB-style result flow. An unshadowed implicit field is
///          type-checked directly, including float-to-int warning/conversion,
///          int-to-bool conversion, and scalar/object mismatch rules.
///
///          For normal variables, the RHS is evaluated before symbol resolution
///          so a new unsuffixed variable may adopt string, boolean, float, or
///          object type. Concrete object-class provenance is tracked and stale
///          mappings removed. Active FOR variables are diagnosed. Whole-array
///          versus scalar assignment is rejected. An integer variable assigned
///          a float-producing arithmetic binary expression is promoted to float
///          unless its name has `%` or `&`; other float-to-int assignments
///          record narrowing and warning `B2002`.
/// @param v Mutable destination name that may be scope-resolved.
/// @param l LET statement supplying RHS and diagnostic location.
void SemanticAnalyzer::analyzeVarAssignment(VarExpr &v, const LetStmt &l) {
    RuntimeStmtContext ctx(*this);

    // Check if trying to assign to a constant
    if (constants_.find(v.name) != constants_.end()) {
        std::string msg = "cannot assign to constant '" + v.name + "'";
        de.emit(il::support::Severity::Error, "B2020", l.loc, 1, std::move(msg));
        return;
    }

    // BUG-003: Check if this is an assignment to the function name (VB-style implicit return)
    if (activeFunction_ && string_utils::iequals(v.name, activeFunction_->name)) {
        activeFunctionNameAssigned_ = true;
    }

    if (!scopes_.resolve(v.name).has_value()) {
        if (const auto *field = semantic_analyzer_detail::findActiveInstanceField(*this, v.name)) {
            Type exprTy = Type::Unknown;
            if (l.expr)
                exprTy = visitExpr(*l.expr);

            const Type fieldTy = semantic_analyzer_detail::semanticTypeFromOopField(*field);
            if (!l.expr || fieldTy == Type::Unknown || exprTy == Type::Unknown)
                return;

            if (fieldTy == Type::Int && exprTy == Type::Float) {
                markImplicitConversion(*l.expr, Type::Int);
                std::string msg = "narrowing conversion from FLOAT to INT in assignment";
                de.emit(il::support::Severity::Warning, "B2002", l.loc, 1, std::move(msg));
            } else if (fieldTy == Type::Int && (exprTy == Type::String || exprTy == Type::Object)) {
                std::string msg = "operand type mismatch";
                de.emit(il::support::Severity::Error, "B2001", l.loc, 1, std::move(msg));
            } else if (fieldTy == Type::Bool && exprTy == Type::Int) {
                markImplicitConversion(*l.expr, Type::Bool);
            } else if (fieldTy == Type::String && exprTy != Type::String) {
                std::string msg = "operand type mismatch";
                de.emit(il::support::Severity::Error, "B2001", l.loc, 1, std::move(msg));
            } else if (fieldTy == Type::Bool && exprTy != Type::Bool) {
                std::string msg = "operand type mismatch";
                de.emit(il::support::Severity::Error, "B2001", l.loc, 1, std::move(msg));
            } else if (fieldTy == Type::Object &&
                       (exprTy == Type::Int || exprTy == Type::Float || exprTy == Type::String ||
                        exprTy == Type::Bool)) {
                std::string msg = "operand type mismatch";
                de.emit(il::support::Severity::Error, "B2001", l.loc, 1, std::move(msg));
            }
            return;
        }
    }

    // BUG-001 FIX: Evaluate RHS expression BEFORE resolving variable
    // This allows us to infer the variable's type from the RHS
    Type exprTy = Type::Unknown;
    std::optional<std::string> assignedObjectClass;
    if (l.expr)
        exprTy = visitExpr(*l.expr);
    if (l.expr)
        assignedObjectClass = semantic_analyzer_detail::inferObjectClassQName(*this, *l.expr);

    // Check if variable has no type suffix (ends with alphanumeric, not $#!%&)
    bool hasNoSuffix = !v.name.empty() && std::isalnum(static_cast<unsigned char>(v.name.back()));

    // Check if variable already exists
    bool isNewVariable = (varTypes_.find(v.name) == varTypes_.end());

    // If a new variable has no suffix, adopt the RHS type when it is informative.
    if (isNewVariable && hasNoSuffix &&
        (exprTy == Type::String || exprTy == Type::Bool || exprTy == Type::Float ||
         exprTy == Type::Object || assignedObjectClass.has_value())) {
        varTypes_[v.name] =
            (exprTy == Type::Object || assignedObjectClass.has_value()) ? Type::Object : exprTy;
    }

    resolveAndTrackSymbol(v.name, SymbolKind::Definition);
    if (ctx.isLoopVariable(v.name))
        ctx.reportLoopVariableMutation(v.name, l.loc, static_cast<uint32_t>(v.name.size()));

    /// Replaces or removes the destination's concrete class mapping while
    /// recording the procedure-scope baseline.
    auto updateTrackedObjectClass = [&](std::optional<std::string> nextClass) {
        auto itClass = objectClassTypes_.find(v.name);
        if (activeProcScope_) {
            std::optional<std::string> previous;
            if (itClass != objectClassTypes_.end())
                previous = itClass->second;
            activeProcScope_->noteObjectClassMutation(v.name, previous);
        }
        if (nextClass)
            objectClassTypes_[v.name] = *nextClass;
        else if (itClass != objectClassTypes_.end())
            objectClassTypes_.erase(itClass);
    };

    if (assignedObjectClass) {
        updateTrackedObjectClass(assignedObjectClass);
    } else if (exprTy == Type::Object) {
        updateTrackedObjectClass(std::nullopt);
    } else if (exprTy != Type::Unknown && objectClassTypes_.contains(v.name)) {
        updateTrackedObjectClass(std::nullopt);
    }

    Type varTy = Type::Int;
    if (auto itType = varTypes_.find(v.name); itType != varTypes_.end())
        varTy = itType->second;

    /// Tests the three whole-array semantic categories.
    auto isArrayType = [](Type ty) {
        return ty == Type::ArrayInt || ty == Type::ArrayString || ty == Type::ArrayObject;
    };

    if (isArrayType(varTy)) {
        if (exprTy != Type::Unknown && exprTy != varTy) {
            std::string msg = "cannot assign scalar to array variable";
            de.emit(il::support::Severity::Error, "B2001", l.loc, 1, std::move(msg));
        }
        return;
    }

    if (!l.expr)
        return;

    if (isArrayType(exprTy)) {
        std::string msg = "cannot assign array value to scalar variable";
        de.emit(il::support::Severity::Error, "B2001", l.loc, 1, std::move(msg));
    } else if (varTy == Type::Int && exprTy == Type::Float) {
        bool allowFloatPromotion = false;
        if (l.expr) {
            if (const auto *bin = as<const BinaryExpr>(*l.expr)) {
                const bool hasExplicitIntSuffix =
                    !v.name.empty() && (v.name.back() == '%' || v.name.back() == '&');
                switch (bin->op) {
                    case BinaryExpr::Op::Div:
                    case BinaryExpr::Op::Add:
                    case BinaryExpr::Op::Sub:
                    case BinaryExpr::Op::Mul:
                        allowFloatPromotion = !hasExplicitIntSuffix;
                        break;
                    default:
                        break;
                }
            }
        }

        if (allowFloatPromotion) {
            if (activeProcScope_) {
                std::optional<Type> previous = varTy;
                activeProcScope_->noteVarTypeMutation(v.name, previous);
            }
            varTypes_[v.name] = Type::Float;
        } else {
            markImplicitConversion(*l.expr, Type::Int);
            std::string msg = "narrowing conversion from FLOAT to INT in assignment";
            de.emit(il::support::Severity::Warning, "B2002", l.loc, 1, std::move(msg));
        }
    } else if (varTy == Type::Int && (exprTy == Type::String || exprTy == Type::Object)) {
        std::string msg = "operand type mismatch";
        de.emit(il::support::Severity::Error, "B2001", l.loc, 1, std::move(msg));
    } else if (varTy == Type::String && exprTy != Type::Unknown && exprTy != Type::String) {
        std::string msg = "operand type mismatch";
        de.emit(il::support::Severity::Error, "B2001", l.loc, 1, std::move(msg));
    } else if (varTy == Type::Bool && exprTy == Type::Int) {
        markImplicitConversion(*l.expr, Type::Bool);
    } else if (varTy == Type::Bool && exprTy != Type::Unknown && exprTy != Type::Bool) {
        std::string msg = "operand type mismatch";
        de.emit(il::support::Severity::Error, "B2001", l.loc, 1, std::move(msg));
    }
}

/// @brief Validates assignment to a named array element.
/// @details Non-dotted names are scope-resolved and checked against array/type
///          maps; dotted field names defer field existence/type validation to
///          lowering. The legacy single index or each multidimensional index is
///          visited: float literals receive integer conversion plus `B2002`,
///          while nonliteral floats and other known non-integers emit `B2001`.
///          RHS checking defaults to integer elements unless the tracked array
///          is string/object. A constant legacy index for one known extent warns
///          when negative or greater than or equal to the stored extent value.
///          Known extents are copied into the AST for later lowering.
/// @param a Mutable array access whose name, indices, conversions, and resolved
///          extents may be updated.
/// @param l Assignment supplying RHS and location.
void SemanticAnalyzer::analyzeArrayAssignment(ArrayExpr &a, const LetStmt &l) {
    // BUG-056 fix: Check if this is an array field access (e.g., "B.CELLS")
    // If the name contains a dot, it's accessing an array field on an object
    bool isArrayField = a.name.find('.') != std::string::npos;

    if (!isArrayField) {
        resolveAndTrackSymbol(a.name, SymbolKind::Reference);
        if (!arrays_.contains(a.name)) {
            de.emit(diag::BasicDiag::UnknownArray,
                    a.loc,
                    static_cast<uint32_t>(a.name.size()),
                    std::initializer_list<diag::Replacement>{diag::Replacement{"name", a.name}});
        }
        if (auto itType = varTypes_.find(a.name);
            itType != varTypes_.end() && itType->second != Type::ArrayInt &&
            itType->second != Type::ArrayString && itType->second != Type::ArrayObject) {
            de.emit(diag::BasicDiag::NotAnArray,
                    a.loc,
                    static_cast<uint32_t>(a.name.size()),
                    std::initializer_list<diag::Replacement>{diag::Replacement{"name", a.name}});
        }
    }
    // For array fields, validation happens during lowering when field types are known

    // Validate each index expression (supports multi-dimensional arrays)
    // For backward compatibility: use 'index' for single-dim, 'indices' for multi-dim
    if (a.index) {
        // Single-dimensional array (backward compatible path)
        auto indexTy = visitExpr(*a.index);
        if (indexTy == Type::Float) {
            if (as<FloatExpr>(*a.index)) {
                insertImplicitCast(*a.index, Type::Int);
                std::string msg = "narrowing conversion from FLOAT to INT in array index";
                de.emit(il::support::Severity::Warning, "B2002", a.loc, 1, std::move(msg));
            } else {
                std::string msg = "index type mismatch";
                de.emit(il::support::Severity::Error, "B2001", a.loc, 1, std::move(msg));
            }
        } else if (indexTy != Type::Unknown && indexTy != Type::Int) {
            std::string msg = "index type mismatch";
            de.emit(il::support::Severity::Error, "B2001", a.loc, 1, std::move(msg));
        }
    } else {
        // Multi-dimensional array (new path)
        for (auto &indexPtr : a.indices) {
            if (!indexPtr)
                continue;
            auto indexTy = visitExpr(*indexPtr);
            if (indexTy == Type::Float) {
                if (as<FloatExpr>(*indexPtr)) {
                    insertImplicitCast(*indexPtr, Type::Int);
                    std::string msg = "narrowing conversion from FLOAT to INT in array index";
                    de.emit(il::support::Severity::Warning, "B2002", a.loc, 1, std::move(msg));
                } else {
                    std::string msg = "index type mismatch";
                    de.emit(il::support::Severity::Error, "B2001", a.loc, 1, std::move(msg));
                }
            } else if (indexTy != Type::Unknown && indexTy != Type::Int) {
                std::string msg = "index type mismatch";
                de.emit(il::support::Severity::Error, "B2001", a.loc, 1, std::move(msg));
            }
        }
    }

    Type valueTy = Type::Unknown;
    if (l.expr) {
        valueTy = visitExpr(*l.expr);

        // Determine expected element type based on array type
        Type expectedElementType = Type::Int; // default for integer arrays
        if (auto itType = varTypes_.find(a.name); itType != varTypes_.end()) {
            if (itType->second == Type::ArrayString)
                expectedElementType = Type::String;
            else if (itType->second == Type::ArrayObject)
                expectedElementType = Type::Object;
        }

        if (expectedElementType == Type::Int) {
            // Integer array: allow Int or Float with narrowing warning.
            if (valueTy == Type::Float) {
                markImplicitConversion(*l.expr, Type::Int);
                std::string msg = "narrowing conversion from FLOAT to INT in array assignment";
                de.emit(il::support::Severity::Warning, "B2002", l.loc, 1, std::move(msg));
            } else if (valueTy != Type::Unknown && valueTy != Type::Int) {
                std::string msg = "array element type mismatch: expected INT, got ";
                msg += semanticTypeName(valueTy);
                de.emit(il::support::Severity::Error, "B2001", l.loc, 1, std::move(msg));
            }
        } else if (expectedElementType == Type::String) {
            // String array: require String type
            if (valueTy != Type::Unknown && valueTy != Type::String) {
                std::string msg = "array element type mismatch: expected STRING, got ";
                msg += semanticTypeName(valueTy);
                de.emit(il::support::Severity::Error, "B2001", l.loc, 1, std::move(msg));
            }
        } else if (expectedElementType == Type::Object) {
            if (valueTy != Type::Unknown && valueTy != Type::Object) {
                std::string msg = "array element type mismatch: expected OBJECT, got ";
                msg += semanticTypeName(valueTy);
                de.emit(il::support::Severity::Error, "B2001", l.loc, 1, std::move(msg));
            }
        }
    }
    auto it = arrays_.find(a.name);
    if (it != arrays_.end() && !it->second.extents.empty() && it->second.extents.size() == 1 &&
        a.index) {
        // Bounds check for single-dimensional arrays
        long long arraySize = it->second.extents[0];
        if (arraySize >= 0) {
            if (auto *ci = as<const IntExpr>(*a.index)) {
                if (ci->value < 0 || ci->value >= arraySize) {
                    std::string msg = "index out of bounds";
                    de.emit(il::support::Severity::Warning, "B3001", a.loc, 1, std::move(msg));
                }
            }
        }
    }

    // BUG-020 fix: Store resolved extents in AST node so the lowerer can access them
    // even after procedure scope cleanup erases ArrayMetadata from the semantic analyzer.
    // Re-lookup in case the earlier lookup was skipped (multi-dimensional arrays).
    auto metaIt = arrays_.find(a.name);
    if (metaIt != arrays_.end() && !metaIt->second.extents.empty()) {
        a.resolvedExtents = metaIt->second.extents;
    }
}

/// @brief Validates assignment to a resolved member/property expression.
/// @details Visits target and optional RHS. Unknown/missing values stop
///          compatibility checks. Float-to-int records narrowing/warning,
///          int-to-bool records conversion, and incompatible string, boolean,
///          integer, or object assignments emit `B2001`.
/// @param m Mutable member target.
/// @param l Assignment supplying RHS and diagnostic location.
void SemanticAnalyzer::analyzeMemberAssignment(MemberAccessExpr &m, const LetStmt &l) {
    Type targetTy = visitExpr(m);
    Type exprTy = Type::Unknown;
    if (l.expr)
        exprTy = visitExpr(*l.expr);

    if (!l.expr || targetTy == Type::Unknown || exprTy == Type::Unknown)
        return;

    if (targetTy == Type::Int && exprTy == Type::Float) {
        markImplicitConversion(*l.expr, Type::Int);
        std::string msg = "narrowing conversion from FLOAT to INT in assignment";
        de.emit(il::support::Severity::Warning, "B2002", l.loc, 1, std::move(msg));
    } else if (targetTy == Type::Int && (exprTy == Type::String || exprTy == Type::Object)) {
        std::string msg = "operand type mismatch";
        de.emit(il::support::Severity::Error, "B2001", l.loc, 1, std::move(msg));
    } else if (targetTy == Type::Bool && exprTy == Type::Int) {
        markImplicitConversion(*l.expr, Type::Bool);
    } else if (targetTy == Type::String && exprTy != Type::String) {
        std::string msg = "operand type mismatch";
        de.emit(il::support::Severity::Error, "B2001", l.loc, 1, std::move(msg));
    } else if (targetTy == Type::Bool && exprTy != Type::Bool) {
        std::string msg = "operand type mismatch";
        de.emit(il::support::Severity::Error, "B2001", l.loc, 1, std::move(msg));
    } else if (targetTy == Type::Object && (exprTy == Type::Int || exprTy == Type::Float ||
                                            exprTy == Type::String || exprTy == Type::Bool)) {
        std::string msg = "operand type mismatch";
        de.emit(il::support::Severity::Error, "B2001", l.loc, 1, std::move(msg));
    }
}

/// @brief Diagnoses a LET target unsupported as an lvalue.
/// @details Visits target and RHS first to retain nested diagnostics, then emits
///          `B2007` unconditionally.
/// @param l LET statement with a non-assignable target form.
void SemanticAnalyzer::analyzeConstExpr(const LetStmt &l) {
    if (l.target)
        visitExpr(*l.target);
    if (l.expr)
        visitExpr(*l.expr);
    std::string msg = "left-hand side of LET must be a variable, array element, or object field";
    de.emit(il::support::Severity::Error, "B2007", l.loc, 1, std::move(msg));
}

/// @brief Dispatches LET validation by the target expression's shape.
/// @details Null targets are ignored. Variables, arrays, and member accesses
///          use dedicated validators. Method-call syntax is treated as an
///          object array-field target: its base and index arguments are visited,
///          while RHS element typing is deferred to lowering. An unshadowed
///          call-expression target may denote an implicit-instance array field
///          and receives index plus element checks; other call/unknown forms
///          emit `B2007`.
/// @param l Mutable assignment and target AST.
void SemanticAnalyzer::analyzeLet(LetStmt &l) {
    if (!l.target)
        return;
    if (auto *v = as<VarExpr>(*l.target)) {
        analyzeVarAssignment(*v, l);
    } else if (auto *a = as<ArrayExpr>(*l.target)) {
        analyzeArrayAssignment(*a, l);
    } else if (auto *mc = as<MethodCallExpr>(*l.target)) {
        // BUG-056: Treat method-like syntax on LHS (obj.field(...)) as array-field assignment.
        // Perform basic index type validation and RHS checks similar to arrays.
        if (mc->base)
            visitExpr(*mc->base);
        // Validate indices: all args must be INT (allow float literals with narrowing warning)
        for (auto &arg : mc->args) {
            if (!arg)
                continue;
            Type ty = visitExpr(*arg);
            if (ty == Type::Float) {
                if (as<FloatExpr>(*arg)) {
                    insertImplicitCast(*arg, Type::Int);
                    std::string msg = "narrowing conversion from FLOAT to INT in array index";
                    de.emit(il::support::Severity::Warning, "B2002", mc->loc, 1, std::move(msg));
                } else {
                    std::string msg = "index type mismatch";
                    de.emit(il::support::Severity::Error, "B2001", mc->loc, 1, std::move(msg));
                }
            } else if (ty != Type::Unknown && ty != Type::Int) {
                std::string msg = "index type mismatch";
                de.emit(il::support::Severity::Error, "B2001", mc->loc, 1, std::move(msg));
            }
        }
        // Analyze RHS to surface type errors; element type will be validated during lowering
        if (l.expr)
            visitExpr(*l.expr);
    } else if (auto *c = as<CallExpr>(*l.target)) {
        if (!scopes_.resolve(c->callee).has_value()) {
            if (const auto *field =
                    semantic_analyzer_detail::findActiveInstanceField(*this, c->callee);
                field && field->isArray) {
                for (auto &arg : c->args) {
                    if (!arg)
                        continue;
                    Type ty = visitExpr(*arg);
                    if (ty == Type::Float) {
                        if (as<FloatExpr>(*arg)) {
                            insertImplicitCast(*arg, Type::Int);
                            std::string msg =
                                "narrowing conversion from FLOAT to INT in array index";
                            de.emit(
                                il::support::Severity::Warning, "B2002", c->loc, 1, std::move(msg));
                        } else {
                            std::string msg = "index type mismatch";
                            de.emit(
                                il::support::Severity::Error, "B2001", c->loc, 1, std::move(msg));
                        }
                    } else if (ty != Type::Unknown && ty != Type::Int) {
                        std::string msg = "index type mismatch";
                        de.emit(il::support::Severity::Error, "B2001", c->loc, 1, std::move(msg));
                    }
                }

                Type exprTy = Type::Unknown;
                if (l.expr)
                    exprTy = visitExpr(*l.expr);

                const Type fieldTy = semantic_analyzer_detail::semanticTypeFromOopField(*field);
                if (!l.expr || fieldTy == Type::Unknown || exprTy == Type::Unknown)
                    return;

                if (fieldTy == Type::Int && exprTy == Type::Float) {
                    markImplicitConversion(*l.expr, Type::Int);
                    std::string msg = "narrowing conversion from FLOAT to INT in array assignment";
                    de.emit(il::support::Severity::Warning, "B2002", l.loc, 1, std::move(msg));
                } else if (fieldTy == Type::Int &&
                           (exprTy == Type::String || exprTy == Type::Object)) {
                    std::string msg = "array element type mismatch: expected INT, got ";
                    msg += semanticTypeName(exprTy);
                    de.emit(il::support::Severity::Error, "B2001", l.loc, 1, std::move(msg));
                } else if (fieldTy == Type::Bool && exprTy == Type::Int) {
                    markImplicitConversion(*l.expr, Type::Bool);
                } else if (fieldTy == Type::String && exprTy != Type::String) {
                    std::string msg = "array element type mismatch: expected STRING, got ";
                    msg += semanticTypeName(exprTy);
                    de.emit(il::support::Severity::Error, "B2001", l.loc, 1, std::move(msg));
                } else if (fieldTy == Type::Bool && exprTy != Type::Bool) {
                    std::string msg = "array element type mismatch: expected BOOLEAN, got ";
                    msg += semanticTypeName(exprTy);
                    de.emit(il::support::Severity::Error, "B2001", l.loc, 1, std::move(msg));
                } else if (fieldTy == Type::Object &&
                           (exprTy == Type::Int || exprTy == Type::Float ||
                            exprTy == Type::String || exprTy == Type::Bool)) {
                    std::string msg = "array element type mismatch: expected OBJECT, got ";
                    msg += semanticTypeName(exprTy);
                    de.emit(il::support::Severity::Error, "B2001", l.loc, 1, std::move(msg));
                }
                return;
            }
        }
        analyzeConstExpr(l);
    } else if (auto *m = as<MemberAccessExpr>(*l.target)) {
        analyzeMemberAssignment(*m, l);
    } else {
        analyzeConstExpr(l);
    }
}

/// @brief Validates an optional RANDOMIZE seed.
/// @details Visits a present seed and accepts integer, float, or unknown
///          recovery type; every other known type emits `B2001`.
/// @param r RANDOMIZE statement to inspect.
void SemanticAnalyzer::analyzeRandomize(const RandomizeStmt &r) {
    if (r.seed) {
        auto ty = visitExpr(*r.seed);
        if (ty != Type::Unknown && ty != Type::Int && ty != Type::Float) {
            std::string msg = "seed type mismatch";
            de.emit(il::support::Severity::Error, "B2001", r.loc, 1, std::move(msg));
        }
    }
}

/// @brief Validates DIM and records scalar or array declaration metadata.
/// @details For arrays, uses the legacy `size` expression when present,
///          otherwise non-null `dimensions`. Integer sizes and integer-valued
///          CONST variables become stored inclusive upper bounds; nonnegative
///          float literals are truncated with conversion/warning. Negative
///          bounds emit `B2003`. When all bounds are known, total element count
///          multiplies `(bound + 1)` with overflow diagnostic `B2004`; partial
///          constants retain only known extents with unknown total.
///
///          Local declarations receive unique scoped names and duplicate-local
///          `B1006`; module declarations use their existing spelling. Array
///          shape/type state and scalar/object-class state are logged for
///          procedure rollback. Explicit class arrays are object arrays; `$` or
///          string type produces string arrays. Scalar `Zanna.System.String` or
///          `Zanna.String` class names map to string and other explicit classes
///          to object. Known extents are copied to the AST.
/// @param d Mutable declaration whose name and resolved extents may be updated.
void SemanticAnalyzer::analyzeDim(DimStmt &d) {
    ArrayMetadata metadata;

    if (d.isArray) {
        // Collect dimension expressions: check 'size' first (backward compat), then 'dimensions'
        std::vector<const ExprPtr *> dimExprs;
        if (d.size) {
            dimExprs.push_back(&d.size);
        } else if (!d.dimensions.empty()) {
            for (const auto &dimExpr : d.dimensions) {
                if (dimExpr)
                    dimExprs.push_back(&dimExpr);
            }
        }

        // Validate and extract extent values
        std::vector<long long> extents;
        bool allConstant = true;

        for (const ExprPtr *dimExprPtr : dimExprs) {
            const ExprPtr &dimExpr = *dimExprPtr;
            FloatExpr *floatLiteral = nullptr;
            auto ty = visitExpr(*dimExpr);

            // Type checking
            if (ty == Type::Float) {
                floatLiteral = as<FloatExpr>(*dimExpr);
                if (floatLiteral != nullptr) {
                    insertImplicitCast(*dimExpr, Type::Int);
                    std::string msg = "narrowing conversion from FLOAT to INT in array size";
                    de.emit(il::support::Severity::Warning, "B2002", d.loc, 1, std::move(msg));
                } else {
                    std::string msg = "size type mismatch";
                    de.emit(il::support::Severity::Error, "B2001", d.loc, 1, std::move(msg));
                }
            } else if (ty != Type::Unknown && ty != Type::Int) {
                std::string msg = "size type mismatch";
                de.emit(il::support::Severity::Error, "B2001", d.loc, 1, std::move(msg));
            }

            // Extract constant extent value
            if (floatLiteral) {
                if (floatLiteral->value < 0.0) {
                    std::string msg = "array extent must be non-negative";
                    de.emit(il::support::Severity::Error, "B2003", d.loc, 1, std::move(msg));
                } else {
                    extents.push_back(static_cast<long long>(floatLiteral->value));
                }
            } else if (auto *ci = as<const IntExpr>(*dimExpr)) {
                long long extent = ci->value;
                if (extent < 0) {
                    std::string msg = "array extent must be non-negative";
                    de.emit(il::support::Severity::Error, "B2003", d.loc, 1, std::move(msg));
                }
                extents.push_back(extent);
            }
            // BUG-010 fix: Handle VarExpr that references a CONST.
            // When DIM uses a CONST (e.g., DIM arr(MAX_ROWS, MAX_COLS)), look up the value.
            else if (auto *varRef = as<const VarExpr>(*dimExpr)) {
                auto constIt = constantValues_.find(varRef->name);
                if (constIt != constantValues_.end()) {
                    long long extent = constIt->second;
                    if (extent < 0) {
                        std::string msg = "array extent must be non-negative";
                        de.emit(il::support::Severity::Error, "B2003", d.loc, 1, std::move(msg));
                    }
                    extents.push_back(extent);
                } else {
                    // Variable is not a known CONST - runtime-computed extent
                    allConstant = false;
                }
            } else {
                // Runtime-computed extent
                allConstant = false;
            }
        }

        // Compute total size if all extents are constant
        if (allConstant && !extents.empty()) {
            long long totalSize = 1;
            bool overflow = false;

            for (long long extent : extents) {
                // BASIC extents are inclusive upper bounds; runtime length is extent + 1.
                if (extent < 0 || extent == LLONG_MAX || totalSize > LLONG_MAX / (extent + 1)) {
                    overflow = true;
                    break;
                }
                totalSize *= (extent + 1);
            }

            if (overflow) {
                std::string msg = "array size computation overflows";
                de.emit(il::support::Severity::Error, "B2004", d.loc, 1, std::move(msg));
                metadata = ArrayMetadata(); // dynamic/unknown
            } else {
                metadata = ArrayMetadata(std::move(extents), totalSize);
            }
        } else if (!extents.empty()) {
            // Partial constant extents - store what we know but mark total as dynamic
            metadata.extents = std::move(extents);
            metadata.totalSize = -1;
        }
        // else: all dynamic, leave metadata as default (empty extents, totalSize=-1)
    }

    if (scopes_.hasScope()) {
        if (scopes_.isDeclaredInCurrentScope(d.name)) {
            std::string msg = "duplicate local '" + d.name + "'";
            de.emit(il::support::Severity::Error,
                    "B1006",
                    d.loc,
                    static_cast<uint32_t>(d.name.size()),
                    std::move(msg));
        } else {
            std::string unique = scopes_.declareLocal(d.name);
            d.name = unique;
            auto insertResult = symbols_.insert(unique);
            if (insertResult.second && activeProcScope_)
                activeProcScope_->noteSymbolInserted(unique);
        }
    } else {
        auto insertResult = symbols_.insert(d.name);
        if (insertResult.second && activeProcScope_)
            activeProcScope_->noteSymbolInserted(d.name);
    }

    if (d.isArray) {
        auto itArray = arrays_.find(d.name);
        if (activeProcScope_) {
            std::optional<ArrayMetadata> previous;
            if (itArray != arrays_.end())
                previous = itArray->second;
            activeProcScope_->noteArrayMutation(d.name, previous);
        }
        arrays_[d.name] = metadata;

        // BUG-010 fix: Store resolved extents in AST node so the lowerer can compute
        // correct array size even when dimensions reference CONSTs (which would otherwise
        // need runtime lookup after procedure scope cleanup erases semantic metadata).
        if (!metadata.extents.empty()) {
            d.resolvedExtents = metadata.extents;
        }

        auto itType = varTypes_.find(d.name);
        if (activeProcScope_) {
            std::optional<Type> previous;
            if (itType != varTypes_.end())
                previous = itType->second;
            activeProcScope_->noteVarTypeMutation(d.name, previous);
        }
        // Determine array element type preferring explicit AS clause over name suffix.
        if (!d.explicitClassQname.empty()) {
            varTypes_[d.name] = Type::ArrayObject;
        } else if (d.type == ::il::frontends::basic::Type::Str ||
                   (!d.name.empty() && d.name.back() == '$')) {
            varTypes_[d.name] = Type::ArrayString;
        } else {
            varTypes_[d.name] = Type::ArrayInt;
        }

        // Arrays do not participate in object class resolution for member access.
        if (auto itClass = objectClassTypes_.find(d.name); itClass != objectClassTypes_.end()) {
            if (activeProcScope_)
                activeProcScope_->noteObjectClassMutation(d.name, itClass->second);
            objectClassTypes_.erase(d.name);
        }
    } else {
        auto itType = varTypes_.find(d.name);
        if (activeProcScope_) {
            std::optional<Type> previous;
            if (itType != varTypes_.end())
                previous = itType->second;
            activeProcScope_->noteVarTypeMutation(d.name, previous);
        }
        // Check for explicit class type Zanna.System.String -> treat as String
        // All other object/class types -> treat as Object
        Type dimType = astToSemanticType(d.type);
        if (!d.explicitClassQname.empty()) {
            std::string qname = JoinDots(d.explicitClassQname);
            if (string_utils::iequals(qname, "zanna.system.string") ||
                string_utils::iequals(qname, "zanna.string")) {
                dimType = Type::String;
            } else {
                // Any other class/interface type is an object
                dimType = Type::Object;
            }
        }
        varTypes_[d.name] = dimType;

        // Track explicit object class names for runtime member typing.
        {
            auto itClass = objectClassTypes_.find(d.name);
            if (activeProcScope_) {
                std::optional<std::string> previous;
                if (itClass != objectClassTypes_.end())
                    previous = itClass->second;
                activeProcScope_->noteObjectClassMutation(d.name, previous);
            }
            if (!d.explicitClassQname.empty()) {
                objectClassTypes_[d.name] = JoinDots(d.explicitClassQname);
            } else if (itClass != objectClassTypes_.end()) {
                objectClassTypes_.erase(d.name);
            }
        }
    }
}

/// @brief Evaluates and records a CONST declaration.
/// @details Visits the optional initializer, inserts the exact name into
///          constant and symbol sets, and caches integer literals verbatim or
///          floating literals truncated to `long long` for DIM lookup. Final
///          type normally comes from the AST declaration; an unsuffixed
///          I64-default constant with float initializer is inferred as float.
///          Type/symbol mutations are logged when a procedure scope is active.
/// @param c Mutable constant declaration and initializer.
void SemanticAnalyzer::analyzeConst(ConstStmt &c) {
    // Evaluate the initializer expression to determine its type
    Type initializerTy = Type::Unknown;
    if (c.initializer) {
        initializerTy = visitExpr(*c.initializer);
    }

    // Track this name as a constant
    constants_.insert(c.name);

    // BUG-010 fix: Store the constant's integer value for use in array dimension expressions.
    // When DIM uses a CONST name (e.g., DIM arr(MAX_ROWS)), we need to retrieve its value.
    if (c.initializer) {
        if (auto *intLit = as<IntExpr>(*c.initializer)) {
            constantValues_[c.name] = intLit->value;
        } else if (auto *floatLit = as<FloatExpr>(*c.initializer)) {
            constantValues_[c.name] = static_cast<long long>(floatLit->value);
        }
    }

    // Also track it as a symbol
    auto insertResult = symbols_.insert(c.name);
    if (insertResult.second && activeProcScope_)
        activeProcScope_->noteSymbolInserted(c.name);

    // Determine final type: infer from initializer if no explicit suffix/AS clause
    // Check if constant name has no type suffix (ends with alphanumeric)
    bool hasNoSuffix = !c.name.empty() && std::isalnum(static_cast<unsigned char>(c.name.back()));

    Type finalType = astToSemanticType(c.type);

    // If no suffix and initializer is Float, infer Float type (BUG-019 fix)
    if (hasNoSuffix && c.type == ::il::frontends::basic::Type::I64 &&
        initializerTy == Type::Float) {
        finalType = Type::Float;
    }

    // Track the type
    auto itType = varTypes_.find(c.name);
    if (activeProcScope_) {
        std::optional<Type> previous;
        if (itType != varTypes_.end())
            previous = itType->second;
        activeProcScope_->noteVarTypeMutation(c.name, previous);
    }
    varTypes_[c.name] = finalType;
}

/// @brief Analyze a STATIC statement declaring procedure-local persistent variables.
/// @details STATIC variables are procedure-scoped like DIM, but their storage persists
///          between calls. This routine only declares/uniquifies and records the
///          symbol; it does not infer a type or visit an initializer here.
/// @param s STATIC statement to validate and register.
void SemanticAnalyzer::analyzeStatic(StaticStmt &s) {
    if (scopes_.hasScope()) {
        if (scopes_.isDeclaredInCurrentScope(s.name)) {
            std::string msg = "duplicate local '" + s.name + "'";
            de.emit(il::support::Severity::Error,
                    "B1006",
                    s.loc,
                    static_cast<uint32_t>(s.name.size()),
                    std::move(msg));
        } else {
            std::string unique = scopes_.declareLocal(s.name);
            s.name = unique;
            auto insertResult = symbols_.insert(unique);
            if (insertResult.second && activeProcScope_)
                activeProcScope_->noteSymbolInserted(unique);
        }
    } else {
        auto insertResult = symbols_.insert(s.name);
        if (insertResult.second && activeProcScope_)
            activeProcScope_->noteSymbolInserted(s.name);
    }
}

/// @brief Analyze a SHARED statement listing names that refer to module-level state.
/// @details For compatibility: procedures can already access module-level globals without SHARED.
///          This handler resolves each name as a reference, then inserts it into
///          the known-symbol set without allocating a local binding. Newly
///          inserted names inside a procedure are logged for rollback.
/// @param s SHARED statement being analyzed.
void SemanticAnalyzer::analyzeShared(SharedStmt &s) {
    for (auto &name : s.names) {
        resolveAndTrackSymbol(name, SymbolKind::Reference);
        // Record as a known symbol; do not allocate local storage here.
        auto insertResult = symbols_.insert(name);
        if (insertResult.second && activeProcScope_)
            activeProcScope_->noteSymbolInserted(name);
    }
}

/// @brief Validates REDIM bounds and replaces tracked array metadata.
/// @details Visits the legacy `size` or each non-null dimension. Float literals
///          receive integer conversion/warning but are not added to the
///          compile-time extent vector; nonliteral floats and other known
///          non-integers emit mismatch diagnostics. Integer literals, including
///          diagnosed negatives, are collected; other expressions make the
///          shape dynamic. The target is scope-resolved and must exist as an
///          array. Fully constant integer bounds compute `(bound + 1)` products;
///          overflow silently falls back to unknown metadata. Dynamic/empty
///          bounds also replace metadata with unknown shape.
/// @param d Mutable REDIM statement whose name and bound expressions may be
///          resolved/annotated.
void SemanticAnalyzer::analyzeReDim(ReDimStmt &d) {
    long long sz = -1;
    std::vector<long long> extents;
    bool allConstant = true;
    std::vector<ExprPtr *> dimExprs;
    if (d.size)
        dimExprs.push_back(&d.size);
    else {
        for (auto &dim : d.dimensions) {
            if (dim)
                dimExprs.push_back(&dim);
        }
    }

    for (ExprPtr *dimPtr : dimExprs) {
        FloatExpr *floatLiteral = nullptr;
        auto ty = visitExpr(**dimPtr);
        if (ty == Type::Float) {
            floatLiteral = as<FloatExpr>(**dimPtr);
            if (floatLiteral != nullptr) {
                insertImplicitCast(**dimPtr, Type::Int);
                std::string msg = "narrowing conversion from FLOAT to INT in array size";
                de.emit(il::support::Severity::Warning, "B2002", d.loc, 1, std::move(msg));
            } else {
                std::string msg = "size type mismatch";
                de.emit(il::support::Severity::Error, "B2001", d.loc, 1, std::move(msg));
            }
        } else if (ty != Type::Unknown && ty != Type::Int) {
            std::string msg = "size type mismatch";
            de.emit(il::support::Severity::Error, "B2001", d.loc, 1, std::move(msg));
        }
        if (floatLiteral) {
            if (floatLiteral->value < 0.0) {
                std::string msg = "array size must be non-negative";
                de.emit(il::support::Severity::Error, "B2003", d.loc, 1, std::move(msg));
            }
        } else if (auto *ci = as<const IntExpr>(**dimPtr)) {
            sz = ci->value;
            if (sz < 0) {
                std::string msg = "array size must be non-negative";
                de.emit(il::support::Severity::Error, "B2003", d.loc, 1, std::move(msg));
            }
            extents.push_back(sz);
        } else {
            allConstant = false;
        }
    }

    resolveAndTrackSymbol(d.name, SymbolKind::Reference);

    auto itArray = arrays_.find(d.name);
    if (itArray == arrays_.end()) {
        de.emit(diag::BasicDiag::UnknownArray,
                d.loc,
                static_cast<uint32_t>(d.name.size()),
                std::initializer_list<diag::Replacement>{diag::Replacement{"name", d.name}});
        return;
    }

    if (auto itType = varTypes_.find(d.name);
        itType != varTypes_.end() && itType->second != Type::ArrayInt &&
        itType->second != Type::ArrayString && itType->second != Type::ArrayObject) {
        std::string msg = "REDIM target must be an array";
        de.emit(il::support::Severity::Error, "B2001", d.loc, 1, std::move(msg));
        return;
    }

    if (activeProcScope_) {
        activeProcScope_->noteArrayMutation(d.name, itArray->second);
    }
    if (allConstant && !extents.empty()) {
        long long totalSize = 1;
        bool overflow = false;
        for (long long extent : extents) {
            if (extent < 0 || extent == LLONG_MAX || totalSize > LLONG_MAX / (extent + 1)) {
                overflow = true;
                break;
            }
            totalSize *= (extent + 1);
        }
        arrays_[d.name] = overflow ? ArrayMetadata() : ArrayMetadata(std::move(extents), totalSize);
    } else {
        arrays_[d.name] = ArrayMetadata();
    }
}

/// @brief Visits both SWAP operands for nested semantic diagnostics.
/// @details This routine does not verify lvalue form or mutual type
///          compatibility; absent operands are skipped.
/// @param s Mutable SWAP statement.
void SemanticAnalyzer::analyzeSwap(SwapStmt &s) {
    if (s.lhs) {
        visitExpr(*s.lhs);
    }
    if (s.rhs) {
        visitExpr(*s.rhs);
    }
}

} // namespace il::frontends::basic
