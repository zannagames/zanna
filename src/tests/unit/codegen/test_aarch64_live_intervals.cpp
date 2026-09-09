//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/codegen/test_aarch64_live_intervals.cpp
// Purpose: Pins the interval model of the AArch64 function-wide allocator:
//          the position layout (reverse post-order, read/write half
//          positions, block exit), range lists with holes (diamond, loop
//          back edge, redefinition inside a block), fixed physical ranges
//          from the effects model (explicit writes to their last read, call
//          clobbers as points, ABI live-ins from the block start, the return
//          registers read by `ret`), call and EH-push crossing flags, spill
//          weights by loop depth, hints from moves and parallel copies, and
//          determinism.
// Key invariants:
//   - Every expectation is an exact range string, so a change in the
//     position convention fails here first.
// Ownership/Lifetime: Standalone test binary.
// Links: src/codegen/aarch64/ra/LiveIntervals.hpp,
//        docs/internals/backend-codegen-review-2026-09.md (Phase 3 C5)
//
//===----------------------------------------------------------------------===//

#include "tests/TestHarness.hpp"

#include "codegen/aarch64/TargetAArch64.hpp"
#include "codegen/aarch64/ra/LiveIntervals.hpp"

#include <string>
#include <utility>
#include <vector>

using namespace zanna::codegen::aarch64;
using zanna::codegen::aarch64::ra::LiveIntervals;
using zanna::codegen::aarch64::ra::RangeList;

