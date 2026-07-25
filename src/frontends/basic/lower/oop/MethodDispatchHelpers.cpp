//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/lower/oop/MethodDispatchHelpers.cpp
// Purpose: Implementation of OOP method dispatch helpers for BASIC lowering.
// Key invariants: See MethodDispatchHelpers.hpp for documentation.
// Ownership/Lifetime: Non-owning reference to Lowerer context.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements method target resolution and reusable array bounds checks
///        for BASIC OOP lowering.

#include "MethodDispatchHelpers.hpp"

#include "frontends/basic/DiagnosticEmitter.hpp"
#include "frontends/basic/ILTypeUtils.hpp"
#include "frontends/basic/Lowerer.hpp"
#include "frontends/basic/NameMangler_OOP.hpp"
#include "frontends/basic/StringUtils.hpp"
#include "frontends/basic/sem/OverloadResolution.hpp"
#include "frontends/basic/sem/RuntimeMethodIndex.hpp"
#include "il/runtime/classes/RuntimeClasses.hpp"

namespace il::frontends::basic {

// ============================================================================
// MethodDispatchResolver
// ============================================================================

/// @brief Bind a dispatch resolver to its lowering context (non-owning).
/// @param lowerer Lowerer that outlives this resolver.
MethodDispatchResolver::MethodDispatchResolver(Lowerer &lowerer) noexcept : lowerer_(lowerer) {}

/// @brief Check whether a method is accessible from the current class.
/// @param classInfo The class declaring the method.
/// @param methodName The method being accessed.
/// @param loc Call-site location (unused beyond signature symmetry).
/// @return An error message if the method is private and accessed from outside its class;
///         otherwise an empty string.
std::string MethodDispatchResolver::checkAccessControl(const ClassInfo &classInfo,
                                                       const std::string &methodName,
                                                       il::support::SourceLoc loc) {
    auto it = classInfo.methods.find(methodName);
    if (it != classInfo.methods.end() && it->second.sig.access == Access::Private &&
        lowerer_.currentClass() != classInfo.qualifiedName) {
        return "cannot access private member '" + methodName + "' of class '" +
               classInfo.qualifiedName + "'";
    }
    return {};
}

/// @brief Resolve a static method call to a concrete dispatch target.
/// @param classQName Qualified class name.
/// @param methodName Method name.
/// @param argTypes Argument types (for overload selection).
/// @param loc Call-site location for diagnostics.
/// @return A Resolution: Direct (mangled target) for a user static method, RuntimeCatalog for a
///         runtime class method, or Unresolved on failure.
MethodDispatchResolver::Resolution MethodDispatchResolver::resolveStaticCall(
    const std::string &classQName,
    const std::string &methodName,
    const std::vector<Type> &argTypes,
    il::support::SourceLoc loc) {
    Resolution result;

    // Check user-defined class first
    if (const ClassInfo *ci = lowerer_.oopIndex_.findClass(classQName)) {
        // Resolve overload
        std::string selected = methodName;
        if (auto resolved = sem::resolveMethodOverload(lowerer_.oopIndex_,
                                                       classQName,
                                                       methodName,
                                                       /*isStatic*/ true,
                                                       argTypes,
                                                       lowerer_.currentClass(),
                                                       lowerer_.diagnosticEmitter(),
                                                       loc)) {
            selected = resolved->methodName;
        } else if (lowerer_.diagnosticEmitter()) {
            result.kind = Resolution::Kind::Unresolved;
            return result;
        }

        auto it = ci->methods.find(selected);
        if (it != ci->methods.end() && it->second.isStatic) {
            result.kind = Resolution::Kind::Direct;
            result.target = mangleMethod(ci->qualifiedName, selected);
            result.hasReceiver = false;

            // Determine return type
            if (auto retType = lowerer_.findMethodReturnType(classQName, selected)) {
                result.returnKind = type_conv::astToIlType(*retType).kind;
            }
            return result;
        }
    }

    // Try runtime catalog
    if (auto catalogResult = tryRuntimeCatalog(classQName, methodName, argTypes.size())) {
        return *catalogResult;
    }

    result.kind = Resolution::Kind::Unresolved;
    return result;
}

/// @brief Resolve an instance method call to a concrete dispatch strategy.
/// @param receiverClassQName Qualified class of the receiver.
/// @param methodName Method name.
/// @param argTypes Argument types (for overload selection).
/// @param isBaseQualified True for `BASE.method(...)` calls (forces direct dispatch to base).
/// @param loc Call-site location for diagnostics.
/// @return A Resolution: Virtual (with vtable slot) for an overridable method, or Direct
///         (mangled target on the declaring/base class); Unresolved (possibly access-denied)
///         on failure. Return-type kind is filled from the declaring class.
MethodDispatchResolver::Resolution MethodDispatchResolver::resolveInstanceCall(
    const std::string &receiverClassQName,
    const std::string &methodName,
    const std::vector<Type> &argTypes,
    bool isBaseQualified,
    il::support::SourceLoc loc) {
    Resolution result;
    result.hasReceiver = true;

    if (receiverClassQName.empty()) {
        result.kind = Resolution::Kind::Unresolved;
        return result;
    }

    // Access control check
    if (const ClassInfo *ci = lowerer_.oopIndex_.findClass(receiverClassQName)) {
        std::string accessError = checkAccessControl(*ci, methodName, loc);
        if (!accessError.empty()) {
            result.kind = Resolution::Kind::Unresolved;
            result.accessDenied = true;
            result.accessErrorMsg = accessError;
            return result;
        }
    }

    // Resolve overload
    std::string selected = methodName;
    // BUG-OOP-002/003 fix: Track the declaring class for inherited method dispatch
    std::string declaringClass = receiverClassQName;
    if (auto resolved = sem::resolveMethodOverload(lowerer_.oopIndex_,
                                                   receiverClassQName,
                                                   methodName,
                                                   /*isStatic*/ false,
                                                   argTypes,
                                                   lowerer_.currentClass(),
                                                   lowerer_.diagnosticEmitter(),
                                                   loc)) {
        selected = resolved->methodName;
        declaringClass = resolved->qualifiedClass; // Use declaring class for dispatch
    } else if (lowerer_.diagnosticEmitter()) {
        result.kind = Resolution::Kind::Unresolved;
        return result;
    }

    // Determine virtual slot - use declaring class for inherited methods
    int slot = getVirtualSlot(lowerer_.oopIndex_, declaringClass, selected);

    // Determine target class for direct dispatch - use declaring class for inherited methods
    std::string directQClass = declaringClass;
    if (isBaseQualified) {
        const std::string cur = lowerer_.currentClass();
        if (!cur.empty()) {
            if (const ClassInfo *ci = lowerer_.oopIndex_.findClass(cur)) {
                if (!ci->baseQualified.empty())
                    directQClass = ci->baseQualified;
            }
        }
    }

    // Virtual dispatch (not BASE-qualified)
    if (slot >= 0 && !isBaseQualified) {
        result.kind = Resolution::Kind::Virtual;
        result.slot = slot;
        // BUG-OOP-002/003 fix: Use declaring class for return type lookup
        if (auto retType = lowerer_.findMethodReturnType(declaringClass, selected)) {
            result.returnKind = type_conv::astToIlType(*retType).kind;
        }
        return result;
    }

    // Direct call
    result.kind = Resolution::Kind::Direct;
    const ClassInfo *targetCI = lowerer_.oopIndex_.findClass(directQClass);
    std::string emitClassName = targetCI ? targetCI->qualifiedName : directQClass;
    result.target = emitClassName.empty() ? selected : mangleMethod(emitClassName, selected);

    // Return type lookup - use base class for BASE-qualified calls
    const std::string retClassLookup = isBaseQualified ? directQClass : receiverClassQName;
    if (auto retType = lowerer_.findMethodReturnType(retClassLookup, selected)) {
        result.returnKind = type_conv::astToIlType(*retType).kind;
    }

    return result;
}

/// @brief Resolve an interface method call to its itable slot.
/// @param interfaceQName Qualified interface name.
/// @param methodName Method name.
/// @param argCount Argument count (used to disambiguate same-named slots).
/// @return An Interface resolution carrying the slot index and interface id, or Unresolved if
///         the interface or a matching slot is not found. Prefers an arity match, else the
///         first name match.
MethodDispatchResolver::Resolution MethodDispatchResolver::resolveInterfaceCall(
    const std::string &interfaceQName, const std::string &methodName, std::size_t argCount) {
    Resolution result;
    result.hasReceiver = true;

    // Find interface in OOP index
    const InterfaceInfo *iface = nullptr;
    for (const auto &p : lowerer_.oopIndex_.interfacesByQname()) {
        if (p.first == interfaceQName) {
            iface = &p.second;
            break;
        }
    }

    if (!iface) {
        result.kind = Resolution::Kind::Unresolved;
        return result;
    }

    // Find slot by method name and arity
    int slotIndex = -1;
    for (std::size_t idx = 0; idx < iface->slots.size(); ++idx) {
        const auto &sig = iface->slots[idx];
        if (sig.name != methodName)
            continue;
        if (sig.paramTypes.size() == argCount) {
            slotIndex = static_cast<int>(idx);
            break;
        }
        // Fallback: first name match
        if (slotIndex < 0)
            slotIndex = static_cast<int>(idx);
    }

    if (slotIndex < 0) {
        result.kind = Resolution::Kind::Unresolved;
        return result;
    }

    result.kind = Resolution::Kind::Interface;
    result.slot = slotIndex;
    result.ifaceId = iface->ifaceId;

    // Return type from interface signature
    if (slotIndex >= 0 && static_cast<std::size_t>(slotIndex) < iface->slots.size()) {
        if (iface->slots[static_cast<std::size_t>(slotIndex)].returnType) {
            result.returnKind = type_conv::astToIlType(
                                    *iface->slots[static_cast<std::size_t>(slotIndex)].returnType)
                                    .kind;
        }
    }

    return result;
}

/// @brief Try to resolve a method against the runtime class catalog.
/// @param classQName Qualified class name.
/// @param methodName Method name.
/// @param argCount Argument count.
/// @return A RuntimeCatalog resolution (with target symbol, return kind, expected args), or
///         nullopt when @p classQName is not a runtime class or the method is not found.
std::optional<MethodDispatchResolver::Resolution> MethodDispatchResolver::tryRuntimeCatalog(
    const std::string &classQName, const std::string &methodName, std::size_t argCount) {
    // Check if this is a runtime class
    if (!il::runtime::findRuntimeClassByQName(classQName))
        return std::nullopt;

    auto &midx = runtimeMethodIndex();
    auto info = midx.find(classQName, methodName, argCount);
    if (!info)
        return std::nullopt;

    Resolution result;
    result.kind = Resolution::Kind::RuntimeCatalog;
    result.target = info->target;
    result.hasReceiver = false; // Static calls don't have receiver
    result.returnKind = basicTypeToILKind(info->ret);
    result.expectedArgs = info->args;

    return result;
}

// ============================================================================
// BoundsCheckEmitter
// ============================================================================

/// @brief Bind a bounds-check emitter to its lowering context (non-owning).
/// @brief Binds a bounds-check emitter to its lowering context.
/// @param lowerer Lowerer that owns the active function and emission helpers.
BoundsCheckEmitter::BoundsCheckEmitter(Lowerer &lowerer) noexcept : lowerer_(lowerer) {}

/// @brief Emit a runtime array bounds check for an index access.
/// @param arrHandle The array handle value.
/// @param index The index value to validate.
/// @param elemKind Element kind (selects the length helper: str/obj/i64).
/// @param isObjectArray Whether the array stores objects (uses the obj length helper).
/// @param loc Source location for diagnostics.
/// @param labelPrefix Prefix for the generated ok/oob block labels (defaults to "bc").
/// @details Loads the array length, tests `index < 0 || index >= len`, and branches: the OOB
///          path calls `rt_arr_oob_panic` and traps; control continues in the ok block.
void BoundsCheckEmitter::emitBoundsCheck(il::core::Value arrHandle,
                                         il::core::Value index,
                                         Type elemKind,
                                         bool isObjectArray,
                                         il::support::SourceLoc loc,
                                         std::string_view labelPrefix) {
    lowerer_.curLoc = loc;

    // Get array length using appropriate helper
    il::core::Value len;
    if (elemKind == Type::Str) {
        lowerer_.requireArrayStrLen();
        len = lowerer_.emitCallRet(
            il::core::Type(il::core::Type::Kind::I64), "rt_arr_str_len", {arrHandle});
    } else if (isObjectArray) {
        lowerer_.requireArrayObjLen();
        len = lowerer_.emitCallRet(
            il::core::Type(il::core::Type::Kind::I64), "rt_arr_obj_len", {arrHandle});
    } else {
        lowerer_.requireArrayI64Len();
        len = lowerer_.emitCallRet(
            il::core::Type(il::core::Type::Kind::I64), "rt_arr_i64_len", {arrHandle});
    }

    // Check index < 0 || index >= len
    il::core::Value isNeg = lowerer_.emitBinary(
        il::core::Opcode::SCmpLT, lowerer_.ilBoolTy(), index, il::core::Value::constInt(0));
    il::core::Value tooHigh =
        lowerer_.emitBinary(il::core::Opcode::SCmpGE, lowerer_.ilBoolTy(), index, len);

    auto emitc = lowerer_.emitCommon(loc);
    il::core::Value isNeg64 = emitc.widen_to(isNeg, 1, 64, Signedness::Unsigned);
    il::core::Value tooHigh64 = emitc.widen_to(tooHigh, 1, 64, Signedness::Unsigned);
    il::core::Value oobInt = emitc.logical_or(isNeg64, tooHigh64);
    il::core::Value oobCond = lowerer_.emitBinary(
        il::core::Opcode::ICmpNe, lowerer_.ilBoolTy(), oobInt, il::core::Value::constInt(0));

    // Create ok/oob blocks
    ProcedureContext &ctx = lowerer_.context();
    il::core::Function *func = ctx.function();
    std::size_t curIdx = ctx.currentIndex();
    unsigned bcId = ctx.consumeBoundsCheckId();
    BlockNamer *blockNamer = ctx.blockNames().namer();

    std::string prefix(labelPrefix);
    if (prefix.empty())
        prefix = "bc";

    std::size_t okIdx = func->blocks.size();
    std::string okLbl = blockNamer ? blockNamer->tag(prefix + "_ok" + std::to_string(bcId))
                                   : lowerer_.mangler.block(prefix + "_ok" + std::to_string(bcId));
    lowerer_.builder->addBlock(*func, okLbl);

    std::size_t oobIdx = func->blocks.size();
    std::string oobLbl = blockNamer
                             ? blockNamer->tag(prefix + "_oob" + std::to_string(bcId))
                             : lowerer_.mangler.block(prefix + "_oob" + std::to_string(bcId));
    lowerer_.builder->addBlock(*func, oobLbl);

    il::core::BasicBlock *ok = &func->blocks[okIdx];
    il::core::BasicBlock *oob = &func->blocks[oobIdx];

    // Emit conditional branch
    ctx.setCurrent(&func->blocks[curIdx]);
    lowerer_.emitCBr(oobCond, oob, ok);

    // OOB path: panic and trap
    ctx.setCurrent(oob);
    lowerer_.requireArrayOobPanic();
    lowerer_.emitCall("rt_arr_oob_panic", {index, len});
    lowerer_.emitTrap();

    // Continue in ok block
    ctx.setCurrent(ok);
}

/// @brief Flatten multi-dimensional array indices into a single linear (row-major) index.
/// @param indices One index value per dimension.
/// @param extents Inclusive upper bounds per dimension (BASIC convention; length = extent + 1).
/// @param loc Source location for the emitted arithmetic.
/// @return The linearized index value. Emits a diagnostic/trap when rank metadata is missing.
il::core::Value BoundsCheckEmitter::computeFlattenedIndex(
    const std::vector<il::core::Value> &indices,
    const std::vector<long long> &extents,
    il::support::SourceLoc loc) {
    if (indices.empty())
        return il::core::Value::constInt(0);

    if (indices.size() == 1)
        return indices[0];

    if (extents.size() != indices.size()) {
        if (auto *em = lowerer_.diagnosticEmitter()) {
            em->emit(il::support::Severity::Error,
                     "B2000",
                     loc,
                     1,
                     "cannot flatten multidimensional array index without matching extents");
        }
        lowerer_.emitTrap();
        return il::core::Value::constInt(0);
    }

    lowerer_.curLoc = loc;
    return lowerer_.emitRowMajorFlatIndex(indices, extents);
}

} // namespace il::frontends::basic
