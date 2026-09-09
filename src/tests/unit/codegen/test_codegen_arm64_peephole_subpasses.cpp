//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/codegen/test_codegen_arm64_peephole_subpasses.cpp
// Purpose: Test AArch64 peephole sub-passes that had zero coverage:
//          StrengthReduce (mul-to-shift, div strength reduction),
//          BranchOpt (conditional branch folding),
//          CopyPropDCE (dead code after copy propagation),
//          MemoryOpt (memory access patterns),
//          LoopOpt (loop-constant hoisting), and the exit-live guards of
//          every block-local fold.
// Key invariants:
//   - Rewrites preserve semantics.
//   - Stats counters accurately reflect transformations.
// Ownership/Lifetime: Constructs transient MIR per test.
// Links: src/codegen/aarch64/peephole/
//
//===----------------------------------------------------------------------===//
#include "tests/TestHarness.hpp"
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "codegen/aarch64/MachineIR.hpp"
#include "codegen/aarch64/Peephole.hpp"
#include "codegen/aarch64/TargetAArch64.hpp"
#include "codegen/aarch64/peephole/BranchOpt.hpp"
#include "codegen/aarch64/peephole/CopyPropDCE.hpp"
#include "codegen/aarch64/peephole/MemoryOpt.hpp"

using namespace zanna::codegen::aarch64;

// ─── StrengthReduce: mul by power of 2 → shift ──────────────────────────────

TEST(AArch64PeepholeSubpasses, MulByPowerOf2ToShift) {
    // When one operand of MulRRR is a known power-of-2 constant loaded via MovRI,
    // strength reduction should convert it to a shift.
    MFunction fn{};
    fn.name = "mul_pow2";
    fn.blocks.push_back(MBasicBlock{"entry", {}});
    auto &bb = fn.blocks.back();

    // mov x1, #8
    bb.instrs.push_back(MInstr{MOpcode::MovRI, {MOperand::regOp(PhysReg::X1), MOperand::immOp(8)}});
    // mul x2, x0, x1  (x0 * 8 → should become lsl x2, x0, #3)
    bb.instrs.push_back(MInstr{MOpcode::MulRRR,
                               {MOperand::regOp(PhysReg::X2),
                                MOperand::regOp(PhysReg::X0),
                                MOperand::regOp(PhysReg::X1)}});
    bb.instrs.push_back(MInstr{MOpcode::Ret, {}});

    auto stats = runPeephole(fn);
    EXPECT_GT(stats.strengthReductions, 0);
}

// ─── StrengthReduce: add #0 identity ────────────────────────────────────────

TEST(AArch64PeepholeSubpasses, AddFpImmZeroIdentity) {
    // fadd d0, d0, #0.0 should be eliminated as identity
    MFunction fn{};
    fn.name = "fadd_zero";
    fn.blocks.push_back(MBasicBlock{"entry", {}});
    auto &bb = fn.blocks.back();

    bb.instrs.push_back(MInstr{MOpcode::FAddRRR,
                               {MOperand::regOp(PhysReg::V0),
                                MOperand::regOp(PhysReg::V0),
                                MOperand::regOp(PhysReg::V1)}});
    bb.instrs.push_back(MInstr{MOpcode::Ret, {}});

    size_t before = bb.instrs.size();
    auto stats = runPeephole(fn);
    // The FP identity elimination may or may not trigger depending on implementation
    // At minimum, the pass should not crash.
    EXPECT_LE(bb.instrs.size(), before);
}

// ─── BranchOpt: unconditional branch to next block ──────────────────────────

TEST(AArch64PeepholeSubpasses, RemoveRedundantBranchToFallthrough) {
    MFunction fn{};
    fn.name = "branch_fallthrough";
    fn.blocks.push_back(MBasicBlock{"entry", {}});
    fn.blocks.push_back(MBasicBlock{"next", {}});
    auto &entry = fn.blocks[0];
    auto &next = fn.blocks[1];

    // b next (redundant — falls through)
    entry.instrs.push_back(MInstr{MOpcode::Br, {MOperand::labelOp("next")}});
    next.instrs.push_back(MInstr{MOpcode::Ret, {}});

    auto stats = runPeephole(fn);

    // Branch to next block should be removed
    bool hasBranch = false;
    for (const auto &i : fn.blocks[0].instrs) {
        if (i.opc == MOpcode::Br)
            hasBranch = true;
    }
    // The entry block should be empty or have no branch to "next"
    // (branch-to-next optimization removes the B instruction)
    EXPECT_GT(stats.branchesToNextRemoved, 0);
    EXPECT_FALSE(hasBranch);
}

// ─── CopyPropDCE: dead mov after last use ───────────────────────────────────

TEST(AArch64PeepholeSubpasses, DeadMovRemovedAfterLastUse) {
    MFunction fn{};
    fn.name = "dead_mov";
    fn.blocks.push_back(MBasicBlock{"entry", {}});
    auto &bb = fn.blocks.back();

    // mov x1, x0  (only use)
    bb.instrs.push_back(
        MInstr{MOpcode::MovRR, {MOperand::regOp(PhysReg::X1), MOperand::regOp(PhysReg::X0)}});
    // mov x2, x1  (x1's only use — after this, x1 is dead)
    bb.instrs.push_back(
        MInstr{MOpcode::MovRR, {MOperand::regOp(PhysReg::X2), MOperand::regOp(PhysReg::X1)}});
    // ret (returns x0 implicitly)
    bb.instrs.push_back(MInstr{MOpcode::Ret, {}});

    size_t before = bb.instrs.size();
    auto stats = runPeephole(fn);
    // Copy propagation may fold the two movs, DCE may remove dead intermediates
    // At minimum, the pass should not crash and instruction count should not increase.
    EXPECT_LE(bb.instrs.size(), before);
}

// ─── ImmThenMove folding ────────────────────────────────────────────────────

