//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares numeric expression lowering helpers for BASIC.
/// @details Provides helper methods that implement BASIC numeric operators,
///          including arithmetic, exponentiation, division/modulus, and string
///          concatenation. The helper borrows a @ref Lowerer to perform operand
///          coercions, select the correct IL opcodes, and apply special-case
///          optimisations while preserving BASIC type semantics.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "frontends/basic/Lowerer.hpp"

#include <optional>

namespace il::frontends::basic {

/// @brief Helper for lowering BASIC numeric binary expressions.
/// @details Encapsulates the logic for numeric operator handling, including
///          operand normalization, opcode selection, and specialised patterns
///          (such as exponentiation or string concatenation).
struct NumericExprLowering {
    /// @brief Bind the numeric lowering helper to a lowerer instance.
    /// @param lowerer Borrowed lowering engine; it must outlive this helper.
    explicit NumericExprLowering(Lowerer &lowerer) noexcept;

    /// @brief Lower integer division or modulus operations.
    /// @details Coerces both operands to `i64` and emits SDivChk0 or SRemChk0
    ///          to preserve BASIC's divide-by-zero behavior.
    /// @param expr Binary expression representing IDIV or MOD.
    /// @return Lowered r-value carrying the operation result.
    [[nodiscard]] Lowerer::RVal lowerDivOrMod(const BinaryExpr &expr);

    /// @brief Lower exponentiation expressions.
    /// @details Applies type coercions to operands, emits runtime helper calls
    ///          where needed, and returns the resulting value.
    /// @param expr Binary expression representing POW.
    /// @param lhs Normalized left operand.
    /// @param rhs Normalized right operand.
    /// @return Lowered r-value for the exponentiation result.
    [[nodiscard]] Lowerer::RVal lowerPowBinary(const BinaryExpr &expr,
                                               Lowerer::RVal lhs,
                                               Lowerer::RVal rhs);

    /// @brief Lower string binary operators (e.g., concatenation).
    /// @details Applies string-specific coercions, emits runtime helpers for
    ///          concatenation, and returns the resulting string value.
    /// @param expr Binary expression representing a string operator.
    /// @param lhs Normalized left operand.
    /// @param rhs Normalized right operand.
    /// @return String result for concatenation or an `i64` logical result for
    ///         a comparison.
    [[nodiscard]] Lowerer::RVal lowerStringBinary(const BinaryExpr &expr,
                                                  Lowerer::RVal lhs,
                                                  Lowerer::RVal rhs);

    /// @brief Lower generic numeric binary operators.
    /// @details Handles integer and floating-point arithmetic as well as
    ///          comparisons, applying BASIC's promotion rules and emitting the
    ///          appropriate IL opcode.
    /// @param expr Binary expression representing the operator.
    /// @param lhs Normalized left operand.
    /// @param rhs Normalized right operand.
    /// @return Lowered r-value for the operator result.
    [[nodiscard]] Lowerer::RVal lowerNumericBinary(const BinaryExpr &expr,
                                                   Lowerer::RVal lhs,
                                                   Lowerer::RVal rhs);

  private:
    /// @brief Normalized operand configuration for numeric operators.
    /// @details Captures whether the operation is floating-point and which
    ///          IL type should be used for arithmetic and result values.
    struct NumericOpConfig {
        bool isFloat{false};             ///< True when operands are treated as float.
        il::core::Type arithmeticType{}; ///< IL type used for arithmetic operations.
        il::core::Type resultType{};     ///< IL type of the final result.
    };

    /// @brief Selected opcode and result metadata for numeric operations.
    /// @details Records the IL opcode to emit and any post-processing needs,
    ///          such as promoting boolean results to BASIC logical words.
    struct OpcodeSelection {
        il::core::Opcode opcode{il::core::Opcode::IAddOvf}; ///< IL opcode to emit.
        il::core::Type resultType{};                        ///< Resulting IL type.
        bool promoteBoolToI64{false};                       ///< Whether to widen booleans.
    };

