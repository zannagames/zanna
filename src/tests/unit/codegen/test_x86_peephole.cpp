//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/codegen/test_x86_peephole.cpp
// Purpose: Unit tests for the x86-64 peephole optimization pass, covering all
//          10 sub-passes: MOV-zero to XOR, CMP-zero to TEST, arithmetic
//          identities, strength reduction, identity move removal, consecutive
//          move folding, dead code elimination, block layout, branch chain
//          elimination, branch inversion, and fallthrough jump removal.
//
// Key invariants:
//   - Each test constructs MIR directly and calls runPeepholes().
//   - Tests verify both the instruction sequence AND the statistics counters.
//
// Ownership/Lifetime:
//   - Standalone test binary.
//
// Links: src/codegen/x86_64/Peephole.hpp
//
//===----------------------------------------------------------------------===//

#include "tests/TestHarness.hpp"

#include "codegen/x86_64/MachineIR.hpp"
#include "codegen/x86_64/OperandRoles.hpp"
#include "codegen/x86_64/Peephole.hpp"
#include "codegen/x86_64/Scheduler.hpp"
#include "codegen/x86_64/TargetX64.hpp"
#include "codegen/x86_64/peephole/MemoryOpt.hpp"
#include "codegen/x86_64/peephole/MovFolding.hpp"

using namespace zanna::codegen::x64;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
namespace {

/// Helper: create a GPR register operand for a physical register.
Operand gpr(PhysReg pr) {
    return OpReg{true, RegClass::GPR, static_cast<uint16_t>(pr)};
}

/// Helper: create an XMM register operand.
Operand xmm(PhysReg pr) {
    return OpReg{true, RegClass::XMM, static_cast<uint16_t>(pr)};
}

/// Helper: create an immediate operand.
Operand imm(int64_t val) {
    return OpImm{val};
}

/// Helper: create a frame-pointer-relative memory operand.
Operand mem(PhysReg base, int32_t disp) {
    return OpMem{std::get<OpReg>(gpr(base)), OpReg{}, 1, disp, false};
}

bool sameRegOperand(const Operand &lhs, const Operand &rhs) {
    const auto *l = std::get_if<OpReg>(&lhs);
    const auto *r = std::get_if<OpReg>(&rhs);
    return l && r && l->isPhys && r->isPhys && l->cls == r->cls && l->idOrPhys == r->idOrPhys;
}

/// Helper: create a label operand.
Operand lbl(const std::string &name) {
    return OpLabel{name};
}

/// Helper: build a single-block function with given instructions.
MFunction makeFunc(const std::string &blockLabel, std::vector<MInstr> instrs) {
    MFunction fn{};
    fn.name = "test_fn";
    MBasicBlock block{};
    block.label = blockLabel;
    block.instructions = std::move(instrs);
    fn.blocks.push_back(std::move(block));
    return fn;
}

} // namespace

// ---------------------------------------------------------------------------
// Pass 1a: MOV #0 -> XOR
// ---------------------------------------------------------------------------

TEST(X86Peephole, MovZeroToXor) {
    // mov rax, #0 should become xor eax, eax
    auto fn = makeFunc(".Lentry",
                       {
                           MInstr{MOpcode::MOVri, {gpr(PhysReg::RAX), imm(0)}},
                           MInstr{MOpcode::RET, {}},
                       });

    auto count = runPeepholes(fn);
    EXPECT_TRUE(count > 0U);

    auto &instrs = fn.blocks[0].instructions;
    ASSERT_TRUE(instrs.size() >= 1U);
    // First instruction should now be XORrr32
    EXPECT_EQ(instrs[0].opcode, MOpcode::XORrr32);
}

TEST(X86Peephole, MovZeroToXorSkipsWhenFlagsRead) {
    // mov rax, #0 followed by JCC reading flags — should NOT rewrite to XOR
    auto fn = makeFunc(".Lentry",
                       {
                           MInstr{MOpcode::CMPrr, {gpr(PhysReg::RBX), gpr(PhysReg::RCX)}},
                           MInstr{MOpcode::MOVri, {gpr(PhysReg::RAX), imm(0)}},
                           MInstr{MOpcode::JCC, {imm(0), lbl(".Ltarget")}},
                           MInstr{MOpcode::RET, {}},
                       });

    runPeepholes(fn);

    auto &instrs = fn.blocks[0].instructions;
    // The MOVri should still be there since JCC reads flags
    bool hasMov = false;
    for (auto &i : instrs) {
        if (i.opcode == MOpcode::MOVri)
            hasMov = true;
    }
    EXPECT_TRUE(hasMov);
}

// ---------------------------------------------------------------------------
// Pass 1b: CMP #0 -> TEST
// ---------------------------------------------------------------------------

TEST(X86Peephole, CmpZeroToTest) {
    // cmp rax, #0 should become test rax, rax
    auto fn = makeFunc(".Lentry",
                       {
                           MInstr{MOpcode::CMPri, {gpr(PhysReg::RAX), imm(0)}},
                           MInstr{MOpcode::JCC, {imm(0), lbl(".Ltarget")}},
                           MInstr{MOpcode::RET, {}},
                       });

    auto count = runPeepholes(fn);
    EXPECT_TRUE(count > 0U);

    auto &instrs = fn.blocks[0].instructions;
    // First instruction should now be TESTrr
    bool hasTest = false;
    for (auto &i : instrs) {
        if (i.opcode == MOpcode::TESTrr)
            hasTest = true;
    }
    EXPECT_TRUE(hasTest);
}

// ---------------------------------------------------------------------------
// Pass 1c: Arithmetic Identity Elimination
// ---------------------------------------------------------------------------

TEST(X86Peephole, ArithIdentityAddZero) {
    // add rax, #0 should be eliminated (when flags not read)
    auto fn = makeFunc(".Lentry",
                       {
                           MInstr{MOpcode::ADDri, {gpr(PhysReg::RAX), imm(0)}},
                           MInstr{MOpcode::RET, {}},
                       });

    auto count = runPeepholes(fn);
    EXPECT_TRUE(count > 0U);

    // add #0 should be removed, leaving only RET
    auto &instrs = fn.blocks[0].instructions;
    for (auto &i : instrs) {
        EXPECT_NE(i.opcode, MOpcode::ADDri);
    }
}

