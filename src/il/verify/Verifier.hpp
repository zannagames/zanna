//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This file declares the top-level Verifier class, which provides the public
// interface for validating IL modules. This is the main entry point for IL
// verification, coordinating all verification passes.
//
// The IL specification defines structural and semantic constraints that every
// valid IL module must satisfy: unique symbol names, well-typed expressions,
// properly structured control flow, balanced exception handlers, and valid
// references between entities. The Verifier orchestrates the specialized verifier
// components (ExternVerifier, GlobalVerifier, FunctionVerifier, EhVerifier) to
// enforce these constraints.
//
// Key Responsibilities:
// - Provide the public verify() interface for IL module validation
// - Orchestrate the verification pipeline (externs -> globals -> functions -> EH)
// - Collect bounded diagnostics and expose a primary-error convenience result
// - Ensure verification is stateless and thread-safe
//
// Design Notes:
// The Verifier class is a simple facade with only a static verify() method. It
// delegates to specialized verifier components, each responsible for one aspect
// of module validation. Verification proceeds in dependency order: externs must
// be validated before functions (since functions reference externs), functions
// must be validated before exception handling (since EH analysis examines function
// bodies). verifyAll advances through stages until its diagnostic budget is
// exhausted; verify returns the first collected error with the remaining
// diagnostics attached as notes.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares the public facade for complete IL module verification.
/// @details The facade offers a primary-error `Expected` interface and a bounded
///          multi-diagnostic interface over the same extern, global, function,
///          and exception-handling pipeline.

#pragma once

#include "support/diag_expected.hpp"

#include <cstddef>
#include <vector>

namespace il::core {
struct Module;
}

namespace il::verify {

/// @brief Verifies structural and type rules for a module.
class Verifier {
  public:
    /// @brief Verify module @p m against the IL specification.
    /// @details Collects up to 50 diagnostics, returns the first error, and
    ///          attaches other collected diagnostics as notes. Warning-only
    ///          results are considered successful.
    /// @param m Module to verify.
    /// @return Expected success or diagnostic on failure.
    [[nodiscard]] static il::support::Expected<void> verify(const il::core::Module &m);

    /// @brief Collect verifier diagnostics from each verifier stage.
    /// @details Runs stages in dependency order until the diagnostic cap is
    ///          reached, normalizes missing codes/stages, and removes exact
    ///          duplicates. A zero cap returns an empty vector.
    /// @param m Module to verify.
    /// @param maxDiagnostics Maximum diagnostics to return.
    /// @return Empty vector when valid; otherwise one or more diagnostics.
    [[nodiscard]] static std::vector<il::support::Diag> verifyAll(const il::core::Module &m,
                                                                  size_t maxDiagnostics = 50);
};

} // namespace il::verify
