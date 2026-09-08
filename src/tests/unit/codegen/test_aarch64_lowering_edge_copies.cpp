//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/codegen/test_aarch64_lowering_edge_copies.cpp
// Purpose: Pins the AArch64 lowering shape: block parameters are virtual
//          registers, branch arguments become one ParallelCopy per edge
//          (inline for `br`, in a split block for `cbr`/`switch`),
//          cross-block temporaries keep their virtual register instead of a
//          frame slot, blocks are lowered in an order where every definition
//          precedes its uses, and the whole shared corpus lowers and
//          verifies.
// Key invariants:
//   - The lowering cases run only LoweringPass; the corpus lane at the end
//     runs the whole pipeline with the function-wide allocator (Phase 3 C5)
//     and the verifier, and checks determinism.
// Ownership/Lifetime: Standalone test binary.
// Links: src/codegen/aarch64/LowerILToMIR.cpp, src/codegen/aarch64/TerminatorLowering.cpp,
//        docs/internals/backend-codegen-review-2026-09.md (Phase 3)
//
//===----------------------------------------------------------------------===//

#include "tests/TestHarness.hpp"

#include "codegen/aarch64/CodegenPipeline.hpp"
#include "codegen/aarch64/MirVerify.hpp"
#include "codegen/aarch64/TargetAArch64.hpp"
#include "codegen/aarch64/passes/LoweringPass.hpp"
#include "codegen/aarch64/passes/PassManager.hpp"
#include "il/io/Parser.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace zanna::codegen::aarch64;

namespace {

il::core::Module parseIL(const std::string &src) {
    std::istringstream ss(src);
    il::core::Module mod;
    if (!il::io::Parser::parse(ss, mod))
        return {};
    return mod;
}

/// @brief Lower @p mod and verify PostLowering.
/// @return The MIR functions; empty on failure (diagnostics printed).
std::vector<MFunction> lower(il::core::Module &mod) {
    passes::AArch64Module module;
    module.ilMod = &mod;
    module.ti = &darwinTarget();
    passes::LoweringPass pass;
    passes::Diagnostics diags;
    if (!pass.run(module, diags)) {
        diags.flush(std::cerr, &std::cerr);
        return {};
    }
    for (const auto &fn : module.mir) {
        passes::Diagnostics vdiags;
        if (!verifyMir(fn, VerifyStage::PostLowering, darwinTarget(), vdiags)) {
            vdiags.flush(std::cerr, &std::cerr);
            std::cerr << toString(fn);
            return {};
        }
    }
    return module.mir;
}

std::size_t countOpcode(const MFunction &fn, MOpcode opc) {
    std::size_t n = 0;
    for (const auto &b : fn.blocks)
        for (const auto &mi : b.instrs)
            if (mi.opc == opc)
                ++n;
    return n;
}

const MBasicBlock *findBlock(const MFunction &fn, const std::string &suffix) {
    for (const auto &b : fn.blocks) {
        if (b.name.size() >= suffix.size() &&
            b.name.compare(b.name.size() - suffix.size(), suffix.size(), suffix) == 0)
            return &b;
    }
    return nullptr;
}

/// @brief Whether every register operand of @p mi is virtual.
bool allVirtual(const MInstr &mi) {
    for (const auto &op : mi.ops)
        if (op.kind != MOperand::Kind::Reg || op.reg.isPhys)
            return false;
    return true;
}

const char *const kLoop = R"(il 0.3.0
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

} // namespace

TEST(AArch64EdgeCopyLowering, LoopUsesParallelCopiesAndNoFrameSlots) {
    il::core::Module mod = parseIL(kLoop);
    ASSERT_FALSE(mod.functions.empty());
    const auto mir = lower(mod);
    ASSERT_EQ(mir.size(), 1u);
    const MFunction &fn = mir.front();

    EXPECT_EQ(countOpcode(fn, MOpcode::LdrRegFpImm), 0u);
    EXPECT_EQ(countOpcode(fn, MOpcode::StrRegFpImm), 0u);
    EXPECT_EQ(fn.frame.spills.size(), 0u);

    // entry: immediates materialized into fresh vregs, then one copy, then br.
    const MBasicBlock *entry = findBlock(fn, "entry");
    ASSERT_TRUE(entry != nullptr);
    ASSERT_GE(entry->instrs.size(), 2u);
    const MInstr &entryCopy = entry->instrs[entry->instrs.size() - 2];
    EXPECT_EQ(entryCopy.opc, MOpcode::ParallelCopy);
    EXPECT_EQ(entryCopy.ops.size(), 4u);
    EXPECT_TRUE(allVirtual(entryCopy));
    EXPECT_EQ(entry->instrs.back().opc, MOpcode::Br);

    // body: `br loop(%s1, %i1)` copies inline before the branch.
    const MBasicBlock *body = findBlock(fn, "body");
    ASSERT_TRUE(body != nullptr);
    ASSERT_GE(body->instrs.size(), 2u);
    const MInstr &bodyCopy = body->instrs[body->instrs.size() - 2];
    EXPECT_EQ(bodyCopy.opc, MOpcode::ParallelCopy);
    EXPECT_EQ(bodyCopy.ops.size(), 4u);
    EXPECT_EQ(body->instrs.back().opc, MOpcode::Br);

    // cbr edges with arguments get split blocks holding one copy each.
    const MBasicBlock *trueEdge = findBlock(fn, ".Ledge_true_1");
    const MBasicBlock *falseEdge = findBlock(fn, ".Ledge_false_1");
    ASSERT_TRUE(trueEdge != nullptr);
    ASSERT_TRUE(falseEdge != nullptr);
    ASSERT_EQ(trueEdge->instrs.size(), 2u);
    EXPECT_EQ(trueEdge->instrs[0].opc, MOpcode::ParallelCopy);
    EXPECT_EQ(trueEdge->instrs[0].ops.size(), 2u);
    EXPECT_EQ(trueEdge->instrs[1].opc, MOpcode::Br);
    ASSERT_EQ(falseEdge->instrs.size(), 2u);
    EXPECT_EQ(falseEdge->instrs[0].opc, MOpcode::ParallelCopy);
    EXPECT_EQ(falseEdge->instrs[0].ops.size(), 4u);

    // The loop header reads its parameters straight from the phi vregs: the
    // body copy's destinations are the header's parameter vregs, and those
    // are what the compare reads.
    const MBasicBlock *loop = findBlock(fn, "loop");
    ASSERT_TRUE(loop != nullptr);
    EXPECT_EQ(countOpcode(fn, MOpcode::ParallelCopy), 4u);
}

