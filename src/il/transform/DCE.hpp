//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/transform/DCE.hpp
// Purpose: Declares the dead code elimination pass -- removes instructions
//          and block parameters whose results are never used, using backward
//          dataflow liveness analysis. Preserves side-effectful operations.
// Key invariants:
//   - Instructions with side effects (stores, calls, terminators) are never
//     removed.
//   - SSA form and CFG integrity are maintained after elimination.
// Ownership/Lifetime: Free function operating on a caller-owned Module.
// Links: il/core/fwd.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares module-level dead code and unused block-parameter elimination.

#pragma once

#include "il/core/fwd.hpp"

namespace il::transform {

/// @brief Eliminate dead instructions, stack traffic, and block parameters.
/// @details Preserves trapping and effectful operations, removes unused pure
///          calls and provably nontrapping memory operations, and compacts
///          predecessor branch arguments alongside eliminated block parameters.
/// @param M Module simplified in place and re-interned afterward.
void dce(il::core::Module &M);

} // namespace il::transform
