//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/lower/oop/Lower_OOP_RuntimeHelpers.cpp
// Purpose: Implementation of consolidated OOP runtime emission helpers.
// Key invariants: Centralizes patterns for parameter initialization, array field
//                 allocation, and method epilogue. (BUG-056, BUG-073, etc.)
// Ownership/Lifetime: Operates on Lowerer state without owning AST or module.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements reusable parameter, field-array, epilogue, body, and
///        dispatch-table helpers for BASIC OOP emission.

#include "frontends/basic/lower/oop/Lower_OOP_RuntimeHelpers.hpp"
#include "frontends/basic/ILTypeUtils.hpp"
#include "frontends/basic/Lowerer.hpp"

#include <unordered_set>

namespace il::frontends::basic {

namespace {
using AstType = ::il::frontends::basic::Type;
using IlType = il::core::Type;
using Value = il::core::Value;
using Opcode = il::core::Opcode;
} // namespace

/// @brief Bind an OOP emission helper to its lowering context (non-owning).
/// @param lowerer Lowerer that outlives this helper.
OopEmitHelper::OopEmitHelper(Lowerer &lowerer) noexcept : lowerer_(lowerer) {}

// -------------------------------------------------------------------------
// Parameter Initialization
// -------------------------------------------------------------------------

/// @brief Emit slot allocation and the incoming-value store for one method/procedure parameter.
/// @param param The parameter declaration.
/// @param fn The IL function being built (supplies the incoming block-parameter value).
/// @param paramIdx Index of this parameter's value in @p fn's params.
/// @param[in,out] paramNames Set of parameter names seen so far (this name is inserted).
/// @details Allocates a 1-byte slot for BOOLEAN, else 8 bytes; preserves object-class typing so
///          member calls resolve (BUG-073); marks arrays and stores object/non-object arrays
///          through the appropriate path (BUG-OOP-038); records the symbol and its slot.
void OopEmitHelper::emitParamInit(const Param &param,
                                  il::core::Function &fn,
                                  std::size_t paramIdx,
                                  std::unordered_set<std::string> &paramNames) {
    paramNames.insert(param.name);
    lowerer_.registerProcParam(param.name); // BUG-BAS-002 fix
    lowerer_.curLoc = param.loc;

    // Allocate slot: BOOLEAN uses 1 byte, everything else 8 bytes
    Value slot = lowerer_.emitAlloca((!param.is_array && param.type == AstType::Bool) ? 1 : 8);

    if (param.is_array) {
        lowerer_.markArray(param.name);
        lowerer_.emitStore(IlType(IlType::Kind::Ptr), slot, Value::null());
    }

    // Preserve object-class typing for parameters so member calls resolve. (BUG-073)
    if (!param.objectClass.empty())
        lowerer_.setSymbolObjectType(param.name, lowerer_.qualify(param.objectClass));
    else
        lowerer_.setSymbolType(param.name, param.type);

    lowerer_.markSymbolReferenced(param.name);
    auto &info = lowerer_.ensureSymbol(param.name);
    info.slotId = slot.id;

    // Determine IL type for the parameter
    IlType ilParamTy = (!param.objectClass.empty() || param.is_array)
                           ? IlType(IlType::Kind::Ptr)
                           : type_conv::astToIlType(param.type);

    Value incoming = Value::temp(fn.params[paramIdx].id);
    if (param.is_array) {
        // Object arrays require distinct runtime calls. (BUG-OOP-038)
        bool isObjectArray = !param.objectClass.empty();
        lowerer_.storeArray(slot, incoming, param.type, isObjectArray);
    } else
        lowerer_.emitStore(ilParamTy, slot, incoming);
}

/// @brief Emit parameter initialization for every parameter of a procedure/method.
/// @param params The parameter declarations.
/// @param fn The IL function being built.
/// @param selfOffset Index offset of the first user parameter (1 for instance methods that take
///        an implicit `self`, 0 otherwise).
/// @param[in,out] paramNames Accumulates the initialized parameter names.
void OopEmitHelper::emitAllParamInits(const std::vector<Param> &params,
                                      il::core::Function &fn,
                                      std::size_t selfOffset,
                                      std::unordered_set<std::string> &paramNames) {
    for (std::size_t i = 0; i < params.size(); ++i) {
        emitParamInit(params[i], fn, selfOffset + i, paramNames);
    }
}

// -------------------------------------------------------------------------
// Array Field Initialization
// -------------------------------------------------------------------------

/// @brief Allocate and initialize the array-typed fields of a newly constructed object.
/// @param klass The class declaration (provides field extents/types).
/// @param selfSlotId Slot id holding the `self` pointer.
/// @details For each array field, computes the total length from inclusive BASIC extents,
///          allocates the matching runtime array (str/object/i64), and stores the handle into
///          the field via a GEP at the field's layout offset.
void OopEmitHelper::emitArrayFieldInits(const ClassDecl &klass, unsigned selfSlotId) {
    const ClassLayout *layout = lowerer_.findClassLayout(klass.name);
    if (!layout)
        return;

    Value selfPtr = lowerer_.loadSelfPointer(selfSlotId);

    for (const auto &field : klass.fields) {
        if (!field.isArray || field.arrayExtents.empty())
            continue;

        // Compute total length as the product of inclusive extents
        // BASIC DIM uses inclusive upper bounds (e.g., DIM a(7) => 8 elements)
        long long total = 1;
        for (long long e : field.arrayExtents)
            total *= (e + 1);
        Value length = Value::constInt(total);

        // Find field offset in layout
        const auto *fi = layout->findField(field.name);
        if (!fi)
            continue;

        // Allocate appropriate array type
        Value handle;
        if (field.type == AstType::Str) {
            lowerer_.requireArrayStrAlloc();
            handle = lowerer_.emitCallRet(IlType(IlType::Kind::Ptr), "rt_arr_str_alloc", {length});
        } else if (!field.objectClassName.empty()) {
            // Object-typed fields use object array allocation. (BUG-089)
            lowerer_.requireArrayObjNew();
            handle = lowerer_.emitCallRet(IlType(IlType::Kind::Ptr), "rt_arr_obj_new", {length});
        } else {
            lowerer_.requireArrayI64New();
            handle = lowerer_.emitCallRet(IlType(IlType::Kind::Ptr), "rt_arr_i64_new", {length});
        }

        // Store handle into object field
        Value fieldPtr = lowerer_.emitBinary(Opcode::GEP,
                                             IlType(IlType::Kind::Ptr),
                                             selfPtr,
                                             Value::constInt(static_cast<long long>(fi->offset)));
        lowerer_.emitStore(IlType(IlType::Kind::Ptr), fieldPtr, handle);
    }
}

// -------------------------------------------------------------------------
// Method Epilogue
// -------------------------------------------------------------------------

/// @brief Emit the method exit cleanup that releases owned locals.
/// @param paramNames Borrowed parameter names — their arrays are not released (BUG-105).
/// @param excludeFromObjRelease Object locals excluded from release (e.g. the returned value).
/// @details Releases deferred temporaries, then object locals, then array locals; callers retain
///          ownership of borrowed parameters.
void OopEmitHelper::emitMethodEpilogue(
    const std::unordered_set<std::string> &paramNames,
    const std::unordered_set<std::string> &excludeFromObjRelease) {
    lowerer_.curLoc = {};
    lowerer_.releaseDeferredTemps();
    std::unordered_set<std::string> objectReleaseSkips = paramNames;
    objectReleaseSkips.insert(excludeFromObjRelease.begin(), excludeFromObjRelease.end());
    lowerer_.releaseObjectLocals(objectReleaseSkips);
    // Borrowed parameters are not released; caller owns their lifetime. (BUG-105)
    lowerer_.releaseArrayLocals(paramNames);
}

// -------------------------------------------------------------------------
// Body Statement Lowering
// -------------------------------------------------------------------------

/// @brief Lower a method/procedure body and branch to its shared exit block.
/// @param bodyStmts The body statements.
/// @param exitIdx Block index of the exit (epilogue) block.
/// @details An empty body branches straight to the exit. Otherwise statements are lowered and,
///          if control falls through (the current block is not terminated), a branch to the
///          exit block is appended.
void OopEmitHelper::emitBodyAndBranchToExit(const std::vector<const Stmt *> &bodyStmts,
                                            std::size_t exitIdx) {
    auto &ctx = lowerer_.context();

    if (bodyStmts.empty()) {
        lowerer_.curLoc = {};
        il::core::Function *func = ctx.function();
        il::core::BasicBlock *exitBlock = &func->blocks[exitIdx];
        lowerer_.emitBr(exitBlock);
    } else {
        lowerer_.lowerStatementSequence(bodyStmts, /*stopOnTerminated=*/true);
        if (ctx.current() && !ctx.current()->terminated) {
            il::core::Function *func = ctx.function();
            il::core::BasicBlock *exitBlock = &func->blocks[exitIdx];
            lowerer_.emitBr(exitBlock);
        }
    }
}

// -------------------------------------------------------------------------
// VTable/ITable Population (duplicated logic consolidated)
// -------------------------------------------------------------------------

/// @brief Find the class that actually implements a method, walking up from a starting class.
/// @param oopIndex OOP index for class/inheritance lookups.
/// @param startQClass Qualified class to start the search from.
/// @param methodName Method to locate.
/// @return The qualified name of the nearest ancestor (or self) with a non-abstract definition,
///         or @p startQClass as a fallback.
std::string OopEmitHelper::findImplementorClass(const OopIndex &oopIndex,
                                                const std::string &startQClass,
                                                const std::string &methodName) {
    const ClassInfo *cur = oopIndex.findClass(startQClass);
    while (cur) {
        auto itM = cur->methods.find(methodName);
        if (itM != cur->methods.end()) {
            if (!itM->second.isAbstract)
                return cur->qualifiedName;
        }
        if (cur->baseQualified.empty())
            break;
        cur = oopIndex.findClass(cur->baseQualified);
    }
    return startQClass; // fallback
}

/// @brief Build the vtable slot → method-name map for a class.
/// @param oopIndex OOP index for class/inheritance lookups.
/// @param classQName Qualified class whose vtable is being built.
/// @param[out] maxSlot Receives the highest virtual slot index encountered.
/// @return A vector indexed by slot, holding the method name occupying each slot (empty for a
///         class with no virtual methods).
/// @details Two passes over the inheritance chain: the first computes the slot count, the second
///          fills names. Walking most-derived first lets a derived override win its slot.
std::vector<std::string> OopEmitHelper::buildVtableSlotMap(const OopIndex &oopIndex,
                                                           const std::string &classQName,
                                                           std::size_t &maxSlot) {
    maxSlot = 0;
    bool hasAnyVirtual = false;

    // First pass: compute max slot
    {
        const ClassInfo *cur = oopIndex.findClass(classQName);
        while (cur) {
            for (const auto &mp : cur->methods) {
                const auto &mi = mp.second;
                if (!mi.isVirtual || mi.slot < 0)
                    continue;
                hasAnyVirtual = true;
                maxSlot = std::max<std::size_t>(maxSlot, static_cast<std::size_t>(mi.slot));
            }
            if (cur->baseQualified.empty())
                break;
            cur = oopIndex.findClass(cur->baseQualified);
        }
    }

    const std::size_t slotCount = hasAnyVirtual ? (maxSlot + 1) : 0;
    std::vector<std::string> slotToName(slotCount);

    // Second pass: build slot-to-name mapping
    {
        const ClassInfo *cur = oopIndex.findClass(classQName);
        while (cur) {
            for (const auto &mp : cur->methods) {
                const auto &mname = mp.first;
                const auto &mi = mp.second;
                if (!mi.isVirtual || mi.slot < 0)
                    continue;
                const std::size_t s = static_cast<std::size_t>(mi.slot);
                if (s < slotToName.size())
                    slotToName[s] = mname; // prefer most-derived assignment first in walk
            }
            if (cur->baseQualified.empty())
                break;
            cur = oopIndex.findClass(cur->baseQualified);
        }
    }

    return slotToName;
}

} // namespace il::frontends::basic
