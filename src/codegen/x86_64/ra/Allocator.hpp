//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/ra/Allocator.hpp
// Purpose: Declare the linear-scan allocator phase that orchestrates register
//          assignment, spill insertion, and PX_COPY lowering across Machine IR
//          blocks.
// Key invariants:
//   - Allocation proceeds in block order using deterministic register pools.
//   - Physical argument registers are reserved during call setup to prevent
//     spill reloads from overwriting them.
// Ownership/Lifetime:
//   - Mutates the provided MFunction in place; caller owns the function.
//   - LiveIntervals reference must outlive the allocator.
// Links: src/codegen/x86_64/ra/Allocator.cpp,
//        src/codegen/x86_64/RegAllocLinear.hpp,
//        src/codegen/x86_64/ra/Spiller.hpp,
//        src/codegen/x86_64/ra/Liveness.hpp,
//        src/codegen/x86_64/ra/LiveIntervals.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "../MachineIR.hpp"
#include "../RegAllocLinear.hpp"
#include "../TargetX64.hpp"
#include "LiveIntervals.hpp"
#include "Liveness.hpp"
#include "Spiller.hpp"

#include <bitset>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/// @file
/// @brief Declares the x86-64 linear-scan allocator and per-vreg state.

namespace zanna::codegen::x64::ra {

class Coalescer;

/// @brief Allocation state for a single virtual register.
struct VirtualAllocation {
    /// @brief Whether the allocator has observed this virtual id.
    bool seen{false};
    /// @brief Register bank required by every occurrence of the virtual id.
    RegClass cls{RegClass::GPR};
    /// @brief Whether @c phys currently holds the value.
    bool hasPhys{false};
    /// @brief Current physical register when @c hasPhys is true.
    PhysReg phys{PhysReg::RAX};
    /// @brief Spill-home assignment and slot-reuse metadata.
    SpillPlan spill{};
    /// @brief Whether a spilled cross-block value is already reloaded in this block.
    bool cachedInBlock{
        false}; ///< True when a cross-block vreg has been loaded into a register this block.
    /// True when the vreg is pinned to one physical register for its whole
    /// lifetime by the global pre-pass. Pinned vregs never enter the active
    /// sets, never spill, and never release their register; operand rewriting
    /// substitutes the pinned register directly.
    bool pinnedGlobal{false};
    /// Cached LiveIntervals lookup. Intervals are immutable during allocation,
    /// so the per-instruction expiry scan can reuse one lookup per vreg
    /// instead of re-hashing on every instruction.
    const LiveInterval *interval{nullptr};
    /// @brief Whether @c interval has been looked up, including a null result.
    bool intervalCached{false};
};

/// @brief Core linear-scan allocator working over Machine IR.
/// @details Combines CFG liveness, immutable intervals, global callee-saved
///          pinning, local pool allocation, spill-slot management, call
///          preservation, and parallel-copy lowering. The bound function is
///          rewritten in place and all analysis references are borrowed.
class LinearScanAllocator {
  public:
    /// @brief Construct a linear-scan allocator for the supplied Machine IR function.
    /// @details Captures references to the function, target description, and precomputed live
    ///          intervals so that the allocator can walk instructions in block order without
    ///          recomputing analysis.  Ownership of the referenced data remains with the caller.
    LinearScanAllocator(MFunction &func, const TargetInfo &target, const LiveIntervals &intervals);

    /// @brief Execute the allocation algorithm over the bound Machine IR function.
    /// @details Initialises register pools, walks each basic block in layout order, assigns
    ///          physical registers or spill slots as required, and records the resulting mapping
    ///          inside @ref AllocationResult for downstream passes.
    /// @return Final virtual-to-physical map and GPR/XMM spill-slot counts.
    /// @throws std::runtime_error If register classes conflict or pressure
    ///         cannot be relieved by spilling.
    [[nodiscard]] AllocationResult run();

  private:
    friend class Coalescer;