TEST(X86Peephole, ArithIdentityShiftZero) {
    // shl rax, #0 should be eliminated (when flags not read)
    auto fn = makeFunc(".Lentry",
                       {
                           MInstr{MOpcode::SHLri, {gpr(PhysReg::RAX), imm(0)}},
                           MInstr{MOpcode::RET, {}},
                       });

    auto count = runPeepholes(fn);
    EXPECT_TRUE(count > 0U);

    auto &instrs = fn.blocks[0].instructions;
    for (auto &i : instrs) {
        EXPECT_NE(i.opcode, MOpcode::SHLri);
    }
}

TEST(X86Peephole, ArithIdentityAndAllOnes) {
    // and rax, #-1 should be eliminated (when flags not read)
    auto fn = makeFunc(".Lentry",
                       {
                           MInstr{MOpcode::ANDri, {gpr(PhysReg::RAX), imm(-1)}},
                           MInstr{MOpcode::RET, {}},
                       });

    auto count = runPeepholes(fn);
    EXPECT_TRUE(count > 0U);

    auto &instrs = fn.blocks[0].instructions;
    for (auto &i : instrs) {
        EXPECT_NE(i.opcode, MOpcode::ANDri);
    }
}

TEST(X86Peephole, ArithIdentityOrZero) {
    // or rax, #0 should be eliminated
    auto fn = makeFunc(".Lentry",
                       {
                           MInstr{MOpcode::ORri, {gpr(PhysReg::RAX), imm(0)}},
                           MInstr{MOpcode::RET, {}},
                       });

    runPeepholes(fn);

    auto &instrs = fn.blocks[0].instructions;
    for (auto &i : instrs) {
        EXPECT_NE(i.opcode, MOpcode::ORri);
    }
}

TEST(X86Peephole, ArithIdentityPreservedWhenFlagsRead) {
    // add rax, #0 followed by JCC reading flags — should NOT be removed
    auto fn = makeFunc(".Lentry",
                       {
                           MInstr{MOpcode::ADDri, {gpr(PhysReg::RAX), imm(0)}},
                           MInstr{MOpcode::JCC, {imm(0), lbl(".Ltarget")}},
                           MInstr{MOpcode::RET, {}},
                       });

    runPeepholes(fn);

    auto &instrs = fn.blocks[0].instructions;
    bool hasAdd = false;
    for (auto &i : instrs) {
        if (i.opcode == MOpcode::ADDri)
            hasAdd = true;
    }
    EXPECT_TRUE(hasAdd);
}

// ---------------------------------------------------------------------------
// Pass 1d: Strength Reduction (MUL power-of-2 -> SHL)
// ---------------------------------------------------------------------------

TEST(X86Peephole, StrengthReductionMulPow2) {
    // Load #8 into rcx, then imul rax, rcx -> shl rax, #3
    auto fn = makeFunc(".Lentry",
                       {
                           MInstr{MOpcode::MOVri, {gpr(PhysReg::RCX), imm(8)}},
                           MInstr{MOpcode::IMULrr, {gpr(PhysReg::RAX), gpr(PhysReg::RCX)}},
                           MInstr{MOpcode::RET, {}},
                       });

    auto count = runPeepholes(fn);
    EXPECT_TRUE(count > 0U);

    auto &instrs = fn.blocks[0].instructions;
    bool hasShl = false;
    for (auto &i : instrs) {
        if (i.opcode == MOpcode::SHLri) {
            hasShl = true;
            // Should shift by 3 (since 2^3 = 8)
            ASSERT_TRUE(i.operands.size() >= 2U);
            auto *shiftImm = std::get_if<OpImm>(&i.operands[1]);
            ASSERT_NE(shiftImm, nullptr);
            EXPECT_EQ(shiftImm->val, 3);
        }
    }
    EXPECT_TRUE(hasShl);
}

TEST(X86Peephole, StrengthReductionNoRewriteNonPow2) {
    // Load #7 into rcx, then imul rax, rcx — 7 is not power-of-2, no rewrite
    auto fn = makeFunc(".Lentry",
                       {
                           MInstr{MOpcode::MOVri, {gpr(PhysReg::RCX), imm(7)}},
                           MInstr{MOpcode::IMULrr, {gpr(PhysReg::RAX), gpr(PhysReg::RCX)}},
                           MInstr{MOpcode::RET, {}},
                       });

    runPeepholes(fn);

    auto &instrs = fn.blocks[0].instructions;
    bool hasImul = false;
    for (auto &i : instrs) {
        if (i.opcode == MOpcode::IMULrr)
            hasImul = true;
    }
    EXPECT_TRUE(hasImul);
}

TEST(X86Peephole, StrengthReductionInvalidatesPopDefinedConstant) {
    auto fn = makeFunc(".Lentry",
                       {
                           MInstr{MOpcode::MOVri, {gpr(PhysReg::RCX), imm(8)}},
                           MInstr{MOpcode::POP, {gpr(PhysReg::RCX)}},
                           MInstr{MOpcode::IMULrr, {gpr(PhysReg::RAX), gpr(PhysReg::RCX)}},
                           MInstr{MOpcode::RET, {}},
                       });

    runPeepholes(fn);

    bool hasImul = false;
    bool hasShl = false;
    for (const auto &instr : fn.blocks[0].instructions) {
        hasImul = hasImul || instr.opcode == MOpcode::IMULrr;
        hasShl = hasShl || instr.opcode == MOpcode::SHLri;
    }
    EXPECT_TRUE(hasImul);
    EXPECT_FALSE(hasShl);
}

// ---------------------------------------------------------------------------
// Pass 3: Identity Move Removal
// ---------------------------------------------------------------------------

TEST(X86Peephole, RemoveIdentityMovRR) {
    // mov rax, rax (identity) should be removed
    auto fn =
        makeFunc(".Lentry",
                 {
                     MInstr{MOpcode::MOVrr, {gpr(PhysReg::RAX), gpr(PhysReg::RBX)}}, // non-identity
                     MInstr{MOpcode::MOVrr, {gpr(PhysReg::RAX), gpr(PhysReg::RAX)}}, // identity
                     MInstr{MOpcode::MOVrr, {gpr(PhysReg::RCX), gpr(PhysReg::RCX)}}, // identity
                     MInstr{MOpcode::RET, {}},
                 });

    auto count = runPeepholes(fn);
    EXPECT_TRUE(count > 0U);

    auto &instrs = fn.blocks[0].instructions;
    // Should remove 2 identity moves, leaving non-identity mov + ret
    int movCount = 0;
    for (auto &i : instrs) {
        if (i.opcode == MOpcode::MOVrr)
            ++movCount;
    }
    EXPECT_EQ(movCount, 1); // Only the non-identity mov remains
}

