//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/codegen/test_aarch64_instr_effects.cpp
// Purpose: Pins the shared AArch64 instruction-effects model: implicit ABI
//          registers at calls and returns, NZCV producers/consumers, memory
//          classes, emit-time scratch clobbers, and the invariant that every
//          role query in the backend (peephole classifiers, pre-RA traits,
//          scheduler) agrees with ra::operandRoles on real MIR.
// Key invariants:
//   - The corpus test lowers IL through the full pipeline and compares
//     peephole::classifyOperand against ra::operandRoles on every operand, so
//     an opcode classified in one table and not the other fails here rather
//     than as a miscompile.
// Ownership/Lifetime: Standalone test binary.
// Links: src/codegen/aarch64/InstrEffects.hpp,
//        src/codegen/aarch64/ra/OperandRoles.hpp,
//        src/codegen/aarch64/peephole/PeepholeCommon.hpp
//
//===----------------------------------------------------------------------===//

#include "tests/TestHarness.hpp"

#include "codegen/aarch64/InstrEffects.hpp"
#include "codegen/aarch64/TargetAArch64.hpp"
#include "codegen/aarch64/passes/LegalizePass.hpp"
#include "codegen/aarch64/passes/LoweringPass.hpp"
#include "codegen/aarch64/passes/PassManager.hpp"
#include "codegen/aarch64/passes/PeepholePass.hpp"
#include "codegen/aarch64/passes/RegAllocPass.hpp"
#include "codegen/aarch64/peephole/PeepholeCommon.hpp"
#include "codegen/aarch64/ra/OperandRoles.hpp"
#include "il/io/Parser.hpp"

#include <sstream>
#include <string>

using namespace zanna::codegen::aarch64;

namespace {

MOperand x(PhysReg r) {
    return MOperand::regOp(r);
}

MOperand imm(long long v) {
    return MOperand::immOp(v);
}

il::core::Module parseIL(const std::string &src) {
    std::istringstream ss(src);
    il::core::Module mod;
    if (!il::io::Parser::parse(ss, mod))
        return {};
    return mod;
}

/// Lower @p il through Lowering → Legalize → RegAlloc (→ Peephole when
/// @p optimize) and hand every MIR function to @p visit.
template <typename Visit>
void forEachLoweredFunction(const std::string &il, bool optimize, Visit visit) {
    passes::AArch64Module module;
    il::core::Module mod = parseIL(il);
    ASSERT_FALSE(mod.functions.empty());
    module.ilMod = &mod;
    module.ti = &darwinTarget();
    passes::PassManager pm;
    pm.addPass(std::make_unique<passes::LoweringPass>());
    pm.addPass(std::make_unique<passes::LegalizePass>());
    pm.addPass(std::make_unique<passes::RegAllocPass>());
    if (optimize)
        pm.addPass(std::make_unique<passes::PeepholePass>());
    passes::Diagnostics diags;
    ASSERT_TRUE(pm.run(module, diags));
    for (const auto &fn : module.mir)
        visit(fn);
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
  %r = call @step(%sum0, %v)
  %big = scmp_gt %r, 4096
  cbr %big, useb(%sum0, %i0), usem(%sum0, %i0, %r)
useb(%s3: i64, %j3: i64):
  br join(%s3, %j3, 4096)
usem(%s4: i64, %j4: i64, %m4: i64):
  br join(%s4, %j4, %m4)
join(%s5: i64, %j5: i64, %val: i64):
  %acc = iadd.ovf %s5, %val
  %masked = and %acc, 268435455
  %next_i = iadd.ovf %j5, 1
  br loop(%masked, %next_i)
exit(%result: i64):
  %rr = and %result, 255
  ret %rr
}
)";

} // namespace

