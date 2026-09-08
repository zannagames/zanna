//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/codegen/test_aarch64_pass_manager.cpp
// Purpose: Verify the AArch64 modular PassManager pipeline (Priority 2F).
//
// Background:
//   The AArch64 backend previously had a monolithic pipeline embedded in
//   cmd_codegen_arm64.cpp.  Priority 2F extracts the per-phase logic into
//   formal Pass subclasses registered with the common PassManager<AArch64Module>
//   template, matching the architecture already used by the x86_64 backend.
//
// What is verified:
//   1. PipelineRoundtrip   — Full pass sequence
//                            (Lower → RegAlloc → Scheduler → BlockLayout → Peephole → Emit)
//                            produces correct assembly for a simple function.
//   2. PartialPipeline     — Running only LoweringPass populates mir
//                            but leaves assembly empty.
//   3. FailPassShortCircuit — A pass that signals failure stops subsequent passes.
//   4. EmptyModule         — PassManager on an empty IL module succeeds with no output.
//
//===----------------------------------------------------------------------===//

#include "tests/TestHarness.hpp"
#include <sstream>
#include <string>
#include <utility>

#include "codegen/aarch64/CodegenPipeline.hpp"
#include "codegen/aarch64/TargetAArch64.hpp"
#include "codegen/aarch64/passes/BlockLayoutPass.hpp"
#include "codegen/aarch64/passes/EmitPass.hpp"
#include "codegen/aarch64/passes/LegalizePass.hpp"
#include "codegen/aarch64/passes/LoweringPass.hpp"
#include "codegen/aarch64/passes/PassManager.hpp"
#include "codegen/aarch64/passes/PeepholePass.hpp"
#include "codegen/aarch64/passes/RegAllocPass.hpp"
#include "codegen/aarch64/passes/SchedulerPass.hpp"
#include "codegen/aarch64/ra/Liveness.hpp"
#include "il/io/Parser.hpp"
#include <stdexcept>

using namespace zanna::codegen::aarch64;
using namespace zanna::codegen::aarch64::passes;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

/// Parse an IL text string into an il::core::Module.
/// Returns a default-constructed Module on parse failure.
static il::core::Module parseIL(const std::string &src) {
    std::istringstream ss(src);
    il::core::Module mod;
    if (!il::io::Parser::parse(ss, mod))
        return {};
    return mod;
}

/// Build a PassManager with the standard AArch64 full pipeline.
static PassManager buildFullPipeline() {
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

} // namespace

// ---------------------------------------------------------------------------
// Test 1: Full pipeline roundtrip — simple constant-return function.
// ---------------------------------------------------------------------------
//
// func @forty_two() -> i64 { entry: ret 42 }
//
// The full pipeline should succeed and produce assembly containing:
//   - A function label (_forty_two on Darwin)
//   - A move immediate (mov x0, #42 or similar)
//   - A ret instruction
//
TEST(AArch64PassManager, PipelineRoundtrip) {
    const std::string il = "il 0.1\n"
                           "func @forty_two() -> i64 {\n"
                           "entry:\n"
                           "  ret 42\n"
                           "}\n";

    il::core::Module mod = parseIL(il);
    ASSERT_FALSE(mod.functions.empty());

    const TargetInfo &ti = darwinTarget();
    AArch64Module m;
    m.ilMod = &mod;
    m.ti = &ti;

    PassManager pm = buildFullPipeline();
    Diagnostics diags;
    const bool ok = pm.run(m, diags);

    EXPECT_TRUE(ok);
    EXPECT_TRUE(diags.errors().empty());

    // Assembly must be non-empty and contain the function label.
    EXPECT_FALSE(m.assembly.empty());
    EXPECT_NE(m.assembly.find("forty_two"), std::string::npos);
    // Must contain a return instruction.
    EXPECT_NE(m.assembly.find("ret"), std::string::npos);
    // Must contain a move-immediate for the constant 42.
    const bool hasImm = m.assembly.find("42") != std::string::npos ||
                        m.assembly.find("#42") != std::string::npos ||
                        m.assembly.find("0x2a") != std::string::npos;
    EXPECT_TRUE(hasImm);
}