TEST(X86Peephole, RemoveIdentityMovSDRR) {
    // movsd xmm0, xmm0 (identity) should be removed
    auto fn = makeFunc(
        ".Lentry",
        {
            MInstr{MOpcode::MOVSDrr, {xmm(PhysReg::XMM0), xmm(PhysReg::XMM1)}}, // non-identity
            MInstr{MOpcode::MOVSDrr, {xmm(PhysReg::XMM0), xmm(PhysReg::XMM0)}}, // identity
            MInstr{MOpcode::RET, {}},
        });

    auto count = runPeepholes(fn);
    EXPECT_TRUE(count > 0U);

    auto &instrs = fn.blocks[0].instructions;
    int movsdCount = 0;
    for (auto &i : instrs) {
        if (i.opcode == MOpcode::MOVSDrr)
            ++movsdCount;
    }
    EXPECT_EQ(movsdCount, 1);
}

TEST(X86Peephole, ConsecutiveMoveFoldPreservesMemoryBaseUse) {
    auto fn = makeFunc(".Lentry",
                       {
                           MInstr{MOpcode::MOVrr, {gpr(PhysReg::R10), gpr(PhysReg::RBX)}},
                           MInstr{MOpcode::MOVrr, {gpr(PhysReg::RCX), gpr(PhysReg::R10)}},
                           MInstr{MOpcode::MOVmr, {gpr(PhysReg::RAX), mem(PhysReg::R10, 0)}},
                           MInstr{MOpcode::RET, {}},
                       });

    runPeepholes(fn);

    bool hasBaseDef = false;
    for (const auto &instr : fn.blocks[0].instructions) {
        if (instr.opcode == MOpcode::MOVrr && instr.operands.size() == 2 &&
            sameRegOperand(instr.operands[0], gpr(PhysReg::R10)) &&
            sameRegOperand(instr.operands[1], gpr(PhysReg::RBX))) {
            hasBaseDef = true;
        }
    }
    EXPECT_TRUE(hasBaseDef);
}

TEST(X86Peephole, ConsecutiveMoveFoldPreservesReadModifyWriteUse) {
    auto fn = makeFunc(".Lentry",
                       {
                           MInstr{MOpcode::MOVrr, {gpr(PhysReg::R10), gpr(PhysReg::RBX)}},
                           MInstr{MOpcode::MOVrr, {gpr(PhysReg::RCX), gpr(PhysReg::R10)}},
                           MInstr{MOpcode::ADDri, {gpr(PhysReg::R10), imm(5)}},
                           MInstr{MOpcode::MOVrr, {gpr(PhysReg::RAX), gpr(PhysReg::R10)}},
                           MInstr{MOpcode::RET, {}},
                       });

    runPeepholes(fn);

    bool hasInitialMove = false;
    for (const auto &instr : fn.blocks[0].instructions) {
        if (instr.opcode == MOpcode::MOVrr && instr.operands.size() == 2 &&
            sameRegOperand(instr.operands[0], gpr(PhysReg::R10)) &&
            sameRegOperand(instr.operands[1], gpr(PhysReg::RBX))) {
            hasInitialMove = true;
        }
    }
    EXPECT_TRUE(hasInitialMove);
}

TEST(X86Peephole, ConsecutiveMoveFoldPreservesReturnRegisterUse) {
    auto fn = makeFunc(".Lentry",
                       {
                           MInstr{MOpcode::MOVrr, {gpr(PhysReg::RAX), gpr(PhysReg::RBX)}},
                           MInstr{MOpcode::MOVrr, {gpr(PhysReg::RCX), gpr(PhysReg::RAX)}},
                           MInstr{MOpcode::RET, {}},
                       });

    runPeepholes(fn);

    bool hasReturnMove = false;
    for (const auto &instr : fn.blocks[0].instructions) {
        if (instr.opcode == MOpcode::MOVrr && instr.operands.size() == 2 &&
            sameRegOperand(instr.operands[0], gpr(PhysReg::RAX)) &&
            sameRegOperand(instr.operands[1], gpr(PhysReg::RBX))) {
            hasReturnMove = true;
        }
    }
    EXPECT_TRUE(hasReturnMove);
}

// ---------------------------------------------------------------------------
// Pass 9: Conditional Branch Inversion
// ---------------------------------------------------------------------------

TEST(X86Peephole, BranchInversion) {
    // JCC(eq, .Lnext) / JMP(.Lexit) where .Lnext is the next block
    // Should become JCC(ne, .Lexit) with JMP removed.
    MFunction fn{};
    fn.name = "test_branch_inv";

    MBasicBlock bb1{};
    bb1.label = ".Lentry";
    bb1.instructions = {
        MInstr{MOpcode::CMPrr, {gpr(PhysReg::RAX), gpr(PhysReg::RBX)}},
        MInstr{MOpcode::JCC, {imm(0), lbl(".Lnext")}}, // cc=0 is eq
        MInstr{MOpcode::JMP, {lbl(".Lexit")}},
    };

    MBasicBlock bb2{};
    bb2.label = ".Lnext";
    bb2.instructions = {
        MInstr{MOpcode::RET, {}},
    };

    MBasicBlock bb3{};
    bb3.label = ".Lexit";
    bb3.instructions = {
        MInstr{MOpcode::RET, {}},
    };

    fn.blocks = {std::move(bb1), std::move(bb2), std::move(bb3)};

    auto count = runPeepholes(fn);
    EXPECT_TRUE(count > 0U);

    // After inversion: .Lentry should have CMP + JCC(ne, .Lexit), no JMP
    auto &entryInstrs = fn.blocks[0].instructions;
    bool hasJmp = false;
    for (auto &i : entryInstrs) {
        if (i.opcode == MOpcode::JMP)
            hasJmp = true;
    }
    EXPECT_FALSE(hasJmp);
}

// ---------------------------------------------------------------------------
// Pass 10: Fallthrough Jump Removal
// ---------------------------------------------------------------------------

TEST(X86Peephole, FallthroughJumpRemoval) {
    // JMP .Lnext where .Lnext is the immediately following block
    MFunction fn{};
    fn.name = "test_fallthrough";

    MBasicBlock bb1{};
    bb1.label = ".Lentry";
    bb1.instructions = {
        MInstr{MOpcode::MOVrr, {gpr(PhysReg::RAX), gpr(PhysReg::RBX)}},
        MInstr{MOpcode::JMP, {lbl(".Lnext")}},
    };

    MBasicBlock bb2{};
    bb2.label = ".Lnext";
    bb2.instructions = {
        MInstr{MOpcode::RET, {}},
    };

    fn.blocks = {std::move(bb1), std::move(bb2)};

    auto count = runPeepholes(fn);
    EXPECT_TRUE(count > 0U);

    // The JMP to the next block should be removed
    auto &entryInstrs = fn.blocks[0].instructions;
    bool hasJmp = false;
    for (auto &i : entryInstrs) {
        if (i.opcode == MOpcode::JMP)
            hasJmp = true;
    }
    EXPECT_FALSE(hasJmp);
}

