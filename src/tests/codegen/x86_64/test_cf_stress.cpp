//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/codegen/x86_64/test_cf_stress.cpp
// Purpose: Stress-test the x86-64 backend's control flow code generation.
//          Exercises every comparison predicate (signed, unsigned, float),
//          conditional and unconditional branches, switch statements, direct
//          and indirect function calls, loops, nested if/else, and edge cases.
// Key invariants:
//   - Integer comparisons (10 predicates) map to correct SETcc condition codes
//   - Float comparisons (8 predicates) use NaN-safe sequences where needed
//   - Conditional branches emit TEST/JCC/JMP sequences
//   - Switch emits linear CMP+JCC chain
//   - Direct calls emit "callq label", indirect calls emit "callq *%reg"
// Ownership/Lifetime: IL modules constructed within each test scope.
// Links: src/codegen/x86_64/Lowering.CF.cpp,
//        src/codegen/x86_64/Lowering.EmitCommon.cpp,
//        src/codegen/x86_64/Lowering.Arith.cpp
//
//===----------------------------------------------------------------------===//

#include "codegen/x86_64/Backend.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace zanna::codegen::x64;

namespace {

// ===----------------------------------------------------------------------===
// Helper functions
// ===----------------------------------------------------------------------===

[[nodiscard]] ILValue val(int id) noexcept {
    ILValue v{};
    v.kind = ILValue::Kind::I64;
    v.id = id;
    return v;
}

[[nodiscard]] ILValue valF(int id) noexcept {
    ILValue v{};
    v.kind = ILValue::Kind::F64;
    v.id = id;
    return v;
}

[[nodiscard]] ILValue valB(int id) noexcept {
    ILValue v{};
    v.kind = ILValue::Kind::I1;
    v.id = id;
    return v;
}

[[nodiscard]] ILValue valP(int id) noexcept {
    ILValue v{};
    v.kind = ILValue::Kind::PTR;
    v.id = id;
    return v;
}

[[nodiscard]] ILValue imm(int64_t v) noexcept {
    ILValue c{};
    c.kind = ILValue::Kind::I64;
    c.id = -1;
    c.i64 = v;
    return c;
}

[[nodiscard]] ILValue lab(const char *name) noexcept {
    ILValue v{};
    v.kind = ILValue::Kind::LABEL;
    v.id = -1;
    v.label = name;
    return v;
}

[[nodiscard]] ILInstr makeOp(const char *opc,
                             std::vector<ILValue> ops,
                             int res,
                             ILValue::Kind k = ILValue::Kind::I64) {
    ILInstr instr{};
    instr.opcode = opc;
    instr.ops = std::move(ops);
    instr.resultId = res;
    instr.resultKind = k;
    return instr;
}

/// Make a "cmp" instruction with explicit condition code as 3rd operand.
/// This is how the adapter translates ICmpEq/Ne, SCmpLT/LE/GT/GE, UCmpGT/GE/LT/LE.
[[nodiscard]] ILInstr makeCmp(int lhsId, int rhsId, int condCode, int resId) {
    ILInstr instr{};
    instr.opcode = "cmp";
    instr.ops = {val(lhsId), val(rhsId), imm(condCode)};
    instr.resultId = resId;
    instr.resultKind = ILValue::Kind::I1;
    return instr;
}

/// Make a floating-point comparison instruction.
[[nodiscard]] ILInstr makeFCmp(const char *cmpOp, int lhsId, int rhsId, int resId) {
    ILInstr instr{};
    instr.opcode = cmpOp;
    instr.ops = {valF(lhsId), valF(rhsId)};
    instr.resultId = resId;
    instr.resultKind = ILValue::Kind::I1;
    return instr;
}

[[nodiscard]] ILInstr makeRet(int id, ILValue::Kind k = ILValue::Kind::I64) {
    ILInstr instr{};
    instr.opcode = "ret";
    ILValue ref{};
    ref.kind = k;
    ref.id = id;
    instr.ops = {ref};
    return instr;
}

[[nodiscard]] ILInstr makeBr(const char *target) {
    ILInstr instr{};
    instr.opcode = "br";
    instr.ops = {lab(target)};
    instr.resultId = -1;
    return instr;
}

[[nodiscard]] ILInstr makeCbr(int condId, const char *trueTarget, const char *falseTarget) {
    ILInstr instr{};
    instr.opcode = "cbr";
    instr.ops = {valB(condId), lab(trueTarget), lab(falseTarget)};
    instr.resultId = -1;
    return instr;
}

/// Build a switch_i32 instruction.
/// Format: ops[0] = scrutinee, then (caseValue, caseLabel) pairs, then default label.
[[nodiscard]] ILInstr makeSwitch(int scrutId,
                                 const std::vector<std::pair<int64_t, const char *>> &cases,
                                 const char *defaultLabel) {
    ILInstr instr{};
    instr.opcode = "switch_i32";
    instr.ops.push_back(val(scrutId));
    for (const auto &[caseVal, caseLab] : cases) {
        instr.ops.push_back(imm(caseVal));
        instr.ops.push_back(lab(caseLab));
    }
    instr.ops.push_back(lab(defaultLabel));
    instr.resultId = -1;
    return instr;
}

[[nodiscard]] std::string compileToAsm(ILModule &m) {
    CodegenOptions options{};
    options.targetPlatform = CodegenOptions::TargetPlatform::Linux;
    const CodegenResult res = emitModuleToAssembly(m, options);
    return res.asmText;
}

[[nodiscard]] std::size_t countOccurrences(const std::string &text, const std::string &pattern) {
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = text.find(pattern, pos)) != std::string::npos) {
        ++count;
        pos += pattern.size();
    }
    return count;
}

