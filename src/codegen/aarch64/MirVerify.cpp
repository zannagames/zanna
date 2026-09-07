//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/MirVerify.cpp
// Purpose: Implements the AArch64 MIR verifier: structural checks at every
//          stage, physical-register dataflow checks after register
//          allocation, and immediate-encodability checks after pseudo
//          expansion.
// Key invariants:
//   - Every register fact comes from effectsOf()/ra::operandRoles; the
//     verifier adds no opcode table of its own beyond frame-offset shapes.
//   - The physical liveness solved here runs over the shared MirCfg (built
//     from ra::classifyControlFlow), so it sees the same edges RA saw.
//   - Reports are capped per function so one broken function stays readable.
// Ownership/Lifetime:
//   - Stateless; all containers are function-local.
// Links: src/codegen/aarch64/MirVerify.hpp,
//        src/codegen/aarch64/InstrEffects.cpp,
//        src/codegen/aarch64/MirCfg.hpp,
//        src/codegen/aarch64/ra/Liveness.cpp
//
//===----------------------------------------------------------------------===//

#include "codegen/aarch64/MirVerify.hpp"

#include "codegen/aarch64/InstrEffects.hpp"
#include "codegen/aarch64/MirCfg.hpp"
#include "codegen/aarch64/Noreturn.hpp"
#include "codegen/aarch64/ra/Liveness.hpp"
#include "codegen/aarch64/ra/OperandRoles.hpp"

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

/// @file
/// @brief Implements verifyMir() for AArch64 MIR.

namespace zanna::codegen::aarch64 {

namespace {

/// @brief Upper bound on diagnostics emitted for one function.
constexpr std::size_t kMaxReportsPerFunction = 24;

/// @brief Byte distance from the frame pointer to the first incoming stack argument.
constexpr long long kIncomingArgBase = 16;

/// @brief Whether @p stage is at or beyond @p floor in pipeline order.
[[nodiscard]] bool atLeast(VerifyStage stage, VerifyStage floor) noexcept {
    return static_cast<unsigned>(stage) >= static_cast<unsigned>(floor);
}

/// @brief Collects violations for one function and forwards them as diagnostics.
class Reporter {
  public:
    Reporter(const MFunction &fn, VerifyStage stage, common::Diagnostics &diags)
        : fn_(fn), stage_(stage), diags_(diags) {}

