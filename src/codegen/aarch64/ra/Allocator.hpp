//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/ra/Allocator.hpp
// Purpose: Core linear-scan register allocator class for AArch64. Owns all
//          allocation state and drives the per-block, per-instruction
//          allocation loop.
//
// Key invariants:
//   - After run(), every MReg in the MFunction has isPhys=true.
//   - Spill slots are allocated via FrameBuilder; callee-saved usage is
//     recorded for prologue/epilogue generation.
//   - Cross-block register persistence restores canonical spills before
//     re-adopting single-predecessor exit registers.
//
// Ownership/Lifetime:
//   - Constructed per-function; borrows MFunction and TargetInfo references.
//   - Must not outlive the MFunction it modifies.
//
// Design note (spill organization):
//   Spill decisions are intentionally interleaved with the linear-scan loop here rather than
//   factored into a standalone "Spiller" stage like the x86-64 backend (codegen/x86_64/ra/
//   Spiller). Victim selection reads live per-instruction allocator state (next-use distance,
//   live-out, dirty flags, register pools) and inserts reload/store code in place, so coupling
//   avoids threading that mutable state across a stage boundary. The two backends deliberately
//   differ; spill correctness here is covered by the AArch64 spill/pressure tests
//   (Arm64SpillFPR.*, Arm64CrossBlockPhi.*, test_aarch64_frame_spill_reuse) and the VM<->native
//   differential gate, so this is not a missing stage to be "completed".
//
// Links: codegen/aarch64/ra/Allocator.cpp,
//        codegen/aarch64/RegAllocLinear.hpp,
//        codegen/aarch64/ra/Liveness.hpp,
//        codegen/aarch64/ra/RegPools.hpp,
//        codegen/aarch64/ra/VState.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Liveness.hpp"
#include "RegPools.hpp"
#include "VState.hpp"
#include "codegen/aarch64/FrameBuilder.hpp"
#include "codegen/aarch64/MachineIR.hpp"
#include "codegen/aarch64/RegAllocLinear.hpp"
#include "codegen/aarch64/TargetAArch64.hpp"

/// @file
/// @brief Declares the stateful per-function AArch64 linear allocator.

namespace zanna::codegen::aarch64::ra {

/// @brief Per-function linear-scan register allocator for AArch64 MIR.
///
/// Drives the allocation loop over all basic blocks and instructions.
/// Virtual registers are mapped to physical registers or spill slots; the
/// function is rewritten in place with physical operands, spills, and reloads.
/// Callee-saved usage and frame state are recorded directly on the function;
/// the current public `AllocationResult` remains a legacy statistics shape.
class LinearAllocator {
  public:
    /// @brief Construct an allocator for @p fn using target register constraints from @p ti.
    ///
    /// Scans explicit physical operands for ABI live-ins that must be excluded
    /// from allocation, initializes physical pools, and computes the original
    /// function's CFG liveness.
    ///
    /// @param[in,out] fn Machine function that subsequent @ref run mutates.
    /// @param ti Target register sets and calling-convention description.
    /// @pre @p fn and @p ti outlive this allocator.
    LinearAllocator(MFunction &fn, const TargetInfo &ti);

    /// @brief Allocate every block and finalize function frame metadata.
    ///
    /// Optionally pins hot cross-block slots, restores eligible
    /// single-predecessor register state, allocates each instruction, finalizes
    /// spill layout, and publishes used callee-saved registers.
    ///
    /// @return Default allocation statistics; the current implementation
    ///         records all substantive results directly in the function.
    /// @throws std::runtime_error when register or emergency-scratch allocation
    ///         cannot satisfy an instruction.
    /// @post Every virtual register operand in the function is physical.
    AllocationResult run();

  private:
    /// Function being rewritten in place.
    MFunction &fn_;

    /// Borrowed AArch64 ABI and register description.
    const TargetInfo &ti_;

    /// Spill-slot allocator and final frame-layout builder.
    FrameBuilder fb_;

    /// Free physical registers and callee-saved usage flags.
    RegPools pools_;

    /// Allocation state keyed by virtual GPR identifier.
    std::unordered_map<uint16_t, VState> gprStates_;

