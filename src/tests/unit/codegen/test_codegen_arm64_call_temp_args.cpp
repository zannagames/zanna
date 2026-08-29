//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/codegen/test_codegen_arm64_call_temp_args.cpp
// Purpose: Verify CLI lowers calls with a single non-entry temp argument by computing into X9.
// Key invariants: To be documented.
// Ownership/Lifetime: To be documented.
// Links: docs/internals/architecture.md
//
//===----------------------------------------------------------------------===//
#include "tests/TestHarness.hpp"
#include <filesystem>
#include <fstream>
#include <map>
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

/// @brief Returns the expected mangled symbol name for a call target.
static std::string blSym(const std::string &name) {
#if defined(__APPLE__)
    return "bl _" + name;
#else
    return "bl " + name;
#endif
}

TEST(Arm64CLI, CallWithTempRR) {
    const std::string in = outPath("arm64_call_temp_rr.il");
    const std::string out = outPath("arm64_call_temp_rr.s");
    const std::string il = "il 0.1\n"
                           "extern @h(i64, i64) -> i64\n"
                           "func @f(%a:i64, %b:i64) -> i64 {\n"
                           "entry(%a:i64, %b:i64):\n"
                           "  %t1 = iadd.ovf %a, %b\n"
                           "  %t0 = call @h(%t1, %a)\n"
                           "  ret %t0\n"
                           "}\n";
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-S", out.c_str()};
    ASSERT_EQ(cmd_codegen_arm64(3, const_cast<char **>(argv)), 0);
    const std::string asmText = readFile(out);
    // Expect an add instruction and the call (register assignment may vary
    // due to peephole compute-into-target folding).
    EXPECT_TRUE(asmText.find("adds x") != std::string::npos);
    EXPECT_NE(asmText.find(blSym("h")), std::string::npos);
}

TEST(Arm64CLI, CallWithTempRI) {
    const std::string in = outPath("arm64_call_temp_ri.il");
    const std::string out = outPath("arm64_call_temp_ri.s");
    const std::string il = "il 0.1\n"
                           "extern @h(i64, i64) -> i64\n"
                           "func @f(%a:i64, %b:i64) -> i64 {\n"
                           "entry(%a:i64, %b:i64):\n"
                           "  %t1 = iadd.ovf %b, 5\n"
                           "  %t0 = call @h(%a, %t1)\n"
                           "  ret %t0\n"
                           "}\n";
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-S", out.c_str()};
    ASSERT_EQ(cmd_codegen_arm64(3, const_cast<char **>(argv)), 0);
    const std::string asmText = readFile(out);
    // Expect an add-immediate instruction and the call.
    EXPECT_TRUE(asmText.find("adds x") != std::string::npos);
    EXPECT_NE(asmText.find("#5"), std::string::npos);
    EXPECT_NE(asmText.find(blSym("h")), std::string::npos);
}

TEST(Arm64CLI, CallWithTempShift) {
    const std::string in = outPath("arm64_call_temp_shl.il");
    const std::string out = outPath("arm64_call_temp_shl.s");
    const std::string il = "il 0.1\n"
                           "extern @h(i64, i64) -> i64\n"
                           "func @f(%a:i64, %b:i64) -> i64 {\n"
                           "entry(%a:i64, %b:i64):\n"
                           "  %t1 = shl %a, 3\n"
                           "  %t0 = call @h(%t1, %b)\n"
                           "  ret %t0\n"
                           "}\n";
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-S", out.c_str()};
    ASSERT_EQ(cmd_codegen_arm64(3, const_cast<char **>(argv)), 0);
    const std::string asmText = readFile(out);
    // Expect an lsl instruction and the call.
    EXPECT_TRUE(asmText.find("lsl x") != std::string::npos);
    EXPECT_NE(asmText.find("#3"), std::string::npos);
    EXPECT_NE(asmText.find(blSym("h")), std::string::npos);
}

TEST(Arm64CLI, CallWithCompareTemp) {
    const std::string in = outPath("arm64_call_temp_cmp.il");
    const std::string out = outPath("arm64_call_temp_cmp.s");
    const std::string il = "il 0.1\n"
                           "extern @h(i1, i64) -> i64\n"
                           "func @f(%a:i64, %b:i64) -> i64 {\n"
                           "entry(%a:i64, %b:i64):\n"
                           "  %t1 = icmp_eq %a, %b\n"
                           "  %t0 = call @h(%t1, %a)\n"
                           "  ret %t0\n"
                           "}\n";
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-S", out.c_str()};
    ASSERT_EQ(cmd_codegen_arm64(3, const_cast<char **>(argv)), 0);
    const std::string asmText = readFile(out);
    EXPECT_NE(asmText.find("cmp x0, x1"), std::string::npos);
    EXPECT_NE(asmText.find("cset"), std::string::npos);
    EXPECT_NE(asmText.find(blSym("h")), std::string::npos);
}

TEST(Arm64CLI, CallWithTwoTemps) {
    const std::string in = outPath("arm64_call_two_temps.il");
    const std::string out = outPath("arm64_call_two_temps.s");
    const std::string il = "il 0.1\n"
                           "extern @h(i64, i64) -> i64\n"
                           "func @f(%a:i64, %b:i64) -> i64 {\n"
                           "entry(%a:i64, %b:i64):\n"
                           "  %t1 = iadd.ovf %a, %b\n"
                           "  %t2 = shl %b, 1\n"
                           "  %t0 = call @h(%t1, %t2)\n"
                           "  ret %t0\n"
                           "}\n";
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-S", out.c_str()};
    ASSERT_EQ(cmd_codegen_arm64(3, const_cast<char **>(argv)), 0);
    const std::string asmText = readFile(out);
    // Expect both temps materialized before the call.
    EXPECT_NE(asmText.find("adds x"), std::string::npos);
    EXPECT_NE(asmText.find("lsl x"), std::string::npos);
    EXPECT_NE(asmText.find(blSym("h")), std::string::npos);
}

