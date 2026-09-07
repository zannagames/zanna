//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/codegen/test_aarch64_expand_pseudos.cpp
// Purpose: Pins ExpandPseudosPass: every emit-time pseudo form becomes explicit
//          MIR with the historical scratch choice, the scratch never collides
//          with an operand or a live reserved scratch register, encodable
//          forms are untouched, and the pipeline leaves no pseudo form for the
//          emitters.
// Key invariants:
//   - Expected sequences are the exact instructions the text emitter used to
//     produce implicitly, so generated code is unchanged by the pass.
// Ownership/Lifetime: Standalone test binary.
// Links: src/codegen/aarch64/passes/ExpandPseudosPass.hpp,
//        src/codegen/aarch64/InstrEffects.hpp
//
//===----------------------------------------------------------------------===//

#include "tests/TestHarness.hpp"

#include "codegen/aarch64/CodegenPipeline.hpp"
#include "codegen/aarch64/InstrEffects.hpp"
#include "codegen/aarch64/TargetAArch64.hpp"
#include "codegen/aarch64/passes/ExpandPseudosPass.hpp"
#include "il/io/Parser.hpp"

#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace zanna::codegen::aarch64;

namespace {

MOperand x(PhysReg r) {
    return MOperand::regOp(r);
}

MOperand imm(long long v) {
    return MOperand::immOp(v);
}

MInstr ins(MOpcode opc, std::vector<MOperand> ops) {
    return MInstr{opc, std::move(ops)};
}

/// Single-block function holding @p instrs followed by `Ret`.
MFunction fnOf(std::vector<MInstr> instrs) {
    MFunction fn;
    fn.name = "f";
    MBasicBlock bb;
    bb.name = "entry";
    bb.instrs = std::move(instrs);
    bb.instrs.push_back(ins(MOpcode::Ret, {}));
    fn.blocks.push_back(std::move(bb));
    return fn;
}

/// Render the entry block's instructions one per line (without `Ret`).
std::vector<std::string> lines(const MFunction &fn) {
    std::vector<std::string> out;
    for (const auto &mi : fn.blocks.front().instrs) {
        if (mi.opc == MOpcode::Ret)
            continue;
        out.push_back(toString(mi));
    }
    return out;
}

void expectLines(const MFunction &fn, const std::vector<std::string> &expected) {
    const auto actual = lines(fn);
    if (actual != expected) {
        std::cerr << "expected:\n";
        for (const auto &l : expected)
            std::cerr << "  " << l << "\n";
        std::cerr << "actual:\n";
        for (const auto &l : actual)
            std::cerr << "  " << l << "\n";
    }
    EXPECT_TRUE(actual == expected);
}

bool anyPseudoLeft(const MFunction &fn) {
    for (const auto &bb : fn.blocks)
        for (const auto &mi : bb.instrs)
            if (emitTimeScratchClobber(mi))
                return true;
    return false;
}

long long f64Bits(double v) {
    long long bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    return bits;
}

il::core::Module parseIL(const std::string &src) {
    std::istringstream ss(src);
    il::core::Module mod;
    if (!il::io::Parser::parse(ss, mod))
        return {};
    return mod;
}

constexpr long long kWide = 0x123456; // not add/sub imm12(+lsl12) encodable

} // namespace

TEST(AArch64ExpandPseudos, WideAluImmediateUsesX9UnlessItIsAnOperand) {
    MFunction plain = fnOf({ins(MOpcode::AddRI, {x(PhysReg::X0), x(PhysReg::X1), imm(kWide)})});
    EXPECT_EQ(expandPseudoInstructions(plain), 1u);
    expectLines(plain, {"MovRI @x9:gpr, #1193046", "AddRRR @x0:gpr, @x1:gpr, @x9:gpr"});
    EXPECT_FALSE(anyPseudoLeft(plain));

    MFunction onScratch = fnOf({ins(MOpcode::AddRI, {x(PhysReg::X9), x(PhysReg::X9), imm(kWide)})});
    EXPECT_EQ(expandPseudoInstructions(onScratch), 1u);
    expectLines(onScratch, {"MovRI @x16:gpr, #1193046", "AddRRR @x9:gpr, @x9:gpr, @x16:gpr"});

    MFunction sub = fnOf({ins(MOpcode::SubRI, {x(PhysReg::X0), x(PhysReg::X9), imm(kWide)})});
    EXPECT_EQ(expandPseudoInstructions(sub), 1u);
    expectLines(sub, {"MovRI @x16:gpr, #1193046", "SubRRR @x0:gpr, @x9:gpr, @x16:gpr"});
}

