//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/codegen/test_codegen_arm64_fp.cpp
// Purpose: Verify floating-point support in AArch64 backend.
//
//===----------------------------------------------------------------------===//
#include "tests/TestHarness.hpp"
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include "codegen/aarch64/TargetAArch64.hpp"
#include "codegen/aarch64/passes/EmitPass.hpp"
#include "codegen/aarch64/passes/LegalizePass.hpp"
#include "codegen/aarch64/passes/LoweringPass.hpp"
#include "codegen/aarch64/passes/PassManager.hpp"
#include "codegen/aarch64/passes/RegAllocPass.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Module.hpp"
#include "il/core/Type.hpp"
#include "il/core/Value.hpp"
#include "tools/zanna/cmd_codegen_arm64.hpp"

using namespace zanna::tools::ilc;
using namespace zanna::codegen::aarch64;

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

// Test 1: FP addition - f(x: f64) -> f64 returns x + 1.0
TEST(Arm64FP, FAddSimple) {
    const std::string in = outPath("arm64_fp_fadd.il");
    const std::string out = outPath("arm64_fp_fadd.s");
    // f64 parameters use v0, returns in v0
    // Note: We need to materialize 1.0 which requires sitofp from an integer
    const std::string il = "il 0.1\n"
                           "func @fadd1(%x:f64) -> f64 {\n"
                           "entry(%x:f64):\n"
                           "  %one = sitofp 1\n"
                           "  %r = fadd %x, %one\n"
                           "  ret %r\n"
                           "}\n";
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-S", out.c_str(), "-O0"};
    ASSERT_EQ(cmd_codegen_arm64(4, const_cast<char **>(argv)), 0);
    const std::string asmText = readFile(out);
    // Expect fadd with d-registers
    EXPECT_NE(asmText.find("fadd d"), std::string::npos);
    // Expect scvtf for sitofp
    EXPECT_NE(asmText.find("scvtf d"), std::string::npos);
}

// Test 2: FP subtraction
TEST(Arm64FP, FSubSimple) {
    const std::string in = outPath("arm64_fp_fsub.il");
    const std::string out = outPath("arm64_fp_fsub.s");
    const std::string il = "il 0.1\n"
                           "func @fsub1(%x:f64, %y:f64) -> f64 {\n"
                           "entry(%x:f64, %y:f64):\n"
                           "  %r = fsub %x, %y\n"
                           "  ret %r\n"
                           "}\n";
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-S", out.c_str(), "-O0"};
    ASSERT_EQ(cmd_codegen_arm64(4, const_cast<char **>(argv)), 0);
    const std::string asmText = readFile(out);
    EXPECT_NE(asmText.find("fsub d"), std::string::npos);
}

// Test 3: FP multiplication
TEST(Arm64FP, FMulSimple) {
    const std::string in = outPath("arm64_fp_fmul.il");
    const std::string out = outPath("arm64_fp_fmul.s");
    const std::string il = "il 0.1\n"
                           "func @fmul1(%x:f64, %y:f64) -> f64 {\n"
                           "entry(%x:f64, %y:f64):\n"
                           "  %r = fmul %x, %y\n"
                           "  ret %r\n"
                           "}\n";
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-S", out.c_str(), "-O0"};
    ASSERT_EQ(cmd_codegen_arm64(4, const_cast<char **>(argv)), 0);
    const std::string asmText = readFile(out);
    EXPECT_NE(asmText.find("fmul d"), std::string::npos);
}

// Test 4: FP division
TEST(Arm64FP, FDivSimple) {
    const std::string in = outPath("arm64_fp_fdiv.il");
    const std::string out = outPath("arm64_fp_fdiv.s");
    const std::string il = "il 0.1\n"
                           "func @fdiv1(%x:f64, %y:f64) -> f64 {\n"
                           "entry(%x:f64, %y:f64):\n"
                           "  %r = fdiv %x, %y\n"
                           "  ret %r\n"
                           "}\n";
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-S", out.c_str(), "-O0"};
    ASSERT_EQ(cmd_codegen_arm64(4, const_cast<char **>(argv)), 0);
    const std::string asmText = readFile(out);
    EXPECT_NE(asmText.find("fdiv d"), std::string::npos);
}

