//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/il/transform/test_Reassociate.cpp
// Purpose: Validate operand canonicalization for commutative+associative ops.
// Key invariants:
//   - Temporaries sort before constants.
//   - Non-commutative ops (Sub, SDiv) are untouched.
// Ownership/Lifetime: Transient modules.
// Links: il/transform/Reassociate.cpp
//
//===----------------------------------------------------------------------===//

#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Module.hpp"
#include "il/core/Opcode.hpp"
#include "il/core/Param.hpp"
#include "il/core/Type.hpp"
#include "il/core/Value.hpp"
#include "il/transform/EarlyCSE.hpp"
#include "il/transform/Reassociate.hpp"
#include "tests/TestHarness.hpp"

#include <limits>

using namespace il::core;

namespace {

Instr makeBinary(Opcode op, unsigned result, Value lhs, Value rhs) {
    Instr i;
    i.op = op;
    i.result = result;
    i.operands.push_back(lhs);
    i.operands.push_back(rhs);
    return i;
}

} // namespace

TEST(Reassociate, SwapsConstBeforeTemp) {
    // 1 + %0  →  %0 + 1  (temp should come first)
    Module mod;
    Function fn;
    fn.name = "test";
    fn.retType = Type(Type::Kind::I64);

    BasicBlock entry;
    entry.label = "entry";
    {
        Param p;
        p.name = "x";
        p.type = Type(Type::Kind::I64);
        p.id = 0;
        entry.params.push_back(std::move(p));
    }
    entry.instructions.push_back(makeBinary(Opcode::Add, 1, Value::constInt(1), Value::temp(0)));

    Instr ret;
    ret.op = Opcode::Ret;
    ret.operands.push_back(Value::temp(1));
    entry.instructions.push_back(std::move(ret));
    entry.terminated = true;

    fn.blocks.push_back(std::move(entry));
    mod.functions.push_back(std::move(fn));

    il::transform::reassociate(mod);

    const auto &add = mod.functions[0].blocks[0].instructions[0];
    EXPECT_EQ(add.operands[0].kind, Value::Kind::Temp);
    EXPECT_EQ(add.operands[1].kind, Value::Kind::ConstInt);
}

TEST(Reassociate, BranchArgumentsCountAsUses) {
    // %a = and %x, M ; %b = and %a, 8191 ; br next(%a)
    // %a is read twice: by %b and by the branch argument. Flattening the tree
    // rooted at %b must therefore keep %a as a leaf; rewriting %a's definition
    // into an internal node (M & 8191) would change the value `next` receives.
    constexpr long long kMask = 268435455;
    Module mod;
    Function fn;
    fn.name = "test";
    fn.retType = Type(Type::Kind::I64);

    BasicBlock entry;
    entry.label = "entry";
    {
        Param p;
        p.name = "x";
        p.type = Type(Type::Kind::I64);
        p.id = 0;
        entry.params.push_back(std::move(p));
    }
    entry.instructions.push_back(
        makeBinary(Opcode::And, 1, Value::temp(0), Value::constInt(kMask)));
    entry.instructions.push_back(makeBinary(Opcode::And, 2, Value::temp(1), Value::constInt(8191)));
    {
        Instr br;
        br.op = Opcode::Br;
        br.type = Type(Type::Kind::Void);
        br.addBranchTarget("next");
        br.brArgs.back().push_back(Value::temp(1));
        br.brArgs.back().push_back(Value::temp(2));
        entry.instructions.push_back(std::move(br));
    }
    entry.terminated = true;

    BasicBlock next;
    next.label = "next";
    for (unsigned id : {3u, 4u}) {
        Param p;
        p.name = id == 3 ? "p" : "q";
        p.type = Type(Type::Kind::I64);
        p.id = id;
        next.params.push_back(std::move(p));
    }
    {
        Instr ret;
        ret.op = Opcode::Ret;
        ret.operands.push_back(Value::temp(3));
        next.instructions.push_back(std::move(ret));
    }
    next.terminated = true;

    fn.blocks.push_back(std::move(entry));
    fn.blocks.push_back(std::move(next));
    mod.functions.push_back(std::move(fn));

    il::transform::reassociate(mod);

    const auto &defA = mod.functions[0].blocks[0].instructions[0];
    ASSERT_EQ(defA.operands.size(), 2u);
    const bool aReadsX = (defA.operands[0].kind == Value::Kind::Temp && defA.operands[0].id == 0) ||
                         (defA.operands[1].kind == Value::Kind::Temp && defA.operands[1].id == 0);
    const bool aMasks =
        (defA.operands[0].kind == Value::Kind::ConstInt && defA.operands[0].i64 == kMask) ||
        (defA.operands[1].kind == Value::Kind::ConstInt && defA.operands[1].i64 == kMask);
    EXPECT_TRUE(aReadsX);
    EXPECT_TRUE(aMasks);

    const auto &defB = mod.functions[0].blocks[0].instructions[1];
    ASSERT_EQ(defB.operands.size(), 2u);
    const bool bReadsA = (defB.operands[0].kind == Value::Kind::Temp && defB.operands[0].id == 1) ||
                         (defB.operands[1].kind == Value::Kind::Temp && defB.operands[1].id == 1);
    EXPECT_TRUE(bReadsA);
}

