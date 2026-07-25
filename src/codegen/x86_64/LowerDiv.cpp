//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/LowerDiv.cpp
// Purpose: Expand signed and unsigned 64-bit division/remainder pseudos into
//          explicit CQO/IDIV or XOR/DIV sequences for the x86-64 backend.
// Key invariants:
//   - Checked pseudos are guarded with division-by-zero trap tests.
//   - Plain div/rem pseudos lower directly when the divisor is proven nonzero.
//   - A single trap block per function is reused to minimise code growth.
//   - The pass executes between IL→MIR lowering and register allocation.
// Ownership/Lifetime:
//   - Mutates the MFunction in-place; no persistent auxiliary structures.
// Links: src/codegen/x86_64/LowerILToMIR.hpp,
//        src/codegen/x86_64/MachineIR.hpp
//
//===----------------------------------------------------------------------===//

#include "MachineIR.hpp"

#include "codegen/common/MagicDivision.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

/**
 * @file
 * @brief Implements x86-64 signed and unsigned 64-bit div/rem pseudo expansion.
 *
 * Constant divisors are strength-reduced to shifts, masks, or magic-number
 * multiplies when legal. Remaining pseudos become explicit architectural
 * RDX:RAX division sequences, with checked operations branching to lazily
 * created divide-by-zero or overflow trap blocks.
 */

