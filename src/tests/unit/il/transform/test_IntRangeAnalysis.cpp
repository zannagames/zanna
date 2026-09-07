//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/il/transform/test_IntRangeAnalysis.cpp
// Purpose: Validate the whole-function integer range analysis and the
//          CheckOpt/verifier consumers that demote or delete checks from its
//          proofs (ADR 0026).
// Key invariants:
//   - Recorded ranges are sound: unbounded loop accumulators never receive a
//     finite bound; guarded induction variables receive the guard bound.
//   - Every demotion CheckOpt performs re-verifies through the shared prover.
// Ownership/Lifetime: Builds a transient module per test invocation.
// Links: docs/adr/0026-range-analysis-demotion-proofs.md
//
//===----------------------------------------------------------------------===//

#include "il/analysis/IntRangeAnalysis.hpp"

#include "il/analysis/CFG.hpp"
#include "il/analysis/Dominators.hpp"
#include "il/api/expected_api.hpp"
#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Module.hpp"
#include "il/core/Opcode.hpp"
#include "il/transform/AnalysisManager.hpp"
#include "il/transform/CheckOpt.hpp"
#include "il/transform/analysis/Liveness.hpp"
#include "il/transform/analysis/LoopInfo.hpp"
#include "il/verify/Verifier.hpp"
#include "tests/TestHarness.hpp"

#include <sstream>
#include <string>

using namespace il::core;

namespace {

Module parseModule(const std::string &text) {
    Module module;
    std::istringstream input(text);
    auto parsed = il::api::v2::parse_text_expected(input, module);
    ASSERT_TRUE(static_cast<bool>(parsed));
    return module;
}

/// @brief Find a block param's SSA id by block label and param index.
unsigned paramId(const Function &fn, const std::string &label, size_t index) {
    for (const auto &block : fn.blocks)
        if (block.label == label)
            return block.params.at(index).id;
    ASSERT_TRUE(false && "block not found");
    return 0;
}

size_t countOpcode(const Function &fn, Opcode op) {
    size_t count = 0;
    for (const auto &block : fn.blocks)
        for (const auto &instr : block.instructions)
            if (instr.op == op)
                ++count;
    return count;
}

il::transform::AnalysisRegistry makeRegistry() {
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
    registry.registerFunctionAnalysis<zanna::analysis::IntRangeInfo>(
        "int-ranges", [](Module &, Function &fn) { return zanna::analysis::computeIntRanges(fn); });
    return registry;
}

void runCheckOpt(Module &module) {
    auto registry = makeRegistry();
    il::transform::AnalysisManager manager(module, registry);
    il::transform::CheckOpt pass;
    auto preserved = pass.run(module.functions.front(), manager);
    manager.invalidateAfterFunctionPass(preserved, module.functions.front());
}

const char *const kCountedLoop = R"(il 0.3.0
func @main() -> i64 {
entry:
  br loop(0, 0)
loop(%sum: i64, %i: i64):
  %done = scmp_ge %i, 1000
  cbr %done, exit(%sum), body(%sum, %i)
body(%sum0: i64, %i0: i64):
  %raw = iadd.ovf %sum0, %i0
  %new_sum = and %raw, 255
  %next_i = iadd.ovf %i0, 1
  br loop(%new_sum, %next_i)
exit(%res: i64):
  ret %res
}
)";

// Rotated (do-while) shape: body self-loops with the latch compare at the end,
// matching what loop-rotate produces at O2.
const char *const kRotatedLoop = R"(il 0.3.0
func @main() -> i64 {
entry:
  br body(0, 1)
body(%sum0: i64, %i0: i64):
  %q = sdiv.chk0 %i0, 3
  %raw = iadd.ovf %sum0, %q
  %new_sum = and %raw, 255
  %next_i = iadd.ovf %i0, 1
  %done = scmp_ge %next_i, 1000
  cbr %done, exit(%new_sum), body(%new_sum, %next_i)
exit(%res: i64):
  ret %res
}
)";