    /// @brief Normalize operands according to BASIC numeric promotion rules.
    /// @details Updates @p lhs and @p rhs in place to match the selected
    ///          arithmetic type and returns the chosen configuration.
    /// @param expr Binary expression guiding promotion rules.
    /// @param lhs Left-hand operand to normalize.
    /// @param rhs Right-hand operand to normalize.
    /// @return Operand configuration describing the chosen types.
    [[nodiscard]] NumericOpConfig normalizeNumericOperands(const BinaryExpr &expr,
                                                           Lowerer::RVal &lhs,
                                                           Lowerer::RVal &rhs);

    /// @brief Apply special-case patterns for constant expressions.
    /// @details Recognizes bespoke patterns (such as 0 - X negation) and emits
    ///          a direct lowering when possible.
    /// @param expr Binary expression under consideration.
    /// @param lhs Normalized left operand.
    /// @param rhs Normalized right operand.
    /// @param config Operand configuration from normalization.
    /// @return Lowered value when a special case applies; otherwise empty.
    [[nodiscard]] std::optional<Lowerer::RVal> applySpecialConstantPatterns(
        const BinaryExpr &expr,
        Lowerer::RVal &lhs,
        Lowerer::RVal &rhs,
        const NumericOpConfig &config);

    /// @brief Select the IL opcode for a numeric binary operator.
    /// @details Returns the opcode and result metadata after considering the
    ///          operand configuration and operator kind.
    /// @param op Binary operator enumeration.
    /// @param config Operand configuration derived from normalization.
    /// @return Opcode selection describing the emission plan.
    [[nodiscard]] OpcodeSelection selectNumericOpcode(BinaryExpr::Op op,
                                                      const NumericOpConfig &config);

    /// @brief Borrowed lowering engine used for emission and diagnostics.
    /// @invariant Non-null throughout the helper's lifetime.
    Lowerer *lowerer_{nullptr};
};

/// @brief Lower integer division or modulus with an explicit lowerer.
/// @details Convenience wrapper that constructs @ref NumericExprLowering and
///          forwards to @ref NumericExprLowering::lowerDivOrMod.
/// @param lowerer Active lowering engine.
/// @param expr Binary expression representing IDIV or MOD.
/// @return Lowered r-value carrying the operation result.
[[nodiscard]] Lowerer::RVal lowerDivOrMod(Lowerer &lowerer, const BinaryExpr &expr);

/// @brief Lower exponentiation using an explicit lowerer.
/// @details Convenience wrapper for @ref NumericExprLowering::lowerPowBinary.
/// @param lowerer Active lowering engine.
/// @param expr Binary expression representing POW.
/// @param lhs Normalized left operand.
/// @param rhs Normalized right operand.
/// @return Lowered r-value representing the result.
[[nodiscard]] Lowerer::RVal lowerPowBinary(Lowerer &lowerer,
                                           const BinaryExpr &expr,
                                           Lowerer::RVal lhs,
                                           Lowerer::RVal rhs);

/// @brief Lower string binary operators using an explicit lowerer.
/// @details Convenience wrapper for @ref NumericExprLowering::lowerStringBinary.
/// @param lowerer Active lowering engine.
/// @param expr Binary expression representing a string operator.
/// @param lhs Normalized left operand.
/// @param rhs Normalized right operand.
/// @return Lowered r-value representing the string result.
[[nodiscard]] Lowerer::RVal lowerStringBinary(Lowerer &lowerer,
                                              const BinaryExpr &expr,
                                              Lowerer::RVal lhs,
                                              Lowerer::RVal rhs);

/// @brief Lower generic numeric binary operators using an explicit lowerer.
/// @details Convenience wrapper for @ref NumericExprLowering::lowerNumericBinary.
/// @param lowerer Active lowering engine.
/// @param expr Binary expression representing the operator.
/// @param lhs Normalized left operand.
/// @param rhs Normalized right operand.
/// @return Lowered r-value representing the result.
[[nodiscard]] Lowerer::RVal lowerNumericBinary(Lowerer &lowerer,
                                               const BinaryExpr &expr,
                                               Lowerer::RVal lhs,
                                               Lowerer::RVal rhs);

} // namespace il::frontends::basic