TEST(X86Peephole, TraceLayoutPreservesJccOnlyFallthrough) {
    MFunction fn{};
    fn.name = "test_jcc_fallthrough";

    MBasicBlock entry{};
    entry.label = ".Lentry";
    entry.instructions = {
        MInstr{MOpcode::TESTrr, {gpr(PhysReg::RAX), gpr(PhysReg::RAX)}},
        MInstr{MOpcode::JCC, {imm(1), lbl(".Ltrue")}},
        MInstr{MOpcode::JMP, {lbl(".Lfalse")}},
    };

    MBasicBlock trueBlock{};
    trueBlock.label = ".Ltrue";
    trueBlock.instructions = {
        MInstr{MOpcode::RET, {}},
    };

    MBasicBlock falseBlock{};
    falseBlock.label = ".Lfalse";
    falseBlock.instructions = {
        MInstr{MOpcode::RET, {}},
    };

    fn.blocks = {std::move(entry), std::move(falseBlock), std::move(trueBlock)};

    runPeepholes(fn);
    ASSERT_GE(fn.blocks.size(), 3U);
    EXPECT_EQ(fn.blocks[0].label, ".Lentry");
    EXPECT_EQ(fn.blocks[1].label, ".Lfalse");
    EXPECT_EQ(fn.blocks[2].label, ".Ltrue");

    const auto &entryInstrs = fn.blocks[0].instructions;
    ASSERT_GE(entryInstrs.size(), 3U);
    EXPECT_EQ(entryInstrs[1].opcode, MOpcode::JCC);
    const auto *trueTarget = std::get_if<OpLabel>(&entryInstrs[1].operands[1]);
    ASSERT_NE(trueTarget, nullptr);
    EXPECT_EQ(trueTarget->name, ".Ltrue");
    EXPECT_EQ(entryInstrs.back().opcode, MOpcode::JMP);
    const auto *falseTarget = std::get_if<OpLabel>(&entryInstrs.back().operands[0]);
    ASSERT_NE(falseTarget, nullptr);
    EXPECT_EQ(falseTarget->name, ".Lfalse");
}

TEST(X86Peephole, FallthroughJumpAfterJccIsPreserved) {
    MFunction fn{};
    fn.name = "test_jcc_explicit_false_edge";

    MBasicBlock entry{};
    entry.label = ".Lentry";
    entry.instructions = {
        MInstr{MOpcode::TESTrr, {gpr(PhysReg::RAX), gpr(PhysReg::RAX)}},
        MInstr{MOpcode::JCC, {imm(1), lbl(".Ltrue")}},
        MInstr{MOpcode::JMP, {lbl(".Lfalse")}},
    };

    MBasicBlock falseBlock{};
    falseBlock.label = ".Lfalse";
    falseBlock.instructions = {
        MInstr{MOpcode::RET, {}},
    };

    MBasicBlock trueBlock{};
    trueBlock.label = ".Ltrue";
    trueBlock.instructions = {
        MInstr{MOpcode::RET, {}},
    };

    fn.blocks = {std::move(entry), std::move(falseBlock), std::move(trueBlock)};

    runPeepholes(fn);

    const auto &entryInstrs = fn.blocks[0].instructions;
    ASSERT_GE(entryInstrs.size(), 3U);
    EXPECT_EQ(entryInstrs[1].opcode, MOpcode::JCC);
    EXPECT_EQ(entryInstrs[2].opcode, MOpcode::JMP);
}

TEST(X86Peephole, ColdBlockMovePreservesJccOnlyFallthrough) {
    MFunction fn{};
    fn.name = "test_cold_jcc_fallthrough";

    MBasicBlock entry{};
    entry.label = ".Lentry";
    entry.instructions = {
        MInstr{MOpcode::TESTrr, {gpr(PhysReg::RAX), gpr(PhysReg::RAX)}},
        MInstr{MOpcode::JCC, {imm(1), lbl(".Lhot")}},
    };

    MBasicBlock trap{};
    trap.label = ".Ltrap_div0";
    trap.instructions = {
        MInstr{MOpcode::UD2, {}},
    };

    MBasicBlock hot{};
    hot.label = ".Lhot";
    hot.instructions = {
        MInstr{MOpcode::RET, {}},
    };

    fn.blocks = {std::move(entry), std::move(trap), std::move(hot)};

    runPeepholes(fn);

    ASSERT_EQ(fn.blocks.size(), 3U);
    EXPECT_EQ(fn.blocks[0].label, ".Lentry");
    EXPECT_EQ(fn.blocks[1].label, ".Ltrap_div0");
    EXPECT_EQ(fn.blocks[2].label, ".Lhot");
}

// ---------------------------------------------------------------------------
// Pass 8: Branch Chain Elimination
// ---------------------------------------------------------------------------

TEST(X86Peephole, BranchChainElimination) {
    // .Lentry: JMP .Ltrampoline
    // .Ltrampoline: JMP .Ltarget (single-JMP block)
    // .Ltarget: RET
    // Should retarget .Lentry's JMP directly to .Ltarget.
    MFunction fn{};
    fn.name = "test_branch_chain";

    MBasicBlock bb1{};
    bb1.label = ".Lentry";
    bb1.instructions = {
        MInstr{MOpcode::JMP, {lbl(".Ltrampoline")}},
    };

    MBasicBlock bb2{};
    bb2.label = ".Ltrampoline";
    bb2.instructions = {
        MInstr{MOpcode::JMP, {lbl(".Ltarget")}},
    };

    MBasicBlock bb3{};
    bb3.label = ".Ltarget";
    bb3.instructions = {
        MInstr{MOpcode::RET, {}},
    };

    fn.blocks = {std::move(bb1), std::move(bb2), std::move(bb3)};

    auto count = runPeepholes(fn);
    EXPECT_TRUE(count > 0U);

    // The entry block should now jump directly to .Ltarget
    // (may have been further optimized by trace layout and fallthrough removal,
    // but the chain should be resolved)
    bool chainResolved = true;
    for (auto &block : fn.blocks) {
        for (auto &i : block.instructions) {
            if (i.opcode == MOpcode::JMP) {
                auto *target = std::get_if<OpLabel>(&i.operands[0]);
                if (target && target->name == ".Ltrampoline")
                    chainResolved = false;
            }
        }
    }
    EXPECT_TRUE(chainResolved);
}

