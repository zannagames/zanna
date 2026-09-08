//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/codegen/test_codegen_arm64_peephole.cpp
// Purpose: Verify AArch64 peephole optimizations work correctly.
//
// Tests:
// - Identity move removal (mov r, r)
// - Identity FPR move removal (fmov d, d)
// - Consecutive move folding
//
//===----------------------------------------------------------------------===//
#include "tests/TestHarness.hpp"
#include <algorithm>
#include <sstream>
#include <string>

#include "codegen/aarch64/AsmEmitter.hpp"
#include "codegen/aarch64/MachineIR.hpp"
#include "codegen/aarch64/Peephole.hpp"

using namespace zanna::codegen::aarch64;

/// @brief Test that identity GPR moves (mov r, r) are removed.
TEST(AArch64Peephole, RemoveIdentityMovRR) {
    MFunction fn{};
    fn.name = "test_identity_mov";
    fn.blocks.push_back(MBasicBlock{"entry", {}});
    auto &bb = fn.blocks.back();

    // mov x0, x1 (not identity)
    bb.instrs.push_back(
        MInstr{MOpcode::MovRR, {MOperand::regOp(PhysReg::X0), MOperand::regOp(PhysReg::X1)}});
    // mov x0, x0 (identity - should be removed)
    bb.instrs.push_back(
        MInstr{MOpcode::MovRR, {MOperand::regOp(PhysReg::X0), MOperand::regOp(PhysReg::X0)}});
    // mov x2, x2 (identity - should be removed)
    bb.instrs.push_back(
        MInstr{MOpcode::MovRR, {MOperand::regOp(PhysReg::X2), MOperand::regOp(PhysReg::X2)}});
    // ret
    bb.instrs.push_back(MInstr{MOpcode::Ret, {}});

    EXPECT_EQ(bb.instrs.size(), 4U);

    auto stats = runPeephole(fn);

    // Should have removed 2 identity moves
    EXPECT_EQ(stats.identityMovesRemoved, 2);
    // Remaining: mov x0, x1 and ret
    EXPECT_EQ(bb.instrs.size(), 2U);
    EXPECT_EQ(bb.instrs[0].opc, MOpcode::MovRR);
    EXPECT_EQ(bb.instrs[1].opc, MOpcode::Ret);
}

/// @brief Test that identity FPR moves (fmov d, d) are removed.
TEST(AArch64Peephole, RemoveIdentityFMovRR) {
    MFunction fn{};
    fn.name = "test_identity_fmov";
    fn.blocks.push_back(MBasicBlock{"entry", {}});
    auto &bb = fn.blocks.back();

    // fmov d0, d1 (not identity)
    bb.instrs.push_back(
        MInstr{MOpcode::FMovRR, {MOperand::regOp(PhysReg::V0), MOperand::regOp(PhysReg::V1)}});
    // fmov d0, d0 (identity - should be removed)
    bb.instrs.push_back(
        MInstr{MOpcode::FMovRR, {MOperand::regOp(PhysReg::V0), MOperand::regOp(PhysReg::V0)}});
    // ret
    bb.instrs.push_back(MInstr{MOpcode::Ret, {}});

    EXPECT_EQ(bb.instrs.size(), 3U);

    auto stats = runPeephole(fn);

    // Should have removed 1 identity FPR move
    EXPECT_EQ(stats.identityFMovesRemoved, 1);
    // Remaining: fmov d0, d1 and ret
    EXPECT_EQ(bb.instrs.size(), 2U);
    EXPECT_EQ(bb.instrs[0].opc, MOpcode::FMovRR);
    EXPECT_EQ(bb.instrs[1].opc, MOpcode::Ret);
}

