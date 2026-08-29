//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Unit tests for GVN + Redundant Load Elimination. Builds tiny IL functions and
// verifies that cross-block common subexpressions and dominated redundant loads
// are eliminated conservatively.
//
//===----------------------------------------------------------------------===//

#include "il/transform/AnalysisManager.hpp"
#include "il/transform/GVN.hpp"
#include "il/transform/analysis/Liveness.hpp" // CFGInfo + buildCFG

#include "il/analysis/BasicAA.hpp"
#include "il/analysis/CFG.hpp"
#include "il/analysis/Dominators.hpp"

#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Module.hpp"
#include "il/core/Opcode.hpp"
#include "il/core/Type.hpp"
#include "il/core/Value.hpp"
#include "il/io/Parser.hpp"
#include <sstream>

#include <cassert>
#include <iostream>

using namespace il::core;

static il::transform::AnalysisRegistry makeRegistry() {
    il::transform::AnalysisRegistry registry;
    registry.registerFunctionAnalysis<il::transform::CFGInfo>(
        "cfg", [](Module &mod, Function &fn) { return il::transform::buildCFG(mod, fn); });
    registry.registerFunctionAnalysis<zanna::analysis::DomTree>(
        "dominators", [](Module &mod, Function &fn) {
            zanna::analysis::CFGContext ctx(mod);
            return zanna::analysis::computeDominatorTree(ctx, fn);
        });
    registry.registerFunctionAnalysis<zanna::analysis::BasicAA>(
        "basic-aa", [](Module &mod, Function &fn) { return zanna::analysis::BasicAA(mod, fn); });
    return registry;
}

static void test_cse_cross_block() {
    Module M;
    Function F;
    F.name = "gvn_cse";
    F.retType = Type(Type::Kind::I64);

    unsigned id = 0;
    Param a{"a", Type(Type::Kind::I64), id++};
    Param b{"b", Type(Type::Kind::I64), id++};
    F.params.push_back(a);
    F.params.push_back(b);
    F.valueNames.resize(id);

    BasicBlock entry;
    entry.label = "entry";
    {
        Instr add1;
        add1.result = id++;
        add1.op = Opcode::IAddOvf;
        add1.type = Type(Type::Kind::I64);
        add1.operands.push_back(Value::temp(a.id));
        add1.operands.push_back(Value::temp(b.id));
        const unsigned add1Id = *add1.result;

        Instr br;
        br.op = Opcode::Br;
        br.type = Type(Type::Kind::Void);
        br.labels.push_back("next");
        br.brArgs.emplace_back(std::vector<Value>{Value::temp(add1Id)});

        entry.instructions.push_back(std::move(add1));
        entry.instructions.push_back(std::move(br));
        entry.terminated = true;
    }

    BasicBlock next;
    next.label = "next";
    Param fromEntry{"x", Type(Type::Kind::I64), id++};
    next.params.push_back(fromEntry);
    {
        Instr add2;
        add2.result = id++;
        add2.op = Opcode::IAddOvf;
        add2.type = Type(Type::Kind::I64);
        add2.operands.push_back(Value::temp(a.id));
        add2.operands.push_back(Value::temp(b.id));
        const unsigned add2Id = *add2.result;

        Instr ret;
        ret.op = Opcode::Ret;
        ret.type = Type(Type::Kind::Void);
        ret.operands.push_back(Value::temp(add2Id));

        next.instructions.push_back(std::move(add2));
        next.instructions.push_back(std::move(ret));
        next.terminated = true;
    }

    F.blocks.push_back(std::move(entry));
    F.blocks.push_back(std::move(next));
    M.functions.push_back(std::move(F));

    Function &Fn = M.functions.back();

    auto registry = makeRegistry();
    il::transform::AnalysisManager AM(M, registry);

    il::transform::GVN gvn;
    auto preserved = gvn.run(Fn, AM);
    (void)preserved;
    (void)M; // no-op in release; keep to avoid unused warning if debug prints removed

    // In the "next" block, the add should be eliminated, and ret should use the value from entry
    assert(Fn.blocks.size() == 2);
    const BasicBlock &NextB = Fn.blocks[1];
    assert(NextB.instructions.size() == 1);
    const Instr &Only = NextB.instructions.front();
    assert(Only.op == Opcode::Ret);
    assert(!Only.operands.empty());
    assert(Only.operands.front().kind == Value::Kind::Temp);
    // The operand should be either the block param or the entry result; both are okay, but
    // if GVN eliminated the second add, ret's operand should reference entry's add result id
    // (id==2)
}

