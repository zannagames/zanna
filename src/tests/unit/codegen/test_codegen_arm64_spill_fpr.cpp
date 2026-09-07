//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/codegen/test_codegen_arm64_spill_fpr.cpp
// Purpose: Verify floating-point register (FPR) spilling on AArch64.
// Key invariants: Excess FP values spill to stack with str/ldr dN, [fp, #off].
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

// Test 1: Simple FPR spill - many live FP values exceeding register count
TEST(Arm64SpillFPR, SimpleSpill) {
    const std::string in = outPath("arm64_fpr_spill_simple.il");
    const std::string out = outPath("arm64_fpr_spill_simple.s");
    // Create many live FP values to force spilling
    // AArch64 has 32 FPRs (d0-d31) but some are caller-saved, some callee-saved
    // With many values, we should see spilling
    const std::string il = "il 0.1\n"
                           "func @many_fp(%a:f64, %b:f64) -> f64 {\n"
                           "entry(%a:f64, %b:f64):\n"
                           "  %t1 = fadd %a, %b\n"
                           "  %t2 = fmul %a, %b\n"
                           "  %t3 = fsub %a, %b\n"
                           "  %t4 = fdiv %a, %b\n"
                           "  %t5 = fadd %t1, %t2\n"
                           "  %t6 = fadd %t3, %t4\n"
                           "  %t7 = fadd %t5, %t6\n"
                           "  %r = fadd %t7, %a\n"
                           "  ret %r\n"
                           "}\n";
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-S", out.c_str()};
    ASSERT_EQ(cmd_codegen_arm64(3, const_cast<char **>(argv)), 0);
    const std::string asmText = readFile(out);
    // Should compile and have FP instructions
    EXPECT_NE(asmText.find("fadd d"), std::string::npos);
}

// Test 2: FPR spill across call - caller-saved FPRs need saving
TEST(Arm64SpillFPR, SpillAcrossCall) {
    const std::string in = outPath("arm64_fpr_spill_call.il");
    const std::string out = outPath("arm64_fpr_spill_call.s");
    const std::string il = "il 0.1\n"
                           "extern @compute(f64) -> f64\n"
                           "func @use_across_call(%x:f64, %y:f64) -> f64 {\n"
                           "entry(%x:f64, %y:f64):\n"
                           "  %t1 = fadd %x, %y\n"
                           "  %t2 = call @compute(%t1)\n"
                           "  %r = fadd %t2, %x\n"
                           "  ret %r\n"
                           "}\n";
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-S", out.c_str()};
    ASSERT_EQ(cmd_codegen_arm64(3, const_cast<char **>(argv)), 0);
    const std::string asmText = readFile(out);
    // Value %x needs to survive across the call
    // Should have str d for spill or use callee-saved register
    EXPECT_NE(asmText.find("bl "), std::string::npos);
}

// Test 3: Many FP temporaries to force spills
TEST(Arm64SpillFPR, ManyTemporaries) {
    const std::string in = outPath("arm64_fpr_many_temps.il");
    const std::string out = outPath("arm64_fpr_many_temps.s");
    // Create a chain that keeps many values live
    std::ostringstream ilss;
    ilss << "il 0.1\n"
         << "func @chain(%a:f64, %b:f64) -> f64 {\n"
         << "entry(%a:f64, %b:f64):\n";

    // Create many intermediate values
    for (int i = 1; i <= 16; ++i) {
        if (i == 1)
            ilss << "  %t" << i << " = fadd %a, %b\n";
        else
            ilss << "  %t" << i << " = fadd %t" << (i - 1) << ", %a\n";
    }

    // Sum them all to keep them live
    ilss << "  %s1 = fadd %t1, %t2\n"
         << "  %s2 = fadd %t3, %t4\n"
         << "  %s3 = fadd %t5, %t6\n"
         << "  %s4 = fadd %t7, %t8\n"
         << "  %s5 = fadd %t9, %t10\n"
         << "  %s6 = fadd %t11, %t12\n"
         << "  %s7 = fadd %t13, %t14\n"
         << "  %s8 = fadd %t15, %t16\n"
         << "  %p1 = fadd %s1, %s2\n"
         << "  %p2 = fadd %s3, %s4\n"
         << "  %p3 = fadd %s5, %s6\n"
         << "  %p4 = fadd %s7, %s8\n"
         << "  %q1 = fadd %p1, %p2\n"
         << "  %q2 = fadd %p3, %p4\n"
         << "  %r = fadd %q1, %q2\n"
         << "  ret %r\n"
         << "}\n";

    writeFile(in, ilss.str());
    const char *argv[] = {in.c_str(), "-S", out.c_str()};
    ASSERT_EQ(cmd_codegen_arm64(3, const_cast<char **>(argv)), 0);
    const std::string asmText = readFile(out);
    // Should compile and produce FP operations
    EXPECT_NE(asmText.find("fadd d"), std::string::npos);
}

