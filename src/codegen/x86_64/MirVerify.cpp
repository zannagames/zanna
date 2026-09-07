//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/MirVerify.cpp
// Purpose: Implements the x86-64 MIR verifier: structural checks at every
//          stage and physical-register dataflow checks after allocation.
// Key invariants:
//   - Register facts come from effectsOf()/operandRoles(); the physical
//     liveness solved here uses the allocator's CFG classifier
//     (ra::classifyControlFlow), so it sees the edges RA saw.
//   - Reports are capped per function so one broken function stays readable.
// Ownership/Lifetime:
//   - Stateless; all containers are function-local.
// Links: src/codegen/x86_64/MirVerify.hpp,
//        src/codegen/x86_64/OperandRoles.cpp,
//        src/codegen/x86_64/ra/Liveness.cpp,
//        src/codegen/common/ra/CfgExtract.hpp
//
//===----------------------------------------------------------------------===//

#include "codegen/x86_64/MirVerify.hpp"

#include "codegen/common/ra/CfgExtract.hpp"
#include "codegen/x86_64/OperandRoles.hpp"
#include "codegen/x86_64/ra/Liveness.hpp"

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

/// @file
/// @brief Implements verifyMir() for x86-64 MIR.

namespace zanna::codegen::x64 {

namespace {

/// @brief Upper bound on diagnostics emitted for one function.
constexpr std::size_t kMaxReportsPerFunction = 24;

/// @brief Byte distance from RBP to the first incoming stack argument.
constexpr int32_t kIncomingArgBase = 16;

/// @brief Whether @p stage is at or beyond @p floor in pipeline order.
[[nodiscard]] bool atLeast(VerifyStage stage, VerifyStage floor) noexcept {
    return static_cast<unsigned>(stage) >= static_cast<unsigned>(floor);
}

/// @brief Collects violations for one function and forwards them as diagnostics.
class Reporter {
  public:
    Reporter(const MFunction &fn, VerifyStage stage, common::Diagnostics &diags)
        : fn_(fn), stage_(stage), diags_(diags) {}

    void error(const char *rule,
               const MBasicBlock *block,
               const MInstr *instr,
               const std::string &what) {
        ok_ = false;
        if (++count_ > kMaxReportsPerFunction)
            return;
        std::ostringstream msg;
        msg << "mir verify [" << verifyStageName(stage_) << "] function '" << fn_.name << "'";
        if (block != nullptr)
            msg << " block '" << block->label << "'";
        msg << ": " << what;
        if (instr != nullptr)
            msg << " at '" << toString(*instr) << "'";
        if (count_ == kMaxReportsPerFunction)
            msg << " (further reports for this function suppressed)";
        diags_.error(std::string("V-CG-MIR-") + rule, msg.str());
    }

    [[nodiscard]] bool ok() const noexcept {
        return ok_;
    }