TEST(AArch64InstrEffects, CallReadsArgumentsAndClobbersCallerSaved) {
    const TargetInfo &ti = darwinTarget();
    const InstrEffects fx = effectsOf(MInstr{MOpcode::Bl, {MOperand::labelOp("callee")}}, ti);
    EXPECT_TRUE(fx.isCall);
    EXPECT_TRUE(fx.writesFlags);
    EXPECT_EQ(fx.mem, InstrEffects::Mem::Barrier);
    for (PhysReg r : ti.intArgOrder)
        EXPECT_TRUE(fx.uses.contains(r));
    for (PhysReg r : ti.f64ArgOrder)
        EXPECT_TRUE(fx.uses.contains(r));
    EXPECT_TRUE(fx.uses.contains(PhysReg::SP));
    for (PhysReg r : ti.callerSavedGPR)
        EXPECT_TRUE(fx.defs.contains(r));
    for (PhysReg r : ti.callerSavedFPR)
        EXPECT_TRUE(fx.defs.contains(r));
    EXPECT_TRUE(fx.defs.contains(PhysReg::X30));
    for (PhysReg r : ti.calleeSavedGPR)
        EXPECT_FALSE(fx.defs.contains(r));
    EXPECT_FALSE(fx.isNoReturn);

    const InstrEffects trap =
        effectsOf(MInstr{MOpcode::Bl, {MOperand::labelOp("rt_trap_ovf")}}, ti);
    EXPECT_TRUE(trap.isNoReturn);
}

TEST(AArch64InstrEffects, ReturnReadsReturnRegisters) {
    const TargetInfo &ti = darwinTarget();
    const InstrEffects fx = effectsOf(MInstr{MOpcode::Ret, {}}, ti);
    EXPECT_TRUE(fx.isTerminator);
    EXPECT_TRUE(fx.uses.contains(ti.intReturnReg));
    EXPECT_TRUE(fx.uses.contains(ti.f64ReturnReg));
    EXPECT_TRUE(fx.defs.empty());
}

TEST(AArch64InstrEffects, ExplicitRolesFlagsAndMemory) {
    const TargetInfo &ti = darwinTarget();
    {
        const InstrEffects fx = effectsOf(
            MInstr{MOpcode::AddsRRR, {x(PhysReg::X0), x(PhysReg::X1), x(PhysReg::X2)}}, ti);
        EXPECT_TRUE(fx.defs.contains(PhysReg::X0));
        EXPECT_TRUE(fx.uses.contains(PhysReg::X1));
        EXPECT_TRUE(fx.uses.contains(PhysReg::X2));
        EXPECT_FALSE(fx.uses.contains(PhysReg::X0));
        EXPECT_TRUE(fx.writesFlags);
        EXPECT_FALSE(fx.readsFlags);
        EXPECT_EQ(fx.mem, InstrEffects::Mem::None);
    }
    {
        const InstrEffects fx = effectsOf(
            MInstr{MOpcode::Csel,
                   {x(PhysReg::X3), x(PhysReg::X4), x(PhysReg::X5), MOperand::condOp("ne")}},
            ti);
        EXPECT_TRUE(fx.readsFlags);
        EXPECT_FALSE(fx.writesFlags);
        EXPECT_TRUE(fx.defs.contains(PhysReg::X3));
    }
    {
        const InstrEffects fx =
            effectsOf(MInstr{MOpcode::LdrRegFpImm, {x(PhysReg::X6), imm(-16)}}, ti);
        EXPECT_EQ(fx.mem, InstrEffects::Mem::Load);
        EXPECT_TRUE(fx.uses.contains(PhysReg::X29));
        EXPECT_TRUE(fx.defs.contains(PhysReg::X6));
    }
    {
        const InstrEffects fx =
            effectsOf(MInstr{MOpcode::StrRegBaseRegLsl,
                             {x(PhysReg::X7), x(PhysReg::X8), x(PhysReg::X10), imm(3)}},
                      ti);
        EXPECT_EQ(fx.mem, InstrEffects::Mem::Store);
        EXPECT_TRUE(fx.uses.contains(PhysReg::X7));
        EXPECT_TRUE(fx.uses.contains(PhysReg::X8));
        EXPECT_TRUE(fx.uses.contains(PhysReg::X10));
        EXPECT_TRUE(fx.defs.empty());
    }
    {
        const InstrEffects fx = effectsOf(MInstr{MOpcode::SubSpImm, {imm(32)}}, ti);
        EXPECT_TRUE(fx.uses.contains(PhysReg::SP));
        EXPECT_TRUE(fx.defs.contains(PhysReg::SP));
        EXPECT_EQ(fx.mem, InstrEffects::Mem::Barrier);
    }
    {
        const InstrEffects fx =
            effectsOf(MInstr{MOpcode::JumpTable,
                             {x(PhysReg::X0), MOperand::labelOp("tbl"), MOperand::labelOp("L1")}},
                      ti);
        EXPECT_TRUE(fx.isTerminator);
        EXPECT_TRUE(fx.uses.contains(PhysReg::X0));
        EXPECT_TRUE(fx.defs.contains(kScratchGPR2));
        EXPECT_TRUE(fx.defs.contains(kScratchGPR3));
    }
}