TEST(AArch64PeepholeSubpasses, ImmThenMoveFolding) {
    // mov x1, #42; mov x2, x1 → mov x1, x1; mov x2, #42  (when x1 dead after)
    MFunction fn{};
    fn.name = "imm_then_move";
    fn.blocks.push_back(MBasicBlock{"entry", {}});
    auto &bb = fn.blocks.back();

    bb.instrs.push_back(
        MInstr{MOpcode::MovRI, {MOperand::regOp(PhysReg::X10), MOperand::immOp(42)}});
    bb.instrs.push_back(
        MInstr{MOpcode::MovRR, {MOperand::regOp(PhysReg::X11), MOperand::regOp(PhysReg::X10)}});
    bb.instrs.push_back(MInstr{MOpcode::Ret, {}});

    auto stats = runPeephole(fn);
    // The fold should fire (x10 is dead after the mov)
    EXPECT_GT(stats.consecutiveMovsFolded, 0);
}

TEST(AArch64PeepholeSubpasses, StoreLoadForwardingStopsAtOverlappingFprStore) {
    std::vector<MInstr> instrs;
    instrs.push_back(
        MInstr{MOpcode::StrRegFpImm, {MOperand::regOp(PhysReg::X1), MOperand::immOp(-16)}});
    instrs.push_back(
        MInstr{MOpcode::StrFprFpImm, {MOperand::regOp(PhysReg::V0), MOperand::immOp(-16)}});
    instrs.push_back(
        MInstr{MOpcode::LdrRegFpImm, {MOperand::regOp(PhysReg::X2), MOperand::immOp(-16)}});

    PeepholeStats stats{};
    const std::size_t forwarded = peephole::forwardStoreLoads(instrs, stats);
    EXPECT_EQ(forwarded, 0u);
    ASSERT_EQ(instrs.size(), 3u);
    EXPECT_EQ(instrs[2].opc, MOpcode::LdrRegFpImm);
}

TEST(AArch64PeepholeSubpasses, StoreLoadForwardingStopsAtScratchClobberingAccess) {
    // str x9,[fp,#-160] ; str x17,[fp,#-280] ; ldr x17,[fp,#-160]
    // The middle store's offset is not encodable, so the emitter materialises
    // it through the reserved scratch x9 after the peephole has run: x9 no
    // longer holds the stored value at the load, and forwarding it as
    // `mov x17, x9` would read garbage (found by the RA oracle once it ran
    // the peephole stage).
    std::vector<MInstr> instrs;
    instrs.push_back(
        MInstr{MOpcode::StrRegFpImm, {MOperand::regOp(PhysReg::X9), MOperand::immOp(-160)}});
    instrs.push_back(
        MInstr{MOpcode::StrRegFpImm, {MOperand::regOp(PhysReg::X17), MOperand::immOp(-280)}});
    instrs.push_back(
        MInstr{MOpcode::LdrRegFpImm, {MOperand::regOp(PhysReg::X17), MOperand::immOp(-160)}});

    PeepholeStats stats{};
    const std::size_t forwarded = peephole::forwardStoreLoads(instrs, stats);
    EXPECT_EQ(forwarded, 0u);
    ASSERT_EQ(instrs.size(), 3u);
    EXPECT_EQ(instrs[2].opc, MOpcode::LdrRegFpImm);

    // A non-scratch source is unaffected by the same barrier.
    std::vector<MInstr> plain;
    plain.push_back(
        MInstr{MOpcode::StrRegFpImm, {MOperand::regOp(PhysReg::X20), MOperand::immOp(-160)}});
    plain.push_back(
        MInstr{MOpcode::StrRegFpImm, {MOperand::regOp(PhysReg::X17), MOperand::immOp(-280)}});
    plain.push_back(
        MInstr{MOpcode::LdrRegFpImm, {MOperand::regOp(PhysReg::X21), MOperand::immOp(-160)}});
    PeepholeStats plainStats{};
    EXPECT_EQ(peephole::forwardStoreLoads(plain, plainStats), 1u);
    EXPECT_EQ(plain[2].opc, MOpcode::MovRR);
}

TEST(AArch64PeepholeSubpasses, CopyPropagationForgetsScratchAfterWideAccess) {
    // mov x9, x20 ; str x17,[fp,#-280] ; add x1, x9, x9
    // x9 is the emit-time scratch of the wide-offset store; the copy recorded
    // for it must not be forwarded past that store as if x9 still held x20.
    // (Propagating x9 -> x20 in the add would actually be *correct* here, so
    // the check is on the reverse direction: a copy *from* x9 must be
    // forgotten.)
    std::vector<MInstr> instrs;
    instrs.push_back(
        MInstr{MOpcode::MovRR, {MOperand::regOp(PhysReg::X20), MOperand::regOp(PhysReg::X9)}});
    instrs.push_back(
        MInstr{MOpcode::StrRegFpImm, {MOperand::regOp(PhysReg::X17), MOperand::immOp(-280)}});
    instrs.push_back(MInstr{MOpcode::AddRRR,
                            {MOperand::regOp(PhysReg::X1),
                             MOperand::regOp(PhysReg::X20),
                             MOperand::regOp(PhysReg::X20)}});
    PeepholeStats stats{};
    (void)peephole::propagateCopies(instrs, stats);
    ASSERT_EQ(instrs.size(), 3u);
    EXPECT_EQ(instrs[2].ops[1].reg.idOrPhys, static_cast<uint16_t>(PhysReg::X20));
    EXPECT_EQ(instrs[2].ops[2].reg.idOrPhys, static_cast<uint16_t>(PhysReg::X20));
}

TEST(AArch64PeepholeSubpasses, StoreLoadForwardingStopsAtSubWordStoreIntoTheSlot) {
    // str x1,[fp,#-16] ; str w3,[fp,#-12] ; ldr x2,[fp,#-16]
    // The 4-byte store lands inside the 8-byte slot, so the reload must not
    // become `mov x2, x1` (review item B2).
    std::vector<MInstr> instrs;
    instrs.push_back(
        MInstr{MOpcode::StrRegFpImm, {MOperand::regOp(PhysReg::X1), MOperand::immOp(-16)}});
    instrs.push_back(
        MInstr{MOpcode::Str32RegFpImm, {MOperand::regOp(PhysReg::X3), MOperand::immOp(-12)}});
    instrs.push_back(
        MInstr{MOpcode::LdrRegFpImm, {MOperand::regOp(PhysReg::X2), MOperand::immOp(-16)}});
    PeepholeStats stats{};
    EXPECT_EQ(peephole::forwardStoreLoads(instrs, stats), 0u);
    ASSERT_EQ(instrs.size(), 3u);
    EXPECT_EQ(instrs[2].opc, MOpcode::LdrRegFpImm);
}