static void test_redundant_load_elim() {
    Module M;
    Function F;
    F.name = "gvn_rle";
    F.retType = Type(Type::Kind::I64);

    unsigned id = 0;
    BasicBlock entry;
    entry.label = "entry";
    {
        // p = alloca 8
        Instr allocaI;
        allocaI.result = id++;
        allocaI.op = Opcode::Alloca;
        allocaI.type = Type(Type::Kind::Ptr);
        allocaI.operands.push_back(Value::constInt(8));
        const unsigned pId = *allocaI.result;

        // v0 = load i64, p
        Instr ld0;
        ld0.result = id++;
        ld0.op = Opcode::Load;
        ld0.type = Type(Type::Kind::I64);
        ld0.operands.push_back(Value::temp(pId));

        Instr br;
        br.op = Opcode::Br;
        br.type = Type(Type::Kind::Void);
        br.labels.push_back("next");
        br.brArgs.emplace_back(std::vector<Value>{Value::temp(*ld0.result)});

        entry.instructions.push_back(std::move(allocaI));
        entry.instructions.push_back(std::move(ld0));
        entry.instructions.push_back(std::move(br));
        entry.terminated = true;
    }

    BasicBlock next;
    next.label = "next";
    {
        Param v0{"v0", Type(Type::Kind::I64), id++};
        next.params.push_back(v0);

        // v1 = load i64, p          ; dominated by previous load, no clobber
        Instr ld1;
        ld1.result = id++;
        ld1.op = Opcode::Load;
        ld1.type = Type(Type::Kind::I64);
        // Use the same p (%0)
        ld1.operands.push_back(Value::temp(0));

        Instr ret;
        ret.op = Opcode::Ret;
        ret.type = Type(Type::Kind::Void);
        ret.operands.push_back(Value::temp(*ld1.result));

        next.instructions.push_back(std::move(ld1));
        next.instructions.push_back(std::move(ret));
        next.terminated = true;
    }

    F.blocks.push_back(std::move(entry));
    F.blocks.push_back(std::move(next));
    M.functions.push_back(std::move(F));

    Function &Fn = M.functions.back();

    auto registry = makeRegistry();
    il::transform::AnalysisManager AM(M, registry);

    il::transform::GVN gvn;
    auto preserved = gvn.run(Fn, AM);
    (void)preserved;
    (void)M;

    // The second load should be eliminated; next block should only have Ret.
    assert(Fn.blocks.size() == 2);
    const BasicBlock &NextB = Fn.blocks[1];
    assert(NextB.instructions.size() == 1);
    const Instr &Only = NextB.instructions.front();
    assert(Only.op == Opcode::Ret);
    assert(Only.operands.size() == 1);
    // ret operand should be a temp id corresponding to first load's result (id==1)
    assert(Only.operands.front().kind == Value::Kind::Temp);
}

