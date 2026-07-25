//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file Lowerer_Emit.cpp
/// @brief Implements typed IL emission, representation conversion, aggregate
///        storage, and managed-value ownership helpers for the Zia lowerer.
///
/// @details Emitted values and instructions belong to the current builder
///          function. Managed temporaries are tracked until released or
///          transferred to an owner, while inline aggregate operations retain
///          copied managed fields and release displaced values. Deferred
///          ownership records are valid only along the control-flow edge on
///          which their values were produced.
///
//===----------------------------------------------------------------------===//

#include "frontends/zia/Lowerer.hpp"
#include "frontends/zia/RuntimeNames.hpp"
#include "il/runtime/RuntimeSignatures.hpp"
#include "support/alignment.hpp"
#include <algorithm>
#include <cctype>
#include <functional>
#include <utility>

namespace il::frontends::zia {

using namespace runtime;

namespace {

/// @brief True if @p type is stored inline by value (struct, fixed array, or
///        tuple) rather than behind a heap pointer — affects copy/load lowering.
bool isInlineAggregateType(TypeRef type) {
    return type && (type->kind == TypeKindSem::Struct || type->kind == TypeKindSem::FixedArray ||
                    type->kind == TypeKindSem::Tuple);
}

} // namespace

//=============================================================================
// Block Management
//=============================================================================

/// @brief Create a new basic block with a unique label derived from @p base.
/// @details Allocates a new block in the current function being lowered. The
///          block is not immediately set as the insertion point; use setBlock()
///          to begin emitting instructions into it.
/// @param base Base name for the block label (e.g., "then", "else", "loop").
/// @return Index of the newly created block.
size_t Lowerer::createBlock(const std::string &base) {
    return blockMgr_.createBlock(base);
}

/// @brief Set the current insertion point to the block at @p blockIdx.
/// @details All subsequent instruction emissions will append to this block
///          until setBlock() is called again with a different index.
/// @param blockIdx Index of the block to make current (from createBlock()).
void Lowerer::setBlock(size_t blockIdx) {
    blockMgr_.setBlock(blockIdx);
}

//=============================================================================
// Instruction Emission Helpers
//=============================================================================

/// @brief Emit a typed two-operand IL instruction.
/// @param op Binary opcode to emit.
/// @param ty Result type recorded on the instruction.
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @return Fresh temporary containing the instruction result.
Lowerer::Value Lowerer::emitBinary(Opcode op, Type ty, Value lhs, Value rhs) {
    unsigned id = nextTempId();
    il::core::Instr instr;
    instr.result = id;
    instr.op = op;
    instr.type = ty;
    instr.operands = {lhs, rhs};
    instr.loc = curLoc_;
    blockMgr_.currentBlock()->instructions.push_back(instr);
    return Value::temp(id);
}

/// @brief Emit a typed one-operand IL instruction.
/// @param op Unary opcode to emit.
/// @param ty Result type recorded on the instruction.
/// @param operand Input value.
/// @return Fresh temporary containing the instruction result.
Lowerer::Value Lowerer::emitUnary(Opcode op, Type ty, Value operand) {
    unsigned id = nextTempId();
    il::core::Instr instr;
    instr.result = id;
    instr.op = op;
    instr.type = ty;
    instr.operands = {operand};
    instr.loc = curLoc_;
    blockMgr_.currentBlock()->instructions.push_back(instr);
    return Value::temp(id);
}

/// @brief Zero-extend the low 32 bits of a byte-representation value to `i64`.
/// @param value Value represented as IL `i32`.
/// @return Fresh `i64` temporary containing the zero-extended value.
/// @details The conversion round-trips through zero-initialized stack storage
///          and masks the loaded word to make the upper bits deterministic.
Lowerer::Value Lowerer::widenByteToInteger(Value value) {
    // Zero-extend i32 to i64 via alloca/store/load pattern
    // Store i32 value, load as i64 (upper bits will be zero due to alloca zeroing)
    unsigned slotId = nextTempId();
    il::core::Instr allocaInstr;
    allocaInstr.result = slotId;
    allocaInstr.op = Opcode::Alloca;
    allocaInstr.type = Type(Type::Kind::Ptr);
    allocaInstr.operands = {Value::constInt(static_cast<long long>(kMachineWordSize))};
    allocaInstr.loc = curLoc_;
    blockMgr_.currentBlock()->instructions.push_back(allocaInstr);
    Value slot = Value::temp(slotId);

    // Store i32 value at offset 0
    emitStore(slot, value, Type(Type::Kind::I32));

    // Load as i64 - the upper 32 bits will be whatever was in memory (likely zero from alloca)
    // To ensure zeroing, we mask after loading
    Value loaded = emitLoad(slot, Type(Type::Kind::I64));
    return emitBinary(Opcode::And, Type(Type::Kind::I64), loaded, Value::constInt(0xFFFFFFFFLL));
}

/// @brief Widen a supported integral IL representation to `i64`.
/// @param value Integral value to widen.
/// @param valueType Current IL type of @p value.
/// @return Zero-extended result for `i32` or `i1`; @p value unchanged for
///         `i64` and unhandled kinds.
Lowerer::Value Lowerer::widenIntegralToI64(Value value, Type valueType) {
    switch (valueType.kind) {
        case Type::Kind::I64:
            return value;
        case Type::Kind::I32:
            return widenByteToInteger(value);
        case Type::Kind::I1:
            return emitUnary(Opcode::Zext1, Type(Type::Kind::I64), value);
        default:
            return value;
    }
}

/// @brief Return whether a semantic map should lower to the IntMap runtime ABI.
/// @param mapType Semantic type of the map receiver.
/// @return True for `Map[Integer, T]`; false for all other receiver types.
bool Lowerer::usesIntegerMapRuntime(TypeRef mapType) const {
    if (!mapType || mapType->kind != TypeKindSem::Map)
        return false;
    TypeRef keyType = mapType->keyType();
    return keyType && keyType->kind == TypeKindSem::Integer;
}

/// @brief Convert a map key value to the runtime representation used by its backing store.
/// @param keyValue Lowered key value.
/// @param keyIlType IL type of @p keyValue.
/// @param mapType Semantic map type used to choose between Map and IntMap.
/// @return Widened i64 key for IntMap, or the original value for string-keyed Map.
Lowerer::Value Lowerer::coerceMapKeyForRuntime(Value keyValue, Type keyIlType, TypeRef mapType) {
    if (usesIntegerMapRuntime(mapType))
        return widenIntegralToI64(keyValue, keyIlType);
    return keyValue;
}

/// @brief Emit a null check for pointer-like values.
/// @param ptr Pointer or string value.
/// @param ptrType IL type of @p ptr.
/// @return An i1 value that is true when @p ptr is non-null.
/// @details IL integer comparisons cannot directly compare pointer operands. This helper
///          stores the pointer-sized value into a temporary stack slot, reloads the bits as
///          i64, and compares that integer representation with zero.
Lowerer::Value Lowerer::emitPointerIsNonNull(Value ptr, Type ptrType) {
    unsigned ptrSlotId = nextTempId();
    il::core::Instr ptrSlotInstr;
    ptrSlotInstr.result = ptrSlotId;
    ptrSlotInstr.op = Opcode::Alloca;
    ptrSlotInstr.type = Type(Type::Kind::Ptr);
    ptrSlotInstr.operands = {Value::constInt(static_cast<long long>(kMachineWordSize))};
    ptrSlotInstr.loc = curLoc_;
    blockMgr_.currentBlock()->instructions.push_back(ptrSlotInstr);
    Value ptrSlot = Value::temp(ptrSlotId);

    emitStore(ptrSlot, ptr, ptrType);
    Value ptrAsI64 = emitLoad(ptrSlot, Type(Type::Kind::I64));
    return emitBinary(Opcode::ICmpNe, Type(Type::Kind::I1), ptrAsI64, Value::constInt(0));
}

/// @brief Emit an index-range check when bounds checks are enabled.
/// @param index `i64` index to validate.
/// @param lowerInclusive Inclusive lower bound.
/// @param upperExclusive Exclusive upper bound.
/// @return Checked index result, or @p index unchanged when checks are disabled.
Lowerer::Value Lowerer::emitIndexCheck(Value index, Value lowerInclusive, Value upperExclusive) {
    if (!options_.boundsChecks)
        return index;

    unsigned id = nextTempId();
    il::core::Instr instr;
    instr.result = id;
    instr.op = Opcode::IdxChk;
    instr.type = Type(Type::Kind::I64);
    instr.operands = {index, lowerInclusive, upperExclusive};
    instr.loc = curLoc_;
    blockMgr_.currentBlock()->instructions.push_back(instr);
    return Value::temp(id);
}

/// @brief Trap when a range step is less than one.
/// @param stepValue `i64` range-step value to validate.
/// @return @p stepValue on the success continuation, or unchanged immediately
///         when bounds checks are disabled.
/// @details With checks enabled, emission terminates the current block with a
///          conditional branch and leaves the insertion point in the success
///          block.
Lowerer::Value Lowerer::emitPositiveStepCheck(Value stepValue) {
    if (!options_.boundsChecks)
        return stepValue;

    Value invalid = emitBinary(Opcode::SCmpLT, Type(Type::Kind::I1), stepValue, Value::constInt(1));
    size_t trapIdx = createBlock("range_step_trap");
    size_t okIdx = createBlock("range_step_ok");
    emitCBr(invalid, trapIdx, okIdx);

    setBlock(trapIdx);
    il::core::Instr trap;
    trap.op = Opcode::Trap;
    trap.type = Type(Type::Kind::Void);
    trap.loc = curLoc_;
    blockMgr_.currentBlock()->instructions.push_back(std::move(trap));
    blockMgr_.currentBlock()->terminated = true;

    setBlock(okIdx);
    return stepValue;
}

/// @brief Narrow a signed integer to the checked Zia Byte representation.
/// @param value Integer value to convert.
/// @return Fresh `i32` result produced by `CastSiNarrowChk`.
Lowerer::Value Lowerer::narrowIntegerToByte(Value value) {
    return emitUnary(Opcode::CastSiNarrowChk, Type(Type::Kind::I32), value);
}

/// @brief Coerce a lowered value to a target semantic type.
/// @param value Value to convert or reclassify.
/// @param valueIlType Current IL representation of @p value.
/// @param sourceType Semantic source type, or null/unknown when unavailable.
/// @param targetType Required semantic destination type, or null for no
///        conversion.
/// @return Converted value and its destination IL representation.
/// @details Handles boxing for `Any` and type parameters, unboxing unknown
///          pointer values, optional wrapping, struct-to-interface heap
///          wrapping, and checked numeric/Byte conversions. Representation-
///          compatible values are returned without an emitted conversion.
LowerResult Lowerer::coerceValueToType(Value value,
                                       Type valueIlType,
                                       TypeRef sourceType,
                                       TypeRef targetType) {
    if (!targetType)
        return {value, valueIlType};

    Type targetIlType = mapType(targetType);
    if (targetType->kind == TypeKindSem::Unit)
        return {Value::null(), targetIlType};

    TypeRef effectiveSource = sourceType;
    bool sourceIsUnknownish = !effectiveSource || effectiveSource->kind == TypeKindSem::Unknown ||
                              effectiveSource->kind == TypeKindSem::Any;

    if (targetType->kind == TypeKindSem::Any) {
        if (valueIlType.kind == Type::Kind::Ptr)
            return {value, Type(Type::Kind::Ptr)};
        TypeRef boxType = (!sourceIsUnknownish && effectiveSource) ? effectiveSource
                                                                   : reverseMapType(valueIlType);
        return {emitBoxValue(value, valueIlType, boxType), Type(Type::Kind::Ptr)};
    }

    if (targetType->kind == TypeKindSem::TypeParam) {
        if (valueIlType.kind == Type::Kind::Ptr)
            return {value, Type(Type::Kind::Ptr)};
        TypeRef boxType = (!sourceIsUnknownish && effectiveSource) ? effectiveSource
                                                                   : reverseMapType(valueIlType);
        return {emitBoxValue(value, valueIlType, boxType), Type(Type::Kind::Ptr)};
    }

    if (sourceIsUnknownish && valueIlType.kind == Type::Kind::Ptr &&
        targetIlType.kind != Type::Kind::Ptr) {
        return emitUnbox(value, targetIlType);
    }

    if (sourceIsUnknownish) {
        effectiveSource = reverseMapType(valueIlType);
    }

    if (targetType->kind == TypeKindSem::Optional) {
        TypeRef innerType = targetType->innerType();
        auto optionalStoreType = [&]() {
            Type optionalIlType = mapType(targetType);
            return value.kind == Value::Kind::NullPtr && optionalIlType.kind == Type::Kind::Str
                       ? Type(Type::Kind::Ptr)
                       : optionalIlType;
        };
        if (effectiveSource && effectiveSource->kind == TypeKindSem::Optional)
            return {value, optionalStoreType()};
        if (innerType) {
            auto coercedInner = coerceValueToType(value, valueIlType, effectiveSource, innerType);
            return {emitOptionalWrap(coercedInner.value, innerType), mapType(targetType)};
        }
        return {value, mapType(targetType)};
    }

    if (targetType->kind == TypeKindSem::Interface && effectiveSource &&
        effectiveSource->kind == TypeKindSem::Struct) {
        const StructTypeInfo *info = getOrCreateStructTypeInfo(effectiveSource->name);
        if (info && info->classId > 0) {
            Value wrapper = emitCallRet(
                Type(Type::Kind::Ptr),
                "rt_obj_new_i64",
                {Value::constInt(static_cast<int64_t>(info->classId)),
                 Value::constInt(static_cast<int64_t>(kClassFieldsOffset + info->totalSize))});
            Value payload = emitGEP(wrapper, static_cast<int64_t>(kClassFieldsOffset));
            emitStructTypeInitialize(*info, payload, value);
            return {wrapper, Type(Type::Kind::Ptr)};
        }
    }

    if (effectiveSource && effectiveSource->kind == TypeKindSem::Unknown &&
        valueIlType.kind == Type::Kind::Ptr && targetIlType.kind != Type::Kind::Ptr) {
        return emitUnbox(value, targetIlType);
    }

    if (effectiveSource) {
        if (effectiveSource->kind == TypeKindSem::Number &&
            targetType->kind == TypeKindSem::Integer) {
            return {emitUnary(Opcode::CastFpToSiRteChk, Type(Type::Kind::I64), value),
                    Type(Type::Kind::I64)};
        }
        if (effectiveSource->kind == TypeKindSem::Integer &&
            targetType->kind == TypeKindSem::Number) {
            return {emitUnary(Opcode::Sitofp, Type(Type::Kind::F64), value), Type(Type::Kind::F64)};
        }
        if (effectiveSource->kind == TypeKindSem::Integer &&
            targetType->kind == TypeKindSem::Byte) {
            return {narrowIntegerToByte(value), Type(Type::Kind::I32)};
        }
        if (effectiveSource->kind == TypeKindSem::Byte &&
            targetType->kind == TypeKindSem::Integer) {
            return {widenByteToInteger(value), Type(Type::Kind::I64)};
        }
        if (effectiveSource->kind == TypeKindSem::Byte && targetType->kind == TypeKindSem::Number) {
            Value widened = widenByteToInteger(value);
            return {emitUnary(Opcode::Sitofp, Type(Type::Kind::F64), widened),
                    Type(Type::Kind::F64)};
        }
    }

    return {value, targetIlType};
}

/// @brief Check if a string-returning call returns a borrowed reference.
/// @param callee Canonical lowered callee name.
/// @return False under the current runtime contract, where string-returning
///         accessors return owned handles.
/// @details Runtime string accessors should return owned handles. Keep this
///          hook for future borrowed APIs, but default to the owned contract.
static bool isBorrowedStringCall(const std::string &callee) {
    (void)callee;
    return false;
}

/// @brief Emit a direct call that produces a result value.
/// @param retTy IL return type expected from the callee.
/// @param callee Canonical direct-call target name.
/// @param args Call arguments in ABI order.
/// @return Fresh temporary containing the call result.
/// @details Records the target as a used external, consumes deferred arguments
///          identified by the runtime signature, and defers release of owned
///          string or pointer results according to the runtime ownership
///          catalog.
Lowerer::Value Lowerer::emitCallRet(Type retTy,
                                    const std::string &callee,
                                    const std::vector<Value> &args) {
    usedExterns_.insert(callee);
    unsigned id = nextTempId();
    il::core::Instr instr;
    instr.result = id;
    instr.op = Opcode::Call;
    instr.type = retTy;
    instr.setDirectCallee(callee);
    instr.operands = args;
    instr.loc = curLoc_;
    blockMgr_.currentBlock()->instructions.push_back(instr);

    if (const auto *descriptor = il::runtime::findRuntimeDescriptor(callee)) {
        for (std::size_t i = 0; i < args.size() && i < 64; ++i) {
            if ((descriptor->signature.consumedArgMask & (std::uint64_t{1} << i)) != 0)
                consumeDeferred(args[i]);
        }
    }

    Value result = Value::temp(id);

    // Auto-track owned call results for deferred release at statement
    // boundary. Object results are tracked only when the runtime ownership
    // catalog explicitly marks the return as owned; unmarked object accessors
    // may be borrowed references into another runtime object.
    if (retTy.kind == Type::Kind::Str && !isBorrowedStringCall(callee))
        deferRelease(result, /*isString=*/true);
    else if (retTy.kind == Type::Kind::Ptr) {
        const auto *descriptor = il::runtime::findRuntimeDescriptor(callee);
        const bool returnsOwned = descriptor && descriptor->signature.returnsOwned;
        if (returnsOwned)
            deferRelease(result, /*isString=*/false);
    }

    // Zia automatically reclaims boxed temporaries after each statement, so
    // language-visible Queue and Stack instances must retain inserted values.
    // Their C runtime constructors intentionally default to borrowing mode for
    // low-level callers; opt only Zia-created instances into owning mode.
    if (callee == kCollectionsQueueNew)
        emitCall(kCollectionsQueueSetOwnsElements, {result, Value::constBool(true)});
    else if (callee == kCollectionsStackNew)
        emitCall(kCollectionsStackSetOwnsElements, {result, Value::constBool(true)});

    return result;
}

/// @brief Emit a void direct call.
/// @param callee Canonical direct-call target name.
/// @param args Call arguments in ABI order.
/// @details Records the callee as a used external and consumes deferred
///          arguments identified by the runtime ownership signature.
void Lowerer::emitCall(const std::string &callee, const std::vector<Value> &args) {
    usedExterns_.insert(callee);
    il::core::Instr instr;
    instr.op = Opcode::Call;
    instr.type = Type(Type::Kind::Void);
    instr.setDirectCallee(callee);
    instr.operands = args;
    instr.loc = curLoc_;
    blockMgr_.currentBlock()->instructions.push_back(instr);

    if (const auto *descriptor = il::runtime::findRuntimeDescriptor(callee)) {
        for (std::size_t i = 0; i < args.size() && i < 64; ++i) {
            if ((descriptor->signature.consumedArgMask & (std::uint64_t{1} << i)) != 0)
                consumeDeferred(args[i]);
        }
    }
}

/// @brief Emit a void indirect call through a function pointer.
/// @param funcPtr Runtime function pointer to invoke.
/// @param args Call arguments in ABI order after the function-pointer operand.
void Lowerer::emitCallIndirect(Value funcPtr, const std::vector<Value> &args) {
    il::core::Instr instr;
    instr.op = Opcode::CallIndirect;
    instr.type = Type(Type::Kind::Void);
    instr.setIndirectSignature(Type(Type::Kind::Void), {}, true);
    // For call.indirect, the function pointer is the first operand
    instr.operands.push_back(funcPtr);
    for (const auto &arg : args) {
        instr.operands.push_back(arg);
    }
    instr.loc = curLoc_;
    blockMgr_.currentBlock()->instructions.push_back(instr);
}

/// @brief Emit a value-returning indirect call through a function pointer.
/// @param retTy IL return type expected from the target.
/// @param funcPtr Runtime function pointer to invoke.
/// @param args Call arguments in ABI order after the function-pointer operand.
/// @return Fresh temporary containing the indirect call result.
Lowerer::Value Lowerer::emitCallIndirectRet(Type retTy,
                                            Value funcPtr,
                                            const std::vector<Value> &args) {
    unsigned id = nextTempId();
    il::core::Instr instr;
    instr.result = id;
    instr.op = Opcode::CallIndirect;
    instr.type = retTy;
    instr.setIndirectSignature(retTy, {}, true);
    // For call.indirect, the function pointer is the first operand
    instr.operands.push_back(funcPtr);
    for (const auto &arg : args) {
        instr.operands.push_back(arg);
    }
    instr.loc = curLoc_;
    blockMgr_.currentBlock()->instructions.push_back(instr);
    return Value::temp(id);
}

/// @brief Emit a direct call uniformly for void and value-returning targets.
/// @param callee Canonical direct-call target name.
/// @param args Call arguments in ABI order.
/// @param returnType IL return type of the target.
/// @return Call result and type, using an integer-zero placeholder for void.
LowerResult Lowerer::emitCallWithReturn(const std::string &callee,
                                        const std::vector<Value> &args,
                                        Type returnType) {
    if (returnType.kind == Type::Kind::Void) {
        emitCall(callee, args);
        return {Value::constInt(0), Type(Type::Kind::Void)};
    }
    return {emitCallRet(returnType, callee, args), returnType};
}

/// @brief Convert a lowered value to the Zia runtime string representation.
/// @param val Value to format.
/// @param sourceType Semantic type of @p val, or null when unavailable.
/// @return @p val for strings or unknown types; otherwise an owned string
///         produced by the matching runtime formatting helper.
Lowerer::Value Lowerer::emitToString(Value val, TypeRef sourceType) {
    if (!sourceType)
        return val;

    switch (sourceType->kind) {
        case TypeKindSem::String:
            return val;
        case TypeKindSem::Byte:
            return emitCallRet(Type(Type::Kind::Str), kStringFromInt, {widenByteToInteger(val)});
        case TypeKindSem::Integer:
        case TypeKindSem::Enum:
            return emitCallRet(Type(Type::Kind::Str), kStringFromInt, {val});
        case TypeKindSem::Number:
            return emitCallRet(Type(Type::Kind::Str), kStringFromNum, {val});
        case TypeKindSem::Boolean:
            return emitCallRet(Type(Type::Kind::Str), kTextFmtBool, {val});
        default:
            return emitCallRet(Type(Type::Kind::Str), kObjectToString, {val});
    }
}

/// @brief Terminate the current block with an unconditional branch.
/// @param targetIdx Index of the destination block in the current function.
void Lowerer::emitBr(size_t targetIdx) {
    // Use index-based access to avoid stale pointer after vector reallocation
    il::core::Instr instr;
    instr.op = Opcode::Br;
    instr.type = Type(Type::Kind::Void);
    instr.addBranchTarget(currentFunc_->blocks[targetIdx].label);
    instr.loc = curLoc_;
    blockMgr_.currentBlock()->instructions.push_back(std::move(instr));
    blockMgr_.currentBlock()->terminated = true;
}

/// @brief Terminate the current block with a conditional branch.
/// @param cond `i1` branch condition.
/// @param trueIdx Destination block index when @p cond is true.
/// @param falseIdx Destination block index when @p cond is false.
void Lowerer::emitCBr(Value cond, size_t trueIdx, size_t falseIdx) {
    // Use index-based access to avoid stale pointer after vector reallocation
    il::core::Instr instr;
    instr.op = Opcode::CBr;
    instr.type = Type(Type::Kind::Void);
    instr.operands.push_back(cond);
    instr.addBranchTarget(currentFunc_->blocks[trueIdx].label);
    instr.addBranchTarget(currentFunc_->blocks[falseIdx].label);
    instr.loc = curLoc_;
    blockMgr_.currentBlock()->instructions.push_back(std::move(instr));
    blockMgr_.currentBlock()->terminated = true;
}

/// @brief Terminate the current block with a value return.
/// @param val Value returned to the caller.
void Lowerer::emitRet(Value val) {
    // Use index-based access to avoid stale pointer after vector reallocation
    il::core::Instr instr;
    instr.op = Opcode::Ret;
    instr.type = Type(Type::Kind::Void);
    instr.operands.push_back(val);
    instr.loc = curLoc_;
    blockMgr_.currentBlock()->instructions.push_back(std::move(instr));
    blockMgr_.currentBlock()->terminated = true;
}

/// @brief Terminate the current block with a void return.
void Lowerer::emitRetVoid() {
    // Use index-based access to avoid stale pointer after vector reallocation
    il::core::Instr instr;
    instr.op = Opcode::Ret;
    instr.type = Type(Type::Kind::Void);
    instr.loc = curLoc_;
    blockMgr_.currentBlock()->instructions.push_back(std::move(instr));
    blockMgr_.currentBlock()->terminated = true;
}

/// @brief Materialize an owned runtime string from an interned global.
/// @param globalName Module string-global label to materialize.
/// @return Fresh string handle scheduled for deferred release.
Lowerer::Value Lowerer::emitConstStr(const std::string &globalName) {
    Value result = builder_->emitConstStr(globalName, curLoc_);
    // Native const_str materialisation currently returns a fresh runtime
    // string handle.  Treat it exactly like any other owned string result so
    // statement cleanup, slot stores, and returns transfer or release that
    // reference instead of leaking every literal evaluation.
    deferRelease(result, /*isString=*/true);
    return result;
}

/// @brief Materialize an owned empty runtime string.
/// @return Fresh empty-string handle scheduled for deferred release.
Lowerer::Value Lowerer::emitEmptyString() {
    return emitConstStr(getStringGlobal(""));
}

/// @brief Reserve a fresh IL temporary identifier.
/// @return Identifier unique within the builder's current value namespace.
/// @details When a function is active, also seeds its debug-facing value name
///          with the generated `%tN` form.
unsigned Lowerer::nextTempId() {
    unsigned id = builder_->reserveTempId();
    if (currentFunc_) {
        if (currentFunc_->valueNames.size() <= id)
            currentFunc_->valueNames.resize(id + 1);
        if (currentFunc_->valueNames[id].empty())
            currentFunc_->valueNames[id] = "%t" + std::to_string(id);
    }
    return id;
}

/// @brief Assign a readable, function-local name to a temporary.
/// @param id Temporary identifier to name.
/// @param name Preferred non-empty name.
/// @details Duplicate preferred names receive an `$<id>` suffix. The request
///          is ignored when no function is active or @p name is empty.
void Lowerer::nameTemp(unsigned id, const std::string &name) {
    if (!currentFunc_ || name.empty())
        return;
    if (currentFunc_->valueNames.size() <= id)
        currentFunc_->valueNames.resize(id + 1);

    bool nameInUse = false;
    for (size_t i = 0; i < currentFunc_->valueNames.size(); ++i) {
        if (i != id && currentFunc_->valueNames[i] == name) {
            nameInUse = true;
            break;
        }
    }
    currentFunc_->valueNames[id] = nameInUse ? name + "$" + std::to_string(id) : name;
}

//=============================================================================
// Boxing/Unboxing Helpers
//=============================================================================

/// @brief Box a primitive IL value into the runtime object representation.
/// @param val Primitive or pointer value to box.
/// @param type Current IL representation of @p val.
/// @return Owned boxed pointer for primitive kinds; pointer and unsupported
///         kinds are returned unchanged.
Lowerer::Value Lowerer::emitBox(Value val, Type type) {
    switch (type.kind) {
        case Type::Kind::I64:
        case Type::Kind::I32:
        case Type::Kind::I16:
            return emitCallRet(Type(Type::Kind::Ptr), kBoxI64, {val});
        case Type::Kind::F64:
            return emitCallRet(Type(Type::Kind::Ptr), kBoxF64, {val});
        case Type::Kind::I1:
            return emitCallRet(Type(Type::Kind::Ptr), kBoxI1, {val});
        case Type::Kind::Str:
            return emitCallRet(Type(Type::Kind::Ptr), kBoxStr, {val});
        case Type::Kind::Ptr:
            // Objects don't need boxing
            return val;
        default:
            return val;
    }
}

/// @brief Box a value using both its IL representation and semantic type.
/// @param val Value to box.
/// @param ilType Current IL representation of @p val.
/// @param semanticType Semantic type used to recognize inline structs and
///        managed nested fields.
/// @return Owned heap box for value types and primitives, or @p val when its
///         representation requires no box.
/// @details Structs are copied into a value-type heap box after preloading
///          their fields, then every recursively nested managed field is
///          registered with the runtime ownership descriptor.
Lowerer::Value Lowerer::emitBoxValue(Value val, Type ilType, TypeRef semanticType) {
    // Check if this is a struct type that needs heap allocation
    if (semanticType && semanticType->kind == TypeKindSem::Struct &&
        ilType.kind == Type::Kind::Ptr) {
        // Look up the struct type info
        const StructTypeInfo *info = getOrCreateStructTypeInfo(semanticType->name);
        if (info && info->totalSize > 0) {
            struct ManagedValueField {
                size_t offset;
                int64_t kind;
            };

            std::vector<ManagedValueField> managedFields;
            std::function<void(TypeRef, size_t)> collectManagedFields = [&](TypeRef type,
                                                                            size_t baseOffset) {
                if (!type)
                    return;
                if (type->kind == TypeKindSem::Struct) {
                    if (const StructTypeInfo *nested = getOrCreateStructTypeInfo(type->name)) {
                        for (const auto &field : nested->fields)
                            collectManagedFields(field.type, baseOffset + field.offset);
                    }
                    return;
                }
                if (type->kind == TypeKindSem::FixedArray) {
                    TypeRef elemType = type->elementType();
                    const size_t elemSize = getSemanticTypeSize(elemType);
                    for (size_t i = 0; i < type->elementCount; ++i)
                        collectManagedFields(elemType, baseOffset + i * elemSize);
                    return;
                }
                if (type->kind == TypeKindSem::Tuple) {
                    const auto elements = type->tupleElementTypes();
                    for (size_t i = 0; i < elements.size(); ++i)
                        collectManagedFields(elements[i],
                                             baseOffset + getTupleElementOffset(type, i));
                    return;
                }

                if (isStringType(type)) {
                    managedFields.push_back({baseOffset, /*RT_VALUE_FIELD_STR=*/2});
                    return;
                }

                bool objectLike = needsRelease(type);
                if (type->kind == TypeKindSem::Interface || type->kind == TypeKindSem::Function ||
                    type->kind == TypeKindSem::Any)
                    objectLike = true;
                if (objectLike && mapType(type).kind == Type::Kind::Ptr) {
                    managedFields.push_back({baseOffset, /*RT_VALUE_FIELD_OBJ=*/1});
                }
            };
            collectManagedFields(semanticType, 0);

            // Read all field values BEFORE allocating heap memory. The source
            // pointer (val) may point into a callee's C stack frame that was
            // just returned from; in native code that frame is freed on return
            // and will be overwritten by the very next function call
            // (rt_box_value_type → rt_heap_alloc). Reading first keeps all
            // fields in IL temporaries (vregs) that survive the call via
            // callee-save registers or spills into the current frame.
            std::vector<Value> fieldValues;
            fieldValues.reserve(info->fields.size());
            for (const auto &field : info->fields)
                fieldValues.push_back(emitFieldLoad(&field, val));

            // Now it is safe to allocate heap memory
            Value heapPtr = emitCallRet(Type(Type::Kind::Ptr),
                                        kBoxValueType,
                                        {Value::constInt(static_cast<int64_t>(info->totalSize))});

            // Write the pre-read field values into the heap object
            for (size_t i = 0; i < info->fields.size(); ++i)
                emitFieldStore(&info->fields[i], heapPtr, fieldValues[i]);

            for (const auto &field : managedFields) {
                emitCall(kBoxValueTypeAddField,
                         {heapPtr,
                          Value::constInt(static_cast<int64_t>(field.offset)),
                          Value::constInt(field.kind),
                          Value::constBool(false)});
            }

            return heapPtr;
        }
    }

    // Fall back to standard boxing
    return emitBox(val, ilType);
}

/// @brief Unbox a runtime object to a primitive IL representation.
/// @param boxed Runtime object pointer or already-pointer value.
/// @param expectedType Requested IL representation.
/// @return Unboxed value and actual IL type; pointer and unsupported targets
///         remain pointer-valued.
LowerResult Lowerer::emitUnbox(Value boxed, Type expectedType) {
    switch (expectedType.kind) {
        case Type::Kind::I64:
        case Type::Kind::I32:
        case Type::Kind::I16: {
            Value unboxed = emitCallRet(Type(Type::Kind::I64), kUnboxI64, {boxed});
            return {unboxed, Type(Type::Kind::I64)};
        }
        case Type::Kind::F64: {
            Value unboxed = emitCallRet(Type(Type::Kind::F64), kUnboxF64, {boxed});
            return {unboxed, Type(Type::Kind::F64)};
        }
        case Type::Kind::I1: {
            Value unboxed = emitCallRet(Type(Type::Kind::I1), kUnboxI1, {boxed});
            return {unboxed, Type(Type::Kind::I1)};
        }
        case Type::Kind::Str: {
            Value unboxed = emitCallRet(Type(Type::Kind::Str), kUnboxStr, {boxed});
            return {unboxed, Type(Type::Kind::Str)};
        }
        case Type::Kind::Ptr:
            // Object references don't need unboxing
            return {boxed, Type(Type::Kind::Ptr)};
        default:
            return {boxed, Type(Type::Kind::Ptr)};
    }
}

/// @brief Unbox a runtime value with semantic handling for value types.
/// @param boxed Boxed runtime pointer.
/// @param ilType Requested IL representation.
/// @param semanticType Requested semantic type.
/// @return Stack copy for a known non-empty struct, otherwise the primitive
///         result of emitUnbox().
LowerResult Lowerer::emitUnboxValue(Value boxed, Type ilType, TypeRef semanticType) {
    // Check if this is a struct type that needs copying from heap to stack
    if (semanticType && semanticType->kind == TypeKindSem::Struct &&
        ilType.kind == Type::Kind::Ptr) {
        // Look up the struct type info
        const StructTypeInfo *info = getOrCreateStructTypeInfo(semanticType->name);
        if (info && info->totalSize > 0) {
            // Allocate stack memory for the copy
            Value stackCopy = emitStructTypeCopy(*info, boxed);
            return {stackCopy, Type(Type::Kind::Ptr)};
        }
    }

    // Fall back to standard unboxing
    return emitUnbox(boxed, ilType);
}

/// @brief Convert a value to the pointer-compatible Optional representation.
/// @param val Present payload value.
/// @param innerType Semantic payload type.
/// @return Heap box for struct and primitive payloads; pointer/string payloads
///         are already nullable and are returned unchanged.
Lowerer::Value Lowerer::emitOptionalWrap(Value val, TypeRef innerType) {
    Type ilType = mapType(innerType);
    // Structs map to ptr at the IL level but still have value semantics.
    // Optionals of structs therefore need a boxed heap copy, not a raw
    // stack pointer that would dangle after the current expression returns.
    if (innerType && innerType->kind == TypeKindSem::Struct)
        return emitBoxValue(val, ilType, innerType);
    // Reference-like IL values already use null as the optional sentinel.
    // That includes both object pointers and raw string pointers.
    if (ilType.kind == Type::Kind::Ptr || ilType.kind == Type::Kind::Str)
        return val;
    // Primitive/value payloads need boxing to convert to ptr for Optional representation.
    return emitBox(val, ilType);
}

/// @brief Convert a present Optional payload back to its underlying value.
/// @param val Non-null optional representation.
/// @param innerType Semantic payload type.
/// @return Fresh stack copy for a struct, unboxed primitive, or unchanged
///         pointer/string payload.
LowerResult Lowerer::emitOptionalUnwrap(Value val, TypeRef innerType) {
    Type ilType = mapType(innerType);
    // Struct optionals carry boxed heap payloads so force-unwrap / narrowing
    // produces a fresh stack value with normal copy semantics.
    if (innerType && innerType->kind == TypeKindSem::Struct)
        return emitUnboxValue(val, ilType, innerType);
    // Reference-like optional payloads use null to represent None, so the
    // stored IL value is already the underlying value.
    if (ilType.kind == Type::Kind::Ptr || ilType.kind == Type::Kind::Str)
        return {val, ilType};
    // Primitive/value payloads need unboxing from ptr back to their concrete type.
    return emitUnbox(val, ilType);
}

//=============================================================================
// Low-Level Instruction Emission
//=============================================================================

/// @brief Emit a constant-byte-offset pointer calculation.
/// @param ptr Base pointer.
/// @param offset Signed byte offset from @p ptr.
/// @return Fresh pointer temporary produced by `GEP`.
Lowerer::Value Lowerer::emitGEP(Value ptr, int64_t offset) {
    unsigned gepId = nextTempId();
    il::core::Instr gepInstr;
    gepInstr.result = gepId;
    gepInstr.op = Opcode::GEP;
    gepInstr.type = Type(Type::Kind::Ptr);
    gepInstr.operands = {ptr, Value::constInt(offset)};
    gepInstr.loc = curLoc_;
    blockMgr_.currentBlock()->instructions.push_back(std::move(gepInstr));
    return Value::temp(gepId);
}

/// @brief Emit a typed load from memory.
/// @param ptr Address to read.
/// @param type IL type of the stored value.
/// @return Fresh temporary containing the loaded value.
Lowerer::Value Lowerer::emitLoad(Value ptr, Type type) {
    unsigned loadId = nextTempId();
    il::core::Instr loadInstr;
    loadInstr.result = loadId;
    loadInstr.op = Opcode::Load;
    loadInstr.type = type;
    loadInstr.operands = {ptr};
    loadInstr.loc = curLoc_;
    blockMgr_.currentBlock()->instructions.push_back(std::move(loadInstr));
    return Value::temp(loadId);
}

/// @brief Emit a typed store to memory.
/// @param ptr Destination address.
/// @param val Value to write.
/// @param type IL type used for the memory operation.
void Lowerer::emitStore(Value ptr, Value val, Type type) {
    il::core::Instr storeInstr;
    storeInstr.op = Opcode::Store;
    storeInstr.type = type;
    storeInstr.operands = {ptr, val};
    storeInstr.loc = curLoc_;
    blockMgr_.currentBlock()->instructions.push_back(std::move(storeInstr));
}

/// @brief Load a field from an aggregate or entity instance.
/// @param field Resolved field layout, including byte offset and ownership.
/// @param selfPtr Base address of the containing value.
/// @return Weak referent, inline aggregate address, or loaded scalar value.
/// @details Loaded strings are retained and deferred because memory loads
///          produce borrowed handles.
Lowerer::Value Lowerer::emitFieldLoad(const FieldLayout *field, Value selfPtr) {
    Value fieldAddr = emitGEP(selfPtr, static_cast<int64_t>(field->offset));
    if (field->isWeak) {
        Value weakHandle = emitLoad(fieldAddr, Type(Type::Kind::Ptr));
        return emitCallRet(Type(Type::Kind::Ptr), runtime::kMemoryWeakRefGet, {weakHandle});
    }
    // Inline aggregates live directly inside the containing value. Loading them
    // as a pointer-sized scalar would read the first bytes of the aggregate.
    if (isInlineAggregateType(field->type))
        return fieldAddr;
    Type fieldType = mapType(field->type);
    Value loaded = emitLoad(fieldAddr, fieldType);
    // BUG-ADV-001: Retain loaded string fields to prevent use-after-free.
    // Load gives a borrowed reference; callers may consume the string in
    // concatenation or pass it cross-module, making the borrow dangling.
    if (fieldType.kind == Type::Kind::Str) {
        emitCall(runtime::kStrRetainMaybe, {loaded});
        deferRelease(loaded, /*isString=*/true);
    }
    return loaded;
}

/// @brief Store a value into an aggregate or entity field.
/// @param field Resolved field layout, including byte offset and ownership.
/// @param selfPtr Base address of the containing value.
/// @param val New field value.
/// @details Weak fields replace their runtime weak handle; other fields use
///          recursive inline-value ownership and replacement semantics.
void Lowerer::emitFieldStore(const FieldLayout *field, Value selfPtr, Value val) {
    Value fieldAddr = emitGEP(selfPtr, static_cast<int64_t>(field->offset));
    if (field->isWeak) {
        Value oldHandle = emitLoad(fieldAddr, Type(Type::Kind::Ptr));
        Value newHandle = emitCallRet(Type(Type::Kind::Ptr), runtime::kMemoryWeakRefNew, {val});
        // The field takes the weak-handle creation reference. Without this
        // transfer, statement cleanup frees the handle while the field still
        // points at it.
        consumeDeferred(newHandle);
        emitStore(fieldAddr, newHandle, Type(Type::Kind::Ptr));
        emitCall(runtime::kMemoryWeakRefFree, {oldHandle});
        return;
    }
    emitInlineValueStore(field->type, fieldAddr, val, true);
}

/// @brief Store a semantic value into inline storage with ownership handling.
/// @param valueType Semantic type of the stored value.
/// @param destPtr Destination address.
/// @param value Scalar value or source pointer for an inline aggregate.
/// @param destInitialized True when an existing managed value must be released.
/// @details Aggregate values are copied recursively. String and owned-object
///          values transfer a deferred reference when available or retain a
///          borrowed reference before replacing the destination.
void Lowerer::emitInlineValueStore(TypeRef valueType,
                                   Value destPtr,
                                   Value value,
                                   bool destInitialized) {
    if (isInlineAggregateType(valueType)) {
        emitInlineValueCopy(valueType, destPtr, value, destInitialized);
        return;
    }

    Type ilType = mapType(valueType);
    if (ilType.kind == Type::Kind::Str) {
        const bool owned = consumeDeferred(value);
        if (destInitialized) {
            Value oldValue = emitLoad(destPtr, ilType);
            if (!owned)
                emitCall(runtime::kStrRetainMaybe, {value});
            emitStore(destPtr, value, ilType);
            emitCall(runtime::kStrReleaseMaybe, {oldValue});
        } else {
            if (!owned)
                emitCall(runtime::kStrRetainMaybe, {value});
            emitStore(destPtr, value, ilType);
        }
        return;
    }

    if (ilType.kind == Type::Kind::Ptr && needsRelease(valueType)) {
        const bool owned = consumeDeferred(value);
        if (!owned)
            emitCall("rt_obj_retain_maybe", {value});
        Value oldValue = Value::null();
        if (destInitialized)
            oldValue = emitLoad(destPtr, ilType);
        emitStore(destPtr, value, ilType);
        if (destInitialized)
            emitManagedRelease(oldValue, /*isString=*/false);
        return;
    }

    emitStore(destPtr, value, ilType);
}

/// @brief Move a managed handle out of a slot without releasing it.
/// @param slot Address of the owning slot.
/// @param type Managed string or pointer representation to load.
/// @return Previously stored handle; the slot is reset to null.
Lowerer::Value Lowerer::takeManagedValueFromSlot(Value slot, Type type) {
    Value value = emitLoad(slot, type);
    // Managed String and object handles share the pointer representation. A
    // raw null store records an ownership move without decrementing the handle.
    emitStore(slot, Value::null(), Type(Type::Kind::Ptr));
    return value;
}

/// @brief Recursively copy an inline semantic value between storage regions.
/// @param valueType Semantic type describing the storage layout.
/// @param destPtr Destination base address.
/// @param sourcePtr Source base address.
/// @param destInitialized True when displaced managed fields must be released.
/// @details Struct fields, fixed-array elements, and tuple elements are copied
///          according to their computed layouts; leaf values delegate to
///          emitInlineValueStore().
void Lowerer::emitInlineValueCopy(TypeRef valueType,
                                  Value destPtr,
                                  Value sourcePtr,
                                  bool destInitialized) {
    if (!valueType) {
        Value loaded = emitLoad(sourcePtr, Type(Type::Kind::Ptr));
        emitStore(destPtr, loaded, Type(Type::Kind::Ptr));
        return;
    }

    if (valueType->kind == TypeKindSem::Struct) {
        if (const StructTypeInfo *info = getOrCreateStructTypeInfo(valueType->name)) {
            if (destInitialized)
                emitStructTypeStore(*info, destPtr, sourcePtr);
            else
                emitStructTypeInitialize(*info, destPtr, sourcePtr);
            return;
        }
    }

    if (valueType->kind == TypeKindSem::FixedArray) {
        TypeRef elemType = valueType->elementType();
        const size_t elemSize = getSemanticTypeSize(elemType);
        for (size_t i = 0; i < valueType->elementCount; ++i) {
            Value dstElem = i == 0 ? destPtr : emitGEP(destPtr, static_cast<int64_t>(i * elemSize));
            Value srcElem =
                i == 0 ? sourcePtr : emitGEP(sourcePtr, static_cast<int64_t>(i * elemSize));
            emitInlineValueCopy(elemType, dstElem, srcElem, destInitialized);
        }
        return;
    }

    if (valueType->kind == TypeKindSem::Tuple) {
        const auto elements = valueType->tupleElementTypes();
        for (size_t i = 0; i < elements.size(); ++i) {
            const size_t offset = getTupleElementOffset(valueType, i);
            Value dstElem = offset == 0 ? destPtr : emitGEP(destPtr, static_cast<int64_t>(offset));
            Value srcElem =
                offset == 0 ? sourcePtr : emitGEP(sourcePtr, static_cast<int64_t>(offset));
            emitInlineValueCopy(elements[i], dstElem, srcElem, destInitialized);
        }
        return;
    }

    Type ilType = mapType(valueType);
    Value loaded = emitLoad(sourcePtr, ilType);
    emitInlineValueStore(valueType, destPtr, loaded, destInitialized);
}

/// @brief Recursively initialize inline storage to semantic zero/null values.
/// @param valueType Semantic type describing the destination layout.
/// @param destPtr Destination base address.
void Lowerer::emitInlineValueZero(TypeRef valueType, Value destPtr) {
    if (valueType && valueType->kind == TypeKindSem::Struct) {
        if (const StructTypeInfo *info = getOrCreateStructTypeInfo(valueType->name)) {
            for (const auto &field : info->fields) {
                Value fieldAddr = emitGEP(destPtr, static_cast<int64_t>(field.offset));
                emitInlineValueZero(field.type, fieldAddr);
            }
            return;
        }
    }

    if (valueType && valueType->kind == TypeKindSem::FixedArray) {
        TypeRef elemType = valueType->elementType();
        const size_t elemSize = getSemanticTypeSize(elemType);
        for (size_t i = 0; i < valueType->elementCount; ++i) {
            Value elemPtr = i == 0 ? destPtr : emitGEP(destPtr, static_cast<int64_t>(i * elemSize));
            emitInlineValueZero(elemType, elemPtr);
        }
        return;
    }

    if (valueType && valueType->kind == TypeKindSem::Tuple) {
        const auto elements = valueType->tupleElementTypes();
        for (size_t i = 0; i < elements.size(); ++i) {
            const size_t offset = getTupleElementOffset(valueType, i);
            Value elemPtr = offset == 0 ? destPtr : emitGEP(destPtr, static_cast<int64_t>(offset));
            emitInlineValueZero(elements[i], elemPtr);
        }
        return;
    }

    Type ilType = mapType(valueType);
    switch (ilType.kind) {
        case Type::Kind::I64:
        case Type::Kind::I32:
        case Type::Kind::I16:
        case Type::Kind::I1:
            emitStore(destPtr, Value::constInt(0), ilType);
            break;
        case Type::Kind::F64:
            emitStore(destPtr, Value::constFloat(0.0), ilType);
            break;
        case Type::Kind::Str:
            emitStore(destPtr, Value::null(), Type(Type::Kind::Ptr));
            break;
        case Type::Kind::Ptr:
        default:
            emitStore(destPtr, Value::null(), Type(Type::Kind::Ptr));
            break;
    }
}

/// @brief Allocate and zero-initialize stack storage for an inline value.
/// @param valueType Semantic type whose storage size and fields are used.
/// @return Pointer to at least one byte of initialized stack storage.
Lowerer::Value Lowerer::emitInlineValueAlloc(TypeRef valueType) {
    const size_t storageSize = std::max<size_t>(1, getSemanticTypeSize(valueType));
    unsigned allocaId = nextTempId();
    il::core::Instr allocaInstr;
    allocaInstr.result = allocaId;
    allocaInstr.op = Opcode::Alloca;
    allocaInstr.type = Type(Type::Kind::Ptr);
    allocaInstr.operands = {Value::constInt(static_cast<int64_t>(storageSize))};
    allocaInstr.loc = curLoc_;
    blockMgr_.currentBlock()->instructions.push_back(std::move(allocaInstr));

    Value destPtr = Value::temp(allocaId);
    emitInlineValueZero(valueType, destPtr);
    return destPtr;
}

/// @brief Allocate a stack copy of a struct value.
/// @param info Resolved struct layout.
/// @param sourcePtr Address of the source struct storage.
/// @return Pointer to initialized stack storage containing an ownership-safe
///         field-by-field copy.
Lowerer::Value Lowerer::emitStructTypeCopy(const StructTypeInfo &info, Value sourcePtr) {
    // Allocate stack space for the copy
    unsigned allocaId = nextTempId();
    il::core::Instr allocaInstr;
    allocaInstr.result = allocaId;
    allocaInstr.op = Opcode::Alloca;
    allocaInstr.type = Type(Type::Kind::Ptr);
    allocaInstr.operands = {Value::constInt(static_cast<int64_t>(info.totalSize))};
    allocaInstr.loc = curLoc_;
    blockMgr_.currentBlock()->instructions.push_back(allocaInstr);
    Value destPtr = Value::temp(allocaId);

    emitStructTypeInitialize(info, destPtr, sourcePtr);
    return destPtr;
}

/// @brief Initialize unowned struct storage from another struct value.
/// @param info Resolved struct layout.
/// @param destPtr Address of uninitialized destination storage.
/// @param sourcePtr Address of source storage.
void Lowerer::emitStructTypeInitialize(const StructTypeInfo &info, Value destPtr, Value sourcePtr) {
    for (const auto &field : info.fields) {
        Value fieldAddr = emitGEP(destPtr, static_cast<int64_t>(field.offset));
        Value srcAddr = emitGEP(sourcePtr, static_cast<int64_t>(field.offset));
        emitInlineValueCopy(field.type, fieldAddr, srcAddr, false);
    }
}

/// @brief Replace an initialized struct value with an ownership-safe copy.
/// @param info Resolved struct layout.
/// @param destPtr Address of initialized destination storage.
/// @param sourcePtr Address of source storage.
void Lowerer::emitStructTypeStore(const StructTypeInfo &info, Value destPtr, Value sourcePtr) {
    for (const auto &field : info.fields) {
        Value fieldAddr = emitGEP(destPtr, static_cast<int64_t>(field.offset));
        Value srcAddr = emitGEP(sourcePtr, static_cast<int64_t>(field.offset));
        emitInlineValueCopy(field.type, fieldAddr, srcAddr, true);
    }
}

/// @brief Allocate zero-initialized stack storage for a struct layout.
/// @param info Resolved struct layout.
/// @return Pointer to the initialized storage.
Lowerer::Value Lowerer::emitStructTypeAlloc(const StructTypeInfo &info) {
    // Allocate stack space for the struct type
    unsigned allocaId = nextTempId();
    il::core::Instr allocaInstr;
    allocaInstr.result = allocaId;
    allocaInstr.op = Opcode::Alloca;
    allocaInstr.type = Type(Type::Kind::Ptr);
    allocaInstr.operands = {Value::constInt(static_cast<int64_t>(info.totalSize))};
    allocaInstr.loc = curLoc_;
    blockMgr_.currentBlock()->instructions.push_back(allocaInstr);
    Value destPtr = Value::temp(allocaId);

    // Zero-initialize all fields
    for (const auto &field : info.fields) {
        Value fieldAddr = emitGEP(destPtr, static_cast<int64_t>(field.offset));
        emitInlineValueZero(field.type, fieldAddr);
    }

    return destPtr;
}

/// @brief Apply value semantics and ownership tracking to a call result.
/// @param result Raw value returned by the call instruction.
/// @param semanticType Semantic return type.
/// @param ilType IL return representation.
/// @return Stack-materialized struct or the original result with @p ilType.
/// @details Owned struct boxes are copied into inline storage and then deferred
///          for release; other releasable pointer results are also deferred.
LowerResult Lowerer::materializeCallResult(Value result, TypeRef semanticType, Type ilType) {
    if (semanticType && semanticType->kind == TypeKindSem::Struct &&
        ilType.kind == Type::Kind::Ptr) {
        LowerResult materialized = emitUnboxValue(result, ilType, semanticType);
        // User functions and methods return value structs in an owned heap box.
        // The stack copy retains any managed fields, so the transferred box can
        // be reclaimed at the statement boundary after materialization.
        deferRelease(result, /*isString=*/false);
        return materialized;
    }
    if (ilType.kind == Type::Kind::Ptr && needsRelease(semanticType))
        deferRelease(result, /*isString=*/false);
    return {result, ilType};
}

//=============================================================================
// Type Mapping
//=============================================================================

/// @brief Report an internal semantic-to-lowering invariant violation.
/// @param loc Source location associated with the unresolved construct.
/// @param code Stable diagnostic code.
/// @param message Human-readable diagnostic message.
void Lowerer::reportLoweringInvariant(il::support::SourceLoc loc,
                                      std::string code,
                                      std::string message) {
    il::support::Diagnostic diag{
        il::support::Severity::Error,
        std::move(message),
        loc,
        std::move(code),
    };
    if (loc.isValid()) {
        diag.range = il::support::SourceRange{
            loc,
            il::support::SourceLoc{loc.file_id, loc.line, loc.column + 1},
        };
    }
    diag.stage = "lower";
    diag.help = "This indicates semantic analysis left an unresolved construct for lowering.";
    diag_.report(std::move(diag));
}

/// @brief Test whether a semantic type is unusable by lowering.
/// @param type Semantic type to validate.
/// @return True when @p type is null.
bool Lowerer::isInvalidLoweringType(TypeRef type) const {
    return !type;
}

/// @brief Report a lowering invariant failure and return an error-typed value.
/// @param loc Source location associated with the failure.
/// @param code Stable diagnostic code.
/// @param message Human-readable diagnostic message.
/// @return Integer-zero placeholder with IL `error` type.
LowerResult Lowerer::poisonValue(il::support::SourceLoc loc,
                                 std::string code,
                                 std::string message) {
    reportLoweringInvariant(loc, std::move(code), std::move(message));
    return {Value::constInt(0), Type(Type::Kind::Error)};
}

/// @brief Map a resolved Zia semantic type to its IL representation.
/// @param type Semantic type to map.
/// @return Corresponding IL type, or `void`/`error` after reporting an
///         invariant violation for an unusable type.
Lowerer::Type Lowerer::mapType(TypeRef type) {
    if (!type) {
        reportLoweringInvariant(
            curLoc_, "V-ZIA-LOWER-MISSING-TYPE", "missing semantic type during lowering");
        return Type(Type::Kind::Void);
    }

    if (isInvalidLoweringType(type)) {
        reportLoweringInvariant(curLoc_,
                                "V-ZIA-LOWER-UNRESOLVED-TYPE",
                                "unresolved semantic type '" + type->toString() +
                                    "' reached lowering");
        return Type(Type::Kind::Error);
    }

    return Type(toILType(*type));
}

/// @brief Recover a representative semantic type from an IL type.
/// @param ilType IL representation to classify.
/// @return Canonical semantic type for the representation, or `Unknown` for
///         unsupported kinds.
TypeRef Lowerer::reverseMapType(Type ilType) {
    switch (ilType.kind) {
        case Type::Kind::I64:
            return types::integer();
        case Type::Kind::F64:
            return types::number();
        case Type::Kind::I1:
            return types::boolean();
        case Type::Kind::Str:
            return types::string();
        case Type::Kind::I32:
        case Type::Kind::I16:
            return types::byte();
        case Type::Kind::Ptr:
            return types::ptr();
        case Type::Kind::Void:
            return types::voidType();
        default:
            return types::unknown();
    }
}

/// @brief Return the size in bytes of an IL type.
/// @details Used for struct layout calculations and GEP offset computation.
///          Sizes follow x86-64 conventions: 8 bytes for pointers and 64-bit
///          integers/floats, 4 for i32, 2 for i16, 1 for i1.
/// @param type IL type to measure.
/// @return Size in bytes.
size_t Lowerer::getILTypeSize(Type type) {
    switch (type.kind) {
        case Type::Kind::I64:
        case Type::Kind::F64:
        case Type::Kind::Ptr:
        case Type::Kind::Str:
            return 8;
        case Type::Kind::I32:
            return 4;
        case Type::Kind::I16:
            return 2;
        case Type::Kind::I1:
            return 1;
        default:
            return 8;
    }
}

/// @brief Return the alignment requirement in bytes for an IL type.
/// @details Alignments follow x86-64 SysV ABI: types align to their natural
///          size, with booleans promoted to 8-byte alignment to prevent
///          misalignment when adjacent to pointer-sized fields.
/// @param type IL type to query.
/// @return Alignment in bytes.
size_t Lowerer::getILTypeAlignment(Type type) {
    // All types align to their size, with a minimum of 8 for pointer-sized types
    // This matches the x86-64 SysV ABI requirements
    switch (type.kind) {
        case Type::Kind::I64:
        case Type::Kind::F64:
        case Type::Kind::Ptr:
        case Type::Kind::Str:
            return 8;
        case Type::Kind::I32:
            return 4;
        case Type::Kind::I16:
            return 2;
        case Type::Kind::I1:
            // Boolean fields should be aligned to 8 bytes to avoid misalignment
            // when followed by 8-byte fields
            return 8;
        default:
            return 8;
    }
}

/// @brief Compute inline storage size for a semantic type.
/// @param type Semantic type to measure, or null for pointer-sized fallback.
/// @return Size in bytes, recursively accounting for structs, fixed arrays,
///         and padded tuples.
size_t Lowerer::getSemanticTypeSize(TypeRef type) {
    if (!type)
        return getILTypeSize(Type(Type::Kind::Ptr));

    switch (type->kind) {
        case TypeKindSem::Struct:
            if (const StructTypeInfo *info = getOrCreateStructTypeInfo(type->name))
                return info->totalSize;
            return getILTypeSize(mapType(type));
        case TypeKindSem::FixedArray: {
            TypeRef elemType = type->elementType();
            return getSemanticTypeSize(elemType) * type->elementCount;
        }
        case TypeKindSem::Tuple:
            return getTupleStorageSize(type);
        default:
            return getILTypeSize(mapType(type));
    }
}

/// @brief Compute inline storage alignment for a semantic type.
/// @param type Semantic type to inspect, or null for pointer alignment.
/// @return Maximum required field/element alignment in bytes for aggregates,
///         otherwise the mapped IL alignment.
size_t Lowerer::getSemanticTypeAlignment(TypeRef type) {
    if (!type)
        return getILTypeAlignment(Type(Type::Kind::Ptr));

    switch (type->kind) {
        case TypeKindSem::Struct: {
            const StructTypeInfo *info = getOrCreateStructTypeInfo(type->name);
            size_t alignment = 1;
            if (info) {
                for (const auto &field : info->fields)
                    alignment = std::max(alignment, getSemanticTypeAlignment(field.type));
                return alignment;
            }
            return getILTypeAlignment(mapType(type));
        }
        case TypeKindSem::FixedArray:
            return getSemanticTypeAlignment(type->elementType());
        case TypeKindSem::Tuple: {
            size_t alignment = 1;
            for (const auto &elemType : type->tupleElementTypes())
                alignment = std::max(alignment, getSemanticTypeAlignment(elemType));
            return alignment;
        }
        default:
            return getILTypeAlignment(mapType(type));
    }
}

/// @brief Compute the byte offset of a tuple element.
/// @param tupleType Semantic tuple type.
/// @param index Zero-based element index.
/// @return Aligned element offset; zero for a non-tuple input. An out-of-range
///         index returns the computed end offset.
size_t Lowerer::getTupleElementOffset(TypeRef tupleType, size_t index) {
    if (!tupleType || tupleType->kind != TypeKindSem::Tuple)
        return 0;

    const auto elements = tupleType->tupleElementTypes();
    size_t offset = 0;
    for (size_t i = 0; i < elements.size(); ++i) {
        offset = alignTo(offset, getSemanticTypeAlignment(elements[i]));
        if (i == index)
            return offset;
        offset += getSemanticTypeSize(elements[i]);
    }
    return offset;
}

/// @brief Compute total padded inline storage for a tuple.
/// @param tupleType Semantic tuple type.
/// @return Size in bytes rounded to the tuple's maximum element alignment, or
///         zero for a non-tuple input.
size_t Lowerer::getTupleStorageSize(TypeRef tupleType) {
    if (!tupleType || tupleType->kind != TypeKindSem::Tuple)
        return 0;

    const auto elements = tupleType->tupleElementTypes();
    size_t offset = 0;
    size_t alignment = 1;
    for (const auto &elemType : elements) {
        const size_t elemAlignment = getSemanticTypeAlignment(elemType);
        alignment = std::max(alignment, elemAlignment);
        offset = alignTo(offset, elemAlignment);
        offset += getSemanticTypeSize(elemType);
    }
    return alignTo(offset, alignment);
}

/// @brief Round @p offset up to the next multiple of @p alignment.
/// @details Used during struct layout to ensure each field starts at a
///          properly aligned address. Delegates to il::support::alignUp.
/// @param offset Current byte offset to align.
/// @param alignment Required alignment (must be a power of 2).
/// @return Smallest value >= @p offset that is a multiple of @p alignment.
size_t Lowerer::alignTo(size_t offset, size_t alignment) {
    return il::support::alignUp(offset, alignment);
}

//=============================================================================
// Local Variable Management
//=============================================================================

/// @brief Bind a function-local name to a current SSA value.
/// @param name Source-level local name to define or replace.
/// @param value Lowered value associated with @p name.
/// @details Temporary values also receive @p name as their preferred debug
///          value name.
void Lowerer::defineLocal(const std::string &name, Value value) {
    locals_[name] = value;
    if (value.kind == Value::Kind::Temp)
        nameTemp(value.id, name);
}

/// @brief Look up the current SSA binding for a local name.
/// @param name Source-level local name.
/// @return Pointer to the stored value, or null when no binding exists.
Lowerer::Value *Lowerer::lookupLocal(const std::string &name) {
    // Check regular locals first
    auto it = locals_.find(name);
    return it != locals_.end() ? &it->second : nullptr;
}

/// @brief Allocate pointer-sized stack storage for a local variable.
/// @param name Local name used for slot registration and debug naming.
/// @param type Intended stored type; storage is uniformly one machine word.
/// @return Pointer temporary for the new slot.
Lowerer::Value Lowerer::createSlot(const std::string &name, Type type) {
    // Allocate stack space for the variable
    unsigned allocaId = nextTempId();
    il::core::Instr allocaInstr;
    allocaInstr.result = allocaId;
    allocaInstr.op = Opcode::Alloca;
    allocaInstr.type = Type(Type::Kind::Ptr);
    allocaInstr.operands = {Value::constInt(static_cast<long long>(kMachineWordSize))};
    allocaInstr.loc = curLoc_;
    blockMgr_.currentBlock()->instructions.push_back(allocaInstr);

    Value slot = Value::temp(allocaId);
    nameTemp(allocaId, name);
    slots_[name] = slot;
    return slot;
}

/// @brief Store a value into a registered local slot.
/// @param name Local name whose slot is targeted.
/// @param value Value to write.
/// @param type IL type used for the store.
/// @details Does nothing when @p name has no registered slot.
void Lowerer::storeToSlot(const std::string &name, Value value, Type type) {
    auto it = slots_.find(name);
    if (it == slots_.end())
        return;

    il::core::Instr storeInstr;
    storeInstr.op = Opcode::Store;
    storeInstr.type = type;
    storeInstr.operands = {it->second, value};
    storeInstr.loc = curLoc_;
    blockMgr_.currentBlock()->instructions.push_back(storeInstr);
}

/// @brief Load a value from a registered local slot.
/// @param name Local name whose slot is read.
/// @param type IL type used for the load.
/// @return Fresh loaded temporary, or integer zero when no slot exists.
Lowerer::Value Lowerer::loadFromSlot(const std::string &name, Type type) {
    auto it = slots_.find(name);
    if (it == slots_.end())
        return Value::constInt(0);

    unsigned loadId = nextTempId();
    il::core::Instr loadInstr;
    loadInstr.result = loadId;
    loadInstr.op = Opcode::Load;
    loadInstr.type = type;
    loadInstr.operands = {it->second};
    loadInstr.loc = curLoc_;
    blockMgr_.currentBlock()->instructions.push_back(loadInstr);

    return Value::temp(loadId);
}

/// @brief Remove a local slot binding without emitting storage cleanup.
/// @param name Local name to remove from the slot map.
void Lowerer::removeSlot(const std::string &name) {
    slots_.erase(name);
}

/// @brief Resolve the current method receiver.
/// @param result Receives the loaded or directly bound `self` pointer.
/// @return True when `self` is available from a slot or local binding.
bool Lowerer::getSelfPtr(Value &result) {
    // Check if self is stored in a slot (used in class/struct type methods)
    auto slotIt = slots_.find("self");
    if (slotIt != slots_.end()) {
        result = loadFromSlot("self", Type(Type::Kind::Ptr));
        return true;
    }

    // Check if self is a regular local
    Value *local = lookupLocal("self");
    if (local) {
        result = *local;
        return true;
    }

    return false;
}

//=============================================================================
// Helper Functions
//=============================================================================

/// @brief Apply the Zia entry-point name mapping.
/// @param name Qualified lowered function name.
/// @return `main` for source entry point `start`; otherwise @p name unchanged.
std::string Lowerer::mangleFunctionName(const std::string &name) {
    // Entry point is special
    if (name == "start")
        return "main";
    return name;
}

/// @brief Intern string contents in the module string table.
/// @param value UTF-8 string contents.
/// @return Stable module-global label for @p value.
std::string Lowerer::getStringGlobal(const std::string &value) {
    return stringTable_.intern(value);
}

/// @brief Compare two byte strings with C character case folding.
/// @param a First string.
/// @param b Second string.
/// @return True when lengths and every case-folded byte are equal.
bool Lowerer::equalsIgnoreCase(const std::string &a, const std::string &b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

//=============================================================================
// Deferred Release (Automatic Memory Management)
//=============================================================================

/// @brief Determine whether a semantic value owns a releasable runtime handle.
/// @param type Semantic type to classify.
/// @return True for strings, managed reference/container types, boxed optional
///         payloads, type parameters, and named runtime pointers.
bool Lowerer::needsRelease(TypeRef type) const {
    if (!type)
        return false;
    switch (type->kind) {
        case TypeKindSem::Optional: {
            TypeRef inner = type->innerType();
            if (!inner)
                return false;
            switch (inner->kind) {
                case TypeKindSem::Struct:
                    return true; // Optional structs carry an owned boxed payload.
                case TypeKindSem::Integer:
                case TypeKindSem::Number:
                case TypeKindSem::Boolean:
                case TypeKindSem::Byte:
                    return true; // Optional primitives are boxed.
                default:
                    return needsRelease(inner);
            }
        }
        case TypeKindSem::String:
        case TypeKindSem::Class:
        case TypeKindSem::Interface:
        case TypeKindSem::List:
        case TypeKindSem::Map:
        case TypeKindSem::Set:
        case TypeKindSem::Any:
        case TypeKindSem::TypeParam:
            return true;
        // Ptr with a non-empty name is likely a runtime class (Seq, etc.)
        case TypeKindSem::Ptr:
            return !type->name.empty();
        default:
            return false;
    }
}

/// @brief Determine whether a semantic type is String or Optional[String].
/// @param type Semantic type to classify.
/// @return True for String after recursively removing Optional wrappers.
bool Lowerer::isStringType(TypeRef type) const {
    if (!type)
        return false;
    if (type->kind == TypeKindSem::Optional)
        return isStringType(type->innerType());
    return type->kind == TypeKindSem::String;
}

/// @brief Release a managed handle and return its resulting reference count.
/// @param value String or object handle to release.
/// @param isString True to use string release semantics.
/// @return Integer zero for strings; for objects, the post-release reference
///         count after running destructor dispatch and freeing at zero.
/// @details Object release introduces destroy and continuation blocks and
///          leaves the insertion point at the continuation.
Lowerer::Value Lowerer::emitManagedReleaseRet(Value value, bool isString) {
    if (isString) {
        emitCall(kStrReleaseMaybe, {value});
        return Value::constInt(0);
    }

    Value nextRefCount = emitCallRet(Type(Type::Kind::I64), "rt_heap_release_deferred", {value});

    unsigned slotId = nextTempId();
    il::core::Instr allocaInstr;
    allocaInstr.result = slotId;
    allocaInstr.op = Opcode::Alloca;
    allocaInstr.type = Type(Type::Kind::Ptr);
    allocaInstr.operands = {Value::constInt(static_cast<long long>(kMachineWordSize))};
    allocaInstr.loc = curLoc_;
    blockMgr_.currentBlock()->instructions.push_back(allocaInstr);
    Value resultSlot = Value::temp(slotId);
    emitStore(resultSlot, nextRefCount, Type(Type::Kind::I64));

    Value isZero =
        emitBinary(Opcode::ICmpEq, Type(Type::Kind::I1), nextRefCount, Value::constInt(0));
    size_t destroyIdx = createBlock("release_destroy");
    size_t contIdx = createBlock("release_cont");
    emitCBr(isZero, destroyIdx, contIdx);

    setBlock(destroyIdx);
    emitCall("__zia_dtor_dispatch", {value});
    emitCall("rt_obj_free", {value});
    emitBr(contIdx);

    setBlock(contIdx);
    return emitLoad(resultSlot, Type(Type::Kind::I64));
}

/// @brief Release a managed string, object, or dynamically boxed handle.
/// @param value Managed handle to release; null-safe runtime helpers are used.
/// @param isString True when @p value has the direct string representation.
/// @details Object release branches to destructor dispatch and deallocation
///          only when the checked runtime helper reports a heap object whose
///          reference count reached zero. The insertion point finishes in a
///          continuation block.
void Lowerer::emitManagedRelease(Value value, bool isString) {
    if (isString) {
        emitCall(kStrReleaseMaybe, {value});
        return;
    }

    // A semantic object can dynamically contain a heap object, a string
    // handle (for Any/collection values), or an opaque runtime handle such as
    // a GUI widget. The checked object helper releases the first two and is a
    // no-op for opaque handles; branching on its result preserves Zia class
    // destructor dispatch only for a heap object whose count reached zero.
    Value shouldDestroy = emitCallRet(Type(Type::Kind::I1), "rt_obj_release_check0", {value});
    size_t destroyIdx = createBlock("release_destroy");
    size_t contIdx = createBlock("release_cont");
    emitCBr(shouldDestroy, destroyIdx, contIdx);

    setBlock(destroyIdx);
    emitCall("__zia_dtor_dispatch", {value});
    emitCall("rt_obj_free", {value});
    emitBr(contIdx);

    setBlock(contIdx);
}

/// @brief Schedule an owned temporary for release at an expression boundary.
/// @param v Owned value to track.
/// @param isString True for the direct string release path.
/// @details Duplicate temporary identifiers are ignored. Each entry records
///          its defining block so later cleanup never references an SSA value
///          on a different control-flow edge.
void Lowerer::deferRelease(Value v, bool isString) {
    if (v.kind == Value::Kind::Temp) {
        for (const auto &pending : deferredTemps_) {
            if (pending.value.kind == Value::Kind::Temp && pending.value.id == v.id)
                return;
        }
    }
    deferredTemps_.push_back({v, isString, blockMgr_.currentBlockIndex()});
}

/// @brief Transfer ownership out of the deferred-release set.
/// @param v Temporary whose pending release should be canceled.
/// @return True when the most recently scheduled matching entry was removed.
bool Lowerer::consumeDeferred(Value v) {
    if (v.kind != Value::Kind::Temp)
        return false;

    // Remove the LAST matching entry (most recently deferred)
    for (auto it = deferredTemps_.rbegin(); it != deferredTemps_.rend(); ++it) {
        if (it->value.kind == Value::Kind::Temp && it->value.id == v.id) {
            // Convert reverse iterator to forward iterator for erase
            deferredTemps_.erase(std::next(it).base());
            return true;
        }
    }
    return false;
}

/// @brief Test whether a local slot owns a string-compatible handle.
/// @param name Local slot name.
/// @return True for a registered slot whose mapped semantic type is `str`,
///         including Optional[String].
bool Lowerer::isOwnedStringSlot(const std::string &name) const {
    if (slots_.find(name) == slots_.end())
        return false;
    auto typeIt = localTypes_.find(name);
    if (typeIt == localTypes_.end() || !typeIt->second)
        return false;
    // Optional<String> slots also hold (nullable) string handles.
    return const_cast<Lowerer *>(this)->mapType(typeIt->second).kind == Type::Kind::Str;
}

/// @brief Release every owned string slot in deterministic name order.
/// @details Emits nothing after the current block has terminated.
void Lowerer::releaseLocalStringSlots() {
    if (isTerminated())
        return;
    // Deterministic order: sort names (unordered_map iteration order varies).
    std::vector<std::string> names;
    for (const auto &[name, slot] : slots_) {
        if (isOwnedStringSlot(name))
            names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    for (const auto &name : names) {
        Value handle = loadFromSlot(name, Type(Type::Kind::Str));
        emitCall(kStrReleaseMaybe, {handle});
    }
}

/// @brief Release one local slot when it owns a string handle.
/// @param name Local slot name.
void Lowerer::releaseOwnedStringSlot(const std::string &name) {
    if (isTerminated() || !isOwnedStringSlot(name))
        return;
    Value handle = loadFromSlot(name, Type(Type::Kind::Str));
    emitCall(kStrReleaseMaybe, {handle});
}

/// @brief Transfer or retain a string value into an owning local slot.
/// @param name Destination local slot name.
/// @param value String handle to store.
/// @param releaseDisplaced True to release the slot's previous handle.
/// @details A deferred value transfers its existing reference; a borrowed
///          value is retained to create the slot's ownership reference.
void Lowerer::storeOwnedStringToSlot(const std::string &name, Value value, bool releaseDisplaced) {
    // Owned temporaries move their transferred reference into the slot;
    // borrowed values mint the slot's reference with a retain.
    const bool owned = consumeDeferred(value);
    if (!owned)
        emitCall(kStrRetainMaybe, {value});
    if (releaseDisplaced) {
        Value displaced = loadFromSlot(name, Type(Type::Kind::Str));
        emitCall(kStrReleaseMaybe, {displaced});
    }
    storeToSlot(name, value, Type(Type::Kind::Str));
}

/// @brief Release string slots introduced or rebound within a lexical block.
/// @param liveBefore Slot map captured before entering the block.
/// @details A slot is local to the block when its name was absent or mapped to
///          a different storage value in @p liveBefore.
void Lowerer::releaseBlockStringSlots(const std::unordered_map<std::string, Value> &liveBefore) {
    if (isTerminated())
        return;
    std::vector<std::string> names;
    for (const auto &[name, slot] : slots_) {
        const auto previous = liveBefore.find(name);
        const bool introducedHere =
            previous == liveBefore.end() || !il::core::valueEquals(previous->second, slot);
        if (introducedHere && isOwnedStringSlot(name))
            names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    for (const auto &name : names) {
        Value handle = loadFromSlot(name, Type(Type::Kind::Str));
        emitCall(kStrReleaseMaybe, {handle});
    }
}

/// @brief Test whether a local slot owns a releasable object handle.
/// @param name Local slot name.
/// @return True for a registered pointer-representation slot whose semantic
///         type satisfies needsRelease().
bool Lowerer::isOwnedObjectSlot(const std::string &name) const {
    if (slots_.find(name) == slots_.end())
        return false;
    auto typeIt = localTypes_.find(name);
    if (typeIt == localTypes_.end() || !typeIt->second)
        return false;
    auto *self = const_cast<Lowerer *>(this);
    return self->mapType(typeIt->second).kind == Type::Kind::Ptr &&
           self->needsRelease(typeIt->second);
}

/// @brief Release every owned object slot in deterministic name order.
/// @details Emits nothing after the current block has terminated.
void Lowerer::releaseLocalObjectSlots() {
    if (isTerminated())
        return;
    std::vector<std::string> names;
    for (const auto &[name, slot] : slots_) {
        if (isOwnedObjectSlot(name))
            names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    for (const auto &name : names) {
        Value handle = loadFromSlot(name, Type(Type::Kind::Ptr));
        emitManagedRelease(handle, /*isString=*/false);
    }
}

/// @brief Release one local slot when it owns an object handle.
/// @param name Local slot name.
void Lowerer::releaseOwnedObjectSlot(const std::string &name) {
    if (isTerminated() || !isOwnedObjectSlot(name))
        return;
    Value handle = loadFromSlot(name, Type(Type::Kind::Ptr));
    emitManagedRelease(handle, /*isString=*/false);
}

/// @brief Enter a loop and snapshot the slots live before its body.
/// @param breakTarget Destination block index for `break`.
/// @param continueTarget Destination block index for `continue`.
void Lowerer::pushLoopScope(size_t breakTarget, size_t continueTarget) {
    loopStack_.push(breakTarget, continueTarget);
    loopSlotSnapshots_.push_back(slots_);
}

/// @brief Leave the innermost loop and discard its slot snapshot.
void Lowerer::popLoopScope() {
    loopStack_.pop();
    if (!loopSlotSnapshots_.empty())
        loopSlotSnapshots_.pop_back();
}

/// @brief Release managed slots introduced in the current loop body.
/// @details Uses the snapshot captured by pushLoopScope() and emits nothing
///          when no loop is active or the block is already terminated.
void Lowerer::releaseCurrentLoopBodySlots() {
    if (loopSlotSnapshots_.empty() || isTerminated())
        return;
    releaseBlockStringSlots(loopSlotSnapshots_.back());
    releaseBlockObjectSlots(loopSlotSnapshots_.back());
}

/// @brief Transfer or retain an object value into an owning local slot.
/// @param name Destination local slot name.
/// @param value Object handle to store.
/// @param releaseDisplaced True to release the slot's previous handle.
/// @details A deferred value transfers its existing reference; a borrowed
///          value is retained to create the slot's ownership reference.
void Lowerer::storeOwnedObjectToSlot(const std::string &name, Value value, bool releaseDisplaced) {
    const bool owned = consumeDeferred(value);
    if (!owned)
        emitCall("rt_obj_retain_maybe", {value});
    if (releaseDisplaced) {
        Value displaced = loadFromSlot(name, Type(Type::Kind::Ptr));
        emitManagedRelease(displaced, /*isString=*/false);
    }
    storeToSlot(name, value, Type(Type::Kind::Ptr));
}

/// @brief Release object slots introduced or rebound within a lexical block.
/// @param liveBefore Slot map captured before entering the block.
/// @details A slot is local to the block when its name was absent or mapped to
///          a different storage value in @p liveBefore.
void Lowerer::releaseBlockObjectSlots(const std::unordered_map<std::string, Value> &liveBefore) {
    if (isTerminated())
        return;
    std::vector<std::string> names;
    for (const auto &[name, slot] : slots_) {
        const auto previous = liveBefore.find(name);
        const bool introducedHere =
            previous == liveBefore.end() || !il::core::valueEquals(previous->second, slot);
        if (introducedHere && isOwnedObjectSlot(name))
            names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    for (const auto &name : names) {
        Value handle = loadFromSlot(name, Type(Type::Kind::Ptr));
        emitManagedRelease(handle, /*isString=*/false);
    }
}

/// @brief Release all pending temporaries valid in the current block.
/// @details Clears pending state without emission when the block is terminated.
void Lowerer::releaseDeferredTemps() {
    if (deferredTemps_.empty() || isTerminated()) {
        deferredTemps_.clear();
        return;
    }

    releaseDeferredTempsFrom(0);
}

/// @brief Release pending temporaries from one saved boundary onward.
/// @param first Index of the first deferred entry owned by the current
///        expression or statement.
/// @details Entries are removed from tracking immediately; only values
///          defined in the active block receive emitted release operations.
void Lowerer::releaseDeferredTempsFrom(size_t first) {
    if (first >= deferredTemps_.size())
        return;
    if (isTerminated()) {
        deferredTemps_.erase(deferredTemps_.begin() + static_cast<std::ptrdiff_t>(first),
                             deferredTemps_.end());
        return;
    }

    // Only release temps defined in the current block (SSA scoping).
    // Control-flow expression lowerers call this before leaving each defining
    // block. A different-block entry here belongs to a nested expression path
    // that cannot execute on the current edge and must not be referenced.
    size_t curBlock = blockMgr_.currentBlockIndex();
    std::vector<DeferredRelease> pending;
    pending.reserve(deferredTemps_.size() - first);
    std::move(deferredTemps_.begin() + static_cast<std::ptrdiff_t>(first),
              deferredTemps_.end(),
              std::back_inserter(pending));
    deferredTemps_.erase(deferredTemps_.begin() + static_cast<std::ptrdiff_t>(first),
                         deferredTemps_.end());
    for (const auto &t : pending) {
        if (t.blockIdx != curBlock)
            continue;
        emitManagedRelease(t.value, t.isString);
    }
}

/// @brief Emit the module-wide `__zia_dtor_dispatch` helper.
/// @details Collects defined class destructors by runtime class identifier and
///          emits a deterministic comparison chain that calls the matching
///          `<Type>.__dtor(self)` implementation. The caller's lowering,
///          ownership, and owning-type contexts are saved and restored around
///          helper generation.
void Lowerer::emitDestructorDispatch() {
    std::vector<std::pair<int, std::string>> destructors;
    destructors.reserve(classTypes_.size());
    for (const auto &[typeName, info] : classTypes_) {
        const std::string dtorName = typeName + ".__dtor";
        if (definedFunctions_.count(dtorName) > 0)
            destructors.emplace_back(info.classId, dtorName);
    }

    std::sort(destructors.begin(), destructors.end(), [](const auto &lhs, const auto &rhs) {
        return lhs.first < rhs.first;
    });

    Function *savedFunc = currentFunc_;
    auto savedLocals = std::exchange(locals_, {});
    auto savedSlots = std::exchange(slots_, {});
    auto savedLocalTypes = std::exchange(localTypes_, {});
    auto savedDeferredTemps = std::exchange(deferredTemps_, {});
    TypeRef savedReturnType = currentReturnType_;
    const ClassTypeInfo *savedEntityType = currentClassType_;
    const StructTypeInfo *savedValueType = currentStructType_;

    auto &fn = builder_->startFunction(
        "__zia_dtor_dispatch", Type(Type::Kind::Void), {{"self", Type(Type::Kind::Ptr)}});
    currentFunc_ = &fn;
    currentReturnType_ = types::voidType();
    currentClassType_ = nullptr;
    currentStructType_ = nullptr;
    definedFunctions_.insert("__zia_dtor_dispatch");
    blockMgr_.bind(builder_.get(), &fn);
    builder_->createBlock(fn, "entry_0", fn.params);
    setBlock(fn.blocks.size() - 1);

    Value selfValue = Value::temp(fn.blocks.back().params[0].id);
    if (destructors.empty()) {
        emitRetVoid();
    } else {
        Value classId = emitCallRet(Type(Type::Kind::I64), "rt_obj_class_id", {selfValue});
        size_t defaultIdx = createBlock("dtor_default");
        std::vector<size_t> testBlocks;
        std::vector<size_t> callBlocks;
        testBlocks.reserve(destructors.size());
        callBlocks.reserve(destructors.size());

        for (size_t i = 0; i < destructors.size(); ++i) {
            testBlocks.push_back(i == 0 ? blockMgr_.currentBlockIndex()
                                        : createBlock("dtor_test_" + std::to_string(i)));
            callBlocks.push_back(createBlock("dtor_call_" + std::to_string(i)));
        }

        for (size_t i = 0; i < destructors.size(); ++i) {
            if (i != 0)
                setBlock(testBlocks[i]);
            Value match = emitBinary(Opcode::ICmpEq,
                                     Type(Type::Kind::I1),
                                     classId,
                                     Value::constInt(destructors[i].first));
            size_t falseIdx = (i + 1 < destructors.size()) ? testBlocks[i + 1] : defaultIdx;
            emitCBr(match, callBlocks[i], falseIdx);
            setBlock(callBlocks[i]);
            emitCall(destructors[i].second, {selfValue});
            emitRetVoid();
        }

        setBlock(defaultIdx);
        emitRetVoid();
    }

    currentFunc_ = savedFunc;
    currentReturnType_ = savedReturnType;
    currentClassType_ = savedEntityType;
    currentStructType_ = savedValueType;
    locals_ = std::move(savedLocals);
    slots_ = std::move(savedSlots);
    localTypes_ = std::move(savedLocalTypes);
    deferredTemps_ = std::move(savedDeferredTemps);
    if (savedFunc)
        blockMgr_.bind(builder_.get(), savedFunc);
    else
        blockMgr_.reset(nullptr);
}

} // namespace il::frontends::zia