// ---------------------------------------------------------------------------
// Pass 7: Cold Block Reordering
// ---------------------------------------------------------------------------

TEST(X86Peephole, ColdBlockMovedToEnd) {
    // Blocks: entry, trap_div0, body — trap should move after body
    MFunction fn{};
    fn.name = "test_cold_block";

    MBasicBlock entry{};
    entry.label = ".Lentry";
    entry.instructions = {
        MInstr{MOpcode::JMP, {lbl(".Lbody")}},
    };

    MBasicBlock trap{};
    trap.label = ".Ltrap_div0";
    trap.instructions = {
        MInstr{MOpcode::UD2, {}},
    };

    MBasicBlock body{};
    body.label = ".Lbody";
    body.instructions = {
        MInstr{MOpcode::RET, {}},
    };

    fn.blocks = {std::move(entry), std::move(trap), std::move(body)};

    runPeepholes(fn);

    // Entry should still be first
    EXPECT_EQ(fn.blocks[0].label, ".Lentry");
    // Trap block (cold) should be last
    EXPECT_EQ(fn.blocks.back().label, ".Ltrap_div0");
}

TEST(X86Peephole, StoreLoadForwardingReplacesFrameReload) {
    auto fn = makeFunc(".Lentry",
                       {
                           MInstr{MOpcode::MOVrm, {mem(PhysReg::RBP, -8), gpr(PhysReg::RDX)}},
                           MInstr{MOpcode::MOVmr, {gpr(PhysReg::RAX), mem(PhysReg::RBP, -8)}},
                           MInstr{MOpcode::RET, {}},
                       });

    auto count = runPeepholes(fn);
    EXPECT_TRUE(count > 0U);

    bool hasForwardedMove = false;
    bool hasReload = false;
    for (const auto &instr : fn.blocks[0].instructions) {
        if (instr.opcode == MOpcode::MOVrr && instr.operands.size() == 2 &&
            sameRegOperand(instr.operands[0], gpr(PhysReg::RAX)) &&
            sameRegOperand(instr.operands[1], gpr(PhysReg::RDX)))
            hasForwardedMove = true;
        if (instr.opcode == MOpcode::MOVmr)
            hasReload = true;
    }
    EXPECT_TRUE(hasForwardedMove);
    EXPECT_FALSE(hasReload);
}

TEST(X86Peephole, DeadFrameStoreEliminatesOverwrittenStore) {
    auto fn = makeFunc(".Lentry",
                       {
                           MInstr{MOpcode::MOVrm, {mem(PhysReg::RBP, -8), gpr(PhysReg::RCX)}},
                           MInstr{MOpcode::MOVrm, {mem(PhysReg::RBP, -8), gpr(PhysReg::RDX)}},
                           MInstr{MOpcode::MOVmr, {gpr(PhysReg::RAX), mem(PhysReg::RBP, -8)}},
                           MInstr{MOpcode::RET, {}},
                       });

    auto count = runPeepholes(fn);
    EXPECT_TRUE(count > 0U);

    int stores = 0;
    for (const auto &instr : fn.blocks[0].instructions) {
        if (instr.opcode == MOpcode::MOVrm)
            ++stores;
    }
    EXPECT_EQ(stores, 1);
}

TEST(X86Peephole, DeadFlagProducerRemovedWhenFlagsUnused) {
    auto fn = makeFunc(".Lentry",
                       {
                           MInstr{MOpcode::CMPri, {gpr(PhysReg::RBX), imm(0)}},
                           MInstr{MOpcode::MOVri, {gpr(PhysReg::RAX), imm(1)}},
                           MInstr{MOpcode::RET, {}},
                       });

    auto count = runPeepholes(fn);
    EXPECT_TRUE(count > 0U);

    for (const auto &instr : fn.blocks[0].instructions) {
        EXPECT_NE(instr.opcode, MOpcode::CMPri);
        EXPECT_NE(instr.opcode, MOpcode::TESTrr);
    }
}

TEST(X86Peephole, DcePreservesPhysicalRegisterCarriedToFallthroughBlock) {
    MFunction fn{};
    fn.name = "test_dce_fallthrough_carry";

    MBasicBlock entry{};
    entry.label = ".Lentry";
    entry.instructions = {
        MInstr{MOpcode::MOVri, {gpr(PhysReg::R10), imm(42)}},
    };

    MBasicBlock next{};
    next.label = ".Lnext";
    next.instructions = {
        MInstr{MOpcode::MOVrr, {gpr(PhysReg::RAX), gpr(PhysReg::R10)}},
        MInstr{MOpcode::RET, {}},
    };

    fn.blocks = {std::move(entry), std::move(next)};
    runPeepholes(fn);

    bool hasCarryDef = false;
    for (const auto &instr : fn.blocks[0].instructions) {
        if (instr.opcode == MOpcode::MOVri && instr.operands.size() == 2 &&
            sameRegOperand(instr.operands[0], gpr(PhysReg::R10))) {
            hasCarryDef = true;
        }
    }
    EXPECT_TRUE(hasCarryDef);
}

TEST(X86Peephole, FlagProducingDeadRegisterKeptWhenBranchReadsFlags) {
    auto fn = makeFunc(".Lentry",
                       {
                           MInstr{MOpcode::ADDrr, {gpr(PhysReg::RBX), gpr(PhysReg::RCX)}},
                           MInstr{MOpcode::JCC, {imm(1), lbl(".Ltarget")}},
                           MInstr{MOpcode::RET, {}},
                       });

    runPeepholes(fn);

    bool hasAdd = false;
    for (const auto &instr : fn.blocks[0].instructions) {
        if (instr.opcode == MOpcode::ADDrr)
            hasAdd = true;
    }
    EXPECT_TRUE(hasAdd);
}

TEST(X86Peephole, CallArgumentDCEHonorsTargetABI) {
    auto makeCallFunc = [] {
        return makeFunc(".Lentry",
                        {
                            MInstr{MOpcode::MOVSDrr, {xmm(PhysReg::XMM4), xmm(PhysReg::XMM5)}},
                            MInstr{MOpcode::CALL, {lbl("callee")}},
                            MInstr{MOpcode::RET, {}},
                        });
    };

    auto sysvFn = makeCallFunc();
    auto winFn = makeCallFunc();

    runPeepholes(sysvFn, sysvTarget());
    runPeepholes(winFn, win64Target());

    auto hasXmm4Move = [](const MFunction &fn) {
        for (const auto &instr : fn.blocks[0].instructions) {
            if (instr.opcode == MOpcode::MOVSDrr && instr.operands.size() == 2 &&
                sameRegOperand(instr.operands[0], xmm(PhysReg::XMM4)))
                return true;
        }
        return false;
    };

    EXPECT_TRUE(hasXmm4Move(sysvFn));
    EXPECT_FALSE(hasXmm4Move(winFn));
}

