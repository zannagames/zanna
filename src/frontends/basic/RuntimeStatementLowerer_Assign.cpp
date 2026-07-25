//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/RuntimeStatementLowerer_Assign.cpp
// Purpose: Implementation of assignment-related runtime statement lowering.
//          Handles scalar slot assignments, array element assignments, and
//          the common assignment coercion logic.
// Key invariants: Maintains Lowerer's runtime lowering semantics exactly.
// Ownership/Lifetime: Borrows Lowerer reference; coordinates with parent.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/// @file RuntimeStatementLowerer_Assign.cpp
/// @brief Implements lifetime-aware scalar and array-element assignment.
/// @details Scalar stores coerce to slot representation and balance old/new
///          string or object ownership. Array stores resolve local, module, and
///          member-array metadata before selecting the element-specific runtime
///          helper.

#include "Lowerer.hpp"
#include "RuntimeStatementLowerer.hpp"
#include "frontends/basic/ASTUtils.hpp"
#include "frontends/basic/LocationScope.hpp"
#include "frontends/basic/NameMangler_OOP.hpp"
#include "frontends/basic/StringUtils.hpp"
#include "frontends/basic/lower/MemberArrayResolver.hpp"
#include "il/runtime/RuntimeClassNames.hpp"

#include <cassert>

using namespace il::core;
using il::runtime::RuntimeFeature;
using AstType = ::il::frontends::basic::Type;