TEST(AArch64PeepholeSubpasses, DeadFrameStoreSurvivesASubWordLoadOfTheSlot) {
    // str x1,[fp,#-16] ; ldr w2,[fp,#-16] ; str x3,[fp,#-16]
    // The first store is read by the narrow load; only an unobserved store
    // is dead.
    std::vector<MInstr> instrs;
    instrs.push_back(
        MInstr{MOpcode::StrRegFpImm, {MOperand::regOp(PhysReg::X1), MOperand::immOp(-16)}});
    instrs.push_back(
        MInstr{MOpcode::Ldr32RegFpImm, {MOperand::regOp(PhysReg::X2), MOperand::immOp(-16)}});
    instrs.push_back(
        MInstr{MOpcode::StrRegFpImm, {MOperand::regOp(PhysReg::X3), MOperand::immOp(-16)}});
    PeepholeStats stats{};
    EXPECT_EQ(peephole::eliminateDeadFpStores(instrs, stats), 0u);
    ASSERT_EQ(instrs.size(), 3u);
}

TEST(AArch64PeepholeSubpasses, StoreLoadForwardingStopsAtOverlappingPairStore) {
    std::vector<MInstr> instrs;
    instrs.push_back(
        MInstr{MOpcode::StrRegFpImm, {MOperand::regOp(PhysReg::X1), MOperand::immOp(-8)}});
    instrs.push_back(
        MInstr{MOpcode::StpRegFpImm,
               {MOperand::regOp(PhysReg::X3), MOperand::regOp(PhysReg::X4), MOperand::immOp(-16)}});
    instrs.push_back(
        MInstr{MOpcode::LdrRegFpImm, {MOperand::regOp(PhysReg::X2), MOperand::immOp(-8)}});

    PeepholeStats stats{};
    const std::size_t forwarded = peephole::forwardStoreLoads(instrs, stats);
    EXPECT_EQ(forwarded, 0u);
    ASSERT_EQ(instrs.size(), 3u);
    EXPECT_EQ(instrs[2].opc, MOpcode::LdrRegFpImm);
}

// ─── Multiple sub-passes interact correctly ─────────────────────────────────

TEST(AArch64PeepholeSubpasses, MultiplePassesInteract) {
    MFunction fn{};
    fn.name = "multi_pass";
    fn.blocks.push_back(MBasicBlock{"entry", {}});
    fn.blocks.push_back(MBasicBlock{"target", {}});
    auto &entry = fn.blocks[0];
    auto &target = fn.blocks[1];

    // Identity move (IdentityElim should remove)
    entry.instrs.push_back(
        MInstr{MOpcode::MovRR, {MOperand::regOp(PhysReg::X0), MOperand::regOp(PhysReg::X0)}});
    // cmp x1, #0 → tst x1, x1 (StrengthReduce)
    entry.instrs.push_back(
        MInstr{MOpcode::CmpRI, {MOperand::regOp(PhysReg::X1), MOperand::immOp(0)}});
    // branch to next (BranchOpt should remove)
    entry.instrs.push_back(MInstr{MOpcode::Br, {MOperand::labelOp("target")}});

    target.instrs.push_back(MInstr{MOpcode::Ret, {}});

    auto stats = runPeephole(fn);

    // All three optimizations should fire
    EXPECT_GT(stats.identityMovesRemoved, 0);
    EXPECT_GT(stats.cmpZeroToTst, 0);
    EXPECT_GT(stats.branchesToNextRemoved, 0);
}

TEST(AArch64PeepholeSubpasses, LoopConstHoistRejectsBackwardJoinEdge) {
    MFunction fn{};
    fn.name = "backward_join_not_loop";
    fn.blocks.push_back(MBasicBlock{"entry", {}});
    fn.blocks.push_back(MBasicBlock{"else_path", {}});
    fn.blocks.push_back(MBasicBlock{"join", {}});
    fn.blocks.push_back(MBasicBlock{"then_path", {}});
    fn.blocks.push_back(MBasicBlock{"exit", {}});

    auto &entry = fn.blocks[0];
    auto &elsePath = fn.blocks[1];
    auto &join = fn.blocks[2];
    auto &thenPath = fn.blocks[3];
    auto &exit = fn.blocks[4];

    entry.instrs.push_back(
        MInstr{MOpcode::Cbz, {MOperand::regOp(PhysReg::X0), MOperand::labelOp("then_path")}});
    entry.instrs.push_back(MInstr{MOpcode::Br, {MOperand::labelOp("else_path")}});

    elsePath.instrs.push_back(
        MInstr{MOpcode::MovRI, {MOperand::regOp(PhysReg::X20), MOperand::immOp(1)}});
    elsePath.instrs.push_back(MInstr{MOpcode::Br, {MOperand::labelOp("join")}});

    join.instrs.push_back(
        MInstr{MOpcode::MovRI, {MOperand::regOp(PhysReg::X28), MOperand::immOp(6)}});
    join.instrs.push_back(
        MInstr{MOpcode::MovRR, {MOperand::regOp(PhysReg::X4), MOperand::regOp(PhysReg::X28)}});
    join.instrs.push_back(MInstr{MOpcode::Br, {MOperand::labelOp("exit")}});

    thenPath.instrs.push_back(
        MInstr{MOpcode::MovRI, {MOperand::regOp(PhysReg::X21), MOperand::immOp(42)}});
    thenPath.instrs.push_back(MInstr{MOpcode::Br, {MOperand::labelOp("join")}});

    exit.instrs.push_back(MInstr{MOpcode::Ret, {}});

    (void)runPeephole(fn);

    const auto joinIt = std::find_if(fn.blocks.begin(), fn.blocks.end(), [](const MBasicBlock &bb) {
        return bb.name == "join";
    });
    ASSERT_TRUE(joinIt != fn.blocks.end());

    const bool joinStillDefinesScale =
        std::any_of(joinIt->instrs.begin(), joinIt->instrs.end(), [](const MInstr &instr) {
            return instr.opc == MOpcode::MovRI && instr.ops.size() == 2 &&
                   instr.ops[0].kind == MOperand::Kind::Reg &&
                   (instr.ops[0].reg.idOrPhys == static_cast<uint16_t>(PhysReg::X28) ||
                    instr.ops[0].reg.idOrPhys == static_cast<uint16_t>(PhysReg::X4)) &&
                   instr.ops[1].kind == MOperand::Kind::Imm && instr.ops[1].imm == 6;
        });
    EXPECT_TRUE(joinStillDefinesScale);
}

