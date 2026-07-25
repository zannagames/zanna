//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/FpCompareLowering.hpp
// Purpose: Shared AArch64 FCMP result-materialization helpers. Maps each IL
//          floating-point compare opcode to an AArch64 condition code and
//          emits the instruction sequence that lands a 0/1 boolean in a GPR.
// Key invariants:
//   - AArch64 FCMP sets NZCV = 0011 (N=0, Z=0, C=1, V=1) for unordered (NaN)
//     operands. The condition codes chosen here (eq, mi, ls, gt, ge) all
//     evaluate FALSE under that flag state, so ordered predicates need no
//     extra unordered masking — a single CSET is exact. FCmpNE uses `ne`,
//     which is intentionally unordered-TRUE (Z=0), matching IL FCmpNE
//     semantics; FCmpOrd/FCmpUno map directly to `vc`/`vs`.
//   - Helpers are pure with respect to control flow: they only append MInstrs
//     to the supplied MBasicBlock; they never remove or reorder instructions.
//   - The caller supplies the destination vreg. The retained nextVRegId
//     parameter is compatibility state and is not modified.
// Ownership/Lifetime:
//   - Stateless inline free functions; no heap allocation and no retained
//     state. The MBasicBlock and vreg counter are caller-owned.
// Links: src/codegen/aarch64/LoweringContext.hpp,
//        src/codegen/aarch64/MachineIR.hpp, src/il/core/Opcode.hpp,
//        src/codegen/aarch64/InstrLowering.cpp (primary caller)
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Defines the canonical AArch64 condition-code mapping for IL
 *        floating-point comparisons.
 *
 * AArch64 `FCMP` encodes unordered operands in NZCV. The selected primitive
 * conditions already reproduce each IL ordered/unordered predicate, so result
 * materialization requires one `CSET` into the caller-selected GPR vreg.
 */

#pragma once

#include "codegen/aarch64/LoweringContext.hpp"
#include "codegen/aarch64/MachineIR.hpp"
#include "il/core/Opcode.hpp"

namespace zanna::codegen::aarch64 {

/// @brief Return the primitive AArch64 condition used by an FCMP opcode.
/// @details Ordered predicates map to conditions that are false for FCMP's
///          unordered NZCV state. `FCmpNE` is intentionally unordered-true,
///          while `FCmpOrd` and `FCmpUno` test the V flag directly.
/// @param op IL floating-point comparison opcode.
/// @return Pointer to a static AArch64 condition-code spelling. Unsupported
///         opcodes conservatively map to `eq`.
inline const char *fpCondCode(il::core::Opcode op) {
    switch (op) {
        case il::core::Opcode::FCmpEQ:
            return "eq";
        case il::core::Opcode::FCmpNE:
            return "ne";
        case il::core::Opcode::FCmpLT:
            return "mi";
        case il::core::Opcode::FCmpLE:
            return "ls";
        case il::core::Opcode::FCmpGT:
            return "gt";
        case il::core::Opcode::FCmpGE:
            return "ge";
        case il::core::Opcode::FCmpOrd:
            return "vc";
        case il::core::Opcode::FCmpUno:
            return "vs";
        default:
            return "eq";
    }
}

/// @brief Materialize an IL FP-comparison result into a 0/1 GPR vreg.
/// @details After FCMP, unordered operands set NZCV = 0011 (N=0, Z=0, C=1,
///          V=1). Every condition code returned by fpCondCode() for the
///          ordered predicates — eq (Z), mi (N), ls (!C || Z), gt (Z==0 &&
///          N==V), ge (N==V) — evaluates FALSE under that state, so a single
///          CSET captures the exact IL semantics; no unordered masking is
///          needed. `ne` is unordered-true by construction (Z=0), which is
///          the documented IL FCmpNE behaviour, and `vc`/`vs` directly encode
///          FCmpOrd/FCmpUno.
/// @param out Machine block receiving the `Cset` instruction.
/// @param op IL floating-point comparison opcode.
/// @param dst Caller-allocated destination GPR virtual-register id.
/// @param nextVRegId Compatibility reference retained by the lowering API;
///        this helper does not read or modify it.
/// @post Exactly one instruction is appended to @p out.
inline void emitFpCompareResult(MBasicBlock &out,
                                il::core::Opcode op,
                                uint16_t dst,
                                uint16_t &nextVRegId) {
    (void)nextVRegId;
    out.instrs.push_back(MInstr{
        MOpcode::Cset, {MOperand::vregOp(RegClass::GPR, dst), MOperand::condOp(fpCondCode(op))}});
}

} // namespace zanna::codegen::aarch64
