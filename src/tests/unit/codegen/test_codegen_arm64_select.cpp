//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/codegen/test_codegen_arm64_select.cpp
// Purpose: Verify select-like patterns using cbr + join with phi transport.
//          Covers simple constants and values loaded from memory.
//
//===----------------------------------------------------------------------===//
#include "tests/TestHarness.hpp"
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

TEST(Arm64CLI, Select_ConstArms) {
    const std::string in = outPath("arm64_select_const.il");
    const std::string out = outPath("arm64_select_const.s");
    const std::string il = "il 0.3.0\n"
                           "func @f(%x:i64) -> i64 {\n"
                           "entry(%x:i64):\n"
                           "  %cond = scmp_gt %x, 0\n"
                           "  cbr %cond, then, els\n"
                           "then():\n"
                           "  br join(1)\n"
                           "els():\n"
                           "  br join(0)\n"
                           "join(%v:i64):\n"
                           "  ret %v\n"
                           "}\n";
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-S", out.c_str()};
    ASSERT_EQ(cmd_codegen_arm64(3, const_cast<char **>(argv)), 0);
    const std::string asmText = readFile(out);
    // Expect compare (cmp or tst), conditional branch, and phi transport.
    // Block parameters may use spill slots or direct edge copies.
    // Note: peephole optimizer may convert `cmp x, #0` to `tst x, x`
    const bool hasCompare =
        asmText.find("cmp") != std::string::npos || asmText.find("tst") != std::string::npos;
    EXPECT_TRUE(hasCompare);
    EXPECT_NE(asmText.find("b."), std::string::npos);
    EXPECT_NE(asmText.find("Ljoin:"), std::string::npos);
    // The join parameter is hinted into x0 (ADR 0339): the arms materialise
    // `mov x0, #1` / `mov x0, #0` directly, or an older shape moves a
    // register into x0 at the join. Either transport is legal.
    const bool returnsThroughX0 = asmText.find(" mov x0, x") != std::string::npos ||
                                  asmText.find(" mov x0, #") != std::string::npos;
    EXPECT_TRUE(returnsThroughX0);
    const bool hasSpillTransport =
        asmText.find(" str x") != std::string::npos || asmText.find(" ldr x") != std::string::npos;
    const bool hasEdgeSplit = asmText.find(".Ledge_") != std::string::npos;
    EXPECT_TRUE(hasSpillTransport || hasEdgeSplit);
}

TEST(Arm64CLI, Select_LoadArms) {
    const std::string in = outPath("arm64_select_load.il");
    const std::string out = outPath("arm64_select_load.s");
    const std::string il = "il 0.3.0\n"
                           "func @g(%x:i64) -> i64 {\n"
                           "entry(%x:i64):\n"
                           "  %a = alloca 8\n"
                           "  %b = alloca 8\n"
                           "  store i64, %a, 11\n"
                           "  store i64, %b, 22\n"
                           "  %cond = scmp_gt %x, 0\n"
                           "  cbr %cond, then, els\n"
                           "then():\n"
                           "  %av = load i64, %a\n"
                           "  br join(%av)\n"
                           "els():\n"
                           "  %bv = load i64, %b\n"
                           "  br join(%bv)\n"
                           "join(%v:i64):\n"
                           "  ret %v\n"
                           "}\n";
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-S", out.c_str()};
    ASSERT_EQ(cmd_codegen_arm64(3, const_cast<char **>(argv)), 0);
    const std::string asmText = readFile(out);
    // O1 IL optimization promotes the local slots, so this may lower either as
    // stack traffic or as direct immediate edge values.
    const bool hasStackTransport =
        asmText.find("str x") != std::string::npos || asmText.find("ldr x") != std::string::npos;
    const bool hasPromotedValues =
        asmText.find("#11") != std::string::npos && asmText.find("#22") != std::string::npos;
    EXPECT_TRUE(hasStackTransport || hasPromotedValues);
    EXPECT_NE(asmText.find("b."), std::string::npos);
    EXPECT_NE(asmText.find("Ljoin:"), std::string::npos);
    EXPECT_NE(asmText.find(" mov x"), std::string::npos);
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, &argv);
    return zanna_test::run_all_tests();
}
