//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file Lowerer_Expr.cpp
/// @brief Dispatches Zia expressions and lowers identifiers, ternaries, and
///        value-producing `if` expressions.
///
/// @details Every expression produces an IL value paired with its precise IL
///          representation. Identifier resolution spans local SSA bindings,
///          slots, implicit fields, globals, properties, and function
///          addresses. Conditional expressions move managed branch results
///          through an owning merge slot so exactly one reachable value is
///          transferred to the enclosing expression.
///
//===----------------------------------------------------------------------===//

#include "frontends/zia/Lowerer.hpp"
#include "frontends/zia/RuntimeNames.hpp"
#include "frontends/zia/ZiaLocationScope.hpp"

namespace il::frontends::zia {

using namespace runtime;

//=============================================================================
// Expression Lowering Dispatcher
//=============================================================================

/// @brief Central expression dispatcher: lower any AST expression to an IL value.
/// @param expr Expression to lower; null yields a poison value.
/// @return The lowered value and its IL type.
/// @details Guards against runaway recursion (kMaxLowerDepth, 512) and installs a location
///          scope for diagnostics, then switches on the expression kind to the matching
///          `lower*` routine. Unknown kinds produce a poison value rather than crashing.
LowerResult Lowerer::lowerExpr(Expr *expr) {
    if (!expr)
        return poisonValue({}, "V-ZIA-LOWER-NULL-EXPR", "null expression reached lowering");

    if (++exprLowerDepth_ > kMaxLowerDepth) {
        --exprLowerDepth_;
        reportLoweringInvariant(expr->loc,
                                "V-ZIA-LOWER-DEPTH",
                                "expression nesting too deep during lowering (limit: 512)");
        return {Value::constInt(0), Type(Type::Kind::I64)};
    }

    struct DepthGuard {
        unsigned &d;

        /// @brief Restore the expression-recursion counter on every exit path.
        ~DepthGuard() {
            --d;
        }
    } exprGuard_{exprLowerDepth_};

    ZiaLocationScope locScope(*this, expr->loc);

    switch (expr->kind) {
        case ExprKind::IntLiteral:
            return lowerIntLiteral(static_cast<IntLiteralExpr *>(expr));
        case ExprKind::NumberLiteral:
            return lowerNumberLiteral(static_cast<NumberLiteralExpr *>(expr));
        case ExprKind::StringLiteral:
            return lowerStringLiteral(static_cast<StringLiteralExpr *>(expr));
        case ExprKind::BoolLiteral:
            return lowerBoolLiteral(static_cast<BoolLiteralExpr *>(expr));
        case ExprKind::NullLiteral:
            return lowerNullLiteral(static_cast<NullLiteralExpr *>(expr));
        case ExprKind::UnitLiteral:
            return lowerUnitLiteral(static_cast<UnitLiteralExpr *>(expr));
        case ExprKind::Ident:
            return lowerIdent(static_cast<IdentExpr *>(expr));
        case ExprKind::SelfExpr: {
            Value selfPtr;
            if (getSelfPtr(selfPtr)) {
                return {selfPtr, Type(Type::Kind::Ptr)};
            }
            return {Value::constInt(0), Type(Type::Kind::Ptr)};
        }
        case ExprKind::SuperExpr: {
            // Super returns self pointer but is used for dispatching to parent methods
            Value selfPtr;
            if (getSelfPtr(selfPtr)) {
                return {selfPtr, Type(Type::Kind::Ptr)};
            }
            return {Value::constInt(0), Type(Type::Kind::Ptr)};
        }
        case ExprKind::Binary:
            return lowerBinary(static_cast<BinaryExpr *>(expr));
        case ExprKind::Unary:
            return lowerUnary(static_cast<UnaryExpr *>(expr));
        case ExprKind::Ternary:
            return lowerTernary(static_cast<TernaryExpr *>(expr));
        case ExprKind::If:
            return lowerIfExpr(static_cast<IfExpr *>(expr));
        case ExprKind::StructLiteral:
            return lowerStructLiteral(static_cast<StructLiteralExpr *>(expr));
        case ExprKind::Call:
            return lowerCall(static_cast<CallExpr *>(expr));
        case ExprKind::Field:
            return lowerField(static_cast<FieldExpr *>(expr));
        case ExprKind::New:
            return lowerNew(static_cast<NewExpr *>(expr));
        case ExprKind::Coalesce:
            return lowerCoalesce(static_cast<CoalesceExpr *>(expr));
        case ExprKind::OptionalChain:
            return lowerOptionalChain(static_cast<OptionalChainExpr *>(expr));
        case ExprKind::ListLiteral:
            return lowerListLiteral(static_cast<ListLiteralExpr *>(expr));
        case ExprKind::MapLiteral:
            return lowerMapLiteral(static_cast<MapLiteralExpr *>(expr));
        case ExprKind::Index:
            return lowerIndex(static_cast<IndexExpr *>(expr));
        case ExprKind::Try:
            return lowerTry(static_cast<TryExpr *>(expr));
        case ExprKind::ForceUnwrap:
            return lowerForceUnwrap(static_cast<ForceUnwrapExpr *>(expr));
        case ExprKind::Await:
            return lowerAwait(static_cast<AwaitExpr *>(expr));
        case ExprKind::Lambda:
            return lowerLambda(static_cast<LambdaExpr *>(expr));
        case ExprKind::Tuple:
            return lowerTuple(static_cast<TupleExpr *>(expr));
        case ExprKind::TupleIndex:
            return lowerTupleIndex(static_cast<TupleIndexExpr *>(expr));
        case ExprKind::Block:
            return lowerBlockExpr(static_cast<BlockExpr *>(expr));
        case ExprKind::Match:
            return lowerMatchExpr(static_cast<MatchExpr *>(expr));
        case ExprKind::As:
            return lowerAs(static_cast<AsExpr *>(expr));
        case ExprKind::Is:
            return lowerIsExpr(static_cast<IsExpr *>(expr));
        case ExprKind::Range:
            return lowerRange(static_cast<RangeExpr *>(expr));
        case ExprKind::SetLiteral:
            return lowerSetLiteral(static_cast<SetLiteralExpr *>(expr));
        default:
            return poisonValue(
                expr->loc, "V-ZIA-LOWER-UNKNOWN-EXPR", "unknown expression kind reached lowering");
    }
}

//=============================================================================
// Identifier Expression Lowering
//=============================================================================

/// @brief Lower an identifier reference to its current IL value.
/// @param expr Identifier expression.
/// @return The loaded value and IL type, or a diagnostic + placeholder if unresolved.
/// @details Resolves the name in priority order: slot-based mutable local, plain local,
///          implicit `self.field` inside a struct/class method, module-level constant,
///          module-level variable, auto-evaluated property getter, and finally a defined or
///          extern function (yielding its address for use as a function pointer). Optional
///          storage that is used at its inner type is unwrapped here. An unknown identifier is
///          reported (V3000).
LowerResult Lowerer::lowerIdent(IdentExpr *expr) {
    std::string resolvedName = sema_.resolvedIdentifierName(expr);
    if (resolvedName.empty())
        resolvedName = expr->name;

    auto semaType = sema_.typeOf(expr);
    /// @brief Prefers a concrete semantic identifier type over a fallback.
    /// @param fallback Type inferred from storage or declaration metadata.
    /// @return Concrete semantic type when available.
    auto resolveIdentType = [&](TypeRef fallback) -> TypeRef {
        if (semaType && semaType->kind != TypeKindSem::Unknown &&
            semaType->kind != TypeKindSem::Any)
            return semaType;
        return fallback;
    };
    /// @brief Adapts a stored identifier value to its semantic use type.
    /// @param storedValue IL value loaded or retrieved from storage.
    /// @param storageType Declared storage type.
    /// @param useType Semantic type required at this expression.
    /// @return Lowered value, unwrapping an optional when required.
    auto lowerStoredValue =
        [&](Value storedValue, TypeRef storageType, TypeRef useType) -> LowerResult {
        if (storageType && storageType->kind == TypeKindSem::Optional && useType &&
            useType->kind != TypeKindSem::Optional) {
            TypeRef innerType = storageType->innerType();
            if (innerType && innerType->equals(*useType))
                return emitOptionalUnwrap(storedValue, innerType);
        }

        Type resultType = mapType(useType ? useType : storageType);
        return {storedValue, resultType};
    };

    // Check for slot-based mutable variables first (e.g., loop variables)
    auto slotIt = slots_.find(expr->name);
    if (slotIt != slots_.end()) {
        auto localTypeIt = localTypes_.find(expr->name);
        TypeRef storageType = (localTypeIt != localTypes_.end()) ? localTypeIt->second : nullptr;
        TypeRef useType = resolveIdentType(storageType);
        Type loadType = mapType(storageType ? storageType : useType);
        Value loaded = loadFromSlot(expr->name, loadType);
        return lowerStoredValue(loaded, storageType, useType);
    }

    Value *local = lookupLocal(expr->name);
    if (local) {
        auto localTypeIt = localTypes_.find(expr->name);
        TypeRef storageType = (localTypeIt != localTypes_.end()) ? localTypeIt->second : nullptr;
        TypeRef useType = resolveIdentType(storageType);
        return lowerStoredValue(*local, storageType, useType);
    }

    // Check for implicit field access (self.field) inside a struct type method
    if (currentStructType_) {
        const FieldLayout *field = currentStructType_->findField(expr->name);
        if (field) {
            Value selfPtr;
            if (getSelfPtr(selfPtr)) {
                Value loaded = emitFieldLoad(field, selfPtr);
                return {loaded, mapType(field->type)};
            }
        }
    }

    // Check for implicit field access (self.field) inside an class method
    if (currentClassType_) {
        const FieldLayout *field = currentClassType_->findField(expr->name);
        if (field) {
            Value selfPtr;
            if (getSelfPtr(selfPtr)) {
                Value loaded = emitFieldLoad(field, selfPtr);
                return {loaded, mapType(field->type)};
            }
        }
    }

    // Check for global constants (module-level const declarations)
    auto constIt = globalConstants_.find(resolvedName);
    if (constIt != globalConstants_.end()) {
        const Value &val = constIt->second;
        // Determine the type from the value kind
        Type ilType;
        switch (val.kind) {
            case Value::Kind::ConstFloat:
                ilType = Type(Type::Kind::F64);
                break;
            case Value::Kind::ConstStr: {
                // String constants need to emit a const_str instruction to load the global
                // The stored value's str field contains the global label (e.g., ".L10")
                Value loaded = emitConstStr(val.str);
                return {loaded, Type(Type::Kind::Str)};
            }
            case Value::Kind::GlobalAddr:
                ilType = Type(Type::Kind::Str);
                break;
            case Value::Kind::ConstInt:
                // Check if it's a boolean (i1) or integer (i64)
                ilType = val.isBool ? Type(Type::Kind::I1) : Type(Type::Kind::I64);
                break;
            default:
                ilType = Type(Type::Kind::I64);
                break;
        }
        return {val, ilType};
    }

    // Check for global mutable variables (module-level var declarations)
    auto globalIt = globalVariables_.find(resolvedName);
    if (globalIt != globalVariables_.end()) {
        TypeRef storageType = globalIt->second;
        TypeRef useType = resolveIdentType(storageType);
        Type loadType = mapType(storageType ? storageType : useType);
        Value addr = getGlobalVarAddr(resolvedName, storageType);
        Value loaded = emitLoad(addr, loadType);
        return lowerStoredValue(loaded, storageType, useType);
    }

    // Check for auto-evaluated property getters (e.g., Pi → call Zanna.Math.get_Pi())
    std::string autoGetter = sema_.autoEvalGetter(expr);
    if (!autoGetter.empty()) {
        TypeRef type = sema_.typeOf(expr);
        Type ilType = mapType(type);
        Value result = emitCallRet(ilType, autoGetter, {});
        return {result, ilType};
    }

    // Check if identifier refers to a function - return its address for function pointers
    // This enables passing functions to Thread.Start, callbacks, etc.
    std::string mangledName = mangleFunctionName(resolvedName);
    if (definedFunctions_.find(mangledName) != definedFunctions_.end()) {
        // Function is defined in this module - return its address
        return {Value::global(mangledName), Type(Type::Kind::Ptr)};
    }

    // Check if it's an extern function (runtime API)
    Symbol *sym = sema_.findExternFunction(resolvedName);
    if (sym) {
        // External function reference - return its address
        return {Value::global(resolvedName), Type(Type::Kind::Ptr)};
    }

    diag_.report({il::support::Severity::Error,
                  "Unknown identifier '" + expr->name + "' reached lowering",
                  expr->loc,
                  "V3000"});
    return {Value::constInt(0), Type(Type::Kind::I64)};
}

//=============================================================================
// Ternary Expression Lowering
//=============================================================================

/// @brief Lower a ternary `cond ? then : else` expression.
/// @param expr Ternary expression.
/// @return The selected branch value and its IL type (void placeholder for void results).
/// @details Allocates a result slot, branches into then/else blocks that each store their
///          value, and reloads at the merge block. When the result type is optional but a
///          branch produced a non-optional value, that branch is wrapped via
///          emitOptionalWrap(). Reference-typed results are scheduled for deferred release.
LowerResult Lowerer::lowerTernary(TernaryExpr *expr) {
    const size_t conditionReleaseMark = deferredTemps_.size();
    auto cond = lowerExpr(expr->condition.get());
    TypeRef resultType = sema_.typeOf(expr);
    Type ilResultType = mapType(resultType);
    bool expectsOptional = resultType && resultType->kind == TypeKindSem::Optional;
    TypeRef optionalInner = expectsOptional ? resultType->innerType() : nullptr;

    // Allocate a stack slot for the result before branching.
    unsigned allocaId = nextTempId();
    il::core::Instr allocaInstr;
    allocaInstr.result = allocaId;
    allocaInstr.op = Opcode::Alloca;
    allocaInstr.type = Type(Type::Kind::Ptr);
    allocaInstr.operands = {Value::constInt(static_cast<long long>(kMachineWordSize))};
    allocaInstr.loc = curLoc_;
    blockMgr_.currentBlock()->instructions.push_back(allocaInstr);
    Value resultSlot = Value::temp(allocaId);

    size_t thenIdx = createBlock("ternary_then");
    size_t elseIdx = createBlock("ternary_else");
    size_t mergeIdx = createBlock("ternary_merge");

    releaseDeferredTempsFrom(conditionReleaseMark);
    emitCBr(cond.value, thenIdx, elseIdx);

    setBlock(thenIdx);
    {
        const size_t branchReleaseMark = deferredTemps_.size();
        auto thenResult = lowerExpr(expr->thenExpr.get());
        Value thenValue = thenResult.value;
        if (expectsOptional) {
            TypeRef thenType = sema_.typeOf(expr->thenExpr.get());
            if (!thenType || thenType->kind != TypeKindSem::Optional) {
                if (optionalInner)
                    thenValue = emitOptionalWrap(thenResult.value, optionalInner);
            }
        }
        if (ilResultType.kind != Type::Kind::Void) {
            if (needsRelease(resultType))
                emitInlineValueStore(resultType, resultSlot, thenValue, /*destInitialized=*/false);
            else {
                emitStore(resultSlot, thenValue, ilResultType);
                consumeDeferred(thenValue);
            }
        }
        releaseDeferredTempsFrom(branchReleaseMark);
    }
    emitBr(mergeIdx);

    setBlock(elseIdx);
    {
        const size_t branchReleaseMark = deferredTemps_.size();
        auto elseResult = lowerExpr(expr->elseExpr.get());
        Value elseValue = elseResult.value;
        if (expectsOptional) {
            TypeRef elseType = sema_.typeOf(expr->elseExpr.get());
            if (!elseType || elseType->kind != TypeKindSem::Optional) {
                if (optionalInner)
                    elseValue = emitOptionalWrap(elseResult.value, optionalInner);
            }
        }
        if (ilResultType.kind != Type::Kind::Void) {
            if (needsRelease(resultType))
                emitInlineValueStore(resultType, resultSlot, elseValue, /*destInitialized=*/false);
            else {
                emitStore(resultSlot, elseValue, ilResultType);
                consumeDeferred(elseValue);
            }
        }
        releaseDeferredTempsFrom(branchReleaseMark);
    }
    emitBr(mergeIdx);

    setBlock(mergeIdx);
    if (ilResultType.kind == Type::Kind::Void)
        return {Value::constInt(0), Type(Type::Kind::Void)};

    const bool managedResult = needsRelease(resultType);
    Value resultValue = managedResult ? takeManagedValueFromSlot(resultSlot, ilResultType)
                                      : emitLoad(resultSlot, ilResultType);
    if (managedResult)
        deferRelease(resultValue, isStringType(resultType));

    return {resultValue, ilResultType};
}

//=============================================================================
// If-Expression Lowering
//=============================================================================

/// @brief Lower an `if`/`else` used as a value-producing expression.
/// @param expr If-expression with then/else branches.
/// @return The selected branch value and its IL type (void placeholder for void results).
/// @details Structurally identical to lowerTernary(): a result slot plus then/else/merge
///          blocks, optional-wrapping of a non-optional branch when the result type is
///          optional, and deferred release for reference-typed results.
LowerResult Lowerer::lowerIfExpr(IfExpr *expr) {
    const size_t conditionReleaseMark = deferredTemps_.size();
    auto cond = lowerExpr(expr->condition.get());
    TypeRef resultType = sema_.typeOf(expr);
    Type ilResultType = mapType(resultType);
    bool expectsOptional = resultType && resultType->kind == TypeKindSem::Optional;
    TypeRef optionalInner = expectsOptional ? resultType->innerType() : nullptr;

    // Allocate a stack slot for the result before branching.
    unsigned allocaId = nextTempId();
    il::core::Instr allocaInstr;
    allocaInstr.result = allocaId;
    allocaInstr.op = Opcode::Alloca;
    allocaInstr.type = Type(Type::Kind::Ptr);
    allocaInstr.operands = {Value::constInt(static_cast<long long>(kMachineWordSize))};
    allocaInstr.loc = curLoc_;
    blockMgr_.currentBlock()->instructions.push_back(allocaInstr);
    Value resultSlot = Value::temp(allocaId);

    size_t thenIdx = createBlock("ifexpr_then");
    size_t elseIdx = createBlock("ifexpr_else");
    size_t mergeIdx = createBlock("ifexpr_merge");

    releaseDeferredTempsFrom(conditionReleaseMark);
    emitCBr(cond.value, thenIdx, elseIdx);

    setBlock(thenIdx);
    {
        const size_t branchReleaseMark = deferredTemps_.size();
        auto thenResult = lowerExpr(expr->thenBranch.get());
        Value thenValue = thenResult.value;
        if (expectsOptional) {
            TypeRef thenType = sema_.typeOf(expr->thenBranch.get());
            if (!thenType || thenType->kind != TypeKindSem::Optional) {
                if (optionalInner)
                    thenValue = emitOptionalWrap(thenResult.value, optionalInner);
            }
        }
        if (ilResultType.kind != Type::Kind::Void) {
            if (needsRelease(resultType))
                emitInlineValueStore(resultType, resultSlot, thenValue, /*destInitialized=*/false);
            else {
                emitStore(resultSlot, thenValue, ilResultType);
                consumeDeferred(thenValue);
            }
        }
        releaseDeferredTempsFrom(branchReleaseMark);
    }
    emitBr(mergeIdx);

    setBlock(elseIdx);
    {
        const size_t branchReleaseMark = deferredTemps_.size();
        auto elseResult = lowerExpr(expr->elseBranch.get());
        Value elseValue = elseResult.value;
        if (expectsOptional) {
            TypeRef elseType = sema_.typeOf(expr->elseBranch.get());
            if (!elseType || elseType->kind != TypeKindSem::Optional) {
                if (optionalInner)
                    elseValue = emitOptionalWrap(elseResult.value, optionalInner);
            }
        }
        if (ilResultType.kind != Type::Kind::Void) {
            if (needsRelease(resultType))
                emitInlineValueStore(resultType, resultSlot, elseValue, /*destInitialized=*/false);
            else {
                emitStore(resultSlot, elseValue, ilResultType);
                consumeDeferred(elseValue);
            }
        }
        releaseDeferredTempsFrom(branchReleaseMark);
    }
    emitBr(mergeIdx);

    setBlock(mergeIdx);
    if (ilResultType.kind == Type::Kind::Void)
        return {Value::constInt(0), Type(Type::Kind::Void)};

    const bool managedResult = needsRelease(resultType);
    Value resultValue = managedResult ? takeManagedValueFromSlot(resultSlot, ilResultType)
                                      : emitLoad(resultSlot, ilResultType);
    if (managedResult)
        deferRelease(resultValue, isStringType(resultType));

    return {resultValue, ilResultType};
}

} // namespace il::frontends::zia