// ---------------------------------------------------------------------------
// Test 2: Partial pipeline — LoweringPass only; mir populated, assembly empty.
// ---------------------------------------------------------------------------
//
// Running only the LoweringPass should populate mir but not assembly.
//
TEST(AArch64PassManager, PartialPipeline) {
    const std::string il = "il 0.1\n"
                           "func @add_two(%a:i64, %b:i64) -> i64 {\n"
                           "entry:\n"
                           "  %r = iadd.ovf %a, %b\n"
                           "  ret %r\n"
                           "}\n";

    il::core::Module mod = parseIL(il);
    ASSERT_FALSE(mod.functions.empty());

    const TargetInfo &ti = darwinTarget();
    AArch64Module m;
    m.ilMod = &mod;
    m.ti = &ti;

    // Only add the lowering pass.
    PassManager pm;
    pm.addPass(std::make_unique<LoweringPass>());

    Diagnostics diags;
    const bool ok = pm.run(m, diags);

    EXPECT_TRUE(ok);
    // MIR should be populated (one function).
    EXPECT_EQ(m.mir.size(), 1u);
    // Assembly should not have been emitted yet.
    EXPECT_TRUE(m.assembly.empty());
}

TEST(AArch64PassManager, LegalizePassExpandsOverflowPseudos) {
    const std::string il = "il 0.1\n"
                           "func @checked_add(%a:i64, %b:i64) -> i64 {\n"
                           "entry(%a:i64, %b:i64):\n"
                           "  %r = iadd.ovf %a, %b\n"
                           "  ret %r\n"
                           "}\n";

    il::core::Module mod = parseIL(il);
    ASSERT_FALSE(mod.functions.empty());

    const TargetInfo &ti = darwinTarget();
    AArch64Module m;
    m.ilMod = &mod;
    m.ti = &ti;

    PassManager pm;
    pm.addPass(std::make_unique<LoweringPass>());
    pm.addPass(std::make_unique<LegalizePass>());

    Diagnostics diags;
    ASSERT_TRUE(pm.run(m, diags));
    ASSERT_EQ(m.mir.size(), 1u);

    bool sawPseudo = false;
    bool sawGuard = false;
    bool sawTrapCall = false;
    for (const auto &bb : m.mir[0].blocks) {
        for (const auto &instr : bb.instrs) {
            sawPseudo =
                sawPseudo || instr.opc == MOpcode::AddOvfRRR || instr.opc == MOpcode::AddOvfRI;
            sawGuard = sawGuard || instr.opc == MOpcode::BCond;
            sawTrapCall = sawTrapCall || instr.opc == MOpcode::Bl;
        }
    }

    EXPECT_FALSE(sawPseudo);
    EXPECT_TRUE(sawGuard);
    EXPECT_TRUE(sawTrapCall);
}

TEST(AArch64PassManager, LegalizePassInsertsMainRuntimeInitOnce) {
    AArch64Module m;
    m.ti = &darwinTarget();

    MFunction fn;
    fn.name = "main";
    MBasicBlock entry;
    entry.name = "entry";
    entry.instrs.push_back(MInstr{MOpcode::Ret, {}});
    fn.blocks.push_back(std::move(entry));
    m.mir.push_back(std::move(fn));

    LegalizePass pass;
    Diagnostics diags;
    ASSERT_TRUE(pass.run(m, diags));
    ASSERT_TRUE(pass.run(m, diags));

    ASSERT_EQ(m.mir.size(), 1u);
    ASSERT_EQ(m.mir[0].blocks.size(), 1u);
    const auto &instrs = m.mir[0].blocks[0].instrs;
    ASSERT_GE(instrs.size(), 3u);
    ASSERT_EQ(instrs[0].opc, MOpcode::Bl);
    ASSERT_EQ(instrs[1].opc, MOpcode::Bl);
    ASSERT_FALSE(m.mir[0].isLeaf);
    ASSERT_FALSE(instrs[0].ops.empty());
    ASSERT_FALSE(instrs[1].ops.empty());
    EXPECT_EQ(instrs[0].ops[0].label, "rt_legacy_context");
    EXPECT_EQ(instrs[1].ops[0].label, "rt_set_current_context");

    std::size_t initCalls = 0;
    for (const auto &instr : instrs) {
        if (instr.opc == MOpcode::Bl && !instr.ops.empty() &&
            instr.ops[0].kind == MOperand::Kind::Label &&
            (instr.ops[0].label == "rt_legacy_context" ||
             instr.ops[0].label == "rt_set_current_context")) {
            ++initCalls;
        }
    }
    EXPECT_EQ(initCalls, 2u);
}

