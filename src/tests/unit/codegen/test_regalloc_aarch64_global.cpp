//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/codegen/test_regalloc_aarch64_global.cpp
// Purpose: Pins the AArch64 function-wide allocator on MIR shapes: a
//          loop-carried parameter keeps one register and the loop has no
//          frame access; a value live across a call lands in a callee-saved
//          register that is published; a marshalled argument register and an
//          ABI live-in stay out of the pool while occupied; pressure spills
//          the lightest values, reloads them at every use, and shares slots;
//          a swap cycle lowers to three moves; a value live across
//          `rt_native_eh_push` is memory-homed and a copy between two such
//          values routes through x17; FPR values follow the same rules;
//          allocation is deterministic; 500 simultaneously live values
//          allocate and verify; identity moves disappear.
// Key invariants:
//   - Every allocated function passes the PostRA verifier.
// Ownership/Lifetime: Standalone test binary.
// Links: src/codegen/aarch64/ra/GlobalAllocator.hpp,
//        docs/internals/backend-codegen-review-2026-09.md (Phase 3 C5)
//
//===----------------------------------------------------------------------===//

#include "tests/TestHarness.hpp"

#include "codegen/aarch64/MirVerify.hpp"
#include "codegen/aarch64/TargetAArch64.hpp"
#include "codegen/aarch64/passes/LoweringPass.hpp"
#include "codegen/aarch64/passes/PassManager.hpp"
#include "codegen/aarch64/ra/GlobalAllocator.hpp"
#include "il/io/Parser.hpp"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace zanna::codegen::aarch64;
using zanna::codegen::aarch64::ra::GlobalAllocator;

namespace {

MOperand v(uint16_t id, RegClass cls = RegClass::GPR) {
    return MOperand::vregOp(cls, id);
}

MOperand x(PhysReg r) {
    return MOperand::regOp(r);
}

MOperand imm(long long value) {
    return MOperand::immOp(value);
}

MOperand label(const char *name) {
    return MOperand::labelOp(name);
}

MInstr ins(MOpcode opc, std::vector<MOperand> ops) {
    return MInstr{opc, std::move(ops)};
}

MBasicBlock block(const char *name, std::vector<MInstr> instrs) {
    MBasicBlock bb;
    bb.name = name;
    bb.instrs = std::move(instrs);
    return bb;
}

MFunction function(std::vector<MBasicBlock> blocks) {
    MFunction fn;
    fn.name = "f";
    fn.blocks = std::move(blocks);
    return fn;
}

const TargetInfo &target() {
    return darwinTarget();
}

/// @brief Verify @p fn at PostRA, printing the diagnostics on failure.
bool verifiesPostRA(const MFunction &fn) {
    passes::Diagnostics diags;
    const bool ok = verifyMir(fn, VerifyStage::PostRA, target(), diags);
    if (!ok) {
        diags.flush(std::cerr, &std::cerr);
        std::cerr << toString(fn);
    }
    return ok;
}

std::size_t countOpcode(const MFunction &fn, MOpcode opc) {
    std::size_t n = 0;
    for (const auto &b : fn.blocks)
        for (const auto &mi : b.instrs)
            if (mi.opc == opc)
                ++n;
    return n;
}

std::size_t countFrameAccesses(const MBasicBlock &bb) {
    std::size_t n = 0;
    for (const auto &mi : bb.instrs) {
        if (mi.opc == MOpcode::LdrRegFpImm || mi.opc == MOpcode::StrRegFpImm ||
            mi.opc == MOpcode::LdrFprFpImm || mi.opc == MOpcode::StrFprFpImm)
            ++n;
    }
    return n;
}

bool hasVirtualOperand(const MFunction &fn) {
    for (const auto &b : fn.blocks)
        for (const auto &mi : b.instrs)
            for (const auto &op : mi.ops)
                if (op.kind == MOperand::Kind::Reg && !op.reg.isPhys)
                    return true;
    return false;
}

bool isCalleeSavedGPR(PhysReg r) {
    return std::find(target().calleeSavedGPR.begin(), target().calleeSavedGPR.end(), r) !=
           target().calleeSavedGPR.end();
}

il::core::Module parseIL(const std::string &src) {
    std::istringstream ss(src);
    il::core::Module mod;
    if (!il::io::Parser::parse(ss, mod))
        return {};
    return mod;
}

} // namespace

