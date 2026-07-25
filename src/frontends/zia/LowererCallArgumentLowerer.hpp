//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file LowererCallArgumentLowerer.hpp
/// @brief Helper for lowering semantically bound call/new arguments.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "frontends/zia/Lowerer.hpp"

namespace il::frontends::zia {

/// @brief Converts source call arguments into final IL call argument vectors.
///
/// @details Handles named-argument ordering, default parameters, and variadic
///          list packing for both function calls and `new` expressions.
class CallArgumentLowerer final {
  public:
    /// @brief Construct a call-argument lowering helper.
    /// @param lowerer Owning lowering context used for expression emission,
    ///        semantic bindings, coercions, and diagnostics.
    explicit CallArgumentLowerer(Lowerer &lowerer) : lowerer_(lowerer) {}

    /// @brief Lower each call argument expression in source order, returning
    ///        one LowerResult per argument (before parameter reordering).
    /// @param args Source arguments to evaluate.
    /// @return Lowered results in the same order as @p args.
    std::vector<LowerResult> lowerSourceArgs(const std::vector<CallArg> &args);

    /// @brief Determine the order in which source arguments map to parameters.
    /// @param args Source arguments supplied by the call site.
    /// @param binding Resolved semantic binding, or `nullptr` for positional
    ///        identity order.
    /// @return Source indices for fixed parameters followed by variadic
    ///         arguments.
    static std::vector<int> orderedArgSources(const std::vector<CallArg> &args,
                                              const Sema::CallArgBinding *binding);

    /// @brief Produce the final argument vector for a resolved callable.
    /// @param args Source arguments supplied by the call site.
    /// @param paramTypes Resolved parameter types in declaration order.
    /// @param params Parameter declarations used for defaults and variadic
    ///        metadata, or `nullptr` when unavailable.
    /// @param binding Semantic source-to-parameter binding, or `nullptr` for
    ///        positional coercion.
    /// @return Coerced values in parameter order, including any packed
    ///         variadic list.
    std::vector<Lowerer::Value> lowerResolvedArgs(const std::vector<CallArg> &args,
                                                  const std::vector<TypeRef> &paramTypes,
                                                  const std::vector<Param> *params,
                                                  const Sema::CallArgBinding *binding);

    /// @brief Lower arguments for an ordinary resolved call expression.
    /// @param expr Call expression whose semantic binding supplies the mapping.
    /// @param paramTypes Resolved parameter types in declaration order.
    /// @param params Parameter declarations, or `nullptr` when unavailable.
    /// @return Coerced argument values in final parameter order.
    std::vector<Lowerer::Value> lowerResolvedCallArgs(CallExpr *expr,
                                                      const std::vector<TypeRef> &paramTypes,
                                                      const std::vector<Param> *params);

    /// @brief Lower constructor arguments for a resolved `new` expression.
    /// @param expr Allocation expression whose semantic binding supplies the
    ///        mapping.
    /// @param paramTypes Resolved constructor parameter types.
    /// @param params Constructor parameter declarations, or `nullptr` when
    ///        unavailable.
    /// @return Coerced constructor arguments in final parameter order.
    std::vector<Lowerer::Value> lowerResolvedNewArgs(NewExpr *expr,
                                                     const std::vector<TypeRef> &paramTypes,
                                                     const std::vector<Param> *params);

  private:
    using Type = il::core::Type;
    using Value = il::core::Value;

    /// Shared lowering context that owns semantic state and IL emission.
    Lowerer &lowerer_;
};

} // namespace il::frontends::zia
