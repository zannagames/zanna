//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/codegen/test_codegen_cfg_extract.cpp
// Purpose: Unit tests for shared MIR CFG extraction (CfgExtract.hpp), its
//          use by both backend liveness analyses, and the x86-64 MirCfg
//          snapshot built on it. The key regression covered:
//          a block containing several conditional branches before its final
//          unconditional jump (switch compare cascades) must contribute an
//          edge for EVERY conditional branch, not just the one nearest the
//          terminator — otherwise values used only in an early case block are
//          dropped from liveOut and the allocator never preserves them.
//
// Key invariants:
//   - Every JCC / BCond / Cbz / Cbnz target is a successor.
//   - RET / UD2 / no-return calls end a block with no successors.
//   - Blocks without unconditional terminators fall through to the next
//     layout block.
//
// Ownership/Lifetime:
//   - Standalone test binary.
//
// Links: src/codegen/common/ra/CfgExtract.hpp,
//        src/codegen/x86_64/ra/Liveness.cpp,
//        src/codegen/aarch64/ra/Liveness.cpp
//
//===----------------------------------------------------------------------===//

#include "tests/TestHarness.hpp"

#include "codegen/aarch64/MachineIR.hpp"
#include "codegen/aarch64/ra/Liveness.hpp"
#include "codegen/common/ra/CfgExtract.hpp"
#include "codegen/x86_64/MachineIR.hpp"
#include "codegen/x86_64/MirCfg.hpp"
#include "codegen/x86_64/ra/Liveness.hpp"

#include <algorithm>
#include <vector>

namespace {

bool contains(const std::vector<std::size_t> &haystack, std::size_t needle) {
    return std::find(haystack.begin(), haystack.end(), needle) != haystack.end();
}

} // namespace

// ---------------------------------------------------------------------------
// x86-64: switch-style compare cascade — every JCC contributes an edge.
// ---------------------------------------------------------------------------
TEST(CfgExtract, X64SwitchCascadeKeepsAllCaseSuccessors) {
    using namespace zanna::codegen::x64;

    MFunction fn{};
    fn.name = "cascade";

    // entry:
    //   v1 = 42                      (value used only in case "one")
    //   v2 = 7                       (value used only in case "two")
    //   CMP v3, $1 ; JCC one
    //   CMP v3, $2 ; JCC two
    //   JMP def
    MBasicBlock entry{};
    entry.label = "cascade_entry";
    entry.instructions = {
        MInstr::make(MOpcode::MOVri, {makeVRegOperand(RegClass::GPR, 1), makeImmOperand(42)}),
        MInstr::make(MOpcode::MOVri, {makeVRegOperand(RegClass::GPR, 2), makeImmOperand(7)}),
        MInstr::make(MOpcode::CMPri, {makeVRegOperand(RegClass::GPR, 3), makeImmOperand(1)}),
        MInstr::make(MOpcode::JCC, {makeImmOperand(0), makeLabelOperand("case_one")}),
        MInstr::make(MOpcode::CMPri, {makeVRegOperand(RegClass::GPR, 3), makeImmOperand(2)}),
        MInstr::make(MOpcode::JCC, {makeImmOperand(0), makeLabelOperand("case_two")}),
        MInstr::make(MOpcode::JMP, {makeLabelOperand("case_def")}),
    };

    MBasicBlock caseOne{};
    caseOne.label = "case_one";
    caseOne.instructions = {
        MInstr::make(MOpcode::MOVrr,
                     {makePhysRegOperand(RegClass::GPR, static_cast<uint16_t>(PhysReg::RAX)),
                      makeVRegOperand(RegClass::GPR, 1)}),
        MInstr::make(MOpcode::RET),
    };

    MBasicBlock caseTwo{};
    caseTwo.label = "case_two";
    caseTwo.instructions = {
        MInstr::make(MOpcode::MOVrr,
                     {makePhysRegOperand(RegClass::GPR, static_cast<uint16_t>(PhysReg::RAX)),
                      makeVRegOperand(RegClass::GPR, 2)}),
        MInstr::make(MOpcode::RET),
    };

    MBasicBlock caseDef{};
    caseDef.label = "case_def";
    caseDef.instructions = {MInstr::make(MOpcode::RET)};

    fn.blocks = {entry, caseOne, caseTwo, caseDef};

    ra::LivenessAnalysis liveness;
    liveness.run(fn);

    const auto &succs = liveness.successors(0);
    ASSERT_EQ(succs.size(), 3u);
    EXPECT_TRUE(contains(succs, 1));
    EXPECT_TRUE(contains(succs, 2));
    EXPECT_TRUE(contains(succs, 3));

    // The regression that motivated the rewrite: v1 is used only in the FIRST
    // case block. With the old nearest-JCC scan, edge entry->case_one was
    // dropped and v1 vanished from liveOut(entry).
    EXPECT_TRUE(liveness.liveOut(0).count(1) != 0);
    EXPECT_TRUE(liveness.liveOut(0).count(2) != 0);
}

