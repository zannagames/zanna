//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/codegen/test_aarch64_mir_cfg.cpp
// Purpose: Unit tests for the shared AArch64 MIR control-flow graph (MirCfg)
//          and the post-RA block-exit liveness seed (blockExitLive). Covers
//          the four shapes on which the retired ad-hoc CFG builders
//          disagreed: a mid-block Br, a no-return call, a JumpTable, and a
//          trailing conditional branch (BCond / Tbz) that falls through.
// Key invariants:
//   - MirCfg edges match the register allocator's liveness CFG exactly.
//   - Back edges are dominance-proven; layout-created backward branches to
//     join blocks are not loops.
//   - blockExitLive is carried ∪ callee-saved ∪ SP/FP/LR, plus the return
//     registers only when the block leaves the function.
// Ownership/Lifetime: Standalone test binary.
// Links: src/codegen/aarch64/MirCfg.hpp, src/codegen/aarch64/ra/Liveness.hpp
//
//===----------------------------------------------------------------------===//

#include "tests/TestHarness.hpp"

#include "codegen/aarch64/MirCfg.hpp"
#include "codegen/aarch64/TargetAArch64.hpp"
#include "codegen/aarch64/ra/Liveness.hpp"

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

std::vector<std::size_t> vec(std::initializer_list<std::size_t> v) {
    return std::vector<std::size_t>(v);
}

/// The allocator's liveness CFG for @p fn, block by block.
std::vector<std::vector<std::size_t>> raSuccessors(const MFunction &fn) {
    ra::LivenessAnalysis liveness;
    liveness.run(fn);
    std::vector<std::vector<std::size_t>> succs;
    for (std::size_t bi = 0; bi < liveness.numBlocks(); ++bi)
        succs.push_back(liveness.successors(bi));
    return succs;
}

} // namespace

// ---------------------------------------------------------------------------
// Edge shapes
// ---------------------------------------------------------------------------

TEST(AArch64MirCfg, MidBlockBrEndsTheBlock) {
    // Dead instructions after a Br contribute no edge and no fallthrough.
    MFunction fn = function({
        block("entry", {br("b"), bcond("c"), br("c")}),
        block("a", {ret()}),
        block("b", {ret()}),
        block("c", {ret()}),
    });
    const MirCfg cfg(fn);
    EXPECT_EQ(cfg.succs(0), vec({2}));
    EXPECT_FALSE(cfg.fallsThrough(0));
    EXPECT_EQ(cfg.preds(1), vec({}));
    EXPECT_EQ(cfg.successors(), raSuccessors(fn));
}

TEST(AArch64MirCfg, NoReturnCallHasNoSuccessors) {
    MFunction fn = function({
        block("entry", {bcond("ok"), ins(MOpcode::Bl, {label("rt_trap_ovf")})}),
        block("after", {ret()}),
        block("ok", {ret()}),
    });
    const MirCfg cfg(fn);
    EXPECT_EQ(cfg.succs(0), vec({2}));
    EXPECT_FALSE(cfg.fallsThrough(0));
    EXPECT_EQ(cfg.preds(1), vec({}));
    EXPECT_EQ(cfg.successors(), raSuccessors(fn));

    // An ordinary call keeps the fallthrough.
    fn.blocks[0].instrs.back() = ins(MOpcode::Bl, {label("callee")});
    const MirCfg cfg2(fn);
    EXPECT_EQ(cfg2.succs(0), vec({1, 2}));
    EXPECT_TRUE(cfg2.fallsThrough(0));
    EXPECT_EQ(cfg2.successors(), raSuccessors(fn));
}

TEST(AArch64MirCfg, JumpTableHasCaseSuccessorsAndNoFallthrough) {
    MFunction fn = function({
        block("entry",
              {ins(MOpcode::JumpTable,
                   {x(PhysReg::X0), label(".Ljt"), label("c1"), label("c0"), label("c1")})}),
        block("next", {ret()}),
        block("c0", {ret()}),
        block("c1", {ret()}),
    });
    const MirCfg cfg(fn);
    EXPECT_EQ(cfg.succs(0), vec({2, 3}));
    EXPECT_FALSE(cfg.fallsThrough(0));
    EXPECT_EQ(cfg.preds(1), vec({}));
    EXPECT_EQ(cfg.preds(3), vec({0}));
    EXPECT_EQ(cfg.successors(), raSuccessors(fn));
}

