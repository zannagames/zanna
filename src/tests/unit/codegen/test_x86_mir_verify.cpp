//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/codegen/test_x86_mir_verify.cpp
// Purpose: Pins the x86-64 MIR verifier: every rule has a hand-built MIR
//          function that violates it and is rejected with the expected
//          `V-CG-MIR-*` code, well-formed MIR passes, and the PassManager
//          post-pass hook verifies real programs after every pipeline pass.
// Key invariants:
//   - A rule that stops firing on its failing case breaks this test before
//     it can hide a miscompile.
//   - The pipeline corpus runs with verification at -O0, -O1, and -O2 so the
//     verifier's rules hold on the MIR the optimizing passes actually produce.
// Ownership/Lifetime: Standalone test binary.
// Links: src/codegen/x86_64/MirVerify.hpp,
//        src/codegen/common/PassManager.hpp
//
//===----------------------------------------------------------------------===//

#include "tests/TestHarness.hpp"

#include "codegen/x86_64/MachineIR.hpp"
#include "codegen/x86_64/MirVerify.hpp"
#include "codegen/x86_64/TargetX64.hpp"
#include "codegen/x86_64/passes/LegalizePass.hpp"
#include "codegen/x86_64/passes/LoweringPass.hpp"
#include "codegen/x86_64/passes/PassManager.hpp"
#include "codegen/x86_64/passes/PeepholePass.hpp"
#include "codegen/x86_64/passes/PreRegAllocOptPass.hpp"
#include "codegen/x86_64/passes/RegAllocPass.hpp"
#include "codegen/x86_64/passes/SchedulerPass.hpp"
#include "il/io/Parser.hpp"

#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace zanna::codegen::x64;

namespace {

Operand gpr(PhysReg pr) {
    return OpReg{true, RegClass::GPR, static_cast<uint16_t>(pr)};
}

Operand imm(int64_t val) {
    return OpImm{val};
}

Operand mem(PhysReg base, int32_t disp) {
    return makeMemOperand(OpReg{true, RegClass::GPR, static_cast<uint16_t>(base)}, disp);
}

Operand lbl(const std::string &name) {
    return OpLabel{name};
}

MBasicBlock block(const std::string &name, std::vector<MInstr> instrs) {
    MBasicBlock bb;
    bb.label = name;
    bb.instructions = std::move(instrs);
    return bb;
}

MFunction singleBlock(std::vector<MInstr> instrs) {
    MFunction fn;
    fn.name = "f";
    fn.blocks.push_back(block("entry", std::move(instrs)));
    return fn;
}

bool hasCode(const passes::Diagnostics &diags, const std::string &code) {
    for (const auto &d : diags.diagnostics()) {
        if (d.code == code)
            return true;
    }
    return false;
}

bool rejectsWith(const MFunction &fn,
                 const FrameInfo &frame,
                 VerifyStage stage,
                 const std::string &code) {
    passes::Diagnostics diags;
    if (verifyMir(fn, frame, stage, sysvTarget(), diags))
        return false;
    return hasCode(diags, code);
}

bool rejectsWith(const MFunction &fn, VerifyStage stage, const std::string &code) {
    return rejectsWith(fn, FrameInfo{}, stage, code);
}

bool accepts(const MFunction &fn, const FrameInfo &frame, VerifyStage stage) {
    passes::Diagnostics diags;
    const bool ok = verifyMir(fn, frame, stage, sysvTarget(), diags);
    if (!ok) {
        for (const auto &e : diags.errors())
            std::cerr << e << "\n";
    }
    return ok;
}

il::core::Module parseIL(const std::string &src) {
    std::istringstream ss(src);
    il::core::Module mod;
    if (!il::io::Parser::parse(ss, mod))
        return {};
    return mod;
}

const char *const kCorpus = R"(il 0.3.0

func @step(%x: i64, %k: i64) -> i64 {
entry(%x: i64, %k: i64):
  %bit = and %k, 1
  %odd = icmp_ne %bit, 0
  cbr %odd, oddpath(%x, %k), evenpath(%x, %k)
oddpath(%x0: i64, %k0: i64):
  %t = imul.ovf %x0, 3
  %t1 = iadd.ovf %t, %k0
  br merge1(%t1, %k0)
evenpath(%x2: i64, %k2: i64):
  %t2 = shl %x2, 1
  %t3 = isub.ovf %t2, %k2
  br merge1(%t3, %k2)
merge1(%m: i64, %k3: i64):
  %q1 = sdiv.chk0 %m, 7
  %r1 = srem.chk0 %k3, 5
  %q2 = udiv.chk0 %m, 10
  %s = iadd.ovf %q1, %r1
  %s2 = iadd.ovf %s, %q2
  %r = and %s2, 1048575
  ret %r
}

func @fsum(%a: f64, %n: i64) -> f64 {
entry(%a: f64, %n: i64):
  br loop(%a, 0)
loop(%acc: f64, %i: i64):
  %done = scmp_ge %i, %n
  cbr %done, exit(%acc), body(%acc, %i)
body(%acc0: f64, %i0: i64):
  %fi = sitofp %i0
  %acc1 = fadd %acc0, %fi
  %acc2 = fmul %acc1, 1.5
  %i1 = iadd.ovf %i0, 1
  br loop(%acc2, %i1)
exit(%res: f64):
  ret %res
}

func @boom() -> void {
entry:
  trap.from_err i32 9
}

func @main() -> i64 {
entry:
  %slot = alloca 64
  store i64, %slot, 77
  br loop(0, 0)
loop(%sum: i64, %i: i64):
  %done = scmp_ge %i, 8
  cbr %done, exit(%sum), body(%sum, %i)
body(%sum0: i64, %i0: i64):
  %idx:i64 = idx.chk %i0, 0, 8
  %off = shl %idx, 3
  %p = gep %slot, %off
  store i64, %p, %i0
  %v = load i64, %p
  %s = call @step(%v, %i0)
  %f = call @fsum(1.0, %i0)
  %fi = fptosi %f
  %sum1 = iadd.ovf %sum0, %s
  %sum2 = iadd.ovf %sum1, %fi
  %i1 = iadd.ovf %i0, 1
  br loop(%sum2, %i1)
exit(%r: i64):
  %big = iadd.ovf %r, 1048576
  %m = and %big, 255
  ret %m
}
)";

} // namespace