// ---------------------------------------------------------------------------
// x86-64: JCC as final instruction falls through; RET/UD2 end the block.
// ---------------------------------------------------------------------------
TEST(CfgExtract, X64FallthroughAndNoSuccessorTerminators) {
    using namespace zanna::codegen::x64;

    MFunction fn{};
    fn.name = "fallthrough";

    MBasicBlock entry{};
    entry.label = "ft_entry";
    entry.instructions = {
        MInstr::make(MOpcode::TESTrr,
                     {makeVRegOperand(RegClass::GPR, 1), makeVRegOperand(RegClass::GPR, 1)}),
        MInstr::make(MOpcode::JCC, {makeImmOperand(0), makeLabelOperand("ft_target")}),
    };

    MBasicBlock next{};
    next.label = "ft_next";
    next.instructions = {MInstr::make(MOpcode::UD2)};

    MBasicBlock target{};
    target.label = "ft_target";
    target.instructions = {MInstr::make(MOpcode::RET)};

    fn.blocks = {entry, next, target};

    ra::LivenessAnalysis liveness;
    liveness.run(fn);

    const auto &entrySuccs = liveness.successors(0);
    ASSERT_EQ(entrySuccs.size(), 2u);
    EXPECT_TRUE(contains(entrySuccs, 1)); // fallthrough
    EXPECT_TRUE(contains(entrySuccs, 2)); // JCC target

    EXPECT_TRUE(liveness.successors(1).empty()); // UD2: no successors
    EXPECT_TRUE(liveness.successors(2).empty()); // RET: no successors
}

// ---------------------------------------------------------------------------
// AArch64: cascade of conditional branches keeps every successor.
// ---------------------------------------------------------------------------
TEST(CfgExtract, A64ConditionalCascadeKeepsAllSuccessors) {
    using namespace zanna::codegen::aarch64;

    MFunction fn{};
    fn.name = "a64_cascade";

    MBasicBlock entry{};
    entry.name = "entry";
    entry.instrs = {
        MInstr{MOpcode::CmpRI, {MOperand::vregOp(RegClass::GPR, 3), MOperand::immOp(1)}},
        MInstr{MOpcode::BCond, {MOperand::condOp("eq"), MOperand::labelOp("one")}},
        MInstr{MOpcode::CmpRI, {MOperand::vregOp(RegClass::GPR, 3), MOperand::immOp(2)}},
        MInstr{MOpcode::BCond, {MOperand::condOp("eq"), MOperand::labelOp("two")}},
        MInstr{MOpcode::Br, {MOperand::labelOp("def")}},
    };

    MBasicBlock one{};
    one.name = "one";
    one.instrs = {MInstr{MOpcode::Ret, {}}};

    MBasicBlock two{};
    two.name = "two";
    two.instrs = {MInstr{MOpcode::Ret, {}}};

    MBasicBlock def{};
    def.name = "def";
    def.instrs = {MInstr{MOpcode::Ret, {}}};

    fn.blocks = {entry, one, two, def};

    ra::LivenessAnalysis liveness;
    liveness.run(fn);

    const auto &succs = liveness.successors(0);
    ASSERT_EQ(succs.size(), 3u);
    EXPECT_TRUE(contains(succs, 1));
    EXPECT_TRUE(contains(succs, 2));
    EXPECT_TRUE(contains(succs, 3));
}