TEST(IntRangeAnalysis, CountedLoopGuardBoundsInductionVariable) {
    Module module = parseModule(kCountedLoop);
    Function &fn = module.functions.front();
    auto info = zanna::analysis::computeIntRanges(fn);

    const auto *body = info.entryFor("body");
    ASSERT_TRUE(body != nullptr);
    auto it = body->find(paramId(fn, "body", 1));
    ASSERT_TRUE(it != body->end());
    ASSERT_TRUE(it->second.lower.has_value());
    ASSERT_TRUE(it->second.upper.has_value());
    EXPECT_EQ(*it->second.lower, 0);
    EXPECT_EQ(*it->second.upper, 999);
}

TEST(IntRangeAnalysis, UnboundedAccumulatorNeverGetsFiniteBound) {
    // %sum accumulates %i without a mask: its true range grows without bound,
    // so a finite upper bound at the loop header would be UNSOUND.
    Module module = parseModule(R"(il 0.3.0
func @main() -> i64 {
entry:
  br loop(0, 0)
loop(%sum: i64, %i: i64):
  %done = scmp_ge %i, 1000
  cbr %done, exit(%sum), body(%sum, %i)
body(%sum0: i64, %i0: i64):
  %new_sum = iadd.ovf %sum0, %i0
  %next_i = iadd.ovf %i0, 1
  br loop(%new_sum, %next_i)
exit(%res: i64):
  ret %res
}
)");
    Function &fn = module.functions.front();
    auto info = zanna::analysis::computeIntRanges(fn);
    const auto *loop = info.entryFor("loop");
    ASSERT_TRUE(loop != nullptr);
    auto it = loop->find(paramId(fn, "loop", 0));
    if (it != loop->end())
        EXPECT_FALSE(it->second.upper.has_value());
}

TEST(IntRangeAnalysis, RotatedSelfLoopKeepsFullInductionBounds) {
    Module module = parseModule(kRotatedLoop);
    Function &fn = module.functions.front();
    auto info = zanna::analysis::computeIntRanges(fn);
    const auto *body = info.entryFor("body");
    ASSERT_TRUE(body != nullptr);
    auto it = body->find(paramId(fn, "body", 1));
    ASSERT_TRUE(it != body->end());
    ASSERT_TRUE(it->second.lower.has_value());
    ASSERT_TRUE(it->second.upper.has_value());
    EXPECT_EQ(*it->second.lower, 1);
    EXPECT_EQ(*it->second.upper, 999);
}

TEST(IntRangeAnalysis, ReachesBlocksBeyondLegacySweepLimit) {
    std::ostringstream text;
    text << "il 0.3.0\nfunc @main() -> i64 {\nentry:\n  br b0\n";
    constexpr unsigned kChainLength = 32;
    for (unsigned i = 0; i < kChainLength; ++i) {
        text << "b" << i << ":\n";
        if (i + 1 == kChainLength)
            text << "  ret 0\n";
        else
            text << "  br b" << (i + 1) << "\n";
    }
    text << "}\n";

    Module module = parseModule(text.str());
    auto info = zanna::analysis::computeIntRanges(module.functions.front());
    EXPECT_TRUE(info.entryFor("b31") != nullptr);
}

// A rotated loop whose induction variable travels through a chain of blocks
// (block parameters at every hop) before the increment and the latch compare.
// The header is the widening point, so its upper bound is stripped during the
// ascent and only narrowing recovers it; narrowing carries a fact one edge per
// sweep, so the use four blocks past the header needs at least four sweeps.
// This is the shape inlining produces when it splits a caller block into a
// chain of continuation blocks.
constexpr const char *kChainedLoop = R"(il 0.3.0
func @main() -> i64 {
entry:
  br head(0, 0)
head(%sum: i64, %i: i64):
  %a = and %sum, 255
  br h1(%a, %i)
h1(%s1: i64, %i1: i64):
  br h2(%s1, %i1)
h2(%s2: i64, %i2: i64):
  br h3(%s2, %i2)
h3(%s3: i64, %i3: i64):
  br h4(%s3, %i3)
h4(%s4: i64, %i4: i64):
  %next_i = iadd.ovf %i4, 1
  %done = scmp_ge %next_i, 1000
  cbr %done, exit(%s4), head(%s4, %next_i)
exit(%res: i64):
  ret %res
}
)";

