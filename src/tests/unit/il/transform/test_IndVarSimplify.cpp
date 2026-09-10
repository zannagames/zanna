//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Unit test for IndVarSimplify: exercises the strength-reduction preconditions
// and rewrite. A symbolic base must be skipped (its overflow behaviour cannot
// be proven), a loop-local base must be skipped, and a provably bounded loop
// whose latch has two predecessors must have the carried address argument
// appended on every edge into the latch while staying verifier-clean. A loop
// whose address feeds plain integer arithmetic must also be skipped, because the
// loop-carried parameter that would replace the address cannot be bounded and
// the arithmetic verifies only on that range -- whether the consumer reads the
// address directly or receives it through a block parameter.
//
//===----------------------------------------------------------------------===//

#include "il/transform/AnalysisManager.hpp"
#include "il/transform/IndVarSimplify.hpp"
#include "il/transform/LoopSimplify.hpp"
#include "il/transform/analysis/LoopInfo.hpp"

#include "il/analysis/CFG.hpp"
#include "il/analysis/Dominators.hpp"
#include "il/transform/analysis/Liveness.hpp" // for CFGInfo + buildCFG

#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Module.hpp"
#include "il/core/Opcode.hpp"
#include "il/core/Type.hpp"
#include "il/core/Value.hpp"

#include "il/verify/Verifier.hpp"

#include <cassert>

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
    registry.registerFunctionAnalysis<il::transform::LoopInfo>(
        "loop-info",
        [](Module &mod, Function &fn) { return il::transform::computeLoopInfo(mod, fn); });
    return registry;
}

