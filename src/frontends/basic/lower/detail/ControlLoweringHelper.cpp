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

/// @brief Lower If.
void ControlLoweringHelper::lowerIf(const IfStmt &stmt) {
    access_.lowerIf(stmt);
}

/// @brief Lower While.
void ControlLoweringHelper::lowerWhile(const WhileStmt &stmt) {
    access_.lowerWhile(stmt);
}

/// @brief Lower Do.
void ControlLoweringHelper::lowerDo(const DoStmt &stmt) {
    access_.lowerDo(stmt);
}

/// @brief Lower For.
void ControlLoweringHelper::lowerFor(const ForStmt &stmt) {
    access_.lowerFor(stmt);
}

/// @brief Lower For Each.
void ControlLoweringHelper::lowerForEach(const ForEachStmt &stmt) {
    access_.lowerForEach(stmt);
}

/// @brief Lower Select Case.
void ControlLoweringHelper::lowerSelectCase(const SelectCaseStmt &stmt) {
    access_.lowerSelectCase(stmt);
}

/// @brief Lower Next.
void ControlLoweringHelper::lowerNext(const NextStmt &stmt) {
    access_.lowerNext(stmt);
}

/// @brief Lower Exit.
void ControlLoweringHelper::lowerExit(const ExitStmt &stmt) {
    access_.lowerExit(stmt);
}

/// @brief Lower Goto.
void ControlLoweringHelper::lowerGoto(const GotoStmt &stmt) {
    access_.lowerGoto(stmt);
}

/// @brief Lower Gosub.
void ControlLoweringHelper::lowerGosub(const GosubStmt &stmt) {
    access_.lowerGosub(stmt);
}

/// @brief Lower Gosub Return.
void ControlLoweringHelper::lowerGosubReturn(const ReturnStmt &stmt) {
    access_.lowerGosubReturn(stmt);
}

/// @brief Lower On Error Goto.
void ControlLoweringHelper::lowerOnErrorGoto(const OnErrorGoto &stmt) {
    access_.lowerOnErrorGoto(stmt);
}

/// @brief Lower Resume.
void ControlLoweringHelper::lowerResume(const Resume &stmt) {
    access_.lowerResume(stmt);
}

/// @brief Lower End.
void ControlLoweringHelper::lowerEnd(const EndStmt &stmt) {
    access_.lowerEnd(stmt);
}

/// @brief Lower Try Catch.
void ControlLoweringHelper::lowerTryCatch(const TryCatchStmt &stmt) {
    access_.lowerTryCatch(stmt);
}

} // namespace il::frontends::basic::lower::detail