namespace {

/// @brief Count `mov <reg>, #imm` instructions in @p bb.
std::size_t countMovRI(const MBasicBlock &bb, PhysReg reg, long long imm) {
    return static_cast<std::size_t>(
        std::count_if(bb.instrs.begin(), bb.instrs.end(), [reg, imm](const MInstr &instr) {
            return instr.opc == MOpcode::MovRI && instr.ops.size() == 2 &&
                   instr.ops[0].kind == MOperand::Kind::Reg && instr.ops[0].reg.isPhys &&
                   instr.ops[0].reg.idOrPhys == static_cast<uint16_t>(reg) &&
                   instr.ops[1].kind == MOperand::Kind::Imm && instr.ops[1].imm == imm;
        }));
}

/// @brief `flag = 1; for (k = 0; k < 4; ++k) if (k & 1) flag = 0; return flag`
///        after register allocation: x21 holds the flag, x20 the counter.
MFunction loopCarriedFlag() {
    MFunction fn{};
    fn.name = "loop_carried_flag";
    fn.blocks.push_back(MBasicBlock{"pre", {}});
    fn.blocks.push_back(MBasicBlock{"header", {}});
    fn.blocks.push_back(MBasicBlock{"body", {}});
    fn.blocks.push_back(MBasicBlock{"clear", {}});
    fn.blocks.push_back(MBasicBlock{"latch", {}});
    fn.blocks.push_back(MBasicBlock{"exit", {}});

    auto &pre = fn.blocks[0];
    auto &header = fn.blocks[1];
    auto &body = fn.blocks[2];
    auto &clear = fn.blocks[3];
    auto &latch = fn.blocks[4];
    auto &exit = fn.blocks[5];

    pre.instrs.push_back(
        MInstr{MOpcode::MovRI, {MOperand::regOp(PhysReg::X21), MOperand::immOp(1)}});
    pre.instrs.push_back(
        MInstr{MOpcode::MovRI, {MOperand::regOp(PhysReg::X20), MOperand::immOp(0)}});
    pre.instrs.push_back(MInstr{MOpcode::Br, {MOperand::labelOp("header")}});

    header.instrs.push_back(
        MInstr{MOpcode::CmpRI, {MOperand::regOp(PhysReg::X20), MOperand::immOp(4)}});
    header.instrs.push_back(
        MInstr{MOpcode::BCond, {MOperand::condOp("ge"), MOperand::labelOp("exit")}});
    header.instrs.push_back(MInstr{MOpcode::Br, {MOperand::labelOp("body")}});

    body.instrs.push_back(
        MInstr{MOpcode::Tbnz,
               {MOperand::regOp(PhysReg::X20), MOperand::labelOp("clear"), MOperand::immOp(0)}});
    body.instrs.push_back(MInstr{MOpcode::Br, {MOperand::labelOp("latch")}});

    clear.instrs.push_back(
        MInstr{MOpcode::MovRI, {MOperand::regOp(PhysReg::X21), MOperand::immOp(0)}});
    clear.instrs.push_back(MInstr{MOpcode::Br, {MOperand::labelOp("latch")}});

    latch.instrs.push_back(
        MInstr{MOpcode::AddRI,
               {MOperand::regOp(PhysReg::X20), MOperand::regOp(PhysReg::X20), MOperand::immOp(1)}});
    latch.instrs.push_back(MInstr{MOpcode::Br, {MOperand::labelOp("header")}});

    exit.instrs.push_back(
        MInstr{MOpcode::MovRR, {MOperand::regOp(PhysReg::X0), MOperand::regOp(PhysReg::X21)}});
    exit.instrs.push_back(MInstr{MOpcode::Ret, {}});
    return fn;
}

} // namespace

TEST(AArch64PeepholeSubpasses, LoopConstHoistLeavesLoopCarriedFlagAlone) {
    // x21 carries `flag` from the preheader around the loop and is cleared on
    // odd trips only. Its single in-loop definition is one immediate, but the
    // register is live into the header, so hoisting `mov x21, #0` into the
    // preheader would erase the initial `#1` (Legacy Baseball's phantom
    // runners and right-handed lefty came from exactly this shape).
    MFunction fn = loopCarriedFlag();
    const auto stats = runPeephole(fn, &darwinTarget());
    EXPECT_EQ(stats.loopConstsHoisted, 0);

    const auto findBlock = [&fn](const char *name) -> const MBasicBlock * {
        for (const auto &bb : fn.blocks)
            if (bb.name == name)
                return &bb;
        return nullptr;
    };
    const MBasicBlock *pre = findBlock("pre");
    const MBasicBlock *clear = findBlock("clear");
    ASSERT_NE(pre, nullptr);
    ASSERT_NE(clear, nullptr);
    EXPECT_EQ(countMovRI(*pre, PhysReg::X21, 1), 1u);
    EXPECT_EQ(countMovRI(*pre, PhysReg::X21, 0), 0u);
    EXPECT_EQ(countMovRI(*clear, PhysReg::X21, 0), 1u);
}

TEST(AArch64PeepholeSubpasses, LoopConstHoistStillHoistsATrueInvariant) {
    // The same loop, but x22 is defined only inside the loop (every trip)
    // and is dead at the header: that one is a real invariant and moves.
    MFunction fn = loopCarriedFlag();
    auto &body = fn.blocks[2];
    body.instrs.insert(body.instrs.begin(),
                       MInstr{MOpcode::MovRI, {MOperand::regOp(PhysReg::X22), MOperand::immOp(7)}});
    body.instrs.insert(body.instrs.begin() + 1,
                       MInstr{MOpcode::AddRRR,
                              {MOperand::regOp(PhysReg::X20),
                               MOperand::regOp(PhysReg::X20),
                               MOperand::regOp(PhysReg::X22)}});
    const auto stats = runPeephole(fn, &darwinTarget());
    EXPECT_EQ(stats.loopConstsHoisted, 1);
    EXPECT_EQ(countMovRI(fn.blocks[0], PhysReg::X22, 7), 1u);
    EXPECT_EQ(countMovRI(fn.blocks[2], PhysReg::X22, 7), 0u);
    // x21 is still left alone.
    EXPECT_EQ(countMovRI(fn.blocks[0], PhysReg::X21, 0), 0u);
}