// ---------------------------------------------------------------------------
// Shapes
// ---------------------------------------------------------------------------

TEST(Arm64GlobalRegAlloc, LoopCarriedParameterStaysInOneRegister) {
    const char *const src = R"(il 0.3.0
func @main() -> i64 {
entry:
  br loop(0, 0)
loop(%s: i64, %i: i64):
  %done = scmp_ge %i, 10
  cbr %done, exit(%s), body(%s, %i)
body(%s0: i64, %i0: i64):
  %s1 = iadd.ovf %s0, %i0
  %i1 = iadd.ovf %i0, 1
  br loop(%s1, %i1)
exit(%r: i64):
  ret %r
}
)";
    il::core::Module mod = parseIL(src);
    ASSERT_FALSE(mod.functions.empty());
    passes::AArch64Module module;
    module.ilMod = &mod;
    module.ti = &target();
    passes::LoweringPass lowering;
    passes::Diagnostics diags;
    ASSERT_TRUE(lowering.run(module, diags));
    ASSERT_EQ(module.mir.size(), 1u);
    MFunction &fn = module.mir[0];

    (void)ra::allocateGlobal(fn, target());

    EXPECT_TRUE(verifiesPostRA(fn));
    EXPECT_FALSE(hasVirtualOperand(fn));
    EXPECT_EQ(countOpcode(fn, MOpcode::ParallelCopy), 0u);
    // No value goes through memory anywhere in the loop.
    for (const auto &bb : fn.blocks)
        EXPECT_EQ(countFrameAccesses(bb), 0u);
    EXPECT_TRUE(fn.frame.spills.empty());
}

TEST(Arm64GlobalRegAlloc, CallCrossingValueGetsACalleeSavedRegister) {
    MFunction fn = function({block("entry",
                                   {ins(MOpcode::MovRI, {v(1), imm(5)}),
                                    ins(MOpcode::Bl, {label("callee")}),
                                    ins(MOpcode::MovRR, {x(PhysReg::X0), v(1)}),
                                    ins(MOpcode::Ret, {})})});
    fn.isLeaf = false;
    GlobalAllocator alloc(fn, target());
    const auto stats = alloc.run();
    EXPECT_EQ(stats.spilled, 0u);
    const PhysReg r = alloc.assignedRegister(1);
    EXPECT_TRUE(isCalleeSavedGPR(r));
    EXPECT_NE(std::find(fn.savedGPRs.begin(), fn.savedGPRs.end(), r), fn.savedGPRs.end());
    EXPECT_TRUE(verifiesPostRA(fn));
    // A value that does not cross the call takes a caller-saved register.
    MFunction leaf = function({block("entry",
                                     {ins(MOpcode::MovRI, {v(1), imm(5)}),
                                      ins(MOpcode::MovRR, {x(PhysReg::X0), v(1)}),
                                      ins(MOpcode::Ret, {})})});
    GlobalAllocator leafAlloc(leaf, target());
    (void)leafAlloc.run();
    EXPECT_FALSE(isCalleeSavedGPR(leafAlloc.assignedRegister(1)));
    EXPECT_TRUE(leaf.savedGPRs.empty());
}

TEST(Arm64GlobalRegAlloc, MarshalledArgumentRegisterStaysOutOfThePool) {
    // x0 holds the marshalled argument from the move to the call; v2 is
    // defined and used in between and must not land in x0.
    MFunction fn = function({block("entry",
                                   {ins(MOpcode::MovRI, {v(1), imm(1)}),
                                    ins(MOpcode::MovRR, {x(PhysReg::X0), v(1)}),
                                    ins(MOpcode::MovRI, {v(2), imm(7)}),
                                    ins(MOpcode::StrRegFpImm, {v(2), imm(-8)}),
                                    ins(MOpcode::Bl, {label("callee")}),
                                    ins(MOpcode::Ret, {})})});
    fn.frame.locals.push_back(MFunction::StackLocal{1, 8, 8, -8});
    GlobalAllocator alloc(fn, target());
    (void)alloc.run();
    EXPECT_NE(alloc.assignedRegister(2), PhysReg::X0);
    EXPECT_NE(alloc.assignedRegister(2), PhysReg::SP);
    EXPECT_TRUE(verifiesPostRA(fn));
}

