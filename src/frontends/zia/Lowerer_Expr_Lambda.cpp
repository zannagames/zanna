//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file Lowerer_Expr_Lambda.cpp
/// @brief Lowers lambdas, block expressions, casts, type tests, and struct
///        literals.
///
/// @details Lambdas become uniquely named module functions plus a uniform
///          two-pointer closure containing the function and captured
///          environment. Nested function emission saves and restores the
///          enclosing lowering context. This file also implements lexical
///          block cleanup, representation-correct `as` conversions, and
///          runtime-aware `is` checks for class and interface hierarchies.
///
//===----------------------------------------------------------------------===//

#include "frontends/zia/Lowerer.hpp"
#include "frontends/zia/RuntimeNames.hpp"
#include "il/runtime/classes/RuntimeClasses.hpp"

#include <algorithm>
#include <functional>
#include <string_view>
#include <utility>

namespace il::frontends::zia {

using namespace runtime;

/// @brief Total byte size of `[funcPtr, envPtr]` closure storage.
static constexpr int kClosureSize = 16;
/// @brief Byte offset of the environment pointer within closure storage.
static constexpr int kClosureEnvOffset = 8;

//=============================================================================
// Lambda Expression Lowering
//=============================================================================

/// @brief Lower a lambda expression to a top-level function plus a heap closure value.
/// @param expr Lambda expression.
/// @return A pointer to the closure struct `{ funcPtr, envPtr }`.
/// @details Emits a uniquely named function `__lambda_N` whose first parameter is the
///          environment pointer (uniform closure ABI), capturing referenced variables by value
///          into an aligned env struct. The enclosing function/block/temp context is saved
///          before lowering the body and restored afterward (using a function index, since the
///          functions vector may reallocate). No-capture lambdas use a null env pointer.
LowerResult Lowerer::lowerLambda(LambdaExpr *expr) {
    // Generate unique lambda function name (per-instance counter, not static)
    std::string lambdaName = "__lambda_" + std::to_string(lambdaCounter_++);

    // Check if lambda has captured variables
    bool hasCaptures = !expr->captures.empty();

    // Determine return type (inferred as the body's type if not specified)
    TypeRef returnType = types::unknown();
    if (expr->returnType) {
        returnType = sema_.resolveType(expr->returnType.get());
    } else {
        returnType = sema_.typeOf(expr->body.get());
    }
    Type ilReturnType = mapType(returnType);

    // The lambda's full function type carries any parameter types that Sema
    // inferred from context (target-typed lambdas), which the AST param nodes
    // lack when the source omitted the annotation.
    TypeRef lambdaFnType = sema_.typeOf(expr);
    /// @brief Retrieves a context-inferred lambda parameter type.
    /// @param i Parameter index.
    /// @return Inferred type, or `unknown` when unavailable.
    auto inferredParamType = [&](size_t i) -> TypeRef {
        if (lambdaFnType && lambdaFnType->kind == TypeKindSem::Function) {
            const auto &fnParams = lambdaFnType->paramTypes();
            if (i < fnParams.size() && fnParams[i])
                return fnParams[i];
        }
        return types::unknown();
    };

    // Build parameter list - always add env pointer as first param for uniform closure ABI
    std::vector<il::core::Param> params;
    params.reserve(expr->params.size() + 1);
    params.push_back({"__env", Type(Type::Kind::Ptr)});
    for (size_t i = 0; i < expr->params.size(); ++i) {
        const auto &param = expr->params[i];
        TypeRef paramType =
            param.type ? sema_.resolveType(param.type.get()) : inferredParamType(i);
        params.push_back({param.name, mapType(paramType)});
    }

    // Collect info about captured variables before switching contexts
    // We need to capture their current values/slot pointers
    struct CaptureInfo {
        std::string name;
        Value value;
        Type type;
        TypeRef semType;
        bool isSlot{false};
    };

    std::vector<CaptureInfo> captureInfos;
    if (hasCaptures) {
        for (const auto &cap : expr->captures) {
            CaptureInfo info;
            info.name = cap.name;
            info.isSlot = false;

            // Look up the variable's type - prefer localTypes_ (set during lowering)
            // over sema_.lookupVarType() which may fail due to scope mismatch
            auto localTypeIt = localTypes_.find(cap.name);
            TypeRef varType = (localTypeIt != localTypes_.end()) ? localTypeIt->second
                                                                 : sema_.lookupVarType(cap.name);

            // Look up the variable in current scope
            auto slotIt = slots_.find(cap.name);
            if (slotIt != slots_.end()) {
                // Load from slot to capture by value
                info.type = varType ? mapType(varType) : Type(Type::Kind::I64);
                info.semType = varType;
                info.value = loadFromSlot(cap.name, info.type);
                info.isSlot = true;
            } else {
                auto localIt = locals_.find(cap.name);
                if (localIt != locals_.end()) {
                    info.value = localIt->second;
                    info.type = varType ? mapType(varType) : Type(Type::Kind::I64);
                    info.semType = varType;
                } else {
                    // Not found - might be a global or error
                    info.value = Value::constInt(0);
                    info.type = Type(Type::Kind::I64);
                    info.semType = types::unknown();
                }
            }
            captureInfos.push_back(info);
        }
    }

    std::vector<size_t> captureOffsets(captureInfos.size(), 0);
    size_t envSize = 0;
    size_t envAlignment = 1;
    if (hasCaptures) {
        for (size_t i = 0; i < captureInfos.size(); ++i) {
            size_t alignment = getILTypeAlignment(captureInfos[i].type);
            envAlignment = std::max(envAlignment, alignment);
            envSize = alignTo(envSize, alignment);
            captureOffsets[i] = envSize;
            envSize += getILTypeSize(captureInfos[i].type);
        }
        envSize = alignTo(envSize, envAlignment);
    }

    // Save current function context (use index instead of pointer to handle vector reallocation)
    TypeRef savedReturnType = currentReturnType_;
    unsigned savedNextTemp = builder_->saveTempId();
    size_t savedFuncIdx = kInvalidIndex;
    if (currentFunc_) {
        for (size_t i = 0; i < module_->functions.size(); ++i) {
            if (&module_->functions[i] == currentFunc_) {
                savedFuncIdx = i;
                break;
            }
        }
    }
    size_t savedBlockIdx = blockMgr_.currentBlockIndex();
    unsigned savedNextBlockId = blockMgr_.nextBlockId();
    auto savedLocals = std::exchange(locals_, {});
    auto savedSlots = std::exchange(slots_, {});
    auto savedLocalTypes = std::exchange(localTypes_, {});
    auto savedDeferredTemps = std::exchange(deferredTemps_, {});

    // Create the lambda function and entry block via IRBuilder so param IDs are assigned.
    currentFunc_ = &builder_->startFunction(lambdaName, ilReturnType, params);
    currentReturnType_ = returnType;
    definedFunctions_.insert(lambdaName);

    blockMgr_.bind(builder_.get(), currentFunc_);

    // Create entry block with the lambda's params as block params.
    builder_->createBlock(*currentFunc_, "entry_0", currentFunc_->params);
    const size_t entryIdx = currentFunc_->blocks.size() - 1;
    setBlock(entryIdx);

    // Load captured variables from the environment struct if we have captures
    const auto &blockParams = currentFunc_->blocks[entryIdx].params;
    // First parameter is always __env (may be null for no-capture lambdas)
    if (hasCaptures) {
        Value envPtr = Value::temp(blockParams[0].id);

        // Load each captured variable from the environment
        for (size_t i = 0; i < captureInfos.size(); ++i) {
            const auto &info = captureInfos[i];

            // GEP to get field address within env struct
            Value fieldAddr = emitGEP(envPtr, static_cast<int64_t>(captureOffsets[i]));

            // Load the captured value
            Value capturedVal = emitLoad(fieldAddr, info.type);

            // Create a slot for mutable captured variables
            createSlot(info.name, info.type);
            storeToSlot(info.name, capturedVal, info.type);
            if (info.type.kind == Type::Kind::Str) // env value is borrowed; the slot owns +1
                emitCall(runtime::kStrRetainMaybe, {capturedVal});
            localTypes_[info.name] = info.semType ? info.semType : types::unknown();
        }
    }

    // Define user parameters as locals (skip __env at index 0)
    for (size_t i = 0; i < expr->params.size(); ++i) {
        size_t paramIdx = i + 1; // Skip __env
        if (paramIdx < blockParams.size()) {
            TypeRef paramType = expr->params[i].type
                                    ? sema_.resolveType(expr->params[i].type.get())
                                    : inferredParamType(i);
            Type ilParamType = mapType(paramType);
            createSlot(expr->params[i].name, ilParamType);
            storeToSlot(expr->params[i].name, Value::temp(blockParams[paramIdx].id), ilParamType);
            if (ilParamType.kind == Type::Kind::Str) // params are borrowed; the slot owns +1
                emitCall(runtime::kStrRetainMaybe, {Value::temp(blockParams[paramIdx].id)});
            localTypes_[expr->params[i].name] = paramType;
        }
    }

    // Lower the body - handle both block expressions and simple expressions
    LowerResult bodyResult{Value::constInt(0), Type(Type::Kind::Void)};
    if (auto *blockExpr = dynamic_cast<BlockExpr *>(expr->body.get())) {
        // Lower each statement in the block
        for (auto &stmt : blockExpr->statements) {
            lowerStmt(stmt.get());
        }
        // The block may have a final value expression
        if (blockExpr->value) {
            bodyResult = lowerExpr(blockExpr->value.get());
        }
    } else {
        bodyResult = lowerExpr(expr->body.get());
    }

    // Return the body result
    if (ilReturnType.kind == Type::Kind::Void) {
        if (!blockMgr_.isTerminated()) {
            releaseLocalStringSlots(); // capture/param slots own +1
            emitRetVoid();
        }
    } else {
        if (!blockMgr_.isTerminated()) {
            if (returnType && returnType->kind == TypeKindSem::Unit) {
                releaseLocalStringSlots();
                emitRet(Value::null());
            } else {
                Value returnValue = bodyResult.value;
                if (returnType && returnType->kind == TypeKindSem::Optional) {
                    TypeRef bodyType = sema_.typeOf(expr->body.get());
                    if (!bodyType || bodyType->kind != TypeKindSem::Optional) {
                        TypeRef innerType = returnType->innerType();
                        if (innerType)
                            returnValue = emitOptionalWrap(bodyResult.value, innerType);
                    }
                }
                if (ilReturnType.kind == Type::Kind::Str) {
                    // Mint the caller's reference when the value is borrowed
                    // (a slot load) — the slot releases just below.
                    if (!consumeDeferred(returnValue))
                        emitCall(runtime::kStrRetainMaybe, {returnValue});
                }
                releaseLocalStringSlots(); // capture/param slots own +1
                emitRet(returnValue);
            }
        }
    }

    // Restore context (use saved index to get fresh pointer after potential vector reallocation)
    if (savedFuncIdx != kInvalidIndex) {
        currentFunc_ = &module_->functions[savedFuncIdx];
        blockMgr_.reset(currentFunc_);
        blockMgr_.setNextBlockId(savedNextBlockId);
        blockMgr_.setBlock(savedBlockIdx);
        builder_->restoreTempId(savedNextTemp);
        builder_->restoreFunction(currentFunc_);
    } else {
        currentFunc_ = nullptr;
    }
    locals_ = std::move(savedLocals);
    slots_ = std::move(savedSlots);
    localTypes_ = std::move(savedLocalTypes);
    deferredTemps_ = std::move(savedDeferredTemps);
    currentReturnType_ = savedReturnType;

    // Get the function pointer
    Value funcPtr = Value::global(lambdaName);

    // Always create a uniform closure struct: { funcPtr, envPtr }
    // For no-capture lambdas, envPtr is null

    // Allocate environment if we have captures
    Value envPtr = Value::null(); // null for no captures
    if (hasCaptures) {
        // Allocate environment struct using rt_alloc
        Value envSizeVal = Value::constInt(static_cast<int64_t>(envSize));
        envPtr = emitCallRet(Type(Type::Kind::Ptr), "rt_alloc", {envSizeVal});

        // Store captured values into the environment
        for (size_t i = 0; i < captureInfos.size(); ++i) {
            const auto &info = captureInfos[i];
            Value fieldAddr = emitGEP(envPtr, static_cast<int64_t>(captureOffsets[i]));
            emitStore(fieldAddr, info.value, info.type);
        }
    }

    // Allocate closure struct: { ptr funcPtr, ptr envPtr } = 16 bytes
    Value closureSizeVal = Value::constInt(kClosureSize);
    Value closurePtr = emitCallRet(Type(Type::Kind::Ptr), "rt_alloc", {closureSizeVal});

    // Store function pointer at offset 0
    emitStore(closurePtr, funcPtr, Type(Type::Kind::Ptr));

    // Store environment pointer at closure env offset
    Value envFieldAddr = emitGEP(closurePtr, kClosureEnvOffset);
    emitStore(envFieldAddr, envPtr, Type(Type::Kind::Ptr));

    return {closurePtr, Type(Type::Kind::Ptr)};
}

//=============================================================================
// Block Expression Lowering
//=============================================================================

/// @brief Lower a block expression (`{ stmts...; value }`) to its trailing value.
/// @param expr Block expression.
/// @return The final value expression's result, or a null pointer for a value-less block.
/// @details Lowers statements until the block terminates, then the optional trailing value.
///          Scope cleanups registered within the block are emitted on normal exit, and the
///          enclosing local/slot/type maps are saved and restored so block-local bindings do
///          not leak.
LowerResult Lowerer::lowerBlockExpr(BlockExpr *expr) {
    auto localsBackup = locals_;
    auto slotsBackup = slots_;
    auto localTypesBackup = localTypes_;
    const size_t cleanupStart = cleanupStack_.size();

    for (auto &stmt : expr->statements) {
        if (isTerminated())
            break;
        lowerStmt(stmt.get());
    }

    LowerResult result{Value::null(), Type(Type::Kind::Ptr)};
    if (!isTerminated() && expr->value)
        result = lowerExpr(expr->value.get());

    if (!isTerminated())
        emitCleanupsFrom(cleanupStart);
    cleanupStack_.resize(cleanupStart);

    locals_ = std::move(localsBackup);
    slots_ = std::move(slotsBackup);
    localTypes_ = std::move(localTypesBackup);
    return result;
}

//=============================================================================
// As (Type Cast) Expression Lowering
//=============================================================================

/// @brief Lower an `as` type-cast expression.
/// @param expr Cast expression.
/// @return The converted value and its IL type.
/// @details Unwraps an optional source (trapping on null) before converting. Numeric casts use
///          real conversion opcodes — checked f64→i64, i64→f64 widening, checked i64→byte
///          narrowing, byte→i64 zero-extension — rather than bit reinterpretation. Assignable
///          types coerce directly; class/interface downcasts call `rt_cast_as` /
///          `rt_cast_as_iface` and trap if the runtime cast fails.
LowerResult Lowerer::lowerAs(AsExpr *expr) {
    // Lower the source value expression
    auto source = lowerExpr(expr->value.get());

    // Resolve the target type
    TypeRef targetType = sema_.resolveType(expr->type.get());
    Type ilTargetType = mapType(targetType);
    TypeRef sourceType = sema_.typeOf(expr->value.get());

    /// @brief Emits a null check and trap for a cast result.
    /// @param ptr Pointer value to test.
    /// @param ptrType IL type used to preserve the pointer bits.
    /// @param labelPrefix Prefix for generated control-flow labels.
    auto emitTrapIfNull = [&](Value ptr, Type ptrType, std::string_view labelPrefix) {
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
        storePtrInstr.type = ptrType;
        storePtrInstr.operands = {ptrSlot, ptr};
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

        size_t okIdx = createBlock(std::string(labelPrefix) + ".ok");
        size_t failIdx = createBlock(std::string(labelPrefix) + ".fail");
        emitCBr(isNotNull, okIdx, failIdx);

        setBlock(failIdx);
        il::core::Instr trapInstr;
        trapInstr.op = Opcode::Trap;
        trapInstr.type = Type(Type::Kind::Void);
        trapInstr.loc = curLoc_;
        blockMgr_.currentBlock()->instructions.push_back(trapInstr);
        blockMgr_.currentBlock()->terminated = true;

        setBlock(okIdx);
    };

    if (sourceType && sourceType->kind == TypeKindSem::Optional && sourceType->innerType()) {
        TypeRef innerType = sourceType->innerType();
        emitTrapIfNull(source.value, source.type, "as.unwrap");
        auto unwrapped = emitOptionalUnwrap(source.value, innerType);
        source = unwrapped;
        sourceType = innerType;
    }

    // Numeric conversions require actual IL conversion instructions to avoid
    // raw bit reinterpretation (e.g., f64 bits read as i64 -> garbage).
    if (sourceType && targetType) {
        if (sourceType->kind == TypeKindSem::Number && targetType->kind == TypeKindSem::Integer) {
            // f64 -> i64: checked truncation (traps on NaN/overflow)
            unsigned convId = nextTempId();
            il::core::Instr conv;
            conv.result = convId;
            conv.op = Opcode::CastFpToSiRteChk;
            conv.type = Type(Type::Kind::I64);
            conv.operands = {source.value};
            conv.loc = curLoc_;
            blockMgr_.currentBlock()->instructions.push_back(conv);
            return {Value::temp(convId), conv.type};
        }
        if (sourceType->kind == TypeKindSem::Integer && targetType->kind == TypeKindSem::Number) {
            // i64 -> f64: widening (may lose precision for values > 2^53)
            unsigned convId = nextTempId();
            il::core::Instr conv;
            conv.result = convId;
            conv.op = Opcode::Sitofp;
            conv.type = Type(Type::Kind::F64);
            conv.operands = {source.value};
            conv.loc = curLoc_;
            blockMgr_.currentBlock()->instructions.push_back(conv);
            return {Value::temp(convId), conv.type};
        }
        if (sourceType->kind == TypeKindSem::Integer && targetType->kind == TypeKindSem::Byte) {
            // i64 -> i32 (byte): checked narrowing (traps on overflow)
            unsigned convId = nextTempId();
            il::core::Instr conv;
            conv.result = convId;
            conv.op = Opcode::CastSiNarrowChk;
            conv.type = Type(Type::Kind::I32);
            conv.operands = {source.value};
            conv.loc = curLoc_;
            blockMgr_.currentBlock()->instructions.push_back(conv);
            return {Value::temp(convId), conv.type};
        }
        if (sourceType->kind == TypeKindSem::Byte && targetType->kind == TypeKindSem::Integer) {
            // i32 -> i64: zero-extend widening
            Value widened = widenByteToInteger(source.value);
            return {widened, Type(Type::Kind::I64)};
        }
    }

    // Narrowing to a runtime collection class is verified at runtime. The class
    // ids are exact (no hierarchy walk), so an unchecked `as` would let an
    // unrelated handle through and fail later inside an unrelated accessor with
    // a trap that names the wrong operation. Null narrows to null so nullable
    // runtime handles keep their `== null` guards. A source already statically
    // known to be that class needs no check.
    if (sourceType && targetType && targetType->kind == TypeKindSem::Ptr &&
        sourceType->name != targetType->name) {
        if (auto classId = il::runtime::runtimeCollectionClassId(targetType->name)) {
            Value checked = emitCallRet(Type(Type::Kind::Ptr),
                                        "rt_cast_runtime_class",
                                        {source.value, Value::constInt(*classId)});
            return {checked, ilTargetType};
        }
    }

    if (sourceType && targetType && targetType->isAssignableFrom(*sourceType))
        return coerceValueToType(source.value, source.type, sourceType, targetType);

    if (sourceType && targetType &&
        (sourceType->kind == TypeKindSem::Class || sourceType->kind == TypeKindSem::Interface) &&
        targetType->kind == TypeKindSem::Class) {
        if (const ClassTypeInfo *targetInfo = getOrCreateClassTypeInfo(targetType->name)) {
            Value casted = emitCallRet(
                Type(Type::Kind::Ptr),
                "rt_cast_as",
                {source.value, Value::constInt(static_cast<int64_t>(targetInfo->classId))});
            emitTrapIfNull(casted, Type(Type::Kind::Ptr), "as.cast");
            return {casted, ilTargetType};
        }
    }

    if (sourceType && targetType &&
        (sourceType->kind == TypeKindSem::Class || sourceType->kind == TypeKindSem::Interface) &&
        targetType->kind == TypeKindSem::Interface) {
        auto ifaceIt = interfaceTypes_.find(targetType->name);
        if (ifaceIt != interfaceTypes_.end()) {
            Value casted = emitCallRet(
                Type(Type::Kind::Ptr),
                "rt_cast_as_iface",
                {source.value, Value::constInt(static_cast<int64_t>(ifaceIt->second.ifaceId))});
            emitTrapIfNull(casted, Type(Type::Kind::Ptr), "as.iface");
            return {casted, ilTargetType};
        }
    }

    return coerceValueToType(source.value, source.type, sourceType, targetType);
}

//=============================================================================
// Is Expression Lowering
//=============================================================================

/// @brief Lower an `is` type-test expression to a boolean.
/// @param expr Is-expression.
/// @return An `i1` value: true when the source's runtime type matches the target.
/// @details For an optional source tested against its inner type, this is a non-null check.
///          Non-class/interface targets compare static types for equality. Interface targets
///          query `rt_type_implements`. Class targets compare the runtime class id against the
///          target and all of its descendant class ids (so `obj is Base` is true for subclass
///          instances), via a single compare or an OR-chain.
LowerResult Lowerer::lowerIsExpr(IsExpr *expr) {
    // Lower the value being tested
    auto source = lowerExpr(expr->value.get());

    // Resolve the target type name
    TypeRef targetType = sema_.resolveType(expr->type.get());
    if (!targetType) {
        return {Value::constBool(false), Type(Type::Kind::I1)};
    }

    TypeRef sourceType = sema_.typeOf(expr->value.get());
    if (sourceType && sourceType->kind == TypeKindSem::Optional && sourceType->innerType()) {
        TypeRef innerType = sourceType->innerType();
        if (targetType->equals(*innerType)) {
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
            storePtrInstr.type = source.type;
            storePtrInstr.operands = {ptrSlot, source.value};
            storePtrInstr.loc = curLoc_;
            blockMgr_.currentBlock()->instructions.push_back(storePtrInstr);

            Value ptrAsI64 = emitLoad(ptrSlot, Type(Type::Kind::I64));
            Value isNotNull =
                emitBinary(Opcode::ICmpNe, Type(Type::Kind::I1), ptrAsI64, Value::constInt(0));
            return {isNotNull, Type(Type::Kind::I1)};
        }
    }

    if (targetType->kind != TypeKindSem::Class && targetType->kind != TypeKindSem::Interface) {
        bool matches = sourceType && sourceType->equals(*targetType);
        return {Value::constBool(matches), Type(Type::Kind::I1)};
    }

    if (targetType->kind == TypeKindSem::Interface) {
        auto ifaceIt = interfaceTypes_.find(targetType->name);
        if (ifaceIt == interfaceTypes_.end())
            return {Value::constBool(false), Type(Type::Kind::I1)};
        Value typeId = emitCallRet(Type(Type::Kind::I64), "rt_typeid_of", {source.value});
        Value implements =
            emitCallRet(Type(Type::Kind::I64),
                        "rt_type_implements",
                        {typeId, Value::constInt(static_cast<int64_t>(ifaceIt->second.ifaceId))});
        Value result =
            emitBinary(Opcode::ICmpNe, Type(Type::Kind::I1), implements, Value::constInt(0));
        return {result, Type(Type::Kind::I1)};
    }

    // Look up the class type info for the target type
    std::string targetName = targetType->name;
    auto it = classTypes_.find(targetName);
    if (it == classTypes_.end()) {
        return {Value::constBool(false), Type(Type::Kind::I1)};
    }

    // Collect the target class ID and all descendant class IDs.
    // `obj is T` should return true when obj's runtime type is T or any
    // subclass of T (standard OOP semantics).
    std::vector<int64_t> matchIds;
    /// @brief Collects a class identifier and all descendant identifiers.
    /// @param name Registered class name.
    std::function<void(const std::string &)> collectDescendants = [&](const std::string &name) {
        auto cit = classTypes_.find(name);
        if (cit == classTypes_.end())
            return;
        matchIds.push_back(static_cast<int64_t>(cit->second.classId));
        for (const auto &[className, info] : classTypes_) {
            if (info.baseClass == name)
                collectDescendants(className);
        }
    };
    collectDescendants(targetName);

    // Emit: classId = call rt_obj_class_id(source)
    Value classId = emitCallRet(Type(Type::Kind::I64), "rt_obj_class_id", {source.value});

    if (matchIds.size() == 1) {
        // Common case: no subclasses — single comparison
        unsigned cmpId = nextTempId();
        il::core::Instr cmpInstr;
        cmpInstr.result = cmpId;
        cmpInstr.op = Opcode::ICmpEq;
        cmpInstr.type = Type(Type::Kind::I1);
        cmpInstr.operands = {classId, Value::constInt(matchIds[0])};
        cmpInstr.loc = curLoc_;
        blockMgr_.currentBlock()->instructions.push_back(cmpInstr);
        return {Value::temp(cmpId), Type(Type::Kind::I1)};
    }

    // Multiple classes: emit OR chain of comparisons on i1 values.
    // Zext each i1 comparison to i64, OR them, then trunc back to i1.
    Value accum;
    for (size_t i = 0; i < matchIds.size(); ++i) {
        unsigned cmpId = nextTempId();
        il::core::Instr cmpInstr;
        cmpInstr.result = cmpId;
        cmpInstr.op = Opcode::ICmpEq;
        cmpInstr.type = Type(Type::Kind::I1);
        cmpInstr.operands = {classId, Value::constInt(matchIds[i])};
        cmpInstr.loc = curLoc_;
        blockMgr_.currentBlock()->instructions.push_back(cmpInstr);

        // Zext i1 → i64 for the OR chain
        Value ext = emitUnary(Opcode::Zext1, Type(Type::Kind::I64), Value::temp(cmpId));

        if (i == 0) {
            accum = ext;
        } else {
            accum = emitBinary(Opcode::Or, Type(Type::Kind::I64), accum, ext);
        }
    }
    // Trunc i64 → i1 for boolean result
    Value result = emitUnary(Opcode::Trunc1, Type(Type::Kind::I1), accum);
    return {result, Type(Type::Kind::I1)};
}

} // namespace il::frontends::zia
