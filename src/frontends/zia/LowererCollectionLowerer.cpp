//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file LowererCollectionLowerer.cpp
/// @brief CollectionLowerer implementation for collection and tuple expressions.
///
//===----------------------------------------------------------------------===//

#include "frontends/zia/LowererCollectionLowerer.hpp"

#include "frontends/zia/RuntimeNames.hpp"

#include <utility>

namespace il::frontends::zia {

using namespace runtime;

/// @brief Build a boxed-element collection literal (shared by list and set literals).
/// @param elements The literal's element expressions.
/// @param constructor Runtime constructor helper (e.g. kListNew / kSetNew).
/// @param addElement Runtime add helper (e.g. kListAdd / kSetPut).
/// @return A pointer to the constructed collection.
/// @details Creates the collection, then lowers each element and boxes it to a uniform `obj`
///          before adding, so heterogeneous element representations share one storage form.
LowerResult CollectionLowerer::lowerBoxedElementLiteral(const std::vector<ExprPtr> &elements,
                                                        const char *constructor,
                                                        const char *addElement) {
    Value collection = lowerer_.emitCallRet(Type(Type::Kind::Ptr), constructor, {});

    for (auto &elem : elements) {
        auto result = lowerer_.lowerExpr(elem.get());
        TypeRef elemType = lowerer_.sema_.typeOf(elem.get());
        Value boxed = lowerer_.emitBoxValue(result.value, result.type, elemType);
        lowerer_.emitCall(addElement, {collection, boxed});
    }

    return {collection, Type(Type::Kind::Ptr)};
}

/// @brief Lower a list literal `[a, b, ...]` to a populated runtime list.
/// @param expr List literal whose elements are evaluated and boxed.
/// @return Pointer-valued result for the populated runtime list.
LowerResult CollectionLowerer::lowerListLiteral(ListLiteralExpr *expr) {
    return lowerBoxedElementLiteral(expr->elements, kListNew, kListAdd);
}

/// @brief Lower a set literal `{a, b, ...}` to a populated runtime set.
/// @param expr Set literal whose elements are evaluated and boxed.
/// @return Pointer-valued result for the populated runtime set.
LowerResult CollectionLowerer::lowerSetLiteral(SetLiteralExpr *expr) {
    return lowerBoxedElementLiteral(expr->elements, kSetNew, kSetPut);
}

/// @brief Lower a map (or empty-set) literal to a populated runtime collection.
/// @param expr Map literal expression.
/// @return A pointer to the constructed map (or set for an empty `{}` typed as a set).
/// @details Each value is boxed before insertion. String-keyed maps use the Map runtime and
///          integer-keyed maps use IntMap after widening keys to i64. An empty literal whose
///          sema type is `Set` builds a set instead of a map.
LowerResult CollectionLowerer::lowerMapLiteral(MapLiteralExpr *expr) {
    TypeRef literalType = lowerer_.sema_.typeOf(expr);
    if (literalType && literalType->kind == TypeKindSem::Set && expr->entries.empty()) {
        Value set = lowerer_.emitCallRet(Type(Type::Kind::Ptr), kSetNew, {});
        return {set, Type(Type::Kind::Ptr)};
    }

    const bool integerKeyed = lowerer_.usesIntegerMapRuntime(literalType);
    Value map =
        lowerer_.emitCallRet(Type(Type::Kind::Ptr), integerKeyed ? kIntMapNew : kMapNew, {});
    const char *setHelper = integerKeyed ? kIntMapSet : kMapSet;

    for (auto &entry : expr->entries) {
        auto keyResult = lowerer_.lowerExpr(entry.key.get());
        Value runtimeKey =
            lowerer_.coerceMapKeyForRuntime(keyResult.value, keyResult.type, literalType);
        auto valueResult = lowerer_.lowerExpr(entry.value.get());
        TypeRef valueType = lowerer_.sema_.typeOf(entry.value.get());
        Value boxedValue = lowerer_.emitBoxValue(valueResult.value, valueResult.type, valueType);
        lowerer_.emitCall(setHelper, {map, runtimeKey, boxedValue});
    }

    return {map, Type(Type::Kind::Ptr)};
}

/// @brief Lower a tuple literal into stack-allocated inline storage.
/// @param expr Tuple expression.
/// @return A pointer to the allocated tuple storage.
/// @details Allocas storage sized for the tuple, then stores each element inline at its
///          computed offset (no boxing — tuples store elements by value).
LowerResult CollectionLowerer::lowerTuple(TupleExpr *expr) {
    TypeRef tupleType = lowerer_.sema_.typeOf(expr);
    size_t tupleSize = lowerer_.getTupleStorageSize(tupleType);

    unsigned slotId = lowerer_.nextTempId();
    il::core::Instr allocInstr;
    allocInstr.result = slotId;
    allocInstr.op = Opcode::Alloca;
    allocInstr.type = Type(Type::Kind::Ptr);
    allocInstr.operands = {Value::constInt(static_cast<int64_t>(tupleSize))};
    allocInstr.loc = lowerer_.curLoc_;
    lowerer_.blockMgr_.currentBlock()->instructions.push_back(std::move(allocInstr));
    Value tuplePtr = Value::temp(slotId);

    const auto elementTypes = tupleType ? tupleType->tupleElementTypes() : std::vector<TypeRef>{};
    for (size_t i = 0; i < expr->elements.size(); ++i) {
        auto &elem = expr->elements[i];
        auto result = lowerer_.lowerExpr(elem.get());
        TypeRef elemType =
            i < elementTypes.size() ? elementTypes[i] : lowerer_.sema_.typeOf(elem.get());
        size_t offset = lowerer_.getTupleElementOffset(tupleType, i);
        Value elemPtr = emitTupleElementAddress(tuplePtr, offset);
        lowerer_.emitInlineValueStore(elemType, elemPtr, result.value, false);
    }

    return {tuplePtr, Type(Type::Kind::Ptr)};
}

/// @brief Lower a tuple element access (`tuple.N`).
/// @param expr Tuple-index expression.
/// @return The element value (aggregate elements are returned by address).
/// @details Computes the element address at the element's offset; scalar elements are loaded
///          (string loads retained), while struct/fixed-array/tuple elements are returned as
///          pointers into the tuple storage.
LowerResult CollectionLowerer::lowerTupleIndex(TupleIndexExpr *expr) {
    auto tupleResult = lowerer_.lowerExpr(expr->tuple.get());

    TypeRef tupleType = lowerer_.sema_.typeOf(expr->tuple.get());
    TypeRef elemType = tupleType->tupleElementType(expr->index);
    Type ilType = lowerer_.mapType(elemType);

    Value elemPtr = emitTupleElementAddress(tupleResult.value,
                                            lowerer_.getTupleElementOffset(tupleType, expr->index));
    if (elemType &&
        (elemType->kind == TypeKindSem::Struct || elemType->kind == TypeKindSem::FixedArray ||
         elemType->kind == TypeKindSem::Tuple))
        return {elemPtr, Type(Type::Kind::Ptr)};

    Value elemVal = lowerer_.emitLoad(elemPtr, ilType);
    if (ilType.kind == Type::Kind::Str)
        lowerer_.emitCall(kStrRetainMaybe, {elemVal});
    return {elemVal, ilType};
}

/// @brief Lower an index/subscript expression (`base[index]`).
/// @param expr Index expression.
/// @return The indexed element value.
/// @details Dispatches by base type: fixed arrays use bounds-checked inline addressing, strings
///          return a one-character substring, and list/map collections go through the boxed
///          runtime get path.
LowerResult CollectionLowerer::lowerIndex(IndexExpr *expr) {
    auto base = lowerer_.lowerExpr(expr->base.get());
    auto index = lowerer_.lowerExpr(expr->index.get());

    TypeRef baseType = lowerer_.sema_.typeOf(expr->base.get());

    if (baseType && baseType->kind == TypeKindSem::FixedArray)
        return lowerFixedArrayIndex(base.value, index.value, index.type, baseType);

    if (baseType && baseType->kind == TypeKindSem::String)
        return lowerStringIndex(base.value, index.value, index.type);

    return lowerBoxedCollectionIndex(base.value, index.value, index.type, expr);
}

/// @brief Compute the address of a tuple element at a constant byte offset.
/// @param tuplePtr Base address of the tuple's inline storage.
/// @param offset Constant byte offset of the requested element.
/// @return @p tuplePtr unchanged for offset 0, otherwise a GEP to the element.
CollectionLowerer::Value CollectionLowerer::emitTupleElementAddress(Value tuplePtr, size_t offset) {
    if (offset == 0)
        return tuplePtr;
    return lowerer_.emitGEP(tuplePtr, static_cast<int64_t>(offset));
}

/// @brief Compute an address from a base pointer plus a runtime (non-constant) byte offset.
/// @param basePtr Base address of the inline storage.
/// @param byteOffset Runtime byte offset from @p basePtr.
/// @return A GEP pointer value.
CollectionLowerer::Value CollectionLowerer::emitRuntimeOffsetAddress(Value basePtr,
                                                                     Value byteOffset) {
    unsigned gepId = lowerer_.nextTempId();
    il::core::Instr gepInstr;
    gepInstr.result = gepId;
    gepInstr.op = Opcode::GEP;
    gepInstr.type = Type(Type::Kind::Ptr);
    gepInstr.operands = {basePtr, byteOffset};
    gepInstr.loc = lowerer_.curLoc_;
    lowerer_.blockMgr_.currentBlock()->instructions.push_back(std::move(gepInstr));
    return Value::temp(gepId);
}

/// @brief Lower a fixed-array element access with a bounds check.
/// @param baseValue The array base pointer.
/// @param indexValue The (possibly narrow) index value.
/// @param indexType IL type of the index.
/// @param baseType Semantic fixed-array type (supplies element type and count).
/// @return The element value, returned by address for aggregate element types.
/// @details Bounds-checks the index against `[0, elementCount)`, computes `index * elemSize`,
///          and addresses the element inline. Scalar elements are loaded (strings retained).
LowerResult CollectionLowerer::lowerFixedArrayIndex(Value baseValue,
                                                    Value indexValue,
                                                    Type indexType,
                                                    TypeRef baseType) {
    TypeRef elemType = baseType->elementType();
    Type ilElemType = elemType ? lowerer_.mapType(elemType) : Type(Type::Kind::I64);
    size_t elemSize = lowerer_.getSemanticTypeSize(elemType);
    Value checkedIndex =
        lowerer_.emitIndexCheck(lowerer_.widenIntegralToI64(indexValue, indexType),
                                Value::constInt(0),
                                Value::constInt(static_cast<int64_t>(baseType->elementCount)));

    unsigned mulId = lowerer_.nextTempId();
    il::core::Instr mulInstr;
    mulInstr.result = mulId;
    mulInstr.op = Opcode::IMulOvf;
    mulInstr.type = Type(Type::Kind::I64);
    mulInstr.operands = {checkedIndex, Value::constInt(static_cast<int64_t>(elemSize))};
    mulInstr.loc = lowerer_.curLoc_;
    lowerer_.blockMgr_.currentBlock()->instructions.push_back(std::move(mulInstr));
    Value byteOffset = Value::temp(mulId);

    Value elemAddr = emitRuntimeOffsetAddress(baseValue, byteOffset);
    if (elemType &&
        (elemType->kind == TypeKindSem::Struct || elemType->kind == TypeKindSem::FixedArray ||
         elemType->kind == TypeKindSem::Tuple))
        return {elemAddr, Type(Type::Kind::Ptr)};

    Value elemVal = lowerer_.emitLoad(elemAddr, ilElemType);
    if (ilElemType.kind == Type::Kind::Str)
        lowerer_.emitCall(kStrRetainMaybe, {elemVal});
    return {elemVal, ilElemType};
}

/// @brief Lower string indexing (`str[i]`) to a single-character substring.
/// @param baseValue The string value.
/// @param indexValue The index value.
/// @param indexType IL type of the index.
/// @return A length-1 string containing the indexed character.
LowerResult CollectionLowerer::lowerStringIndex(Value baseValue, Value indexValue, Type indexType) {
    Value one = Value::constInt(1);
    Value i64Index = lowerer_.widenIntegralToI64(indexValue, indexType);
    Value result =
        lowerer_.emitCallRet(Type(Type::Kind::Str), kStringSubstring, {baseValue, i64Index, one});
    return {result, Type(Type::Kind::Str)};
}

/// @brief Lower indexing of a boxed collection (list or map) and unbox the element.
/// @param baseValue The collection value.
/// @param indexValue The key/index value.
/// @param indexType IL type of the index.
/// @param expr The index expression (supplies base and element sema types).
/// @return The unboxed element value.
/// @details Maps trap on a missing key (guarded by Has before Get) before loading the value;
///          integer-keyed maps route through IntMap with a widened i64 key. Lists use kListGet
///          with a widened I64 index. The boxed result is unboxed to the element type.
LowerResult CollectionLowerer::lowerBoxedCollectionIndex(Value baseValue,
                                                         Value indexValue,
                                                         Type indexType,
                                                         IndexExpr *expr) {
    TypeRef baseType = lowerer_.sema_.typeOf(expr->base.get());
    Value boxed;
    if (baseType && baseType->kind == TypeKindSem::Map) {
        const bool integerKeyed = lowerer_.usesIntegerMapRuntime(baseType);
        const char *hasHelper = integerKeyed ? kIntMapContainsKey : kMapContainsKey;
        const char *getHelper = integerKeyed ? kIntMapGet : kMapGet;
        Value runtimeKey = lowerer_.coerceMapKeyForRuntime(indexValue, indexType, baseType);
        Value hasKey =
            lowerer_.emitCallRet(Type(Type::Kind::I1), hasHelper, {baseValue, runtimeKey});
        size_t okIdx = lowerer_.createBlock("map_index_ok");
        size_t failIdx = lowerer_.createBlock("map_index_missing");
        lowerer_.emitCBr(hasKey, okIdx, failIdx);

        lowerer_.setBlock(failIdx);
        il::core::Instr trapInstr;
        trapInstr.op = Opcode::Trap;
        trapInstr.type = Type(Type::Kind::Void);
        trapInstr.loc = lowerer_.curLoc_;
        lowerer_.blockMgr_.currentBlock()->instructions.push_back(std::move(trapInstr));
        lowerer_.blockMgr_.currentBlock()->terminated = true;

        lowerer_.setBlock(okIdx);
        boxed = lowerer_.emitCallRet(Type(Type::Kind::Ptr), getHelper, {baseValue, runtimeKey});
    } else {
        Value i64Index = lowerer_.widenIntegralToI64(indexValue, indexType);
        boxed = lowerer_.emitCallRet(Type(Type::Kind::Ptr), kListGet, {baseValue, i64Index});
    }

    TypeRef elemType = lowerer_.sema_.typeOf(expr);
    Type ilType = lowerer_.mapType(elemType);
    return lowerer_.emitUnboxValue(boxed, ilType, elemType);
}

} // namespace il::frontends::zia
