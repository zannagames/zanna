//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares logical expression lowering helpers.
/// @details Provides the helper that lowers BASIC logical operators (AND/OR and
///          short-circuit ANDALSO/ORELSE). The helper borrows a @ref Lowerer to
///          emit IL control flow or bitwise operations while preserving BASIC
///          truthiness semantics and diagnostic reporting.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "frontends/basic/Lowerer.hpp"

namespace il::frontends::basic {

/// @brief Helper for lowering BASIC logical binary expressions.
/// @details Encapsulates the logic for short-circuit and eager logical
///          operators. Short-circuit forms build explicit control flow while
///          eager forms emit bitwise operations on BASIC's logical word type.
struct LogicalExprLowering {
    /// @brief Bind the logical lowering helper to a lowerer instance.
    /// @param lowerer Borrowed lowering engine; it must outlive this helper.
    explicit LogicalExprLowering(Lowerer &lowerer) noexcept;

    /// @brief Lower a logical binary expression into IL.
    /// @details Evaluates operands, applies BASIC boolean coercions, and emits
    ///          either short-circuit control flow or eager bitwise operations
    ///          depending on the operator kind.
    /// @param expr Logical binary expression to lower.
    /// @return Lowered r-value representing the logical result.
    [[nodiscard]] Lowerer::RVal lower(const BinaryExpr &expr);

  private:
    /// @brief Borrowed lowering engine used for emission and diagnostics.
    /// @invariant Non-null throughout the helper's lifetime.
    Lowerer *lowerer_{nullptr};
};

/// @brief Lower a logical binary expression using an explicit lowerer.
/// @details Convenience wrapper that constructs @ref LogicalExprLowering and
///          forwards the lowering request.
/// @param lowerer Active lowering engine used to emit IL.
/// @param expr Logical binary expression to lower.
/// @return Lowered r-value representing the logical result.
[[nodiscard]] Lowerer::RVal lowerLogicalBinary(Lowerer &lowerer, const BinaryExpr &expr);

} // namespace il::frontends::basic