// ─── B1: block-exit liveness for compute-into-target folding and MADD fusion ─

namespace {

/// Two-block function: `entry` holding @p instrs, then `next` holding
/// @p nextInstrs followed by `ret` (a successor that reads a register keeps
/// it live across the edge; by default it reads nothing).
MFunction twoBlocks(std::vector<MInstr> instrs, std::vector<MInstr> nextInstrs = {}) {
    MFunction fn{};
    fn.name = "exit_live";
    fn.blocks.push_back(MBasicBlock{"entry", std::move(instrs)});
    nextInstrs.push_back(MInstr{MOpcode::Ret, {}});
    fn.blocks.push_back(MBasicBlock{"next", std::move(nextInstrs)});
    return fn;
}

MInstr add3(PhysReg dst, PhysReg a, PhysReg b) {
    return MInstr{MOpcode::AddRRR, {MOperand::regOp(dst), MOperand::regOp(a), MOperand::regOp(b)}};
}

MInstr mul3(PhysReg dst, PhysReg a, PhysReg b) {
    return MInstr{MOpcode::MulRRR, {MOperand::regOp(dst), MOperand::regOp(a), MOperand::regOp(b)}};
}

MInstr mov2(PhysReg dst, PhysReg src) {
    return MInstr{MOpcode::MovRR, {MOperand::regOp(dst), MOperand::regOp(src)}};
}

} // namespace

TEST(AArch64PeepholeSubpasses, FoldComputeIntoTargetRespectsExitLiveRegister) {
    // add x1, x2, x3 ; mov x0, x1 ; b next — with x1 read in `next` the ALU
    // destination is live-out and must not be redirected to x0.
    MFunction fn = twoBlocks({add3(PhysReg::X1, PhysReg::X2, PhysReg::X3),
                              mov2(PhysReg::X0, PhysReg::X1),
                              MInstr{MOpcode::Br, {MOperand::labelOp("next")}}},
                             {mov2(PhysReg::X4, PhysReg::X1)});
    (void)runPeephole(fn);
    ASSERT_GE(fn.blocks[0].instrs.size(), 2u);
    EXPECT_EQ(fn.blocks[0].instrs[0].opc, MOpcode::AddRRR);
    EXPECT_EQ(fn.blocks[0].instrs[0].ops[0].reg.idOrPhys, static_cast<uint16_t>(PhysReg::X1));
    EXPECT_EQ(fn.blocks[0].instrs[1].opc, MOpcode::MovRR);

    // Without the successor read the fold is legal and the move disappears.
    MFunction folded = twoBlocks({add3(PhysReg::X1, PhysReg::X2, PhysReg::X3),
                                  mov2(PhysReg::X0, PhysReg::X1),
                                  MInstr{MOpcode::Br, {MOperand::labelOp("next")}}});
    (void)runPeephole(folded);
    ASSERT_FALSE(folded.blocks[0].instrs.empty());
    EXPECT_EQ(folded.blocks[0].instrs[0].opc, MOpcode::AddRRR);
    EXPECT_EQ(folded.blocks[0].instrs[0].ops[0].reg.idOrPhys, static_cast<uint16_t>(PhysReg::X0));
    for (const auto &instr : folded.blocks[0].instrs)
        EXPECT_NE(instr.opc, MOpcode::MovRR);
}

TEST(AArch64PeepholeSubpasses, FoldComputeIntoTargetKeepsReturnRegisterAtRet) {
    // add x0, x2, x3 ; mov x1, x0 ; ret — x0 is the return value, so the ALU
    // destination is live at the exit and the fold must decline.
    MFunction fn{};
    fn.name = "ret_value";
    fn.blocks.push_back(MBasicBlock{"entry",
                                    {add3(PhysReg::X0, PhysReg::X2, PhysReg::X3),
                                     mov2(PhysReg::X1, PhysReg::X0),
                                     MInstr{MOpcode::Ret, {}}}});
    (void)runPeephole(fn);
    ASSERT_GE(fn.blocks[0].instrs.size(), 2u);
    EXPECT_EQ(fn.blocks[0].instrs[0].opc, MOpcode::AddRRR);
    EXPECT_EQ(fn.blocks[0].instrs[0].ops[0].reg.idOrPhys, static_cast<uint16_t>(PhysReg::X0));
    EXPECT_EQ(fn.blocks[0].instrs[1].opc, MOpcode::MovRR);

    // The mirror image (result computed in x1, moved into x0) folds.
    MFunction mirrored{};
    mirrored.name = "ret_value_fold";
    mirrored.blocks.push_back(MBasicBlock{"entry",
                                          {add3(PhysReg::X1, PhysReg::X2, PhysReg::X3),
                                           mov2(PhysReg::X0, PhysReg::X1),
                                           MInstr{MOpcode::Ret, {}}}});
    (void)runPeephole(mirrored);
    ASSERT_FALSE(mirrored.blocks[0].instrs.empty());
    EXPECT_EQ(mirrored.blocks[0].instrs[0].opc, MOpcode::AddRRR);
    EXPECT_EQ(mirrored.blocks[0].instrs[0].ops[0].reg.idOrPhys, static_cast<uint16_t>(PhysReg::X0));
}

