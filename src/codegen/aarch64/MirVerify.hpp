//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/MirVerify.hpp
// Purpose: Structural and dataflow verifier for AArch64 MIR, run between
//          pipeline passes so a pass that breaks an invariant fails with a
//          diagnostic instead of miscompiling silently.
// Key invariants:
//   - Rules are cumulative by stage: everything checked at PostLowering is
//     also checked at PostRA, and so on down the pipeline.
//   - The verifier never mutates MIR and never throws; every violation is
//     reported through Diagnostics with a stable `V-CG-MIR-*` code.
//   - Register semantics come from InstrEffects / ra::operandRoles, the same
//     tables every optimizing pass consumes, so the verifier and the passes
//     cannot disagree about what an instruction reads or writes.
// Ownership/Lifetime:
//   - Stateless free functions; callers own the function, target, and sink.
// Links: src/codegen/aarch64/MirVerify.cpp,
//        src/codegen/aarch64/InstrEffects.hpp,
//        src/codegen/common/PassManager.hpp (post-pass hook),
//        docs/internals/backend-codegen-review-2026-09.md (Phase 2.4)
//
//===----------------------------------------------------------------------===//
#pragma once

#include "codegen/aarch64/MachineIR.hpp"
#include "codegen/aarch64/TargetAArch64.hpp"
#include "codegen/common/Diagnostics.hpp"

#include <cstddef>
#include <string>

/// @file
/// @brief Declares the AArch64 MIR verifier and its pipeline-stage model.

namespace zanna::codegen::aarch64 {

/// @brief Pipeline position whose invariants a verification run enforces.
/// @details Stages are ordered along the pipeline; a later stage enforces
///          every rule of the earlier ones plus its own:
///          - PostLowering: structural rules only (operand roles classify,
///            branch labels resolve, terminator placement, virtual-register
///            class consistency, carried-exit metadata well-formed).
///          - PostRA: no virtual registers remain; reserved scratch registers
///            are never live across an instruction that clobbers them
///            implicitly; the entry block's physical live-in set is a subset
///            of the ABI inputs; frame- and stack-relative offsets lie inside
///            the finalized frame; every callee-saved register written is in
///            the function's save list.
///          - PostExpand: every immediate is directly encodable (no emit-time
///            scratch expansion remains). Enforced once ExpandPseudosPass runs.
///          - PostPeephole / PostSchedule: currently the PostExpand rule set;
///            reserved so later phases can tighten them independently.
enum class VerifyStage : unsigned {
    PostLowering = 0,
    PostRA = 1,
    PostExpand = 2,
    PostPeephole = 3,
    PostSchedule = 4,
};

/// @brief Human-readable stage name used in diagnostics.
/// @param stage Stage to name.
/// @return Static string such as `"post-ra"`.
[[nodiscard]] const char *verifyStageName(VerifyStage stage) noexcept;

/// @brief Verify one MIR function against the invariants of @p stage.
/// @details Reports every violation found (not only the first) as an error
///          diagnostic with code `V-CG-MIR-<RULE>` and a message naming the
///          function, block, and offending instruction. The scan is bounded
///          by a fixed number of reports per function so a badly broken
///          function cannot flood the sink.
/// @param fn Function to verify; not modified.
/// @param stage Pipeline position whose invariants apply.
/// @param target ABI description (argument, return, and callee-saved sets).
/// @param diags Sink receiving one error per violation.
/// @return `true` when no violation was found.
bool verifyMir(const MFunction &fn,
               VerifyStage stage,
               const TargetInfo &target,
               common::Diagnostics &diags);

/// @brief Whether MIR verification was requested through the environment.
/// @details `ZANNA_VERIFY_MIR` set to a non-empty value other than `0` turns
///          the post-pass verifier on for every pipeline in the process. The
///          CLI `--verify-mir` flag is the per-invocation equivalent.
/// @return `true` when the environment requests verification.
[[nodiscard]] bool mirVerificationRequested() noexcept;

} // namespace zanna::codegen::aarch64