  private:
    const MFunction &fn_;
    VerifyStage stage_;
    common::Diagnostics &diags_;
    std::size_t count_{0};
    bool ok_{true};
};

/// @brief Whether @p opc unconditionally ends a block's instruction stream.
[[nodiscard]] bool isHardTerminator(MOpcode opc) noexcept {
    return opc == MOpcode::RET || opc == MOpcode::JMP || opc == MOpcode::UD2 ||
           opc == MOpcode::JUMPTABLE;
}

/// @brief Whether a physical-register ordinal names a real x86-64 register.
[[nodiscard]] bool isValidPhysOrdinal(uint16_t ordinal) noexcept {
    return ordinal <= static_cast<uint16_t>(PhysReg::XMM15);
}

/// @brief Physical registers named by @p mi's explicit register operands.
[[nodiscard]] PhysRegMask operandRegs(const MInstr &mi) noexcept {
    PhysRegMask mask = 0;
    for (const auto &op : mi.operands) {
        if (const auto *reg = std::get_if<OpReg>(&op)) {
            if (reg->isPhys && isValidPhysOrdinal(reg->idOrPhys))
                mask |= physRegBit(static_cast<PhysReg>(reg->idOrPhys));
        }
    }
    return mask;
}

/// @brief Physical registers explicitly defined by @p mi according to operandRoles.
[[nodiscard]] PhysRegMask explicitDefs(const MInstr &mi) noexcept {
    PhysRegMask mask = 0;
    for (std::size_t idx = 0; idx < mi.operands.size(); ++idx) {
        const auto *reg = std::get_if<OpReg>(&mi.operands[idx]);
        if (reg == nullptr || !reg->isPhys || !isValidPhysOrdinal(reg->idOrPhys))
            continue;
        if (operandRoles(mi, idx).second)
            mask |= physRegBit(static_cast<PhysReg>(reg->idOrPhys));
    }
    return mask;
}

/// @brief The reserved scratch registers the lowering keeps out of allocation.
[[nodiscard]] PhysRegMask reservedScratch() noexcept {
    return physRegBit(PhysReg::R10) | physRegBit(PhysReg::R11);
}

/// @brief Registers that may legitimately be live on entry to a function.
[[nodiscard]] PhysRegMask entryInputs(const TargetInfo &target) noexcept {
    PhysRegMask mask = 0;
    for (PhysReg reg : target.intArgOrder)
        mask |= physRegBit(reg);
    for (PhysReg reg : target.f64ArgOrder)
        mask |= physRegBit(reg);
    for (PhysReg reg : target.calleeSavedGPR)
        mask |= physRegBit(reg);
    for (PhysReg reg : target.calleeSavedFPR)
        mask |= physRegBit(reg);
    mask |= physRegBit(PhysReg::RSP) | physRegBit(PhysReg::RBP);
    mask |= physRegBit(PhysReg::RAX); // AL carries the vararg vector count
    return mask;
}

/// @brief Render a register mask as a comma-separated register list.
[[nodiscard]] std::string describe(PhysRegMask mask) {
    std::string out;
    for (unsigned bit = 0; bit <= static_cast<unsigned>(PhysReg::XMM15); ++bit) {
        if ((mask & (PhysRegMask{1} << bit)) == 0)
            continue;
        if (!out.empty())
            out += ", ";
        out += regName(static_cast<PhysReg>(bit));
    }
    return out;
}

// -----------------------------------------------------------------------------
// Structural rules (every stage)
// -----------------------------------------------------------------------------

void checkStructure(const MFunction &fn, Reporter &report) {
    std::unordered_map<std::string, std::size_t> blockIndex;
    for (std::size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        if (!blockIndex.emplace(fn.blocks[bi].label, bi).second)
            report.error("DUP-LABEL", &fn.blocks[bi], nullptr, "duplicate block label");
    }

    std::unordered_map<uint16_t, RegClass> vregClass;

    /// Check one register reference for class consistency and valid ordinals.
    const auto checkReg = [&](const OpReg &reg, const MBasicBlock &block, const MInstr &mi) {
        if (reg.isPhys) {
            if (!isValidPhysOrdinal(reg.idOrPhys)) {
                report.error("REG-CLASS",
                             &block,
                             &mi,
                             "physical register ordinal " + std::to_string(reg.idOrPhys) +
                                 " is out of range");
                return;
            }
            const auto phys = static_cast<PhysReg>(reg.idOrPhys);
            if ((reg.cls == RegClass::GPR) != isGPR(phys)) {
                report.error("REG-CLASS",
                             &block,
                             &mi,
                             std::string("register ") + regName(phys) +
                                 " carries the wrong register class");
            }
            return;
        }
        auto [it, inserted] = vregClass.emplace(reg.idOrPhys, reg.cls);
        if (!inserted && it->second != reg.cls) {
            report.error("VREG-CLASS",
                         &block,
                         &mi,
                         "virtual register %v" + std::to_string(reg.idOrPhys) +
                             " is used with two register classes");
        }
    };

    /// Require operand @p idx to be a label naming a block of this function.
    const auto requireBlock = [&](const MInstr &mi, std::size_t idx, const MBasicBlock &block) {
        const OpLabel *label =
            idx < mi.operands.size() ? std::get_if<OpLabel>(&mi.operands[idx]) : nullptr;
        if (label == nullptr) {
            report.error(
                "LABEL", &block, &mi, "branch is missing its label operand " + std::to_string(idx));
            return;
        }
        if (blockIndex.find(label->name) == blockIndex.end()) {
            report.error(
                "LABEL", &block, &mi, "branch target '" + label->name + "' does not name a block");
        }
    };

    for (const auto &block : fn.blocks) {
        bool seenTerminator = false;
        for (const auto &mi : block.instructions) {
            if (seenTerminator)
                report.error("AFTER-TERM", &block, &mi, "instruction follows a block terminator");
            if (isHardTerminator(mi.opcode))
                seenTerminator = true;

            if (mi.opcode == MOpcode::LABEL) {
                report.error(
                    "LABEL-INBLOCK", &block, &mi, "in-block LABEL pseudo survived block splitting");
            }

            for (const auto &op : mi.operands) {
                if (const auto *reg = std::get_if<OpReg>(&op)) {
                    checkReg(*reg, block, mi);
                } else if (const auto *mem = std::get_if<OpMem>(&op)) {
                    checkReg(mem->base, block, mi);
                    if (mem->base.cls != RegClass::GPR)
                        report.error("MEM", &block, &mi, "memory base is not a GPR");
                    if (mem->hasIndex) {
                        checkReg(mem->index, block, mi);
                        if (mem->index.cls != RegClass::GPR)
                            report.error("MEM", &block, &mi, "memory index is not a GPR");
                        if (mem->scale != 1 && mem->scale != 2 && mem->scale != 4 &&
                            mem->scale != 8)
                            report.error("MEM", &block, &mi, "memory scale is not 1/2/4/8");
                    }
                }
            }

            if (mi.opcode == MOpcode::JMP) {
                if (!mi.operands.empty() && std::holds_alternative<OpLabel>(mi.operands[0]))
                    requireBlock(mi, 0, block);
                else if (mi.operands.empty() || !std::holds_alternative<OpReg>(mi.operands[0]))
                    report.error("LABEL", &block, &mi, "jump has neither a label nor a register");
            } else if (mi.opcode == MOpcode::JCC) {
                requireBlock(mi, 1, block);
            } else if (mi.opcode == MOpcode::JUMPTABLE) {
                // [0]=index, [1]=table symbol, [2..]=case labels.
                if (mi.operands.size() < 3)
                    report.error("LABEL", &block, &mi, "jump table has no case labels");
                for (std::size_t k = 2; k < mi.operands.size(); ++k)
                    requireBlock(mi, k, block);
            } else if (mi.opcode == MOpcode::CALL) {
                if (mi.operands.empty() || (!std::holds_alternative<OpLabel>(mi.operands[0]) &&
                                            !std::holds_alternative<OpReg>(mi.operands[0])))
                    report.error("CALL", &block, &mi, "call target is neither label nor register");
            }
        }
    }

    if (!fn.blocks.empty()) {
        const auto &last = fn.blocks.back();
        const bool ends =
            !last.instructions.empty() && isHardTerminator(last.instructions.back().opcode);
        if (!ends) {
            report.error("FALLOFF",
                         &last,
                         last.instructions.empty() ? nullptr : &last.instructions.back(),
                         "last block does not end in a terminator");
        }
    }
}

// -----------------------------------------------------------------------------
// Post-RA rules
// -----------------------------------------------------------------------------

struct PhysLiveness {
    std::vector<PhysRegMask> liveIn;
    std::vector<PhysRegMask> liveOut;
};

[[nodiscard]] PhysLiveness computePhysLiveness(const MFunction &fn, const TargetInfo &target) {
    const std::size_t n = fn.blocks.size();
    std::unordered_map<std::string, std::size_t> blockIndex;
    for (std::size_t bi = 0; bi < n; ++bi)
        blockIndex.emplace(fn.blocks[bi].label, bi);

    const auto succs = zanna::codegen::ra::extractSuccessors(
        fn.blocks,
        blockIndex,
        [](const MBasicBlock &bb) -> const std::vector<MInstr> & { return bb.instructions; },
        [](const MInstr &mi) { return ra::classifyControlFlow(mi); });

    std::vector<PhysRegMask> gen(n, 0);
    std::vector<PhysRegMask> kill(n, 0);
    for (std::size_t bi = 0; bi < n; ++bi) {
        for (const auto &mi : fn.blocks[bi].instructions) {
            const InstrEffects fx = effectsOf(mi, target);
            gen[bi] |= fx.uses & ~kill[bi];
            kill[bi] |= fx.defs;
        }
    }

    PhysLiveness result;
    result.liveIn.assign(n, 0);
    result.liveOut.assign(n, 0);
    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t bi = n; bi-- > 0;) {
            PhysRegMask out = 0;
            for (std::size_t s : succs[bi])
                out |= result.liveIn[s];
            const PhysRegMask in = gen[bi] | (out & ~kill[bi]);
            if (out != result.liveOut[bi] || in != result.liveIn[bi]) {
                result.liveOut[bi] = out;
                result.liveIn[bi] = in;
                changed = true;
            }
        }
    }
    return result;
}