/// Check whether the text contains any conditional jump (jcc) instruction.
/// The backend may fuse comparison+branch into cmpq+jcc (e.g., jl, jg)
/// instead of materialising a boolean and using testq+jne.  Both patterns
/// are correct; this helper accepts either.
[[nodiscard]] bool hasAnyConditionalJump(const std::string &text) {
    static const char *jcc[] = {
        "je ",
        "jne ",
        "jl ",
        "jle ",
        "jg ",
        "jge ",
        "jb ",
        "jbe ",
        "ja ",
        "jae ",
        "js ",
        "jns ",
        "jo ",
        "jno ",
        "jp ",
        "jnp ",
    };
    for (auto p : jcc)
        if (text.find(p) != std::string::npos)
            return true;
    return false;
}

/// Count total conditional jump instructions in the text.
[[nodiscard]] std::size_t countConditionalJumps(const std::string &text) {
    static const char *jcc[] = {
        "je ",
        "jne ",
        "jl ",
        "jle ",
        "jg ",
        "jge ",
        "jb ",
        "jbe ",
        "ja ",
        "jae ",
        "js ",
        "jns ",
        "jo ",
        "jno ",
        "jp ",
        "jnp ",
    };
    std::size_t total = 0;
    for (auto p : jcc)
        total += countOccurrences(text, p);
    return total;
}

/// Check whether the text contains any flag-setting comparison (testq or cmpq).
[[nodiscard]] bool hasAnyComparison(const std::string &text) {
    return text.find("testq") != std::string::npos || text.find("cmpq") != std::string::npos;
}

// ===----------------------------------------------------------------------===
// Test bookkeeping
// ===----------------------------------------------------------------------===

struct CategoryStats {
    const char *name;
    int total{0};
    int pass{0};
    int fail{0};
};

struct TestContext {
    std::vector<CategoryStats> categories;
    int currentCat{-1};
    int globalFail{0};

    void beginCategory(const char *name) {
        categories.push_back({name, 0, 0, 0});
        currentCat = static_cast<int>(categories.size()) - 1;
    }

    void checkCustom(const char *caseName, bool passed) {
        auto &cat = categories[static_cast<std::size_t>(currentCat)];
        ++cat.total;
        if (passed) {
            ++cat.pass;
        } else {
            ++cat.fail;
            ++globalFail;
            std::cerr << "FAIL [" << cat.name << "] " << caseName << "\n";
        }
    }

    void checkAsm(const char *caseName, const std::string &asmText, const std::string &expected) {
        auto &cat = categories[static_cast<std::size_t>(currentCat)];
        ++cat.total;

        if (asmText.find(expected) != std::string::npos) {
            ++cat.pass;
        } else {
            ++cat.fail;
            ++globalFail;
            std::cerr << "FAIL [" << cat.name << "] " << caseName << "\n"
                      << "  expected substring: \"" << expected << "\"\n"
                      << "  actual output:\n"
                      << asmText << "\n";
        }
    }

