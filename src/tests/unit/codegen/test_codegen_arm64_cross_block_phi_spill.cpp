//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/codegen/test_codegen_arm64_cross_block_phi_spill.cpp
// Purpose: Verify phi node handling with register pressure across blocks.
// Key invariants: Values crossing blocks via phi may require spilling.
// Ownership/Lifetime: To be documented.
// Links: docs/internals/architecture.md
//
//===----------------------------------------------------------------------===//
#include "tests/TestHarness.hpp"

#include <cstdlib>

#include "tests/common/PosixCompat.h" // setenv on every host (self-guarded)
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "tools/zanna/cmd_codegen_arm64.hpp"

using namespace zanna::tools::ilc;

static std::string outPath(const std::string &name) {
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

// Test 1: Simple if-else with phi
TEST(Arm64CrossBlockPhi, SimpleIfElsePhi) {
    const std::string in = outPath("arm64_phi_simple.il");
    const std::string out = outPath("arm64_phi_simple.s");
    const std::string il = "il 0.1\n"
                           "func @max(%a:i64, %b:i64) -> i64 {\n"
                           "entry(%a:i64, %b:i64):\n"
                           "  %c = scmp_gt %a, %b\n"
                           "  cbr %c, ta, tb\n"
                           "ta:\n"
                           "  br join(%a)\n"
                           "tb:\n"
                           "  br join(%b)\n"
                           "join(%r:i64):\n"
                           "  ret %r\n"
                           "}\n";
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-S", out.c_str()};
    ASSERT_EQ(cmd_codegen_arm64(3, const_cast<char **>(argv)), 0);
    const std::string asmText = readFile(out);
    // Should have compare and conditional branch
    EXPECT_NE(asmText.find("cmp x"), std::string::npos);
    EXPECT_NE(asmText.find("b."), std::string::npos);
}

// Test 2: Loop with phi and high register pressure
TEST(Arm64CrossBlockPhi, LoopWithPressure) {
    const std::string in = outPath("arm64_phi_loop_pressure.il");
    const std::string out = outPath("arm64_phi_loop_pressure.s");
    const std::string il = "il 0.1\n"
                           "func @loop_sum(%n:i64, %a:i64, %b:i64, %c:i64) -> i64 {\n"
                           "entry(%n:i64, %a:i64, %b:i64, %c:i64):\n"
                           "  %x = iadd.ovf %a, %b\n"
                           "  %y = imul.ovf %b, %c\n"
                           "  %z = isub.ovf %a, %c\n"
                           "  br loop(0, 0)\n"
                           "loop(%i:i64, %sum:i64):\n"
                           "  %t1 = iadd.ovf %sum, %x\n"
                           "  %t2 = iadd.ovf %t1, %y\n"
                           "  %t3 = iadd.ovf %t2, %z\n"
                           "  %next_i = iadd.ovf %i, 1\n"
                           "  %done = icmp_eq %next_i, %n\n"
                           "  cbr %done, exit(%t3), loop(%next_i, %t3)\n"
                           "exit(%result:i64):\n"
                           "  ret %result\n"
                           "}\n";
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-S", out.c_str()};
    ASSERT_EQ(cmd_codegen_arm64(3, const_cast<char **>(argv)), 0);
    const std::string asmText = readFile(out);
    // Should have loop structure
    EXPECT_NE(asmText.find("adds x"), std::string::npos);
}

// Test 3: Multiple phis in join block
TEST(Arm64CrossBlockPhi, MultiplePhis) {
    const std::string in = outPath("arm64_phi_multi.il");
    const std::string out = outPath("arm64_phi_multi.s");
    const std::string il = "il 0.1\n"
                           "func @swap_if(%c:i64, %a:i64, %b:i64) -> i64 {\n"
                           "entry(%c:i64, %a:i64, %b:i64):\n"
                           "  %cond = icmp_ne %c, 0\n"
                           "  cbr %cond, swap, noswap\n"
                           "swap:\n"
                           "  br join(%b, %a)\n"
                           "noswap:\n"
                           "  br join(%a, %b)\n"
                           "join(%x:i64, %y:i64):\n"
                           "  %r = iadd.ovf %x, %y\n"
                           "  ret %r\n"
                           "}\n";
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-S", out.c_str()};
    ASSERT_EQ(cmd_codegen_arm64(3, const_cast<char **>(argv)), 0);
    const std::string asmText = readFile(out);
    // Should compile and have branches
    EXPECT_NE(asmText.find("b"), std::string::npos);
}

// Test 4: Phi with FP values
TEST(Arm64CrossBlockPhi, FPPhi) {
    const std::string in = outPath("arm64_phi_fp.il");
    const std::string out = outPath("arm64_phi_fp.s");
    const std::string il = "il 0.1\n"
                           "func @fp_max(%a:f64, %b:f64) -> f64 {\n"
                           "entry(%a:f64, %b:f64):\n"
                           "  %c = fcmp_gt %a, %b\n"
                           "  cbr %c, ta, tb\n"
                           "ta:\n"
                           "  br join(%a)\n"
                           "tb:\n"
                           "  br join(%b)\n"
                           "join(%r:f64):\n"
                           "  ret %r\n"
                           "}\n";
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-S", out.c_str()};
    ASSERT_EQ(cmd_codegen_arm64(3, const_cast<char **>(argv)), 0);
    const std::string asmText = readFile(out);
    // Should have fcmp
    EXPECT_NE(asmText.find("fcmp d"), std::string::npos);
}

// Test 5: Nested loop with phi
TEST(Arm64CrossBlockPhi, NestedLoopPhi) {
    const std::string in = outPath("arm64_phi_nested.il");
    const std::string out = outPath("arm64_phi_nested.s");
    const std::string il = "il 0.1\n"
                           "func @nested(%n:i64, %m:i64) -> i64 {\n"
                           "entry(%n:i64, %m:i64):\n"
                           "  br outer(0, 0)\n"
                           "outer(%i:i64, %total:i64):\n"
                           "  br inner(0, %total)\n"
                           "inner(%j:i64, %sum:i64):\n"
                           "  %prod = imul.ovf %i, %j\n"
                           "  %new_sum = iadd.ovf %sum, %prod\n"
                           "  %next_j = iadd.ovf %j, 1\n"
                           "  %j_done = icmp_eq %next_j, %m\n"
                           "  cbr %j_done, inner_exit(%new_sum), inner(%next_j, %new_sum)\n"
                           "inner_exit(%inner_result:i64):\n"
                           "  %next_i = iadd.ovf %i, 1\n"
                           "  %i_done = icmp_eq %next_i, %n\n"
                           "  cbr %i_done, exit(%inner_result), outer(%next_i, %inner_result)\n"
                           "exit(%final:i64):\n"
                           "  ret %final\n"
                           "}\n";
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-S", out.c_str()};
    ASSERT_EQ(cmd_codegen_arm64(3, const_cast<char **>(argv)), 0);
    const std::string asmText = readFile(out);
    // Should have multiplication — either as a standalone mul or as a fused madd
    // (peephole fuses mul+add → madd when they are adjacent after regalloc).
    const bool hasMul = asmText.find("mul x") != std::string::npos;
    const bool hasMadd = asmText.find("madd x") != std::string::npos;
    EXPECT_TRUE(hasMul || hasMadd);
}

// Test 6: Phi with call in predecessor
TEST(Arm64CrossBlockPhi, PhiAfterCall) {
    const std::string in = outPath("arm64_phi_call.il");
    const std::string out = outPath("arm64_phi_call.s");
    const std::string il = "il 0.1\n"
                           "extern @get_val() -> i64\n"
                           "func @phi_call(%c:i64) -> i64 {\n"
                           "entry(%c:i64):\n"
                           "  %cond = icmp_ne %c, 0\n"
                           "  cbr %cond, call, nocall\n"
                           "call:\n"
                           "  %v = call @get_val()\n"
                           "  br join(%v)\n"
                           "nocall:\n"
                           "  br join(42)\n"
                           "join(%r:i64):\n"
                           "  ret %r\n"
                           "}\n";
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-S", out.c_str()};
    ASSERT_EQ(cmd_codegen_arm64(3, const_cast<char **>(argv)), 0);
    const std::string asmText = readFile(out);
    // Should have bl for the call
    EXPECT_NE(asmText.find("bl "), std::string::npos);
}

int main(int argc, char **argv) {
    // This test pins the retired block-local allocation path (frame slots,
    // PhiStore edges) until Phase 3 C7 deletes it; see ADR 0338.
    setenv("ZANNA_LOCAL_RA", "1", 1);
    zanna_test::init(&argc, &argv);
    return zanna_test::run_all_tests();
}
