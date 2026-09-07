//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/InstrEffects.hpp
// Purpose: The single model of what an AArch64 MIR instruction reads and
//          writes: explicit register operands (via ra::operandRoles), the
//          implicit ABI registers of calls and returns, NZCV, memory, and the
//          reserved scratch registers the emitters may write while expanding
//          wide immediates and large offsets.
// Key invariants:
//   - ra::operandRoles is the only explicit use/def table; nothing here
//     re-derives operand positions per opcode.
//   - Every post-RA pass that reasons about liveness, copies, or reordering
//     consumes effectsOf() (or the predicates below) rather than a local table,
//     so implicit effects can never be modeled by one pass and forgotten by
//     another.
//   - PhysRegSet bit layout: bits 0..31 are X0..X30 and SP, bits 32..63 are
//     V0..V31. It matches the RegSet used by the CFG-aware DCE.
// Ownership/Lifetime:
//   - Stateless free functions and value types; no retained state.
// Links: src/codegen/aarch64/ra/OperandRoles.hpp,
//        src/codegen/aarch64/peephole/CopyPropDCE.cpp,
//        src/codegen/aarch64/passes/SchedulerPass.cpp,
//        docs/internals/backend-codegen-review-2026-09.md (Phase 2.1)
//
//===----------------------------------------------------------------------===//
#pragma once

#include "codegen/aarch64/MachineIR.hpp"
#include "codegen/aarch64/TargetAArch64.hpp"

#include <cstdint>

/// @file
/// @brief Declares the shared instruction-effects model for AArch64 MIR.

namespace zanna::codegen::aarch64 {

/// @brief Bit set over AArch64 physical registers.
/// @details Bits 0..31 map X0..X30 and SP; bits 32..63 map V0..V31.
struct PhysRegSet {
    uint64_t bits{0}; ///< Membership bits (see layout above).

    /// @brief Map a physical register to its bit index.
    /// @param reg Register to map.
    /// @return Bit index in `[0, 64)`, or 64 when @p reg is not representable.
    [[nodiscard]] static constexpr unsigned bitOf(PhysReg reg) noexcept {
        const auto ordinal = static_cast<unsigned>(reg);
        if (reg <= PhysReg::SP)
            return ordinal;
        if (reg >= PhysReg::V0 && reg <= PhysReg::V31)
            return 32u + (ordinal - static_cast<unsigned>(PhysReg::V0));
        return 64u;
    }

    /// @brief Add @p reg to the set (no-op for unrepresentable registers).
    void add(PhysReg reg) noexcept {
        const unsigned bit = bitOf(reg);
        if (bit < 64u)
            bits |= (uint64_t{1} << bit);
    }

    /// @brief Remove @p reg from the set.
    void remove(PhysReg reg) noexcept {
        const unsigned bit = bitOf(reg);
        if (bit < 64u)
            bits &= ~(uint64_t{1} << bit);
    }

    /// @brief Membership test.
    [[nodiscard]] bool contains(PhysReg reg) const noexcept {
        const unsigned bit = bitOf(reg);
        return bit < 64u && (bits & (uint64_t{1} << bit)) != 0;
    }

    /// @brief Whether the set is empty.
    [[nodiscard]] bool empty() const noexcept {
        return bits == 0;
    }

    /// @brief Union in place.
    PhysRegSet &operator|=(const PhysRegSet &other) noexcept {
        bits |= other.bits;
        return *this;
    }

    /// @brief Intersection test.
    [[nodiscard]] bool intersects(const PhysRegSet &other) const noexcept {
        return (bits & other.bits) != 0;
    }
};

/// @brief Everything one MIR instruction observably reads and writes.
struct InstrEffects {
    /// @brief Memory behavior of the instruction.
    enum class Mem : uint8_t {
        None,   ///< Touches no memory.
        Load,   ///< Reads memory.
        Store,  ///< Writes memory.
        Barrier ///< May read and write arbitrary memory (calls, SP adjustments).
    };