TEST(AArch64MirCfg, TrailingConditionalBranchFallsThrough) {
    MFunction fn = function({
        block("entry", {bcond("far")}),
        block("mid", {ins(MOpcode::Tbz, {x(PhysReg::X1), label("far")})}),
        block("near", {ins(MOpcode::Cbnz, {x(PhysReg::X2), label("far")})}),
        block("far", {ret()}),
    });
    const MirCfg cfg(fn);
    EXPECT_EQ(cfg.succs(0), vec({1, 3}));
    EXPECT_EQ(cfg.succs(1), vec({2, 3}));
    EXPECT_EQ(cfg.succs(2), vec({3}));
    EXPECT_TRUE(cfg.fallsThrough(0));
    EXPECT_TRUE(cfg.fallsThrough(1));
    EXPECT_TRUE(cfg.fallsThrough(2));
    EXPECT_FALSE(cfg.fallsThrough(3));
    EXPECT_EQ(cfg.preds(3), vec({0, 1, 2}));
    EXPECT_EQ(cfg.successors(), raSuccessors(fn));
}

TEST(AArch64MirCfg, EmptyBlockFallsThroughAndLabelsResolve) {
    MFunction fn = function({
        block("entry", {}),
        block("exit", {ret()}),
    });
    const MirCfg cfg(fn);
    EXPECT_EQ(cfg.succs(0), vec({1}));
    EXPECT_TRUE(cfg.fallsThrough(0));
    EXPECT_TRUE(cfg.hasEdge(0, 1));
    EXPECT_FALSE(cfg.hasEdge(1, 0));
    ASSERT_TRUE(cfg.indexOf("exit").has_value());
    EXPECT_EQ(*cfg.indexOf("exit"), 1u);
    EXPECT_FALSE(cfg.indexOf("missing").has_value());
}

// ---------------------------------------------------------------------------
// Direct exit edges (join-copy insertion points)
// ---------------------------------------------------------------------------

TEST(AArch64MirCfg, ExitsDirectlyToRequiresBrOrFallthrough) {
    MFunction fn = function({
        block("viaBr", {br("join")}),
        block("viaFall", {bcond("elsewhere")}),
        block("join", {ret()}),
        block("viaCond", {bcond("join"), br("elsewhere")}),
        block("viaTable",
              {ins(MOpcode::JumpTable, {x(PhysReg::X0), label(".Ljt"), label("join")})}),
        block("viaTrap", {ins(MOpcode::Bl, {label("rt_trap")})}),
        block("elsewhere", {ret()}),
    });
    const MirCfg cfg(fn);
    EXPECT_TRUE(cfg.exitsDirectlyTo(fn, 0, 2));  // br join
    EXPECT_TRUE(cfg.exitsDirectlyTo(fn, 1, 2));  // trailing b.eq + fallthrough into join
    EXPECT_FALSE(cfg.exitsDirectlyTo(fn, 3, 2)); // conditional edge only
    EXPECT_FALSE(cfg.exitsDirectlyTo(fn, 4, 2)); // jump-table edge
    EXPECT_FALSE(cfg.exitsDirectlyTo(fn, 5, 6)); // trap never reaches the next block
    EXPECT_FALSE(cfg.exitsDirectlyTo(fn, 0, 1)); // no edge at all
}

// ---------------------------------------------------------------------------
// Dominators, back edges, natural loops, loop depth
// ---------------------------------------------------------------------------

TEST(AArch64MirCfg, LoopBackEdgeAndNaturalLoop) {
    MFunction fn = function({
        block("entry", {br("header")}),
        block("header", {bcond("exit")}),
        block("body", {bcond("skip")}),
        block("latch", {br("header")}),
        block("skip", {br("header")}),
        block("exit", {ret()}),
    });
    const MirCfg cfg(fn);
    EXPECT_TRUE(cfg.dominates(1, 3));
    EXPECT_TRUE(cfg.dominates(1, 4));
    EXPECT_FALSE(cfg.dominates(3, 4));

    const auto edges = cfg.backEdges();
    ASSERT_EQ(edges.size(), 2u);
    EXPECT_EQ(edges[0].latch, 3u);
    EXPECT_EQ(edges[0].header, 1u);
    EXPECT_EQ(edges[1].latch, 4u);
    EXPECT_EQ(edges[1].header, 1u);

    const NaturalLoop loop = cfg.naturalLoop(edges[0]);
    EXPECT_EQ(loop.blocks, vec({1, 2, 3}));
    EXPECT_TRUE(loop.contains(2));
    EXPECT_FALSE(loop.contains(4));
    EXPECT_FALSE(loop.contains(5));

    // The allocator's depth counter (ra::computeLoopDepths) counts one
    // natural loop per back edge: both latches reach the header and body,
    // so those sit in two loops; each latch sits in its own only.
    const auto depth = cfg.loopDepths();
    ASSERT_EQ(depth.size(), 6u);
    EXPECT_EQ(depth[0], 0u);
    EXPECT_EQ(depth[1], 2u);
    EXPECT_EQ(depth[2], 2u);
    EXPECT_EQ(depth[3], 1u);
    EXPECT_EQ(depth[4], 1u);
    EXPECT_EQ(depth[5], 0u);
}