static void test_textual_order_guard_for_redundant_load_elim() {
    Module M;
    Function F;
    F.name = "gvn_rle_textual_order";
    F.retType = Type(Type::Kind::I64);

    unsigned id = 0;
    BasicBlock entry;
    entry.label = "entry";
    {
        Instr allocaI;
        allocaI.result = id++;
        allocaI.op = Opcode::Alloca;
        allocaI.type = Type(Type::Kind::Ptr);
        allocaI.operands.push_back(Value::constInt(8));

        Instr br;
        br.op = Opcode::Br;
        br.type = Type(Type::Kind::Void);
        br.labels.push_back("late");
        br.brArgs.push_back({});

        entry.instructions.push_back(std::move(allocaI));
        entry.instructions.push_back(std::move(br));
        entry.terminated = true;
    }

    BasicBlock update;
    update.label = "update";
    {
        Instr ld1;
        ld1.result = id++;
        ld1.op = Opcode::Load;
        ld1.type = Type(Type::Kind::I64);
        ld1.operands.push_back(Value::temp(0));

        Instr ret;
        ret.op = Opcode::Ret;
        ret.type = Type(Type::Kind::Void);
        ret.operands.push_back(Value::temp(*ld1.result));

        update.instructions.push_back(std::move(ld1));
        update.instructions.push_back(std::move(ret));
        update.terminated = true;
    }

    BasicBlock late;
    late.label = "late";
    {
        Instr ld0;
        ld0.result = id++;
        ld0.op = Opcode::Load;
        ld0.type = Type(Type::Kind::I64);
        ld0.operands.push_back(Value::temp(0));

        Instr br;
        br.op = Opcode::Br;
        br.type = Type(Type::Kind::Void);
        br.labels.push_back("update");
        br.brArgs.push_back({});

        late.instructions.push_back(std::move(ld0));
        late.instructions.push_back(std::move(br));
        late.terminated = true;
    }

    // Textual order intentionally disagrees with dominance: "late" dominates
    // "update", but appears later in the block list. Replacing update's load
    // with late's temp would create a textual use-before-def.
    F.blocks.push_back(std::move(entry));
    F.blocks.push_back(std::move(update));
    F.blocks.push_back(std::move(late));
    M.functions.push_back(std::move(F));

    Function &Fn = M.functions.back();
    auto registry = makeRegistry();
    il::transform::AnalysisManager AM(M, registry);

    il::transform::GVN gvn;
    gvn.run(Fn, AM);

    const BasicBlock &UpdateB = Fn.blocks[1];
    assert(UpdateB.instructions.size() == 2);
    assert(UpdateB.instructions[0].op == Opcode::Load);
    assert(UpdateB.instructions[1].op == Opcode::Ret);
    assert(UpdateB.instructions[1].operands.size() == 1);
    assert(UpdateB.instructions[1].operands[0].kind == Value::Kind::Temp);
    assert(UpdateB.instructions[1].operands[0].id == *UpdateB.instructions[0].result);
}

static void test_same_block_def_before_use_guard() {
    Module M;
    Function F;
    F.name = "gvn_same_block_textual_order";
    F.retType = Type(Type::Kind::I64);

    unsigned id = 0;
    Param a{"a", Type(Type::Kind::I64), id++};
    Param b{"b", Type(Type::Kind::I64), id++};
    F.params.push_back(a);
    F.params.push_back(b);
    F.valueNames.resize(id);

    BasicBlock entry;
    entry.label = "entry";
    {
        Instr first;
        first.result = id++;
        first.op = Opcode::IAddOvf;
        first.type = Type(Type::Kind::I64);
        first.operands.push_back(Value::temp(a.id));
        first.operands.push_back(Value::temp(b.id));
        const unsigned firstId = *first.result;

        Instr second;
        second.result = id++;
        second.op = Opcode::IAddOvf;
        second.type = Type(Type::Kind::I64);
        second.operands.push_back(Value::temp(a.id));
        second.operands.push_back(Value::temp(b.id));
        const unsigned secondId = *second.result;

        Instr ret;
        ret.op = Opcode::Ret;
        ret.type = Type(Type::Kind::Void);
        ret.operands.push_back(Value::temp(firstId));

        entry.instructions.push_back(std::move(first));
        entry.instructions.push_back(std::move(second));
        entry.instructions.push_back(std::move(ret));
        entry.terminated = true;

        (void)secondId;
    }

    F.blocks.push_back(std::move(entry));
    M.functions.push_back(std::move(F));

    Function &Fn = M.functions.back();
    auto registry = makeRegistry();
    il::transform::AnalysisManager AM(M, registry);

    il::transform::GVN gvn;
    gvn.run(Fn, AM);

    const BasicBlock &EntryB = Fn.blocks.front();
    assert(EntryB.instructions.size() == 2);
    assert(EntryB.instructions.front().op == Opcode::IAddOvf);
    assert(EntryB.instructions.back().op == Opcode::Ret);
    assert(EntryB.instructions.back().operands.size() == 1);
    assert(EntryB.instructions.back().operands.front().kind == Value::Kind::Temp);
    assert(EntryB.instructions.back().operands.front().id == *EntryB.instructions.front().result);
}