/// @brief Main.
int main() {
    Module M;
    Function F;
    F.name = "indvars_simple";
    F.retType = Type(Type::Kind::I64);

    unsigned id = 0;
    // Function params: N bound, B base
    Param N{"N", Type(Type::Kind::I64), id++};
    Param Bp{"B", Type(Type::Kind::I64), id++};
    F.params.push_back(N);
    F.params.push_back(Bp);
    F.valueNames.resize(id);

    // entry: branch to loop.preheader
    BasicBlock entry;
    entry.label = "entry";
    {
        Instr br;
        br.op = Opcode::Br;
        br.type = Type(Type::Kind::Void);
        br.labels.push_back("loop.preheader");
        br.brArgs.emplace_back(std::vector<Value>{});
        entry.instructions.push_back(std::move(br));
        entry.terminated = true;
    }

    // preheader: jump to loop with i0 = 0
    BasicBlock preheader;
    preheader.label = "loop.preheader";
    {
        Instr br;
        br.op = Opcode::Br;
        br.type = Type(Type::Kind::Void);
        br.labels.push_back("loop");
        // i0 = 0 (header param later)
        br.brArgs.emplace_back(std::vector<Value>{Value::constInt(0)});
        preheader.instructions.push_back(std::move(br));
        preheader.terminated = true;
    }

    // loop header with param i
    BasicBlock loop;
    loop.label = "loop";
    Param i{"i", Type(Type::Kind::I64), id++};
    loop.params.push_back(i);

    // addr = B + i*8
    Instr mul;
    mul.result = id++;
    mul.op = Opcode::Mul;
    mul.type = Type(Type::Kind::I64);
    mul.operands.push_back(Value::temp(i.id));
    mul.operands.push_back(Value::constInt(8));

    Instr add;
    add.result = id++;
    add.op = Opcode::Add;
    add.type = Type(Type::Kind::I64);
    add.operands.push_back(Value::temp(Bp.id));
    add.operands.push_back(Value::temp(*mul.result));

    // compare i < N (not used to steer CFG here; keep simple)
    Instr ret;
    ret.op = Opcode::Ret;
    ret.type = Type(Type::Kind::Void);
    ret.operands.push_back(Value::temp(*add.result));

    // branch to latch carrying i as param
    Instr brToLatch;
    brToLatch.op = Opcode::Br;
    brToLatch.type = Type(Type::Kind::Void);
    brToLatch.labels.push_back("latch");
    brToLatch.brArgs.emplace_back(std::vector<Value>{Value::temp(i.id)});

    loop.instructions.push_back(std::move(mul));
    loop.instructions.push_back(std::move(add));
    loop.instructions.push_back(std::move(brToLatch));
    loop.terminated = true;

    // latch(i_l): i_next = i_l + 1; br loop(i_next)
    BasicBlock latch;
    latch.label = "latch";
    Param i_l{"i.l", Type(Type::Kind::I64), id++};
    latch.params.push_back(i_l);

    Instr inc;
    inc.result = id++;
    inc.op = Opcode::Add;
    inc.type = Type(Type::Kind::I64);
    inc.operands.push_back(Value::temp(i_l.id));
    inc.operands.push_back(Value::constInt(1));

    Instr back;
    back.op = Opcode::Br;
    back.type = Type(Type::Kind::Void);
    back.labels.push_back("loop");
    back.brArgs.emplace_back(std::vector<Value>{Value::temp(*inc.result)});

    latch.instructions.push_back(std::move(inc));
    latch.instructions.push_back(std::move(back));
    latch.terminated = true;

    F.blocks.push_back(std::move(entry));
    F.blocks.push_back(std::move(preheader));
    F.blocks.push_back(std::move(loop));
    F.blocks.push_back(std::move(latch));
    M.functions.push_back(std::move(F));

    Function &Fn = M.functions.back();

    // Run LoopSimplify to ensure preheader naming and structure if needed
    il::transform::AnalysisRegistry registry = makeRegistry();
    il::transform::AnalysisManager AM(M, registry);

    // Compute loop-info once to seed
    (void)AM.getFunctionResult<il::transform::LoopInfo>("loop-info", Fn);

    il::transform::IndVarSimplify pass;
    auto preserved = pass.run(Fn, AM);
    (void)preserved;

    // The base B is a raw function parameter with no provable bound, so the
    // rewrite cannot show its address values stay overflow-free; the loop
    // must be left untouched.
    BasicBlock *H = nullptr;
    for (auto &B : Fn.blocks)
        if (B.label == "loop")
            H = &B;
    assert(H);
    assert(H->params.size() == 1 && "symbolic base must not be strength-reduced");

    Module M2;
    Function F2;
    F2.name = "indvars_loop_local_base";
    F2.retType = Type(Type::Kind::I64);
    id = 0;
    Param B2{"B", Type(Type::Kind::I64), id++};
    F2.params.push_back(B2);

    BasicBlock entry2;
    entry2.label = "entry";
    {
        Instr br;
        br.op = Opcode::Br;
        br.type = Type(Type::Kind::Void);
        br.labels.push_back("loop.preheader");
        br.brArgs.emplace_back(std::vector<Value>{});
        entry2.instructions.push_back(std::move(br));
        entry2.terminated = true;
    }

    BasicBlock preheader2;
    preheader2.label = "loop.preheader";
    {
        Instr br;
        br.op = Opcode::Br;
        br.type = Type(Type::Kind::Void);
        br.labels.push_back("loop");
        br.brArgs.emplace_back(std::vector<Value>{Value::constInt(0)});
        preheader2.instructions.push_back(std::move(br));
        preheader2.terminated = true;
    }

    BasicBlock loop2;
    loop2.label = "loop";
    Param i2{"i", Type(Type::Kind::I64), id++};
    loop2.params.push_back(i2);

    Instr localBase;
    localBase.result = id++;
    localBase.op = Opcode::Add;
    localBase.type = Type(Type::Kind::I64);
    localBase.operands = {Value::temp(B2.id), Value::temp(i2.id)};
    const unsigned localBaseId = *localBase.result;

    Instr mul2;
    mul2.result = id++;
    mul2.op = Opcode::Mul;
    mul2.type = Type(Type::Kind::I64);
    mul2.operands = {Value::temp(i2.id), Value::constInt(8)};
    const unsigned mul2Id = *mul2.result;

    Instr addr2;
    addr2.result = id++;
    addr2.op = Opcode::Add;
    addr2.type = Type(Type::Kind::I64);
    addr2.operands = {Value::temp(localBaseId), Value::temp(mul2Id)};

    Instr toLatch2;
    toLatch2.op = Opcode::Br;
    toLatch2.type = Type(Type::Kind::Void);
    toLatch2.labels.push_back("latch");
    toLatch2.brArgs.emplace_back(std::vector<Value>{Value::temp(i2.id)});

    loop2.instructions.push_back(std::move(localBase));
    loop2.instructions.push_back(std::move(mul2));
    loop2.instructions.push_back(std::move(addr2));
    loop2.instructions.push_back(std::move(toLatch2));
    loop2.terminated = true;

    BasicBlock latch2;
    latch2.label = "latch";
    Param il2{"i.l", Type(Type::Kind::I64), id++};
    latch2.params.push_back(il2);
    Instr inc2;
    inc2.result = id++;
    inc2.op = Opcode::Add;
    inc2.type = Type(Type::Kind::I64);
    inc2.operands = {Value::temp(il2.id), Value::constInt(1)};
    Instr back2;
    back2.op = Opcode::Br;
    back2.type = Type(Type::Kind::Void);
    back2.labels.push_back("loop");
    back2.brArgs.emplace_back(std::vector<Value>{Value::temp(*inc2.result)});
    latch2.instructions.push_back(std::move(inc2));
    latch2.instructions.push_back(std::move(back2));
    latch2.terminated = true;

    F2.blocks.push_back(std::move(entry2));
    F2.blocks.push_back(std::move(preheader2));
    F2.blocks.push_back(std::move(loop2));
    F2.blocks.push_back(std::move(latch2));
    M2.functions.push_back(std::move(F2));

    Function &Fn2 = M2.functions.back();
    il::transform::AnalysisRegistry registry2 = makeRegistry();
    il::transform::AnalysisManager AM2(M2, registry2);
    (void)AM2.getFunctionResult<il::transform::LoopInfo>("loop-info", Fn2);
    auto preserved2 = pass.run(Fn2, AM2);
    (void)preserved2;

    BasicBlock *H2 = nullptr;
    for (auto &B : Fn2.blocks)
        if (B.label == "loop")
            H2 = &B;
    assert(H2);
    assert(H2->params.size() == 1 && "loop-local bases must not leave partial params behind");

    // Regression: a rotated loop whose latch has two predecessors — the
    // header's conditional edge and a body join (a wrap adjustment). The
    // rewrite must append the carried address argument on BOTH edges into the
    // latch and the result must satisfy the strict verifier.
    Module M3;
    Function F3;
    F3.name = "indvars_two_pred_latch";
    F3.retType = Type(Type::Kind::I64);
    id = 0;

    BasicBlock entry3;
    entry3.label = "entry";
    {
        Instr br;
        br.op = Opcode::Br;
        br.type = Type(Type::Kind::Void);
        br.labels.push_back("loop.preheader");
        br.brArgs.emplace_back(std::vector<Value>{});
        entry3.instructions.push_back(std::move(br));
        entry3.terminated = true;
    }

    BasicBlock preheader3;
    preheader3.label = "loop.preheader";
    {
        Instr br;
        br.op = Opcode::Br;
        br.type = Type(Type::Kind::Void);
        br.labels.push_back("loop");
        br.brArgs.emplace_back(std::vector<Value>{Value::constInt(0)});
        preheader3.instructions.push_back(std::move(br));
        preheader3.terminated = true;
    }

    // loop(i): addr = 83 + i*251; wrap when addr < 200, then rejoin the latch.
    BasicBlock loop3;
    loop3.label = "loop";
    Param i3{"i", Type(Type::Kind::I64), id++};
    loop3.params.push_back(i3);

    Instr mul3;
    mul3.result = id++;
    mul3.op = Opcode::Mul;
    mul3.type = Type(Type::Kind::I64);
    mul3.operands = {Value::temp(i3.id), Value::constInt(251)};
    const unsigned mul3Id = *mul3.result;

    Instr addr3;
    addr3.result = id++;
    addr3.op = Opcode::Add;
    addr3.type = Type(Type::Kind::I64);
    addr3.operands = {Value::constInt(83), Value::temp(mul3Id)};
    const unsigned addr3Id = *addr3.result;

    Instr wrapCmp;
    wrapCmp.result = id++;
    wrapCmp.op = Opcode::SCmpLT;
    wrapCmp.type = Type(Type::Kind::I1);
    wrapCmp.operands = {Value::temp(addr3Id), Value::constInt(200)};

    Instr headerCbr;
    headerCbr.op = Opcode::CBr;
    headerCbr.type = Type(Type::Kind::Void);
    headerCbr.operands.push_back(Value::temp(*wrapCmp.result));
    headerCbr.labels = {"wrap", "latch"};
    headerCbr.brArgs = {{}, {Value::temp(i3.id)}};

    loop3.instructions.push_back(std::move(mul3));
    loop3.instructions.push_back(std::move(addr3));
    loop3.instructions.push_back(std::move(wrapCmp));
    loop3.instructions.push_back(std::move(headerCbr));
    loop3.terminated = true;

    BasicBlock wrap3;
    wrap3.label = "wrap";
    {
        Instr br;
        br.op = Opcode::Br;
        br.type = Type(Type::Kind::Void);
        br.labels.push_back("latch");
        br.brArgs.emplace_back(std::vector<Value>{Value::temp(i3.id)});
        wrap3.instructions.push_back(std::move(br));
        wrap3.terminated = true;
    }

    BasicBlock latch3;
    latch3.label = "latch";
    Param il3{"i.l", Type(Type::Kind::I64), id++};
    latch3.params.push_back(il3);

    Instr inc3;
    inc3.result = id++;
    inc3.op = Opcode::Add;
    inc3.type = Type(Type::Kind::I64);
    inc3.operands = {Value::temp(il3.id), Value::constInt(1)};
    const unsigned inc3Id = *inc3.result;

    // Trip count 3: small enough for the range fixpoint to converge before
    // widening even through the two-edge join, so the counter is provably
    // bounded and the gate lets the rewrite fire (mirrors the xenoscape
    // near-cloud loop this regression was reduced from).
    Instr guard3;
    guard3.result = id++;
    guard3.op = Opcode::SCmpLT;
    guard3.type = Type(Type::Kind::I1);
    guard3.operands = {Value::temp(inc3Id), Value::constInt(3)};

    Instr latchCbr;
    latchCbr.op = Opcode::CBr;
    latchCbr.type = Type(Type::Kind::Void);
    latchCbr.operands.push_back(Value::temp(*guard3.result));
    latchCbr.labels = {"loop", "exit"};
    latchCbr.brArgs = {{Value::temp(inc3Id)}, {}};

    latch3.instructions.push_back(std::move(inc3));
    latch3.instructions.push_back(std::move(guard3));
    latch3.instructions.push_back(std::move(latchCbr));
    latch3.terminated = true;

    BasicBlock exit3;
    exit3.label = "exit";
    {
        Instr ret3;
        ret3.op = Opcode::Ret;
        ret3.type = Type(Type::Kind::Void);
        ret3.operands.push_back(Value::constInt(0));
        exit3.instructions.push_back(std::move(ret3));
        exit3.terminated = true;
    }

    F3.blocks.push_back(std::move(entry3));
    F3.blocks.push_back(std::move(preheader3));
    F3.blocks.push_back(std::move(loop3));
    F3.blocks.push_back(std::move(wrap3));
    F3.blocks.push_back(std::move(latch3));
    F3.blocks.push_back(std::move(exit3));
    F3.valueNames.resize(id);
    M3.functions.push_back(std::move(F3));

    Function &Fn3 = M3.functions.back();
    il::transform::AnalysisRegistry registry3 = makeRegistry();
    il::transform::AnalysisManager AM3(M3, registry3);
    (void)AM3.getFunctionResult<il::transform::LoopInfo>("loop-info", Fn3);
    auto preserved3 = pass.run(Fn3, AM3);
    (void)preserved3;

    BasicBlock *H3 = nullptr;
    BasicBlock *L3 = nullptr;
    BasicBlock *W3 = nullptr;
    for (auto &B : Fn3.blocks) {
        if (B.label == "loop")
            H3 = &B;
        else if (B.label == "latch")
            L3 = &B;
        else if (B.label == "wrap")
            W3 = &B;
    }
    assert(H3 && L3 && W3);
    assert(H3->params.size() == 2 && "bounded const-base loop must strength-reduce");
    assert(L3->params.size() == 2 && "latch must carry the address");

    // Every edge into the latch supplies the appended address argument.
    const Instr &headerTerm = H3->instructions.back();
    for (size_t t = 0; t < headerTerm.labels.size(); ++t)
        if (headerTerm.labels[t] == "latch")
            assert(headerTerm.brArgs[t].size() == L3->params.size());
    const Instr &wrapTerm = W3->instructions.back();
    assert(wrapTerm.labels.size() == 1 && wrapTerm.labels[0] == "latch");
    assert(wrapTerm.brArgs[0].size() == L3->params.size() &&
           "body-join edge into the latch must carry the address argument");

    // The carried increment is checked arithmetic and the header mul is gone.
    bool sawCheckedInc = false;
    for (const Instr &I : L3->instructions)
        if (I.op == Opcode::IAddOvf)
            sawCheckedInc = true;
    assert(sawCheckedInc && "address increment must be emitted as iadd.ovf");
    for (const Instr &I : H3->instructions)
        assert(I.op != Opcode::Mul && "header address mul must be strength-reduced away");

    // The rewritten module must satisfy the strict verifier.
    auto verified3 = il::verify::Verifier::verify(M3);
    assert(static_cast<bool>(verified3) && "transformed module must verify");

    // Regression: the same bounded loop, but the address feeds a plain subtract
    // that the verifier accepts only because the address has a provable range.
    // Strength-reducing the address into a loop-carried parameter destroys that
    // range, so the loop must be left alone rather than stranding an
    // unverifiable `sub` (the xenoscape parallax shape).
    Module M4;
    Function F4;
    F4.name = "indvars_addr_feeds_plain_sub";
    F4.retType = Type(Type::Kind::I64);
    id = 0;

    BasicBlock entry4;
    entry4.label = "entry";
    {
        Instr br;
        br.op = Opcode::Br;
        br.type = Type(Type::Kind::Void);
        br.labels.push_back("loop.preheader");
        br.brArgs.emplace_back(std::vector<Value>{});
        entry4.instructions.push_back(std::move(br));
        entry4.terminated = true;
    }

    BasicBlock preheader4;
    preheader4.label = "loop.preheader";
    {
        Instr br;
        br.op = Opcode::Br;
        br.type = Type(Type::Kind::Void);
        br.labels.push_back("loop");
        br.brArgs.emplace_back(std::vector<Value>{Value::constInt(0)});
        preheader4.instructions.push_back(std::move(br));
        preheader4.terminated = true;
    }

    BasicBlock loop4;
    loop4.label = "loop";
    Param i4{"i", Type(Type::Kind::I64), id++};
    loop4.params.push_back(i4);

    Instr mul4;
    mul4.result = id++;
    mul4.op = Opcode::Mul;
    mul4.type = Type(Type::Kind::I64);
    mul4.operands = {Value::temp(i4.id), Value::constInt(251)};

    Instr addr4;
    addr4.result = id++;
    addr4.op = Opcode::Add;
    addr4.type = Type(Type::Kind::I64);
    addr4.operands = {Value::constInt(83), Value::temp(*mul4.result)};
    const unsigned addr4Id = *addr4.result;

    Instr delta4;
    delta4.result = id++;
    delta4.op = Opcode::Sub;
    delta4.type = Type(Type::Kind::I64);
    delta4.operands = {Value::temp(addr4Id), Value::constInt(4)};

    Instr cmp4;
    cmp4.result = id++;
    cmp4.op = Opcode::SCmpLT;
    cmp4.type = Type(Type::Kind::I1);
    cmp4.operands = {Value::temp(*delta4.result), Value::constInt(200)};

    Instr headerCbr4;
    headerCbr4.op = Opcode::CBr;
    headerCbr4.type = Type(Type::Kind::Void);
    headerCbr4.operands.push_back(Value::temp(*cmp4.result));
    headerCbr4.labels = {"wrap", "latch"};
    headerCbr4.brArgs = {{}, {Value::temp(i4.id)}};

    loop4.instructions.push_back(std::move(mul4));
    loop4.instructions.push_back(std::move(addr4));
    loop4.instructions.push_back(std::move(delta4));
    loop4.instructions.push_back(std::move(cmp4));
    loop4.instructions.push_back(std::move(headerCbr4));
    loop4.terminated = true;

    BasicBlock wrap4;
    wrap4.label = "wrap";
    {
        Instr br;
        br.op = Opcode::Br;
        br.type = Type(Type::Kind::Void);
        br.labels.push_back("latch");
        br.brArgs.emplace_back(std::vector<Value>{Value::temp(i4.id)});
        wrap4.instructions.push_back(std::move(br));
        wrap4.terminated = true;
    }

    BasicBlock latch4;
    latch4.label = "latch";
    Param il4{"i.l", Type(Type::Kind::I64), id++};
    latch4.params.push_back(il4);

    Instr inc4;
    inc4.result = id++;
    inc4.op = Opcode::Add;
    inc4.type = Type(Type::Kind::I64);
    inc4.operands = {Value::temp(il4.id), Value::constInt(1)};
    const unsigned inc4Id = *inc4.result;

    Instr guard4;
    guard4.result = id++;
    guard4.op = Opcode::SCmpLT;
    guard4.type = Type(Type::Kind::I1);
    guard4.operands = {Value::temp(inc4Id), Value::constInt(3)};

    Instr latchCbr4;
    latchCbr4.op = Opcode::CBr;
    latchCbr4.type = Type(Type::Kind::Void);
    latchCbr4.operands.push_back(Value::temp(*guard4.result));
    latchCbr4.labels = {"loop", "exit"};
    latchCbr4.brArgs = {{Value::temp(inc4Id)}, {}};

    latch4.instructions.push_back(std::move(inc4));
    latch4.instructions.push_back(std::move(guard4));
    latch4.instructions.push_back(std::move(latchCbr4));
    latch4.terminated = true;

    BasicBlock exit4;
    exit4.label = "exit";
    {
        Instr ret4;
        ret4.op = Opcode::Ret;
        ret4.type = Type(Type::Kind::Void);
        ret4.operands.push_back(Value::constInt(0));
        exit4.instructions.push_back(std::move(ret4));
        exit4.terminated = true;
    }

    F4.blocks.push_back(std::move(entry4));
    F4.blocks.push_back(std::move(preheader4));
    F4.blocks.push_back(std::move(loop4));
    F4.blocks.push_back(std::move(wrap4));
    F4.blocks.push_back(std::move(latch4));
    F4.blocks.push_back(std::move(exit4));
    F4.valueNames.resize(id);
    M4.functions.push_back(std::move(F4));

    // The demoted input is already verifier-clean: the counter's range bounds
    // the address, which in turn bounds the subtract.
    auto verified4Before = il::verify::Verifier::verify(M4);
    assert(static_cast<bool>(verified4Before) && "demoted input must verify");

    Function &Fn4 = M4.functions.back();
    il::transform::AnalysisRegistry registry4 = makeRegistry();
    il::transform::AnalysisManager AM4(M4, registry4);
    (void)AM4.getFunctionResult<il::transform::LoopInfo>("loop-info", Fn4);
    auto preserved4 = pass.run(Fn4, AM4);
    (void)preserved4;

    BasicBlock *H4 = nullptr;
    for (auto &B : Fn4.blocks)
        if (B.label == "loop")
            H4 = &B;
    assert(H4);
    assert(H4->params.size() == 1 &&
           "an address consumed by plain arithmetic must not be strength-reduced");
    auto verified4 = il::verify::Verifier::verify(M4);
    assert(static_cast<bool>(verified4) && "module must still verify after the pass");

    // Regression: the consuming plain add is not a direct user of the address.
    // The address crosses a branch argument into a join block, so the range loss
    // reaches the add through a block parameter; a direct-users-only guard would
    // let this loop through and strand the add.
    Module M5;
    Function F5;
    F5.name = "indvars_addr_escapes_to_join";
    F5.retType = Type(Type::Kind::I64);
    id = 0;

    BasicBlock entry5;
    entry5.label = "entry";
    {
        Instr br;
        br.op = Opcode::Br;
        br.type = Type(Type::Kind::Void);
        br.labels.push_back("loop.preheader");
        br.brArgs.emplace_back(std::vector<Value>{});
        entry5.instructions.push_back(std::move(br));
        entry5.terminated = true;
    }

    BasicBlock preheader5;
    preheader5.label = "loop.preheader";
    {
        Instr br;
        br.op = Opcode::Br;
        br.type = Type(Type::Kind::Void);
        br.labels.push_back("loop");
        br.brArgs.emplace_back(std::vector<Value>{Value::constInt(0)});
        preheader5.instructions.push_back(std::move(br));
        preheader5.terminated = true;
    }

    BasicBlock loop5;
    loop5.label = "loop";
    Param i5{"i", Type(Type::Kind::I64), id++};
    loop5.params.push_back(i5);

    Instr mul5;
    mul5.result = id++;
    mul5.op = Opcode::Mul;
    mul5.type = Type(Type::Kind::I64);
    mul5.operands = {Value::temp(i5.id), Value::constInt(251)};

    Instr addr5;
    addr5.result = id++;
    addr5.op = Opcode::Add;
    addr5.type = Type(Type::Kind::I64);
    addr5.operands = {Value::constInt(83), Value::temp(*mul5.result)};
    const unsigned addr5Id = *addr5.result;

    Instr cmp5;
    cmp5.result = id++;
    cmp5.op = Opcode::SCmpLT;
    cmp5.type = Type(Type::Kind::I1);
    cmp5.operands = {Value::temp(addr5Id), Value::constInt(200)};

    Instr headerCbr5;
    headerCbr5.op = Opcode::CBr;
    headerCbr5.type = Type(Type::Kind::Void);
    headerCbr5.operands.push_back(Value::temp(*cmp5.result));
    headerCbr5.labels = {"wrap", "latch"};
    headerCbr5.brArgs = {{Value::temp(addr5Id)}, {Value::temp(i5.id)}};

    loop5.instructions.push_back(std::move(mul5));
    loop5.instructions.push_back(std::move(addr5));
    loop5.instructions.push_back(std::move(cmp5));
    loop5.instructions.push_back(std::move(headerCbr5));
    loop5.terminated = true;

    BasicBlock wrap5;
    wrap5.label = "wrap";
    Param carried5{"carried", Type(Type::Kind::I64), id++};
    wrap5.params.push_back(carried5);
    {
        Instr bump;
        bump.result = id++;
        bump.op = Opcode::Add;
        bump.type = Type(Type::Kind::I64);
        bump.operands = {Value::temp(carried5.id), Value::constInt(6)};

        Instr sink;
        sink.op = Opcode::Br;
        sink.type = Type(Type::Kind::Void);
        sink.labels.push_back("latch");
        sink.brArgs.emplace_back(std::vector<Value>{Value::temp(i5.id)});

        wrap5.instructions.push_back(std::move(bump));
        wrap5.instructions.push_back(std::move(sink));
        wrap5.terminated = true;
    }

    BasicBlock latch5;
    latch5.label = "latch";
    Param il5{"i.l", Type(Type::Kind::I64), id++};
    latch5.params.push_back(il5);

    Instr inc5;
    inc5.result = id++;
    inc5.op = Opcode::Add;
    inc5.type = Type(Type::Kind::I64);
    inc5.operands = {Value::temp(il5.id), Value::constInt(1)};
    const unsigned inc5Id = *inc5.result;

    Instr guard5;
    guard5.result = id++;
    guard5.op = Opcode::SCmpLT;
    guard5.type = Type(Type::Kind::I1);
    guard5.operands = {Value::temp(inc5Id), Value::constInt(3)};

    Instr latchCbr5;
    latchCbr5.op = Opcode::CBr;
    latchCbr5.type = Type(Type::Kind::Void);
    latchCbr5.operands.push_back(Value::temp(*guard5.result));
    latchCbr5.labels = {"loop", "exit"};
    latchCbr5.brArgs = {{Value::temp(inc5Id)}, {}};

    latch5.instructions.push_back(std::move(inc5));
    latch5.instructions.push_back(std::move(guard5));
    latch5.instructions.push_back(std::move(latchCbr5));
    latch5.terminated = true;

    BasicBlock exit5;
    exit5.label = "exit";
    {
        Instr ret5;
        ret5.op = Opcode::Ret;
        ret5.type = Type(Type::Kind::Void);
        ret5.operands.push_back(Value::constInt(0));
        exit5.instructions.push_back(std::move(ret5));
        exit5.terminated = true;
    }

    F5.blocks.push_back(std::move(entry5));
    F5.blocks.push_back(std::move(preheader5));
    F5.blocks.push_back(std::move(loop5));
    F5.blocks.push_back(std::move(wrap5));
    F5.blocks.push_back(std::move(latch5));
    F5.blocks.push_back(std::move(exit5));
    F5.valueNames.resize(id);
    M5.functions.push_back(std::move(F5));

    auto verified5Before = il::verify::Verifier::verify(M5);
    assert(static_cast<bool>(verified5Before) && "demoted input must verify");

    Function &Fn5 = M5.functions.back();
    il::transform::AnalysisRegistry registry5 = makeRegistry();
    il::transform::AnalysisManager AM5(M5, registry5);
    (void)AM5.getFunctionResult<il::transform::LoopInfo>("loop-info", Fn5);
    auto preserved5 = pass.run(Fn5, AM5);
    (void)preserved5;

    BasicBlock *H5 = nullptr;
    for (auto &B : Fn5.blocks)
        if (B.label == "loop")
            H5 = &B;
    assert(H5);
    assert(H5->params.size() == 1 &&
           "range loss through a block parameter must also block the rewrite");
    auto verified5 = il::verify::Verifier::verify(M5);
    assert(static_cast<bool>(verified5) && "module must still verify after the pass");

    return 0;
}
