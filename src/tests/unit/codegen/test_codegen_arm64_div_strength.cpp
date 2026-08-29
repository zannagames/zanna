//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/codegen/test_codegen_arm64_div_strength.cpp
// Purpose: Verify division strength reduction in ARM64 peephole:
//          unsigned div by power-of-2 -> logical shift right.
//
//===----------------------------------------------------------------------===//
#include "tests/TestHarness.hpp"
#include <sstream>
#include <string>

#include "codegen/aarch64/MachineIR.hpp"
#include "codegen/aarch64/Peephole.hpp"
#include "codegen/aarch64/peephole/PeepholeCommon.hpp"

using namespace zanna::codegen::aarch64;
using namespace zanna::codegen::aarch64::peephole;

/// Unsigned division by 8 (power of 2) should become lsr by 3.
TEST(AArch64DivStrength, UDivByPowerOf2BecomesLsr) {
    MFunction fn{};
    fn.name = "test_udiv_pow2";
    fn.blocks.push_back(MBasicBlock{"entry", {}, {}});
    auto &bb = fn.blocks.back();

    // mov x1, #8 (load constant divisor)
    bb.instrs.push_back(MInstr{MOpcode::MovRI, {MOperand::regOp(PhysReg::X1), MOperand::immOp(8)}});
    // udiv x0, x2, x1 (unsigned divide by 8)
    bb.instrs.push_back(MInstr{MOpcode::UDivRRR,
                               {MOperand::regOp(PhysReg::X0),
                                MOperand::regOp(PhysReg::X2),
                                MOperand::regOp(PhysReg::X1)}});
    // ret
    bb.instrs.push_back(MInstr{MOpcode::Ret, {}});

    auto stats = runPeephole(fn);

    // UDivRRR should have been rewritten to LsrRI
    EXPECT_TRUE(stats.strengthReductions >= 1);
    EXPECT_EQ(bb.instrs[1].opc, MOpcode::LsrRI);
    EXPECT_EQ(bb.instrs[1].ops[2].imm, 3); // log2(8) = 3
}

/// Unsigned division by 1 (2^0) should become lsr by 0 (identity).
TEST(AArch64DivStrength, UDivBy1BecomesLsr0) {
    MFunction fn{};
    fn.name = "test_udiv_by_1";
    fn.blocks.push_back(MBasicBlock{"entry", {}, {}});
    auto &bb = fn.blocks.back();

    bb.instrs.push_back(MInstr{MOpcode::MovRI, {MOperand::regOp(PhysReg::X1), MOperand::immOp(1)}});
    bb.instrs.push_back(MInstr{MOpcode::UDivRRR,
                               {MOperand::regOp(PhysReg::X0),
                                MOperand::regOp(PhysReg::X2),
                                MOperand::regOp(PhysReg::X1)}});
    bb.instrs.push_back(MInstr{MOpcode::Ret, {}});

    auto stats = runPeephole(fn);

    EXPECT_TRUE(stats.strengthReductions >= 1);
    EXPECT_EQ(bb.instrs[1].opc, MOpcode::LsrRI);
    EXPECT_EQ(bb.instrs[1].ops[2].imm, 0); // log2(1) = 0
}

/// Signed division by power-of-2 should be reduced to sign-corrected shift.
/// For x / 4 (k=2): asr tmp, x, #63; lsr tmp, tmp, #62; add tmp, x, tmp; asr dst, tmp, #2
TEST(AArch64DivStrength, SDivByPowerOf2BecomesShift) {
    MFunction fn{};
    fn.name = "test_sdiv_pow2";
    fn.blocks.push_back(MBasicBlock{"entry", {}, {}});
    auto &bb = fn.blocks.back();

    bb.instrs.push_back(MInstr{MOpcode::MovRI, {MOperand::regOp(PhysReg::X1), MOperand::immOp(4)}});
    bb.instrs.push_back(MInstr{MOpcode::SDivRRR,
                               {MOperand::regOp(PhysReg::X0),
                                MOperand::regOp(PhysReg::X2),
                                MOperand::regOp(PhysReg::X1)}});
    bb.instrs.push_back(MInstr{MOpcode::Ret, {}});

    auto stats = runPeephole(fn);

    // SDivRRR should be expanded to 4 instructions:
    // mov x1, #4 (original, may be optimized away)
    // asr x1, x2, #63
    // lsr x1, x1, #62
    // add x1, x2, x1
    // asr x0, x1, #2
    EXPECT_TRUE(stats.strengthReductions >= 1);
    // The SDivRRR at original index 1 should now be an AsrRI (first of expansion)
    EXPECT_EQ(bb.instrs[1].opc, MOpcode::AsrRI);
}

