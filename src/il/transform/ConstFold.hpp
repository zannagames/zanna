//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/transform/ConstFold.hpp
// Purpose: Declares the constant folding pass -- replaces instructions whose
//          operands are all literal constants with the computed constant result.
//          Covers integer arithmetic, bitwise ops, comparisons, and selected
//          math intrinsics.
// Key invariants:
//   - Only folds when both operands are literals and the operation is safe.
//   - Mutates the module in place; no dataflow analysis is performed.
// Ownership/Lifetime: Free function operating on a caller-owned Module.
// Links: il/core/fwd.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares behavior-preserving IL constant folding.

#pragma once

#include "il/core/fwd.hpp"

namespace il::transform {

/// @brief Fold literal computations and selected deterministic runtime calls.
/// @details Replaces uses of foldable instruction results, removes the original
///          instructions after stable-storage substitution, and leaves any
///          operation that could trap or vary by host math behavior untouched.
/// @param m Module updated in place; all function identifiers are re-interned
///          after compaction.
void constFold(core::Module &m);

} // namespace il::transform