namespace zanna::codegen::x64 {

namespace {
/// @brief Produce a shallow copy of a Machine IR operand.
///
/// @details Operands in Phase A Machine IR are small value types, so copying by
///          value preserves all necessary information when duplicating operands
///          across newly emitted instructions.  The helper centralises the
///          intent and documents that no deep cloning is required.
///
/// @param operand Operand instance to duplicate.
/// @return Copy of @p operand that can be reused in emitted instructions.
[[nodiscard]] Operand cloneOperand(const Operand &operand) {
    return operand;
}

/// @brief Locate a basic block index using its label, if present.
///
/// @details Iterates over @p fn until it finds a block whose @c label matches
///          the requested string.  The search allows the lowering pass to reuse
///          an existing trap block rather than materialising a duplicate.
///
/// @param fn Function currently being rewritten.
/// @param label Block label to search for.
/// @return Index of the block or empty optional when no block matches.
[[nodiscard]] std::optional<std::size_t> findBlockIndex(const MFunction &fn,
                                                        std::string_view label) {
    for (std::size_t idx = 0; idx < fn.blocks.size(); ++idx) {
        if (fn.blocks[idx].label == label) {
            return idx;
        }
    }
    return std::nullopt;
}

/// @brief Generate a unique label for the continuation block after a pseudo.
///
/// @details Prefers reusing the source block or function name to keep emitted
///          labels stable between compilations.  When neither is available a
///          synthetic prefix is used.  The @p sequence counter differentiates
///          multiple lowered pseudos originating from the same block.
///
/// @param fn Function currently being processed.
/// @param block Basic block that owned the pseudo instruction.
/// @param sequence Running identifier incremented per lowered pseudo.
/// @return Deterministic label for the continuation block.
[[nodiscard]] std::string makeContinuationLabel(const MFunction &fn,
                                                const MBasicBlock &block,
                                                unsigned sequence) {
    std::string base;
    if (!block.label.empty()) {
        base = block.label;
    } else if (!fn.name.empty()) {
        base = fn.name;
    } else {
        base = ".Ldiv";
    }
    base += ".div.";
    base += std::to_string(sequence);
    base += ".after";
    return base;
}

/// @brief Create an operand referencing a physical general-purpose register.
///
/// @details Lowered IDIV sequences use x86-64's implicit RAX/RDX pair and a
///          scratch register for the explicit divisor.
///          This helper ensures the correct register class is used whenever the
///          sequence materialises operands for those registers.
///
/// @param reg Physical register enumerator.
/// @return Operand representing @p reg.
[[nodiscard]] Operand makePhysRegOperand(PhysReg reg) {
    return x64::makePhysRegOperand(RegClass::GPR, static_cast<uint16_t>(reg));
}

/// @brief Predicate: does @p value fit a signed 32-bit immediate?
/// @details Mirrors the helper in ISel.cpp; duplicated here so the div-lowering
///          translation unit stays self-contained. Used to decide whether the
///          remainder mask for a power-of-2 @c urem can be encoded directly
///          into @c ANDri or whether it must be materialised through a temp.
/// @param value 64-bit candidate immediate.
/// @return True if @p value is representable as a signed 32-bit integer.
[[nodiscard]] bool fitsSignedImm32(int64_t value) noexcept {
    return value >= static_cast<int64_t>(std::numeric_limits<int32_t>::min()) &&
           value <= static_cast<int64_t>(std::numeric_limits<int32_t>::max());
}

/// @brief Return log2(v) if v is a positive power of 2, else -1.
/// @param v Candidate positive divisor.
/// @return Base-two logarithm for a power of two, otherwise @c -1.
[[nodiscard]] int log2IfPowerOf2(int64_t v) {
    if (v <= 0 || (v & (v - 1)) != 0)
        return -1;
    int log = 0;
    while ((1LL << log) != v)
        ++log;
    return log;
}

/// @brief Scan backward in a block for a MOVri that loads into the given vreg.
/// @details Stops at the first earlier definition of the register, preventing a
///          stale constant from being propagated across a redefinition.
/// @param block Block containing the divisor use.
/// @param beforeIdx Exclusive upper bound of the backward scan.
/// @param regOp Candidate virtual-register operand.
/// @return The immediate value, or nullopt if not found.
[[nodiscard]] std::optional<int64_t> findVRegConstant(const MBasicBlock &block,
                                                      std::size_t beforeIdx,
                                                      const Operand &regOp) {
    if (!std::holds_alternative<OpReg>(regOp))
        return std::nullopt;
    const auto &target = std::get<OpReg>(regOp);
    // Only look for non-physical vregs
    if (target.isPhys)
        return std::nullopt;

    for (std::size_t i = beforeIdx; i > 0; --i) {
        const auto &instr = block.instructions[i - 1];
        if (instr.opcode == MOpcode::MOVri && instr.operands.size() >= 2) {
            if (std::holds_alternative<OpReg>(instr.operands[0])) {
                const auto &dst = std::get<OpReg>(instr.operands[0]);
                if (dst.cls == target.cls && dst.idOrPhys == target.idOrPhys && !dst.isPhys) {
                    if (std::holds_alternative<OpImm>(instr.operands[1]))
                        return std::get<OpImm>(instr.operands[1]).val;
                }
            }
        }
        // If the vreg is redefined by another instruction, stop looking.
        if (instr.operands.size() >= 1 && std::holds_alternative<OpReg>(instr.operands[0])) {
            const auto &dst = std::get<OpReg>(instr.operands[0]);
            if (dst.cls == target.cls && dst.idOrPhys == target.idOrPhys && !dst.isPhys)
                break;
        }
    }
    return std::nullopt;
}

/// @brief Classification flags decoded from a division/remainder pseudo opcode.
struct DivOpcodeKind {
    bool isDiv{false};           ///< true ⇒ produce quotient (div), false ⇒ remainder.
    bool isSigned{false};        ///< true ⇒ uses CQO/IDIV; false ⇒ XOR/DIV.
    bool isCheckedSigned{false}; ///< true ⇒ emit INT_MIN / -1 overflow guard.
    bool needsDiv0Check{false};  ///< true ⇒ emit divisor-is-zero trap branch.
    bool matched{false};         ///< true ⇒ opcode was a div/rem pseudo we handle.
};

/// @brief Decode the opcode flags for a candidate instruction.
/// @details Returns `matched == false` for any opcode that is not a div/rem
///          pseudo so callers can quickly skip non-targets.
/// @param opcode Machine opcode to classify.
/// @return Independent quotient, signedness, checking, and match flags.
[[nodiscard]] DivOpcodeKind classifyDivOpcode(MOpcode opcode) noexcept {
    DivOpcodeKind k;
    const bool isSignedDiv = opcode == MOpcode::DIVS64rr;
    const bool isSignedRem = opcode == MOpcode::REMS64rr;
    const bool isCheckedSignedDiv = opcode == MOpcode::DIVS64Chk0rr;
    const bool isCheckedSignedRem = opcode == MOpcode::REMS64Chk0rr;
    const bool isCheckedUnsignedDiv = opcode == MOpcode::DIVU64Chk0rr;
    const bool isCheckedUnsignedRem = opcode == MOpcode::REMU64Chk0rr;
    const bool isUnsignedDiv = opcode == MOpcode::DIVU64rr || isCheckedUnsignedDiv;
    const bool isUnsignedRem = opcode == MOpcode::REMU64rr || isCheckedUnsignedRem;
    k.matched = isSignedDiv || isSignedRem || isCheckedSignedDiv || isCheckedSignedRem ||
                isUnsignedDiv || isUnsignedRem;
    if (!k.matched)
        return k;
    k.isDiv = isSignedDiv || isCheckedSignedDiv || isUnsignedDiv;
    k.isSigned = isSignedDiv || isSignedRem || isCheckedSignedDiv || isCheckedSignedRem;
    k.isCheckedSigned = isCheckedSignedDiv || isCheckedSignedRem;
    k.needsDiv0Check = k.isCheckedSigned || isCheckedUnsignedDiv || isCheckedUnsignedRem;
    return k;
}

/// @brief Find or create a trap block (div0 / overflow) for the current function.
/// @details Reuses an existing block matching @p label when present, patching in
///          any missing CALL/UD2 to keep the trap shape canonical. Otherwise a
///          fresh block is appended to @p fn. The resolved index is cached in
///          @p trapIndex so subsequent calls return in O(1).
/// @param fn Function that owns or will receive the trap block.
/// @param label Unique trap-block label.
/// @param callee Runtime trap routine invoked by the block.
/// @param trapIndex Optional cached block index, populated before return.
/// @return Index of the canonical trap block in @c fn.blocks.
[[nodiscard]] std::size_t ensureTrapBlock(MFunction &fn,
                                          const std::string &label,
                                          const char *callee,
                                          std::optional<std::size_t> &trapIndex) {
    if (trapIndex)
        return *trapIndex;

    if (auto existing = findBlockIndex(fn, label)) {
        trapIndex = *existing;
        auto &trapBlock = fn.blocks[*trapIndex];
        /// Match a direct call to the requested runtime trap routine.
        const bool hasCall = std::any_of(
            trapBlock.instructions.begin(), trapBlock.instructions.end(), [&](const MInstr &instr) {
                return instr.opcode == MOpcode::CALL && !instr.operands.empty() &&
                       std::holds_alternative<OpLabel>(instr.operands[0]) &&
                       std::get<OpLabel>(instr.operands[0]).name == callee;
            });
        if (!hasCall) {
            trapBlock.append(
                MInstr::make(MOpcode::CALL, std::vector<Operand>{makeLabelOperand(callee)}));
        }
        /// Recognize the non-returning illegal-instruction terminator.
        const bool hasTerminator =
            std::any_of(trapBlock.instructions.begin(),
                        trapBlock.instructions.end(),
                        [](const MInstr &instr) { return instr.opcode == MOpcode::UD2; });
        if (!hasTerminator)
            trapBlock.append(MInstr::make(MOpcode::UD2));
        return *trapIndex;
    }

    MBasicBlock trapBlock{};
    trapBlock.label = label;
    trapBlock.append(MInstr::make(MOpcode::CALL, std::vector<Operand>{makeLabelOperand(callee)}));
    trapBlock.append(MInstr::make(MOpcode::UD2));
    fn.blocks.push_back(std::move(trapBlock));
    trapIndex = fn.blocks.size() - 1U;
    return *trapIndex;
}

/// @brief Try replacing an unsigned div/rem by a power-of-2 constant with a
///        shift/mask sequence in-place.
/// @param block Block containing the candidate pseudo.
/// @param instrIdx Candidate index, advanced to the final inserted instruction.
/// @param kind Decoded opcode behavior.
/// @param candidate Original div/rem pseudo.
/// @param dividendOp Dividend operand.
/// @param divisorOp Virtual register whose local constant definition is sought.
/// @return true if the rewrite happened (and @p instrIdx was advanced past the
///         emitted instruction(s)). false if the dispatcher must fall through
///         to the generic IDIV/DIV expansion.
[[nodiscard]] bool tryLowerDivByPowerOfTwo(MBasicBlock &block,
                                           std::size_t &instrIdx,
                                           const DivOpcodeKind &kind,
                                           const MInstr &candidate,
                                           const Operand &dividendOp,
                                           const Operand &divisorOp) {
    const bool isUnsigned = !kind.isSigned;
    if (!isUnsigned)
        return false;
    auto constVal = findVRegConstant(block, instrIdx, divisorOp);
    if (!constVal)
        return false;
    const int log = log2IfPowerOf2(*constVal);
    if (log < 0 || log > 63)
        return false;

    const Operand destClone = cloneOperand(candidate.operands[0]);
    const Operand dividendClone = cloneOperand(dividendOp);

    // SHR/AND are in-place ⇒ first move dividend → dest.
    if (std::holds_alternative<OpImm>(dividendClone)) {
        block.instructions[instrIdx] = MInstr::make(
            MOpcode::MOVri,
            std::vector<Operand>{cloneOperand(destClone), cloneOperand(dividendClone)});
    } else {
        block.instructions[instrIdx] = MInstr::make(
            MOpcode::MOVrr,
            std::vector<Operand>{cloneOperand(destClone), cloneOperand(dividendClone)});
    }

    if (*constVal == 1) {
        if (!kind.isDiv) {
            block.instructions[instrIdx] = MInstr::make(
                MOpcode::MOVri, std::vector<Operand>{cloneOperand(destClone), makeImmOperand(0)});
        }
        return true;
    }

    if (kind.isDiv) {
        // udiv x, 2^k → shr x, k
        block.instructions.insert(
            block.instructions.begin() + static_cast<std::ptrdiff_t>(instrIdx + 1),
            MInstr::make(MOpcode::SHRri,
                         std::vector<Operand>{cloneOperand(destClone), makeImmOperand(log)}));
        ++instrIdx; // skip the inserted SHR
        return true;
    }

    // urem x, 2^k → and x, (2^k - 1)
    const int64_t mask = *constVal - 1;
    if (fitsSignedImm32(mask)) {
        block.instructions.insert(
            block.instructions.begin() + static_cast<std::ptrdiff_t>(instrIdx + 1),
            MInstr::make(MOpcode::ANDri,
                         std::vector<Operand>{cloneOperand(destClone), makeImmOperand(mask)}));
        ++instrIdx;
        return true;
    }
    const Operand scratchRegOp = makePhysRegOperand(PhysReg::R11);
    block.instructions.insert(
        block.instructions.begin() + static_cast<std::ptrdiff_t>(instrIdx + 1),
        MInstr::make(MOpcode::MOVri,
                     std::vector<Operand>{cloneOperand(scratchRegOp), makeImmOperand(mask)}));
    block.instructions.insert(
        block.instructions.begin() + static_cast<std::ptrdiff_t>(instrIdx + 2),
        MInstr::make(MOpcode::ANDrr,
                     std::vector<Operand>{cloneOperand(destClone), cloneOperand(scratchRegOp)}));
    instrIdx += 2;
    return true;
}

/// @brief Try replacing a div/rem by a non-trivial constant with a
///        magic-multiply sequence (Hacker's Delight §10), avoiding the 20-40
///        cycle IDIV/DIV entirely.
/// @details Handles signed divisors |d| >= 2 (including signed powers of two
///          via the bias-shift sequence) and unsigned non-power-of-2 divisors
///          >= 3 (unsigned powers of two are handled by
///          tryLowerDivByPowerOfTwo). Checked forms qualify too: a non-zero
///          constant divisor satisfies the div0 check statically, and d != -1
///          rules out the INT_MIN / -1 overflow. The emitted sequence mirrors
///          the AArch64 peephole's proven expansions.
/// @param block Block containing the candidate pseudo.
/// @param instrIdx Candidate index, advanced to the final replacement instruction.
/// @param kind Decoded quotient/remainder and signedness behavior.
/// @param candidate Original pseudo supplying the destination.
/// @param dividendOp Register dividend read by the generated sequence.
/// @param divisorOp Virtual register whose constant definition is sought.
/// @return true if the pseudo was rewritten in place (and @p instrIdx points
///         at the last emitted instruction).
[[nodiscard]] bool tryLowerDivByMagic(MBasicBlock &block,
                                      std::size_t &instrIdx,
                                      const DivOpcodeKind &kind,
                                      const MInstr &candidate,
                                      const Operand &dividendOp,
                                      const Operand &divisorOp) {
    // The sequence reads the dividend several times; require a register.
    if (!std::holds_alternative<OpReg>(dividendOp))
        return false;
    auto constVal = findVRegConstant(block, instrIdx, divisorOp);
    if (!constVal)
        return false;
    const int64_t divisor = *constVal;

    const Operand dest = cloneOperand(candidate.operands[0]);
    const Operand dividend = cloneOperand(dividendOp);
    const Operand rax = makePhysRegOperand(PhysReg::RAX);
    const Operand rdx = makePhysRegOperand(PhysReg::RDX);
    const Operand r10 = makePhysRegOperand(PhysReg::R10);
    const Operand r11 = makePhysRegOperand(PhysReg::R11);

    std::vector<MInstr> seq;
    /// Append a two-operand instruction while cloning both source descriptors.
    const auto emit2 = [&](MOpcode op, const Operand &a, const Operand &b) {
        seq.push_back(MInstr::make(op, std::vector<Operand>{cloneOperand(a), cloneOperand(b)}));
    };
    /// Append an immediate shift of @p reg by @p amount.
    const auto emitShift = [&](MOpcode op, const Operand &reg, int amount) {
        seq.push_back(MInstr::make(
            op, std::vector<Operand>{cloneOperand(reg), makeImmOperand(amount)}));
    };

    // Emits quotient computation; returns the operand holding the quotient.
    Operand quotient = rdx;
    if (kind.isSigned) {
        if (divisor == 0 || divisor == 1 || divisor == -1 ||
            divisor == std::numeric_limits<int64_t>::min())
            return false;
        const int64_t absDivisor = divisor < 0 ? -divisor : divisor;
        const int log = log2IfPowerOf2(absDivisor);
        if (log >= 1) {
            // Signed power of two: bias by (2^k - 1) for negative dividends,
            // then arithmetic shift.
            emit2(MOpcode::MOVrr, r11, dividend);
            emitShift(MOpcode::SARri, r11, 63);
            emitShift(MOpcode::SHRri, r11, 64 - log);
            emit2(MOpcode::ADDrr, r11, dividend);
            emitShift(MOpcode::SARri, r11, log);
            quotient = r11;
        } else {
            const zanna::codegen::MagicNumber magic =
                zanna::codegen::computeSignedMagic(absDivisor);
            if (magic.multiplier == 0)
                return false;
            emit2(MOpcode::MOVrr, rax, dividend);
            seq.push_back(MInstr::make(
                MOpcode::MOVri,
                std::vector<Operand>{cloneOperand(r11), makeImmOperand(magic.multiplier)}));
            seq.push_back(
                MInstr::make(MOpcode::IMULr, std::vector<Operand>{cloneOperand(r11)}));
            if (magic.needsAdd)
                emit2(MOpcode::ADDrr, rdx, dividend);
            if (magic.shift > 0)
                emitShift(MOpcode::SARri, rdx, magic.shift);
            emit2(MOpcode::MOVrr, r11, dividend);
            emitShift(MOpcode::SHRri, r11, 63);
            emit2(MOpcode::ADDrr, rdx, r11);
            quotient = rdx;
        }
        if (divisor < 0) {
            // Negate: r10 = 0 - q.
            seq.push_back(MInstr::make(
                MOpcode::MOVri, std::vector<Operand>{cloneOperand(r10), makeImmOperand(0)}));
            emit2(MOpcode::SUBrr, r10, quotient);
            quotient = r10;
        }
    } else {
        const uint64_t unsignedDivisor = static_cast<uint64_t>(divisor);
        if (unsignedDivisor <= 1)
            return false;
        const auto magic = zanna::codegen::computeUnsignedMagic(unsignedDivisor);
        if (!magic.has_value())
            return false; // powers of two take the shift path
        emit2(MOpcode::MOVrr, rax, dividend);
        seq.push_back(MInstr::make(
            MOpcode::MOVri,
            std::vector<Operand>{cloneOperand(r11),
                                 makeImmOperand(static_cast<int64_t>(magic->multiplier))}));
        seq.push_back(MInstr::make(MOpcode::MULr, std::vector<Operand>{cloneOperand(r11)}));
        if (magic->needsAdd) {
            emit2(MOpcode::MOVrr, r11, dividend);
            emit2(MOpcode::SUBrr, r11, rdx);
            emitShift(MOpcode::SHRri, r11, 1);
            emit2(MOpcode::ADDrr, rdx, r11);
        }
        if (magic->shift > 0)
            emitShift(MOpcode::SHRri, rdx, static_cast<int>(magic->shift));
        quotient = rdx;
    }

    if (kind.isDiv) {
        emit2(MOpcode::MOVrr, dest, quotient);
    } else {
        // remainder = dividend - quotient * divisor (low 64 bits are sign
        // agnostic).
        emit2(MOpcode::MOVrr, rax, quotient);
        seq.push_back(MInstr::make(
            MOpcode::MOVri, std::vector<Operand>{cloneOperand(r10), makeImmOperand(divisor)}));
        emit2(MOpcode::IMULrr, rax, r10);
        emit2(MOpcode::MOVrr, dest, dividend);
        emit2(MOpcode::SUBrr, dest, rax);
    }

    block.instructions.erase(block.instructions.begin() +
                             static_cast<std::ptrdiff_t>(instrIdx));
    block.instructions.insert(block.instructions.begin() +
                                  static_cast<std::ptrdiff_t>(instrIdx),
                              std::make_move_iterator(seq.begin()),
                              std::make_move_iterator(seq.end()));
    instrIdx += seq.size() - 1U;
    return true;
}

/// @brief Emit the INT_MIN / -1 overflow guard for a checked signed div/rem.
/// @details For checked div the overflow path jumps to the trap label; for
///          checked rem the result is forced to 0 and control jumps to
///          @p afterLabel. On the non-overflow path control falls through to a
///          @c LABEL marking the post-guard join point.
/// @param currentBlock Block extended with the guard sequence.
/// @param fn Function providing unique local labels.
/// @param raxOp RAX operand containing the materialized dividend.
/// @param divisorRegOp Register containing the divisor.
/// @param scratchRegOp Scratch GPR used to materialize @c INT64_MIN.
/// @param destOp Pseudo destination used for the checked-remainder special case.
/// @param dividendClone Original dividend used for constant-folding the first test.
/// @param kind Decoded operation flags; must describe a checked signed pseudo.
/// @param ovfTrapLabel Shared overflow-trap block label.
/// @param afterLabel Continuation label for the checked-remainder special case.
void emitDivOverflowGuards(MBasicBlock &currentBlock,
                           MFunction &fn,
                           const Operand &raxOp,
                           const Operand &divisorRegOp,
                           const Operand &scratchRegOp,
                           const Operand &destOp,
                           const Operand &dividendClone,
                           const DivOpcodeKind &kind,
                           const std::string &ovfTrapLabel,
                           const std::string &afterLabel) {
    const std::string skipOverflowLabel = fn.makeLocalLabel(currentBlock.label + ".divchk_skip");
    const bool dividendIsImmediate = std::holds_alternative<OpImm>(dividendClone);
    const bool dividendIsMin = dividendIsImmediate && std::get<OpImm>(dividendClone).val ==
                                                          std::numeric_limits<int64_t>::min();

    if (!dividendIsImmediate) {
        currentBlock.append(MInstr::make(
            MOpcode::MOVri,
            std::vector<Operand>{cloneOperand(scratchRegOp),
                                 makeImmOperand(std::numeric_limits<int64_t>::min())}));
        currentBlock.append(MInstr::make(
            MOpcode::CMPrr, std::vector<Operand>{cloneOperand(raxOp), cloneOperand(scratchRegOp)}));
        currentBlock.append(MInstr::make(
            MOpcode::JCC,
            std::vector<Operand>{makeImmOperand(1), makeLabelOperand(skipOverflowLabel)}));
    } else if (!dividendIsMin) {
        currentBlock.append(
            MInstr::make(MOpcode::JMP, std::vector<Operand>{makeLabelOperand(skipOverflowLabel)}));
    }

    currentBlock.append(MInstr::make(
        MOpcode::CMPri, std::vector<Operand>{cloneOperand(divisorRegOp), makeImmOperand(-1)}));
    currentBlock.append(
        MInstr::make(MOpcode::JCC,
                     std::vector<Operand>{makeImmOperand(1), makeLabelOperand(skipOverflowLabel)}));

    if (kind.isDiv) {
        currentBlock.append(
            MInstr::make(MOpcode::JMP, std::vector<Operand>{makeLabelOperand(ovfTrapLabel)}));
    } else {
        currentBlock.append(MInstr::make(
            MOpcode::MOVri, std::vector<Operand>{cloneOperand(destOp), makeImmOperand(0)}));
        currentBlock.append(
            MInstr::make(MOpcode::JMP, std::vector<Operand>{makeLabelOperand(afterLabel)}));
    }

    currentBlock.append(
        MInstr::make(MOpcode::LABEL, std::vector<Operand>{makeLabelOperand(skipOverflowLabel)}));
}

/// @brief Emit the canonical CQO/IDIV (signed) or XOR/DIV (unsigned) sequence
///        and copy the quotient or remainder into @p destOp.
/// @param currentBlock Block extended with the architectural division sequence.
/// @param raxOp RAX operand holding the low dividend and eventual quotient.
/// @param rdxOp RDX operand holding the high dividend and eventual remainder.
/// @param divisorRegOp Explicit divisor register, distinct from RAX and RDX.
/// @param destOp Destination that receives the selected result.
/// @param kind Decoded signedness and quotient-versus-remainder behavior.
void emitDivOrIdiv(MBasicBlock &currentBlock,
                   const Operand &raxOp,
                   const Operand &rdxOp,
                   const Operand &divisorRegOp,
                   const Operand &destOp,
                   const DivOpcodeKind &kind) {
    if (kind.isSigned) {
        currentBlock.append(MInstr::make(MOpcode::CQO, {}));
        currentBlock.append(
            MInstr::make(MOpcode::IDIVrm, std::vector<Operand>{cloneOperand(divisorRegOp)}));
    } else {
        currentBlock.append(MInstr::make(
            MOpcode::XORrr32, std::vector<Operand>{cloneOperand(rdxOp), cloneOperand(rdxOp)}));
        currentBlock.append(
            MInstr::make(MOpcode::DIVrm, std::vector<Operand>{cloneOperand(divisorRegOp)}));
    }
    const Operand resultPhys = kind.isDiv ? raxOp : rdxOp;
    currentBlock.append(MInstr::make(
        MOpcode::MOVrr, std::vector<Operand>{cloneOperand(destOp), cloneOperand(resultPhys)}));
}

} // namespace

/// @brief Rewrite division and remainder pseudos into explicit guarded sequences.
///
/// @details Walks each machine basic block in search of signed or unsigned
///          integer division pseudos.  Matching instructions are replaced with a
///          guarded control-flow pattern: the divisor is tested for zero, a
///          shared trap block is invoked when necessary, and otherwise the
///          CQO/IDIV (signed) or XOR/DIV (unsigned) sequence executes using the
///          architecturally implicit RDX:RAX register pair.  The
///          remaining instructions from the original block are moved into a
///          freshly created continuation block so the program order remains
///          intact after the branch sequence.  A single trap block is allocated
///          lazily and reused for every lowered pseudo within the function.
///
/// @param fn Machine IR function being rewritten in place.
/// @throws std::runtime_error If a matched pseudo has missing or unsupported operands.
void lowerSignedDivRem(MFunction &fn) {
    const std::string trapLabel = ".Ltrap_div0_" + fn.name;
    const std::string ovfTrapLabel = ".Ltrap_ovf_" + fn.name;
    std::optional<std::size_t> div0TrapIndex{};
    std::optional<std::size_t> ovfTrapIndex{};
    unsigned sequenceId{0U};

    for (std::size_t blockIdx = 0; blockIdx < fn.blocks.size(); ++blockIdx) {
        for (std::size_t instrIdx = 0; instrIdx < fn.blocks[blockIdx].instructions.size();
             ++instrIdx) {
            const MInstr &candidate = fn.blocks[blockIdx].instructions[instrIdx];
            const DivOpcodeKind kind = classifyDivOpcode(candidate.opcode);
            if (!kind.matched)
                continue;
            if (candidate.operands.size() < 3U)
                throw std::runtime_error("x86-64 div lowering: pseudo requires dest/lhs/rhs");
            if (!std::holds_alternative<OpReg>(candidate.operands[0]))
                throw std::runtime_error(
                    "x86-64 div lowering: pseudo destination must be a register");

            const Operand &dividendOp = candidate.operands[1];
            const Operand &divisorOp = candidate.operands[2];
            const bool dividendSupported = std::holds_alternative<OpReg>(dividendOp) ||
                                           std::holds_alternative<OpImm>(dividendOp);
            if (!dividendSupported)
                throw std::runtime_error(
                    "x86-64 div lowering: pseudo dividend must be a register or immediate");
            if (!std::holds_alternative<OpReg>(divisorOp))
                throw std::runtime_error("x86-64 div lowering: pseudo divisor must be a register");

            if (tryLowerDivByPowerOfTwo(
                    fn.blocks[blockIdx], instrIdx, kind, candidate, dividendOp, divisorOp))
                continue;

            if (tryLowerDivByMagic(
                    fn.blocks[blockIdx], instrIdx, kind, candidate, dividendOp, divisorOp))
                continue;

            MInstr pseudo = std::move(fn.blocks[blockIdx].instructions[instrIdx]);

            MBasicBlock afterBlock{};
            afterBlock.label = makeContinuationLabel(fn, fn.blocks[blockIdx], sequenceId++);
            {
                auto &block = fn.blocks[blockIdx];
                const auto tailBegin =
                    block.instructions.begin() + static_cast<std::ptrdiff_t>(instrIdx + 1U);
                afterBlock.instructions.assign(std::make_move_iterator(tailBegin),
                                               std::make_move_iterator(block.instructions.end()));
                block.instructions.erase(tailBegin, block.instructions.end());
                block.instructions.erase(block.instructions.begin() +
                                         static_cast<std::ptrdiff_t>(instrIdx));
            }

            if (kind.needsDiv0Check)
                (void)ensureTrapBlock(fn, trapLabel, "rt_trap_div0", div0TrapIndex);
            if (kind.isCheckedSigned && kind.isDiv)
                (void)ensureTrapBlock(fn, ovfTrapLabel, "rt_trap_ovf", ovfTrapIndex);

            auto &currentBlock = fn.blocks[blockIdx];

            const Operand destOp = cloneOperand(pseudo.operands[0]);
            const Operand dividendClone = cloneOperand(dividendOp);
            const Operand divisorClone = cloneOperand(divisorOp);
            const Operand raxOp = makePhysRegOperand(PhysReg::RAX);
            const Operand rdxOp = makePhysRegOperand(PhysReg::RDX);
            const Operand divisorRegOp = makePhysRegOperand(PhysReg::R10);
            const Operand scratchRegOp = makePhysRegOperand(PhysReg::R11);

            // Materialise operands before any local branches. The register
            // allocator tracks cached virtual-register locations linearly within
            // a MachineIR block; using vregs after the overflow-check labels can
            // otherwise reuse a register loaded only on one branch path.
            currentBlock.append(MInstr::make(
                MOpcode::MOVrr,
                std::vector<Operand>{cloneOperand(divisorRegOp), cloneOperand(divisorClone)}));
            if (kind.needsDiv0Check) {
                currentBlock.append(MInstr::make(
                    MOpcode::TESTrr,
                    std::vector<Operand>{cloneOperand(divisorRegOp), cloneOperand(divisorRegOp)}));
                currentBlock.append(MInstr::make(
                    MOpcode::JCC,
                    std::vector<Operand>{makeImmOperand(0), makeLabelOperand(trapLabel)}));
            }

            if (std::holds_alternative<OpImm>(dividendClone)) {
                currentBlock.append(MInstr::make(
                    MOpcode::MOVri,
                    std::vector<Operand>{cloneOperand(raxOp), cloneOperand(dividendClone)}));
            } else {
                currentBlock.append(MInstr::make(
                    MOpcode::MOVrr,
                    std::vector<Operand>{cloneOperand(raxOp), cloneOperand(dividendClone)}));
            }

            if (kind.isCheckedSigned) {
                emitDivOverflowGuards(currentBlock,
                                      fn,
                                      raxOp,
                                      divisorRegOp,
                                      scratchRegOp,
                                      destOp,
                                      dividendClone,
                                      kind,
                                      ovfTrapLabel,
                                      afterBlock.label);
            }

            // IDIV/DIV implicitly consume RDX:RAX, so the explicit divisor must
            // not be allocated to either register. Keep it in R10 so the
            // post-check division reads a value available on every non-trap path.
            emitDivOrIdiv(currentBlock, raxOp, rdxOp, divisorRegOp, destOp, kind);

            currentBlock.append(MInstr::make(
                MOpcode::JMP, std::vector<Operand>{makeLabelOperand(afterBlock.label)}));

            const std::size_t nextInstrIdx = currentBlock.instructions.size();
            fn.blocks.push_back(std::move(afterBlock));
            instrIdx = nextInstrIdx;
        }
    }
}

} // namespace zanna::codegen::x64
