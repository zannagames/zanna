//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/RuntimeStatementLowerer.cpp
// Purpose: Implementation of runtime statement lowering extracted from Lowerer.
//          Handles lowering of BASIC runtime statements (terminal control,
//          assignments, variable declarations) to IL and runtime calls.
//
// NOTE: This file has been split into focused modules:
//   - RuntimeStatementLowerer_Terminal.cpp : BEEP, CLS, COLOR, LOCATE, etc.
//   - RuntimeStatementLowerer_Assign.cpp   : assignScalarSlot, assignArrayElement
//   - RuntimeStatementLowerer_Decl.cpp     : DIM, REDIM, CONST, STATIC, etc.
//
// This file retains the constructor and lowerLet (the largest, most complex
// function that handles various assignment target forms).
//
// Key invariants: Maintains Lowerer's runtime lowering semantics exactly
// Ownership/Lifetime: Borrows Lowerer reference; coordinates with parent
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/// @file RuntimeStatementLowerer.cpp
/// @brief Implements LET target dispatch and complex runtime-backed assignments.
/// @details Terminal, declaration, and low-level slot/array operations live in
///          sibling split files; this unit retains construction, scalar coercion,
///          runtime-constructor recognition, and assignments to variables,
///          field arrays, properties, members, and static fields.

#include "RuntimeStatementLowerer.hpp"
#include "Lowerer.hpp"
#include "RuntimeCallHelpers.hpp"
#include "frontends/basic/ASTUtils.hpp"
#include "frontends/basic/ILTypeUtils.hpp"
#include "frontends/basic/IdentifierUtil.hpp"
#include "frontends/basic/LocationScope.hpp"
#include "frontends/basic/NameMangler_OOP.hpp"
#include "frontends/basic/StringUtils.hpp"
#include "frontends/basic/sem/OverloadResolution.hpp"
#include "frontends/basic/sem/RuntimePropertyIndex.hpp"
#include "il/runtime/RuntimeClassNames.hpp"
#include "il/runtime/classes/RuntimeClasses.hpp"

#include <cassert>

using namespace il::core;
using il::runtime::RuntimeFeature;
using AstType = ::il::frontends::basic::Type;

