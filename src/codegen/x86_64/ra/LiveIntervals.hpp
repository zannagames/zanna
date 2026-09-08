//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/ra/LiveIntervals.hpp
// Purpose: Function-wide live intervals with holes for the x86-64
//          function-wide register allocator: a linear position space over
//          the blocks in reverse post-order, one range list per virtual
//          register built from the CFG liveness solution, fixed range lists
//          for the physical registers the lowered code names explicitly or
//          touches implicitly (effects model), spill weights, and register
//          hints.
// Key invariants:
//   - The position and range model is the shared one
//     (common/ra/IntervalAssign.hpp): instruction i of block b reads at
//     base[b] + 2i and writes at base[b] + 2i + 1; a block also owns an exit
//     position after its last write. Blocks are numbered in reverse
//     post-order over MirCfg (sorted successors), so positions are
//     deterministic.
//   - Every register fact comes from operandRoles (virtual operands, memory
//     address registers are reads) and effectsOf (physical registers,
//     implicit RAX/RDX/RCX effects, call argument reads and clobbers, return
//     reads); the model keeps no opcode table of its own.
//   - Virtual register ids are unique across classes (verifier VREG-CLASS),
//     so intervals are keyed by id with a class tag.
// Ownership/Lifetime:
//   - LiveIntervals is a snapshot: it holds no reference to the function and
//     is invalidated by any change to it.
// Links: src/codegen/x86_64/ra/LiveIntervals.cpp,
//        src/codegen/x86_64/ra/GlobalAllocator.hpp,
//        src/codegen/x86_64/ra/Liveness.hpp, src/codegen/x86_64/PhysLiveness.hpp,
//        src/codegen/common/ra/IntervalAssign.hpp,
//        docs/internals/backend-codegen-review-2026-09.md (Phase 3 C8)
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/common/ra/IntervalAssign.hpp"
#include "codegen/x86_64/MachineIR.hpp"
#include "codegen/x86_64/TargetX64.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/// @file
/// @brief Declares the interval model of the x86-64 function-wide allocator.

namespace zanna::codegen::x64::ra {

/// @brief A position in the function's linear order (see the file header).
using Pos = zanna::codegen::ra::Pos;

/// @brief Sentinel for "no position".
inline constexpr Pos kNoPos = zanna::codegen::ra::kNoPos;

/// @brief One inclusive live range `[start, end]` in positions.
using LiveRange = zanna::codegen::ra::LiveRange;

/// @brief Sorted, disjoint, merged list of live ranges (shared model).
using RangeList = zanna::codegen::ra::RangeList;

/// @brief Number of physical register ordinals (RAX .. XMM15).
inline constexpr std::size_t kPhysRegCount = static_cast<std::size_t>(PhysReg::XMM15) + 1;

/// @brief The interval of one virtual register plus what the allocator needs
///        to place it.
struct VRegInterval {
    uint16_t id{0};                  ///< Virtual register id (unique across classes).
    RegClass cls{RegClass::GPR};     ///< Register class.
    RangeList live;                  ///< Positions at which the value is live.
    std::vector<Pos> uses;           ///< Sorted read positions.
    std::vector<Pos> defs;           ///< Sorted write positions.
    double weight{0.0};              ///< Spill weight: Σ (uses + defs) · 10^loopDepth.
    bool crossesCall{false};         ///< Live at the write position of some call.
    bool crossesEhPush{false};       ///< Live across `rt_native_eh_push` (memory-homed, EH-1).
    bool hasPhysHint{false};         ///< Whether @ref hintPhys is set.
    PhysReg hintPhys{PhysReg::RAX};  ///< Physical register hint when @ref hasPhysHint.
    std::vector<uint16_t> hintVRegs; ///< Virtual registers this one is copied to/from.
};

/// @brief Block numbering and per-block position bases.
struct FunctionPositions {
    std::vector<std::size_t> rpo;    ///< Block indices in reverse post-order (unreachable last).
    std::vector<Pos> blockBase;      ///< Per block: position of its first instruction's read.
    std::vector<Pos> blockExit;      ///< Per block: exit position (after the last write).
    std::vector<unsigned> loopDepth; ///< Per block: natural-loop nesting depth.
    Pos total{0};                    ///< One past the last position.

    /// @brief Read position of instruction @p i in block @p b.
    [[nodiscard]] Pos readPos(std::size_t b, std::size_t i) const noexcept {
        return blockBase[b] + static_cast<Pos>(2 * i);
    }

    /// @brief Write position of instruction @p i in block @p b.
    [[nodiscard]] Pos writePos(std::size_t b, std::size_t i) const noexcept {
        return blockBase[b] + static_cast<Pos>(2 * i) + 1;
    }
};

/// @brief Whole-function live intervals of one pre-allocation MIR function.
class LiveIntervals {
  public:
    /// @brief Build every interval of @p fn (see the file header for the model).
    /// @param fn Function with virtual registers (block-parameter vregs, PX_COPY edges).
    /// @param target ABI description for the effects model.
    void build(const MFunction &fn, const TargetInfo &target);

    /// @brief Block numbering and positions.
    [[nodiscard]] const FunctionPositions &positions() const noexcept {
        return positions_;
    }

    /// @brief Every virtual-register interval, sorted by id.
    [[nodiscard]] const std::vector<VRegInterval> &vregs() const noexcept {
        return vregs_;
    }

    /// @brief Interval of virtual register @p id, or nullptr.
    [[nodiscard]] const VRegInterval *find(uint16_t id) const noexcept;

    /// @brief Index into vregs() of virtual register @p id, or SIZE_MAX.
    [[nodiscard]] std::size_t indexOf(uint16_t id) const noexcept;

    /// @brief Positions at which physical register @p reg is occupied by the
    ///        lowered code itself (explicit and implicit writes until their
    ///        last read, call clobbers, ABI live-ins).
    [[nodiscard]] const RangeList &fixed(PhysReg reg) const noexcept;

    /// @brief Write positions of every call, sorted.
    [[nodiscard]] const std::vector<Pos> &callPositions() const noexcept {
        return callPositions_;
    }

    /// @brief Write positions of every `rt_native_eh_push`/`setjmp` call, sorted.
    [[nodiscard]] const std::vector<Pos> &ehPushPositions() const noexcept {
        return ehPushPositions_;
    }

    /// @brief Render every interval for diagnostics and tests.
    [[nodiscard]] std::string dump() const;

  private:
    FunctionPositions positions_;
    std::vector<VRegInterval> vregs_;
    std::vector<std::size_t> indexById_; ///< id -> index into vregs_ (SIZE_MAX when absent).
    std::array<RangeList, kPhysRegCount> fixed_{};
    std::vector<Pos> callPositions_;
    std::vector<Pos> ehPushPositions_;

    void numberBlocks(const MFunction &fn);
    void buildVRegIntervals(const MFunction &fn);
    void buildFixedIntervals(const MFunction &fn, const TargetInfo &target);
    void collectHints(const MFunction &fn);
};

} // namespace zanna::codegen::x64::ra