    /// @brief Record one violation.
    /// @param rule Suffix of the `V-CG-MIR-` diagnostic code.
    /// @param block Block containing the violation, or null for function-level rules.
    /// @param instr Offending instruction, or null.
    /// @param what Human-readable description.
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
            msg << " block '" << block->name << "'";
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
    return opc == MOpcode::Br || opc == MOpcode::Ret || opc == MOpcode::JumpTable;
}

/// @brief Whether @p opc is a conditional branch with its target at operand 1.
[[nodiscard]] bool isConditionalBranch(MOpcode opc) noexcept {
    return opc == MOpcode::BCond || opc == MOpcode::Cbz || opc == MOpcode::Cbnz ||
           opc == MOpcode::Tbz || opc == MOpcode::Tbnz;
}

/// @brief Whether a physical-register ordinal names a real AArch64 register.
[[nodiscard]] bool isValidPhysOrdinal(uint16_t ordinal) noexcept {
    return ordinal <= static_cast<uint16_t>(PhysReg::V31);
}

/// @brief Last immediate operand of @p mi, if any.
[[nodiscard]] std::optional<long long> lastImmediate(const MInstr &mi) noexcept {
    for (std::size_t k = mi.ops.size(); k > 0; --k) {
        if (mi.ops[k - 1].kind == MOperand::Kind::Imm)
            return mi.ops[k - 1].imm;
    }
    return std::nullopt;
}

/// @brief Bytes accessed by a frame-relative memory form (0 for address-only).
[[nodiscard]] long long frameAccessBytes(MOpcode opc) noexcept {
    switch (opc) {
        case MOpcode::Ldr8RegFpImm:
        case MOpcode::Str8RegFpImm:
            return 1;
        case MOpcode::Ldr16RegFpImm:
        case MOpcode::Str16RegFpImm:
            return 2;
        case MOpcode::Ldr32RegFpImm:
        case MOpcode::Str32RegFpImm:
            return 4;
        case MOpcode::LdrRegFpImm:
        case MOpcode::StrRegFpImm:
        case MOpcode::LdrFprFpImm:
        case MOpcode::StrFprFpImm:
        case MOpcode::PhiStoreGPR:
        case MOpcode::PhiStoreFPR:
            return 8;
        case MOpcode::LdpRegFpImm:
        case MOpcode::StpRegFpImm:
        case MOpcode::LdpFprFpImm:
        case MOpcode::StpFprFpImm:
            return 16;
        default:
            return 0;
    }
}

/// @brief Physical registers named by @p mi's explicit operands.
[[nodiscard]] PhysRegSet operandRegs(const MInstr &mi) noexcept {
    PhysRegSet set;
    for (const auto &op : mi.ops) {
        if (op.kind == MOperand::Kind::Reg && op.reg.isPhys && isValidPhysOrdinal(op.reg.idOrPhys))
            set.add(static_cast<PhysReg>(op.reg.idOrPhys));
    }
    return set;
}

/// @brief Physical registers explicitly defined by @p mi according to operandRoles.
[[nodiscard]] PhysRegSet explicitDefs(const MInstr &mi) {
    PhysRegSet set;
    for (std::size_t idx = 0; idx < mi.ops.size(); ++idx) {
        const auto &op = mi.ops[idx];
        if (op.kind != MOperand::Kind::Reg || !op.reg.isPhys ||
            !isValidPhysOrdinal(op.reg.idOrPhys))
            continue;
        if (ra::operandRoles(mi, idx).second)
            set.add(static_cast<PhysReg>(op.reg.idOrPhys));
    }
    return set;
}

/// @brief The reserved scratch registers no allocated value may occupy across a clobber.
[[nodiscard]] PhysRegSet reservedScratch() noexcept {
    PhysRegSet set = emitScratchGPRs();
    set.add(kScratchFPR);
    set.add(kScratchFPR2);
    return set;
}

/// @brief Registers that may legitimately be live on entry to a function.
[[nodiscard]] PhysRegSet entryInputs(const TargetInfo &target) noexcept {
    PhysRegSet set;
    for (PhysReg reg : target.intArgOrder)
        set.add(reg);
    for (PhysReg reg : target.f64ArgOrder)
        set.add(reg);
    for (PhysReg reg : target.calleeSavedGPR)
        set.add(reg);
    for (PhysReg reg : target.calleeSavedFPR)
        set.add(reg);
    set.add(PhysReg::SP);
    set.add(PhysReg::X29);
    set.add(PhysReg::X30);
    set.add(PhysReg::X8); // indirect result location
    return set;
}

/// @brief Render a PhysRegSet as a comma-separated register list.
[[nodiscard]] std::string describe(PhysRegSet set) {
    std::string out;
    for (unsigned bit = 0; bit < 64; ++bit) {
        if ((set.bits & (uint64_t{1} << bit)) == 0)
            continue;
        const PhysReg reg =
            bit < 32 ? static_cast<PhysReg>(bit)
                     : static_cast<PhysReg>(static_cast<unsigned>(PhysReg::V0) + (bit - 32));
        if (!out.empty())
            out += ", ";
        out += regName(reg);
    }
    return out;
}

// -----------------------------------------------------------------------------
// Structural rules (every stage)
// -----------------------------------------------------------------------------

/// @brief Run the stage-independent structural rules.
/// @return `false` when an operand failed to classify; dataflow rules are then skipped.
bool checkStructure(const MFunction &fn, Reporter &report) {
    bool rolesOk = true;

    std::unordered_map<std::string, std::size_t> blockIndex;
    for (std::size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        if (!blockIndex.emplace(fn.blocks[bi].name, bi).second)
            report.error("DUP-LABEL", &fn.blocks[bi], nullptr, "duplicate block label");
    }

    std::unordered_map<uint16_t, RegClass> vregClass;

    for (const auto &block : fn.blocks) {
        bool seenTerminator = false;
        for (const auto &mi : block.instrs) {
            if (seenTerminator)
                report.error("AFTER-TERM", &block, &mi, "instruction follows a block terminator");
            if (isHardTerminator(mi.opc))
                seenTerminator = true;

            for (std::size_t idx = 0; idx < mi.ops.size(); ++idx) {
                const auto &op = mi.ops[idx];
                switch (op.kind) {
                    case MOperand::Kind::Reg: {
                        try {
                            (void)ra::operandRoles(mi, idx);
                        } catch (const std::logic_error &ex) {
                            rolesOk = false;
                            report.error("ROLE",
                                         &block,
                                         &mi,
                                         std::string("register operand ") + std::to_string(idx) +
                                             " has no classified role: " + ex.what());
                        }
                        if (op.reg.isPhys) {
                            if (!isValidPhysOrdinal(op.reg.idOrPhys)) {
                                report.error("REG-CLASS",
                                             &block,
                                             &mi,
                                             "physical register ordinal " +
                                                 std::to_string(op.reg.idOrPhys) +
                                                 " is out of range");
                            } else {
                                const auto reg = static_cast<PhysReg>(op.reg.idOrPhys);
                                const bool gpr = isGPR(reg);
                                if ((op.reg.cls == RegClass::GPR) != gpr) {
                                    report.error("REG-CLASS",
                                                 &block,
                                                 &mi,
                                                 std::string("register ") + regName(reg) +
                                                     " carries the wrong register class");
                                }
                                if (reg == PhysReg::X18) {
                                    report.error("RESERVED-REG",
                                                 &block,
                                                 &mi,
                                                 "platform register x18 must not be used");
                                }
                            }
                        } else {
                            auto [it, inserted] = vregClass.emplace(op.reg.idOrPhys, op.reg.cls);
                            if (!inserted && it->second != op.reg.cls) {
                                report.error("VREG-CLASS",
                                             &block,
                                             &mi,
                                             "virtual register %v" +
                                                 std::to_string(op.reg.idOrPhys) +
                                                 " is used with two register classes");
                            }
                        }
                        break;
                    }
                    case MOperand::Kind::Cond:
                        if (op.cond == nullptr)
                            report.error("COND", &block, &mi, "condition operand is null");
                        break;
                    case MOperand::Kind::Imm:
                    case MOperand::Kind::Label:
                        break;
                }
            }

            // Branch targets must name blocks of this function.
            const auto requireBlock = [&](std::size_t idx) {
                if (idx >= mi.ops.size() || mi.ops[idx].kind != MOperand::Kind::Label) {
                    report.error("LABEL",
                                 &block,
                                 &mi,
                                 "branch is missing its label operand " + std::to_string(idx));
                    return;
                }
                if (blockIndex.find(mi.ops[idx].label) == blockIndex.end()) {
                    report.error("LABEL",
                                 &block,
                                 &mi,
                                 "branch target '" + mi.ops[idx].label + "' does not name a block");
                }
            };
            if (mi.opc == MOpcode::Br) {
                requireBlock(0);
            } else if (isConditionalBranch(mi.opc)) {
                requireBlock(1);
            } else if (mi.opc == MOpcode::JumpTable) {
                // [0]=index, [1]=table symbol, [2..]=case labels.
                if (mi.ops.size() < 3)
                    report.error("LABEL", &block, &mi, "jump table has no case labels");
                for (std::size_t k = 2; k < mi.ops.size(); ++k)
                    requireBlock(k);
            }
        }

        // Carried-exit metadata: sorted, unique, valid ordinals.
        for (std::size_t k = 0; k < block.carriedExitRegs.size(); ++k) {
            const uint16_t ordinal = block.carriedExitRegs[k];
            if (!isValidPhysOrdinal(ordinal)) {
                report.error("CARRY",
                             &block,
                             nullptr,
                             "carriedExitRegs entry " + std::to_string(ordinal) +
                                 " is not a physical register");
                break;
            }
            if (k > 0 && block.carriedExitRegs[k - 1] >= ordinal) {
                report.error("CARRY", &block, nullptr, "carriedExitRegs is not sorted and unique");
                break;
            }
        }
    }

    // The layout's last block cannot fall off the end of the function.
    if (!fn.blocks.empty()) {
        const auto &last = fn.blocks.back();
        const bool ends = !last.instrs.empty() && (isHardTerminator(last.instrs.back().opc) ||
                                                   isNoReturnCall(last.instrs.back()));
        if (!ends) {
            report.error("FALLOFF",
                         &last,
                         last.instrs.empty() ? nullptr : &last.instrs.back(),
                         "last block does not end in a terminator");
        }
    }

    return rolesOk;
}

// -----------------------------------------------------------------------------
// Post-RA rules
// -----------------------------------------------------------------------------

/// @brief Per-block physical liveness over the RA control-flow graph.
struct PhysLiveness {
    std::vector<PhysRegSet> liveIn;  ///< Registers live before each block.
    std::vector<PhysRegSet> liveOut; ///< Registers live after each block.
};

/// @brief Solve backward physical-register liveness for @p fn.
[[nodiscard]] PhysLiveness computePhysLiveness(const MFunction &fn, const TargetInfo &target) {
    const std::size_t n = fn.blocks.size();
    const MirCfg cfg(fn);
    const auto &succs = cfg.successors();

    std::vector<PhysRegSet> gen(n);
    std::vector<PhysRegSet> kill(n);
    for (std::size_t bi = 0; bi < n; ++bi) {
        for (const auto &mi : fn.blocks[bi].instrs) {
            const InstrEffects fx = effectsOf(mi, target);
            gen[bi].bits |= fx.uses.bits & ~kill[bi].bits;
            kill[bi].bits |= fx.defs.bits;
        }
    }

    PhysLiveness result;
    result.liveIn.assign(n, {});
    result.liveOut.assign(n, {});
    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t bi = n; bi-- > 0;) {
            PhysRegSet out;
            for (std::size_t s : succs[bi])
                out.bits |= result.liveIn[s].bits;
            PhysRegSet in;
            in.bits = gen[bi].bits | (out.bits & ~kill[bi].bits);
            if (out.bits != result.liveOut[bi].bits || in.bits != result.liveIn[bi].bits) {
                result.liveOut[bi] = out;
                result.liveIn[bi] = in;
                changed = true;
            }
        }
    }
    return result;
}

