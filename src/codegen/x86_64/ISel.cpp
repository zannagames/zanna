//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/ISel.cpp
// Purpose: Define instruction selection helpers that map pseudo Machine IR
//          emitted by LowerILToMIR into concrete x86-64 encodings.
// Key invariants:
//   - Transformations preserve instruction ordering while rewriting
//     opcode/operand combinations to legal encodings (e.g. cmp immediate
//     forms or inserting movzx after setcc).
//   - Resulting instruction streams remain valid for register allocation
//     and emission.
// Ownership/Lifetime:
//   - Operates entirely in-place on Machine IR borrowed from callers without
//     allocating persistent auxiliary structures.
// Links: src/codegen/x86_64/ISel.hpp,
//        src/codegen/x86_64/MachineIR.hpp
//
//===----------------------------------------------------------------------===//

#include "ISel.hpp"
#include "OperandRoles.hpp"
#include "OperandUtils.hpp"
#include "Unsupported.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

/**
 * @file
 * @brief Implements x86-64 MIR legalization, select expansion, and address folds.
 *
 * Selection is deliberately performed before register allocation. Helpers use
 * operand-role metadata and whole-function virtual-register use checks to
 * preserve destructive two-operand semantics, EFLAGS observability, and values
 * that cross basic-block boundaries while reducing MIR to encodable forms.
 */