TEST(Arm64GlobalRegAlloc, AbiLiveInIsNeverAllocatedWhileItIsRead) {
    // x0 and x1 are incoming arguments read after v5 is defined.
    MFunction fn = function({block("entry",
                                   {ins(MOpcode::MovRI, {v(5), imm(3)}),
                                    ins(MOpcode::MovRR, {v(1), x(PhysReg::X0)}),
                                    ins(MOpcode::AddRRR, {v(2), v(1), v(5)}),
                                    ins(MOpcode::AddRRR, {v(3), v(2), x(PhysReg::X1)}),
                                    ins(MOpcode::MovRR, {x(PhysReg::X0), v(3)}),
                                    ins(MOpcode::Ret, {})})});
    GlobalAllocator alloc(fn, target());
    (void)alloc.run();
    EXPECT_NE(alloc.assignedRegister(5), PhysReg::X0);
    EXPECT_NE(alloc.assignedRegister(5), PhysReg::X1);
    EXPECT_NE(alloc.assignedRegister(2), PhysReg::X1);
    // v1 takes x0 by hint: the move becomes an identity and disappears.
    EXPECT_EQ(alloc.assignedRegister(1), PhysReg::X0);
    EXPECT_TRUE(verifiesPostRA(fn));
}

TEST(Arm64GlobalRegAlloc, PressureSpillsAndReloadsAtEveryUse) {
    // 40 values defined up front and consumed one by one: more than the
    // allocatable GPR file, so the lightest ones spill.
    MFunction fn = function({block("entry", {})});
    auto &bb = fn.blocks[0];
    const int n = 40;
    for (int i = 1; i <= n; ++i)
        bb.instrs.push_back(ins(MOpcode::MovRI, {v(static_cast<uint16_t>(i)), imm(i)}));
    uint16_t acc = 1;
    uint16_t next = n + 1;
    for (int i = 2; i <= n; ++i) {
        bb.instrs.push_back(ins(MOpcode::AddRRR, {v(next), v(acc), v(static_cast<uint16_t>(i))}));
        acc = next++;
    }
    bb.instrs.push_back(ins(MOpcode::MovRR, {x(PhysReg::X0), v(acc)}));
    bb.instrs.push_back(ins(MOpcode::Ret, {}));

    GlobalAllocator alloc(fn, target());
    const auto stats = alloc.run();
    EXPECT_GT(stats.spilled, 0u);
    EXPECT_GT(stats.reloads, 0u);
    EXPECT_GT(stats.spillStores, 0u);
    EXPECT_GT(stats.spillSlots, 0u);
    EXPECT_FALSE(hasVirtualOperand(fn));
    EXPECT_TRUE(verifiesPostRA(fn));
    // Every spilled value has a slot inside the finalized frame.
    for (const auto &slot : fn.frame.spills) {
        EXPECT_LT(slot.offset, 0);
        EXPECT_GE(slot.offset, -fn.frame.totalBytes);
    }
    // The register file is used before anything spills: at least 20 distinct
    // registers appear.
    std::vector<uint16_t> regs;
    for (const auto &mi : bb.instrs)
        for (const auto &op : mi.ops)
            if (op.kind == MOperand::Kind::Reg && op.reg.cls == RegClass::GPR)
                regs.push_back(op.reg.idOrPhys);
    std::sort(regs.begin(), regs.end());
    regs.erase(std::unique(regs.begin(), regs.end()), regs.end());
    EXPECT_GE(regs.size(), 20u);
}

TEST(Arm64GlobalRegAlloc, SpilledValuesWithDisjointRangesShareASlot) {
    // Two phases, each with 40 simultaneously live values; the second phase
    // starts after the first is dead, so its spills reuse the first's slots.
    MFunction fn = function({block("entry", {})});
    auto &bb = fn.blocks[0];
    uint16_t next = 1;
    for (int phase = 0; phase < 2; ++phase) {
        const uint16_t first = next;
        for (int i = 0; i < 40; ++i)
            bb.instrs.push_back(ins(MOpcode::MovRI, {v(next++), imm(i + phase * 100)}));
        uint16_t acc = first;
        for (int i = 1; i < 40; ++i) {
            bb.instrs.push_back(
                ins(MOpcode::AddRRR, {v(next), v(acc), v(static_cast<uint16_t>(first + i))}));
            acc = next++;
        }
        bb.instrs.push_back(
            ins(MOpcode::MovRR, {x(phase == 0 ? PhysReg::X1 : PhysReg::X0), v(acc)}));
    }
    bb.instrs.push_back(ins(MOpcode::Ret, {}));

    GlobalAllocator alloc(fn, target());
    const auto stats = alloc.run();
    EXPECT_GT(stats.spilled, stats.spillSlots);
    EXPECT_TRUE(verifiesPostRA(fn));
}