/// @brief Test that consecutive moves are folded.
/// mov x1, x2; mov x3, x1 -> mov x3, x2 (if x1 is dead)
TEST(AArch64Peephole, FoldConsecutiveMoves) {
    MFunction fn{};
    fn.name = "test_fold_moves";
    fn.blocks.push_back(MBasicBlock{"entry", {}});
    auto &bb = fn.blocks.back();

    // mov x1, x2
    bb.instrs.push_back(
        MInstr{MOpcode::MovRR, {MOperand::regOp(PhysReg::X1), MOperand::regOp(PhysReg::X2)}});
    // mov x3, x1 (can be folded to mov x3, x2 since x1 is not used after)
    bb.instrs.push_back(
        MInstr{MOpcode::MovRR, {MOperand::regOp(PhysReg::X3), MOperand::regOp(PhysReg::X1)}});
    // ret
    bb.instrs.push_back(MInstr{MOpcode::Ret, {}});

    EXPECT_EQ(bb.instrs.size(), 3U);

    auto stats = runPeephole(fn);

    // Should have folded 1 consecutive move pair
    EXPECT_EQ(stats.consecutiveMovsFolded, 1);
    // First move becomes identity (x2, x2) and gets removed
    // Second move becomes mov x3, x2
    // Final: mov x3, x2 and ret
    EXPECT_EQ(bb.instrs.size(), 2U);
    EXPECT_EQ(bb.instrs[0].opc, MOpcode::MovRR);
    // Verify the folded move has x3 as dst and x2 as src
    const auto &foldedMov = bb.instrs[0];
    EXPECT_EQ(static_cast<PhysReg>(foldedMov.ops[0].reg.idOrPhys), PhysReg::X3);
    EXPECT_EQ(static_cast<PhysReg>(foldedMov.ops[1].reg.idOrPhys), PhysReg::X2);
}

/// @brief Test that consecutive moves are NOT folded when the intermediate
/// register is used later.
TEST(AArch64Peephole, NoFoldWhenIntermediateLive) {
    MFunction fn{};
    fn.name = "test_no_fold";
    fn.blocks.push_back(MBasicBlock{"entry", {}});
    auto &bb = fn.blocks.back();

    // mov x1, x2
    bb.instrs.push_back(
        MInstr{MOpcode::MovRR, {MOperand::regOp(PhysReg::X1), MOperand::regOp(PhysReg::X2)}});
    // mov x3, x1
    bb.instrs.push_back(
        MInstr{MOpcode::MovRR, {MOperand::regOp(PhysReg::X3), MOperand::regOp(PhysReg::X1)}});
    // add x4, x1, x5 (x1 is still used, so we can't fold the moves above)
    bb.instrs.push_back(MInstr{MOpcode::AddRRR,
                               {MOperand::regOp(PhysReg::X4),
                                MOperand::regOp(PhysReg::X1),
                                MOperand::regOp(PhysReg::X5)}});
    // ret
    bb.instrs.push_back(MInstr{MOpcode::Ret, {}});

    EXPECT_EQ(bb.instrs.size(), 4U);

    auto stats = runPeephole(fn);

    // Should NOT have folded because x1 is used later
    EXPECT_EQ(stats.consecutiveMovsFolded, 0);
    EXPECT_EQ(bb.instrs.size(), 4U);
}

/// @brief Test mixed identity moves and real operations.
TEST(AArch64Peephole, MixedOperations) {
    MFunction fn{};
    fn.name = "test_mixed";
    fn.blocks.push_back(MBasicBlock{"entry", {}});
    auto &bb = fn.blocks.back();

    // mov x0, x0 (identity)
    bb.instrs.push_back(
        MInstr{MOpcode::MovRR, {MOperand::regOp(PhysReg::X0), MOperand::regOp(PhysReg::X0)}});
    // add x1, x2, x3
    bb.instrs.push_back(MInstr{MOpcode::AddRRR,
                               {MOperand::regOp(PhysReg::X1),
                                MOperand::regOp(PhysReg::X2),
                                MOperand::regOp(PhysReg::X3)}});
    // mov x4, x4 (identity)
    bb.instrs.push_back(
        MInstr{MOpcode::MovRR, {MOperand::regOp(PhysReg::X4), MOperand::regOp(PhysReg::X4)}});
    // sub x5, x6, x7
    bb.instrs.push_back(MInstr{MOpcode::SubRRR,
                               {MOperand::regOp(PhysReg::X5),
                                MOperand::regOp(PhysReg::X6),
                                MOperand::regOp(PhysReg::X7)}});
    // ret
    bb.instrs.push_back(MInstr{MOpcode::Ret, {}});

    EXPECT_EQ(bb.instrs.size(), 5U);

    auto stats = runPeephole(fn);

    // Should have removed 2 identity moves
    EXPECT_EQ(stats.identityMovesRemoved, 2);
    // Remaining: add, sub, ret
    EXPECT_EQ(bb.instrs.size(), 3U);
    EXPECT_EQ(bb.instrs[0].opc, MOpcode::AddRRR);
    EXPECT_EQ(bb.instrs[1].opc, MOpcode::SubRRR);
    EXPECT_EQ(bb.instrs[2].opc, MOpcode::Ret);
}