    PhysRegSet uses;          ///< Explicit and implicit physical registers read.
    PhysRegSet defs;          ///< Explicit and implicit physical registers written.
    bool readsFlags{false};   ///< Reads NZCV.
    bool writesFlags{false};  ///< Writes NZCV (compares, flag-setting ALU, calls).
    Mem mem{Mem::None};       ///< Memory behavior.
    bool isCall{false};       ///< `Bl` / `Blr`.
    bool isTerminator{false}; ///< Ends a basic block's straight-line flow.
    bool isNoReturn{false};   ///< Direct call to a runtime helper that never returns.
};

/// @brief Compute the effects of @p instr under target @p target.
/// @details Explicit operand roles come from ra::operandRoles; calls add the
///          argument registers and SP as uses and every caller-saved register
///          plus LR as defs; returns add the return registers as uses; FP- and
///          SP-relative forms add the base register as a use; jump tables add
///          the reserved dispatch scratch registers as defs. While emit-time
///          expansion still exists (until ExpandPseudosPass lands) the reserved
///          scratch GPRs are added to `defs` for every instruction the emitters
///          may expand through them (see emitTimeScratchClobber()).
/// @param instr Instruction to classify.
/// @param target ABI description supplying argument, return, and caller-saved sets.
/// @return The instruction's effects.
/// @throws std::logic_error when a register operand has no classified role
///         (an unclassified opcode is a backend bug the verifier reports).
[[nodiscard]] InstrEffects effectsOf(const MInstr &instr, const TargetInfo &target);

/// @brief Physical registers a call may overwrite: caller-saved GPR/FPR sets and LR.
[[nodiscard]] PhysRegSet callClobberSet(const TargetInfo &target) noexcept;

/// @brief Whether @p opc writes NZCV.
[[nodiscard]] bool setsFlags(MOpcode opc) noexcept;

/// @brief Whether @p opc reads NZCV.
[[nodiscard]] bool readsFlags(MOpcode opc) noexcept;

/// @brief Whether @p opc reads memory (scalar, pair, or phi-slot loads).
[[nodiscard]] bool isLoadOpcode(MOpcode opc) noexcept;

/// @brief Whether @p opc writes memory (scalar, pair, SP-relative, or phi-slot stores).
[[nodiscard]] bool isStoreOpcode(MOpcode opc) noexcept;

/// @brief Whether @p opc ends straight-line control flow.
[[nodiscard]] bool isTerminatorOpcode(MOpcode opc) noexcept;

/// @brief Whether @p opc addresses memory (or forms an address) relative to x29.
/// @details Covers the FP-relative scalar/pair loads and stores, phi-slot
///          stores, and `AddFpImm`; the offset is the instruction's last
///          immediate operand.
[[nodiscard]] bool isFrameRelativeOpcode(MOpcode opc) noexcept;

/// @brief Whether @p opc stores through SP into the outgoing-argument area.
[[nodiscard]] bool isSpRelativeOpcode(MOpcode opc) noexcept;

/// @brief Whether @p opc adjusts SP (`SubSpImm` / `AddSpImm`).
[[nodiscard]] bool isSpAdjustOpcode(MOpcode opc) noexcept;

/// @brief The reserved scratch GPRs the emitters may write during expansion (x9, x16, x17).
[[nodiscard]] PhysRegSet emitScratchGPRs() noexcept;

/// @brief Whether the emitters may write a reserved scratch GPR while emitting @p instr.
/// @details Wide immediates on ALU/compare forms, non-FP8 `FMovRI`, and FP-,
///          SP-, pair-, and base-relative accesses whose displacement exceeds
///          the encodable range are materialized through kScratchGPR/2/3 at
///          emit time. Those writes are invisible in the operands, so every
///          post-RA reordering or liveness pass must treat the instruction as
///          defining the scratch set. Removed once ExpandPseudosPass makes the
///          expansion explicit MIR.
[[nodiscard]] bool emitTimeScratchClobber(const MInstr &instr) noexcept;

} // namespace zanna::codegen::aarch64
