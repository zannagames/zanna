//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/transform/DSE.hpp
// Purpose: Dead Store Elimination -- function-level pass that removes stores
//          provably overwritten before being read. Intra-block DSE uses a
//          backward scan; cross-block DSE uses forward dataflow on
//          non-escaping allocas. Aliasing queries rely on BasicAA.
// Key invariants:
//   - Only removes stores that are provably dead (overwritten on all paths).
//   - Calls conservatively clobber memory per BasicAA ModRef classification.
// Ownership/Lifetime: Free functions operating on caller-owned Function and
//          AnalysisManager references.
// Links: il/core/Function.hpp, il/transform/AnalysisManager.hpp,
//        il/analysis/BasicAA.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares alias-aware intra- and cross-block dead-store elimination.

#pragma once

#include "il/core/Function.hpp"
#include "il/transform/AnalysisManager.hpp"

namespace il::transform {

/// \brief Eliminate trivially dead stores inside a function (intra-block).
/// \details Performs a backward walk per basic block. A store to address P is
/// considered dead if no intervening load of an alias of P occurs before a
/// subsequent store to an alias of P. Calls conservatively clobber memory when
/// they may Mod/Ref according to BasicAA's ModRef classification.
/// \param F Function to transform in place.
/// \param AM Analysis manager for alias/modref queries.
/// \return True when any store was removed.
bool runDSE(il::core::Function &F, AnalysisManager &AM);

/// \brief Eliminate dead stores across basic block boundaries.
/// \details Compatibility entry point for the canonical MemorySSA-based
/// cross-block proof. Kept for callers that used the former standalone pass.
/// \param F Function to transform in place.
/// \param AM Analysis manager for alias/modref queries.
/// \return True when any store was removed.
bool runCrossBlockDSE(il::core::Function &F, AnalysisManager &AM);

/// \brief MemorySSA-based dead store elimination.
/// \details Uses the MemorySSA analysis to find dead stores. Calls are not
/// treated as read barriers for non-escaping
/// allocas (because calls cannot access stack memory that has not escaped).
///
/// This pass should run after runDSE (intra-block).
///
/// \param F Function to transform in place.
/// \param AM Analysis manager providing MemorySSA and BasicAA results.
/// \return True when any store was removed.
bool runMemorySSADSE(il::core::Function &F, AnalysisManager &AM);

} // namespace il::transform
