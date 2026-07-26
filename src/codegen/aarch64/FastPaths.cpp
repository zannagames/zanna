//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: codegen/aarch64/FastPaths.cpp
// Purpose: Fast-path pattern matching dispatcher for common IL patterns.
//          Routes the function to a specialized handler (memory, cast, arithmetic,
//          call, or return) that returns a fully-lowered MFunction without going
//          through the generic InstrLowering pass.
// Key invariants:
//   - Fast paths are tried in order of specificity; first match wins.
//   - Each attempt operates on a scratch MFunction copy so failures are side-effect free.
//   - Fast-path output must be semantically identical to generic lowering.
// Ownership/Lifetime:
//   - Borrows the caller's MFunction as a seed; returns an owned copy on success.
// Links: src/codegen/aarch64/FastPaths.hpp,
//        src/codegen/aarch64/fastpaths/FastPathsInternal.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements ordered, transactional AArch64 fast-path probes.
 *
 * Memory and cast patterns are tried before progressively broader arithmetic,
 * call, and return patterns. Every probe receives an isolated machine function
 * and frame builder so only a successful returned value can escape.
 */

#include "FastPaths.hpp"
#include "fastpaths/FastPathsInternal.hpp"

namespace zanna::codegen::aarch64 {
namespace {

/// @brief Try a single fast-path category on a temporary scratch copy of the MFunction.
/// @details Creates a fresh copy of @p seedMf and a matching FrameBuilder, invokes
///          @p attempt, and returns the result. The scratch copy ensures that a
///          failed attempt leaves the caller's MFunction unmodified.
/// @tparam AttemptFn Callable `std::optional<MFunction>(FastPathContext&)`.
/// @param fn                         IL function being compiled.
/// @param ti                         Target calling-convention and register info.
/// @param seedMf                     Template MFunction state (name, params, etc.).
/// @param stringLiteralByteLengths   Optional map of string-literal sizes.
/// @param knownVarArgNamedArgCounts  Optional map of variadic arg counts.
/// @param attempt                    Fast-path probe called with the scratch context.
/// @return The lowered MFunction if the probe matched, nullopt otherwise.
template <typename AttemptFn>
std::optional<MFunction> tryFastPathAttempt(
    const il::core::Function &fn,
    const TargetInfo &ti,
    const MFunction &seedMf,
    const std::unordered_map<std::string, std::size_t> *stringLiteralByteLengths,
    const std::unordered_map<std::string, std::size_t> *knownVarArgNamedArgCounts,
    AttemptFn &&attempt) {
    MFunction scratchMf = seedMf;
    FrameBuilder scratchFb(scratchMf);
    fastpaths::FastPathContext scratchCtx(
        fn, ti, scratchFb, scratchMf, stringLiteralByteLengths, knownVarArgNamedArgCounts);
    if (auto result = attempt(scratchCtx))
        return std::move(*result);
    return std::nullopt;
}

} // namespace

/// @brief Try all specialized lowering categories in specificity order.
/// @param fn Source IL function.
/// @param ti Target ABI and register description.
/// @param fb Caller frame builder; intentionally left unchanged.
/// @param mf Seed machine function copied for each category.
/// @param stringLiteralByteLengths Optional string-literal size metadata.
/// @param knownVarArgNamedArgCounts Optional fixed-prefix arities for variadic callees.
/// @return First complete fast-path result, or `std::nullopt`.
std::optional<MFunction> tryFastPaths(
    const il::core::Function &fn,
    const TargetInfo &ti,
    FrameBuilder &fb,
    MFunction &mf,
    const std::unordered_map<std::string, std::size_t> *stringLiteralByteLengths,
    const std::unordered_map<std::string, std::size_t> *knownVarArgNamedArgCounts) {
    if (fn.blocks.empty())
        return std::nullopt;
    (void)fb;

    // =========================================================================
    // Try fast-paths in order of specificity
    // =========================================================================
    // More specific patterns (memory, casts) are tried first, followed by
    // more general patterns (arithmetic, calls, returns).

    // Memory operations: alloca/store/load/ret pattern
    /// @brief Attempt memory-operation fast-path recognition.
    /// @param ctx Prepared matching and emission context.
    /// @return Lowered function on a complete match, otherwise `std::nullopt`.
    if (auto result = tryFastPathAttempt(
            fn,
            ti,
            mf,
            stringLiteralByteLengths,
            knownVarArgNamedArgCounts,
            [](fastpaths::FastPathContext &ctx) { return fastpaths::tryMemoryFastPaths(ctx); }))
        return result;

    // Type conversions: zext1/trunc1, narrowing casts, FP conversions
    /// @brief Attempt cast and conversion fast-path recognition.
    /// @param ctx Prepared matching and emission context.
    /// @return Lowered function on a complete match, otherwise `std::nullopt`.
    if (auto result = tryFastPathAttempt(
            fn,
            ti,
            mf,
            stringLiteralByteLengths,
            knownVarArgNamedArgCounts,
            [](fastpaths::FastPathContext &ctx) { return fastpaths::tryCastFastPaths(ctx); }))
        return result;

    // Integer arithmetic: add/sub/mul/and/or/xor, comparisons, shifts
    /// @brief Attempt integer-arithmetic fast-path recognition.
    /// @param ctx Prepared matching and emission context.
    /// @return Lowered function on a complete match, otherwise `std::nullopt`.
    if (auto result = tryFastPathAttempt(fn,
                                         ti,
                                         mf,
                                         stringLiteralByteLengths,
                                         knownVarArgNamedArgCounts,
                                         [](fastpaths::FastPathContext &ctx) {
                                             return fastpaths::tryIntArithmeticFastPaths(ctx);
                                         }))
        return result;

    // Floating-point arithmetic: fadd/fsub/fmul/fdiv
    /// @brief Attempt floating-point arithmetic fast-path recognition.
    /// @param ctx Prepared matching and emission context.
    /// @return Lowered function on a complete match, otherwise `std::nullopt`.
    if (auto result = tryFastPathAttempt(fn,
                                         ti,
                                         mf,
                                         stringLiteralByteLengths,
                                         knownVarArgNamedArgCounts,
                                         [](fastpaths::FastPathContext &ctx) {
                                             return fastpaths::tryFPArithmeticFastPaths(ctx);
                                         }))
        return result;

    // Call lowering: call @callee(args...) feeding ret
    /// @brief Attempt call-and-return fast-path recognition.
    /// @param ctx Prepared matching and emission context.
    /// @return Lowered function on a complete match, otherwise `std::nullopt`.
    if (auto result = tryFastPathAttempt(
            fn,
            ti,
            mf,
            stringLiteralByteLengths,
            knownVarArgNamedArgCounts,
            [](fastpaths::FastPathContext &ctx) { return fastpaths::tryCallFastPaths(ctx); }))
        return result;

    // Simple returns: ret %param, ret const, ret const_str/addr_of
    /// @brief Attempt simple-return fast-path recognition.
    /// @param ctx Prepared matching and emission context.
    /// @return Lowered function on a complete match, otherwise `std::nullopt`.
    if (auto result = tryFastPathAttempt(
            fn,
            ti,
            mf,
            stringLiteralByteLengths,
            knownVarArgNamedArgCounts,
            [](fastpaths::FastPathContext &ctx) { return fastpaths::tryReturnFastPaths(ctx); }))
        return result;

    // No fast-path matched
    return std::nullopt;
}

} // namespace zanna::codegen::aarch64