/// @brief Run the rules that hold once every value has a physical register.
void checkPostRA(const MFunction &fn, const TargetInfo &target, Reporter &report) {
    bool physOnly = true;
    PhysRegSet calleeSaved;
    for (PhysReg reg : target.calleeSavedGPR)
        calleeSaved.add(reg);
    for (PhysReg reg : target.calleeSavedFPR)
        calleeSaved.add(reg);
    PhysRegSet saved;
    for (PhysReg reg : fn.savedGPRs)
        saved.add(reg);
    for (PhysReg reg : fn.savedFPRs)
        saved.add(reg);

    for (const auto &block : fn.blocks) {
        long long spRegion = 0; // bytes reserved by the active SubSpImm, if any
        for (const auto &mi : block.instrs) {
            bool hasVreg = false;
            for (const auto &op : mi.ops) {
                if (op.kind == MOperand::Kind::Reg && !op.reg.isPhys)
                    hasVreg = true;
            }
            if (hasVreg) {
                physOnly = false;
                report.error("VREG", &block, &mi, "virtual register remains after allocation");
                continue;
            }

            // Frame-relative offsets stay inside the finalized frame or the
            // caller's incoming argument area.
            if (isFrameRelativeOpcode(mi.opc)) {
                const auto off = lastImmediate(mi);
                if (!off.has_value()) {
                    report.error("FRAME-OFFSET", &block, &mi, "frame access has no offset");
                } else {
                    const long long size = frameAccessBytes(mi.opc);
                    const long long total = fn.frame.totalBytes;
                    const bool inLocals = *off >= -total && *off + size <= 0;
                    const bool inIncoming = *off >= kIncomingArgBase;
                    if (!inLocals && !inIncoming) {
                        report.error("FRAME-OFFSET",
                                     &block,
                                     &mi,
                                     "fp-relative offset " + std::to_string(*off) +
                                         " lies outside the " + std::to_string(total) +
                                         "-byte frame");
                    }
                }
            }

            // SP-relative stores stay inside the region an enclosing SubSpImm reserved.
            if (mi.opc == MOpcode::SubSpImm) {
                spRegion = lastImmediate(mi).value_or(0);
            } else if (mi.opc == MOpcode::AddSpImm) {
                spRegion = 0;
            } else if (isSpRelativeOpcode(mi.opc)) {
                const auto off = lastImmediate(mi);
                const long long limit = std::max<long long>(spRegion, fn.frame.maxOutgoingBytes);
                if (!off.has_value() || *off < 0 || *off + 8 > limit) {
                    report.error("SP-OFFSET",
                                 &block,
                                 &mi,
                                 "sp-relative offset " + std::to_string(off.value_or(-1)) +
                                     " lies outside the " + std::to_string(limit) +
                                     "-byte outgoing area");
                }
            }

            // Callee-saved registers written by the body must be in the save list;
            // the frame registers are written only by the prologue/epilogue.
            const PhysRegSet defs = explicitDefs(mi);
            PhysRegSet unsaved;
            unsaved.bits = defs.bits & calleeSaved.bits & ~saved.bits;
            if (!unsaved.empty()) {
                report.error("CALLEE-SAVE",
                             &block,
                             &mi,
                             "writes callee-saved " + describe(unsaved) +
                                 " which is not in the function's save list");
            }
            if (defs.contains(PhysReg::X29) || defs.contains(PhysReg::SP) ||
                defs.contains(PhysReg::X30)) {
                report.error("RESERVED-WRITE",
                             &block,
                             &mi,
                             "explicit write to a frame register (x29/x30/sp)");
            }
        }
    }

    if (!physOnly)
        return;

    // Dataflow rules: reserved scratch never live across an implicit clobber;
    // entry live-in restricted to ABI inputs.
    const PhysLiveness lv = computePhysLiveness(fn, target);
    const PhysRegSet scratch = reservedScratch();

    for (std::size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        const auto &block = fn.blocks[bi];
        PhysRegSet live = lv.liveOut[bi];
        for (std::size_t ii = block.instrs.size(); ii-- > 0;) {
            const auto &mi = block.instrs[ii];
            const InstrEffects fx = effectsOf(mi, target);
            PhysRegSet implicit;
            implicit.bits = fx.defs.bits & ~explicitDefs(mi).bits;
            if (!fx.isCall)
                implicit.bits &= ~operandRegs(mi).bits;
            PhysRegSet hazard;
            hazard.bits = implicit.bits & live.bits & scratch.bits;
            if (!hazard.empty()) {
                report.error("SCRATCH-CLOBBER",
                             &block,
                             &mi,
                             "reserved scratch " + describe(hazard) +
                                 " is live across an instruction that clobbers it");
            }
            live.bits &= ~fx.defs.bits;
            live.bits |= fx.uses.bits;
        }
    }

    PhysRegSet badEntry;
    if (!lv.liveIn.empty())
        badEntry.bits = lv.liveIn.front().bits & ~entryInputs(target).bits;
    if (!badEntry.empty()) {
        report.error("ENTRY-LIVEIN",
                     fn.blocks.empty() ? nullptr : &fn.blocks.front(),
                     nullptr,
                     "registers read before any definition: " + describe(badEntry));
    }
}