    void checkTrue(const char *caseName, bool condition, const char *what) {
        auto &cat = categories[static_cast<std::size_t>(currentCat)];
        ++cat.total;
        if (condition) {
            ++cat.pass;
        } else {
            ++cat.fail;
            ++globalFail;
            std::cerr << "FAIL [" << cat.name << "] " << caseName << "\n"
                      << "  expected " << what << "\n";
        }
    }

    void checkCount(const char *caseName,
                    const std::string &asmText,
                    const std::string &pattern,
                    std::size_t minCount) {
        auto &cat = categories[static_cast<std::size_t>(currentCat)];
        ++cat.total;

        const std::size_t actual = countOccurrences(asmText, pattern);
        if (actual >= minCount) {
            ++cat.pass;
        } else {
            ++cat.fail;
            ++globalFail;
            std::cerr << "FAIL [" << cat.name << "] " << caseName << "\n"
                      << "  expected at least " << minCount << " of \"" << pattern << "\"\n"
                      << "  found " << actual << "\n";
        }
    }

    void printSummary() const {
        std::cout << "\n=== x86-64 Control Flow Stress Test ===\n";
        std::cout << "Category                 Total  Pass  Fail\n";
        int totalAll = 0, passAll = 0, failAll = 0;
        for (const auto &cat : categories) {
            std::string padded = cat.name;
            while (padded.size() < 25)
                padded.push_back(' ');
            std::cout << padded << " " << cat.total << "     " << cat.pass << "     " << cat.fail
                      << "\n";
            totalAll += cat.total;
            passAll += cat.pass;
            failAll += cat.fail;
        }
        std::cout << "----------------------------------------------\n";
        std::cout << "TOTAL                     " << totalAll << "    " << passAll << "     "
                  << failAll << "\n\n";
    }
};

// ===----------------------------------------------------------------------===
// Part A: Condition Code Completeness
// ===----------------------------------------------------------------------===

/// Test 1: All 10 integer comparison condition codes.
void testIntCmpCodes(TestContext &ctx) {
    ctx.beginCategory("Int cmp codes (10)");

    ILValue a = val(0);
    ILValue b = val(1);
    a.id = 0;
    b.id = 1;

    // 10 comparisons: cmp(a, b, condCode) → result
    // Codes: 0=eq, 1=ne, 2=slt, 3=sle, 4=sgt, 5=sge, 6=ugt, 7=uge, 8=ult, 9=ule
    std::vector<ILInstr> instrs;
    for (int cc = 0; cc <= 9; ++cc) {
        instrs.push_back(makeCmp(0, 1, cc, 10 + cc));
    }

    // zext each I1 to I64 and chain add to prevent DCE
    for (int cc = 0; cc <= 9; ++cc) {
        instrs.push_back(makeOp("zext", {valB(10 + cc)}, 20 + cc));
    }

    // Chain adds: 20+21+...+29
    int accum = 20;
    for (int cc = 1; cc <= 9; ++cc) {
        instrs.push_back(makeOp("add", {val(accum), val(20 + cc)}, 30 + cc));
        accum = 30 + cc;
    }
    instrs.push_back(makeRet(accum));

    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0, 1};
    entry.paramKinds = {ILValue::Kind::I64, ILValue::Kind::I64};
    entry.instrs = std::move(instrs);

    ILFunction fn{};
    fn.name = "int_cmp_all";
    fn.blocks = {entry};

    ILModule m{};
    m.funcs = {fn};

    const auto text = compileToAsm(m);

    // Verify all 10 SETcc variants are present
    ctx.checkAsm("sete", text, "sete ");
    ctx.checkAsm("setne", text, "setne ");
    ctx.checkAsm("setl", text, "setl ");
    ctx.checkAsm("setle", text, "setle ");
    ctx.checkAsm("setg", text, "setg ");
    ctx.checkAsm("setge", text, "setge ");
    ctx.checkAsm("seta", text, "seta ");
    ctx.checkAsm("setae", text, "setae ");
    ctx.checkAsm("setb", text, "setb ");
    ctx.checkAsm("setbe", text, "setbe ");
    // Verify CMP instructions present
    ctx.checkCount("cmpq_count", text, "cmpq", 10);
}

