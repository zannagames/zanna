//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/A64ImmediateUtils.hpp
// Purpose: Shared AArch64 immediate-classification and legalization helpers.
//          Classifies integer constants into the encoding forms accepted by the
//          ISA (ADD/SUB 12-bit shifted immediates, MOVZ/MOVN/MOVK sequences) and
//          emits legalized arithmetic instructions when a direct imm form is not
//          available.
// Key invariants:
//   - classifyAddSubImmEncoding() is a pure predicate; it never modifies MIR.
//   - forEachMoveWideInst() chooses MOVZ vs MOVN to minimise instruction count.
//   - emitLegalizedSignedImmArith() appends to MBasicBlock in-place; it never
//     removes or reorders existing instructions.
// Ownership/Lifetime:
//   - All helpers are stateless free functions / function templates; no heap
//     allocation occurs.
// Links: src/codegen/aarch64/MachineIR.hpp,
//        src/codegen/aarch64/passes/LegalizePass.cpp (primary caller)
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Provides reusable AArch64 immediate classification and legalization.
 *
 * These helpers keep immediate-encoding policy consistent between instruction
 * selection and legalization. Classification is pure; emission helpers append
 * complete machine instructions to caller-owned blocks and obtain any required
 * temporary register through a supplied callback.
 */

#pragma once

#include "codegen/aarch64/MachineIR.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace zanna::codegen::aarch64 {

/// @brief Encoding parameters for an ADD/SUB 12-bit immediate instruction.
/// @details AArch64 ADD/SUB accept a 12-bit unsigned immediate with an optional
///          logical left-shift of 12 bits, covering values 0–0xFFF and 0x1000–0xFFF000.
struct AddSubImmEncoding {
    uint32_t imm12; ///< The 12-bit immediate value (before any shift is applied).
    bool shift12;   ///< True if the immediate is shifted left 12 bits in the encoding.
};

/// @brief AArch64 MOVE-WIDE instruction variants for materializing 64-bit constants.
enum class MoveWideOpcode {
    MovZ, ///< Zero-and-move: sets the register to imm16 << shift, zeroing all other bits.
    MovN, ///< Negate-and-move: sets the register to ~(imm16 << shift), zeroing all other bits.
    MovK, ///< Keep-and-move: inserts imm16 into the 16-bit lane at @p shift, keeping other bits.
};

/// @brief A single decoded MOVE-WIDE instruction for use with forEachMoveWideInst().
struct MoveWideInst {
    MoveWideOpcode opcode; ///< Which MOVE-WIDE variant to emit (MOVZ / MOVN / MOVK).
    uint16_t imm16;        ///< The 16-bit immediate payload for this lane.
    uint8_t shift;         ///< Bit position of the lane: 0, 16, 32, or 48.
};

/// @brief Return the absolute (unsigned) magnitude of a signed integer immediate.
/// @details Handles the two's-complement minimum value correctly: -(INT64_MIN + 1) + 1 == 2^63.
/// @param imm Signed immediate (from the MIR operand).
/// @return Unsigned magnitude; always positive.
[[nodiscard]] inline uint64_t absImmUnsigned(long long imm) noexcept {
    return imm < 0 ? static_cast<uint64_t>(-(imm + 1)) + 1ULL : static_cast<uint64_t>(imm);
}

/// @brief Test whether @p imm fits in the AArch64 ADD/SUB immediate encoding.
/// @details Returns the imm12 value and shift12 flag if the value fits in 12 bits
///          (0–0xFFF, shift12=false) or is a multiple of 0x1000 whose top 12 bits
///          fit in 12 bits (0x1000–0xFFF000, shift12=true). Returns nullopt otherwise.
/// @param imm Unsigned value to test (caller should pass the magnitude for signed imms).
/// @return Filled AddSubImmEncoding on success, nullopt if the value requires MOVZ/MOVK.
[[nodiscard]] inline std::optional<AddSubImmEncoding> classifyAddSubImmEncoding(
    uint64_t imm) noexcept {
    if (imm <= 0xFFFULL)
        return AddSubImmEncoding{static_cast<uint32_t>(imm), false};
    if ((imm & 0xFFFULL) == 0 && (imm >> 12) <= 0xFFFULL)
        return AddSubImmEncoding{static_cast<uint32_t>(imm >> 12), true};
    return std::nullopt;
}