// ---------------------------------------------------------------------------
// Well-formed MIR passes.
// ---------------------------------------------------------------------------

TEST(X86MirVerify, AcceptsWellFormedPostRaFunction) {
    // SysV prologue/epilogue as FrameLowering emits it, with a 16-byte frame
    // and one callee-saved register.
    FrameInfo frame;
    frame.frameSize = 16;
    frame.usedCalleeSaved = {PhysReg::RBX};

    MFunction fn;
    fn.name = "ok";
    fn.blocks.push_back(
        block("entry",
              {
                  MInstr::make(MOpcode::ADDri, {gpr(PhysReg::RSP), imm(-8)}),
                  MInstr::make(MOpcode::MOVrm, {mem(PhysReg::RSP, 0), gpr(PhysReg::RBP)}),
                  MInstr::make(MOpcode::MOVrr, {gpr(PhysReg::RBP), gpr(PhysReg::RSP)}),
                  MInstr::make(MOpcode::ADDri, {gpr(PhysReg::RSP), imm(-16)}),
                  MInstr::make(MOpcode::MOVrm, {mem(PhysReg::RBP, -8), gpr(PhysReg::RBX)}),
                  MInstr::make(MOpcode::MOVri, {gpr(PhysReg::RBX), imm(5)}),
                  MInstr::make(MOpcode::CMPri, {gpr(PhysReg::RDI), imm(0)}),
                  MInstr::make(MOpcode::JCC, {imm(4), lbl("done")}),
              }));
    fn.blocks.push_back(
        block("body",
              {
                  MInstr::make(MOpcode::MOVrm, {mem(PhysReg::RBP, -16), gpr(PhysReg::RBX)}),
                  MInstr::make(MOpcode::CALL, {lbl("callee")}),
                  MInstr::make(MOpcode::MOVmr, {gpr(PhysReg::RAX), mem(PhysReg::RBP, -16)}),
              }));
    fn.blocks.push_back(
        block("done",
              {
                  MInstr::make(MOpcode::MOVmr, {gpr(PhysReg::RBX), mem(PhysReg::RBP, -8)}),
                  MInstr::make(MOpcode::MOVrr, {gpr(PhysReg::RSP), gpr(PhysReg::RBP)}),
                  MInstr::make(MOpcode::MOVmr, {gpr(PhysReg::RBP), mem(PhysReg::RSP, 0)}),
                  MInstr::make(MOpcode::ADDri, {gpr(PhysReg::RSP), imm(8)}),
                  MInstr::make(MOpcode::RET, {}),
              }));

    for (VerifyStage stage : {VerifyStage::PostLegalize,
                              VerifyStage::PostRA,
                              VerifyStage::PostSchedule,
                              VerifyStage::PostPeephole}) {
        EXPECT_TRUE(accepts(fn, frame, stage));
    }
}

