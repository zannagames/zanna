//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/il/verify/EhVerifier.cpp
//
// Purpose:
//   Implement the exception-handling verifier entry point. The verifier now
//   orchestrates reusable EhModel/EhChecks helpers to perform stack balance and
//   resume-edge validation while preserving historic diagnostics.
//
// Key invariants:
//   * Each function is analysed independently using a freshly constructed
//     EhModel.
//   * Only functions containing EH operations trigger additional checks.
//
// Ownership/Lifetime:
//   The verifier borrows IR nodes from the inspected module and never assumes
//   ownership. Diagnostics are surfaced via Expected results.
//
// Links:
//   docs/il/il-guide.md#reference
//
//===----------------------------------------------------------------------===//

#include "il/verify/EhVerifier.hpp"

#include "il/core/Module.hpp"
#include "il/verify/EhChecks.hpp"
#include "il/verify/EhModel.hpp"

/// @file
/// @brief Coordinates the exception-handling verification workflow.
/// @details Each function is analysed independently by constructing an
///          @ref EhModel on the fly and forwarding it to the reusable check
///          suite while preserving the existing diagnostic shape.

using namespace il::core;

namespace il::verify {

/// @brief Validate exception-handling metadata for every function in a module.
/// @details The verifier iterates each function, constructing an @ref EhModel
///          to reflect its handlers and unwind edges.  Functions without EH
///          instructions are skipped.  Remaining functions are passed through the
///          @ref checkEhStackBalance, @ref checkDominanceOfHandlers,
///          @ref checkUnreachableHandlers, and @ref checkResumeEdges routines in
///          sequence.  Any failure propagates immediately via the
///          @ref il::support::Expected channel while success yields an empty
///          result.
/// @param module Module whose functions are being verified.
/// @param sink Diagnostic sink reserved for future structured reporting.
/// @return Empty value on success or the first emitted diagnostic.
il::support::Expected<void> EhVerifier::run(const Module &module, DiagSink &sink) const {
    (void)sink;

    for (const auto &fn : module.functions) {
        EhModel model(fn);
        if (!model.hasEhInstructions())
            continue;

        if (auto result = checkEhStackBalance(model); !result)
            return result;

        if (auto result = checkDominanceOfHandlers(model); !result)
            return result;

        if (auto result = checkUnreachableHandlers(model); !result)
            return result;

        if (auto result = checkResumeEdges(model); !result)
            return result;
    }

    return {};
}

} // namespace il::verify
