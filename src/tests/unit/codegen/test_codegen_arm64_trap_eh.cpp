//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/codegen/test_codegen_arm64_trap_eh.cpp
// Purpose: Verify AArch64 lowering for IL traps and EH markers.
// Key invariants: To be documented.
// Ownership/Lifetime: To be documented.
// Links: docs/internals/architecture.md
//
//===----------------------------------------------------------------------===//
#include "tests/TestHarness.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "codegen/aarch64/LowerILToMIR.hpp"
#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Type.hpp"
#include "il/core/Value.hpp"
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
#if defined(__APPLE__)
static const bool kDarwinHost = true;
#else
static const bool kDarwinHost = false;
#endif

static std::string blSym(const std::string &name) {
    return kDarwinHost ? "bl _" + name : "bl " + name;
}

static bool hasExactCall(const std::string &asmText, const std::string &name) {
    const std::string expected = blSym(name);
    std::istringstream lines(asmText);
    for (std::string line; std::getline(lines, line);) {
        const std::size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos)
            continue;
        const std::size_t last = line.find_last_not_of(" \t\r");
        if (line.substr(first, last - first + 1) == expected)
            return true;
    }
    return false;
}

static std::string sliceBeforeCall(const std::string &asmText,
                                   const std::string &label,
                                   const std::string &callee) {
    const std::size_t labelPos = asmText.find(label);
    const std::size_t callPos = asmText.find(blSym(callee));
    if (labelPos == std::string::npos || callPos == std::string::npos || callPos < labelPos)
        return {};
    return asmText.substr(labelPos, callPos - labelPos);
}

static bool preparesX0(const std::string &asmSlice) {
    return asmSlice.find("mov x0") != std::string::npos ||
           asmSlice.find("mov w0") != std::string::npos ||
           asmSlice.find("ldr x0") != std::string::npos ||
           asmSlice.find("ldr w0") != std::string::npos;
}

TEST(Arm64CLI, TrapSimple) {
    const std::string in = outPath("arm64_trap.il");
    const std::string out = outPath("arm64_trap.s");
    const std::string il = "il 0.1\n"
                           "func @t() -> i64 {\n"
                           "entry:\n"
                           "  trap\n"
                           "}\n";
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-S", out.c_str()};
    ASSERT_EQ(cmd_codegen_arm64(3, const_cast<char **>(argv)), 0);
    const std::string asmText = readFile(out);
    const std::size_t movNullPos = asmText.find("mov x0, #0");
    const std::size_t trapCallPos = asmText.find(blSym("rt_trap_string"));
    EXPECT_NE(movNullPos, std::string::npos);
    EXPECT_NE(trapCallPos, std::string::npos);
    if (movNullPos != std::string::npos && trapCallPos != std::string::npos)
        EXPECT_LT(movNullPos, trapCallPos);
}

TEST(Arm64Lowering, TrapPayloadPreparesX0) {
    using namespace il::core;
    using namespace zanna::codegen::aarch64;

    Function fn;
    fn.name = "tp";
    fn.retType = Type(Type::Kind::I64);

    BasicBlock entry;
    entry.label = "entry";
    entry.terminated = true;

    Instr trap;
    trap.op = Opcode::Trap;
    trap.type = Type(Type::Kind::Void);
    trap.operands = {Value::constInt(42)};

    entry.instructions = {trap};
    fn.blocks = {entry};

    LowerILToMIR lowering{darwinTarget()};
    const MFunction mir = lowering.lowerFunction(fn);

    ASSERT_EQ(mir.blocks.size(), 1u);
    std::vector<uint16_t> payloadRegs;
    bool sawPayloadIntoX0 = false;
    bool sawNullMove = false;
    bool sawTrapCall = false;
    for (const auto &mi : mir.blocks[0].instrs) {
        if (mi.opc == MOpcode::MovRI && mi.ops.size() == 2 &&
            mi.ops[0].kind == MOperand::Kind::Reg && mi.ops[1].kind == MOperand::Kind::Imm) {
            if (mi.ops[1].imm == 42) {
                if (mi.ops[0].reg.isPhys &&
                    static_cast<PhysReg>(mi.ops[0].reg.idOrPhys) == PhysReg::X0) {
                    sawPayloadIntoX0 = true;
                } else {
                    payloadRegs.push_back(mi.ops[0].reg.idOrPhys);
                }
            }
            if (mi.ops[0].reg.isPhys &&
                static_cast<PhysReg>(mi.ops[0].reg.idOrPhys) == PhysReg::X0 && mi.ops[1].imm == 0) {
                sawNullMove = true;
            }
        }
        if (mi.opc == MOpcode::MovRR && mi.ops.size() == 2 &&
            mi.ops[0].kind == MOperand::Kind::Reg && mi.ops[0].reg.isPhys &&
            static_cast<PhysReg>(mi.ops[0].reg.idOrPhys) == PhysReg::X0 &&
            mi.ops[1].kind == MOperand::Kind::Reg && !mi.ops[1].reg.isPhys) {
            sawPayloadIntoX0 =
                sawPayloadIntoX0 ||
                std::find(payloadRegs.begin(), payloadRegs.end(), mi.ops[1].reg.idOrPhys) !=
                    payloadRegs.end();
        }
        if (mi.opc == MOpcode::Bl && !mi.ops.empty() && mi.ops[0].kind == MOperand::Kind::Label &&
            mi.ops[0].label == "rt_trap_string") {
            sawTrapCall = true;
        }
    }

    EXPECT_TRUE(sawPayloadIntoX0);
    EXPECT_FALSE(sawNullMove);
    EXPECT_TRUE(sawTrapCall);
}

