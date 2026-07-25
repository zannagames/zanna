//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/RuntimeCallHelpers.hpp
// Purpose: Unified runtime call builder for BASIC lowering. Centralises patterns
//          for emitting runtime calls with coercion, error checking, and feature
//          tracking that were duplicated across IoStatementLowerer,
//          RuntimeStatementLowerer, and related lowering code.
// Key invariants: Builder borrows the Lowerer context and only emits instructions
//                 when a procedure context is active.
// Ownership/Lifetime: Non-owning reference to Lowerer; IR objects remain owned
//                     by the lowering pipeline.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/// @file RuntimeCallHelpers.hpp
/// @brief Declares a fluent runtime-call emission adapter for BASIC lowering.
/// @details RuntimeCallBuilder owns its pending argument values and optional
///          source location, borrows a Lowerer, and emits only when an explicit
///          call method is invoked.

#pragma once

#include "il/runtime/RuntimeSignatures.hpp"
#include "support/source_location.hpp"
#include "zanna/il/Module.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace il::frontends::basic {

class Lowerer;

/// @brief Fluent builder for emitting runtime calls with automatic coercion and error handling.
///
/// @details This class consolidates common patterns found across IoStatementLowerer,
///          RuntimeStatementLowerer, and other lowering code:
///          - LocationScope tracking for source location
///          - Argument coercion (ensureI64, narrow32, normalizeChannelToI32)
///          - Runtime feature tracking (requestHelper, trackRuntime)
///          - Manual helper requirements (requireXxx)
///          - Error checking (emitRuntimeErrCheck + emitTrapFromErr)
///
/// @example Basic usage:
/// @code
///   RuntimeCallBuilder(lowerer_)
///       .at(stmt.loc)
///       .arg(path.value)
///       .argNarrow32(Value::constInt(static_cast<int32_t>(stmt.mode)))
///       .argChannel(channel.value, channel.type)
///       .withFeature(RuntimeFeature::OpenErrVstr)
///       .callWithErrCheck(Type(Type::Kind::I32), "rt_open_err_vstr", "open");
/// @endcode
/// @invariant The borrowed Lowerer outlives the builder.
class RuntimeCallBuilder {
  public:
    /// IL value type accepted by runtime calls.
    using Value = il::core::Value;
    /// IL type descriptor used for coercion and return values.
    using Type = il::core::Type;
    /// Runtime helper feature identifier.
    using RuntimeFeature = il::runtime::RuntimeFeature;

    /// @brief Construct a builder bound to the lowering context.
    /// @param lowerer Borrowed lowering driver receiving emitted instructions.
    /// @pre @p lowerer outlives the builder.
    explicit RuntimeCallBuilder(Lowerer &lowerer) noexcept;

    // -------------------------------------------------------------------------
    // Location tracking
    // -------------------------------------------------------------------------

    /// @brief Set the source location for emitted instructions.
    /// @param loc Source location to associate with the call.
    /// @return Reference to this builder for chaining.
    RuntimeCallBuilder &at(il::support::SourceLoc loc) noexcept;

    // -------------------------------------------------------------------------
    // Argument collection with coercion
    // -------------------------------------------------------------------------

    /// @brief Add an argument without coercion.
    /// @param v Value to pass as an argument.
    /// @return Reference to this builder for chaining.
    RuntimeCallBuilder &arg(Value v);

    /// @brief Add an argument that will be narrowed to 32 bits before the call.
    /// @param v 64-bit value to narrow.
    /// @return Reference to this builder for chaining.
    RuntimeCallBuilder &argNarrow32(Value v);

    /// @brief Add a channel argument (normalised to i32).
    /// @details Applies the same normalisation as Lowerer::normalizeChannelToI32.
    /// @param v Channel value (may be i32 or i64).
    /// @param ty Type of the channel value.
    /// @return Reference to this builder for chaining.
    RuntimeCallBuilder &argChannel(Value v, Type ty);

    /// @brief Add an argument coerced to i64.
    /// @param v Value to coerce.
    /// @param ty Current type of the value.
    /// @return Reference to this builder for chaining.
    RuntimeCallBuilder &argI64(Value v, Type ty);