/// Test 2: All 8 float comparison condition codes.
void testFpCmpCodes(TestContext &ctx) {
    ctx.beginCategory("FP cmp codes (8)");

    const char *fcmpOps[] = {
        "fcmp_eq",
        "fcmp_ne",
        "fcmp_lt",
        "fcmp_le",
        "fcmp_gt",
        "fcmp_ge",
        "fcmp_ord",
        "fcmp_uno",
    };

    std::vector<ILInstr> instrs;
    for (int i = 0; i < 8; ++i) {
        instrs.push_back(makeFCmp(fcmpOps[i], 0, 1, 10 + i));
    }

    // zext each I1 → I64 and chain-add
    for (int i = 0; i < 8; ++i) {
        instrs.push_back(makeOp("zext", {valB(10 + i)}, 20 + i));
    }

    int accum = 20;
    for (int i = 1; i < 8; ++i) {
        instrs.push_back(makeOp("add", {val(accum), val(20 + i)}, 30 + i));
        accum = 30 + i;
    }
    instrs.push_back(makeRet(accum));

    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0, 1};
    entry.paramKinds = {ILValue::Kind::F64, ILValue::Kind::F64};
    entry.instrs = std::move(instrs);

    ILFunction fn{};
    fn.name = "fp_cmp_all";
    fn.blocks = {entry};

    ILModule m{};
    m.funcs = {fn};

    const auto text = compileToAsm(m);

    // Verify UCOMISD present
    ctx.checkAsm("ucomisd", text, "ucomisd");

    // NaN-safe patterns for eq/ne/lt/le:
    // eq: sete + setnp + and
    ctx.checkAsm("eq_setnp", text, "setnp ");
    // ne: setne + setp + or
    ctx.checkAsm("ne_setp", text, "setp ");
    // gt: seta (simple)
    ctx.checkAsm("gt_seta", text, "seta ");
    // ge: setae (simple)
    ctx.checkAsm("ge_setae", text, "setae ");
}

/// Test 3: Comparison feeding a conditional branch.
void testCmpFeedsCbr(TestContext &ctx) {
    ctx.beginCategory("Cmp feeds cbr");

    // entry: cmp(a, b, 2=slt) → %2, cbr %2, then, else
    // then: ret 1
    // else: ret 0
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0, 1};
    entry.paramKinds = {ILValue::Kind::I64, ILValue::Kind::I64};
    entry.instrs = {
        makeCmp(0, 1, 2, 2), // scmp_lt
        makeCbr(2, "then_blk", "else_blk"),
    };

    ILBlock thenBlk{};
    thenBlk.name = "then_blk";
    thenBlk.instrs = {makeRet(-1)}; // ret with literal

    // Use a separate approach: return an immediate
    ILInstr retOne{};
    retOne.opcode = "ret";
    retOne.ops = {imm(1)};
    retOne.resultId = -1;
    thenBlk.instrs = {retOne};

    ILBlock elseBlk{};
    elseBlk.name = "else_blk";
    ILInstr retZero{};
    retZero.opcode = "ret";
    retZero.ops = {imm(0)};
    retZero.resultId = -1;
    elseBlk.instrs = {retZero};

    ILFunction fn{};
    fn.name = "cmp_cbr";
    fn.blocks = {entry, thenBlk, elseBlk};

    ILModule m{};
    m.funcs = {fn};

    const auto text = compileToAsm(m);

    ctx.checkAsm("has_cmpq", text, "cmpq");
    // The backend may fuse cmp+cbr into cmpq+jcc (e.g. jl) rather than
    // materialising a boolean with testq+jne.  Accept either pattern.
    ctx.checkCustom("has_flag_test", hasAnyComparison(text));
    ctx.checkCustom("has_jcc", hasAnyConditionalJump(text));
    ctx.checkAsm("has_jmp", text, "jmp ");
    ctx.checkAsm("has_ret", text, "ret");
}

// ===----------------------------------------------------------------------===
// Part B: Control Flow Patterns
// ===----------------------------------------------------------------------===

