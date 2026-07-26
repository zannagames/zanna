//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file Lowerer_Expr_Method.cpp
/// @brief Method call and type construction lowering for the Zia IL lowerer.
///
/// @details This file handles method call dispatch (direct, virtual, interface),
///          collection method calls (List, Map, Set), inline lowering of list
///          combinators, and value/class construction via function-call
///          syntax. Construction follows resolved initializer bindings and
///          preserves declaration-order field layout and managed ownership.
///
//===----------------------------------------------------------------------===//

#include "frontends/common/CollectionMethodCatalog.hpp"
#include "frontends/zia/Lowerer.hpp"
#include "frontends/zia/RuntimeNames.hpp"
#include <algorithm>
#include <optional>
#include <unordered_map>

namespace il::frontends::zia {

using namespace runtime;
using CollectionMethod = il::frontends::common::CollectionMethodId;

namespace {

/// @brief Append @p typeName's field declarations (base-class fields first,
///        then this class's) to @p out — gives the in-memory field order used
///        when lowering method receivers / field access.
/// @param sema Semantic analyzer used to resolve class declarations.
/// @param typeName Qualified class name whose fields are collected.
/// @param out Destination vector extended with inherited and local non-static
///        fields.
void appendClassFieldDecls(Sema &sema,
                           const std::string &typeName,
                           std::vector<const FieldDecl *> &out) {
    ClassDecl *decl = sema.findClassDecl(typeName);
    if (!decl)
        return;
    if (!decl->baseClass.empty())
        appendClassFieldDecls(sema, decl->baseClass, out);
    for (const auto &member : decl->members) {
        if (member->kind != DeclKind::Field)
            continue;
        auto *field = static_cast<FieldDecl *>(member.get());
        if (!field->isStatic)
            out.push_back(field);
    }
}

/// @brief Collect a struct's non-static fields in declaration order.
/// @param sema Semantic analyzer used to resolve the struct declaration.
/// @param typeName Qualified struct name.
/// @return Field declaration pointers, or an empty vector for an unknown type.
std::vector<const FieldDecl *> collectStructFieldDecls(Sema &sema, const std::string &typeName) {
    std::vector<const FieldDecl *> out;
    StructDecl *decl = sema.findStructDecl(typeName);
    if (!decl)
        return out;
    for (const auto &member : decl->members) {
        if (member->kind != DeclKind::Field)
            continue;
        auto *field = static_cast<FieldDecl *>(member.get());
        if (!field->isStatic)
            out.push_back(field);
    }
    return out;
}

} // namespace

//=============================================================================
// List Method Call Helper
//=============================================================================

/// @brief Try to lower a built-in List method call.
/// @param baseValue Runtime list receiver.
/// @param baseType Semantic list type supplying its element type.
/// @param methodName Source method name.
/// @param expr Call expression containing method arguments.
/// @return Lowered result when @p methodName is recognized; otherwise
///         `std::nullopt`.
/// @details Collection access and mutation route through the List runtime,
///          with element boxing/unboxing and index widening. Functional
///          combinators delegate to lowerListCombinator().
std::optional<LowerResult> Lowerer::lowerListMethodCall(Value baseValue,
                                                        TypeRef baseType,
                                                        const std::string &methodName,
                                                        CallExpr *expr) {
    // Functional combinators are not in the shared collection-method table; they
    // lower to inline loops that invoke the closure argument.
    if (auto combinator = lowerListCombinator(baseValue, baseType, methodName, expr))
        return combinator;

    // Use O(1) dispatch table lookup instead of sequential string comparisons
    const CollectionMethod method = il::frontends::common::lookupCollectionMethod(methodName);

    switch (method) {
        case CollectionMethod::Get:
            if (expr->args.size() >= 1) {
                auto indexResult = lowerExpr(expr->args[0].value.get());
                Value indexValue = widenIntegralToI64(indexResult.value, indexResult.type);
                Value boxed = emitCallRet(Type(Type::Kind::Ptr), kListGet, {baseValue, indexValue});
                TypeRef elemType = baseType ? baseType->elementType() : nullptr;
                if (!elemType)
                    elemType = sema_.typeOf(expr);
                Type ilElemType = mapType(elemType);
                return emitUnboxValue(boxed, ilElemType, elemType);
            }
            break;

        case CollectionMethod::First:
        case CollectionMethod::Last: {
            const char *callee =
                method == CollectionMethod::First ? kCollectionsListFirst : kCollectionsListLast;
            Value boxed = emitCallRet(Type(Type::Kind::Ptr), callee, {baseValue});
            TypeRef elemType = baseType ? baseType->elementType() : nullptr;
            if (!elemType)
                elemType = sema_.typeOf(expr);
            Type ilElemType = mapType(elemType);
            return emitUnboxValue(boxed, ilElemType, elemType);
        }

        case CollectionMethod::RemoveAt:
            if (expr->args.size() >= 1) {
                auto indexResult = lowerExpr(expr->args[0].value.get());
                Value indexValue = widenIntegralToI64(indexResult.value, indexResult.type);
                emitCall(kListRemoveAt, {baseValue, indexValue});
                return LowerResult{Value::constInt(0), Type(Type::Kind::Void)};
            }
            break;

        case CollectionMethod::Remove:
            if (expr->args.size() >= 1) {
                auto valueResult = lowerExpr(expr->args[0].value.get());
                TypeRef argType = sema_.typeOf(expr->args[0].value.get());
                Value boxedValue = emitBoxValue(valueResult.value, valueResult.type, argType);
                Value result =
                    emitCallRet(Type(Type::Kind::I1), kListRemove, {baseValue, boxedValue});
                return LowerResult{result, Type(Type::Kind::I1)};
            }
            break;

        case CollectionMethod::Insert:
            if (expr->args.size() >= 2) {
                auto indexResult = lowerExpr(expr->args[0].value.get());
                Value indexValue = widenIntegralToI64(indexResult.value, indexResult.type);
                auto valueResult = lowerExpr(expr->args[1].value.get());
                TypeRef argType = sema_.typeOf(expr->args[1].value.get());
                Value boxedValue = emitBoxValue(valueResult.value, valueResult.type, argType);
                emitCall(kListInsert, {baseValue, indexValue, boxedValue});
                return LowerResult{Value::constInt(0), Type(Type::Kind::Void)};
            }
            break;

        case CollectionMethod::Find:
        case CollectionMethod::IndexOf:
            if (expr->args.size() >= 1) {
                auto valueResult = lowerExpr(expr->args[0].value.get());
                TypeRef argType = sema_.typeOf(expr->args[0].value.get());
                Value boxedValue = emitBoxValue(valueResult.value, valueResult.type, argType);
                Value result =
                    emitCallRet(Type(Type::Kind::I64), kListFind, {baseValue, boxedValue});
                return LowerResult{result, Type(Type::Kind::I64)};
            }
            break;

        case CollectionMethod::Has:
        case CollectionMethod::Contains:
            if (expr->args.size() >= 1) {
                auto valueResult = lowerExpr(expr->args[0].value.get());
                TypeRef argType = sema_.typeOf(expr->args[0].value.get());
                Value boxedValue = emitBoxValue(valueResult.value, valueResult.type, argType);
                Value result =
                    emitCallRet(Type(Type::Kind::I1), kListContains, {baseValue, boxedValue});
                return LowerResult{result, Type(Type::Kind::I1)};
            }
            break;

        case CollectionMethod::IsEmpty: {
            Value result =
                emitCallRet(Type(Type::Kind::I1), kCollectionsListGetIsEmpty, {baseValue});
            return LowerResult{result, Type(Type::Kind::I1)};
        }

        case CollectionMethod::Set:
            if (expr->args.size() >= 2) {
                auto indexResult = lowerExpr(expr->args[0].value.get());
                Value indexValue = widenIntegralToI64(indexResult.value, indexResult.type);
                auto valueResult = lowerExpr(expr->args[1].value.get());
                TypeRef argType = sema_.typeOf(expr->args[1].value.get());
                Value boxedValue = emitBoxValue(valueResult.value, valueResult.type, argType);
                emitCall(kListSet, {baseValue, indexValue, boxedValue});
                return LowerResult{Value::constInt(0), Type(Type::Kind::Void)};
            }
            break;

        case CollectionMethod::Add:
        case CollectionMethod::Push: {
            std::vector<Value> args;
            args.reserve(expr->args.size() + 1);
            args.push_back(baseValue);
            for (auto &arg : expr->args) {
                auto result = lowerExpr(arg.value.get());
                TypeRef argType = sema_.typeOf(arg.value.get());
                args.push_back(emitBoxValue(result.value, result.type, argType));
            }
            emitCall(kListAdd, args);
            return LowerResult{Value::constInt(0), Type(Type::Kind::Void)};
        }

        case CollectionMethod::Pop: {
            // Pop removes and returns the last element as a boxed obj.
            Value boxed = emitCallRet(Type(Type::Kind::Ptr), kListPop, {baseValue});
            TypeRef elemType = baseType ? baseType->elementType() : nullptr;
            if (!elemType)
                elemType = sema_.typeOf(expr);
            Type ilElemType = mapType(elemType);
            return emitUnboxValue(boxed, ilElemType, elemType);
        }

        case CollectionMethod::Size:
        case CollectionMethod::Count:
        case CollectionMethod::Length:
        case CollectionMethod::Len: {
            std::vector<Value> args;
            args.push_back(baseValue);
            Value result = emitCallRet(Type(Type::Kind::I64), kListCount, args);
            return LowerResult{result, Type(Type::Kind::I64)};
        }

        case CollectionMethod::Clear: {
            std::vector<Value> args;
            args.push_back(baseValue);
            emitCall(kListClear, args);
            return LowerResult{Value::constInt(0), Type(Type::Kind::Void)};
        }

        case CollectionMethod::Sort:
            emitCall(kCollectionsListSort, {baseValue});
            return LowerResult{Value::constInt(0), Type(Type::Kind::Void)};

        case CollectionMethod::SortDesc:
            emitCall(kCollectionsListSortDesc, {baseValue});
            return LowerResult{Value::constInt(0), Type(Type::Kind::Void)};

        case CollectionMethod::Reverse:
            emitCall(kCollectionsListReverse, {baseValue});
            return LowerResult{Value::constInt(0), Type(Type::Kind::Void)};

        case CollectionMethod::Shuffle:
            emitCall(kCollectionsListShuffle, {baseValue});
            return LowerResult{Value::constInt(0), Type(Type::Kind::Void)};

        default:
            break;
    }

    return std::nullopt;
}

//=============================================================================
// List Combinator Helper (map / filter / reduce / firstWhere / any / all / sum)
//=============================================================================

/// @brief Try to lower a List functional combinator as explicit IL control flow.
/// @param baseValue Runtime list receiver.
/// @param baseType Semantic list type supplying its element type.
/// @param methodName Candidate combinator name.
/// @param expr Call expression containing closure and accumulator arguments.
/// @return Lowered map/filter/reduce/firstWhere/any/all/sum result, or
///         `std::nullopt` for another method.
/// @details Emits indexed loops over a stable receiver slot, invokes closure
///          values through their uniform function/environment ABI, unboxes
///          elements, and removes all scratch slots at the merge block.
std::optional<LowerResult> Lowerer::lowerListCombinator(Value baseValue,
                                                        TypeRef baseType,
                                                        const std::string &methodName,
                                                        CallExpr *expr) {
    const std::string &m = methodName;
    const bool isCombinator = (m == "map" || m == "filter" || m == "reduce" ||
                               m == "firstWhere" || m == "any" || m == "all" || m == "sum");
    if (!isCombinator)
        return std::nullopt;

    TypeRef elemType =
        baseType && baseType->elementType() ? baseType->elementType() : types::unknown();
    Type ilElemType = mapType(elemType);
    const Type i64 = Type(Type::Kind::I64);
    const Type i1 = Type(Type::Kind::I1);
    const Type ptr = Type(Type::Kind::Ptr);

    const std::string tag = std::to_string(nextTempId());
    const std::string listVar = "__cmb_list_" + tag;
    const std::string idxVar = "__cmb_idx_" + tag;
    const std::string lenVar = "__cmb_len_" + tag;

    // Loop scaffolding shared by every combinator: iterate index 0..count over a
    // snapshot of the receiver list held in a slot.
    /// @brief Initializes list, index, and length slots for a combinator loop.
    /// @param list List value to iterate.
    auto beginLoop = [&](Value list) {
        createSlot(listVar, ptr);
        createSlot(idxVar, i64);
        createSlot(lenVar, i64);
        storeToSlot(listVar, list, ptr);
        storeToSlot(idxVar, Value::constInt(0), i64);
        storeToSlot(lenVar, emitCallRet(i64, kListCount, {list}), i64);
    };
    /// @brief Removes the temporary combinator-loop slots.
    auto cleanupLoop = [&]() {
        removeSlot(listVar);
        removeSlot(idxVar);
        removeSlot(lenVar);
    };

    // Load and unbox the current element inside a loop body.
    /// @brief Loads and unboxes the current list element.
    /// @return Current element and its IL type.
    auto currentElement = [&]() -> LowerResult {
        Value list = loadFromSlot(listVar, ptr);
        Value idx = loadFromSlot(idxVar, i64);
        Value boxed = emitCallRet(ptr, kListGet, {list, idx});
        return emitUnboxValue(boxed, ilElemType, elemType);
    };
    /// @brief Advances the combinator-loop index by one.
    auto advance = [&]() {
        Value idx = loadFromSlot(idxVar, i64);
        storeToSlot(idxVar, emitBinary(Opcode::IAddOvf, i64, idx, Value::constInt(1)), i64);
    };

    // Invoke the closure argument at `argIndex` with the given arguments.
    /// @brief Invokes a closure argument using the uniform environment-first ABI.
    /// @param argIndex Call-expression argument containing the closure.
    /// @param callArgs User arguments passed after the closure environment.
    /// @param retIl Expected IL return type.
    /// @return Indirect-call result.
    auto callClosure = [&](size_t argIndex, const std::vector<Value> &callArgs,
                           Type retIl) -> Value {
        auto closure = lowerExpr(expr->args[argIndex].value.get());
        Value funcPtr = emitLoad(closure.value, ptr);
        Value envPtr = emitLoad(emitGEP(closure.value, /*kClosureEnvOffset=*/8), ptr);
        std::vector<Value> full;
        full.reserve(callArgs.size() + 1);
        full.push_back(envPtr);
        for (const auto &a : callArgs)
            full.push_back(a);
        return emitCallIndirectRet(retIl, funcPtr, full);
    };

    // --- sum: no closure; accumulate element values ---
    if (m == "sum") {
        const bool isFloat = elemType && elemType->kind == TypeKindSem::Number;
        const Type accIl = isFloat ? Type(Type::Kind::F64) : i64;
        const std::string accVar = "__cmb_acc_" + tag;
        beginLoop(baseValue);
        createSlot(accVar, accIl);
        storeToSlot(accVar, isFloat ? Value::constFloat(0.0) : Value::constInt(0), accIl);

        size_t cond = createBlock("sum_cond"), body = createBlock("sum_body"),
               end = createBlock("sum_end");
        emitBr(cond);
        setBlock(cond);
        emitCBr(emitBinary(Opcode::SCmpLT, i1, loadFromSlot(idxVar, i64), loadFromSlot(lenVar, i64)),
                body, end);
        setBlock(body);
        LowerResult elem = currentElement();
        Value acc = loadFromSlot(accVar, accIl);
        storeToSlot(accVar, emitBinary(isFloat ? Opcode::FAdd : Opcode::IAddOvf, accIl, acc,
                                       elem.value),
                    accIl);
        advance();
        emitBr(cond);
        setBlock(end);
        Value result = loadFromSlot(accVar, accIl);
        removeSlot(accVar);
        cleanupLoop();
        return LowerResult{result, accIl};
    }

    // --- reduce(initial, (acc, item) => acc) ---
    if (m == "reduce") {
        auto init = lowerExpr(expr->args[0].value.get());
        TypeRef accType = sema_.typeOf(expr->args[0].value.get());
        Type accIl = accType ? mapType(accType) : init.type;
        const std::string accVar = "__cmb_acc_" + tag;
        createSlot(accVar, accIl);
        storeToSlot(accVar, init.value, accIl);
        beginLoop(baseValue);

        size_t cond = createBlock("reduce_cond"), body = createBlock("reduce_body"),
               end = createBlock("reduce_end");
        emitBr(cond);
        setBlock(cond);
        emitCBr(emitBinary(Opcode::SCmpLT, i1, loadFromSlot(idxVar, i64), loadFromSlot(lenVar, i64)),
                body, end);
        setBlock(body);
        LowerResult elem = currentElement();
        Value acc = loadFromSlot(accVar, accIl);
        Value next = callClosure(1, {acc, elem.value}, accIl);
        storeToSlot(accVar, next, accIl);
        advance();
        emitBr(cond);
        setBlock(end);
        Value result = loadFromSlot(accVar, accIl);
        removeSlot(accVar);
        cleanupLoop();
        return LowerResult{result, accIl};
    }

    // --- map: build a new list of the closure's results ---
    if (m == "map") {
        TypeRef fnType = sema_.typeOf(expr->args[0].value.get());
        TypeRef outElem = (fnType && fnType->kind == TypeKindSem::Function) ? fnType->returnType()
                                                                            : types::unknown();
        Type outElemIl = mapType(outElem);
        Value outList = emitCallRet(ptr, kListNew, {});
        const std::string outVar = "__cmb_out_" + tag;
        createSlot(outVar, ptr);
        storeToSlot(outVar, outList, ptr);
        beginLoop(baseValue);

        size_t cond = createBlock("map_cond"), body = createBlock("map_body"),
               end = createBlock("map_end");
        emitBr(cond);
        setBlock(cond);
        emitCBr(emitBinary(Opcode::SCmpLT, i1, loadFromSlot(idxVar, i64), loadFromSlot(lenVar, i64)),
                body, end);
        setBlock(body);
        LowerResult elem = currentElement();
        Value mapped = callClosure(0, {elem.value}, outElemIl);
        Value boxed = emitBoxValue(mapped, outElemIl, outElem);
        emitCall(kListAdd, {loadFromSlot(outVar, ptr), boxed});
        advance();
        emitBr(cond);
        setBlock(end);
        Value result = loadFromSlot(outVar, ptr);
        removeSlot(outVar);
        cleanupLoop();
        return LowerResult{result, ptr};
    }

    // --- filter: keep elements for which the predicate is true ---
    if (m == "filter") {
        Value outList = emitCallRet(ptr, kListNew, {});
        const std::string outVar = "__cmb_out_" + tag;
        createSlot(outVar, ptr);
        storeToSlot(outVar, outList, ptr);
        beginLoop(baseValue);

        size_t cond = createBlock("filter_cond"), body = createBlock("filter_body"),
               keep = createBlock("filter_keep"), next = createBlock("filter_next"),
               end = createBlock("filter_end");
        emitBr(cond);
        setBlock(cond);
        emitCBr(emitBinary(Opcode::SCmpLT, i1, loadFromSlot(idxVar, i64), loadFromSlot(lenVar, i64)),
                body, end);
        setBlock(body);
        LowerResult elem = currentElement();
        Value pred = callClosure(0, {elem.value}, i1);
        emitCBr(pred, keep, next);
        setBlock(keep);
        emitCall(kListAdd, {loadFromSlot(outVar, ptr), emitBoxValue(elem.value, ilElemType, elemType)});
        emitBr(next);
        setBlock(next);
        advance();
        emitBr(cond);
        setBlock(end);
        Value result = loadFromSlot(outVar, ptr);
        removeSlot(outVar);
        cleanupLoop();
        return LowerResult{result, ptr};
    }

    // --- any / all / firstWhere: short-circuit predicate scans ---
    const bool isAny = (m == "any");
    const bool isAll = (m == "all");
    const bool isFirst = (m == "firstWhere");

    const std::string resVar = "__cmb_res_" + tag;
    TypeRef optType = types::optional(elemType);
    const Type resIl = isFirst ? mapType(optType) : i1;
    createSlot(resVar, resIl);
    if (isFirst)
        storeToSlot(resVar, Value::null(), resIl); // optional element defaults to null
    else
        storeToSlot(resVar, Value::constBool(isAll), resIl); // any:false, all:true
    beginLoop(baseValue);

    size_t cond = createBlock("scan_cond"), body = createBlock("scan_body"),
           hit = createBlock("scan_hit"), next = createBlock("scan_next"),
           end = createBlock("scan_end");
    emitBr(cond);
    setBlock(cond);
    emitCBr(emitBinary(Opcode::SCmpLT, i1, loadFromSlot(idxVar, i64), loadFromSlot(lenVar, i64)),
            body, end);
    setBlock(body);
    LowerResult elem = currentElement();
    Value pred = callClosure(0, {elem.value}, i1);
    // any/firstWhere fire on true; all fires (records false and stops) on false.
    if (isAll) {
        emitCBr(pred, next, hit);
    } else {
        emitCBr(pred, hit, next);
    }
    setBlock(hit);
    if (isFirst) {
        auto wrapped = emitOptionalWrap(elem.value, elemType);
        storeToSlot(resVar, wrapped, resIl);
    } else {
        storeToSlot(resVar, Value::constBool(isAny), resIl); // any->true, all->false
    }
    emitBr(end); // short-circuit out of the loop
    setBlock(next);
    advance();
    emitBr(cond);
    setBlock(end);
    Value result = loadFromSlot(resVar, resIl);
    removeSlot(resVar);
    cleanupLoop();
    return LowerResult{result, resIl};
}

//=============================================================================
// Map Method Call Helper
//=============================================================================

/// @brief Try to lower a built-in Map method call.
/// @param baseValue Runtime map receiver.
/// @param baseType Semantic map type supplying key and value types.
/// @param methodName Source method name.
/// @param expr Call expression containing method arguments.
/// @return Lowered result when @p methodName is recognized; otherwise
///         `std::nullopt`.
/// @details Selects string-keyed Map or integer-keyed IntMap runtime helpers,
///          coerces keys to their ABI representation, and boxes or unboxes map
///          values at the language boundary.
std::optional<LowerResult> Lowerer::lowerMapMethodCall(Value baseValue,
                                                       TypeRef baseType,
                                                       const std::string &methodName,
                                                       CallExpr *expr) {
    TypeRef valType = baseType->typeArgs.size() > 1 ? baseType->typeArgs[1] : nullptr;
    const bool integerKeyed = usesIntegerMapRuntime(baseType);
    const char *setHelper = integerKeyed ? kIntMapSet : kMapSet;
    const char *getHelper = integerKeyed ? kIntMapGet : kMapGet;
    const char *getOrHelper = integerKeyed ? kIntMapGetOr : kMapGetOr;
    const char *hasHelper = integerKeyed ? kIntMapContainsKey : kMapContainsKey;
    const char *countHelper = integerKeyed ? kIntMapCount : kMapCount;
    const char *removeHelper = integerKeyed ? kIntMapRemove : kMapRemove;
    const char *clearHelper = integerKeyed ? kIntMapClear : kMapClear;
    const char *keysHelper = integerKeyed ? kIntMapKeys : kMapKeys;
    const char *valuesHelper = integerKeyed ? kIntMapValues : kMapValues;

    /// @brief Lowers and coerces a map key to its runtime representation.
    /// @param keyExpr Map key expression.
    /// @return Runtime map-key value.
    auto lowerRuntimeKey = [&](Expr *keyExpr) {
        auto keyResult = lowerExpr(keyExpr);
        return coerceMapKeyForRuntime(keyResult.value, keyResult.type, baseType);
    };

    // Use O(1) dispatch table lookup instead of sequential string comparisons
    const CollectionMethod method = il::frontends::common::lookupCollectionMethod(methodName);

    switch (method) {
        case CollectionMethod::Set:
        case CollectionMethod::Put:
            if (expr->args.size() >= 2) {
                Value runtimeKey = lowerRuntimeKey(expr->args[0].value.get());
                auto valueResult = lowerExpr(expr->args[1].value.get());
                TypeRef argType = sema_.typeOf(expr->args[1].value.get());
                Value boxedValue = emitBoxValue(valueResult.value, valueResult.type, argType);
                emitCall(setHelper, {baseValue, runtimeKey, boxedValue});
                return LowerResult{Value::constInt(0), Type(Type::Kind::Void)};
            }
            break;

        case CollectionMethod::Get:
            if (expr->args.size() >= 1) {
                Value runtimeKey = lowerRuntimeKey(expr->args[0].value.get());
                TypeRef resultType = sema_.typeOf(expr);
                Type resultIlType = resultType ? mapType(resultType) : Type(Type::Kind::Ptr);
                if (resultType && resultType->kind == TypeKindSem::Optional) {
                    if (valType && valType->kind == TypeKindSem::String) {
                        if (!integerKeyed) {
                            Value str = emitCallRet(
                                Type(Type::Kind::Str), kMapGetOptStr, {baseValue, runtimeKey});
                            return LowerResult{str, Type(Type::Kind::Str)};
                        }

                        std::string slotName =
                            "__intmap_get_opt_str_" + std::to_string(nextTempId());
                        createSlot(slotName, Type(Type::Kind::Str));
                        Value boxed =
                            emitCallRet(Type(Type::Kind::Ptr), getHelper, {baseValue, runtimeKey});
                        Value hasValue = emitPointerIsNonNull(boxed, Type(Type::Kind::Ptr));

                        size_t hasValueIdx = createBlock("intmap_get_str_has");
                        size_t missingIdx = createBlock("intmap_get_str_missing");
                        size_t mergeIdx = createBlock("intmap_get_str_merge");
                        emitCBr(hasValue, hasValueIdx, missingIdx);

                        setBlock(hasValueIdx);
                        Value str = emitCallRet(Type(Type::Kind::Str), kUnboxStr, {boxed});
                        storeToSlot(slotName, str, Type(Type::Kind::Str));
                        emitBr(mergeIdx);

                        setBlock(missingIdx);
                        storeToSlot(slotName, Value::null(), Type(Type::Kind::Ptr));
                        emitBr(mergeIdx);

                        setBlock(mergeIdx);
                        Value result = loadFromSlot(slotName, Type(Type::Kind::Str));
                        removeSlot(slotName);
                        return LowerResult{result, Type(Type::Kind::Str)};
                    }

                    Value boxed =
                        emitCallRet(Type(Type::Kind::Ptr), getHelper, {baseValue, runtimeKey});
                    return LowerResult{boxed, resultIlType};
                }

                Value boxed =
                    emitCallRet(Type(Type::Kind::Ptr), getHelper, {baseValue, runtimeKey});
                if (valType)
                    return emitUnboxValue(boxed, mapType(valType), valType);
                return LowerResult{boxed, Type(Type::Kind::Ptr)};
            }
            break;

        case CollectionMethod::GetOr:
            if (expr->args.size() >= 2) {
                Value runtimeKey = lowerRuntimeKey(expr->args[0].value.get());
                auto defaultResult = lowerExpr(expr->args[1].value.get());
                TypeRef argType = sema_.typeOf(expr->args[1].value.get());
                Value boxedDefault = emitBoxValue(defaultResult.value, defaultResult.type, argType);
                Value boxed = emitCallRet(
                    Type(Type::Kind::Ptr), getOrHelper, {baseValue, runtimeKey, boxedDefault});
                if (valType) {
                    Type ilValueType = mapType(valType);
                    return emitUnboxValue(boxed, ilValueType, valType);
                }
                return LowerResult{boxed, Type(Type::Kind::Ptr)};
            }
            break;

        case CollectionMethod::ContainsKey:
        case CollectionMethod::HasKey:
        case CollectionMethod::Has:
            if (expr->args.size() >= 1) {
                Value runtimeKey = lowerRuntimeKey(expr->args[0].value.get());
                Value result =
                    emitCallRet(Type(Type::Kind::I1), hasHelper, {baseValue, runtimeKey});
                return LowerResult{result, Type(Type::Kind::I1)};
            }
            break;

        case CollectionMethod::Size:
        case CollectionMethod::Count:
        case CollectionMethod::Length:
        case CollectionMethod::Len: {
            Value result = emitCallRet(Type(Type::Kind::I64), countHelper, {baseValue});
            return LowerResult{result, Type(Type::Kind::I64)};
        }

        case CollectionMethod::Remove:
            if (expr->args.size() >= 1) {
                Value runtimeKey = lowerRuntimeKey(expr->args[0].value.get());
                Value result =
                    emitCallRet(Type(Type::Kind::I1), removeHelper, {baseValue, runtimeKey});
                return LowerResult{result, Type(Type::Kind::I1)};
            }
            break;

        case CollectionMethod::SetIfMissing:
            if (expr->args.size() >= 2) {
                Value runtimeKey = lowerRuntimeKey(expr->args[0].value.get());
                auto valueResult = lowerExpr(expr->args[1].value.get());
                TypeRef argType = sema_.typeOf(expr->args[1].value.get());
                Value boxedValue = emitBoxValue(valueResult.value, valueResult.type, argType);
                if (integerKeyed) {
                    std::string slotName =
                        "__intmap_set_if_missing_" + std::to_string(nextTempId());
                    createSlot(slotName, Type(Type::Kind::I1));
                    Value exists =
                        emitCallRet(Type(Type::Kind::I1), hasHelper, {baseValue, runtimeKey});

                    size_t existsIdx = createBlock("intmap_set_missing_exists");
                    size_t insertIdx = createBlock("intmap_set_missing_insert");
                    size_t mergeIdx = createBlock("intmap_set_missing_merge");
                    emitCBr(exists, existsIdx, insertIdx);

                    setBlock(existsIdx);
                    storeToSlot(slotName, Value::constBool(false), Type(Type::Kind::I1));
                    emitBr(mergeIdx);

                    setBlock(insertIdx);
                    emitCall(setHelper, {baseValue, runtimeKey, boxedValue});
                    storeToSlot(slotName, Value::constBool(true), Type(Type::Kind::I1));
                    emitBr(mergeIdx);

                    setBlock(mergeIdx);
                    Value result = loadFromSlot(slotName, Type(Type::Kind::I1));
                    removeSlot(slotName);
                    return LowerResult{result, Type(Type::Kind::I1)};
                }
                Value result = emitCallRet(
                    Type(Type::Kind::I1), kMapSetIfMissing, {baseValue, runtimeKey, boxedValue});
                return LowerResult{result, Type(Type::Kind::I1)};
            }
            break;

        case CollectionMethod::Clear:
            emitCall(clearHelper, {baseValue});
            return LowerResult{Value::constInt(0), Type(Type::Kind::Void)};

        case CollectionMethod::Keys: {
            Value seq = emitCallRet(Type(Type::Kind::Ptr), keysHelper, {baseValue});
            return LowerResult{seq, Type(Type::Kind::Ptr)};
        }

        case CollectionMethod::Values: {
            Value seq = emitCallRet(Type(Type::Kind::Ptr), valuesHelper, {baseValue});
            return LowerResult{seq, Type(Type::Kind::Ptr)};
        }

        default:
            break;
    }

    return std::nullopt;
}

//=============================================================================
// Set Method Call Helper
//=============================================================================

/// @brief Try to lower a built-in Set method call.
/// @param baseValue Runtime set receiver.
/// @param baseType Semantic set type supplying its element type.
/// @param methodName Source method name.
/// @param expr Call expression containing method arguments.
/// @return Lowered result when @p methodName is recognized; otherwise
///         `std::nullopt`.
std::optional<LowerResult> Lowerer::lowerSetMethodCall(Value baseValue,
                                                       TypeRef baseType,
                                                       const std::string &methodName,
                                                       CallExpr *expr) {
    const CollectionMethod method = il::frontends::common::lookupCollectionMethod(methodName);

    switch (method) {
        case CollectionMethod::Has:
        case CollectionMethod::Contains:
            if (expr->args.size() >= 1) {
                auto valueResult = lowerExpr(expr->args[0].value.get());
                TypeRef argType = sema_.typeOf(expr->args[0].value.get());
                Value boxedValue = emitBoxValue(valueResult.value, valueResult.type, argType);
                Value result = emitCallRet(Type(Type::Kind::I1), kSetHas, {baseValue, boxedValue});
                return LowerResult{result, Type(Type::Kind::I1)};
            }
            break;

        case CollectionMethod::Add:
        case CollectionMethod::Put:
            if (expr->args.size() >= 1) {
                auto valueResult = lowerExpr(expr->args[0].value.get());
                TypeRef argType = sema_.typeOf(expr->args[0].value.get());
                Value boxedValue = emitBoxValue(valueResult.value, valueResult.type, argType);
                Value result = emitCallRet(Type(Type::Kind::I1), kSetPut, {baseValue, boxedValue});
                return LowerResult{result, Type(Type::Kind::I1)};
            }
            break;

        case CollectionMethod::Remove:
            if (expr->args.size() >= 1) {
                auto valueResult = lowerExpr(expr->args[0].value.get());
                TypeRef argType = sema_.typeOf(expr->args[0].value.get());
                Value boxedValue = emitBoxValue(valueResult.value, valueResult.type, argType);
                Value result = emitCallRet(Type(Type::Kind::I1), kSetDrop, {baseValue, boxedValue});
                return LowerResult{result, Type(Type::Kind::I1)};
            }
            break;

        case CollectionMethod::Size:
        case CollectionMethod::Count:
        case CollectionMethod::Length:
        case CollectionMethod::Len: {
            Value result = emitCallRet(Type(Type::Kind::I64), kSetCount, {baseValue});
            return LowerResult{result, Type(Type::Kind::I64)};
        }

        case CollectionMethod::Clear:
            emitCall(kSetClear, {baseValue});
            return LowerResult{Value::constInt(0), Type(Type::Kind::Void)};

        default:
            break;
    }

    return std::nullopt;
}

//=============================================================================
// Method Call Helper
//=============================================================================

/// @brief Emit a direct call to a resolved user method.
/// @param method Resolved method declaration.
/// @param typeName Qualified owning-type name.
/// @param selfValue Receiver passed as the first argument.
/// @param expr Call expression containing explicit arguments.
/// @return Materialized method result, or a void placeholder.
/// @details Uses cached substituted parameter/return types, semantic argument
///          bindings, and generic-method erasure metadata. Concrete generic
///          return values are unboxed from an erased pointer representation.
LowerResult Lowerer::lowerMethodCall(MethodDecl *method,
                                     const std::string &typeName,
                                     Value selfValue,
                                     CallExpr *expr) {
    // Look up the cached method type - this has already-substituted types for generics
    TypeRef methodType = sema_.getMethodType(typeName, method);
    if (!methodType)
        methodType = sema_.getMethodType(typeName, method->name);
    TypeRef concreteGenericType = sema_.genericMethodConcreteType(expr);
    TypeRef erasedGenericType = sema_.genericMethodErasedType(expr);
    if (erasedGenericType)
        methodType = erasedGenericType;
    std::vector<TypeRef> paramTypes;
    TypeRef returnType = types::voidType();
    if (methodType && methodType->kind == TypeKindSem::Function) {
        paramTypes = methodType->paramTypes();
        returnType = methodType->returnType();
    }

    std::vector<Value> args;
    args.reserve(paramTypes.size() + 1);
    args.push_back(selfValue);
    std::vector<Value> boundArgs =
        lowerResolvedCallArgs(expr, paramTypes, method ? &method->params : nullptr);
    args.insert(args.end(), boundArgs.begin(), boundArgs.end());

    Type ilReturnType = mapType(returnType);

    std::string methodName = sema_.loweredMethodName(typeName, method);
    if (methodName.empty())
        methodName = typeName + "." + method->name;

    // Handle void return types correctly - don't try to store void results
    if (ilReturnType.kind == Type::Kind::Void) {
        emitCall(methodName, args);
        return {Value::constInt(0), Type(Type::Kind::Void)};
    } else {
        Value result = emitCallRet(ilReturnType, methodName, args);
        if (concreteGenericType && concreteGenericType->kind == TypeKindSem::Function) {
            TypeRef concreteReturn = concreteGenericType->returnType();
            if (returnType && returnType->kind == TypeKindSem::TypeParam && concreteReturn &&
                concreteReturn->kind != TypeKindSem::TypeParam) {
                Type concreteIlReturn = mapType(concreteReturn);
                return emitUnboxValue(result, concreteIlReturn, concreteReturn);
            }
        }
        return materializeCallResult(result, returnType, ilReturnType);
    }
}

//=============================================================================
// Value Type Construction Helper
//=============================================================================

/// @brief Try to construct a value type through `TypeName(...)` syntax.
/// @param typeName Qualified struct name.
/// @param expr Type-call expression containing constructor arguments.
/// @return Pointer to initialized inline stack storage, or `std::nullopt` when
///         @p typeName is not a known struct.
/// @details Calls an explicit resolved `init` method when present; otherwise
///          initializes fields in declaration order from bound arguments and
///          defaults with semantic coercion.
std::optional<LowerResult> Lowerer::lowerStructTypeConstruction(const std::string &typeName,
                                                                CallExpr *expr) {
    const StructTypeInfo *infoPtr = getOrCreateStructTypeInfo(typeName);
    if (!infoPtr)
        return std::nullopt;

    const StructTypeInfo &info = *infoPtr;

    // Allocate stack space for the value
    unsigned allocaId = nextTempId();
    il::core::Instr allocaInstr;
    allocaInstr.result = allocaId;
    allocaInstr.op = Opcode::Alloca;
    allocaInstr.type = Type(Type::Kind::Ptr);
    allocaInstr.operands = {Value::constInt(static_cast<int64_t>(info.totalSize))};
    allocaInstr.loc = curLoc_;
    blockMgr_.currentBlock()->instructions.push_back(allocaInstr);
    Value ptr = Value::temp(allocaId);

    MethodDecl *resolvedInit = sema_.resolvedTypeCallInitDecl(expr);
    std::string initOwner = sema_.resolvedTypeCallInitOwnerType(expr);
    auto initIt = info.methodMap.find("init");
    if (resolvedInit || initIt != info.methodMap.end()) {
        // Call the explicit init method (like class types do)
        MethodDecl *initDecl = resolvedInit ? resolvedInit : initIt->second;
        if (initOwner.empty())
            initOwner = typeName;
        std::string initName = sema_.loweredMethodName(initOwner, initDecl);
        if (initName.empty())
            initName = initOwner + ".init";
        std::vector<Value> initArgs;
        initArgs.push_back(ptr); // self is first argument
        TypeRef initType = sema_.getMethodType(initOwner, initDecl);
        std::vector<TypeRef> initParamTypes =
            initType ? initType->paramTypes() : std::vector<TypeRef>{};
        std::vector<Value> argValues =
            lowerResolvedCallArgs(expr, initParamTypes, initDecl ? &initDecl->params : nullptr);
        for (const auto &argVal : argValues) {
            initArgs.push_back(argVal);
        }
        emitCall(initName, initArgs);
    } else {
        auto loweredSources = lowerSourceArgs(expr->args);
        const auto *binding = sema_.callArgBinding(expr);
        std::vector<const FieldDecl *> fieldDecls = collectStructFieldDecls(sema_, typeName);

        // No init method - store arguments directly into fields.
        for (size_t i = 0; i < info.fields.size(); ++i) {
            const FieldLayout &field = info.fields[i];
            Value fieldValue;
            int sourceIndex = binding && i < binding->fixedParamSources.size()
                                  ? binding->fixedParamSources[i]
                                  : (i < loweredSources.size() ? static_cast<int>(i) : -1);

            if (sourceIndex >= 0) {
                size_t idx = toIndex(sourceIndex);
                TypeRef argType = sema_.typeOf(expr->args[idx].value.get());
                auto coerced = coerceValueToType(
                    loweredSources[idx].value, loweredSources[idx].type, argType, field.type);
                fieldValue = coerced.value;
            } else if (i < fieldDecls.size() && fieldDecls[i]->initializer) {
                auto initValue = lowerExpr(fieldDecls[i]->initializer.get());
                TypeRef initType = sema_.typeOf(fieldDecls[i]->initializer.get());
                auto coerced =
                    coerceValueToType(initValue.value, initValue.type, initType, field.type);
                fieldValue = coerced.value;
            } else {
                Type ilFieldType = mapType(field.type);
                switch (ilFieldType.kind) {
                    case Type::Kind::I1:
                        fieldValue = Value::constBool(false);
                        break;
                    case Type::Kind::I64:
                    case Type::Kind::I16:
                    case Type::Kind::I32:
                        fieldValue = Value::constInt(0);
                        break;
                    case Type::Kind::F64:
                        fieldValue = Value::constFloat(0.0);
                        break;
                    case Type::Kind::Str:
                        fieldValue = emitEmptyString();
                        break;
                    case Type::Kind::Ptr:
                        fieldValue = Value::null();
                        break;
                    default:
                        fieldValue = Value::constInt(0);
                        break;
                }
            }

            // GEP to get field address
            unsigned gepId = nextTempId();
            il::core::Instr gepInstr;
            gepInstr.result = gepId;
            gepInstr.op = Opcode::GEP;
            gepInstr.type = Type(Type::Kind::Ptr);
            gepInstr.operands = {ptr, Value::constInt(static_cast<int64_t>(field.offset))};
            gepInstr.loc = curLoc_;
            blockMgr_.currentBlock()->instructions.push_back(gepInstr);
            Value fieldAddr = Value::temp(gepId);

            // Store the value
            il::core::Instr storeInstr;
            storeInstr.op = Opcode::Store;
            storeInstr.type = mapType(field.type);
            storeInstr.operands = {fieldAddr, fieldValue};
            storeInstr.loc = curLoc_;
            blockMgr_.currentBlock()->instructions.push_back(storeInstr);
        }
    }

    return LowerResult{ptr, Type(Type::Kind::Ptr)};
}

//=============================================================================
// Entity Type Construction Helper
//=============================================================================

/// @brief Try to construct a class through `TypeName(...)` syntax.
/// @param typeName Qualified class name.
/// @param expr Type-call expression containing constructor arguments.
/// @return Owned heap object, or `std::nullopt` when @p typeName is not a
///         known class.
/// @details Allocates a runtime object carrying the class identifier, then
///          calls an explicit resolved `init` method or initializes fields in
///          declaration order with defaults, weak-field handling, and
///          ownership-safe inline stores.
std::optional<LowerResult> Lowerer::lowerClassTypeConstruction(const std::string &typeName,
                                                               CallExpr *expr) {
    const ClassTypeInfo *infoPtr = getOrCreateClassTypeInfo(typeName);
    if (!infoPtr)
        return std::nullopt;

    const ClassTypeInfo &info = *infoPtr;

    // Allocate heap memory for the class using rt_obj_new_i64
    Value ptr = emitCallRet(Type(Type::Kind::Ptr),
                            "rt_obj_new_i64",
                            {Value::constInt(static_cast<int64_t>(info.classId)),
                             Value::constInt(static_cast<int64_t>(info.totalSize))});

    // Check if the class has an explicit init method
    MethodDecl *resolvedInit = sema_.resolvedTypeCallInitDecl(expr);
    std::string initOwner = sema_.resolvedTypeCallInitOwnerType(expr);
    auto initIt = info.methodMap.find("init");
    if (resolvedInit || initIt != info.methodMap.end()) {
        // Call the explicit init method
        MethodDecl *initDecl = resolvedInit ? resolvedInit : initIt->second;
        if (initOwner.empty())
            initOwner = typeName;
        std::string initName = sema_.loweredMethodName(initOwner, initDecl);
        if (initName.empty())
            initName = initOwner + ".init";
        std::vector<Value> initArgs;
        initArgs.push_back(ptr); // self is first argument
        TypeRef initType = sema_.getMethodType(initOwner, initDecl);
        std::vector<TypeRef> initParamTypes =
            initType ? initType->paramTypes() : std::vector<TypeRef>{};
        std::vector<Value> argValues =
            lowerResolvedCallArgs(expr, initParamTypes, initDecl ? &initDecl->params : nullptr);
        for (const auto &argVal : argValues) {
            initArgs.push_back(argVal);
        }
        emitCall(initName, initArgs);
    } else {
        auto loweredSources = lowerSourceArgs(expr->args);
        const auto *binding = sema_.callArgBinding(expr);
        std::vector<const FieldDecl *> fieldDecls;
        appendClassFieldDecls(sema_, typeName, fieldDecls);

        // No explicit init - do inline field initialization
        for (size_t i = 0; i < info.fields.size(); ++i) {
            const auto &field = info.fields[i];
            std::optional<Value> fieldValue;

            int sourceIndex = binding && i < binding->fixedParamSources.size()
                                  ? binding->fixedParamSources[i]
                                  : (i < loweredSources.size() ? static_cast<int>(i) : -1);
            if (sourceIndex >= 0) {
                size_t idx = toIndex(sourceIndex);
                TypeRef argType = sema_.typeOf(expr->args[idx].value.get());
                auto coerced = coerceValueToType(
                    loweredSources[idx].value, loweredSources[idx].type, argType, field.type);
                fieldValue = coerced.value;
            } else if (i < fieldDecls.size() && fieldDecls[i]->initializer) {
                auto initValue = lowerExpr(fieldDecls[i]->initializer.get());
                TypeRef initType = sema_.typeOf(fieldDecls[i]->initializer.get());
                auto coerced =
                    coerceValueToType(initValue.value, initValue.type, initType, field.type);
                fieldValue = coerced.value;
            } else if (!field.type || (field.type->kind != TypeKindSem::Struct &&
                                       field.type->kind != TypeKindSem::FixedArray &&
                                       field.type->kind != TypeKindSem::Tuple)) {
                Type ilFieldType = mapType(field.type);
                switch (ilFieldType.kind) {
                    case Type::Kind::I1:
                        fieldValue = Value::constBool(false);
                        break;
                    case Type::Kind::I64:
                    case Type::Kind::I16:
                    case Type::Kind::I32:
                        fieldValue = Value::constInt(0);
                        break;
                    case Type::Kind::F64:
                        fieldValue = Value::constFloat(0.0);
                        break;
                    case Type::Kind::Str:
                        fieldValue = emitEmptyString();
                        break;
                    case Type::Kind::Ptr:
                        fieldValue = Value::null();
                        break;
                    default:
                        fieldValue = Value::constInt(0);
                        break;
                }
            }

            Value fieldAddr = emitGEP(ptr, static_cast<int64_t>(field.offset));
            if (field.isWeak && fieldValue) {
                emitFieldStore(&field, ptr, *fieldValue);
            } else if (fieldValue) {
                emitInlineValueStore(field.type, fieldAddr, *fieldValue, false);
            } else {
                emitInlineValueZero(field.type, fieldAddr);
            }
        }
    }

    return LowerResult{ptr, Type(Type::Kind::Ptr)};
}

//=============================================================================
// Struct-Literal Lowering (ZIA-001)
//=============================================================================

/// @brief Lower a struct-literal expression: `TypeName { field = val, ... }`.
/// @param expr Struct literal with named field initializers.
/// @return Pointer to initialized inline stack storage.
/// @details Reorders the named fields by declaration order, then delegates to
///          the same ownership and coercion rules used by type construction;
///          omitted fields receive semantic zero values.
LowerResult Lowerer::lowerStructLiteral(StructLiteralExpr *expr) {
    TypeRef literalType = sema_.typeOf(expr);
    const std::string typeName =
        literalType && !literalType->name.empty() ? literalType->name : expr->typeName;
    const StructTypeInfo *infoPtr = getOrCreateStructTypeInfo(typeName);
    if (!infoPtr) {
        // Fallback: treat as a zero-initialised value (unreachable after sema checks)
        return {Value::constInt(0), Type(Type::Kind::Ptr)};
    }
    const StructTypeInfo &info = *infoPtr;

    struct LoweredLiteralField {
        Value value;
        Type ilType{Type::Kind::Void};
        TypeRef semanticType;
    };

    // Build a map from field name to lowered value for quick lookup.
    std::unordered_map<std::string, LoweredLiteralField> fieldValues;
    for (auto &f : expr->fields) {
        auto result = lowerExpr(f.value.get());
        fieldValues[f.name] = {result.value, result.type, sema_.typeOf(f.value.get())};
    }

    std::vector<const FieldDecl *> fieldDecls = collectStructFieldDecls(sema_, typeName);

    /// @brief Produces the zero/default value for a struct field IL type.
    /// @param ilType Field IL type.
    /// @return Type-appropriate default value.
    auto typedDefaultFor = [&](Type ilType) -> Value {
        switch (ilType.kind) {
            case Type::Kind::I1:
                return Value::constBool(false);
            case Type::Kind::I64:
            case Type::Kind::I16:
            case Type::Kind::I32:
                return Value::constInt(0);
            case Type::Kind::F64:
                return Value::constFloat(0.0);
            case Type::Kind::Str:
                return emitEmptyString();
            case Type::Kind::Ptr:
                return Value::null();
            default:
                return Value::constInt(0);
        }
    };

    // Build arg list in field declaration order (matches init parameter order),
    // coercing named literal values and honoring field initializers/defaults.
    std::vector<Value> argValues;
    argValues.reserve(info.fields.size());
    for (size_t i = 0; i < info.fields.size(); ++i) {
        const auto &field = info.fields[i];
        Type ilFieldType = mapType(field.type);
        auto it = fieldValues.find(field.name);
        if (it != fieldValues.end()) {
            auto coerced = coerceValueToType(
                it->second.value, it->second.ilType, it->second.semanticType, field.type);
            argValues.push_back(coerced.value);
        } else if (i < fieldDecls.size() && fieldDecls[i]->initializer) {
            auto initValue = lowerExpr(fieldDecls[i]->initializer.get());
            TypeRef initType = sema_.typeOf(fieldDecls[i]->initializer.get());
            auto coerced = coerceValueToType(initValue.value, initValue.type, initType, field.type);
            argValues.push_back(coerced.value);
        } else {
            argValues.push_back(typedDefaultFor(ilFieldType));
        }
    }

    // Allocate stack space for the value.
    unsigned allocaId = nextTempId();
    il::core::Instr allocaInstr;
    allocaInstr.result = allocaId;
    allocaInstr.op = Opcode::Alloca;
    allocaInstr.type = Type(Type::Kind::Ptr);
    allocaInstr.operands = {Value::constInt(static_cast<int64_t>(info.totalSize))};
    allocaInstr.loc = curLoc_;
    blockMgr_.currentBlock()->instructions.push_back(allocaInstr);
    Value ptr = Value::temp(allocaId);

    // If an explicit init method exists, call it (same as lowerStructTypeConstruction).
    auto initIt = info.methodMap.find("init");
    if (initIt != info.methodMap.end()) {
        std::string initName = sema_.loweredMethodName(typeName, initIt->second);
        if (initName.empty())
            initName = typeName + ".init";
        std::vector<Value> initArgs;
        initArgs.push_back(ptr); // self is first argument
        for (const auto &argVal : argValues)
            initArgs.push_back(argVal);
        emitCall(initName, initArgs);
    } else {
        // No init method — store args directly into fields by declaration order.
        for (size_t i = 0; i < argValues.size() && i < info.fields.size(); ++i) {
            const FieldLayout &field = info.fields[i];
            Value fieldAddr = emitGEP(ptr, static_cast<int64_t>(field.offset));
            emitStore(fieldAddr, argValues[i], mapType(field.type));
        }
    }

    return {ptr, Type(Type::Kind::Ptr)};
}

} // namespace il::frontends::zia
