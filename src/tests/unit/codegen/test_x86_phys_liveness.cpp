//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/codegen/test_x86_phys_liveness.cpp
// Purpose: Pins the x86-64 physical liveness every post-RA rewrite reads
//          (computePhysLiveness) and the exit-live seed built on it
//          (blockExitLive): reads in a successor keep a register live, a
//          redefinition kills it, call clobbers and argument reads come from
//          the effects model, a loop-carried register is live around the back
//          edge, a return reads the result registers, and the block-local DCE
//          removes a definition no successor reads while keeping one that is.
// Key invariants:
//   - The solver reads only effectsOf() and MirCfg; nothing else feeds the
//     exit-live set.
// Ownership/Lifetime: Standalone test binary.
// Links: src/codegen/x86_64/PhysLiveness.hpp, src/codegen/x86_64/Peephole.hpp,
//        docs/internals/backend-codegen-review-2026-09.md (Phase 3 C8)
//
//===----------------------------------------------------------------------===//

#include "tests/TestHarness.hpp"

#include "codegen/x86_64/MachineIR.hpp"
#include "codegen/x86_64/Peephole.hpp"
#include "codegen/x86_64/PhysLiveness.hpp"
#include "codegen/x86_64/TargetX64.hpp"

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

Operand lbl(const std::string &name) {
    return OpLabel{name};
}

MInstr movri(PhysReg dst, int64_t val) {
    return MInstr{MOpcode::MOVri, {gpr(dst), imm(val)}};
}

MInstr addrr(PhysReg dst, PhysReg src) {
    return MInstr{MOpcode::ADDrr, {gpr(dst), gpr(src)}};
}

MInstr jmp(const std::string &target) {
    return MInstr{MOpcode::JMP, {lbl(target)}};
}

MInstr jcc(const std::string &target) {
    return MInstr{MOpcode::JCC, {imm(4), lbl(target)}};
}

MInstr ret() {
    return MInstr{MOpcode::RET, {}};
}

MBasicBlock block(const std::string &name, std::vector<MInstr> instrs) {
    MBasicBlock bb;
    bb.label = name;
    bb.instructions = std::move(instrs);
    return bb;
}

MFunction function(std::vector<MBasicBlock> blocks) {
    MFunction fn;
    fn.name = "f";
    fn.blocks = std::move(blocks);
    return fn;
}

bool has(PhysRegMask mask, PhysReg reg) {
    return (mask & physRegBit(reg)) != 0;
}

const TargetInfo &target() {
    return sysvTarget();
}

} // namespace

TEST(X86PhysLiveness, SuccessorReadKeepsRegisterLiveAcrossTheEdge) {
    MFunction fn = function({
        block("entry", {movri(PhysReg::RAX, 1), jmp("next")}),
        block("next", {addrr(PhysReg::RBX, PhysReg::RAX), ret()}),
    });
    const PhysLiveness lv = computePhysLiveness(fn, target());
    ASSERT_EQ(lv.liveOut.size(), 2u);
    EXPECT_TRUE(has(lv.liveOut[0], PhysReg::RAX));
    EXPECT_TRUE(has(lv.liveIn[1], PhysReg::RAX));
    EXPECT_TRUE(has(lv.liveIn[1], PhysReg::RBX));  // read-modify-write
    EXPECT_FALSE(has(lv.liveIn[0], PhysReg::RAX)); // written before read
}

TEST(X86PhysLiveness, SuccessorRedefinitionKillsTheRegister) {
    MFunction fn = function({
        block("entry", {movri(PhysReg::RAX, 1), jmp("next")}),
        block("next", {movri(PhysReg::RAX, 2), ret()}),
    });
    const PhysLiveness lv = computePhysLiveness(fn, target());
    EXPECT_FALSE(has(lv.liveOut[0], PhysReg::RAX));
    // The return still reads XMM0 through the effects model.
    EXPECT_TRUE(has(lv.liveOut[0], PhysReg::XMM0));
}