/// @brief Invoke @p emit once per MOVE-WIDE instruction needed to load @p imm into a register.
/// @details Decomposes the 64-bit value into four 16-bit lanes and selects the encoding
///          strategy (MOVZ-seed or MOVN-seed) that minimises total instruction count by
///          choosing whichever has fewer non-zero lanes. The first call always uses MOVZ or
///          MOVN; subsequent non-trivial lanes use MOVK.
/// @tparam EmitFn Callable with signature `void(MoveWideInst)`.
/// @param imm  The 64-bit immediate to materialise.
/// @param emit Callback invoked for each MOVE-WIDE instruction in emission order.
/// @post At least one instruction is emitted, including for zero and all-ones values.
template <typename EmitFn> inline void forEachMoveWideInst(uint64_t imm, EmitFn &&emit) {
    const std::array<uint16_t, 4> chunks = {
        static_cast<uint16_t>(imm & 0xFFFFULL),
        static_cast<uint16_t>((imm >> 16) & 0xFFFFULL),
        static_cast<uint16_t>((imm >> 32) & 0xFFFFULL),
        static_cast<uint16_t>((imm >> 48) & 0xFFFFULL),
    };

    const uint64_t invImm = ~imm;
    const std::array<uint16_t, 4> invChunks = {
        static_cast<uint16_t>(invImm & 0xFFFFULL),
        static_cast<uint16_t>((invImm >> 16) & 0xFFFFULL),
        static_cast<uint16_t>((invImm >> 32) & 0xFFFFULL),
        static_cast<uint16_t>((invImm >> 48) & 0xFFFFULL),
    };

    int nzCount = 0;
    int invNzCount = 0;
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        if (chunks[i] != 0)
            ++nzCount;
        if (invChunks[i] != 0)
            ++invNzCount;
    }

    const bool useMovn = invNzCount < nzCount;
    const auto &seedChunks = useMovn ? invChunks : chunks;
    const MoveWideOpcode seedOpcode = useMovn ? MoveWideOpcode::MovN : MoveWideOpcode::MovZ;

    int firstLane = -1;
    for (std::size_t i = 0; i < seedChunks.size(); ++i) {
        if (seedChunks[i] != 0) {
            firstLane = static_cast<int>(i);
            break;
        }
    }

    if (firstLane < 0) {
        emit(MoveWideInst{seedOpcode, 0, 0});
        return;
    }

    emit(MoveWideInst{seedOpcode,
                      seedChunks[static_cast<std::size_t>(firstLane)],
                      static_cast<uint8_t>(firstLane * 16)});

    for (std::size_t i = 0; i < chunks.size(); ++i) {
        if (static_cast<int>(i) == firstLane)
            continue;
        if (useMovn) {
            if (chunks[i] != 0xFFFFu) {
                emit(MoveWideInst{MoveWideOpcode::MovK, chunks[i], static_cast<uint8_t>(i * 16)});
            }
            continue;
        }
        if (chunks[i] != 0) {
            emit(MoveWideInst{MoveWideOpcode::MovK, chunks[i], static_cast<uint8_t>(i * 16)});
        }
    }
}

/// @brief Arithmetic direction for emitLegalizedSignedImmArith().
enum class SignedImmArithKind {
    Add, ///< Emit dst = lhs + imm (or dst = lhs - |imm| when imm is negative).
    Sub, ///< Emit dst = lhs - imm (or dst = lhs + |imm| when imm is negative).
};

/// @brief Emit a legalized signed-immediate arithmetic instruction into @p out.
/// @details If the magnitude of @p imm fits in the ADD/SUB 12-bit encoding, emits
///          a single ADD-imm or SUB-imm instruction (swapping to the complement form
///          when the sign makes it cheaper). Otherwise calls @p makeTemp to materialize
///          the immediate into a temporary register and emits the register form.
/// @tparam MakeTempFn  Callable `MOperand(long long imm)` that materializes the constant
///                     and returns an MOperand for the new virtual register.
/// @param out       Basic block to append the new instruction(s) to.
/// @param dst       Destination operand (written).
/// @param lhs       Left-hand source operand (read).
/// @param imm       Signed immediate operand from the MIR instruction.
/// @param kind      Whether to prefer ADD or SUB encoding for positive values.
/// @param addImmOpc MOpcode for the reg+imm ADD variant.
/// @param subImmOpc MOpcode for the reg+imm SUB variant.
/// @param addRegOpc MOpcode for the reg+reg ADD variant (used when imm is too large).
/// @param subRegOpc MOpcode for the reg+reg SUB variant (used when imm is too large).
/// @param makeTemp  Factory called only when the imm-form is not available.
/// @post Appends exactly one arithmetic instruction, plus any instructions
///       emitted internally by @p makeTemp when register materialization is required.
template <typename MakeTempFn>
inline void emitLegalizedSignedImmArith(MBasicBlock &out,
                                        const MOperand &dst,
                                        const MOperand &lhs,
                                        long long imm,
                                        SignedImmArithKind kind,
                                        MOpcode addImmOpc,
                                        MOpcode subImmOpc,
                                        MOpcode addRegOpc,
                                        MOpcode subRegOpc,
                                        MakeTempFn &&makeTemp) {
    const uint64_t magnitude = absImmUnsigned(imm);
    if (classifyAddSubImmEncoding(magnitude).has_value()) {
        const bool useAddImm = (kind == SignedImmArithKind::Add) ? (imm >= 0) : (imm < 0);
        out.instrs.push_back(
            MInstr{useAddImm ? addImmOpc : subImmOpc,
                   {dst, lhs, MOperand::immOp(static_cast<long long>(magnitude))}});
        return;
    }

    const MOperand tmp = makeTemp(imm);
    out.instrs.push_back(
        MInstr{kind == SignedImmArithKind::Add ? addRegOpc : subRegOpc, {dst, lhs, tmp}});
}

} // namespace zanna::codegen::aarch64
