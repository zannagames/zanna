//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/TerminatorLowering.cpp
// Purpose: Control-flow terminator lowering for IL→MIR conversion.
//          Handles br, cbr, trap, TrapFromErr, switch, and resume.label.
//
// Key invariants:
//   - Terminators are lowered after all non-terminator instructions so
//     branch targets and phi-edge vreg mappings are fully resolved.
//   - SSA phi-edge copies are inserted as PhiStoreGPR/PhiStoreFPR
//     instructions into predecessor (or edge split) blocks.
//   - Switch trees with >3 cases are lowered to a recursive binary search
//     over new auxiliary blocks.
//
// Ownership/Lifetime:
//   - Modifies mf in place; all map/builder arguments are borrowed.
//
// Links: src/codegen/aarch64/TerminatorLowering.hpp,
//        src/codegen/aarch64/InstrLowering.hpp,
//        src/codegen/aarch64/OpcodeMappings.hpp
//
//===----------------------------------------------------------------------===//

#include "TerminatorLowering.hpp"
#include "FpCompareLowering.hpp"
#include "InstrLowering.hpp"
#include "Noreturn.hpp"
#include "OpcodeMappings.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <stdexcept>

/**
 * @file
 * @brief Implements AArch64 branch, switch, trap, and resume terminator lowering.
 *
 * This late lowering phase uses block-local temporary snapshots to avoid
 * resolving operands through mappings overwritten by later blocks. Phi
 * arguments are transferred through canonical frame slots, and switches choose
 * among linear, dense jump-table, and sparse binary-tree representations.
 */