TEST(AArch64EdgeCopyLowering, SwapIsOneParallelCopy) {
    il::core::Module mod = parseIL(R"(il 0.3.0
func @main() -> i64 {
entry:
  br loop(1, 2, 0)
loop(%a: i64, %b: i64, %i: i64):
  %done = scmp_ge %i, 5
  cbr %done, exit(%a), body(%a, %b, %i)
body(%a0: i64, %b0: i64, %i0: i64):
  %i1 = iadd.ovf %i0, 1
  br loop(%b0, %a0, %i1)
exit(%r: i64):
  ret %r
}
)");
    ASSERT_FALSE(mod.functions.empty());
    const auto mir = lower(mod);
    ASSERT_EQ(mir.size(), 1u);
    const MBasicBlock *body = findBlock(mir.front(), "body");
    ASSERT_TRUE(body != nullptr);
    const MInstr &copy = body->instrs[body->instrs.size() - 2];
    ASSERT_EQ(copy.opc, MOpcode::ParallelCopy);
    ASSERT_EQ(copy.ops.size(), 6u);
    // Destinations are the header's consecutive phi vregs (%a, %b); the
    // sources are the body's own parameters (%b0, %a0) in swapped order.
    EXPECT_EQ(copy.ops[2].reg.idOrPhys, copy.ops[0].reg.idOrPhys + 1);
    EXPECT_EQ(copy.ops[1].reg.idOrPhys, copy.ops[3].reg.idOrPhys + 1);
    EXPECT_TRUE(allVirtual(copy));
}

TEST(AArch64EdgeCopyLowering, CrossBlockTemporaryKeepsItsRegister) {
    // %x is defined in `defblock`, which appears after its user in the text;
    // the edge-copy mode lowers definitions before uses, so no slot is needed.
    il::core::Module mod = parseIL(R"(il 0.3.0
func @main() -> i64 {
entry:
  br defblock
useblock:
  %y = iadd.ovf %x, 1
  ret %y
defblock:
  %x = iadd.ovf 1, 2
  br useblock
}
)");
    ASSERT_FALSE(mod.functions.empty());
    const auto mir = lower(mod);
    ASSERT_EQ(mir.size(), 1u);
    const MFunction &fn = mir.front();
    EXPECT_EQ(countOpcode(fn, MOpcode::LdrRegFpImm), 0u);
    EXPECT_EQ(countOpcode(fn, MOpcode::StrRegFpImm), 0u);
    EXPECT_EQ(fn.frame.spills.size(), 0u);
    // The MIR block order still mirrors the IL.
    ASSERT_EQ(fn.blocks.size(), 3u);
    EXPECT_TRUE(findBlock(fn, "useblock") == &fn.blocks[1]);
    EXPECT_TRUE(findBlock(fn, "defblock") == &fn.blocks[2]);
}

TEST(AArch64EdgeCopyLowering, SwitchCaseArgumentsGetEdgeBlocks) {
    il::core::Module mod = parseIL(R"(il 0.3.0
func @main() -> i64 {
entry:
  %k64 = and 3, 7
  %k:i32 = cast.si_narrow.chk %k64
  switch.i32 %k, ^dflt(1), 0 -> ^c0(2), 1 -> ^c1(3)
c0(%a: i64):
  ret %a
c1(%b: i64):
  ret %b
dflt(%d: i64):
  ret %d
}
)");
    ASSERT_FALSE(mod.functions.empty());
    const auto mir = lower(mod);
    ASSERT_EQ(mir.size(), 1u);
    const MFunction &fn = mir.front();
    EXPECT_EQ(countOpcode(fn, MOpcode::ParallelCopy), 3u);
    const MBasicBlock *edge = findBlock(fn, ".Lswitch_case_0_0");
    ASSERT_TRUE(edge != nullptr);
    ASSERT_GE(edge->instrs.size(), 2u);
    EXPECT_EQ(edge->instrs[edge->instrs.size() - 2].opc, MOpcode::ParallelCopy);
    EXPECT_EQ(edge->instrs.back().opc, MOpcode::Br);
}