TEST(X86MirVerify, AcceptsVirtualRegistersBeforeAllocation) {
    MFunction fn = singleBlock({
        MInstr::make(MOpcode::MOVri, {makeVRegOperand(RegClass::GPR, 1), imm(1)}),
        MInstr::make(MOpcode::MOVrr, {gpr(PhysReg::RAX), makeVRegOperand(RegClass::GPR, 1)}),
        MInstr::make(MOpcode::RET, {}),
    });
    EXPECT_TRUE(accepts(fn, FrameInfo{}, VerifyStage::PostLegalize));
    EXPECT_TRUE(rejectsWith(fn, VerifyStage::PostRA, "V-CG-MIR-VREG"));

    MFunction viaMem = singleBlock({
        MInstr::make(MOpcode::MOVmr,
                     {gpr(PhysReg::RAX), makeMemOperand(OpReg{false, RegClass::GPR, 3}, 0)}),
        MInstr::make(MOpcode::RET, {}),
    });
    EXPECT_TRUE(rejectsWith(viaMem, VerifyStage::PostRA, "V-CG-MIR-VREG"));
}

// ---------------------------------------------------------------------------
// Structural rules.
// ---------------------------------------------------------------------------

TEST(X86MirVerify, RejectsBranchToMissingBlock) {
    MFunction jmp = singleBlock({MInstr::make(MOpcode::JMP, {lbl("missing")})});
    EXPECT_TRUE(rejectsWith(jmp, VerifyStage::PostLegalize, "V-CG-MIR-LABEL"));

    MFunction jcc = singleBlock({
        MInstr::make(MOpcode::JCC, {imm(4), lbl("missing")}),
        MInstr::make(MOpcode::RET, {}),
    });
    EXPECT_TRUE(rejectsWith(jcc, VerifyStage::PostLegalize, "V-CG-MIR-LABEL"));

    MFunction jt = singleBlock({
        MInstr::make(MOpcode::JUMPTABLE,
                     {gpr(PhysReg::RAX), lbl(".Ljt_0"), lbl("entry"), lbl("nope")}),
    });
    EXPECT_TRUE(rejectsWith(jt, VerifyStage::PostLegalize, "V-CG-MIR-LABEL"));
}

TEST(X86MirVerify, RejectsInstructionAfterTerminator) {
    MFunction fn = singleBlock({
        MInstr::make(MOpcode::RET, {}),
        MInstr::make(MOpcode::MOVri, {gpr(PhysReg::RAX), imm(1)}),
    });
    EXPECT_TRUE(rejectsWith(fn, VerifyStage::PostLegalize, "V-CG-MIR-AFTER-TERM"));
}

TEST(X86MirVerify, RejectsLastBlockFallingOffTheEnd) {
    MFunction fn = singleBlock({MInstr::make(MOpcode::MOVri, {gpr(PhysReg::RAX), imm(1)})});
    EXPECT_TRUE(rejectsWith(fn, VerifyStage::PostLegalize, "V-CG-MIR-FALLOFF"));
}

TEST(X86MirVerify, RejectsInBlockLabelPseudo) {
    MFunction fn = singleBlock({
        MInstr::make(MOpcode::LABEL, {lbl("inner")}),
        MInstr::make(MOpcode::RET, {}),
    });
    EXPECT_TRUE(rejectsWith(fn, VerifyStage::PostLegalize, "V-CG-MIR-LABEL-INBLOCK"));
}

TEST(X86MirVerify, RejectsVirtualRegisterWithTwoClasses) {
    MFunction fn = singleBlock({
        MInstr::make(MOpcode::MOVri, {makeVRegOperand(RegClass::GPR, 4), imm(1)}),
        MInstr::make(MOpcode::MOVSDrr,
                     {OpReg{true, RegClass::XMM, static_cast<uint16_t>(PhysReg::XMM0)},
                      makeVRegOperand(RegClass::XMM, 4)}),
        MInstr::make(MOpcode::RET, {}),
    });
    EXPECT_TRUE(rejectsWith(fn, VerifyStage::PostLegalize, "V-CG-MIR-VREG-CLASS"));
}

