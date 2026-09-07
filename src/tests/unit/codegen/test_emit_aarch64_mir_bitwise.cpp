//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/codegen/test_emit_aarch64_mir_bitwise.cpp
// Purpose: Verify MIR bitwise and wide-immediate emission for AArch64.
// Key invariants:
//   - Functions are expanded by ExpandPseudosPass before emission, exactly
//     as the pipeline does; the emitter itself rejects any pseudo form.
//   - Wide-immediate expansions never materialise the immediate into a
//     register that is one of the instruction's operands.
// Ownership/Lifetime: Standalone test binary.
// Links: src/codegen/aarch64/passes/ExpandPseudosPass.hpp,
//        src/codegen/aarch64/AsmEmitter.hpp
//
//===----------------------------------------------------------------------===//
#include "tests/TestHarness.hpp"
#include <sstream>
#include <stdexcept>
#include <string>

#include "codegen/aarch64/AsmEmitter.hpp"
#include "codegen/aarch64/MachineIR.hpp"
#include "codegen/aarch64/passes/ExpandPseudosPass.hpp"

using namespace zanna::codegen::aarch64;

namespace {

MFunction functionOf(const MInstr &mi) {
    MFunction fn{};
    fn.name = "mir_bits";
    fn.blocks.push_back(MBasicBlock{});
    fn.blocks.back().instrs.push_back(mi);
    return fn;
}

/// Emit @p fn as text without any expansion (what the emitter sees raw).
std::string emitRaw(const MFunction &fn) {
    auto &ti = darwinTarget();
    AsmEmitter emitter{ti};
    std::ostringstream os;
    emitter.emitFunction(os, fn);
    return os.str();
}

/// Expand pseudo forms as the pipeline does, then emit as text.
std::string emit(const MInstr &mi) {
    MFunction fn = functionOf(mi);
    (void)expandPseudoInstructions(fn);
    return emitRaw(fn);
}

} // namespace

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

// Every emit-time expansion now lives in ExpandPseudosPass; the emitter must
// refuse a pseudo form instead of silently writing a scratch register.
TEST(AArch64MIR, EmitterRejectsUnexpandedPseudoForms) {
    constexpr long long kWide = 0x123456;
    const MInstr forms[] = {
        MInstr{
            MOpcode::AddRI,
            {MOperand::regOp(PhysReg::X0), MOperand::regOp(PhysReg::X1), MOperand::immOp(kWide)}},
        MInstr{MOpcode::AndRI,
               {MOperand::regOp(PhysReg::X0), MOperand::regOp(PhysReg::X1), MOperand::immOp(5)}},
        MInstr{MOpcode::CmpRI, {MOperand::regOp(PhysReg::X0), MOperand::immOp(kWide)}},
        MInstr{MOpcode::AddFpImm, {MOperand::regOp(PhysReg::X0), MOperand::immOp(-5000)}},
        MInstr{MOpcode::LdrRegFpImm, {MOperand::regOp(PhysReg::X0), MOperand::immOp(-300)}},
        MInstr{MOpcode::StrRegSpImm, {MOperand::regOp(PhysReg::X0), MOperand::immOp(12)}},
        MInstr{MOpcode::LdpRegFpImm,
               {MOperand::regOp(PhysReg::X0), MOperand::regOp(PhysReg::X1), MOperand::immOp(-520)}},
    };
    for (const MInstr &mi : forms) {
        EXPECT_THROWS(emitRaw(functionOf(mi)), std::runtime_error);
        // The same instruction is fine once expanded.
        MFunction fn = functionOf(mi);
        EXPECT_GT(expandPseudoInstructions(fn), 0u);
        EXPECT_FALSE(emitRaw(fn).empty());
    }
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, &argv);
    return zanna_test::run_all_tests();
}