TEST(AArch64Peephole, CrossBlockDeadFpStoreSkipsAddressableStackLocals) {
    MFunction fn{};
    fn.name = "test_dead_fp_store_locals";
    fn.frame.locals.push_back(MFunction::StackLocal{1, 8, 8, -24});
    fn.frame.spills.push_back(MFunction::SpillSlot{2, 8, 8, -40});
    fn.blocks.push_back(MBasicBlock{"entry", {}});

    auto &entry = fn.blocks[0];
    entry.instrs.push_back(
        MInstr{MOpcode::StrRegFpImm, {MOperand::regOp(PhysReg::X9), MOperand::immOp(-24)}});
    entry.instrs.push_back(
        MInstr{MOpcode::StrRegFpImm, {MOperand::regOp(PhysReg::X10), MOperand::immOp(-40)}});
    entry.instrs.push_back(MInstr{MOpcode::Ret, {}});

    (void)runPeephole(fn);

    const bool keptLocalStore =
        std::any_of(entry.instrs.begin(), entry.instrs.end(), [](const MInstr &instr) {
            return instr.opc == MOpcode::StrRegFpImm && instr.ops.size() >= 2 &&
                   instr.ops[1].kind == MOperand::Kind::Imm && instr.ops[1].imm == -24;
        });
    const bool removedSpillStore =
        std::none_of(entry.instrs.begin(), entry.instrs.end(), [](const MInstr &instr) {
            return instr.opc == MOpcode::StrRegFpImm && instr.ops.size() >= 2 &&
                   instr.ops[1].kind == MOperand::Kind::Imm && instr.ops[1].imm == -40;
        });

    EXPECT_TRUE(keptLocalStore);
    EXPECT_TRUE(removedSpillStore);
}

/// @brief Test that peephole produces correct assembly output.
TEST(AArch64Peephole, EmittedAssemblyNoIdentityMoves) {
    auto &ti = darwinTarget();
    AsmEmitter emit{ti};

    MFunction fn{};
    fn.name = "test_emit";
    fn.blocks.push_back(MBasicBlock{"entry", {}});
    auto &bb = fn.blocks.back();

    // mov x0, x1 (real move)
    bb.instrs.push_back(
        MInstr{MOpcode::MovRR, {MOperand::regOp(PhysReg::X0), MOperand::regOp(PhysReg::X1)}});
    // mov x0, x0 (identity - should be removed)
    bb.instrs.push_back(
        MInstr{MOpcode::MovRR, {MOperand::regOp(PhysReg::X0), MOperand::regOp(PhysReg::X0)}});
    // add x2, x0, x3
    bb.instrs.push_back(MInstr{MOpcode::AddRRR,
                               {MOperand::regOp(PhysReg::X2),
                                MOperand::regOp(PhysReg::X0),
                                MOperand::regOp(PhysReg::X3)}});
    // ret
    bb.instrs.push_back(MInstr{MOpcode::Ret, {}});

    // Run peephole
    [[maybe_unused]] auto stats = runPeephole(fn);

    // Emit assembly
    std::ostringstream os;
    emit.emitFunction(os, fn);
    const std::string asmText = os.str();

    // Count occurrences of "mov x0"
    std::size_t movCount = 0;
    std::size_t pos = 0;
    while ((pos = asmText.find("mov x0", pos)) != std::string::npos) {
        ++movCount;
        pos += 6;
    }

    // Should only have one "mov x0" (the mov x0, x1), not two
    EXPECT_EQ(movCount, 1U);
    // Should have the add instruction
    EXPECT_NE(asmText.find("add x2, x0, x3"), std::string::npos);
}

