//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/codegen/test_aarch64_phys_liveness.cpp
// Purpose: Pins the function-wide physical-register liveness every post-RA
//          rewrite reads (computePhysLiveness) and the exit-live seed built
//          on it (blockExitLive): reads in a successor keep a register live,
//          a redefinition kills it, call clobbers and argument reads come
//          from the effects model, a loop-carried register is live around
//          the back edge, and a return reads the result registers.
// Key invariants:
//   - The solver reads only effectsOf() and MirCfg; nothing else feeds the
//     exit-live set.
// Ownership/Lifetime: Standalone test binary.
// Links: src/codegen/aarch64/PhysLiveness.hpp, src/codegen/aarch64/MirCfg.hpp,
//        docs/internals/backend-codegen-review-2026-09.md (Phase 3 C4)
//
//===----------------------------------------------------------------------===//

#include "tests/TestHarness.hpp"

#include "codegen/aarch64/MirCfg.hpp"
#include "codegen/aarch64/PhysLiveness.hpp"
#include "codegen/aarch64/TargetAArch64.hpp"

#include <string>
#include <utility>
#include <vector>

using namespace zanna::codegen::aarch64;

namespace {

MOperand x(PhysReg r) {
    return MOperand::regOp(r);
}

MOperand label(const char *name) {
    return MOperand::labelOp(name);
}

MInstr ins(MOpcode opc, std::vector<MOperand> ops) {
    return MInstr{opc, std::move(ops)};
}

MBasicBlock block(const char *name, std::vector<MInstr> instrs) {
    MBasicBlock bb;
    bb.name = name;
    bb.instrs = std::move(instrs);
    return bb;
}

MFunction function(std::vector<MBasicBlock> blocks) {
    MFunction fn;
    fn.name = "f";
    fn.blocks = std::move(blocks);
    return fn;
}

MInstr br(const char *target) {
    return ins(MOpcode::Br, {label(target)});
}

MInstr bcond(const char *target) {
    return ins(MOpcode::BCond, {MOperand::condOp("eq"), label(target)});
}

MInstr ret() {
    return ins(MOpcode::Ret, {});
}

MInstr movri(PhysReg dst, long long imm) {
    return ins(MOpcode::MovRI, {x(dst), MOperand::immOp(imm)});
}

MInstr movrr(PhysReg dst, PhysReg src) {
    return ins(MOpcode::MovRR, {x(dst), x(src)});
}

MInstr add(PhysReg dst, PhysReg a, PhysReg b) {
    return ins(MOpcode::AddRRR, {x(dst), x(a), x(b)});
}

const TargetInfo &target() {
    return darwinTarget();
}

} // namespace

// ---------------------------------------------------------------------------
// computePhysLiveness
// ---------------------------------------------------------------------------

TEST(AArch64PhysLiveness, SuccessorReadKeepsRegisterLiveAcrossTheEdge) {
    MFunction fn = function({
        block("entry", {movri(PhysReg::X1, 1), br("next")}),
        block("next", {movrr(PhysReg::X0, PhysReg::X1), ret()}),
    });
    const PhysLiveness lv = computePhysLiveness(fn, target());
    ASSERT_EQ(lv.liveOut.size(), 2u);
    EXPECT_TRUE(lv.liveOut[0].contains(PhysReg::X1));
    EXPECT_TRUE(lv.liveIn[1].contains(PhysReg::X1));
    EXPECT_FALSE(lv.liveIn[0].contains(PhysReg::X1));
    // Nothing reads x2 anywhere.
    EXPECT_FALSE(lv.liveOut[0].contains(PhysReg::X2));
}

TEST(AArch64PhysLiveness, SuccessorRedefinitionKillsTheRegister) {
    MFunction fn = function({
        block("entry", {movri(PhysReg::X1, 1), br("next")}),
        block("next", {movri(PhysReg::X1, 2), movrr(PhysReg::X0, PhysReg::X1), ret()}),
    });
    const PhysLiveness lv = computePhysLiveness(fn, target());
    EXPECT_FALSE(lv.liveOut[0].contains(PhysReg::X1));
    EXPECT_FALSE(lv.liveIn[1].contains(PhysReg::X1));
}

