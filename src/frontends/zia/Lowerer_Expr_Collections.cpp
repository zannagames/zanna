//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file Lowerer_Expr_Collections.cpp
/// @brief Lowerer entry points for collection, tuple, and index expressions.
///
/// @details These thin facade methods keep the public Lowerer dispatch surface
///          stable while delegating collection-specific representation,
///          boxing, layout, bounds-check, and runtime choices to
///          CollectionLowerer.
///
//===----------------------------------------------------------------------===//

#include "frontends/zia/LowererCollectionLowerer.hpp"

namespace il::frontends::zia {

/// @brief Lower a list literal through the collection helper.
/// @param expr List literal to evaluate and populate.
/// @return Pointer-valued result for the runtime list.
LowerResult Lowerer::lowerListLiteral(ListLiteralExpr *expr) {
    return CollectionLowerer(*this).lowerListLiteral(expr);
}

/// @brief Lower a set literal through the collection helper.
/// @param expr Set literal to evaluate and populate.
/// @return Pointer-valued result for the runtime set.
LowerResult Lowerer::lowerSetLiteral(SetLiteralExpr *expr) {
    return CollectionLowerer(*this).lowerSetLiteral(expr);
}

/// @brief Lower a map or semantically typed empty-set literal.
/// @param expr Map literal to evaluate and populate.
/// @return Pointer-valued result for the selected runtime collection.
LowerResult Lowerer::lowerMapLiteral(MapLiteralExpr *expr) {
    return CollectionLowerer(*this).lowerMapLiteral(expr);
}

/// @brief Lower a tuple literal into inline stack storage.
/// @param expr Tuple literal whose elements are stored by value.
/// @return Pointer to the tuple's inline storage.
LowerResult Lowerer::lowerTuple(TupleExpr *expr) {
    return CollectionLowerer(*this).lowerTuple(expr);
}

/// @brief Lower a constant tuple-element access.
/// @param expr Tuple-index expression to evaluate.
/// @return Loaded scalar value or address of an aggregate element.
LowerResult Lowerer::lowerTupleIndex(TupleIndexExpr *expr) {
    return CollectionLowerer(*this).lowerTupleIndex(expr);
}

/// @brief Lower fixed-array, string, list, or map indexing.
/// @param expr Index expression to evaluate.
/// @return Indexed value in its semantic IL representation.
LowerResult Lowerer::lowerIndex(IndexExpr *expr) {
    return CollectionLowerer(*this).lowerIndex(expr);
}

} // namespace il::frontends::zia