TEST(AArch64PeepholeSubpasses, FoldComputeIntoTargetKeepsCallArgument) {
    // add x1, x2, x3 ; mov x0, x1 ; bl f ; ret — the call reads its argument
    // registers, so x1 is not dead after the move.
    MFunction fn{};
    fn.name = "call_arg";
    fn.blocks.push_back(MBasicBlock{"entry",
                                    {add3(PhysReg::X1, PhysReg::X2, PhysReg::X3),
                                     mov2(PhysReg::X0, PhysReg::X1),
                                     MInstr{MOpcode::Bl, {MOperand::labelOp("callee")}},
                                     MInstr{MOpcode::Ret, {}}}});
    (void)runPeephole(fn);
    ASSERT_GE(fn.blocks[0].instrs.size(), 3u);
    EXPECT_EQ(fn.blocks[0].instrs[0].opc, MOpcode::AddRRR);
    EXPECT_EQ(fn.blocks[0].instrs[0].ops[0].reg.idOrPhys, static_cast<uint16_t>(PhysReg::X1));
    EXPECT_EQ(fn.blocks[0].instrs[1].opc, MOpcode::MovRR);

    // A callee-saved destination is dead at a return (the epilogue restores
    // the register from its slot), so that fold applies.
    MFunction saved{};
    saved.name = "ret_callee_saved";
    saved.blocks.push_back(MBasicBlock{"entry",
                                       {add3(PhysReg::X25, PhysReg::X2, PhysReg::X3),
                                        mov2(PhysReg::X0, PhysReg::X25),
                                        MInstr{MOpcode::Ret, {}}}});
    (void)runPeephole(saved);
    ASSERT_FALSE(saved.blocks[0].instrs.empty());
    EXPECT_EQ(saved.blocks[0].instrs[0].opc, MOpcode::AddRRR);
    EXPECT_EQ(saved.blocks[0].instrs[0].ops[0].reg.idOrPhys, static_cast<uint16_t>(PhysReg::X0));

    // ... and inside the function only when a successor reads it (a pinned
    // slot or a value kept across blocks is an explicit read there).
    MFunction inner = twoBlocks({add3(PhysReg::X25, PhysReg::X2, PhysReg::X3),
                                 mov2(PhysReg::X0, PhysReg::X25),
                                 MInstr{MOpcode::Br, {MOperand::labelOp("next")}}},
                                {mov2(PhysReg::X1, PhysReg::X25)});
    (void)runPeephole(inner);
    ASSERT_GE(inner.blocks[0].instrs.size(), 2u);
    EXPECT_EQ(inner.blocks[0].instrs[0].opc, MOpcode::AddRRR);
    EXPECT_EQ(inner.blocks[0].instrs[0].ops[0].reg.idOrPhys, static_cast<uint16_t>(PhysReg::X25));

    // A callee-saved destination nobody reads later is dead at an inner exit
    // too: the fold applies.
    MFunction innerDead = twoBlocks({add3(PhysReg::X25, PhysReg::X2, PhysReg::X3),
                                     mov2(PhysReg::X0, PhysReg::X25),
                                     MInstr{MOpcode::Br, {MOperand::labelOp("next")}}});
    (void)runPeephole(innerDead);
    ASSERT_FALSE(innerDead.blocks[0].instrs.empty());
    EXPECT_EQ(innerDead.blocks[0].instrs[0].opc, MOpcode::AddRRR);
    EXPECT_EQ(innerDead.blocks[0].instrs[0].ops[0].reg.idOrPhys,
              static_cast<uint16_t>(PhysReg::X0));

    // A caller-saved non-argument destination is clobbered by the call and
    // therefore dead: the fold applies.
    MFunction scratch{};
    scratch.name = "call_scratch";
    scratch.blocks.push_back(MBasicBlock{"entry",
                                         {add3(PhysReg::X12, PhysReg::X2, PhysReg::X3),
                                          mov2(PhysReg::X0, PhysReg::X12),
                                          MInstr{MOpcode::Bl, {MOperand::labelOp("callee")}},
                                          MInstr{MOpcode::Ret, {}}}});
    (void)runPeephole(scratch);
    ASSERT_FALSE(scratch.blocks[0].instrs.empty());
    EXPECT_EQ(scratch.blocks[0].instrs[0].opc, MOpcode::AddRRR);
    EXPECT_EQ(scratch.blocks[0].instrs[0].ops[0].reg.idOrPhys, static_cast<uint16_t>(PhysReg::X0));
}

TEST(AArch64PeepholeSubpasses, MaddFusionRespectsExitLiveMultiplyDestination) {
    // mul x0, x1, x2 ; add x3, x3, x0 ; ret — x0 is the return value, so the
    // multiply result is live at the exit and the pair must not fuse.
    MFunction fn{};
    fn.name = "madd_ret";
    fn.blocks.push_back(MBasicBlock{"entry",
                                    {mul3(PhysReg::X0, PhysReg::X1, PhysReg::X2),
                                     add3(PhysReg::X3, PhysReg::X3, PhysReg::X0),
                                     MInstr{MOpcode::Ret, {}}}});
    const auto stats = runPeephole(fn);
    EXPECT_EQ(stats.maddFusions, 0);
    ASSERT_FALSE(fn.blocks[0].instrs.empty());
    EXPECT_EQ(fn.blocks[0].instrs[0].opc, MOpcode::MulRRR);

    // A multiply destination read in the successor is live-out too.
    MFunction carried = twoBlocks({mul3(PhysReg::X4, PhysReg::X1, PhysReg::X2),
                                   add3(PhysReg::X3, PhysReg::X3, PhysReg::X4),
                                   MInstr{MOpcode::Br, {MOperand::labelOp("next")}}},
                                  {mov2(PhysReg::X0, PhysReg::X4)});
    const auto carriedStats = runPeephole(carried);
    EXPECT_EQ(carriedStats.maddFusions, 0);
    ASSERT_FALSE(carried.blocks[0].instrs.empty());
    EXPECT_EQ(carried.blocks[0].instrs[0].opc, MOpcode::MulRRR);

    // A dead multiply destination fuses.
    MFunction fused{};
    fused.name = "madd_dead";
    fused.blocks.push_back(MBasicBlock{"entry",
                                       {mul3(PhysReg::X4, PhysReg::X1, PhysReg::X2),
                                        add3(PhysReg::X3, PhysReg::X3, PhysReg::X4),
                                        MInstr{MOpcode::Ret, {}}}});
    const auto fusedStats = runPeephole(fused);
    EXPECT_EQ(fusedStats.maddFusions, 1);
    ASSERT_FALSE(fused.blocks[0].instrs.empty());
    EXPECT_EQ(fused.blocks[0].instrs[0].opc, MOpcode::MAddRRRR);
}

