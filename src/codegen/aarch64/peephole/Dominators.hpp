//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/peephole/Dominators.hpp
// Purpose: Peephole-namespace spelling of the shared bit-vector dominator
//          analysis (codegen/common/ra/Dominators.hpp). The AArch64
//          implementation moved to the common header so both backends'
//          MirCfg helpers compute dominators the same way; this header keeps
//          the historical names for the peephole passes.
//
// Key invariants:
//   - Entry block (index 0) dominates only itself in the result.
//   - Unreachable blocks (no predecessors after index 0) dominate only themselves.
//   - Prefer MirCfg::dominators(), which feeds the analysis the CFG every
//     other consumer sees; call computeDominators directly only with an
//     index-based predecessor table built from MirCfg.
//
// Ownership/Lifetime:
//   - Aliases only; see the common header for the value type.
//
// Links: codegen/common/ra/Dominators.hpp, codegen/aarch64/MirCfg.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/common/ra/Dominators.hpp"

/// @file
/// @brief Re-exports the shared dominator analysis for AArch64 peephole passes.

namespace zanna::codegen::aarch64::peephole {

/// @brief Packed dominator sets (see zanna::codegen::ra::DominatorSets).
using DominatorSets = zanna::codegen::ra::DominatorSets;

/// @brief Shared dominator computation (see zanna::codegen::ra::computeDominators).
using zanna::codegen::ra::computeDominators;

} // namespace zanna::codegen::aarch64::peephole
