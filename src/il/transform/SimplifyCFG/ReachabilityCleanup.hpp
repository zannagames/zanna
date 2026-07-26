//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/transform/SimplifyCFG/ReachabilityCleanup.hpp
// Purpose: Reachability-based cleanup for SimplifyCFG. Computes reachability
//          from the function entry block and removes unreachable blocks,
//          preserving EH structure and blocks referenced by retained EH regions.
// Key invariants:
//   - EH-sensitive blocks are never removed even if unreachable.
//   - Blocks referenced by retained unreachable EH regions are not removed.
// Ownership/Lifetime: Stateless free function operating on caller-owned IR
//          via the SimplifyCFGPassContext reference.
// Links: il/transform/SimplifyCFG.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares exception-aware unreachable-block cleanup for SimplifyCFG.
 *
 * @details The subtransform computes entry reachability across supported
 *          terminators and removes dead blocks unless exception-handling
 *          relationships require their retention.
 */

#pragma once

#include "il/transform/SimplifyCFG.hpp"

namespace il::transform::simplify_cfg {

/// @brief Remove blocks that are unreachable from the entry block.
/// @details Performs a reachability traversal that follows branch, conditional
///          branch, switch, and resume edges, then erases blocks not marked
///          reachable, except EH-sensitive blocks and blocks referenced by
///          those retained regions. Statistics and optional debug logging are
///          updated via the pass context.
/// @param ctx Pass context providing the function, EH checks, and stats sink.
/// @return True if any unreachable blocks were removed.
bool removeUnreachableBlocks(SimplifyCFG::SimplifyCFGPassContext &ctx);

} // namespace il::transform::simplify_cfg