TEST(Arm64CLI, TrapFromErr) {
    const std::string in = outPath("arm64_trap_from_err.il");
    const std::string out = outPath("arm64_trap_from_err.s");
    const std::string il = "il 0.1\n"
                           "func @te() -> i64 {\n"
                           "entry:\n"
                           "  trap.from_err i32 7\n"
                           "}\n";
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-S", out.c_str()};
    ASSERT_EQ(cmd_codegen_arm64(3, const_cast<char **>(argv)), 0);
    const std::string asmText = readFile(out);
    EXPECT_TRUE(hasExactCall(asmText, "rt_trap_raise_error"));
    const std::string preCall = sliceBeforeCall(asmText, "Lentry", "rt_trap_raise_error");
    EXPECT_NE(preCall.find("#7"), std::string::npos);
    EXPECT_TRUE(preparesX0(preCall));
}

TEST(Arm64CLI, TrapFromErrAcceptsNonEntryBlockParam) {
    const std::string in = outPath("arm64_trap_from_err_block_param.il");
    const std::string out = outPath("arm64_trap_from_err_block_param.s");
    const std::string il = "il 0.1\n"
                           "func @raise(%ignored:i32, %code:i32) -> i64 {\n"
                           "entry(%ignored:i32, %code:i32):\n"
                           "  br fail(%code)\n"
                           "fail(%err:i32):\n"
                           "  trap.from_err i32 %err\n"
                           "}\n";
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-S", out.c_str(), "-O0"};
    ASSERT_EQ(cmd_codegen_arm64(4, const_cast<char **>(argv)), 0);
    const std::string asmText = readFile(out);
    EXPECT_TRUE(hasExactCall(asmText, "rt_trap_raise_error"));
    const std::string preCall = sliceBeforeCall(asmText, "Lfail", "rt_trap_raise_error");
    EXPECT_TRUE(preparesX0(preCall));
}

TEST(Arm64CLI, EhMarkersLowerToNativeHelpers) {
    const std::string in = outPath("arm64_eh.il");
    const std::string out = outPath("arm64_eh.s");
    const std::string il = "il 0.1\n"
                           "func @errors_demo() -> i64 {\n"
                           "entry:\n"
                           "  eh.push ^handle\n"
                           "  trap.from_err i32 6\n"
                           "handler ^handle(%err:Error, %tok:ResumeTok):\n"
                           "  eh.entry\n"
                           "  resume.label %tok, ^done\n"
                           "done:\n"
                           "  ret 0\n"
                           "}\n";
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-S", out.c_str()};
    ASSERT_EQ(cmd_codegen_arm64(3, const_cast<char **>(argv)), 0);
    const std::string asmText = readFile(out);
    EXPECT_NE(asmText.find(blSym("rt_native_eh_frame_alloc")), std::string::npos);
    EXPECT_NE(asmText.find(blSym("rt_native_eh_push")), std::string::npos);
    // Darwin targets the mask-free `_setjmp`; other hosts `setjmp`.
    EXPECT_NE(asmText.find(blSym(kDarwinHost ? "_setjmp" : "setjmp")), std::string::npos);
    EXPECT_TRUE(hasExactCall(asmText, "rt_trap_raise_error"));
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, &argv);
    return zanna_test::run_all_tests();
}