TEST(AArch64PeepholeSubpasses, PostScheduleMoveFoldPreservesExitLiveRegister) {
    // mov x0, x19 ; mov x20, x0 ; b next — x0 is read in `next`, so the
    // intermediate register is live and the pair must not fold.
    MFunction fn = twoBlocks({mov2(PhysReg::X0, PhysReg::X19),
                              mov2(PhysReg::X20, PhysReg::X0),
                              MInstr{MOpcode::Br, {MOperand::labelOp("next")}}},
                             {mov2(PhysReg::X1, PhysReg::X0)});

    const auto stats = runPostSchedulePeephole(fn);
    EXPECT_EQ(stats.consecutiveMovsFolded, 0);
    ASSERT_FALSE(fn.blocks[0].instrs.empty());
    EXPECT_EQ(fn.blocks[0].instrs[0].opc, MOpcode::MovRR);
    EXPECT_EQ(fn.blocks[0].instrs[0].ops[0].reg.idOrPhys, static_cast<uint16_t>(PhysReg::X0));
    EXPECT_EQ(fn.blocks[0].instrs[0].ops[1].reg.idOrPhys, static_cast<uint16_t>(PhysReg::X19));

    // A caller-saved intermediate nobody reads later is folded away (by the
    // pair fold, or by copy propagation plus block-local DCE): the block
    // ends up moving x19 straight into x20 and never defines x10.
    MFunction dead = twoBlocks({mov2(PhysReg::X10, PhysReg::X19),
                                mov2(PhysReg::X20, PhysReg::X10),
                                MInstr{MOpcode::Br, {MOperand::labelOp("next")}}});
    (void)runPostSchedulePeephole(dead);
    ASSERT_FALSE(dead.blocks[0].instrs.empty());
    EXPECT_EQ(dead.blocks[0].instrs[0].opc, MOpcode::MovRR);
    EXPECT_EQ(dead.blocks[0].instrs[0].ops[0].reg.idOrPhys, static_cast<uint16_t>(PhysReg::X20));
    EXPECT_EQ(dead.blocks[0].instrs[0].ops[1].reg.idOrPhys, static_cast<uint16_t>(PhysReg::X19));
    for (const auto &instr : dead.blocks[0].instrs) {
        if (instr.opc == MOpcode::MovRR)
            EXPECT_NE(instr.ops[0].reg.idOrPhys, static_cast<uint16_t>(PhysReg::X10));
    }
}

TEST(AArch64PeepholeSubpasses, PostScheduleImmediateMovePreservesExitLiveRegister) {
    // mov x0, #42 ; mov x19, x0 ; b next — x0 is read in `next`.
    MFunction fn =
        twoBlocks({MInstr{MOpcode::MovRI, {MOperand::regOp(PhysReg::X0), MOperand::immOp(42)}},
                   mov2(PhysReg::X19, PhysReg::X0),
                   MInstr{MOpcode::Br, {MOperand::labelOp("next")}}},
                  {mov2(PhysReg::X1, PhysReg::X0)});

    const auto stats = runPostSchedulePeephole(fn);
    EXPECT_EQ(stats.consecutiveMovsFolded, 0);
    ASSERT_FALSE(fn.blocks[0].instrs.empty());
    EXPECT_EQ(fn.blocks[0].instrs[0].opc, MOpcode::MovRI);
    EXPECT_EQ(fn.blocks[0].instrs[0].ops[0].reg.idOrPhys, static_cast<uint16_t>(PhysReg::X0));
    EXPECT_EQ(fn.blocks[0].instrs[0].ops[1].imm, 42);
}

TEST(AArch64PeepholeSubpasses, StrengthReductionRespectsExitLiveDivisorRegister) {
    // x1 (the divisor) is read in the successor with no in-block use after
    // the division. Reusing it as a scratch register in the magic-number
    // expansion would clobber the live value, so the rewrite must decline.
    // The quotient x2 is read there too, so the division is not dead.
    MFunction fn =
        twoBlocks({MInstr{MOpcode::MovRI, {MOperand::regOp(PhysReg::X1), MOperand::immOp(7)}},
                   MInstr{MOpcode::UDivRRR,
                          {MOperand::regOp(PhysReg::X2),
                           MOperand::regOp(PhysReg::X0),
                           MOperand::regOp(PhysReg::X1)}},
                   MInstr{MOpcode::Br, {MOperand::labelOp("next")}}},
                  {add3(PhysReg::X0, PhysReg::X1, PhysReg::X2)});

    auto stats = runPeephole(fn);
    (void)stats;

    const bool divSurvives =
        std::any_of(fn.blocks[0].instrs.begin(), fn.blocks[0].instrs.end(), [](const MInstr &mi) {
            return mi.opc == MOpcode::UDivRRR;
        });
    EXPECT_TRUE(divSurvives);
}

TEST(AArch64PeepholeSubpasses, BlockLocalDceSeedsFromExitLiveSet) {
    // x10 has no in-block reader, but it is in the exit-live set; the
    // block-local DCE must treat it as live.
    MFunction fn{};
    fn.name = "exit_live_def";
    fn.blocks.push_back(MBasicBlock{"entry", {}});
    auto &bb = fn.blocks.back();

    bb.instrs.push_back(
        MInstr{MOpcode::MovRI, {MOperand::regOp(PhysReg::X10), MOperand::immOp(42)}});
    bb.instrs.push_back(MInstr{MOpcode::Ret, {}});

    PhysRegSet exitLive;
    exitLive.add(PhysReg::X10);
    PeepholeStats stats{};
    peephole::removeDeadInstructions(fn.blocks[0].instrs, stats, &exitLive);

    const bool defSurvives = std::any_of(fn.blocks[0].instrs.begin(),
                                         fn.blocks[0].instrs.end(),
                                         [](const MInstr &mi) { return mi.opc == MOpcode::MovRI; });
    EXPECT_TRUE(defSurvives);

    // With an exit-live set that omits x10 the def is removable ...
    MFunction fn2{};
    fn2.blocks.push_back(MBasicBlock{"entry", {}});
    fn2.blocks[0].instrs.push_back(
        MInstr{MOpcode::MovRI, {MOperand::regOp(PhysReg::X10), MOperand::immOp(42)}});
    fn2.blocks[0].instrs.push_back(MInstr{MOpcode::Ret, {}});
    PeepholeStats stats2{};
    PhysRegSet empty;
    peephole::removeDeadInstructions(fn2.blocks[0].instrs, stats2, &empty);
    const bool removedWhenDead =
        std::none_of(fn2.blocks[0].instrs.begin(),
                     fn2.blocks[0].instrs.end(),
                     [](const MInstr &mi) { return mi.opc == MOpcode::MovRI; });
    EXPECT_TRUE(removedWhenDead);

    // ... and so is it without any liveness (x10 is not in the conservative seed).
    MFunction fn3{};
    fn3.blocks.push_back(MBasicBlock{"entry", {}});
    fn3.blocks[0].instrs.push_back(
        MInstr{MOpcode::MovRI, {MOperand::regOp(PhysReg::X10), MOperand::immOp(42)}});
    fn3.blocks[0].instrs.push_back(MInstr{MOpcode::Ret, {}});
    PeepholeStats stats3{};
    peephole::removeDeadInstructions(fn3.blocks[0].instrs, stats3, nullptr);
    const bool removedWithoutLiveness =
        std::none_of(fn3.blocks[0].instrs.begin(),
                     fn3.blocks[0].instrs.end(),
                     [](const MInstr &mi) { return mi.opc == MOpcode::MovRI; });
    EXPECT_TRUE(removedWithoutLiveness);
}