    /// @brief Add an argument coerced to f64.
    /// @param v Value to coerce.
    /// @param ty Current type of the value.
    /// @return Reference to this builder for chaining.
    RuntimeCallBuilder &argF64(Value v, Type ty);

    // -------------------------------------------------------------------------
    // Runtime feature/helper tracking
    // -------------------------------------------------------------------------

    /// @brief Request a runtime feature helper.
    /// @details Calls Lowerer::requestHelper to ensure the helper is declared.
    /// @param feature Runtime feature to request.
    /// @return Reference to this builder for chaining.
    RuntimeCallBuilder &withFeature(RuntimeFeature feature);

    /// @brief Track a runtime feature for ordered declaration.
    /// @details Calls Lowerer::trackRuntime for deterministic extern emission.
    /// @param feature Runtime feature to track.
    /// @return Reference to this builder for chaining.
    RuntimeCallBuilder &trackFeature(RuntimeFeature feature);

    /// @brief Set a manual helper requirement.
    /// @details Allows setting any manual helper requirement via a function pointer.
    /// @param requireFn Pointer to member function like &Lowerer::requireSleepMs.
    /// @return Reference to this builder for chaining.
    RuntimeCallBuilder &withManualHelper(void (Lowerer::*requireFn)());

    // -------------------------------------------------------------------------
    // Call emission
    // -------------------------------------------------------------------------

    /// @brief Emit a void call with no error checking.
    /// @param callee Name of the runtime function to call.
    void call(const std::string &callee);

    /// @brief Emit a call with a return value but no error checking.
    /// @param retTy Return type of the call.
    /// @param callee Name of the runtime function to call.
    /// @return Value representing the call result.
    Value callRet(Type retTy, const std::string &callee);

    /// @brief Emit a call using emitRuntimeHelper (feature + call combined).
    /// @param feature Runtime feature to request.
    /// @param callee Name of the runtime function to call.
    /// @param retTy Return type of the call.
    /// @return Value representing the call result.
    Value callHelper(RuntimeFeature feature, const std::string &callee, Type retTy);

    /// @brief Emit a void call using emitRuntimeHelper.
    /// @param feature Runtime feature to request.
    /// @param callee Name of the runtime function to call.
    void callHelperVoid(RuntimeFeature feature, const std::string &callee);

    /// @brief Emit a call with error checking and trap on failure.
    /// @details Emits the call, then calls emitRuntimeErrCheck with emitTrapFromErr.
    /// @param retTy Return type (typically i32 for error codes).
    /// @param callee Name of the runtime function to call.
    /// @param labelStem Stem used for failure/continuation block labels.
    void callWithErrCheck(Type retTy, const std::string &callee, std::string_view labelStem);

    /// @brief Emit a call with custom error handling.
    /// @param retTy Return type of the call.
    /// @param callee Name of the runtime function to call.
    /// @param labelStem Stem used for failure/continuation block labels.
    /// @param onFailure Callback invoked in the failure block.
    void callWithErrHandler(Type retTy,
                            const std::string &callee,
                            std::string_view labelStem,
                            const std::function<void(Value)> &onFailure);

    // -------------------------------------------------------------------------
    // Accessors
    // -------------------------------------------------------------------------

    /// @brief Get the collected arguments.
    /// @return Const reference to builder-owned argument values.
    [[nodiscard]] const std::vector<Value> &args() const noexcept;

    /// @brief Get the current source location.
    /// @return Source location if set, empty optional otherwise.
    [[nodiscard]] std::optional<il::support::SourceLoc> location() const noexcept;

    /// @brief Clear collected arguments for reuse.
    /// @details Does not clear the source location set by @ref at.
    void clearArgs() noexcept;

  private:
    /// Borrowed lowering driver receiving features, locations, and instructions.
    Lowerer &lowerer_;
    /// Ordered arguments retained across calls until explicitly cleared.
    std::vector<Value> args_;
    /// Optional source location applied before coercion or emission.
    std::optional<il::support::SourceLoc> loc_;

    /// @brief Apply stored location to lowerer before emission.
    /// @details Leaves the lowerer's current location unchanged when none is stored.
    void applyLoc() const;
};

} // namespace il::frontends::basic
