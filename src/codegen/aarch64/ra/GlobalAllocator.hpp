//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/ra/GlobalAllocator.hpp
// Purpose: Function-wide register allocator for AArch64 MIR lowered in the
//          edge-copy mode (block parameters are virtual registers, branch
//          arguments are ParallelCopy edges). Two phases: whole-interval
//          linear scan over intervals with holes (assign or spill
//          everywhere, weight-based eviction, hints), then a rewrite that
//          replaces operands, reloads and stores spilled values around each
//          instruction, lowers every ParallelCopy through the shared
//          sequentializer, and lays out shared spill slots.
// Key invariants:
//   - The assignment map is complete before the rewrite starts; the rewrite
//     never changes an assignment.
//   - A spilled value is reloaded at every use into a register that is free
//     at the instruction (pool first, reserved scratch under the arity bound
//     otherwise) and stored after every definition; no reload survives a
//     call or a branch (EH-2), and a value live across `rt_native_eh_push`
//     is never given a register (EH-1).
//   - ParallelCopy cycles break through a free pool register or, when none
//     is free, a fresh frame slot; mem-to-mem moves route through the
//     reserved x17/v17, so the reserved scratch is never live across another
//     instruction.
//   - Two spilled values share one slot iff their range lists do not
//     intersect; hotter slots are allocated first so they sit nearest x29.
//   - Deterministic: vector state indexed by virtual register, fixed pool
//     order, id tie-breaks; safe to run on functions in parallel.
// Ownership/Lifetime:
//   - Borrows the function and target for the duration of run().
// Links: src/codegen/aarch64/ra/GlobalAllocator.cpp,
//        src/codegen/aarch64/ra/LiveIntervals.hpp,
//        src/codegen/common/ra/ParallelCopy.hpp,
//        src/codegen/aarch64/FrameBuilder.hpp,
//        docs/internals/backend-codegen-review-2026-09.md (Phase 3 C5)
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/aarch64/FrameBuilder.hpp"
#include "codegen/aarch64/MachineIR.hpp"
#include "codegen/aarch64/TargetAArch64.hpp"
#include "codegen/aarch64/ra/LiveIntervals.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

/// @file
/// @brief Declares the AArch64 function-wide register allocator.

namespace zanna::codegen::aarch64 {

/// @brief Statistics produced by AArch64 register allocation.
struct AllocationResult {
    int gprSpillSlots{0}; ///< Number of frame slots the allocator created for spills.
};

} // namespace zanna::codegen::aarch64

namespace zanna::codegen::aarch64::ra {

/// @brief Counters the function-wide allocator reports.
struct GlobalAllocationStats {
    std::size_t vregs{0};       ///< Virtual registers with an interval.
    std::size_t spilled{0};     ///< Intervals that got no register.
    std::size_t reloads{0};     ///< Loads emitted for spilled uses.
    std::size_t spillStores{0}; ///< Stores emitted for spilled definitions.
    std::size_t spillSlots{0};  ///< Frame slots allocated for spilled values.
    std::size_t edgeMoves{0};   ///< Instructions emitted for parallel copies.
};

/// @brief Function-wide allocator over the interval model.
class GlobalAllocator {
  public:
    /// @brief Bind to @p fn (edge-copy lowering shape) and @p ti.
    GlobalAllocator(MFunction &fn, const TargetInfo &ti);

    /// @brief Build intervals, assign, lay out slots, rewrite, publish frame
    ///        and callee-saved metadata.
    /// @throws std::runtime_error on a malformed input (a parallel copy with
    ///         mismatched classes, or more simultaneous
    ///         spilled operands than the reserved scratch can serve).
    GlobalAllocationStats run();

    /// @brief The interval model built by run() (tests and dumps).
    [[nodiscard]] const LiveIntervals &intervals() const noexcept {
        return intervals_;
    }

    /// @brief Register assigned to @p vreg after run(); SP when spilled or unknown.
    [[nodiscard]] PhysReg assignedRegister(uint16_t vreg) const noexcept;

    /// @brief Frame offset of @p vreg's spill slot after run(); 0 when not spilled.
    [[nodiscard]] int spillOffset(uint16_t vreg) const noexcept;

  private:
    MFunction &fn_;
    const TargetInfo &ti_;
    FrameBuilder fb_;
    LiveIntervals intervals_;

    std::vector<PhysReg> gprOrder_; ///< Allocatable GPRs, caller-saved first.
    std::vector<PhysReg> fprOrder_; ///< Allocatable FPRs, caller-saved first.
    std::array<bool, 64> allocatable_{};
    std::array<bool, 64> calleeSaved_{};
    std::array<bool, 64> savedUsed_{};

    std::vector<PhysReg> assigned_; ///< Per interval index; SP = no register.
    std::vector<int> slotOffset_;   ///< Per interval index; 0 = no slot.
    std::array<RangeList, 64> occupied_;
    std::array<std::vector<std::size_t>, 64> assignedTo_;

    uint32_t nextTempSlotKey_{0xFFFF0000u};
    GlobalAllocationStats stats_{};

    void buildPools();
    void assign();
    void assignOne(std::size_t idx);
    void place(std::size_t idx, PhysReg reg);
    void unassign(std::size_t idx);
    void assignSlots();
    void rewrite();
    void rewriteBlock(std::size_t bi);
    void lowerParallelCopy(std::size_t bi,
                           std::size_t ii,
                           const MInstr &mi,
                           std::vector<MInstr> &out);
    [[nodiscard]] const std::vector<PhysReg> &orderFor(RegClass cls) const noexcept;
    [[nodiscard]] bool freeAt(PhysReg reg, Pos read, Pos write) const noexcept;
    /// @brief Choose a temporary of @p cls free at positions @p read and
    ///        @p write (pool order first, reserved scratch last), excluding
    ///        @p blocked (explicit operands) and @p taken (temporaries of this
    ///        instruction); records the choice in @p taken.
    /// @throws std::runtime_error when the reserved scratch is exhausted.
    PhysReg pickTemp(RegClass cls,
                     Pos read,
                     Pos write,
                     const std::vector<PhysReg> &blocked,
                     std::vector<PhysReg> &taken);
    void noteUse(PhysReg reg);
    void finish();
};

/// @brief Run the function-wide allocator on @p fn.
/// @param[in,out] fn Function in the lowering shape (block-parameter vregs, ParallelCopy edges).
/// @param ti Target register sets and calling convention.
/// @return Allocation statistics (spill slot count).
/// @post Every register operand is physical, no ParallelCopy remains, the
///       frame is finalized, and `savedGPRs`/`savedFPRs` are published.
[[nodiscard]] AllocationResult allocateGlobal(MFunction &fn, const TargetInfo &ti);

} // namespace zanna::codegen::aarch64::ra