TEST(AArch64ExpandPseudos, LogicalImmediateFallsBackToRegisterForm) {
    // 5 is not a logical immediate.
    MFunction fn = fnOf({ins(MOpcode::AndRI, {x(PhysReg::X9), x(PhysReg::X1), imm(5)})});
    EXPECT_EQ(expandPseudoInstructions(fn), 1u);
    expectLines(fn, {"MovRI @x16:gpr, #5", "AndRRR @x9:gpr, @x1:gpr, @x16:gpr"});

    MFunction orr = fnOf({ins(MOpcode::OrrRI, {x(PhysReg::X0), x(PhysReg::X1), imm(5)})});
    EXPECT_EQ(expandPseudoInstructions(orr), 1u);
    expectLines(orr, {"MovRI @x9:gpr, #5", "OrrRRR @x0:gpr, @x1:gpr, @x9:gpr"});

    MFunction eor = fnOf({ins(MOpcode::EorRI, {x(PhysReg::X0), x(PhysReg::X1), imm(0xFF)})});
    // 0xFF is a logical immediate: untouched.
    EXPECT_EQ(expandPseudoInstructions(eor), 0u);
    expectLines(eor, {"EorRI @x0:gpr, @x1:gpr, #255"});
}

TEST(AArch64ExpandPseudos, CompareAndFlagSettingFormsPreferX16) {
    MFunction cmp = fnOf({ins(MOpcode::CmpRI, {x(PhysReg::X0), imm(kWide)})});
    EXPECT_EQ(expandPseudoInstructions(cmp), 1u);
    expectLines(cmp, {"MovRI @x16:gpr, #1193046", "CmpRR @x0:gpr, @x16:gpr"});

    MFunction cmpScratch = fnOf({ins(MOpcode::CmpRI, {x(PhysReg::X16), imm(kWide)})});
    EXPECT_EQ(expandPseudoInstructions(cmpScratch), 1u);
    expectLines(cmpScratch, {"MovRI @x9:gpr, #1193046", "CmpRR @x16:gpr, @x9:gpr"});

    MFunction adds = fnOf({ins(MOpcode::AddsRI, {x(PhysReg::X2), x(PhysReg::X16), imm(kWide)})});
    EXPECT_EQ(expandPseudoInstructions(adds), 1u);
    expectLines(adds, {"MovRI @x9:gpr, #1193046", "AddsRRR @x2:gpr, @x16:gpr, @x9:gpr"});

    MFunction subs = fnOf({ins(MOpcode::SubsRI, {x(PhysReg::X2), x(PhysReg::X3), imm(-kWide)})});
    EXPECT_EQ(expandPseudoInstructions(subs), 1u);
    expectLines(subs, {"MovRI @x16:gpr, #-1193046", "SubsRRR @x2:gpr, @x3:gpr, @x16:gpr"});
}

TEST(AArch64ExpandPseudos, NonFp8ConstantGoesThroughX16) {
    MFunction fn = fnOf({ins(MOpcode::FMovRI, {x(PhysReg::V0), imm(f64Bits(0.1))})});
    EXPECT_EQ(expandPseudoInstructions(fn), 1u);
    expectLines(fn,
                {"MovRI @x16:gpr, #" + std::to_string(f64Bits(0.1)), "FMovGR @v0:fpr, @x16:gpr"});

    MFunction fp8 = fnOf({ins(MOpcode::FMovRI, {x(PhysReg::V0), imm(f64Bits(1.5))})});
    EXPECT_EQ(expandPseudoInstructions(fp8), 0u);
}