// -----------------------------------------------------------------------------
// Post-expand rules
// -----------------------------------------------------------------------------

/// @brief Every immediate must be encodable without an emit-time scratch expansion.
void checkPostExpand(const MFunction &fn, Reporter &report) {
    for (const auto &block : fn.blocks) {
        for (const auto &mi : block.instrs) {
            if (emitTimeScratchClobber(mi)) {
                report.error("IMM-ENCODE",
                             &block,
                             &mi,
                             "immediate requires emit-time scratch expansion after ExpandPseudos");
            }
        }
    }
}

} // namespace

const char *verifyStageName(VerifyStage stage) noexcept {
    switch (stage) {
        case VerifyStage::PostLowering:
            return "post-lowering";
        case VerifyStage::PostRA:
            return "post-ra";
        case VerifyStage::PostExpand:
            return "post-expand";
        case VerifyStage::PostPeephole:
            return "post-peephole";
        case VerifyStage::PostSchedule:
            return "post-schedule";
    }
    return "unknown";
}

bool verifyMir(const MFunction &fn,
               VerifyStage stage,
               const TargetInfo &target,
               common::Diagnostics &diags) {
    Reporter report(fn, stage, diags);
    const bool rolesOk = checkStructure(fn, report);
    if (rolesOk && atLeast(stage, VerifyStage::PostRA))
        checkPostRA(fn, target, report);
    if (atLeast(stage, VerifyStage::PostExpand))
        checkPostExpand(fn, report);
    return report.ok();
}

bool mirVerificationRequested() noexcept {
    if (const char *value = std::getenv("ZANNA_VERIFY_MIR"))
        return value[0] != '\0' && value[0] != '0';
    return false;
}

} // namespace zanna::codegen::aarch64