TEST(IntRangeAnalysis, RecoveredLoopBoundReachesUsesManyBlocksPastHeader) {
    Module module = parseModule(kChainedLoop);
    Function &fn = module.functions.front();
    auto info = zanna::analysis::computeIntRanges(fn);

    const auto *h4 = info.entryFor("h4");
    ASSERT_TRUE(h4 != nullptr);
    auto it = h4->find(paramId(fn, "h4", 1));
    ASSERT_TRUE(it != h4->end());
    ASSERT_TRUE(it->second.lower.has_value());
    ASSERT_TRUE(it->second.upper.has_value());
    EXPECT_EQ(*it->second.lower, 0);
    EXPECT_EQ(*it->second.upper, 999);
}

TEST(IntRangeAnalysis, VerifierAcceptsDemotedAddManyBlocksPastHeader) {
    // CheckOpt demotes `iadd.ovf %i4, 1` to a plain `add` on the strength of
    // the range above; the verifier must re-prove it from the same analysis
    // even though the add sits several blocks past the loop header (before
    // the narrowing budget followed the block count, this failed after
    // inlining lengthened such chains).
    std::string text = kChainedLoop;
    const std::string checked = "%next_i = iadd.ovf %i4, 1";
    const auto at = text.find(checked);
    ASSERT_TRUE(at != std::string::npos);
    text.replace(at, checked.size(), "%next_i = add %i4, 1");

    Module module = parseModule(text);
    auto verified = il::verify::Verifier::verify(module);
    EXPECT_TRUE(static_cast<bool>(verified));
}

TEST(CheckOptRanges, DemotesGuardedLoopArithmetic) {
    Module module = parseModule(kCountedLoop);
    runCheckOpt(module);
    const Function &fn = module.functions.front();
    // %next_i (i0 <= 999) and %raw (both operands bounded) both demote.
    EXPECT_EQ(countOpcode(fn, Opcode::IAddOvf), 0u);
    // The demoted module must still verify (shared prover, ADR 0026).
    auto verified = il::verify::Verifier::verify(module);
    EXPECT_TRUE(static_cast<bool>(verified));
}

TEST(CheckOptRanges, DemotesRotatedLoopDivAndArithmetic) {
    Module module = parseModule(kRotatedLoop);
    runCheckOpt(module);
    const Function &fn = module.functions.front();
    EXPECT_EQ(countOpcode(fn, Opcode::SDivChk0), 0u);
    EXPECT_EQ(countOpcode(fn, Opcode::IAddOvf), 0u);
    auto verified = il::verify::Verifier::verify(module);
    EXPECT_TRUE(static_cast<bool>(verified));
}

TEST(CheckOptRanges, DeletesProvablyInBoundsIdxChk) {
    Module module = parseModule(R"(il 0.3.0
func @main() -> i64 {
entry:
  br loop(0, 0)
loop(%sum: i64, %j: i64):
  %end = scmp_ge %j, 1024
  cbr %end, exit(%sum), body(%sum, %j)
body(%sum0: i64, %j0: i64):
  %jj = idx.chk %j0, 0, 1024
  %s = iadd.ovf %sum0, %jj
  %m = and %s, 255
  %next_j = iadd.ovf %j0, 1
  br loop(%m, %next_j)
exit(%res: i64):
  ret %res
}
)");
    runCheckOpt(module);
    EXPECT_EQ(countOpcode(module.functions.front(), Opcode::IdxChk), 0u);
    auto verified = il::verify::Verifier::verify(module);
    EXPECT_TRUE(static_cast<bool>(verified));
}