TEST(AArch64InstrEffects, EmitTimeScratchClobbersAreDefs) {
    const TargetInfo &ti = darwinTarget();
    // Encodable forms touch no scratch.
    EXPECT_FALSE(
        emitTimeScratchClobber(MInstr{MOpcode::AddRI, {x(PhysReg::X0), x(PhysReg::X1), imm(8)}}));
    EXPECT_FALSE(emitTimeScratchClobber(
        MInstr{MOpcode::AddRI, {x(PhysReg::X0), x(PhysReg::X1), imm(0x123000)}}));
    EXPECT_FALSE(emitTimeScratchClobber(MInstr{MOpcode::CmpRI, {x(PhysReg::X0), imm(4095)}}));
    EXPECT_FALSE(emitTimeScratchClobber(
        MInstr{MOpcode::AndRI, {x(PhysReg::X0), x(PhysReg::X1), imm(0xFF)}}));
    EXPECT_FALSE(emitTimeScratchClobber(MInstr{MOpcode::StrRegFpImm, {x(PhysReg::X0), imm(-256)}}));
    // Wide forms expand through x9/x16/x17.
    const MInstr wideAdd{MOpcode::AddRI, {x(PhysReg::X0), x(PhysReg::X1), imm(0x123456)}};
    EXPECT_TRUE(emitTimeScratchClobber(wideAdd));
    const InstrEffects fx = effectsOf(wideAdd, ti);
    EXPECT_TRUE(fx.defs.contains(kScratchGPR));
    EXPECT_TRUE(fx.defs.contains(kScratchGPR2));
    EXPECT_TRUE(fx.defs.contains(kScratchGPR3));
    EXPECT_TRUE(emitTimeScratchClobber(MInstr{MOpcode::CmpRI, {x(PhysReg::X0), imm(4096)}}));
    EXPECT_TRUE(
        emitTimeScratchClobber(MInstr{MOpcode::AndRI, {x(PhysReg::X0), x(PhysReg::X1), imm(5)}}));
    EXPECT_TRUE(emitTimeScratchClobber(MInstr{MOpcode::StrRegFpImm, {x(PhysReg::X0), imm(-1000)}}));
    EXPECT_TRUE(emitTimeScratchClobber(MInstr{MOpcode::AddFpImm, {x(PhysReg::X0), imm(-5000)}}));
    EXPECT_TRUE(emitTimeScratchClobber(
        MInstr{MOpcode::StpRegFpImm, {x(PhysReg::X0), x(PhysReg::X1), imm(-520)}}));
}