TEST(X86PhysLiveness, CallClobbersCallerSavedAndReadsArguments) {
    MFunction fn = function({
        block("entry",
              {movri(PhysReg::RDI, 7), MInstr{MOpcode::CALL, {lbl("callee")}}, jmp("next")}),
        block("next", {MInstr{MOpcode::MOVrr, {gpr(PhysReg::RBX), gpr(PhysReg::R10)}}, ret()}),
    });
    const PhysLiveness lv = computePhysLiveness(fn, target());
    // R10 is read by the successor but the call clobbers it: live-out of
    // entry, not live-in.
    EXPECT_TRUE(has(lv.liveOut[0], PhysReg::R10));
    EXPECT_FALSE(has(lv.liveIn[0], PhysReg::R10));
    // RDI is an argument read of the call, defined by the mov before it.
    EXPECT_FALSE(has(lv.liveIn[0], PhysReg::RDI));
    // RSI and RCX are argument reads the block never defines: live-in.
    EXPECT_TRUE(has(lv.liveIn[0], PhysReg::RSI));
    EXPECT_TRUE(has(lv.liveIn[0], PhysReg::RCX));
}

TEST(X86PhysLiveness, CallWithARecordedArgumentMaskReadsOnlyThoseRegisters) {
    // Call lowering records the argument registers it marshalled; a CALL
    // without a mask (hand-built here) reads every argument register.
    MInstr masked{MOpcode::CALL, {lbl("callee")}};
    masked.callArgMask = physRegBit(PhysReg::RDI);
    const InstrEffects fx = effectsOf(masked, target());
    EXPECT_TRUE(has(fx.uses, PhysReg::RDI));
    EXPECT_FALSE(has(fx.uses, PhysReg::RSI));
    EXPECT_FALSE(has(fx.uses, PhysReg::RAX));
    EXPECT_TRUE(has(fx.uses, PhysReg::RSP));
    EXPECT_TRUE(has(fx.defs, PhysReg::RAX));
    const InstrEffects unknown = effectsOf(MInstr{MOpcode::CALL, {lbl("callee")}}, target());
    EXPECT_TRUE(has(unknown.uses, PhysReg::RSI));
    EXPECT_TRUE(has(unknown.uses, PhysReg::RAX));

    MFunction fn = function({
        block("entry", {movri(PhysReg::RDI, 7), masked, jmp("next")}),
        block("next", {ret()}),
    });
    const PhysLiveness lv = computePhysLiveness(fn, target());
    EXPECT_FALSE(has(lv.liveIn[0], PhysReg::RDI));
    EXPECT_FALSE(has(lv.liveIn[0], PhysReg::RSI));
}

TEST(X86PhysLiveness, TrapBlockCallDoesNotKeepArgumentRegistersLiveAroundALoop) {
    // A shared overflow trap block (`call rt_trap_ovf; ud2`, no arguments) is
    // a successor of every checked add. Its call must not make RDI..R9 live
    // across the loop, or the allocator could never use a caller-saved
    // register in it.
    MInstr trapCall{MOpcode::CALL, {lbl("rt_trap_ovf")}};
    trapCall.callArgMask = 0;
    MFunction fn = function({
        block("entry", {movri(PhysReg::RBX, 0), jmp("loop")}),
        block("loop", {addrr(PhysReg::RBX, PhysReg::RBX), jcc("trap"), jcc("loop"), jmp("exit")}),
        block("exit", {ret()}),
        block("trap", {trapCall, MInstr{MOpcode::UD2, {}}}),
    });
    const PhysLiveness lv = computePhysLiveness(fn, target());
    for (PhysReg reg : {PhysReg::RDI,
                        PhysReg::RSI,
                        PhysReg::RDX,
                        PhysReg::RCX,
                        PhysReg::R8,
                        PhysReg::R9,
                        PhysReg::XMM1}) {
        EXPECT_FALSE(has(lv.liveIn[1], reg));
        EXPECT_FALSE(has(lv.liveOut[1], reg));
        EXPECT_FALSE(has(lv.liveIn[3], reg));
    }
    EXPECT_TRUE(has(lv.liveIn[1], PhysReg::RBX));
}

TEST(X86PhysLiveness, ReturnReadsTheResultRegisters) {
    MFunction fn = function({block("entry", {ret()})});
    const PhysLiveness lv = computePhysLiveness(fn, target());
    EXPECT_TRUE(has(lv.liveIn[0], PhysReg::RAX));
    EXPECT_TRUE(has(lv.liveIn[0], PhysReg::XMM0));
    EXPECT_FALSE(has(lv.liveIn[0], PhysReg::RBX));
}