namespace {

MOperand v(uint16_t id, RegClass cls = RegClass::GPR) {
    return MOperand::vregOp(cls, id);
}

MOperand x(PhysReg r) {
    return MOperand::regOp(r);
}

MOperand imm(long long value) {
    return MOperand::immOp(value);
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

const TargetInfo &target() {
    return darwinTarget();
}

std::string ranges(const LiveIntervals &li, uint16_t id) {
    const auto *iv = li.find(id);
    return iv ? iv->live.toString() : std::string("<none>");
}

} // namespace

// ---------------------------------------------------------------------------
// RangeList
// ---------------------------------------------------------------------------

TEST(AArch64LiveIntervals, RangeListMergesOverlapsAndAdjacency) {
    RangeList rl;
    rl.add(10, 12);
    rl.add(1, 3);
    rl.add(5, 6);
    EXPECT_EQ(rl.toString(), "[1,3] [5,6] [10,12]");
    rl.add(4, 4); // adjacent to both neighbours: [1,6]
    EXPECT_EQ(rl.toString(), "[1,6] [10,12]");
    rl.add(7, 9); // bridges to [1,12]
    EXPECT_EQ(rl.toString(), "[1,12]");
    EXPECT_TRUE(rl.contains(1));
    EXPECT_TRUE(rl.contains(12));
    EXPECT_FALSE(rl.contains(0));
    EXPECT_FALSE(rl.contains(13));

    RangeList other;
    other.add(13, 20);
    EXPECT_FALSE(rl.intersects(other));
    other.add(12, 12);
    EXPECT_TRUE(rl.intersects(other));
    EXPECT_EQ(rl.firstIntersection(other), 12u);
}

// ---------------------------------------------------------------------------
// Straight line
// ---------------------------------------------------------------------------

TEST(AArch64LiveIntervals, StraightLinePositionsAndRanges) {
    // i0: mov v1,#1   reads 0 / writes 1
    // i1: mov v2,#2   reads 2 / writes 3
    // i2: add v3,v1,v2 reads 4 / writes 5
    // i3: mov x0,v3   reads 6 / writes 7
    // i4: ret         reads 8 ; exit position 11
    MFunction fn = function({block("entry",
                                   {ins(MOpcode::MovRI, {v(1), imm(1)}),
                                    ins(MOpcode::MovRI, {v(2), imm(2)}),
                                    ins(MOpcode::AddRRR, {v(3), v(1), v(2)}),
                                    ins(MOpcode::MovRR, {x(PhysReg::X0), v(3)}),
                                    ins(MOpcode::Ret, {})})});
    LiveIntervals li;
    li.build(fn, target());
    const auto &pos = li.positions();
    ASSERT_EQ(pos.rpo.size(), 1u);
    EXPECT_EQ(pos.blockBase[0], 0u);
    EXPECT_EQ(pos.blockExit[0], 11u);
    EXPECT_EQ(pos.readPos(0, 2), 4u);
    EXPECT_EQ(pos.writePos(0, 2), 5u);

    EXPECT_EQ(ranges(li, 1), "[1,4]");
    EXPECT_EQ(ranges(li, 2), "[3,4]");
    EXPECT_EQ(ranges(li, 3), "[5,6]");
    // x0 is written at 7 and read by the return at 8.
    EXPECT_EQ(li.fixed(PhysReg::X0).toString(), "[7,8]");
    // v0 is read by the return and never written: live-in from the start.
    EXPECT_EQ(li.fixed(PhysReg::V0).toString(), "[0,8]");
    // The move into x0 hints v3.
    ASSERT_TRUE(li.find(3) != nullptr);
    EXPECT_TRUE(li.find(3)->hasPhysHint());
    EXPECT_EQ(li.find(3)->hintPhys, PhysReg::X0);
    EXPECT_FALSE(li.find(1)->crossesCall);
}

TEST(AArch64LiveIntervals, DeadDefinitionOccupiesItsWritePosition) {
    MFunction fn = function({block("entry",
                                   {ins(MOpcode::MovRI, {v(1), imm(1)}),
                                    ins(MOpcode::MovRI, {v(2), imm(2)}), // dead
                                    ins(MOpcode::MovRR, {x(PhysReg::X0), v(1)}),
                                    ins(MOpcode::Ret, {})})});
    LiveIntervals li;
    li.build(fn, target());
    EXPECT_EQ(ranges(li, 2), "[3,3]");
    EXPECT_EQ(ranges(li, 1), "[1,4]");
}

TEST(AArch64LiveIntervals, RedefinitionInsideABlockOpensASecondRange) {
    // v1 is defined, used, redefined, used again: two ranges with a hole.
    MFunction fn = function({block("entry",
                                   {ins(MOpcode::MovRI, {v(1), imm(1)}),         // w1
                                    ins(MOpcode::MovRR, {x(PhysReg::X1), v(1)}), // r2
                                    ins(MOpcode::MovRI, {v(2), imm(9)}),         // w5
                                    ins(MOpcode::MovRI, {v(1), imm(3)}),         // w7
                                    ins(MOpcode::AddRRR, {v(3), v(1), v(2)}),    // r8
                                    ins(MOpcode::MovRR, {x(PhysReg::X0), v(3)}),
                                    ins(MOpcode::Ret, {})})});
    LiveIntervals li;
    li.build(fn, target());
    EXPECT_EQ(ranges(li, 1), "[1,2] [7,8]");
    EXPECT_EQ(ranges(li, 2), "[5,8]");
    // x1 is written at 3 and never read again: a point.
    EXPECT_EQ(li.fixed(PhysReg::X1).toString(), "[3,3]");
}

// ---------------------------------------------------------------------------
// Control flow
// ---------------------------------------------------------------------------

TEST(AArch64LiveIntervals, LoopCarriedParameterCoversTheBackEdge) {
    // entry(0): mov v1,#0 ; pcopy v10<-v1 ; br header       base 0, exit 7
    // header(1): cmp v10,#10 ; b.ge exit                     base 8, exit 13
    // body(2): add v11,v10,#1 ; pcopy v10<-v11 ; br header   (rpo last)
    // exit(3): mov x0,v10 ; ret
    MFunction fn = function({
        block("entry",
              {ins(MOpcode::MovRI, {v(1), imm(0)}),
               ins(MOpcode::ParallelCopy, {v(10), v(1)}),
               ins(MOpcode::Br, {label("header")})}),
        block("header",
              {ins(MOpcode::CmpRI, {v(10), imm(10)}),
               ins(MOpcode::BCond, {MOperand::condOp("ge"), label("exit")})}),
        block("body",
              {ins(MOpcode::AddRI, {v(11), v(10), imm(1)}),
               ins(MOpcode::ParallelCopy, {v(10), v(11)}),
               ins(MOpcode::Br, {label("header")})}),
        block("exit", {ins(MOpcode::MovRR, {x(PhysReg::X0), v(10)}), ins(MOpcode::Ret, {})}),
    });
    LiveIntervals li;
    li.build(fn, target());
    const auto &pos = li.positions();
    ASSERT_EQ(pos.rpo, (std::vector<std::size_t>{0, 1, 3, 2}));
    EXPECT_EQ(pos.blockBase[0], 0u);
    EXPECT_EQ(pos.blockBase[1], 8u);
    EXPECT_EQ(pos.blockBase[3], 14u);
    EXPECT_EQ(pos.blockBase[2], 20u);
    EXPECT_EQ(pos.blockExit[2], 27u);
    EXPECT_EQ(pos.loopDepth[1], 1u);
    EXPECT_EQ(pos.loopDepth[2], 1u);
    EXPECT_EQ(pos.loopDepth[0], 0u);
    EXPECT_EQ(pos.loopDepth[3], 0u);

    EXPECT_EQ(ranges(li, 1), "[1,2]");
    // v10: defined by the entry copy (3) to the entry exit (7), through the
    // header (8..13), read at the exit block start (14), read in the body
    // (20), redefined by the back-edge copy (23) to the body exit (27).
    EXPECT_EQ(ranges(li, 10), "[3,14] [20,20] [23,27]");
    EXPECT_EQ(ranges(li, 11), "[21,22]");
    // Loop-depth weights: v11 has one def and one use inside the loop.
    EXPECT_NEAR(li.find(11)->weight, 20.0, 1e-9);
    // v10: def(entry) + use(header) + use(exit) + use(body) + def(body) = 1 + 10 + 1 + 10 + 10.
    EXPECT_NEAR(li.find(10)->weight, 32.0, 1e-9);
    // The parallel copies link v10 with v1 and v11.
    const auto &hints = li.find(10)->hintVRegs;
    EXPECT_EQ(hints.size(), 2u);
}

TEST(AArch64LiveIntervals, SelfCopyOnTheBackEdgeKeepsTheParameterLiveThroughTheBody) {
    // A loop parameter the body never changes reaches the header again
    // through a self parallel copy in the latch. The copy's destination
    // precedes its source, so a gen/kill walk in operand order kills the
    // value before seeing its own read; the latch then has no upward-exposed
    // use, the body's live-out loses the value, and the body gets a hole in
    // which the allocator hands its register to a temporary (Legacy
    // Baseball's type_book.ensure trapped on a pointer-valued index this way).
    // entry(0): mov v1,#0 ; pcopy v10<-v1 ; br header       base 0, exit 7
    // header(1): cmp v10,#10 ; b.ge exit                     base 8, exit 13
    // exit(4): mov x0,v10 ; ret                              base 14, exit 19
    // body(2): mov v11,#1 ; br latch                         base 20, exit 25
    // latch(3): pcopy v10<-v10 ; br header                   base 26, exit 31
    MFunction fn = function({
        block("entry",
              {ins(MOpcode::MovRI, {v(1), imm(0)}),
               ins(MOpcode::ParallelCopy, {v(10), v(1)}),
               ins(MOpcode::Br, {label("header")})}),
        block("header",
              {ins(MOpcode::CmpRI, {v(10), imm(10)}),
               ins(MOpcode::BCond, {MOperand::condOp("ge"), label("exit")})}),
        block("body", {ins(MOpcode::MovRI, {v(11), imm(1)}), ins(MOpcode::Br, {label("latch")})}),
        block("latch",
              {ins(MOpcode::ParallelCopy, {v(10), v(10)}), ins(MOpcode::Br, {label("header")})}),
        block("exit", {ins(MOpcode::MovRR, {x(PhysReg::X0), v(10)}), ins(MOpcode::Ret, {})}),
    });
    LiveIntervals li;
    li.build(fn, target());
    ASSERT_EQ(li.positions().rpo, (std::vector<std::size_t>{0, 1, 4, 2, 3}));
    EXPECT_EQ(li.positions().blockBase[2], 20u);
    EXPECT_EQ(li.positions().blockBase[3], 26u);
    // v10 is live from the entry copy through the header and the exit read,
    // then through the whole body and latch: no hole at [20,25].
    EXPECT_EQ(ranges(li, 10), "[3,14] [20,31]");
    EXPECT_EQ(ranges(li, 11), "[21,21]");
}

TEST(AArch64LiveIntervals, DiamondLeavesAHoleOnTheUntakenArm) {
    // entry(0): mov v1,#1 ; mov v2,#2 ; cbz v1, right   base 0 (3 instrs) exit 7
    // left(1): mov v3,#3 ; pcopy v20<-v3 ; br join      base 8 exit 15
    // right(2): mov v4,#4 ; pcopy v20<-v4 ; br join     rpo: 0,1,3?,2 -> see below
    // join(3): add v5,v20,v2 ; mov x0,v5 ; ret
    MFunction fn = function({
        block("entry",
              {ins(MOpcode::MovRI, {v(1), imm(1)}),
               ins(MOpcode::MovRI, {v(2), imm(2)}),
               ins(MOpcode::Cbz, {v(1), label("right")})}),
        block("left",
              {ins(MOpcode::MovRI, {v(3), imm(3)}),
               ins(MOpcode::ParallelCopy, {v(20), v(3)}),
               ins(MOpcode::Br, {label("join")})}),
        block("right",
              {ins(MOpcode::MovRI, {v(4), imm(4)}),
               ins(MOpcode::ParallelCopy, {v(20), v(4)}),
               ins(MOpcode::Br, {label("join")})}),
        block("join",
              {ins(MOpcode::AddRRR, {v(5), v(20), v(2)}),
               ins(MOpcode::MovRR, {x(PhysReg::X0), v(5)}),
               ins(MOpcode::Ret, {})}),
    });
    LiveIntervals li;
    li.build(fn, target());
    const auto &pos = li.positions();
    // DFS from entry visits left (1) first and reaches the join (3) through
    // it, so the join finishes before left, and right (2) finishes last but
    // one: post-order 3,1,2,0 gives reverse post-order 0,2,1,3.
    ASSERT_EQ(pos.rpo, (std::vector<std::size_t>{0, 2, 1, 3}));
    EXPECT_EQ(pos.blockBase[2], 8u);
    EXPECT_EQ(pos.blockBase[1], 16u);
    EXPECT_EQ(pos.blockBase[3], 24u);
    // v2 is live through both arms into the join: one continuous range.
    EXPECT_EQ(ranges(li, 2), "[3,24]");
    // v3 lives only on the left arm (positions 16..23), v4 only on the right
    // arm (8..15).
    EXPECT_EQ(ranges(li, 3), "[17,18]");
    EXPECT_EQ(ranges(li, 4), "[9,10]");
    // v20 is defined by each arm's copy and read at the join start; the
    // right arm's range ends at that arm's exit (a hole across the left arm's
    // instructions until its own copy).
    EXPECT_EQ(ranges(li, 20), "[11,15] [19,24]");
}

// ---------------------------------------------------------------------------
// Calls, ABI live-ins, EH
// ---------------------------------------------------------------------------

TEST(AArch64LiveIntervals, CallClobbersArePointsAndMarshallingIsFixed) {
    // i0: mov v1,#7       w1
    // i1: mov v2,#9       w3
    // i2: mov x0,v1       r4 w5   (x0 fixed from 5 to the call's read at 6)
    // i3: bl f            r6 w7   (clobber points at 7; x0 result read at 8)
    // i4: mov v3,x0       r8 w9
    // i5: add v4,v3,v2    r10 w11
    // i6: mov x0,v4       r12 w13
    // i7: ret             r14
    MFunction fn = function({block("entry",
                                   {ins(MOpcode::MovRI, {v(1), imm(7)}),
                                    ins(MOpcode::MovRI, {v(2), imm(9)}),
                                    ins(MOpcode::MovRR, {x(PhysReg::X0), v(1)}),
                                    ins(MOpcode::Bl, {label("f")}),
                                    ins(MOpcode::MovRR, {v(3), x(PhysReg::X0)}),
                                    ins(MOpcode::AddRRR, {v(4), v(3), v(2)}),
                                    ins(MOpcode::MovRR, {x(PhysReg::X0), v(4)}),
                                    ins(MOpcode::Ret, {})})});
    LiveIntervals li;
    li.build(fn, target());
    ASSERT_EQ(li.callPositions().size(), 1u);
    EXPECT_EQ(li.callPositions()[0], 7u);
    // x0: marshalled at 5, read by the call at 6, clobbered/defined by the
    // call at 7, read by the result move at 8, then the return value 13..14.
    EXPECT_EQ(li.fixed(PhysReg::X0).toString(), "[5,8] [13,14]");
    // A caller-saved register nobody names: only the call clobber point.
    EXPECT_EQ(li.fixed(PhysReg::X10).toString(), "[7,7]");
    // A callee-saved register is untouched by the call.
    EXPECT_TRUE(li.fixed(PhysReg::X19).empty());
    // The other argument registers are read by the call (no arity): live-in
    // from the start to the call, then clobbered.
    EXPECT_EQ(li.fixed(PhysReg::X1).toString(), "[0,7]");
    EXPECT_TRUE(li.find(2)->crossesCall);
    EXPECT_FALSE(li.find(1)->crossesCall);
    EXPECT_FALSE(li.find(3)->crossesCall);
    EXPECT_EQ(li.find(1)->hintPhys, PhysReg::X0);
    EXPECT_EQ(li.find(3)->hintPhys, PhysReg::X0);
}

TEST(AArch64LiveIntervals, AbiLiveInIsFixedFromTheEntry) {
    // x0 is an incoming argument read at i1; x1 at i3.
    MFunction fn = function({block("entry",
                                   {ins(MOpcode::MovRI, {v(9), imm(1)}),
                                    ins(MOpcode::MovRR, {v(1), x(PhysReg::X0)}),
                                    ins(MOpcode::AddRRR, {v(2), v(1), v(9)}),
                                    ins(MOpcode::AddRRR, {v(3), v(2), x(PhysReg::X1)}),
                                    ins(MOpcode::MovRR, {x(PhysReg::X0), v(3)}),
                                    ins(MOpcode::Ret, {})})});
    LiveIntervals li;
    li.build(fn, target());
    EXPECT_EQ(li.fixed(PhysReg::X0).toString(), "[0,2] [9,10]");
    EXPECT_EQ(li.fixed(PhysReg::X1).toString(), "[0,6]");
    EXPECT_EQ(li.find(1)->hintPhys, PhysReg::X0);
}

TEST(AArch64LiveIntervals, EhPushCrossingIsFlagged) {
    MFunction fn = function({block("entry",
                                   {ins(MOpcode::MovRI, {v(1), imm(7)}),
                                    ins(MOpcode::Bl, {label("rt_native_eh_push")}),
                                    ins(MOpcode::MovRI, {v(2), imm(8)}),
                                    ins(MOpcode::AddRRR, {v(3), v(1), v(2)}),
                                    ins(MOpcode::MovRR, {x(PhysReg::X0), v(3)}),
                                    ins(MOpcode::Ret, {})})});
    LiveIntervals li;
    li.build(fn, target());
    ASSERT_EQ(li.ehPushPositions().size(), 1u);
    EXPECT_TRUE(li.find(1)->crossesEhPush);
    EXPECT_TRUE(li.find(1)->crossesCall);
    EXPECT_FALSE(li.find(2)->crossesEhPush);

    // The setjmp the native EH lowering places after the push counts too
    // (both spellings).
    for (const char *sym : {"setjmp", "_setjmp"}) {
        MFunction sj = function({block("entry",
                                       {ins(MOpcode::MovRI, {v(1), imm(7)}),
                                        ins(MOpcode::Bl, {label(sym)}),
                                        ins(MOpcode::MovRR, {x(PhysReg::X0), v(1)}),
                                        ins(MOpcode::Ret, {})})});
        LiveIntervals sli;
        sli.build(sj, target());
        EXPECT_EQ(sli.ehPushPositions().size(), 1u);
        EXPECT_TRUE(sli.find(1)->crossesEhPush);
    }
}

TEST(AArch64LiveIntervals, FprIntervalsUseTheirOwnClass) {
    MFunction fn = function({block(
        "entry",
        {ins(MOpcode::FMovRR, {v(1, RegClass::FPR), x(PhysReg::V0)}),
         ins(MOpcode::FAddRRR, {v(2, RegClass::FPR), v(1, RegClass::FPR), v(1, RegClass::FPR)}),
         ins(MOpcode::FMovRR, {x(PhysReg::V0), v(2, RegClass::FPR)}),
         ins(MOpcode::Ret, {})})});
    LiveIntervals li;
    li.build(fn, target());
    EXPECT_EQ(li.find(1)->cls, RegClass::FPR);
    EXPECT_EQ(ranges(li, 1), "[1,2]");
    EXPECT_EQ(ranges(li, 2), "[3,4]");
    EXPECT_EQ(li.fixed(PhysReg::V0).toString(), "[0,0] [5,6]");
    EXPECT_EQ(li.find(2)->hintPhys, PhysReg::V0);
}

TEST(AArch64LiveIntervals, BuildIsDeterministic) {
    MFunction fn = function({
        block("entry",
              {ins(MOpcode::MovRI, {v(1), imm(1)}),
               ins(MOpcode::MovRI, {v(2), imm(2)}),
               ins(MOpcode::Cbz, {v(1), label("right")})}),
        block("left",
              {ins(MOpcode::ParallelCopy, {v(20), v(1)}), ins(MOpcode::Br, {label("join")})}),
        block("right",
              {ins(MOpcode::ParallelCopy, {v(20), v(2)}), ins(MOpcode::Br, {label("join")})}),
        block("join", {ins(MOpcode::MovRR, {x(PhysReg::X0), v(20)}), ins(MOpcode::Ret, {})}),
    });
    LiveIntervals a;
    a.build(fn, target());
    LiveIntervals b;
    b.build(fn, target());
    EXPECT_EQ(a.dump(), b.dump());
    EXPECT_NE(a.dump().find("%v20:gpr"), std::string::npos);
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, &argv);
    return zanna_test::run_all_tests();
}
