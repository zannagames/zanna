//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/transform/SimplifyCFG/ForwardingElimination.hpp
// Purpose: Empty forwarding-block elimination for SimplifyCFG. Identifies
//          blocks that only forward control and arguments to a single
//          successor, rewrites predecessor terminators to target the successor
//          directly, and removes the redundant blocks. Preserves EH semantics.
// Key invariants:
//   - Only blocks with a single unconditional branch and no side effects are
//     candidates for removal.
//   - EH-sensitive blocks are never eliminated.
// Ownership/Lifetime: Stateless free function operating on caller-owned IR
//          via the SimplifyCFGPassContext reference.
// Links: il/transform/SimplifyCFG.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares empty forwarding-block elimination for SimplifyCFG.
 *
 * @details The subtransform redirects predecessor edges through side-effect-free
 *          forwarding blocks, composes their block arguments, and removes only
 *          blocks whose elimination preserves exception-handling semantics.
 */

#pragma once

#include "il/transform/SimplifyCFG.hpp"

namespace il::transform::simplify_cfg {

/// @brief Remove trivial forwarding blocks from the current function.
/// @details Scans for blocks that are safe to remove (single unconditional
///          branch, no side effects, no reuse of locally defined temporaries),
///          redirects all predecessor edges to the forwarding block's successor
///          with substituted argument values, and erases the block when no
///          remaining edges refer to it. Updates statistics and optional debug
///          logs through the pass context to report redirected edges and removed
///          blocks.
/// @param ctx Pass context containing the function under transformation and
///            statistics/logging utilities.
/// @return True if any edges were redirected or blocks removed.
bool removeEmptyForwarders(SimplifyCFG::SimplifyCFGPassContext &ctx);

} // namespace il::transform::simplify_cfg
