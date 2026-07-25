//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file Lowerer_Expr_Optional.cpp
/// @brief Lowers coalescing, optional chaining, propagation, force-unwrapping,
///        and awaiting expressions.
///
/// @details Nullable optional representations are tested by round-tripping
///          their pointer bits through integer-comparable storage. Coalescing
///          and chaining merge both paths through ownership-aware result
///          slots. Postfix propagation emits early returns for Optional and
///          Result failures, force-unwrapping traps on null, and `await`
///          materializes the resolved Future payload in its semantic
///          representation.
///
//===----------------------------------------------------------------------===//

#include "frontends/zia/Lowerer.hpp"
#include "frontends/zia/RuntimeNames.hpp"
#include "il/runtime/classes/RuntimeClasses.hpp"

namespace il::frontends::zia {

using namespace runtime;

namespace {

/// @brief True if @p name is one of the size/length property spellings
///        (Length/Len/Count/size, any case) treated as a count accessor.
/// @param name Property spelling to classify.
/// @return True for a recognized count/length alias.
bool isCountLikeProperty(const std::string &name) {
    return name == "Length" || name == "length" || name == "Len" || name == "Count" ||
           name == "count" || name == "size";
}

} // namespace

//=============================================================================
// Coalesce Expression Lowering
//=============================================================================

/// @brief Lower a null-coalescing expression (`left ?? right`).
/// @param expr Coalesce expression.
/// @return The unwrapped left value when present, otherwise the right value.
/// @details Optionals use a nullable-pointer representation, so the left value is stored and
///          reloaded as I64 for a null comparison. On the non-null path the (boxed) payload is
///          unwrapped; on the null path the right operand is evaluated. Both paths store into
///          a result slot reloaded at the merge block; reference results are deferred-released.
LowerResult Lowerer::lowerCoalesce(CoalesceExpr *expr) {
    // Get the type to determine how to handle the coalesce
    TypeRef leftType = sema_.typeOf(expr->left.get());
    TypeRef resultType = sema_.typeOf(expr);
    Type ilResultType = mapType(resultType);
    bool expectsOptional = resultType && resultType->kind == TypeKindSem::Optional;
    TypeRef optionalInner = expectsOptional ? resultType->innerType() : nullptr;
    (void)optionalInner;
    // Unwrap type comes from the left operand's optional inner type, not the result.
    // For nested coalescing (a ?? b) ?? c, the left may already be non-optional.
    bool leftIsOptional = leftType && leftType->kind == TypeKindSem::Optional;
    TypeRef innerType = leftIsOptional ? leftType->innerType() : nullptr;

    // Optionals use a nullable pointer representation. Primitive and struct
    // payloads are boxed before wrapping, so the null check is uniform.

    // Allocate a stack slot for the result BEFORE branching
    unsigned allocaId = nextTempId();
    il::core::Instr allocaInstr;
    allocaInstr.result = allocaId;
    allocaInstr.op = Opcode::Alloca;
    allocaInstr.type = Type(Type::Kind::Ptr);
    allocaInstr.operands = {Value::constInt(static_cast<long long>(kMachineWordSize))};
    allocaInstr.loc = curLoc_;
    blockMgr_.currentBlock()->instructions.push_back(allocaInstr);
    Value resultSlot = Value::temp(allocaId);

    // Lower the left expression. If it produces an owned optional, take that
    // reference out of statement cleanup so each outgoing edge can either
    // transfer or release it explicitly.
    const size_t leftReleaseMark = deferredTemps_.size();
    auto left = lowerExpr(expr->left.get());
    const bool leftOwned = consumeDeferred(left.value);

    // Create blocks for the coalesce
    size_t hasValueIdx = createBlock("coalesce_has");
    size_t isNullIdx = createBlock("coalesce_null");
    size_t mergeIdx = createBlock("coalesce_merge");

    // Check if it's null (for reference types, compare pointer to 0)
    // Note: ICmpNe requires i64 operands, so we convert the pointer via alloca/store/load
    unsigned ptrSlotId = nextTempId();
    il::core::Instr ptrSlotInstr;
    ptrSlotInstr.result = ptrSlotId;
    ptrSlotInstr.op = Opcode::Alloca;
    ptrSlotInstr.type = Type(Type::Kind::Ptr);
    ptrSlotInstr.operands = {Value::constInt(static_cast<long long>(kMachineWordSize))};
    ptrSlotInstr.loc = curLoc_;
    blockMgr_.currentBlock()->instructions.push_back(ptrSlotInstr);
    Value ptrSlot = Value::temp(ptrSlotId);

    il::core::Instr storePtrInstr;
    storePtrInstr.op = Opcode::Store;
    storePtrInstr.type = left.type;
    storePtrInstr.operands = {ptrSlot, left.value};
    storePtrInstr.loc = curLoc_;
    blockMgr_.currentBlock()->instructions.push_back(storePtrInstr);

    unsigned ptrAsI64Id = nextTempId();
    il::core::Instr loadAsI64Instr;
    loadAsI64Instr.result = ptrAsI64Id;
    loadAsI64Instr.op = Opcode::Load;
    loadAsI64Instr.type = Type(Type::Kind::I64);
    loadAsI64Instr.operands = {ptrSlot};
    loadAsI64Instr.loc = curLoc_;
    blockMgr_.currentBlock()->instructions.push_back(loadAsI64Instr);
    Value ptrAsI64 = Value::temp(ptrAsI64Id);

    Value isNotNull =
        emitBinary(Opcode::ICmpNe, Type(Type::Kind::I1), ptrAsI64, Value::constInt(0));
    releaseDeferredTempsFrom(leftReleaseMark);
    emitCBr(isNotNull, hasValueIdx, isNullIdx);

    // Has value block - store left value and branch to merge
    setBlock(hasValueIdx);
    {
        const size_t branchReleaseMark = deferredTemps_.size();
        Value unwrapped = left.value;
        if (innerType) {
            auto innerVal = emitOptionalUnwrap(left.value, innerType);
            unwrapped = innerVal.value;
        }
        const bool transfersLeft =
            leftOwned && needsRelease(resultType) && il::core::valueEquals(unwrapped, left.value);
        if (transfersLeft)
            deferRelease(unwrapped, isStringType(resultType));
        if (needsRelease(resultType))
            emitInlineValueStore(resultType, resultSlot, unwrapped, /*destInitialized=*/false);
        else {
            emitStore(resultSlot, unwrapped, ilResultType);
            consumeDeferred(unwrapped);
        }
        releaseDeferredTempsFrom(branchReleaseMark);
        if (leftOwned && !transfersLeft)
            emitManagedRelease(left.value, isStringType(leftType));
    }
    emitBr(mergeIdx);

    // Is null block - evaluate right, store, and branch to merge
    setBlock(isNullIdx);
    if (leftOwned)
        emitManagedRelease(left.value, isStringType(leftType));
    const size_t branchReleaseMark = deferredTemps_.size();
    auto right = lowerExpr(expr->right.get());
    {
        if (needsRelease(resultType))
            emitInlineValueStore(resultType, resultSlot, right.value, /*destInitialized=*/false);
        else {
            emitStore(resultSlot, right.value, ilResultType);
            consumeDeferred(right.value);
        }
        releaseDeferredTempsFrom(branchReleaseMark);
    }
    emitBr(mergeIdx);

    // Merge block - load the result
    setBlock(mergeIdx);
    const bool managedResult = needsRelease(resultType);
    Value resultValue = managedResult ? takeManagedValueFromSlot(resultSlot, ilResultType)
                                      : emitLoad(resultSlot, ilResultType);
    if (managedResult)
        deferRelease(resultValue, isStringType(resultType));

    return {resultValue, ilResultType};
}

//=============================================================================
// Optional Chain Expression Lowering
//=============================================================================

/// @brief Lower an optional-chaining field access (`base?.field`).
/// @param expr Optional-chain expression.
/// @return An optional-typed result: null when the base is null, else the field re-wrapped.
/// @details Null-checks the optional base; the null path stores null. The has-value path
///          unwraps the base and reads @p field — instance fields, struct/class properties,
///          built-in count/length accessors for list/map/set/string, or runtime-class getters
///          — then re-wraps the loaded value as optional. Both paths merge through a slot.
LowerResult Lowerer::lowerOptionalChain(OptionalChainExpr *expr) {
    const size_t baseReleaseMark = deferredTemps_.size();
    auto base = lowerExpr(expr->base.get());
    TypeRef baseType = sema_.typeOf(expr->base.get());
    if (!baseType || baseType->kind != TypeKindSem::Optional) {
        return {Value::null(), Type(Type::Kind::Ptr)};
    }

    TypeRef resultType = sema_.typeOf(expr);
    Type resultIlType = mapType(resultType);
    TypeRef innerType = baseType->innerType();
    TypeRef fieldType = types::unknown();
    const bool baseOwned = consumeDeferred(base.value);

    // Allocate a stack slot for the result (optional pointer)
    unsigned resultSlotId = nextTempId();
    il::core::Instr resultAlloca;
    resultAlloca.result = resultSlotId;
    resultAlloca.op = Opcode::Alloca;
    resultAlloca.type = Type(Type::Kind::Ptr);
    resultAlloca.operands = {Value::constInt(static_cast<long long>(kMachineWordSize))};
    resultAlloca.loc = curLoc_;
    blockMgr_.currentBlock()->instructions.push_back(resultAlloca);
    Value resultSlot = Value::temp(resultSlotId);

    // Compare optional pointer with null
    unsigned ptrSlotId = nextTempId();
    il::core::Instr ptrSlotInstr;
    ptrSlotInstr.result = ptrSlotId;
    ptrSlotInstr.op = Opcode::Alloca;
    ptrSlotInstr.type = Type(Type::Kind::Ptr);
    ptrSlotInstr.operands = {Value::constInt(static_cast<long long>(kMachineWordSize))};
    ptrSlotInstr.loc = curLoc_;
    blockMgr_.currentBlock()->instructions.push_back(ptrSlotInstr);
    Value ptrSlot = Value::temp(ptrSlotId);

    il::core::Instr storePtrInstr;
    storePtrInstr.op = Opcode::Store;
    storePtrInstr.type = base.type;
    storePtrInstr.operands = {ptrSlot, base.value};
    storePtrInstr.loc = curLoc_;
    blockMgr_.currentBlock()->instructions.push_back(storePtrInstr);

    unsigned ptrAsI64Id = nextTempId();
    il::core::Instr loadAsI64Instr;
    loadAsI64Instr.result = ptrAsI64Id;
    loadAsI64Instr.op = Opcode::Load;
    loadAsI64Instr.type = Type(Type::Kind::I64);
    loadAsI64Instr.operands = {ptrSlot};
    loadAsI64Instr.loc = curLoc_;
    blockMgr_.currentBlock()->instructions.push_back(loadAsI64Instr);
    Value ptrAsI64 = Value::temp(ptrAsI64Id);

    Value isNull = emitBinary(Opcode::ICmpEq, Type(Type::Kind::I1), ptrAsI64, Value::constInt(0));

    size_t hasValueIdx = createBlock("optchain_has");
    size_t isNullIdx = createBlock("optchain_null");
    size_t mergeIdx = createBlock("optchain_merge");
    releaseDeferredTempsFrom(baseReleaseMark);
    emitCBr(isNull, isNullIdx, hasValueIdx);

    // Null block
    setBlock(isNullIdx);
    il::core::Instr storeNull;
    storeNull.op = Opcode::Store;
    storeNull.type = resultIlType.kind == Type::Kind::Str ? Type(Type::Kind::Ptr) : resultIlType;
    storeNull.operands = {resultSlot, Value::null()};
    storeNull.loc = curLoc_;
    blockMgr_.currentBlock()->instructions.push_back(storeNull);
    if (baseOwned)
        emitManagedRelease(base.value, isStringType(baseType));
    emitBr(mergeIdx);

    // Has value block
    setBlock(hasValueIdx);
    const size_t branchReleaseMark = deferredTemps_.size();
    Value fieldValue = Value::null();
    if (innerType) {
        if (innerType->kind == TypeKindSem::Struct || innerType->kind == TypeKindSem::Class) {
            Value receiver = base.value;
            if (innerType->kind == TypeKindSem::Struct)
                receiver = emitOptionalUnwrap(base.value, innerType).value;

            const std::unordered_map<std::string, StructTypeInfo> &valueTypes = structTypes_;
            const std::unordered_map<std::string, ClassTypeInfo> &entityTypes = classTypes_;
            bool loadedField = false;
            if (innerType->kind == TypeKindSem::Struct) {
                auto it = valueTypes.find(innerType->name);
                if (it != valueTypes.end()) {
                    const FieldLayout *field = it->second.findField(expr->field);
                    if (field) {
                        fieldType = field->type;
                        fieldValue = emitFieldLoad(field, receiver);
                        loadedField = true;
                    }
                }
            } else {
                auto it = entityTypes.find(innerType->name);
                if (it != entityTypes.end()) {
                    const FieldLayout *field = it->second.findField(expr->field);
                    if (field) {
                        fieldType = field->type;
                        fieldValue = emitFieldLoad(field, receiver);
                        loadedField = true;
                    }
                }
            }

            if (!loadedField) {
                std::string declaringOwner;
                if (const PropertyDecl *prop = sema_.propertyDeclForLowering(
                        innerType->name, expr->field, &declaringOwner);
                    prop && prop->getterBody) {
                    fieldType = prop->type ? sema_.resolveType(prop->type.get()) : types::unknown();
                    Type ilFieldType = mapType(fieldType);
                    std::string getterName = declaringOwner + ".get_" + prop->name;
                    fieldValue = emitCallRet(ilFieldType, getterName, {receiver});
                }
            }
        } else if (innerType->kind == TypeKindSem::List) {
            if (isCountLikeProperty(expr->field)) {
                fieldType = types::integer();
                fieldValue = emitCallRet(Type(Type::Kind::I64), kListCount, {base.value});
            }
        } else if (innerType->kind == TypeKindSem::Map) {
            if (isCountLikeProperty(expr->field)) {
                fieldType = types::integer();
                fieldValue =
                    emitCallRet(Type(Type::Kind::I64),
                                usesIntegerMapRuntime(innerType) ? kIntMapCount : kMapCount,
                                {base.value});
            }
        } else if (innerType->kind == TypeKindSem::Set) {
            if (isCountLikeProperty(expr->field)) {
                fieldType = types::integer();
                fieldValue = emitCallRet(Type(Type::Kind::I64), kSetCount, {base.value});
            }
        } else if (innerType->kind == TypeKindSem::String) {
            if (expr->field == "Length" || expr->field == "length") {
                fieldType = types::integer();
                fieldValue = emitCallRet(Type(Type::Kind::I64), kStringLength, {base.value});
            }
        } else if (innerType->kind == TypeKindSem::Ptr && !innerType->name.empty()) {
            std::string getterName = innerType->name + ".get_" + expr->field;
            const auto &registry = il::runtime::RuntimeRegistry::instance();
            if (auto prop = registry.findProperty(innerType->name, expr->field);
                prop && prop->getter && *prop->getter) {
                getterName = prop->getter;
            }

            if (Symbol *getterSym = sema_.findExternFunction(getterName);
                getterSym && getterSym->type) {
                fieldType = getterSym->type;
                if (fieldType->kind == TypeKindSem::Function && fieldType->returnType())
                    fieldType = fieldType->returnType();
                Type ilFieldType = mapType(fieldType);
                fieldValue = emitCallRet(ilFieldType, getterName, {base.value});
            }
        }
    }

    Value optionalValue = Value::null();
    if (fieldType && fieldType->kind == TypeKindSem::Optional) {
        optionalValue = fieldValue;
    } else if (fieldType && fieldType->kind != TypeKindSem::Unknown) {
        optionalValue = emitOptionalWrap(fieldValue, fieldType);
    }

    if (needsRelease(resultType))
        emitInlineValueStore(resultType, resultSlot, optionalValue, /*destInitialized=*/false);
    else {
        emitStore(resultSlot, optionalValue, resultIlType);
        consumeDeferred(optionalValue);
    }
    releaseDeferredTempsFrom(branchReleaseMark);
    if (baseOwned)
        emitManagedRelease(base.value, isStringType(baseType));
    emitBr(mergeIdx);

    setBlock(mergeIdx);
    const bool managedResult = needsRelease(resultType);
    Value resultValue = managedResult ? takeManagedValueFromSlot(resultSlot, resultIlType)
                                      : emitLoad(resultSlot, resultIlType);
    if (managedResult)
        deferRelease(resultValue, isStringType(resultType));

    return {resultValue, resultIlType};
}

/// @brief Lower an optional-chaining method call (`base?.method(...)`).
/// @param callee The optional-chain callee carrying the receiver base.
/// @param expr The call expression supplying arguments.
/// @return An optional-typed result: null when the base is null, else the wrapped call result.
/// @details Null-checks the optional receiver; the null path stores null (and is a no-op for
///          void methods). The has-value path unwraps the receiver and dispatches through the
///          appropriate path — interface itable, class virtual dispatch, or a direct method
///          call — then wraps a non-void result as optional. Paths merge through a slot.
LowerResult Lowerer::lowerOptionalMethodCall(OptionalChainExpr *callee, CallExpr *expr) {
    const size_t baseReleaseMark = deferredTemps_.size();
    auto base = lowerExpr(callee->base.get());
    TypeRef baseType = sema_.typeOf(callee->base.get());
    if (!baseType || baseType->kind != TypeKindSem::Optional || !baseType->innerType()) {
        return {Value::null(), Type(Type::Kind::Ptr)};
    }

    MethodDecl *method = sema_.resolvedMethodDecl(expr);
    TypeRef receiverType = baseType->innerType();
    std::string ownerType = sema_.resolvedMethodOwnerType(expr);
    if (ownerType.empty() && receiverType)
        ownerType = receiverType->name;
    std::string slotKey = sema_.resolvedMethodSlotKey(expr);

    TypeRef methodType = method ? sema_.getMethodType(ownerType, method) : nullptr;
    TypeRef methodReturnType = methodType && methodType->kind == TypeKindSem::Function
                                   ? methodType->returnType()
                                   : types::voidType();
    TypeRef resultType = sema_.typeOf(expr);
    Type resultIlType = resultType ? mapType(resultType) : mapType(methodReturnType);
    bool returnsVoid = methodReturnType && methodReturnType->kind == TypeKindSem::Void;
    const bool baseOwned = consumeDeferred(base.value);

    Value resultSlot;
    if (!returnsVoid) {
        unsigned resultSlotId = nextTempId();
        il::core::Instr resultAlloca;
        resultAlloca.result = resultSlotId;
        resultAlloca.op = Opcode::Alloca;
        resultAlloca.type = Type(Type::Kind::Ptr);
        resultAlloca.operands = {Value::constInt(static_cast<long long>(kMachineWordSize))};
        resultAlloca.loc = curLoc_;
        blockMgr_.currentBlock()->instructions.push_back(resultAlloca);
        resultSlot = Value::temp(resultSlotId);
    }

    unsigned ptrSlotId = nextTempId();
    il::core::Instr ptrSlotInstr;
    ptrSlotInstr.result = ptrSlotId;
    ptrSlotInstr.op = Opcode::Alloca;
    ptrSlotInstr.type = Type(Type::Kind::Ptr);
    ptrSlotInstr.operands = {Value::constInt(static_cast<long long>(kMachineWordSize))};
    ptrSlotInstr.loc = curLoc_;
    blockMgr_.currentBlock()->instructions.push_back(ptrSlotInstr);
    Value ptrSlot = Value::temp(ptrSlotId);

    emitStore(ptrSlot, base.value, base.type);
    Value ptrAsI64 = emitLoad(ptrSlot, Type(Type::Kind::I64));
    Value isNull = emitBinary(Opcode::ICmpEq, Type(Type::Kind::I1), ptrAsI64, Value::constInt(0));

    size_t hasValueIdx = createBlock("optmethod_has");
    size_t isNullIdx = createBlock("optmethod_null");
    size_t mergeIdx = createBlock("optmethod_merge");
    releaseDeferredTempsFrom(baseReleaseMark);
    emitCBr(isNull, isNullIdx, hasValueIdx);

    setBlock(isNullIdx);
    if (!returnsVoid) {
        Type nullStoreType =
            resultIlType.kind == Type::Kind::Str ? Type(Type::Kind::Ptr) : resultIlType;
        emitStore(resultSlot, Value::null(), nullStoreType);
    }
    if (baseOwned)
        emitManagedRelease(base.value, isStringType(baseType));
    emitBr(mergeIdx);

    setBlock(hasValueIdx);
    const size_t branchReleaseMark = deferredTemps_.size();
    Value receiver = base.value;
    if (receiverType && receiverType->kind == TypeKindSem::Struct)
        receiver = emitOptionalUnwrap(base.value, receiverType).value;

    LowerResult callResult{Value::constInt(0), Type(Type::Kind::Void)};
    if (receiverType && receiverType->kind == TypeKindSem::Interface) {
        auto ifaceIt = interfaceTypes_.find(receiverType->name);
        if (ifaceIt != interfaceTypes_.end()) {
            callResult = lowerInterfaceMethodCall(
                ifaceIt->second, slotKey, ownerType, method, receiver, expr);
        }
    } else if (receiverType && receiverType->kind == TypeKindSem::Class && method &&
               !method->isStatic) {
        if (const ClassTypeInfo *entityInfo = getOrCreateClassTypeInfo(receiverType->name)) {
            callResult =
                lowerVirtualMethodCall(*entityInfo, slotKey, ownerType, method, receiver, expr);
        }
    } else {
        callResult = lowerMethodCall(method, ownerType, receiver, expr);
    }

    if (!returnsVoid) {
        Value optionalValue = methodReturnType && methodReturnType->kind == TypeKindSem::Optional
                                  ? callResult.value
                                  : emitOptionalWrap(callResult.value, methodReturnType);
        if (needsRelease(resultType))
            emitInlineValueStore(resultType, resultSlot, optionalValue, /*destInitialized=*/false);
        else {
            emitStore(resultSlot, optionalValue, resultIlType);
            consumeDeferred(optionalValue);
        }
    }
    releaseDeferredTempsFrom(branchReleaseMark);
    if (baseOwned)
        emitManagedRelease(base.value, isStringType(baseType));
    emitBr(mergeIdx);

    setBlock(mergeIdx);
    if (returnsVoid)
        return {Value::constInt(0), Type(Type::Kind::Void)};

    const bool managedResult = needsRelease(resultType);
    Value resultValue = managedResult ? takeManagedValueFromSlot(resultSlot, resultIlType)
                                      : emitLoad(resultSlot, resultIlType);
    if (managedResult)
        deferRelease(resultValue, isStringType(resultType));
    return {resultValue, resultIlType};
}

//=============================================================================
// Try Expression Lowering
//=============================================================================

/// @brief Lower a postfix `?` try/propagation expression.
/// @param expr Try expression.
/// @return The unwrapped success/inner value when present.
/// @details For a `Result` operand, branches on `IsOk`: the error path returns the Result from
///          the current function early; the ok path unwraps via the type-appropriate
///          `Zanna.Result.Unwrap*` helper. For an `Optional` operand, a null operand returns
///          early (null or void, matching the function's return type) and a present value is
///          unwrapped.
LowerResult Lowerer::lowerTry(TryExpr *expr) {
    auto operand = lowerExpr(expr->operand.get());
    TypeRef operandType = sema_.typeOf(expr->operand.get());

    if (operandType && operandType->kind == TypeKindSem::Result) {
        TypeRef successType =
            !operandType->typeArgs.empty() ? operandType->typeArgs[0] : types::unknown();

        auto resultUnwrapCallee = [](TypeRef type) -> const char * {
            if (!type)
                return kResultUnwrap;
            switch (type->kind) {
                case TypeKindSem::String:
                    return kResultUnwrapStr;
                case TypeKindSem::Integer:
                case TypeKindSem::Enum:
                    return kResultUnwrapI64;
                case TypeKindSem::Number:
                    return kResultUnwrapF64;
                default:
                    return kResultUnwrap;
            }
        };

        size_t okIdx = createBlock("try.result_ok");
        size_t errIdx = createBlock("try.result_err");
        Value isOk = emitCallRet(Type(Type::Kind::I1), kResultGetIsOk, {operand.value});
        emitCBr(isOk, okIdx, errIdx);

        setBlock(errIdx);
        emitRet(operand.value);

        setBlock(okIdx);
        Type ilSuccessType = mapType(successType);
        const char *callee = resultUnwrapCallee(successType);
        Type runtimeReturn =
            std::string_view(callee) == kResultUnwrap ? Type(Type::Kind::Ptr) : ilSuccessType;
        Value raw = emitCallRet(runtimeReturn, callee, {operand.value});
        if (runtimeReturn.kind == ilSuccessType.kind)
            return {raw, ilSuccessType};
        return emitUnboxValue(raw, ilSuccessType, successType);
    }

    // Create blocks for the null check
    size_t hasValueIdx = createBlock("try.hasvalue");
    size_t returnNullIdx = createBlock("try.returnnull");

    // Check if the value is null (comparing pointer as i64 to 0)
    // First, store the pointer and load as i64 for comparison
    unsigned ptrSlotId = nextTempId();
    il::core::Instr ptrSlotInstr;
    ptrSlotInstr.result = ptrSlotId;
    ptrSlotInstr.op = Opcode::Alloca;
    ptrSlotInstr.type = Type(Type::Kind::Ptr);
    ptrSlotInstr.operands = {Value::constInt(static_cast<long long>(kMachineWordSize))};
    ptrSlotInstr.loc = curLoc_;
    blockMgr_.currentBlock()->instructions.push_back(ptrSlotInstr);
    Value ptrSlot = Value::temp(ptrSlotId);

    il::core::Instr storePtrInstr;
    storePtrInstr.op = Opcode::Store;
    storePtrInstr.type = operand.type;
    storePtrInstr.operands = {ptrSlot, operand.value};
    storePtrInstr.loc = curLoc_;
    blockMgr_.currentBlock()->instructions.push_back(storePtrInstr);

    unsigned ptrAsI64Id = nextTempId();
    il::core::Instr loadAsI64Instr;
    loadAsI64Instr.result = ptrAsI64Id;
    loadAsI64Instr.op = Opcode::Load;
    loadAsI64Instr.type = Type(Type::Kind::I64);
    loadAsI64Instr.operands = {ptrSlot};
    loadAsI64Instr.loc = curLoc_;
    blockMgr_.currentBlock()->instructions.push_back(loadAsI64Instr);
    Value ptrAsI64 = Value::temp(ptrAsI64Id);

    Value isNotNull =
        emitBinary(Opcode::ICmpNe, Type(Type::Kind::I1), ptrAsI64, Value::constInt(0));
    emitCBr(isNotNull, hasValueIdx, returnNullIdx);

    // Return null block - return null from the current function
    setBlock(returnNullIdx);
    // For functions returning optional types, return null (0 as pointer)
    // For void functions, we just return void
    if (currentFunc_->retType.kind == Type::Kind::Void) {
        emitRetVoid();
    } else {
        // Return null for optional/pointer return types
        emitRet(Value::null());
    }

    // Has value block - continue with the unwrapped value
    setBlock(hasValueIdx);

    // Return the operand value (unwrap optionals when needed)
    if (operandType && operandType->kind == TypeKindSem::Optional) {
        TypeRef innerTypeRef = operandType->innerType();
        if (innerTypeRef)
            return emitOptionalUnwrap(operand.value, innerTypeRef);
    }
    return operand;
}

//=============================================================================
// Force-Unwrap Expression Lowering
//=============================================================================

/// @brief Lower a force-unwrap expression (`expr!`).
/// @param expr Force-unwrap expression.
/// @return The unwrapped inner value.
/// @details Null-checks the optional operand and traps if it is null; otherwise unwraps and
///          returns the inner value. Non-optional operands pass through unchanged (sema should
///          already have rejected those).
LowerResult Lowerer::lowerForceUnwrap(ForceUnwrapExpr *expr) {
    auto operand = lowerExpr(expr->operand.get());

    TypeRef operandType = sema_.typeOf(expr->operand.get());
    if (!operandType || operandType->kind != TypeKindSem::Optional) {
        // Sema should have caught this; fall through as identity
        return operand;
    }

    TypeRef innerType = operandType->innerType();
    if (!innerType)
        return operand;

    // Null check: store pointer, load as i64, compare != 0
    unsigned ptrSlotId = nextTempId();
    il::core::Instr ptrSlotInstr;
    ptrSlotInstr.result = ptrSlotId;
    ptrSlotInstr.op = Opcode::Alloca;
    ptrSlotInstr.type = Type(Type::Kind::Ptr);
    ptrSlotInstr.operands = {Value::constInt(static_cast<long long>(kMachineWordSize))};
    ptrSlotInstr.loc = curLoc_;
    blockMgr_.currentBlock()->instructions.push_back(ptrSlotInstr);
    Value ptrSlot = Value::temp(ptrSlotId);

    il::core::Instr storePtrInstr;
    storePtrInstr.op = Opcode::Store;
    storePtrInstr.type = operand.type;
    storePtrInstr.operands = {ptrSlot, operand.value};
    storePtrInstr.loc = curLoc_;
    blockMgr_.currentBlock()->instructions.push_back(storePtrInstr);

    unsigned ptrAsI64Id = nextTempId();
    il::core::Instr loadAsI64Instr;
    loadAsI64Instr.result = ptrAsI64Id;
    loadAsI64Instr.op = Opcode::Load;
    loadAsI64Instr.type = Type(Type::Kind::I64);
    loadAsI64Instr.operands = {ptrSlot};
    loadAsI64Instr.loc = curLoc_;
    blockMgr_.currentBlock()->instructions.push_back(loadAsI64Instr);
    Value ptrAsI64 = Value::temp(ptrAsI64Id);

    size_t unwrapOkIdx = createBlock("unwrap.ok");
    size_t unwrapFailIdx = createBlock("unwrap.fail");

    Value isNotNull =
        emitBinary(Opcode::ICmpNe, Type(Type::Kind::I1), ptrAsI64, Value::constInt(0));
    emitCBr(isNotNull, unwrapOkIdx, unwrapFailIdx);

    // Trap block -- abort if null
    setBlock(unwrapFailIdx);
    il::core::Instr trapInstr;
    trapInstr.op = Opcode::Trap;
    trapInstr.type = Type(Type::Kind::Void);
    trapInstr.loc = curLoc_;
    blockMgr_.currentBlock()->instructions.push_back(trapInstr);

    // Continue with unwrapped value
    setBlock(unwrapOkIdx);
    return emitOptionalUnwrap(operand.value, innerType);
}

//=============================================================================
// Await Expression Lowering
//=============================================================================

/// @brief Lower an `await` expression.
/// @param expr Await expression whose operand produces a future.
/// @return The resolved payload, unboxed to the awaited type.
/// @details Lowers the future operand and calls `Zanna.Threads.Future.Get`, which blocks until
///          the future resolves. Struct or non-pointer payloads are unboxed to their IL type;
///          pointer/any/void payloads are returned as-is.
LowerResult Lowerer::lowerAwait(AwaitExpr *expr) {
    // Lower the future-producing operand expression.
    auto futureResult = lowerExpr(expr->operand.get());

    // Emit call to Zanna.Threads.Future.Get(future) which blocks until resolved.
    Value result = emitCallRet(Type(Type::Kind::Ptr), runtime::kFutureGet, {futureResult.value});

    TypeRef awaitedType = sema_.typeOf(expr);
    if (!awaitedType || awaitedType->kind == TypeKindSem::Any ||
        awaitedType->kind == TypeKindSem::Unknown || awaitedType->kind == TypeKindSem::Void)
        return {result, Type(Type::Kind::Ptr)};

    Type ilType = mapType(awaitedType);
    if (awaitedType->kind == TypeKindSem::Struct || ilType.kind != Type::Kind::Ptr)
        return emitUnboxValue(result, ilType, awaitedType);
    return {result, ilType};
}

} // namespace il::frontends::zia