TEST(X86Scheduler, PrioritizesLoadUseChainWithinBlock) {
    auto fn = makeFunc(".Lentry",
                       {
                           MInstr{MOpcode::ADDrr, {gpr(PhysReg::RBX), gpr(PhysReg::RCX)}},
                           MInstr{MOpcode::MOVmr, {gpr(PhysReg::RAX), mem(PhysReg::RBP, -8)}},
                           MInstr{MOpcode::ADDrr, {gpr(PhysReg::RAX), gpr(PhysReg::RDX)}},
                           MInstr{MOpcode::RET, {}},
                       });

    const auto changed = scheduleFunction(fn);
    EXPECT_TRUE(changed > 0U);
    ASSERT_FALSE(fn.blocks[0].instructions.empty());
    EXPECT_EQ(fn.blocks[0].instructions[0].opcode, MOpcode::MOVmr);
}

TEST(X86Scheduler, HoistsLoadAboveStoreToDisjointFrameSlot) {
    // A long-latency load from rbp-16 feeding an add may hoist above a store
    // to the DIFFERENT slot rbp-8: 8-byte frame slots with distinct
    // displacements are provably disjoint.
    auto fn = makeFunc(".Lentry",
                       {
                           MInstr{MOpcode::MOVrm, {mem(PhysReg::RBP, -8), gpr(PhysReg::RBX)}},
                           MInstr{MOpcode::MOVmr, {gpr(PhysReg::RAX), mem(PhysReg::RBP, -16)}},
                           MInstr{MOpcode::ADDrr, {gpr(PhysReg::RAX), gpr(PhysReg::RDX)}},
                           MInstr{MOpcode::RET, {}},
                       });

    const auto changed = scheduleFunction(fn);
    EXPECT_TRUE(changed > 0U);
    ASSERT_FALSE(fn.blocks[0].instructions.empty());
    EXPECT_EQ(fn.blocks[0].instructions[0].opcode, MOpcode::MOVmr);
}

TEST(X86Scheduler, KeepsStoreLoadOrderOnSameFrameSlot) {
    auto fn = makeFunc(".Lentry",
                       {
                           MInstr{MOpcode::MOVrm, {mem(PhysReg::RBP, -8), gpr(PhysReg::RBX)}},
                           MInstr{MOpcode::MOVmr, {gpr(PhysReg::RAX), mem(PhysReg::RBP, -8)}},
                           MInstr{MOpcode::RET, {}},
                       });

    scheduleFunction(fn);
    const auto &instrs = fn.blocks[0].instructions;
    ASSERT_EQ(instrs.size(), 3U);
    EXPECT_EQ(instrs[0].opcode, MOpcode::MOVrm);
    EXPECT_EQ(instrs[1].opcode, MOpcode::MOVmr);
}

TEST(X86Scheduler, KeepsStoreOrderOnUnknownMemory) {
    // Stores through arbitrary registers may alias: order must be preserved.
    auto fn = makeFunc(".Lentry",
                       {
                           MInstr{MOpcode::MOVrm, {mem(PhysReg::RDI, 0), gpr(PhysReg::RBX)}},
                           MInstr{MOpcode::MOVrm, {mem(PhysReg::RSI, 0), gpr(PhysReg::RCX)}},
                           MInstr{MOpcode::RET, {}},
                       });

    scheduleFunction(fn);
    const auto &instrs = fn.blocks[0].instructions;
    ASSERT_EQ(instrs.size(), 3U);
    const auto *firstMem = std::get_if<OpMem>(&instrs[0].operands[0]);
    ASSERT_TRUE(firstMem != nullptr);
    EXPECT_EQ(static_cast<PhysReg>(firstMem->base.idOrPhys), PhysReg::RDI);
}

TEST(X86Peephole, DeadCodeEliminatesWholeUnusedMoveChainInOneRun) {
    auto fn = makeFunc(".Lentry",
                       {
                           MInstr{MOpcode::MOVri, {gpr(PhysReg::R10), imm(1)}},
                           MInstr{MOpcode::MOVrr, {gpr(PhysReg::R11), gpr(PhysReg::R10)}},
                           MInstr{MOpcode::MOVrr, {gpr(PhysReg::RCX), gpr(PhysReg::R11)}},
                           MInstr{MOpcode::RET, {}},
                       });

    const auto count = runPeepholes(fn);
    EXPECT_GE(count, 3U);
    ASSERT_EQ(fn.blocks[0].instructions.size(), 1U);
    EXPECT_EQ(fn.blocks[0].instructions[0].opcode, MOpcode::RET);
}

TEST(X86Peephole, LinearMoveFoldingHandlesChainedAdjacentMoves) {
    std::vector<MInstr> instrs = {
        MInstr{MOpcode::MOVrr, {gpr(PhysReg::R10), gpr(PhysReg::RAX)}},
        MInstr{MOpcode::MOVrr, {gpr(PhysReg::R11), gpr(PhysReg::R10)}},
        MInstr{MOpcode::MOVrr, {gpr(PhysReg::RCX), gpr(PhysReg::R11)}},
        MInstr{MOpcode::RET, {}},
    };

    peephole::PeepholeStats stats{};
    EXPECT_EQ(peephole::foldConsecutiveMoves(instrs, stats), 2U);
    EXPECT_EQ(stats.consecutiveMovsFolded, 2U);
    ASSERT_EQ(instrs[2].operands.size(), 2U);
    EXPECT_TRUE(sameRegOperand(instrs[2].operands[1], gpr(PhysReg::RAX)));
}

TEST(X86Peephole, LinearMoveFoldingPreservesRegistersCarriedToSuccessors) {
    std::vector<MInstr> instrs = {
        MInstr{MOpcode::MOVrr, {gpr(PhysReg::R10), gpr(PhysReg::RAX)}},
        MInstr{MOpcode::MOVrr, {gpr(PhysReg::R11), gpr(PhysReg::R10)}},
        MInstr{MOpcode::JMP, {lbl(".Lsuccessor")}},
    };

    peephole::PeepholeStats stats{};
    EXPECT_EQ(peephole::foldConsecutiveMoves(instrs, stats), 0U);
    EXPECT_EQ(stats.consecutiveMovsFolded, 0U);
    ASSERT_EQ(instrs[0].operands.size(), 2U);
    EXPECT_TRUE(sameRegOperand(instrs[0].operands[0], gpr(PhysReg::R10)));
}