    /// Allocation state keyed by virtual FPR identifier.
    std::unordered_map<uint16_t, VState> fprStates_;
    unsigned currentInstrIdx_{0};        ///< Current instruction index for LRU tracking.
    std::size_t currentBlockIdx_{0};     ///< Current block index for liveness lookups.
    unsigned currentBlockInstrCount_{0}; ///< Instruction count of the current block.
    std::unordered_map<uint16_t, std::vector<unsigned>>
        usePositionsGPR_; ///< All use positions for GPR vregs.
    std::unordered_map<uint16_t, std::vector<unsigned>>
        usePositionsFPR_;                 ///< All use positions for FPR vregs.
    std::vector<unsigned> callPositions_; ///< Positions of call instructions in current block.
    std::unordered_set<uint16_t>
        protectedOperandGPR_; ///< Current-instruction GPR operands that must not be evicted.
    std::unordered_set<uint16_t>
        protectedOperandFPR_; ///< Current-instruction FPR operands that must not be evicted.
    std::unordered_set<PhysReg>
        protectedPhysGPR_; ///< Explicit physical GPR operands that scratch/spills must not clobber.
    std::unordered_set<PhysReg>
        protectedPhysFPR_; ///< Explicit physical FPR operands that scratch/spills must not clobber.

    // CFG + liveness (extracted to shared-solver-backed LivenessAnalysis).
    /// Original virtual-register CFG liveness.
    LivenessAnalysis liveness_;

    // Cross-block register persistence: exit-state cache.

    /// @brief Snapshot of the vreg→physical register mapping at the end of one basic block,
    ///        used to seed the allocation state at single-predecessor successors.
    struct BlockExitState {
        std::unordered_map<uint16_t, PhysReg> gpr; ///< GPR vreg → physical register map.
        std::unordered_map<uint16_t, PhysReg> fpr; ///< FPR vreg → physical register map.
    };

    /// Exit snapshots indexed by block.
    std::vector<BlockExitState> blockExitStates_;

    /// Set after `rt_arr_obj_get` until its X0-to-vreg result move is spilled.
    bool pendingGetBarrier_{false};

    /// @brief Frame slots pinned to callee-saved registers for the whole
    ///        function (tier 1 of the two-tier allocation scheme).
    /// @details AArch64 lowering routes block params and cross-block temps
    ///          through FP-relative slots (PhiStore + block-entry loads).
    ///          Pinning maps the hottest of those slots onto callee-saved
    ///          registers; every ldr/str against a pinned slot rewrites into a
    ///          register move, which the copy-propagation peephole then folds.
    std::unordered_map<int, PhysReg> pinnedSlotGPR_{};
    std::unordered_map<int, PhysReg> pinnedSlotFPR_{};

    /// @brief Choose and reserve pinned slots (weights from loop depth).
    ///
    /// Pinning is disabled for loops containing calls and for functions using
    /// native exception snapshots. Only written, full-width slot accesses that
    /// are sufficiently hot and never address-taken or accessed by unsupported
    /// forms are candidates.
    void assignPinnedSlots();

    /// @brief Rewrite a pinned-slot ldr/str/PhiStore into a register move.
    /// @param[in,out] ins Candidate FP-relative access.
    /// @return `true` when @p ins referenced a pinned slot and was rewritten.
    bool rewritePinnedSlotAccess(MInstr &ins);

    /// Argument registers removed from the free pools because call marshalling
    /// has already written them; returned to the pools after the next call.
    std::vector<PhysReg> reservedForCall_;

    // ---- Cross-block ----
    /// @brief Restore predecessor spills and seed registers from its exit state.
    /// @param bi Current block index. Restoration is attempted only for a
    ///           previously allocated, unique predecessor.
    void restoreFromPredecessor(std::size_t bi);

    // ---- Physical-register clobbers ----
    /// @brief Evict any vreg resident in a physical register that @p ins defines,
    ///        spilling it first when its value is still needed. Argument registers
    ///        written here are additionally reserved until the next call.
    /// @param[in,out] ins Instruction whose physical definitions may also
    ///                    satisfy same-instruction virtual uses.
    /// @param[out] prefix Spill stores that must precede @p ins.
    void evictPhysDefClobbers(MInstr &ins, std::vector<MInstr> &prefix);

    /// @brief Return argument registers reserved during call marshalling to the pools.
    void releaseCallReserved();

