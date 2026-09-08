//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/codegen/test_codegen_review_x64_frame.cpp
// Purpose: Regression tests for x86_64 frame lowering bugs found during the
//          comprehensive backend codegen review.
//
//===----------------------------------------------------------------------===//
#include "codegen/x86_64/FrameLowering.hpp"
#include "codegen/x86_64/LowerILToMIR.hpp"
#include "codegen/x86_64/MachineIR.hpp"
#include "codegen/x86_64/ra/SpillSlots.hpp"
#include "tests/TestHarness.hpp"

#include <algorithm>
#include <variant>

using namespace zanna::codegen::x64;

// Helper to count instructions with a given opcode
static int countOpcode(const MFunction &func, MOpcode opc) {
    int count = 0;
    for (const auto &block : func.blocks)
        for (const auto &instr : block.instructions)
            if (instr.opcode == opc)
                ++count;
    return count;
}

static bool hasCallTarget(const MFunction &func, const char *target) {
    for (const auto &block : func.blocks) {
        for (const auto &instr : block.instructions) {
            if (instr.opcode != MOpcode::CALL || instr.operands.empty())
                continue;
            const auto *label = std::get_if<OpLabel>(&instr.operands[0]);
            if (label && label->name == target)
                return true;
        }
    }
    return false;
}

// Helper to count stack probe instructions: MOVmr loading from (%rsp) into RAX
// (the probe touch pattern). Excludes loads into RBP which are the standard
// frame setup `mov (%rsp), %rbp`.
static int countStackProbes(const MFunction &func) {
    int count = 0;
    for (const auto &block : func.blocks) {
        for (const auto &instr : block.instructions) {
            if (instr.opcode == MOpcode::MOVmr && instr.operands.size() >= 2) {
                // Check if destination is RAX (probe target, not RBP frame restore)
                const auto *dst = std::get_if<OpReg>(&instr.operands[0]);
                if (!dst || !dst->isPhys || static_cast<PhysReg>(dst->idOrPhys) != PhysReg::RAX)
                    continue;

                // Check if source is (%rsp) with zero displacement
                if (const auto *mem = std::get_if<OpMem>(&instr.operands[1])) {
                    if (mem->base.isPhys &&
                        static_cast<PhysReg>(mem->base.idOrPhys) == PhysReg::RSP &&
                        mem->disp == 0) {
                        ++count;
                    }
                }
            }
        }
    }
    return count;
}

static std::vector<int32_t> collectLeaRbpDisps(const MFunction &func) {
    std::vector<int32_t> disps;
    for (const auto &block : func.blocks) {
        for (const auto &instr : block.instructions) {
            if (instr.opcode != MOpcode::LEA || instr.operands.size() < 2)
                continue;
            const auto *mem = std::get_if<OpMem>(&instr.operands[1]);
            if (!mem || !mem->base.isPhys)
                continue;
            if (static_cast<PhysReg>(mem->base.idOrPhys) != PhysReg::RBP)
                continue;
            disps.push_back(mem->disp);
        }
    }
    return disps;
}

static ILInstr makeAllocaInstr(int resultId, int64_t sizeBytes) {
    ILInstr instr{};
    instr.opcode = "alloca";
    instr.resultId = resultId;
    instr.resultKind = ILValue::Kind::PTR;
    instr.ops = {ILValue{.kind = ILValue::Kind::I64, .id = -1, .i64 = sizeBytes}};
    return instr;
}

static ILInstr makeRetZero() {
    ILInstr ret{};
    ret.opcode = "ret";
    ret.ops = {ILValue{.kind = ILValue::Kind::I64, .id = -1, .i64 = 0}};
    return ret;
}

// ---------------------------------------------------------------------------
// Fix: Large frame stack probing now emits actual probe code on Unix/macOS
// ---------------------------------------------------------------------------

// Helper to compute the total bytes subtracted from RSP by ADDri instructions
// (which use negative immediates for subtraction).
static int totalRspSubtraction(const MFunction &func) {
    int total = 0;
    for (const auto &block : func.blocks) {
        for (const auto &instr : block.instructions) {
            if (instr.opcode != MOpcode::ADDri || instr.operands.size() < 2)
                continue;
            const auto *dst = std::get_if<OpReg>(&instr.operands[0]);
            if (!dst || !dst->isPhys || static_cast<PhysReg>(dst->idOrPhys) != PhysReg::RSP)
                continue;
            const auto *imm = std::get_if<OpImm>(&instr.operands[1]);
            if (imm && imm->val < 0)
                total += static_cast<int>(-imm->val);
        }
    }
    return total;
}

TEST(X64FrameLowering, LargeFrameEmitsProbeLoop) {
    MFunction func;
    func.name = "test_large_frame";
    MBasicBlock block;
    block.label = "entry";
    // Add a RET so insertPrologueEpilogue has something to attach epilogue to
    block.instructions.push_back(MInstr::make(MOpcode::RET, {}));
    func.blocks.push_back(std::move(block));

    FrameInfo frame;
    // Frame larger than one page (4096 bytes) to trigger probing
    frame.frameSize = 8192;

    const auto &target = sysvTarget();
    insertPrologueEpilogue(func, target, frame);

#if !defined(_WIN32)
    // On Unix/macOS, the large frame should emit at least one probe (MOVmr from (%rsp))
    int probeCount = countStackProbes(func);
    EXPECT_TRUE(probeCount >= 1); // 8192 bytes = 2 pages, at least 1 probe
#endif
}

