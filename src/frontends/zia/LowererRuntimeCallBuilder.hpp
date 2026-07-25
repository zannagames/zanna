//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file LowererRuntimeCallBuilder.hpp
/// @brief Runtime-call argument lowering helper for the Zia lowerer.
///
/// @details Runtime calls have ABI-level expectations that are stricter than
///          ordinary Zia calls: byte values must be widened before integer
///          helper calls, numeric arguments may need integer-to-float
///          conversion, and primitive values passed to object parameters must
///          be boxed. This helper centralizes those rules so every runtime
///          call path uses the same coercion behavior.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "frontends/zia/Lowerer.hpp"

namespace il::runtime {
struct RuntimeDescriptor;
} // namespace il::runtime

namespace il::frontends::zia {

/// @brief Builds ABI-correct argument vectors for canonical runtime calls.
///
/// @details The builder is a thin private-helper facade over @ref Lowerer. It
///          does not own state; it only uses the active lowerer to emit
///          conversion, widening, and boxing instructions while preserving the
///          argument ordering selected by semantic analysis.
class RuntimeCallBuilder final {
  public:
    /// @brief Create a builder that emits into @p lowerer.
    /// @param lowerer Active Zia lowerer receiving generated IL instructions.
    explicit RuntimeCallBuilder(Lowerer &lowerer) : lowerer_(lowerer) {}

    /// @brief Coerce one lowered argument to the runtime ABI type.
    /// @param argValue Lowered IL value for the source argument.
    /// @param argIlType Current IL type of @p argValue.
    /// @param semanticType Zia semantic type for the source argument, if known.
    /// @param expectedType Runtime descriptor parameter type, or null when the
    ///                     callee is not described by the registry.
    /// @return IL value compatible with @p expectedType, or the legacy widened
    ///         value when no descriptor is available.
    Lowerer::Value coerceRuntimeArgument(Lowerer::Value argValue,
                                         Lowerer::Type argIlType,
                                         TypeRef semanticType,
                                         const Lowerer::Type *expectedType);

    /// @brief Lower explicit call arguments in semantic parameter order.
    /// @param args Source call arguments.
    /// @param orderedSources Source indices in the order expected by the
    ///                       runtime callee.
    /// @param paramOffset Number of implicit arguments already present in the
    ///                    final call vector, such as a receiver/self pointer.
    /// @param descriptor Runtime descriptor for signature-driven coercion, or
    ///                   null to use legacy fallback coercions.
    /// @return Lowered argument values ready to append to a runtime call.
    std::vector<Lowerer::Value> lowerExplicitArgs(const std::vector<CallArg> &args,
                                                  const std::vector<int> &orderedSources,
                                                  size_t paramOffset,
                                                  const il::runtime::RuntimeDescriptor *descriptor);

  private:
    /// Shared lowering context that emits ABI conversion instructions.
    Lowerer &lowerer_;
};

} // namespace il::frontends::zia