/// Signed division by arbitrary non-power-of-2 constant should lower to a
/// multiply-high magic-number sequence that preserves truncation toward zero.
TEST(AArch64DivStrength, SDivByConstantUsesMagicMultiply) {
    MFunction fn{};
    fn.name = "test_sdiv_const";
    fn.blocks.push_back(MBasicBlock{"entry", {}, {}});
    auto &bb = fn.blocks.back();

    bb.instrs.push_back(MInstr{MOpcode::MovRI, {MOperand::regOp(PhysReg::X1), MOperand::immOp(7)}});
    bb.instrs.push_back(MInstr{MOpcode::SDivRRR,
                               {MOperand::regOp(PhysReg::X0),
                                MOperand::regOp(PhysReg::X2),
                                MOperand::regOp(PhysReg::X1)}});
    bb.instrs.push_back(MInstr{MOpcode::Ret, {}});

    auto stats = runPeephole(fn);

    EXPECT_TRUE(stats.strengthReductions >= 1);
    bool foundSmulh = false;
    bool foundAsr = false;
    for (const auto &instr : bb.instrs) {
        if (instr.opc == MOpcode::SmulhRRR)
            foundSmulh = true;
        if (instr.opc == MOpcode::AsrRI)
            foundAsr = true;
        EXPECT_NE(instr.opc, MOpcode::SDivRRR);
    }
    EXPECT_TRUE(foundSmulh);
    EXPECT_TRUE(foundAsr);
}

/// Unsigned division by arbitrary non-power-of-2 constant should lower to an
/// unsigned multiply-high magic-number sequence.
TEST(AArch64DivStrength, UDivByNonPowerOf2UsesMagicMultiply) {
    MFunction fn{};
    fn.name = "test_udiv_non_pow2";
    fn.blocks.push_back(MBasicBlock{"entry", {}, {}});
    auto &bb = fn.blocks.back();

    bb.instrs.push_back(MInstr{MOpcode::MovRI, {MOperand::regOp(PhysReg::X1), MOperand::immOp(7)}});
    bb.instrs.push_back(MInstr{MOpcode::UDivRRR,
                               {MOperand::regOp(PhysReg::X0),
                                MOperand::regOp(PhysReg::X2),
                                MOperand::regOp(PhysReg::X1)}});
    bb.instrs.push_back(MInstr{MOpcode::Ret, {}});

    auto stats = runPeephole(fn);

    EXPECT_TRUE(stats.strengthReductions >= 1);
    bool foundUmulh = false;
    for (const auto &instr : bb.instrs) {
        if (instr.opc == MOpcode::UmulhRRR)
            foundUmulh = true;
        EXPECT_NE(instr.opc, MOpcode::UDivRRR);
    }
    EXPECT_TRUE(foundUmulh);
}

/// Signed division by a negative constant should still preserve C/IL
/// truncation-toward-zero semantics after strength reduction.
TEST(AArch64DivStrength, SDivByNegativeConstantUsesMagicMultiply) {
    MFunction fn{};
    fn.name = "test_sdiv_neg_const";
    fn.blocks.push_back(MBasicBlock{"entry", {}, {}});
    auto &bb = fn.blocks.back();

    bb.instrs.push_back(
        MInstr{MOpcode::MovRI, {MOperand::regOp(PhysReg::X1), MOperand::immOp(-7)}});
    bb.instrs.push_back(MInstr{MOpcode::SDivRRR,
                               {MOperand::regOp(PhysReg::X0),
                                MOperand::regOp(PhysReg::X2),
                                MOperand::regOp(PhysReg::X1)}});
    bb.instrs.push_back(MInstr{MOpcode::Ret, {}});

    auto stats = runPeephole(fn);

    EXPECT_TRUE(stats.strengthReductions >= 1);
    bool foundSmulh = false;
    bool foundNegate = false;
    for (const auto &instr : bb.instrs) {
        if (instr.opc == MOpcode::SmulhRRR)
            foundSmulh = true;
        if (instr.opc == MOpcode::SubRRR && instr.ops.size() == 3 &&
            instr.ops[1].kind == MOperand::Kind::Reg && instr.ops[2].kind == MOperand::Kind::Reg) {
            foundNegate = true;
        }
        EXPECT_NE(instr.opc, MOpcode::SDivRRR);
    }
    EXPECT_TRUE(foundSmulh);
    EXPECT_TRUE(foundNegate);
}

