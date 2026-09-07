//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/codegen/test_x86_codegen_stats.cpp
// Purpose: Pins the x86-64 codegen statistics counters and report format so
//          scripts/codegen_stats.sh reads one contract for both backends.
// Key invariants:
//   - RBP-relative memory operands count as frame traffic; LEA does not.
// Ownership/Lifetime: Standalone test binary.
// Links: src/codegen/x86_64/CodegenStats.hpp
//
//===----------------------------------------------------------------------===//

#include "tests/TestHarness.hpp"

#include "codegen/x86_64/CodegenStats.hpp"
#include "codegen/x86_64/MachineIR.hpp"
#include "codegen/x86_64/TargetX64.hpp"

#include <string>

using namespace zanna::codegen::x64;

namespace {

OpReg phys(PhysReg reg) {
    OpReg r;
    r.isPhys = true;
    r.cls = RegClass::GPR;
    r.idOrPhys = static_cast<uint16_t>(reg);
    return r;
}

OpMem frame(int32_t disp) {
    OpMem m;
    m.base = phys(PhysReg::RBP);
    m.disp = disp;
    return m;
}

OpMem heap(int32_t disp) {
    OpMem m;
    m.base = phys(PhysReg::RSI);
    m.disp = disp;
    return m;
}

} // namespace

TEST(X86CodegenStats, CountsFrameTrafficSlotsAndSaves) {
    MFunction fn;
    fn.name = "f";
    MBasicBlock entry;
    entry.label = "entry";
    // Prologue save of RBX into the callee-saved area: register preservation,
    // not frame traffic.
    entry.instructions.push_back(MInstr::make(MOpcode::MOVrm, {frame(-8), phys(PhysReg::RBX)}));
    entry.instructions.push_back(MInstr::make(MOpcode::MOVmr, {phys(PhysReg::RAX), frame(-16)}));
    entry.instructions.push_back(MInstr::make(MOpcode::MOVrm, {frame(-24), phys(PhysReg::RAX)}));
    entry.instructions.push_back(MInstr::make(MOpcode::ADDrm, {phys(PhysReg::RAX), frame(-32)}));
    entry.instructions.push_back(MInstr::make(MOpcode::LEA, {phys(PhysReg::RCX), frame(-40)}));
    entry.instructions.push_back(MInstr::make(MOpcode::MOVmr, {phys(PhysReg::RDX), heap(8)}));
    entry.instructions.push_back(
        MInstr::make(MOpcode::MOVrr, {phys(PhysReg::RDI), phys(PhysReg::RDX)}));
    entry.instructions.push_back(MInstr::make(MOpcode::CALL, {OpLabel{"callee"}}));
    entry.instructions.push_back(MInstr::make(MOpcode::RET, {}));
    fn.blocks.push_back(std::move(entry));

    FrameInfo frameInfo;
    frameInfo.spillAreaGPR = 16;
    frameInfo.spillAreaXMM = 8;
    frameInfo.frameSize = 48;
    frameInfo.usedCalleeSaved = {PhysReg::RBX};

    const CodegenStats s = computeCodegenStats(fn, frameInfo);
    EXPECT_EQ(s.functions, 1u);
    EXPECT_EQ(s.blocks, 1u);
    EXPECT_EQ(s.instructions, 9u);
    EXPECT_EQ(s.calls, 1u);
    EXPECT_EQ(s.branches, 1u);
    EXPECT_EQ(s.moves, 1u);
    EXPECT_EQ(s.loads, 2u);
    EXPECT_EQ(s.stores, 2u);
    EXPECT_EQ(s.frameLoads, 2u);  // MOVmr [rbp-16] and ADDrm [rbp-32]; LEA excluded
    EXPECT_EQ(s.frameStores, 1u); // MOVrm [rbp-24]; the RBX save is excluded
    EXPECT_EQ(s.offsetPrefixes, 0u);
    EXPECT_EQ(s.spillSlots, 3u);
    EXPECT_EQ(s.frameBytes, 48u);
    EXPECT_EQ(s.calleeSaved, 1u);

    const std::string line = formatCodegenStats(s, fn.name);
    EXPECT_CONTAINS(line, "[codegen-stats] arch=x64 fn=f ");
    EXPECT_CONTAINS(line, " frameLoads=2 ");
    EXPECT_CONTAINS(line, " spillSlots=3 ");
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, &argv);
    return zanna_test::run_all_tests();
}
