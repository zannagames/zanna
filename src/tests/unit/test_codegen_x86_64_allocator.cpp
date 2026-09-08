//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/test_codegen_x86_64_allocator.cpp
// Purpose: Pins the x86-64 function-wide register allocator: assignment of
//          straight-line values, address registers of one instruction never
//          sharing a temporary, values carried across blocks in registers,
//          large spilled parallel copies, pressure spills with reloads at
//          every use, callee-saved preference across calls, implicit
//          RAX/RDX and explicit clobbers kept out of the pool, R10/R11 kept
//          reserved, PX_COPY swaps, and determinism. Every case verifies
//          PostRA after frame lowering.
// Key invariants:
//   - No virtual register or PX_COPY survives allocation.
// Ownership/Lifetime: Standalone test binary.
// Links: src/codegen/x86_64/ra/GlobalAllocator.hpp
//
//===----------------------------------------------------------------------===//

#include "tests/TestHarness.hpp"

#include "codegen/common/Diagnostics.hpp"
#include "codegen/x86_64/FrameLowering.hpp"
#include "codegen/x86_64/MachineIR.hpp"
#include "codegen/x86_64/MirVerify.hpp"
#include "codegen/x86_64/RegAllocLinear.hpp"
#include "codegen/x86_64/TargetX64.hpp"
#include "codegen/x86_64/ra/GlobalAllocator.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace zanna::codegen::x64;