TEST(Arm64GlobalRegAlloc, SwapCycleLowersToThreeMoves) {
    MFunction fn = function({
        block("entry",
              {ins(MOpcode::MovRR, {v(1), x(PhysReg::X0)}),
               ins(MOpcode::MovRR, {v(2), x(PhysReg::X1)}),
               ins(MOpcode::ParallelCopy, {v(10), v(1), v(11), v(2)}),
               ins(MOpcode::Br, {label("next")})}),
        block("next",
              {ins(MOpcode::ParallelCopy, {v(10), v(11), v(11), v(10)}), // swap
               ins(MOpcode::AddRRR, {v(3), v(10), v(11)}),
               ins(MOpcode::SubRRR, {v(4), v(3), v(11)}),
               ins(MOpcode::MovRR, {x(PhysReg::X0), v(4)}),
               ins(MOpcode::Ret, {})}),
    });
    GlobalAllocator alloc(fn, target());
    (void)alloc.run();
    EXPECT_TRUE(verifiesPostRA(fn));
    EXPECT_EQ(countOpcode(fn, MOpcode::ParallelCopy), 0u);
    // The swap in `next` is exactly three register moves and no memory op.
    const auto &next = fn.blocks[1];
    std::size_t moves = 0;
    for (const auto &mi : next.instrs) {
        if (mi.opc == MOpcode::MovRR)
            ++moves;
    }
    EXPECT_EQ(moves, 3u);
    EXPECT_EQ(countFrameAccesses(next), 0u);
}

TEST(Arm64GlobalRegAlloc, EhPushCrossingValuesAreMemoryHomed) {
    MFunction fn = function({block("entry",
                                   {ins(MOpcode::MovRI, {v(1), imm(7)}),
                                    ins(MOpcode::MovRI, {v(2), imm(9)}),
                                    ins(MOpcode::Bl, {label("rt_native_eh_push")}),
                                    ins(MOpcode::MovRR, {v(3), x(PhysReg::X0)}),
                                    ins(MOpcode::AddRRR, {v(4), v(1), v(3)}),
                                    ins(MOpcode::AddRRR, {v(5), v(4), v(2)}),
                                    ins(MOpcode::MovRR, {x(PhysReg::X0), v(5)}),
                                    ins(MOpcode::Ret, {})})});
    fn.isLeaf = false;
    GlobalAllocator alloc(fn, target());
    const auto stats = alloc.run();
    EXPECT_EQ(alloc.assignedRegister(1), PhysReg::SP);
    EXPECT_EQ(alloc.assignedRegister(2), PhysReg::SP);
    EXPECT_NE(alloc.spillOffset(1), 0);
    EXPECT_NE(alloc.spillOffset(2), 0);
    // v3 is defined after the push: a plain register.
    EXPECT_NE(alloc.assignedRegister(3), PhysReg::SP);
    EXPECT_EQ(stats.spilled, 2u);
    EXPECT_TRUE(verifiesPostRA(fn));
    // Both values are stored before the push and reloaded at their uses.
    EXPECT_GE(countOpcode(fn, MOpcode::StrRegFpImm), 2u);
    EXPECT_GE(countOpcode(fn, MOpcode::LdrRegFpImm), 2u);
}