TEST(AArch64ExpandPseudos, LargeFrameAddressAndAccesses) {
    MFunction addr = fnOf({ins(MOpcode::AddFpImm, {x(PhysReg::X0), imm(-5000)})});
    EXPECT_EQ(expandPseudoInstructions(addr), 1u);
    expectLines(addr, {"MovRI @x9:gpr, #-5000", "AddRRR @x0:gpr, @x29:gpr, @x9:gpr"});

    MFunction load = fnOf({ins(MOpcode::LdrRegFpImm, {x(PhysReg::X0), imm(-300)})});
    EXPECT_EQ(expandPseudoInstructions(load), 1u);
    expectLines(load,
                {"MovRI @x9:gpr, #-300",
                 "AddRRR @x9:gpr, @x29:gpr, @x9:gpr",
                 "LdrRegBaseImm @x0:gpr, @x9:gpr, #0"});

    // A store whose source is x9 must not use x9 as the address.
    MFunction store = fnOf({ins(MOpcode::StrRegFpImm, {x(PhysReg::X9), imm(-300)})});
    EXPECT_EQ(expandPseudoInstructions(store), 1u);
    expectLines(store,
                {"MovRI @x16:gpr, #-300",
                 "AddRRR @x16:gpr, @x29:gpr, @x16:gpr",
                 "StrRegBaseImm @x9:gpr, @x16:gpr, #0"});

    MFunction narrow = fnOf({ins(MOpcode::Str16RegFpImm, {x(PhysReg::X2), imm(-1000)})});
    EXPECT_EQ(expandPseudoInstructions(narrow), 1u);
    expectLines(narrow,
                {"MovRI @x9:gpr, #-1000",
                 "AddRRR @x9:gpr, @x29:gpr, @x9:gpr",
                 "Str16RegBaseImm @x2:gpr, @x9:gpr, #0"});

    MFunction fpr = fnOf({ins(MOpcode::LdrFprFpImm, {x(PhysReg::V3), imm(-264)})});
    EXPECT_EQ(expandPseudoInstructions(fpr), 1u);
    expectLines(fpr,
                {"MovRI @x9:gpr, #-264",
                 "AddRRR @x9:gpr, @x29:gpr, @x9:gpr",
                 "LdrFprBaseImm @v3:fpr, @x9:gpr, #0"});
}

TEST(AArch64ExpandPseudos, BaseAccessAvoidsItsBaseRegister) {
    MFunction fn = fnOf({ins(MOpcode::LdrRegBaseImm, {x(PhysReg::X0), x(PhysReg::X9), imm(-300)})});
    EXPECT_EQ(expandPseudoInstructions(fn), 1u);
    expectLines(fn,
                {"MovRI @x16:gpr, #-300",
                 "AddRRR @x16:gpr, @x9:gpr, @x16:gpr",
                 "LdrRegBaseImm @x0:gpr, @x16:gpr, #0"});

    // Positive offsets that fit the scaled unsigned form are encodable as-is.
    MFunction scaled =
        fnOf({ins(MOpcode::LdrRegBaseImm, {x(PhysReg::X0), x(PhysReg::X1), imm(4096)})});
    EXPECT_EQ(expandPseudoInstructions(scaled), 0u);

    MFunction unaligned =
        fnOf({ins(MOpcode::LdrRegBaseImm, {x(PhysReg::X0), x(PhysReg::X1), imm(4097)})});
    EXPECT_EQ(expandPseudoInstructions(unaligned), 1u);
}

