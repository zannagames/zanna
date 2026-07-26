//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/transform/SimplifyCFG/BranchFolding.hpp
// Purpose: Declares branch folding routines for the SimplifyCFG pass.
// Key invariants: Transformations preserve control flow equivalence.
// Ownership/Lifetime: Operates on caller-owned functions and blocks.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares trivial conditional-branch and switch folding.
 *
 * @details These SimplifyCFG subtransforms replace statically determined or
 *          equivalent successor choices with direct branches while retaining
 *          the selected edge's block arguments.
 */

#pragma once

#include "il/transform/SimplifyCFG.hpp"

namespace il::transform::simplify_cfg {

/// \brief Replace switches with a single reachable case by an unconditional branch.
/// \details Examines `switch.i32` terminators; when all but one case are
///          unreachable (or map to the same destination), rewrites the block to
///          a direct `br` and updates associated branch arguments.
/// \param ctx Pass context providing function, module, and stats.
/// \return True when any switch was simplified.
bool foldTrivialSwitches(SimplifyCFG::SimplifyCFGPassContext &ctx);

/// \brief Simplify conditional branches with constant or identical targets.
/// \details Handles `cbr` where the condition is a constant, or where both
///          successors are the same block (or branch arguments are identical),
///          replacing them with an unconditional branch and trimming params.
/// \param ctx Pass context providing function, module, and stats.
/// \return True when any conditional branch was simplified.
bool foldTrivialConditionalBranches(SimplifyCFG::SimplifyCFGPassContext &ctx);

} // namespace il::transform::simplify_cfg