/// @brief Test statistics are accurate.
TEST(AArch64Peephole, StatsAccuracy) {
    MFunction fn{};
    fn.name = "test_stats";
    fn.blocks.push_back(MBasicBlock{"entry", {}});
    auto &bb = fn.blocks.back();

    // 3 identity GPR moves
    bb.instrs.push_back(
        MInstr{MOpcode::MovRR, {MOperand::regOp(PhysReg::X0), MOperand::regOp(PhysReg::X0)}});
    bb.instrs.push_back(
        MInstr{MOpcode::MovRR, {MOperand::regOp(PhysReg::X1), MOperand::regOp(PhysReg::X1)}});
    bb.instrs.push_back(
        MInstr{MOpcode::MovRR, {MOperand::regOp(PhysReg::X2), MOperand::regOp(PhysReg::X2)}});
    // 2 identity FPR moves
    bb.instrs.push_back(
        MInstr{MOpcode::FMovRR, {MOperand::regOp(PhysReg::V0), MOperand::regOp(PhysReg::V0)}});
    bb.instrs.push_back(
        MInstr{MOpcode::FMovRR, {MOperand::regOp(PhysReg::V1), MOperand::regOp(PhysReg::V1)}});
    // ret
    bb.instrs.push_back(MInstr{MOpcode::Ret, {}});

    auto stats = runPeephole(fn);

    EXPECT_EQ(stats.identityMovesRemoved, 3);
    EXPECT_EQ(stats.identityFMovesRemoved, 2);
    EXPECT_EQ(stats.total(), 5);
    // Only ret should remain
    EXPECT_EQ(bb.instrs.size(), 1U);
}

/// @brief Test that cmp reg, #0 is converted to tst reg, reg.
TEST(AArch64Peephole, CmpZeroToTst) {
    MFunction fn{};
    fn.name = "test_cmp_zero";
    fn.blocks.push_back(MBasicBlock{"entry", {}});
    fn.blocks.push_back(MBasicBlock{"target", {}});
    fn.blocks.back().instrs.push_back(MInstr{MOpcode::Ret, {}});
    auto &bb = fn.blocks.front();

    // cmp x0, #0 (should become tst x0, x0)
    bb.instrs.push_back(MInstr{MOpcode::CmpRI, {MOperand::regOp(PhysReg::X0), MOperand::immOp(0)}});
    // b.gt target  — reads flags (use gt, not eq/ne, to avoid cbz fusion)
    bb.instrs.push_back(
        MInstr{MOpcode::BCond, {MOperand::condOp("gt"), MOperand::labelOp("target")}});
    // cmp x1, #5 (should NOT be changed - not zero)
    bb.instrs.push_back(MInstr{MOpcode::CmpRI, {MOperand::regOp(PhysReg::X1), MOperand::immOp(5)}});
    // b.lt target  — reads flags
    bb.instrs.push_back(
        MInstr{MOpcode::BCond, {MOperand::condOp("lt"), MOperand::labelOp("target")}});
    // cmp x2, #0 (should become tst x2, x2)
    bb.instrs.push_back(MInstr{MOpcode::CmpRI, {MOperand::regOp(PhysReg::X2), MOperand::immOp(0)}});
    // ret
    bb.instrs.push_back(MInstr{MOpcode::Ret, {}});

    EXPECT_EQ(bb.instrs.size(), 6U);

    auto stats = runPeephole(fn);

    // Should have converted 2 cmp #0 to tst
    EXPECT_EQ(stats.cmpZeroToTst, 2);
    EXPECT_EQ(bb.instrs.size(), 6U);

    // First instruction should now be TstRR
    EXPECT_EQ(bb.instrs[0].opc, MOpcode::TstRR);
    EXPECT_EQ(static_cast<PhysReg>(bb.instrs[0].ops[0].reg.idOrPhys), PhysReg::X0);
    EXPECT_EQ(static_cast<PhysReg>(bb.instrs[0].ops[1].reg.idOrPhys), PhysReg::X0);

    // Third instruction should still be CmpRI (not zero)
    EXPECT_EQ(bb.instrs[2].opc, MOpcode::CmpRI);

    // Fifth instruction should now be TstRR
    EXPECT_EQ(bb.instrs[4].opc, MOpcode::TstRR);
    EXPECT_EQ(static_cast<PhysReg>(bb.instrs[4].ops[0].reg.idOrPhys), PhysReg::X2);
    EXPECT_EQ(static_cast<PhysReg>(bb.instrs[4].ops[1].reg.idOrPhys), PhysReg::X2);
}

