//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/codegen/test_aarch64_global_liveness.cpp
// Purpose: Verify that proper backward-dataflow liveness eliminates spurious
//          block-end spills for vregs that are not genuinely live at block
//          boundaries.
//
// Background:
//   The previous (conservative) computeLiveOutSets() added every vreg
//   referenced in ANY instruction of a successor block to liveOut, without
//   distinguishing USE operands from DEF operands.  For a loop block that is
//   its own successor, this meant every vreg defined inside the loop ended up
//   in liveOut[loop], triggering a block-end spill even if the vreg was never
//   needed in any successor.
//
//   The fix replaces computeLiveOutSets() with a proper backward dataflow:
//     gen[B]  = vregs used in B before any def of that vreg in B
//     kill[B] = vregs defined in B
//     liveIn[B]  = gen[B] ∪ (liveOut[B] \ kill[B])
//     liveOut[B] = ∪_{S ∈ succs(B)} liveIn[S]
//   iterated to fixed point.
//
//   After the fix, for SSA-like MIR (each vreg defined at most once), any
//   vreg defined inside a loop block is in kill[loop], so the back-edge
//   contribution liveOut[loop] ∩ {loop-defined vregs} = ∅.  Only vregs
//   that are truly needed by a successor (e.g. phi slot loads in exit blocks)
//   remain in liveOut.
//
// Interaction with Priority-2D (PhiStoreGPR) fix:
//   The 2D fix cleared the dirty flag for phi-arg vregs, eliminating their
//   redundant block-end spill.  The 2E fix (this test) eliminates block-end
//   spills for ALL other vregs in the loop that are not genuinely live-out:
//   phi-loaded inputs (%i, %sum), comparison temporaries (%done), and
//   constant materializations.
//
// Tests:
//   1. SinglePhiLoopMinimalSpills  - iota100 loop; str x count <= 2
//   2. TwoPhiLoopMinimalSpills     - loop_sum; str x count <= 6
//   3. IntermediateTempNotSpilled  - loop with %sq intermediate; str x <= 6
//   4. ConstantMaterNotSpilled     - constant vregs not block-end spilled;
//                                    str x <= 3
//
// Before-fix / after-fix measurements (after the Priority-2D fix is active):
//   Test 1: before = 5  str x, after <= 2
//   Test 2: before = 11 str x, after <= 6
//   Test 3: before ~= 8 str x, after <= 6
//   Test 4: before ~= 5 str x, after <= 3
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
// Test 1: Single GPR phi loop — all spurious block-end spills eliminated.
// ---------------------------------------------------------------------------
//
// IL: func @iota100() -> i64 { ... }  (same as phi_coalescer_single)
//
// After 2D fix: 5 str x remain (1 entry phi store + 4 spurious block-end
//   spills for phi_i, const-1, const-100, %done).
// After 2E fix: liveOut[loop] = ∅ → 0 block-end spills; only entry phi
//   store remains → 1 str x.
//
// Bound: <= 2 (fails with 5 before fix, passes with 1 after fix).
//
TEST(AArch64GlobalLiveness, SinglePhiLoopMinimalSpills) {
    const std::string in = testOut("global_liveness_single.il");
    const std::string out = testOut("global_liveness_single.s");

    const std::string il = "il 0.1\n"
                           "func @iota100_lv() -> i64 {\n"
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
    const char *argv[] = {in.c_str(), "-S", out.c_str(), "-O1"};
    ASSERT_EQ(cmd_codegen_arm64(4, const_cast<char **>(argv)), 0);

    const std::string asmText = readFile(out);
    const int strCount = countSubstr(asmText, "str x");

    // Before fix: 5 str x (1 entry phi + 4 block-end spills).
    // After fix:  <= 2 str x (only entry phi store; loop block-end = empty).
    if (strCount > 2) {
        std::cerr << "Expected at most 2 'str x' (global liveness); got " << strCount
                  << "\nAssembly:\n"
                  << asmText << "\n";
    }
    EXPECT_TRUE(strCount <= 2);
}

// ---------------------------------------------------------------------------
// Test 2: Two GPR phis — all spurious block-end spills eliminated.
// ---------------------------------------------------------------------------
//
// IL: func @loop_sum() -> i64 { ... }  (same as phi_coalescer_two)
//
// After 2D fix: 11 str x (entry×2, phi-stores×3, block-end: %i, %sum,
//   %done, const-1, const-10).
// After 2E fix: block-end = ∅ → only entry×2 + phi-stores×3 remain.
//   Phi stores may be combined by peephole (stp) reducing str x further.
//
// Bound: <= 6 (fails with 11 before fix, passes with ~5 after fix).
//
TEST(AArch64GlobalLiveness, TwoPhiLoopMinimalSpills) {
    const std::string in = testOut("global_liveness_two.il");
    const std::string out = testOut("global_liveness_two.s");

    const std::string il = "il 0.1\n"
                           "func @loop_sum_lv() -> i64 {\n"
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
    const char *argv[] = {in.c_str(), "-S", out.c_str(), "-O1"};
    ASSERT_EQ(cmd_codegen_arm64(4, const_cast<char **>(argv)), 0);

    const std::string asmText = readFile(out);
    const int strCount = countSubstr(asmText, "str x");

    // Before fix: 11 str x (5 spurious block-end spills + 6 phi/entry stores).
    // After fix:  <= 6 str x (block-end spills eliminated; entry + phi stores remain).
    if (strCount > 6) {
        std::cerr << "Expected at most 6 'str x' (global liveness two phi); got " << strCount
                  << "\nAssembly:\n"
                  << asmText << "\n";
    }
    EXPECT_TRUE(strCount <= 6);
}

