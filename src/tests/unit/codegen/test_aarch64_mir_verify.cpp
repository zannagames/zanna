//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/codegen/test_aarch64_mir_verify.cpp
// Purpose: Pins the AArch64 MIR verifier: every rule has a hand-built MIR
//          function that violates it and is rejected with the expected
//          `V-CG-MIR-*` code, well-formed MIR passes, the pipeline accepts the
//          verifier at every stage on real programs, and the PassManager
//          post-pass hook runs after each pass and short-circuits on failure.
// Key invariants:
//   - A rule that stops firing on its failing case breaks this test before
//     it can hide a miscompile.
//   - The pipeline corpus runs with verification at -O0, -O1, and -O2 so the
//     verifier's rules hold on the MIR the optimizing passes actually produce.
// Ownership/Lifetime: Standalone test binary.
// Links: src/codegen/aarch64/MirVerify.hpp,
//        src/codegen/common/PassManager.hpp
//
//===----------------------------------------------------------------------===//

#include "tests/TestHarness.hpp"

#include "codegen/aarch64/CodegenPipeline.hpp"
#include "codegen/aarch64/MirVerify.hpp"
#include "codegen/aarch64/TargetAArch64.hpp"
#include "codegen/aarch64/passes/LegalizePass.hpp"
#include "codegen/aarch64/passes/LoweringPass.hpp"
#include "codegen/aarch64/passes/PassManager.hpp"
#include "codegen/aarch64/passes/RegAllocPass.hpp"
#include "il/io/Parser.hpp"

#include <iostream>
#include <memory>
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

MOperand label(const char *name) {
    return MOperand::labelOp(name);
}

MInstr ins(MOpcode opc, std::vector<MOperand> ops) {
    return MInstr{opc, std::move(ops)};
}

/// Single-block function `entry:` holding @p instrs.
MFunction singleBlock(std::vector<MInstr> instrs) {
    MFunction fn;
    fn.name = "f";
    MBasicBlock bb;
    bb.name = "entry";
    bb.instrs = std::move(instrs);
    fn.blocks.push_back(std::move(bb));
    return fn;
}

/// Whether @p diags recorded an error with exactly @p code.
bool hasCode(const passes::Diagnostics &diags, const std::string &code) {
    for (const auto &d : diags.diagnostics()) {
        if (d.code == code)
            return true;
    }
    return false;
}