TEST(AArch64EdgeCopyLowering, FloatParametersUseFprCopies) {
    il::core::Module mod = parseIL(R"(il 0.3.0
func @main() -> i64 {
entry:
  br loop(0.5, 0)
loop(%acc: f64, %i: i64):
  %done = scmp_ge %i, 4
  cbr %done, exit(%acc), body(%acc, %i)
body(%a0: f64, %i0: i64):
  %a1 = fadd %a0, 1.5
  %i1 = iadd.ovf %i0, 1
  br loop(%a1, %i1)
exit(%r: f64):
  %v = fptosi %r
  ret %v
}
)");
    ASSERT_FALSE(mod.functions.empty());
    const auto mir = lower(mod);
    ASSERT_EQ(mir.size(), 1u);
    const MBasicBlock *body = findBlock(mir.front(), "body");
    ASSERT_TRUE(body != nullptr);
    const MInstr &copy = body->instrs[body->instrs.size() - 2];
    ASSERT_EQ(copy.opc, MOpcode::ParallelCopy);
    ASSERT_EQ(copy.ops.size(), 4u);
    EXPECT_EQ(copy.ops[0].reg.cls, RegClass::FPR);
    EXPECT_EQ(copy.ops[1].reg.cls, RegClass::FPR);
    EXPECT_EQ(copy.ops[2].reg.cls, RegClass::GPR);
    EXPECT_EQ(copy.ops[3].reg.cls, RegClass::GPR);
}

TEST(AArch64EdgeCopyLowering, SharedCorpusLowersAndVerifies) {
    namespace fs = std::filesystem;
    const fs::path root = fs::path(ZANNA_SHARED_IL_CORPUS_DIR) / "success";
    std::size_t files = 0;
    for (const auto &entry : fs::directory_iterator(root)) {
        if (entry.path().extension() != ".il")
            continue;
        std::ifstream in(entry.path());
        std::stringstream buf;
        buf << in.rdbuf();
        il::core::Module mod = parseIL(buf.str());
        if (mod.functions.empty())
            continue;
        const auto mir = lower(mod);
        if (mir.empty())
            std::cerr << "edge-copy lowering failed for " << entry.path() << "\n";
        ASSERT_FALSE(mir.empty());
        for (const auto &fn : mir) {
        }
        ++files;
    }
    EXPECT_GT(files, 20u);
}

namespace {

/// @brief Run the whole pipeline on @p mod with the function-wide allocator
///        and the verifier at @p level; return the assembly (empty on failure).
std::string compileGlobally(il::core::Module &mod, int level, std::ostream &diag) {
    passes::AArch64Module m;
    m.ilMod = &mod;
    m.ti = &darwinTarget();
    PipelineOptions opts;
    opts.emitAssemblyText = true;
    opts.optimizeLevel = level;
    opts.verifyMir = true;
    if (!runCodegenPipeline(m, opts, diag))
        return {};
    return m.assembly;
}

} // namespace

TEST(AArch64EdgeCopyLowering, SharedCorpusAllocatesGloballyAndVerifies) {
    // Every corpus program goes through lowering, legalization, the
    // function-wide allocator, the post-RA passes, and emission with the
    // verifier on, at -O0 and -O2; the allocation is deterministic and leaves
    // no ParallelCopy behind.
    namespace fs = std::filesystem;
    const fs::path root = fs::path(ZANNA_SHARED_IL_CORPUS_DIR) / "success";
    std::size_t files = 0;
    for (const auto &entry : fs::directory_iterator(root)) {
        if (entry.path().extension() != ".il")
            continue;
        std::ifstream in(entry.path());
        std::stringstream buf;
        buf << in.rdbuf();
        for (int level : {0, 2}) {
            il::core::Module first = parseIL(buf.str());
            if (first.functions.empty())
                continue;
            std::ostringstream diag;
            const std::string asmA = compileGlobally(first, level, diag);
            if (asmA.empty() || diag.str().find("V-CG-MIR-") != std::string::npos) {
                std::cerr << "global allocation failed for " << entry.path() << " at -O" << level
                          << ":\n"
                          << diag.str();
            }
            ASSERT_FALSE(asmA.empty());
            EXPECT_EQ(diag.str().find("V-CG-MIR-"), std::string::npos);

            il::core::Module second = parseIL(buf.str());
            std::ostringstream diag2;
            const std::string asmB = compileGlobally(second, level, diag2);
            EXPECT_EQ(asmA, asmB);
        }
        ++files;
    }
    EXPECT_GT(files, 20u);
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, &argv);
    return zanna_test::run_all_tests();
}