void checkPostRA(const MFunction &fn,
                 const FrameInfo &frame,
                 const TargetInfo &target,
                 Reporter &report) {
    bool physOnly = true;
    PhysRegMask calleeSaved = 0;
    for (PhysReg reg : target.calleeSavedGPR)
        calleeSaved |= physRegBit(reg);
    for (PhysReg reg : target.calleeSavedFPR)
        calleeSaved |= physRegBit(reg);
    // The frame registers are written by the prologue/epilogue themselves.
    calleeSaved &= ~(physRegBit(PhysReg::RBP) | physRegBit(PhysReg::RSP));
    PhysRegMask saved = 0;
    for (PhysReg reg : frame.usedCalleeSaved)
        saved |= physRegBit(reg);

    const int32_t spLimit = std::max(frame.outgoingArgArea, kSlotSizeBytes);

    for (const auto &block : fn.blocks) {
        for (const auto &mi : block.instructions) {
            bool hasVreg = false;
            for (const auto &op : mi.operands) {
                if (const auto *reg = std::get_if<OpReg>(&op)) {
                    hasVreg = hasVreg || !reg->isPhys;
                } else if (const auto *mem = std::get_if<OpMem>(&op)) {
                    hasVreg = hasVreg || !mem->base.isPhys || (mem->hasIndex && !mem->index.isPhys);
                }
            }
            if (hasVreg) {
                physOnly = false;
                report.error("VREG", &block, &mi, "virtual register remains after allocation");
                continue;
            }

            for (const auto &op : mi.operands) {
                const auto *mem = std::get_if<OpMem>(&op);
                if (mem == nullptr)
                    continue;
                const auto base = static_cast<PhysReg>(mem->base.idOrPhys);
                if (base == PhysReg::RBP && !mem->hasIndex) {
                    const bool inLocals = mem->disp >= -frame.frameSize && mem->disp < 0;
                    const bool inIncoming = mem->disp >= kIncomingArgBase;
                    if (!inLocals && !inIncoming) {
                        report.error("FRAME-OFFSET",
                                     &block,
                                     &mi,
                                     "rbp-relative displacement " + std::to_string(mem->disp) +
                                         " lies outside the " + std::to_string(frame.frameSize) +
                                         "-byte frame");
                    }
                } else if (base == PhysReg::RSP && !mem->hasIndex) {
                    if (mem->disp < 0 || mem->disp >= spLimit) {
                        report.error("SP-OFFSET",
                                     &block,
                                     &mi,
                                     "rsp-relative displacement " + std::to_string(mem->disp) +
                                         " lies outside the " + std::to_string(spLimit) +
                                         "-byte outgoing area");
                    }
                }
            }

            const PhysRegMask unsaved = explicitDefs(mi) & calleeSaved & ~saved;
            if (unsaved != 0) {
                report.error("CALLEE-SAVE",
                             &block,
                             &mi,
                             "writes callee-saved " + describe(unsaved) +
                                 " which is not in the frame's save list");
            }
        }
    }

    if (!physOnly)
        return;

    const PhysLiveness lv = computePhysLiveness(fn, target);
    const PhysRegMask scratch = reservedScratch();

    for (std::size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        const auto &block = fn.blocks[bi];
        PhysRegMask live = lv.liveOut[bi];
        for (std::size_t ii = block.instructions.size(); ii-- > 0;) {
            const auto &mi = block.instructions[ii];
            const InstrEffects fx = effectsOf(mi, target);
            PhysRegMask implicit = fx.defs & ~explicitDefs(mi);
            if (!fx.isCall)
                implicit &= ~operandRegs(mi);
            const PhysRegMask hazard = implicit & live & scratch;
            if (hazard != 0) {
                report.error("SCRATCH-CLOBBER",
                             &block,
                             &mi,
                             "reserved scratch " + describe(hazard) +
                                 " is live across an instruction that clobbers it");
            }
            live &= ~fx.defs;
            live |= fx.uses;
        }
    }

    if (!lv.liveIn.empty()) {
        const PhysRegMask badEntry = lv.liveIn.front() & ~entryInputs(target);
        if (badEntry != 0) {
            report.error("ENTRY-LIVEIN",
                         &fn.blocks.front(),
                         nullptr,
                         "registers read before any definition: " + describe(badEntry));
        }
    }
}

} // namespace

const char *verifyStageName(VerifyStage stage) noexcept {
    switch (stage) {
        case VerifyStage::PostLegalize:
            return "post-legalize";
        case VerifyStage::PostRA:
            return "post-ra";
        case VerifyStage::PostSchedule:
            return "post-schedule";
        case VerifyStage::PostPeephole:
            return "post-peephole";
    }
    return "unknown";
}

bool verifyMir(const MFunction &fn,
               const FrameInfo &frame,
               VerifyStage stage,
               const TargetInfo &target,
               common::Diagnostics &diags) {
    Reporter report(fn, stage, diags);
    checkStructure(fn, report);
    if (atLeast(stage, VerifyStage::PostRA))
        checkPostRA(fn, frame, target, report);
    return report.ok();
}

bool mirVerificationRequested() noexcept {
    if (const char *value = std::getenv("ZANNA_VERIFY_MIR"))
        return value[0] != '\0' && value[0] != '0';
    return false;
}

} // namespace zanna::codegen::x64