namespace il::frontends::basic {

/// @brief Assign a value to a scalar slot with BASIC-compatible coercions.
///
/// @details Handles boolean conversion, floating/integer promotion and
///          demotion, string retain/release, and object lifetime maintenance.
///          Runtime string objects are treated as string slots. Object stores
///          retain the incoming pointer, release the old pointer, conditionally
///          invoke an available class destructor, free the final reference, and
///          continue before storing the new pointer.
///
/// @param slotInfo Metadata describing the target slot's type and traits.
/// @param slot     Value referencing the storage location.
/// @param value    Lowered right-hand side value paired with its type.
/// @param loc      Source location for diagnostics and helper calls.
void RuntimeStatementLowerer::assignScalarSlot(const Lowerer::SlotType &slotInfo,
                                               Lowerer::Value slot,
                                               Lowerer::RVal value,
                                               il::support::SourceLoc loc) {
    LocationScope location(lowerer_, loc);
    il::core::Type targetTy = slotInfo.type;
    const bool isRuntimeStringObject =
        slotInfo.isObject &&
        (string_utils::iequals(slotInfo.objectClass, il::runtime::RTCLASS_STRING) ||
         string_utils::iequals(slotInfo.objectClass, "Zanna.System.String"));
    if (isRuntimeStringObject)
        targetTy = il::core::Type(il::core::Type::Kind::Str);
    bool isStr = targetTy.kind == il::core::Type::Kind::Str;
    bool isF64 = targetTy.kind == il::core::Type::Kind::F64;
    bool isBool = slotInfo.isBoolean;

    if (!isStr && !isF64 && !isBool && value.type.kind == il::core::Type::Kind::I1) {
        value = lowerer_.coerceToI64(std::move(value), loc);
    }
    if (isF64 && value.type.kind == il::core::Type::Kind::I64) {
        value = lowerer_.coerceToF64(std::move(value), loc);
    } else if (!isStr && !isF64 && !isBool && value.type.kind == il::core::Type::Kind::F64) {
        value = lowerer_.coerceToI64(std::move(value), loc);
    }

    if (targetTy.kind == il::core::Type::Kind::I1 && value.type.kind != il::core::Type::Kind::I1) {
        value = lowerer_.coerceToBool(std::move(value), loc);
    }

    if (isStr) {
        lowerer_.requireStrRetainMaybe();
        lowerer_.emitCall("rt_str_retain_maybe", {value.value});
        lowerer_.requireStrReleaseMaybe();
        Value oldValue = lowerer_.emitLoad(targetTy, slot);
        lowerer_.emitCall("rt_str_release_maybe", {oldValue});
    }

    else if (slotInfo.isObject) {
        lowerer_.requestHelper(RuntimeFeature::ObjReleaseChk0);
        lowerer_.requestHelper(RuntimeFeature::ObjFree);
        lowerer_.requestHelper(RuntimeFeature::ObjRetainMaybe);

        lowerer_.emitCall("rt_obj_retain_maybe", {value.value});
        Value oldValue = lowerer_.emitLoad(il::core::Type(il::core::Type::Kind::Ptr), slot);
        Value shouldDestroy =
            lowerer_.emitCallRet(lowerer_.ilBoolTy(), "rt_obj_release_check0", {oldValue});

        ProcedureContext &ctx = lowerer_.context();
        Function *func = ctx.function();
        BasicBlock *origin = ctx.current();
        if (func && origin) {
            std::size_t originIdx = ctx.blockIndex(origin);
            BlockNamer *blockNamer = ctx.blockNames().namer();
            std::string base = "obj_assign";
            std::string destroyLbl = blockNamer ? blockNamer->generic(base + "_dtor")
                                                : lowerer_.mangler.block(base + "_dtor");
            std::string contLbl = blockNamer ? blockNamer->generic(base + "_cont")
                                             : lowerer_.mangler.block(base + "_cont");

            std::size_t destroyIdx = func->blocks.size();
            lowerer_.builder->addBlock(*func, destroyLbl);
            std::size_t contIdx = func->blocks.size();
            lowerer_.builder->addBlock(*func, contLbl);

            BasicBlock *destroyBlk = &func->blocks[destroyIdx];
            BasicBlock *contBlk = &func->blocks[contIdx];

            ctx.setCurrent(&func->blocks[originIdx]);
            lowerer_.emitCBr(shouldDestroy, destroyBlk, contBlk);

            ctx.setCurrent(destroyBlk);
            if (!slotInfo.objectClass.empty()) {
                std::string dtor = mangleClassDtor(slotInfo.objectClass);
                bool haveDtor = false;
                if (lowerer_.mod) {
                    for (const auto &fn : lowerer_.mod->functions) {
                        if (fn.name == dtor) {
                            haveDtor = true;
                            break;
                        }
                    }
                }
                if (haveDtor)
                    lowerer_.emitCall(dtor, {oldValue});
            }
            lowerer_.emitCall("rt_obj_free", {oldValue});
            lowerer_.emitBr(contBlk);

            ctx.setCurrent(contBlk);
        }

        targetTy = il::core::Type(il::core::Type::Kind::Ptr);
    }

    lowerer_.emitStore(targetTy, slot, value.value);
}

/// @brief Store a value into a BASIC array element with range checks.
///
/// @details Loads the target array metadata, evaluates the index expression,
///          resolves implicit/dotted member and module object-array information,
///          and then selects string, object, F64, or I64 storage. Numeric values
///          are coerced to the target representation; string/object helpers own
///          their element lifetime semantics.
///
/// @param target Array expression describing the destination element.
/// @param value  Lowered right-hand side value being assigned.
/// @param loc    Source location for diagnostics and helper invocations.
void RuntimeStatementLowerer::assignArrayElement(const ArrayExpr &target,
                                                 Lowerer::RVal value,
                                                 il::support::SourceLoc loc) {
    LocationScope location(lowerer_, loc);

    Lowerer::ArrayAccess access =
        lowerer_.lowerArrayAccess(target, Lowerer::ArrayAccessKind::Store);

    // =========================================================================
    // Member Array Field Resolution (BUG-056, BUG-058, BUG-089, BUG-108)
    // =========================================================================
    // Consolidated via resolveMemberArrayField(). See MemberArrayResolver.cpp
    // for the unified resolution logic covering dotted member arrays, implicit
    // field arrays, object array detection, and local variable shadowing.
    // =========================================================================
    const auto *info = lowerer_.findSymbol(target.name);
    const MemberArrayInfo fieldInfo = lowerer_.resolveMemberArrayField(target.name);
    const bool isMemberArray = fieldInfo.isDottedAccess;
    const bool isImplicitFieldArray = fieldInfo.isField && !fieldInfo.isDottedAccess;
    std::string moduleObjectClass;
    if (!isMemberArray && (!info || !info->isObject))
        moduleObjectClass = lowerer_.lookupModuleArrayElemClass(target.name);

    // For implicit field arrays, recompute base as ME.<field> to ensure we are
    // storing into the instance field array even when the name is unqualified.
    if (isImplicitFieldArray) {
        if (const auto *scope = lowerer_.activeFieldScope(); scope && scope->layout) {
            const auto *selfInfo = lowerer_.findSymbol("ME");
            if (selfInfo && selfInfo->slotId) {
                lowerer_.curLoc = loc;
                Value selfPtr = lowerer_.emitLoad(il::core::Type(il::core::Type::Kind::Ptr),
                                                  Value::temp(*selfInfo->slotId));
                lowerer_.curLoc = loc;
                long long offset = 0;
                if (const Lowerer::ClassLayout::Field *f2 = scope->layout->findField(target.name))
                    offset = static_cast<long long>(f2->offset);
                Value fieldPtr = lowerer_.emitBinary(Opcode::GEP,
                                                     il::core::Type(il::core::Type::Kind::Ptr),
                                                     selfPtr,
                                                     Value::constInt(offset));
                lowerer_.curLoc = loc;
                access.base =
                    lowerer_.emitLoad(il::core::Type(il::core::Type::Kind::Ptr), fieldPtr);
            }
        }
    }

    // The target array element type selects the runtime helper; RHS type only
    // controls coercion into that target representation.
    if ((info && info->type == AstType::Str) ||
        (fieldInfo.isField && fieldInfo.elementAstType == ::il::frontends::basic::Type::Str)) {
        // String array: use rt_arr_str_put (handles retain/release)
        // Pass the string handle directly - the C runtime expects rt_string by value.
        lowerer_.emitCall("rt_arr_str_put", {access.base, access.index, value.value});
    } else if ((!isMemberArray && info && info->isObject) || fieldInfo.isObjectArray ||
               !moduleObjectClass.empty()) {
        // Object arrays (including member object arrays) use rt_arr_obj_put (BUG-089)
        lowerer_.requireArrayObjPut();
        lowerer_.emitCall("rt_arr_obj_put", {access.base, access.index, value.value});
    } else if ((info && info->type == AstType::F64) ||
               (fieldInfo.isField &&
                fieldInfo.elementAstType == ::il::frontends::basic::Type::F64)) {
        // Float array (SINGLE/DOUBLE): use rt_arr_f64_set
        Lowerer::RVal coerced = lowerer_.ensureF64(std::move(value), loc);
        lowerer_.requireArrayF64Set();
        lowerer_.emitCall("rt_arr_f64_set", {access.base, access.index, coerced.value});
    } else {
        // Integer/numeric array: use rt_arr_i64_set (all Zanna integers are 64-bit)
        // Runtime ABI: rt_arr_i64_set expects its value operand as i64.
        // Always normalize the RHS to i64 (handles i1/i16/i32/f64).
        Lowerer::RVal coerced = lowerer_.ensureI64(std::move(value), loc);
        lowerer_.requireArrayI64Set();
        lowerer_.emitCall("rt_arr_i64_set", {access.base, access.index, coerced.value});
    }
}

} // namespace il::frontends::basic
