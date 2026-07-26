//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/transform/SCCP.hpp
// Purpose: Sparse Conditional Constant Propagation -- worklist-based dataflow
//          analysis that simultaneously performs constant propagation and dead
//          branch elimination using three-state lattice (Bottom/Constant/Top).
//          Block parameters are treated as SSA phi nodes merging values from
//          executable predecessors only.
// Key invariants:
//   - Conservative: values are assumed overdefined unless proven constant.
//   - Only executable CFG edges are analysed; dead code is skipped.
// Ownership/Lifetime: Free functions operating on caller-owned Function/Module IR.
// Links: il/core/fwd.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares sparse conditional constant propagation over IL SSA.
 *
 * @details SCCP jointly discovers executable CFG edges and constant SSA values
 *          using a bottom/constant/overdefined lattice. Block parameters merge
 *          only values from executable predecessor edges, allowing the rewrite
 *          phase to substitute constants, fold branches, and discard dead
 *          regions conservatively.
 */

#pragma once

#include "il/core/fwd.hpp"

namespace il::transform {

/// @brief Propagate constants through the IL using sparse conditional evaluation.
///
/// @details Identifies executable regions of the CFG, evaluates instructions whose
/// operands become constant, folds conditional branches, and rewrites uses of
/// discovered constants.  Block parameters are treated as SSA phi nodes whose
/// meet only considers executable predecessors.
///
/// @param function Function optimised in place.
/// @return True when the function IR was rewritten.
bool sccp(core::Function &function);

/// @brief Propagate constants through every function in a module.
///
/// @details Applies SCCP independently to each function. Callers that need
/// change tracking should invoke the function overload instead.
///
/// @param module Module optimised in place.
void sccp(core::Module &module);

} // namespace il::transform