TEST(CheckOptRanges, KeepsIdxChkWhenBoundsTooTight) {
    // Index reaches 1023 but the check demands < 1000: deleting it would drop
    // a real trap.
    Module module = parseModule(R"(il 0.3.0
func @main() -> i64 {
entry:
  br loop(0, 0)
loop(%sum: i64, %j: i64):
  %end = scmp_ge %j, 1024
  cbr %end, exit(%sum), body(%sum, %j)
body(%sum0: i64, %j0: i64):
  %jj = idx.chk %j0, 0, 1000
  %s = iadd.ovf %sum0, %jj
  %m = and %s, 255
  %next_j = iadd.ovf %j0, 1
  br loop(%m, %next_j)
exit(%res: i64):
  ret %res
}
)");
    runCheckOpt(module);
    EXPECT_EQ(countOpcode(module.functions.front(), Opcode::IdxChk), 1u);
}

TEST(CheckOptRanges, DemotesBranchGuardedDivisor) {
    Module module = parseModule(R"(il 0.3.0
func @main(%x: i64, %d: i64) -> i64 {
entry(%x: i64, %d: i64):
  %pos = scmp_gt %d, 0
  cbr %pos, divide(%x, %d), fallback(%x)
divide(%x0: i64, %d0: i64):
  %q = sdiv.chk0 %x0, %d0
  ret %q
fallback(%x1: i64):
  ret %x1
}
)");
    runCheckOpt(module);
    const Function &fn = module.functions.front();
    EXPECT_EQ(countOpcode(fn, Opcode::SDivChk0), 0u);
    EXPECT_EQ(countOpcode(fn, Opcode::SDiv), 1u);
    auto verified = il::verify::Verifier::verify(module);
    EXPECT_TRUE(static_cast<bool>(verified));
}

TEST(CheckOptRanges, KeepsUnguardedVariableDivisor) {
    Module module = parseModule(R"(il 0.3.0
func @main(%x: i64, %d: i64) -> i64 {
entry(%x: i64, %d: i64):
  %q = sdiv.chk0 %x, %d
  ret %q
}
)");
    runCheckOpt(module);
    EXPECT_EQ(countOpcode(module.functions.front(), Opcode::SDivChk0), 1u);
}

TEST(CheckOptRanges, SelectUnionOfBoundedArmsDemotesAdd) {
    // The select's range is the union of its arm ranges: [4096,4096] and
    // [0,8191] join to [0,8191], which proves the add cannot overflow. This is
    // the fact IfConvert relies on when it folds a clamping branch to select.
    Module module = parseModule(R"(il 0.3.0
func @main(%c: i1, %x: i64) -> i64 {
entry(%c: i1, %x: i64):
  %m = and %x, 8191
  %v = select i64, %c, 4096, %m
  %s = iadd.ovf %v, 1
  ret %s
}
)");
    runCheckOpt(module);
    EXPECT_EQ(countOpcode(module.functions.front(), Opcode::IAddOvf), 0u);
    auto verified = il::verify::Verifier::verify(module);
    EXPECT_TRUE(static_cast<bool>(verified));
}

TEST(CheckOptRanges, SelectWithUnboundedArmKeepsCheckedAdd) {
    // One arm unbounded -> the union is unbounded -> no demotion.
    Module module = parseModule(R"(il 0.3.0
func @main(%c: i1, %x: i64) -> i64 {
entry(%c: i1, %x: i64):
  %v = select i64, %c, %x, 4096
  %s = iadd.ovf %v, 1
  ret %s
}
)");
    runCheckOpt(module);
    EXPECT_EQ(countOpcode(module.functions.front(), Opcode::IAddOvf), 1u);
}

TEST(VerifierRanges, RejectsUnprovablePlainAdd) {
    // Plain add of two unbounded params must still be rejected: the analysis
    // has no facts, so neither the local nor the global prover accepts it.
    Module module = parseModule(R"(il 0.3.0
func @main(%a: i64, %b: i64) -> i64 {
entry(%a: i64, %b: i64):
  %s = add %a, %b
  ret %s
}
)");
    auto verified = il::verify::Verifier::verify(module);
    EXPECT_FALSE(static_cast<bool>(verified));
}

} // namespace

int main(int argc, char **argv) {
    zanna_test::init(&argc, argv);
    return zanna_test::run_all_tests();
}