// Test 5: Integer to FP conversion (sitofp)
TEST(Arm64FP, SitofpConversion) {
    const std::string in = outPath("arm64_fp_sitofp.il");
    const std::string out = outPath("arm64_fp_sitofp.s");
    const std::string il = "il 0.1\n"
                           "func @itof(%x:i64) -> f64 {\n"
                           "entry(%x:i64):\n"
                           "  %r = sitofp %x\n"
                           "  ret %r\n"
                           "}\n";
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-S", out.c_str(), "-O0"};
    ASSERT_EQ(cmd_codegen_arm64(4, const_cast<char **>(argv)), 0);
    const std::string asmText = readFile(out);
    // Expect scvtf dN, xM
    EXPECT_NE(asmText.find("scvtf d"), std::string::npos);
    // Return value goes through v0: converted straight into d0 (the result
    // is hinted into the return register, ADR 0339) or moved there.
    const bool returnsInD0 = asmText.find("scvtf d0") != std::string::npos ||
                             asmText.find("fmov d0") != std::string::npos;
    EXPECT_TRUE(returnsInD0);
}

// Test 6: FP to integer conversion (fptosi)
TEST(Arm64FP, FptosiConversion) {
    const std::string in = outPath("arm64_fp_fptosi.il");
    const std::string out = outPath("arm64_fp_fptosi.s");
    const std::string il = "il 0.1\n"
                           "func @ftoi(%x:f64) -> i64 {\n"
                           "entry(%x:f64):\n"
                           "  %r = cast.fp_to_si.rte.chk %x\n"
                           "  ret %r\n"
                           "}\n";
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-S", out.c_str(), "-O0"};
    ASSERT_EQ(cmd_codegen_arm64(4, const_cast<char **>(argv)), 0);
    const std::string asmText = readFile(out);
    // Expect fcvtzs xN, dM
    EXPECT_NE(asmText.find("fcvtzs x"), std::string::npos);
}

TEST(Arm64FP, PlainFptosiChecksNaNAndRange) {
    il::core::Function fn;
    fn.name = "ftoi_plain";
    fn.retType = il::core::Type(il::core::Type::Kind::I64);

    il::core::BasicBlock entry;
    entry.label = "entry";
    entry.params.push_back({"x", il::core::Type(il::core::Type::Kind::F64), 0});

    il::core::Instr cast;
    cast.result = 1;
    cast.op = il::core::Opcode::Fptosi;
    cast.type = il::core::Type(il::core::Type::Kind::I64);
    cast.operands.push_back(il::core::Value::temp(0));
    entry.instructions.push_back(cast);

    il::core::Instr ret;
    ret.op = il::core::Opcode::Ret;
    ret.type = il::core::Type(il::core::Type::Kind::Void);
    ret.operands.push_back(il::core::Value::temp(1));
    entry.instructions.push_back(ret);
    entry.terminated = true;

    fn.blocks.push_back(entry);
    il::core::Module mod;
    mod.functions.push_back(fn);

    passes::AArch64Module module;
    module.ilMod = &mod;
    module.ti = &darwinTarget();

    passes::PassManager pm;
    pm.addPass(std::make_unique<passes::LoweringPass>());
    pm.addPass(std::make_unique<passes::LegalizePass>());
    pm.addPass(std::make_unique<passes::RegAllocPass>());
    pm.addPass(std::make_unique<passes::EmitPass>());

    passes::Diagnostics diags;
    ASSERT_TRUE(pm.run(module, diags));
    const std::string &asmText = module.assembly;

    EXPECT_NE(asmText.find("fcmp d"), std::string::npos);
    EXPECT_NE(asmText.find("b.vs L.Ltrap_fp_invalid"), std::string::npos);
    EXPECT_NE(asmText.find("L.Ltrap_fp_ovf"), std::string::npos);
    EXPECT_NE(asmText.find("fcvtzs x"), std::string::npos);
}