TEST(Arm64GlobalRegAlloc, MemoryToMemoryCopyRoutesThroughX17) {
    // Values live across the push are memory-homed; a parallel copy between
    // two of them whose ranges overlap (v1 stays live after the copy, so it
    // cannot share v10's slot) is a mem-to-mem move through the reserved x17.
    MFunction fn = function({
        block("entry",
              {ins(MOpcode::MovRI, {v(1), imm(7)}),
               ins(MOpcode::MovRI, {v(2), imm(9)}),
               ins(MOpcode::Bl, {label("rt_native_eh_push")}),
               ins(MOpcode::ParallelCopy, {v(10), v(1), v(11), v(2)}),
               ins(MOpcode::Br, {label("next")})}),
        block("next",
              {ins(MOpcode::Bl, {label("rt_native_eh_push")}),
               ins(MOpcode::AddRRR, {v(3), v(10), v(11)}),
               ins(MOpcode::AddRRR, {v(5), v(3), v(1)}),
               ins(MOpcode::MovRR, {x(PhysReg::X0), v(5)}),
               ins(MOpcode::Ret, {})}),
    });
    fn.isLeaf = false;
    GlobalAllocator alloc(fn, target());
    (void)alloc.run();
    EXPECT_EQ(alloc.assignedRegister(1), PhysReg::SP);
    EXPECT_EQ(alloc.assignedRegister(10), PhysReg::SP);
    EXPECT_NE(alloc.spillOffset(1), alloc.spillOffset(10));
    EXPECT_TRUE(verifiesPostRA(fn));
    bool sawX17Load = false;
    bool sawX17Store = false;
    for (const auto &mi : fn.blocks[0].instrs) {
        if (mi.opc == MOpcode::LdrRegFpImm &&
            mi.ops[0].reg.idOrPhys == static_cast<uint16_t>(PhysReg::X17))
            sawX17Load = true;
        if (mi.opc == MOpcode::StrRegFpImm &&
            mi.ops[0].reg.idOrPhys == static_cast<uint16_t>(PhysReg::X17))
            sawX17Store = true;
    }
    EXPECT_TRUE(sawX17Load);
    EXPECT_TRUE(sawX17Store);

    // When the copied value dies at the copy, the two memory homes are one
    // slot and the copy is an identity: no instruction at all.
    MFunction shared = function({
        block("entry",
              {ins(MOpcode::MovRI, {v(1), imm(7)}),
               ins(MOpcode::Bl, {label("rt_native_eh_push")}),
               ins(MOpcode::ParallelCopy, {v(10), v(1)}),
               ins(MOpcode::Br, {label("next")})}),
        block("next",
              {ins(MOpcode::Bl, {label("rt_native_eh_push")}),
               ins(MOpcode::MovRR, {x(PhysReg::X0), v(10)}),
               ins(MOpcode::Ret, {})}),
    });
    shared.isLeaf = false;
    GlobalAllocator sharedAlloc(shared, target());
    const auto stats = sharedAlloc.run();
    EXPECT_EQ(sharedAlloc.spillOffset(1), sharedAlloc.spillOffset(10));
    EXPECT_EQ(stats.edgeMoves, 0u);
    EXPECT_TRUE(verifiesPostRA(shared));
}

TEST(Arm64GlobalRegAlloc, FprValuesFollowTheSameRules) {
    MFunction fn = function({block(
        "entry",
        {ins(MOpcode::FMovRR, {v(1, RegClass::FPR), x(PhysReg::V0)}),
         ins(MOpcode::Bl, {label("callee")}),
         ins(MOpcode::FAddRRR, {v(2, RegClass::FPR), v(1, RegClass::FPR), v(1, RegClass::FPR)}),
         ins(MOpcode::FMovRR, {x(PhysReg::V0), v(2, RegClass::FPR)}),
         ins(MOpcode::Ret, {})})});
    fn.isLeaf = false;
    GlobalAllocator alloc(fn, target());
    (void)alloc.run();
    const PhysReg r = alloc.assignedRegister(1);
    EXPECT_TRUE(r >= PhysReg::V8 && r <= PhysReg::V15);
    EXPECT_NE(std::find(fn.savedFPRs.begin(), fn.savedFPRs.end(), r), fn.savedFPRs.end());
    // v2 takes v0 by hint.
    EXPECT_EQ(alloc.assignedRegister(2), PhysReg::V0);
    EXPECT_TRUE(verifiesPostRA(fn));
}

TEST(Arm64GlobalRegAlloc, IdentityMovesDisappear) {
    MFunction fn = function({block("entry",
                                   {ins(MOpcode::MovRR, {v(1), x(PhysReg::X0)}),
                                    ins(MOpcode::AddRI, {v(2), v(1), imm(1)}),
                                    ins(MOpcode::MovRR, {v(3), v(2)}),
                                    ins(MOpcode::MovRR, {x(PhysReg::X0), v(3)}),
                                    ins(MOpcode::Ret, {})})});
    GlobalAllocator alloc(fn, target());
    (void)alloc.run();
    EXPECT_TRUE(verifiesPostRA(fn));
    // add x0, x0, #1 ; ret — every move was an identity.
    EXPECT_EQ(countOpcode(fn, MOpcode::MovRR), 0u);
    ASSERT_EQ(fn.blocks[0].instrs.size(), 2u);
    EXPECT_EQ(fn.blocks[0].instrs[0].opc, MOpcode::AddRI);
}

