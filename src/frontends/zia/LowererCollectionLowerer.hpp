//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file LowererCollectionLowerer.hpp
/// @brief Focused helper for Zia collection and tuple expression lowering.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "frontends/zia/Lowerer.hpp"

namespace il::frontends::zia {

/// @brief Lowers list/set/map literals, tuple expressions, and index access.
///
/// @details This helper keeps collection-specific lowering decisions out of the
///          main Lowerer expression files while preserving Lowerer as the owner
///          of IL emission state.
class CollectionLowerer final {
  public:
    /// @brief Construct a collection-expression lowering helper.
    /// @param lowerer Owning lowering context used for semantic queries and IL
    ///        emission.
    explicit CollectionLowerer(Lowerer &lowerer) : lowerer_(lowerer) {}

    /// @brief Lower a list literal `[a, b, ...]` to runtime list construction.
    /// @param expr List literal whose elements are evaluated and boxed.
    /// @return Pointer-valued result for the populated runtime list.
    LowerResult lowerListLiteral(ListLiteralExpr *expr);

    /// @brief Lower a set literal `{a, b, ...}` to runtime set construction.
    /// @param expr Set literal whose elements are evaluated and boxed.
    /// @return Pointer-valued result for the populated runtime set.
    LowerResult lowerSetLiteral(SetLiteralExpr *expr);

    /// @brief Lower a map literal `{k: v, ...}` to runtime map construction.
    /// @param expr Map literal whose entries are evaluated and inserted.
    /// @return Pointer-valued result for the populated runtime map, or for an
    ///         empty runtime set when semantic analysis typed `{}` as a set.
    LowerResult lowerMapLiteral(MapLiteralExpr *expr);

    /// @brief Lower a tuple literal `(a, b, ...)` into inline stack storage.
    /// @param expr Tuple literal whose elements are stored at layout offsets.
    /// @return Pointer to the allocated tuple storage.
    LowerResult lowerTuple(TupleExpr *expr);

    /// @brief Lower a tuple element access `t.N`.
    /// @param expr Tuple-index expression to evaluate.
    /// @return Loaded scalar value, or an address for an aggregate element.
    LowerResult lowerTupleIndex(TupleIndexExpr *expr);

    /// @brief Lower an index expression `base[idx]`, dispatching on the base
    ///        type (fixed array, string, or boxed collection).
    /// @param expr Index expression to evaluate.
    /// @return Indexed value, or an address when the fixed-array element is an
    ///         inline aggregate.
    LowerResult lowerIndex(IndexExpr *expr);

  private:
    using Type = il::core::Type;
    using Value = il::core::Value;
    using Opcode = il::core::Opcode;

    /// Shared lowering context that owns semantic state and IL emission.
    Lowerer &lowerer_;

    /// @brief Construct a boxed-element runtime collection.
    /// @param elements Element expressions to evaluate in source order.
    /// @param constructor Runtime helper that creates the empty collection.
    /// @param addElement Runtime helper that inserts one boxed element.
    /// @return Pointer-valued result for the populated collection.
    LowerResult lowerBoxedElementLiteral(const std::vector<ExprPtr> &elements,
                                         const char *constructor,
                                         const char *addElement);

    /// @brief Compute the address of the tuple element at byte @p offset.
    /// @param tuplePtr Base address of inline tuple storage.
    /// @param offset Constant byte offset of the requested element.
    /// @return @p tuplePtr for offset zero, otherwise an emitted GEP result.
    Value emitTupleElementAddress(Value tuplePtr, size_t offset);

    /// @brief Compute @p basePtr + @p byteOffset as a runtime pointer.
    /// @param basePtr Base address of inline storage.
    /// @param byteOffset Runtime byte offset from @p basePtr.
    /// @return Emitted GEP pointer value.
    Value emitRuntimeOffsetAddress(Value basePtr, Value byteOffset);

    /// @brief Lower a bounds-checked fixed-array element access.
    /// @param baseValue Address of the array's inline storage.
    /// @param indexValue Index value to widen and bounds-check.
    /// @param indexType IL representation of @p indexValue.
    /// @param baseType Semantic fixed-array type supplying element layout and
    ///        extent.
    /// @return Loaded scalar value, or the computed address for an aggregate
    ///         element.
    LowerResult lowerFixedArrayIndex(Value baseValue,
                                     Value indexValue,
                                     Type indexType,
                                     TypeRef baseType);

    /// @brief Lower `str[idx]` to a one-character substring operation.
    /// @param baseValue Runtime string value.
    /// @param indexValue Character index to widen to `i64`.
    /// @param indexType IL representation of @p indexValue.
    /// @return String-valued result containing the indexed character.
    LowerResult lowerStringIndex(Value baseValue, Value indexValue, Type indexType);

    /// @brief Lower list or map indexing through the boxed collection runtime.
    /// @param baseValue Runtime collection pointer.
    /// @param indexValue List index or map key.
    /// @param indexType IL representation of @p indexValue.
    /// @param expr Original expression supplying semantic collection and
    ///        element types.
    /// @return Element unboxed to its semantic IL representation.
    LowerResult lowerBoxedCollectionIndex(Value baseValue,
                                          Value indexValue,
                                          Type indexType,
                                          IndexExpr *expr);
};

} // namespace il::frontends::zia
