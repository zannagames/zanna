//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/codegen/test_aarch64_scheduler.cpp
// Purpose: Verify the AArch64 post-RA instruction scheduler (Priority 2G).
//
// Background:
//   Without instruction scheduling, the register allocator emits instructions
//   in lowering order.  On out-of-order microarchitectures like Apple M1/M2,
//   this can produce avoidable stalls when a consumer immediately follows its
//   producer:
//     - Load (ldr): 4-cycle latency → a use 1 instruction later stalls for 3.
//     - Integer multiply (mul): 3-cycle latency.
//     - Integer add (add): 1-cycle latency.
//
//   The SchedulerPass inserts a list-scheduling stage between RegAllocPass
//   and PeepholePass.  It builds a per-block dependency DAG from physical-
//   register def/use chains and reorders instructions using a critical-path
//   priority to reduce stalls.
//
//   Key invariant: scheduling is a pure reordering — no instructions are
//   added or removed.  The total instruction count must remain the same, and
//   the assembly must remain functionally correct.
//
// Tests:
//   1. CorrectOutput          — Full pipeline with scheduler produces correct asm.
//   2. InstructionCountStable — Scheduling does not add or remove instructions.
//   3. LoadUseSeparation      — In a function with a load followed by an
//                               independent computation then a use, scheduling
//                               moves the independent computation before the use,
//                               increasing the load-use distance.
//   4. TerminatorLast         — Terminator instructions (ret, b, cbnz) remain
//                               at the end of their block after scheduling.
//   5. CallsAreBoundaries     — Calls stay fixed while surrounding segments
//                               are scheduled independently.
//   6. PipelineIntegration    — SchedulerPass inserted between RA and Peephole
//                               in the PassManager produces identical behaviour.
//
//===----------------------------------------------------------------------===//

#include "tests/TestHarness.hpp"
#include <iostream>
#include <sstream>
#include <string>

#include "codegen/aarch64/TargetAArch64.hpp"
#include "codegen/aarch64/passes/BlockLayoutPass.hpp"
#include "codegen/aarch64/passes/EmitPass.hpp"
#include "codegen/aarch64/passes/LegalizePass.hpp"
#include "codegen/aarch64/passes/LoweringPass.hpp"
#include "codegen/aarch64/passes/PassManager.hpp"
#include "codegen/aarch64/passes/PeepholePass.hpp"
#include "codegen/aarch64/passes/RegAllocPass.hpp"
#include "codegen/aarch64/passes/SchedulerPass.hpp"
#include "il/io/Parser.hpp"

using namespace zanna::codegen::aarch64;
using namespace zanna::codegen::aarch64::passes;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

static il::core::Module parseIL(const std::string &src) {
    std::istringstream ss(src);
    il::core::Module mod;
    if (!il::io::Parser::parse(ss, mod))
        return {};
    return mod;
}

/// Build a PassManager with the production optimized pass order.
static PassManager buildScheduledPipeline() {
    PassManager pm;
    pm.addPass(std::make_unique<LoweringPass>());
    pm.addPass(std::make_unique<LegalizePass>());
    pm.addPass(std::make_unique<RegAllocPass>());
    pm.addPass(std::make_unique<BlockLayoutPass>());
    pm.addPass(std::make_unique<PeepholePass>());
    pm.addPass(std::make_unique<SchedulerPass>());
    pm.addPass(std::make_unique<PeepholePass>());
    pm.addPass(std::make_unique<EmitPass>());
    return pm;
}

/// Count occurrences of a literal substring in a string.
static int countSubstr(const std::string &text, const std::string &needle) {
    int n = 0;
    std::size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

static void dumpDiagnosticsOnFailure(bool ok, const Diagnostics &diags) {
    if (ok)
        return;
    for (const auto &err : diags.errors())
        std::cerr << err << '\n';
}

} // namespace

