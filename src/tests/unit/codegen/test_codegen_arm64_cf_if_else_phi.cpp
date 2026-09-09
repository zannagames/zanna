//===----------------------------------------------------------------------===//
// Part of the Zanna project, under the GNU GPL v3.
//===----------------------------------------------------------------------===//
// File: tests/unit/codegen/test_codegen_arm64_cf_if_else_phi.cpp
// Purpose: Verify if/else lowering with block params (phi via edge transport).
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

TEST(Arm64CLI, CF_IfElse_Phi) {
    const std::string in = outPath("arm64_cf_ifelse.il");
    const std::string out = outPath("arm64_cf_ifelse.s");
    const std::string il = "il 0.3.0\n"
                           "func @f(%x:i64) -> i64 {\n"
                           "entry(%x:i64):\n"
                           "  %cond = scmp_gt %x, 0\n"
                           "  cbr %cond, then, else\n"
                           "then:\n"
                           "  br join(1)\n"
                           "else:\n"
                           "  br join(2)\n"
                           "join(%v:i64):\n"
                           "  ret %v\n"
                           "}\n";
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-S", out.c_str()};
    ASSERT_EQ(cmd_codegen_arm64(3, const_cast<char **>(argv)), 0);
    const std::string asmText = readFile(out);
    // Expect conditional branch present. Block parameters may travel via spill
    // slots or direct edge copies depending on the join simplification pass.
    EXPECT_NE(asmText.find("b."), std::string::npos);
    EXPECT_EQ(asmText.find(".edge.t."), std::string::npos);
    EXPECT_EQ(asmText.find(".edge.f."), std::string::npos);
    EXPECT_NE(asmText.find("Ljoin:"), std::string::npos);
    // The join parameter is hinted into x0 (ADR 0339): the arms materialise
    // `mov x0, #1` / `mov x0, #2` directly, or an older shape moves a
    // register into x0 at the join. Either transport is legal.
    const bool returnsThroughX0 = asmText.find(" mov x0, x") != std::string::npos ||
                                  asmText.find(" mov x0, #") != std::string::npos;
    EXPECT_TRUE(returnsThroughX0);
    const bool hasSpillTransport =
        asmText.find(" str x") != std::string::npos || asmText.find(" ldr x") != std::string::npos;
    const bool hasEdgeSplit = asmText.find(".Ledge_") != std::string::npos;
    EXPECT_TRUE(hasSpillTransport || hasEdgeSplit);
}

TEST(Arm64CLI, CF_BlockParamUsedInDominatedSuccessor) {
    const std::string in = outPath("arm64_cf_param_successor.il");
    const std::string out = outPath("arm64_cf_param_successor.s");
    const std::string il = "il 0.3.0\n"
                           "func @f(%flag:i1) -> i64 {\n"
                           "entry(%flag:i1):\n"
                           "  cbr %flag, left, right\n"
                           "left:\n"
                           "  br carrier(41)\n"
                           "right:\n"
                           "  br carrier(42)\n"
                           "use:\n"
                           "  ret %carried\n"
                           "carrier(%carried:i64):\n"
                           "  cbr %flag, use, exit\n"
                           "exit:\n"
                           "  ret 0\n"
                           "}\n";
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-O0", "-S", out.c_str()};
    ASSERT_EQ(cmd_codegen_arm64(4, const_cast<char **>(argv)), 0);
    const std::string asmText = readFile(out);

    auto blockText = [&](const std::string &label) {
        const std::size_t start = asmText.find(label);
        if (start == std::string::npos)
            return std::string{};
        const std::size_t next = asmText.find("\nL", start + label.size());
        if (next == std::string::npos)
            return asmText.substr(start);
        return asmText.substr(start, next - start);
    };

    const std::string carrierBlock = blockText("Lcarrier:");
    const std::string useBlock = blockText("Luse:");
    ASSERT_FALSE(carrierBlock.empty());
    ASSERT_FALSE(useBlock.empty());
    // The carried parameter reaches the return register in `use`, either
    // straight from the register the allocator gave it (`mov x0, x1`) or,
    // under an allocator that homes block parameters in the frame, through
    // a reload. What must not happen is `use` returning without touching x0.
    const bool carriedReachesX0 = useBlock.find("mov x0") != std::string::npos ||
                                  useBlock.find("ldr x0") != std::string::npos;
    EXPECT_TRUE(carriedReachesX0);
    EXPECT_NE(useBlock.find("ret"), std::string::npos);
    // Whatever transport is used, the carrier block itself computes nothing
    // but its branch: the value arrives from the predecessors.
    EXPECT_EQ(carrierBlock.find("add "), std::string::npos);
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, &argv);
    return zanna_test::run_all_tests();
}