/// Test 4: Simple if/else (diamond CFG).
void testDiamondCfg(TestContext &ctx) {
    ctx.beginCategory("Diamond if/else");

    // entry: cmp(a, 0) → cbr(then, else)
    // then: %t=add(a,1), br(join)
    // else: %e=add(a,2), br(join)
    // join: ret (need value — use a constant for simplicity)
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0};
    entry.paramKinds = {ILValue::Kind::I64};
    entry.instrs = {
        makeCmp(0, 0, 1, 2), // icmp_ne a, a (will be false, but exercises the pattern)
        makeCbr(2, "then_blk", "else_blk"),
    };

    // Rewrite: use a non-trivial comparison
    entry.instrs[0] = makeOp("cmp", {val(0), imm(0), imm(1)}, 2, ILValue::Kind::I1);

    ILBlock thenBlk{};
    thenBlk.name = "then_blk";
    thenBlk.instrs = {
        makeOp("add", {val(0), imm(10)}, 3),
        makeBr("join_blk"),
    };

    ILBlock elseBlk{};
    elseBlk.name = "else_blk";
    elseBlk.instrs = {
        makeOp("add", {val(0), imm(20)}, 4),
        makeBr("join_blk"),
    };

    ILBlock joinBlk{};
    joinBlk.name = "join_blk";
    // Return the param directly — the add results are dead but prevent block elimination
    ILInstr retI{};
    retI.opcode = "ret";
    retI.ops = {val(0)};
    retI.resultId = -1;
    joinBlk.instrs = {retI};

    ILFunction fn{};
    fn.name = "diamond";
    fn.blocks = {entry, thenBlk, elseBlk, joinBlk};

    ILModule m{};
    m.funcs = {fn};

    const auto text = compileToAsm(m);

    ctx.checkAsm("has_testq", text, "testq");
    ctx.checkAsm("has_jne", text, "jne ");
    ctx.checkAsm("has_jmp", text, "jmp ");
    ctx.checkAsm("has_ret", text, "ret");
    // Should have at least 3 block labels (then, else, join or similar)
    ctx.checkCount("labels", text, ":", 4);
}

/// Test 5: While loop with backward branch.
void testWhileLoop(TestContext &ctx) {
    ctx.beginCategory("While loop");

    // entry: br(loop_hdr)
    // loop_hdr: cmp(param, 0, ne) → cbr(body, exit)
    // body: add(param, -1) → br(loop_hdr)    [backward branch!]
    // exit: ret param
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0};
    entry.paramKinds = {ILValue::Kind::I64};
    entry.instrs = {makeBr("loop_hdr")};

    ILBlock loopHdr{};
    loopHdr.name = "loop_hdr";
    loopHdr.instrs = {
        makeOp("cmp", {val(0), imm(0), imm(1)}, 2, ILValue::Kind::I1), // ne
        makeCbr(2, "body", "exit"),
    };

    ILBlock body{};
    body.name = "body";
    body.instrs = {
        makeOp("add", {val(0), imm(-1)}, 3),
        makeBr("loop_hdr"), // backward branch
    };

    ILBlock exitBlk{};
    exitBlk.name = "exit";
    exitBlk.instrs = {makeRet(0)};

    ILFunction fn{};
    fn.name = "while_loop";
    fn.blocks = {entry, loopHdr, body, exitBlk};

    ILModule m{};
    m.funcs = {fn};

    const auto text = compileToAsm(m);

    // Should have multiple jumps — at least 2 jmp (entry→loop, body→loop)
    ctx.checkCount("jmp_count", text, "jmp ", 2);
    // Should have at least 1 conditional jump (may be jne, jl, etc.)
    ctx.checkCustom("has_jcc", hasAnyConditionalJump(text));
    // Should have at least 3 labels
    ctx.checkCount("labels", text, ":", 4);
    ctx.checkAsm("has_ret", text, "ret");
}