// ---------------------------------------------------------------------------
// AArch64: Cbz/Cbnz fallthrough plus dead code after Br is ignored.
// ---------------------------------------------------------------------------
TEST(CfgExtract, A64CbzFallthroughAndBrEndsScan) {
    using namespace zanna::codegen::aarch64;

    MFunction fn{};
    fn.name = "a64_cbz";

    MBasicBlock entry{};
    entry.name = "entry";
    entry.instrs = {
        MInstr{MOpcode::Cbz, {MOperand::vregOp(RegClass::GPR, 1), MOperand::labelOp("zero")}},
        MInstr{MOpcode::Br, {MOperand::labelOp("other")}},
        // Dead conditional after the Br must not contribute an edge.
        MInstr{MOpcode::BCond, {MOperand::condOp("ne"), MOperand::labelOp("zero")}},
    };

    MBasicBlock zero{};
    zero.name = "zero";
    zero.instrs = {MInstr{MOpcode::Ret, {}}};

    MBasicBlock other{};
    other.name = "other";
    other.instrs = {MInstr{MOpcode::Ret, {}}};

    fn.blocks = {entry, zero, other};

    ra::LivenessAnalysis liveness;
    liveness.run(fn);

    const auto &succs = liveness.successors(0);
    ASSERT_EQ(succs.size(), 2u);
    EXPECT_TRUE(contains(succs, 1));
    EXPECT_TRUE(contains(succs, 2));
}

// ---------------------------------------------------------------------------
// Shared extractor: unresolvable labels are skipped; sorting/dedup applied.
// ---------------------------------------------------------------------------
TEST(CfgExtract, SharedExtractorSkipsUnknownLabelsAndDedups) {
    using zanna::codegen::ra::BranchDesc;

    struct FakeInstr {
        BranchDesc::Kind kind{BranchDesc::Kind::None};
        std::string label{};
    };

    struct FakeBlock {
        std::vector<FakeInstr> instrs{};
    };

    std::vector<FakeBlock> blocks(2);
    blocks[0].instrs = {
        FakeInstr{BranchDesc::Kind::Cond, "external_symbol"}, // unknown: skipped
        FakeInstr{BranchDesc::Kind::Cond, "b"},
        FakeInstr{BranchDesc::Kind::Uncond, "b"}, // duplicate of the Cond edge
    };
    blocks[1].instrs = {FakeInstr{BranchDesc::Kind::Return, ""}};

    std::unordered_map<std::string, std::size_t> index{{"a", 0}, {"b", 1}};

    auto succs = zanna::codegen::ra::extractSuccessors(
        blocks,
        index,
        [](const FakeBlock &blk) -> const std::vector<FakeInstr> & { return blk.instrs; },
        [](const FakeInstr &ins) {
            return BranchDesc{ins.kind, ins.label.empty() ? nullptr : &ins.label};
        });

    ASSERT_EQ(succs[0].size(), 1u);
    EXPECT_EQ(succs[0][0], 1u);
    EXPECT_TRUE(succs[1].empty());
}