    /// @brief Read/write role of one instruction operand.
    struct OperandRole {
        /// @brief Whether the current operand value is consumed.
        bool isUse{false};
        /// @brief Whether the instruction produces a value in the operand.
        bool isDef{false};
    };

    /// @brief Scratch physical register to return after one rewritten instruction.
    struct ScratchRelease {
        /// @brief Temporary register to release.
        PhysReg phys{PhysReg::RAX};
        /// @brief Pool to which @c phys belongs.
        RegClass cls{RegClass::GPR};
    };

    /// @brief Borrowed function rewritten during @ref run.
    MFunction &func_;
    /// @brief Borrowed immutable ABI register description.
    const TargetInfo &target_;
    /// @brief Borrowed immutable intervals for @c func_.
    const LiveIntervals &intervals_;
    /// @brief CFG-aware liveness computed at the start of allocation.
    LivenessAnalysis liveness_;
    /// @brief Result map and final spill counts returned to the caller.
    AllocationResult result_{};
    /// @brief Spill-slot allocator and load/store instruction factory.
    Spiller spiller_{};
    /// @brief Index of the block currently being rewritten.
    std::size_t currentBlockIdx_{0};

    /// @brief Per-virtual-register allocation records keyed by id.
    std::unordered_map<uint16_t, VirtualAllocation> states_{};
    /// @brief Deterministically ordered available GPR pool.
    std::deque<PhysReg> freeGPR_{}; ///< O(1) pop_front for register allocation.
    /// @brief Deterministically ordered available XMM pool.
    std::deque<PhysReg> freeXMM_{}; ///< O(1) pop_front for register allocation.
    /// @brief Active virtual registers in GPR class. Uses unordered_set for O(1) insert/erase.
    std::unordered_set<uint16_t> activeGPR_{};
    /// @brief Active virtual registers in XMM class. Uses unordered_set for O(1) insert/erase.
    std::unordered_set<uint16_t> activeXMM_{};
    /// @brief Vregs protected from eviction while rewriting the current instruction.
    std::unordered_set<uint16_t>
        pinnedForInstr_{}; ///< Vregs materialized or referenced by the current instruction.
    /// @brief Linear instruction position used by interval expiry.
    std::size_t currentInstrIdx_{0}; ///< Current instruction index for liveness checks.
    /// @brief Vregs requiring stable spill homes at non-carryable CFG boundaries.
    std::unordered_set<uint16_t>
        crossBlockSpillVRegs_{}; ///< Vregs that cross a non-carryable CFG boundary.

    /// @brief Whole-lifetime register pins chosen by the global pre-pass.
    std::unordered_map<uint16_t, PhysReg> pinnedGlobals_{};

    /// @brief Per-block flag: block reads no virtual registers (trap stubs).
    /// @details Such blocks are transparent for register carrying: an edge
    ///          into them cannot observe a stale carried value.
    std::vector<char> blockReadsNoVRegs_{};

    /// @brief Argument registers reserved during call setup, with their class.
    struct ReservedReg {
        /// @brief Reserved argument register.
        PhysReg phys{PhysReg::RAX};
        /// @brief Pool from which the register was removed.
        RegClass cls{RegClass::GPR};
    };

    /// @brief Free argument registers withheld until the pending call is processed.
    std::vector<ReservedReg> reservedForCall_{}; ///< Arg registers reserved during call setup.

    /// @brief Precomputed bitset of caller-saved GPR registers for O(1) lookup.
    /// @details Indexed by static_cast<int>(PhysReg). Avoids linear search in CALL handling.
    std::bitset<32> callerSavedGPRBits_{};

    /// @brief Precomputed bitset of caller-saved FPR registers for O(1) lookup.
    std::bitset<32> callerSavedFPRBits_{};

    /// @brief Populate the free-register pools from the target description.
    /// @details Queries the @ref TargetInfo to enumerate allocatable registers for each class
    ///          and seeds the internal free lists that drive linear-scan allocation.
    void buildPools();