TEST(X64FrameLowering, LargeFrameExactProbeDepth) {
    // A frame of 4097 bytes should probe exactly 4097 bytes below RBP,
    // not round up to 8192.
    MFunction func;
    func.name = "test_exact_probe";
    MBasicBlock block;
    block.label = "entry";
    block.instructions.push_back(MInstr::make(MOpcode::RET, {}));
    func.blocks.push_back(std::move(block));

    FrameInfo frame;
    frame.frameSize = 4097;

    const auto &target = sysvTarget();
    insertPrologueEpilogue(func, target, frame);

#if !defined(_WIN32)
    // Exactly one probe for 4097 bytes (one full page, then 1-byte tail)
    int probeCount = countStackProbes(func);
    EXPECT_EQ(probeCount, 1);

    // The total RSP subtraction for the frame should be exactly frameSize + 8
    // (8 for the push %rbp at the start of the prologue).
    int totalSub = totalRspSubtraction(func);
    EXPECT_EQ(totalSub, 4097 + 8); // frame + push rbp
#endif
}

TEST(X64FrameLowering, LargeFrame8193ExactDepth) {
    // 8193 = 2 full pages + 1 byte tail.  Should emit 2 probes, not 3.
    MFunction func;
    func.name = "test_8193_probe";
    MBasicBlock block;
    block.label = "entry";
    block.instructions.push_back(MInstr::make(MOpcode::RET, {}));
    func.blocks.push_back(std::move(block));

    FrameInfo frame;
    frame.frameSize = 8193;

    const auto &target = sysvTarget();
    insertPrologueEpilogue(func, target, frame);

#if !defined(_WIN32)
    int probeCount = countStackProbes(func);
    EXPECT_EQ(probeCount, 2);

    int totalSub = totalRspSubtraction(func);
    EXPECT_EQ(totalSub, 8193 + 8); // frame + push rbp
#endif
}

TEST(X64FrameLowering, SmallFrameNoProbe) {
    MFunction func;
    func.name = "test_small_frame";
    MBasicBlock block;
    block.label = "entry";
    block.instructions.push_back(MInstr::make(MOpcode::RET, {}));
    func.blocks.push_back(std::move(block));

    FrameInfo frame;
    // Frame smaller than a page — no probing needed
    frame.frameSize = 256;

    const auto &target = sysvTarget();
    insertPrologueEpilogue(func, target, frame);

    // Small frames should NOT emit stack probes
    EXPECT_EQ(countStackProbes(func), 0);
}

TEST(X64FrameLowering, ZeroFrameNoPrologue) {
    MFunction func;
    func.name = "test_zero_frame";
    MBasicBlock block;
    block.label = "entry";
    block.instructions.push_back(MInstr::make(MOpcode::RET, {}));
    func.blocks.push_back(std::move(block));

    FrameInfo frame;
    frame.frameSize = 0;

    const auto &target = sysvTarget();
    insertPrologueEpilogue(func, target, frame);

    // Leaf functions with no frame, no calls, and no callee-saved registers
    // should skip the prologue entirely (leaf frame elimination).
    int movCount = countOpcode(func, MOpcode::MOVrr);
    EXPECT_EQ(movCount, 0); // No prologue for leaf functions
}

TEST(X64FrameLowering, IncomingStackParamsPreventLeafElision) {
    MFunction func;
    func.name = "stack_params";
    MBasicBlock block;
    block.label = "entry";
    const OpReg rbp{true, RegClass::GPR, static_cast<uint16_t>(PhysReg::RBP)};
    block.instructions.push_back(
        MInstr::make(MOpcode::MOVmr, {makeVRegOperand(RegClass::GPR, 1), makeMemOperand(rbp, 16)}));
    block.instructions.push_back(MInstr::make(MOpcode::RET, {}));
    func.blocks.push_back(std::move(block));

    FrameInfo frame;
    frame.frameSize = 0;

    insertPrologueEpilogue(func, sysvTarget(), frame);

    ASSERT_FALSE(func.blocks.empty());
    ASSERT_GE(func.blocks.front().instructions.size(), 4u);
    EXPECT_EQ(func.blocks.front().instructions[0].opcode, MOpcode::ADDri);
    EXPECT_EQ(func.blocks.front().instructions[1].opcode, MOpcode::MOVrm);
    EXPECT_EQ(func.blocks.front().instructions[2].opcode, MOpcode::MOVrr);
}