TEST(AArch64PhysLiveness, CallClobberKillsCallerSavedAndReadsArguments) {
    // x10 is read after the call, but the call clobbers it: not live-in.
    // x0 is an argument register the call reads (every argument register
    // counts as read for a callee of unknown arity, `Bl` carries none):
    // live-in.
    MFunction fn = function({
        block("entry", {movri(PhysReg::X0, 0), movri(PhysReg::X10, 1), br("next")}),
        block("next",
              {ins(MOpcode::Bl, {label("callee")}), movrr(PhysReg::X0, PhysReg::X10), ret()}),
    });
    const PhysLiveness lv = computePhysLiveness(fn, target());
    EXPECT_FALSE(lv.liveIn[1].contains(PhysReg::X10));
    EXPECT_FALSE(lv.liveOut[0].contains(PhysReg::X10));
    EXPECT_TRUE(lv.liveIn[1].contains(PhysReg::X0));
    EXPECT_TRUE(lv.liveOut[0].contains(PhysReg::X0));

    // A callee-saved register survives the call: live across it.
    MFunction saved = function({
        block("entry", {movri(PhysReg::X20, 1), br("next")}),
        block("next",
              {ins(MOpcode::Bl, {label("callee")}), movrr(PhysReg::X0, PhysReg::X20), ret()}),
    });
    const PhysLiveness slv = computePhysLiveness(saved, target());
    EXPECT_TRUE(slv.liveOut[0].contains(PhysReg::X20));
}

TEST(AArch64PhysLiveness, NoReturnTrapCallReadsOnlyItsArguments) {
    // The shared overflow trap takes no arguments, so its call reads no
    // argument register: a loop that can trap must not see its scratch
    // registers pinned live around the back edge by the trap block.
    MFunction fn = function({
        block("entry", {movri(PhysReg::X5, 1), br("loop")}),
        block("loop",
              {ins(MOpcode::AndRI, {x(PhysReg::X5), x(PhysReg::X5), MOperand::immOp(1)}),
               bcond("trap"),
               br("loop")}),
        block("trap", {ins(MOpcode::Bl, {label("rt_trap_ovf")})}),
    });
    const PhysLiveness lv = computePhysLiveness(fn, target());
    EXPECT_FALSE(lv.liveIn[2].contains(PhysReg::X0));
    EXPECT_FALSE(lv.liveIn[2].contains(PhysReg::X5));
    EXPECT_FALSE(lv.liveIn[2].contains(PhysReg::V0));

    // The two-argument bounds trap reads x0 and x1 but not x2.
    MFunction oob = function({
        block("entry",
              {movri(PhysReg::X0, 1), movri(PhysReg::X1, 2), movri(PhysReg::X2, 3), br("trap")}),
        block("trap", {ins(MOpcode::Bl, {label("rt_arr_oob_panic")})}),
    });
    const PhysLiveness olv = computePhysLiveness(oob, target());
    EXPECT_TRUE(olv.liveIn[1].contains(PhysReg::X0));
    EXPECT_TRUE(olv.liveIn[1].contains(PhysReg::X1));
    EXPECT_FALSE(olv.liveIn[1].contains(PhysReg::X2));
}

TEST(AArch64PhysLiveness, ReturnReadsTheResultRegisters) {
    MFunction fn = function({
        block("entry", {movri(PhysReg::X0, 7), br("exit")}),
        block("exit", {ret()}),
    });
    const PhysLiveness lv = computePhysLiveness(fn, target());
    EXPECT_TRUE(lv.liveIn[1].contains(PhysReg::X0));
    EXPECT_TRUE(lv.liveIn[1].contains(PhysReg::V0));
    EXPECT_TRUE(lv.liveOut[0].contains(PhysReg::X0));
    // A block that leaves the function has no live-out of its own.
    EXPECT_TRUE(lv.liveOut[1].empty());
}

TEST(AArch64PhysLiveness, LoopCarriedRegisterIsLiveAroundTheBackEdge) {
    // header: cmp/branch on x19; body: x19 = x19 + x20; latch back to header.
    MFunction fn = function({
        block("entry", {movri(PhysReg::X19, 0), movri(PhysReg::X20, 1), br("header")}),
        block("header",
              {ins(MOpcode::CmpRI, {x(PhysReg::X19), MOperand::immOp(10)}), bcond("exit")}),
        block("body", {add(PhysReg::X19, PhysReg::X19, PhysReg::X20), br("header")}),
        block("exit", {movrr(PhysReg::X0, PhysReg::X19), ret()}),
    });
    const PhysLiveness lv = computePhysLiveness(fn, target());
    // x19 and x20 are live around the whole loop.
    EXPECT_TRUE(lv.liveOut[0].contains(PhysReg::X19));
    EXPECT_TRUE(lv.liveOut[0].contains(PhysReg::X20));
    EXPECT_TRUE(lv.liveIn[1].contains(PhysReg::X19));
    EXPECT_TRUE(lv.liveIn[1].contains(PhysReg::X20));
    EXPECT_TRUE(lv.liveOut[2].contains(PhysReg::X19));
    EXPECT_TRUE(lv.liveOut[2].contains(PhysReg::X20));
    // Only x19 leaves the loop.
    EXPECT_TRUE(lv.liveIn[3].contains(PhysReg::X19));
    EXPECT_FALSE(lv.liveIn[3].contains(PhysReg::X20));
}

