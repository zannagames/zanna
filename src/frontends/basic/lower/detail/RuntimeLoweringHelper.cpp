//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE in the project root for license information.
//
// File: src/frontends/basic/lower/detail/RuntimeLoweringHelper.cpp
// Purpose: Implements the internal facade for stateful runtime statement
//          lowering.
// Key invariants: Every operation delegates through the same DetailAccess
//                 instance and preserves Lowerer's symbol/slot bookkeeping.
// Ownership/Lifetime: Borrows Lowerer state; statement nodes remain caller-owned.
// Links: src/frontends/basic/lower/detail/LowererDetail.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements internal forwarding for BASIC runtime statements.

#include "frontends/basic/Lowerer.hpp"
#include "frontends/basic/lower/detail/LowererDetail.hpp"

namespace il::frontends::basic::lower::detail {

/// @brief Creates a runtime-statement helper over a Lowerer facade.
/// @param access Borrowed forwarding facade retained by value.
RuntimeLoweringHelper::RuntimeLoweringHelper(Lowerer::DetailAccess access) noexcept
    : access_(access) {}

/// @brief Forwards assignment lowering.
/// @param stmt LET assignment to lower.
void RuntimeLoweringHelper::lowerLet(const LetStmt &stmt) {
    access_.lowerLet(stmt);
}

/// @brief Forwards constant-declaration lowering.
/// @param stmt CONST declaration to lower.
void RuntimeLoweringHelper::lowerConst(const ConstStmt &stmt) {
    access_.lowerConst(stmt);
}

/// @brief Forwards persistent-local declaration lowering.
/// @param stmt STATIC declaration to lower.
void RuntimeLoweringHelper::lowerStatic(const StaticStmt &stmt) {
    access_.lowerStatic(stmt);
}

/// @brief Forwards variable or array declaration lowering.
/// @param stmt DIM declaration to lower.
void RuntimeLoweringHelper::lowerDim(const DimStmt &stmt) {
    access_.lowerDim(stmt);
}

/// @brief Forwards array-resize lowering.
/// @param stmt REDIM operation to lower.
void RuntimeLoweringHelper::lowerReDim(const ReDimStmt &stmt) {
    access_.lowerReDim(stmt);
}

/// @brief Forwards random-generator seeding.
/// @param stmt RANDOMIZE statement to lower.
void RuntimeLoweringHelper::lowerRandomize(const RandomizeStmt &stmt) {
    access_.lowerRandomize(stmt);
}

/// @brief Forwards lvalue exchange lowering.
/// @param stmt SWAP statement to lower.
void RuntimeLoweringHelper::lowerSwap(const SwapStmt &stmt) {
    access_.lowerSwap(stmt);
}

} // namespace il::frontends::basic::lower::detail
