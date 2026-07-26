//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/transform/SimplifyCFG/ParamCanonicalization.hpp
// Purpose: Parameter and branch-argument canonicalisation for SimplifyCFG.
//          Removes unused block parameters and parameters identical across all
//          predecessors, then realigns branch argument lists for CFG edge arity
//          compatibility. Avoids EH-sensitive regions.
// Key invariants:
//   - After canonicalisation, block parameter count matches all incoming edge
//     argument counts.
//   - EH-sensitive blocks are skipped.
// Ownership/Lifetime: Stateless free function operating on caller-owned IR
//          via the SimplifyCFGPassContext reference.
// Links: il/transform/SimplifyCFG.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares block-parameter and edge-argument canonicalization.
 *
 * @details The SimplifyCFG subtransform removes redundant or unused block
 *          parameters and keeps every incoming edge's argument vector aligned
 *          with the resulting block signature.
 */

#pragma once

#include "il/transform/SimplifyCFG.hpp"

namespace il::transform::simplify_cfg {

/// @brief Canonicalise block parameters and incoming branch arguments.
/// @details For each non-EH-sensitive block, the helper first replaces
///          parameters that always receive the same value from every
///          predecessor by substituting that value within the block and
///          removing the corresponding arguments on incoming edges. It then
///          drops parameters that are never referenced in the block body and
///          trims predecessor argument lists to match the updated signature.
///          Statistics and debug logging are updated through the pass context,
///          and the function returns whether any canonicalisation occurred.
/// @param ctx Pass context containing the function under transformation and
///            mutable statistics.
/// @return True if any block signatures or edge arguments were simplified.
bool canonicalizeParamsAndArgs(SimplifyCFG::SimplifyCFGPassContext &ctx);

} // namespace il::transform::simplify_cfg