/// @brief Test that add/sub with #0 are converted to mov.
TEST(AArch64Peephole, ArithmeticIdentityAddSub) {
    MFunction fn{};
    fn.name = "test_arith_identity";
    fn.blocks.push_back(MBasicBlock{"entry", {}});
    auto &bb = fn.blocks.back();

    // add x0, x1, #0 (should become mov x0, x1)
    bb.instrs.push_back(
        MInstr{MOpcode::AddRI,
               {MOperand::regOp(PhysReg::X0), MOperand::regOp(PhysReg::X1), MOperand::immOp(0)}});
    // sub x2, x3, #0 (should become mov x2, x3)
    bb.instrs.push_back(
        MInstr{MOpcode::SubRI,
               {MOperand::regOp(PhysReg::X2), MOperand::regOp(PhysReg::X3), MOperand::immOp(0)}});
    // add x4, x5, #10 (should NOT be changed - not zero)
    bb.instrs.push_back(
        MInstr{MOpcode::AddRI,
               {MOperand::regOp(PhysReg::X4), MOperand::regOp(PhysReg::X5), MOperand::immOp(10)}});
    // ret
    bb.instrs.push_back(MInstr{MOpcode::Ret, {}});

    EXPECT_EQ(bb.instrs.size(), 4U);

    auto stats = runPeephole(fn);

    // Should have converted 2 arithmetic identities
    EXPECT_EQ(stats.arithmeticIdentities, 2);
    EXPECT_EQ(bb.instrs.size(), 4U);

    // First instruction should now be MovRR
    EXPECT_EQ(bb.instrs[0].opc, MOpcode::MovRR);
    EXPECT_EQ(bb.instrs[0].ops.size(), 2U);
    EXPECT_EQ(static_cast<PhysReg>(bb.instrs[0].ops[0].reg.idOrPhys), PhysReg::X0);
    EXPECT_EQ(static_cast<PhysReg>(bb.instrs[0].ops[1].reg.idOrPhys), PhysReg::X1);

    // Second instruction should now be MovRR
    EXPECT_EQ(bb.instrs[1].opc, MOpcode::MovRR);

    // Third instruction should still be AddRI (not zero)
    EXPECT_EQ(bb.instrs[2].opc, MOpcode::AddRI);
}

/// @brief Test that shift by #0 is converted to mov.
TEST(AArch64Peephole, ArithmeticIdentityShift) {
    MFunction fn{};
    fn.name = "test_shift_identity";
    fn.blocks.push_back(MBasicBlock{"entry", {}});
    auto &bb = fn.blocks.back();

    // lsl x0, x1, #0 (should become mov x0, x1)
    bb.instrs.push_back(
        MInstr{MOpcode::LslRI,
               {MOperand::regOp(PhysReg::X0), MOperand::regOp(PhysReg::X1), MOperand::immOp(0)}});
    // lsr x2, x3, #0 (should become mov x2, x3)
    bb.instrs.push_back(
        MInstr{MOpcode::LsrRI,
               {MOperand::regOp(PhysReg::X2), MOperand::regOp(PhysReg::X3), MOperand::immOp(0)}});
    // asr x4, x5, #0 (should become mov x4, x5)
    bb.instrs.push_back(
        MInstr{MOpcode::AsrRI,
               {MOperand::regOp(PhysReg::X4), MOperand::regOp(PhysReg::X5), MOperand::immOp(0)}});
    // lsl x6, x7, #2 (should NOT be changed - not zero)
    bb.instrs.push_back(
        MInstr{MOpcode::LslRI,
               {MOperand::regOp(PhysReg::X6), MOperand::regOp(PhysReg::X7), MOperand::immOp(2)}});
    // ret
    bb.instrs.push_back(MInstr{MOpcode::Ret, {}});

    EXPECT_EQ(bb.instrs.size(), 5U);

    auto stats = runPeephole(fn);

    // Should have converted 3 shift identities
    EXPECT_EQ(stats.arithmeticIdentities, 3);
    EXPECT_EQ(bb.instrs.size(), 5U);

    // First three instructions should now be MovRR
    EXPECT_EQ(bb.instrs[0].opc, MOpcode::MovRR);
    EXPECT_EQ(bb.instrs[1].opc, MOpcode::MovRR);
    EXPECT_EQ(bb.instrs[2].opc, MOpcode::MovRR);

    // Fourth instruction should still be LslRI (not zero)
    EXPECT_EQ(bb.instrs[3].opc, MOpcode::LslRI);
}