TEST(X86PhysLiveness, LoopCarriedRegisterIsLiveAroundTheBackEdge) {
    MFunction fn = function({
        block("entry", {movri(PhysReg::RBX, 0), jmp("head")}),
        block("head", {addrr(PhysReg::RBX, PhysReg::RCX), jcc("head"), jmp("exit")}),
        block("exit", {ret()}),
    });
    const PhysLiveness lv = computePhysLiveness(fn, target());
    EXPECT_TRUE(has(lv.liveOut[1], PhysReg::RBX));
    EXPECT_TRUE(has(lv.liveIn[1], PhysReg::RBX));
    EXPECT_TRUE(has(lv.liveIn[1], PhysReg::RCX));
    EXPECT_TRUE(has(lv.liveIn[0], PhysReg::RCX)); // never defined: an entry live-in
    EXPECT_FALSE(has(lv.liveIn[0], PhysReg::RBX));
}

TEST(X86PhysLiveness, ExitLiveIsSolvedLiveOutPlusFrameRegisters) {
    MFunction fn = function({
        block("entry", {movri(PhysReg::RAX, 1), jmp("next")}),
        block("next", {movri(PhysReg::RAX, 2), ret()}),
    });
    const PhysLiveness lv = computePhysLiveness(fn, target());
    const PhysRegMask live = blockExitLive(fn, 0, target(), lv);
    EXPECT_TRUE(has(live, PhysReg::RSP));
    EXPECT_TRUE(has(live, PhysReg::RBP));
    EXPECT_TRUE(has(live, PhysReg::XMM0)); // read by the successor's ret
    EXPECT_FALSE(has(live, PhysReg::RAX)); // successor writes it first
    EXPECT_FALSE(has(live, PhysReg::RBX)); // nobody reads it
}

TEST(X86PhysLiveness, ExitLiveAddsReturnRegistersAtAFunctionExit) {
    MFunction fn = function({block("entry", {movri(PhysReg::RBX, 1), ret()})});
    const PhysLiveness lv = computePhysLiveness(fn, target());
    const PhysRegMask live = blockExitLive(fn, 0, target(), lv);
    EXPECT_TRUE(has(live, PhysReg::RAX));
    EXPECT_TRUE(has(live, PhysReg::XMM0));
    EXPECT_TRUE(has(live, PhysReg::RSP));
    EXPECT_FALSE(has(live, PhysReg::RBX));
}

TEST(X86PhysLiveness, BlockDceSeedsFromTheSolvedExitLiveSet) {
    // rcx is defined and never read by any successor: dead. rax is read by
    // the successor: kept. rdx is dead in the successor's shadow because the
    // successor redefines it before reading.
    MFunction fn = function({
        block(
            "entry",
            {movri(PhysReg::RAX, 1), movri(PhysReg::RCX, 5), movri(PhysReg::RDX, 9), jmp("next")}),
        block("next", {movri(PhysReg::RDX, 3), addrr(PhysReg::RAX, PhysReg::RDX), ret()}),
    });
    (void)runPeepholes(fn, target());

    const auto &entry = fn.blocks[0].instructions;
    bool definesRax = false;
    bool definesRcx = false;
    bool definesRdx = false;
    for (const auto &mi : entry) {
        if (mi.opcode != MOpcode::MOVri || mi.operands.empty())
            continue;
        const auto *dst = std::get_if<OpReg>(&mi.operands[0]);
        if (dst == nullptr)
            continue;
        definesRax = definesRax || static_cast<PhysReg>(dst->idOrPhys) == PhysReg::RAX;
        definesRcx = definesRcx || static_cast<PhysReg>(dst->idOrPhys) == PhysReg::RCX;
        definesRdx = definesRdx || static_cast<PhysReg>(dst->idOrPhys) == PhysReg::RDX;
    }
    EXPECT_TRUE(definesRax);
    EXPECT_FALSE(definesRcx);
    EXPECT_FALSE(definesRdx);
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, &argv);
    return zanna_test::run_all_tests();
}
