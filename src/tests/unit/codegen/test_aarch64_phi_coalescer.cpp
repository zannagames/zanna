//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/codegen/test_aarch64_phi_coalescer.cpp
// Purpose: Verify that phi-edge copies suppress redundant block-end spills.
//
// Background:
//   When TerminatorLowering emits phi-edge stores before a branch, the
//   register allocator's block-end pass can see the source vreg in
//   liveOutGPR_ (because the loop block is its own successor and its
//   instructions reference the vreg). If the vreg is still "dirty" at
//   block-end, the allocator emits a *second* store to a separate regalloc
//   spill slot — a wasted store every loop iteration.
//
//   The fix: TerminatorLowering emits PhiStoreGPR/PhiStoreFPR pseudo-opcodes.
//   The allocator processes these identically to StrRegFpImm/StrFprFpImm
//   but also clears the source vreg's dirty flag, suppressing the redundant
//   block-end spill of the same value to a separate slot.
//
// Note on peephole interaction:
//   Adjacent PhiStoreGPR instructions for the same vreg (e.g., both edges of
//   a cbr passing the same value) are often combined into a single STP by the
//   peephole optimizer. The "str x" count checks below focus on non-STP stores
//   and are set to bounds that fail before the fix and pass after it.
//
// Tests:
//   1. SinglePhiLoop       - 1 GPR phi, loop back-edge: str x count <= 5
//   2. TwoPhiLoop          - 2 GPR phis: str x count <= 11
//   3. FPRPhiLoop          - 1 FPR phi: compilation succeeds, fadd + cbnz present
//   4. LoopStructurePreserved - loop assembly structure is correct after fix
//
//===----------------------------------------------------------------------===//

#include "tests/TestHarness.hpp"

#include <cstdlib>

#include "tests/common/PosixCompat.h" // setenv on every host (self-guarded)
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "tools/zanna/cmd_codegen_arm64.hpp"

using namespace zanna::tools::ilc;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

static std::string testOut(const std::string &name) {
    namespace fs = std::filesystem;
    const fs::path dir{"build/test-out/arm64"};
    fs::create_directories(dir);
    return (dir / name).string();
}

static void writeFile(const std::string &path, const std::string &text) {
    std::ofstream ofs(path);
    ASSERT_TRUE(static_cast<bool>(ofs));
    ofs << text;
}

