//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/ra/GlobalAllocator.hpp
// Purpose: Function-wide register allocator for x86-64 MIR (block parameters
//          are virtual registers, branch arguments are PX_COPY edges). Two
//          phases: the shared whole-interval linear scan
//          (common/ra/IntervalAssign.hpp: assign or spill everywhere,
//          weight-based eviction, hints), then a rewrite that replaces
//          operands, reloads and stores spilled values around each
//          instruction, lowers every PX_COPY through the shared
//          sequentializer, and lays out shared spill slots per class.
// Key invariants:
//   - The assignment map is complete before the rewrite starts; the rewrite
//     never changes an assignment.
//   - A spilled value is reloaded at every use into a register that is free
//     at the instruction (pool first, the reserved R10/R11 for GPRs, and as
//     a last resort a pool register saved and restored around the
//     instruction) and stored after every definition; no reload survives a
//     call or a branch, and a value live across `rt_native_eh_push`/`setjmp`
//     is never given a register (EH-1).
//   - A temporary never names a register the instruction itself reads or
//     writes, explicitly or implicitly (RAX/RDX of a division, RCX of a
//     shift, the argument registers of a call, R10/R11 of a jump table).
//   - Two spilled values of one class share one slot iff their range lists
//     do not intersect; hotter slots get the lower indices.
//   - Deterministic: vector state indexed by virtual register, fixed pool
//     order, id tie-breaks; safe to run on functions in parallel.
// Ownership/Lifetime:
//   - Borrows the function and target for the duration of run().
// Links: src/codegen/x86_64/ra/GlobalAllocator.cpp,
//        src/codegen/x86_64/ra/LiveIntervals.hpp,
//        src/codegen/common/ra/IntervalAssign.hpp,
//        src/codegen/common/ra/ParallelCopy.hpp,
//        src/codegen/x86_64/ra/SpillSlots.hpp,
//        docs/adr/0339-aarch64-function-wide-register-allocation.md
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/common/ra/IntervalAssign.hpp"
#include "codegen/x86_64/MachineIR.hpp"
#include "codegen/x86_64/OperandRoles.hpp"
#include "codegen/x86_64/RegAllocLinear.hpp"
#include "codegen/x86_64/TargetX64.hpp"
#include "codegen/x86_64/ra/LiveIntervals.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

/// @file
/// @brief Declares the x86-64 function-wide register allocator.

namespace zanna::codegen::x64::ra {

/// @brief Counters the function-wide allocator reports.
struct GlobalAllocationStats {
    std::size_t vregs{0};         ///< Virtual registers with an interval.
    std::size_t spilled{0};       ///< Intervals that got no register.
    std::size_t reloads{0};       ///< Loads emitted for spilled uses.
    std::size_t spillStores{0};   ///< Stores emitted for spilled definitions.
    std::size_t spillSlotsGPR{0}; ///< GPR frame slots allocated (shared and temporary).
    std::size_t spillSlotsXMM{0}; ///< XMM frame slots allocated (shared and temporary).
    std::size_t edgeMoves{0};     ///< Instructions emitted for parallel copies.
    std::size_t spillArounds{0};  ///< Temporaries served by saving a live register.
};

/// @brief Function-wide allocator over the interval model.
class GlobalAllocator {
  public:
    /// @brief Bind to @p fn and @p ti.
    GlobalAllocator(MFunction &fn, const TargetInfo &ti);

    /// @brief Build intervals, assign, lay out slots, rewrite.
    /// @throws std::runtime_error on a malformed input (a parallel copy with
    ///         mismatched classes or a non-register operand, an unknown
    ///         virtual register) or when a terminator presents more spilled
    ///         operands than any register can serve.
    GlobalAllocationStats run();

    /// @brief The interval model built by run() (tests and dumps).
    [[nodiscard]] const LiveIntervals &intervals() const noexcept {
        return intervals_;
    }

    /// @brief Register assigned to @p vreg after run(); RSP when spilled or unknown.
    [[nodiscard]] PhysReg assignedRegister(uint16_t vreg) const noexcept;

    /// @brief Slot index of @p vreg's spill slot after run(); -1 when not spilled.
    [[nodiscard]] int spillSlot(uint16_t vreg) const noexcept;

  private:
    /// @brief One spilled operand of the instruction being rewritten.
    struct Reload {
        uint16_t vreg;
        RegClass cls;
        PhysReg tmp;
        int slot;
        bool isUse;
        bool isDef;
    };

    /// @brief Instruction sequences that wrap one rewritten instruction.
    struct Wrap {
        std::vector<MInstr> before; ///< Saves and reloads.
        std::vector<MInstr> after;  ///< Spill stores and restores.
    };

    MFunction &fn_;
    const TargetInfo &ti_;
    LiveIntervals intervals_;

    std::vector<PhysReg> gprOrder_; ///< Allocatable GPRs, caller-saved first.
    std::vector<PhysReg> xmmOrder_; ///< Allocatable XMMs, caller-saved first.
    std::array<bool, kPhysRegCount> allocatable_{};
    std::array<bool, kPhysRegCount> calleeSaved_{};

    std::vector<PhysReg> assigned_;                 ///< Per interval index; RSP = no register.
    std::vector<int> slotIndex_;                    ///< Per interval index; -1 = no slot.
    std::array<RangeList, kPhysRegCount> occupied_; ///< Per ordinal after assignment.
    std::array<std::size_t, 2> nextSlot_{};         ///< Next free slot index per class.

    GlobalAllocationStats stats_{};
    AllocationResult result_{};

    void buildPools();
    void assign();
    void rewrite();
    void rewriteBlock(std::size_t bi);
    void lowerParallelCopy(std::size_t bi,
                           std::size_t ii,
                           const MInstr &mi,
                           std::vector<MInstr> &out);
    [[nodiscard]] const std::vector<PhysReg> &orderFor(RegClass cls) const noexcept;
    [[nodiscard]] bool freeAt(PhysReg reg, Pos read, Pos write) const noexcept;
    [[nodiscard]] int freshSlot(RegClass cls);
    /// @brief Choose a temporary of @p cls free at positions @p read and
    ///        @p write (pool order first, reserved scratch next), excluding
    ///        @p blocked (registers the instruction touches) and @p taken
    ///        (temporaries of this instruction); records the choice in
    ///        @p taken. When nothing is free and @p wrap is non-null, a pool
    ///        register is saved into a fresh slot before and restored after.
    /// @throws std::runtime_error when no temporary can be provided.
    PhysReg pickTemp(RegClass cls,
                     Pos read,
                     Pos write,
                     PhysRegMask blocked,
                     std::vector<PhysReg> &taken,
                     Wrap *wrap);
};

} // namespace zanna::codegen::x64::ra
