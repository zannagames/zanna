//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/test_codegen_x86_64_live_intervals.cpp
// Purpose: Pins the x86-64 interval model of the function-wide allocator:
//          positions, ranges with holes across untaken arms, loop-carried
//          ranges covering the back edge, memory address registers as reads,
//          call positions and fixed ranges from the effects model, hints
//          from moves and PX_COPY pairs, and determinism.
// Key invariants:
//   - Every fact comes from operandRoles/effectsOf and the CFG liveness.
// Ownership/Lifetime: Standalone test binary.
// Links: src/codegen/x86_64/ra/LiveIntervals.hpp
//
//===----------------------------------------------------------------------===//

#include "tests/TestHarness.hpp"

#include "codegen/x86_64/MachineIR.hpp"
#include "codegen/x86_64/TargetX64.hpp"
#include "codegen/x86_64/ra/LiveIntervals.hpp"

#include <string>
#include <utility>
#include <vector>

using namespace zanna::codegen::x64;
using zanna::codegen::x64::ra::kNoPos;
using zanna::codegen::x64::ra::LiveIntervals;
using zanna::codegen::x64::ra::VRegInterval;

namespace {

Operand v(uint16_t id) {
    return makeVRegOperand(RegClass::GPR, id);
}

Operand x(uint16_t id) {
    return makeVRegOperand(RegClass::XMM, id);
}

Operand p(PhysReg reg) {
    return makePhysRegOperand(RegClass::GPR, static_cast<uint16_t>(reg));
}

Operand imm(int64_t val) {
    return makeImmOperand(val);
}

Operand lbl(const std::string &name) {
    return makeLabelOperand(name);
}

MBasicBlock block(const std::string &name, std::vector<MInstr> instrs) {
    MBasicBlock bb;
    bb.label = name;
    bb.instructions = std::move(instrs);
    return bb;
}

MFunction function(std::vector<MBasicBlock> blocks) {
    MFunction fn;
    fn.name = "f";
    fn.blocks = std::move(blocks);
    return fn;
}

const VRegInterval &interval(const LiveIntervals &li, uint16_t id) {
    const VRegInterval *iv = li.find(id);
    ASSERT_TRUE(iv != nullptr);
    return *iv;
}

} // namespace

TEST(X86LiveIntervals, StraightLineRangesAndPositions) {
    // v1 defined at write 1, read at 4 (add) -> [1,4]; v2 written at 3, read at 4 -> [3,4].
    MFunction fn = function({block("entry",
                                   {MInstr::make(MOpcode::MOVri, {v(1), imm(42)}),
                                    MInstr::make(MOpcode::MOVri, {v(2), imm(7)}),
                                    MInstr::make(MOpcode::ADDrr, {v(1), v(2)}),
                                    MInstr::make(MOpcode::MOVrr, {p(PhysReg::RAX), v(1)}),
                                    MInstr::make(MOpcode::RET, {})})});
    LiveIntervals li;
    li.build(fn, sysvTarget());
    EXPECT_EQ(li.positions().rpo.size(), 1u);
    EXPECT_EQ(li.positions().blockBase[0], 0u);
    EXPECT_EQ(li.positions().readPos(0, 2), 4u);
    EXPECT_EQ(li.positions().writePos(0, 2), 5u);
    EXPECT_EQ(interval(li, 1).live.toString(), "[1,6]"); // def, add (rmw at 4/5), read at 6
    EXPECT_EQ(interval(li, 2).live.toString(), "[3,4]");
    EXPECT_EQ(interval(li, 1).uses.size(), 2u);
    EXPECT_EQ(interval(li, 1).defs.size(), 2u); // mov and the add's write
    // The move to RAX hints v1 to RAX.
    EXPECT_TRUE(interval(li, 1).hasPhysHint);
    EXPECT_EQ(static_cast<int>(interval(li, 1).hintPhys), static_cast<int>(PhysReg::RAX));
}