TEST(X86Peephole, FrameStoreForwardingContinuesAcrossLea) {
    // LEA computes an address without touching memory; it must not act as a
    // memory barrier for store-to-load forwarding. Any later dereference of
    // the leaked address carries its own memory operand and barriers there.
    std::vector<MInstr> instrs = {
        MInstr{MOpcode::MOVrm, {mem(PhysReg::RBP, -8), gpr(PhysReg::RAX)}},
        MInstr{MOpcode::LEA, {gpr(PhysReg::RDX), mem(PhysReg::RBP, -8)}},
        MInstr{MOpcode::MOVmr, {gpr(PhysReg::RBX), mem(PhysReg::RBP, -8)}},
    };

    peephole::PeepholeStats stats{};
    EXPECT_EQ(peephole::forwardFrameStoreLoads(instrs, stats), 1U);
    EXPECT_EQ(instrs[2].opcode, MOpcode::MOVrr);
}

TEST(X86Peephole, FrameStoreForwardingBarriersAtDereferenceAfterLea) {
    // Writing through the LEA-derived pointer is an unknown memory access and
    // must clear the tracked forwarding state.
    std::vector<MInstr> instrs = {
        MInstr{MOpcode::MOVrm, {mem(PhysReg::RBP, -8), gpr(PhysReg::RAX)}},
        MInstr{MOpcode::LEA, {gpr(PhysReg::RDX), mem(PhysReg::RBP, -8)}},
        MInstr{MOpcode::MOVrm, {mem(PhysReg::RDX, 0), gpr(PhysReg::RCX)}},
        MInstr{MOpcode::MOVmr, {gpr(PhysReg::RBX), mem(PhysReg::RBP, -8)}},
    };

    peephole::PeepholeStats stats{};
    EXPECT_EQ(peephole::forwardFrameStoreLoads(instrs, stats), 0U);
    EXPECT_EQ(instrs[3].opcode, MOpcode::MOVmr);
}

TEST(X86Peephole, FrameStoreForwardingInvalidatesWhenStoredRegisterIsClobbered) {
    std::vector<MInstr> instrs = {
        MInstr{MOpcode::MOVrm, {mem(PhysReg::RBP, -8), gpr(PhysReg::RAX)}},
        MInstr{MOpcode::MOVmr, {gpr(PhysReg::RBX), mem(PhysReg::RBP, -8)}},
        MInstr{MOpcode::MOVri, {gpr(PhysReg::RAX), imm(7)}},
        MInstr{MOpcode::MOVmr, {gpr(PhysReg::RCX), mem(PhysReg::RBP, -8)}},
    };

    peephole::PeepholeStats stats{};
    EXPECT_EQ(peephole::forwardFrameStoreLoads(instrs, stats), 1U);
    EXPECT_EQ(instrs[1].opcode, MOpcode::MOVrr);
    EXPECT_EQ(instrs[3].opcode, MOpcode::MOVmr);
}

// The allocator spills a value living in RDX right before `cqo` because the
// division sequence clobbers RDX implicitly. A later reload of that slot must
// not be rewritten into `mov rbx, rdx`: RDX now holds the remainder.
TEST(X86Peephole, FrameStoreForwardingStopsAtImplicitDivisionClobber) {
    std::vector<MInstr> instrs = {
        MInstr{MOpcode::MOVrm, {mem(PhysReg::RBP, -16), gpr(PhysReg::RDX)}},
        MInstr{MOpcode::MOVrr, {gpr(PhysReg::RAX), gpr(PhysReg::RSI)}},
        MInstr{MOpcode::CQO, {}},
        MInstr{MOpcode::IDIVrm, {gpr(PhysReg::RCX)}},
        MInstr{MOpcode::MOVrr, {gpr(PhysReg::RDI), gpr(PhysReg::RDX)}},
        MInstr{MOpcode::MOVmr, {gpr(PhysReg::RBX), mem(PhysReg::RBP, -16)}},
    };
    peephole::PeepholeStats stats{};
    EXPECT_EQ(peephole::forwardFrameStoreLoads(instrs, stats), 0U);
    EXPECT_EQ(instrs[5].opcode, MOpcode::MOVmr);
}

// Same hazard for the widening multiply, which writes RAX:RDX implicitly.
TEST(X86Peephole, FrameStoreForwardingStopsAtImplicitMultiplyClobber) {
    std::vector<MInstr> instrs = {
        MInstr{MOpcode::MOVrm, {mem(PhysReg::RBP, -16), gpr(PhysReg::RAX)}},
        MInstr{MOpcode::IMULr, {gpr(PhysReg::RCX)}},
        MInstr{MOpcode::MOVmr, {gpr(PhysReg::RBX), mem(PhysReg::RBP, -16)}},
    };
    peephole::PeepholeStats stats{};
    EXPECT_EQ(peephole::forwardFrameStoreLoads(instrs, stats), 0U);
    EXPECT_EQ(instrs[2].opcode, MOpcode::MOVmr);
}