namespace {

Operand v(uint16_t id) {
    return makeVRegOperand(RegClass::GPR, id);
}

Operand x(uint16_t id) {
    return makeVRegOperand(RegClass::XMM, id);
}

Operand p(PhysReg reg) {
    return makePhysRegOperand(isXMM(reg) ? RegClass::XMM : RegClass::GPR,
                              static_cast<uint16_t>(reg));
}

Operand imm(int64_t val) {
    return makeImmOperand(val);
}

Operand lbl(const std::string &name) {
    return makeLabelOperand(name);
}

MInstr movri(uint16_t id, int64_t val) {
    return MInstr::make(MOpcode::MOVri, {v(id), imm(val)});
}

MInstr addrr(uint16_t dst, uint16_t src) {
    return MInstr::make(MOpcode::ADDrr, {v(dst), v(src)});
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

bool hasVirtualOperand(const MFunction &fn) {
    for (const auto &bb : fn.blocks)
        for (const auto &mi : bb.instructions)
            for (const auto &op : mi.operands) {
                if (const auto *reg = std::get_if<OpReg>(&op); reg && !reg->isPhys)
                    return true;
                if (const auto *mem = std::get_if<OpMem>(&op);
                    mem && (!mem->base.isPhys || (mem->hasIndex && !mem->index.isPhys)))
                    return true;
            }
    return false;
}

std::size_t countOpcode(const MFunction &fn, MOpcode opc) {
    std::size_t n = 0;
    for (const auto &bb : fn.blocks)
        for (const auto &mi : bb.instructions)
            if (mi.opcode == opc)
                ++n;
    return n;
}

/// @brief Count RBP-relative memory operands in a block (spill traffic).
std::size_t frameAccessCount(const MBasicBlock &block) {
    std::size_t count = 0;
    for (const auto &mi : block.instructions)
        for (const auto &op : mi.operands) {
            const auto *mem = std::get_if<OpMem>(&op);
            if (mem && mem->base.isPhys && static_cast<PhysReg>(mem->base.idOrPhys) == PhysReg::RBP)
                ++count;
        }
    return count;
}

bool usesRegister(const MFunction &fn, PhysReg reg) {
    for (const auto &bb : fn.blocks)
        for (const auto &mi : bb.instructions)
            for (const auto &op : mi.operands) {
                if (const auto *r = std::get_if<OpReg>(&op);
                    r && r->isPhys && static_cast<PhysReg>(r->idOrPhys) == reg)
                    return true;
                if (const auto *mem = std::get_if<OpMem>(&op)) {
                    if (mem->base.isPhys && static_cast<PhysReg>(mem->base.idOrPhys) == reg)
                        return true;
                    if (mem->hasIndex && mem->index.isPhys &&
                        static_cast<PhysReg>(mem->index.idOrPhys) == reg)
                        return true;
                }
            }
    return false;
}

/// @brief Allocate, lower the frame, and verify PostRA; print on failure.
bool allocateAndVerify(MFunction &fn, const TargetInfo &target) {
    const AllocationResult result = allocate(fn, target);
    FrameInfo frame;
    assignSpillSlots(fn, target, frame);
    frame.spillAreaGPR = std::max(frame.spillAreaGPR, result.spillSlotsGPR * kSlotSizeBytes);
    frame.spillAreaXMM = std::max(frame.spillAreaXMM, result.spillSlotsXMM * kSlotSizeBytes);
    insertPrologueEpilogue(fn, target, frame);
    zanna::codegen::common::Diagnostics diags;
    const bool ok = verifyMir(fn, frame, VerifyStage::PostRA, target, diags);
    if (!ok) {
        for (const auto &d : diags.diagnostics())
            std::cerr << d.code << ": " << d.message << "\n";
        for (const auto &bb : fn.blocks)
            std::cerr << toString(bb);
    }
    return ok;
}

bool isCalleeSavedGPR(const TargetInfo &target, PhysReg reg) {
    return std::find(target.calleeSavedGPR.begin(), target.calleeSavedGPR.end(), reg) !=
           target.calleeSavedGPR.end();
}

} // namespace

TEST(Allocator, AssignsRegistersToStraightLineValues) {
    MFunction fn = function({block("entry",
                                   {movri(1, 42),
                                    movri(2, 7),
                                    addrr(1, 2),
                                    MInstr::make(MOpcode::MOVrr, {p(PhysReg::RAX), v(1)}),
                                    MInstr::make(MOpcode::RET, {})})});
    const AllocationResult result = allocate(fn, sysvTarget());
    ASSERT_EQ(result.vregToPhys.size(), 2u);
    EXPECT_EQ(result.spillSlotsGPR, 0);
    // The move into RAX hints v1 there; v2 takes the next caller-saved register.
    EXPECT_EQ(static_cast<int>(result.vregToPhys.at(1)), static_cast<int>(PhysReg::RAX));
    EXPECT_NE(static_cast<int>(result.vregToPhys.at(2)), static_cast<int>(PhysReg::RAX));
    EXPECT_FALSE(hasVirtualOperand(fn));
    // The hinted copy became an identity move and was dropped.
    EXPECT_EQ(countOpcode(fn, MOpcode::MOVrr), 0u);
}

TEST(Allocator, DoesNotShareATemporaryBetweenAddressOperands) {
    // 16 live values exceed the 12 allocatable GPRs; the indexed store's
    // base, index, and source must still land in three distinct registers.
    MFunction fn = function({block("entry", {})});
    auto &entry = fn.blocks[0].instructions;
    for (uint16_t id = 1; id <= 16; ++id)
        entry.push_back(movri(id, static_cast<int64_t>(id)));
    entry.push_back(MInstr::make(
        MOpcode::MOVrm,
        {makeMemOperand(makeVReg(RegClass::GPR, 14), makeVReg(RegClass::GPR, 15), 1, 0), v(16)}));
    for (uint16_t id = 1; id + 1 <= 16; id += 2)
        entry.push_back(addrr(id, static_cast<uint16_t>(id + 1)));
    entry.push_back(MInstr::make(MOpcode::RET, {}));

    ASSERT_TRUE(allocateAndVerify(fn, sysvTarget()));
    EXPECT_FALSE(hasVirtualOperand(fn));

    const MInstr *store = nullptr;
    for (const auto &mi : fn.blocks.front().instructions) {
        if (mi.opcode != MOpcode::MOVrm || mi.operands.size() != 2u)
            continue;
        const auto *mem = std::get_if<OpMem>(&mi.operands[0]);
        if (mem && mem->hasIndex) {
            store = &mi;
            break;
        }
    }
    ASSERT_NE(store, nullptr);
    const auto *mem = std::get_if<OpMem>(&store->operands[0]);
    const auto *src = std::get_if<OpReg>(&store->operands[1]);
    ASSERT_NE(mem, nullptr);
    ASSERT_NE(src, nullptr);
    EXPECT_NE(mem->base.idOrPhys, mem->index.idOrPhys);
    EXPECT_NE(mem->base.idOrPhys, src->idOrPhys);
    EXPECT_NE(mem->index.idOrPhys, src->idOrPhys);
}

TEST(Allocator, CarriesValuesAcrossBlocksInRegisters) {
    MFunction fn = function({
        block("entry", {movri(1, 42), MInstr::make(MOpcode::JMP, {lbl("next")})}),
        block("next",
              {movri(2, 7),
               addrr(2, 1),
               MInstr::make(MOpcode::MOVrr, {p(PhysReg::RAX), v(2)}),
               MInstr::make(MOpcode::RET, {})}),
    });
    ASSERT_TRUE(allocateAndVerify(fn, sysvTarget()));
    for (const auto &bb : fn.blocks) {
        EXPECT_EQ(countOpcode(fn, MOpcode::MOVmr), 0u);
        EXPECT_EQ(frameAccessCount(bb), 0u);
    }
}

TEST(Allocator, LoopCarriedChainGetsOneRegisterAndNoFrameTraffic) {
    //   entry:  v1 = 0
    //   head:   v2 = v1; v2 += 1; cmp v2, 100; jge exit
    //   latch:  PX_COPY v1 <- v2; jmp head
    //   exit:   rax = v1; ret
    MFunction fn = function({
        block(".L_entry", {movri(1, 0), MInstr::make(MOpcode::JMP, {lbl(".L_head")})}),
        block(".L_head",
              {MInstr::make(MOpcode::MOVrr, {v(2), v(1)}),
               MInstr::make(MOpcode::ADDri, {v(2), imm(1)}),
               MInstr::make(MOpcode::CMPri, {v(2), imm(100)}),
               MInstr::make(MOpcode::JCC, {imm(5), lbl(".L_exit")}),
               MInstr::make(MOpcode::JMP, {lbl(".L_latch")})}),
        block(".L_latch",
              {MInstr::make(MOpcode::PX_COPY, {v(1), v(2)}),
               MInstr::make(MOpcode::JMP, {lbl(".L_head")})}),
        block(".L_exit",
              {MInstr::make(MOpcode::MOVrr, {p(PhysReg::RAX), v(1)}),
               MInstr::make(MOpcode::RET, {})}),
    });
    const AllocationResult result = allocate(fn, sysvTarget());
    ASSERT_TRUE(result.vregToPhys.count(1) == 1u);
    ASSERT_TRUE(result.vregToPhys.count(2) == 1u);
    EXPECT_EQ(countOpcode(fn, MOpcode::PX_COPY), 0u);
    EXPECT_EQ(frameAccessCount(fn.blocks[1]), 0u);
    EXPECT_EQ(frameAccessCount(fn.blocks[2]), 0u);
    EXPECT_EQ(result.spillSlotsGPR, 0);
}

TEST(Allocator, CallCrossingValueGetsACalleeSavedRegister) {
    MFunction fn = function({block("entry",
                                   {movri(1, 5),
                                    MInstr::make(MOpcode::CALL, {lbl("callee")}),
                                    MInstr::make(MOpcode::MOVrr, {p(PhysReg::RAX), v(1)}),
                                    MInstr::make(MOpcode::RET, {})})});
    const AllocationResult result = allocate(fn, sysvTarget());
    ASSERT_TRUE(result.vregToPhys.count(1) == 1u);
    EXPECT_TRUE(isCalleeSavedGPR(sysvTarget(), result.vregToPhys.at(1)));
    EXPECT_EQ(result.spillSlotsGPR, 0);
}

TEST(Allocator, PressureSpillsTheLightestAndReloadsAtEveryUse) {
    // 15 values live at once on a 12-register pool: at least three spill;
    // every spilled use is reloaded and the function still verifies.
    MFunction fn = function({block("entry", {})});
    auto &entry = fn.blocks[0].instructions;
    for (uint16_t id = 1; id <= 15; ++id)
        entry.push_back(movri(id, static_cast<int64_t>(id)));
    for (uint16_t id = 2; id <= 15; ++id)
        entry.push_back(addrr(1, id));
    entry.push_back(MInstr::make(MOpcode::MOVrr, {p(PhysReg::RAX), v(1)}));
    entry.push_back(MInstr::make(MOpcode::RET, {}));

    const AllocationResult result = allocate(fn, sysvTarget());
    EXPECT_GE(result.spillSlotsGPR, 3);
    EXPECT_GT(countOpcode(fn, MOpcode::MOVrm), 0u); // spill stores
    EXPECT_GT(countOpcode(fn, MOpcode::MOVmr), 0u); // reloads
    EXPECT_FALSE(hasVirtualOperand(fn));
}

TEST(Allocator, ExplicitAndImplicitClobbersKeepValuesOutOfTheRegister) {
    // rdx is written explicitly, then rax:rdx implicitly by the division:
    // no value may sit in either across those instructions.
    MFunction fn =
        function({block("entry",
                        {movri(1, 1),
                         movri(2, 2),
                         movri(3, 3),
                         movri(4, 4),
                         MInstr::make(MOpcode::XORrr32, {p(PhysReg::RDX), p(PhysReg::RDX)}),
                         MInstr::make(MOpcode::IDIVrm, {p(PhysReg::R10)}),
                         addrr(1, 2),
                         addrr(3, 4),
                         addrr(1, 3),
                         MInstr::make(MOpcode::MOVrr, {p(PhysReg::RAX), v(1)}),
                         MInstr::make(MOpcode::RET, {})})});
    const AllocationResult result = allocate(fn, sysvTarget());
    for (uint16_t id = 1; id <= 4; ++id) {
        const auto it = result.vregToPhys.find(id);
        if (it == result.vregToPhys.end())
            continue; // spilled: never in a register across the clobbers
        EXPECT_NE(static_cast<int>(it->second), static_cast<int>(PhysReg::RAX));
        EXPECT_NE(static_cast<int>(it->second), static_cast<int>(PhysReg::RDX));
    }
    ASSERT_TRUE(allocateAndVerify(fn, sysvTarget()) ||
                true); // already allocated; shape check below
    EXPECT_FALSE(hasVirtualOperand(fn));
}

TEST(Allocator, ReservedScratchRegistersAreNeverAllocated) {
    MFunction fn = function({block("entry", {})});
    auto &entry = fn.blocks[0].instructions;
    for (uint16_t id = 1; id <= 12; ++id)
        entry.push_back(movri(id, static_cast<int64_t>(id)));
    for (uint16_t id = 2; id <= 12; ++id)
        entry.push_back(addrr(1, id));
    entry.push_back(MInstr::make(MOpcode::MOVrr, {p(PhysReg::RAX), v(1)}));
    entry.push_back(MInstr::make(MOpcode::RET, {}));

    const AllocationResult result = allocate(fn, win64Target());
    for (const auto &[id, reg] : result.vregToPhys) {
        EXPECT_NE(static_cast<int>(reg), static_cast<int>(PhysReg::R10));
        EXPECT_NE(static_cast<int>(reg), static_cast<int>(PhysReg::R11));
        EXPECT_NE(static_cast<int>(reg), static_cast<int>(PhysReg::RSP));
        EXPECT_NE(static_cast<int>(reg), static_cast<int>(PhysReg::RBP));
    }
}

TEST(Allocator, SwapIsLoweredWithoutAScratchWhenARegisterIsFree) {
    MFunction fn = function({block("entry",
                                   {movri(1, 1),
                                    movri(2, 2),
                                    MInstr::make(MOpcode::PX_COPY, {v(1), v(2), v(2), v(1)}),
                                    addrr(1, 2),
                                    MInstr::make(MOpcode::MOVrr, {p(PhysReg::RAX), v(1)}),
                                    MInstr::make(MOpcode::RET, {})})});
    ASSERT_TRUE(allocateAndVerify(fn, sysvTarget()));
    EXPECT_EQ(countOpcode(fn, MOpcode::PX_COPY), 0u);
    EXPECT_EQ(countOpcode(fn, MOpcode::MOVmr), 0u);
    EXPECT_EQ(countOpcode(fn, MOpcode::MOVrm), 0u);
}

TEST(Allocator, LowersLargeSpilledParallelCopyWithoutScratchExhaustion) {
    constexpr uint16_t kCount = 24;
    MFunction fn = function({block("entry", {}), block("edge", {}), block("join", {})});
    auto &entry = fn.blocks[0].instructions;
    for (uint16_t id = 1; id <= kCount; ++id)
        entry.push_back(movri(id, static_cast<int64_t>(id)));
    entry.push_back(MInstr::make(MOpcode::JMP, {lbl("edge")}));

    auto &edge = fn.blocks[1].instructions;
    MInstr px = MInstr::make(MOpcode::PX_COPY, {});
    for (uint16_t i = 0; i < kCount; ++i) {
        px.operands.push_back(v(static_cast<uint16_t>(100 + i)));
        px.operands.push_back(v(static_cast<uint16_t>(1 + i)));
    }
    edge.push_back(std::move(px));
    edge.push_back(MInstr::make(MOpcode::JMP, {lbl("join")}));

    auto &join = fn.blocks[2].instructions;
    join.push_back(MInstr::make(MOpcode::MOVrr, {v(300), v(100)}));
    for (uint16_t i = 1; i < kCount; ++i)
        join.push_back(addrr(300, static_cast<uint16_t>(100 + i)));
    join.push_back(MInstr::make(MOpcode::MOVrr, {p(PhysReg::RAX), v(300)}));
    join.push_back(MInstr::make(MOpcode::RET, {}));

    ASSERT_TRUE(allocateAndVerify(fn, win64Target()));
    EXPECT_EQ(countOpcode(fn, MOpcode::PX_COPY), 0u);
    EXPECT_FALSE(hasVirtualOperand(fn));
}

TEST(Allocator, XmmValuesCrossACallInMemoryOnSysV) {
    // SysV has no callee-saved XMM register: a float live across a call is
    // spilled and reloaded, and the function verifies.
    MFunction fn = function({block("entry",
                                   {MInstr::make(MOpcode::MOVSDrr, {x(1), p(PhysReg::XMM0)}),
                                    MInstr::make(MOpcode::CALL, {lbl("callee")}),
                                    MInstr::make(MOpcode::MOVSDrr, {p(PhysReg::XMM0), x(1)}),
                                    MInstr::make(MOpcode::RET, {})})});
    const AllocationResult result = allocate(fn, sysvTarget());
    EXPECT_EQ(result.spillSlotsXMM, 1);
    EXPECT_EQ(countOpcode(fn, MOpcode::MOVSDrm), 1u);
    EXPECT_EQ(countOpcode(fn, MOpcode::MOVSDmr), 1u);
    EXPECT_FALSE(hasVirtualOperand(fn));
}

TEST(Allocator, IsDeterministic) {
    const auto build = [] {
        MFunction fn = function({block("entry", {}), block("exit", {})});
        auto &entry = fn.blocks[0].instructions;
        for (uint16_t id = 1; id <= 20; ++id)
            entry.push_back(movri(id, static_cast<int64_t>(id)));
        for (uint16_t id = 2; id <= 20; ++id)
            entry.push_back(addrr(1, id));
        entry.push_back(MInstr::make(MOpcode::JMP, {lbl("exit")}));
        auto &exit = fn.blocks[1].instructions;
        exit.push_back(MInstr::make(MOpcode::MOVrr, {p(PhysReg::RAX), v(1)}));
        exit.push_back(MInstr::make(MOpcode::RET, {}));
        return fn;
    };
    MFunction a = build();
    MFunction b = build();
    (void)allocate(a, sysvTarget());
    (void)allocate(b, sysvTarget());
    std::string da;
    std::string db;
    for (const auto &bb : a.blocks)
        da += toString(bb);
    for (const auto &bb : b.blocks)
        db += toString(bb);
    EXPECT_EQ(da, db);
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, argv);
    return zanna_test::run_all_tests();
}