/// Later constant reuses in the same block must not leak backward into earlier
/// division strength reduction decisions.
TEST(AArch64DivStrength, SDivStrengthReductionUsesDominatingConstant) {
    constexpr long long kMagicDiv3 = 6148914691236517206LL;

    MFunction fn{};
    fn.name = "test_sdiv_const_dominating";
    fn.blocks.push_back(MBasicBlock{"entry", {}, {}});
    auto &bb = fn.blocks.back();

    bb.instrs.push_back(
        MInstr{MOpcode::MovRI, {MOperand::regOp(PhysReg::X15), MOperand::immOp(3)}});
    bb.instrs.push_back(MInstr{MOpcode::SDivRRR,
                               {MOperand::regOp(PhysReg::X13),
                                MOperand::regOp(PhysReg::X14),
                                MOperand::regOp(PhysReg::X15)}});
    bb.instrs.push_back(
        MInstr{MOpcode::MovRR, {MOperand::regOp(PhysReg::X0), MOperand::regOp(PhysReg::X13)}});
    bb.instrs.push_back(
        MInstr{MOpcode::MovRI, {MOperand::regOp(PhysReg::X15), MOperand::immOp(5)}});
    bb.instrs.push_back(MInstr{MOpcode::Ret, {}});

    auto stats = runPeephole(fn);

    EXPECT_TRUE(stats.strengthReductions >= 1);
    bool foundMagicDiv3 = false;
    for (const auto &instr : bb.instrs) {
        if (instr.opc == MOpcode::MovRI && instr.ops.size() == 2 &&
            instr.ops[0].kind == MOperand::Kind::Reg && instr.ops[0].reg.isPhys &&
            instr.ops[1].kind == MOperand::Kind::Imm && instr.ops[1].imm == kMagicDiv3) {
            foundMagicDiv3 = true;
            break;
        }
    }
    EXPECT_TRUE(foundMagicDiv3);
}

/// Non-power-of-2 signed division must not be strength-reduced when the
/// following MSUB still needs the divisor to form a remainder.
TEST(AArch64DivStrength, SDivConstantRemainderKeepsDivSequence) {
    MFunction fn{};
    fn.name = "test_sdiv_const_preserve_divisor";
    fn.blocks.push_back(MBasicBlock{"entry", {}, {}});
    auto &bb = fn.blocks.back();

    bb.instrs.push_back(MInstr{MOpcode::MovRI, {MOperand::regOp(PhysReg::X1), MOperand::immOp(3)}});
    bb.instrs.push_back(MInstr{MOpcode::SDivRRR,
                               {MOperand::regOp(PhysReg::X3),
                                MOperand::regOp(PhysReg::X2),
                                MOperand::regOp(PhysReg::X1)}});
    bb.instrs.push_back(MInstr{MOpcode::MSubRRRR,
                               {MOperand::regOp(PhysReg::X0),
                                MOperand::regOp(PhysReg::X3),
                                MOperand::regOp(PhysReg::X1),
                                MOperand::regOp(PhysReg::X2)}});
    bb.instrs.push_back(MInstr{MOpcode::Ret, {}});

    auto stats = runPeephole(fn);

    EXPECT_EQ(stats.strengthReductions, 0);
    ASSERT_GE(bb.instrs.size(), 4u);
    EXPECT_EQ(bb.instrs[1].opc, MOpcode::SDivRRR);
    EXPECT_EQ(bb.instrs[2].opc, MOpcode::MSubRRRR);
    EXPECT_TRUE(samePhysReg(bb.instrs[2].ops[2], MOperand::regOp(PhysReg::X1)));
}

