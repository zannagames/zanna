//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/codegen/test_emit_aarch64_mir_bitwise.cpp
// Purpose: Verify MIR bitwise rr emission for and/or/xor.
// Key invariants: To be documented.
// Ownership/Lifetime: To be documented.
// Links: docs/internals/architecture.md
//
//===----------------------------------------------------------------------===//
#include "tests/TestHarness.hpp"
#include <sstream>
#include <string>

#include "codegen/aarch64/AsmEmitter.hpp"
#include "codegen/aarch64/MachineIR.hpp"

using namespace zanna::codegen::aarch64;

static std::string emit(const MInstr &mi) {
    auto &ti = darwinTarget();
    AsmEmitter emit{ti};
    MFunction fn{};
    fn.name = "mir_bits";
    fn.blocks.push_back(MBasicBlock{});
    fn.blocks.back().instrs.push_back(mi);
    std::ostringstream os;
    emit.emitFunction(os, fn);
    return os.str();
}

TEST(AArch64MIR, BitwiseRR) {
    {
        auto text = emit(MInstr{MOpcode::AndRRR,
                                {MOperand::regOp(PhysReg::X0),
                                 MOperand::regOp(PhysReg::X0),
                                 MOperand::regOp(PhysReg::X1)}});
        EXPECT_NE(text.find("and x0, x0, x1"), std::string::npos);
    }
    {
        auto text = emit(MInstr{MOpcode::OrrRRR,
                                {MOperand::regOp(PhysReg::X0),
                                 MOperand::regOp(PhysReg::X0),
                                 MOperand::regOp(PhysReg::X1)}});
        EXPECT_NE(text.find("orr x0, x0, x1"), std::string::npos);
    }
    {
        auto text = emit(MInstr{MOpcode::EorRRR,
                                {MOperand::regOp(PhysReg::X0),
                                 MOperand::regOp(PhysReg::X0),
                                 MOperand::regOp(PhysReg::X1)}});
        EXPECT_NE(text.find("eor x0, x0, x1"), std::string::npos);
    }
}

TEST(AArch64MIR, BitwiseRIFallsBackForNonLogicalImmediate) {
    {
        auto text = emit(MInstr{
            MOpcode::AndRI,
            {MOperand::regOp(PhysReg::X0), MOperand::regOp(PhysReg::X1), MOperand::immOp(5)}});
        EXPECT_NE(text.find("and x0, x1,"), std::string::npos);
        EXPECT_EQ(text.find("and x0, x1, #"), std::string::npos);
    }
}

TEST(AArch64MIR, MovRIUsesSharedWideImmediatePlan) {
    {
        auto text = emit(
            MInstr{MOpcode::MovRI, {MOperand::regOp(PhysReg::X0), MOperand::immOp(0x100000000LL)}});
        EXPECT_NE(text.find("movz x0, #1, lsl #32"), std::string::npos);
        EXPECT_EQ(text.find("movz x0, #0"), std::string::npos);
    }
    {
        auto text =
            emit(MInstr{MOpcode::MovRI, {MOperand::regOp(PhysReg::X0), MOperand::immOp(-1)}});
        EXPECT_NE(text.find("movn x0, #0"), std::string::npos);
        EXPECT_EQ(text.find("movz x0, #65535"), std::string::npos);
    }
}

// The register allocator can hand x9/x16/x17 out as one-instruction emergency
// reload homes, and fast paths keep values in x9 across neighbouring
// instructions. A wide-immediate expansion must therefore never materialise
// the immediate into a register that is one of the instruction's operands.
TEST(AArch64MIR, WideImmediateExpansionAvoidsOperandScratch) {
    constexpr long long kWide = 0x123456; // not add/sub imm12(+lsl12) encodable
    {
        auto text = emit(MInstr{
            MOpcode::AddRI,
            {MOperand::regOp(kScratchGPR), MOperand::regOp(kScratchGPR), MOperand::immOp(kWide)}});
        EXPECT_EQ(text.find("add x9, x9, x9"), std::string::npos);
        EXPECT_NE(text.find("add x9, x9, x16"), std::string::npos);
    }
    {
        auto text = emit(MInstr{
            MOpcode::SubRI,
            {MOperand::regOp(PhysReg::X0), MOperand::regOp(kScratchGPR), MOperand::immOp(kWide)}});
        EXPECT_EQ(text.find("sub x0, x9, x9"), std::string::npos);
        EXPECT_NE(text.find("sub x0, x9, x16"), std::string::npos);
    }
    {
        // 5 is not a logical immediate, so the and goes through a scratch too.
        auto text = emit(MInstr{
            MOpcode::AndRI,
            {MOperand::regOp(kScratchGPR), MOperand::regOp(PhysReg::X1), MOperand::immOp(5)}});
        EXPECT_EQ(text.find("and x9, x1, x9"), std::string::npos);
        EXPECT_NE(text.find("and x9, x1, x16"), std::string::npos);
    }
    {
        auto text =
            emit(MInstr{MOpcode::CmpRI, {MOperand::regOp(kScratchGPR2), MOperand::immOp(kWide)}});
        EXPECT_EQ(text.find("cmp x16, x16"), std::string::npos);
        EXPECT_NE(text.find("cmp x16, x9"), std::string::npos);
    }
    {
        auto text = emit(MInstr{
            MOpcode::AddsRI,
            {MOperand::regOp(PhysReg::X2), MOperand::regOp(kScratchGPR2), MOperand::immOp(kWide)}});
        EXPECT_EQ(text.find("adds x2, x16, x16"), std::string::npos);
        EXPECT_NE(text.find("adds x2, x16, x9"), std::string::npos);
    }
    {
        // The non-conflicting case keeps the historical scratch choice.
        auto text = emit(MInstr{
            MOpcode::AddRI,
            {MOperand::regOp(PhysReg::X0), MOperand::regOp(PhysReg::X1), MOperand::immOp(kWide)}});
        EXPECT_NE(text.find("add x0, x1, x9"), std::string::npos);
    }
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, &argv);
    return zanna_test::run_all_tests();
}