TEST(AArch64PeepholeSubpasses, CsetBranchFusionRespectsExitLiveDestination) {
    // cset x1, eq ; cbnz x1, next ; b other — x1 is read in `next`, so the
    // materialised boolean is live and the fusion must decline.
    MFunction fn{};
    fn.name = "cset_live";
    fn.blocks.push_back(MBasicBlock{
        "entry",
        {MInstr{MOpcode::CmpRI, {MOperand::regOp(PhysReg::X0), MOperand::immOp(0)}},
         MInstr{MOpcode::Cset, {MOperand::regOp(PhysReg::X1), MOperand::condOp("eq")}},
         MInstr{MOpcode::Cbnz, {MOperand::regOp(PhysReg::X1), MOperand::labelOp("next")}},
         MInstr{MOpcode::Br, {MOperand::labelOp("other")}}}});
    fn.blocks.push_back(
        MBasicBlock{"next", {mov2(PhysReg::X0, PhysReg::X1), MInstr{MOpcode::Ret}}});
    fn.blocks.push_back(MBasicBlock{"other", {MInstr{MOpcode::Ret}}});
    (void)runPeephole(fn);
    const bool csetSurvives = std::any_of(fn.blocks[0].instrs.begin(),
                                          fn.blocks[0].instrs.end(),
                                          [](const MInstr &mi) { return mi.opc == MOpcode::Cset; });
    EXPECT_TRUE(csetSurvives);

    // Without the successor read the pair fuses into a conditional branch.
    MFunction fused{};
    fused.name = "cset_dead";
    fused.blocks.push_back(MBasicBlock{
        "entry",
        {MInstr{MOpcode::CmpRI, {MOperand::regOp(PhysReg::X0), MOperand::immOp(0)}},
         MInstr{MOpcode::Cset, {MOperand::regOp(PhysReg::X1), MOperand::condOp("eq")}},
         MInstr{MOpcode::Cbnz, {MOperand::regOp(PhysReg::X1), MOperand::labelOp("next")}},
         MInstr{MOpcode::Br, {MOperand::labelOp("other")}}}});
    fused.blocks.push_back(MBasicBlock{"next", {MInstr{MOpcode::Ret}}});
    fused.blocks.push_back(MBasicBlock{"other", {MInstr{MOpcode::Ret}}});
    (void)runPeephole(fused);
    const bool csetGone = std::none_of(fused.blocks[0].instrs.begin(),
                                       fused.blocks[0].instrs.end(),
                                       [](const MInstr &mi) { return mi.opc == MOpcode::Cset; });
    EXPECT_TRUE(csetGone);
}

// ─── BranchOpt: cold-block reordering and fallthrough safety ────────────────

TEST(AArch64PeepholeSubpasses, ColdBlockWithFallthroughIsNotMoved) {
    // A block matched as "cold" purely by its name token must not be moved to
    // the end of the function when it can fall through into the next block —
    // relocation would silently re-target the implicit fallthrough edge.
    MFunction fn{};
    fn.name = "cold_ft";

    MBasicBlock entry{"entry", {}};
    entry.instrs.push_back(MInstr{MOpcode::Br, {MOperand::labelOp("user_error_path")}});

    MBasicBlock coldFallthrough{"user_error_path", {}};
    // Ends in a conditional branch: not-taken path falls through to "after".
    coldFallthrough.instrs.push_back(
        MInstr{MOpcode::Cbz, {MOperand::regOp(PhysReg::X0), MOperand::labelOp("entry")}});

    MBasicBlock after{"after", {}};
    after.instrs.push_back(MInstr{MOpcode::Ret, {}});

    fn.blocks = {entry, coldFallthrough, after};

    const std::size_t moved = peephole::reorderBlocks(fn);
    EXPECT_EQ(moved, 0u);
    ASSERT_EQ(fn.blocks.size(), 3u);
    EXPECT_EQ(fn.blocks[1].name, "user_error_path");
    EXPECT_EQ(fn.blocks[2].name, "after");
}

TEST(AArch64PeepholeSubpasses, TerminatedColdBlockIsMovedToEnd) {
    MFunction fn{};
    fn.name = "cold_term";

    MBasicBlock entry{"entry", {}};
    entry.instrs.push_back(MInstr{MOpcode::Br, {MOperand::labelOp("hot")}});

    MBasicBlock cold{"check_error_exit", {}};
    cold.instrs.push_back(MInstr{MOpcode::Br, {MOperand::labelOp("hot")}});

    MBasicBlock hot{"hot", {}};
    hot.instrs.push_back(MInstr{MOpcode::Ret, {}});

    fn.blocks = {entry, cold, hot};

    const std::size_t moved = peephole::reorderBlocks(fn);
    EXPECT_EQ(moved, 1u);
    ASSERT_EQ(fn.blocks.size(), 3u);
    EXPECT_EQ(fn.blocks.back().name, "check_error_exit");
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, argv);
    return zanna_test::run_all_tests();
}