// ZB-30 helper: replay the `mov xD, xS` sequence between the entry label and
// the `bl` symbolically and report which incoming register each argument
// register finally carries. Independent of scratch-register choice and of the
// exact emission order, so it checks the parallel-move CONTRACT, not one
// particular schedule.
static std::string simulateArgMoves(const std::string &asmText, int argCount) {
    std::istringstream in(asmText);
    std::string line;
    std::map<std::string, std::string> reg; // current symbolic value per register
    for (int i = 0; i < 32; ++i) {
        const std::string x = "x" + std::to_string(i);
        reg[x] = x;
    }
    bool inBody = false;
    while (std::getline(in, line)) {
        if (!inBody) {
            if (line.find("Lentry") != std::string::npos)
                inBody = true;
            continue;
        }
        if (line.find("bl ") != std::string::npos)
            break;
        const std::size_t m = line.find("mov x");
        if (m == std::string::npos)
            continue;
        std::string rest = line.substr(m + 4);
        const std::size_t comma = rest.find(',');
        if (comma == std::string::npos)
            continue;
        std::string dst = rest.substr(0, comma);
        std::string src = rest.substr(comma + 1);
        auto trim = [](std::string &t) {
            while (!t.empty() && (t.front() == ' ' || t.front() == '\t'))
                t.erase(t.begin());
            while (!t.empty() && (t.back() == ' ' || t.back() == '\t' || t.back() == '\r'))
                t.pop_back();
        };
        trim(dst);
        trim(src);
        if (src.empty() || src[0] != 'x')
            continue; // immediate move
        reg[dst] = reg[src];
    }
    std::string out;
    for (int i = 0; i < argCount; ++i) {
        if (i)
            out += ' ';
        out += reg["x" + std::to_string(i)];
    }
    return out;
}

// ZB-30: a function that forwards its parameters shifted by one hit the call
// fast path, whose parallel-move sequencer emitted x3<-x4, x2<-x3, x1<-x2,
// x0<-x1 (every argument became x4). The baseball season diverged from the VM
// oracle on day 41 through exactly this shape (RunnerEngine.hitAndRunDoubleOffChance).
TEST(Arm64CLI, CallFastPathShiftedParamForwarding) {
    const std::string in = outPath("arm64_call_fastpath_shift.il");
    const std::string out = outPath("arm64_call_fastpath_shift.s");
    const std::string il = "il 0.1\n"
                           "extern @h(i64, i64, i64, i1) -> i64\n"
                           "func @f(ptr %s, i64 %a, i64 %b, i64 %c, i1 %d) -> i64 {\n"
                           "entry(%s:ptr, %a:i64, %b:i64, %c:i64, %d:i1):\n"
                           "  %t0 = call @h(%a, %b, %c, %d)\n"
                           "  ret %t0\n"
                           "}\n";
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-S", out.c_str()};
    ASSERT_EQ(cmd_codegen_arm64(3, const_cast<char **>(argv)), 0);
    const std::string asmText = readFile(out);
    EXPECT_NE(asmText.find(blSym("h")), std::string::npos);
    EXPECT_EQ(simulateArgMoves(asmText, 4), "x1 x2 x3 x4");
    // The forwarded i1 is canonicalized like generic lowering does.
    EXPECT_NE(asmText.find("and x3, x3, #"), std::string::npos);
}

// ZB-30: a full rotation is a pure cycle and needs the scratch register.
TEST(Arm64CLI, CallFastPathRotatedParamForwarding) {
    const std::string in = outPath("arm64_call_fastpath_rotate.il");
    const std::string out = outPath("arm64_call_fastpath_rotate.s");
    const std::string il = "il 0.1\n"
                           "extern @h(i64, i64, i64, i64) -> i64\n"
                           "func @f(i64 %a, i64 %b, i64 %c, i64 %d) -> i64 {\n"
                           "entry(%a:i64, %b:i64, %c:i64, %d:i64):\n"
                           "  %t0 = call @h(%b, %c, %d, %a)\n"
                           "  ret %t0\n"
                           "}\n";
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-S", out.c_str()};
    ASSERT_EQ(cmd_codegen_arm64(3, const_cast<char **>(argv)), 0);
    const std::string asmText = readFile(out);
    EXPECT_NE(asmText.find(blSym("h")), std::string::npos);
    EXPECT_EQ(simulateArgMoves(asmText, 4), "x1 x2 x3 x0");
}

// ZB-30: swapping two arguments is the smallest cycle.
TEST(Arm64CLI, CallFastPathSwappedParamForwarding) {
    const std::string in = outPath("arm64_call_fastpath_swap.il");
    const std::string out = outPath("arm64_call_fastpath_swap.s");
    const std::string il = "il 0.1\n"
                           "extern @h(i64, i64) -> i64\n"
                           "func @f(i64 %a, i64 %b) -> i64 {\n"
                           "entry(%a:i64, %b:i64):\n"
                           "  %t0 = call @h(%b, %a)\n"
                           "  ret %t0\n"
                           "}\n";
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-S", out.c_str()};
    ASSERT_EQ(cmd_codegen_arm64(3, const_cast<char **>(argv)), 0);
    const std::string asmText = readFile(out);
    EXPECT_EQ(simulateArgMoves(asmText, 2), "x1 x0");
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, &argv);
    return zanna_test::run_all_tests();
}
