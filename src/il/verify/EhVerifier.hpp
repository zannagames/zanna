//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/verify/EhVerifier.hpp
// Purpose: Validates exception handling stack balance in IL functions.
//          Ensures eh.push/eh.pop instructions maintain balanced stack
//          discipline on all control-flow paths, detects underflows and leaks,
//          and verifies resume.* instructions occur within active handlers.
// Key invariants:
//   - Every eh.push must have a corresponding eh.pop on all paths.
//   - No handler stack leaks at function exit.
//   - resume.* requires an active resume token.
// Ownership/Lifetime: EhVerifier is a stateless class; run() borrows the
//          module and diagnostic sink for the duration of the call.
// Links: il/verify/DiagSink.hpp, support/diag_expected.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares the module-level coordinator for EH verification.
/// @details `EhVerifier` builds an independent canonical model for each
///          function containing EH-relevant instructions and runs the reusable
///          stack, provenance, dominance, reachability, and resume-edge checks.

#pragma once

#include "il/verify/DiagSink.hpp"

#include "support/diag_expected.hpp"

namespace il::core {
struct Module;
}

namespace il::verify {

/// @brief Stateless verifier pass for function-local EH invariants.
class EhVerifier {
  public:
    /// @brief Analyse EH-bearing functions in @p module.
    /// @details Skips functions without EH-relevant opcodes and returns
    ///          immediately on the first failed check. The current
    ///          implementation accepts @p sink for verifier-interface
    ///          compatibility but returns diagnostics through `Expected`.
    /// @param module Module whose functions are analysed.
    /// @param sink Reserved diagnostic sink; currently not written.
    /// @return Success or the first stack, provenance, dominance, reachability,
    ///         or resume-edge diagnostic.
    [[nodiscard]] il::support::Expected<void> run(const il::core::Module &module,
                                                  DiagSink &sink) const;
};

} // namespace il::verify