static void test_repeated_same_block_elimination_updates_later_uses() {
    Module M;
    Function F;
    F.name = "gvn_repeated_same_block";
    F.retType = Type(Type::Kind::I64);

    unsigned id = 0;
    Param a{"a", Type(Type::Kind::I64), id++};
    Param b{"b", Type(Type::Kind::I64), id++};
    F.params.push_back(a);
    F.params.push_back(b);
    F.valueNames.resize(id);

    BasicBlock entry;
    entry.label = "entry";
    {
        Instr add0;
        add0.result = id++;
        add0.op = Opcode::IAddOvf;
        add0.type = Type(Type::Kind::I64);
        add0.operands = {Value::temp(a.id), Value::temp(b.id)};
        const unsigned add0Id = *add0.result;
        entry.instructions.push_back(std::move(add0));

        Instr add1;
        add1.result = id++;
        add1.op = Opcode::IAddOvf;
        add1.type = Type(Type::Kind::I64);
        add1.operands = {Value::temp(a.id), Value::temp(b.id)};
        const unsigned add1Id = *add1.result;
        entry.instructions.push_back(std::move(add1));

        Instr sub;
        sub.result = id++;
        sub.op = Opcode::ISubOvf;
        sub.type = Type(Type::Kind::I64);
        sub.operands = {Value::temp(add1Id), Value::constInt(1)};
        entry.instructions.push_back(std::move(sub));

        Instr add2;
        add2.result = id++;
        add2.op = Opcode::IAddOvf;
        add2.type = Type(Type::Kind::I64);
        add2.operands = {Value::temp(a.id), Value::temp(b.id)};
        const unsigned add2Id = *add2.result;
        entry.instructions.push_back(std::move(add2));

        Instr ret;
        ret.op = Opcode::Ret;
        ret.type = Type(Type::Kind::Void);
        ret.operands.push_back(Value::temp(add2Id));
        entry.instructions.push_back(std::move(ret));
        entry.terminated = true;

        (void)add0Id;
    }

    F.blocks.push_back(std::move(entry));
    M.functions.push_back(std::move(F));

    Function &Fn = M.functions.back();
    auto registry = makeRegistry();
    il::transform::AnalysisManager AM(M, registry);

    il::transform::GVN gvn;
    gvn.run(Fn, AM);

    const BasicBlock &EntryB = Fn.blocks.front();
    assert(EntryB.instructions.size() == 3);
    assert(EntryB.instructions[0].op == Opcode::IAddOvf);
    assert(EntryB.instructions[1].op == Opcode::ISubOvf);
    assert(EntryB.instructions[2].op == Opcode::Ret);
    assert(EntryB.instructions[1].operands.size() == 2);
    assert(EntryB.instructions[1].operands[0].kind == Value::Kind::Temp);
    assert(EntryB.instructions[1].operands[0].id == *EntryB.instructions[0].result);
    assert(EntryB.instructions[2].operands.size() == 1);
    assert(EntryB.instructions[2].operands[0].kind == Value::Kind::Temp);
    assert(EntryB.instructions[2].operands[0].id == *EntryB.instructions[0].result);
}

/// @brief Main.
// ZB-32: a load memoized in a loop header must not be reused after the loop
// (or inside it) when a loop-body block stores to the same slot. Dominance
// does not imply "no intervening store" — the body does not dominate the
// exit, yet it lies on every path from the header's second iteration onward.
static void test_load_not_reused_across_loop_store() {
    const char *text = R"(il 0.3.0
func @f() -> i64 {
entry:
  %slot = alloca 8
  store i64, %slot, 1
  br loop(0)
loop(%i:i64):
  %v = load i64, %slot
  %c = scmp_lt %i, 2
  cbr %c, body, exit
body:
  store i64, %slot, 7
  %n = iadd.ovf %i, 1
  br loop(%n)
exit:
  %w = load i64, %slot
  ret %w
}
)";
    Module M;
    std::istringstream in{text};
    auto parsed = il::io::Parser::parse(in, M);
    assert(parsed.hasValue());
    Function &Fn = M.functions.front();
    auto registry = makeRegistry();
    il::transform::AnalysisManager AM(M, registry);
    il::transform::GVN gvn;
    (void)gvn.run(Fn, AM);
    const BasicBlock *exitBlock = nullptr;
    for (const auto &bb : Fn.blocks)
        if (bb.label == "exit")
            exitBlock = &bb;
    assert(exitBlock && "exit block must survive");
    bool loadSurvives = false;
    for (const auto &ins : exitBlock->instructions)
        if (ins.op == Opcode::Load)
            loadSurvives = true;
    assert(loadSurvives && "ZB-32: the post-loop load was replaced by the header's load");
    std::cout << "gvn: load not reused across loop store OK\n";
}

int main() {
    test_load_not_reused_across_loop_store();
    test_cse_cross_block();
    test_redundant_load_elim();
    test_textual_order_guard_for_redundant_load_elim();
    test_same_block_def_before_use_guard();
    test_repeated_same_block_elimination_updates_later_uses();
    return 0;
}