// Test 7: FP comparison (fcmp_lt)
TEST(Arm64FP, FCmpLT) {
    const std::string in = outPath("arm64_fp_fcmp_lt.il");
    const std::string out = outPath("arm64_fp_fcmp_lt.s");
    const std::string il = "il 0.1\n"
                           "func @cmplt(%x:f64, %y:f64) -> i64 {\n"
                           "entry(%x:f64, %y:f64):\n"
                           "  %c = fcmp_lt %x, %y\n"
                           "  %r = zext1 %c\n"
                           "  ret %r\n"
                           "}\n";
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-S", out.c_str(), "-O0"};
    ASSERT_EQ(cmd_codegen_arm64(4, const_cast<char **>(argv)), 0);
    const std::string asmText = readFile(out);
    // Expect fcmp dN, dM
    EXPECT_NE(asmText.find("fcmp d"), std::string::npos);
    // Expect cset for the result
    EXPECT_NE(asmText.find("cset x"), std::string::npos);
}

// Test 8: Call an extern FP function and return its result
TEST(Arm64FP, CallFPExtern) {
    const std::string in = outPath("arm64_fp_call.il");
    const std::string out = outPath("arm64_fp_call.s");
    const std::string il = "il 0.1\n"
                           "extern @rt_add_double(f64, f64) -> f64\n"
                           "func @caller(%a:f64, %b:f64) -> f64 {\n"
                           "entry(%a:f64, %b:f64):\n"
                           "  %r = call @rt_add_double(%a, %b)\n"
                           "  ret %r\n"
                           "}\n";
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-S", out.c_str(), "-O0"};
    ASSERT_EQ(cmd_codegen_arm64(4, const_cast<char **>(argv)), 0);
    const std::string asmText = readFile(out);
    // Expect bl rt_add_double
    EXPECT_NE(asmText.find(blSym("rt_add_double")), std::string::npos);
    // Args should be marshalled to v0, v1 for FP
    // Result comes back in v0
}

// Test 9: Mixed integer and FP call
TEST(Arm64FP, MixedCall) {
    const std::string in = outPath("arm64_fp_mixed.il");
    const std::string out = outPath("arm64_fp_mixed.s");
    const std::string il = "il 0.1\n"
                           "extern @mixed(i64, f64) -> f64\n"
                           "func @caller(%n:i64, %x:f64) -> f64 {\n"
                           "entry(%n:i64, %x:f64):\n"
                           "  %r = call @mixed(%n, %x)\n"
                           "  ret %r\n"
                           "}\n";
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-S", out.c_str(), "-O0"};
    ASSERT_EQ(cmd_codegen_arm64(4, const_cast<char **>(argv)), 0);
    const std::string asmText = readFile(out);
    EXPECT_NE(asmText.find(blSym("mixed")), std::string::npos);
}

TEST(Arm64FP, NonEncodableImmediateUsesBitcastPath_SystemAsm) {
    const std::string in = outPath("arm64_fp_nonenc_imm.il");
    const std::string out = outPath("arm64_fp_nonenc_imm.s");
    const std::string il = "il 0.1\n"
                           "func @ret_pi() -> f64 {\n"
                           "entry:\n"
                           "  ret 3.14\n"
                           "}\n";
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "--system-asm", "-S", out.c_str()};
    ASSERT_EQ(cmd_codegen_arm64(4, const_cast<char **>(argv)), 0);
    const std::string asmText = readFile(out);
    EXPECT_NE(asmText.find("fmov d0, x16"), std::string::npos);
    EXPECT_EQ(asmText.find("fmov d0, #3.140000"), std::string::npos);
}

TEST(Arm64FP, EncodableImmediateKeepsDirectForm_SystemAsm) {
    const std::string in = outPath("arm64_fp_enc_imm.il");
    const std::string out = outPath("arm64_fp_enc_imm.s");
    const std::string il = "il 0.1\n"
                           "func @ret_two() -> f64 {\n"
                           "entry:\n"
                           "  ret 2.0\n"
                           "}\n";
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "--system-asm", "-S", out.c_str()};
    ASSERT_EQ(cmd_codegen_arm64(4, const_cast<char **>(argv)), 0);
    const std::string asmText = readFile(out);
    EXPECT_NE(asmText.find("fmov d0, #2.000000"), std::string::npos);
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, &argv);
    return zanna_test::run_all_tests();
}