// ---------------------------------------------------------------------------
// x86-64 MirCfg: the shared snapshot agrees with the allocator's liveness CFG
// on the shapes the retired private builders disagreed on, and exposes
// fallthrough, dominators, back edges, and natural loops.
// ---------------------------------------------------------------------------
TEST(CfgExtract, X64MirCfgShapesMatchLivenessAndExposeFallthrough) {
    using namespace zanna::codegen::x64;

    MFunction fn{};
    fn.name = "mircfg_shapes";

    MBasicBlock entry{};
    entry.label = "entry";
    entry.instructions = {
        MInstr::make(MOpcode::JCC, {makeImmOperand(0), makeLabelOperand("far")}),
    };
    MBasicBlock midJmp{};
    midJmp.label = "mid_jmp";
    midJmp.instructions = {
        MInstr::make(MOpcode::JMP, {makeLabelOperand("far")}),
        MInstr::make(MOpcode::JCC, {makeImmOperand(0), makeLabelOperand("entry")}), // dead
    };
    MBasicBlock trap{};
    trap.label = "trap";
    trap.instructions = {MInstr::make(MOpcode::UD2)};
    MBasicBlock table{};
    table.label = "table";
    table.instructions = {
        MInstr::make(MOpcode::JUMPTABLE,
                     {makeVRegOperand(RegClass::GPR, 1),
                      makeLabelOperand(".Ljt"),
                      makeLabelOperand("far"),
                      makeLabelOperand("trap")}),
    };
    MBasicBlock far{};
    far.label = "far";
    far.instructions = {MInstr::make(MOpcode::RET)};

    fn.blocks = {entry, midJmp, trap, table, far};

    const MirCfg cfg(fn);
    ra::LivenessAnalysis liveness;
    liveness.run(fn);
    for (std::size_t bi = 0; bi < fn.blocks.size(); ++bi)
        EXPECT_TRUE(cfg.succs(bi) == liveness.successors(bi));

    EXPECT_TRUE(cfg.fallsThrough(0));  // trailing JCC
    EXPECT_FALSE(cfg.fallsThrough(1)); // JMP ends the block; dead JCC ignored
    EXPECT_FALSE(cfg.fallsThrough(2)); // UD2
    EXPECT_FALSE(cfg.fallsThrough(3)); // JUMPTABLE
    EXPECT_TRUE(cfg.anyFallthrough());

    EXPECT_TRUE(cfg.hasEdge(0, 1));
    EXPECT_TRUE(cfg.hasEdge(0, 4));
    EXPECT_TRUE(cfg.hasEdge(1, 4));
    EXPECT_FALSE(cfg.hasEdge(1, 0));
    EXPECT_TRUE(cfg.succs(2).empty());
    EXPECT_TRUE(cfg.hasEdge(3, 4));
    EXPECT_TRUE(cfg.hasEdge(3, 2));
    EXPECT_TRUE(cfg.preds(3).empty()); // unreachable
    ASSERT_EQ(cfg.preds(4).size(), 3u);
    ASSERT_TRUE(cfg.indexOf("far").has_value());
    EXPECT_EQ(*cfg.indexOf("far"), 4u);
    EXPECT_TRUE(cfg.backEdges().empty());
}

TEST(CfgExtract, X64MirCfgLoopsAreDominanceProven) {
    using namespace zanna::codegen::x64;

    MFunction fn{};
    fn.name = "mircfg_loop";

    MBasicBlock entry{};
    entry.label = "entry";
    entry.instructions = {MInstr::make(MOpcode::JMP, {makeLabelOperand("header")})};
    MBasicBlock header{};
    header.label = "header";
    header.instructions = {
        MInstr::make(MOpcode::JCC, {makeImmOperand(0), makeLabelOperand("exit")}),
    };
    MBasicBlock body{};
    body.label = "body";
    body.instructions = {MInstr::make(MOpcode::JMP, {makeLabelOperand("header")})};
    MBasicBlock exitBlock{};
    exitBlock.label = "exit";
    exitBlock.instructions = {MInstr::make(MOpcode::RET)};
    // A join placed before one of its predecessors: the backward JMP is not
    // a loop because the join does not dominate the late block.
    MBasicBlock late{};
    late.label = "late";
    late.instructions = {MInstr::make(MOpcode::JMP, {makeLabelOperand("exit")})};

    fn.blocks = {entry, header, body, exitBlock, late};

    const MirCfg cfg(fn);
    EXPECT_TRUE(cfg.dominates(1, 2));
    EXPECT_FALSE(cfg.dominates(3, 4));

    const auto edges = cfg.backEdges();
    ASSERT_EQ(edges.size(), 1u);
    EXPECT_EQ(edges[0].latch, 2u);
    EXPECT_EQ(edges[0].header, 1u);

    const NaturalLoop loop = cfg.naturalLoop(edges[0]);
    ASSERT_EQ(loop.blocks.size(), 2u);
    EXPECT_TRUE(loop.contains(1));
    EXPECT_TRUE(loop.contains(2));
    EXPECT_FALSE(loop.contains(3));

    const auto depth = cfg.loopDepths();
    ASSERT_EQ(depth.size(), 5u);
    EXPECT_EQ(depth[1], 1u);
    EXPECT_EQ(depth[2], 1u);
    EXPECT_EQ(depth[3], 0u);
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, argv);
    return zanna_test::run_all_tests();
}