namespace zanna::codegen::x64 {

namespace {

/// @brief Ensure a zero-extension follows a @c setcc instruction.
///
/// @details The lowering pipeline expects boolean results to be materialised as
///          0/1 integers.  `setcc` writes a byte, so this helper inserts a
///          `movzx` when the subsequent instruction does not already perform the
///          zero-extension.  The helper scans for the destination register,
///          reuses it as both operands of the new instruction, and inserts the
///          @c movzx immediately after @p index in @p block.
///
/// @param block Machine basic block containing the @c setcc.
/// @param index Index of the @c setcc instruction within the block.
void ensureMovzxAfterSetcc(MBasicBlock &block, std::size_t index) {
    if (index >= block.instructions.size()) {
        return;
    }
    auto &setcc = block.instructions[index];
    Operand *destOperand = nullptr;
    for (auto &operand : setcc.operands) {
        if (std::holds_alternative<OpReg>(operand)) {
            destOperand = &operand;
            break;
        }
    }
    if (!destOperand) {
        return;
    }

    if (const auto *destReg = asReg(*destOperand); destReg && destReg->cls != RegClass::GPR) {
        return;
    }

    if (index + 1 < block.instructions.size()) {
        auto &next = block.instructions[index + 1];
        if (next.opcode == MOpcode::MOVZXrr8 && next.operands.size() >= 2 &&
            sameRegister(next.operands[0], *destOperand) &&
            sameRegister(next.operands[1], *destOperand)) {
            return;
        }
    }

    MInstr movzx =
        MInstr::make(MOpcode::MOVZXrr8,
                     std::vector<Operand>{cloneOperand(*destOperand), cloneOperand(*destOperand)});
    block.instructions.insert(block.instructions.begin() + static_cast<std::ptrdiff_t>(index + 1),
                              std::move(movzx));
}

/// @brief Predicate: does @p value fit into a signed 32-bit immediate?
/// @details The x86-64 ALU and compare encodings accept a signed-imm32 form;
///          immediates outside that range must be materialised into a register
///          before use. This helper centralises the bound check so legalisers
///          stay self-consistent.
/// @param value 64-bit candidate immediate.
/// @return True if @p value lies in [INT32_MIN, INT32_MAX].
[[nodiscard]] bool fitsSignedImm32(int64_t value) noexcept {
    return value >= std::numeric_limits<int32_t>::min() &&
           value <= std::numeric_limits<int32_t>::max();
}

/// @brief Track the maximum virtual-register id seen so far.
/// @details Helper used while scanning the function to find the next available
///          virtual register id; only virtual (non-physical) operands update
///          the running maximum.
/// @param reg Register descriptor to observe.
/// @param maxId Running maximum updated for a virtual register.
void observeVirtualRegister(const OpReg &reg, uint16_t &maxId) noexcept {
    if (!reg.isPhys) {
        maxId = std::max(maxId, reg.idOrPhys);
    }
}

/// @brief Compute the next free virtual register id for @p func.
/// @details Scans every operand of every instruction in every block, observing
///          both top-level register operands and the base/index registers
///          embedded inside memory operands. Returns @c max(seen) + 1, so the
///          result can be used directly as the next allocation id without
///          colliding with existing vregs.
/// @param func Machine function to scan.
/// @return The smallest unused virtual GPR/XMM id (callers typically split
///         by class outside this helper since the id space is shared).
[[nodiscard]] uint32_t nextVirtualGprId(const MFunction &func) {
    uint16_t maxId = 0;
    for (const auto &block : func.blocks) {
        for (const auto &instr : block.instructions) {
            for (const auto &operand : instr.operands) {
                if (const auto *reg = asReg(operand)) {
                    observeVirtualRegister(*reg, maxId);
                    continue;
                }

                const auto *mem = asMem(operand);
                if (!mem) {
                    continue;
                }
                observeVirtualRegister(mem->base, maxId);
                if (mem->hasIndex) {
                    observeVirtualRegister(mem->index, maxId);
                }
            }
        }
    }
    return static_cast<uint32_t>(maxId) + 1U;
}

/// @brief Allocate a fresh GPR-class virtual register and return it as an operand.
/// @details Bumps @p nextVreg in place. Traps via @ref phaseAUnsupported when
///          the running counter would overflow the 16-bit operand id space —
///          this is a synthetic ceiling that protects encoders and analyses
///          which rely on @c uint16_t register ids.
/// @param nextVreg Running counter updated in place; receives the new id.
/// @return @c Operand carrying the freshly allocated virtual GPR.
/// @throws std::runtime_error Through @c phaseAUnsupported if ids are exhausted.
[[nodiscard]] Operand makeTempGprOperand(uint32_t &nextVreg) {
    if (nextVreg >= std::numeric_limits<uint16_t>::max()) {
        phaseAUnsupported("too many virtual registers in function");
    }
    return makeVRegOperand(RegClass::GPR, static_cast<uint16_t>(nextVreg++));
}

/// @brief Hoist an un-encodable immediate operand into a temporary GPR.
/// @details When an instruction's RHS holds an immediate that cannot be encoded
///          as a 32-bit signed displacement, this routine inserts a preceding
///          @c MOVri that materialises the immediate into a freshly allocated
///          virtual GPR and rewrites the original instruction to consume the
///          register form (@p registerOpcode) instead. The MOVri inherits the
///          source location of the instruction it precedes so diagnostics stay
///          attributable to the originating IL site.
/// @param block Machine basic block being legalised in place.
/// @param index Index of the candidate instruction within @c block.instructions.
/// @param nextVreg Running counter used to allocate the temporary virtual reg id.
/// @param registerOpcode Register-register opcode to substitute when lifting.
/// @return True when a MOVri was inserted (caller must adjust its iterator).
/// @throws std::runtime_error Through @c phaseAUnsupported if ids are exhausted.
bool materialiseImmediateRhs(MBasicBlock &block,
                             std::size_t index,
                             uint32_t &nextVreg,
                             MOpcode registerOpcode) {
    if (index >= block.instructions.size()) {
        return false;
    }

    auto &instr = block.instructions[index];
    if (instr.operands.size() < 2 || !isImm(instr.operands[1])) {
        return false;
    }

    const Operand immediate = cloneOperand(instr.operands[1]);
    const Operand temp = makeTempGprOperand(nextVreg);
    MInstr materialise =
        MInstr::make(MOpcode::MOVri, std::vector<Operand>{cloneOperand(temp), immediate});
    materialise.loc = instr.loc;

    instr.opcode = registerOpcode;
    instr.operands[1] = cloneOperand(temp);
    block.instructions.insert(block.instructions.begin() + static_cast<std::ptrdiff_t>(index),
                              std::move(materialise));
    return true;
}

/// @brief Normalise CMP opcodes and materialise unencodable immediates.
/// @param block Basic block containing the comparison.
/// @param index Candidate instruction index.
/// @param nextVreg Running temporary-register counter.
/// @return True when a MOVri was inserted before @p index.
bool legaliseCmp(MBasicBlock &block, std::size_t index, uint32_t &nextVreg) {
    if (index >= block.instructions.size()) {
        return false;
    }

    auto &instr = block.instructions[index];
    if (instr.operands.size() < 2) {
        return false;
    }

    if ((instr.opcode == MOpcode::CMPrr || instr.opcode == MOpcode::CMPri) &&
        isImm(instr.operands[1])) {
        const auto &imm = std::get<OpImm>(instr.operands[1]);
        if (fitsSignedImm32(imm.val)) {
            instr.opcode = MOpcode::CMPri;
            return false;
        }
        return materialiseImmediateRhs(block, index, nextVreg, MOpcode::CMPrr);
    }

    if (instr.opcode == MOpcode::CMPri && !isImm(instr.operands[1])) {
        instr.opcode = MOpcode::CMPrr;
    }
    return false;
}

/// @brief Attempt to fold an LEA address expression into a downstream memory use.
/// @details Combines @p leaMem (the LEA's effective address) with @p useMem
///          (a load/store memory operand) into a single SIB-form address. The
///          fold fails when both expressions already use an index register —
///          x86-64 SIB addressing has room for only one — or when the summed
///          displacement overflows the signed-imm32 range.
/// @param leaMem Memory expression produced by the LEA being folded.
/// @param useMem Memory expression of the consuming instruction.
/// @return The combined memory operand, or @c std::nullopt if no legal fusion exists.
[[nodiscard]] std::optional<OpMem> combineLeaMemUse(const OpMem &leaMem, const OpMem &useMem) {
    if (leaMem.hasIndex && useMem.hasIndex) {
        return std::nullopt;
    }

    const int64_t combinedDisp = static_cast<int64_t>(leaMem.disp) + useMem.disp;
    if (combinedDisp < static_cast<int64_t>(std::numeric_limits<int32_t>::min()) ||
        combinedDisp > static_cast<int64_t>(std::numeric_limits<int32_t>::max())) {
        return std::nullopt;
    }

    OpMem combined = leaMem;
    combined.disp = static_cast<int32_t>(combinedDisp);
    if (useMem.hasIndex) {
        combined.index = useMem.index;
        combined.scale = useMem.scale;
        combined.hasIndex = true;
    }
    return combined;
}

/// @brief Canonicalise add/sub opcodes to legal immediate/register forms.
///
/// @details Instruction selection prefers `add` with signed-imm32 immediates
///          because those map directly to x86-64 ALU encodings. Larger
///          immediates, or subtraction immediates whose negation cannot be
///          encoded, are materialised in a temporary GPR.
/// @param block Basic block containing the arithmetic instruction.
/// @param index Candidate instruction index.
/// @param nextVreg Running temporary-register counter.
/// @return True when a MOVri was inserted before @p index.
bool legaliseAddSub(MBasicBlock &block, std::size_t index, uint32_t &nextVreg) {
    auto &instr = block.instructions[index];
    if (instr.operands.size() < 2) {
        return false;
    }
    switch (instr.opcode) {
        case MOpcode::ADDrr:
        case MOpcode::ADDri:
            if (isImm(instr.operands[1])) {
                const auto &imm = std::get<OpImm>(instr.operands[1]);
                if (fitsSignedImm32(imm.val)) {
                    instr.opcode = MOpcode::ADDri;
                    return false;
                }
                return materialiseImmediateRhs(block, index, nextVreg, MOpcode::ADDrr);
            }
            if (instr.opcode == MOpcode::ADDri) {
                instr.opcode = MOpcode::ADDrr;
            }
            break;
        case MOpcode::SUBrr:
            if (auto *imm = asImm(instr.operands[1])) {
                if (imm->val != std::numeric_limits<int64_t>::min()) {
                    const int64_t negated = -imm->val;
                    if (fitsSignedImm32(negated)) {
                        imm->val = negated;
                        instr.opcode = MOpcode::ADDri;
                        return false;
                    }
                }
                return materialiseImmediateRhs(block, index, nextVreg, MOpcode::SUBrr);
            }
            break;
        default:
            break;
    }
    return false;
}

/// @brief Canonicalise bitwise opcodes to legal operand kinds and zeroing idioms.
///
/// @details Normalises AND/OR/XOR to use immediate forms when the second operand
///          is a signed-imm32 literal and falls back to register forms otherwise.
///          Larger constants are materialised into temporary GPRs. Additionally
///          rewrites register self-XOR into the canonical 32-bit self-XOR used by
///          later peephole passes to recognise zeroing patterns.
/// @param block Basic block containing the bitwise instruction.
/// @param index Candidate instruction index.
/// @param nextVreg Running temporary-register counter.
/// @return True when a MOVri was inserted before @p index.
bool legaliseBitwise(MBasicBlock &block, std::size_t index, uint32_t &nextVreg) {
    auto &instr = block.instructions[index];
    if (instr.operands.size() < 2) {
        return false;
    }

    /// Select an immediate or register form for one bitwise opcode pair.
    const auto convertForm = [&](MOpcode rr, MOpcode ri) -> bool {
        if (instr.opcode != rr && instr.opcode != ri) {
            return false;
        }

        if (isImm(instr.operands[1])) {
            const auto &imm = std::get<OpImm>(instr.operands[1]);
            if (fitsSignedImm32(imm.val)) {
                instr.opcode = ri;
                return false;
            }
            return materialiseImmediateRhs(block, index, nextVreg, rr);
        }

        if (instr.opcode == ri && !isImm(instr.operands[1])) {
            instr.opcode = rr;
        }
        return false;
    };

    if (convertForm(MOpcode::ANDrr, MOpcode::ANDri) || convertForm(MOpcode::ORrr, MOpcode::ORri) ||
        convertForm(MOpcode::XORrr, MOpcode::XORri)) {
        return true;
    }

    if (instr.opcode == MOpcode::XORrr) {
        if (sameRegister(instr.operands[0], instr.operands[1])) {
            const auto *dst = asReg(instr.operands[0]);
            if (dst && dst->cls == RegClass::GPR) {
                instr.opcode = MOpcode::XORrr32;
                instr.operands[1] = cloneOperand(instr.operands[0]);
            }
        }
        return false;
    }

    // Do not rewrite XORri $0 as a self-XOR.  "x ^ 0" is an identity, while
    // "xor r32, r32" zeroes the destination.
    return false;
}

/// @brief Append the appropriate GPR-class move for @p src into @p out.
/// @details Chooses between immediate-to-register (@c MOVri) and register-to-register
///          (@c MOVrr) based on the variant of @p src. Memory and label operands are
///          rejected (the caller is expected to use a more specialised emitter for
///          loads). Both operands are cloned so the caller can keep ownership of the
///          originals.
/// @param out Instruction stream the synthesised move is appended to.
/// @param dst Destination GPR operand.
/// @param src Source operand; must be an immediate or register.
/// @return True if a move was appended, false if @p src was not a supported kind.
bool appendGprMove(std::vector<MInstr> &out, const Operand &dst, const Operand &src) {
    if (std::holds_alternative<OpImm>(src)) {
        out.push_back(MInstr::make(MOpcode::MOVri,
                                   std::vector<Operand>{cloneOperand(dst), cloneOperand(src)}));
        return true;
    }
    if (std::holds_alternative<OpReg>(src)) {
        out.push_back(MInstr::make(MOpcode::MOVrr,
                                   std::vector<Operand>{cloneOperand(dst), cloneOperand(src)}));
        return true;
    }
    return false;
}

/// @brief Attempt to lower a GPR select pseudo into TEST/MOV/CMOV sequence.
///
/// @details Matches the explicit SELECT_GPR pseudo emitted by LowerILToMIR and
///          rebuilds it as a flags-setting TEST followed by MOV (false path)
///          and CMOVNE (true path).
///
/// @param func Machine function supplying the label allocator.
/// @param block Machine basic block undergoing transformation.
/// @param index Index of the candidate SELECT_GPR instruction within the block.
/// @return @c true when the pseudo was replaced.
bool lowerGprSelect(MFunction &func, MBasicBlock &block, std::size_t index) {
    auto &selectInstr = block.instructions[index];
    if (selectInstr.opcode != MOpcode::SELECT_GPR || selectInstr.operands.size() != 4) {
        return false;
    }

    const auto *destReg = asReg(selectInstr.operands[0]);
    if (!destReg || destReg->cls != RegClass::GPR) {
        return false;
    }

    const Operand &condVal = selectInstr.operands[1];
    const Operand &falseVal = selectInstr.operands[2];
    const Operand &trueVal = selectInstr.operands[3];
    if (!std::holds_alternative<OpReg>(condVal)) {
        return false;
    }

    std::vector<MInstr> replacement{};
    replacement.push_back(MInstr::make(
        MOpcode::TESTrr, std::vector<Operand>{cloneOperand(condVal), cloneOperand(condVal)}));

    if (!appendGprMove(replacement, selectInstr.operands[0], falseVal)) {
        return false;
    }

    if (std::holds_alternative<OpReg>(trueVal)) {
        replacement.push_back(MInstr::make(
            MOpcode::CMOVNErr,
            std::vector<Operand>{cloneOperand(selectInstr.operands[0]), cloneOperand(trueVal)}));
    } else if (std::holds_alternative<OpImm>(trueVal)) {
        const std::string doneLabel = func.makeLocalLabel(".Lselect_gpr_done");
        replacement.push_back(MInstr::make(
            MOpcode::JCC, std::vector<Operand>{makeImmOperand(0), makeLabelOperand(doneLabel)}));
        if (!appendGprMove(replacement, selectInstr.operands[0], trueVal)) {
            return false;
        }
        replacement.push_back(
            MInstr::make(MOpcode::LABEL, std::vector<Operand>{makeLabelOperand(doneLabel)}));
    } else {
        return false;
    }

    auto beginIt = block.instructions.begin() + static_cast<std::ptrdiff_t>(index);
    block.instructions.erase(beginIt);
    // Recalculate insert position since erase invalidates iterators
    block.instructions.insert(block.instructions.begin() + static_cast<std::ptrdiff_t>(index),
                              replacement.begin(),
                              replacement.end());
    return true;
}

/// @brief Lower XMM select pseudos into TEST/JCC/MOVSD branch sequences.
///
/// @details Matches the explicit SELECT_XMM pseudo emitted by the IL bridge and
///          rewrites it into a small branchy sequence using unique local
///          labels. The rewritten sequence uses `movsd` for both true and false
///          paths and explicit jumps to the join point so later layout changes
///          cannot turn the false arm into a fallthrough past the epilogue.
///
/// @param func Machine function supplying the label allocator.
/// @param block Machine basic block containing the pattern.
/// @param index Index of the SELECT_XMM pseudo inside @p block.
/// @return @c true when a select pattern was rewritten.
bool lowerXmmSelect(MFunction &func, MBasicBlock &block, std::size_t index) {
    auto &selectInstr = block.instructions[index];
    if (selectInstr.opcode != MOpcode::SELECT_XMM || selectInstr.operands.size() != 4) {
        return false;
    }

    const auto *destReg = asReg(selectInstr.operands[0]);
    if (!destReg || destReg->cls != RegClass::XMM) {
        return false;
    }

    const Operand &condVal = selectInstr.operands[1];
    const Operand &falseVal = selectInstr.operands[2];
    const Operand &trueVal = selectInstr.operands[3];
    if (!std::holds_alternative<OpReg>(falseVal) || !std::holds_alternative<OpReg>(trueVal)) {
        return false;
    }
    if (!std::holds_alternative<OpReg>(condVal)) {
        return false;
    }

    const std::string falseLabel = func.makeLocalLabel(".Lfalse");
    const std::string endLabel = func.makeLocalLabel(".Lend");

    std::vector<MInstr> replacement{};
    replacement.push_back(MInstr::make(
        MOpcode::TESTrr, std::vector<Operand>{cloneOperand(condVal), cloneOperand(condVal)}));
    replacement.push_back(MInstr::make(
        MOpcode::JCC, std::vector<Operand>{makeImmOperand(0), makeLabelOperand(falseLabel)}));
    replacement.push_back(MInstr::make(
        MOpcode::MOVSDrr,
        std::vector<Operand>{cloneOperand(selectInstr.operands[0]), cloneOperand(trueVal)}));
    replacement.push_back(
        MInstr::make(MOpcode::JMP, std::vector<Operand>{makeLabelOperand(endLabel)}));
    replacement.push_back(
        MInstr::make(MOpcode::LABEL, std::vector<Operand>{makeLabelOperand(falseLabel)}));
    replacement.push_back(MInstr::make(
        MOpcode::MOVSDrr,
        std::vector<Operand>{cloneOperand(selectInstr.operands[0]), cloneOperand(falseVal)}));
    replacement.push_back(
        MInstr::make(MOpcode::JMP, std::vector<Operand>{makeLabelOperand(endLabel)}));
    replacement.push_back(
        MInstr::make(MOpcode::LABEL, std::vector<Operand>{makeLabelOperand(endLabel)}));

    auto beginIt = block.instructions.begin() + static_cast<std::ptrdiff_t>(index);
    block.instructions.erase(beginIt);
    block.instructions.insert(block.instructions.begin() + static_cast<std::ptrdiff_t>(index),
                              replacement.begin(),
                              replacement.end());
    return true;
}

/// @brief Determine whether @p instr reads @p needle as a use (not a def).
/// @details Walks every operand role-pair, ignoring def positions. Memory
///          operands are recursed: both the base and (optional) index register
///          are checked because either may carry a use of the queried register
///          even though the operand itself is a memory expression.
/// @param instr Instruction to inspect.
/// @param needle Register operand to look for.
/// @return True if any use slot or memory operand references @p needle.
bool instructionUsesRegister(const MInstr &instr, const Operand &needle) {
    for (std::size_t idx = 0; idx < instr.operands.size(); ++idx) {
        const auto [isUse, isDef] = operandRoles(instr, idx);
        (void)isDef;
        if (!isUse) {
            continue;
        }

        if (sameRegister(instr.operands[idx], needle)) {
            return true;
        }

        const auto *mem = std::get_if<OpMem>(&instr.operands[idx]);
        if (!mem) {
            continue;
        }
        if (sameRegister(Operand{mem->base}, needle)) {
            return true;
        }
        if (mem->hasIndex && sameRegister(Operand{mem->index}, needle)) {
            return true;
        }
    }
    return false;
}

/// @brief Tally per-instruction virtual register uses into @p useCount.
/// @details Templated on @c CountMap so the same scan can populate either a
///          @c std::vector<uint32_t> (indexed by vreg id) or a sparse map.
///          Physical registers are skipped because they are not subject to
///          allocation. Memory operands contribute uses for both base and
///          index components, which mirrors the legaliser's view of operand
///          dependencies.
/// @tparam CountMap Indexable container supporting @c operator[](uint16_t).
/// @param instr Instruction whose use operands are inspected.
/// @param useCount Accumulator updated in place.
template <typename CountMap>
void countVirtualRegisterUses(const MInstr &instr, CountMap &useCount) {
    for (std::size_t idx = 0; idx < instr.operands.size(); ++idx) {
        const auto [isUse, isDef] = operandRoles(instr, idx);
        (void)isDef;
        if (!isUse) {
            continue;
        }

        if (const auto *reg = asReg(instr.operands[idx])) {
            if (!reg->isPhys) {
                ++useCount[reg->idOrPhys];
            }
            continue;
        }

        const auto *mem = std::get_if<OpMem>(&instr.operands[idx]);
        if (!mem) {
            continue;
        }
        if (!mem->base.isPhys) {
            ++useCount[mem->base.idOrPhys];
        }
        if (mem->hasIndex && !mem->index.isPhys) {
            ++useCount[mem->index.idOrPhys];
        }
    }
}

/// @brief Count occurrences of virtual register @p vreg as a use within @p instr.
/// @details Same scanning rules as @ref instructionUsesRegister but returns a
///          count rather than a boolean so callers can detect single-use
///          versus multi-use patterns (important for fold legality).
/// @param instr Instruction to scan.
/// @param vreg Virtual register id to count.
/// @return Number of distinct use positions referencing @p vreg.
std::size_t countVirtualRegisterUsesInInstr(const MInstr &instr, uint16_t vreg) {
    std::size_t count = 0;
    for (std::size_t idx = 0; idx < instr.operands.size(); ++idx) {
        const auto [isUse, isDef] = operandRoles(instr, idx);
        (void)isDef;
        if (!isUse) {
            continue;
        }

        if (const auto *reg = asReg(instr.operands[idx])) {
            if (!reg->isPhys && reg->idOrPhys == vreg) {
                ++count;
            }
            continue;
        }

        const auto *mem = std::get_if<OpMem>(&instr.operands[idx]);
        if (!mem) {
            continue;
        }
        if (!mem->base.isPhys && mem->base.idOrPhys == vreg) {
            ++count;
        }
        if (mem->hasIndex && !mem->index.isPhys && mem->index.idOrPhys == vreg) {
            ++count;
        }
    }
    return count;
}

/// @brief Sum uses of @p vreg across [@p begin, @p end) inside @p block.
/// @details Provides a bounded view used by fold legalisers that want to
///          confirm a value's only uses lie within a specific candidate
///          window. @p end is clamped to @c block.instructions.size() so
///          callers do not need to guard against off-by-one ranges.
/// @param block Block whose instructions form the search range.
/// @param vreg Virtual register id being counted.
/// @param begin First instruction index (inclusive).
/// @param end Past-the-end instruction index (exclusive, clamped).
/// @return Number of uses of @p vreg within the inspected interval.
std::size_t countVirtualRegisterUsesInRange(const MBasicBlock &block,
                                            uint16_t vreg,
                                            std::size_t begin,
                                            std::size_t end) {
    const std::size_t limit = std::min(end, block.instructions.size());
    std::size_t count = 0;
    for (std::size_t idx = begin; idx < limit; ++idx) {
        count += countVirtualRegisterUsesInInstr(block.instructions[idx], vreg);
    }
    return count;
}

/// @brief Count every use of @p vreg across all blocks of @p func.
/// @details Equivalent to summing @ref countVirtualRegisterUsesInRange over
///          the entire function. Useful when a peephole must verify a vreg
///          is dead after rewriting (e.g. dead boolean materialisation).
/// @param func Machine function to scan.
/// @param vreg Virtual register id being counted.
/// @return Total number of use-position occurrences.
std::size_t countVirtualRegisterUsesInFunction(const MFunction &func, uint16_t vreg) {
    std::size_t count = 0;
    for (const auto &block : func.blocks) {
        for (const auto &instr : block.instructions) {
            count += countVirtualRegisterUsesInInstr(instr, vreg);
        }
    }
    return count;
}

/// @brief Check whether @p vreg is read from any block other than @p localBlock.
/// @details Local fold legalisers can combine this with a bounded same-block
///          use count before deleting a virtual-register definition. Without
///          the whole-function side of the check, a single local use can hide
///          a live use in a later basic block.
/// @param func Function whose blocks are scanned.
/// @param localBlock Block excluded from the scan.
/// @param vreg Virtual register id to find.
/// @return @c true if any other block reads @p vreg.
bool virtualRegisterUsedOutsideBlock(const MFunction &func,
                                     const MBasicBlock &localBlock,
                                     uint16_t vreg) {
    for (const auto &candidateBlock : func.blocks) {
        if (&candidateBlock == &localBlock) {
            continue;
        }
        for (const auto &instr : candidateBlock.instructions) {
            if (countVirtualRegisterUsesInInstr(instr, vreg) != 0) {
                return true;
            }
        }
    }
    return false;
}

/// @brief Compare two register descriptors for exact register identity.
/// @param lhs First register descriptor.
/// @param rhs Second register descriptor.
/// @return @c true when physicality, class, and identifier all match.
bool sameMachineRegister(const OpReg &lhs, const OpReg &rhs) noexcept {
    return lhs.isPhys == rhs.isPhys && lhs.cls == rhs.cls && lhs.idOrPhys == rhs.idOrPhys;
}

/// @brief Determine whether @p instr defines @p needle.
/// @details Fold legalizers use this to reject stale definitions. The scan is
///          role-aware so destructive two-operand instructions count as defs,
///          while memory operands only contribute address uses. A call is
///          conservatively treated as clobbering any physical register.
/// @param instr Instruction to inspect.
/// @param needle Register whose definition is queried.
/// @return @c true when @p instr defines or conservatively clobbers @p needle.
bool instructionDefinesRegister(const MInstr &instr, const OpReg &needle) {
    if (needle.isPhys && instr.opcode == MOpcode::CALL) {
        return true;
    }

    for (std::size_t idx = 0; idx < instr.operands.size(); ++idx) {
        const auto [isUse, isDef] = operandRoles(instr, idx);
        (void)isUse;
        if (!isDef) {
            continue;
        }

        const auto *reg = asReg(instr.operands[idx]);
        if (reg && sameMachineRegister(*reg, needle)) {
            return true;
        }
    }
    return false;
}

/// @brief Check whether @p reg is redefined in [@p begin, @p end).
/// @param block Block containing the bounded interval.
/// @param reg Register to track.
/// @param begin First instruction index, inclusive.
/// @param end Past-the-end instruction index, clamped to the block size.
/// @return @c true when an instruction in the interval defines @p reg.
bool registerRedefinedInRange(const MBasicBlock &block,
                              const OpReg &reg,
                              std::size_t begin,
                              std::size_t end) {
    const std::size_t limit = std::min(end, block.instructions.size());
    for (std::size_t idx = begin; idx < limit; ++idx) {
        if (instructionDefinesRegister(block.instructions[idx], reg)) {
            return true;
        }
    }
    return false;
}

/// @brief Check whether virtual GPR @p vreg is redefined in [@p begin, @p end).
/// @param block Block containing the bounded interval.
/// @param vreg Virtual GPR identifier to track.
/// @param begin First instruction index, inclusive.
/// @param end Past-the-end instruction index.
/// @return @c true when the interval redefines @p vreg.
bool virtualRegisterRedefinedInRange(const MBasicBlock &block,
                                     uint16_t vreg,
                                     std::size_t begin,
                                     std::size_t end) {
    OpReg reg{};
    reg.isPhys = false;
    reg.cls = RegClass::GPR;
    reg.idOrPhys = vreg;
    return registerRedefinedInRange(block, reg, begin, end);
}

/// @brief Check whether an address expression's base or index changes before use.
/// @param mem Address expression whose input registers are tracked.
/// @param block Block containing the interval.
/// @param begin First instruction index, inclusive.
/// @param end Past-the-end instruction index.
/// @return @c true if the base or active index is redefined in the interval.
bool addressInputsRedefinedInRange(const OpMem &mem,
                                   const MBasicBlock &block,
                                   std::size_t begin,
                                   std::size_t end) {
    if (registerRedefinedInRange(block, mem.base, begin, end)) {
        return true;
    }
    if (mem.hasIndex && registerRedefinedInRange(block, mem.index, begin, end)) {
        return true;
    }
    return false;
}

/// @brief Check whether a register has uses outside the local fold window.
/// @details The compare/branch fold may only remove boolean materialisation
///          when the materialized value feeds the local test and nothing else.
///          Block-argument edge copies live in separate synthetic blocks, so the
///          check must scan the full function rather than only the current block.
/// @param func Function whose uses are scanned.
/// @param patternBlock Block containing the fold candidate.
/// @param firstAllowedUse First local use index belonging to the pattern.
/// @param lastAllowedUse Last local use index belonging to the pattern.
/// @param needle Register operand whose other uses make the fold illegal.
/// @return @c true when a use exists outside the allowed inclusive interval.
bool registerUsedOutsideFoldPattern(const MFunction &func,
                                    const MBasicBlock &patternBlock,
                                    std::size_t firstAllowedUse,
                                    std::size_t lastAllowedUse,
                                    const Operand &needle) {
    for (const auto &candidateBlock : func.blocks) {
        for (std::size_t idx = 0; idx < candidateBlock.instructions.size(); ++idx) {
            if (&candidateBlock == &patternBlock && idx >= firstAllowedUse &&
                idx <= lastAllowedUse) {
                continue;
            }
            if (instructionUsesRegister(candidateBlock.instructions[idx], needle)) {
                return true;
            }
        }
    }
    return false;
}

/// @brief Decide whether EFLAGS is still observable after @p index.
/// @details Scans forward from @p index + 1 and returns true the moment a
///          downstream instruction is found to read EFLAGS, false when the
///          next encountered instruction unconditionally redefines EFLAGS
///          before any read. A LABEL conservatively counts as a read because
///          it may begin a basic block reachable from elsewhere.
/// @param instrs Instruction stream to walk.
/// @param index Index of the instruction whose EFLAGS production is in question.
/// @return True if any consumer reads EFLAGS before the next redefinition.
bool flagsReadBeforeClobber(const std::vector<MInstr> &instrs, std::size_t index) {
    for (std::size_t scan = index + 1; scan < instrs.size(); ++scan) {
        const MOpcode opcode = instrs[scan].opcode;
        if (usesEFlags(opcode)) {
            return true;
        }
        if (definesEFlags(opcode)) {
            return false;
        }
        if (opcode == MOpcode::LABEL) {
            return true;
        }
    }
    return false;
}

/// @brief Fold compare/setcc/test branch chains back into a direct flags branch.
/// @details Matches the common sequence:
///            cmp/ucomis
///            setcc %v
///            movzx %v, %v   ; optional
///            test  %v, %v
///            jne   target
///          and rewrites it so the branch uses the original compare flags
///          directly. This removes redundant boolean materialisation when the
///          compare result is consumed only by the terminating branch.
/// @param func Function used for whole-function boolean-use validation.
/// @param block Block containing the candidate chain.
/// @param index Index of the leading comparison.
/// @return @c true when the chain was replaced and redundant instructions erased.
bool foldCompareBranch(const MFunction &func, MBasicBlock &block, std::size_t index) {
    if (index + 3 >= block.instructions.size()) {
        return false;
    }

    /// Recognize compare opcodes whose EFLAGS can feed the replacement branch.
    const auto isCompare = [](MOpcode opcode) {
        return opcode == MOpcode::CMPrr || opcode == MOpcode::CMPri || opcode == MOpcode::UCOMIS;
    };

    auto &cmpInstr = block.instructions[index];
    if (!isCompare(cmpInstr.opcode)) {
        return false;
    }

    auto &setccInstr = block.instructions[index + 1];
    if (setccInstr.opcode != MOpcode::SETcc || setccInstr.operands.size() < 2) {
        return false;
    }

    const auto *setCond = asImm(setccInstr.operands[0]);
    const auto *setDst = asReg(setccInstr.operands[1]);
    if (!setCond || !setDst || setDst->cls != RegClass::GPR) {
        return false;
    }

    std::size_t testIndex = index + 2;
    if (block.instructions[testIndex].opcode == MOpcode::MOVZXrr8 ||
        block.instructions[testIndex].opcode == MOpcode::MOVZXrr32) {
        const auto &movzxInstr = block.instructions[testIndex];
        if (movzxInstr.operands.size() < 2 ||
            !sameRegister(movzxInstr.operands[0], setccInstr.operands[1]) ||
            !sameRegister(movzxInstr.operands[1], setccInstr.operands[1])) {
            return false;
        }
        ++testIndex;
    }

    if (testIndex + 1 >= block.instructions.size()) {
        return false;
    }

    auto &testInstr = block.instructions[testIndex];
    if (testInstr.opcode != MOpcode::TESTrr || testInstr.operands.size() < 2) {
        return false;
    }
    if (!sameRegister(testInstr.operands[0], setccInstr.operands[1]) ||
        !sameRegister(testInstr.operands[1], setccInstr.operands[1])) {
        return false;
    }

    if (registerUsedOutsideFoldPattern(func, block, index + 2, testIndex, setccInstr.operands[1])) {
        return false;
    }

    auto &jccInstr = block.instructions[testIndex + 1];
    if (jccInstr.opcode != MOpcode::JCC || jccInstr.operands.size() < 2) {
        return false;
    }
    const auto *branchCond = asImm(jccInstr.operands[0]);
    if (!branchCond || branchCond->val != 1) {
        return false;
    }
    for (std::size_t useIdx = testIndex + 2; useIdx < block.instructions.size(); ++useIdx) {
        for (const auto &operand : block.instructions[useIdx].operands) {
            if (sameRegister(operand, setccInstr.operands[1])) {
                return false;
            }
            if (const auto *mem = std::get_if<OpMem>(&operand)) {
                const Operand base = mem->base;
                if (sameRegister(base, setccInstr.operands[1])) {
                    return false;
                }
                if (mem->hasIndex) {
                    const Operand indexOp = mem->index;
                    if (sameRegister(indexOp, setccInstr.operands[1])) {
                        return false;
                    }
                }
            }
        }
    }

    jccInstr.operands[0] = makeImmOperand(setCond->val);
    auto eraseBegin = block.instructions.begin() + static_cast<std::ptrdiff_t>(index + 1);
    auto eraseEnd = block.instructions.begin() + static_cast<std::ptrdiff_t>(testIndex + 1);
    block.instructions.erase(eraseBegin, eraseEnd);
    return true;
}

/// @brief Erase a set of instruction indices from a block, deduplicated and in
///        reverse order so each erase leaves remaining indices valid.
/// @details Shared by SIB/IMUL/LEA folding passes. Indices past the block end
///          are silently skipped (defensive — callers should only push valid
///          indices but this matches the existing behaviour).
/// @param block Block whose instruction vector is shortened.
/// @param indices Candidate indices; sorted and deduplicated in place.
void eraseInstructionsReverse(MBasicBlock &block, std::vector<std::size_t> &indices) {
    std::sort(indices.begin(), indices.end(), std::greater<std::size_t>());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    for (std::size_t eraseIdx : indices) {
        if (eraseIdx < block.instructions.size()) {
            block.instructions.erase(block.instructions.begin() +
                                     static_cast<std::ptrdiff_t>(eraseIdx));
        }
    }
}

} // namespace

/// @brief Construct an instruction selector bound to a target description.
///
/// @param target Target description supplying register and ABI metadata.
ISel::ISel(const TargetInfo &target) noexcept : target_{&target} {}

/// @brief Lower arithmetic pseudos into canonical Machine IR encodings.
///
/// @details Walks every instruction in the function and normalises add/sub
///          forms via @ref legaliseAddSub.  Floating-point instructions are
///          currently emitted in legal form and therefore left untouched.  The
///          target reference is unused for Phase A but retained for future
///          expansion.
///
/// @param func Machine function undergoing selection.
void ISel::lowerArithmetic(MFunction &func) const {
    (void)target_;
    uint32_t nextVreg = nextVirtualGprId(func);
    for (auto &block : func.blocks) {
        for (std::size_t idx = 0; idx < block.instructions.size(); ++idx) {
            bool insertedMaterialise = false;
            switch (block.instructions[idx].opcode) {
                case MOpcode::ADDrr:
                case MOpcode::ADDri:
                case MOpcode::SUBrr:
                    insertedMaterialise = legaliseAddSub(block, idx, nextVreg);
                    break;
                case MOpcode::ANDrr:
                case MOpcode::ANDri:
                case MOpcode::ORrr:
                case MOpcode::ORri:
                case MOpcode::XORrr:
                case MOpcode::XORri:
                    insertedMaterialise = legaliseBitwise(block, idx, nextVreg);
                    break;
                case MOpcode::IMULrr:
                case MOpcode::FADD:
                case MOpcode::FSUB:
                case MOpcode::FMUL:
                case MOpcode::FDIV:
                    // These already encode legal register-register forms in Phase A.
                    break;
                default:
                    break;
            }
            if (insertedMaterialise) {
                ++idx;
            }
        }
    }

    // Replace IMULrr-by-small-constant (3, 5, 9) with LEA strength reductions
    // before folding LEA addresses into memory operands.
    lowerMulToLea(func);

    // Collapse shift-and-add value computations into a single LEA so the
    // synthesized address can then participate in the memory folds below.
    synthesizeValueLea(func);

    // After normalising arithmetic, fold trivial address computations into
    // users to reduce register pressure and improve addressing modes.
    foldLeaIntoMem(func);

    // Fold SHL+ADD patterns into SIB addressing modes for load/store.
    foldSibAddressing(func);
}

/// @brief Lower compare and branch constructs to legal encodings.
///
/// @details Canonicalises compare opcodes, ensures boolean materialisation via
///          @ref ensureMovzxAfterSetcc, and converts stray `test` instructions
///          with immediate operands into `cmp` against zero.  The pass operates
///          locally within each block without changing control-flow structure.
///
/// @param func Machine function undergoing selection.
void ISel::lowerCompareAndBranch(MFunction &func) const {
    (void)target_;
    uint32_t nextVreg = nextVirtualGprId(func);
    for (auto &block : func.blocks) {
        for (std::size_t idx = 0; idx < block.instructions.size(); ++idx) {
            if (foldCompareBranch(func, block, idx)) {
                if (legaliseCmp(block, idx, nextVreg)) {
                    ++idx;
                }
                continue;
            }
            auto &instr = block.instructions[idx];
            switch (instr.opcode) {
                case MOpcode::CMPrr:
                case MOpcode::CMPri:
                    if (legaliseCmp(block, idx, nextVreg)) {
                        ++idx;
                    }
                    break;
                case MOpcode::UCOMIS:
                    break;
                case MOpcode::SETcc:
                    ensureMovzxAfterSetcc(block, idx);
                    break;
                case MOpcode::TESTrr:
                    // TESTrr should always have two register operands (self-test).
                    // Do NOT rewrite to CMP $0 if an immediate sneaks through:
                    // TEST computes (reg AND imm) while CMP computes (reg - 0),
                    // which set different flags for non-zero masks.
                    break;
                default:
                    break;
            }
        }
    }
}

/// @brief Lower select constructs to ensure boolean values are extended.
///
/// @details Selection uses @ref ensureMovzxAfterSetcc to make sure any
///          `setcc` results feeding selects are promoted to full integers.  The
///          method is intentionally conservative and only inserts missing
///          zero-extensions.
///
/// @param func Machine function undergoing selection.
void ISel::lowerSelect(MFunction &func) const {
    (void)target_;
    for (auto &block : func.blocks) {
        for (std::size_t idx = 0; idx < block.instructions.size(); ++idx) {
            if (lowerXmmSelect(func, block, idx)) {
                idx += 6;
                continue;
            }

            if (lowerGprSelect(func, block, idx)) {
                idx += 2;
                continue;
            }

            auto &instr = block.instructions[idx];
            if (instr.opcode == MOpcode::SETcc) {
                ensureMovzxAfterSetcc(block, idx);
            }
        }
    }
}

/// \brief Fold SHL+ADD sequences into SIB addressing modes.
/// \details Matches patterns where a shift by 1, 2, or 3 (scale 2, 4, 8) is added
///          to a base pointer, and the result is used as a memory base. Transforms
///          into disp(base, index, scale) addressing to reduce instruction count.
///          Optimized to use O(1) map lookups instead of O(n) linear scans for
///          MOVrr definitions.
void ISel::foldSibAddressing(MFunction &func) const {
    (void)target_;

    /// @brief Candidate destructive shift definition and its SIB scale.
    struct ShlInfo {
        /// Instruction index of the SHL definition.
        std::size_t defIdx{0};
        /// Shifted virtual-register identifier.
        uint16_t srcVreg{0};
        /// Encodable SIB scale: two, four, or eight.
        uint8_t scale{1}; // 2, 4, or 8
    };

    /// @brief Candidate destructive add combining a base and shifted value.
    struct AddInfo {
        /// Instruction index of the ADD definition.
        std::size_t defIdx{0};
        /// Virtual register holding the unshifted base.
        uint16_t baseVreg{0};
        /// Virtual register holding the scaled index.
        uint16_t shiftedVreg{0};
    };

    /// @brief MOVrr definition info for O(1) lookup.
    struct MovInfo {
        /// Instruction index of the copy definition.
        std::size_t defIdx{0};
        /// Source register identifier.
        uint16_t srcVreg{0};
        /// Whether @ref srcVreg names a physical rather than virtual register.
        bool srcIsPhys{false};
    };

    for (auto &block : func.blocks) {
        std::unordered_map<uint16_t, ShlInfo> shlDefs; // result vreg -> info
        std::unordered_map<uint16_t, AddInfo> addDefs; // result vreg -> info
        std::unordered_map<uint16_t, MovInfo> movDefs; // dest vreg -> info (for O(1) lookup)

        // First pass: record SHL, ADD, and MOVrr definitions.
        for (std::size_t idx = 0; idx < block.instructions.size(); ++idx) {
            const auto &instr = block.instructions[idx];

            // Record MOVrr for O(1) lookup later (replaces O(n) linear scan)
            if (instr.opcode == MOpcode::MOVrr && instr.operands.size() >= 2) {
                const auto *dst = asReg(instr.operands[0]);
                const auto *src = asReg(instr.operands[1]);
                if (dst && src && !dst->isPhys && dst->cls == RegClass::GPR) {
                    MovInfo info{};
                    info.defIdx = idx;
                    info.srcVreg = src->idOrPhys;
                    info.srcIsPhys = src->isPhys;
                    movDefs[dst->idOrPhys] = info;
                }
            }

            // Record SHLri with shift 1, 2, or 3 (scale 2, 4, 8)
            if (instr.opcode == MOpcode::SHLri && instr.operands.size() >= 2) {
                const auto *dst = asReg(instr.operands[0]);
                const auto *shiftAmt = asImm(instr.operands[1]);
                if (dst && !dst->isPhys && dst->cls == RegClass::GPR && shiftAmt) {
                    const int64_t shift = shiftAmt->val;
                    if (shift >= 1 && shift <= 3) {
                        ShlInfo info{};
                        info.defIdx = idx;
                        info.srcVreg = dst->idOrPhys; // SHL is destructive, src == dst
                        info.scale = static_cast<uint8_t>(1 << shift);
                        shlDefs[dst->idOrPhys] = info;
                    }
                }
            }

            // Record ADDrr where one operand might be from SHL
            if (instr.opcode == MOpcode::ADDrr && instr.operands.size() >= 2) {
                const auto *dst = asReg(instr.operands[0]);
                const auto *src = asReg(instr.operands[1]);
                if (dst && src && !dst->isPhys && !src->isPhys && dst->cls == RegClass::GPR &&
                    src->cls == RegClass::GPR) {
                    // Check if src is from a SHL - then dst is base + shifted
                    if (shlDefs.count(src->idOrPhys)) {
                        AddInfo info{};
                        info.defIdx = idx;
                        info.baseVreg = dst->idOrPhys;
                        info.shiftedVreg = src->idOrPhys;
                        addDefs[dst->idOrPhys] = info;
                    }
                }
            }
        }

        // Second pass: find memory operands using ADD results and transform
        std::vector<std::size_t> toErase;
        for (std::size_t idx = 0; idx < block.instructions.size(); ++idx) {
            auto &instr = block.instructions[idx];

            for (auto &op : instr.operands) {
                auto *mem = std::get_if<OpMem>(&op);
                if (!mem || mem->hasIndex) {
                    continue; // Skip if not memory or already has index
                }

                if (mem->base.isPhys) {
                    continue;
                }

                const uint16_t baseId = mem->base.idOrPhys;
                auto addIt = addDefs.find(baseId);
                if (addIt == addDefs.end()) {
                    continue;
                }

                const AddInfo &addInfo = addIt->second;
                auto shlIt = shlDefs.find(addInfo.shiftedVreg);
                if (shlIt == shlDefs.end()) {
                    continue;
                }

                const ShlInfo &shlInfo = shlIt->second;

                if (shlInfo.defIdx >= addInfo.defIdx || addInfo.defIdx >= idx) {
                    continue;
                }
                if (virtualRegisterRedefinedInRange(
                        block, addInfo.shiftedVreg, shlInfo.defIdx + 1, addInfo.defIdx) ||
                    virtualRegisterRedefinedInRange(block, baseId, addInfo.defIdx + 1, idx)) {
                    continue;
                }

                // Check that the ADD result and SHL result are single-use after
                // their definitions. The destructive source reads of SHL/ADD
                // are not uses of the newly defined values.
                if (countVirtualRegisterUsesInRange(
                        block, baseId, addInfo.defIdx + 1, block.instructions.size()) != 1 ||
                    virtualRegisterUsedOutsideBlock(func, block, baseId)) {
                    continue;
                }
                if (countVirtualRegisterUsesInRange(block,
                                                    addInfo.shiftedVreg,
                                                    shlInfo.defIdx + 1,
                                                    block.instructions.size()) != 1 ||
                    virtualRegisterUsedOutsideBlock(func, block, addInfo.shiftedVreg)) {
                    continue;
                }

                // Find the original index register before the SHL using O(1) map lookup
                uint16_t indexVreg = shlInfo.srcVreg;
                auto movIt = movDefs.find(shlInfo.srcVreg);
                if (movIt != movDefs.end() && !movIt->second.srcIsPhys &&
                    movIt->second.defIdx < shlInfo.defIdx &&
                    !virtualRegisterRedefinedInRange(
                        block, shlInfo.srcVreg, movIt->second.defIdx + 1, shlInfo.defIdx) &&
                    !virtualRegisterRedefinedInRange(
                        block, movIt->second.srcVreg, movIt->second.defIdx + 1, idx) &&
                    countVirtualRegisterUsesInRange(
                        block, shlInfo.srcVreg, movIt->second.defIdx + 1, shlInfo.defIdx + 1) ==
                        1 &&
                    !virtualRegisterUsedOutsideBlock(func, block, shlInfo.srcVreg)) {
                    indexVreg = movIt->second.srcVreg;
                    toErase.push_back(movIt->second.defIdx); // Mark MOV for removal
                }

                // Find the original base register before ADD using O(1) map lookup
                uint16_t realBaseVreg = addInfo.baseVreg;
                auto baseMovIt = movDefs.find(addInfo.baseVreg);
                if (baseMovIt != movDefs.end() && baseMovIt->second.defIdx < addInfo.defIdx &&
                    !virtualRegisterRedefinedInRange(
                        block, addInfo.baseVreg, baseMovIt->second.defIdx + 1, addInfo.defIdx) &&
                    countVirtualRegisterUsesInRange(block,
                                                    addInfo.baseVreg,
                                                    baseMovIt->second.defIdx + 1,
                                                    addInfo.defIdx + 1) == 1 &&
                    !virtualRegisterUsedOutsideBlock(func, block, addInfo.baseVreg)) {
                    if (!baseMovIt->second.srcIsPhys &&
                        !virtualRegisterRedefinedInRange(
                            block, baseMovIt->second.srcVreg, baseMovIt->second.defIdx + 1, idx)) {
                        realBaseVreg = baseMovIt->second.srcVreg;
                        toErase.push_back(baseMovIt->second.defIdx);
                    }
                }

                // Update memory operand with SIB addressing
                if (!mem->base.isPhys) {
                    mem->base.idOrPhys = realBaseVreg;
                }
                mem->index.isPhys = false;
                mem->index.cls = RegClass::GPR;
                mem->index.idOrPhys = indexVreg;
                mem->scale = shlInfo.scale;
                mem->hasIndex = true;

                // Mark SHL and ADD for removal
                toErase.push_back(shlInfo.defIdx);
                toErase.push_back(addInfo.defIdx);
            }
        }

        eraseInstructionsReverse(block, toErase);
    }
}

/// @brief Replace IMULrr-by-small-constant with LEA strength reduction.
///
/// @details For each block, scans for the pattern:
///   MOVri  constReg, <factor>   (factor ∈ {3, 5, 9})
///   IMULrr dstReg, constReg
///
/// and rewrites it to:
///   LEA dstReg, [dstReg + dstReg * scale]
///
/// where scale = factor - 1 ∈ {2, 4, 8}.
///
/// The transformation avoids the higher latency of IMUL (3+ cycles) and
/// the flag-clobbering side-effect.  Only unchecked IMULrr is eligible;
/// IMULOvfrr carries an overflow trap and must remain as IMUL.  The
/// supplying MOVri is erased when the constant virtual register has exactly
/// one use (the IMUL itself).
///
/// @param func Machine function to transform.
void ISel::lowerMulToLea(MFunction &func) const {
    (void)target_;

    /// Map factors 3, 5, and 9 to their corresponding SIB scale.
    /// A zero result marks a factor that cannot be strength-reduced this way.
    auto factorToScale = [](int64_t factor) -> uint8_t {
        if (factor == 3)
            return 2;
        if (factor == 5)
            return 4;
        if (factor == 9)
            return 8;
        return 0; // not a LEA-eligible constant
    };

    for (auto &block : func.blocks) {
        // First pass: collect MOVri definitions of eligible constants
        // inside this block. Use legality is checked at function scope below
        // because the supplying constant register may be live into another block.
        /// @brief Eligible constant definition that may feed an IMUL.
        struct MovRiDef {
            /// Instruction index of the defining MOVri.
            std::size_t defIdx{0};
            /// Multiplication factor loaded by the definition.
            int64_t factor{0};
            /// Precomputed SIB scale equal to @ref factor minus one.
            uint8_t scale{0}; // precomputed LEA scale
        };

        std::unordered_map<uint16_t, MovRiDef> constDefs; // vreg id → MOVri info

        for (std::size_t idx = 0; idx < block.instructions.size(); ++idx) {
            const auto &instr = block.instructions[idx];

            // Record single-constant MOVri definitions.
            if (instr.opcode == MOpcode::MOVri && instr.operands.size() >= 2) {
                const auto *dst = asReg(instr.operands[0]);
                const auto *imm = asImm(instr.operands[1]);
                if (dst && !dst->isPhys && dst->cls == RegClass::GPR && imm) {
                    const uint8_t scale = factorToScale(imm->val);
                    if (scale != 0) {
                        MovRiDef def{};
                        def.defIdx = idx;
                        def.factor = imm->val;
                        def.scale = scale;
                        constDefs[dst->idOrPhys] = def;
                    }
                }
            }
        }

        // Second pass: find eligible IMULrr and rewrite.
        std::vector<std::size_t> toErase;

        for (std::size_t idx = 0; idx < block.instructions.size(); ++idx) {
            auto &instr = block.instructions[idx];

            if (instr.opcode != MOpcode::IMULrr || instr.operands.size() < 2)
                continue;

            // IMULrr layout: operands[0] = dst (also src0), operands[1] = src1
            const auto *dst = asReg(instr.operands[0]);
            const auto *src = asReg(instr.operands[1]);
            if (!dst || !src)
                continue;

            // Both registers must be virtual GPRs for this transformation.
            if (dst->isPhys || src->isPhys)
                continue;
            if (dst->cls != RegClass::GPR || src->cls != RegClass::GPR)
                continue;
            if (dst->idOrPhys == src->idOrPhys)
                continue;

            // The source register must have been defined by a MOVri with an
            // eligible factor.
            const uint16_t srcId = src->idOrPhys;
            auto defIt = constDefs.find(srcId);
            if (defIt == constDefs.end())
                continue;

            const MovRiDef &def = defIt->second;
            if (def.defIdx >= idx)
                continue;

            if (flagsReadBeforeClobber(block.instructions, idx))
                continue;

            // The constant register must have exactly one read-use in the whole
            // function (this IMUL); otherwise erasing the MOVri would break a
            // later block.
            if (countVirtualRegisterUsesInFunction(func, srcId) != 1)
                continue;

            // Build the replacement LEA: dst ← [dst + dst * scale]
            // OpMem with base == index captures dst*(1+scale) = dst*factor.
            const uint16_t dstId = dst->idOrPhys;
            OpMem leaMem{};
            leaMem.base.isPhys = false;
            leaMem.base.cls = RegClass::GPR;
            leaMem.base.idOrPhys = dstId;
            leaMem.index.isPhys = false;
            leaMem.index.cls = RegClass::GPR;
            leaMem.index.idOrPhys = dstId;
            leaMem.scale = def.scale;
            leaMem.disp = 0;
            leaMem.hasIndex = true;

            instr.opcode = MOpcode::LEA;
            instr.operands[0] = cloneOperand(instr.operands[0]); // dst unchanged
            instr.operands[1] = Operand{leaMem};

            // Trim any trailing operands (IMULrr has exactly 2, LEA has 2).
            instr.operands.resize(2);

            // Mark the supplying MOVri for removal.
            toErase.push_back(def.defIdx);
        }

        eraseInstructionsReverse(block, toErase);
    }
}

/// \brief Collapse shift-and-add value chains into a single LEA.
/// \details Matches the adjacent quadruple the lowering emits for
///          `d = x + (y << k)`:
///            MOVrr t, y / SHLri t, #k / MOVrr d, x / ADDrr d, t
///          and rewrites the ADDrr into `LEA d, [x + y * 2^k]`, erasing the
///          three feeder instructions. Requires k in 1..3 (SIB-encodable), the
///          shifted temp to have no other use, distinct registers where the
///          rewrite would otherwise read a clobbered value, and no consumer of
///          the ADDrr's flag write (LEA does not set flags).
void ISel::synthesizeValueLea(MFunction &func) const {
    (void)target_;
    /// Recognize virtual GPR descriptors accepted by value-LEA patterns.
    const auto isVirtGprOp = [](const OpReg &r) { return !r.isPhys && r.cls == RegClass::GPR; };

    for (auto &block : func.blocks) {
        auto &instrs = block.instructions;
        std::vector<std::size_t> toErase;

        // Shape A (what the lowering emits when the add reuses the shifted
        // register): MOVrr t, y / SHLri t, #k / ADDrr t, x
        //   → LEA t, [x + y * 2^k]
        for (std::size_t i = 0; i + 2 < instrs.size(); ++i) {
            const MInstr &movT = instrs[i];
            const MInstr &shl = instrs[i + 1];
            MInstr &add = instrs[i + 2];
            if (movT.opcode != MOpcode::MOVrr || shl.opcode != MOpcode::SHLri ||
                add.opcode != MOpcode::ADDrr)
                continue;
            if (movT.operands.size() < 2 || shl.operands.size() < 2 || add.operands.size() < 2)
                continue;
            const auto *t = std::get_if<OpReg>(&movT.operands[0]);
            const auto *y = std::get_if<OpReg>(&movT.operands[1]);
            const auto *shlDst = std::get_if<OpReg>(&shl.operands[0]);
            const auto *k = std::get_if<OpImm>(&shl.operands[1]);
            const auto *addDst = std::get_if<OpReg>(&add.operands[0]);
            const auto *x = std::get_if<OpReg>(&add.operands[1]);
            if (!t || !y || !shlDst || !k || !addDst || !x)
                continue;
            if (!isVirtGprOp(*t) || !isVirtGprOp(*y) || !isVirtGprOp(*x))
                continue;
            if (shlDst->idOrPhys != t->idOrPhys || addDst->idOrPhys != t->idOrPhys)
                continue;
            if (k->val < 1 || k->val > 3)
                continue;
            // The LEA reads x and y where the ADD stood; neither may alias
            // the temp the erased feeders wrote.
            if (t->idOrPhys == x->idOrPhys || t->idOrPhys == y->idOrPhys)
                continue;
            if (flagsReadBeforeClobber(instrs, i + 2))
                continue;

            OpMem leaMem{};
            leaMem.base = *x;
            leaMem.index = *y;
            leaMem.scale = static_cast<uint8_t>(1U << static_cast<unsigned>(k->val));
            leaMem.disp = 0;
            leaMem.hasIndex = true;

            add.opcode = MOpcode::LEA;
            add.operands[1] = Operand{leaMem};
            add.operands.resize(2);
            toErase.push_back(i);
            toErase.push_back(i + 1);
            i += 2;
        }
        eraseInstructionsReverse(block, toErase);
        toErase.clear();

        // Shape B (separate destination copy):
        // MOVrr t, y / SHLri t, #k / MOVrr d, x / ADDrr d, t
        //   → LEA d, [x + y * 2^k]
        for (std::size_t i = 0; i + 3 < instrs.size(); ++i) {
            const MInstr &movT = instrs[i];
            const MInstr &shl = instrs[i + 1];
            const MInstr &movD = instrs[i + 2];
            MInstr &add = instrs[i + 3];

            if (movT.opcode != MOpcode::MOVrr || shl.opcode != MOpcode::SHLri ||
                movD.opcode != MOpcode::MOVrr || add.opcode != MOpcode::ADDrr)
                continue;
            if (movT.operands.size() < 2 || shl.operands.size() < 2 || movD.operands.size() < 2 ||
                add.operands.size() < 2)
                continue;

            const auto *t = std::get_if<OpReg>(&movT.operands[0]);
            const auto *y = std::get_if<OpReg>(&movT.operands[1]);
            const auto *shlDst = std::get_if<OpReg>(&shl.operands[0]);
            const auto *k = std::get_if<OpImm>(&shl.operands[1]);
            const auto *d = std::get_if<OpReg>(&movD.operands[0]);
            const auto *x = std::get_if<OpReg>(&movD.operands[1]);
            const auto *addDst = std::get_if<OpReg>(&add.operands[0]);
            const auto *addSrc = std::get_if<OpReg>(&add.operands[1]);
            if (!t || !y || !shlDst || !k || !d || !x || !addDst || !addSrc)
                continue;

            /// Recognize virtual GPR descriptors accepted by the four-op shape.
            const auto isVirtGpr = [](const OpReg &r) {
                return !r.isPhys && r.cls == RegClass::GPR;
            };
            if (!isVirtGpr(*t) || !isVirtGpr(*y) || !isVirtGpr(*d) || !isVirtGpr(*x))
                continue;
            if (shlDst->idOrPhys != t->idOrPhys || addDst->idOrPhys != d->idOrPhys ||
                addSrc->idOrPhys != t->idOrPhys)
                continue;
            if (k->val < 1 || k->val > 3)
                continue;
            // The LEA reads x and y at the ADD's position; the erased MOVrr
            // d, x must not have clobbered either, and the temp must be
            // distinct from every surviving register.
            if (d->idOrPhys == y->idOrPhys || t->idOrPhys == d->idOrPhys ||
                t->idOrPhys == x->idOrPhys || t->idOrPhys == y->idOrPhys)
                continue;
            if (countVirtualRegisterUsesInFunction(func, t->idOrPhys) != 2)
                continue;
            if (flagsReadBeforeClobber(instrs, i + 3))
                continue;

            OpMem leaMem{};
            leaMem.base = *x;
            leaMem.index = *y;
            leaMem.scale = static_cast<uint8_t>(1U << static_cast<unsigned>(k->val));
            leaMem.disp = 0;
            leaMem.hasIndex = true;

            add.opcode = MOpcode::LEA;
            add.operands[1] = Operand{leaMem};
            add.operands.resize(2);

            toErase.push_back(i);
            toErase.push_back(i + 1);
            toErase.push_back(i + 2);
            i += 3;
        }
        eraseInstructionsReverse(block, toErase);
    }
}

/// \brief Fold single-use LEA temps into memory operands.
/// \details For each block, finds LEA-def'd virtual registers with a single use
///          as a base in a memory operand and replaces the user with the LEA's
///          addressing mode, erasing the defining LEA.
void ISel::foldLeaIntoMem(MFunction &func) const {
    (void)target_;
    for (auto &block : func.blocks) {
        std::unordered_map<uint16_t, std::size_t> leaDefIdx; // vreg id -> instr idx

        // First pass: record LEA defs.
        for (std::size_t idx = 0; idx < block.instructions.size(); ++idx) {
            const auto &instr = block.instructions[idx];
            if (instr.opcode == MOpcode::LEA && instr.operands.size() >= 2) {
                if (const auto *dst = asReg(instr.operands[0])) {
                    if (!dst->isPhys && dst->cls == RegClass::GPR) {
                        leaDefIdx[dst->idOrPhys] = idx;
                    }
                }
            }
        }

        // Second pass: try to fold at use sites and erase the LEA.
        std::vector<std::size_t> toErase;
        for (std::size_t idx = 0; idx < block.instructions.size(); ++idx) {
            auto &instr = block.instructions[idx];
            for (auto &op : instr.operands) {
                if (auto *mem = std::get_if<OpMem>(&op)) {
                    const OpReg &base = mem->base;
                    if (!base.isPhys && base.cls == RegClass::GPR) {
                        const uint16_t v = base.idOrPhys;
                        auto defIt = leaDefIdx.find(v);
                        if (defIt == leaDefIdx.end()) {
                            continue;
                        }

                        const std::size_t defIndex = defIt->second;
                        if (defIndex >= idx || defIndex >= block.instructions.size()) {
                            continue;
                        }

                        if (virtualRegisterRedefinedInRange(block, v, defIndex + 1, idx)) {
                            continue;
                        }

                        if (countVirtualRegisterUsesInFunction(func, v) != 1) {
                            continue;
                        }

                        const auto &defInstr = block.instructions[defIndex];
                        if (defInstr.opcode == MOpcode::LEA && defInstr.operands.size() >= 2) {
                            if (const auto *srcMem = std::get_if<OpMem>(&defInstr.operands[1])) {
                                if (addressInputsRedefinedInRange(
                                        *srcMem, block, defIndex + 1, idx)) {
                                    continue;
                                }
                                if (auto combined = combineLeaMemUse(*srcMem, *mem)) {
                                    *mem = *combined;
                                    toErase.push_back(defIndex);
                                    leaDefIdx.erase(defIt);
                                }
                            }
                        }
                    }
                }
            }
        }

        eraseInstructionsReverse(block, toErase);
    }
}

/// @brief Verify that no select pseudo-instructions survived ISel.
/// @details Lowering emits explicit SELECT_GPR/SELECT_XMM pseudos. They must be
///          replaced before allocation and emission.
void ISel::validateSelectLowering(const MFunction &func) const {
    for (const auto &block : func.blocks) {
        for (const auto &instr : block.instructions) {
            if (instr.opcode == MOpcode::SELECT_GPR || instr.opcode == MOpcode::SELECT_XMM) {
                phaseAUnsupported(("select pseudo survived ISel (opcode=" +
                                   std::to_string(static_cast<int>(instr.opcode)) +
                                   ", operands=" + std::to_string(instr.operands.size()) + ")")
                                      .c_str());
            }
            if ((instr.opcode == MOpcode::MOVri || instr.opcode == MOpcode::MOVrr ||
                 instr.opcode == MOpcode::MOVSDrr) &&
                instr.operands.size() > 2) {
                phaseAUnsupported(("legacy select placeholder survived ISel (opcode=" +
                                   std::to_string(static_cast<int>(instr.opcode)) +
                                   ", operands=" + std::to_string(instr.operands.size()) + ")")
                                      .c_str());
            }
        }
    }
}

} // namespace zanna::codegen::x64