/// Run the verifier and return whether @p code was reported.
bool rejectsWith(const MFunction &fn, VerifyStage stage, const std::string &code) {
    passes::Diagnostics diags;
    const bool ok = verifyMir(fn, stage, darwinTarget(), diags);
    if (ok)
        return false;
    return hasCode(diags, code);
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
// Well-formed MIR passes at every stage.
// ---------------------------------------------------------------------------

TEST(AArch64MirVerify, AcceptsWellFormedPostRaFunction) {
    MFunction fn;
    fn.name = "ok";
    fn.frame.totalBytes = 16;
    fn.savedGPRs = {PhysReg::X19};

    MBasicBlock entry;
    entry.name = "entry";
    entry.instrs = {
        ins(MOpcode::MovRI, {x(PhysReg::X19), imm(7)}),
        ins(MOpcode::StrRegFpImm, {x(PhysReg::X19), imm(-8)}),
        ins(MOpcode::CmpRI, {x(PhysReg::X0), imm(0)}),
        ins(MOpcode::BCond, {MOperand::condOp("eq"), label("done")}),
    };
    MBasicBlock body;
    body.name = "body";
    body.instrs = {
        ins(MOpcode::SubSpImm, {imm(16)}),
        ins(MOpcode::StrRegSpImm, {x(PhysReg::X0), imm(8)}),
        ins(MOpcode::Bl, {label("callee")}),
        ins(MOpcode::AddSpImm, {imm(16)}),
        ins(MOpcode::LdrRegFpImm, {x(PhysReg::X0), imm(-8)}),
    };
    MBasicBlock done;
    done.name = "done";
    done.instrs = {ins(MOpcode::Ret, {})};
    done.carriedExitRegs = {static_cast<uint16_t>(PhysReg::X0),
                            static_cast<uint16_t>(PhysReg::X19)};

    fn.blocks = {std::move(entry), std::move(body), std::move(done)};

    for (VerifyStage stage : {VerifyStage::PostLowering,
                              VerifyStage::PostRA,
                              VerifyStage::PostExpand,
                              VerifyStage::PostPeephole,
                              VerifyStage::PostSchedule}) {
        passes::Diagnostics diags;
        EXPECT_TRUE(verifyMir(fn, stage, darwinTarget(), diags));
        EXPECT_TRUE(diags.errors().empty());
    }
}

TEST(AArch64MirVerify, AcceptsVirtualRegistersBeforeAllocation) {
    MFunction fn = singleBlock({
        ins(MOpcode::MovRI, {MOperand::vregOp(RegClass::GPR, 1), imm(1)}),
        ins(MOpcode::MovRR, {x(PhysReg::X0), MOperand::vregOp(RegClass::GPR, 1)}),
        ins(MOpcode::Ret, {}),
    });
    passes::Diagnostics diags;
    EXPECT_TRUE(verifyMir(fn, VerifyStage::PostLowering, darwinTarget(), diags));
    EXPECT_TRUE(rejectsWith(fn, VerifyStage::PostRA, "V-CG-MIR-VREG"));
}

// ---------------------------------------------------------------------------
// Structural rules (every stage).
// ---------------------------------------------------------------------------

TEST(AArch64MirVerify, RejectsBranchToMissingBlock) {
    MFunction fn = singleBlock({ins(MOpcode::Br, {label("missing")})});
    EXPECT_TRUE(rejectsWith(fn, VerifyStage::PostLowering, "V-CG-MIR-LABEL"));
}

TEST(AArch64MirVerify, RejectsJumpTableCaseToMissingBlock) {
    MFunction fn = singleBlock({
        ins(MOpcode::JumpTable, {x(PhysReg::X0), label(".Ljt_0"), label("entry"), label("nope")}),
    });
    EXPECT_TRUE(rejectsWith(fn, VerifyStage::PostLowering, "V-CG-MIR-LABEL"));
}

TEST(AArch64MirVerify, RejectsInstructionAfterTerminator) {
    MFunction fn = singleBlock({
        ins(MOpcode::Ret, {}),
        ins(MOpcode::MovRI, {x(PhysReg::X0), imm(1)}),
    });
    EXPECT_TRUE(rejectsWith(fn, VerifyStage::PostLowering, "V-CG-MIR-AFTER-TERM"));
}

TEST(AArch64MirVerify, RejectsLastBlockFallingOffTheEnd) {
    MFunction fn = singleBlock({ins(MOpcode::MovRI, {x(PhysReg::X0), imm(1)})});
    EXPECT_TRUE(rejectsWith(fn, VerifyStage::PostLowering, "V-CG-MIR-FALLOFF"));

    // A conditional branch at the end of the last block also falls off.
    MFunction fn2 = singleBlock({
        ins(MOpcode::CmpRI, {x(PhysReg::X0), imm(0)}),
        ins(MOpcode::BCond, {MOperand::condOp("eq"), label("entry")}),
    });
    EXPECT_TRUE(rejectsWith(fn2, VerifyStage::PostLowering, "V-CG-MIR-FALLOFF"));
}

TEST(AArch64MirVerify, RejectsVirtualRegisterWithTwoClasses) {
    MFunction fn = singleBlock({
        ins(MOpcode::MovRI, {MOperand::vregOp(RegClass::GPR, 4), imm(1)}),
        ins(MOpcode::FMovRR, {x(PhysReg::V0), MOperand::vregOp(RegClass::FPR, 4)}),
        ins(MOpcode::Ret, {}),
    });
    EXPECT_TRUE(rejectsWith(fn, VerifyStage::PostLowering, "V-CG-MIR-VREG-CLASS"));
}

TEST(AArch64MirVerify, RejectsPhysicalRegisterWithWrongClass) {
    MOperand bad = MOperand::regOp(PhysReg::X3);
    bad.reg.cls = RegClass::FPR;
    MFunction fn = singleBlock({
        ins(MOpcode::MovRI, {bad, imm(1)}),
        ins(MOpcode::Ret, {}),
    });
    EXPECT_TRUE(rejectsWith(fn, VerifyStage::PostLowering, "V-CG-MIR-REG-CLASS"));
}

TEST(AArch64MirVerify, RejectsUnsortedCarriedExitRegs) {
    MFunction fn = singleBlock({ins(MOpcode::Ret, {})});
    fn.blocks[0].carriedExitRegs = {static_cast<uint16_t>(PhysReg::X3),
                                    static_cast<uint16_t>(PhysReg::X1)};
    EXPECT_TRUE(rejectsWith(fn, VerifyStage::PostLowering, "V-CG-MIR-CARRY"));
}

TEST(AArch64MirVerify, RejectsDuplicateBlockLabels) {
    MFunction fn = singleBlock({ins(MOpcode::Br, {label("entry")})});
    MBasicBlock dup;
    dup.name = "entry";
    dup.instrs = {ins(MOpcode::Ret, {})};
    fn.blocks.push_back(std::move(dup));
    EXPECT_TRUE(rejectsWith(fn, VerifyStage::PostLowering, "V-CG-MIR-DUP-LABEL"));
}

TEST(AArch64MirVerify, RejectsPlatformRegister) {
    MFunction fn = singleBlock({
        ins(MOpcode::MovRI, {x(PhysReg::X18), imm(1)}),
        ins(MOpcode::Ret, {}),
    });
    EXPECT_TRUE(rejectsWith(fn, VerifyStage::PostLowering, "V-CG-MIR-RESERVED-REG"));
}

// ---------------------------------------------------------------------------
// Post-RA rules.
// ---------------------------------------------------------------------------

TEST(AArch64MirVerify, RejectsFrameOffsetOutsideFrame) {
    MFunction fn = singleBlock({
        ins(MOpcode::LdrRegFpImm, {x(PhysReg::X0), imm(-8)}),
        ins(MOpcode::Ret, {}),
    });
    fn.frame.totalBytes = 0;
    EXPECT_TRUE(rejectsWith(fn, VerifyStage::PostRA, "V-CG-MIR-FRAME-OFFSET"));

    fn.frame.totalBytes = 16;
    passes::Diagnostics diags;
    EXPECT_TRUE(verifyMir(fn, VerifyStage::PostRA, darwinTarget(), diags));

    // A pair access must fit entirely inside the frame.
    MFunction pair = singleBlock({
        ins(MOpcode::LdpRegFpImm, {x(PhysReg::X0), x(PhysReg::X1), imm(-8)}),
        ins(MOpcode::Ret, {}),
    });
    pair.frame.totalBytes = 16;
    EXPECT_TRUE(rejectsWith(pair, VerifyStage::PostRA, "V-CG-MIR-FRAME-OFFSET"));

    // Incoming stack arguments live above the saved fp/lr pair.
    MFunction incoming = singleBlock({
        ins(MOpcode::LdrRegFpImm, {x(PhysReg::X0), imm(16)}),
        ins(MOpcode::Ret, {}),
    });
    passes::Diagnostics diags2;
    EXPECT_TRUE(verifyMir(incoming, VerifyStage::PostRA, darwinTarget(), diags2));
}

TEST(AArch64MirVerify, RejectsStackStoreOutsideReservedArea) {
    MFunction fn = singleBlock({
        ins(MOpcode::StrRegSpImm, {x(PhysReg::X0), imm(0)}),
        ins(MOpcode::Ret, {}),
    });
    EXPECT_TRUE(rejectsWith(fn, VerifyStage::PostRA, "V-CG-MIR-SP-OFFSET"));

    MFunction reserved = singleBlock({
        ins(MOpcode::SubSpImm, {imm(16)}),
        ins(MOpcode::StrRegSpImm, {x(PhysReg::X0), imm(8)}),
        ins(MOpcode::AddSpImm, {imm(16)}),
        ins(MOpcode::Ret, {}),
    });
    passes::Diagnostics diags;
    EXPECT_TRUE(verifyMir(reserved, VerifyStage::PostRA, darwinTarget(), diags));

    MFunction past = singleBlock({
        ins(MOpcode::SubSpImm, {imm(16)}),
        ins(MOpcode::StrRegSpImm, {x(PhysReg::X0), imm(16)}),
        ins(MOpcode::AddSpImm, {imm(16)}),
        ins(MOpcode::Ret, {}),
    });
    EXPECT_TRUE(rejectsWith(past, VerifyStage::PostRA, "V-CG-MIR-SP-OFFSET"));
}

TEST(AArch64MirVerify, RejectsUnsavedCalleeSavedWrite) {
    MFunction fn = singleBlock({
        ins(MOpcode::MovRI, {x(PhysReg::X19), imm(1)}),
        ins(MOpcode::Ret, {}),
    });
    EXPECT_TRUE(rejectsWith(fn, VerifyStage::PostRA, "V-CG-MIR-CALLEE-SAVE"));

    fn.savedGPRs = {PhysReg::X19};
    passes::Diagnostics diags;
    EXPECT_TRUE(verifyMir(fn, VerifyStage::PostRA, darwinTarget(), diags));

    MFunction fpr = singleBlock({
        ins(MOpcode::FMovRR, {x(PhysReg::V8), x(PhysReg::V0)}),
        ins(MOpcode::Ret, {}),
    });
    EXPECT_TRUE(rejectsWith(fpr, VerifyStage::PostRA, "V-CG-MIR-CALLEE-SAVE"));
}

TEST(AArch64MirVerify, RejectsExplicitFrameRegisterWrite) {
    MFunction fn = singleBlock({
        ins(MOpcode::MovRI, {x(PhysReg::X29), imm(1)}),
        ins(MOpcode::Ret, {}),
    });
    EXPECT_TRUE(rejectsWith(fn, VerifyStage::PostRA, "V-CG-MIR-RESERVED-WRITE"));
}

TEST(AArch64MirVerify, RejectsScratchLiveAcrossCall) {
    MFunction fn = singleBlock({
        ins(MOpcode::MovRI, {x(PhysReg::X9), imm(1)}),
        ins(MOpcode::Bl, {label("callee")}),
        ins(MOpcode::AddRRR, {x(PhysReg::X0), x(PhysReg::X9), x(PhysReg::X9)}),
        ins(MOpcode::Ret, {}),
    });
    EXPECT_TRUE(rejectsWith(fn, VerifyStage::PostRA, "V-CG-MIR-SCRATCH-CLOBBER"));

    // Redefining x9 after the call makes it legal again.
    MFunction fixed = singleBlock({
        ins(MOpcode::MovRI, {x(PhysReg::X9), imm(1)}),
        ins(MOpcode::Bl, {label("callee")}),
        ins(MOpcode::MovRI, {x(PhysReg::X9), imm(2)}),
        ins(MOpcode::AddRRR, {x(PhysReg::X0), x(PhysReg::X9), x(PhysReg::X9)}),
        ins(MOpcode::Ret, {}),
    });
    passes::Diagnostics diags;
    EXPECT_TRUE(verifyMir(fixed, VerifyStage::PostRA, darwinTarget(), diags));
}

TEST(AArch64MirVerify, RejectsScratchLiveAcrossBlockBoundaryClobber) {
    // x9 is defined in `entry`, clobbered by the call in `mid`, and read in `exit`.
    MFunction fn;
    fn.name = "cross";
    MBasicBlock entry;
    entry.name = "entry";
    entry.instrs = {ins(MOpcode::MovRI, {x(PhysReg::X9), imm(1)}),
                    ins(MOpcode::Br, {label("mid")})};
    MBasicBlock mid;
    mid.name = "mid";
    mid.instrs = {ins(MOpcode::Bl, {label("callee")}), ins(MOpcode::Br, {label("exit")})};
    MBasicBlock exit;
    exit.name = "exit";
    exit.instrs = {ins(MOpcode::MovRR, {x(PhysReg::X0), x(PhysReg::X9)}), ins(MOpcode::Ret, {})};
    fn.blocks = {std::move(entry), std::move(mid), std::move(exit)};
    EXPECT_TRUE(rejectsWith(fn, VerifyStage::PostRA, "V-CG-MIR-SCRATCH-CLOBBER"));
}

TEST(AArch64MirVerify, RejectsScratchLiveAcrossWideImmediateExpansion) {
    // The emitter materializes #0x12345 through a scratch register; x9 must
    // not hold a live value across it.
    MFunction fn = singleBlock({
        ins(MOpcode::MovRI, {x(PhysReg::X9), imm(1)}),
        ins(MOpcode::AddRI, {x(PhysReg::X0), x(PhysReg::X1), imm(0x12345)}),
        ins(MOpcode::AddRRR, {x(PhysReg::X0), x(PhysReg::X0), x(PhysReg::X9)}),
        ins(MOpcode::Ret, {}),
    });
    EXPECT_TRUE(rejectsWith(fn, VerifyStage::PostRA, "V-CG-MIR-SCRATCH-CLOBBER"));

    // Once immediates are expanded the same instruction is itself illegal.
    EXPECT_TRUE(rejectsWith(fn, VerifyStage::PostExpand, "V-CG-MIR-IMM-ENCODE"));

    MFunction encodable = singleBlock({
        ins(MOpcode::MovRI, {x(PhysReg::X9), imm(1)}),
        ins(MOpcode::AddRI, {x(PhysReg::X0), x(PhysReg::X1), imm(4095)}),
        ins(MOpcode::AddRRR, {x(PhysReg::X0), x(PhysReg::X0), x(PhysReg::X9)}),
        ins(MOpcode::Ret, {}),
    });
    passes::Diagnostics diags;
    EXPECT_TRUE(verifyMir(encodable, VerifyStage::PostExpand, darwinTarget(), diags));
}

TEST(AArch64MirVerify, RejectsRegisterReadBeforeDefinition) {
    MFunction fn = singleBlock({
        ins(MOpcode::AddRRR, {x(PhysReg::X0), x(PhysReg::X10), x(PhysReg::X10)}),
        ins(MOpcode::Ret, {}),
    });
    EXPECT_TRUE(rejectsWith(fn, VerifyStage::PostRA, "V-CG-MIR-ENTRY-LIVEIN"));

    // Argument registers are legitimate inputs.
    MFunction args = singleBlock({
        ins(MOpcode::AddRRR, {x(PhysReg::X0), x(PhysReg::X1), x(PhysReg::X2)}),
        ins(MOpcode::Ret, {}),
    });
    passes::Diagnostics diags;
    EXPECT_TRUE(verifyMir(args, VerifyStage::PostRA, darwinTarget(), diags));
}

TEST(AArch64MirVerify, RejectsUninitializedReadOnOnePath) {
    // x10 is defined only on the `set` path, then read at the join.
    MFunction fn;
    fn.name = "onepath";
    MBasicBlock entry;
    entry.name = "entry";
    entry.instrs = {ins(MOpcode::CmpRI, {x(PhysReg::X0), imm(0)}),
                    ins(MOpcode::BCond, {MOperand::condOp("eq"), label("join")})};
    MBasicBlock set;
    set.name = "set";
    set.instrs = {ins(MOpcode::MovRI, {x(PhysReg::X10), imm(5)})};
    MBasicBlock join;
    join.name = "join";
    join.instrs = {ins(MOpcode::MovRR, {x(PhysReg::X0), x(PhysReg::X10)}), ins(MOpcode::Ret, {})};
    fn.blocks = {std::move(entry), std::move(set), std::move(join)};
    EXPECT_TRUE(rejectsWith(fn, VerifyStage::PostRA, "V-CG-MIR-ENTRY-LIVEIN"));
}

TEST(AArch64MirVerify, ReportsAreCappedPerFunction) {
    std::vector<MInstr> body;
    for (int i = 0; i < 100; ++i)
        body.push_back(ins(MOpcode::Br, {label("missing")}));
    MFunction fn = singleBlock(std::move(body));
    passes::Diagnostics diags;
    EXPECT_FALSE(verifyMir(fn, VerifyStage::PostLowering, darwinTarget(), diags));
    EXPECT_GT(diags.errors().size(), 1u);
    EXPECT_LT(diags.errors().size(), 100u);
}

// ---------------------------------------------------------------------------
// Pipeline integration: verification at every stage on real programs.
// ---------------------------------------------------------------------------

TEST(AArch64MirVerify, PipelineVerifiesEveryStageOnRealPrograms) {
    for (int level : {0, 1, 2}) {
        il::core::Module mod = parseIL(kCorpus);
        ASSERT_FALSE(mod.functions.empty());

        passes::AArch64Module m;
        m.ilMod = &mod;
        m.ti = &darwinTarget();

        PipelineOptions opts;
        opts.emitAssemblyText = true;
        opts.optimizeLevel = level;
        opts.verifyMir = true;

        std::ostringstream diag;
        const bool ok = runCodegenPipeline(m, opts, diag);
        if (!ok)
            std::cerr << "pipeline failed at -O" << level << ":\n" << diag.str();
        EXPECT_TRUE(ok);
        EXPECT_EQ(diag.str().find("V-CG-MIR-"), std::string::npos);
        EXPECT_FALSE(m.assembly.empty());
    }
}

TEST(AArch64MirVerify, PipelineVerifierRejectsBrokenPassOutput) {
    // A pass that leaves a dangling branch behind is caught by the hook
    // installed on the pass manager rather than by the emitter.
    class BreakBranchPass final : public passes::Pass {
      public:
        bool run(passes::AArch64Module &module, passes::Diagnostics &) override {
            for (auto &fn : module.mir)
                fn.blocks.front().instrs.insert(fn.blocks.front().instrs.begin(),
                                                ins(MOpcode::Br, {label("nowhere")}));
            return true;
        }
    };

    class NeverRunsPass final : public passes::Pass {
      public:
        bool run(passes::AArch64Module &, passes::Diagnostics &) override {
            ran = true;
            return true;
        }

        bool ran = false;
    };

    il::core::Module mod = parseIL(kCorpus);
    ASSERT_FALSE(mod.functions.empty());
    passes::AArch64Module m;
    m.ilMod = &mod;
    m.ti = &darwinTarget();

    auto never = std::make_unique<NeverRunsPass>();
    NeverRunsPass *neverPtr = never.get();

    passes::PassManager pm;
    std::vector<VerifyStage> stages;
    pm.addPass(std::make_unique<passes::LoweringPass>());
    stages.push_back(VerifyStage::PostLowering);
    pm.addPass(std::make_unique<passes::LegalizePass>());
    stages.push_back(VerifyStage::PostLowering);
    pm.addPass(std::make_unique<BreakBranchPass>());
    stages.push_back(VerifyStage::PostLowering);
    pm.addPass(std::move(never));
    stages.push_back(VerifyStage::PostLowering);

    std::size_t hookCalls = 0;
    pm.setPostPassHook(
        [&](passes::AArch64Module &module, passes::Diagnostics &diags, std::size_t passIndex) {
            ++hookCalls;
            bool ok = true;
            for (const auto &fn : module.mir)
                ok = verifyMir(fn, stages.at(passIndex), *module.ti, diags) && ok;
            return ok;
        });

    passes::Diagnostics diags;
    EXPECT_FALSE(pm.run(m, diags));
    EXPECT_TRUE(hasCode(diags, "V-CG-MIR-LABEL"));
    EXPECT_EQ(hookCalls, 3u);
    EXPECT_FALSE(neverPtr->ran);
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, &argv);
    return zanna_test::run_all_tests();
}