    /// @brief Retrieve the free list associated with a register class.
    /// @details Returns either the general-purpose or floating-point pool so helpers can push
    ///          and pop physical registers while allocating or releasing values.
    /// @param cls Register bank whose pool is required.
    /// @return Mutable deque of currently available physical registers.
    [[nodiscard]] std::deque<PhysReg> &poolFor(RegClass cls);

    /// @brief Retrieve the currently active interval set for a register class.
    /// @details Provides access to the set that tracks active virtual register identifiers so
    ///          expiration checks can scan the appropriate list.
    /// @param cls Register bank whose active set is required.
    /// @return Mutable set of virtual-register ids currently holding registers.
    [[nodiscard]] std::unordered_set<uint16_t> &activeFor(RegClass cls);

    /// @brief Lookup or initialise allocation state for a virtual register.
    /// @details Fetches the @ref VirtualAllocation record keyed by @p id, initialising defaults
    ///          when the register is observed for the first time so state mutations remain
    ///          centralised.
    /// @param cls Register class required by this occurrence.
    /// @param id Virtual-register identifier.
    /// @return Mutable persistent allocation record.
    /// @throws std::runtime_error If a previously seen id is reused in another class.
    [[nodiscard]] VirtualAllocation &stateFor(RegClass cls, uint16_t id);

    /// @brief Look up (once) and cache the live interval for @p vreg on its state.
    /// @param vreg Virtual-register identifier to query.
    /// @param state Matching allocation record receiving the cached pointer.
    /// @return Stable interval pointer, or @c nullptr when analysis has no interval.
    const LiveInterval *cachedInterval(uint16_t vreg, VirtualAllocation &state);

    /// @brief Mark a virtual register as active within its class list.
    /// @details Inserts the identifier into the appropriate active set so future spill decisions
    ///          can consider its live interval ordering.
    /// @param cls Register bank owning the active value.
    /// @param id Virtual-register identifier to insert.
    void addActive(RegClass cls, uint16_t id);

    /// @brief Remove a virtual register from the active set once it expires.
    /// @details Updates bookkeeping so the allocator knows the associated physical register can
    ///          be released back to the free pool.
    /// @param cls Register bank owning the active value.
    /// @param id Virtual-register identifier to erase.
    void removeActive(RegClass cls, uint16_t id);

    /// @brief Acquire a physical register for the next live virtual value.
    /// @details Removes a register from the free pool when available; otherwise triggers a spill
    ///          via @ref spillOne to free space before returning the assigned physical register.
    /// @param cls Register bank from which to allocate.
    /// @param prefix Instruction vector receiving a victim spill store.
    /// @return Physical register removed from the front of the free pool.
    /// @throws std::runtime_error If no register can be freed.
    [[nodiscard]] PhysReg takeRegister(RegClass cls, std::vector<MInstr> &prefix);

    /// @brief Return a physical register to the free pool once it becomes idle.
    /// @details Pushes @p phys back into the free list for @p cls so subsequent allocations can
    ///          reuse it.
    /// @param phys Register becoming available.
    /// @param cls Register bank receiving @p phys.
    /// @throws std::runtime_error If the pool already contains @p phys.
    void releaseRegister(PhysReg phys, RegClass cls);

    /// @brief Release registers for vregs whose live intervals have ended.
    /// @details At each instruction, checks all active vregs and releases those whose
    ///          interval ends at or before the current instruction.
    void expireIntervals();

    /// @brief Spill the active interval with the furthest end point.
    /// @details Selects a victim live range in @p cls, emits the necessary spill code into
    ///          @p prefix, updates state bookkeeping, and frees the associated physical register
    ///          for reuse.
    /// @param cls Register bank under pressure.
    /// @param prefix Instruction vector receiving the victim store.
    /// @return @c true when a register was made available in the free pool.
    [[nodiscard]] bool spillOne(RegClass cls, std::vector<MInstr> &prefix);

