//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file Lowerer_Dispatch.cpp
/// @brief Virtual and interface method dispatch for the Zia IL lowerer.
///
//===----------------------------------------------------------------------===//

#include "frontends/zia/Lowerer.hpp"
#include "frontends/zia/RuntimeNames.hpp"

namespace il::frontends::zia {

using il::core::Opcode;
using il::core::Type;
using il::core::Value;
using namespace runtime;

/// Dispatch table entry: (classId, qualifiedMethodName)
using DispatchEntry = std::pair<int, std::string>;

//=============================================================================
// Virtual Method Dispatch
//=============================================================================

/// @brief Lower a virtual (overridable) method call via runtime class-id dispatch.
/// @param entityInfo Static type of the receiver at the call site.
/// @param slotKey Method slot key identifying the overridable method.
/// @param ownerType Type that declares the method (used to resolve its signature).
/// @param method The method declaration being called (may be null).
/// @param selfValue The receiver value, passed as the first argument.
/// @param expr The call expression supplying the remaining arguments.
/// @return The lowered call result (or a void placeholder for void methods).
/// @details Builds a dispatch table of every class (the receiver type and all its subclasses)
///          that supplies this slot. With one entry it emits a direct call; with several it
///          fetches the runtime class id and emits a single O(1) SwitchI32 to per-class call
///          blocks, storing each result in an alloca and joining at a common end block.
LowerResult Lowerer::lowerVirtualMethodCall(const ClassTypeInfo &entityInfo,
                                            const std::string &slotKey,
                                            const std::string &ownerType,
                                            MethodDecl *method,
                                            Value selfValue,
                                            CallExpr *expr) {
    TypeRef methodType = sema_.getMethodType(ownerType, method);
    std::vector<TypeRef> paramTypes;
    TypeRef returnType = methodType && methodType->kind == TypeKindSem::Function
                             ? methodType->returnType()
                             : types::voidType();
    if (methodType && methodType->kind == TypeKindSem::Function)
        paramTypes = methodType->paramTypes();
    Type ilReturnType = mapType(returnType);

    // Build argument list: self + call args
    std::vector<Value> args;
    args.reserve(paramTypes.size() + 1);
    args.push_back(selfValue);
    std::vector<Value> boundArgs =
        lowerResolvedCallArgs(expr, paramTypes, method ? &method->params : nullptr);
    args.insert(args.end(), boundArgs.begin(), boundArgs.end());

    // Build dispatch table
    std::vector<DispatchEntry> dispatchTable;
    /// @brief Adds one class implementation for the selected virtual slot.
    /// @param info Class metadata to inspect.
    auto addEntry = [&](const ClassTypeInfo &info) {
        auto vtIt = info.vtableIndex.find(slotKey);
        if (vtIt != info.vtableIndex.end())
            dispatchTable.emplace_back(info.classId, info.vtable[vtIt->second]);
    };

    addEntry(entityInfo);
    for (const auto &[name, info] : classTypes_) {
        if (name == entityInfo.name)
            continue;
        std::string parent = info.baseClass;
        while (!parent.empty()) {
            if (parent == entityInfo.name) {
                addEntry(info);
                break;
            }
            auto it = classTypes_.find(parent);
            if (it == classTypes_.end())
                break;
            parent = it->second.baseClass;
        }
    }

    // Get runtime class_id
    Value classIdVal = emitCallRet(Type(Type::Kind::I64), "rt_obj_class_id", {selfValue});

    // Single implementation - direct call
    if (dispatchTable.size() <= 1) {
        std::string target = dispatchTable.empty() ? sema_.loweredMethodName(ownerType, method)
                                                   : dispatchTable[0].second;
        // Handle void return types correctly
        if (ilReturnType.kind == Type::Kind::Void) {
            emitCall(target, args);
            return {Value::constInt(0), Type(Type::Kind::Void)};
        } else {
            Value result = emitCallRet(ilReturnType, target, args);
            return materializeCallResult(result, returnType, ilReturnType);
        }
    }

    // Multiple implementations - SwitchI32 dispatch
    //
    // Narrow the I64 class ID to I32 for the switch scrutinee, then emit a
    // single SwitchI32 instruction that jumps directly to the right call block.
    // This replaces the previous O(N) if-else chain with O(1) dispatch.
    Value classIdI32 = emitUnary(Opcode::CastUiNarrowChk, Type(Type::Kind::I32), classIdVal);

    size_t endBlock = createBlock("vdispatch_end");
    Value resultSlot;
    if (ilReturnType.kind != Type::Kind::Void) {
        unsigned id = nextTempId();
        il::core::Instr instr;
        instr.result = id;
        instr.op = Opcode::Alloca;
        instr.type = Type(Type::Kind::Ptr);
        instr.operands.push_back(Value::constInt(static_cast<long long>(kMachineWordSize)));
        instr.loc = curLoc_;
        blockMgr_.currentBlock()->instructions.push_back(instr);
        resultSlot = Value::temp(id);
    }

    // Create a call block for each dispatch entry.
    std::vector<size_t> callBlocks;
    callBlocks.reserve(dispatchTable.size());
    for (size_t i = 0; i < dispatchTable.size(); ++i)
        callBlocks.push_back(createBlock("vdispatch_call_" + std::to_string(i)));

    // Emit SwitchI32: scrutinee is the I32 class ID, cases are class IDs.
    // Default target is the last entry (fallback).
    {
        il::core::Instr sw;
        sw.op = Opcode::SwitchI32;
        sw.type = Type(Type::Kind::Void);
        sw.operands.push_back(classIdI32);

        // Default case → last call block (fallback for unknown class IDs).
        sw.addBranchTarget(currentFunc_->blocks[callBlocks.back()].label);

        // One case per dispatch entry.
        for (size_t i = 0; i < dispatchTable.size(); ++i) {
            sw.operands.push_back(Value::constInt(static_cast<int64_t>(dispatchTable[i].first)));
            sw.addBranchTarget(currentFunc_->blocks[callBlocks[i]].label);
        }

        sw.loc = curLoc_;
        blockMgr_.currentBlock()->instructions.push_back(std::move(sw));
        blockMgr_.currentBlock()->terminated = true;
    }

    // Emit each call block: call the target method, store result, branch to end.
    for (size_t i = 0; i < dispatchTable.size(); ++i) {
        const auto &[classId, targetMethod] = dispatchTable[i];
        setBlock(callBlocks[i]);
        if (ilReturnType.kind == Type::Kind::Void)
            emitCall(targetMethod, args);
        else
            emitStore(resultSlot, emitCallRet(ilReturnType, targetMethod, args), ilReturnType);
        emitBr(endBlock);
    }

    setBlock(endBlock);
    if (ilReturnType.kind == Type::Kind::Void)
        return {Value::constInt(0), Type(Type::Kind::Void)};
    return materializeCallResult(emitLoad(resultSlot, ilReturnType), returnType, ilReturnType);
}

//=============================================================================
// Interface Method Dispatch
//=============================================================================

/// @brief Lower an interface method call via runtime itable lookup.
/// @param ifaceInfo The interface type whose method is being invoked.
/// @param slotKey Method slot key identifying the interface method.
/// @param ownerType Type used to resolve the method signature.
/// @param method The method declaration being called (may be null).
/// @param selfValue The receiver value, passed as the first argument.
/// @param expr The call expression supplying the remaining arguments.
/// @return The lowered call result (or a void placeholder for void methods).
/// @details Resolves the method's interface slot index, fetches the receiver's class id and
///          itable at runtime (`rt_get_interface_impl`), loads the function pointer at
///          `itable[slot]`, and emits an indirect call through it.
LowerResult Lowerer::lowerInterfaceMethodCall(const InterfaceTypeInfo &ifaceInfo,
                                              const std::string &slotKey,
                                              const std::string &ownerType,
                                              MethodDecl *method,
                                              Value selfValue,
                                              CallExpr *expr) {
    TypeRef methodType = sema_.getMethodType(ownerType, method);
    std::vector<TypeRef> paramTypes;
    TypeRef returnType = methodType && methodType->kind == TypeKindSem::Function
                             ? methodType->returnType()
                             : types::voidType();
    if (methodType && methodType->kind == TypeKindSem::Function)
        paramTypes = methodType->paramTypes();
    Type ilReturnType = mapType(returnType);

    // Build argument list: self + call args
    std::vector<Value> args;
    args.reserve(paramTypes.size() + 1);
    args.push_back(selfValue);
    std::vector<Value> boundArgs =
        lowerResolvedCallArgs(expr, paramTypes, method ? &method->params : nullptr);
    args.insert(args.end(), boundArgs.begin(), boundArgs.end());

    // Look up the method's slot index in the interface
    size_t slotIdx = ifaceInfo.findSlot(slotKey);
    if (slotIdx == SIZE_MAX) {
        // Fallback: method not in interface slot map (shouldn't happen)
        return {Value::constInt(0), ilReturnType};
    }

    // Get object's class ID (from heap header, works for Zia objects)
    Value classId = emitCallRet(Type(Type::Kind::I64), "rt_obj_class_id", {selfValue});

    // Emit itable lookup: rt_get_interface_impl(classId, ifaceId) -> Ptr to itable
    Value itable = emitCallRet(Type(Type::Kind::Ptr),
                               "rt_get_interface_impl",
                               {classId, Value::constInt(static_cast<int64_t>(ifaceInfo.ifaceId))});

    // Load function pointer from itable[slotIdx]
    int64_t offset = static_cast<int64_t>(slotIdx * 8ULL);
    Value entryPtr =
        emitBinary(Opcode::GEP, Type(Type::Kind::Ptr), itable, Value::constInt(offset));
    Value fnPtr = emitLoad(entryPtr, Type(Type::Kind::Ptr));

    // Emit indirect call through function pointer
    if (ilReturnType.kind == Type::Kind::Void) {
        emitCallIndirect(fnPtr, args);
        return {Value::constInt(0), Type(Type::Kind::Void)};
    }
    Value result = emitCallIndirectRet(ilReturnType, fnPtr, args);
    return materializeCallResult(result, returnType, ilReturnType);
}

} // namespace il::frontends::zia
