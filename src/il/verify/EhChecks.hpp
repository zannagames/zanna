//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/il/verify/EhChecks.hpp
//
// Purpose:
//   Declare reusable predicates and invariants that operate on the canonical
//   EhModel. These checks capture EH structural constraints shared across
//   verifier components.
//
// EH Verification Checks:
//   This module provides four complementary checks that together ensure
//   well-formed exception handling in IL programs:
//
//   1. checkEhStackBalance - Validates eh.push/eh.pop balance across all paths
//   2. checkDominanceOfHandlers - Ensures handlers dominate protected blocks
//   3. checkUnreachableHandlers - Detects dead handler code
//   4. checkResumeEdges - Validates resume.label postdominance requirements
//
// Testing:
//   All checks are comprehensively tested in test_il_verify_eh_checks.cpp with
//   both passing and failing test cases for each invariant.
//
// Key invariants:
//   * Diagnostics mirror the wording emitted by the legacy EH verifier.
//   * Callers construct an EhModel and supply it to the desired predicates.
//
// Ownership/Lifetime:
//   Functions observe the EhModel and never take ownership of IR resources.
//
// Links:
//   docs/il/il-guide.md#reference
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares exception-handling invariants evaluated over `EhModel`.
/// @details The checks cover path-sensitive stack and resume-token flow,
///          dominance of protected regions, relevant handler reachability, and
///          post-dominance of `resume.label` targets. Each function returns the
///          first structured verifier diagnostic it encounters.

#pragma once

#include "il/verify/EhModel.hpp"

#include "support/diag_expected.hpp"

namespace il::verify {

/// @brief Validate that eh.push/eh.pop instructions remain balanced.
/// @details Simulates reachable paths with handler-stack and resume-token
///          provenance state. It rejects ordinary stack underflow, live
///          handlers at returns, invalid handler entry, and malformed resume
///          token consumption or forwarding.
/// @param model Canonical EH model describing the function.
/// @return Success when balanced; diagnostic otherwise.
[[nodiscard]] il::support::Expected<void> checkEhStackBalance(const EhModel &model);

/// @brief Validate that exception handlers dominate the blocks they protect.
///
/// Handler Dominance Invariant:
///   When an `eh.push ^handler` instruction installs a handler, the basic block
///   containing that eh.push must dominate all basic blocks that could potentially
///   fault while under the handler's protection. This ensures structured exception
///   handling: a handler cannot be installed for code paths that may have already
///   executed, which would create non-deterministic exception dispatch.
///
/// The check builds a forward dominator tree and computes handler coverage (which
/// blocks are protected by which handlers). For each handler, it verifies that the
/// block containing the eh.push instruction dominates every block in its coverage set.
///
/// @param model Canonical EH model describing the function.
/// @return Success when all eh.push blocks dominate their protected blocks; diagnostic otherwise.
[[nodiscard]] il::support::Expected<void> checkDominanceOfHandlers(const EhModel &model);

/// @brief Validate reachability of handlers required by protected faulting code.
///
/// Handler Reachability Invariant:
///   A handler associated with a potentially faulting instruction in a reachable
///   protected region must be reachable from the function entry when normal CFG
///   edges and simulated exception dispatch edges are both considered. A
///   referenced handler whose protected region cannot fault is treated as
///   unused, rather than invalid.
///
/// The check identifies handler blocks from canonical push sites, then performs
/// a CFG traversal from entry while tracking the active handler stack.
///
/// @param model Canonical EH model describing the function.
/// @return Success when every required handler is reachable; otherwise a
///         diagnostic listing unreachable handler labels.
[[nodiscard]] il::support::Expected<void> checkUnreachableHandlers(const EhModel &model);

/// @brief Validate resume.label edges against handler coverage information.
/// @details Computes the blocks protected by each handler and requires every
///          resolved `resume.label` target in that handler to post-dominate each
///          covered faulting block with outgoing control flow. Malformed or
///          unresolved targets are left to structural verification.
/// @param model Canonical EH model describing the function.
/// @return Success when all resume targets are valid; diagnostic otherwise.
[[nodiscard]] il::support::Expected<void> checkResumeEdges(const EhModel &model);

} // namespace il::verify