TEST(AArch64ExpandPseudos, ScratchLiveAcrossTheExpansionIsSkipped) {
    // x9 carries a value across the wide add; the expansion must use x16.
    MFunction fn = fnOf({
        ins(MOpcode::MovRI, {x(PhysReg::X9), imm(1)}),
        ins(MOpcode::AddRI, {x(PhysReg::X0), x(PhysReg::X1), imm(kWide)}),
        ins(MOpcode::AddRRR, {x(PhysReg::X0), x(PhysReg::X0), x(PhysReg::X9)}),
    });
    EXPECT_EQ(expandPseudoInstructions(fn), 1u);
    expectLines(fn,
                {"MovRI @x9:gpr, #1",
                 "MovRI @x16:gpr, #1193046",
                 "AddRRR @x0:gpr, @x1:gpr, @x16:gpr",
                 "AddRRR @x0:gpr, @x0:gpr, @x9:gpr"});

    // A call in between ends x9's live range, so the historical choice returns.
    MFunction afterCall = fnOf({
        ins(MOpcode::MovRI, {x(PhysReg::X9), imm(1)}),
        ins(MOpcode::AddRI, {x(PhysReg::X0), x(PhysReg::X1), imm(kWide)}),
        ins(MOpcode::Bl, {MOperand::labelOp("callee")}),
        ins(MOpcode::AddRRR, {x(PhysReg::X0), x(PhysReg::X0), x(PhysReg::X9)}),
    });
    EXPECT_EQ(expandPseudoInstructions(afterCall), 1u);
    EXPECT_EQ(lines(afterCall)[1], "MovRI @x9:gpr, #1193046");

    // Both x9 and x16 live: x17 is the last resort.
    MFunction twoLive = fnOf({
        ins(MOpcode::MovRI, {x(PhysReg::X9), imm(1)}),
        ins(MOpcode::MovRI, {x(PhysReg::X16), imm(2)}),
        ins(MOpcode::AddRI, {x(PhysReg::X0), x(PhysReg::X1), imm(kWide)}),
        ins(MOpcode::AddRRR, {x(PhysReg::X0), x(PhysReg::X9), x(PhysReg::X16)}),
    });
    EXPECT_EQ(expandPseudoInstructions(twoLive), 1u);
    EXPECT_EQ(lines(twoLive)[2], "MovRI @x17:gpr, #1193046");
}

TEST(AArch64ExpandPseudos, PairOutsideImm7SplitsIntoScalars) {
    MFunction far = fnOf({ins(MOpcode::LdpRegFpImm, {x(PhysReg::X0), x(PhysReg::X1), imm(-520)})});
    EXPECT_EQ(expandPseudoInstructions(far), 3u);
    expectLines(far,
                {"MovRI @x9:gpr, #-520",
                 "AddRRR @x9:gpr, @x29:gpr, @x9:gpr",
                 "LdrRegBaseImm @x0:gpr, @x9:gpr, #0",
                 "MovRI @x9:gpr, #-512",
                 "AddRRR @x9:gpr, @x29:gpr, @x9:gpr",
                 "LdrRegBaseImm @x1:gpr, @x9:gpr, #0"});

    // Unaligned but near: the scalars stay in range and need no scratch.
    MFunction unaligned =
        fnOf({ins(MOpcode::StpRegFpImm, {x(PhysReg::X0), x(PhysReg::X1), imm(-20)})});
    EXPECT_EQ(expandPseudoInstructions(unaligned), 1u);
    expectLines(unaligned, {"StrRegFpImm @x0:gpr, #-20", "StrRegFpImm @x1:gpr, #-12"});

    MFunction fpr = fnOf({ins(MOpcode::StpFprFpImm, {x(PhysReg::V0), x(PhysReg::V1), imm(-600)})});
    EXPECT_EQ(expandPseudoInstructions(fpr), 3u);
    EXPECT_EQ(lines(fpr)[2], "StrFprBaseImm @v0:fpr, @x9:gpr, #0");
    EXPECT_EQ(lines(fpr)[5], "StrFprBaseImm @v1:fpr, @x9:gpr, #0");
}

