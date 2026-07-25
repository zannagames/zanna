//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/ISel.hpp
// Purpose: Declare the instruction selection helpers that canonicalise Machine IR.
// Key invariants:
//   - Transformations preserve instruction ordering while rewriting pseudo-ops
//     into concrete x86-64 encodings.
//   - i1 values are materialised via SETcc + MOVZX idioms.
//   - Compare+branch pairs are folded to avoid redundant flag tests.
// Ownership/Lifetime:
//   - The selector mutates Machine IR in-place; no dynamic resources are
//     allocated beyond temporary worklists.
// Links: src/codegen/x86_64/ISel.cpp,
//        src/codegen/x86_64/MachineIR.hpp,
//        src/codegen/x86_64/TargetX64.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "MachineIR.hpp"
#include "TargetX64.hpp"

/**
 * @file
 * @brief Declares x86-64 Machine IR legalization and local instruction selection.
 *
 * The selector converts lowering pseudos and operand combinations into forms
 * accepted by register allocation and the final emitters. It also performs
 * legality-aware local folds that exploit x86-64 addressing modes without
 * changing observable flag or virtual-register behavior.
 */

namespace zanna::codegen::x64 {

/**
 * @brief Canonicalizes lowered Machine IR into concrete x86-64 forms.
 *
 * The selector fixes immediate/register modes for arithmetic, lowers explicit
 * scalar and floating-point select pseudos, simplifies compare/branch chains,
 * materializes byte booleans, and folds eligible address computations into LEA
 * or SIB memory operands. The bound target descriptor is borrowed.
 */
class ISel {
  public:
    /**
     * @brief Construct an instruction selector bound to a target ABI.
     * @param target Borrowed target descriptor that must outlive this instance.
     */
    explicit ISel(const TargetInfo &target) noexcept;

    /**
     * @brief Legalize arithmetic operands and apply address-strength reductions.
     * @param func Machine function rewritten in place.
     * @throws std::exception If temporary-vreg creation or an unsupported form fails.
     */
    void lowerArithmetic(MFunction &func) const;

    /**
     * @brief Legalize compares and simplify boolean conditional-branch chains.
     * @param func Machine function rewritten in place.
     * @throws std::exception If temporary-vreg creation fails.
     */
    void lowerCompareAndBranch(MFunction &func) const;

    /**
     * @brief Expand GPR and XMM select pseudos into legal control/data-flow MIR.
     * @param func Machine function rewritten in place.
     */
    void lowerSelect(MFunction &func) const;

    /**
     * @brief Verify that no explicit or legacy select placeholder survived.
     * @param func Selected function to inspect without modification.
     * @throws std::runtime_error Through @c phaseAUnsupported on a residual pseudo.
     */
    void validateSelectLowering(const MFunction &func) const;

  private:
    /// Borrowed target descriptor; never null after construction.
    const TargetInfo *target_{nullptr};

    /// @brief Fold LEA base addresses into memory operands.
    /// @details Scans all blocks looking for LEA instructions whose result
    /// temporary has exactly one use as a memory operand base. When found,
    /// the LEA's offset is folded directly into the memory operand's
    /// displacement, eliminating the intermediate register.
    /// @param func The machine function to optimize.
    void foldLeaIntoMem(MFunction &func) const;

    /// @brief Fold shift-add patterns into SIB addressing modes.
    /// @details Identifies patterns where an index register is shifted left
    /// and added to a base register, converting them into x86's scaled-index-base
    /// addressing mode (e.g., [base + index*scale + disp]).
    /// @param func The machine function to optimize.
    void foldSibAddressing(MFunction &func) const;

    /// @brief Replace IMULrr-by-small-constant with LEA strength reduction.
    /// @details Detects patterns where a MOVri loads a constant 3, 5, or 9
    /// into a virtual register used exactly once as the second operand of an
    /// IMULrr. Replaces the multiply with a LEA using SIB addressing:
    ///   x*3 → lea [x + x*2], x*5 → lea [x + x*4], x*9 → lea [x + x*8].
    /// The supplying MOVri is erased when the constant register becomes dead.
    /// IMULOvfrr (overflow-checked) is intentionally left untouched.
    /// @param func The machine function to optimize.
    void lowerMulToLea(MFunction &func) const;

    /// @brief Collapse MOVrr+SHLri+MOVrr+ADDrr value chains into a single LEA.
    /// @details `d = x + (y << k)` becomes `LEA d, [x + y * 2^k]` for k in
    /// 1..3 when the shifted temp has no other use and no consumer reads the
    /// ADD's flag write.
    /// @param func The machine function to optimize.
    void synthesizeValueLea(MFunction &func) const;
};

} // namespace zanna::codegen::x64