    // ---- Next-use analysis ----
    /// @brief Build per-vreg use-position maps for @p bb to guide eviction decisions.
    /// @param bb Original block instruction sequence to index.
    void computeNextUses(const MBasicBlock &bb);

    /// @brief Return true if @p vreg has a use after the next call in the block.
    /// @param vreg Virtual-register identifier.
    /// @param cls Register class selecting the position map.
    /// @return `true` when a call lies between the current position and the
    ///         virtual register's last later use.
    bool nextUseAfterCall(uint16_t vreg, RegClass cls) const;

    /// @brief Return the instruction-distance to the next use of @p vreg, or UINT_MAX.
    /// @param vreg Virtual-register identifier.
    /// @param cls Register class selecting the position map.
    /// @return Positive distance from the current instruction, or `UINT_MAX`
    ///         when there is no later use in the block.
    unsigned getNextUseDistance(uint16_t vreg, RegClass cls) const;

    /// @brief Compute the spill slot last-use index for @p vreg (used to size spill coverage).
    /// @param vreg Virtual-register identifier.
    /// @param cls Register class selecting use positions and live-out state.
    /// @param forceLiveOut Treat the spill as covering the block exit regardless
    ///                     of computed liveness.
    /// @return Greatest relevant use or block-exit instruction index.
    unsigned computeSpillLastUse(uint16_t vreg, RegClass cls, bool forceLiveOut = false) const;

    /// @brief Allocate or retrieve the current spill slot for @p vreg and return its FP offset.
    /// @param vreg Virtual-register identifier.
    /// @param cls Register class used to determine slot lifetime.
    /// @param forceLiveOut Extend slot coverage through the block exit.
    /// @return Signed frame-pointer-relative spill offset.
    int ensureCurrentSpillSlot(uint16_t vreg, RegClass cls, bool forceLiveOut = false);

    /// @brief Return true if @p vreg appears as an operand in the current instruction.
    /// @param vreg Virtual-register identifier.
    /// @param cls Register class selecting the protected set.
    /// @return Whether pressure eviction must exclude @p vreg.
    [[nodiscard]] bool isProtectedOperand(uint16_t vreg, RegClass cls) const;

    /// @brief Return true if @p phys appears as an explicit physical operand in the current
    /// instruction.
    /// @param phys Physical register to query.
    /// @param cls Register class selecting the protected set.
    /// @return Whether spill scratch and eviction must avoid @p phys.
    [[nodiscard]] bool isProtectedPhys(PhysReg phys, RegClass cls) const;

    /// @brief Return true if @p vreg is live at the exit of the current block.
    /// @param vreg Virtual-register identifier.
    /// @param cls Register class selecting the liveness result.
    /// @return Membership in the current block's class-specific live-out set.
    [[nodiscard]] bool isLiveOut(uint16_t vreg, RegClass cls) const;

    // ---- Spilling ----
    /// @brief Evict @p id from its physical register and emit a store to its spill slot.
    /// @param cls Register class selecting allocator state and store opcode.
    /// @param id Resident virtual-register identifier.
    /// @param[out] prefix Receives a spill store when the value remains needed
    ///                    and its backing slot is stale.
    void spillVictim(RegClass cls, uint16_t id, std::vector<MInstr> &prefix);

    /// @brief Select the LRU-allocated vreg as eviction candidate for @p cls.
    /// @param cls Register class whose resident states are searched.
    /// @return Deterministically selected virtual-register identifier, or
    ///         `UINT16_MAX` when every resident is protected.
    uint16_t selectLRUVictim(RegClass cls);

    /// @brief Select the vreg with the furthest next use as eviction candidate for @p cls.
    /// @param cls Register class whose resident states are searched.
    /// @return Deterministically selected virtual-register identifier, falling
    ///         back to LRU selection when necessary.
    uint16_t selectFurthestVictim(RegClass cls);

    /// @brief Proactively evict a vreg when register pressure exceeds available pools.
    /// @param cls Register class that needs a free pool entry.
    /// @param[out] prefix Receives any required spill store.
    void maybeSpillForPressure(RegClass cls, std::vector<MInstr> &prefix);

