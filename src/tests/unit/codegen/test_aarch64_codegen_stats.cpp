//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/codegen/test_aarch64_codegen_stats.cpp
// Purpose: Pins the AArch64 codegen statistics: what counts as a frame access
//          (including accesses through an offset-materialization prefix), the
//          spill-slot and callee-saved counters, the report line format, and
//          that the pipeline reports at -O0.
// Key invariants:
//   - Counter names are part of the scripts/codegen_stats.sh contract.
// Ownership/Lifetime: Standalone test binary.
// Links: src/codegen/aarch64/CodegenStats.hpp,
//        src/codegen/aarch64/passes/CodegenStatsPass.hpp
//
//===----------------------------------------------------------------------===//

#include "tests/TestHarness.hpp"

#include "codegen/aarch64/CodegenPipeline.hpp"
#include "codegen/aarch64/CodegenStats.hpp"
#include "codegen/aarch64/TargetAArch64.hpp"
#include "il/io/Parser.hpp"
#include "tests/common/PosixCompat.h"

#include <iostream>
#include <sstream>
#include <string>

using namespace zanna::codegen::aarch64;

namespace {

MInstr ins(MOpcode opc, std::initializer_list<MOperand> ops) {
    MInstr mi;
    mi.opc = opc;
    mi.ops = ops;
    return mi;
}

MOperand r(PhysReg reg) {
    return MOperand::regOp(reg);
}

MOperand imm(long long v) {
    return MOperand::immOp(v);
}

} // namespace

TEST(AArch64CodegenStats, CountsFrameTrafficPrefixesSlotsAndSaves) {
    MFunction fn;
    fn.name = "f";
    fn.savedGPRs = {PhysReg::X19, PhysReg::X20};
    fn.savedFPRs = {PhysReg::V8};
    fn.frame.totalBytes = 64;
    fn.frame.spills.push_back(MFunction::SpillSlot{1, 8, 8, -8});
    fn.frame.spills.push_back(MFunction::SpillSlot{2, 8, 8, -16});
    fn.frame.spills.push_back(MFunction::SpillSlot{3, 8, 8, -8}); // shares slot -8

    MBasicBlock entry{"entry", {}};
    // Direct frame accesses.
    entry.instrs.push_back(ins(MOpcode::LdrRegFpImm, {r(PhysReg::X0), imm(-8)}));
    entry.instrs.push_back(ins(MOpcode::StrRegFpImm, {r(PhysReg::X0), imm(-16)}));
    // A wide-offset access after ExpandPseudos: prefix + base-form access.
    entry.instrs.push_back(ins(MOpcode::MovRI, {r(PhysReg::X9), imm(-4096)}));
    entry.instrs.push_back(ins(MOpcode::AddRRR, {r(PhysReg::X9), r(PhysReg::X29), r(PhysReg::X9)}));
    entry.instrs.push_back(ins(MOpcode::LdrRegBaseImm, {r(PhysReg::X1), r(PhysReg::X9), imm(0)}));
    // A base-form access through a pointer is not a frame access.
    entry.instrs.push_back(ins(MOpcode::LdrRegBaseImm, {r(PhysReg::X2), r(PhysReg::X1), imm(8)}));
    entry.instrs.push_back(ins(MOpcode::MovRR, {r(PhysReg::X3), r(PhysReg::X2)}));
    entry.instrs.push_back(ins(MOpcode::Bl, {MOperand::labelOp("callee")}));
    entry.instrs.push_back(ins(MOpcode::Ret, {}));
    fn.blocks.push_back(std::move(entry));

    const CodegenStats s = computeCodegenStats(fn);
    EXPECT_EQ(s.functions, 1u);
    EXPECT_EQ(s.blocks, 1u);
    EXPECT_EQ(s.instructions, 9u);
    EXPECT_EQ(s.calls, 1u);
    EXPECT_EQ(s.branches, 1u);
    EXPECT_EQ(s.moves, 1u);
    EXPECT_EQ(s.loads, 3u);
    EXPECT_EQ(s.stores, 1u);
    EXPECT_EQ(s.frameLoads, 2u);
    EXPECT_EQ(s.frameStores, 1u);
    EXPECT_EQ(s.offsetPrefixes, 1u);
    EXPECT_EQ(s.spillSlots, 2u);
    EXPECT_EQ(s.frameBytes, 64u);
    EXPECT_EQ(s.calleeSaved, 3u);

    const std::string line = formatCodegenStats(s, fn.name);
    EXPECT_CONTAINS(line, "[codegen-stats] arch=arm64 fn=f ");
    EXPECT_CONTAINS(line, " instrs=9 ");
    EXPECT_CONTAINS(line, " frameLoads=2 ");
    EXPECT_CONTAINS(line, " offsetPrefixes=1 ");
    EXPECT_CONTAINS(line, " spillSlots=2 ");
    EXPECT_CONTAINS(line, " calleeSaved=3");

    CodegenStats total;
    total.add(s);
    total.add(s);
    EXPECT_EQ(total.functions, 2u);
    EXPECT_EQ(total.frameLoads, 4u);
}

TEST(AArch64CodegenStats, PipelineReportsAtEveryLevel) {
    const char *il = R"(il 0.3.0
func @main() -> i64 {
entry:
  %a = iadd.ovf 40, 2
  ret %a
}
)";
    setenv("ZANNA_CODEGEN_STATS", "1", 1);
    for (int level : {0, 2}) {
        il::core::Module mod;
        std::istringstream in(il);
        ASSERT_TRUE(il::io::Parser::parse(in, mod));
        passes::AArch64Module m;
        m.ilMod = &mod;
        m.ti = &darwinTarget();
        PipelineOptions opts;
        opts.emitAssemblyText = true;
        opts.optimizeLevel = level;
        std::ostringstream diag;
        const bool ok = runCodegenPipeline(m, opts, diag);
        if (!ok)
            std::cerr << diag.str();
        EXPECT_TRUE(ok);
        EXPECT_CONTAINS(diag.str(), "[codegen-stats] arch=arm64 fn=main ");
        EXPECT_CONTAINS(diag.str(), "[codegen-stats] arch=arm64 fn=<module> functions=1 ");
    }
    unsetenv("ZANNA_CODEGEN_STATS");
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, &argv);
    return zanna_test::run_all_tests();
}