TEST(Arm64GlobalRegAlloc, AllocationIsDeterministic) {
    /// Build the pressure function of PressureSpillsAndReloadsAtEveryUse.
    const auto build = []() {
        MFunction fn = function({block("entry", {})});
        auto &bb = fn.blocks[0];
        for (int i = 1; i <= 40; ++i)
            bb.instrs.push_back(ins(MOpcode::MovRI, {v(static_cast<uint16_t>(i)), imm(i)}));
        uint16_t acc = 1;
        uint16_t next = 41;
        for (int i = 2; i <= 40; ++i) {
            bb.instrs.push_back(
                ins(MOpcode::AddRRR, {v(next), v(acc), v(static_cast<uint16_t>(i))}));
            acc = next++;
        }
        bb.instrs.push_back(ins(MOpcode::MovRR, {x(PhysReg::X0), v(acc)}));
        bb.instrs.push_back(ins(MOpcode::Ret, {}));
        return fn;
    };
    MFunction a = build();
    MFunction b = build();
    (void)ra::allocateGlobal(a, target());
    (void)ra::allocateGlobal(b, target());
    EXPECT_EQ(toString(a), toString(b));
}

TEST(Arm64GlobalRegAlloc, FiveHundredLiveValuesAllocateAndVerify) {
    MFunction fn = function({block("entry", {})});
    auto &bb = fn.blocks[0];
    const int n = 500;
    for (int i = 1; i <= n; ++i)
        bb.instrs.push_back(ins(MOpcode::MovRI, {v(static_cast<uint16_t>(i)), imm(i)}));
    uint16_t acc = 1;
    uint16_t next = n + 1;
    for (int i = 2; i <= n; ++i) {
        bb.instrs.push_back(ins(MOpcode::AddRRR, {v(next), v(acc), v(static_cast<uint16_t>(i))}));
        acc = next++;
    }
    bb.instrs.push_back(ins(MOpcode::MovRR, {x(PhysReg::X0), v(acc)}));
    bb.instrs.push_back(ins(MOpcode::Ret, {}));
    EXPECT_NO_THROW((void)ra::allocateGlobal(fn, target()));
    EXPECT_FALSE(hasVirtualOperand(fn));
    EXPECT_TRUE(verifiesPostRA(fn));
}

TEST(Arm64GlobalRegAlloc, ThreeSpilledOperandsUseThreeTemporaries) {
    // madd v4, v1, v2, v3 with every input memory-homed (across an EH push)
    // reloads into three distinct registers.
    MFunction fn = function({block("entry",
                                   {ins(MOpcode::MovRI, {v(1), imm(1)}),
                                    ins(MOpcode::MovRI, {v(2), imm(2)}),
                                    ins(MOpcode::MovRI, {v(3), imm(3)}),
                                    ins(MOpcode::Bl, {label("rt_native_eh_push")}),
                                    ins(MOpcode::MAddRRRR, {v(4), v(1), v(2), v(3)}),
                                    ins(MOpcode::MovRR, {x(PhysReg::X0), v(4)}),
                                    ins(MOpcode::Ret, {})})});
    fn.isLeaf = false;
    GlobalAllocator alloc(fn, target());
    (void)alloc.run();
    EXPECT_TRUE(verifiesPostRA(fn));
    const MInstr *madd = nullptr;
    for (const auto &mi : fn.blocks[0].instrs)
        if (mi.opc == MOpcode::MAddRRRR)
            madd = &mi;
    ASSERT_TRUE(madd != nullptr);
    std::vector<uint16_t> inputs{
        madd->ops[1].reg.idOrPhys, madd->ops[2].reg.idOrPhys, madd->ops[3].reg.idOrPhys};
    std::sort(inputs.begin(), inputs.end());
    EXPECT_EQ(std::unique(inputs.begin(), inputs.end()), inputs.end());
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, &argv);
    return zanna_test::run_all_tests();
}