/// Non-power-of-2 unsigned division must also remain intact when an MSUB-based
/// remainder still consumes the divisor register.
TEST(AArch64DivStrength, UDivConstantRemainderKeepsDivSequence) {
    MFunction fn{};
    fn.name = "test_udiv_const_preserve_divisor";
    fn.blocks.push_back(MBasicBlock{"entry", {}, {}});
    auto &bb = fn.blocks.back();

    bb.instrs.push_back(MInstr{MOpcode::MovRI, {MOperand::regOp(PhysReg::X1), MOperand::immOp(7)}});
    bb.instrs.push_back(MInstr{MOpcode::UDivRRR,
                               {MOperand::regOp(PhysReg::X3),
                                MOperand::regOp(PhysReg::X2),
                                MOperand::regOp(PhysReg::X1)}});
    bb.instrs.push_back(MInstr{MOpcode::MSubRRRR,
                               {MOperand::regOp(PhysReg::X0),
                                MOperand::regOp(PhysReg::X3),
                                MOperand::regOp(PhysReg::X1),
                                MOperand::regOp(PhysReg::X2)}});
    bb.instrs.push_back(MInstr{MOpcode::Ret, {}});

    auto stats = runPeephole(fn);

    EXPECT_EQ(stats.strengthReductions, 0);
    ASSERT_GE(bb.instrs.size(), 4u);
    EXPECT_EQ(bb.instrs[1].opc, MOpcode::UDivRRR);
    EXPECT_EQ(bb.instrs[2].opc, MOpcode::MSubRRRR);
    EXPECT_TRUE(samePhysReg(bb.instrs[2].ops[2], MOperand::regOp(PhysReg::X1)));
}

/// UREM by power-of-2: udiv+msub -> and mask
TEST(AArch64DivStrength, URemByPowerOf2BecomesAnd) {
    MFunction fn{};
    fn.name = "test_urem_pow2";
    fn.blocks.push_back(MBasicBlock{"entry", {}, {}});
    auto &bb = fn.blocks.back();

    // mov x1, #8 (divisor)
    bb.instrs.push_back(MInstr{MOpcode::MovRI, {MOperand::regOp(PhysReg::X1), MOperand::immOp(8)}});
    // udiv x3, x2, x1
    bb.instrs.push_back(MInstr{MOpcode::UDivRRR,
                               {MOperand::regOp(PhysReg::X3),
                                MOperand::regOp(PhysReg::X2),
                                MOperand::regOp(PhysReg::X1)}});
    // msub x0, x3, x1, x2  (x0 = x2 - x3*x1 = remainder)
    bb.instrs.push_back(MInstr{MOpcode::MSubRRRR,
                               {MOperand::regOp(PhysReg::X0),
                                MOperand::regOp(PhysReg::X3),
                                MOperand::regOp(PhysReg::X1),
                                MOperand::regOp(PhysReg::X2)}});
    // ret
    bb.instrs.push_back(MInstr{MOpcode::Ret, {}});

    auto stats = runPeephole(fn);

    // UDIV+MSUB should be fused into AND dst, lhs, #7
    EXPECT_TRUE(stats.strengthReductions >= 1);
    // Find the AndRI instruction
    bool foundAnd = false;
    for (const auto &instr : bb.instrs) {
        if (instr.opc == MOpcode::AndRI) {
            foundAnd = true;
            EXPECT_EQ(instr.ops[2].imm, 7); // 8-1 = 7
            break;
        }
    }
    EXPECT_TRUE(foundAnd);
}