    /// @brief Decide whether a spilled operand can be reloaded as scratch-only.
    /// @details Use-only operands whose live interval ends at the current instruction and that
    ///          are not live-out of the current block do not need to be cached in allocator
    ///          state. Reloading them as scratch avoids extending a dead spilled value's active
    ///          lifetime and gives the existing scratch-release path a real purpose.
    /// @param vreg Virtual register being rewritten.
    /// @param role Classified use/def role for the current operand.
    /// @return True when the reload can be released immediately after the instruction.
    [[nodiscard]] bool canUseScratchReload(uint16_t vreg, const OperandRole &role) const;

    /// @brief Spill an active virtual register, reusing stack slots when safe.
    /// @details Cross-block values receive dedicated spill homes, while
    ///          block-local values can reuse disjoint spill slots based on their
    ///          live intervals.
    /// @param vreg Active virtual register to spill.
    /// @param state Matching allocation record updated in place.
    /// @param out Instruction vector receiving the spill store.
    void spillActiveValue(uint16_t vreg, VirtualAllocation &state, std::vector<MInstr> &out);

    /// @brief Allocate registers for every instruction in the provided block.
    /// @details Walks the block's instructions in order, handles live range transitions, and
    ///          cooperates with the coalescer to apply PX_COPY eliminations.
    /// @param block Machine block rewritten in place.
    /// @param coalescer Parallel-copy helper sharing allocator state.
    void processBlock(MBasicBlock &block, Coalescer &coalescer);

    /// @brief Record vregs referenced by the current instruction so they cannot be spilled
    /// mid-rewrite.
    /// @param instr Instruction whose register and memory-address operands are inspected.
    void pinInstructionVRegs(const MInstr &instr);

    /// @brief Predicate: is @p reg caller-saved in register class @p cls?
    /// @param reg Physical register to classify.
    /// @param cls Register bank selecting the precomputed ABI bitset.
    /// @return @c true when the ABI marks @p reg volatile in @p cls.
    [[nodiscard]] bool isCallerSaved(PhysReg reg, RegClass cls) const noexcept;

    /// @brief Pop and return the first free callee-saved register for @p cls.
    /// @param cls Register bank whose free pool is searched.
    /// @return {true, reg} if a callee-saved register is available, {false, _} otherwise.
    [[nodiscard]] std::pair<bool, PhysReg> takeFreeCalleeSaved(RegClass cls);

    /// @brief Collect vregs in @p cls whose live values would be clobbered by a CALL.
    /// @details A vreg is included if it is currently in a caller-saved register and
    ///          (a) it is live-out of the current block, or (b) its interval extends
    ///          past the CALL, or (c) interval info is missing (conservative).
    /// @param cls Register bank whose active set is scanned.
    /// @return Candidate virtual-register ids in active-set iteration order.
    [[nodiscard]] std::vector<uint16_t> collectCallerSavedToSpill(RegClass cls) const;

    /// @brief Re-home or spill caller-saved values for one register class across a CALL.
    /// @details For each candidate vreg: first try to move it into a free callee-saved
    ///          register (a register-to-register copy emitted into @p prefix). If no
    ///          callee-saved register is available, spill the value to memory and remove
    ///          it from the active set.
    /// @param cls Register bank of every candidate.
    /// @param candidates Active virtual-register ids to preserve.
    /// @param prefix Instruction vector receiving moves and spill stores before the call.
    void spillOrRehomeAcrossCall(RegClass cls,
                                 const std::vector<uint16_t> &candidates,
                                 std::vector<MInstr> &prefix);

    /// @brief Release or spill registers at block boundaries using CFG-aware liveOut.
    /// @details Carries active values across a safe linear edge. At other
    ///          boundaries, ensures live-out values have spill homes and
    ///          releases all locally held registers.
    /// @param block The block that was just processed.
    /// @param blockIdx Index of the block (for liveOut lookup).
    void releaseActiveForBlock(MBasicBlock &block, std::size_t blockIdx);

