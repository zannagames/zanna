//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file LowererBinaryOperatorLowerer.hpp
/// @brief Helper for lowering non-assignment binary operators.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "frontends/zia/Lowerer.hpp"

namespace il::frontends::zia {

/// @brief Lowers binary operators after assignment and short-circuit handling.
///
/// @details Keeps arithmetic, comparison, string concatenation, and bitwise
///          operator selection out of the main binary expression entry file.
class BinaryOperatorLowerer final {
  public:
    /// @brief Construct a binary-operator lowering helper.
    /// @param lowerer Owning lowering context used to inspect semantic types
    ///        and emit IL instructions.
    explicit BinaryOperatorLowerer(Lowerer &lowerer) : lowerer_(lowerer) {}

    /// @brief Lower a binary expression, selecting the integer/float/string/
    ///        boolean/pointer opcode (and overflow-checked variants) for the
    ///        operator and operand types.
    /// @param expr Semantically analyzed binary expression to lower.
    /// @return Emitted value together with its IL result type.
    LowerResult lowerBinary(BinaryExpr *expr);

  private:
    using Type = il::core::Type;
    using Value = il::core::Value;
    using Opcode = il::core::Opcode;

    /// Shared lowering context that owns the active module and insertion point.
    Lowerer &lowerer_;
};

} // namespace il::frontends::zia