/// @brief Test that tst instruction emits correct assembly.
TEST(AArch64Peephole, TstEmitsCorrectly) {
    auto &ti = darwinTarget();
    AsmEmitter emit{ti};

    MFunction fn{};
    fn.name = "test_tst_emit";
    fn.blocks.push_back(MBasicBlock{"entry", {}});
    auto &bb = fn.blocks.back();

    // cmp x0, #0 (will become tst x0, x0)
    bb.instrs.push_back(MInstr{MOpcode::CmpRI, {MOperand::regOp(PhysReg::X0), MOperand::immOp(0)}});
    // cset x1, eq
    bb.instrs.push_back(
        MInstr{MOpcode::Cset, {MOperand::regOp(PhysReg::X1), MOperand::condOp("eq")}});
    // ret
    bb.instrs.push_back(MInstr{MOpcode::Ret, {}});

    // Run peephole
    auto stats = runPeephole(fn);
    EXPECT_EQ(stats.cmpZeroToTst, 1);

    // Emit assembly
    std::ostringstream os;
    emit.emitFunction(os, fn);
    const std::string asmText = os.str();

    // Should have "tst x0, x0" instead of "cmp x0, #0"
    EXPECT_NE(asmText.find("tst x0, x0"), std::string::npos);
    EXPECT_EQ(asmText.find("cmp x0, #0"), std::string::npos);
}

/// @brief Test that branches to the next block are removed.
TEST(AArch64Peephole, RemoveBranchToNextBlock) {
    MFunction fn{};
    fn.name = "test_br_next";

    // Block 1: entry -> branches to block2 (should be removed)
    fn.blocks.push_back(MBasicBlock{"entry", {}});
    fn.blocks[0].instrs.push_back(
        MInstr{MOpcode::MovRI, {MOperand::regOp(PhysReg::X0), MOperand::immOp(42)}});
    fn.blocks[0].instrs.push_back(MInstr{MOpcode::Br, {MOperand::labelOp("block2")}});

    // Block 2: block2 -> branches to block3 (should be removed)
    fn.blocks.push_back(MBasicBlock{"block2", {}});
    fn.blocks[1].instrs.push_back(
        MInstr{MOpcode::AddRI,
               {MOperand::regOp(PhysReg::X1), MOperand::regOp(PhysReg::X0), MOperand::immOp(1)}});
    fn.blocks[1].instrs.push_back(MInstr{MOpcode::Br, {MOperand::labelOp("block3")}});

    // Block 3: block3 -> branches to exit (NOT next, should NOT be removed)
    fn.blocks.push_back(MBasicBlock{"block3", {}});
    fn.blocks[2].instrs.push_back(MInstr{MOpcode::Br, {MOperand::labelOp("exit")}});

    // Block 4: different_block
    fn.blocks.push_back(MBasicBlock{"different_block", {}});
    fn.blocks[3].instrs.push_back(MInstr{MOpcode::Ret, {}});

    // Block 5: exit
    fn.blocks.push_back(MBasicBlock{"exit", {}});
    fn.blocks[4].instrs.push_back(MInstr{MOpcode::Ret, {}});

    auto stats = runPeephole(fn);

    // Should have removed 2 branches (entry->block2, block2->block3)
    EXPECT_EQ(stats.branchesToNextRemoved, 2);

    // entry should now have just MovRI (branch removed)
    EXPECT_EQ(fn.blocks[0].instrs.size(), 1U);
    EXPECT_EQ(fn.blocks[0].instrs[0].opc, MOpcode::MovRI);

    // block2 should now have just AddRI (branch removed)
    EXPECT_EQ(fn.blocks[1].instrs.size(), 1U);
    EXPECT_EQ(fn.blocks[1].instrs[0].opc, MOpcode::AddRI);

    // block3 should still have the branch (not to next block)
    EXPECT_EQ(fn.blocks[2].instrs.size(), 1U);
    EXPECT_EQ(fn.blocks[2].instrs[0].opc, MOpcode::Br);
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, argv);
    return zanna_test::run_all_tests();
}
