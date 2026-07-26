//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE in the project root for license information.
//
// File: src/frontends/basic/lower/detail/ControlLoweringHelper.cpp
// Purpose: Implements the internal control-flow forwarding facade.
// Key invariants: Each operation delegates to the same Lowerer represented by
//                 DetailAccess and preserves the callee's CFG contracts.
// Ownership/Lifetime: Borrows Lowerer state; statement nodes remain caller-owned.
// Links: src/frontends/basic/lower/detail/LowererDetail.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements internal forwarding for BASIC control-flow lowering.

#include "frontends/basic/Lowerer.hpp"
#include "frontends/basic/lower/detail/LowererDetail.hpp"

namespace il::frontends::basic::lower::detail {

/// @brief Creates a control-flow helper over an authorized Lowerer facade.
/// @param access Borrowed forwarding facade retained by value.
ControlLoweringHelper::ControlLoweringHelper(Lowerer::DetailAccess access) noexcept
    : access_(access) {}

/// @copydoc ControlLoweringHelper::lowerIf()
void ControlLoweringHelper::lowerIf(const IfStmt &stmt) {
    access_.lowerIf(stmt);
}

/// @copydoc ControlLoweringHelper::lowerWhile()
void ControlLoweringHelper::lowerWhile(const WhileStmt &stmt) {
    access_.lowerWhile(stmt);
}

/// @copydoc ControlLoweringHelper::lowerDo()
void ControlLoweringHelper::lowerDo(const DoStmt &stmt) {
    access_.lowerDo(stmt);
}

/// @copydoc ControlLoweringHelper::lowerFor()
void ControlLoweringHelper::lowerFor(const ForStmt &stmt) {
    access_.lowerFor(stmt);
}

/// @copydoc ControlLoweringHelper::lowerForEach()
void ControlLoweringHelper::lowerForEach(const ForEachStmt &stmt) {
    access_.lowerForEach(stmt);
}

/// @copydoc ControlLoweringHelper::lowerSelectCase()
void ControlLoweringHelper::lowerSelectCase(const SelectCaseStmt &stmt) {
    access_.lowerSelectCase(stmt);
}

/// @copydoc ControlLoweringHelper::lowerNext()
void ControlLoweringHelper::lowerNext(const NextStmt &stmt) {
    access_.lowerNext(stmt);
}

/// @copydoc ControlLoweringHelper::lowerExit()
void ControlLoweringHelper::lowerExit(const ExitStmt &stmt) {
    access_.lowerExit(stmt);
}

/// @copydoc ControlLoweringHelper::lowerGoto()
void ControlLoweringHelper::lowerGoto(const GotoStmt &stmt) {
    access_.lowerGoto(stmt);
}

/// @copydoc ControlLoweringHelper::lowerGosub()
void ControlLoweringHelper::lowerGosub(const GosubStmt &stmt) {
    access_.lowerGosub(stmt);
}

/// @copydoc ControlLoweringHelper::lowerGosubReturn()
void ControlLoweringHelper::lowerGosubReturn(const ReturnStmt &stmt) {
    access_.lowerGosubReturn(stmt);
}

/// @copydoc ControlLoweringHelper::lowerOnErrorGoto()
void ControlLoweringHelper::lowerOnErrorGoto(const OnErrorGoto &stmt) {
    access_.lowerOnErrorGoto(stmt);
}

/// @copydoc ControlLoweringHelper::lowerResume()
void ControlLoweringHelper::lowerResume(const Resume &stmt) {
    access_.lowerResume(stmt);
}

/// @copydoc ControlLoweringHelper::lowerEnd()
void ControlLoweringHelper::lowerEnd(const EndStmt &stmt) {
    access_.lowerEnd(stmt);
}

/// @copydoc ControlLoweringHelper::lowerTryCatch()
void ControlLoweringHelper::lowerTryCatch(const TryCatchStmt &stmt) {
    access_.lowerTryCatch(stmt);
}

} // namespace il::frontends::basic::lower::detail
