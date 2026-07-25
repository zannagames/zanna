//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/LowerILToMIR.hpp
// Purpose: Minimal IL→MIR lowering adapter for AArch64 (Phase A).
// Key invariants:
//   - Stateless between lowerFunction() calls; per-function state is reset.
//   - TargetInfo must outlive this object.
// Ownership/Lifetime:
//   - Non-owning; holds a non-owning pointer to an externally-owned TargetInfo.
// Links: src/codegen/aarch64/LowerILToMIR.cpp,
//        src/codegen/aarch64/InstrLowering.hpp,
//        src/codegen/aarch64/LoweringContext.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares per-function IL-to-AArch64-MIR orchestration.
 *
 * The lowerer borrows immutable target and optional literal metadata while
 * owning variadic-callee metadata. Each function call builds fresh frame,
 * liveness, phi, virtual-register, and block-lowering state.
 */

#pragma once

#include <cstddef>
#include <optional>
#include <ostream>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "codegen/aarch64/MachineIR.hpp"
#include "codegen/aarch64/TargetAArch64.hpp"
#include "il/core/Function.hpp"

namespace zanna::codegen::aarch64 {

/// @brief Lowers IL functions to AArch64 Machine IR (MIR).
///
/// This class implements the instruction selection phase of code generation,
/// converting IL instructions into target-specific MIR instructions.
///
/// @invariant The lowerer is stateless between function calls - all per-function
///            state is cleared at the start of each lowerFunction() call.
/// @invariant The TargetInfo reference must remain valid for the lifetime of
///            this object.
class LowerILToMIR {
  public:
    /// @brief Construct a lowerer bound to a target ABI and an optional string-literal size table.
    /// @param ti Target info (calling convention, register names, alignment).
    /// @param stringLiteralByteLengths Optional per-symbol byte-length map for
    ///        runtime-string materialisation; the pointed-to map must outlive the lowerer.
    explicit LowerILToMIR(const TargetInfo &ti,
                          const std::unordered_map<std::string, std::size_t>
                              *stringLiteralByteLengths = nullptr) noexcept
        : ti_(&ti), stringLiteralByteLengths_(stringLiteralByteLengths) {}

    /// @brief Register known named-argument counts for variadic callees.
    /// @details The lowerer uses this table to emit the correct argument count for
    ///          runtime variadic-call conventions when calling known vararg functions.
    /// @param knownVarArgNamedArgCounts Map from callee name to named-arg count.
    void setKnownVarArgCallees(
        std::unordered_map<std::string, std::size_t> knownVarArgNamedArgCounts) {
        knownVarArgNamedArgCounts_ = std::move(knownVarArgNamedArgCounts);
    }

    /// @brief Look up the known named-argument count for @p callee, if registered.
    /// @param callee Direct callee name.
    /// @return The named-arg count, or nullopt if @p callee is not a known vararg callee.
    [[maybe_unused]] [[nodiscard]] std::optional<std::size_t>
    knownVarArgNamedArgs(std::string_view callee) const;

    /// @brief Lower an IL function to MIR.
    /// @param fn The IL function to lower.
    /// @return The lowered MIR function.
    MFunction lowerFunction(const il::core::Function &fn) const;

  private:
    const TargetInfo *ti_{};
    const std::unordered_map<std::string, std::size_t> *stringLiteralByteLengths_{};
    std::unordered_map<std::string, std::size_t> knownVarArgNamedArgCounts_{};
};

} // namespace zanna::codegen::aarch64
