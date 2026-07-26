//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/transform/SimplifyCFG/BlockMerging.hpp
// Purpose: Block-merging sub-transformation for SimplifyCFG. Merges blocks
//          with a single predecessor into that predecessor after substituting
//          branch arguments for block parameters. Reduces block count while
//          preserving terminator semantics, parameter alignment, and
//          EH-sensitive constraints.
// Key invariants:
//   - Merge only occurs when the predecessor branches unconditionally.
//   - EH-sensitive blocks are never merged.
// Ownership/Lifetime: Stateless free function operating on caller-owned IR
//          via the SimplifyCFGPassContext reference.
// Links: il/transform/SimplifyCFG.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares single-predecessor block merging for SimplifyCFG.
 *
 * @details The subtransform substitutes incoming edge arguments for block
 *          parameters and splices eligible blocks into unconditional
 *          predecessors while preserving exception-sensitive CFG structure.
 */

#pragma once

#include "il/transform/SimplifyCFG.hpp"

namespace il::transform::simplify_cfg {

/// @brief Merge eligible single-predecessor blocks within the current function.
/// @details Iterates blocks in order and attempts to merge a block into its sole
///          predecessor when the predecessor branches unconditionally to it and
///          the block is not EH-sensitive. The merge substitutes incoming
///          branch arguments for block parameters, splices non-terminator
///          instructions into the predecessor, replaces the predecessor's
///          terminator with the merged terminator, and deletes the redundant
///          block. Statistics and optional debug logging are updated through
///          the pass context.
/// @param ctx Pass context containing the function under transformation and
///            the statistics sink.
/// @return True when the CFG changed as a result of merging.
bool mergeSinglePredBlocks(SimplifyCFG::SimplifyCFGPassContext &ctx);

} // namespace il::transform::simplify_cfg
