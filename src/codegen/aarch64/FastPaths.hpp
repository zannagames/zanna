//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/FastPaths.hpp
// Purpose: Fast-path pattern matching for common IL patterns.
// Key invariants:
//   - Each fast-path returns a fully-lowered MFunction or nullopt.
//   - Fast-path output must be semantically identical to generic lowering.
// Ownership/Lifetime:
//   - Stateless free function; borrows references for the duration of the call
//     and does not retain them.
// Links: src/codegen/aarch64/FastPaths.cpp,
//        src/codegen/aarch64/fastpaths/FastPathsInternal.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares side-effect-free probes for specialized AArch64 IL lowering.
 *
 * Each category is attempted against a scratch copy of the seed machine
 * function. A failed probe therefore leaves caller state untouched; the first
 * successful category returns a complete machine function by value.
 */

#pragma once

#include "FrameBuilder.hpp"
#include "MachineIR.hpp"
#include "TargetAArch64.hpp"
#include "il/core/Function.hpp"

#include <cstddef>
#include <optional>
#include <unordered_map>

namespace zanna::codegen::aarch64 {

/// @brief Try fast-path lowering for simple function patterns.
/// @details Recognises a small set of trivially-lowerable IL function shapes
///          (e.g. single-block returns of a constant, identity passthrough,
///          simple register-to-register copies) and produces a complete MIR
///          function in one pass — skipping the general per-instruction
///          dispatcher. Returns @c nullopt for any function that does not match,
///          leaving the caller to fall back to the generic lowering pipeline.
/// @param fn  Source IL function under consideration.
/// @param ti  Target description (used for ABI, scratch-register choices).
/// @param fb  Caller frame builder retained for API symmetry; probes use a
///            scratch builder bound to their scratch machine function.
/// @param mf  Pre-allocated MIR function that the fast-path writes into.
/// @param stringLiteralByteLengths Optional map from string-literal label to byte length;
///        used by patterns that lower constant-string passthrough.
/// @param knownVarArgNamedArgCounts Optional map of vararg function names to their
///        non-variadic prefix arity; allows fast-path matching of calls into varargs.
/// @return The lowered MFunction if a fast-path matched, @c nullopt otherwise.
/// @post @p mf and @p fb are unchanged when no fast path matches.
std::optional<MFunction> tryFastPaths(
    const il::core::Function &fn,
    const TargetInfo &ti,
    FrameBuilder &fb,
    MFunction &mf,
    const std::unordered_map<std::string, std::size_t> *stringLiteralByteLengths = nullptr,
    const std::unordered_map<std::string, std::size_t> *knownVarArgNamedArgCounts = nullptr);

} // namespace zanna::codegen::aarch64