static std::string readFile(const std::string &path) {
    std::ifstream ifs(path);
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

/// Count occurrences of a literal substring in a string.
static int countSubstr(const std::string &text, const std::string &needle) {
    int n = 0;
    std::size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

} // namespace

// ---------------------------------------------------------------------------
// Test 1: Single GPR phi in a loop — redundant block-end spill eliminated.
// ---------------------------------------------------------------------------
//
// IL:  func @iota100() -> i64 {
//        entry:          br loop(0)
//        loop(%i:i64):   %next = add %i, 1; %done = icmp_eq %next, 100
//                        cbr %done, exit(%next), loop(%next)
//        exit(%r:i64):   ret %r
//      }
//
// Before fix (StrRegFpImm): %next dirty after phi stores, block-end emits
//   an extra str for %next → str x count = 6.
// After fix (PhiStoreGPR): dirty cleared, no redundant block-end store
//   → str x count = 5.
//
// Bound: <= 5 (fails with 6 before fix, passes with 5 after fix).
//
TEST(AArch64PhiCoalescer, SinglePhiLoop) {
    const std::string in = testOut("phi_coalescer_single.il");
    const std::string out = testOut("phi_coalescer_single.s");

    const std::string il = "il 0.1\n"
                           "func @iota100() -> i64 {\n"
                           "entry:\n"
                           "  br loop(0)\n"
                           "loop(%i:i64):\n"
                           "  %next = iadd.ovf %i, 1\n"
                           "  %done = icmp_eq %next, 100\n"
                           "  cbr %done, exit(%next), loop(%next)\n"
                           "exit(%r:i64):\n"
                           "  ret %r\n"
                           "}\n";

    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-S", out.c_str()};
    ASSERT_EQ(cmd_codegen_arm64(3, const_cast<char **>(argv)), 0);

    const std::string asmText = readFile(out);
    const int strCount = countSubstr(asmText, "str x");

    // Before fix: block-end emits extra str for phi arg %next → 6 str x.
    // After fix:  PhiStoreGPR clears dirty; no block-end store for %next → 5.
    if (strCount > 5) {
        std::cerr << "Expected at most 5 'str x' (phi coalescer fix); got " << strCount
                  << "\nAssembly:\n"
                  << asmText << "\n";
    }
    EXPECT_TRUE(strCount <= 5);
}

// ---------------------------------------------------------------------------
// Test 2: Two GPR phis in a loop — two redundant block-end spills eliminated.
// ---------------------------------------------------------------------------
//
// loop(%i:i64, %sum:i64):
//   %new_sum = add %sum, %i  |  %next_i = add %i, 1
//   cbr (%next_i == 10), exit(%new_sum), loop(%next_i, %new_sum)
//
// Before fix: block-end emits extra str for %next_i and %new_sum (2 extra).
// After fix:  PhiStoreGPR clears dirty for both → 0 extra block-end stores.
//
// Measured counts (after fix, including callee-saved push and block-end spills
// for non-phi-arg vregs): 11.
// Bound: <= 11 (fails with 13 before fix, passes with 11 after fix).
//
TEST(AArch64PhiCoalescer, TwoPhiLoop) {
    const std::string in = testOut("phi_coalescer_two.il");
    const std::string out = testOut("phi_coalescer_two.s");

    const std::string il = "il 0.1\n"
                           "func @loop_sum() -> i64 {\n"
                           "entry:\n"
                           "  br loop(0, 0)\n"
                           "loop(%i:i64, %sum:i64):\n"
                           "  %new_sum = iadd.ovf %sum, %i\n"
                           "  %next_i  = iadd.ovf %i, 1\n"
                           "  %done    = icmp_eq %next_i, 10\n"
                           "  cbr %done, exit(%new_sum), loop(%next_i, %new_sum)\n"
                           "exit(%r:i64):\n"
                           "  ret %r\n"
                           "}\n";

    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-S", out.c_str()};
    ASSERT_EQ(cmd_codegen_arm64(3, const_cast<char **>(argv)), 0);

    const std::string asmText = readFile(out);
    const int strCount = countSubstr(asmText, "str x");

    // Before fix: 2 extra block-end stores for phi arg vregs → ~13 str x.
    // After fix:  PhiStoreGPR removes those 2 extra stores → 11 str x.
    if (strCount > 11) {
        std::cerr << "Expected at most 11 'str x'; got " << strCount << "\nAssembly:\n"
                  << asmText << "\n";
    }
    EXPECT_TRUE(strCount <= 11);
}

// ---------------------------------------------------------------------------
// Test 3: FPR phi in loop — PhiStoreFPR handled, compilation succeeds.
// ---------------------------------------------------------------------------
TEST(AArch64PhiCoalescer, FPRPhiLoop) {
    const std::string in = testOut("phi_coalescer_fpr.il");
    const std::string out = testOut("phi_coalescer_fpr.s");

    const std::string il = "il 0.1\n"
                           "func @fp_accum() -> f64 {\n"
                           "entry:\n"
                           "  br loop(0.0)\n"
                           "loop(%acc:f64):\n"
                           "  %one = fadd %acc, 1.0\n"
                           "  %done = fcmp_eq %one, 10.0\n"
                           "  cbr %done, exit(%one), loop(%one)\n"
                           "exit(%r:f64):\n"
                           "  ret %r\n"
                           "}\n";

    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-S", out.c_str()};
    // Must compile without crashing: PhiStoreFPR must be handled in RA.
    ASSERT_EQ(cmd_codegen_arm64(3, const_cast<char **>(argv)), 0);

    const std::string asmText = readFile(out);
    // FP addition must be present.
    EXPECT_NE(asmText.find("fadd"), std::string::npos);
    // Conditional branch (cbnz from peephole, or b.ne without peephole).
    const bool hasCondBr =
        asmText.find("cbnz") != std::string::npos || asmText.find("b.") != std::string::npos;
    EXPECT_TRUE(hasCondBr);
}

// ---------------------------------------------------------------------------
// Test 4: Loop assembly structure must be preserved after the fix.
// ---------------------------------------------------------------------------
TEST(AArch64PhiCoalescer, LoopStructurePreserved) {
    const std::string in = testOut("phi_coalescer_correct.il");
    const std::string out = testOut("phi_coalescer_correct.s");

    const std::string il = "il 0.1\n"
                           "func @count5() -> i64 {\n"
                           "entry:\n"
                           "  br loop(0)\n"
                           "loop(%i:i64):\n"
                           "  %next = iadd.ovf %i, 1\n"
                           "  %done = icmp_eq %next, 5\n"
                           "  cbr %done, exit(%next), loop(%next)\n"
                           "exit(%r:i64):\n"
                           "  ret %r\n"
                           "}\n";

    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-S", out.c_str()};
    ASSERT_EQ(cmd_codegen_arm64(3, const_cast<char **>(argv)), 0);

    const std::string asmText = readFile(out);

    // Counter increment must be present.
    EXPECT_NE(asmText.find("adds x"), std::string::npos);
    // Compare must be present.
    EXPECT_NE(asmText.find("cmp x"), std::string::npos);
    // Load from phi slot at loop entry must be present.
    EXPECT_NE(asmText.find("ldr x"), std::string::npos);
    // Loop back-edge must be present — either an unconditional "b Lloop" or
    // a conditional branch targeting Lloop (the peephole may fuse cmp+branch
    // or invert the branch direction, eliminating the explicit "b Lloop").
    const bool hasLoopBackEdge =
        asmText.find("Lloop") != std::string::npos ||      // any reference to loop label
        asmText.find("Lloop_body") != std::string::npos || // after phi-spill elim
        asmText.find("cbnz") != std::string::npos;         // fused cmp+branch
    EXPECT_TRUE(hasLoopBackEdge);
    // Conditional loop exit must be present (cbnz from peephole or b.ne).
    const bool hasCondExit =
        asmText.find("cbnz") != std::string::npos || asmText.find("b.") != std::string::npos;
    EXPECT_TRUE(hasCondExit);
}

int main(int argc, char **argv) {
    // This test pins the retired block-local allocation path (frame slots,
    // PhiStore edges) until Phase 3 C7 deletes it; see ADR 0338.
    setenv("ZANNA_LOCAL_RA", "1", 1);
    zanna_test::init(&argc, &argv);
    return zanna_test::run_all_tests();
}