TEST(X86MirVerify, RejectsPhysicalRegisterWithWrongClass) {
    MFunction fn = singleBlock({
        MInstr::make(MOpcode::MOVri,
                     {OpReg{true, RegClass::XMM, static_cast<uint16_t>(PhysReg::RAX)}, imm(1)}),
        MInstr::make(MOpcode::RET, {}),
    });
    EXPECT_TRUE(rejectsWith(fn, VerifyStage::PostLegalize, "V-CG-MIR-REG-CLASS"));
}

TEST(X86MirVerify, RejectsMalformedMemoryOperand) {
    OpMem bad;
    bad.base = OpReg{true, RegClass::GPR, static_cast<uint16_t>(PhysReg::RBP)};
    bad.index = OpReg{true, RegClass::GPR, static_cast<uint16_t>(PhysReg::RCX)};
    bad.hasIndex = true;
    bad.scale = 3;
    MFunction fn = singleBlock({
        MInstr::make(MOpcode::MOVmr, {gpr(PhysReg::RAX), Operand{bad}}),
        MInstr::make(MOpcode::RET, {}),
    });
    EXPECT_TRUE(rejectsWith(fn, VerifyStage::PostLegalize, "V-CG-MIR-MEM"));
}

TEST(X86MirVerify, RejectsDuplicateBlockLabels) {
    MFunction fn = singleBlock({MInstr::make(MOpcode::JMP, {lbl("entry")})});
    fn.blocks.push_back(block("entry", {MInstr::make(MOpcode::RET, {})}));
    EXPECT_TRUE(rejectsWith(fn, VerifyStage::PostLegalize, "V-CG-MIR-DUP-LABEL"));
}

// ---------------------------------------------------------------------------
// Post-RA rules.
// ---------------------------------------------------------------------------

TEST(X86MirVerify, RejectsFrameDisplacementOutsideFrame) {
    MFunction fn = singleBlock({
        MInstr::make(MOpcode::MOVmr, {gpr(PhysReg::RAX), mem(PhysReg::RBP, -8)}),
        MInstr::make(MOpcode::RET, {}),
    });
    FrameInfo empty;
    EXPECT_TRUE(rejectsWith(fn, empty, VerifyStage::PostRA, "V-CG-MIR-FRAME-OFFSET"));

    FrameInfo sized;
    sized.frameSize = 16;
    EXPECT_TRUE(accepts(fn, sized, VerifyStage::PostRA));

    // An unresolved frame-slot placeholder is far outside any frame.
    MFunction placeholder = singleBlock({
        MInstr::make(MOpcode::MOVmr, {gpr(PhysReg::RAX), mem(PhysReg::RBP, -800000008)}),
        MInstr::make(MOpcode::RET, {}),
    });
    EXPECT_TRUE(rejectsWith(placeholder, sized, VerifyStage::PostRA, "V-CG-MIR-FRAME-OFFSET"));

    // Incoming stack arguments live above the return address.
    MFunction incoming = singleBlock({
        MInstr::make(MOpcode::MOVmr, {gpr(PhysReg::RAX), mem(PhysReg::RBP, 16)}),
        MInstr::make(MOpcode::RET, {}),
    });
    EXPECT_TRUE(accepts(incoming, empty, VerifyStage::PostRA));
}

TEST(X86MirVerify, RejectsStackDisplacementOutsideOutgoingArea) {
    MFunction fn = singleBlock({
        MInstr::make(MOpcode::MOVrm, {mem(PhysReg::RSP, 16), gpr(PhysReg::RAX)}),
        MInstr::make(MOpcode::RET, {}),
    });
    FrameInfo none;
    EXPECT_TRUE(rejectsWith(fn, none, VerifyStage::PostRA, "V-CG-MIR-SP-OFFSET"));

    FrameInfo outgoing;
    outgoing.outgoingArgArea = 32;
    EXPECT_TRUE(accepts(fn, outgoing, VerifyStage::PostRA));
}

TEST(X86MirVerify, RejectsUnsavedCalleeSavedWrite) {
    MFunction fn = singleBlock({
        MInstr::make(MOpcode::MOVri, {gpr(PhysReg::RBX), imm(1)}),
        MInstr::make(MOpcode::RET, {}),
    });
    FrameInfo none;
    EXPECT_TRUE(rejectsWith(fn, none, VerifyStage::PostRA, "V-CG-MIR-CALLEE-SAVE"));

    FrameInfo saved;
    saved.usedCalleeSaved = {PhysReg::RBX};
    EXPECT_TRUE(accepts(fn, saved, VerifyStage::PostRA));
}

