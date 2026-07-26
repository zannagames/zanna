//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/transform/SimplifyCFG/JumpThreading.hpp
// Purpose: Declares jump threading routines for the SimplifyCFG pass.
// Key invariants: Transformations preserve control flow semantics.
// Ownership/Lifetime: Operates on caller-owned functions and blocks.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares constant-edge jump threading for SimplifyCFG.
 *
 * @details Jump threading uses values supplied on predecessor edges to resolve
 *          an intermediate conditional branch and retarget the predecessor
 *          directly to the selected successor with correctly remapped arguments.
 */

#pragma once

#include "il/transform/SimplifyCFG.hpp"

namespace il::transform::simplify_cfg {

/// \brief Thread jumps through blocks with predictable branch conditions.
/// \details When a predecessor passes a constant value that determines a
///          conditional branch outcome, redirect the predecessor to bypass
///          the intermediate block and jump directly to the known successor.
///
/// Example transformation:
///   Before:                          After:
///   pred:                            pred:
///     br B(1)                          br C(args_to_C)
///   B(cond):                         B(cond):  // may become dead
///     cbr cond, C, D                   cbr cond, C, D
///
/// This eliminates unnecessary branches and can enable further simplifications.
///
/// \param ctx Pass context providing function, module, and stats.
/// \return True when any jump was threaded.
bool threadJumps(SimplifyCFG::SimplifyCFGPassContext &ctx);

} // namespace il::transform::simplify_cfg
