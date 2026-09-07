//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/MirVerify.hpp
// Purpose: Structural and dataflow verifier for x86-64 MIR, run between
//          pipeline passes so a pass that breaks an invariant fails with a
//          diagnostic instead of miscompiling silently.
// Key invariants:
//   - Rules are cumulative by stage in pipeline order.
//   - The verifier never mutates MIR and never throws; every violation is an
//     error diagnostic with a stable `V-CG-MIR-*` code.
//   - Register semantics come from effectsOf()/operandRoles(), the same
//     tables the optimizing passes consume.
// Ownership/Lifetime:
//   - Stateless free functions; callers own the function, frame, target, sink.
// Links: src/codegen/x86_64/MirVerify.cpp,
//        src/codegen/x86_64/OperandRoles.hpp,
//        src/codegen/x86_64/FrameLowering.hpp,
//        docs/internals/backend-codegen-review-2026-09.md (Phase 2.4)
//
//===----------------------------------------------------------------------===//
#pragma once

#include "codegen/common/Diagnostics.hpp"
#include "codegen/x86_64/FrameLowering.hpp"
#include "codegen/x86_64/MachineIR.hpp"
#include "codegen/x86_64/TargetX64.hpp"

/// @file
/// @brief Declares the x86-64 MIR verifier and its pipeline-stage model.

namespace zanna::codegen::x64 {

/// @brief Pipeline position whose invariants a verification run enforces.
/// @details Ordered along the x86-64 pipeline (legalize → RA → schedule →
///          peephole). A later stage enforces every earlier rule plus its own:
///          - PostLegalize: structural rules (labels resolve, no in-block LABEL
///            pseudo, terminator placement, register classes consistent,
///            memory operands well-formed).
///          - PostRA: no virtual registers; frame-relative displacements lie
///            inside the finalized frame; reserved scratch (R10/R11) is never
///            live across an implicit clobber; the entry live-in set is a
///            subset of the ABI inputs; callee-saved registers written by the
///            body are in the frame's save list.
///          - PostSchedule / PostPeephole: currently the PostRA rule set.
enum class VerifyStage : unsigned {
    PostLegalize = 0,
    PostRA = 1,
    PostSchedule = 2,
    PostPeephole = 3,
};

/// @brief Human-readable stage name used in diagnostics.
[[nodiscard]] const char *verifyStageName(VerifyStage stage) noexcept;

/// @brief Verify one MIR function against the invariants of @p stage.
/// @param fn Function to verify; not modified.
/// @param frame Frame summary parallel to @p fn (consulted from PostRA on).
/// @param stage Pipeline position whose invariants apply.
/// @param target ABI description.
/// @param diags Sink receiving one error per violation (capped per function).
/// @return `true` when no violation was found.
bool verifyMir(const MFunction &fn,
               const FrameInfo &frame,
               VerifyStage stage,
               const TargetInfo &target,
               common::Diagnostics &diags);

/// @brief Whether MIR verification was requested through `ZANNA_VERIFY_MIR`.
[[nodiscard]] bool mirVerificationRequested() noexcept;

} // namespace zanna::codegen::x64
