//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/transform/Mem2Reg.hpp
// Purpose: Memory-to-register promotion pass -- transforms non-escaping
//          primitive-typed allocas into SSA temporaries using sealed SSA
//          construction with block parameters at join points. Eliminates
//          redundant load/store pairs.
// Key invariants:
//   - Only allocas of primitive types (i1, i16, i32, i64, f64) are promoted.
//   - Allocas whose addresses escape (passed to calls, stored to memory) are
//     left as memory operations.
//   - SSA form is maintained via block parameters at control flow join points.
// Ownership/Lifetime: Free function operating on a caller-owned Module.
//          Optional Mem2RegStats pointer is borrowed.
// Links: il/core/Module.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "il/core/Module.hpp"

namespace zanna::passes {

/// @brief Statistics collected during memory-to-register promotion.
/// @details Tracks the number of allocas promoted to SSA temporaries and
///          the number of load/store instructions eliminated as a result.
struct Mem2RegStats {
    unsigned promotedVars{0};  ///< Number of allocas promoted to SSA form.
    unsigned removedLoads{0};  ///< Number of load instructions eliminated.
    unsigned removedStores{0}; ///< Number of store instructions eliminated.
};

/// @brief Promote simple allocas to SSA form.
/// @param M Module whose functions are transformed in place.
/// @param stats Optional borrowed accumulator for successful promotions and removals.
/// @param enableParallel Allow independent function-level workers. Defaults to
///        false so standalone calls follow the pass manager's deterministic policy.
/// @details Runs bounded scalar replacement before sealed-SSA construction,
///          skips exception-handling functions, and restores a function snapshot
///          if branch-argument repair cannot complete safely.
void mem2reg(il::core::Module &M, Mem2RegStats *stats = nullptr, bool enableParallel = false);

} // namespace zanna::passes