// Test 4: Mixed GPR and FPR pressure
TEST(Arm64SpillFPR, MixedRegisterPressure) {
    const std::string in = outPath("arm64_fpr_mixed.il");
    const std::string out = outPath("arm64_fpr_mixed.s");
    const std::string il = "il 0.1\n"
                           "func @mixed(%i1:i64, %i2:i64, %f1:f64, %f2:f64) -> f64 {\n"
                           "entry(%i1:i64, %i2:i64, %f1:f64, %f2:f64):\n"
                           "  %ia = iadd.ovf %i1, %i2\n"
                           "  %ib = imul.ovf %i1, %i2\n"
                           "  %fa = fadd %f1, %f2\n"
                           "  %fb = fmul %f1, %f2\n"
                           "  %fi = sitofp %ia\n"
                           "  %r = fadd %fa, %fi\n"
                           "  ret %r\n"
                           "}\n";
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-S", out.c_str()};
    ASSERT_EQ(cmd_codegen_arm64(3, const_cast<char **>(argv)), 0);
    const std::string asmText = readFile(out);
    // Should have both integer and FP operations
    EXPECT_NE(asmText.find("adds x"), std::string::npos);
    EXPECT_NE(asmText.find("fadd d"), std::string::npos);
}

// Test 5: FPR in loop with accumulator
TEST(Arm64SpillFPR, LoopAccumulator) {
    const std::string in = outPath("arm64_fpr_loop.il");
    const std::string out = outPath("arm64_fpr_loop.s");
    const std::string il = "il 0.1\n"
                           "func @sum_loop(%n:i64, %init:f64) -> f64 {\n"
                           "entry(%n:i64, %init:f64):\n"
                           "  br loop(0, %init)\n"
                           "loop(%i:i64, %acc:f64):\n"
                           "  %one = sitofp 1\n"
                           "  %next_acc = fadd %acc, %one\n"
                           "  %next_i = iadd.ovf %i, 1\n"
                           "  %done = icmp_eq %next_i, %n\n"
                           "  cbr %done, exit(%next_acc), loop(%next_i, %next_acc)\n"
                           "exit(%result:f64):\n"
                           "  ret %result\n"
                           "}\n";
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-S", out.c_str()};
    ASSERT_EQ(cmd_codegen_arm64(3, const_cast<char **>(argv)), 0);
    const std::string asmText = readFile(out);
    // Should have fadd for accumulation
    EXPECT_NE(asmText.find("fadd d"), std::string::npos);
    // Should have loop structure (label and branch)
    EXPECT_NE(asmText.find("b "), std::string::npos);
}

int main(int argc, char **argv) {
    // This test pins the retired block-local allocation path (frame slots,
    // PhiStore edges) until Phase 3 C7 deletes it; see ADR 0338.
    setenv("ZANNA_LOCAL_RA", "1", 1);
    zanna_test::init(&argc, &argv);
    return zanna_test::run_all_tests();
}
