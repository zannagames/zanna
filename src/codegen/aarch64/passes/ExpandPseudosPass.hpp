//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/passes/ExpandPseudosPass.hpp
// Purpose: Post-RA pass that rewrites every MIR form the emitters used to
//          expand through a hidden scratch register (wide ALU/compare
//          immediates, non-FP8 FMovRI, out-of-range frame/base/pair/SP
//          offsets) into explicit MIR, so no instruction reaching the emitters
//          needs an emit-time scratch write.
// Key invariants:
//   - Runs last among the MIR passes (after every peephole and the scheduler)
//     so the frame-slot forwarders and the scheduler still see the compact
//     pseudo forms; after it, emitTimeScratchClobber() is false for every
//     instruction and both emitters assert encodability.
//   - The scratch chosen for an expansion is never an operand of the
//     instruction and never a reserved scratch register that is live at that
//     point of the block; exhaustion is an internal compiler error.
//   - Generated sequences mirror the text emitter's historical expansions
//     (same scratch preference order, same instruction shapes).
// Ownership/Lifetime:
//   - Stateless pass; mutates AArch64Module::mir in place.
// Links: src/codegen/aarch64/passes/ExpandPseudosPass.cpp,
//        src/codegen/aarch64/InstrEffects.hpp (encodability predicates),
//        src/codegen/aarch64/AsmEmitter.cpp, binenc/A64BinaryEncoder.cpp,
//        docs/internals/backend-codegen-review-2026-09.md (Phase 2.2 / A4)
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/aarch64/MachineIR.hpp"
#include "codegen/aarch64/passes/PassManager.hpp"

#include <cstddef>

/**
 * @file
 * @brief Declares explicit expansion of emit-time pseudo forms for AArch64 MIR.
 */

namespace zanna::codegen::aarch64 {

/// @brief Expand every emit-time pseudo form in @p fn into explicit MIR.
/// @details See ExpandPseudosPass for the forms handled. Exposed so unit
///          tests and the emitter tests can expand hand-built functions.
/// @param fn Function rewritten in place.
/// @return Number of instructions that were rewritten.
/// @throws Internal compiler error when no scratch register is free for an
///         expansion (every reserved scratch is an operand or live).
std::size_t expandPseudoInstructions(MFunction &fn);

namespace passes {

/**
 * @brief Materializes wide immediates and large offsets as explicit MIR.
 *
 * Forms rewritten (each only when the immediate is not directly encodable):
 * `AddRI`/`SubRI`/`AddsRI`/`SubsRI` (mov xS,#imm; op dst,lhs,xS),
 * `AndRI`/`OrrRI`/`EorRI`, `CmpRI` (mov xS,#imm; cmp lhs,xS), `FMovRI`
 * (mov xS,#bits; fmov dst,xS), `AddFpImm` (mov xS,#off; add dst,x29,xS),
 * frame- and base-relative scalar loads/stores (mov xS,#off; add xS,base,xS;
 * op rt,[xS,#0]), `Ldp`/`Stp` pairs outside imm7 (two scalar accesses), and
 * SP-relative argument stores outside the scaled range (add xS,sp,#0;
 * add xS,xS,#off; str rt,[xS,#0]).
 */
class ExpandPseudosPass final : public Pass {
  public:
    /**
     * @brief Expands every function in the module.
     * @param[in,out] module Module whose MIR is rewritten in place.
     * @param diags Diagnostic sink; receives an error if @p module has no target.
     * @return `true` unless the module is missing its target description.
     */
    bool run(AArch64Module &module, Diagnostics &diags) override;
};

} // namespace passes
} // namespace zanna::codegen::aarch64
