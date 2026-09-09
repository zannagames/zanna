//===----------------------------------------------------------------------===//
// Part of the Zanna project, under the GNU GPL v3.
//===----------------------------------------------------------------------===//
// File: tests/unit/codegen/test_codegen_arm64_cf_loop_phi.cpp
// Purpose: Verify loop lowering with loop-carried block params.
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

TEST(Arm64CLI, CF_Loop_Phi) {
    const std::string in = outPath("arm64_cf_loop.il");
    const std::string out = outPath("arm64_cf_loop.s");
    // Sum 1..10 using loop-carried phi
    const std::string il = "il 0.3.0\n"
                           "func @main() -> i64 {\n"
                           "entry:\n"
                           "  br loop(0, 0)\n"
                           "loop(%i:i64, %acc:i64):\n"
                           "  %c = scmp_ge %i, 10\n"
                           "  cbr %c, exit(%acc), body(%i, %acc)\n"
                           "body(%i:i64, %acc:i64):\n"
                           "  %i1 = iadd.ovf %i, 1\n"
                           "  %acc1 = iadd.ovf %acc, %i1\n"
                           "  br loop(%i1, %acc1)\n"
                           "exit(%res:i64):\n"
                           "  ret %res\n"
                           "}\n";
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-O2", "-S", out.c_str()};
    ASSERT_EQ(cmd_codegen_arm64(4, const_cast<char **>(argv)), 0);
    const std::string asmText = readFile(out);
    // Loop phi cleanup should split the header into a one-time reload block and
    // redirect the loop so iterations stay in registers. The current lowering
    // may either keep a distinct hot-body label or collapse the body into the
    // canonical loop label when the reload edge becomes trivial.
    EXPECT_EQ(asmText.find(".edge.t."), std::string::npos);
    EXPECT_EQ(asmText.find(".edge.f."), std::string::npos);
    const bool hasSplitHotBody = asmText.find("Lbody_body:") != std::string::npos &&
                                 asmText.find("b Lbody_body") != std::string::npos;
    const bool hasCollapsedBody = asmText.find("Lbody:\n") != std::string::npos &&
                                  asmText.find("b Lbody") != std::string::npos;
    EXPECT_TRUE(hasSplitHotBody || hasCollapsedBody);
}

TEST(Arm64CLI, CF_Loop_Phi_NoHeaderReloads) {
    // The loop-carried parameters stay in registers for the whole loop: the
    // function-wide allocator (ADR 0339) gives each one a register, so the
    // header reloads nothing from the frame and the back edge is a plain
    // branch to the header.
    const std::string in = outPath("arm64_cf_loop_pair.il");
    const std::string out = outPath("arm64_cf_loop_pair.s");
    const std::string il = "il 0.3.0\n"
                           "func @main() -> i64 {\n"
                           "entry:\n"
                           "  br loop(0, 0)\n"
                           "loop(%sum:i64, %i:i64):\n"
                           "  %done = scmp_ge %i, 10\n"
                           "  cbr %done, exit(%sum), body(%sum, %i)\n"
                           "body(%sum0:i64, %i0:i64):\n"
                           "  %t1 = iadd.ovf %i0, 1\n"
                           "  %t2 = imul.ovf %t1, 2\n"
                           "  %t3 = iadd.ovf %i0, 3\n"
                           "  %t4 = iadd.ovf %t2, %t3\n"
                           "  %t5 = imul.ovf %t4, 5\n"
                           "  %t6 = isub.ovf %t5, %i0\n"
                           "  %t7 = iadd.ovf %t6, 7\n"
                           "  %t8 = imul.ovf %t7, 3\n"
                           "  %t9 = isub.ovf %t8, 11\n"
                           "  %new_sum = iadd.ovf %sum0, %t9\n"
                           "  %next_i = iadd.ovf %i0, 1\n"
                           "  br loop(%new_sum, %next_i)\n"
                           "exit(%result:i64):\n"
                           "  ret %result\n"
                           "}\n";
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-O2", "-S", out.c_str()};
    ASSERT_EQ(cmd_codegen_arm64(4, const_cast<char **>(argv)), 0);
    const std::string asmText = readFile(out);
    EXPECT_NE(asmText.find("Lbody:\n"), std::string::npos);
    EXPECT_NE(asmText.find("b Lbody\n"), std::string::npos);
    // No frame traffic inside the function body: the only pair loads are the
    // epilogue's callee-saved / frame-record restores.
    for (std::size_t pos = asmText.find("ldp x"); pos != std::string::npos;
         pos = asmText.find("ldp x", pos + 1)) {
        const std::string line = asmText.substr(pos, asmText.find('\n', pos) - pos);
        EXPECT_TRUE(line.find("[sp") != std::string::npos);
    }
    EXPECT_EQ(asmText.find("[x29, #-"), std::string::npos);
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, &argv);
    return zanna_test::run_all_tests();
}