TEST(AArch64InstrEffects, PeepholeClassifiersMatchRoleTable) {
    // Hand-built shapes covering every operand convention.
    const MInstr samples[] = {
        MInstr{MOpcode::MovRR, {x(PhysReg::X0), x(PhysReg::X1)}},
        MInstr{MOpcode::MovRI, {x(PhysReg::X0), imm(1)}},
        MInstr{MOpcode::AddRRR, {x(PhysReg::X0), x(PhysReg::X1), x(PhysReg::X2)}},
        MInstr{MOpcode::AddRRRLsl, {x(PhysReg::X0), x(PhysReg::X1), x(PhysReg::X2), imm(3)}},
        MInstr{MOpcode::MAddRRRR, {x(PhysReg::X0), x(PhysReg::X1), x(PhysReg::X2), x(PhysReg::X3)}},
        MInstr{MOpcode::CmpRR, {x(PhysReg::X1), x(PhysReg::X2)}},
        MInstr{MOpcode::Cset, {x(PhysReg::X1), MOperand::condOp("eq")}},
        MInstr{MOpcode::LdrRegBaseRegLsl, {x(PhysReg::X0), x(PhysReg::X1), x(PhysReg::X2), imm(3)}},
        MInstr{MOpcode::StrRegBaseImm, {x(PhysReg::X0), x(PhysReg::X1), imm(8)}},
        MInstr{MOpcode::LdpRegFpImm, {x(PhysReg::X0), x(PhysReg::X1), imm(-16)}},
        MInstr{MOpcode::StpRegFpImm, {x(PhysReg::X0), x(PhysReg::X1), imm(-16)}},
        MInstr{MOpcode::Cbz, {x(PhysReg::X0), MOperand::labelOp("L")}},
        MInstr{MOpcode::Tbnz, {x(PhysReg::X0), MOperand::labelOp("L"), imm(3)}},
        MInstr{MOpcode::Blr, {x(PhysReg::X0)}},
        MInstr{MOpcode::FCsel,
               {x(PhysReg::V0), x(PhysReg::V1), x(PhysReg::V2), MOperand::condOp("gt")}},
        MInstr{MOpcode::SCvtF, {x(PhysReg::V0), x(PhysReg::X1)}},
        MInstr{MOpcode::AddPageOff, {x(PhysReg::X0), x(PhysReg::X0), MOperand::labelOp("sym")}},
    };
    for (const MInstr &mi : samples) {
        for (std::size_t idx = 0; idx < mi.ops.size(); ++idx) {
            if (mi.ops[idx].kind != MOperand::Kind::Reg)
                continue;
            const auto expected = ra::operandRoles(mi, idx);
            const auto actual = peephole::classifyOperand(mi, idx);
            EXPECT_EQ(actual.first, expected.first);
            EXPECT_EQ(actual.second, expected.second);
            // usesReg/definesReg are per register: a register named by two
            // operands (e.g. `add x0, x0, :lo12:sym`) carries both roles.
            bool anyUse = false;
            bool anyDef = false;
            for (std::size_t other = 0; other < mi.ops.size(); ++other) {
                if (mi.ops[other].kind != MOperand::Kind::Reg ||
                    mi.ops[other].reg.idOrPhys != mi.ops[idx].reg.idOrPhys ||
                    mi.ops[other].reg.cls != mi.ops[idx].reg.cls)
                    continue;
                const auto roles = ra::operandRoles(mi, other);
                anyUse = anyUse || roles.first;
                anyDef = anyDef || roles.second;
            }
            EXPECT_EQ(peephole::usesReg(mi, mi.ops[idx]), anyUse);
            EXPECT_EQ(peephole::definesReg(mi, mi.ops[idx]), anyDef);
        }
    }
}

TEST(AArch64InstrEffects, EveryCorpusOperandIsClassifiedConsistently) {
    for (bool optimize : {false, true}) {
        std::size_t operandsChecked = 0;
        forEachLoweredFunction(kCorpus, optimize, [&](const MFunction &fn) {
            for (const auto &bb : fn.blocks) {
                for (const auto &mi : bb.instrs) {
                    // Roles must exist for every register operand of every
                    // instruction the pipeline produces ...
                    const InstrEffects fx = effectsOf(mi, darwinTarget());
                    (void)fx;
                    for (std::size_t idx = 0; idx < mi.ops.size(); ++idx) {
                        if (mi.ops[idx].kind != MOperand::Kind::Reg)
                            continue;
                        const auto expected = ra::operandRoles(mi, idx);
                        const auto actual = peephole::classifyOperand(mi, idx);
                        EXPECT_EQ(actual.first, expected.first);
                        EXPECT_EQ(actual.second, expected.second);
                        ++operandsChecked;
                    }
                    // ... and the "first def" convenience query must name a def.
                    if (const auto def = peephole::getDefinedReg(mi)) {
                        bool found = false;
                        for (std::size_t idx = 0; idx < mi.ops.size(); ++idx) {
                            if (mi.ops[idx].kind == MOperand::Kind::Reg && mi.ops[idx].reg.isPhys &&
                                ra::operandRoles(mi, idx).second &&
                                mi.ops[idx].reg.idOrPhys == def->reg.idOrPhys)
                                found = true;
                        }
                        EXPECT_TRUE(found);
                    }
                }
            }
        });
        EXPECT_GT(operandsChecked, 40u);
    }
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, &argv);
    return zanna_test::run_all_tests();
}