    // ---- Register materialization ----
    /// @brief Ensure @p r (a virtual register) is backed by a physical register, inserting
    ///        loads/stores into @p prefix / @p suffix as needed.
    /// @param[in,out] r Register operand converted to physical form.
    /// @param isUse Whether the instruction reads the incoming value.
    /// @param isDef Whether the instruction defines a new value.
    /// @param[out] prefix Instructions emitted before the owning instruction.
    /// @param[out] suffix Instructions emitted after the owning instruction.
    /// @param[in,out] scratch One-instruction temporary registers acquired.
    void materialize(MReg &r,
                     bool isUse,
                     bool isDef,
                     std::vector<MInstr> &prefix,
                     std::vector<MInstr> &suffix,
                     std::vector<PhysReg> &scratch);
    /// @brief Handle a spilled operand: reload from its slot before use, store after def.
    /// @param[in,out] r Virtual register operand converted to its temporary or
    ///                  newly adopted physical register.
    /// @param fprClass Whether @p r belongs to the FPR class.
    /// @param isUse Whether a reload is required.
    /// @param isDef Whether a new value must be preserved.
    /// @param[out] prefix Receives any reload.
    /// @param[out] suffix Receives a one-shot scratch spill after a definition.
    /// @param[in,out] scratch Receives emergency scratch acquisitions.
    void handleSpilledOperand(MReg &r,
                              bool fprClass,
                              bool isUse,
                              bool isDef,
                              std::vector<MInstr> &prefix,
                              std::vector<MInstr> &suffix,
                              std::vector<PhysReg> &scratch);
    /// @brief Assign a fresh physical register to virtual register @p vregId.
    /// @param[in,out] st Virtual-register state receiving residency.
    /// @param vregId Identifier used for next-use/call-crossing preference.
    /// @param fprClass Selects FPR rather than GPR pools.
    /// @throws std::runtime_error if the selected pool is empty.
    void assignNewPhysReg(VState &st, uint16_t vregId, bool fprClass);

    /// @brief Record that @p pr (a callee-saved register) has been used in this function.
    /// @param pr Explicit or allocated physical register to account for.
    void trackCalleeSavedPhys(PhysReg pr);

    /// @brief Return true if @p pr is callee-saved under AAPCS64 for the given @p cls.
    /// @param pr Physical register to query.
    /// @param cls Register class selecting the target saved set.
    /// @return Whether @p pr occurs in the class-specific target set.
    [[nodiscard]] bool isCalleeSaved(PhysReg pr, RegClass cls) const noexcept;

    // ---- Block / instruction allocation ----
    /// @brief Allocate all instructions in @p bb, replacing vregs with physical registers.
    /// @param[in,out] bb Block rewritten with allocation code and published
    ///                   live-through physical registers.
    void allocateBlock(MBasicBlock &bb);

    /// @brief Spill caller-saved physical registers around the call in @p ins.
    /// @param[in,out] ins Call whose explicit operands are materialized.
    /// @param[in,out] rewritten Destination instruction stream receiving spills,
    ///                          the call, and any suffix code.
    void handleCall(MInstr &ins, std::vector<MInstr> &rewritten);

    /// @brief Retire a virtual call operand whose assigned register is clobbered by the call.
    /// @param vreg Virtual call-operand identifier.
    /// @param cls Register class selecting its state and physical pool.
    void retireCallOperandAfterCall(uint16_t vreg, RegClass cls);

    /// @brief Materialize all operands of @p ins and append to @p rewritten.
    /// @param[in,out] ins Non-call instruction rewritten to physical operands.
    /// @param[in,out] rewritten Destination stream receiving prefix, instruction,
    ///                          and suffix code.
    void allocateInstruction(MInstr &ins, std::vector<MInstr> &rewritten);

    // ---- Cleanup ----
    /// @brief Return scratch physical registers acquired during instruction allocation.
    /// @param[in,out] scratch Acquired physical registers; reserved emergency
    ///                        scratch registers are deliberately not pooled.
    void releaseScratch(std::vector<PhysReg> &scratch);

    /// @brief Clear per-block allocation state (vreg→phys maps, call positions, etc.).
    /// @details Returns residents to their FIFO pools in deterministic virtual
    ///          register order while retaining valid spill-state information.
    void releaseBlockState();

    /// @brief Propagate callee-saved register usage into function metadata.
    /// @details Rebuilds the function's saved-GPR and saved-FPR vectors in
    ///          target-defined order.
    void recordCalleeSavedUsage();
};

} // namespace zanna::codegen::aarch64::ra