    /// @brief Pin hot cross-block vregs to callee-saved registers for their
    ///        whole lifetime (tier 1 of the two-tier allocation scheme).
    /// @details Candidates are the vregs the cross-block pre-pass would give
    ///          memory homes. Each pinned vreg holds one callee-saved register
    ///          everywhere, so no block-boundary spills, reloads, or edge
    ///          resolution are needed; the pinned registers are removed from
    ///          the local free pools. The current implementation considers
    ///          GPR candidates only.
    void assignPinnedGlobals();

    /// @brief Check whether the next block can reuse live registers directly.
    /// @details Carrying values in registers across block boundaries is only safe
    ///          when the next layout block is a single-predecessor successor.
    ///          Additional successors are permitted only when they read no
    ///          virtual registers, such as terminating trap stubs.
    /// @param blockIdx Index of the current block in layout order.
    /// @return @c true when live registers can flow directly into the next block.
    [[nodiscard]] bool canCarryIntoNextBlock(std::size_t blockIdx) const;

    /// @brief Classify each operand of an instruction as a use, def, or both.
    /// @details Produces a parallel vector describing operand roles so subsequent handling can
    ///          decide whether to allocate, retain, or release registers around the instruction.
    /// @param instr Instruction whose operands are classified.
    /// @return Role vector parallel to @c instr.operands.
    [[nodiscard]] std::vector<OperandRole> classifyOperands(const MInstr &instr) const;

    /// @brief Handle an instruction operand, inserting spills or reloads as required.
    /// @details Depending on @p role, materialises register operands, schedules spill code into
    ///          @p prefix or @p suffix, and records scratch registers that must be released after
    ///          the instruction executes.
    /// @param operand Operand variant rewritten in place.
    /// @param role Use/definition role of @p operand.
    /// @param prefix Instructions inserted before the current instruction.
    /// @param suffix Instructions inserted after the current instruction.
    /// @param scratch One-instruction temporary registers to release afterward.
    void handleOperand(Operand &operand,
                       const OperandRole &role,
                       std::vector<MInstr> &prefix,
                       std::vector<MInstr> &suffix,
                       std::vector<ScratchRelease> &scratch);

    /// @brief Handle a register operand specifically, mapping it to a physical register.
    /// @details Applies the same logic as @ref handleOperand but operates directly on the
    ///          strongly typed @ref OpReg, updating allocation state and scheduling required
    ///          moves.
    /// @param reg Register operand rewritten in place.
    /// @param role Use/definition role of @p reg.
    /// @param prefix Instructions inserted before the current instruction.
    /// @param suffix Instructions inserted after the current instruction.
    /// @param scratch One-instruction temporary registers to release afterward.
    /// @throws std::runtime_error If allocation state conflicts or no register can be freed.
    void processRegOperand(OpReg &reg,
                           const OperandRole &role,
                           std::vector<MInstr> &prefix,
                           std::vector<MInstr> &suffix,
                           std::vector<ScratchRelease> &scratch);

    /// @brief Construct a MOV instruction that copies between physical registers.
    /// @details Used when spilling or resolving PX_COPY nodes; the helper populates opcode and
    ///          operand fields according to the register class being moved.
    /// @param cls Register class selecting @c MOVrr or @c MOVSDrr.
    /// @param dst Physical destination register.
    /// @param src Physical source register.
    /// @return Newly constructed move instruction.
    [[nodiscard]] MInstr makeMove(RegClass cls, PhysReg dst, PhysReg src) const;

    /// @brief Check if a physical register is an argument register for the current ABI.
    /// @param reg Physical register to test against meaningful ABI argument-order entries.
    /// @return @c true when @p reg carries an integer or floating-point argument.
    [[nodiscard]] bool isArgumentRegister(PhysReg reg) const;

    /// @brief Reserve an argument register during call setup to prevent spill reloads from using
    /// it.
    /// @param reg Argument register to remove from its free pool when available.
    void reserveForCall(PhysReg reg);

    /// @brief Release all reserved argument registers back to the pool after a CALL.
    /// @throws std::runtime_error If reservation bookkeeping would duplicate a free register.
    void releaseCallReserved();
};

} // namespace zanna::codegen::x64::ra
