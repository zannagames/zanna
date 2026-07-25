//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/FrameLowering.hpp
// Purpose: Declare utilities responsible for constructing stack frames.
// Key invariants:
//   - Spill slots are addressed off %rbp with negative displacements.
//   - Stack alignment is maintained at 16-byte boundaries for both supported ABIs.
//   - Callee-saved registers are preserved across function calls.
// Ownership/Lifetime:
//   - Callers retain ownership of Machine IR objects; utilities borrow
//     references for mutation and do not persist state.
// Links: src/codegen/x86_64/FrameLowering.cpp,
//        src/codegen/x86_64/MachineIR.hpp,
//        src/codegen/x86_64/TargetX64.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "MachineIR.hpp"
#include "TargetX64.hpp"

#include <vector>

/**
 * @file
 * @brief Declares x86-64 spill-slot layout and ABI frame materialization.
 *
 * Frame lowering converts symbolic RBP-relative slot placeholders into concrete
 * displacements, determines the required callee-saved register area, reserves
 * outgoing call space, and inserts the prologue and every return-site epilogue.
 * Win64 lowering additionally records an abstract unwind plan for COFF emission.
 */

namespace zanna::codegen::x64 {

/// @brief Kind of abstract Win64 unwind operation recorded during frame lowering.
enum class Win64UnwindOpKind : uint8_t {
    /// Save a nonvolatile GPR with a hardware push.
    PushNonVol,
    /// Reserve the fixed stack-frame allocation.
    AllocStack,
    /// Save a nonvolatile GPR into its frame slot.
    SaveNonVol,
    /// Save a nonvolatile XMM register into a 16-byte frame slot.
    SaveXmm128,
};

/**
 * @brief Describes one prologue action needed to encode Win64 unwind metadata.
 *
 * The binary encoder translates this target-independent record into the
 * corresponding UNWIND_CODE entry after instruction offsets are known.
 */
struct Win64UnwindOp {
    Win64UnwindOpKind kind{Win64UnwindOpKind::AllocStack};
    PhysReg reg{PhysReg::RAX};
    uint32_t stackOffset{0}; ///< Offset from final RSP after the prologue.
};

/**
 * @brief Summarizes the finalized stack-frame requirements of one function.
 *
 * Areas are expressed in bytes. Scalar GPR and double-precision XMM spill slots
 * currently consume eight bytes each; full-width nonvolatile XMM saves consume
 * 16-byte aligned slots separately. The structure is populated across call
 * lowering, spill assignment, and prologue/epilogue insertion.
 */
struct FrameInfo {
    int spillAreaGPR{0};                    ///< Total bytes reserved for GPR spills.
    int spillAreaXMM{0};                    ///< Total bytes reserved for XMM spills.
    int outgoingArgArea{0};                 ///< Bytes reserved for stack-based call arguments.
    int frameSize{0};                       ///< Total size of the frame below %rbp.
    std::vector<PhysReg> usedCalleeSaved{}; ///< Callee-saved registers touched by the function.
    bool prologueEmitted{false};            ///< True when frame lowering inserted setup/teardown.
    bool usesChkstk{false}; ///< True when the Windows large-frame probe helper is used.
    std::vector<Win64UnwindOp> win64UnwindOps{}; ///< Win64 unwind plan for native COFF emission.
};

/**
 * @brief Insert ABI-compliant setup and teardown at every function exit.
 *
 * Empty leaf frames can be elided. Non-leaf frames establish RBP, probe large
 * allocations, preserve used nonvolatile registers, and initialize runtime
 * stack safety for @c main. Win64 operations are mirrored into @p frame's
 * unwind plan.
 *
 * @param func Function whose entry and RET sites are rewritten in place.
 * @param target ABI description, including shadow-space policy.
 * @param frame Finalized frame layout and output unwind metadata.
 * @throws std::runtime_error If a Win64 save offset is not representable.
 */
void insertPrologueEpilogue(MFunction &func, const TargetInfo &target, FrameInfo &frame);

/**
 * @brief Resolve symbolic stack slots and compute the complete frame layout.
 *
 * The pass inventories stack placeholders and physical nonvolatile registers,
 * lays out callee saves, allocas, GPR spills, XMM spills, and outgoing arguments
 * in that order, then rewrites each recognized placeholder displacement.
 *
 * @param func Function to scan and rewrite.
 * @param target ABI register sets, shadow-space size, and ordering policy.
 * @param frame Frame summary updated with byte counts and used registers.
 * @throws std::exception If a placeholder, byte count, or displacement cannot
 *         be represented safely.
 */
void assignSpillSlots(MFunction &func, const TargetInfo &target, FrameInfo &frame);

} // namespace zanna::codegen::x64