TEST(AArch64PhysLiveness, DiamondJoinReadIsLiveOnBothArms) {
    MFunction fn = function({
        block("entry", {movri(PhysReg::X3, 1), bcond("right")}),
        block("left", {movri(PhysReg::X4, 2), br("join")}),
        block("right", {movri(PhysReg::X4, 3), br("join")}),
        block("join", {add(PhysReg::X0, PhysReg::X3, PhysReg::X4), ret()}),
    });
    const PhysLiveness lv = computePhysLiveness(fn, target());
    EXPECT_TRUE(lv.liveOut[1].contains(PhysReg::X3));
    EXPECT_TRUE(lv.liveOut[2].contains(PhysReg::X3));
    EXPECT_TRUE(lv.liveOut[1].contains(PhysReg::X4));
    // x4 is defined on both arms, so it is not live into either.
    EXPECT_FALSE(lv.liveIn[1].contains(PhysReg::X4));
    EXPECT_FALSE(lv.liveOut[0].contains(PhysReg::X4));
    EXPECT_TRUE(lv.liveOut[0].contains(PhysReg::X3));
}

TEST(AArch64PhysLiveness, IsDeterministic) {
    MFunction fn = function({
        block("entry", {movri(PhysReg::X1, 1), movri(PhysReg::X2, 2), bcond("b")}),
        block("a", {movrr(PhysReg::X0, PhysReg::X1), ret()}),
        block("b", {movrr(PhysReg::X0, PhysReg::X2), ret()}),
    });
    const PhysLiveness first = computePhysLiveness(fn, target());
    const PhysLiveness second = computePhysLiveness(fn, target());
    ASSERT_EQ(first.liveOut.size(), second.liveOut.size());
    for (std::size_t bi = 0; bi < first.liveOut.size(); ++bi) {
        EXPECT_EQ(first.liveOut[bi].bits, second.liveOut[bi].bits);
        EXPECT_EQ(first.liveIn[bi].bits, second.liveIn[bi].bits);
    }
}

// ---------------------------------------------------------------------------
// blockExitLive on top of the solved liveness
// ---------------------------------------------------------------------------

TEST(AArch64PhysLiveness, ExitLiveIsSolvedLiveOutPlusFrameRegisters) {
    MFunction fn = function({
        block("entry", {movri(PhysReg::X1, 1), movri(PhysReg::X20, 2), br("next")}),
        block("next", {movrr(PhysReg::X0, PhysReg::X1), ret()}),
    });
    const PhysLiveness lv = computePhysLiveness(fn, target());
    const PhysRegSet live = blockExitLive(fn, 0, target(), lv);
    EXPECT_TRUE(live.contains(PhysReg::X1));
    EXPECT_TRUE(live.contains(PhysReg::SP));
    EXPECT_TRUE(live.contains(PhysReg::X29));
    EXPECT_TRUE(live.contains(PhysReg::X30));
    // Not a function exit: the return registers are not seeded. x0 is
    // written in `next` before its `ret` reads it, so it is dead here; v0 is
    // live only because that `ret` reads it and nothing writes it first.
    EXPECT_FALSE(live.contains(PhysReg::X0));
    EXPECT_TRUE(live.contains(PhysReg::V0));
    // A callee-saved register nobody reads later is dead.
    EXPECT_FALSE(live.contains(PhysReg::X20));
    EXPECT_FALSE(live.contains(PhysReg::X19));
}

TEST(AArch64PhysLiveness, ExitLiveAddsReturnRegistersAtAFunctionExit) {
    MFunction fn = function({block("entry", {ret()})});
    const PhysLiveness lv = computePhysLiveness(fn, target());
    const PhysRegSet live = blockExitLive(fn, 0, target(), lv);
    EXPECT_TRUE(live.contains(PhysReg::X0));
    EXPECT_TRUE(live.contains(PhysReg::V0));
    EXPECT_FALSE(live.contains(PhysReg::X1));
    EXPECT_FALSE(live.contains(PhysReg::X19));
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, &argv);
    return zanna_test::run_all_tests();
}