// ---------------------------------------------------------------------------
// Test 3: Intermediate temporary (%sq = mul %i, %i) — not a phi arg,
//         not live-out, must not appear in block-end spills.
// ---------------------------------------------------------------------------
//
// func @sum_sq() -> i64:
//   loop(%i, %sum):
//     %sq = mul %i, %i         ← NOT passed to any successor
//     %ns = add %sum, %sq
//     %ni = add %i, 1
//     %done = icmp_eq %ni, 10
//     cbr %done, exit(%ns), loop(%ni, %ns)
//
// Before fix: %sq, %done, %i, %sum all block-end spilled → ~8 str x.
// After fix:  liveOut[loop] = ∅ → only entry×2 + phi-stores remain → <= 5.
//
// Bound: <= 5 (fails with ~8 before fix, passes with ~3-4 after fix).
//
TEST(AArch64GlobalLiveness, IntermediateTempNotSpilled) {
    const std::string in = testOut("global_liveness_sq.il");
    const std::string out = testOut("global_liveness_sq.s");

    const std::string il = "il 0.1\n"
                           "func @sum_sq() -> i64 {\n"
                           "entry:\n"
                           "  br loop(0, 0)\n"
                           "loop(%i:i64, %sum:i64):\n"
                           "  %sq   = imul.ovf %i, %i\n"
                           "  %ns   = iadd.ovf %sum, %sq\n"
                           "  %ni   = iadd.ovf %i, 1\n"
                           "  %done = icmp_eq %ni, 10\n"
                           "  cbr %done, exit(%ns), loop(%ni, %ns)\n"
                           "exit(%r:i64):\n"
                           "  ret %r\n"
                           "}\n";

    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-S", out.c_str(), "-O1"};
    ASSERT_EQ(cmd_codegen_arm64(4, const_cast<char **>(argv)), 0);

    const std::string asmText = readFile(out);
    const int strCount = countSubstr(asmText, "str x");

    // Before fix: ~8 str x (intermediate %sq, phi-loaded inputs, %done, etc.).
    // After fix: only phi-stores, entry initializers, and callee-saved push
    // remain (no %sq spill).  The callee-saved push (str x20) adds 1 str x
    // that the original bound of 5 didn't account for.
    if (strCount > 6) {
        std::cerr << "Expected at most 6 'str x' (intermediate temp not spilled); got " << strCount
                  << "\nAssembly:\n"
                  << asmText << "\n";
    }
    EXPECT_TRUE(strCount <= 6);
}

// ---------------------------------------------------------------------------
// Test 4: Constant materializers — MovRI vregs not block-end spilled.
// ---------------------------------------------------------------------------
//
// A loop that explicitly compares against a large constant (materialised
// via movz/movk into a dedicated vreg) should not block-end spill that
// constant vreg: it is not needed by any successor (easily re-materialised).
//
// func @const_loop() -> i64:
//   loop(%i:i64):
//     %limit = 50        ← constant materialisation
//     %next  = add %i, 1
//     %done  = icmp_eq %next, %limit
//     cbr %done, exit(%next), loop(%next)
//
// Before fix: %limit vreg (MovRI) is dirty and in liveOut[loop] → spilled.
// After fix:  %limit is defined in loop → in kill[loop] → not in liveOut.
//
// Bound: <= 3 (fails with ~5 before fix, passes with ~1-2 after fix).
//
TEST(AArch64GlobalLiveness, ConstantMaterNotSpilled) {
    const std::string in = testOut("global_liveness_const.il");
    const std::string out = testOut("global_liveness_const.s");

    // Use icmp_eq with a literal to force a constant materialisation in the
    // loop body.  The IL codegen materialises the RHS of icmp as a vreg when
    // it doesn't fit a 12-bit immediate (here 50 fits, but the MIR lowering
    // still materialises it for the comparison register).
    const std::string il = "il 0.1\n"
                           "func @const_loop() -> i64 {\n"
                           "entry:\n"
                           "  br loop(0)\n"
                           "loop(%i:i64):\n"
                           "  %next  = iadd.ovf %i, 1\n"
                           "  %limit = iadd.ovf 0, 50\n"
                           "  %done  = icmp_eq %next, %limit\n"
                           "  cbr %done, exit(%next), loop(%next)\n"
                           "exit(%r:i64):\n"
                           "  ret %r\n"
                           "}\n";

    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-S", out.c_str(), "-O1"};
    ASSERT_EQ(cmd_codegen_arm64(4, const_cast<char **>(argv)), 0);

    const std::string asmText = readFile(out);
    const int strCount = countSubstr(asmText, "str x");

    // Before fix: %limit, %done, phi_i are all block-end spilled → ~5 str x.
    // After fix:  liveOut[loop] = ∅ → no block-end spills → <= 3 str x.
    if (strCount > 3) {
        std::cerr << "Expected at most 3 'str x' (constant not block-end spilled); got " << strCount
                  << "\nAssembly:\n"
                  << asmText << "\n";
    }
    EXPECT_TRUE(strCount <= 3);
}

int main(int argc, char **argv) {
    // This test pins the retired block-local allocation path (frame slots,
    // PhiStore edges) until Phase 3 C7 deletes it; see ADR 0338.
    setenv("ZANNA_LOCAL_RA", "1", 1);
    zanna_test::init(&argc, &argv);
    return zanna_test::run_all_tests();
}