TEST(X86MirVerify, RejectsScratchLiveAcrossCall) {
    MFunction fn = singleBlock({
        MInstr::make(MOpcode::MOVri, {gpr(PhysReg::R10), imm(1)}),
        MInstr::make(MOpcode::CALL, {lbl("callee")}),
        MInstr::make(MOpcode::ADDrr, {gpr(PhysReg::RAX), gpr(PhysReg::R10)}),
        MInstr::make(MOpcode::RET, {}),
    });
    EXPECT_TRUE(rejectsWith(fn, VerifyStage::PostRA, "V-CG-MIR-SCRATCH-CLOBBER"));

    MFunction fixed = singleBlock({
        MInstr::make(MOpcode::MOVri, {gpr(PhysReg::R10), imm(1)}),
        MInstr::make(MOpcode::CALL, {lbl("callee")}),
        MInstr::make(MOpcode::MOVri, {gpr(PhysReg::R10), imm(2)}),
        MInstr::make(MOpcode::ADDrr, {gpr(PhysReg::RAX), gpr(PhysReg::R10)}),
        MInstr::make(MOpcode::RET, {}),
    });
    EXPECT_TRUE(accepts(fixed, FrameInfo{}, VerifyStage::PostRA));
}

TEST(X86MirVerify, RejectsScratchLiveIntoJumpTableCase) {
    // The jump-table dispatch sequence materializes its target through
    // R10/R11, so a value carried in R10 into a case block is destroyed.
    MFunction fn;
    fn.name = "jt";
    fn.blocks.push_back(
        block("entry",
              {
                  MInstr::make(MOpcode::MOVri, {gpr(PhysReg::R10), imm(1)}),
                  MInstr::make(MOpcode::CMPri, {gpr(PhysReg::RAX), imm(2)}),
                  MInstr::make(MOpcode::JCC, {imm(7), lbl("dflt")}),
                  MInstr::make(MOpcode::JUMPTABLE,
                               {gpr(PhysReg::RAX), lbl(".Ljt_0"), lbl("case0"), lbl("case0")}),
              }));
    fn.blocks.push_back(
        block("case0",
              {
                  MInstr::make(MOpcode::ADDrr, {gpr(PhysReg::RAX), gpr(PhysReg::R10)}),
                  MInstr::make(MOpcode::RET, {}),
              }));
    fn.blocks.push_back(block("dflt", {MInstr::make(MOpcode::RET, {})}));
    EXPECT_TRUE(rejectsWith(fn, VerifyStage::PostRA, "V-CG-MIR-SCRATCH-CLOBBER"));
}

TEST(X86MirVerify, RejectsRegisterReadBeforeDefinition) {
    MFunction fn = singleBlock({
        MInstr::make(MOpcode::MOVrr, {gpr(PhysReg::RAX), gpr(PhysReg::R10)}),
        MInstr::make(MOpcode::RET, {}),
    });
    EXPECT_TRUE(rejectsWith(fn, VerifyStage::PostRA, "V-CG-MIR-ENTRY-LIVEIN"));

    MFunction args = singleBlock({
        MInstr::make(MOpcode::MOVrr, {gpr(PhysReg::RAX), gpr(PhysReg::RDI)}),
        MInstr::make(MOpcode::ADDrr, {gpr(PhysReg::RAX), gpr(PhysReg::RSI)}),
        MInstr::make(MOpcode::RET, {}),
    });
    EXPECT_TRUE(accepts(args, FrameInfo{}, VerifyStage::PostRA));
}

TEST(X86MirVerify, RejectsUninitializedReadOnOnePath) {
    MFunction fn;
    fn.name = "onepath";
    fn.blocks.push_back(block("entry",
                              {
                                  MInstr::make(MOpcode::CMPri, {gpr(PhysReg::RDI), imm(0)}),
                                  MInstr::make(MOpcode::JCC, {imm(4), lbl("join")}),
                              }));
    // R11 is defined only on the `set` path, then read at the join.
    fn.blocks.push_back(block("set", {MInstr::make(MOpcode::MOVri, {gpr(PhysReg::R11), imm(5)})}));
    fn.blocks.push_back(
        block("join",
              {
                  MInstr::make(MOpcode::MOVrr, {gpr(PhysReg::RAX), gpr(PhysReg::R11)}),
                  MInstr::make(MOpcode::RET, {}),
              }));
    EXPECT_TRUE(rejectsWith(fn, VerifyStage::PostRA, "V-CG-MIR-ENTRY-LIVEIN"));
}

// ---------------------------------------------------------------------------
// Pipeline integration through the PassManager post-pass hook.
// ---------------------------------------------------------------------------