/// Test 6: Switch statement (switch_i32).
void testSwitchI32(TestContext &ctx) {
    ctx.beginCategory("Switch (switch_i32)");

    // entry: switch_i32(%scrutinee, default, 1→case1, 2→case2, 3→case3, 5→case5)
    // case1: ret 10
    // case2: ret 20
    // case3: ret 30
    // case5: ret 50
    // default: ret 0
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0};
    entry.paramKinds = {ILValue::Kind::I64};
    entry.instrs = {
        makeSwitch(0, {{1, "case1"}, {2, "case2"}, {3, "case3"}, {5, "case5"}}, "default_blk")};

    auto makeRetBlock = [](const char *name, int64_t retVal) {
        ILBlock blk{};
        blk.name = name;
        ILInstr ret{};
        ret.opcode = "ret";
        ret.ops.push_back({});
        ret.ops.back().kind = ILValue::Kind::I64;
        ret.ops.back().id = -1;
        ret.ops.back().i64 = retVal;
        ret.resultId = -1;
        blk.instrs = {ret};
        return blk;
    };

    ILFunction fn{};
    fn.name = "switch_test";
    fn.blocks = {entry,
                 makeRetBlock("case1", 10),
                 makeRetBlock("case2", 20),
                 makeRetBlock("case3", 30),
                 makeRetBlock("case5", 50),
                 makeRetBlock("default_blk", 0)};

    ILModule m{};
    m.funcs = {fn};

    const auto text = compileToAsm(m);

    // Four compares, each followed by an equality branch. The peephole may
    // invert the last one so its case falls through (`jne default`), so count
    // both spellings: three `je` and four equality branches in total.
    ctx.checkAsm("cmp_1", text, "$1,");
    ctx.checkAsm("cmp_2", text, "$2,");
    ctx.checkAsm("cmp_3", text, "$3,");
    ctx.checkAsm("cmp_5", text, "$5,");
    ctx.checkCount("je_count", text, "je ", 3);
    ctx.checkTrue("eq_branch_count",
                  countOccurrences(text, "je ") + countOccurrences(text, "jne ") >= 4,
                  "four equality branches (je or jne)");
    // Default: jmp at the end
    ctx.checkAsm("default_jmp", text, "jmp ");
    // Multiple return points
    ctx.checkCount("ret_count", text, "ret", 5);
}

/// Test 7: Nested if/else (3 levels deep).
void testNestedIfElse(TestContext &ctx) {
    ctx.beginCategory("Nested if/else (3)");

    // Structure:
    // entry: cmp(a, 0, 4=sgt) → cbr(l1_t, l1_f)
    // l1_t:  cmp(b, 0, 4=sgt) → cbr(l2_tt, l2_tf)
    // l1_f:  cmp(b, 0, 4=sgt) → cbr(l2_ft, l2_ff)
    // l2_tt: ret 1
    // l2_tf: ret 2
    // l2_ft: ret 3
    // l2_ff: ret 4
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0, 1};
    entry.paramKinds = {ILValue::Kind::I64, ILValue::Kind::I64};
    entry.instrs = {
        makeOp("cmp", {val(0), val(1), imm(4)}, 10, ILValue::Kind::I1), // sgt a, b
        makeCbr(10, "l1_t", "l1_f"),
    };

    ILBlock l1t{};
    l1t.name = "l1_t";
    l1t.instrs = {
        makeOp("cmp", {val(1), val(0), imm(2)}, 11, ILValue::Kind::I1), // slt b, a
        makeCbr(11, "l2_tt", "l2_tf"),
    };

    ILBlock l1f{};
    l1f.name = "l1_f";
    l1f.instrs = {
        makeOp("cmp", {val(0), val(1), imm(0)}, 12, ILValue::Kind::I1), // eq a, b
        makeCbr(12, "l2_ft", "l2_ff"),
    };

    auto makeLeaf = [](const char *name, int64_t retVal) {
        ILBlock blk{};
        blk.name = name;
        ILInstr ret{};
        ret.opcode = "ret";
        ret.ops.push_back({});
        ret.ops.back().kind = ILValue::Kind::I64;
        ret.ops.back().id = -1;
        ret.ops.back().i64 = retVal;
        ret.resultId = -1;
        blk.instrs = {ret};
        return blk;
    };

    ILFunction fn{};
    fn.name = "nested_ifelse";
    fn.blocks = {entry,
                 l1t,
                 l1f,
                 makeLeaf("l2_tt", 1),
                 makeLeaf("l2_tf", 2),
                 makeLeaf("l2_ft", 3),
                 makeLeaf("l2_ff", 4)};

    ILModule m{};
    m.funcs = {fn};

    const auto text = compileToAsm(m);

    // Should have 3 cmpq instructions (one per level)
    ctx.checkCount("cmpq_count", text, "cmpq", 3);
    // Should have 3 conditional branches (jcc from each cbr).
    // The backend may emit jne, jl, jg, etc. depending on comparison fusion.
    ctx.checkCustom("jcc_count", countConditionalJumps(text) >= 3);
    // Should have multiple labels (at least 6 blocks)
    ctx.checkCount("labels", text, ":", 7);
    // Should have 4 ret instructions (one per leaf)
    ctx.checkCount("ret_count", text, "ret", 4);
}