// ---------------------------------------------------------------------------
// Test 3: A failing pass stops subsequent passes.
// ---------------------------------------------------------------------------
//
// A pass that returns false should prevent later passes from running.
// We verify this by checking that assembly remains empty when RA fails.
//
namespace {

/// Stub pass that always fails without modifying module state.
class AlwaysFailPass final : public Pass {
  public:
    bool run(AArch64Module & /*module*/, Diagnostics & /*diags*/) override {
        return false;
    }
};

class ThrowingPass final : public Pass {
  public:
    bool run(AArch64Module &, Diagnostics &) override {
        throw std::runtime_error("boom");
    }
};

} // namespace

TEST(AArch64PassManager, FailPassShortCircuit) {
    const std::string il = "il 0.1\n"
                           "func @simple() -> i64 {\n"
                           "entry:\n"
                           "  ret 0\n"
                           "}\n";

    il::core::Module mod = parseIL(il);
    ASSERT_FALSE(mod.functions.empty());

    const TargetInfo &ti = darwinTarget();
    AArch64Module m;
    m.ilMod = &mod;
    m.ti = &ti;

    // Pipeline: Lower → FAIL → (Peephole should NOT run) → (Emit should NOT run).
    PassManager pm;
    pm.addPass(std::make_unique<LoweringPass>());
    pm.addPass(std::make_unique<LegalizePass>());
    pm.addPass(std::make_unique<AlwaysFailPass>());
    pm.addPass(std::make_unique<PeepholePass>());
    pm.addPass(std::make_unique<EmitPass>());

    Diagnostics diags;
    const bool ok = pm.run(m, diags);

    // PassManager should have reported failure.
    EXPECT_FALSE(ok);
    // EmitPass should not have run — assembly must be empty.
    EXPECT_TRUE(m.assembly.empty());
    // MIR may or may not be populated (LoweringPass ran before the failure).
}