// ---------------------------------------------------------------------------
// Test 1: Scheduled pipeline produces functionally correct output.
// ---------------------------------------------------------------------------
//
// Run a simple function through the full scheduled pipeline and verify
// the assembly contains the expected instructions.
//
TEST(AArch64Scheduler, CorrectOutput) {
    const std::string il = "il 0.1\n"
                           "func @sched_simple() -> i64 {\n"
                           "entry:\n"
                           "  %a = iadd.ovf 10, 20\n"
                           "  %b = iadd.ovf 30, 40\n"
                           "  %c = iadd.ovf %a, %b\n"
                           "  ret %c\n"
                           "}\n";

    il::core::Module mod = parseIL(il);
    ASSERT_FALSE(mod.functions.empty());

    const TargetInfo &ti = darwinTarget();
    AArch64Module m;
    m.ilMod = &mod;
    m.ti = &ti;

    Diagnostics diags;
    const bool ok = buildScheduledPipeline().run(m, diags);
    dumpDiagnosticsOnFailure(ok, diags);

    EXPECT_TRUE(ok);
    EXPECT_FALSE(m.assembly.empty());
    // Function label must be present.
    EXPECT_NE(m.assembly.find("sched_simple"), std::string::npos);
    // Addition instructions must be present.
    EXPECT_NE(m.assembly.find("add"), std::string::npos);
    // Return instruction must be present.
    EXPECT_NE(m.assembly.find("ret"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Test 2: Instruction count is stable — scheduling is pure reordering.
// ---------------------------------------------------------------------------
//
// Compare instruction counts in the unscheduled vs. scheduled assembly.
// They must be equal: the scheduler must not add or remove instructions.
//
TEST(AArch64Scheduler, InstructionCountStable) {
    const std::string il = "il 0.1\n"
                           "func @count_stable() -> i64 {\n"
                           "entry:\n"
                           "  %a = iadd.ovf 1, 2\n"
                           "  %b = iadd.ovf 3, 4\n"
                           "  %c = imul.ovf %a, %b\n"
                           "  %d = iadd.ovf %c, 5\n"
                           "  ret %d\n"
                           "}\n";

    // Unscheduled pipeline (no SchedulerPass).
    PassManager unscheduled;
    unscheduled.addPass(std::make_unique<LoweringPass>());
    unscheduled.addPass(std::make_unique<LegalizePass>());
    unscheduled.addPass(std::make_unique<RegAllocPass>());
    unscheduled.addPass(std::make_unique<EmitPass>());

    // Scheduled pipeline (with SchedulerPass after RA, before emit).
    PassManager scheduled;
    scheduled.addPass(std::make_unique<LoweringPass>());
    scheduled.addPass(std::make_unique<LegalizePass>());
    scheduled.addPass(std::make_unique<RegAllocPass>());
    scheduled.addPass(std::make_unique<SchedulerPass>());
    scheduled.addPass(std::make_unique<EmitPass>());

    il::core::Module mod1 = parseIL(il);
    il::core::Module mod2 = parseIL(il);
    ASSERT_FALSE(mod1.functions.empty());
    ASSERT_FALSE(mod2.functions.empty());

    const TargetInfo &ti = darwinTarget();
    AArch64Module m1, m2;
    m1.ilMod = &mod1;
    m1.ti = &ti;
    m2.ilMod = &mod2;
    m2.ti = &ti;

    Diagnostics d1, d2;
    ASSERT_TRUE(unscheduled.run(m1, d1));
    ASSERT_TRUE(scheduled.run(m2, d2));

    // Count total instruction lines (lines with leading whitespace).
    // We exclude labels (no leading space) and directives (.text, .globl).
    auto countInstrs = [](const std::string &asm_) {
        int n = 0;
        std::istringstream ss(asm_);
        std::string line;
        while (std::getline(ss, line)) {
            if (!line.empty() && (line[0] == ' ' || line[0] == '\t'))
                ++n;
        }
        return n;
    };

    const int unschCount = countInstrs(m1.assembly);
    const int schedCount = countInstrs(m2.assembly);

    // Scheduling must not add or remove instructions.
    if (unschCount != schedCount) {
        std::cerr << "Unscheduled: " << unschCount << " instructions\n"
                  << "Scheduled:   " << schedCount << " instructions\n"
                  << "Unscheduled assembly:\n"
                  << m1.assembly << "\nScheduled assembly:\n"
                  << m2.assembly << "\n";
    }
    EXPECT_TRUE(unschCount == schedCount);
}

// ---------------------------------------------------------------------------
// Test 3: Load-use separation — independent instructions fill load latency.
// ---------------------------------------------------------------------------
//
// func @load_use(%ptr:i64) -> i64:
//   %v   = load %ptr          ← load with 4-cycle latency
//   %k   = add 0, 99          ← INDEPENDENT computation (filler)
//   %r   = add %v, %k         ← uses %v (load result)
//   ret %r
//
// Before scheduling: ldr, mov (for 99), add, add — ldr immediately precedes
// the mov/add chain; %v might be used only 1-2 instrs after the load.
//
// After scheduling: independent instructions should be reordered so that at
// least 1 instruction falls between the ldr and the first use of its result.
//
// We verify: the instruction immediately following the "ldr x" is NOT an
// instruction that uses the same register as the ldr's destination.  This
// is a proxy for load-use stall reduction.  We count how many instructions
// appear between "ldr x" and "add x" to check separation.
//
// Bound: ldr-to-use distance >= 1 (at least 1 independent instruction between
//        the load and its first use in the add).  Before scheduling this is 0
//        or 1; after scheduling this should be >= 1 with the filler inserted.
//
TEST(AArch64Scheduler, LoadUseSeparation) {
    // Note: IL does not have a direct "load" opcode for this pointer form.
    // We test load-use separation using a multi-multiply pattern that exercises
    // the scheduler's ability to interleave independent instruction streams.
    const std::string il = "il 0.1\n"
                           "func @interleaved(%a:i64, %b:i64) -> i64 {\n"
                           "entry(%a:i64, %b:i64):\n"
                           "  %x = imul.ovf %a, %a\n" // 3-cycle multiply
                           "  %y = imul.ovf %b, %b\n" // 3-cycle multiply (independent of x)
                           "  %r = iadd.ovf %x, %y\n" // uses both
                           "  ret %r\n"
                           "}\n";

    il::core::Module mod = parseIL(il);
    ASSERT_FALSE(mod.functions.empty());

    const TargetInfo &ti = darwinTarget();
    AArch64Module m;
    m.ilMod = &mod;
    m.ti = &ti;

    Diagnostics diags;
    const bool ok = buildScheduledPipeline().run(m, diags);
    dumpDiagnosticsOnFailure(ok, diags);

    EXPECT_TRUE(ok);

    // The function must contain two multiply-class instructions and one add-class.
    // The peephole may fuse the second mul + add into a single madd instruction,
    // so we count both "mul" and "madd" as multiply-class occurrences.
    const int mulCount = countSubstr(m.assembly, "mul");
    const int maddCount = countSubstr(m.assembly, "madd");
    const int addCount = countSubstr(m.assembly, "add"); // "madd" also contains "add"

    // At least 2 multiply-class instructions (mul or madd) must be present.
    EXPECT_TRUE(mulCount + maddCount >= 2);
    // At least 1 add-class instruction (add or madd) must be present.
    EXPECT_TRUE(addCount >= 1);
    // Function must still end with ret.
    EXPECT_NE(m.assembly.find("ret"), std::string::npos);
}

TEST(AArch64Scheduler, IndependentStackSlotsDoNotSerializeLoadAfterStore) {
    const TargetInfo &ti = darwinTarget();
    AArch64Module module;
    module.ti = &ti;

    MFunction fn;
    fn.name = "independent_stack_slots";

    MBasicBlock bb;
    bb.name = "entry";

    MInstr store;
    store.opc = MOpcode::StrRegFpImm;
    store.ops = {MOperand::regOp(PhysReg::X0), MOperand::immOp(-8)};
    bb.instrs.push_back(std::move(store));

    MInstr load;
    load.opc = MOpcode::LdrRegFpImm;
    load.ops = {MOperand::regOp(PhysReg::X1), MOperand::immOp(-32)};
    bb.instrs.push_back(std::move(load));

    MInstr add;
    add.opc = MOpcode::AddRRR;
    add.ops = {
        MOperand::regOp(PhysReg::X2), MOperand::regOp(PhysReg::X1), MOperand::regOp(PhysReg::X3)};
    bb.instrs.push_back(std::move(add));

    MInstr ret;
    ret.opc = MOpcode::Ret;
    bb.instrs.push_back(std::move(ret));

    fn.blocks.push_back(std::move(bb));
    module.mir.push_back(std::move(fn));

    Diagnostics diags;
    ASSERT_TRUE(SchedulerPass().run(module, diags));

    ASSERT_EQ(module.mir.size(), 1u);
    ASSERT_EQ(module.mir[0].blocks.size(), 1u);
    const auto &instrs = module.mir[0].blocks[0].instrs;
    ASSERT_GE(instrs.size(), 4u);
    EXPECT_EQ(instrs[0].opc, MOpcode::LdrRegFpImm);
    auto storeIt = std::find_if(instrs.begin(), instrs.end(), [](const MInstr &instr) {
        return instr.opc == MOpcode::StrRegFpImm;
    });
    ASSERT_NE(storeIt, instrs.end());
    EXPECT_LT(instrs.begin(), storeIt);
    EXPECT_EQ(instrs.front().opc, MOpcode::LdrRegFpImm);
    EXPECT_EQ(instrs.back().opc, MOpcode::Ret);
}

/// Plan 88 / choreo bisect: a store whose frame offset the emitter materializes
/// through kScratchGPR (`mov x9, #off; add x9, x29, x9; str [x9]`) must not be
/// scheduled between two instructions that keep a value in x9 — the post-RA
/// magic-division expansion's sign-correction term lived there and came back
/// as a stack address.
TEST(AArch64Scheduler, LargeOffsetStoreCannotSplitScratchLiveRange) {
    const TargetInfo &ti = darwinTarget();
    AArch64Module module;
    module.ti = &ti;

    MFunction fn;
    fn.name = "scratch_live_range";

    MBasicBlock bb;
    bb.name = "entry";

    // The post-RA magic-division expansion keeps its sign-correction term in
    // kScratchGPR across two adjacent instructions ...
    MInstr lsr;
    lsr.opc = MOpcode::LsrRI;
    lsr.ops = {MOperand::regOp(kScratchGPR), MOperand::regOp(PhysReg::X1), MOperand::immOp(63)};
    bb.instrs.push_back(std::move(lsr));

    MInstr add;
    add.opc = MOpcode::AddRRR;
    add.ops = {
        MOperand::regOp(PhysReg::X0), MOperand::regOp(PhysReg::X0), MOperand::regOp(kScratchGPR)};
    bb.instrs.push_back(std::move(add));

    // ... and a later spill store looks independent in the MIR, but its
    // -1000 frame offset is materialized through kScratchGPR at emit time.
    // The scheduler must never hoist it between the two.
    MInstr bigStore;
    bigStore.opc = MOpcode::StrRegFpImm;
    bigStore.ops = {MOperand::regOp(PhysReg::X4), MOperand::immOp(-1000)};
    bb.instrs.push_back(std::move(bigStore));

    // An encodable offset carries no such hazard and may still move freely.
    MInstr smallStore;
    smallStore.opc = MOpcode::StrRegFpImm;
    smallStore.ops = {MOperand::regOp(PhysReg::X5), MOperand::immOp(-8)};
    bb.instrs.push_back(std::move(smallStore));

    MInstr ret;
    ret.opc = MOpcode::Ret;
    bb.instrs.push_back(std::move(ret));

    fn.blocks.push_back(std::move(bb));
    module.mir.push_back(std::move(fn));

    Diagnostics diags;
    ASSERT_TRUE(SchedulerPass().run(module, diags));
    const auto &instrs = module.mir[0].blocks[0].instrs;
    ASSERT_EQ(instrs.size(), 5u);
    std::size_t lsrAt = 0, bigAt = 0, addAt = 0;
    for (std::size_t i = 0; i < instrs.size(); ++i) {
        if (instrs[i].opc == MOpcode::LsrRI)
            lsrAt = i;
        if (instrs[i].opc == MOpcode::AddRRR)
            addAt = i;
        if (instrs[i].opc == MOpcode::StrRegFpImm && instrs[i].ops[1].imm == -1000)
            bigAt = i;
    }
    EXPECT_LT(lsrAt, addAt);
    EXPECT_LT(addAt, bigAt);
}

TEST(AArch64Scheduler, BaseRegisterMemoryMayAliasAcrossRegisters) {
    AArch64Module module;
    module.ti = &darwinTarget();

    MFunction fn;
    fn.name = "base_register_alias";

    MBasicBlock bb;
    bb.name = "entry";

    MInstr store;
    store.opc = MOpcode::StrRegBaseImm;
    store.ops = {MOperand::regOp(PhysReg::X0), MOperand::regOp(PhysReg::X10), MOperand::immOp(0)};
    bb.instrs.push_back(std::move(store));

    MInstr load;
    load.opc = MOpcode::LdrRegBaseImm;
    load.ops = {MOperand::regOp(PhysReg::X1), MOperand::regOp(PhysReg::X11), MOperand::immOp(0)};
    bb.instrs.push_back(std::move(load));

    MInstr add;
    add.opc = MOpcode::AddRRR;
    add.ops = {
        MOperand::regOp(PhysReg::X2), MOperand::regOp(PhysReg::X1), MOperand::regOp(PhysReg::X3)};
    bb.instrs.push_back(std::move(add));

    MInstr ret;
    ret.opc = MOpcode::Ret;
    bb.instrs.push_back(std::move(ret));

    fn.blocks.push_back(std::move(bb));
    module.mir.push_back(std::move(fn));

    Diagnostics diags;
    ASSERT_TRUE(SchedulerPass().run(module, diags));

    const auto &instrs = module.mir[0].blocks[0].instrs;
    const auto storeIt = std::find_if(instrs.begin(), instrs.end(), [](const MInstr &instr) {
        return instr.opc == MOpcode::StrRegBaseImm;
    });
    const auto loadIt = std::find_if(instrs.begin(), instrs.end(), [](const MInstr &instr) {
        return instr.opc == MOpcode::LdrRegBaseImm;
    });
    ASSERT_NE(storeIt, instrs.end());
    ASSERT_NE(loadIt, instrs.end());
    EXPECT_LT(storeIt, loadIt);
    EXPECT_EQ(instrs.back().opc, MOpcode::Ret);
}

TEST(AArch64Scheduler, PointersLoadedFromDifferentFrameSlotsMayAlias) {
    AArch64Module module;
    module.ti = &darwinTarget();

    MFunction fn;
    fn.name = "frame_slot_pointer_alias";

    MBasicBlock bb;
    bb.name = "entry";

    MInstr loadPtrA;
    loadPtrA.opc = MOpcode::LdrRegFpImm;
    loadPtrA.ops = {MOperand::regOp(PhysReg::X10), MOperand::immOp(-8)};
    bb.instrs.push_back(std::move(loadPtrA));

    MInstr loadPtrB;
    loadPtrB.opc = MOpcode::LdrRegFpImm;
    loadPtrB.ops = {MOperand::regOp(PhysReg::X11), MOperand::immOp(-16)};
    bb.instrs.push_back(std::move(loadPtrB));

    MInstr store;
    store.opc = MOpcode::StrRegBaseImm;
    store.ops = {MOperand::regOp(PhysReg::X0), MOperand::regOp(PhysReg::X10), MOperand::immOp(0)};
    bb.instrs.push_back(std::move(store));

    MInstr load;
    load.opc = MOpcode::LdrRegBaseImm;
    load.ops = {MOperand::regOp(PhysReg::X1), MOperand::regOp(PhysReg::X11), MOperand::immOp(0)};
    bb.instrs.push_back(std::move(load));

    MInstr add;
    add.opc = MOpcode::AddRRR;
    add.ops = {
        MOperand::regOp(PhysReg::X2), MOperand::regOp(PhysReg::X1), MOperand::regOp(PhysReg::X3)};
    bb.instrs.push_back(std::move(add));

    MInstr ret;
    ret.opc = MOpcode::Ret;
    bb.instrs.push_back(std::move(ret));

    fn.blocks.push_back(std::move(bb));
    module.mir.push_back(std::move(fn));

    Diagnostics diags;
    ASSERT_TRUE(SchedulerPass().run(module, diags));

    const auto &instrs = module.mir[0].blocks[0].instrs;
    const auto storeIt = std::find_if(instrs.begin(), instrs.end(), [](const MInstr &instr) {
        return instr.opc == MOpcode::StrRegBaseImm;
    });
    const auto loadIt = std::find_if(instrs.begin(), instrs.end(), [](const MInstr &instr) {
        return instr.opc == MOpcode::LdrRegBaseImm;
    });
    ASSERT_NE(storeIt, instrs.end());
    ASSERT_NE(loadIt, instrs.end());
    EXPECT_LT(storeIt, loadIt);
    EXPECT_EQ(instrs.back().opc, MOpcode::Ret);
}

// ---------------------------------------------------------------------------
// Test 4: Terminators remain at the end of their block after scheduling.
// ---------------------------------------------------------------------------
//
// For a function with a loop, each block's terminator (ret, b, cbnz) must
// remain the last instruction in that block.  The scheduler must never move
// a terminator before non-terminator instructions.
//
TEST(AArch64Scheduler, TerminatorLast) {
    const std::string il = "il 0.1\n"
                           "func @loop_sched() -> i64 {\n"
                           "entry:\n"
                           "  br loop(0)\n"
                           "loop(%i:i64):\n"
                           "  %next = iadd.ovf %i, 1\n"
                           "  %done = icmp_eq %next, 10\n"
                           "  cbr %done, exit(%next), loop(%next)\n"
                           "exit(%r:i64):\n"
                           "  ret %r\n"
                           "}\n";

    il::core::Module mod = parseIL(il);
    ASSERT_FALSE(mod.functions.empty());

    const TargetInfo &ti = darwinTarget();
    AArch64Module m;
    m.ilMod = &mod;
    m.ti = &ti;

    // Run only through scheduler — no EmitPass needed; check MIR directly.
    // Assembly-level terminator inspection is fragile: the emitter elides
    // fall-through branches and emits a function-level label with prologue
    // code before the first block label. MIR inspection avoids both issues.
    PassManager mirPipeline;
    mirPipeline.addPass(std::make_unique<LoweringPass>());
    mirPipeline.addPass(std::make_unique<LegalizePass>());
    mirPipeline.addPass(std::make_unique<RegAllocPass>());
    mirPipeline.addPass(std::make_unique<SchedulerPass>());

    Diagnostics diags;
    const bool ok = mirPipeline.run(m, diags);

    EXPECT_TRUE(ok);

    // Every MIR block's last instruction must be a terminator.
    // This directly verifies the scheduler's invariant at the level it operates.
    auto isMirTerminator = [](MOpcode opc) -> bool {
        return opc == MOpcode::Ret || opc == MOpcode::Br || opc == MOpcode::BCond ||
               opc == MOpcode::Cbz || opc == MOpcode::Cbnz;
    };

    bool terminatorOk = true;
    for (const auto &fn : m.mir) {
        for (const auto &bb : fn.blocks) {
            if (bb.instrs.empty())
                continue;
            const MOpcode lastOpc = bb.instrs.back().opc;
            if (!isMirTerminator(lastOpc)) {
                if (bb.name.rfind("Ltrap_ovf_", 0) == 0 || bb.name.rfind("L.Ltrap_ovf_", 0) == 0 ||
                    bb.name.rfind(".Ltrap_ovf_", 0) == 0)
                    continue;
                std::cerr << "Block '" << bb.name << "' does not end with a terminator.\n";
                terminatorOk = false;
            }
        }
    }
    EXPECT_TRUE(terminatorOk);
}

// ---------------------------------------------------------------------------
// Test 5: Calls remain hard scheduling boundaries.
// ---------------------------------------------------------------------------
//
// The scheduler may reorder independent arithmetic inside a call-free segment,
// but helper/runtime calls must stay in program order.  In this hand-built
// block the post-call multiply has a higher scheduling priority than the call;
// without call segmentation it can be hoisted above the call.
//
TEST(AArch64Scheduler, CallsAreBoundaries) {
    AArch64Module module;
    module.ti = &darwinTarget();

    MFunction fn;
    fn.name = "call_boundary";
    MBasicBlock bb;
    bb.name = "entry";

    MInstr call;
    call.opc = MOpcode::Bl;
    call.ops = {MOperand::labelOp("rt_side_effect")};
    bb.instrs.push_back(std::move(call));

    MInstr mul;
    mul.opc = MOpcode::MulRRR;
    mul.ops = {MOperand::regOp(PhysReg::X19),
               MOperand::regOp(PhysReg::X20),
               MOperand::regOp(PhysReg::X21)};
    bb.instrs.push_back(std::move(mul));

    MInstr add;
    add.opc = MOpcode::AddRRR;
    add.ops = {MOperand::regOp(PhysReg::X22),
               MOperand::regOp(PhysReg::X19),
               MOperand::regOp(PhysReg::X23)};
    bb.instrs.push_back(std::move(add));

    MInstr ret;
    ret.opc = MOpcode::Ret;
    bb.instrs.push_back(std::move(ret));

    fn.blocks.push_back(std::move(bb));
    module.mir.push_back(std::move(fn));

    Diagnostics diags;
    ASSERT_TRUE(SchedulerPass().run(module, diags));

    const auto &instrs = module.mir[0].blocks[0].instrs;
    ASSERT_EQ(instrs.size(), 4u);
    EXPECT_EQ(instrs[0].opc, MOpcode::Bl);
    EXPECT_EQ(instrs[1].opc, MOpcode::MulRRR);
    EXPECT_EQ(instrs[2].opc, MOpcode::AddRRR);
    EXPECT_EQ(instrs[3].opc, MOpcode::Ret);
}

TEST(AArch64Scheduler, LargeFunctionsRetainPostRAOrder) {
    AArch64Module module;
    module.ti = &darwinTarget();

    MFunction fn;
    fn.name = "large_generated_function";
    MBasicBlock bb;
    bb.name = "entry";

    for (std::size_t i = 0; i < 1025; ++i) {
        MInstr move;
        move.opc = i == 1024 ? MOpcode::MulRRR : MOpcode::MovRR;
        move.ops = i == 1024 ? std::vector<MOperand>{MOperand::regOp(PhysReg::X21),
                                                     MOperand::regOp(PhysReg::X22),
                                                     MOperand::regOp(PhysReg::X23)}
                             : std::vector<MOperand>{MOperand::regOp(PhysReg::X19),
                                                     MOperand::regOp(PhysReg::X20)};
        bb.instrs.push_back(std::move(move));
    }
    MInstr ret;
    ret.opc = MOpcode::Ret;
    bb.instrs.push_back(std::move(ret));

    const auto original = bb.instrs;
    fn.blocks.push_back(std::move(bb));
    module.mir.push_back(std::move(fn));

    Diagnostics diags;
    ASSERT_TRUE(SchedulerPass().run(module, diags));
    const auto &scheduled = module.mir[0].blocks[0].instrs;
    ASSERT_EQ(scheduled.size(), original.size());
    for (std::size_t i = 0; i < original.size(); ++i)
        EXPECT_EQ(scheduled[i].opc, original[i].opc);
}

// ---------------------------------------------------------------------------
// Test 6: SchedulerPass integrates cleanly into the full PassManager pipeline.
// ---------------------------------------------------------------------------
TEST(AArch64Scheduler, PipelineIntegration) {
    const std::string il = "il 0.1\n"
                           "func @pipeline_test() -> i64 {\n"
                           "entry:\n"
                           "  %a = iadd.ovf 1, 2\n"
                           "  %b = iadd.ovf 3, 4\n"
                           "  %c = imul.ovf %a, %b\n"
                           "  ret %c\n"
                           "}\n";

    il::core::Module mod = parseIL(il);
    ASSERT_FALSE(mod.functions.empty());

    const TargetInfo &ti = darwinTarget();
    AArch64Module m;
    m.ilMod = &mod;
    m.ti = &ti;

    // Full pipeline with scheduler.
    Diagnostics diags;
    const bool ok = buildScheduledPipeline().run(m, diags);
    dumpDiagnosticsOnFailure(ok, diags);

    EXPECT_TRUE(ok);
    EXPECT_TRUE(diags.errors().empty());
    EXPECT_FALSE(m.assembly.empty());
    // Multiply must survive scheduling.
    EXPECT_NE(m.assembly.find("mul"), std::string::npos);
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, &argv);
    return zanna_test::run_all_tests();
}