/// Test 8: Direct function call.
void testDirectCall(TestContext &ctx) {
    ctx.beginCategory("Direct call");

    // Callee: add_fn(a, b) → ret a+b
    ILBlock calleeEntry{};
    calleeEntry.name = "entry";
    calleeEntry.paramIds = {0, 1};
    calleeEntry.paramKinds = {ILValue::Kind::I64, ILValue::Kind::I64};
    calleeEntry.instrs = {
        makeOp("add", {val(0), val(1)}, 2),
        makeRet(2),
    };

    ILFunction callee{};
    callee.name = "add_fn";
    callee.blocks = {calleeEntry};

    // Caller: main(x) → call @add_fn(x, 42) → ret result
    ILBlock callerEntry{};
    callerEntry.name = "entry";
    callerEntry.paramIds = {0};
    callerEntry.paramKinds = {ILValue::Kind::I64};

    ILInstr callInstr{};
    callInstr.opcode = "call";
    callInstr.ops = {lab("add_fn"), val(0), imm(42)};
    callInstr.resultId = 5;
    callInstr.resultKind = ILValue::Kind::I64;
    callerEntry.instrs = {callInstr, makeRet(5)};

    ILFunction caller{};
    caller.name = "caller";
    caller.blocks = {callerEntry};

    ILModule m{};
    m.funcs = {callee, caller};

    const auto text = compileToAsm(m);

    ctx.checkAsm("callq_label", text, "callq add_fn");
    ctx.checkAsm("has_add_fn", text, "add_fn:");
    ctx.checkAsm("has_caller", text, "caller:");
}

// ===----------------------------------------------------------------------===
// Part C: Edge Cases
// ===----------------------------------------------------------------------===

/// Test 9: Indirect function call.
void testIndirectCall(TestContext &ctx) {
    ctx.beginCategory("Indirect call");

    // fn(ptr, x) → call.indirect %ptr(x) → ret result
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0, 1};
    entry.paramKinds = {ILValue::Kind::PTR, ILValue::Kind::I64};

    ILInstr callInstr{};
    callInstr.opcode = "call.indirect";
    callInstr.ops = {valP(0), val(1)};
    callInstr.resultId = 5;
    callInstr.resultKind = ILValue::Kind::I64;
    entry.instrs = {callInstr, makeRet(5)};

    ILFunction fn{};
    fn.name = "indirect_caller";
    fn.blocks = {entry};

    ILModule m{};
    m.funcs = {fn};

    const auto text = compileToAsm(m);

    // Indirect call: callq *%rXX
    ctx.checkAsm("callq_indirect", text, "callq *%r");
}

/// Test 10: Empty block fallthrough.
void testEmptyBlockFallthrough(TestContext &ctx) {
    ctx.beginCategory("Empty block fallthru");

    // entry: add(a,1) → br(mid)
    // mid: br(exit)
    // exit: ret(result)
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0};
    entry.paramKinds = {ILValue::Kind::I64};
    entry.instrs = {
        makeOp("add", {val(0), imm(1)}, 2),
        makeBr("mid"),
    };

    ILBlock mid{};
    mid.name = "mid";
    mid.instrs = {makeBr("exit_blk")};

    ILBlock exitBlk{};
    exitBlk.name = "exit_blk";
    exitBlk.instrs = {makeRet(2)};

    ILFunction fn{};
    fn.name = "empty_fallthru";
    fn.blocks = {entry, mid, exitBlk};

    ILModule m{};
    m.funcs = {fn};

    const auto text = compileToAsm(m);

    ctx.checkAsm("compiles", text, "empty_fallthru");
    ctx.checkAsm("has_ret", text, "ret");
    // The empty middle block is threaded away: entry jumps straight to the
    // exit block, so exactly one jump survives and no jump names `mid`.
    ctx.checkCount("jmp_count", text, "jmp ", 1);
    ctx.checkAsm("jmp_to_exit", text, "jmp .L_empty_fallthru_exit_blk");
}