namespace zanna::codegen::aarch64 {

using il::core::Opcode;

/**
 * @brief Looks up the condition code for an integer or FP comparison opcode.
 * @param op IL opcode to classify.
 * @return Static AArch64 condition spelling, or `nullptr` for a non-comparison.
 */
static const char *condForOpcode(Opcode op) {
    return lookupAnyCondition(op);
}

/**
 * @brief Emits a null trap-message argument followed by `rt_trap_string`.
 * @param[in,out] bb Block that receives the `x0 = 0` setup and no-returning call.
 */
static void emitNullTrapCall(MBasicBlock &bb) {
    bb.instrs.push_back(MInstr{MOpcode::MovRI, {MOperand::regOp(PhysReg::X0), MOperand::immOp(0)}});
    bb.instrs.push_back(MInstr{MOpcode::Bl, {MOperand::labelOp("rt_trap_string")}});
}

/// @brief Owns one validated switch key and its final MIR branch label.
struct SwitchCase {
    long long value{0};      ///< Integer constant matched by this case.
    std::string branchLabel; ///< MIR block label to branch to when matched.
};

/// @brief Validate and narrow a switch_i32 case value to a signed 32-bit key.
/// @details Terminator lowering can be reached in tests or debug paths with
///          malformed IL. Rejecting invalid values here avoids silently
///          treating non-constants as zero or emitting compares with the wrong
///          integer width.
/// @param value IL value used as the switch case key.
/// @param caseIndex Zero-based case index for diagnostics.
/// @return The validated i32 case value, widened to long long for MIR immediates.
/// @throws std::runtime_error if @p value is not a ConstInt in i32 range.
static long long checkedSwitchI32CaseValue(const il::core::Value &value, std::size_t caseIndex) {
    if (value.kind != il::core::Value::Kind::ConstInt) {
        throw std::runtime_error("AArch64 terminator lowering: switch_i32 case " +
                                 std::to_string(caseIndex) + " is not a const int");
    }
    if (value.i64 < std::numeric_limits<int32_t>::min() ||
        value.i64 > std::numeric_limits<int32_t>::max()) {
        throw std::runtime_error("AArch64 terminator lowering: switch_i32 case " +
                                 std::to_string(caseIndex) + " is outside i32 range");
    }
    return static_cast<long long>(static_cast<int32_t>(value.i64));
}

/// @brief Build the synthetic spill key used for a switch_i32 dispatch tree.
/// @details FrameBuilder spill keys share a uint32_t namespace. Switch tree
///          spills live in a high reserved range and are checked to avoid
///          wraparound back into normal vreg or temp spill IDs.
/// @param blockIndex Source IL block index owning the switch.
/// @return Reserved spill key for the switch scrutinee.
/// @throws std::runtime_error if @p blockIndex does not fit in the reserved range.
static uint32_t switchSpillKeyForBlock(std::size_t blockIndex) {
    constexpr uint32_t kSwitchSpillBase = 0xE0000000u;
    constexpr std::size_t kMaxSwitchIndex =
        static_cast<std::size_t>(std::numeric_limits<uint32_t>::max() - kSwitchSpillBase);
    if (blockIndex > kMaxSwitchIndex) {
        throw std::runtime_error(
            "AArch64 terminator lowering: switch spill key range exhausted");
    }
    return kSwitchSpillBase + static_cast<uint32_t>(blockIndex);
}

/**
 * @brief Emits a comparison between a switch scrutinee and an integer key.
 *
 * Encodable unsigned 12-bit keys use `CmpRI`; all other values are
 * materialized in a new GPR and compared with `CmpRR`.
 *
 * @param[in,out] bb Dispatch block that receives the comparison sequence.
 * @param scrutineeVReg GPR virtual register holding the switch value.
 * @param imm Signed case key.
 * @param[in,out] nextVRegId Virtual-register allocator used by the fallback.
 * @throws std::runtime_error If a fallback temporary cannot be allocated.
 */
static void emitSwitchCompare(MBasicBlock &bb,
                              uint16_t scrutineeVReg,
                              long long imm,
                              uint16_t &nextVRegId) {
    if (isUImm12(imm)) {
        bb.instrs.push_back(
            MInstr{MOpcode::CmpRI,
                   {MOperand::vregOp(RegClass::GPR, scrutineeVReg), MOperand::immOp(imm)}});
        return;
    }

    const uint16_t caseVReg = allocateNextVReg(nextVRegId);
    bb.instrs.push_back(
        MInstr{MOpcode::MovRI, {MOperand::vregOp(RegClass::GPR, caseVReg), MOperand::immOp(imm)}});
    bb.instrs.push_back(MInstr{MOpcode::CmpRR,
                               {MOperand::vregOp(RegClass::GPR, scrutineeVReg),
                                MOperand::vregOp(RegClass::GPR, caseVReg)}});
}

/**
 * @brief Reloads a switch scrutinee into a fresh virtual register.
 *
 * @param[in,out] bb Auxiliary dispatch block receiving the reload.
 * @param spillOffset Frame-pointer-relative scrutinee spill offset.
 * @param[in,out] nextVRegId Virtual-register allocator state.
 * @return New GPR virtual-register ID holding the reloaded scrutinee.
 * @throws std::runtime_error If the virtual-register range is exhausted.
 */
static uint16_t reloadSwitchScrutinee(MBasicBlock &bb, int spillOffset, uint16_t &nextVRegId) {
    const uint16_t reloaded = allocateNextVReg(nextVRegId);
    bb.instrs.push_back(
        MInstr{MOpcode::LdrRegFpImm,
               {MOperand::vregOp(RegClass::GPR, reloaded), MOperand::immOp(spillOffset)}});
    return reloaded;
}

/**
 * @brief Emits a linear comparison chain for a switch-case subrange.
 *
 * Each case emits a compare followed by `b.eq`; a non-empty default label is
 * emitted as the final unconditional branch.
 *
 * @param[in,out] bb Dispatch block to extend.
 * @param scrutineeVReg GPR virtual register containing the switch value.
 * @param cases Sorted or unsorted case table.
 * @param begin Inclusive first case index.
 * @param end Exclusive last case index.
 * @param defaultBranchLabel Default target, or empty to omit the final branch.
 * @param[in,out] nextVRegId Allocator for comparison temporaries.
 * @pre `begin <= end && end <= cases.size()`.
 */
static void emitLinearSwitch(MBasicBlock &bb,
                             uint16_t scrutineeVReg,
                             const std::vector<SwitchCase> &cases,
                             std::size_t begin,
                             std::size_t end,
                             const std::string &defaultBranchLabel,
                             uint16_t &nextVRegId) {
    for (std::size_t idx = begin; idx < end; ++idx) {
        const auto &switchCase = cases[idx];
        emitSwitchCompare(bb, scrutineeVReg, switchCase.value, nextVRegId);
        bb.instrs.push_back(MInstr{
            MOpcode::BCond, {MOperand::condOp("eq"), MOperand::labelOp(switchCase.branchLabel)}});
    }
    if (!defaultBranchLabel.empty())
        bb.instrs.push_back(MInstr{MOpcode::Br, {MOperand::labelOp(defaultBranchLabel)}});
}

/**
 * @brief Recursively emits a binary-search dispatch tree for a switch subrange.
 *
 * The midpoint is tested for equality, then less-than selects a newly appended
 * left block while the fallthrough branch selects a new right block. Each child
 * reloads the scrutinee from its frame slot. Ranges of at most three cases use
 * `emitLinearSwitch()`.
 *
 * @param[in,out] mf Output function that owns generated auxiliary blocks.
 * @param[in,out] bb Current dispatch block receiving pivot branches.
 * @param scrutineeVReg GPR virtual register valid in @p bb.
 * @param cases Case table sorted by ascending unique key.
 * @param begin Inclusive subrange start.
 * @param end Exclusive subrange end.
 * @param defaultBranchLabel Default switch target.
 * @param spillOffset Stable FP-relative scrutinee spill slot.
 * @param labelPrefix Prefix used to derive child labels.
 * @param[in,out] auxCounter Function-wide unique child-label counter.
 * @param[in,out] nextVRegId Virtual-register allocator state.
 * @pre `begin <= end && end <= cases.size()`.
 */
static void emitSwitchTree(MFunction &mf,
                           MBasicBlock &bb,
                           uint16_t scrutineeVReg,
                           const std::vector<SwitchCase> &cases,
                           std::size_t begin,
                           std::size_t end,
                           const std::string &defaultBranchLabel,
                           int spillOffset,
                           const std::string &labelPrefix,
                           std::size_t &auxCounter,
                           uint16_t &nextVRegId) {
    const std::size_t count = end - begin;
    if (count <= 3) {
        emitLinearSwitch(bb, scrutineeVReg, cases, begin, end, defaultBranchLabel, nextVRegId);
        return;
    }

    const std::size_t pivotIndex = begin + count / 2;
    const SwitchCase &pivot = cases[pivotIndex];
    const std::string leftLabel = labelPrefix + ".Lswitch_left_" + std::to_string(auxCounter++);
    const std::string rightLabel = labelPrefix + ".Lswitch_right_" + std::to_string(auxCounter++);

    emitSwitchCompare(bb, scrutineeVReg, pivot.value, nextVRegId);
    bb.instrs.push_back(
        MInstr{MOpcode::BCond, {MOperand::condOp("eq"), MOperand::labelOp(pivot.branchLabel)}});
    bb.instrs.push_back(
        MInstr{MOpcode::BCond, {MOperand::condOp("lt"), MOperand::labelOp(leftLabel)}});
    bb.instrs.push_back(MInstr{MOpcode::Br, {MOperand::labelOp(rightLabel)}});

    mf.blocks.emplace_back();
    auto &leftBB = mf.blocks.back();
    leftBB.name = leftLabel;
    const uint16_t leftScrutinee = reloadSwitchScrutinee(leftBB, spillOffset, nextVRegId);
    emitSwitchTree(mf,
                   leftBB,
                   leftScrutinee,
                   cases,
                   begin,
                   pivotIndex,
                   defaultBranchLabel,
                   spillOffset,
                   leftLabel,
                   auxCounter,
                   nextVRegId);

    mf.blocks.emplace_back();
    auto &rightBB = mf.blocks.back();
    rightBB.name = rightLabel;
    const uint16_t rightScrutinee = reloadSwitchScrutinee(rightBB, spillOffset, nextVRegId);
    emitSwitchTree(mf,
                   rightBB,
                   rightScrutinee,
                   cases,
                   pivotIndex + 1,
                   end,
                   defaultBranchLabel,
                   spillOffset,
                   rightLabel,
                   auxCounter,
                   nextVRegId);
}

/**
 * @brief Materializes branch arguments into a destination block's phi spill slots.
 *
 * One `PhiStoreGPR` or `PhiStoreFPR` is appended per argument. Empty argument
 * lists are a no-op; non-empty lists require exact class and count agreement
 * with both metadata tables.
 *
 * @param[in,out] edgeBB Source or split-edge MIR block receiving stores.
 * @param dst Destination IL block label.
 * @param args Branch arguments in destination parameter order.
 * @param inBB Source IL block used for producer lookup.
 * @param ti Target metadata for materialization.
 * @param[in,out] fb Frame builder used by nested materialization.
 * @param[in,out] blockTempVReg Source block's temporary mapping snapshot.
 * @param[in,out] tempRegClass Function-wide temporary class map.
 * @param[in,out] nextVRegId Virtual-register allocator state.
 * @param phiRegClass Expected destination parameter classes.
 * @param phiSpillOffset Destination parameter frame offsets.
 * @throws std::runtime_error If metadata is absent/inconsistent, an argument
 *         cannot be materialized, or its register class differs.
 */
static void emitPhiEdgeCopies(
    MBasicBlock &edgeBB,
    const std::string &dst,
    const std::vector<il::core::Value> &args,
    const il::core::BasicBlock &inBB,
    const TargetInfo &ti,
    FrameBuilder &fb,
    std::unordered_map<unsigned, uint16_t> &blockTempVReg,
    std::unordered_map<unsigned, RegClass> &tempRegClass,
    uint16_t &nextVRegId,
    const std::unordered_map<std::string, std::vector<RegClass>> &phiRegClass,
    const std::unordered_map<std::string, std::vector<int>> &phiSpillOffset) {
    if (args.empty())
        return;
    auto itSpill = phiSpillOffset.find(dst);
    if (itSpill == phiSpillOffset.end()) {
        throw std::runtime_error("AArch64 terminator lowering: branch to block '" + dst +
                                 "' carries arguments but target has no phi spill slots");
    }
    auto itClass = phiRegClass.find(dst);
    if (itClass == phiRegClass.end()) {
        throw std::runtime_error("AArch64 terminator lowering: branch to block '" + dst +
                                 "' carries arguments but target has no phi class metadata");
    }
    const auto &classes = itClass->second;
    const auto &spillOffsets = itSpill->second;
    if (classes.size() != spillOffsets.size()) {
        throw std::runtime_error("AArch64 terminator lowering: phi metadata count mismatch for "
                                 "block '" +
                                 dst + "'");
    }
    if (args.size() != classes.size()) {
        throw std::runtime_error("AArch64 terminator lowering: branch argument count mismatch for "
                                 "block '" +
                                 dst + "'");
    }
    for (std::size_t ai = 0; ai < args.size(); ++ai) {
        uint16_t sv = 0;
        RegClass scls = RegClass::GPR;
        if (!materializeValueToVReg(args[ai],
                                    inBB,
                                    ti,
                                    fb,
                                    edgeBB,
                                    blockTempVReg,
                                    tempRegClass,
                                    nextVRegId,
                                    sv,
                                    scls)) {
            throw std::runtime_error("AArch64 terminator lowering: failed to materialize phi-edge "
                                     "argument for block '" +
                                     dst + "'");
        }
        const RegClass dstCls = classes[ai];
        const int offset = spillOffsets[ai];
        if (scls != dstCls) {
            throw std::runtime_error(
                "AArch64 terminator lowering: phi-edge argument register class "
                "mismatch for block '" +
                dst + "'");
        }
        if (dstCls == RegClass::FPR) {
            edgeBB.instrs.push_back(
                MInstr{MOpcode::PhiStoreFPR,
                       {MOperand::vregOp(RegClass::FPR, sv), MOperand::immOp(offset)}});
        } else {
            edgeBB.instrs.push_back(
                MInstr{MOpcode::PhiStoreGPR,
                       {MOperand::vregOp(RegClass::GPR, sv), MOperand::immOp(offset)}});
        }
    }
}

/**
 * @brief Lowers an IL conditional branch to comparisons, branches, and optional edge blocks.
 *
 * A same-block comparison producer is folded into integer compare/condition
 * branches or an FP compare result. Otherwise the boolean value is
 * materialized and tested against zero. Separate true/false edge blocks are
 * appended when phi arguments or identical destinations require distinct
 * transfers.
 *
 * @param term Candidate `CBr` terminator.
 * @param inBB Source IL block containing @p term and possible compare producer.
 * @param[in,out] outBB Corresponding MIR block receiving the branch sequence.
 * @param blockIndex Original block index used in unique edge labels.
 * @param[in,out] mf Output function that owns appended edge blocks.
 * @param ti Target metadata for value materialization.
 * @param[in,out] fb Frame allocator for nested spill materialization.
 * @param phiRegClass Destination phi register classes.
 * @param phiSpillOffset Destination phi frame offsets.
 * @param[in,out] blockTempVReg Source block temporary snapshot.
 * @param[in,out] tempRegClass Function-wide temporary class map.
 * @param[in,out] nextVRegId Virtual-register allocator state.
 * @throws std::runtime_error If a required condition or phi argument cannot be
 *         materialized or its metadata is inconsistent.
 */
static void lowerCBr(const il::core::Instr &term,
                     const il::core::BasicBlock &inBB,
                     MBasicBlock &outBB,
                     std::size_t blockIndex,
                     MFunction &mf,
                     const TargetInfo &ti,
                     FrameBuilder &fb,
                     const std::unordered_map<std::string, std::vector<RegClass>> &phiRegClass,
                     const std::unordered_map<std::string, std::vector<int>> &phiSpillOffset,
                     std::unordered_map<unsigned, uint16_t> &blockTempVReg,
                     std::unordered_map<unsigned, RegClass> &tempRegClass,
                     uint16_t &nextVRegId) {
    if (term.operands.size() < 1 || term.labels.size() != 2)
        return;

    const std::string &trueLbl = term.labels[0];
    const std::string &falseLbl = term.labels[1];
    const bool sameTarget = (trueLbl == falseLbl);
    const bool needTrueEdge = sameTarget || (term.brArgs.size() > 0 && !term.brArgs[0].empty());
    const bool needFalseEdge = sameTarget || (term.brArgs.size() > 1 && !term.brArgs[1].empty());
    const std::string trueEdgeLbl =
        needTrueEdge ? outBB.name + ".Ledge_true_" + std::to_string(blockIndex) : trueLbl;
    const std::string falseEdgeLbl =
        needFalseEdge ? outBB.name + ".Ledge_false_" + std::to_string(blockIndex) : falseLbl;

    // Try to lower a same-block compare producer directly to cmp/fcmp + b.<cond>.
    const auto &cond = term.operands[0];
    bool loweredViaCompare = false;
    if (cond.kind == il::core::Value::Kind::Temp) {
        const auto it = std::find_if(
            inBB.instructions.begin(), inBB.instructions.end(), [&](const il::core::Instr &I) {
                return I.result && *I.result == cond.id;
            });
        if (it != inBB.instructions.end()) {
            const il::core::Instr &cmpI = *it;
            const char *cc = condForOpcode(cmpI.op);
            if (cc && cmpI.operands.size() == 2) {
                uint16_t lhs = 0;
                RegClass lhsCls = RegClass::GPR;
                if (materializeValueToVReg(cmpI.operands[0],
                                           inBB,
                                           ti,
                                           fb,
                                           outBB,
                                           blockTempVReg,
                                           tempRegClass,
                                           nextVRegId,
                                           lhs,
                                           lhsCls)) {
                    const bool isFpCompare = isFloatingPointCompareOp(cmpI.op);
                    if (isFpCompare) {
                        uint16_t rhs = 0;
                        RegClass rhsCls = RegClass::FPR;
                        if (materializeValueToVReg(cmpI.operands[1],
                                                   inBB,
                                                   ti,
                                                   fb,
                                                   outBB,
                                                   blockTempVReg,
                                                   tempRegClass,
                                                   nextVRegId,
                                                   rhs,
                                                   rhsCls) &&
                            lhsCls == RegClass::FPR && rhsCls == RegClass::FPR) {
                            outBB.instrs.push_back(MInstr{MOpcode::FCmpRR,
                                                          {MOperand::vregOp(RegClass::FPR, lhs),
                                                           MOperand::vregOp(RegClass::FPR, rhs)}});
                            (void)cc;
                            const uint16_t cmpResult = allocateNextVReg(nextVRegId);
                            emitFpCompareResult(outBB, cmpI.op, cmpResult, nextVRegId);
                            outBB.instrs.push_back(
                                MInstr{MOpcode::Cbnz,
                                       {MOperand::vregOp(RegClass::GPR, cmpResult),
                                        MOperand::labelOp(trueEdgeLbl)}});
                            outBB.instrs.push_back(
                                MInstr{MOpcode::Br, {MOperand::labelOp(falseEdgeLbl)}});
                            loweredViaCompare = true;
                        }
                    } else if (cmpI.operands[1].kind == il::core::Value::Kind::ConstInt &&
                               isUImm12(cmpI.operands[1].i64)) {
                        outBB.instrs.push_back(MInstr{MOpcode::CmpRI,
                                                      {MOperand::vregOp(RegClass::GPR, lhs),
                                                       MOperand::immOp(cmpI.operands[1].i64)}});
                        outBB.instrs.push_back(
                            MInstr{MOpcode::BCond,
                                   {MOperand::condOp(cc), MOperand::labelOp(trueEdgeLbl)}});
                        outBB.instrs.push_back(
                            MInstr{MOpcode::Br, {MOperand::labelOp(falseEdgeLbl)}});
                        loweredViaCompare = true;
                    } else {
                        uint16_t rhs = 0;
                        RegClass rhsCls = RegClass::GPR;
                        if (materializeValueToVReg(cmpI.operands[1],
                                                   inBB,
                                                   ti,
                                                   fb,
                                                   outBB,
                                                   blockTempVReg,
                                                   tempRegClass,
                                                   nextVRegId,
                                                   rhs,
                                                   rhsCls) &&
                            lhsCls == RegClass::GPR && rhsCls == RegClass::GPR) {
                            outBB.instrs.push_back(MInstr{MOpcode::CmpRR,
                                                          {MOperand::vregOp(RegClass::GPR, lhs),
                                                           MOperand::vregOp(RegClass::GPR, rhs)}});
                            outBB.instrs.push_back(
                                MInstr{MOpcode::BCond,
                                       {MOperand::condOp(cc), MOperand::labelOp(trueEdgeLbl)}});
                            outBB.instrs.push_back(
                                MInstr{MOpcode::Br, {MOperand::labelOp(falseEdgeLbl)}});
                            loweredViaCompare = true;
                        }
                    }
                }
            }
        }
    }
    if (!loweredViaCompare) {
        // Generic path: materialize boolean and branch on non-zero.
        uint16_t cv = 0;
        RegClass cc = RegClass::GPR;
        if (!materializeValueToVReg(
                cond, inBB, ti, fb, outBB, blockTempVReg, tempRegClass, nextVRegId, cv, cc) ||
            cc != RegClass::GPR) {
            throw std::runtime_error(
                "AArch64 terminator lowering: failed to materialize cbr condition");
        }
        outBB.instrs.push_back(
            MInstr{MOpcode::CmpRI, {MOperand::vregOp(RegClass::GPR, cv), MOperand::immOp(0)}});
        outBB.instrs.push_back(
            MInstr{MOpcode::BCond, {MOperand::condOp("ne"), MOperand::labelOp(trueEdgeLbl)}});
        outBB.instrs.push_back(MInstr{MOpcode::Br, {MOperand::labelOp(falseEdgeLbl)}});
    }

    // Emit per-edge phi-argument copy blocks, if needed.
    if (needTrueEdge) {
        MBasicBlock trueEdgeBB;
        trueEdgeBB.name = trueEdgeLbl;
        if (term.brArgs.size() > 0)
            emitPhiEdgeCopies(trueEdgeBB,
                              trueLbl,
                              term.brArgs[0],
                              inBB,
                              ti,
                              fb,
                              blockTempVReg,
                              tempRegClass,
                              nextVRegId,
                              phiRegClass,
                              phiSpillOffset);
        trueEdgeBB.instrs.push_back(MInstr{MOpcode::Br, {MOperand::labelOp(trueLbl)}});
        mf.blocks.push_back(std::move(trueEdgeBB));
    }
    if (needFalseEdge) {
        MBasicBlock falseEdgeBB;
        falseEdgeBB.name = falseEdgeLbl;
        if (term.brArgs.size() > 1)
            emitPhiEdgeCopies(falseEdgeBB,
                              falseLbl,
                              term.brArgs[1],
                              inBB,
                              ti,
                              fb,
                              blockTempVReg,
                              tempRegClass,
                              nextVRegId,
                              phiRegClass,
                              phiSpillOffset);
        falseEdgeBB.instrs.push_back(MInstr{MOpcode::Br, {MOperand::labelOp(falseLbl)}});
        mf.blocks.push_back(std::move(falseEdgeBB));
    }
}

/**
 * @brief Lowers an `Opcode::SwitchI32` terminator.
 *
 * Cases are validated, assigned optional phi-edge blocks, sorted, and checked
 * for duplicate keys. Small sets use comparisons, suitable dense non-negative
 * sets use a jump table unless disabled by `ZANNA_NO_JUMP_TABLES`, and remaining
 * sets spill the scrutinee for a recursive binary-search tree.
 *
 * @param term Switch terminator to validate and lower.
 * @param inBB Containing IL block for operand materialization.
 * @param[in,out] outBB Original MIR dispatch block.
 * @param blockIndex Original block index used for spill keys and labels.
 * @param[in,out] mf Output function receiving edge/tree blocks.
 * @param ti Target metadata for materialization.
 * @param[in,out] fb Frame allocator for phi and scrutinee spills.
 * @param phiRegClass Destination phi register classes.
 * @param phiSpillOffset Destination phi frame offsets.
 * @param[in,out] blockTempVReg Source block temporary snapshot.
 * @param[in,out] tempRegClass Function-wide temporary class map.
 * @param[in,out] nextVRegId Virtual-register allocator state.
 * @param[in,out] switchAuxCounter Unique binary-tree label counter.
 * @throws std::runtime_error If switch structure, keys, metadata, or
 *         materialized operand classes are invalid.
 */
static void lowerSwitchI32(
    const il::core::Instr &term,
    const il::core::BasicBlock &inBB,
    MBasicBlock &outBB,
    std::size_t blockIndex,
    MFunction &mf,
    const TargetInfo &ti,
    FrameBuilder &fb,
    const std::unordered_map<std::string, std::vector<RegClass>> &phiRegClass,
    const std::unordered_map<std::string, std::vector<int>> &phiSpillOffset,
    std::unordered_map<unsigned, uint16_t> &blockTempVReg,
    std::unordered_map<unsigned, RegClass> &tempRegClass,
    uint16_t &nextVRegId,
    std::size_t &switchAuxCounter) {
    if (term.operands.empty()) {
        throw std::runtime_error("AArch64 terminator lowering: switch_i32 missing scrutinee");
    }
    if (term.labels.empty()) {
        throw std::runtime_error("AArch64 terminator lowering: switch_i32 missing default label");
    }
    if (term.labels.front().empty()) {
        throw std::runtime_error("AArch64 terminator lowering: switch_i32 default label is empty");
    }
    if (term.brArgs.size() != term.labels.size()) {
        throw std::runtime_error(
            "AArch64 terminator lowering: switch_i32 branch argument vector count mismatch");
    }
    if (term.operands.size() != term.labels.size()) {
        throw std::runtime_error(
            "AArch64 terminator lowering: switch_i32 operand/label count mismatch");
    }

    uint16_t sv = 0;
    RegClass scls = RegClass::GPR;
    if (!materializeValueToVReg(term.operands[0],
                                inBB,
                                ti,
                                fb,
                                outBB,
                                blockTempVReg,
                                tempRegClass,
                                nextVRegId,
                                sv,
                                scls)) {
        throw std::runtime_error(
            "AArch64 terminator lowering: failed to materialize switch scrutinee");
    }
    if (scls != RegClass::GPR) {
        throw std::runtime_error(
            "AArch64 terminator lowering: switch scrutinee lowered to non-GPR");
    }

    const std::size_t ncases = il::core::switchCaseCount(term);
    std::vector<SwitchCase> cases;
    cases.reserve(ncases);
    for (std::size_t ci = 0; ci < ncases; ++ci) {
        const auto &caseValue = il::core::switchCaseValue(term, ci);
        const std::string &caseLabel = il::core::switchCaseLabel(term, ci);
        if (caseLabel.empty()) {
            throw std::runtime_error("AArch64 terminator lowering: switch_i32 case " +
                                     std::to_string(ci) + " missing target label");
        }
        const long long imm = checkedSwitchI32CaseValue(caseValue, ci);

        const auto &args = il::core::switchCaseArgs(term, ci);
        std::string branchLabel = caseLabel;
        if (!args.empty()) {
            branchLabel = outBB.name + ".Lswitch_case_" + std::to_string(blockIndex) + "_" +
                          std::to_string(ci);
            MBasicBlock edgeBB;
            edgeBB.name = branchLabel;
            emitPhiEdgeCopies(edgeBB,
                              caseLabel,
                              args,
                              inBB,
                              ti,
                              fb,
                              blockTempVReg,
                              tempRegClass,
                              nextVRegId,
                              phiRegClass,
                              phiSpillOffset);
            edgeBB.instrs.push_back(MInstr{MOpcode::Br, {MOperand::labelOp(caseLabel)}});
            mf.blocks.push_back(std::move(edgeBB));
        }
        cases.push_back(SwitchCase{imm, std::move(branchLabel)});
    }

    const std::string &defLbl = il::core::switchDefaultLabel(term);
    std::string defaultBranchLabel = defLbl;
    if (!defLbl.empty()) {
        const auto &defArgs = il::core::switchDefaultArgs(term);
        if (!defArgs.empty()) {
            defaultBranchLabel = outBB.name + ".Lswitch_default_" + std::to_string(blockIndex);
            MBasicBlock edgeBB;
            edgeBB.name = defaultBranchLabel;
            emitPhiEdgeCopies(edgeBB,
                              defLbl,
                              defArgs,
                              inBB,
                              ti,
                              fb,
                              blockTempVReg,
                              tempRegClass,
                              nextVRegId,
                              phiRegClass,
                              phiSpillOffset);
            edgeBB.instrs.push_back(MInstr{MOpcode::Br, {MOperand::labelOp(defLbl)}});
            mf.blocks.push_back(std::move(edgeBB));
        }
    }

    std::sort(cases.begin(), cases.end(), [](const SwitchCase &lhs, const SwitchCase &rhs) {
        return lhs.value < rhs.value;
    });
    for (std::size_t ci = 1; ci < cases.size(); ++ci) {
        if (cases[ci - 1].value == cases[ci].value) {
            throw std::runtime_error("AArch64 terminator lowering: duplicate switch_i32 case " +
                                     std::to_string(cases[ci].value));
        }
    }

    // Dense case sets dispatch through an inline jump table: one unsigned
    // bounds check plus an indirect branch replaces the compare tree. Case
    // labels already point at the dedicated phi-edge blocks built above.
    constexpr std::size_t kMinJumpTableCases = 6;
    constexpr int64_t kMaxJumpTableSpan = 4095; // SubRI/CmpRI immediate ceiling
    constexpr double kMinJumpTableDensity = 0.5;
    if (cases.size() > 3 && cases.size() >= kMinJumpTableCases &&
        std::getenv("ZANNA_NO_JUMP_TABLES") == nullptr) {
        const int64_t lo = cases.front().value;
        const int64_t hi = cases.back().value;
        const int64_t span = hi - lo + 1;
        if (lo >= 0 && lo <= 4095 && span <= kMaxJumpTableSpan &&
            static_cast<double>(cases.size()) >=
                kMinJumpTableDensity * static_cast<double>(span)) {
            // With lo == 0 the scrutinee IS the table index; a bare copy here
            // would tempt copy forwarding into splitting the compare and the
            // dispatch onto different registers.
            uint16_t idxV = sv;
            if (lo != 0) {
                idxV = allocateNextVReg(nextVRegId);
                outBB.instrs.push_back(MInstr{MOpcode::SubRI,
                                              {MOperand::vregOp(RegClass::GPR, idxV),
                                               MOperand::vregOp(RegClass::GPR, sv),
                                               MOperand::immOp(lo)}});
            }
            outBB.instrs.push_back(MInstr{
                MOpcode::CmpRI,
                {MOperand::vregOp(RegClass::GPR, idxV), MOperand::immOp(span)}});
            outBB.instrs.push_back(
                MInstr{MOpcode::BCond,
                       {MOperand::condOp("hs"), MOperand::labelOp(defaultBranchLabel)}});

            // The dispatch tail uses the reserved X16/X17 scratch registers,
            // so only the index needs allocation.
            // The leading ".L" keeps the label assembler-local (a non-local
            // label would start a new Mach-O atom, and adr cannot reference
            // across atoms).
            std::vector<MOperand> jtOps{
                MOperand::vregOp(RegClass::GPR, idxV),
                MOperand::labelOp(".Ljt_" + outBB.name + "_" + std::to_string(blockIndex))};
            std::size_t ci = 0;
            for (int64_t v = lo; v <= hi; ++v) {
                if (ci < cases.size() && cases[ci].value == v) {
                    jtOps.push_back(MOperand::labelOp(cases[ci].branchLabel));
                    ++ci;
                } else {
                    jtOps.push_back(MOperand::labelOp(defaultBranchLabel));
                }
            }
            outBB.instrs.push_back(MInstr{MOpcode::JumpTable, std::move(jtOps)});
            return;
        }
    }

    if (cases.size() <= 3) {
        emitLinearSwitch(outBB, sv, cases, 0, cases.size(), defaultBranchLabel, nextVRegId);
    } else {
        const uint32_t switchSpillKey = switchSpillKeyForBlock(blockIndex);
        const int switchSpillOffset = fb.ensureSpill(switchSpillKey);
        outBB.instrs.push_back(
            MInstr{MOpcode::StrRegFpImm,
                   {MOperand::vregOp(RegClass::GPR, sv), MOperand::immOp(switchSpillOffset)}});
        emitSwitchTree(mf,
                       outBB,
                       sv,
                       cases,
                       0,
                       cases.size(),
                       defaultBranchLabel,
                       switchSpillOffset,
                       outBB.name + ".Lswitch_tree_" + std::to_string(blockIndex),
                       switchAuxCounter,
                       nextVRegId);
    }
}

/**
 * @brief Lowers supported terminators for every original block in an IL function.
 *
 * @param fn Source IL function.
 * @param[in,out] mf Parallel MIR function, plus any appended auxiliary blocks.
 * @param ti Target ABI/register metadata.
 * @param[in,out] fb Frame allocator.
 * @param phiVregId Currently unused phi-vreg compatibility input.
 * @param phiRegClass Phi parameter classes by target label.
 * @param phiSpillOffset Phi parameter offsets by target label.
 * @param[in,out] blockTempVRegSnapshot Temporary mappings by original block.
 * @param[in,out] tempRegClass Function-wide temporary class map.
 * @param[in,out] nextVRegId Virtual-register allocator state.
 * @throws std::runtime_error If terminator validation or materialization fails.
 */
void lowerTerminators(const il::core::Function &fn,
                      MFunction &mf,
                      const TargetInfo &ti,
                      FrameBuilder &fb,
                      const std::unordered_map<std::string, std::vector<uint16_t>> &phiVregId,
                      const std::unordered_map<std::string, std::vector<RegClass>> &phiRegClass,
                      const std::unordered_map<std::string, std::vector<int>> &phiSpillOffset,
                      std::vector<std::unordered_map<unsigned, uint16_t>> &blockTempVRegSnapshot,
                      std::unordered_map<unsigned, RegClass> &tempRegClass,
                      uint16_t &nextVRegId) {
    std::size_t switchAuxCounter = 0;
    for (std::size_t i = 0; i < fn.blocks.size(); ++i) {
        const auto &inBB = fn.blocks[i];
        if (inBB.instructions.empty())
            continue;
        const auto &term = inBB.instructions.back();
        auto &outBB = mf.blocks[i];
        // Use the block's tempVReg snapshot to get correct vreg mappings for temps
        // defined in this block. This avoids using overwritten values from later blocks.
        auto &blockTempVReg = blockTempVRegSnapshot[i];

        switch (term.op) {
            case Opcode::Br:
                if (!term.labels.empty()) {
                    // Emit phi edge copies for target - store to spill slots
                    if (!term.brArgs.empty() && !term.brArgs[0].empty()) {
                        const std::string &dst = term.labels[0];
                        emitPhiEdgeCopies(outBB,
                                          dst,
                                          term.brArgs[0],
                                          inBB,
                                          ti,
                                          fb,
                                          blockTempVReg,
                                          tempRegClass,
                                          nextVRegId,
                                          phiRegClass,
                                          phiSpillOffset);
                    }
                    outBB.instrs.push_back(
                        MInstr{MOpcode::Br, {MOperand::labelOp(term.labels[0])}});
                }
                break;

            case Opcode::Trap: {
                // Phase A: lower trap to a helper call for diagnostics.
                // Skip emitting rt_trap if the block already has a call to a noreturn function
                // like rt_arr_oob_panic (which will abort and never return).
                bool hasNoreturnCall = false;
                for (const auto &mi : outBB.instrs) {
                    if (isNoReturnCall(mi)) {
                        hasNoreturnCall = true;
                        break;
                    }
                }
                if (!hasNoreturnCall) {
                    if (!term.operands.empty()) {
                        uint16_t trapArg = 0;
                        RegClass trapArgCls = RegClass::GPR;
                        if (!materializeValueToVReg(term.operands[0],
                                                    inBB,
                                                    ti,
                                                    fb,
                                                    outBB,
                                                    blockTempVReg,
                                                    tempRegClass,
                                                    nextVRegId,
                                                    trapArg,
                                                    trapArgCls) ||
                            trapArgCls != RegClass::GPR) {
                            throw std::runtime_error(
                                "AArch64 terminator lowering: failed to materialize trap payload");
                        }
                        outBB.instrs.push_back(MInstr{MOpcode::MovRR,
                                                      {MOperand::regOp(PhysReg::X0),
                                                       MOperand::vregOp(RegClass::GPR, trapArg)}});
                        outBB.instrs.push_back(
                            MInstr{MOpcode::Bl, {MOperand::labelOp("rt_trap_string")}});
                    } else {
                        emitNullTrapCall(outBB);
                    }
                }
                break;
            }

            case Opcode::TrapFromErr: {
                bool hasRaiseErrorCall = false;
                for (const auto &mi : outBB.instrs) {
                    if (mi.opc == MOpcode::Bl && !mi.ops.empty() &&
                        mi.ops[0].kind == MOperand::Kind::Label &&
                        mi.ops[0].label == "rt_trap_raise_error") {
                        hasRaiseErrorCall = true;
                        break;
                    }
                }
                if (!hasRaiseErrorCall) {
                    if (term.operands.empty()) {
                        emitNullTrapCall(outBB);
                    } else {
                        uint16_t errToken = 0;
                        RegClass errCls = RegClass::GPR;
                        if (!materializeValueToVReg(term.operands[0],
                                                    inBB,
                                                    ti,
                                                    fb,
                                                    outBB,
                                                    blockTempVReg,
                                                    tempRegClass,
                                                    nextVRegId,
                                                    errToken,
                                                    errCls) ||
                            errCls != RegClass::GPR) {
                            throw std::runtime_error("AArch64 terminator lowering: failed to "
                                                     "materialize trap error token");
                        }
                        outBB.instrs.push_back(MInstr{MOpcode::MovRR,
                                                      {MOperand::regOp(PhysReg::X0),
                                                       MOperand::vregOp(RegClass::GPR, errToken)}});
                        outBB.instrs.push_back(
                            MInstr{MOpcode::Bl, {MOperand::labelOp("rt_trap_raise_error")}});
                    }
                }
                break;
            }

            case Opcode::CBr:
                lowerCBr(term,
                         inBB,
                         outBB,
                         i,
                         mf,
                         ti,
                         fb,
                         phiRegClass,
                         phiSpillOffset,
                         blockTempVReg,
                         tempRegClass,
                         nextVRegId);
                break;


            case Opcode::SwitchI32:
                lowerSwitchI32(term,
                               inBB,
                               outBB,
                               i,
                               mf,
                               ti,
                               fb,
                               phiRegClass,
                               phiSpillOffset,
                               blockTempVReg,
                               tempRegClass,
                               nextVRegId,
                               switchAuxCounter);
                break;

            case Opcode::ResumeLabel:
                // resume.label is a branch to an explicit target label.
                // The resume token operand is ignored in native codegen.
                if (!term.labels.empty()) {
                    outBB.instrs.push_back(
                        MInstr{MOpcode::Br, {MOperand::labelOp(term.labels[0])}});
                }
                break;

            default:
                break;
        }
    }
}

} // namespace zanna::codegen::aarch64
