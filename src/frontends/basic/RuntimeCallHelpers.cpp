//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file RuntimeCallHelpers.cpp
/// @brief Implements the BASIC runtime call builder.
/// @details Provides the out-of-line definitions for @ref RuntimeCallBuilder.
///          The builder offers a fluent interface for assembling runtime call
///          arguments, applying standard coercions, and emitting calls with
///          optional runtime error handling.
//
//===----------------------------------------------------------------------===//

#include "RuntimeCallHelpers.hpp"
#include "Lowerer.hpp"

namespace il::frontends::basic {

/// @brief Construct a runtime call builder bound to a lowerer.
/// @details Stores a reference to the lowering context and initializes an empty
///          argument list; no instructions are emitted until a call is made.
/// @param lowerer Lowering engine used to emit IL instructions.
/// @pre @p lowerer outlives the builder.
RuntimeCallBuilder::RuntimeCallBuilder(Lowerer &lowerer) noexcept : lowerer_(lowerer) {}

/// @brief Record a source location for subsequent emissions.
/// @details The stored location is applied to the lowerer immediately before
///          any arguments are coerced or calls are emitted.
/// @param loc Source location to attach to emitted instructions.
/// @return Reference to this builder for chaining.
RuntimeCallBuilder &RuntimeCallBuilder::at(il::support::SourceLoc loc) noexcept {
    loc_ = loc;
    return *this;
}

/// @brief Append a raw argument without coercion.
/// @details The value is added to the argument list as-is, enabling callers to
///          supply pre-coerced or constant values.
/// @param v Argument value to append.
/// @return Reference to this builder for chaining.
RuntimeCallBuilder &RuntimeCallBuilder::arg(Value v) {
    args_.push_back(v);
    return *this;
}

/// @brief Append an argument narrowed to 32 bits.
/// @details Applies a narrowing conversion using @ref Lowerer::narrow32 and
///          appends the narrowed value to the argument list.
/// @param v 64-bit value to narrow.
/// @return Reference to this builder for chaining.
RuntimeCallBuilder &RuntimeCallBuilder::argNarrow32(Value v) {
    applyLoc();
    Value narrowed = lowerer_.narrow32(v, loc_.value_or(il::support::SourceLoc{}));
    args_.push_back(narrowed);
    return *this;
}

/// @brief Append a channel argument normalized to i32.
/// @details Wraps the value in an r-value, applies
///          @ref Lowerer::normalizeChannelToI32, and appends the normalized
///          channel value.
/// @param v Channel value to normalize.
/// @param ty Type of the channel value.
/// @return Reference to this builder for chaining.
RuntimeCallBuilder &RuntimeCallBuilder::argChannel(Value v, Type ty) {
    applyLoc();
    Lowerer::RVal channel{v, ty};
    channel =
        lowerer_.normalizeChannelToI32(std::move(channel), loc_.value_or(il::support::SourceLoc{}));
    args_.push_back(channel.value);
    return *this;
}

/// @brief Append an argument coerced to i64.
/// @details Uses @ref Lowerer::ensureI64 to coerce the value and appends the
///          resulting i64 to the argument list.
/// @param v Value to coerce.
/// @param ty Current type of the value.
/// @return Reference to this builder for chaining.
RuntimeCallBuilder &RuntimeCallBuilder::argI64(Value v, Type ty) {
    applyLoc();
    Lowerer::RVal rval{v, ty};
    rval = lowerer_.ensureI64(std::move(rval), loc_.value_or(il::support::SourceLoc{}));
    args_.push_back(rval.value);
    return *this;
}

/// @brief Append an argument coerced to f64.
/// @details Uses @ref Lowerer::ensureF64 to coerce the value and appends the
///          resulting f64 to the argument list.
/// @param v Value to coerce.
/// @param ty Current type of the value.
/// @return Reference to this builder for chaining.
RuntimeCallBuilder &RuntimeCallBuilder::argF64(Value v, Type ty) {
    applyLoc();
    Lowerer::RVal rval{v, ty};
    rval = lowerer_.ensureF64(std::move(rval), loc_.value_or(il::support::SourceLoc{}));
    args_.push_back(rval.value);
    return *this;
}

/// @brief Request a runtime helper feature before emitting a call.
/// @details Delegates to @ref Lowerer::requestHelper so the helper requirement
///          is included in later runtime declaration emission.
/// @param feature Runtime feature to request.
/// @return Reference to this builder for chaining.
RuntimeCallBuilder &RuntimeCallBuilder::withFeature(RuntimeFeature feature) {
    lowerer_.requestHelper(feature);
    return *this;
}

/// @brief Track a runtime feature for ordered extern emission.
/// @details Delegates to @ref Lowerer::trackRuntime so runtime externs are
///          emitted deterministically.
/// @param feature Runtime feature to track.
/// @return Reference to this builder for chaining.
RuntimeCallBuilder &RuntimeCallBuilder::trackFeature(RuntimeFeature feature) {
    lowerer_.trackRuntime(feature);
    return *this;
}

/// @brief Invoke an explicit helper requirement callback.
/// @details Calls the specified member function on the lowerer to mark a helper
///          as required when no enum-based runtime feature exists.
/// @param requireFn Member-function pointer to invoke on the lowerer.
/// @return Reference to this builder for chaining.
RuntimeCallBuilder &RuntimeCallBuilder::withManualHelper(void (Lowerer::*requireFn)()) {
    (lowerer_.*requireFn)();
    return *this;
}

/// @brief Emit a void runtime call with collected arguments.
/// @details Applies the stored source location and emits a call with no return
///          value and no error handling. Collected arguments are retained until
///          @ref clearArgs is called.
/// @param callee Runtime function name to call.
void RuntimeCallBuilder::call(const std::string &callee) {
    applyLoc();
    lowerer_.emitCall(callee, args_);
}

/// @brief Emit a runtime call that returns a value.
/// @details Applies the stored source location and emits a call with the given
///          return type; no error handling is added and arguments are retained.
/// @param retTy Return type of the call.
/// @param callee Runtime function name to call.
/// @return Value representing the call result.
RuntimeCallBuilder::Value RuntimeCallBuilder::callRet(Type retTy, const std::string &callee) {
    applyLoc();
    return lowerer_.emitCallRet(retTy, callee, args_);
}

/// @brief Emit a runtime call via a feature helper.
/// @details Requests the runtime feature and then emits the call, returning the
///          value produced by the helper.
/// @param feature Runtime feature to request.
/// @param callee Runtime function name to call.
/// @param retTy Return type of the call.
/// @return Value representing the call result.
RuntimeCallBuilder::Value RuntimeCallBuilder::callHelper(RuntimeFeature feature,
                                                         const std::string &callee,
                                                         Type retTy) {
    applyLoc();
    return lowerer_.emitRuntimeHelper(feature, callee, retTy, args_);
}

/// @brief Emit a void runtime call via a feature helper.
/// @details Requests the runtime feature and emits the call with a void return
///          type.
/// @param feature Runtime feature to request.
/// @param callee Runtime function name to call.
void RuntimeCallBuilder::callHelperVoid(RuntimeFeature feature, const std::string &callee) {
    applyLoc();
    lowerer_.emitRuntimeHelper(feature, callee, Type(Type::Kind::Void), args_);
}

/// @brief Emit a runtime call and trap on non-zero error codes.
/// @details Emits the call, captures its return value, and then invokes
///          @ref Lowerer::emitRuntimeErrCheck to branch to a trap handler when
///          the error flag is non-zero.
/// @param retTy Return type of the call (typically i32 error code).
/// @param callee Runtime function name to call.
/// @param labelStem Prefix for generated failure/continuation blocks.
void RuntimeCallBuilder::callWithErrCheck(Type retTy,
                                          const std::string &callee,
                                          std::string_view labelStem) {
    applyLoc();
    Value err = lowerer_.emitCallRet(retTy, callee, args_);
    /// Emit the standard trap path using the returned runtime error code.
    lowerer_.emitRuntimeErrCheck(err,
                                 loc_.value_or(il::support::SourceLoc{}),
                                 labelStem,
                                 [this](Value code) { lowerer_.emitTrapFromErr(code); });
}

/// @brief Emit a runtime call with a custom error handler.
/// @details Emits the call and then delegates to
///          @ref Lowerer::emitRuntimeErrCheck, invoking @p onFailure in the
///          failure block to emit custom handling logic.
/// @param retTy Return type of the call (typically i32 error code).
/// @param callee Runtime function name to call.
/// @param labelStem Prefix for generated failure/continuation blocks.
/// @param onFailure Callback invoked in the failure block.
/// @note @p onFailure is invoked during this call and is not retained.
void RuntimeCallBuilder::callWithErrHandler(Type retTy,
                                            const std::string &callee,
                                            std::string_view labelStem,
                                            const std::function<void(Value)> &onFailure) {
    applyLoc();
    Value err = lowerer_.emitCallRet(retTy, callee, args_);
    lowerer_.emitRuntimeErrCheck(
        err, loc_.value_or(il::support::SourceLoc{}), labelStem, onFailure);
}

/// @brief Access the collected argument list.
/// @details Returns a reference to the internal argument vector for inspection
///          or testing; callers should not mutate the returned container.
/// @return Const reference to the argument vector.
const std::vector<RuntimeCallBuilder::Value> &RuntimeCallBuilder::args() const noexcept {
    return args_;
}

/// @brief Access the stored source location.
/// @details Returns the source location last set via @ref at, if any.
/// @return Optional source location.
std::optional<il::support::SourceLoc> RuntimeCallBuilder::location() const noexcept {
    return loc_;
}

/// @brief Clear collected arguments.
/// @details Resets only the argument list so the builder can be reused; the
///          optional source location remains set.
void RuntimeCallBuilder::clearArgs() noexcept {
    args_.clear();
}

/// @brief Apply the stored source location to the lowerer.
/// @details When a location was supplied via @ref at, copies it to the lowerer's
///          current location. With no stored location, leaves lowerer state unchanged.
void RuntimeCallBuilder::applyLoc() const {
    if (loc_)
        lowerer_.curLoc = *loc_;
}

} // namespace il::frontends::basic