TEST(AArch64MirCfg, BackwardBranchToJoinIsNotABackEdge) {
    // Layout places the join before one of its predecessors; the join does
    // not dominate that predecessor, so the backward branch is not a loop.
    MFunction fn = function({
        block("entry", {bcond("late")}),
        block("early", {br("join")}),
        block("join", {ret()}),
        block("late", {br("join")}),
    });
    const MirCfg cfg(fn);
    EXPECT_FALSE(cfg.dominates(2, 3));
    EXPECT_TRUE(cfg.backEdges().empty());
}

TEST(AArch64MirCfg, SelfLoopIsItsOwnNaturalLoop) {
    MFunction fn = function({
        block("entry", {br("spin")}),
        block("spin", {ins(MOpcode::Cbnz, {x(PhysReg::X0), label("spin")})}),
        block("exit", {ret()}),
    });
    const MirCfg cfg(fn);
    const auto edges = cfg.backEdges();
    ASSERT_EQ(edges.size(), 1u);
    EXPECT_EQ(edges[0].latch, 1u);
    EXPECT_EQ(edges[0].header, 1u);
    EXPECT_EQ(cfg.naturalLoop(edges[0]).blocks, vec({1}));
}

// ---------------------------------------------------------------------------
// blockExitLive
// ---------------------------------------------------------------------------

TEST(AArch64MirCfg, BlockExitLiveForReturningBlock) {
    MFunction fn = function({block("entry", {ret()})});
    fn.blocks[0].carriedExitRegs = {static_cast<uint16_t>(PhysReg::X5)};
    const PhysRegSet live = blockExitLive(fn, 0, darwinTarget());
    EXPECT_TRUE(live.contains(PhysReg::X0));
    EXPECT_TRUE(live.contains(PhysReg::V0));
    EXPECT_TRUE(live.contains(PhysReg::X5));
    EXPECT_TRUE(live.contains(PhysReg::SP));
    EXPECT_TRUE(live.contains(PhysReg::X29));
    EXPECT_TRUE(live.contains(PhysReg::X30));
    // The epilogue restores callee-saved registers from their slots: a value
    // left in one at a return is dead.
    EXPECT_FALSE(live.contains(PhysReg::X19));
    EXPECT_FALSE(live.contains(PhysReg::X28));
    EXPECT_FALSE(live.contains(PhysReg::V8));
    EXPECT_FALSE(live.contains(PhysReg::X1));
    EXPECT_FALSE(live.contains(PhysReg::X9));
    EXPECT_FALSE(live.contains(PhysReg::V1));
}

TEST(AArch64MirCfg, BlockExitLiveForBranchingBlockOmitsReturnRegs) {
    MFunction fn = function({
        block("entry", {bcond("exit"), br("exit")}),
        block("exit", {ret()}),
    });
    fn.blocks[0].carriedExitRegs = {static_cast<uint16_t>(PhysReg::X2),
                                    static_cast<uint16_t>(PhysReg::V3)};
    const PhysRegSet live = blockExitLive(fn, 0, darwinTarget());
    EXPECT_FALSE(live.contains(PhysReg::X0));
    EXPECT_FALSE(live.contains(PhysReg::V0));
    EXPECT_TRUE(live.contains(PhysReg::X2));
    EXPECT_TRUE(live.contains(PhysReg::V3));
    // Inside the function the callee-saved registers may hold pinned slots
    // or allocator-carried values.
    EXPECT_TRUE(live.contains(PhysReg::X20));
    EXPECT_TRUE(live.contains(PhysReg::V8));
}

TEST(AArch64MirCfg, BlockExitLiveForTrapBlockOmitsReturnRegs) {
    MFunction fn = function({
        block("trap", {ins(MOpcode::Bl, {label("rt_trap_div0")})}),
        block("exit", {ret()}),
    });
    const PhysRegSet live = blockExitLive(fn, 0, darwinTarget());
    EXPECT_FALSE(live.contains(PhysReg::X0));
    EXPECT_TRUE(live.contains(PhysReg::X20)); // conservative: not a return
}

TEST(AArch64MirCfg, BlockExitLiveForFallthroughBlocks) {
    MFunction fn = function({
        block("entry", {bcond("mid")}),
        block("mid", {}),
        block("tail", {ins(MOpcode::MovRR, {x(PhysReg::X0), x(PhysReg::X1)})}),
    });
    // Falls through into another block: not a function exit.
    EXPECT_FALSE(blockExitLive(fn, 0, darwinTarget()).contains(PhysReg::X0));
    EXPECT_FALSE(blockExitLive(fn, 1, darwinTarget()).contains(PhysReg::X0));
    // Falls off the end of the function: conservatively a return.
    EXPECT_TRUE(blockExitLive(fn, 2, darwinTarget()).contains(PhysReg::X0));
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, &argv);
    return zanna_test::run_all_tests();
}