TEST(AArch64PassManager, ThrowingPassBecomesDiagnostic) {
    const std::string il = "il 0.1\n"
                           "func @simple() -> i64 {\n"
                           "entry:\n"
                           "  ret 0\n"
                           "}\n";

    il::core::Module mod = parseIL(il);
    ASSERT_FALSE(mod.functions.empty());

    const TargetInfo &ti = darwinTarget();
    AArch64Module m;
    m.ilMod = &mod;
    m.ti = &ti;

    PassManager pm;
    pm.addPass(std::make_unique<ThrowingPass>());

    Diagnostics diags;
    const bool ok = pm.run(m, diags);

    EXPECT_FALSE(ok);
    ASSERT_FALSE(diags.diagnostics().empty());
    EXPECT_EQ(diags.diagnostics().front().code, "V-CG-PASS-EXCEPTION");
    EXPECT_NE(diags.errors().front().find("boom"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Test 4: Empty IL module — pipeline succeeds with no MIR and no output.
// ---------------------------------------------------------------------------
TEST(AArch64PassManager, EmptyModule) {
    const std::string il = "il 0.1\n";

    il::core::Module mod = parseIL(il);
    // Empty module has no functions.
    EXPECT_TRUE(mod.functions.empty());

    const TargetInfo &ti = darwinTarget();
    AArch64Module m;
    m.ilMod = &mod;
    m.ti = &ti;

    PassManager pm = buildFullPipeline();
    Diagnostics diags;
    const bool ok = pm.run(m, diags);

    EXPECT_TRUE(ok);
    EXPECT_TRUE(m.mir.empty());
}

TEST(AArch64PassManager, NativeOnlyPipelineSkipsTextEmission) {
    const std::string il = "il 0.1\n"
                           "func @forty_two() -> i64 {\n"
                           "entry:\n"
                           "  ret 42\n"
                           "}\n";

    il::core::Module mod = parseIL(il);
    ASSERT_FALSE(mod.functions.empty());

    const TargetInfo &ti = darwinTarget();
    AArch64Module m;
    m.ilMod = &mod;
    m.ti = &ti;

    PipelineOptions opts;
    opts.emitAssemblyText = false;
    opts.useBinaryEmit = true;

    std::ostringstream diag;
    const bool ok = runCodegenPipeline(m, opts, diag);

    EXPECT_TRUE(ok);
    EXPECT_TRUE(m.assembly.empty());
    EXPECT_TRUE(m.binaryText.has_value());
}

TEST(AArch64PassManager, ConditionalBranchLivenessTracksFallthroughSuccessor) {
    MFunction fn;
    fn.name = "fallthrough_live";

    MBasicBlock entry;
    entry.name = "entry";
    entry.instrs.push_back(
        MInstr{MOpcode::BCond, {MOperand::condOp("ne"), MOperand::labelOp("then")}});

    MBasicBlock elseBlock;
    elseBlock.name = "else";
    elseBlock.instrs.push_back(MInstr{MOpcode::AddRRR,
                                      {MOperand::vregOp(RegClass::GPR, 2),
                                       MOperand::vregOp(RegClass::GPR, 1),
                                       MOperand::vregOp(RegClass::GPR, 1)}});
    elseBlock.instrs.push_back(MInstr{MOpcode::Br, {MOperand::labelOp("exit")}});

    MBasicBlock thenBlock;
    thenBlock.name = "then";
    thenBlock.instrs.push_back(MInstr{MOpcode::Br, {MOperand::labelOp("exit")}});

    MBasicBlock exitBlock;
    exitBlock.name = "exit";
    exitBlock.instrs.push_back(MInstr{MOpcode::Ret, {}});

    fn.blocks.push_back(std::move(entry));
    fn.blocks.push_back(std::move(elseBlock));
    fn.blocks.push_back(std::move(thenBlock));
    fn.blocks.push_back(std::move(exitBlock));

    ra::LivenessAnalysis liveness;
    liveness.run(fn);

    EXPECT_TRUE(liveness.liveOutGPR(0).contains(1));
}

TEST(AArch64PassManager, RegAllocPoolExhaustionBecomesDiagnostic) {
    TargetInfo target = darwinTarget();
    target.callerSavedGPR.clear();
    target.calleeSavedGPR.clear();
    target.callerSavedFPR.clear();
    target.calleeSavedFPR.clear();

    MFunction fn;
    fn.name = "reg_pressure";

    // With no allocatable register every value is spilled, and a spilled
    // operand is served from the reserved scratch (x9, x16, x17). Three
    // spilled sources plus an explicit x9 destination need one scratch more
    // than exists: the allocator must fail loudly, not miscompile.
    const auto v = [](uint16_t id) { return MOperand::vregOp(RegClass::GPR, id); };
    MBasicBlock entry;
    entry.name = "entry";
    entry.instrs.push_back(MInstr{MOpcode::MovRI, {v(1), MOperand::immOp(1)}});
    entry.instrs.push_back(MInstr{MOpcode::MovRI, {v(2), MOperand::immOp(2)}});
    entry.instrs.push_back(MInstr{MOpcode::MovRI, {v(3), MOperand::immOp(3)}});
    entry.instrs.push_back(
        MInstr{MOpcode::MAddRRRR, {MOperand::regOp(PhysReg::X9), v(1), v(2), v(3)}});
    entry.instrs.push_back(MInstr{MOpcode::Ret, {}});
    fn.blocks.push_back(std::move(entry));

    AArch64Module m;
    m.ti = &target;
    m.mir.push_back(std::move(fn));

    RegAllocPass pass;
    Diagnostics diags;
    const bool ok = pass.run(m, diags);

    EXPECT_FALSE(ok);
    ASSERT_TRUE(diags.hasErrors());
    ASSERT_FALSE(diags.diagnostics().empty());
    EXPECT_EQ(diags.diagnostics().front().code, "V-CG-AARCH64-REGALLOC");
    EXPECT_NE(diags.errors().front().find(
                  "AArch64 register allocation failed for function 'reg_pressure'"),
              std::string::npos);
    EXPECT_NE(diags.errors().front().find("scratch exhausted"), std::string::npos);
}

TEST(AArch64PassManager, PeepholeRejectsRemainingVirtualRegisters) {
    const TargetInfo &ti = darwinTarget();
    AArch64Module m;
    m.ti = &ti;

    MFunction fn;
    fn.name = "bad_vreg";
    MBasicBlock entry;
    entry.name = "entry";
    entry.instrs.push_back(
        MInstr{MOpcode::MovRR, {MOperand::vregOp(RegClass::GPR, 1), MOperand::regOp(PhysReg::X0)}});
    entry.instrs.push_back(MInstr{MOpcode::Ret, {}});
    fn.blocks.push_back(std::move(entry));
    m.mir.push_back(std::move(fn));

    Diagnostics diags;
    PeepholePass pass;
    EXPECT_FALSE(pass.run(m, diags));
    EXPECT_TRUE(diags.hasErrors());
}

TEST(AArch64PassManager, PeepholeRejectsMissingBranchTarget) {
    const TargetInfo &ti = darwinTarget();
    AArch64Module m;
    m.ti = &ti;

    MFunction fn;
    fn.name = "bad_branch";
    MBasicBlock entry;
    entry.name = "entry";
    entry.instrs.push_back(MInstr{MOpcode::Br, {MOperand::labelOp("missing")}});
    fn.blocks.push_back(std::move(entry));
    m.mir.push_back(std::move(fn));

    Diagnostics diags;
    PeepholePass pass;
    EXPECT_FALSE(pass.run(m, diags));
    EXPECT_TRUE(diags.hasErrors());
}

TEST(AArch64PassManager, PeepholeRejectsInstructionAfterTerminator) {
    const TargetInfo &ti = darwinTarget();
    AArch64Module m;
    m.ti = &ti;

    MFunction fn;
    fn.name = "bad_terminator";
    MBasicBlock entry;
    entry.name = "entry";
    entry.instrs.push_back(MInstr{MOpcode::Ret, {}});
    entry.instrs.push_back(MInstr{MOpcode::Bl, {MOperand::labelOp("callee")}});
    fn.blocks.push_back(std::move(entry));
    m.mir.push_back(std::move(fn));

    Diagnostics diags;
    PeepholePass pass;
    EXPECT_FALSE(pass.run(m, diags));
    EXPECT_TRUE(diags.hasErrors());
}

TEST(AArch64PassManager, OptimizeLevelZeroSkipsBackendOptPasses) {
    const std::string il = "il 0.1\n"
                           "func @branch_only() -> i64 {\n"
                           "entry:\n"
                           "  br next\n"
                           "next:\n"
                           "  ret 0\n"
                           "}\n";

    il::core::Module mod = parseIL(il);
    ASSERT_FALSE(mod.functions.empty());

    const TargetInfo &ti = darwinTarget();
    AArch64Module m;
    m.ilMod = &mod;
    m.ti = &ti;

    PipelineOptions opts;
    opts.emitAssemblyText = true;
    opts.useBinaryEmit = false;
    opts.optimizeLevel = 0;

    std::ostringstream diag;
    const bool ok = runCodegenPipeline(m, opts, diag);

    EXPECT_TRUE(ok);
    EXPECT_NE(m.assembly.find("  b Lnext"), std::string::npos);
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, &argv);
    return zanna_test::run_all_tests();
}