namespace {

/// Install the verifier on @p manager after every pass, using @p stages
/// (parallel to the registered passes) to select the rule set.
void installVerifier(passes::PassManager &manager,
                     const std::vector<VerifyStage> &stages,
                     std::size_t &hookCalls) {
    manager.setPostPassHook([&stages, &hookCalls](passes::Module &module,
                                                  passes::Diagnostics &diags,
                                                  std::size_t passIndex) {
        ++hookCalls;
        if (!module.legalised || module.target == nullptr)
            return true;
        bool ok = true;
        for (std::size_t i = 0; i < module.mir.size(); ++i) {
            ok =
                verifyMir(
                    module.mir[i], module.frames[i], stages.at(passIndex), *module.target, diags) &&
                ok;
        }
        return ok;
    });
}

} // namespace

TEST(X86MirVerify, PipelineVerifiesEveryStageOnRealPrograms) {
    for (int level : {0, 1, 2}) {
        passes::Module module;
        module.il = parseIL(kCorpus);
        ASSERT_FALSE(module.il.functions.empty());
        module.options.optimizeLevel = level;
        module.target = &sysvTarget();

        passes::PassManager manager;
        std::vector<VerifyStage> stages;
        manager.addPass(std::make_unique<passes::LoweringPass>());
        stages.push_back(VerifyStage::PostLegalize);
        manager.addPass(std::make_unique<passes::LegalizePass>());
        stages.push_back(VerifyStage::PostLegalize);
        if (level >= 1) {
            manager.addPass(std::make_unique<passes::PreRegAllocOptPass>());
            stages.push_back(VerifyStage::PostLegalize);
        }
        manager.addPass(std::make_unique<passes::RegAllocPass>());
        stages.push_back(VerifyStage::PostRA);
        manager.addPass(std::make_unique<passes::SchedulerPass>());
        stages.push_back(VerifyStage::PostSchedule);
        manager.addPass(std::make_unique<passes::PeepholePass>());
        stages.push_back(VerifyStage::PostPeephole);

        std::size_t hookCalls = 0;
        installVerifier(manager, stages, hookCalls);

        passes::Diagnostics diags;
        const bool ok = manager.run(module, diags);
        if (!ok) {
            std::cerr << "pipeline failed at -O" << level << ":\n";
            for (const auto &e : diags.errors())
                std::cerr << e << "\n";
        }
        EXPECT_TRUE(ok);
        EXPECT_EQ(hookCalls, stages.size());
        EXPECT_TRUE(module.registersAllocated);
    }
}

TEST(X86MirVerify, PipelineVerifierRejectsBrokenPassOutput) {
    class BreakBranchPass final : public passes::Pass {
      public:
        bool run(passes::Module &module, passes::Diagnostics &) override {
            for (auto &fn : module.mir) {
                auto &entry = fn.blocks.front().instructions;
                entry.insert(entry.begin(), MInstr::make(MOpcode::JMP, {lbl("nowhere")}));
            }
            return true;
        }
    };

    class NeverRunsPass final : public passes::Pass {
      public:
        bool run(passes::Module &, passes::Diagnostics &) override {
            ran = true;
            return true;
        }

        bool ran = false;
    };

    passes::Module module;
    module.il = parseIL(kCorpus);
    ASSERT_FALSE(module.il.functions.empty());
    module.target = &sysvTarget();

    auto never = std::make_unique<NeverRunsPass>();
    NeverRunsPass *neverPtr = never.get();

    passes::PassManager manager;
    std::vector<VerifyStage> stages;
    manager.addPass(std::make_unique<passes::LoweringPass>());
    stages.push_back(VerifyStage::PostLegalize);
    manager.addPass(std::make_unique<passes::LegalizePass>());
    stages.push_back(VerifyStage::PostLegalize);
    manager.addPass(std::make_unique<BreakBranchPass>());
    stages.push_back(VerifyStage::PostLegalize);
    manager.addPass(std::move(never));
    stages.push_back(VerifyStage::PostLegalize);

    std::size_t hookCalls = 0;
    installVerifier(manager, stages, hookCalls);

    passes::Diagnostics diags;
    EXPECT_FALSE(manager.run(module, diags));
    EXPECT_TRUE(hasCode(diags, "V-CG-MIR-LABEL"));
    EXPECT_EQ(hookCalls, 3u);
    EXPECT_FALSE(neverPtr->ran);
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, &argv);
    return zanna_test::run_all_tests();
}
