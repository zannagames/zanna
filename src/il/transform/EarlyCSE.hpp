//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/transform/EarlyCSE.hpp
// Purpose: EarlyCSE (GVN-lite) -- common subexpression elimination over the
//          dominator tree. Commutes operands for commutative ops to improve
//          matching. Skips memory ops and side-effecting instructions. Each
//          block's expressions are visible to all dominated successors.
// Key invariants:
//   - Only eliminates instructions that are pure and non-trapping.
//   - Walks the dominator tree in pre-order; tables are scoped per domtree level.
// Ownership/Lifetime: Free function operating on a caller-owned Module + Function.
// Links: il/core/Function.hpp, il/core/Module.hpp, il/transform/ValueKey.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares dominator-scoped early common subexpression elimination.

#pragma once

#include "il/core/Function.hpp"
#include "il/core/Module.hpp"

namespace il::transform {

/// \brief Eliminate redundant pure instructions across dominator-tree scope.
/// \details Walks the dominator tree in pre-order, maintaining a stack of
///          expression tables. An expression computed in a dominator block is
///          visible in all blocks it dominates, enabling cross-block CSE.
///          Only pure, non-trapping, non-memory instructions are considered.
/// \param M Module containing \p F (needed for CFG construction).
/// \param F Function to optimize in place.
/// \return True when any instruction was removed or simplified.
bool runEarlyCSE(il::core::Module &M, il::core::Function &F);

} // namespace il::transform