TEST(Reassociate, DoesNotSwapSubOperands) {
    // Sub is not commutative — operands must stay in order
    Module mod;
    Function fn;
    fn.name = "test";
    fn.retType = Type(Type::Kind::I64);

    BasicBlock entry;
    entry.label = "entry";
    {
        Param p;
        p.name = "x";
        p.type = Type(Type::Kind::I64);
        p.id = 0;
        entry.params.push_back(std::move(p));
    }
    entry.instructions.push_back(makeBinary(Opcode::Sub, 1, Value::constInt(10), Value::temp(0)));

    Instr ret;
    ret.op = Opcode::Ret;
    ret.operands.push_back(Value::temp(1));
    entry.instructions.push_back(std::move(ret));
    entry.terminated = true;

    fn.blocks.push_back(std::move(entry));
    mod.functions.push_back(std::move(fn));

    il::transform::reassociate(mod);

    const auto &sub = mod.functions[0].blocks[0].instructions[0];
    // Should NOT be swapped — Sub is not commutative
    EXPECT_EQ(sub.operands[0].kind, Value::Kind::ConstInt);
    EXPECT_EQ(sub.operands[1].kind, Value::Kind::Temp);
}

TEST(Reassociate, LeavesAlreadyCanonical) {
    // %0 + 1 — already canonical, should not change
    Module mod;
    Function fn;
    fn.name = "test";
    fn.retType = Type(Type::Kind::I64);

    BasicBlock entry;
    entry.label = "entry";
    {
        Param p;
        p.name = "x";
        p.type = Type(Type::Kind::I64);
        p.id = 0;
        entry.params.push_back(std::move(p));
    }
    entry.instructions.push_back(makeBinary(Opcode::Add, 1, Value::temp(0), Value::constInt(1)));

    Instr ret;
    ret.op = Opcode::Ret;
    ret.operands.push_back(Value::temp(1));
    entry.instructions.push_back(std::move(ret));
    entry.terminated = true;

    fn.blocks.push_back(std::move(entry));
    mod.functions.push_back(std::move(fn));

    il::transform::reassociate(mod);

    const auto &add = mod.functions[0].blocks[0].instructions[0];
    EXPECT_EQ(add.operands[0].kind, Value::Kind::Temp);
    EXPECT_EQ(add.operands[1].kind, Value::Kind::ConstInt);
}

TEST(Reassociate, CanonicalizesAllCommutativeOps) {
    // Test Mul, And, Or, Xor all get canonicalized
    Opcode ops[] = {Opcode::Mul, Opcode::And, Opcode::Or, Opcode::Xor};

    for (auto op : ops) {
        Module mod;
        Function fn;
        fn.name = "test";
        fn.retType = Type(Type::Kind::I64);

        BasicBlock entry;
        entry.label = "entry";
        {
            Param p;
            p.name = "x";
            p.type = Type(Type::Kind::I64);
            p.id = 0;
            entry.params.push_back(std::move(p));
        }
        // const first, temp second — should swap
        entry.instructions.push_back(makeBinary(op, 1, Value::constInt(42), Value::temp(0)));

        Instr ret;
        ret.op = Opcode::Ret;
        ret.operands.push_back(Value::temp(1));
        entry.instructions.push_back(std::move(ret));
        entry.terminated = true;

        fn.blocks.push_back(std::move(entry));
        mod.functions.push_back(std::move(fn));

        il::transform::reassociate(mod);

        const auto &instr = mod.functions[0].blocks[0].instructions[0];
        EXPECT_EQ(instr.operands[0].kind, Value::Kind::Temp);
        EXPECT_EQ(instr.operands[1].kind, Value::Kind::ConstInt);
    }
}

TEST(Reassociate, LargeTempIdsRemainHigherRankedThanConstants) {
    Module mod;
    Function fn;
    fn.name = "test";
    fn.retType = Type(Type::Kind::I64);
    BasicBlock entry;
    entry.label = "entry";
    const unsigned largeId = std::numeric_limits<unsigned>::max();
    entry.instructions.push_back(
        makeBinary(Opcode::Add, 0, Value::constInt(1), Value::temp(largeId)));
    fn.blocks.push_back(std::move(entry));
    mod.functions.push_back(std::move(fn));

    il::transform::reassociate(mod);

    const auto &add = mod.functions.front().blocks.front().instructions.front();
    ASSERT_EQ(add.operands.front().kind, Value::Kind::Temp);
    EXPECT_EQ(add.operands.front().id, largeId);
}

TEST(Reassociate, EquivalentTreesReceiveTheSameAssociation) {
    Module mod;
    Function fn;
    fn.name = "test";
    fn.retType = Type(Type::Kind::I64);
    fn.params = {{"a", Type(Type::Kind::I64), 0},
                 {"b", Type(Type::Kind::I64), 1},
                 {"c", Type(Type::Kind::I64), 2}};
    BasicBlock entry;
    entry.label = "entry";
    entry.instructions.push_back(makeBinary(Opcode::Add, 3, Value::temp(0), Value::temp(1)));
    entry.instructions.push_back(makeBinary(Opcode::Add, 4, Value::temp(3), Value::temp(2)));
    entry.instructions.push_back(makeBinary(Opcode::Add, 5, Value::temp(1), Value::temp(2)));
    entry.instructions.push_back(makeBinary(Opcode::Add, 6, Value::temp(0), Value::temp(5)));
    Instr ret;
    ret.op = Opcode::Ret;
    ret.operands = {Value::temp(6)};
    entry.instructions.push_back(std::move(ret));
    entry.terminated = true;
    fn.blocks.push_back(std::move(entry));
    mod.functions.push_back(std::move(fn));

    il::transform::reassociate(mod);
    EXPECT_TRUE(il::transform::runEarlyCSE(mod, mod.functions.front()));

    unsigned addCount = 0;
    for (const auto &instr : mod.functions.front().blocks.front().instructions)
        addCount += instr.op == Opcode::Add;
    EXPECT_EQ(addCount, 2u);
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, argv);
    return zanna_test::run_all_tests();
}