TEST(AArch64ExpandPseudos, StackStoreOutsideScaledRangeCopiesSp) {
    MFunction fn = fnOf({ins(MOpcode::StrRegSpImm, {x(PhysReg::X0), imm(12)})});
    EXPECT_EQ(expandPseudoInstructions(fn), 1u);
    expectLines(fn,
                {"AddRI @x9:gpr, @sp:gpr, #0",
                 "AddRI @x9:gpr, @x9:gpr, #12",
                 "StrRegBaseImm @x0:gpr, @x9:gpr, #0"});
    EXPECT_FALSE(anyPseudoLeft(fn));

    MFunction aligned = fnOf({ins(MOpcode::StrFprSpImm, {x(PhysReg::V0), imm(32)})});
    EXPECT_EQ(expandPseudoInstructions(aligned), 0u);
}

TEST(AArch64ExpandPseudos, EncodableFormsAreUntouched) {
    MFunction fn = fnOf({
        ins(MOpcode::AddRI, {x(PhysReg::X0), x(PhysReg::X1), imm(4095)}),
        ins(MOpcode::AddRI, {x(PhysReg::X0), x(PhysReg::X1), imm(0x1000)}),
        ins(MOpcode::CmpRI, {x(PhysReg::X0), imm(-4095)}),
        ins(MOpcode::LdrRegFpImm, {x(PhysReg::X0), imm(-256)}),
        ins(MOpcode::StrRegFpImm, {x(PhysReg::X0), imm(255)}),
        ins(MOpcode::AddFpImm, {x(PhysReg::X0), imm(-4095)}),
        ins(MOpcode::LdpRegFpImm, {x(PhysReg::X0), x(PhysReg::X1), imm(-512)}),
        ins(MOpcode::StpRegFpImm, {x(PhysReg::X0), x(PhysReg::X1), imm(504)}),
        ins(MOpcode::AndRI, {x(PhysReg::X0), x(PhysReg::X1), imm(0xFF)}),
        ins(MOpcode::FMovRI, {x(PhysReg::V0), imm(f64Bits(2.0))}),
    });
    const auto before = lines(fn);
    EXPECT_EQ(expandPseudoInstructions(fn), 0u);
    EXPECT_TRUE(lines(fn) == before);
}

TEST(AArch64ExpandPseudos, PipelineLeavesNoPseudoFormForTheEmitters) {
    // A function with a frame far larger than the ±256-byte unscaled range and
    // wide immediates, at every optimization level.
    const char *const il = R"(il 0.3.0

func @big(%n: i64) -> i64 {
entry(%n: i64):
  %buf = alloca 4096
  %p = gep %buf, 8
  store i64, %p, %n
  %v = load i64, %p
  %w = iadd.ovf %v, 1193046
  %m = and %w, 1193046
  %c = icmp_eq %m, 1193046
  cbr %c, yes(%w), no(%m)
yes(%a: i64):
  ret %a
no(%b: i64):
  %f = sitofp %b
  %g = fmul %f, 0.1
  %r = fptosi %g
  ret %r
}

func @main() -> i64 {
entry:
  %r = call @big(7)
  ret %r
}
)";
    for (int level : {0, 1, 2}) {
        il::core::Module mod = parseIL(il);
        ASSERT_FALSE(mod.functions.empty());
        passes::AArch64Module m;
        m.ilMod = &mod;
        m.ti = &darwinTarget();
        PipelineOptions opts;
        opts.emitAssemblyText = true;
        opts.useBinaryEmit = true;
        opts.optimizeLevel = level;
        opts.verifyMir = true;
        std::ostringstream diag;
        const bool ok = runCodegenPipeline(m, opts, diag);
        if (!ok)
            std::cerr << diag.str();
        EXPECT_TRUE(ok);
        for (const auto &fn : m.mir)
            EXPECT_FALSE(anyPseudoLeft(fn));
        EXPECT_NE(m.assembly.find("x29"), std::string::npos);
        EXPECT_TRUE(m.binaryText.has_value());
    }
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, &argv);
    return zanna_test::run_all_tests();
}