TEST(X86LiveIntervals, DiamondLeavesAHoleOnTheUntakenArm) {
    // v1 is defined in entry and read only in the right arm and the join.
    MFunction fn = function({
        block("entry",
              {MInstr::make(MOpcode::MOVri, {v(1), imm(1)}),
               MInstr::make(MOpcode::JCC, {imm(4), lbl("right")}),
               MInstr::make(MOpcode::JMP, {lbl("left")})}),
        block("left",
              {MInstr::make(MOpcode::MOVri, {v(2), imm(2)}),
               MInstr::make(MOpcode::JMP, {lbl("join")})}),
        block("right",
              {MInstr::make(MOpcode::MOVrr, {v(2), v(1)}),
               MInstr::make(MOpcode::JMP, {lbl("join")})}),
        block("join", {MInstr::make(MOpcode::ADDrr, {v(2), v(1)}), MInstr::make(MOpcode::RET, {})}),
    });
    LiveIntervals li;
    li.build(fn, sysvTarget());
    const auto &pos = li.positions();
    // v1 is live through both arms (read in join), so no hole for v1; v2 has
    // one range per arm plus the join: the arms' ranges must not cover each
    // other's positions.
    const VRegInterval &v2 = interval(li, 2);
    EXPECT_GE(v2.live.ranges.size(), 2u);
    // The entry block's exit position is not in v2's ranges (defined later).
    EXPECT_FALSE(v2.live.contains(pos.blockExit[0]));
    const VRegInterval &v1 = interval(li, 1);
    EXPECT_TRUE(v1.live.contains(pos.blockExit[0]));
    EXPECT_TRUE(v1.live.contains(pos.blockBase[3]));
}

TEST(X86LiveIntervals, LoopCarriedValueCoversTheBackEdge) {
    MFunction fn = function({
        block("entry",
              {MInstr::make(MOpcode::MOVri, {v(1), imm(0)}),
               MInstr::make(MOpcode::JMP, {lbl("head")})}),
        block("head",
              {MInstr::make(MOpcode::ADDri, {v(1), imm(1)}),
               MInstr::make(MOpcode::CMPri, {v(1), imm(10)}),
               MInstr::make(MOpcode::JCC, {imm(12), lbl("head")}),
               MInstr::make(MOpcode::JMP, {lbl("exit")})}),
        block("exit",
              {MInstr::make(MOpcode::MOVrr, {p(PhysReg::RAX), v(1)}),
               MInstr::make(MOpcode::RET, {})}),
    });
    LiveIntervals li;
    li.build(fn, sysvTarget());
    const auto &pos = li.positions();
    const VRegInterval &v1 = interval(li, 1);
    EXPECT_TRUE(v1.live.contains(pos.blockBase[1]));
    EXPECT_TRUE(v1.live.contains(pos.blockExit[1]));
    EXPECT_GT(v1.weight, 10.0); // loop depth 1 weights the uses
    EXPECT_FALSE(v1.crossesCall);
}

TEST(X86LiveIntervals, MemoryAddressRegistersAreReads) {
    MFunction fn = function({block(
        "entry",
        {MInstr::make(MOpcode::MOVri, {v(1), imm(0)}),
         MInstr::make(MOpcode::MOVri, {v(2), imm(8)}),
         MInstr::make(MOpcode::MOVrm,
                      {makeMemOperand(makeVReg(RegClass::GPR, 1), makeVReg(RegClass::GPR, 2), 1, 0),
                       p(PhysReg::RAX)}),
         MInstr::make(MOpcode::RET, {})})});
    LiveIntervals li;
    li.build(fn, sysvTarget());
    EXPECT_EQ(interval(li, 1).live.toString(), "[1,4]");
    EXPECT_EQ(interval(li, 2).live.toString(), "[3,4]");
    EXPECT_EQ(interval(li, 1).uses.size(), 1u);
}

TEST(X86LiveIntervals, CallPositionsAndFixedRanges) {
    // rdi is marshalled before the call: fixed from its write to the call's
    // read. v1 lives across the call.
    MFunction fn = function({block("entry",
                                   {MInstr::make(MOpcode::MOVri, {v(1), imm(5)}),
                                    MInstr::make(MOpcode::MOVri, {p(PhysReg::RDI), imm(1)}),
                                    MInstr::make(MOpcode::CALL, {lbl("callee")}),
                                    MInstr::make(MOpcode::MOVrr, {p(PhysReg::RAX), v(1)}),
                                    MInstr::make(MOpcode::RET, {})})});
    LiveIntervals li;
    li.build(fn, sysvTarget());
    ASSERT_EQ(li.callPositions().size(), 1u);
    EXPECT_EQ(li.callPositions()[0], 5u);
    EXPECT_TRUE(interval(li, 1).crossesCall);
    EXPECT_FALSE(interval(li, 1).crossesEhPush);
    EXPECT_TRUE(li.fixed(PhysReg::RDI).contains(3));
    EXPECT_TRUE(li.fixed(PhysReg::RDI).contains(4));
    // Caller-saved registers are clobbered at the call's write position.
    EXPECT_TRUE(li.fixed(PhysReg::RCX).contains(5));
    EXPECT_FALSE(li.fixed(PhysReg::RBX).contains(5));
    // RAX is read by the return.
    EXPECT_TRUE(li.fixed(PhysReg::RAX).contains(8));
}