TEST(X64FrameLowering, MainGetsStackSafetyInitEvenWithoutFrame) {
    MFunction func;
    func.name = "main";
    MBasicBlock block;
    block.label = "entry";
    block.instructions.push_back(MInstr::make(MOpcode::RET, {}));
    func.blocks.push_back(std::move(block));

    FrameInfo frame;
    frame.frameSize = 0;

    insertPrologueEpilogue(func, sysvTarget(), frame);

    EXPECT_TRUE(hasCallTarget(func, "rt_init_stack_safety"));
    EXPECT_GE(countOpcode(func, MOpcode::MOVrr), 1);
}

TEST(X64FrameLowering, Win64MainReservesShadowSpaceForStackSafetyInit) {
    AsmEmitter::RoDataPool roData;
    LowerILToMIR lowering(win64Target(), roData);

    ILBlock entry{};
    entry.name = "entry";
    entry.instrs = {makeRetZero()};

    ILFunction fn{};
    fn.name = "main";
    fn.blocks = {entry};

    MFunction mir = lowering.lower(fn);
    FrameInfo frame{};
    assignSpillSlots(mir, win64Target(), frame);

    EXPECT_GE(frame.outgoingArgArea, 32);
}

TEST(X64FrameLowering, LargeAllocaConsumesMultipleSlots) {
    AsmEmitter::RoDataPool roData;
    LowerILToMIR lowering(sysvTarget(), roData);

    ILBlock entry{};
    entry.name = "entry";
    entry.instrs = {makeAllocaInstr(0, 24), makeRetZero()};

    ILFunction fn{};
    fn.name = "large_alloca";
    fn.blocks = {entry};

    MFunction mir = lowering.lower(fn);
    FrameInfo frame{};
    assignSpillSlots(mir, sysvTarget(), frame);

    const auto disps = collectLeaRbpDisps(mir);
    ASSERT_EQ(disps.size(), 1u);
    EXPECT_EQ(disps[0], -24);
    EXPECT_EQ(frame.frameSize, 32);
}

TEST(X64FrameLowering, MixedAllocaSizesDoNotOverlap) {
    AsmEmitter::RoDataPool roData;
    LowerILToMIR lowering(sysvTarget(), roData);

    ILBlock entry{};
    entry.name = "entry";
    entry.instrs = {makeAllocaInstr(0, 24), makeAllocaInstr(1, 8), makeRetZero()};

    ILFunction fn{};
    fn.name = "mixed_alloca";
    fn.blocks = {entry};

    MFunction mir = lowering.lower(fn);
    FrameInfo frame{};
    assignSpillSlots(mir, sysvTarget(), frame);

    auto disps = collectLeaRbpDisps(mir);
    std::sort(disps.begin(), disps.end());
    ASSERT_EQ(disps.size(), 2u);
    EXPECT_EQ(disps[0], -32);
    EXPECT_EQ(disps[1], -24);
    EXPECT_EQ(frame.frameSize, 32);
}

// ---------------------------------------------------------------------------
// Fix: GPR and XMM spill placeholders use disjoint index ranges, so frame
// lowering classifies slots from the displacement alone. Previously GPR slot
// N and XMM slot N shared one placeholder displacement, disambiguated by
// guessing the class from a neighbouring register operand.
// ---------------------------------------------------------------------------

TEST(X64FrameLowering, GprAndXmmSpillPlaceholdersAreDisjoint) {
    const MInstr gprStore = zanna::codegen::x64::ra::makeSpillStore(RegClass::GPR, 0, PhysReg::RAX);
    const MInstr xmmStore =
        zanna::codegen::x64::ra::makeSpillStore(RegClass::XMM, 0, PhysReg::XMM0);

    const auto *gprMem = std::get_if<OpMem>(&gprStore.operands[0]);
    const auto *xmmMem = std::get_if<OpMem>(&xmmStore.operands[0]);
    ASSERT_TRUE(gprMem != nullptr);
    ASSERT_TRUE(xmmMem != nullptr);

    // Same per-class slot index, different placeholder displacement.
    EXPECT_NE(gprMem->disp, xmmMem->disp);

    // Frame lowering must keep them as distinct frame slots even though the
    // instructions carry matching slot indices.
    MFunction func;
    func.name = "disjoint_spills";
    MBasicBlock block;
    block.label = "entry";
    block.instructions.push_back(gprStore);
    block.instructions.push_back(xmmStore);
    block.instructions.push_back(MInstr::make(MOpcode::RET, {}));
    func.blocks.push_back(std::move(block));

    FrameInfo frame;
    assignSpillSlots(func, sysvTarget(), frame);

    const auto *finalGpr = std::get_if<OpMem>(&func.blocks[0].instructions[0].operands[0]);
    const auto *finalXmm = std::get_if<OpMem>(&func.blocks[0].instructions[1].operands[0]);
    ASSERT_TRUE(finalGpr != nullptr);
    ASSERT_TRUE(finalXmm != nullptr);

    EXPECT_LT(finalGpr->disp, 0);
    EXPECT_LT(finalXmm->disp, 0);
    EXPECT_NE(finalGpr->disp, finalXmm->disp);
    EXPECT_EQ(frame.spillAreaGPR, 8);
    EXPECT_EQ(frame.spillAreaXMM, 8);
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, &argv);
    return zanna_test::run_all_tests();
}