/// Test 11: Both branches of cbr go to the same target.
void testBothBranchesSameTarget(TestContext &ctx) {
    ctx.beginCategory("Same-target cbr");

    // entry: cmp(a, 0, 1=ne) → cbr(target, target)  — both branches go to same block
    // target: ret(a)
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0};
    entry.paramKinds = {ILValue::Kind::I64};
    entry.instrs = {
        makeOp("cmp", {val(0), imm(0), imm(1)}, 2, ILValue::Kind::I1),
        makeCbr(2, "target", "target"),
    };

    ILBlock target{};
    target.name = "target";
    target.instrs = {makeRet(0)};

    ILFunction fn{};
    fn.name = "same_target";
    fn.blocks = {entry, target};

    ILModule m{};
    m.funcs = {fn};

    const auto text = compileToAsm(m);

    ctx.checkAsm("compiles", text, "same_target");
    ctx.checkAsm("has_ret", text, "ret");
    // Should still emit the conditional branch machinery (testq or cmpq)
    ctx.checkCustom("has_flag_test", hasAnyComparison(text));
}

/// Test 12: Comparison after arithmetic (explicit CMP, not flag reuse).
void testCmpAfterArith(TestContext &ctx) {
    ctx.beginCategory("Cmp after arith");

    // entry: %2 = add(a, b)
    //        %3 = cmp(%2, 100, 4=sgt)   — use non-zero imm to get actual cmpq
    //        cbr %3, pos, neg
    // pos: ret %2
    // neg: ret 0
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0, 1};
    entry.paramKinds = {ILValue::Kind::I64, ILValue::Kind::I64};
    entry.instrs = {
        makeOp("add", {val(0), val(1)}, 2),
        makeOp("cmp", {val(2), imm(100), imm(4)}, 3, ILValue::Kind::I1), // sgt vs 100
        makeCbr(3, "pos", "neg"),
    };

    ILBlock pos{};
    pos.name = "pos";
    pos.instrs = {makeRet(2)};

    ILBlock neg{};
    neg.name = "neg";
    ILInstr retZero{};
    retZero.opcode = "ret";
    retZero.ops = {imm(0)};
    retZero.resultId = -1;
    neg.instrs = {retZero};

    ILFunction fn{};
    fn.name = "cmp_after_arith";
    fn.blocks = {entry, pos, neg};

    ILModule m{};
    m.funcs = {fn};

    const auto text = compileToAsm(m);

    ctx.checkAsm("has_addq", text, "addq");
    ctx.checkAsm("has_cmpq", text, "cmpq");
    // The backend may fuse cmp+cbr, so testq may be absent when cmpq+jcc
    // is used directly.  Check for any flag-setting comparison and any jcc.
    ctx.checkCustom("has_flag_test", hasAnyComparison(text));
    ctx.checkCustom("has_jcc", hasAnyConditionalJump(text));
    ctx.checkAsm("has_ret", text, "ret");
}

} // namespace

// ===----------------------------------------------------------------------===
// Main
// ===----------------------------------------------------------------------===

int main() {
    TestContext ctx;

    // Part A: Condition Code Completeness
    testIntCmpCodes(ctx);
    testFpCmpCodes(ctx);
    testCmpFeedsCbr(ctx);

    // Part B: Control Flow Patterns
    testDiamondCfg(ctx);
    testWhileLoop(ctx);
    testSwitchI32(ctx);
    testNestedIfElse(ctx);
    testDirectCall(ctx);

    // Part C: Edge Cases
    testIndirectCall(ctx);
    testEmptyBlockFallthrough(ctx);
    testBothBranchesSameTarget(ctx);
    testCmpAfterArith(ctx);

    ctx.printSummary();

    if (ctx.globalFail != 0) {
        std::cerr << ctx.globalFail << " test(s) FAILED\n";
        return EXIT_FAILURE;
    }
    std::cout << "All tests passed.\n";
    return EXIT_SUCCESS;
}