TEST(X86LiveIntervals, EhPushCallIsMemoryHomedAndImplicitEffectsAreFixed) {
    MFunction fn = function({block("entry",
                                   {MInstr::make(MOpcode::MOVri, {v(1), imm(5)}),
                                    MInstr::make(MOpcode::CALL, {lbl("rt_native_eh_push")}),
                                    MInstr::make(MOpcode::MOVrr, {p(PhysReg::RAX), v(1)}),
                                    MInstr::make(MOpcode::CQO, {}),
                                    MInstr::make(MOpcode::RET, {})})});
    LiveIntervals li;
    li.build(fn, sysvTarget());
    ASSERT_EQ(li.ehPushPositions().size(), 1u);
    EXPECT_TRUE(interval(li, 1).crossesEhPush);
    // CQO reads RAX and writes RDX implicitly.
    EXPECT_TRUE(li.fixed(PhysReg::RDX).contains(7));
    EXPECT_TRUE(li.fixed(PhysReg::RAX).contains(6));
}

TEST(X86LiveIntervals, ParallelCopyPairsHintEachOther) {
    MFunction fn = function({
        block("entry",
              {MInstr::make(MOpcode::PX_COPY, {v(1), p(PhysReg::RDI)}),
               MInstr::make(MOpcode::PX_COPY, {v(2), v(1)}),
               MInstr::make(MOpcode::MOVrr, {p(PhysReg::RAX), v(2)}),
               MInstr::make(MOpcode::RET, {})}),
    });
    LiveIntervals li;
    li.build(fn, sysvTarget());
    EXPECT_TRUE(interval(li, 1).hasPhysHint);
    EXPECT_EQ(static_cast<int>(interval(li, 1).hintPhys), static_cast<int>(PhysReg::RDI));
    ASSERT_EQ(interval(li, 2).hintVRegs.size(), 1u);
    EXPECT_EQ(interval(li, 2).hintVRegs[0], 1);
    EXPECT_TRUE(interval(li, 2).hasPhysHint);
}

TEST(X86LiveIntervals, XmmAndGprClassesAreKept) {
    MFunction fn = function({block(
        "entry",
        {MInstr::make(MOpcode::MOVri, {v(1), imm(1)}),
         MInstr::make(
             MOpcode::MOVSDrr,
             {x(2), makePhysRegOperand(RegClass::XMM, static_cast<uint16_t>(PhysReg::XMM0))}),
         MInstr::make(MOpcode::FADD, {x(2), x(2)}),
         MInstr::make(MOpcode::RET, {})})});
    LiveIntervals li;
    li.build(fn, sysvTarget());
    EXPECT_EQ(static_cast<int>(interval(li, 1).cls), static_cast<int>(RegClass::GPR));
    EXPECT_EQ(static_cast<int>(interval(li, 2).cls), static_cast<int>(RegClass::XMM));
    EXPECT_TRUE(interval(li, 2).hasPhysHint);
}

TEST(X86LiveIntervals, IsDeterministic) {
    MFunction fn = function({
        block("entry",
              {MInstr::make(MOpcode::MOVri, {v(1), imm(0)}),
               MInstr::make(MOpcode::JMP, {lbl("head")})}),
        block("head",
              {MInstr::make(MOpcode::ADDri, {v(1), imm(1)}),
               MInstr::make(MOpcode::JCC, {imm(12), lbl("head")}),
               MInstr::make(MOpcode::JMP, {lbl("exit")})}),
        block("exit",
              {MInstr::make(MOpcode::MOVrr, {p(PhysReg::RAX), v(1)}),
               MInstr::make(MOpcode::RET, {})}),
    });
    LiveIntervals a;
    a.build(fn, sysvTarget());
    LiveIntervals b;
    b.build(fn, sysvTarget());
    EXPECT_EQ(a.dump(), b.dump());
    EXPECT_FALSE(a.dump().empty());
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, argv);
    return zanna_test::run_all_tests();
}