// The shared effects model: implicit fixed registers, ABI sets, flags, memory.
TEST(X86Peephole, EffectsTableModelsImplicitRegistersAndAbi) {
    const TargetInfo &sysv = sysvTarget();
    const auto bit = [](PhysReg r) { return physRegBit(r); };
    {
        const InstrEffects fx = effectsOf(MInstr{MOpcode::CQO, {}}, sysv);
        EXPECT_TRUE((fx.uses & bit(PhysReg::RAX)) != 0);
        EXPECT_TRUE((fx.defs & bit(PhysReg::RDX)) != 0);
        EXPECT_TRUE((fx.defs & bit(PhysReg::RAX)) == 0);
    }
    {
        const InstrEffects fx = effectsOf(MInstr{MOpcode::IDIVrm, {gpr(PhysReg::RCX)}}, sysv);
        EXPECT_TRUE((fx.uses & bit(PhysReg::RCX)) != 0);
        EXPECT_TRUE((fx.uses & bit(PhysReg::RAX)) != 0);
        EXPECT_TRUE((fx.uses & bit(PhysReg::RDX)) != 0);
        EXPECT_TRUE((fx.defs & bit(PhysReg::RAX)) != 0);
        EXPECT_TRUE((fx.defs & bit(PhysReg::RDX)) != 0);
        EXPECT_TRUE(fx.writesFlags);
    }
    {
        const InstrEffects fx = effectsOf(MInstr{MOpcode::IMULr, {gpr(PhysReg::RBX)}}, sysv);
        EXPECT_TRUE((fx.uses & bit(PhysReg::RAX)) != 0);
        EXPECT_TRUE((fx.defs & (bit(PhysReg::RAX) | bit(PhysReg::RDX))) ==
                    (bit(PhysReg::RAX) | bit(PhysReg::RDX)));
    }
    {
        const InstrEffects fx =
            effectsOf(MInstr{MOpcode::SHLrc, {gpr(PhysReg::RBX), gpr(PhysReg::RCX)}}, sysv);
        EXPECT_TRUE((fx.uses & bit(PhysReg::RCX)) != 0);
        EXPECT_TRUE((fx.uses & bit(PhysReg::RBX)) != 0);
        EXPECT_TRUE((fx.defs & bit(PhysReg::RBX)) != 0);
    }
    {
        const InstrEffects fx = effectsOf(MInstr{MOpcode::CALL, {lbl("callee")}}, sysv);
        EXPECT_TRUE(fx.isCall);
        EXPECT_TRUE(fx.memRead && fx.memWrite);
        for (std::size_t i = 0; i < sysv.maxGPRArgs; ++i)
            EXPECT_TRUE((fx.uses & bit(sysv.intArgOrder[i])) != 0);
        for (PhysReg r : sysv.callerSavedGPR)
            EXPECT_TRUE((fx.defs & bit(r)) != 0);
        for (PhysReg r : sysv.calleeSavedGPR)
            EXPECT_TRUE((fx.defs & bit(r)) == 0);
    }
    {
        const InstrEffects fx = effectsOf(MInstr{MOpcode::RET, {}}, sysv);
        EXPECT_TRUE(fx.isTerminator);
        EXPECT_TRUE((fx.uses & bit(sysv.intReturnReg)) != 0);
        EXPECT_TRUE((fx.uses & bit(sysv.f64ReturnReg)) != 0);
    }
    {
        // Memory operand: address registers are reads; the slot is written.
        const InstrEffects fx =
            effectsOf(MInstr{MOpcode::MOVrm, {mem(PhysReg::RBP, -8), gpr(PhysReg::RAX)}}, sysv);
        EXPECT_TRUE((fx.uses & bit(PhysReg::RBP)) != 0);
        EXPECT_TRUE((fx.uses & bit(PhysReg::RAX)) != 0);
        EXPECT_TRUE(fx.memWrite);
        EXPECT_FALSE(fx.memRead);
        EXPECT_TRUE(fx.defs == 0);
    }
    {
        // LEA computes an address without touching memory.
        const InstrEffects fx =
            effectsOf(MInstr{MOpcode::LEA, {gpr(PhysReg::RDX), mem(PhysReg::RBP, -8)}}, sysv);
        EXPECT_FALSE(fx.memRead || fx.memWrite);
        EXPECT_TRUE((fx.defs & bit(PhysReg::RDX)) != 0);
    }
    {
        // The jump-table dispatch sequence writes the reserved R10/R11 scratch.
        const InstrEffects fx = effectsOf(
            MInstr{MOpcode::JUMPTABLE, {gpr(PhysReg::RAX), lbl(".Ljt"), lbl("c0"), lbl("c1")}},
            sysv);
        EXPECT_TRUE(fx.isTerminator);
        EXPECT_TRUE((fx.uses & bit(PhysReg::RAX)) != 0);
        EXPECT_TRUE((fx.defs & (bit(PhysReg::R10) | bit(PhysReg::R11))) ==
                    (bit(PhysReg::R10) | bit(PhysReg::R11)));
    }
}

// A store of a register the division does not touch still forwards.
TEST(X86Peephole, FrameStoreForwardingSurvivesDivisionOfUnrelatedRegister) {
    std::vector<MInstr> instrs = {
        MInstr{MOpcode::MOVrm, {mem(PhysReg::RBP, -16), gpr(PhysReg::R12)}},
        MInstr{MOpcode::CQO, {}},
        MInstr{MOpcode::IDIVrm, {gpr(PhysReg::RCX)}},
        MInstr{MOpcode::MOVmr, {gpr(PhysReg::RBX), mem(PhysReg::RBP, -16)}},
    };
    peephole::PeepholeStats stats{};
    EXPECT_EQ(peephole::forwardFrameStoreLoads(instrs, stats), 1U);
    EXPECT_EQ(instrs[3].opcode, MOpcode::MOVrr);
}

// ---------------------------------------------------------------------------
// Multiple optimizations in combination
// ---------------------------------------------------------------------------

TEST(X86Peephole, CombinedOptimizations) {
    // Combines: MOV #0->XOR, identity mov removal, CMP #0->TEST
    auto fn = makeFunc(
        ".Lentry",
        {
            MInstr{MOpcode::MOVri, {gpr(PhysReg::RAX), imm(0)}},            // -> XOR
            MInstr{MOpcode::MOVrr, {gpr(PhysReg::RBX), gpr(PhysReg::RBX)}}, // identity, removed
            MInstr{MOpcode::CMPri, {gpr(PhysReg::RCX), imm(0)}},            // -> TEST
            MInstr{MOpcode::JCC, {imm(1), lbl(".Ltarget")}},
            MInstr{MOpcode::RET, {}},
        });

    auto count = runPeepholes(fn);
    EXPECT_TRUE(count > 0U);

    auto &instrs = fn.blocks[0].instructions;
    bool hasXor = false, hasTest = false, hasIdentityMov = false;
    for (auto &i : instrs) {
        if (i.opcode == MOpcode::XORrr32)
            hasXor = true;
        if (i.opcode == MOpcode::TESTrr)
            hasTest = true;
        // Check for identity mov (same src and dst)
        if (i.opcode == MOpcode::MOVrr && i.operands.size() == 2) {
            auto *d = std::get_if<OpReg>(&i.operands[0]);
            auto *s = std::get_if<OpReg>(&i.operands[1]);
            if (d && s && d->isPhys && s->isPhys && d->idOrPhys == s->idOrPhys)
                hasIdentityMov = true;
        }
    }
    EXPECT_TRUE(hasXor);
    EXPECT_TRUE(hasTest);
    EXPECT_FALSE(hasIdentityMov);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char **argv) {
    zanna_test::init(&argc, argv);
    return zanna_test::run_all_tests();
}