namespace il::frontends::basic {

/// @brief Determine the runtime class a callee name constructs, if it is a constructor.
/// @param calleeName Callee name as written (qualified or simple).
/// @return The qualified class name when @p calleeName matches a catalog ctor or a `Class.New`
///         that returns an object, otherwise nullopt.
static std::optional<std::string> runtimeCtorClassQNameFromName(std::string_view calleeName) {
    if (calleeName.empty())
        return std::nullopt;

    const auto &classes = il::runtime::runtimeClassCatalog();
    for (const auto &klass : classes) {
        if (!klass.ctor)
            continue;
        if (string_utils::iequals(calleeName, klass.ctor))
            return std::string(klass.qname);
    }
    std::size_t lastDot = calleeName.rfind('.');
    if (lastDot == std::string_view::npos)
        return std::nullopt;

    std::string_view tail = calleeName.substr(lastDot + 1);
    if (!string_utils::iequals(tail, "New"))
        return std::nullopt;

    std::string qname(calleeName.substr(0, lastDot));
    for (const auto &klass : classes) {
        if (!string_utils::iequals(qname, klass.qname))
            continue;
        for (const auto &method : klass.methods) {
            if (!method.name || !method.signature)
                continue;
            if (!string_utils::iequals(method.name, "New"))
                continue;
            std::string_view sig(method.signature);
            std::size_t lparen = sig.find('(');
            std::string_view ret = lparen == std::string_view::npos ? sig : sig.substr(0, lparen);
            if (string_utils::iequals(ret, "obj"))
                return qname;
        }
    }
    return std::nullopt;
}

/// @brief Runtime-ctor class name for a call expression (delegates to the by-name resolver).
/// @param expr Call whose qualified or unqualified callee is inspected.
/// @return Qualified runtime class name, or `std::nullopt` when the call is not a constructor.
static std::optional<std::string> runtimeCtorClassQNameFrom(const CallExpr &expr) {
    std::string calleeName;
    if (!expr.calleeQualified.empty())
        calleeName = JoinDots(expr.calleeQualified);
    else
        calleeName = expr.callee;

    return runtimeCtorClassQNameFromName(calleeName);
}

/// @brief Runtime-ctor class name for a method-call expression (`base.Method`), if any.
/// @param expr Method call whose base runtime class and method are inspected.
/// @return Qualified runtime class name, or `std::nullopt` when it is not an object constructor.
static std::optional<std::string> runtimeCtorClassQNameFrom(const MethodCallExpr &expr) {
    if (!expr.base)
        return std::nullopt;
    auto baseQName = runtimeClassQNameFrom(*expr.base);
    if (!baseQName)
        return std::nullopt;

    std::string calleeName = *baseQName + "." + expr.method;
    return runtimeCtorClassQNameFromName(calleeName);
}

/// @brief Map slot metadata back to the BASIC element type used for array handles.
/// @param slotInfo Lowerer slot metadata for the target array.
/// @return BASIC element type used by runtime array retain/release helpers.
static AstType arrayElementTypeFromSlot(const SlotType &slotInfo) {
    if (slotInfo.type.kind == il::core::Type::Kind::Str)
        return AstType::Str;
    if (slotInfo.type.kind == il::core::Type::Kind::F64)
        return AstType::F64;
    if (slotInfo.isBoolean || slotInfo.type.kind == il::core::Type::Kind::I1)
        return AstType::Bool;
    return AstType::I64;
}

/// @brief Bind a runtime-statement lowerer to its parent Lowerer (non-owning).
/// @param lowerer Parent lowering coordinator receiving all emitted IL.
/// @pre @p lowerer outlives this helper.
RuntimeStatementLowerer::RuntimeStatementLowerer(Lowerer &lowerer) : lowerer_(lowerer) {}

/// @brief Coerce a lowered value to a user-declared BASIC scalar type when supported.
/// @details Setter calls already have overload-selected AST parameter metadata.
///          This helper applies the same scalar conversions used by ordinary
///          assignments so property setter calls receive the declared IL shape.
///          String/object values are returned unchanged because no implicit
///          textual/object conversion is available at this lowering layer.
/// @param value Lowered right-hand side value.
/// @param target Declared BASIC scalar target type.
/// @param loc Source location attributed to any emitted conversion.
/// @return Value coerced for the target type when supported.
Lowerer::RVal RuntimeStatementLowerer::coerceToAstScalar(Lowerer::RVal value,
                                                         AstType target,
                                                         il::support::SourceLoc loc) {
    switch (target) {
        case AstType::F64:
            return lowerer_.coerceToF64(std::move(value), loc);
        case AstType::Bool:
            return lowerer_.coerceToBool(std::move(value), loc);
        case AstType::Str:
            return value;
        default:
            return lowerer_.coerceToI64(std::move(value), loc);
    }
}

/// @brief Lower a BASIC @c LET statement.
///
/// @details Evaluates the right-hand expression and dispatches to the
///          appropriate helper for scalar variables, call/method-call-shaped
///          field arrays, ordinary array elements, or member/property access.
///          Missing operands emit `B2001` plus a trap; unrecognized target kinds
///          are left to semantic diagnostics. The lowering cursor is scoped to
///          the LET location.
///
/// @param stmt Parsed @c LET statement.
void RuntimeStatementLowerer::lowerLet(const LetStmt &stmt) {
    LocationScope loc(lowerer_, stmt.loc);
    if (!stmt.expr || !stmt.target) {
        if (auto *em = lowerer_.diagnosticEmitter()) {
            em->emit(il::support::Severity::Error,
                     "B2001",
                     stmt.loc,
                     1,
                     "LET statement is missing a target or expression");
        }
        lowerer_.emitTrap();
        return;
    }
    Lowerer::RVal value = lowerer_.lowerExpr(*stmt.expr);
    if (auto *var = as<const VarExpr>(*stmt.target))
        lowerLetToVar(stmt, *var, std::move(value));
    else if (auto *mc = as<const MethodCallExpr>(*stmt.target))
        lowerLetToMethodCall(stmt, *mc, std::move(value));
    else if (auto *call = as<const CallExpr>(*stmt.target))
        lowerLetToCall(stmt, *call, std::move(value));
    else if (auto *arr = as<const ArrayExpr>(*stmt.target))
        lowerLetToArray(stmt, *arr, std::move(value));
    else if (auto *member = as<const MemberAccessExpr>(*stmt.target))
        lowerLetToMember(stmt, *member, std::move(value));
}

/// @brief Lower `LET var = value` to a scalar/array slot assignment.
/// @param stmt The LET statement.
/// @param varRef The target variable.
/// @param value The already-lowered right-hand value.
/// @details Refreshes the variable's slot type, refines its object class from authoritative
///          sources (NEW/constructors override; inferred classes only fill generic OBJECT),
///          stores into the slot (array vs scalar), and balances the creation reference of a
///          user-class NEW temporary so its refcount settles with the variable as sole owner.
void RuntimeStatementLowerer::lowerLetToVar(const LetStmt &stmt,
                                            const VarExpr &varRef,
                                            Lowerer::RVal value) {
    const VarExpr *var = &varRef;
    {
        auto storage = lowerer_.resolveVariableStorage(var->name, stmt.loc);
        assert(storage && "LET target should have storage");
        if (!storage)
            return; // Safety: skip if storage lookup fails in Release builds.
        if (stmt.expr && !storage->isField) {
            std::string className;
            bool authoritative = false; // NEW / ctor: always override
            if (const auto *alloc = as<const NewExpr>(*stmt.expr)) {
                className = alloc->className;
                authoritative = true;
            } else if (const auto *call = as<const CallExpr>(*stmt.expr)) {
                if (auto qname = runtimeCtorClassQNameFrom(*call)) {
                    className = *qname;
                    authoritative = true;
                } else
                    className = lowerer_.resolveObjectClass(*stmt.expr);
            } else {
                className = lowerer_.resolveObjectClass(*stmt.expr);
            }
            if (className.empty()) {
                if (const auto *mcall = as<const MethodCallExpr>(*stmt.expr)) {
                    if (auto qname = runtimeCtorClassQNameFrom(*mcall)) {
                        className = *qname;
                        authoritative = true;
                    }
                }
            }
            if (!className.empty()) {
                // Don't override a DIM-declared class type with an inferred one.
                // Only authoritative sources (NEW, constructors) may override.
                // DIM AS OBJECT is generic — always allow refinement.
                Lowerer::SlotType existing = lowerer_.getSlotType(var->name);
                bool isGenericObj =
                    existing.isObject && string_utils::iequals(existing.objectClass, "object");
                if (authoritative || !existing.isObject || existing.objectClass.empty() ||
                    isGenericObj) {
                    lowerer_.setSymbolObjectType(var->name, className);
                }
            }
        }
        // Invariant: Slot typing must be refreshed from symbols/sema on each use
        // to avoid stale kinds when crossing complex control flow (e.g., SELECT CASE). (BUG-076)
        Lowerer::SlotType slotInfo = lowerer_.getSlotType(var->name);

        // Detect user-class NEW expressions — their temporary must be released
        // after assignment to balance the refcount.  rt_obj_new_i64 returns with
        // refcount 1 (creation ref).  assignScalarSlot retains (+1=2) for the
        // variable's ownership.  Without this release the creation ref is
        // never balanced and the object can never reach refcount 0.
        //
        // Only user-defined classes (present in the OOP index) go through
        // rt_obj_new_i64.  Runtime classes (StringBuilder, String, File, etc.)
        // use dedicated ctors with their own allocation and refcount semantics;
        // releasing their return values would corrupt the heap.
        bool isUserClassNew = false;
        if (const auto *alloc = as<const NewExpr>(*stmt.expr)) {
            std::string qname = lowerer_.qualify(alloc->className);
            if (lowerer_.oopIndex_.findClass(qname))
                isUserClassNew = true;
        }
        Lowerer::Value newTempValue = value.value; // save before move

        if (slotInfo.isArray) {
            lowerer_.storeArray(storage->pointer,
                                value.value,
                                arrayElementTypeFromSlot(slotInfo),
                                /*isObjectArray*/ slotInfo.isObject);
        } else {
            assignScalarSlot(slotInfo, storage->pointer, std::move(value), stmt.loc);
        }

        // Release the NEW temporary's creation reference.  After
        // assignScalarSlot retained the value, refcount is 2.  This release
        // drops it to 1 — the variable is the sole owner.
        if (isUserClassNew && slotInfo.isObject) {
            lowerer_.requestHelper(RuntimeFeature::ObjReleaseChk0);
            lowerer_.curLoc = {};
            lowerer_.emitCallRet(lowerer_.ilBoolTy(), "rt_obj_release_check0", {newTempValue});
        }
    }
}

/// @brief Lower `LET obj.arrayField(idx...) = value` (method-call-shaped array element store).
/// @param stmt The LET statement.
/// @param mcRef The method-call-shaped assignment target.
/// @param value The already-lowered right-hand value.
/// @details Resolves the receiver's object field as an array (BUG-056), computes a row-major
///          flattened index for multi-dimensional arrays (BUG-094), emits a bounds check, and
///          stores via the element-kind-appropriate `rt_arr_*_put`/`set` helper. Unsupported
///          lvalue forms are no-ops (the analyzer reports them).
void RuntimeStatementLowerer::lowerLetToMethodCall(const LetStmt &stmt,
                                                   const MethodCallExpr &mcRef,
                                                   Lowerer::RVal value) {
    const MethodCallExpr *mc = &mcRef;
    {
        // Handle array field assignment (obj.arrayField(index) = value). (BUG-056)
        // Only handle simple base forms we can resolve (VarExpr or ME).
        if (mc->base) {
            std::string baseName;
            if (auto *v = as<const VarExpr>(*mc->base))
                baseName = v->name;
            else if (is<MeExpr>(*mc->base))
                baseName = "ME";
            if (!baseName.empty()) {
                // Compute array handle from object field
                const auto *baseSym = lowerer_.findSymbol(baseName);
                if (baseSym && baseSym->slotId) {
                    lowerer_.curLoc = stmt.loc;
                    Value selfPtr = lowerer_.emitLoad(il::core::Type(il::core::Type::Kind::Ptr),
                                                      Value::temp(*baseSym->slotId));
                    std::string klass = lowerer_.getSlotType(baseName).objectClass;
                    if (const Lowerer::ClassLayout *layout = lowerer_.findClassLayout(klass)) {
                        if (const Lowerer::ClassLayout::Field *fld =
                                layout->findField(mc->method)) {
                            lowerer_.curLoc = stmt.loc;
                            Value fieldPtr = lowerer_.emitBinary(
                                Opcode::GEP,
                                il::core::Type(il::core::Type::Kind::Ptr),
                                selfPtr,
                                Value::constInt(static_cast<long long>(fld->offset)));
                            Value arrHandle = lowerer_.emitLoad(
                                il::core::Type(il::core::Type::Kind::Ptr), fieldPtr);

                            // Multi-dimensional arrays require flattened index computation.
                            // (BUG-094)
                            std::vector<Value> indices;
                            indices.reserve(mc->args.size());
                            for (const auto &arg : mc->args) {
                                Lowerer::RVal idx = lowerer_.lowerExpr(*arg);
                                idx = lowerer_.coerceToI64(std::move(idx), stmt.loc);
                                indices.push_back(idx.value);
                            }

                            // Compute flattened index for multi-dimensional arrays
                            Value index = Value::constInt(0);
                            if (!indices.empty()) {
                                if (indices.size() == 1) {
                                    index = indices[0];
                                } else if (fld->isArray && !fld->arrayExtents.empty() &&
                                           fld->arrayExtents.size() == indices.size()) {
                                    lowerer_.curLoc = stmt.loc;
                                    index =
                                        lowerer_.emitRowMajorFlatIndex(indices, fld->arrayExtents);
                                } else {
                                    if (auto *em = lowerer_.diagnosticEmitter()) {
                                        em->emit(il::support::Severity::Error,
                                                 "B2000",
                                                 stmt.loc,
                                                 static_cast<uint32_t>(mc->method.size()),
                                                 "cannot flatten multidimensional field array "
                                                 "without matching extents");
                                    }
                                    lowerer_.emitTrap();
                                    index = Value::constInt(0);
                                }
                            }

                            // Determine element kind for helpers
                            const bool isMemberObjectArray = !fld->objectClassName.empty();

                            // Bounds check (select len helper based on element kind)
                            Value len;
                            if (fld->type == ::il::frontends::basic::Type::Str) {
                                lowerer_.requireArrayStrLen();
                                len =
                                    lowerer_.emitCallRet(il::core::Type(il::core::Type::Kind::I64),
                                                         "rt_arr_str_len",
                                                         {arrHandle});
                            } else if (isMemberObjectArray) {
                                lowerer_.requireArrayObjLen();
                                len =
                                    lowerer_.emitCallRet(il::core::Type(il::core::Type::Kind::I64),
                                                         "rt_arr_obj_len",
                                                         {arrHandle});
                            } else {
                                lowerer_.requireArrayI64Len();
                                len =
                                    lowerer_.emitCallRet(il::core::Type(il::core::Type::Kind::I64),
                                                         "rt_arr_i64_len",
                                                         {arrHandle});
                            }
                            Value isNeg = lowerer_.emitBinary(
                                Opcode::SCmpLT, lowerer_.ilBoolTy(), index, Value::constInt(0));
                            Value tooHigh = lowerer_.emitBinary(
                                Opcode::SCmpGE, lowerer_.ilBoolTy(), index, len);
                            auto emitc = lowerer_.emitCommon(stmt.loc);
                            Value isNeg64 = emitc.widen_to(isNeg, 1, 64, Signedness::Unsigned);
                            Value tooHigh64 = emitc.widen_to(tooHigh, 1, 64, Signedness::Unsigned);
                            Value oobInt = emitc.logical_or(isNeg64, tooHigh64);
                            Value oobCond = lowerer_.emitBinary(
                                Opcode::ICmpNe, lowerer_.ilBoolTy(), oobInt, Value::constInt(0));

                            ProcedureContext &ctx = lowerer_.context();
                            Function *func = ctx.function();
                            size_t curIdx = ctx.currentIndex();
                            unsigned bcId = ctx.consumeBoundsCheckId();
                            BlockNamer *blockNamer = ctx.blockNames().namer();
                            size_t okIdx = func->blocks.size();
                            std::string okLbl =
                                blockNamer ? blockNamer->tag("bc_ok" + std::to_string(bcId))
                                           : lowerer_.mangler.block("bc_ok" + std::to_string(bcId));
                            lowerer_.builder->addBlock(*func, okLbl);
                            size_t oobIdx = func->blocks.size();
                            std::string oobLbl =
                                blockNamer
                                    ? blockNamer->tag("bc_oob" + std::to_string(bcId))
                                    : lowerer_.mangler.block("bc_oob" + std::to_string(bcId));
                            lowerer_.builder->addBlock(*func, oobLbl);
                            BasicBlock *ok = &func->blocks[okIdx];
                            BasicBlock *oob = &func->blocks[oobIdx];
                            ctx.setCurrent(&func->blocks[curIdx]);
                            lowerer_.emitCBr(oobCond, oob, ok);
                            ctx.setCurrent(oob);
                            lowerer_.requireArrayOobPanic();
                            lowerer_.emitCall("rt_arr_oob_panic", {index, len});
                            lowerer_.emitTrap();
                            ctx.setCurrent(ok);

                            // Perform the store
                            if (fld->type == ::il::frontends::basic::Type::Str) {
                                lowerer_.requireArrayStrPut();
                                lowerer_.emitCall("rt_arr_str_put",
                                                  {arrHandle, index, value.value});
                            } else if (isMemberObjectArray) {
                                lowerer_.requireArrayObjPut();
                                lowerer_.emitCall("rt_arr_obj_put",
                                                  {arrHandle, index, value.value});
                            } else if (fld->type == ::il::frontends::basic::Type::F64) {
                                lowerer_.requireArrayF64Set();
                                Lowerer::RVal coerced =
                                    lowerer_.ensureF64(std::move(value), stmt.loc);
                                lowerer_.emitCall("rt_arr_f64_set",
                                                  {arrHandle, index, coerced.value});
                            } else {
                                lowerer_.requireArrayI64Set();
                                Lowerer::RVal coerced =
                                    lowerer_.ensureI64(std::move(value), stmt.loc);
                                lowerer_.emitCall("rt_arr_i64_set",
                                                  {arrHandle, index, coerced.value});
                            }
                            return;
                        }
                    }
                }
            }
        }
        // Fallback: not a supported lvalue form; do nothing here (analyzer should have errored).
    }
}

/// @brief Lower `LET field(idx) = value` where the call shape is an implicit field-array access.
/// @param stmt The LET statement.
/// @param callRef The call-shaped assignment target.
/// @param value The already-lowered right-hand value.
/// @details Handles implicit field arrays inside a method (BUG-089): loads the array handle from
///          the `ME` object field, lowers the index, bounds-checks, and stores via the
///          element-kind-appropriate runtime put/set helper.
void RuntimeStatementLowerer::lowerLetToCall(const LetStmt &stmt,
                                             const CallExpr &callRef,
                                             Lowerer::RVal value) {
    const CallExpr *call = &callRef;
    {
        // CallExpr can be an implicit field array access (e.g., items(i) inside a method).
        // Check if this refers to a field array in the current class. (BUG-089)
        if (lowerer_.isFieldInScope(call->callee)) {
            if (const auto *scope = lowerer_.activeFieldScope(); scope && scope->layout) {
                if (const Lowerer::ClassLayout::Field *fld =
                        scope->layout->findField(call->callee)) {
                    if (fld->isArray) {
                        // This is a field array access. Lower it inline similar to MethodCallExpr
                        // handling. Get the ME pointer and compute the field array handle
                        const auto *selfInfo = lowerer_.findSymbol("ME");
                        if (selfInfo && selfInfo->slotId) {
                            lowerer_.curLoc = stmt.loc;
                            Value selfPtr =
                                lowerer_.emitLoad(il::core::Type(il::core::Type::Kind::Ptr),
                                                  Value::temp(*selfInfo->slotId));
                            lowerer_.curLoc = stmt.loc;
                            Value fieldPtr = lowerer_.emitBinary(
                                Opcode::GEP,
                                il::core::Type(il::core::Type::Kind::Ptr),
                                selfPtr,
                                Value::constInt(static_cast<long long>(fld->offset)));
                            Value arrHandle = lowerer_.emitLoad(
                                il::core::Type(il::core::Type::Kind::Ptr), fieldPtr);

                            std::vector<Value> indices;
                            indices.reserve(call->args.size());
                            for (const auto &arg : call->args) {
                                if (!arg)
                                    continue;
                                Lowerer::RVal idx = lowerer_.lowerExpr(*arg);
                                idx = lowerer_.coerceToI64(std::move(idx), stmt.loc);
                                indices.push_back(idx.value);
                            }
                            Value index = Value::constInt(0);
                            if (indices.size() == 1) {
                                index = indices[0];
                            } else if (!indices.empty() &&
                                       fld->arrayExtents.size() == indices.size()) {
                                lowerer_.curLoc = stmt.loc;
                                index = lowerer_.emitRowMajorFlatIndex(indices, fld->arrayExtents);
                            } else if (!indices.empty()) {
                                if (auto *em = lowerer_.diagnosticEmitter()) {
                                    em->emit(il::support::Severity::Error,
                                             "B2000",
                                             stmt.loc,
                                             static_cast<uint32_t>(call->callee.size()),
                                             "cannot flatten multidimensional field array without "
                                             "matching extents");
                                }
                                lowerer_.emitTrap();
                            }

                            // Now perform bounds-checked array assignment
                            // We need to call the appropriate rt_arr_*_put function
                            bool isMemberObjectArray = !fld->objectClassName.empty();

                            // Bounds check
                            Value len;
                            if (fld->type == ::il::frontends::basic::Type::Str) {
                                lowerer_.requireArrayStrLen();
                                len =
                                    lowerer_.emitCallRet(il::core::Type(il::core::Type::Kind::I64),
                                                         "rt_arr_str_len",
                                                         {arrHandle});
                            } else if (isMemberObjectArray) {
                                lowerer_.requireArrayObjLen();
                                len =
                                    lowerer_.emitCallRet(il::core::Type(il::core::Type::Kind::I64),
                                                         "rt_arr_obj_len",
                                                         {arrHandle});
                            } else {
                                lowerer_.requireArrayI64Len();
                                len =
                                    lowerer_.emitCallRet(il::core::Type(il::core::Type::Kind::I64),
                                                         "rt_arr_i64_len",
                                                         {arrHandle});
                            }

                            Value isNeg = lowerer_.emitBinary(
                                Opcode::SCmpLT, lowerer_.ilBoolTy(), index, Value::constInt(0));
                            Value tooHigh = lowerer_.emitBinary(
                                Opcode::SCmpGE, lowerer_.ilBoolTy(), index, len);
                            auto emitc = lowerer_.emitCommon(stmt.loc);
                            Value isNeg64 = emitc.widen_to(isNeg, 1, 64, Signedness::Unsigned);
                            Value tooHigh64 = emitc.widen_to(tooHigh, 1, 64, Signedness::Unsigned);
                            Value oobInt = emitc.logical_or(isNeg64, tooHigh64);
                            Value oobCond = lowerer_.emitBinary(
                                Opcode::ICmpNe, lowerer_.ilBoolTy(), oobInt, Value::constInt(0));

                            ProcedureContext &ctx = lowerer_.context();
                            Function *func = ctx.function();
                            size_t curIdx = ctx.currentIndex();
                            unsigned bcId = ctx.consumeBoundsCheckId();
                            BlockNamer *blockNamer = ctx.blockNames().namer();
                            size_t okIdx = func->blocks.size();
                            std::string okLbl =
                                blockNamer ? blockNamer->tag("bc_ok" + std::to_string(bcId))
                                           : lowerer_.mangler.block("bc_ok" + std::to_string(bcId));
                            lowerer_.builder->addBlock(*func, okLbl);
                            size_t oobIdx = func->blocks.size();
                            std::string oobLbl =
                                blockNamer
                                    ? blockNamer->tag("bc_oob" + std::to_string(bcId))
                                    : lowerer_.mangler.block("bc_oob" + std::to_string(bcId));
                            lowerer_.builder->addBlock(*func, oobLbl);
                            BasicBlock *ok = &func->blocks[okIdx];
                            BasicBlock *oob = &func->blocks[oobIdx];
                            ctx.setCurrent(&func->blocks[curIdx]);
                            lowerer_.emitCBr(oobCond, oob, ok);
                            ctx.setCurrent(oob);
                            lowerer_.requireArrayOobPanic();
                            lowerer_.emitCall("rt_arr_oob_panic", {index, len});
                            lowerer_.emitTrap();
                            ctx.setCurrent(ok);

                            // Perform the actual assignment
                            if (fld->type == ::il::frontends::basic::Type::Str) {
                                lowerer_.requireArrayStrPut();
                                lowerer_.emitCall("rt_arr_str_put",
                                                  {arrHandle, index, value.value});
                            } else if (isMemberObjectArray) {
                                lowerer_.requireArrayObjPut();
                                lowerer_.emitCall("rt_arr_obj_put",
                                                  {arrHandle, index, value.value});
                            } else if (fld->type == ::il::frontends::basic::Type::F64) {
                                lowerer_.requireArrayF64Set();
                                Lowerer::RVal coerced =
                                    lowerer_.ensureF64(std::move(value), stmt.loc);
                                lowerer_.emitCall("rt_arr_f64_set",
                                                  {arrHandle, index, coerced.value});
                            } else {
                                lowerer_.requireArrayI64Set();
                                Lowerer::RVal coerced =
                                    lowerer_.ensureI64(std::move(value), stmt.loc);
                                lowerer_.emitCall("rt_arr_i64_set",
                                                  {arrHandle, index, coerced.value});
                            }
                            return;
                        }
                    }
                }
            }
        }
        // Not a field array; fall through (analyzer should have errored)
    }
}

/// @brief Lower `LET arr(idx) = value` for an ordinary (non-field) array variable.
/// @param stmt The LET statement.
/// @param arr The array-element assignment target.
/// @param value The already-lowered right-hand value.
/// @details Resolves the array's storage and element kind, lowers/coerces the index, and stores
///          the value with the appropriate bounds-checked runtime array setter.
void RuntimeStatementLowerer::lowerLetToArray(const LetStmt &stmt,
                                              const ArrayExpr &arr,
                                              Lowerer::RVal value) {
    assignArrayElement(arr, std::move(value), stmt.loc);
}

/// @brief Lower `LET base.member = value` to a property setter or static-field store.
/// @param stmt The LET statement.
/// @param memberRef The member-access assignment target.
/// @param value The already-lowered right-hand value.
/// @details Tries, in order: a runtime-class property setter (erroring on read-only/missing),
///          an instance property setter (`set_member`, overload-resolved and coerced), a static
///          property setter on a class name, and finally a static-field global store. Each path
///          coerces @p value to the expected type.
void RuntimeStatementLowerer::lowerLetToMember(const LetStmt &stmt,
                                               const MemberAccessExpr &memberRef,
                                               Lowerer::RVal value) {
    const MemberAccessExpr *member = &memberRef;
    {
        if (auto access = lowerer_.resolveMemberField(*member)) {
            Lowerer::SlotType slotInfo;
            slotInfo.type = access->ilType;
            slotInfo.isArray = false;
            slotInfo.isBoolean = (access->astType == ::il::frontends::basic::Type::Bool);
            slotInfo.isObject =
                !access->objectClassName.empty(); // Object fields use pointer semantics (BUG-082)
            if (slotInfo.isObject) {
                slotInfo.objectClass = access->objectClassName;
            }
            assignScalarSlot(slotInfo, access->ptr, std::move(value), stmt.loc);
        } else {
            // Runtime class property setter via catalog (e.g., Zanna.String)
            {
                auto &pidx = runtimePropertyIndex();

                if (member->base) {
                    if (auto qClass = runtimeClassQNameFrom(*member->base)) {
                        auto prop = pidx.find(*qClass, member->member);
                        if (prop) {
                            if (prop->readonly || prop->setter.empty()) {
                                if (auto *em = lowerer_.diagnosticEmitter()) {
                                    std::string msg = "property '" + member->member + "' on '" +
                                                      *qClass + "' is read-only";
                                    em->emit(il::support::Severity::Error,
                                             "E_PROP_READONLY",
                                             stmt.loc,
                                             static_cast<uint32_t>(member->member.size()),
                                             std::move(msg));
                                }
                                return;
                            }
                            Lowerer::RVal v = value;
                            auto k = type_conv::runtimeScalarToType(prop->type).kind;
                            if (k == Lowerer::Type::Kind::I1)
                                v = lowerer_.coerceToBool(std::move(v), stmt.loc);
                            else if (k == Lowerer::Type::Kind::F64)
                                v = lowerer_.coerceToF64(std::move(v), stmt.loc);
                            else if (k == Lowerer::Type::Kind::I64)
                                v = lowerer_.coerceToI64(std::move(v), stmt.loc);
                            lowerer_.emitCall(prop->setter, {v.value});
                            return;
                        }
                    }
                }

                Lowerer::RVal baseVal = lowerer_.lowerExpr(*member->base);
                std::string qClass;
                {
                    std::string cls = lowerer_.resolveObjectClass(*member->base);
                    if (!cls.empty())
                        qClass = lowerer_.qualify(cls);
                }
                if (qClass.empty() && baseVal.type.kind == Lowerer::Type::Kind::Str)
                    qClass = std::string(il::runtime::RTCLASS_STRING);
                if (!qClass.empty()) {
                    auto prop = pidx.find(qClass, member->member);
                    if (prop) {
                        if (prop->readonly || prop->setter.empty()) {
                            if (auto *em = lowerer_.diagnosticEmitter()) {
                                std::string msg = "property '" + member->member + "' on '" +
                                                  qClass + "' is read-only";
                                em->emit(il::support::Severity::Error,
                                         "E_PROP_READONLY",
                                         stmt.loc,
                                         static_cast<uint32_t>(member->member.size()),
                                         std::move(msg));
                            }
                            return;
                        }
                        Lowerer::RVal v = value;
                        // Coerce according to expected type token
                        auto k = type_conv::runtimeScalarToType(prop->type).kind;
                        if (k == Lowerer::Type::Kind::I1)
                            v = lowerer_.coerceToBool(std::move(v), stmt.loc);
                        else if (k == Lowerer::Type::Kind::F64)
                            v = lowerer_.coerceToF64(std::move(v), stmt.loc);
                        else if (k == Lowerer::Type::Kind::I64)
                            v = lowerer_.coerceToI64(std::move(v), stmt.loc);
                        lowerer_.emitCall(prop->setter, {baseVal.value, v.value});
                        return;
                    } else if (auto *em = lowerer_.diagnosticEmitter()) {
                        std::string msg =
                            "no such property '" + member->member + "' on '" + qClass + "'";
                        em->emit(il::support::Severity::Error,
                                 "E_PROP_NO_SUCH_PROPERTY",
                                 stmt.loc,
                                 static_cast<uint32_t>(member->member.size()),
                                 std::move(msg));
                        return;
                    }
                }
            }
            // Property setter sugar (instance): base.member = value -> call set_member(base, value)
            // Property setter sugar (static):   Class.member = value -> call
            // Class.set_member(value) Static field assignment:          Class.field  = value ->
            // store @Class::field

            std::string className = lowerer_.resolveObjectClass(*member->base);
            if (!className.empty()) {
                std::string qname = lowerer_.qualify(className);
                std::string setter = std::string("set_") + member->member;
                // Overload resolution for instance setter with one user arg (value)
                /// Map a lowered IL scalar kind back to overload-resolution AST type.
                auto mapIlToAst = [](Lowerer::Type t) -> ::il::frontends::basic::Type {
                    using K = Lowerer::Type::Kind;
                    switch (t.kind) {
                        case K::F64:
                            return ::il::frontends::basic::Type::F64;
                        case K::Str:
                            return ::il::frontends::basic::Type::Str;
                        case K::I1:
                            return ::il::frontends::basic::Type::Bool;
                        default:
                            return ::il::frontends::basic::Type::I64;
                    }
                };
                std::vector<::il::frontends::basic::Type> argTypes{mapIlToAst(value.type)};
                auto resolved = sem::resolveMethodOverload(lowerer_.oopIndex_,
                                                           qname,
                                                           member->member,
                                                           /*isStatic*/ false,
                                                           argTypes,
                                                           lowerer_.currentClass(),
                                                           lowerer_.diagnosticEmitter(),
                                                           stmt.loc);
                if (resolved) {
                    setter = resolved->methodName;
                } else if (lowerer_.diagnosticEmitter()) {
                    return;
                }
                std::string callee = mangleMethod(qname, setter);
                Lowerer::RVal base = lowerer_.lowerExpr(*member->base);
                std::vector<Lowerer::Value> args{base.value, value.value};
                if (resolved && resolved->method && !resolved->method->sig.paramTypes.empty()) {
                    value = coerceToAstScalar(
                        std::move(value), resolved->method->sig.paramTypes.front(), stmt.loc);
                    args[1] = value.value;
                }
                lowerer_.emitCall(callee, args);
                return;
            }

            if (const auto *v = as<const VarExpr>(*member->base)) {
                // If a symbol with this name exists (local/param/global), treat as instance, not
                // static
                if (const auto *sym = lowerer_.findSymbol(v->name); sym && sym->slotId) {
                    // analyzer should have already errored if not a property/field; nothing more to
                    // do here
                    return;
                }
                std::string qname = lowerer_.resolveQualifiedClassCasing(lowerer_.qualify(v->name));
                if (const ClassInfo *ci = lowerer_.oopIndex_.findClass(qname)) {
                    // Prefer static property setter when present
                    std::string setter = std::string("set_") + member->member;
                    /// Map a lowered IL scalar kind back to overload-resolution AST type.
                    auto mapIlToAst = [](Lowerer::Type t) -> ::il::frontends::basic::Type {
                        using K = Lowerer::Type::Kind;
                        switch (t.kind) {
                            case K::F64:
                                return ::il::frontends::basic::Type::F64;
                            case K::Str:
                                return ::il::frontends::basic::Type::Str;
                            case K::I1:
                                return ::il::frontends::basic::Type::Bool;
                            default:
                                return ::il::frontends::basic::Type::I64;
                        }
                    };
                    std::vector<::il::frontends::basic::Type> argTypes{mapIlToAst(value.type)};
                    auto resolved = sem::resolveMethodOverload(lowerer_.oopIndex_,
                                                               qname,
                                                               member->member,
                                                               /*isStatic*/ true,
                                                               argTypes,
                                                               lowerer_.currentClass(),
                                                               lowerer_.diagnosticEmitter(),
                                                               stmt.loc);
                    if (resolved) {
                        setter = resolved->methodName;
                    } else if (lowerer_.diagnosticEmitter()) {
                        return;
                    }
                    auto it = ci->methods.find(setter);
                    if (it != ci->methods.end() && it->second.isStatic) {
                        std::string callee = mangleMethod(ci->qualifiedName, setter);
                        if (resolved && resolved->method &&
                            !resolved->method->sig.paramTypes.empty()) {
                            value = coerceToAstScalar(std::move(value),
                                                      resolved->method->sig.paramTypes.front(),
                                                      stmt.loc);
                        }
                        lowerer_.emitCall(callee, {value.value});
                        return;
                    }

                    // Otherwise store into a static field global
                    for (const auto &sf : ci->staticFields) {
                        if (sf.name == member->member) {
                            Lowerer::Type ilTy = sf.objectClassName.empty()
                                                     ? type_conv::astToIlType(sf.type)
                                                     : Lowerer::Type(Lowerer::Type::Kind::Ptr);
                            lowerer_.curLoc = stmt.loc;
                            std::string gname = ci->qualifiedName + "::" + member->member;
                            Lowerer::Value addr =
                                lowerer_.emitUnary(Lowerer::Opcode::AddrOf,
                                                   Lowerer::Type(Lowerer::Type::Kind::Ptr),
                                                   Lowerer::Value::global(gname));
                            // Coerce booleans when needed
                            Lowerer::RVal vcoerced = value;
                            if (ilTy.kind == Lowerer::Type::Kind::I1)
                                vcoerced = lowerer_.coerceToBool(std::move(vcoerced), stmt.loc);
                            lowerer_.emitStore(ilTy, addr, vcoerced.value);
                            return;
                        }
                    }
                }
            }
        }
    }
}

// The following functions have been moved to separate files:
// - visit(BeepStmt), visit(ClsStmt), etc. -> RuntimeStatementLowerer_Terminal.cpp
// - assignScalarSlot, assignArrayElement -> RuntimeStatementLowerer_Assign.cpp
// - lowerConst, lowerStatic, lowerDim, lowerReDim, lowerRandomize, lowerSwap,
//   emitArrayLengthCheck -> RuntimeStatementLowerer_Decl.cpp

} // namespace il::frontends::basic