/// SREM by power-of-2: sdiv+msub -> sign-corrected and+sub
TEST(AArch64DivStrength, SRemByPowerOf2BecomesSignCorrectedAnd) {
    MFunction fn{};
    fn.name = "test_srem_pow2";
    fn.blocks.push_back(MBasicBlock{"entry", {}, {}});
    auto &bb = fn.blocks.back();

    // mov x1, #4 (divisor)
    bb.instrs.push_back(MInstr{MOpcode::MovRI, {MOperand::regOp(PhysReg::X1), MOperand::immOp(4)}});
    // sdiv x3, x2, x1
    bb.instrs.push_back(MInstr{MOpcode::SDivRRR,
                               {MOperand::regOp(PhysReg::X3),
                                MOperand::regOp(PhysReg::X2),
                                MOperand::regOp(PhysReg::X1)}});
    // msub x0, x3, x1, x2  (x0 = x2 - x3*x1 = remainder)
    bb.instrs.push_back(MInstr{MOpcode::MSubRRRR,
                               {MOperand::regOp(PhysReg::X0),
                                MOperand::regOp(PhysReg::X3),
                                MOperand::regOp(PhysReg::X1),
                                MOperand::regOp(PhysReg::X2)}});
    // ret
    bb.instrs.push_back(MInstr{MOpcode::Ret, {}});

    auto stats = runPeephole(fn);

    // SDIV+MSUB should be fused into sign-corrected sequence
    EXPECT_TRUE(stats.strengthReductions >= 1);
    // The SDivRRR and MSubRRRR should both be gone
    bool foundSDiv = false;
    bool foundMSub = false;
    for (const auto &instr : bb.instrs) {
        if (instr.opc == MOpcode::SDivRRR)
            foundSDiv = true;
        if (instr.opc == MOpcode::MSubRRRR)
            foundMSub = true;
    }
    EXPECT_FALSE(foundSDiv);
    EXPECT_FALSE(foundMSub);
}

// ZB-29: a constant fact for a register must die at ANY redefinition — the
// flag-setting forms the overflow-checked lowering emits (adds/subs/…ovf) were
// missing from the tracker's allowlist, so `mov x1, #2; adds x1, x2, #1;
// sdiv x0, x3, x1` was strength-reduced to a divide by 2. In the game this
// made `(a * 1000) / (b * 7 + 1)` return the dividend halved on the native
// build only, and the season diverged from the VM oracle at day 41.
TEST(AArch64DivStrength, ConstantFactDiesAtFlagSettingRedefinition) {
    MFunction fn{};
    fn.name = "test_sdiv_stale_const_adds";
    fn.blocks.push_back(MBasicBlock{"entry", {}, {}});
    auto &bb = fn.blocks.back();
    bb.instrs.push_back(MInstr{MOpcode::MovRI, {MOperand::regOp(PhysReg::X1), MOperand::immOp(2)}});
    bb.instrs.push_back(
        MInstr{MOpcode::AddsRI,
               {MOperand::regOp(PhysReg::X1), MOperand::regOp(PhysReg::X2), MOperand::immOp(1)}});
    bb.instrs.push_back(MInstr{MOpcode::SDivRRR,
                               {MOperand::regOp(PhysReg::X0),
                                MOperand::regOp(PhysReg::X3),
                                MOperand::regOp(PhysReg::X1)}});
    bb.instrs.push_back(MInstr{MOpcode::Ret, {}});
    auto stats = runPeephole(fn);
    EXPECT_EQ(stats.strengthReductions, 0u);
    bool sdivSurvives = false;
    for (const auto &mi : bb.instrs)
        if (mi.opc == MOpcode::SDivRRR)
            sdivSurvives = true;
    EXPECT_TRUE(sdivSurvives);
}

TEST(AArch64DivStrength, ConstantFactDiesAtOverflowMultiplyRedefinition) {
    MFunction fn{};
    fn.name = "test_udiv_stale_const_mulovf";
    fn.blocks.push_back(MBasicBlock{"entry", {}, {}});
    auto &bb = fn.blocks.back();
    bb.instrs.push_back(MInstr{MOpcode::MovRI, {MOperand::regOp(PhysReg::X1), MOperand::immOp(8)}});
    bb.instrs.push_back(MInstr{MOpcode::MulOvfRRR,
                               {MOperand::regOp(PhysReg::X1),
                                MOperand::regOp(PhysReg::X2),
                                MOperand::regOp(PhysReg::X4)}});
    bb.instrs.push_back(MInstr{MOpcode::UDivRRR,
                               {MOperand::regOp(PhysReg::X0),
                                MOperand::regOp(PhysReg::X3),
                                MOperand::regOp(PhysReg::X1)}});
    bb.instrs.push_back(MInstr{MOpcode::Ret, {}});
    auto stats = runPeephole(fn);
    EXPECT_EQ(stats.strengthReductions, 0u);
    bool udivSurvives = false;
    for (const auto &mi : bb.instrs)
        if (mi.opc == MOpcode::UDivRRR)
            udivSurvives = true;
    EXPECT_TRUE(udivSurvives);
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, &argv);
    return zanna_test::run_all_tests();
}
