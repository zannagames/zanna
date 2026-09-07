//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/codegen/test_x86_backend_regressions.cpp
// Purpose: Regression tests for x86-64 backend bugs found during review.
//
//===----------------------------------------------------------------------===//

#include "tests/TestHarness.hpp"

#include <cstdlib>
#ifdef _WIN32
#include "tests/common/PosixCompat.h"
#endif

#include "codegen/x86_64/AsmEmitter.hpp"
#include "codegen/x86_64/Backend.hpp"
#include "codegen/x86_64/FrameLowering.hpp"
#include "codegen/x86_64/ISel.hpp"
#include "codegen/x86_64/LowerILToMIR.hpp"
#include "codegen/x86_64/Lowering.EmitCommon.hpp"
#include "codegen/x86_64/OperandRoles.hpp"
#include "codegen/x86_64/PreRegAllocOpt.hpp"
#include "codegen/x86_64/RegAllocLinear.hpp"
#include "codegen/x86_64/TargetX64.hpp"
#include "codegen/x86_64/peephole/BranchOpt.hpp"
#include "codegen/x86_64/peephole/MemoryOpt.hpp"
#include "codegen/x86_64/peephole/PeepholeCommon.hpp"
#include "codegen/x86_64/ra/Liveness.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

using namespace zanna::codegen::x64;

namespace zanna::codegen::x64 {
void lowerOverflowOps(MFunction &fn);
}

namespace {

ILValue val(ILValue::Kind kind, int id) {
    ILValue value{};
    value.kind = kind;
    value.id = id;
    return value;
}

ILValue imm(int64_t value) {
    ILValue operand{};
    operand.kind = ILValue::Kind::I64;
    operand.id = -1;
    operand.i64 = value;
    return operand;
}

ILValue immI1(int64_t value) {
    ILValue operand{};
    operand.kind = ILValue::Kind::I1;
    operand.id = -1;
    operand.i64 = value;
    return operand;
}

ILValue immPtr(int64_t value) {
    ILValue operand{};
    operand.kind = ILValue::Kind::PTR;
    operand.id = -1;
    operand.i64 = value;
    return operand;
}

ILValue immF64(double value) {
    ILValue operand{};
    operand.kind = ILValue::Kind::F64;
    operand.id = -1;
    operand.f64 = value;
    return operand;
}

ILValue strLit(std::string bytes, std::uint64_t len) {
    ILValue operand{};
    operand.kind = ILValue::Kind::STR;
    operand.id = -1;
    operand.str = std::move(bytes);
    operand.strLen = len;
    return operand;
}

ILValue label(const char *name) {
    ILValue operand{};
    operand.kind = ILValue::Kind::LABEL;
    operand.id = -1;
    operand.label = name;
    return operand;
}

ILInstr op(const char *opcode,
           std::vector<ILValue> ops,
           int resultId = -1,
           ILValue::Kind resultKind = ILValue::Kind::I64) {
    ILInstr instr{};
    instr.opcode = opcode;
    instr.ops = std::move(ops);
    instr.resultId = resultId;
    instr.resultKind = resultKind;
    return instr;
}

ILInstr opBits(const char *opcode,
               std::vector<ILValue> ops,
               int resultId,
               ILValue::Kind resultKind,
               std::uint8_t resultBits) {
    ILInstr instr = op(opcode, std::move(ops), resultId, resultKind);
    instr.resultBits = resultBits;
    return instr;
}

CodegenResult compile(const ILFunction &fn, const CodegenOptions &options = {}) {
    ILModule module{};
    module.funcs = {fn};
    return emitModuleToAssembly(module, options);
}

BinaryEmitResult compileBinary(const ILFunction &fn, const CodegenOptions &options = {}) {
    ILModule module{};
    module.funcs = {fn};
    return emitModuleToBinary(module, options);
}

const zanna::codegen::objfile::CodeSection &primaryTextSection(const BinaryEmitResult &result) {
    if (!result.textSections.empty())
        return result.textSections.front();
    return result.text;
}

void expectCompileRejects(std::string name,
                          std::vector<ILInstr> instrs,
                          std::vector<int> paramIds = {},
                          std::vector<ILValue::Kind> paramKinds = {}) {
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = std::move(paramIds);
    entry.paramKinds = std::move(paramKinds);
    entry.instrs = std::move(instrs);

    ILFunction fn{};
    fn.name = std::move(name);
    fn.blocks = {std::move(entry)};

    EXPECT_THROWS(compile(fn), std::runtime_error);
}

CodegenResult emitRawAssembly(const MFunction &fn) {
    AsmEmitter::RoDataPool roData{};
    return emitMIRToAssembly({fn}, roData, sysvTarget(), {});
}

bool containsRegex(const std::string &text, const std::string &pattern) {
    return std::regex_search(text, std::regex(pattern));
}

bool blockContainsOpcode(const MBasicBlock &block, MOpcode opcode) {
    return std::any_of(block.instructions.begin(),
                       block.instructions.end(),
                       [&](const MInstr &instr) { return instr.opcode == opcode; });
}

std::size_t countOpcode(const MBasicBlock &block, MOpcode opcode) {
    return static_cast<std::size_t>(std::count_if(
        block.instructions.begin(), block.instructions.end(), [&](const MInstr &instr) {
            return instr.opcode == opcode;
        }));
}

MFunction rawFunction(std::string name, std::vector<MInstr> instructions) {
    MFunction fn{};
    fn.name = std::move(name);
    MBasicBlock entry{};
    entry.label = ".L_" + fn.name + "_entry";
    entry.instructions = std::move(instructions);
    fn.blocks.push_back(std::move(entry));
    return fn;
}

const OpLabel *jumpTarget(const MInstr &instr) {
    if (instr.operands.empty())
        return nullptr;
    return std::get_if<OpLabel>(&instr.operands.back());
}

OpMem baseMem(const Operand &base, int32_t disp = 0) {
    OpMem mem{};
    mem.base = std::get<OpReg>(base);
    mem.index = {};
    mem.scale = 1;
    mem.disp = disp;
    mem.hasIndex = false;
    return mem;
}

OpMem indexedMem(const Operand &base, const Operand &index, uint8_t scale, int32_t disp = 0) {
    OpMem mem = baseMem(base, disp);
    mem.index = std::get<OpReg>(index);
    mem.scale = scale;
    mem.hasIndex = true;
    return mem;
}

const OpMem *firstMemOperand(const MInstr &instr) {
    for (const auto &operand : instr.operands) {
        if (const auto *mem = std::get_if<OpMem>(&operand)) {
            return mem;
        }
    }
    return nullptr;
}

bool isControlTerminator(MOpcode opcode) {
    return opcode == MOpcode::JMP || opcode == MOpcode::JCC || opcode == MOpcode::RET ||
           opcode == MOpcode::UD2;
}

bool blockHasNonTerminatorAfterControlTransfer(const MBasicBlock &block) {
    bool seenControlTransfer = false;
    for (const auto &instr : block.instructions) {
        if (seenControlTransfer && !isControlTerminator(instr.opcode)) {
            return true;
        }
        if (isControlTerminator(instr.opcode)) {
            seenControlTransfer = true;
        }
    }
    return false;
}

bool blockHasExplicitConditionalFallthrough(const MBasicBlock &block) {
    if (block.instructions.size() < 2) {
        return false;
    }
    for (std::size_t i = 0; i + 1 < block.instructions.size(); ++i) {
        if (block.instructions[i].opcode == MOpcode::JCC &&
            block.instructions[i + 1].opcode == MOpcode::JMP) {
            return true;
        }
    }
    return false;
}

int dupFd(int fd) {
#if defined(_WIN32)
    return _dup(fd);
#else
    return dup(fd);
#endif
}

int dup2Fd(int src, int dst) {
#if defined(_WIN32)
    return _dup2(src, dst);
#else
    return dup2(src, dst);
#endif
}

int closeFd(int fd) {
#if defined(_WIN32)
    return _close(fd);
#else
    return close(fd);
#endif
}

int fileNumber(FILE *file) {
#if defined(_WIN32)
    return _fileno(file);
#else
    return fileno(file);
#endif
}

template <typename Fn> std::string captureStderr(Fn &&fn) {
    std::fflush(stderr);
    std::cerr.flush();

    FILE *capture = std::tmpfile();
    if (capture == nullptr)
        return {};

    const int stderrFd = fileNumber(stderr);
    const int captureFd = fileNumber(capture);
    const int savedFd = dupFd(stderrFd);
    if (savedFd < 0) {
        std::fclose(capture);
        return {};
    }

    if (dup2Fd(captureFd, stderrFd) < 0) {
        closeFd(savedFd);
        std::fclose(capture);
        return {};
    }

    fn();

    std::fflush(stderr);
    std::cerr.flush();
    dup2Fd(savedFd, stderrFd);
    closeFd(savedFd);

    std::rewind(capture);
    std::string out;
    std::array<char, 256> buffer{};
    while (true) {
        const std::size_t n = std::fread(buffer.data(), 1, buffer.size(), capture);
        if (n == 0)
            break;
        out.append(buffer.data(), n);
    }
    std::fclose(capture);
    return out;
}

} // namespace

TEST(X86BackendRegressions, VoidReturnClearsIntegerReturnRegister) {
    ILBlock entry{};
    entry.name = "entry";
    entry.instrs = {op("ret", {})};

    ILFunction fn{};
    fn.name = "main";
    fn.blocks = {entry};

    const CodegenResult result = compile(fn);
    if (!result.errors.empty()) {
        std::cerr << result.errors << '\n';
    }
    ASSERT_TRUE(result.errors.empty());
    EXPECT_TRUE(containsRegex(result.asmText, R"(xorl\s+%eax,\s*%eax)"));
}

TEST(X86BackendRegressions, ConstNullLoadMaterializesBaseAndEmitsMemoryRead) {
    ILBlock entry{};
    entry.name = "entry";
    entry.instrs = {op("const_null", {}, 0, ILValue::Kind::PTR),
                    op("load", {val(ILValue::Kind::PTR, 0)}, 1, ILValue::Kind::I64),
                    op("ret", {val(ILValue::Kind::I64, 1)})};

    ILFunction fn{};
    fn.name = "null_load";
    fn.blocks = {entry};

    const CodegenResult result = compile(fn);
    if (!result.errors.empty()) {
        std::cerr << result.errors << '\n';
    }
    ASSERT_TRUE(result.errors.empty());
    EXPECT_TRUE(result.asmText.find("xorl ") != std::string::npos ||
                containsRegex(result.asmText, R"(movq\s+\$0,\s*%r[a-z0-9]+)"));
    EXPECT_TRUE(containsRegex(result.asmText, R"(movq\s+(?:0)?\(%r[a-z0-9]+\),\s*%r[a-z0-9]+)"));
}

TEST(X86BackendRegressions, ConstNullStoreMaterializesBaseAndEmitsMemoryWrite) {
    ILBlock entry{};
    entry.name = "entry";
    entry.instrs = {op("const_null", {}, 0, ILValue::Kind::PTR),
                    op("store", {val(ILValue::Kind::PTR, 0), imm(7)}),
                    op("ret", {imm(0)})};

    ILFunction fn{};
    fn.name = "null_store";
    fn.blocks = {entry};

    const CodegenResult result = compile(fn);
    if (!result.errors.empty()) {
        std::cerr << result.errors << '\n';
    }
    ASSERT_TRUE(result.errors.empty());
    EXPECT_TRUE(result.asmText.find("xorl ") != std::string::npos ||
                containsRegex(result.asmText, R"(movq\s+\$0,\s*%r[a-z0-9]+)"));
    EXPECT_TRUE(containsRegex(result.asmText, R"(movq\s+%r[a-z0-9]+,\s*(?:0)?\(%r[a-z0-9]+\))"));
}

TEST(X86BackendRegressions, IdxChkNormalizesNonZeroLowerBound) {
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0};
    entry.paramKinds = {ILValue::Kind::I64};
    entry.instrs = {
        op("idx_chk", {val(ILValue::Kind::I64, 0), imm(10), imm(20)}, 1, ILValue::Kind::I64),
        op("ret", {val(ILValue::Kind::I64, 1)})};

    ILFunction fn{};
    fn.name = "idxchk_norm";
    fn.blocks = {entry};

    const CodegenResult result = compile(fn);
    if (!result.errors.empty()) {
        std::cerr << result.errors << '\n';
    }
    ASSERT_TRUE(result.errors.empty());
    EXPECT_TRUE(result.asmText.find("addq $-10") != std::string::npos ||
                result.asmText.find("subq $10") != std::string::npos);
    EXPECT_TRUE(result.asmText.find("jl ") != std::string::npos);
    EXPECT_TRUE(result.asmText.find("jge ") != std::string::npos);
    EXPECT_NE(result.asmText.find("rt_trap_raise_error"), std::string::npos);
}

// trap.from_err lowers to a plain call of rt_trap_raise_error; the block must
// still end in a terminator (the MIR verifier's FALLOFF rule found a function
// ending in the bare call), so the lowering appends the same defensive UD2 the
// inline trap emitters use.
TEST(X86BackendRegressions, NoReturnRuntimeCallIsFollowedByUd2) {
    ILBlock entry{};
    entry.name = "entry";
    entry.instrs = {op("call", {label("rt_trap_raise_error"), imm(9)})};

    ILFunction fn{};
    fn.name = "raise_only";
    fn.blocks = {entry};

    const CodegenResult result = compile(fn);
    if (!result.errors.empty()) {
        std::cerr << result.errors << '\n';
    }
    ASSERT_TRUE(result.errors.empty());
    const auto callAt = result.asmText.find("rt_trap_raise_error");
    ASSERT_NE(callAt, std::string::npos);
    EXPECT_NE(result.asmText.find("ud2", callAt), std::string::npos);
}

TEST(X86BackendRegressions, IdxChkUsesSignedChecksForNegativeLowerBound) {
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0};
    entry.paramKinds = {ILValue::Kind::I64};
    entry.instrs = {
        op("idx_chk", {val(ILValue::Kind::I64, 0), imm(-5), imm(5)}, 1, ILValue::Kind::I64),
        op("ret", {val(ILValue::Kind::I64, 1)})};

    ILFunction fn{};
    fn.name = "idxchk_signed";
    fn.blocks = {entry};

    const CodegenResult result = compile(fn);
    if (!result.errors.empty()) {
        std::cerr << result.errors << '\n';
    }
    ASSERT_TRUE(result.errors.empty());
    EXPECT_TRUE(result.asmText.find("addq $5") != std::string::npos);
    EXPECT_TRUE(result.asmText.find("jl ") != std::string::npos);
    EXPECT_TRUE(result.asmText.find("jge ") != std::string::npos);
    EXPECT_TRUE(result.asmText.find("jb ") == std::string::npos);
    EXPECT_TRUE(result.asmText.find("jae ") == std::string::npos);
    EXPECT_NE(result.asmText.find("rt_trap_raise_error"), std::string::npos);
}

TEST(X86BackendRegressions, CheckedFpToSiUsesRoundEvenAndStructuredTrap) {
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0};
    entry.paramKinds = {ILValue::Kind::F64};
    entry.instrs = {op("fptosi_chk", {val(ILValue::Kind::F64, 0)}, 1, ILValue::Kind::I64),
                    op("ret", {val(ILValue::Kind::I64, 1)})};

    ILFunction fn{};
    fn.name = "checked_fptosi";
    fn.blocks = {entry};

    const CodegenResult result = compile(fn);
    ASSERT_TRUE(result.errors.empty());
    EXPECT_NE(result.asmText.find("rt_round_even"), std::string::npos);
    EXPECT_NE(result.asmText.find("rt_trap_raise_error"), std::string::npos);
    EXPECT_NE(result.asmText.find(".Lfptosi_chk_invalid_"), std::string::npos);
    EXPECT_NE(result.asmText.find(".Lfptosi_chk_ovf_"), std::string::npos);
}

TEST(X86BackendRegressions, CheckedFpToUiUsesUpperBoundCheckAndStructuredTrap) {
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0};
    entry.paramKinds = {ILValue::Kind::F64};
    entry.instrs = {op("fptoui", {val(ILValue::Kind::F64, 0)}, 1, ILValue::Kind::I64),
                    op("ret", {val(ILValue::Kind::I64, 1)})};

    ILFunction fn{};
    fn.name = "checked_fptoui";
    fn.blocks = {entry};

    const CodegenResult result = compile(fn);
    ASSERT_TRUE(result.errors.empty());
    EXPECT_NE(result.asmText.find("rt_round_even"), std::string::npos);
    EXPECT_NE(result.asmText.find("rt_trap_raise_error"), std::string::npos);
    EXPECT_NE(result.asmText.find(".Lfptoui_invalid_"), std::string::npos);
    EXPECT_NE(result.asmText.find(".Lfptoui_ovf_"), std::string::npos);
}

TEST(X86BackendRegressions, FptosiTruncatingConversionChecksNaNAndRange) {
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0};
    entry.paramKinds = {ILValue::Kind::F64};
    entry.instrs = {op("fptosi", {val(ILValue::Kind::F64, 0)}, 1, ILValue::Kind::I64),
                    op("ret", {val(ILValue::Kind::I64, 1)})};

    ILFunction fn{};
    fn.name = "fptosi_checked";
    fn.blocks = {entry};

    const CodegenResult result = compile(fn);
    ASSERT_TRUE(result.errors.empty());
    EXPECT_NE(result.asmText.find(".Lfptosi_invalid_"), std::string::npos);
    EXPECT_NE(result.asmText.find(".Lfptosi_ovf_"), std::string::npos);
    EXPECT_NE(result.asmText.find("rt_trap_raise_error"), std::string::npos);
    EXPECT_NE(result.asmText.find("cvttsd2siq"), std::string::npos);
}

TEST(X86BackendRegressions, CheckedSignedNarrowEmitsWidthCheck) {
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0};
    entry.paramKinds = {ILValue::Kind::I64};
    entry.instrs = {
        opBits("si_narrow_chk", {val(ILValue::Kind::I64, 0)}, 1, ILValue::Kind::I64, 16),
        op("ret", {val(ILValue::Kind::I64, 1)})};

    ILFunction fn{};
    fn.name = "narrow_i16";
    fn.blocks = {entry};

    const CodegenResult result = compile(fn);
    ASSERT_TRUE(result.errors.empty());
    EXPECT_NE(result.asmText.find("shlq $48"), std::string::npos);
    EXPECT_NE(result.asmText.find("sarq $48"), std::string::npos);
    EXPECT_NE(result.asmText.find(".Lnarrow_chk_trap_"), std::string::npos);
    EXPECT_NE(result.asmText.find("rt_trap_raise_error"), std::string::npos);
}

TEST(X86BackendRegressions, CheckedNarrowLabelsAreSplitBeforeRegisterAllocation) {
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0};
    entry.paramKinds = {ILValue::Kind::I64};
    entry.instrs = {
        opBits("si_narrow_chk", {val(ILValue::Kind::I64, 0)}, 1, ILValue::Kind::I64, 16),
        op("ret", {val(ILValue::Kind::I64, 1)})};

    ILFunction fn{};
    fn.name = "narrow_cfg";
    fn.blocks = {entry};

    ILModule module{};
    module.funcs = {fn};

    AsmEmitter::RoDataPool roData;
    std::vector<MFunction> mir;
    std::vector<FrameInfo> frames;
    std::string errors;
    CodegenOptions options;
    ASSERT_TRUE(legalizeModuleToMIR(module, win64Target(), options, roData, mir, frames, errors));
    if (!errors.empty()) {
        std::cerr << errors << '\n';
    }
    ASSERT_TRUE(errors.empty());
    ASSERT_EQ(mir.size(), 1u);

    bool foundInlineLabel = false;
    bool foundTrapBlock = false;
    bool foundDoneBlock = false;
    bool trapEndsInUd2 = false;
    for (const auto &block : mir.front().blocks) {
        foundTrapBlock =
            foundTrapBlock || block.label.find(".Lnarrow_chk_trap_") != std::string::npos;
        foundDoneBlock =
            foundDoneBlock || block.label.find(".Lnarrow_chk_done_") != std::string::npos;
        foundInlineLabel = foundInlineLabel || blockContainsOpcode(block, MOpcode::LABEL);
        if (block.label.find(".Lnarrow_chk_trap_") != std::string::npos &&
            !block.instructions.empty()) {
            trapEndsInUd2 = block.instructions.back().opcode == MOpcode::UD2;
        }
    }

    EXPECT_TRUE(foundTrapBlock);
    EXPECT_TRUE(foundDoneBlock);
    EXPECT_TRUE(trapEndsInUd2);
    EXPECT_FALSE(foundInlineLabel);
}

TEST(X86BackendRegressions, SubWidthCheckedAddEmitsRangeCheck) {
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0, 1};
    entry.paramKinds = {ILValue::Kind::I64, ILValue::Kind::I64};
    entry.instrs = {opBits("iadd.ovf",
                           {val(ILValue::Kind::I64, 0), val(ILValue::Kind::I64, 1)},
                           2,
                           ILValue::Kind::I64,
                           16),
                    op("ret", {val(ILValue::Kind::I64, 2)})};

    ILFunction fn{};
    fn.name = "iadd_i16";
    fn.blocks = {entry};

    const CodegenResult result = compile(fn);
    ASSERT_TRUE(result.errors.empty());
    // The 16-bit fast path checks overflow via native 16-bit flags: addw sets
    // OF at the target width, jo reaches the trap, and movswq restores the
    // canonical sign-extended form. The widen/shift range check is gone.
    EXPECT_NE(result.asmText.find("addw"), std::string::npos);
    EXPECT_NE(result.asmText.find("jo "), std::string::npos);
    EXPECT_NE(result.asmText.find("movswq"), std::string::npos);
    EXPECT_EQ(result.asmText.find("shlq $48"), std::string::npos);
    EXPECT_NE(result.asmText.find(".Liadd_ovf16_trap_"), std::string::npos);
    EXPECT_NE(result.asmText.find("rt_trap_raise_error"), std::string::npos);
}

TEST(X86BackendRegressions, CompareBranchUsesFlagsDirectly) {
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0, 1};
    entry.paramKinds = {ILValue::Kind::I64, ILValue::Kind::I64};
    entry.instrs = {op("icmp_ne",
                       {val(ILValue::Kind::I64, 0), val(ILValue::Kind::I64, 1)},
                       2,
                       ILValue::Kind::I1),
                    op("cbr", {val(ILValue::Kind::I1, 2), label("yes"), label("no")})};

    ILBlock yes{};
    yes.name = "yes";
    yes.instrs = {op("ret", {imm(7)})};

    ILBlock no{};
    no.name = "no";
    no.instrs = {op("ret", {imm(13)})};

    ILFunction fn{};
    fn.name = "cmp_branch";
    fn.blocks = {entry, yes, no};

    const CodegenResult result = compile(fn);
    if (!result.errors.empty()) {
        std::cerr << result.errors << '\n';
    }
    ASSERT_TRUE(result.errors.empty());
    EXPECT_TRUE(result.asmText.find("cmpq") != std::string::npos);
    EXPECT_TRUE(result.asmText.find("jne ") != std::string::npos);
    EXPECT_TRUE(result.asmText.find("setne ") == std::string::npos);
    EXPECT_TRUE(result.asmText.find("movzbq ") == std::string::npos);
    EXPECT_TRUE(result.asmText.find("testq ") == std::string::npos);
}

TEST(X86BackendRegressions, CompareBranchFoldAcceptsExistingMovzx32) {
    MFunction fn{};
    fn.name = "cmp_branch_movzx32";

    MBasicBlock entry{};
    entry.label = ".L_cmp_branch_movzx32_entry";
    const Operand lhs = makeVRegOperand(RegClass::GPR, 1);
    const Operand rhs = makeVRegOperand(RegClass::GPR, 2);
    const Operand flag = makeVRegOperand(RegClass::GPR, 3);
    entry.instructions = {
        MInstr::make(MOpcode::CMPrr, {lhs, rhs}),
        MInstr::make(MOpcode::SETcc, {makeImmOperand(1), flag}),
        MInstr::make(MOpcode::MOVZXrr32, {flag, flag}),
        MInstr::make(MOpcode::TESTrr, {flag, flag}),
        MInstr::make(MOpcode::JCC, {makeImmOperand(1), makeLabelOperand(".L_yes")})};
    fn.blocks.push_back(std::move(entry));

    ISel isel(sysvTarget());
    isel.lowerCompareAndBranch(fn);

    const auto &block = fn.blocks.front();
    ASSERT_EQ(block.instructions.size(), 2u);
    EXPECT_EQ(block.instructions[0].opcode, MOpcode::CMPrr);
    EXPECT_EQ(block.instructions[1].opcode, MOpcode::JCC);
    EXPECT_FALSE(blockContainsOpcode(block, MOpcode::SETcc));
    EXPECT_FALSE(blockContainsOpcode(block, MOpcode::MOVZXrr32));
    EXPECT_FALSE(blockContainsOpcode(block, MOpcode::TESTrr));
}

TEST(X86BackendRegressions, SetccWithExistingMovzx32StillInsertsMovzx8) {
    MFunction fn{};
    fn.name = "setcc_existing_movzx32";

    MBasicBlock entry{};
    entry.label = ".L_setcc_existing_movzx32_entry";
    const Operand flag = makeVRegOperand(RegClass::GPR, 1);
    entry.instructions = {MInstr::make(MOpcode::SETcc, {makeImmOperand(1), flag}),
                          MInstr::make(MOpcode::MOVZXrr32, {flag, flag})};
    fn.blocks.push_back(std::move(entry));

    ISel isel(sysvTarget());
    isel.lowerCompareAndBranch(fn);

    const auto &block = fn.blocks.front();
    ASSERT_EQ(block.instructions.size(), 3u);
    EXPECT_EQ(countOpcode(block, MOpcode::MOVZXrr8), 1u);
    EXPECT_EQ(countOpcode(block, MOpcode::MOVZXrr32), 1u);
}

TEST(X86BackendRegressions, CompareLoweringEmitsByteZeroExtendAfterSetcc) {
    AsmEmitter::RoDataPool roData;
    LowerILToMIR lowering(sysvTarget(), roData);

    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0};
    entry.paramKinds = {ILValue::Kind::I64};
    entry.instrs = {op("icmp_ne", {val(ILValue::Kind::I64, 0), imm(0)}, 1, ILValue::Kind::I1),
                    op("ret", {imm(0)})};

    ILFunction fn{};
    fn.name = "icmp_byte_zext";
    fn.blocks = {entry};

    const MFunction mir = lowering.lower(fn);
    ASSERT_FALSE(mir.blocks.empty());
    const auto &instrs = mir.blocks.front().instructions;
    auto setccIt = std::find_if(instrs.begin(), instrs.end(), [](const MInstr &instr) {
        return instr.opcode == MOpcode::SETcc;
    });
    ASSERT_TRUE(setccIt != instrs.end());
    const auto setccIndex = static_cast<std::size_t>(std::distance(instrs.begin(), setccIt));
    ASSERT_LT(setccIndex + 1, instrs.size());
    EXPECT_EQ(instrs[setccIndex + 1].opcode, MOpcode::MOVZXrr8);
}

TEST(X86BackendRegressions, NanSafeFcmpNormalizesSetccBytesBeforeBooleanCombine) {
    AsmEmitter::RoDataPool roData;
    LowerILToMIR lowering(sysvTarget(), roData);

    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0, 1};
    entry.paramKinds = {ILValue::Kind::F64, ILValue::Kind::F64};
    entry.instrs = {op("fcmp_eq",
                       {val(ILValue::Kind::F64, 0), val(ILValue::Kind::F64, 1)},
                       2,
                       ILValue::Kind::I1),
                    op("fcmp_ne",
                       {val(ILValue::Kind::F64, 0), val(ILValue::Kind::F64, 1)},
                       3,
                       ILValue::Kind::I1),
                    op("ret", {imm(0)})};

    ILFunction fn{};
    fn.name = "fcmp_byte_zext";
    fn.blocks = {entry};

    const MFunction mir = lowering.lower(fn);
    ASSERT_FALSE(mir.blocks.empty());
    const auto &block = mir.blocks.front();
    EXPECT_EQ(countOpcode(block, MOpcode::SETcc), 4u);
    EXPECT_GE(countOpcode(block, MOpcode::MOVZXrr8), 4u);

    for (std::size_t idx = 0; idx + 1 < block.instructions.size(); ++idx) {
        if (block.instructions[idx].opcode == MOpcode::SETcc) {
            EXPECT_EQ(block.instructions[idx + 1].opcode, MOpcode::MOVZXrr8);
        }
    }
}

TEST(X86BackendRegressions, PreRegAllocCopyForwardingStopsAtTrapBoundary) {
    MFunction fn{};
    fn.name = "pre_ra_ud2_boundary";

    MBasicBlock entry{};
    entry.label = ".L_pre_ra_ud2_boundary";
    const Operand src = makeVRegOperand(RegClass::GPR, 1);
    const Operand copy = makeVRegOperand(RegClass::GPR, 2);
    const Operand dst = makeVRegOperand(RegClass::GPR, 3);
    entry.instructions = {MInstr::make(MOpcode::MOVrr, {copy, src}),
                          MInstr::make(MOpcode::UD2),
                          MInstr::make(MOpcode::ADDrr, {dst, copy})};
    fn.blocks.push_back(std::move(entry));

    EXPECT_EQ(runPreRegAllocOpt(fn), 0u);
    ASSERT_EQ(fn.blocks.front().instructions.size(), 3u);
    EXPECT_EQ(fn.blocks.front().instructions.front().opcode, MOpcode::MOVrr);
}

TEST(X86BackendRegressions, ConditionalBlockArgsUseDedicatedEdgeBlocks) {
    AsmEmitter::RoDataPool roData;
    LowerILToMIR lowering(sysvTarget(), roData);

    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0};
    entry.paramKinds = {ILValue::Kind::I64};
    entry.instrs = {op("icmp_ne", {val(ILValue::Kind::I64, 0), imm(0)}, 1, ILValue::Kind::I1),
                    op("cbr", {val(ILValue::Kind::I1, 1), label("yes"), label("no")})};
    entry.terminatorEdges = {ILBlock::EdgeArg{.to = "yes", .argIds = {-1}, .argValues = {imm(7)}},
                             ILBlock::EdgeArg{.to = "no", .argIds = {-1}, .argValues = {imm(13)}}};

    ILBlock yes{};
    yes.name = "yes";
    yes.paramIds = {2};
    yes.paramKinds = {ILValue::Kind::I64};
    yes.instrs = {op("ret", {val(ILValue::Kind::I64, 2)})};

    ILBlock no{};
    no.name = "no";
    no.paramIds = {3};
    no.paramKinds = {ILValue::Kind::I64};
    no.instrs = {op("ret", {val(ILValue::Kind::I64, 3)})};

    ILFunction fn{};
    fn.name = "cbr_edges";
    fn.blocks = {entry, yes, no};

    const MFunction mir = lowering.lower(fn);
    ASSERT_EQ(mir.blocks.size(), 5u);

    const auto entryIt =
        std::find_if(mir.blocks.begin(), mir.blocks.end(), [](const MBasicBlock &bb) {
            return bb.label == ".L_cbr_edges_entry";
        });
    ASSERT_TRUE(entryIt != mir.blocks.end());

    std::size_t edgeBlockCount = 0;
    for (const auto &bb : mir.blocks) {
        if (bb.label.find(".edge") == std::string::npos)
            continue;
        ++edgeBlockCount;
        EXPECT_TRUE(blockContainsOpcode(bb, MOpcode::PX_COPY));
        ASSERT_FALSE(bb.instructions.empty());
        EXPECT_EQ(bb.instructions.back().opcode, MOpcode::JMP);
        const OpLabel *target = jumpTarget(bb.instructions.back());
        ASSERT_TRUE(target != nullptr);
        EXPECT_TRUE(target->name == ".L_cbr_edges_yes" || target->name == ".L_cbr_edges_no");
    }
    EXPECT_EQ(edgeBlockCount, 2u);

    bool branchesViaHelper = false;
    for (const auto &instr : entryIt->instructions) {
        if (instr.opcode != MOpcode::JCC && instr.opcode != MOpcode::JMP)
            continue;
        const OpLabel *target = jumpTarget(instr);
        if (target && target->name.find(".edge") != std::string::npos)
            branchesViaHelper = true;
    }
    EXPECT_TRUE(branchesViaHelper);
}

TEST(X86BackendRegressions, SwitchBlockArgsUseDedicatedEdgeBlocks) {
    AsmEmitter::RoDataPool roData;
    LowerILToMIR lowering(sysvTarget(), roData);

    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0};
    entry.paramKinds = {ILValue::Kind::I64};
    entry.instrs = {op("switch_i32",
                       {val(ILValue::Kind::I64, 0),
                        imm(1),
                        label("case1"),
                        imm(2),
                        label("case2"),
                        label("def")})};
    entry.terminatorEdges = {
        ILBlock::EdgeArg{.to = "def", .argIds = {-1}, .argValues = {imm(33)}},
        ILBlock::EdgeArg{.to = "case1", .argIds = {-1}, .argValues = {imm(11)}},
        ILBlock::EdgeArg{.to = "case2", .argIds = {-1}, .argValues = {imm(22)}}};

    ILBlock case1{};
    case1.name = "case1";
    case1.paramIds = {1};
    case1.paramKinds = {ILValue::Kind::I64};
    case1.instrs = {op("ret", {val(ILValue::Kind::I64, 1)})};

    ILBlock case2{};
    case2.name = "case2";
    case2.paramIds = {2};
    case2.paramKinds = {ILValue::Kind::I64};
    case2.instrs = {op("ret", {val(ILValue::Kind::I64, 2)})};

    ILBlock def{};
    def.name = "def";
    def.paramIds = {3};
    def.paramKinds = {ILValue::Kind::I64};
    def.instrs = {op("ret", {val(ILValue::Kind::I64, 3)})};

    ILFunction fn{};
    fn.name = "switch_edges";
    fn.blocks = {entry, case1, case2, def};

    const MFunction mir = lowering.lower(fn);
    ASSERT_EQ(mir.blocks.size(), 7u);

    const auto entryIt =
        std::find_if(mir.blocks.begin(), mir.blocks.end(), [](const MBasicBlock &bb) {
            return bb.label == ".L_switch_edges_entry";
        });
    ASSERT_TRUE(entryIt != mir.blocks.end());

    std::size_t edgeBlockCount = 0;

    struct EdgeBlockInfo {
        std::string label;
        std::string target;
        int64_t immediate = 0;
    };

    std::vector<EdgeBlockInfo> edgeBlocks;
    for (const auto &bb : mir.blocks) {
        if (bb.label.find(".edge") == std::string::npos)
            continue;
        ++edgeBlockCount;
        EXPECT_TRUE(blockContainsOpcode(bb, MOpcode::PX_COPY));
        ASSERT_FALSE(bb.instructions.empty());
        EXPECT_EQ(bb.instructions.back().opcode, MOpcode::JMP);
        const OpLabel *target = jumpTarget(bb.instructions.back());
        ASSERT_TRUE(target != nullptr);

        int64_t immediate = 0;
        bool foundImmediate = false;
        for (const auto &instr : bb.instructions) {
            if (instr.opcode != MOpcode::MOVri || instr.operands.size() < 2)
                continue;
            const auto *immOp = std::get_if<OpImm>(&instr.operands[1]);
            if (immOp) {
                immediate = immOp->val;
                foundImmediate = true;
            }
        }
        ASSERT_TRUE(foundImmediate);
        edgeBlocks.push_back({bb.label, target->name, immediate});
    }
    EXPECT_EQ(edgeBlockCount, 3u);

    auto assertEdge = [&](const std::string &helperLabel,
                          const std::string &expectedTarget,
                          int64_t expectedImmediate) {
        const auto edgeIt =
            std::find_if(edgeBlocks.begin(), edgeBlocks.end(), [&](const EdgeBlockInfo &info) {
                return info.label == helperLabel;
            });
        ASSERT_TRUE(edgeIt != edgeBlocks.end());
        EXPECT_EQ(edgeIt->target, expectedTarget);
        EXPECT_EQ(edgeIt->immediate, expectedImmediate);
    };

    std::string case1Edge;
    std::string case2Edge;
    std::string defaultEdge;
    for (std::size_t i = 0; i < entryIt->instructions.size(); ++i) {
        const auto &instr = entryIt->instructions[i];
        if (instr.opcode == MOpcode::CMPri && instr.operands.size() >= 2) {
            const auto *caseValue = std::get_if<OpImm>(&instr.operands[1]);
            ASSERT_TRUE(caseValue != nullptr);
            ASSERT_LT(i + 1, entryIt->instructions.size());
            const OpLabel *target = jumpTarget(entryIt->instructions[i + 1]);
            ASSERT_TRUE(target != nullptr);
            if (caseValue->val == 1) {
                case1Edge = target->name;
            } else if (caseValue->val == 2) {
                case2Edge = target->name;
            }
        } else if (instr.opcode == MOpcode::JMP) {
            const OpLabel *target = jumpTarget(instr);
            ASSERT_TRUE(target != nullptr);
            defaultEdge = target->name;
        }
    }

    ASSERT_FALSE(case1Edge.empty());
    ASSERT_FALSE(case2Edge.empty());
    ASSERT_FALSE(defaultEdge.empty());
    assertEdge(case1Edge, ".L_switch_edges_case1", 11);
    assertEdge(case2Edge, ".L_switch_edges_case2", 22);
    assertEdge(defaultEdge, ".L_switch_edges_def", 33);
}

TEST(X86BackendRegressions, LargerSwitchUsesBalancedDecisionLabels) {
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0};
    entry.paramKinds = {ILValue::Kind::I64};
    entry.instrs = {op("switch_i32",
                       {val(ILValue::Kind::I64, 0),
                        imm(0),
                        label("c0"),
                        imm(10),
                        label("c1"),
                        imm(20),
                        label("c2"),
                        imm(30),
                        label("c3"),
                        imm(40),
                        label("c4"),
                        imm(50),
                        label("c5"),
                        imm(60),
                        label("c6"),
                        imm(70),
                        label("c7"),
                        label("def")})};

    std::vector<ILBlock> blocks;
    blocks.push_back(entry);
    for (int i = 0; i < 8; ++i) {
        ILBlock caseBlock{};
        caseBlock.name = "c" + std::to_string(i);
        caseBlock.instrs = {op("ret", {imm(100 + i)})};
        blocks.push_back(std::move(caseBlock));
    }
    ILBlock def{};
    def.name = "def";
    def.instrs = {op("ret", {imm(0)})};
    blocks.push_back(std::move(def));

    ILFunction fn{};
    fn.name = "switch_tree";
    fn.blocks = std::move(blocks);

    const CodegenResult result = compile(fn);
    ASSERT_TRUE(result.errors.empty());
    // Sparse cases (density below the jump-table threshold) keep the
    // balanced compare tree.
    EXPECT_NE(result.asmText.find(".Lswitch_left_"), std::string::npos);
    EXPECT_NE(result.asmText.find(".Lswitch_right_"), std::string::npos);
    EXPECT_EQ(result.asmText.find(".Ljt_"), std::string::npos);
}

TEST(X86BackendRegressions, DenseSwitchUsesJumpTable) {
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0};
    entry.paramKinds = {ILValue::Kind::I64};

    std::vector<ILValue> switchOps{val(ILValue::Kind::I64, 0)};
    for (int i = 0; i < 8; ++i) {
        switchOps.push_back(imm(i));
        switchOps.push_back(label(("c" + std::to_string(i)).c_str()));
    }
    switchOps.push_back(label("def"));
    entry.instrs = {op("switch_i32", switchOps)};

    std::vector<ILBlock> blocks;
    blocks.push_back(entry);
    for (int i = 0; i < 8; ++i) {
        ILBlock caseBlock{};
        caseBlock.name = "c" + std::to_string(i);
        caseBlock.instrs = {op("ret", {imm(100 + i)})};
        blocks.push_back(std::move(caseBlock));
    }
    ILBlock def{};
    def.name = "def";
    def.instrs = {op("ret", {imm(0)})};
    blocks.push_back(std::move(def));

    ILFunction fn{};
    fn.name = "switch_dense";
    fn.blocks = std::move(blocks);

    const CodegenResult result = compile(fn);
    ASSERT_TRUE(result.errors.empty());
    // Contiguous cases dispatch through the inline jump table: one unsigned
    // bounds check, the table label, and anchor-relative entries.
    EXPECT_NE(result.asmText.find(".Ljt_"), std::string::npos);
    EXPECT_NE(result.asmText.find("movslq"), std::string::npos);
    EXPECT_NE(result.asmText.find("jmp *"), std::string::npos);
    EXPECT_NE(result.asmText.find(".long "), std::string::npos);
    EXPECT_EQ(result.asmText.find(".Lswitch_left_"), std::string::npos);
}

TEST(X86BackendRegressions, CheckedSignedDivisionIncludesOverflowTrap) {
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0, 1};
    entry.paramKinds = {ILValue::Kind::I64, ILValue::Kind::I64};
    entry.instrs = {op("sdiv.chk0",
                       {val(ILValue::Kind::I64, 0), val(ILValue::Kind::I64, 1)},
                       2,
                       ILValue::Kind::I64),
                    op("ret", {val(ILValue::Kind::I64, 2)})};

    ILFunction fn{};
    fn.name = "checked_div";
    fn.blocks = {entry};

    const CodegenResult result = compile(fn);
    ASSERT_TRUE(result.errors.empty());
    EXPECT_NE(result.asmText.find("rt_trap_ovf"), std::string::npos);
    EXPECT_NE(result.asmText.find("cmpq $-1"), std::string::npos);
}

TEST(X86BackendRegressions, CheckedSignedRemainderZerosMinOverflowPath) {
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0, 1};
    entry.paramKinds = {ILValue::Kind::I64, ILValue::Kind::I64};
    entry.instrs = {op("srem.chk0",
                       {val(ILValue::Kind::I64, 0), val(ILValue::Kind::I64, 1)},
                       2,
                       ILValue::Kind::I64),
                    op("ret", {val(ILValue::Kind::I64, 2)})};

    ILFunction fn{};
    fn.name = "checked_rem";
    fn.blocks = {entry};

    const CodegenResult result = compile(fn);
    ASSERT_TRUE(result.errors.empty());
    EXPECT_EQ(result.asmText.find("rt_trap_ovf"), std::string::npos);
    EXPECT_TRUE(
        result.asmText.find("movq $0") != std::string::npos ||
        containsRegex(result.asmText,
                      R"(xor[lq]\s+%(?:e[a-z]+|r[a-z0-9]+d?),\s*%(?:e[a-z]+|r[a-z0-9]+d?))"));
}

TEST(X86BackendRegressions, AssemblyHonorsTargetPlatformSections) {
    ILBlock entry{};
    entry.name = "entry";
    entry.instrs = {op("const_f64", {immF64(3.25)}, 0, ILValue::Kind::F64),
                    op("ret", {val(ILValue::Kind::F64, 0)}, -1, ILValue::Kind::F64)};

    ILFunction fn{};
    fn.name = "const_f64_target";
    fn.blocks = {entry};

    CodegenOptions linuxOpts{};
    linuxOpts.targetPlatform = CodegenOptions::TargetPlatform::Linux;
    const CodegenResult linux = compile(fn, linuxOpts);
    ASSERT_TRUE(linux.errors.empty());
    EXPECT_NE(linux.asmText.find(".section .rodata"), std::string::npos);
    EXPECT_NE(linux.asmText.find(".type"), std::string::npos);
    EXPECT_NE(linux.asmText.find(".note.GNU-stack"), std::string::npos);

    CodegenOptions darwinOpts{};
    darwinOpts.targetPlatform = CodegenOptions::TargetPlatform::Darwin;
    const CodegenResult darwin = compile(fn, darwinOpts);
    ASSERT_TRUE(darwin.errors.empty());
    EXPECT_NE(darwin.asmText.find(".section __TEXT,__const"), std::string::npos);
    EXPECT_EQ(darwin.asmText.find(".type"), std::string::npos);
    EXPECT_EQ(darwin.asmText.find(".note.GNU-stack"), std::string::npos);

    CodegenOptions windowsOpts{};
    windowsOpts.targetPlatform = CodegenOptions::TargetPlatform::Windows;
    const CodegenResult windows = compile(fn, windowsOpts);
    ASSERT_TRUE(windows.errors.empty());
    EXPECT_NE(windows.asmText.find(".section .rdata,\"dr\""), std::string::npos);
    EXPECT_EQ(windows.asmText.find(".type"), std::string::npos);
}

TEST(X86BackendRegressions, BinaryEmissionHonorsTargetPlatformSymbolMangling) {
    ILBlock entry{};
    entry.name = "entry";
    entry.instrs = {op("ret", {imm(0)})};

    ILFunction fn{};
    fn.name = "main";
    fn.blocks = {entry};

    CodegenOptions linuxOpts{};
    linuxOpts.targetPlatform = CodegenOptions::TargetPlatform::Linux;
    const BinaryEmitResult linux = compileBinary(fn, linuxOpts);
    ASSERT_TRUE(linux.errors.empty());
    const auto &linuxText = primaryTextSection(linux);
    EXPECT_NE(linuxText.symbols().find("main"), 0U);
    EXPECT_EQ(linuxText.symbols().find("_main"), 0U);

    CodegenOptions darwinOpts{};
    darwinOpts.targetPlatform = CodegenOptions::TargetPlatform::Darwin;
    const BinaryEmitResult darwin = compileBinary(fn, darwinOpts);
    ASSERT_TRUE(darwin.errors.empty());
    // Binary emission records canonical symbol names. Mach-O ABI underscores
    // are applied by the object writer, not stored in CodeSection.
    const auto &darwinText = primaryTextSection(darwin);
    EXPECT_NE(darwinText.symbols().find("main"), 0U);
    EXPECT_EQ(darwinText.symbols().find("_main"), 0U);
}

TEST(X86BackendRegressions, BinaryEmissionSkipsMergedTextWhenDebugLinesAreDisabled) {
    auto makeRetFn = [](std::string name, int64_t value) {
        ILBlock entry{};
        entry.name = "entry";
        entry.instrs = {op("ret", {imm(value)})};

        ILFunction fn{};
        fn.name = std::move(name);
        fn.blocks = {entry};
        return fn;
    };

    ILModule module{};
    module.funcs.push_back(makeRetFn("first_func", 1));
    module.funcs.push_back(makeRetFn("second_func", 2));

    CodegenOptions opts{};
    opts.emitDebugLines = false;
    const BinaryEmitResult result = emitModuleToBinary(module, opts);

    ASSERT_TRUE(result.errors.empty());
    EXPECT_TRUE(result.text.empty());
    ASSERT_EQ(result.textSections.size(), 2u);
    EXPECT_NE(result.textSections[0].symbols().find("first_func"), 0U);
    EXPECT_NE(result.textSections[1].symbols().find("second_func"), 0U);
}

TEST(X86BackendRegressions, UnknownOpcodeFailsAssemblyEmission) {
    MFunction fn{};
    fn.name = "bad_opcode";
    MBasicBlock entry{};
    entry.label = ".L_bad_opcode_entry";
    entry.instructions.push_back(MInstr::make(static_cast<MOpcode>(9999), {}));
    fn.blocks.push_back(std::move(entry));

    AsmEmitter::RoDataPool roData{};
    const CodegenResult result = emitMIRToAssembly({fn}, roData, sysvTarget(), {});
    EXPECT_NE(result.errors.find("unknown opcode"), std::string::npos);
    EXPECT_EQ(result.asmText.find("# unknown"), std::string::npos);
}

TEST(X86BackendRegressions, AssemblyEmitterRejectsImmediateCallTarget) {
    const MFunction fn =
        rawFunction("bad_call_imm", {MInstr::make(MOpcode::CALL, {makeImmOperand(1)})});

    const CodegenResult result = emitRawAssembly(fn);
    EXPECT_NE(result.errors.find("CALL requires a label, register, or memory target"),
              std::string::npos);
}

TEST(X86BackendRegressions, AssemblyEmitterRejectsXmmJumpTarget) {
    const Operand target = makePhysRegOperand(RegClass::XMM, static_cast<uint16_t>(PhysReg::XMM0));
    const MFunction fn = rawFunction("bad_jmp_xmm", {MInstr::make(MOpcode::JMP, {target})});

    const CodegenResult result = emitRawAssembly(fn);
    EXPECT_NE(result.errors.find("JMP requires a GPR register target"), std::string::npos);
}

TEST(X86BackendRegressions, AssemblyEmitterRejectsNonLabelConditionalBranchTarget) {
    const Operand target = makePhysRegOperand(RegClass::GPR, static_cast<uint16_t>(PhysReg::RAX));
    const MFunction fn =
        rawFunction("bad_jcc_reg", {MInstr::make(MOpcode::JCC, {makeImmOperand(1), target})});

    const CodegenResult result = emitRawAssembly(fn);
    EXPECT_NE(result.errors.find("JCC requires a label target"), std::string::npos);
}

TEST(X86BackendRegressions, AssemblyEmitterAcceptsLabelFirstConditionalBranch) {
    const MFunction fn = rawFunction(
        "label_first_jcc",
        {MInstr::make(MOpcode::JCC, {makeLabelOperand(".L_label_first_done"), makeImmOperand(1)})});

    const CodegenResult result = emitRawAssembly(fn);
    EXPECT_TRUE(result.errors.empty());
    EXPECT_NE(result.asmText.find("jne .L_label_first_done"), std::string::npos);
}

TEST(X86BackendRegressions, AssemblyEmitterRejectsConditionalBranchWithoutCondition) {
    const Operand target = makePhysRegOperand(RegClass::GPR, static_cast<uint16_t>(PhysReg::RAX));
    const MFunction fn = rawFunction(
        "bad_jcc_no_cond",
        {MInstr::make(MOpcode::JCC, {makeLabelOperand(".L_bad_jcc_no_cond_done"), target})});

    const CodegenResult result = emitRawAssembly(fn);
    EXPECT_NE(result.errors.find("JCC requires a condition code"), std::string::npos);
}

TEST(X86BackendRegressions, AssemblyEmitterRejectsRegisterLeaSource) {
    const Operand dst = makePhysRegOperand(RegClass::GPR, static_cast<uint16_t>(PhysReg::RAX));
    const Operand src = makePhysRegOperand(RegClass::GPR, static_cast<uint16_t>(PhysReg::RBX));
    const MFunction fn = rawFunction("bad_lea_reg", {MInstr::make(MOpcode::LEA, {dst, src})});

    const CodegenResult result = emitRawAssembly(fn);
    EXPECT_NE(result.errors.find("LEA requires a memory or RIP-relative source"),
              std::string::npos);
}

TEST(X86BackendRegressions, AssemblyEmitterRejectsNonClShiftCount) {
    const Operand dst = makePhysRegOperand(RegClass::GPR, static_cast<uint16_t>(PhysReg::RAX));
    const Operand count = makePhysRegOperand(RegClass::GPR, static_cast<uint16_t>(PhysReg::RDX));
    const MFunction fn =
        rawFunction("bad_shift_count", {MInstr::make(MOpcode::SHLrc, {dst, count})});

    const CodegenResult result = emitRawAssembly(fn);
    EXPECT_NE(result.errors.find("register-count shift requires RCX/CL"), std::string::npos);
}

TEST(X86BackendRegressions, AssemblyEmitterRejectsXmmSetccDestination) {
    const Operand dst = makePhysRegOperand(RegClass::XMM, static_cast<uint16_t>(PhysReg::XMM0));
    const MFunction fn =
        rawFunction("bad_setcc_xmm", {MInstr::make(MOpcode::SETcc, {makeImmOperand(0), dst})});

    const CodegenResult result = emitRawAssembly(fn);
    EXPECT_NE(result.errors.find("SETcc requires GPR or memory destination"), std::string::npos);
}

TEST(X86BackendRegressions, AssemblyEmitterAcceptsDestinationFirstSetcc) {
    const Operand dst = makePhysRegOperand(RegClass::GPR, static_cast<uint16_t>(PhysReg::RAX));
    const MFunction fn =
        rawFunction("dst_first_setcc", {MInstr::make(MOpcode::SETcc, {dst, makeImmOperand(0)})});

    const CodegenResult result = emitRawAssembly(fn);
    EXPECT_TRUE(result.errors.empty());
    EXPECT_NE(result.asmText.find("sete %al"), std::string::npos);
}

TEST(X86BackendRegressions, AssemblyEmitterRejectsSetccWithoutCondition) {
    const Operand dst = makePhysRegOperand(RegClass::GPR, static_cast<uint16_t>(PhysReg::RAX));
    const MFunction fn = rawFunction(
        "bad_setcc_no_cond",
        {MInstr::make(MOpcode::SETcc, {dst, makeLabelOperand(".L_bad_setcc_no_cond")})});

    const CodegenResult result = emitRawAssembly(fn);
    EXPECT_NE(result.errors.find("SETcc requires a condition code"), std::string::npos);
}

TEST(X86BackendRegressions, AssemblyEmitterRejectsXmmMovzxOperand) {
    const Operand dst = makePhysRegOperand(RegClass::GPR, static_cast<uint16_t>(PhysReg::RAX));
    const Operand src = makePhysRegOperand(RegClass::XMM, static_cast<uint16_t>(PhysReg::XMM0));
    const MFunction fn =
        rawFunction("bad_movzx_xmm", {MInstr::make(MOpcode::MOVZXrr8, {dst, src})});

    const CodegenResult result = emitRawAssembly(fn);
    EXPECT_NE(result.errors.find("MOVZXrr8 requires GPR operands"), std::string::npos);
}

TEST(X86BackendRegressions, SuccessfulAssemblyEmissionIsSilentOnStderr) {
    ILBlock entry{};
    entry.name = "entry";
    entry.instrs = {op("ret", {imm(7)})};

    ILFunction fn{};
    fn.name = "quiet_return";
    fn.blocks = {entry};

    CodegenResult result{};
    const std::string stderrText = captureStderr([&] { result = compile(fn); });

    ASSERT_TRUE(result.errors.empty());
    EXPECT_FALSE(result.asmText.empty());
    EXPECT_TRUE(stderrText.empty());
}

TEST(X86BackendRegressions, TrapPayloadIsForwardedToRuntime) {
    AsmEmitter::RoDataPool roData;
    LowerILToMIR lowering(sysvTarget(), roData);

    ILValue message{};
    message.kind = ILValue::Kind::STR;
    message.id = -1;
    message.str = "boom";
    message.strLen = 4;

    ILInstr trap{};
    trap.opcode = "trap";
    trap.ops = {message};

    ILBlock entry{};
    entry.name = "entry";
    entry.instrs = {trap};

    ILFunction fn{};
    fn.name = "trap_payload";
    fn.blocks = {entry};

    (void)lowering.lower(fn);
    const auto trapIt =
        std::find_if(lowering.callPlans().begin(),
                     lowering.callPlans().end(),
                     [](const CallLoweringPlan &plan) { return plan.callee == "rt_trap_string"; });
    ASSERT_TRUE(trapIt != lowering.callPlans().end());
    ASSERT_EQ(trapIt->args.size(), 1u);
    EXPECT_FALSE(trapIt->args[0].isImm);
}

TEST(X86BackendRegressions, LargeGepImmediateIsMaterializedInsteadOfTruncated) {
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0};
    entry.paramKinds = {ILValue::Kind::PTR};
    entry.instrs = {
        op("gep", {val(ILValue::Kind::PTR, 0), imm(2147483648LL)}, 1, ILValue::Kind::PTR),
        op("ret", {val(ILValue::Kind::PTR, 1)})};

    ILFunction fn{};
    fn.name = "large_gep";
    fn.blocks = {entry};

    const CodegenResult result = compile(fn);
    ASSERT_TRUE(result.errors.empty());
    EXPECT_NE(result.asmText.find("$2147483648"), std::string::npos);
    EXPECT_NE(result.asmText.find("addq"), std::string::npos);
    EXPECT_EQ(result.asmText.find("-2147483648("), std::string::npos);
}

TEST(X86BackendRegressions, LargeLoadDisplacementIsMaterializedInsteadOfTruncated) {
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0};
    entry.paramKinds = {ILValue::Kind::PTR};
    entry.instrs = {
        op("load", {val(ILValue::Kind::PTR, 0), imm(2147483648LL)}, 1, ILValue::Kind::I64),
        op("ret", {val(ILValue::Kind::I64, 1)})};

    ILFunction fn{};
    fn.name = "large_load_disp";
    fn.blocks = {entry};

    const CodegenResult result = compile(fn);
    ASSERT_TRUE(result.errors.empty());
    EXPECT_NE(result.asmText.find("$2147483648"), std::string::npos);
    EXPECT_NE(result.asmText.find("addq"), std::string::npos);
    EXPECT_EQ(result.asmText.find("-2147483648("), std::string::npos);
}

TEST(X86BackendRegressions, InvalidAllocaSizeIsRejected) {
    ILBlock entry{};
    entry.name = "entry";
    entry.instrs = {op("alloca", {imm(0)}, 0, ILValue::Kind::PTR), op("ret", {imm(0)})};

    ILFunction fn{};
    fn.name = "bad_alloca";
    fn.blocks = {entry};

    EXPECT_THROWS(compile(fn), std::runtime_error);
}

TEST(X86BackendRegressions, SelectPseudoIsLoweredBeforeEmission) {
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0};
    entry.paramKinds = {ILValue::Kind::I1};
    entry.instrs = {
        op("select", {val(ILValue::Kind::I1, 0), imm(7), imm(13)}, 1, ILValue::Kind::I64),
        op("ret", {val(ILValue::Kind::I64, 1)})};

    ILFunction fn{};
    fn.name = "select_i64";
    fn.blocks = {entry};

    const CodegenResult result = compile(fn);
    ASSERT_TRUE(result.errors.empty());
    EXPECT_NE(result.asmText.find("cmovne"), std::string::npos);
    EXPECT_EQ(result.asmText.find("SELECT_GPR"), std::string::npos);
    EXPECT_EQ(result.asmText.find("select pseudo survived"), std::string::npos);
}

TEST(X86BackendRegressions, CompareBranchFoldPreservesLiveBooleanResult) {
    MFunction fn{};
    fn.name = "cmp_bool_live";
    MBasicBlock entry{};
    entry.label = ".L_cmp_bool_live_entry";
    const Operand a = makeVRegOperand(RegClass::GPR, 0);
    const Operand b = makeVRegOperand(RegClass::GPR, 1);
    const Operand flag = makeVRegOperand(RegClass::GPR, 2);
    const Operand copy = makeVRegOperand(RegClass::GPR, 3);
    entry.instructions = {
        MInstr::make(MOpcode::CMPrr, {a, b}),
        MInstr::make(MOpcode::SETcc, {makeImmOperand(1), flag}),
        MInstr::make(MOpcode::MOVZXrr8, {flag, flag}),
        MInstr::make(MOpcode::TESTrr, {flag, flag}),
        MInstr::make(MOpcode::JCC, {makeImmOperand(1), makeLabelOperand(".L_true")}),
        MInstr::make(MOpcode::MOVrr, {copy, flag}),
        MInstr::make(MOpcode::JMP, {makeLabelOperand(".L_done")})};
    fn.blocks.push_back(std::move(entry));

    ISel isel(sysvTarget());
    isel.lowerCompareAndBranch(fn);

    const auto &instructions = fn.blocks.front().instructions;
    EXPECT_TRUE(std::any_of(instructions.begin(), instructions.end(), [](const MInstr &instr) {
        return instr.opcode == MOpcode::SETcc;
    }));
}

TEST(X86BackendRegressions, RegAllocSpillsLiveValueBeforePhysicalRdxClobber) {
    MFunction fn{};
    fn.name = "phys_rdx_clobber";

    MBasicBlock entry{};
    entry.label = ".L_phys_rdx_clobber_entry";
    const Operand v1 = makeVRegOperand(RegClass::GPR, 1);
    const Operand v2 = makeVRegOperand(RegClass::GPR, 2);
    const Operand v3 = makeVRegOperand(RegClass::GPR, 3);
    const Operand v4 = makeVRegOperand(RegClass::GPR, 4);
    entry.instructions = {
        MInstr::make(MOpcode::MOVri, {v1, makeImmOperand(1)}),
        MInstr::make(MOpcode::MOVri, {v2, makeImmOperand(2)}),
        MInstr::make(MOpcode::MOVri, {v3, makeImmOperand(3)}),
        MInstr::make(MOpcode::MOVri, {v4, makeImmOperand(4)}),
        MInstr::make(MOpcode::XORrr32,
                     {makePhysRegOperand(RegClass::GPR, static_cast<uint16_t>(PhysReg::RDX)),
                      makePhysRegOperand(RegClass::GPR, static_cast<uint16_t>(PhysReg::RDX))}),
        MInstr::make(MOpcode::ADDrr, {v1, v2}),
        MInstr::make(MOpcode::ADDrr, {v1, v3}),
        MInstr::make(MOpcode::ADDrr, {v1, v4}),
        MInstr::make(MOpcode::RET)};
    fn.blocks.push_back(std::move(entry));

    (void)allocate(fn, sysvTarget());

    const auto &instructions = fn.blocks.front().instructions;
    const auto xorIt =
        std::find_if(instructions.begin(), instructions.end(), [](const MInstr &instr) {
            if (instr.opcode != MOpcode::XORrr32 || instr.operands.empty())
                return false;
            const auto *dst = std::get_if<OpReg>(&instr.operands[0]);
            return dst && dst->isPhys && static_cast<PhysReg>(dst->idOrPhys) == PhysReg::RDX;
        });
    ASSERT_TRUE(xorIt != instructions.end());

    bool spilledRdxBeforeClobber = false;
    for (auto it = instructions.begin(); it != xorIt; ++it) {
        if (it->opcode != MOpcode::MOVrm || it->operands.size() < 2)
            continue;
        const auto *src = std::get_if<OpReg>(&it->operands[1]);
        if (src && src->isPhys && static_cast<PhysReg>(src->idOrPhys) == PhysReg::RDX) {
            spilledRdxBeforeClobber = true;
        }
    }
    EXPECT_TRUE(spilledRdxBeforeClobber);
}

TEST(X86BackendRegressions, RegAllocSpillsLiveValuesBeforeImplicitDivClobber) {
    MFunction fn{};
    fn.name = "implicit_div_clobber";

    MBasicBlock entry{};
    entry.label = ".L_implicit_div_clobber_entry";
    const Operand v1 = makeVRegOperand(RegClass::GPR, 1);
    const Operand v2 = makeVRegOperand(RegClass::GPR, 2);
    const Operand v3 = makeVRegOperand(RegClass::GPR, 3);
    const Operand v4 = makeVRegOperand(RegClass::GPR, 4);
    entry.instructions = {
        MInstr::make(MOpcode::MOVri, {v1, makeImmOperand(1)}),
        MInstr::make(MOpcode::MOVri, {v2, makeImmOperand(2)}),
        MInstr::make(MOpcode::MOVri, {v3, makeImmOperand(3)}),
        MInstr::make(MOpcode::MOVri, {v4, makeImmOperand(4)}),
        MInstr::make(MOpcode::IDIVrm,
                     {makePhysRegOperand(RegClass::GPR, static_cast<uint16_t>(PhysReg::R10))}),
        MInstr::make(MOpcode::ADDrr, {v2, v3}),
        MInstr::make(MOpcode::ADDrr, {v1, v4}),
        MInstr::make(MOpcode::RET)};
    fn.blocks.push_back(std::move(entry));

    (void)allocate(fn, sysvTarget());

    const auto &instructions = fn.blocks.front().instructions;
    const auto divIt =
        std::find_if(instructions.begin(), instructions.end(), [](const MInstr &instr) {
            return instr.opcode == MOpcode::IDIVrm;
        });
    ASSERT_TRUE(divIt != instructions.end());

    bool spilledRax = false;
    bool spilledRdx = false;
    for (auto it = instructions.begin(); it != divIt; ++it) {
        if (it->opcode != MOpcode::MOVrm || it->operands.size() < 2)
            continue;
        const auto *src = std::get_if<OpReg>(&it->operands[1]);
        if (!src || !src->isPhys)
            continue;
        spilledRax = spilledRax || static_cast<PhysReg>(src->idOrPhys) == PhysReg::RAX;
        spilledRdx = spilledRdx || static_cast<PhysReg>(src->idOrPhys) == PhysReg::RDX;
    }
    EXPECT_TRUE(spilledRax);
    EXPECT_TRUE(spilledRdx);
}

TEST(X86BackendRegressions, RegAllocDoesNotAllocateFixedScratchRegisters) {
    MFunction fn{};
    fn.name = "reserved_scratch_regs";

    MBasicBlock entry{};
    entry.label = ".L_reserved_scratch_regs_entry";
    std::vector<Operand> regs;
    for (uint16_t id = 1; id <= 12; ++id) {
        regs.push_back(makeVRegOperand(RegClass::GPR, id));
        entry.instructions.push_back(
            MInstr::make(MOpcode::MOVri, {regs.back(), makeImmOperand(id)}));
    }
    for (std::size_t idx = 1; idx < regs.size(); ++idx) {
        entry.instructions.push_back(MInstr::make(MOpcode::ADDrr, {regs[0], regs[idx]}));
    }
    entry.instructions.push_back(MInstr::make(MOpcode::RET));
    fn.blocks.push_back(std::move(entry));

    (void)allocate(fn, win64Target());

    for (const auto &block : fn.blocks) {
        for (const auto &instr : block.instructions) {
            for (const auto &operand : instr.operands) {
                const auto *reg = std::get_if<OpReg>(&operand);
                if (!reg || !reg->isPhys || reg->cls != RegClass::GPR)
                    continue;
                EXPECT_NE(static_cast<PhysReg>(reg->idOrPhys), PhysReg::R10);
                EXPECT_NE(static_cast<PhysReg>(reg->idOrPhys), PhysReg::R11);
            }
        }
    }
}

TEST(X86BackendRegressions, CoalescerPreservesSpilledMemorySourceCycles) {
    // This test exercises the coalescer's spilled-memory cycle resolution,
    // which only engages when cross-block values hold spill homes. Global
    // pinning would keep both values in callee-saved registers and bypass the
    // path under test, so disable it for this allocation.
    setenv("ZANNA_NO_GLOBAL_RA", "1", 1);

    struct EnvReset {
        ~EnvReset() {
            unsetenv("ZANNA_NO_GLOBAL_RA");
        }
    } envReset;

    MFunction fn{};
    fn.name = "px_copy_spilled_swap";

    const Operand v1 = makeVRegOperand(RegClass::GPR, 1);
    const Operand v2 = makeVRegOperand(RegClass::GPR, 2);

    MBasicBlock entry{};
    entry.label = ".L_px_entry";
    entry.instructions = {MInstr::make(MOpcode::MOVri, {v1, makeImmOperand(1)}),
                          MInstr::make(MOpcode::MOVri, {v2, makeImmOperand(2)}),
                          MInstr::make(MOpcode::JMP, {makeLabelOperand(".L_px_swap")})};

    MBasicBlock filler{};
    filler.label = ".L_px_filler";
    filler.instructions = {MInstr::make(MOpcode::RET)};

    MBasicBlock swap{};
    swap.label = ".L_px_swap";
    swap.instructions = {MInstr::make(MOpcode::PX_COPY, {v1, v2, v2, v1}),
                         MInstr::make(MOpcode::ADDrr, {v1, v2}),
                         MInstr::make(MOpcode::RET)};

    fn.blocks = {entry, filler, swap};

    (void)allocate(fn, sysvTarget());

    const auto &instructions = fn.blocks[2].instructions;
    std::vector<int64_t> loadDispsBeforeFirstStore;
    std::optional<int64_t> firstStoreDisp;
    for (const auto &instr : instructions) {
        if (instr.opcode == MOpcode::MOVrm) {
            const auto *mem = std::get_if<OpMem>(&instr.operands[0]);
            if (mem && mem->base.isPhys &&
                static_cast<PhysReg>(mem->base.idOrPhys) == PhysReg::RBP && mem->disp < 0) {
                firstStoreDisp = mem->disp;
            }
            break;
        }
        if (instr.opcode != MOpcode::MOVmr || instr.operands.size() < 2) {
            continue;
        }
        const auto *mem = std::get_if<OpMem>(&instr.operands[1]);
        if (mem && mem->base.isPhys && static_cast<PhysReg>(mem->base.idOrPhys) == PhysReg::RBP &&
            mem->disp < 0) {
            loadDispsBeforeFirstStore.push_back(mem->disp);
        }
    }
    ASSERT_TRUE(firstStoreDisp.has_value());
    EXPECT_NE(std::find(loadDispsBeforeFirstStore.begin(),
                        loadDispsBeforeFirstStore.end(),
                        *firstStoreDisp),
              loadDispsBeforeFirstStore.end());
}

TEST(X86BackendRegressions, RegAllocPreservesCallerSavedLiveOutAcrossCall) {
    MFunction fn{};
    fn.name = "call_liveout";

    MBasicBlock entry{};
    entry.label = ".L_call_liveout_entry";
    const Operand live = makeVRegOperand(RegClass::GPR, 1);
    entry.instructions = {MInstr::make(MOpcode::MOVri, {live, makeImmOperand(42)}),
                          MInstr::make(MOpcode::CALL, {makeLabelOperand("opaque_call")}),
                          MInstr::make(MOpcode::JMP, {makeLabelOperand(".L_call_liveout_join")})};

    MBasicBlock join{};
    join.label = ".L_call_liveout_join";
    const Operand tmp = makeVRegOperand(RegClass::GPR, 2);
    join.instructions = {MInstr::make(MOpcode::MOVrr, {tmp, live}), MInstr::make(MOpcode::RET)};

    fn.blocks = {entry, join};

    (void)allocate(fn, sysvTarget());

    const auto &instructions = fn.blocks.front().instructions;
    const auto callIt =
        std::find_if(instructions.begin(), instructions.end(), [](const MInstr &instr) {
            return instr.opcode == MOpcode::CALL;
        });
    ASSERT_TRUE(callIt != instructions.end());

    const auto isCallerSaved = [](PhysReg reg) {
        return reg == PhysReg::RAX || reg == PhysReg::RDI || reg == PhysReg::RSI ||
               reg == PhysReg::RDX || reg == PhysReg::RCX || reg == PhysReg::R8 ||
               reg == PhysReg::R9 || reg == PhysReg::R10 || reg == PhysReg::R11;
    };

    bool preservedBeforeCall = false;
    for (auto it = instructions.begin(); it != callIt; ++it) {
        if (it->opcode == MOpcode::MOVrr && it->operands.size() >= 2) {
            const auto *dst = std::get_if<OpReg>(&it->operands[0]);
            const auto *src = std::get_if<OpReg>(&it->operands[1]);
            if (dst && src && dst->isPhys && src->isPhys) {
                const auto dstReg = static_cast<PhysReg>(dst->idOrPhys);
                const auto srcReg = static_cast<PhysReg>(src->idOrPhys);
                preservedBeforeCall =
                    preservedBeforeCall || (isCallerSaved(srcReg) && !isCallerSaved(dstReg));
            }
        }
        if (it->opcode == MOpcode::MOVrm && it->operands.size() >= 2) {
            const auto *src = std::get_if<OpReg>(&it->operands[1]);
            if (src && src->isPhys) {
                preservedBeforeCall =
                    preservedBeforeCall || isCallerSaved(static_cast<PhysReg>(src->idOrPhys));
            }
        }
    }
    EXPECT_TRUE(preservedBeforeCall);
}

TEST(X86BackendRegressions, LivenessCfgResolvesSplitLocalSelectLabels) {
    // splitInternalLabelBlocks promotes in-block select/local labels to real
    // blocks before register allocation; liveness then sees explicit edges.
    // (Feeding liveness an in-block LABEL is now an internal compiler error —
    // the per-block allocator state machine cannot model in-block control
    // flow, so a leaked LABEL must fail loudly rather than miscompile.)
    MFunction fn{};
    fn.name = "local_branch_cfg";

    MBasicBlock entry{};
    entry.label = ".L_local_branch_cfg_entry";
    entry.instructions = {
        MInstr::make(MOpcode::JCC, {makeImmOperand(1), makeLabelOperand(".Llocal_false")}),
        MInstr::make(MOpcode::JMP, {makeLabelOperand(".Llocal_false")})};

    MBasicBlock localFalse{};
    localFalse.label = ".Llocal_false";
    localFalse.instructions = {
        MInstr::make(MOpcode::JMP, {makeLabelOperand(".L_local_branch_cfg_done")})};

    MBasicBlock accidental{};
    accidental.label = ".L_local_branch_cfg_accidental";
    accidental.instructions = {MInstr::make(MOpcode::RET)};

    MBasicBlock done{};
    done.label = ".L_local_branch_cfg_done";
    done.instructions = {MInstr::make(MOpcode::RET)};

    fn.blocks = {entry, localFalse, accidental, done};

    ra::LivenessAnalysis liveness;
    liveness.run(fn);
    const auto &entrySuccs = liveness.successors(0);
    ASSERT_EQ(entrySuccs.size(), 1u);
    EXPECT_EQ(entrySuccs.front(), 1u);

    const auto &falseSuccs = liveness.successors(1);
    ASSERT_EQ(falseSuccs.size(), 1u);
    EXPECT_EQ(falseSuccs.front(), 3u);
}

TEST(X86BackendRegressions, InvalidConditionCodeThrowsDiagnosticException) {
    MFunction fn{};
    fn.name = "bad_cc";

    MBasicBlock entry{};
    entry.label = "bad_cc";
    entry.instructions = {
        MInstr::make(MOpcode::JCC, {makeImmOperand(99), makeLabelOperand(".Lbad_cc_done")})};

    MBasicBlock done{};
    done.label = ".Lbad_cc_done";
    done.instructions = {MInstr::make(MOpcode::RET)};

    fn.blocks = {entry, done};

    AsmEmitter::RoDataPool pool;
    AsmEmitter emitter(pool);
    std::ostringstream out;

    EXPECT_THROWS(emitter.emitFunction(out, fn, sysvTarget()), std::runtime_error);
}

TEST(X86BackendRegressions, SetccOperandRolesFollowDestinationKind) {
    const Operand rax = makePhysRegOperand(RegClass::GPR, static_cast<uint16_t>(PhysReg::RAX));

    const MInstr canonical = MInstr::make(MOpcode::SETcc, {makeImmOperand(0), rax});
    const auto [canonicalCondUse, canonicalCondDef] = operandRoles(canonical, 0);
    const auto [canonicalDstUse, canonicalDstDef] = operandRoles(canonical, 1);
    EXPECT_FALSE(canonicalCondUse);
    EXPECT_FALSE(canonicalCondDef);
    EXPECT_FALSE(canonicalDstUse);
    EXPECT_TRUE(canonicalDstDef);

    const MInstr destinationFirst = MInstr::make(MOpcode::SETcc, {rax, makeImmOperand(0)});
    const auto [dstUse, dstDef] = operandRoles(destinationFirst, 0);
    const auto [condUse, condDef] = operandRoles(destinationFirst, 1);
    EXPECT_FALSE(dstUse);
    EXPECT_TRUE(dstDef);
    EXPECT_FALSE(condUse);
    EXPECT_FALSE(condDef);
}

TEST(X86BackendRegressions, DivisionPseudosRemainObservableUntilLowered) {
    EXPECT_TRUE(hasObservableSideEffects(MOpcode::DIVS64rr));
    EXPECT_TRUE(hasObservableSideEffects(MOpcode::REMS64rr));
    EXPECT_TRUE(hasObservableSideEffects(MOpcode::DIVS64Chk0rr));
    EXPECT_TRUE(hasObservableSideEffects(MOpcode::REMS64Chk0rr));
    EXPECT_TRUE(hasObservableSideEffects(MOpcode::DIVU64rr));
    EXPECT_TRUE(hasObservableSideEffects(MOpcode::REMU64rr));
    EXPECT_TRUE(hasObservableSideEffects(MOpcode::DIVU64Chk0rr));
    EXPECT_TRUE(hasObservableSideEffects(MOpcode::REMU64Chk0rr));
}

TEST(X86BackendRegressions, Win64XmmCalleeSaveUnwindOffsetsAre16ByteAligned) {
    MFunction fn{};
    fn.name = "xmm_callee_save_align";

    MBasicBlock entry{};
    entry.label = ".L_xmm_callee_save_align_entry";
    const Operand rax = makePhysRegOperand(RegClass::GPR, static_cast<uint16_t>(PhysReg::RAX));
    const Operand rbx = makePhysRegOperand(RegClass::GPR, static_cast<uint16_t>(PhysReg::RBX));
    const Operand xmm0 = makePhysRegOperand(RegClass::XMM, static_cast<uint16_t>(PhysReg::XMM0));
    const Operand xmm6 = makePhysRegOperand(RegClass::XMM, static_cast<uint16_t>(PhysReg::XMM6));
    entry.instructions = {MInstr::make(MOpcode::MOVrr, {rbx, rax}),
                          MInstr::make(MOpcode::MOVSDrr, {xmm6, xmm0}),
                          MInstr::make(MOpcode::RET)};
    fn.blocks.push_back(std::move(entry));

    FrameInfo frame{};
    assignSpillSlots(fn, win64Target(), frame);
    insertPrologueEpilogue(fn, win64Target(), frame);

    bool sawXmmSave = false;
    for (const auto &op : frame.win64UnwindOps) {
        if (op.kind != Win64UnwindOpKind::SaveXmm128) {
            continue;
        }
        sawXmmSave = true;
        EXPECT_EQ(op.stackOffset % 16u, 0u);
    }
    EXPECT_TRUE(sawXmmSave);
}

TEST(X86BackendRegressions, XorImmediateZeroRemainsIdentityDuringISel) {
    MFunction fn{};
    fn.name = "xor_zero_identity";
    MBasicBlock entry{};
    entry.label = ".L_xor_zero_identity_entry";
    const Operand value = makeVRegOperand(RegClass::GPR, 1);
    entry.instructions = {MInstr::make(MOpcode::XORrr, {value, makeImmOperand(0)})};
    fn.blocks.push_back(std::move(entry));

    ISel isel(sysvTarget());
    isel.lowerArithmetic(fn);

    ASSERT_EQ(fn.blocks.front().instructions.size(), 1u);
    EXPECT_EQ(fn.blocks.front().instructions.front().opcode, MOpcode::XORri);
}

TEST(X86BackendRegressions, SelectOperandRolesIncludeTrueArm) {
    const Operand dst = makeVRegOperand(RegClass::GPR, 1);
    const Operand cond = makeVRegOperand(RegClass::GPR, 2);
    const Operand falseVal = makeVRegOperand(RegClass::GPR, 3);
    const Operand trueVal = makeVRegOperand(RegClass::GPR, 4);
    const MInstr select = MInstr::make(MOpcode::SELECT_GPR, {dst, cond, falseVal, trueVal});

    const auto [isUse, isDef] = operandRoles(select, 3);
    EXPECT_TRUE(isUse);
    EXPECT_FALSE(isDef);
}

TEST(X86BackendRegressions, GprSelectWithImmediateTrueValueLowersToBranch) {
    MFunction fn{};
    fn.name = "select_true_imm";
    MBasicBlock entry{};
    entry.label = ".L_select_true_imm_entry";
    const Operand dst = makeVRegOperand(RegClass::GPR, 1);
    const Operand cond = makeVRegOperand(RegClass::GPR, 2);
    entry.instructions = {
        MInstr::make(MOpcode::SELECT_GPR, {dst, cond, makeImmOperand(11), makeImmOperand(22)})};
    fn.blocks.push_back(std::move(entry));

    ISel isel(sysvTarget());
    isel.lowerSelect(fn);
    isel.validateSelectLowering(fn);

    const auto &instructions = fn.blocks.front().instructions;
    ASSERT_EQ(instructions.size(), 5u);
    EXPECT_EQ(instructions[0].opcode, MOpcode::TESTrr);
    EXPECT_EQ(instructions[1].opcode, MOpcode::MOVri);
    EXPECT_EQ(instructions[2].opcode, MOpcode::JCC);
    EXPECT_EQ(instructions[3].opcode, MOpcode::MOVri);
    EXPECT_EQ(instructions[4].opcode, MOpcode::LABEL);
    const auto *trueImm = std::get_if<OpImm>(&instructions[3].operands[1]);
    ASSERT_TRUE(trueImm != nullptr);
    EXPECT_EQ(trueImm->val, 22);
}

TEST(X86BackendRegressions, MulToLeaRequiresConstantDefinitionBeforeUse) {
    MFunction fn{};
    fn.name = "mul_future_const";
    MBasicBlock entry{};
    entry.label = ".L_mul_future_const_entry";
    const Operand dst = makeVRegOperand(RegClass::GPR, 1);
    const Operand factor = makeVRegOperand(RegClass::GPR, 2);
    entry.instructions = {MInstr::make(MOpcode::IMULrr, {dst, factor}),
                          MInstr::make(MOpcode::MOVri, {factor, makeImmOperand(3)})};
    fn.blocks.push_back(std::move(entry));

    ISel isel(sysvTarget());
    isel.lowerArithmetic(fn);

    const auto &block = fn.blocks.front();
    EXPECT_TRUE(blockContainsOpcode(block, MOpcode::IMULrr));
    EXPECT_TRUE(blockContainsOpcode(block, MOpcode::MOVri));
}

TEST(X86BackendRegressions, MulToLeaCountsMemoryBaseUsesOfConstantRegister) {
    MFunction fn{};
    fn.name = "mul_const_mem_use";
    MBasicBlock entry{};
    entry.label = ".L_mul_const_mem_use_entry";
    const Operand dst = makeVRegOperand(RegClass::GPR, 1);
    const Operand factor = makeVRegOperand(RegClass::GPR, 2);
    const Operand loaded = makeVRegOperand(RegClass::GPR, 3);
    entry.instructions = {MInstr::make(MOpcode::MOVri, {factor, makeImmOperand(3)}),
                          MInstr::make(MOpcode::IMULrr, {dst, factor}),
                          MInstr::make(MOpcode::MOVmr, {loaded, Operand{baseMem(factor)}})};
    fn.blocks.push_back(std::move(entry));

    ISel isel(sysvTarget());
    isel.lowerArithmetic(fn);

    const auto &block = fn.blocks.front();
    EXPECT_TRUE(blockContainsOpcode(block, MOpcode::IMULrr));
    EXPECT_TRUE(blockContainsOpcode(block, MOpcode::MOVri));
}

TEST(X86BackendRegressions, MulToLeaDoesNotEraseConstantUsedInLaterBlock) {
    MFunction fn{};
    fn.name = "mul_const_cross_block";
    MBasicBlock entry{};
    entry.label = ".L_mul_const_cross_block_entry";
    const Operand value = makeVRegOperand(RegClass::GPR, 1);
    const Operand factor = makeVRegOperand(RegClass::GPR, 2);
    entry.instructions = {
        MInstr::make(MOpcode::MOVri, {factor, makeImmOperand(5)}),
        MInstr::make(MOpcode::IMULrr, {value, factor}),
        MInstr::make(MOpcode::JMP, {makeLabelOperand(".L_mul_const_cross_block_next")})};

    MBasicBlock next{};
    next.label = ".L_mul_const_cross_block_next";
    const Operand copy = makeVRegOperand(RegClass::GPR, 3);
    next.instructions = {MInstr::make(MOpcode::MOVrr, {copy, factor})};

    fn.blocks = {entry, next};

    ISel isel(sysvTarget());
    isel.lowerArithmetic(fn);

    const auto &block = fn.blocks.front();
    EXPECT_TRUE(blockContainsOpcode(block, MOpcode::IMULrr));
    EXPECT_TRUE(blockContainsOpcode(block, MOpcode::MOVri));
    EXPECT_FALSE(blockContainsOpcode(block, MOpcode::LEA));
}

TEST(X86BackendRegressions, MulToLeaRejectsSelfConstantMultiply) {
    MFunction fn{};
    fn.name = "mul_self_const";
    MBasicBlock entry{};
    entry.label = ".L_mul_self_const_entry";
    const Operand value = makeVRegOperand(RegClass::GPR, 1);
    entry.instructions = {MInstr::make(MOpcode::MOVri, {value, makeImmOperand(3)}),
                          MInstr::make(MOpcode::IMULrr, {value, value})};
    fn.blocks.push_back(std::move(entry));

    ISel isel(sysvTarget());
    isel.lowerArithmetic(fn);

    const auto &block = fn.blocks.front();
    EXPECT_TRUE(blockContainsOpcode(block, MOpcode::IMULrr));
    EXPECT_TRUE(blockContainsOpcode(block, MOpcode::MOVri));
}

TEST(X86BackendRegressions, MulToLeaFoldsPriorSingleUseConstant) {
    MFunction fn{};
    fn.name = "mul_const_fold";
    MBasicBlock entry{};
    entry.label = ".L_mul_const_fold_entry";
    const Operand value = makeVRegOperand(RegClass::GPR, 1);
    const Operand factor = makeVRegOperand(RegClass::GPR, 2);
    entry.instructions = {MInstr::make(MOpcode::MOVri, {factor, makeImmOperand(5)}),
                          MInstr::make(MOpcode::IMULrr, {value, factor})};
    fn.blocks.push_back(std::move(entry));

    ISel isel(sysvTarget());
    isel.lowerArithmetic(fn);

    const auto &block = fn.blocks.front();
    EXPECT_FALSE(blockContainsOpcode(block, MOpcode::MOVri));
    EXPECT_FALSE(blockContainsOpcode(block, MOpcode::IMULrr));
    ASSERT_EQ(block.instructions.size(), 1u);
    EXPECT_EQ(block.instructions.front().opcode, MOpcode::LEA);
}

TEST(X86BackendRegressions, SibFoldRequiresAddressDefsBeforeMemoryUse) {
    MFunction fn{};
    fn.name = "sib_future_defs";
    MBasicBlock entry{};
    entry.label = ".L_sib_future_defs_entry";
    const Operand base = makeVRegOperand(RegClass::GPR, 1);
    const Operand index = makeVRegOperand(RegClass::GPR, 2);
    const Operand scaled = makeVRegOperand(RegClass::GPR, 3);
    const Operand addr = makeVRegOperand(RegClass::GPR, 4);
    const Operand loaded = makeVRegOperand(RegClass::GPR, 5);
    entry.instructions = {MInstr::make(MOpcode::MOVmr, {loaded, Operand{baseMem(addr)}}),
                          MInstr::make(MOpcode::MOVrr, {scaled, index}),
                          MInstr::make(MOpcode::SHLri, {scaled, makeImmOperand(3)}),
                          MInstr::make(MOpcode::MOVrr, {addr, base}),
                          MInstr::make(MOpcode::ADDrr, {addr, scaled})};
    fn.blocks.push_back(std::move(entry));

    ISel isel(sysvTarget());
    isel.lowerArithmetic(fn);

    const auto &block = fn.blocks.front();
    const OpMem *mem = firstMemOperand(block.instructions.front());
    ASSERT_TRUE(mem != nullptr);
    EXPECT_FALSE(mem->hasIndex);
    // The value chain itself legally collapses into an LEA; the guarded
    // property is that the earlier memory use keeps its original address.
    EXPECT_TRUE(blockContainsOpcode(block, MOpcode::LEA));
    EXPECT_FALSE(blockContainsOpcode(block, MOpcode::SHLri));
}

TEST(X86BackendRegressions, SibFoldUsesPostDefReadCounts) {
    MFunction fn{};
    fn.name = "sib_fold";
    MBasicBlock entry{};
    entry.label = ".L_sib_fold_entry";
    const Operand base = makeVRegOperand(RegClass::GPR, 1);
    const Operand index = makeVRegOperand(RegClass::GPR, 2);
    const Operand scaled = makeVRegOperand(RegClass::GPR, 3);
    const Operand addr = makeVRegOperand(RegClass::GPR, 4);
    const Operand loaded = makeVRegOperand(RegClass::GPR, 5);
    entry.instructions = {MInstr::make(MOpcode::MOVrr, {scaled, index}),
                          MInstr::make(MOpcode::SHLri, {scaled, makeImmOperand(3)}),
                          MInstr::make(MOpcode::MOVrr, {addr, base}),
                          MInstr::make(MOpcode::ADDrr, {addr, scaled}),
                          MInstr::make(MOpcode::MOVmr, {loaded, Operand{baseMem(addr)}})};
    fn.blocks.push_back(std::move(entry));

    ISel isel(sysvTarget());
    isel.lowerArithmetic(fn);

    const auto &block = fn.blocks.front();
    ASSERT_EQ(block.instructions.size(), 1u);
    EXPECT_EQ(block.instructions.front().opcode, MOpcode::MOVmr);
    const OpMem *mem = firstMemOperand(block.instructions.front());
    ASSERT_TRUE(mem != nullptr);
    EXPECT_TRUE(mem->hasIndex);
    EXPECT_EQ(mem->scale, 8u);
    EXPECT_EQ(mem->base.idOrPhys, 1u);
    EXPECT_EQ(mem->index.idOrPhys, 2u);
}

TEST(X86BackendRegressions, SibFoldKeepsIndexCopyWhenOriginalIndexIsRedefined) {
    MFunction fn{};
    fn.name = "sib_index_source_redef";
    MBasicBlock entry{};
    entry.label = ".L_sib_index_source_redef_entry";
    const Operand base = makeVRegOperand(RegClass::GPR, 1);
    const Operand index = makeVRegOperand(RegClass::GPR, 2);
    const Operand replacement = makeVRegOperand(RegClass::GPR, 6);
    const Operand scaled = makeVRegOperand(RegClass::GPR, 3);
    const Operand addr = makeVRegOperand(RegClass::GPR, 4);
    const Operand loaded = makeVRegOperand(RegClass::GPR, 5);
    entry.instructions = {MInstr::make(MOpcode::MOVrr, {scaled, index}),
                          MInstr::make(MOpcode::SHLri, {scaled, makeImmOperand(3)}),
                          MInstr::make(MOpcode::MOVrr, {index, replacement}),
                          MInstr::make(MOpcode::MOVrr, {addr, base}),
                          MInstr::make(MOpcode::ADDrr, {addr, scaled}),
                          MInstr::make(MOpcode::MOVmr, {loaded, Operand{baseMem(addr)}})};
    fn.blocks.push_back(std::move(entry));

    ISel isel(sysvTarget());
    isel.lowerArithmetic(fn);

    const auto &block = fn.blocks.front();
    EXPECT_FALSE(blockContainsOpcode(block, MOpcode::SHLri));
    EXPECT_FALSE(blockContainsOpcode(block, MOpcode::ADDrr));
    EXPECT_TRUE(blockContainsOpcode(block, MOpcode::MOVrr));
    const OpMem *mem = firstMemOperand(block.instructions.back());
    ASSERT_TRUE(mem != nullptr);
    EXPECT_TRUE(mem->hasIndex);
    EXPECT_EQ(mem->scale, 8u);
    EXPECT_EQ(mem->base.idOrPhys, 1u);
    EXPECT_EQ(mem->index.idOrPhys, 3u);
}

TEST(X86BackendRegressions, SibFoldKeepsBaseCopyWhenOriginalBaseIsRedefined) {
    MFunction fn{};
    fn.name = "sib_base_source_redef";
    MBasicBlock entry{};
    entry.label = ".L_sib_base_source_redef_entry";
    const Operand base = makeVRegOperand(RegClass::GPR, 1);
    const Operand index = makeVRegOperand(RegClass::GPR, 2);
    const Operand replacement = makeVRegOperand(RegClass::GPR, 6);
    const Operand scaled = makeVRegOperand(RegClass::GPR, 3);
    const Operand addr = makeVRegOperand(RegClass::GPR, 4);
    const Operand loaded = makeVRegOperand(RegClass::GPR, 5);
    entry.instructions = {MInstr::make(MOpcode::MOVrr, {scaled, index}),
                          MInstr::make(MOpcode::SHLri, {scaled, makeImmOperand(3)}),
                          MInstr::make(MOpcode::MOVrr, {addr, base}),
                          MInstr::make(MOpcode::MOVrr, {base, replacement}),
                          MInstr::make(MOpcode::ADDrr, {addr, scaled}),
                          MInstr::make(MOpcode::MOVmr, {loaded, Operand{baseMem(addr)}})};
    fn.blocks.push_back(std::move(entry));

    ISel isel(sysvTarget());
    isel.lowerArithmetic(fn);

    const auto &block = fn.blocks.front();
    EXPECT_FALSE(blockContainsOpcode(block, MOpcode::SHLri));
    EXPECT_FALSE(blockContainsOpcode(block, MOpcode::ADDrr));
    const OpMem *mem = firstMemOperand(block.instructions.back());
    ASSERT_TRUE(mem != nullptr);
    EXPECT_TRUE(mem->hasIndex);
    EXPECT_EQ(mem->scale, 8u);
    EXPECT_EQ(mem->base.idOrPhys, 4u);
    EXPECT_EQ(mem->index.idOrPhys, 2u);
}

TEST(X86BackendRegressions, SibFoldKeepsPhysicalBaseCopyWhenSourceIsClobbered) {
    MFunction fn{};
    fn.name = "sib_phys_base_source_clobber";
    MBasicBlock entry{};
    entry.label = ".L_sib_phys_base_source_clobber_entry";
    const Operand rcx = makePhysRegOperand(RegClass::GPR, static_cast<uint16_t>(PhysReg::RCX));
    const Operand rdx = makePhysRegOperand(RegClass::GPR, static_cast<uint16_t>(PhysReg::RDX));
    const Operand index = makeVRegOperand(RegClass::GPR, 2);
    const Operand scaled = makeVRegOperand(RegClass::GPR, 3);
    const Operand addr = makeVRegOperand(RegClass::GPR, 4);
    const Operand loaded = makeVRegOperand(RegClass::GPR, 5);
    entry.instructions = {MInstr::make(MOpcode::MOVrr, {scaled, index}),
                          MInstr::make(MOpcode::SHLri, {scaled, makeImmOperand(3)}),
                          MInstr::make(MOpcode::MOVrr, {addr, rcx}),
                          MInstr::make(MOpcode::MOVrr, {rcx, rdx}),
                          MInstr::make(MOpcode::ADDrr, {addr, scaled}),
                          MInstr::make(MOpcode::MOVmr, {loaded, Operand{baseMem(addr)}})};
    fn.blocks.push_back(std::move(entry));

    ISel isel(sysvTarget());
    isel.lowerArithmetic(fn);

    const auto &block = fn.blocks.front();
    EXPECT_FALSE(blockContainsOpcode(block, MOpcode::SHLri));
    EXPECT_FALSE(blockContainsOpcode(block, MOpcode::ADDrr));
    const OpMem *mem = firstMemOperand(block.instructions.back());
    ASSERT_TRUE(mem != nullptr);
    EXPECT_TRUE(mem->hasIndex);
    EXPECT_FALSE(mem->base.isPhys);
    EXPECT_EQ(mem->base.idOrPhys, 4u);
    EXPECT_EQ(mem->index.idOrPhys, 2u);
}

TEST(X86BackendRegressions, SibFoldUsesLatestIndexCopyDefinitionBeforeShift) {
    MFunction fn{};
    fn.name = "sib_index_copy_redef";
    MBasicBlock entry{};
    entry.label = ".L_sib_index_copy_redef_entry";
    const Operand base = makeVRegOperand(RegClass::GPR, 1);
    const Operand oldIndex = makeVRegOperand(RegClass::GPR, 2);
    const Operand newIndex = makeVRegOperand(RegClass::GPR, 6);
    const Operand scaled = makeVRegOperand(RegClass::GPR, 3);
    const Operand addr = makeVRegOperand(RegClass::GPR, 4);
    const Operand loaded = makeVRegOperand(RegClass::GPR, 5);
    entry.instructions = {MInstr::make(MOpcode::MOVrr, {scaled, oldIndex}),
                          MInstr::make(MOpcode::MOVrr, {scaled, newIndex}),
                          MInstr::make(MOpcode::SHLri, {scaled, makeImmOperand(2)}),
                          MInstr::make(MOpcode::MOVrr, {addr, base}),
                          MInstr::make(MOpcode::ADDrr, {addr, scaled}),
                          MInstr::make(MOpcode::MOVmr, {loaded, Operand{baseMem(addr)}})};
    fn.blocks.push_back(std::move(entry));

    ISel isel(sysvTarget());
    isel.lowerArithmetic(fn);

    const auto &block = fn.blocks.front();
    EXPECT_FALSE(blockContainsOpcode(block, MOpcode::SHLri));
    EXPECT_FALSE(blockContainsOpcode(block, MOpcode::ADDrr));
    const OpMem *mem = firstMemOperand(block.instructions.back());
    ASSERT_TRUE(mem != nullptr);
    EXPECT_TRUE(mem->hasIndex);
    EXPECT_EQ(mem->scale, 4u);
    EXPECT_EQ(mem->base.idOrPhys, 1u);
    EXPECT_EQ(mem->index.idOrPhys, 6u);
}

TEST(X86BackendRegressions, SibFoldUsesLatestBaseCopyDefinitionBeforeAdd) {
    MFunction fn{};
    fn.name = "sib_base_copy_redef";
    MBasicBlock entry{};
    entry.label = ".L_sib_base_copy_redef_entry";
    const Operand oldBase = makeVRegOperand(RegClass::GPR, 1);
    const Operand index = makeVRegOperand(RegClass::GPR, 2);
    const Operand scaled = makeVRegOperand(RegClass::GPR, 3);
    const Operand addr = makeVRegOperand(RegClass::GPR, 4);
    const Operand loaded = makeVRegOperand(RegClass::GPR, 5);
    const Operand newBase = makeVRegOperand(RegClass::GPR, 6);
    entry.instructions = {MInstr::make(MOpcode::MOVrr, {scaled, index}),
                          MInstr::make(MOpcode::SHLri, {scaled, makeImmOperand(1)}),
                          MInstr::make(MOpcode::MOVrr, {addr, oldBase}),
                          MInstr::make(MOpcode::MOVrr, {addr, newBase}),
                          MInstr::make(MOpcode::ADDrr, {addr, scaled}),
                          MInstr::make(MOpcode::MOVmr, {loaded, Operand{baseMem(addr)}})};
    fn.blocks.push_back(std::move(entry));

    ISel isel(sysvTarget());
    isel.lowerArithmetic(fn);

    const auto &block = fn.blocks.front();
    EXPECT_FALSE(blockContainsOpcode(block, MOpcode::SHLri));
    EXPECT_FALSE(blockContainsOpcode(block, MOpcode::ADDrr));
    const OpMem *mem = firstMemOperand(block.instructions.back());
    ASSERT_TRUE(mem != nullptr);
    EXPECT_TRUE(mem->hasIndex);
    EXPECT_EQ(mem->scale, 2u);
    EXPECT_EQ(mem->base.idOrPhys, 6u);
    EXPECT_EQ(mem->index.idOrPhys, 2u);
}

TEST(X86BackendRegressions, SibFoldRejectsRedefinedShiftResultBeforeAdd) {
    MFunction fn{};
    fn.name = "sib_shift_result_redef";
    MBasicBlock entry{};
    entry.label = ".L_sib_shift_result_redef_entry";
    const Operand base = makeVRegOperand(RegClass::GPR, 1);
    const Operand index = makeVRegOperand(RegClass::GPR, 2);
    const Operand scaled = makeVRegOperand(RegClass::GPR, 3);
    const Operand addr = makeVRegOperand(RegClass::GPR, 4);
    const Operand loaded = makeVRegOperand(RegClass::GPR, 5);
    entry.instructions = {MInstr::make(MOpcode::MOVrr, {scaled, index}),
                          MInstr::make(MOpcode::SHLri, {scaled, makeImmOperand(3)}),
                          MInstr::make(MOpcode::MOVri, {scaled, makeImmOperand(7)}),
                          MInstr::make(MOpcode::MOVrr, {addr, base}),
                          MInstr::make(MOpcode::ADDrr, {addr, scaled}),
                          MInstr::make(MOpcode::MOVmr, {loaded, Operand{baseMem(addr)}})};
    fn.blocks.push_back(std::move(entry));

    ISel isel(sysvTarget());
    isel.lowerArithmetic(fn);

    const auto &block = fn.blocks.front();
    EXPECT_TRUE(blockContainsOpcode(block, MOpcode::SHLri));
    EXPECT_TRUE(blockContainsOpcode(block, MOpcode::ADDrr));
    const OpMem *mem = firstMemOperand(block.instructions.back());
    ASSERT_TRUE(mem != nullptr);
    EXPECT_FALSE(mem->hasIndex);
    EXPECT_EQ(mem->base.idOrPhys, 4u);
}

TEST(X86BackendRegressions, SibFoldRejectsRedefinedAddResultBeforeMemoryUse) {
    MFunction fn{};
    fn.name = "sib_add_result_redef";
    MBasicBlock entry{};
    entry.label = ".L_sib_add_result_redef_entry";
    const Operand base = makeVRegOperand(RegClass::GPR, 1);
    const Operand index = makeVRegOperand(RegClass::GPR, 2);
    const Operand scaled = makeVRegOperand(RegClass::GPR, 3);
    const Operand addr = makeVRegOperand(RegClass::GPR, 4);
    const Operand loaded = makeVRegOperand(RegClass::GPR, 5);
    const Operand replacement = makeVRegOperand(RegClass::GPR, 6);
    entry.instructions = {MInstr::make(MOpcode::MOVrr, {scaled, index}),
                          MInstr::make(MOpcode::SHLri, {scaled, makeImmOperand(3)}),
                          MInstr::make(MOpcode::MOVrr, {addr, base}),
                          MInstr::make(MOpcode::ADDrr, {addr, scaled}),
                          MInstr::make(MOpcode::MOVrr, {addr, replacement}),
                          MInstr::make(MOpcode::MOVmr, {loaded, Operand{baseMem(addr)}})};
    fn.blocks.push_back(std::move(entry));

    ISel isel(sysvTarget());
    isel.lowerArithmetic(fn);

    const auto &block = fn.blocks.front();
    // The value chain collapses into an LEA; the guarded property is that the
    // load keeps reading the REDEFINED address (vreg 4, no index) — the LEA's
    // addressing mode must not leak past the redefinition.
    EXPECT_TRUE(blockContainsOpcode(block, MOpcode::LEA));
    const OpMem *mem = firstMemOperand(block.instructions.back());
    ASSERT_TRUE(mem != nullptr);
    EXPECT_FALSE(mem->hasIndex);
    EXPECT_EQ(mem->base.idOrPhys, 4u);
}

TEST(X86BackendRegressions, SibFoldDoesNotEraseAddResultUsedInLaterBlock) {
    MFunction fn{};
    fn.name = "sib_add_cross_block";
    MBasicBlock entry{};
    entry.label = ".L_sib_add_cross_block_entry";
    const Operand base = makeVRegOperand(RegClass::GPR, 1);
    const Operand index = makeVRegOperand(RegClass::GPR, 2);
    const Operand scaled = makeVRegOperand(RegClass::GPR, 3);
    const Operand addr = makeVRegOperand(RegClass::GPR, 4);
    const Operand loaded = makeVRegOperand(RegClass::GPR, 5);
    entry.instructions = {
        MInstr::make(MOpcode::MOVrr, {scaled, index}),
        MInstr::make(MOpcode::SHLri, {scaled, makeImmOperand(3)}),
        MInstr::make(MOpcode::MOVrr, {addr, base}),
        MInstr::make(MOpcode::ADDrr, {addr, scaled}),
        MInstr::make(MOpcode::MOVmr, {loaded, Operand{baseMem(addr)}}),
        MInstr::make(MOpcode::JMP, {makeLabelOperand(".L_sib_add_cross_block_next")})};

    MBasicBlock next{};
    next.label = ".L_sib_add_cross_block_next";
    const Operand copy = makeVRegOperand(RegClass::GPR, 6);
    next.instructions = {MInstr::make(MOpcode::MOVrr, {copy, addr})};

    fn.blocks = {entry, next};

    ISel isel(sysvTarget());
    isel.lowerArithmetic(fn);

    const auto &block = fn.blocks.front();
    // The value chain collapses into an LEA that still DEFINES the address
    // register consumed by the later block; the load's operand must stay a
    // plain base (the two-use address register blocks the LEA-into-mem fold).
    EXPECT_TRUE(blockContainsOpcode(block, MOpcode::LEA));
    const OpMem *mem = nullptr;
    for (const auto &instr : block.instructions) {
        if (instr.opcode == MOpcode::MOVmr) {
            mem = firstMemOperand(instr);
            break;
        }
    }
    ASSERT_TRUE(mem != nullptr);
    EXPECT_FALSE(mem->hasIndex);
}

TEST(X86BackendRegressions, SibFoldDoesNotEraseShiftResultUsedInLaterBlock) {
    MFunction fn{};
    fn.name = "sib_shift_cross_block";
    MBasicBlock entry{};
    entry.label = ".L_sib_shift_cross_block_entry";
    const Operand base = makeVRegOperand(RegClass::GPR, 1);
    const Operand index = makeVRegOperand(RegClass::GPR, 2);
    const Operand scaled = makeVRegOperand(RegClass::GPR, 3);
    const Operand addr = makeVRegOperand(RegClass::GPR, 4);
    const Operand loaded = makeVRegOperand(RegClass::GPR, 5);
    entry.instructions = {
        MInstr::make(MOpcode::MOVrr, {scaled, index}),
        MInstr::make(MOpcode::SHLri, {scaled, makeImmOperand(3)}),
        MInstr::make(MOpcode::MOVrr, {addr, base}),
        MInstr::make(MOpcode::ADDrr, {addr, scaled}),
        MInstr::make(MOpcode::MOVmr, {loaded, Operand{baseMem(addr)}}),
        MInstr::make(MOpcode::JMP, {makeLabelOperand(".L_sib_shift_cross_block_next")})};

    MBasicBlock next{};
    next.label = ".L_sib_shift_cross_block_next";
    const Operand copy = makeVRegOperand(RegClass::GPR, 6);
    next.instructions = {MInstr::make(MOpcode::MOVrr, {copy, scaled})};

    fn.blocks = {entry, next};

    ISel isel(sysvTarget());
    isel.lowerArithmetic(fn);

    const auto &block = fn.blocks.front();
    EXPECT_TRUE(blockContainsOpcode(block, MOpcode::ADDrr));
    EXPECT_TRUE(blockContainsOpcode(block, MOpcode::SHLri));
    ASSERT_GT(block.instructions.size(), 4u);
    const OpMem *mem = firstMemOperand(block.instructions[4]);
    ASSERT_TRUE(mem != nullptr);
    EXPECT_FALSE(mem->hasIndex);
}

TEST(X86BackendRegressions, LeaFoldRequiresDefinitionBeforeMemoryUse) {
    MFunction fn{};
    fn.name = "lea_future_def";
    MBasicBlock entry{};
    entry.label = ".L_lea_future_def_entry";
    const Operand base = makeVRegOperand(RegClass::GPR, 1);
    const Operand index = makeVRegOperand(RegClass::GPR, 2);
    const Operand addr = makeVRegOperand(RegClass::GPR, 3);
    const Operand loaded = makeVRegOperand(RegClass::GPR, 4);
    entry.instructions = {MInstr::make(MOpcode::MOVmr, {loaded, Operand{baseMem(addr)}}),
                          MInstr::make(MOpcode::LEA, {addr, Operand{indexedMem(base, index, 4)}})};
    fn.blocks.push_back(std::move(entry));

    ISel isel(sysvTarget());
    isel.lowerArithmetic(fn);

    const auto &block = fn.blocks.front();
    const OpMem *mem = firstMemOperand(block.instructions.front());
    ASSERT_TRUE(mem != nullptr);
    EXPECT_FALSE(mem->hasIndex);
    EXPECT_TRUE(blockContainsOpcode(block, MOpcode::LEA));
}

TEST(X86BackendRegressions, LeaFoldUsesRoleAwareCountsAndDeferredErase) {
    MFunction fn{};
    fn.name = "lea_fold";
    MBasicBlock entry{};
    entry.label = ".L_lea_fold_entry";
    const Operand base = makeVRegOperand(RegClass::GPR, 1);
    const Operand index = makeVRegOperand(RegClass::GPR, 2);
    const Operand addr = makeVRegOperand(RegClass::GPR, 3);
    const Operand loaded = makeVRegOperand(RegClass::GPR, 4);
    entry.instructions = {
        MInstr::make(MOpcode::LEA, {addr, Operand{indexedMem(base, index, 4, 16)}}),
        MInstr::make(MOpcode::MOVmr, {loaded, Operand{baseMem(addr)}})};
    fn.blocks.push_back(std::move(entry));

    ISel isel(sysvTarget());
    isel.lowerArithmetic(fn);

    const auto &block = fn.blocks.front();
    ASSERT_EQ(block.instructions.size(), 1u);
    EXPECT_EQ(block.instructions.front().opcode, MOpcode::MOVmr);
    const OpMem *mem = firstMemOperand(block.instructions.front());
    ASSERT_TRUE(mem != nullptr);
    EXPECT_TRUE(mem->hasIndex);
    EXPECT_EQ(mem->scale, 4u);
    EXPECT_EQ(mem->disp, 16);
    EXPECT_EQ(mem->base.idOrPhys, 1u);
    EXPECT_EQ(mem->index.idOrPhys, 2u);
}

TEST(X86BackendRegressions, FlagScanStopsAtSharedFlagDefiners) {
    const Operand dst = makePhysRegOperand(RegClass::GPR, static_cast<uint16_t>(PhysReg::RAX));
    const Operand xmm0 = makePhysRegOperand(RegClass::XMM, static_cast<uint16_t>(PhysReg::XMM0));
    const Operand xmm1 = makePhysRegOperand(RegClass::XMM, static_cast<uint16_t>(PhysReg::XMM1));
    const std::vector<MInstr> instructions = {
        MInstr::make(MOpcode::MOVri, {dst, makeImmOperand(0)}),
        MInstr::make(MOpcode::UCOMIS, {xmm0, xmm1}),
        MInstr::make(MOpcode::JCC, {makeImmOperand(0), makeLabelOperand(".L_done")})};

    EXPECT_FALSE(peephole::nextInstrReadsFlags(instructions, 0));
}

TEST(X86BackendRegressions, ImmediateShiftCountsAreMaskedModulo64) {
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0};
    entry.paramKinds = {ILValue::Kind::I64};
    entry.instrs = {op("shl", {val(ILValue::Kind::I64, 0), imm(65)}, 1, ILValue::Kind::I64),
                    op("ashr", {val(ILValue::Kind::I64, 1), imm(-1)}, 2, ILValue::Kind::I64),
                    op("ret", {val(ILValue::Kind::I64, 2)})};

    ILFunction fn{};
    fn.name = "shift_mask";
    fn.blocks = {entry};

    const CodegenResult result = compile(fn);
    ASSERT_TRUE(result.errors.empty());
    EXPECT_NE(result.asmText.find("shlq $1"), std::string::npos);
    EXPECT_NE(result.asmText.find("sarq $63"), std::string::npos);
}

TEST(X86BackendRegressions, IndexedStoreUsesStoreDisplacementOperand) {
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0, 1};
    entry.paramKinds = {ILValue::Kind::PTR, ILValue::Kind::I64};
    entry.instrs = {
        op("shl", {val(ILValue::Kind::I64, 1), imm(3)}, 2, ILValue::Kind::I64),
        op("add", {val(ILValue::Kind::PTR, 0), val(ILValue::Kind::I64, 2)}, 3, ILValue::Kind::PTR),
        op("store", {val(ILValue::Kind::PTR, 3), imm(42), imm(16)}),
        op("ret", {imm(0)})};

    ILFunction fn{};
    fn.name = "indexed_store_disp";
    fn.blocks = {entry};

    const CodegenResult result = compile(fn);
    ASSERT_TRUE(result.errors.empty());
    EXPECT_TRUE(containsRegex(result.asmText, R"(16\(%r[a-z0-9]+,%r[a-z0-9]+,8\))"));
    EXPECT_FALSE(containsRegex(result.asmText, R"(42\(%r[a-z0-9]+,%r[a-z0-9]+,8\))"));
}

TEST(X86BackendRegressions, GepMaterializesImmediateBase) {
    ILValue nullBase{};
    nullBase.kind = ILValue::Kind::PTR;
    nullBase.id = -1;
    nullBase.i64 = 0;

    ILBlock entry{};
    entry.name = "entry";
    entry.instrs = {op("gep", {nullBase, imm(8)}, 1, ILValue::Kind::PTR),
                    op("ret", {val(ILValue::Kind::PTR, 1)})};

    ILFunction fn{};
    fn.name = "gep_null";
    fn.blocks = {entry};

    const CodegenResult result = compile(fn);
    ASSERT_TRUE(result.errors.empty());
    EXPECT_TRUE(containsRegex(result.asmText, R"(leaq\s+8\(%r[a-z0-9]+\),\s*%r[a-z0-9]+)"));
}

TEST(X86BackendRegressions, LeaFoldCombinesOuterDisplacement) {
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0};
    entry.paramKinds = {ILValue::Kind::PTR};
    entry.instrs = {op("gep", {val(ILValue::Kind::PTR, 0), imm(8)}, 1, ILValue::Kind::PTR),
                    op("load", {val(ILValue::Kind::PTR, 1), imm(4)}, 2, ILValue::Kind::I64),
                    op("ret", {val(ILValue::Kind::I64, 2)})};

    ILFunction fn{};
    fn.name = "lea_fold_disp";
    fn.blocks = {entry};

    const CodegenResult result = compile(fn);
    ASSERT_TRUE(result.errors.empty());
    EXPECT_TRUE(containsRegex(result.asmText, R"(movq\s+12\(%r[a-z0-9]+\),\s*%r[a-z0-9]+)"));
}

TEST(X86BackendRegressions, LeaFoldPreservesOuterIndex) {
    MFunction fn{};
    fn.name = "lea_fold_index";

    MBasicBlock entry{};
    entry.label = ".L_lea_fold_index_entry";
    const Operand base = makeVRegOperand(RegClass::GPR, 1);
    const Operand index = makeVRegOperand(RegClass::GPR, 2);
    const Operand addr = makeVRegOperand(RegClass::GPR, 3);
    const Operand loaded = makeVRegOperand(RegClass::GPR, 4);
    entry.instructions = {
        MInstr::make(MOpcode::LEA, {addr, Operand{baseMem(base, 8)}}),
        MInstr::make(MOpcode::MOVmr, {loaded, Operand{indexedMem(addr, index, 2, 4)}})};
    fn.blocks.push_back(std::move(entry));

    ISel isel(sysvTarget());
    isel.lowerArithmetic(fn);

    const auto &block = fn.blocks.front();
    ASSERT_EQ(block.instructions.size(), 1u);
    const OpMem *mem = firstMemOperand(block.instructions.front());
    ASSERT_TRUE(mem != nullptr);
    EXPECT_TRUE(mem->hasIndex);
    EXPECT_EQ(mem->disp, 12);
    EXPECT_EQ(mem->scale, 2u);
    EXPECT_EQ(mem->base.idOrPhys, 1u);
    EXPECT_EQ(mem->index.idOrPhys, 2u);
}

TEST(X86BackendRegressions, LeaFoldRejectsRedefinedLeaResultBeforeMemoryUse) {
    MFunction fn{};
    fn.name = "lea_result_redef";

    MBasicBlock entry{};
    entry.label = ".L_lea_result_redef_entry";
    const Operand base = makeVRegOperand(RegClass::GPR, 1);
    const Operand replacement = makeVRegOperand(RegClass::GPR, 2);
    const Operand addr = makeVRegOperand(RegClass::GPR, 3);
    const Operand loaded = makeVRegOperand(RegClass::GPR, 4);
    entry.instructions = {MInstr::make(MOpcode::LEA, {addr, Operand{baseMem(base, 8)}}),
                          MInstr::make(MOpcode::MOVrr, {addr, replacement}),
                          MInstr::make(MOpcode::MOVmr, {loaded, Operand{baseMem(addr)}})};
    fn.blocks.push_back(std::move(entry));

    ISel isel(sysvTarget());
    isel.lowerArithmetic(fn);

    const auto &block = fn.blocks.front();
    EXPECT_TRUE(blockContainsOpcode(block, MOpcode::LEA));
    const OpMem *mem = firstMemOperand(block.instructions.back());
    ASSERT_TRUE(mem != nullptr);
    EXPECT_FALSE(mem->hasIndex);
    EXPECT_EQ(mem->base.idOrPhys, 3u);
}

TEST(X86BackendRegressions, LeaFoldRejectsRedefinedBaseInputBeforeMemoryUse) {
    MFunction fn{};
    fn.name = "lea_base_input_redef";

    MBasicBlock entry{};
    entry.label = ".L_lea_base_input_redef_entry";
    const Operand base = makeVRegOperand(RegClass::GPR, 1);
    const Operand replacement = makeVRegOperand(RegClass::GPR, 2);
    const Operand addr = makeVRegOperand(RegClass::GPR, 3);
    const Operand loaded = makeVRegOperand(RegClass::GPR, 4);
    entry.instructions = {MInstr::make(MOpcode::LEA, {addr, Operand{baseMem(base, 8)}}),
                          MInstr::make(MOpcode::MOVrr, {base, replacement}),
                          MInstr::make(MOpcode::MOVmr, {loaded, Operand{baseMem(addr)}})};
    fn.blocks.push_back(std::move(entry));

    ISel isel(sysvTarget());
    isel.lowerArithmetic(fn);

    const auto &block = fn.blocks.front();
    EXPECT_TRUE(blockContainsOpcode(block, MOpcode::LEA));
    const OpMem *mem = firstMemOperand(block.instructions.back());
    ASSERT_TRUE(mem != nullptr);
    EXPECT_FALSE(mem->hasIndex);
    EXPECT_EQ(mem->base.idOrPhys, 3u);
}

TEST(X86BackendRegressions, LeaFoldRejectsRedefinedIndexInputBeforeMemoryUse) {
    MFunction fn{};
    fn.name = "lea_index_input_redef";

    MBasicBlock entry{};
    entry.label = ".L_lea_index_input_redef_entry";
    const Operand base = makeVRegOperand(RegClass::GPR, 1);
    const Operand index = makeVRegOperand(RegClass::GPR, 2);
    const Operand addr = makeVRegOperand(RegClass::GPR, 3);
    const Operand loaded = makeVRegOperand(RegClass::GPR, 4);
    const Operand replacement = makeVRegOperand(RegClass::GPR, 5);
    entry.instructions = {MInstr::make(MOpcode::LEA, {addr, Operand{indexedMem(base, index, 4)}}),
                          MInstr::make(MOpcode::MOVrr, {index, replacement}),
                          MInstr::make(MOpcode::MOVmr, {loaded, Operand{baseMem(addr)}})};
    fn.blocks.push_back(std::move(entry));

    ISel isel(sysvTarget());
    isel.lowerArithmetic(fn);

    const auto &block = fn.blocks.front();
    EXPECT_TRUE(blockContainsOpcode(block, MOpcode::LEA));
    const OpMem *mem = firstMemOperand(block.instructions.back());
    ASSERT_TRUE(mem != nullptr);
    EXPECT_FALSE(mem->hasIndex);
    EXPECT_EQ(mem->base.idOrPhys, 3u);
}

TEST(X86BackendRegressions, LeaFoldStillCombinesStableInputsAfterInterveningInstruction) {
    MFunction fn{};
    fn.name = "lea_stable_inputs_fold";

    MBasicBlock entry{};
    entry.label = ".L_lea_stable_inputs_fold_entry";
    const Operand base = makeVRegOperand(RegClass::GPR, 1);
    const Operand index = makeVRegOperand(RegClass::GPR, 2);
    const Operand addr = makeVRegOperand(RegClass::GPR, 3);
    const Operand loaded = makeVRegOperand(RegClass::GPR, 4);
    const Operand scratch = makeVRegOperand(RegClass::GPR, 5);
    entry.instructions = {
        MInstr::make(MOpcode::LEA, {addr, Operand{indexedMem(base, index, 4, 8)}}),
        MInstr::make(MOpcode::MOVri, {scratch, makeImmOperand(99)}),
        MInstr::make(MOpcode::MOVmr, {loaded, Operand{baseMem(addr, 16)}})};
    fn.blocks.push_back(std::move(entry));

    ISel isel(sysvTarget());
    isel.lowerArithmetic(fn);

    const auto &block = fn.blocks.front();
    EXPECT_FALSE(blockContainsOpcode(block, MOpcode::LEA));
    ASSERT_EQ(block.instructions.size(), 2u);
    const OpMem *mem = firstMemOperand(block.instructions.back());
    ASSERT_TRUE(mem != nullptr);
    EXPECT_TRUE(mem->hasIndex);
    EXPECT_EQ(mem->scale, 4u);
    EXPECT_EQ(mem->disp, 24);
    EXPECT_EQ(mem->base.idOrPhys, 1u);
    EXPECT_EQ(mem->index.idOrPhys, 2u);
}

TEST(X86BackendRegressions, LeaFoldDoesNotEraseAddressUsedInLaterBlock) {
    MFunction fn{};
    fn.name = "lea_cross_block";

    MBasicBlock entry{};
    entry.label = ".L_lea_cross_block_entry";
    const Operand base = makeVRegOperand(RegClass::GPR, 1);
    const Operand addr = makeVRegOperand(RegClass::GPR, 2);
    const Operand value = makeVRegOperand(RegClass::GPR, 3);
    const Operand loaded = makeVRegOperand(RegClass::GPR, 4);
    entry.instructions = {
        MInstr::make(MOpcode::LEA, {addr, Operand{baseMem(base, 8)}}),
        MInstr::make(MOpcode::MOVrm, {Operand{baseMem(addr)}, value}),
        MInstr::make(MOpcode::JMP, {makeLabelOperand(".L_lea_cross_block_next")})};

    MBasicBlock next{};
    next.label = ".L_lea_cross_block_next";
    next.instructions = {MInstr::make(MOpcode::MOVmr, {loaded, Operand{baseMem(addr)}})};

    fn.blocks = {std::move(entry), std::move(next)};

    ISel isel(sysvTarget());
    isel.lowerArithmetic(fn);

    ASSERT_EQ(fn.blocks.front().instructions.size(), 3u);
    EXPECT_EQ(fn.blocks.front().instructions.front().opcode, MOpcode::LEA);
    const OpMem *entryMem = firstMemOperand(fn.blocks.front().instructions[1]);
    ASSERT_TRUE(entryMem != nullptr);
    EXPECT_EQ(entryMem->base.idOrPhys, 2u);
    const OpMem *nextMem = firstMemOperand(fn.blocks.back().instructions.front());
    ASSERT_TRUE(nextMem != nullptr);
    EXPECT_EQ(nextMem->base.idOrPhys, 2u);
}

TEST(X86BackendRegressions, UremLargePowerOfTwoMaskMaterializesBeforeBinaryEmission) {
    constexpr int64_t kLargePowerOfTwo = 1LL << 40;

    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0};
    entry.paramKinds = {ILValue::Kind::I64};
    entry.instrs = {
        op("urem", {val(ILValue::Kind::I64, 0), imm(kLargePowerOfTwo)}, 1, ILValue::Kind::I64),
        op("ret", {val(ILValue::Kind::I64, 1)})};

    ILFunction fn{};
    fn.name = "urem_large_mask";
    fn.blocks = {entry};

    const CodegenResult asmResult = compile(fn);
    ASSERT_TRUE(asmResult.errors.empty());
    EXPECT_EQ(asmResult.asmText.find("andq $1099511627775"), std::string::npos);
    EXPECT_NE(asmResult.asmText.find("andq %r11"), std::string::npos);

    const BinaryEmitResult binaryResult = compileBinary(fn);
    EXPECT_TRUE(binaryResult.errors.empty());
}

TEST(X86BackendRegressions, ExistingOverflowTrapBlockGetsRuntimeCall) {
    MFunction fn{};
    fn.name = "ovf_reuse";

    MBasicBlock entry{};
    entry.label = ".L_ovf_reuse_entry";
    const Operand lhs = makeVRegOperand(RegClass::GPR, 1);
    const Operand rhs = makeVRegOperand(RegClass::GPR, 2);
    entry.instructions = {MInstr::make(MOpcode::ADDOvfrr, {lhs, rhs})};

    MBasicBlock trap{};
    trap.label = ".Ltrap_ovf_ovf_reuse";
    trap.instructions = {MInstr::make(MOpcode::UD2)};

    fn.blocks = {entry, trap};
    lowerOverflowOps(fn);

    const auto trapIt = std::find_if(fn.blocks.begin(), fn.blocks.end(), [](const MBasicBlock &bb) {
        return bb.label == ".Ltrap_ovf_ovf_reuse";
    });
    ASSERT_TRUE(trapIt != fn.blocks.end());

    const auto callIt = std::find_if(
        trapIt->instructions.begin(), trapIt->instructions.end(), [](const MInstr &instr) {
            if (instr.opcode != MOpcode::CALL || instr.operands.empty())
                return false;
            const auto *label = std::get_if<OpLabel>(&instr.operands.front());
            return label && label->name == "rt_trap_ovf";
        });
    const auto ud2It =
        std::find_if(trapIt->instructions.begin(),
                     trapIt->instructions.end(),
                     [](const MInstr &instr) { return instr.opcode == MOpcode::UD2; });
    ASSERT_TRUE(callIt != trapIt->instructions.end());
    ASSERT_TRUE(ud2It != trapIt->instructions.end());
    EXPECT_LT(std::distance(trapIt->instructions.begin(), callIt),
              std::distance(trapIt->instructions.begin(), ud2It));
}

TEST(X86BackendRegressions, LabelLiteralEdgeArgsMaterializeAsPointers) {
    AsmEmitter::RoDataPool roData;
    LowerILToMIR lowering(sysvTarget(), roData);

    ILBlock entry{};
    entry.name = "entry";
    entry.instrs = {op("br", {label("target")})};
    entry.terminatorEdges = {
        ILBlock::EdgeArg{.to = "target", .argIds = {-1}, .argValues = {label("global_target")}}};

    ILBlock target{};
    target.name = "target";
    target.paramIds = {0};
    target.paramKinds = {ILValue::Kind::PTR};
    target.instrs = {op("ret", {val(ILValue::Kind::PTR, 0)})};

    ILFunction fn{};
    fn.name = "label_edge";
    fn.blocks = {entry, target};

    const MFunction mir = lowering.lower(fn);

    bool foundLabelLea = false;
    bool foundBadZeroMove = false;
    for (const auto &block : mir.blocks) {
        if (block.label.find(".edge") == std::string::npos)
            continue;
        for (const auto &instr : block.instructions) {
            if (instr.opcode == MOpcode::LEA && instr.operands.size() >= 2) {
                const auto *rip = std::get_if<OpRipLabel>(&instr.operands[1]);
                foundLabelLea = foundLabelLea || (rip && rip->name == "global_target");
            }
            if (instr.opcode == MOpcode::MOVri && instr.operands.size() >= 2) {
                const auto *immediate = std::get_if<OpImm>(&instr.operands[1]);
                foundBadZeroMove = foundBadZeroMove || (immediate && immediate->val == 0);
            }
        }
    }

    EXPECT_TRUE(foundLabelLea);
    EXPECT_FALSE(foundBadZeroMove);
}

TEST(X86BackendRegressions, XmmSelectBranchArmIsSplitBeforeRegAlloc) {
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0};
    entry.paramKinds = {ILValue::Kind::I1};
    entry.instrs = {
        op("const_f64", {immF64(1.5)}, 1, ILValue::Kind::F64),
        op("const_f64", {immF64(2.5)}, 2, ILValue::Kind::F64),
        op("select",
           {val(ILValue::Kind::I1, 0), val(ILValue::Kind::F64, 1), val(ILValue::Kind::F64, 2)},
           3,
           ILValue::Kind::F64),
        op("ret", {val(ILValue::Kind::F64, 3)}, -1, ILValue::Kind::F64)};

    ILFunction fn{};
    fn.name = "xmm_select_split";
    fn.blocks = {entry};

    ILModule module{};
    module.funcs = {fn};

    AsmEmitter::RoDataPool roData;
    std::vector<MFunction> mir;
    std::vector<FrameInfo> frames;
    std::string errors;
    CodegenOptions options;
    ASSERT_TRUE(legalizeModuleToMIR(module, sysvTarget(), options, roData, mir, frames, errors));
    ASSERT_TRUE(errors.empty());
    ASSERT_EQ(mir.size(), 1u);

    bool foundExplicitFalseEdge = false;
    for (const auto &block : mir.front().blocks) {
        EXPECT_FALSE(blockHasNonTerminatorAfterControlTransfer(block));
        foundExplicitFalseEdge =
            foundExplicitFalseEdge || blockHasExplicitConditionalFallthrough(block);
    }
    EXPECT_TRUE(foundExplicitFalseEdge);
}

TEST(X86BackendRegressions, LivenessCfgTracksNonAdjacentJccBeforeFinalJump) {
    MFunction fn{};
    fn.name = "non_adjacent_jcc";

    MBasicBlock entry{};
    entry.label = ".L_non_adjacent_jcc_entry";
    const Operand dst = makeVRegOperand(RegClass::GPR, 1);
    const Operand src = makeVRegOperand(RegClass::GPR, 2);
    entry.instructions = {
        MInstr::make(MOpcode::JCC, {makeImmOperand(1), makeLabelOperand(".L_true")}),
        MInstr::make(MOpcode::MOVrr, {dst, src}),
        MInstr::make(MOpcode::JMP, {makeLabelOperand(".L_false")})};

    MBasicBlock trueBlock{};
    trueBlock.label = ".L_true";
    trueBlock.instructions = {MInstr::make(MOpcode::RET)};

    MBasicBlock falseBlock{};
    falseBlock.label = ".L_false";
    falseBlock.instructions = {MInstr::make(MOpcode::RET)};

    fn.blocks = {entry, trueBlock, falseBlock};

    ra::LivenessAnalysis liveness;
    liveness.run(fn);
    const auto &succs = liveness.successors(0);
    ASSERT_EQ(succs.size(), 2u);
    EXPECT_NE(std::find(succs.begin(), succs.end(), 1u), succs.end());
    EXPECT_NE(std::find(succs.begin(), succs.end(), 2u), succs.end());
}

TEST(X86BackendRegressions, Win64RawRuntimeCallsReserveShadowSpace) {
    MFunction fn{};
    fn.name = "raw_trap_call";

    MBasicBlock entry{};
    entry.label = ".L_raw_trap_call_entry";
    entry.instructions = {MInstr::make(MOpcode::CALL, {makeLabelOperand("rt_trap_ovf")}),
                          MInstr::make(MOpcode::UD2)};
    fn.blocks.push_back(std::move(entry));

    FrameInfo frame{};
    assignSpillSlots(fn, win64Target(), frame);
    EXPECT_GE(frame.outgoingArgArea, 32);
}

TEST(X86BackendRegressions, I1ImmediateOperandsCanonicalizeToZeroOrOne) {
    ILBlock entry{};
    entry.name = "entry";
    entry.instrs = {op("zext", {immI1(7)}, 0, ILValue::Kind::I64),
                    op("ret", {val(ILValue::Kind::I64, 0)})};

    ILFunction fn{};
    fn.name = "i1_imm_zext";
    fn.blocks = {entry};

    const CodegenResult result = compile(fn);
    ASSERT_TRUE(result.errors.empty());
    EXPECT_NE(result.asmText.find("$1"), std::string::npos);
    EXPECT_EQ(result.asmText.find("$7"), std::string::npos);
}

TEST(X86BackendRegressions, I1ImmediateCallArgsCanonicalizeToZeroOrOne) {
    AsmEmitter::RoDataPool roData;
    LowerILToMIR lowering(sysvTarget(), roData);

    ILBlock entry{};
    entry.name = "entry";
    entry.instrs = {op("call", {label("sink"), immI1(-3)}), op("ret", {imm(0)})};

    ILFunction fn{};
    fn.name = "i1_call_arg";
    fn.blocks = {entry};

    (void)lowering.lower(fn);
    ASSERT_EQ(lowering.callPlans().size(), 1u);
    ASSERT_EQ(lowering.callPlans().front().args.size(), 1u);
    EXPECT_TRUE(lowering.callPlans().front().args[0].isImm);
    EXPECT_EQ(lowering.callPlans().front().args[0].imm, 1);
}

TEST(X86BackendRegressions, I1ImmediateEdgeArgsCanonicalizeToZeroOrOne) {
    AsmEmitter::RoDataPool roData;
    LowerILToMIR lowering(sysvTarget(), roData);

    ILBlock entry{};
    entry.name = "entry";
    entry.instrs = {op("br", {label("target")})};
    entry.terminatorEdges = {
        ILBlock::EdgeArg{.to = "target", .argIds = {-1}, .argValues = {immI1(99)}}};

    ILBlock target{};
    target.name = "target";
    target.paramIds = {0};
    target.paramKinds = {ILValue::Kind::I1};
    target.instrs = {op("ret", {val(ILValue::Kind::I1, 0)})};

    ILFunction fn{};
    fn.name = "i1_edge_arg";
    fn.blocks = {entry, target};

    const MFunction mir = lowering.lower(fn);
    bool foundCanonicalMove = false;
    bool foundRawMove = false;
    for (const auto &block : mir.blocks) {
        if (block.label.find(".edge") == std::string::npos) {
            continue;
        }
        for (const auto &instr : block.instructions) {
            if (instr.opcode != MOpcode::MOVri || instr.operands.size() < 2) {
                continue;
            }
            const auto *imm = std::get_if<OpImm>(&instr.operands[1]);
            foundCanonicalMove = foundCanonicalMove || (imm && imm->val == 1);
            foundRawMove = foundRawMove || (imm && imm->val == 99);
        }
    }

    EXPECT_TRUE(foundCanonicalMove);
    EXPECT_FALSE(foundRawMove);
}

TEST(X86BackendRegressions, BlockParameterEdgesRejectTooFewArgs) {
    AsmEmitter::RoDataPool roData;
    LowerILToMIR lowering(sysvTarget(), roData);

    ILBlock entry{};
    entry.name = "entry";
    entry.instrs = {op("br", {label("target")})};
    entry.terminatorEdges = {ILBlock::EdgeArg{.to = "target", .argIds = {}}};

    ILBlock target{};
    target.name = "target";
    target.paramIds = {0};
    target.paramKinds = {ILValue::Kind::I64};
    target.instrs = {op("ret", {val(ILValue::Kind::I64, 0)})};

    ILFunction fn{};
    fn.name = "edge_too_few_args";
    fn.blocks = {entry, target};

    EXPECT_THROWS(lowering.lower(fn), std::runtime_error);
}

TEST(X86BackendRegressions, BlockParameterEdgesRejectTooManyArgs) {
    AsmEmitter::RoDataPool roData;
    LowerILToMIR lowering(sysvTarget(), roData);

    ILBlock entry{};
    entry.name = "entry";
    entry.instrs = {op("br", {label("target")})};
    entry.terminatorEdges = {
        ILBlock::EdgeArg{.to = "target", .argIds = {-1, -1}, .argValues = {imm(1), imm(2)}}};

    ILBlock target{};
    target.name = "target";
    target.paramIds = {0};
    target.paramKinds = {ILValue::Kind::I64};
    target.instrs = {op("ret", {val(ILValue::Kind::I64, 0)})};

    ILFunction fn{};
    fn.name = "edge_too_many_args";
    fn.blocks = {entry, target};

    EXPECT_THROWS(lowering.lower(fn), std::runtime_error);
}

TEST(X86BackendRegressions, BlockParameterEdgesRejectF64ForGprParam) {
    AsmEmitter::RoDataPool roData;
    LowerILToMIR lowering(sysvTarget(), roData);

    ILBlock entry{};
    entry.name = "entry";
    entry.instrs = {op("br", {label("target")})};
    entry.terminatorEdges = {
        ILBlock::EdgeArg{.to = "target", .argIds = {-1}, .argValues = {immF64(1.25)}}};

    ILBlock target{};
    target.name = "target";
    target.paramIds = {0};
    target.paramKinds = {ILValue::Kind::I64};
    target.instrs = {op("ret", {val(ILValue::Kind::I64, 0)})};

    ILFunction fn{};
    fn.name = "edge_f64_to_gpr";
    fn.blocks = {entry, target};

    EXPECT_THROWS(lowering.lower(fn), std::runtime_error);
}

TEST(X86BackendRegressions, BlockParameterEdgesRejectCrossClassSsaValue) {
    AsmEmitter::RoDataPool roData;
    LowerILToMIR lowering(sysvTarget(), roData);

    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0};
    entry.paramKinds = {ILValue::Kind::F64};
    entry.instrs = {op("br", {label("target")})};
    entry.terminatorEdges = {ILBlock::EdgeArg{.to = "target", .argIds = {0}}};

    ILBlock target{};
    target.name = "target";
    target.paramIds = {1};
    target.paramKinds = {ILValue::Kind::I64};
    target.instrs = {op("ret", {val(ILValue::Kind::I64, 1)})};

    ILFunction fn{};
    fn.name = "edge_xmm_to_gpr_ssa";
    fn.blocks = {entry, target};

    EXPECT_THROWS(lowering.lower(fn), std::runtime_error);
}

TEST(X86BackendRegressions, BlockParameterEdgesDefaultMissingParamKindsToI64) {
    AsmEmitter::RoDataPool roData;
    LowerILToMIR lowering(sysvTarget(), roData);

    ILBlock entry{};
    entry.name = "entry";
    entry.instrs = {op("br", {label("target")})};
    entry.terminatorEdges = {
        ILBlock::EdgeArg{.to = "target", .argIds = {-1}, .argValues = {imm(42)}}};

    ILBlock target{};
    target.name = "target";
    target.paramIds = {0};
    target.instrs = {op("ret", {val(ILValue::Kind::I64, 0)})};

    ILFunction fn{};
    fn.name = "edge_missing_param_kind";
    fn.blocks = {entry, target};

    const MFunction lowered = lowering.lower(fn);
    bool foundEdgeCopy = false;
    for (const auto &block : lowered.blocks) {
        foundEdgeCopy = foundEdgeCopy || blockContainsOpcode(block, MOpcode::PX_COPY);
    }
    EXPECT_TRUE(foundEdgeCopy);
}

TEST(X86BackendRegressions, HugeStringLiteralLengthIsRejectedBeforeResize) {
    ILBlock entry{};
    entry.name = "entry";
    entry.instrs = {op("const_str",
                       {strLit("x", std::numeric_limits<std::uint64_t>::max())},
                       0,
                       ILValue::Kind::STR),
                    op("ret", {val(ILValue::Kind::STR, 0)})};

    ILFunction fn{};
    fn.name = "huge_string_literal";
    fn.blocks = {entry};

    EXPECT_THROWS(compile(fn), std::runtime_error);
}

TEST(X86BackendRegressions, LoadRejectsF64Displacement) {
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0};
    entry.paramKinds = {ILValue::Kind::PTR};
    entry.instrs = {op("load", {val(ILValue::Kind::PTR, 0), immF64(4.0)}, 1, ILValue::Kind::I64),
                    op("ret", {val(ILValue::Kind::I64, 1)})};

    ILFunction fn{};
    fn.name = "load_f64_disp";
    fn.blocks = {entry};

    EXPECT_THROWS(compile(fn), std::runtime_error);
}

TEST(X86BackendRegressions, StoreRejectsF64Displacement) {
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0};
    entry.paramKinds = {ILValue::Kind::PTR};
    entry.instrs = {op("store", {val(ILValue::Kind::PTR, 0), imm(7), immF64(4.0)}),
                    op("ret", {imm(0)})};

    ILFunction fn{};
    fn.name = "store_f64_disp";
    fn.blocks = {entry};

    EXPECT_THROWS(compile(fn), std::runtime_error);
}

TEST(X86BackendRegressions, AllocaRejectsF64Size) {
    ILBlock entry{};
    entry.name = "entry";
    entry.instrs = {op("alloca", {immF64(8.0)}, 0, ILValue::Kind::PTR), op("ret", {imm(0)})};

    ILFunction fn{};
    fn.name = "alloca_f64_size";
    fn.blocks = {entry};

    EXPECT_THROWS(compile(fn), std::runtime_error);
}

TEST(X86BackendRegressions, AllocaRejectsNonPointerResult) {
    ILBlock entry{};
    entry.name = "entry";
    entry.instrs = {op("alloca", {imm(8)}, 0, ILValue::Kind::F64), op("ret", {})};

    ILFunction fn{};
    fn.name = "alloca_f64_result";
    fn.blocks = {entry};

    EXPECT_THROWS(compile(fn), std::runtime_error);
}

TEST(X86BackendRegressions, GepRejectsF64ImmediateOffset) {
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0};
    entry.paramKinds = {ILValue::Kind::PTR};
    entry.instrs = {op("gep", {val(ILValue::Kind::PTR, 0), immF64(8.0)}, 1, ILValue::Kind::PTR),
                    op("ret", {val(ILValue::Kind::PTR, 1)})};

    ILFunction fn{};
    fn.name = "gep_f64_offset";
    fn.blocks = {entry};

    EXPECT_THROWS(compile(fn), std::runtime_error);
}

TEST(X86BackendRegressions, GepRejectsNonPointerResult) {
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0};
    entry.paramKinds = {ILValue::Kind::PTR};
    entry.instrs = {op("gep", {val(ILValue::Kind::PTR, 0), imm(8)}, 1, ILValue::Kind::F64),
                    op("ret", {})};

    ILFunction fn{};
    fn.name = "gep_f64_result";
    fn.blocks = {entry};

    EXPECT_THROWS(compile(fn), std::runtime_error);
}

TEST(X86BackendRegressions, GepRejectsNonPointerBase) {
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0};
    entry.paramKinds = {ILValue::Kind::I64};
    entry.instrs = {op("gep", {val(ILValue::Kind::I64, 0), imm(8)}, 1, ILValue::Kind::PTR),
                    op("ret", {})};

    ILFunction fn{};
    fn.name = "gep_i64_base";
    fn.blocks = {entry};

    EXPECT_THROWS(compile(fn), std::runtime_error);
}

TEST(X86BackendRegressions, ConstStrRejectsNonStringOperand) {
    ILBlock entry{};
    entry.name = "entry";
    entry.instrs = {op("const_str", {imm(1)}, 0, ILValue::Kind::STR), op("ret", {})};

    ILFunction fn{};
    fn.name = "const_str_i64_operand";
    fn.blocks = {entry};

    EXPECT_THROWS(compile(fn), std::runtime_error);
}

TEST(X86BackendRegressions, ConstF64RejectsNonF64Operand) {
    ILBlock entry{};
    entry.name = "entry";
    entry.instrs = {op("const_f64", {imm(1)}, 0, ILValue::Kind::F64), op("ret", {})};

    ILFunction fn{};
    fn.name = "const_f64_i64_operand";
    fn.blocks = {entry};

    EXPECT_THROWS(compile(fn), std::runtime_error);
}

TEST(X86BackendRegressions, GAddrRejectsNonLabelOperand) {
    ILBlock entry{};
    entry.name = "entry";
    entry.instrs = {op("gaddr", {imm(1)}, 0, ILValue::Kind::PTR), op("ret", {})};

    ILFunction fn{};
    fn.name = "gaddr_i64_operand";
    fn.blocks = {entry};

    EXPECT_THROWS(compile(fn), std::runtime_error);
}

TEST(X86BackendRegressions, AddrOfRejectsNonPointerOperand) {
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0};
    entry.paramKinds = {ILValue::Kind::I64};
    entry.instrs = {op("addr_of", {val(ILValue::Kind::I64, 0)}, 1, ILValue::Kind::PTR),
                    op("ret", {})};

    ILFunction fn{};
    fn.name = "addr_of_i64_operand";
    fn.blocks = {entry};

    EXPECT_THROWS(compile(fn), std::runtime_error);
}

TEST(X86BackendRegressions, ReusedSsaIdWithDifferentRegisterClassIsRejected) {
    ILBlock entry{};
    entry.name = "entry";
    entry.instrs = {op("add", {imm(1), imm(2)}, 0, ILValue::Kind::I64),
                    op("fadd", {val(ILValue::Kind::F64, 0), immF64(1.0)}, 1, ILValue::Kind::F64),
                    op("ret", {imm(0)})};

    ILFunction fn{};
    fn.name = "ssa_type_reuse";
    fn.blocks = {entry};

    EXPECT_THROWS(compile(fn), std::runtime_error);
}

TEST(X86BackendRegressions, BranchEmitterRejectsNonLabelTarget) {
    AsmEmitter::RoDataPool roData;
    LowerILToMIR lowering(sysvTarget(), roData);
    MBasicBlock block{};
    block.label = ".L_bad_branch";
    MIRBuilder builder(lowering, block);

    ILInstr instr = op("br", {imm(0)});

    EXPECT_THROWS(EmitCommon(builder).emitBranch(instr), std::runtime_error);
}

TEST(X86BackendRegressions, CondBranchEmitterRejectsNonLabelTarget) {
    AsmEmitter::RoDataPool roData;
    LowerILToMIR lowering(sysvTarget(), roData);
    MBasicBlock block{};
    block.label = ".L_bad_cbr";
    MIRBuilder builder(lowering, block);

    ILInstr instr = op("cbr", {val(ILValue::Kind::I1, 0), label("yes"), imm(0)});

    EXPECT_THROWS(EmitCommon(builder).emitCondBranch(instr), std::runtime_error);
}

TEST(X86BackendRegressions, CondBranchRejectsF64Condition) {
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0};
    entry.paramKinds = {ILValue::Kind::F64};
    entry.instrs = {op("cbr", {val(ILValue::Kind::F64, 0), label("yes"), label("no")})};

    ILBlock yes{};
    yes.name = "yes";
    yes.instrs = {op("ret", {imm(1)})};

    ILBlock no{};
    no.name = "no";
    no.instrs = {op("ret", {imm(0)})};

    ILFunction fn{};
    fn.name = "cbr_f64_condition";
    fn.blocks = {entry, yes, no};

    EXPECT_THROWS(compile(fn), std::runtime_error);
}

TEST(X86BackendRegressions, SelectRejectsF64Condition) {
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0};
    entry.paramKinds = {ILValue::Kind::F64};
    entry.instrs = {op("select", {val(ILValue::Kind::F64, 0), imm(1), imm(2)}, 1),
                    op("ret", {val(ILValue::Kind::I64, 1)})};

    ILFunction fn{};
    fn.name = "select_f64_condition";
    fn.blocks = {entry};

    EXPECT_THROWS(compile(fn), std::runtime_error);
}

TEST(X86BackendRegressions, XmmSelectRejectsLabelArm) {
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0};
    entry.paramKinds = {ILValue::Kind::I1};
    entry.instrs = {op("select",
                       {val(ILValue::Kind::I1, 0), label("not_f64"), immF64(0.0)},
                       1,
                       ILValue::Kind::F64),
                    op("ret", {val(ILValue::Kind::F64, 1)}, -1, ILValue::Kind::F64)};

    ILFunction fn{};
    fn.name = "select_label_xmm_arm";
    fn.blocks = {entry};

    EXPECT_THROWS(compile(fn), std::runtime_error);
}

TEST(X86BackendRegressions, IndirectCallRejectsF64TargetRegister) {
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0};
    entry.paramKinds = {ILValue::Kind::F64};
    entry.instrs = {op("call.indirect", {val(ILValue::Kind::F64, 0)}), op("ret", {imm(0)})};

    ILFunction fn{};
    fn.name = "call_indirect_f64_target";
    fn.blocks = {entry};

    EXPECT_THROWS(compile(fn), std::runtime_error);
}

TEST(X86BackendRegressions, FpAddRejectsGprOperand) {
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0};
    entry.paramKinds = {ILValue::Kind::I64};
    entry.instrs = {op("add", {val(ILValue::Kind::I64, 0), immF64(1.0)}, 1, ILValue::Kind::F64),
                    op("ret", {val(ILValue::Kind::F64, 1)}, -1, ILValue::Kind::F64)};

    ILFunction fn{};
    fn.name = "fp_add_gpr_operand";
    fn.blocks = {entry};

    EXPECT_THROWS(compile(fn), std::runtime_error);
}

TEST(X86BackendRegressions, BitwiseAndRejectsF64Result) {
    expectCompileRejects(
        "and_f64_result",
        {op("and", {val(ILValue::Kind::F64, 0), immF64(1.0)}, 1, ILValue::Kind::F64),
         op("ret", {})},
        {0},
        {ILValue::Kind::F64});
}

TEST(X86BackendRegressions, BitwiseOrRejectsF64Result) {
    expectCompileRejects(
        "or_f64_result",
        {op("or", {val(ILValue::Kind::F64, 0), immF64(1.0)}, 1, ILValue::Kind::F64), op("ret", {})},
        {0},
        {ILValue::Kind::F64});
}

TEST(X86BackendRegressions, BitwiseXorRejectsF64Result) {
    expectCompileRejects(
        "xor_f64_result",
        {op("xor", {val(ILValue::Kind::F64, 0), immF64(1.0)}, 1, ILValue::Kind::F64),
         op("ret", {})},
        {0},
        {ILValue::Kind::F64});
}

TEST(X86BackendRegressions, ShiftRejectsF64Result) {
    expectCompileRejects(
        "shl_f64_result",
        {op("shl", {val(ILValue::Kind::F64, 0), imm(1)}, 1, ILValue::Kind::F64), op("ret", {})},
        {0},
        {ILValue::Kind::F64});
}

TEST(X86BackendRegressions, ShiftRejectsF64Count) {
    expectCompileRejects(
        "shl_f64_count",
        {op("shl", {val(ILValue::Kind::I64, 0), val(ILValue::Kind::F64, 1)}, 2, ILValue::Kind::I64),
         op("ret", {val(ILValue::Kind::I64, 2)})},
        {0, 1},
        {ILValue::Kind::I64, ILValue::Kind::F64});
}

TEST(X86BackendRegressions, ConstNullRejectsNonPointerResult) {
    expectCompileRejects("const_null_f64_result",
                         {op("const_null", {}, 0, ILValue::Kind::F64), op("ret", {})});
}

TEST(X86BackendRegressions, FPToSIRejectsNonF64Source) {
    expectCompileRejects(
        "fptosi_i64_source",
        {op("fptosi", {imm(1)}, 0, ILValue::Kind::I64), op("ret", {val(ILValue::Kind::I64, 0)})});
}

TEST(X86BackendRegressions, FPToSIRejectsNonIntegerResult) {
    expectCompileRejects("fptosi_f64_result",
                         {op("fptosi", {immF64(1.0)}, 0, ILValue::Kind::F64), op("ret", {})});
}

TEST(X86BackendRegressions, FPToUIRejectsNonIntegerResult) {
    expectCompileRejects("fptoui_f64_result",
                         {op("fptoui", {immF64(1.0)}, 0, ILValue::Kind::F64), op("ret", {})});
}

TEST(X86BackendRegressions, SIToFPRejectsNonF64Result) {
    expectCompileRejects(
        "sitofp_i64_result",
        {op("sitofp", {imm(1)}, 0, ILValue::Kind::I64), op("ret", {val(ILValue::Kind::I64, 0)})});
}

TEST(X86BackendRegressions, UIToFPRejectsNonF64Result) {
    expectCompileRejects(
        "uitofp_i64_result",
        {op("uitofp", {imm(1)}, 0, ILValue::Kind::I64), op("ret", {val(ILValue::Kind::I64, 0)})});
}

TEST(X86BackendRegressions, NarrowCastRejectsNonIntegerResult) {
    expectCompileRejects(
        "narrow_ptr_result",
        {opBits("si_narrow_chk", {imm(1)}, 0, ILValue::Kind::PTR, 16), op("ret", {})});
}

TEST(X86BackendRegressions, UnknownIntegerCompareOpcodeIsRejected) {
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0};
    entry.paramKinds = {ILValue::Kind::I64};
    entry.instrs = {op("icmp_unknown", {val(ILValue::Kind::I64, 0), imm(1)}, 1, ILValue::Kind::I1),
                    op("ret", {val(ILValue::Kind::I1, 1)})};

    ILFunction fn{};
    fn.name = "icmp_unknown_suffix";
    fn.blocks = {entry};

    EXPECT_THROWS(compile(fn), std::runtime_error);
}

TEST(X86BackendRegressions, UnknownFloatingCompareOpcodeIsRejected) {
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0};
    entry.paramKinds = {ILValue::Kind::F64};
    entry.instrs = {
        op("fcmp_unknown", {val(ILValue::Kind::F64, 0), immF64(1.0)}, 1, ILValue::Kind::I1),
        op("ret", {val(ILValue::Kind::I1, 1)})};

    ILFunction fn{};
    fn.name = "fcmp_unknown_suffix";
    fn.blocks = {entry};

    EXPECT_THROWS(compile(fn), std::runtime_error);
}

TEST(X86BackendRegressions, FcmpEqRejectsGprOperand) {
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0, 1};
    entry.paramKinds = {ILValue::Kind::I64, ILValue::Kind::F64};
    entry.instrs = {op("fcmp_eq",
                       {val(ILValue::Kind::I64, 0), val(ILValue::Kind::F64, 1)},
                       2,
                       ILValue::Kind::I1),
                    op("ret", {val(ILValue::Kind::I1, 2)})};

    ILFunction fn{};
    fn.name = "fcmp_eq_gpr_operand";
    fn.blocks = {entry};

    EXPECT_THROWS(compile(fn), std::runtime_error);
}

TEST(X86BackendRegressions, FcmpGtRejectsGprOperand) {
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0, 1};
    entry.paramKinds = {ILValue::Kind::I64, ILValue::Kind::F64};
    entry.instrs = {op("fcmp_gt",
                       {val(ILValue::Kind::I64, 0), val(ILValue::Kind::F64, 1)},
                       2,
                       ILValue::Kind::I1),
                    op("ret", {val(ILValue::Kind::I1, 2)})};

    ILFunction fn{};
    fn.name = "fcmp_gt_gpr_operand";
    fn.blocks = {entry};

    EXPECT_THROWS(compile(fn), std::runtime_error);
}

TEST(X86BackendRegressions, SwitchRejectsMissingDefaultLabel) {
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0};
    entry.paramKinds = {ILValue::Kind::I64};
    entry.instrs = {op("switch_i32", {val(ILValue::Kind::I64, 0), imm(1)})};

    ILFunction fn{};
    fn.name = "switch_missing_default_label";
    fn.blocks = {entry};

    EXPECT_THROWS(compile(fn), std::runtime_error);
}

TEST(X86BackendRegressions, SwitchRejectsNonIntegerCaseValue) {
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0};
    entry.paramKinds = {ILValue::Kind::I64};
    entry.instrs = {
        op("switch_i32", {val(ILValue::Kind::I64, 0), immF64(1.0), label("case1"), label("def")})};

    ILFunction fn{};
    fn.name = "switch_f64_case_value";
    fn.blocks = {entry};

    EXPECT_THROWS(compile(fn), std::runtime_error);
}

TEST(X86BackendRegressions, SwitchRejectsDanglingCaseValue) {
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0};
    entry.paramKinds = {ILValue::Kind::I64};
    entry.instrs = {op("switch_i32", {val(ILValue::Kind::I64, 0), imm(1), label("def")})};

    ILFunction fn{};
    fn.name = "switch_dangling_case_value";
    fn.blocks = {entry};

    EXPECT_THROWS(compile(fn), std::runtime_error);
}

TEST(X86BackendRegressions, ISelRejectsReservedTemporaryVregSentinel) {
    MFunction fn{};
    fn.name = "isel_reserved_temp";

    MBasicBlock entry{};
    entry.label = ".L_isel_reserved_temp";
    const Operand dst = makeVRegOperand(
        RegClass::GPR, static_cast<uint16_t>(std::numeric_limits<uint16_t>::max() - 1));
    entry.instructions = {MInstr::make(MOpcode::ADDrr, {dst, makeImmOperand(1LL << 40)})};
    fn.blocks = {entry};

    ISel isel(sysvTarget());
    EXPECT_THROWS(isel.lowerArithmetic(fn), std::runtime_error);
}

TEST(X86BackendRegressions, IndexedMemReconstructionRejectsShiftWithoutOriginalIndexCopy) {
    AsmEmitter::RoDataPool roData;
    LowerILToMIR lowering(sysvTarget(), roData);
    MBasicBlock block{};
    block.label = ".L_indexed_mem_no_copy";
    MIRBuilder builder(lowering, block);

    const VReg base = builder.ensureVReg(0, ILValue::Kind::PTR);
    const VReg index = builder.ensureVReg(1, ILValue::Kind::I64);
    const VReg addr = builder.ensureVReg(2, ILValue::Kind::PTR);
    const Operand baseOp = makeVRegOperand(base.cls, base.id);
    const Operand indexOp = makeVRegOperand(index.cls, index.id);
    const Operand addrOp = makeVRegOperand(addr.cls, addr.id);

    block.instructions = {MInstr::make(MOpcode::MOVrr, {addrOp, baseOp}),
                          MInstr::make(MOpcode::SHLri, {indexOp, makeImmOperand(3)}),
                          MInstr::make(MOpcode::ADDrr, {addrOp, indexOp})};

    const ILInstr load = op("load", {val(ILValue::Kind::PTR, 2), imm(0)}, 3, ILValue::Kind::I64);
    const std::optional<Operand> mem = EmitCommon(builder).tryMakeIndexedMem(load, 1);
    EXPECT_FALSE(mem.has_value());
}

TEST(X86BackendRegressions, LargeAluAndCompareImmediatesMaterializeBeforeEmission) {
    MFunction fn{};
    fn.name = "large_alu_immediates";

    MBasicBlock entry{};
    entry.label = ".L_large_alu_immediates_entry";
    const int64_t big = 1LL << 40;
    entry.instructions = {
        MInstr::make(MOpcode::ADDrr, {makeVRegOperand(RegClass::GPR, 1), makeImmOperand(big)}),
        MInstr::make(MOpcode::SUBrr,
                     {makeVRegOperand(RegClass::GPR, 2),
                      makeImmOperand(std::numeric_limits<int64_t>::min())}),
        MInstr::make(MOpcode::SUBrr,
                     {makeVRegOperand(RegClass::GPR, 3),
                      makeImmOperand(static_cast<int64_t>(std::numeric_limits<int32_t>::min()))}),
        MInstr::make(MOpcode::ANDrr, {makeVRegOperand(RegClass::GPR, 4), makeImmOperand(big)}),
        MInstr::make(MOpcode::ORrr, {makeVRegOperand(RegClass::GPR, 5), makeImmOperand(big)}),
        MInstr::make(MOpcode::XORrr, {makeVRegOperand(RegClass::GPR, 6), makeImmOperand(big)}),
        MInstr::make(MOpcode::CMPrr, {makeVRegOperand(RegClass::GPR, 7), makeImmOperand(big)})};
    fn.blocks.push_back(std::move(entry));

    ISel isel(sysvTarget());
    isel.lowerArithmetic(fn);
    isel.lowerCompareAndBranch(fn);

    std::size_t materialized = 0;
    for (const auto &instr : fn.blocks.front().instructions) {
        if (instr.opcode == MOpcode::MOVri) {
            ++materialized;
        }

        if (instr.operands.size() < 2 || !std::holds_alternative<OpImm>(instr.operands[1])) {
            continue;
        }

        const int64_t imm = std::get<OpImm>(instr.operands[1]).val;
        const bool fitsImm32 = imm >= std::numeric_limits<int32_t>::min() &&
                               imm <= std::numeric_limits<int32_t>::max();
        switch (instr.opcode) {
            case MOpcode::ADDri:
            case MOpcode::ANDri:
            case MOpcode::ORri:
            case MOpcode::XORri:
            case MOpcode::CMPri:
                EXPECT_TRUE(fitsImm32);
                break;
            case MOpcode::ADDrr:
            case MOpcode::SUBrr:
            case MOpcode::ANDrr:
            case MOpcode::ORrr:
            case MOpcode::XORrr:
            case MOpcode::CMPrr:
                EXPECT_TRUE(false);
                break;
            default:
                break;
        }
    }
    EXPECT_EQ(materialized, 7u);
}

TEST(X86BackendRegressions, BranchChainEliminationRetargetsJccBeforeFinalJump) {
    MFunction fn{};
    fn.name = "jcc_branch_chain";

    MBasicBlock entry{};
    entry.label = ".L_jcc_branch_chain_entry";
    entry.instructions = {
        MInstr::make(MOpcode::JCC, {makeImmOperand(1), makeLabelOperand(".L_chain")}),
        MInstr::make(MOpcode::JMP, {makeLabelOperand(".L_exit")})};

    MBasicBlock chain{};
    chain.label = ".L_chain";
    chain.instructions = {MInstr::make(MOpcode::JMP, {makeLabelOperand(".L_target")})};

    MBasicBlock target{};
    target.label = ".L_target";
    target.instructions = {MInstr::make(MOpcode::RET)};

    MBasicBlock exit{};
    exit.label = ".L_exit";
    exit.instructions = {MInstr::make(MOpcode::RET)};

    fn.blocks = {entry, chain, target, exit};

    peephole::PeepholeStats stats{};
    peephole::eliminateBranchChains(fn, stats);

    ASSERT_EQ(fn.blocks.front().instructions.front().opcode, MOpcode::JCC);
    ASSERT_GE(fn.blocks.front().instructions.front().operands.size(), 2u);
    const auto *targetLabel =
        std::get_if<OpLabel>(&fn.blocks.front().instructions.front().operands[1]);
    ASSERT_TRUE(targetLabel != nullptr);
    EXPECT_EQ(targetLabel->name, ".L_target");
    EXPECT_EQ(stats.branchChainsEliminated, 1u);
}

TEST(X86BackendRegressions, BranchChainEliminationRetargetsLabelFirstJcc) {
    MFunction fn{};
    fn.name = "label_first_jcc_branch_chain";

    MBasicBlock entry{};
    entry.label = ".L_label_first_jcc_branch_chain_entry";
    entry.instructions = {
        MInstr::make(MOpcode::JCC, {makeLabelOperand(".L_chain"), makeImmOperand(1)}),
        MInstr::make(MOpcode::JMP, {makeLabelOperand(".L_exit")})};

    MBasicBlock chain{};
    chain.label = ".L_chain";
    chain.instructions = {MInstr::make(MOpcode::JMP, {makeLabelOperand(".L_target")})};

    MBasicBlock target{};
    target.label = ".L_target";
    target.instructions = {MInstr::make(MOpcode::RET)};

    MBasicBlock exit{};
    exit.label = ".L_exit";
    exit.instructions = {MInstr::make(MOpcode::RET)};

    fn.blocks = {entry, chain, target, exit};

    peephole::PeepholeStats stats{};
    peephole::eliminateBranchChains(fn, stats);

    ASSERT_EQ(fn.blocks.front().instructions.front().opcode, MOpcode::JCC);
    const auto *targetLabel =
        std::get_if<OpLabel>(&fn.blocks.front().instructions.front().operands[0]);
    ASSERT_TRUE(targetLabel != nullptr);
    EXPECT_EQ(targetLabel->name, ".L_target");
    EXPECT_EQ(stats.branchChainsEliminated, 1u);
}

TEST(X86BackendRegressions, BranchChainEliminationSkipsMalformedSingleJumpBlock) {
    MFunction fn{};
    fn.name = "malformed_single_jump_chain";

    MBasicBlock entry{};
    entry.label = ".L_malformed_single_jump_chain_entry";
    entry.instructions = {MInstr::make(MOpcode::JMP, {makeLabelOperand(".L_chain")})};

    MBasicBlock chain{};
    chain.label = ".L_chain";
    chain.instructions = {MInstr::make(MOpcode::JMP)};

    fn.blocks = {entry, chain};

    peephole::PeepholeStats stats{};
    EXPECT_NO_THROW(peephole::eliminateBranchChains(fn, stats));
    EXPECT_EQ(stats.branchChainsEliminated, 0u);
}

TEST(X86BackendRegressions, BranchInversionHandlesLabelFirstJcc) {
    MFunction fn{};
    fn.name = "label_first_jcc_inversion";

    MBasicBlock entry{};
    entry.label = ".L_label_first_jcc_inversion_entry";
    entry.instructions = {
        MInstr::make(MOpcode::JCC, {makeLabelOperand(".L_next"), makeImmOperand(1)}),
        MInstr::make(MOpcode::JMP, {makeLabelOperand(".L_exit")})};

    MBasicBlock next{};
    next.label = ".L_next";
    next.instructions = {MInstr::make(MOpcode::RET)};

    MBasicBlock exit{};
    exit.label = ".L_exit";
    exit.instructions = {MInstr::make(MOpcode::RET)};

    fn.blocks = {entry, next, exit};

    peephole::PeepholeStats stats{};
    peephole::invertConditionalBranches(fn, stats);

    ASSERT_EQ(fn.blocks.front().instructions.size(), 1u);
    const MInstr &branch = fn.blocks.front().instructions.front();
    ASSERT_EQ(branch.opcode, MOpcode::JCC);
    const auto *targetLabel = std::get_if<OpLabel>(&branch.operands[0]);
    ASSERT_TRUE(targetLabel != nullptr);
    EXPECT_EQ(targetLabel->name, ".L_exit");
    const auto *cond = std::get_if<OpImm>(&branch.operands[1]);
    ASSERT_TRUE(cond != nullptr);
    EXPECT_EQ(cond->val, 0);
    EXPECT_EQ(stats.branchesInverted, 1u);
}

TEST(X86BackendRegressions, ColdBlockMovementPreservesImplicitFallthrough) {
    MFunction fn{};
    fn.name = "cold_fallthrough";

    MBasicBlock entry{};
    entry.label = ".L_cold_fallthrough_entry";
    entry.instructions = {
        MInstr::make(MOpcode::MOVri, {makeVRegOperand(RegClass::GPR, 1), makeImmOperand(1)})};

    MBasicBlock trap{};
    trap.label = ".L_cold_fallthrough_trap";
    trap.instructions = {MInstr::make(MOpcode::UD2)};

    MBasicBlock hot{};
    hot.label = ".L_cold_fallthrough_hot";
    hot.instructions = {MInstr::make(MOpcode::RET)};

    fn.blocks = {entry, trap, hot};

    peephole::PeepholeStats stats{};
    peephole::moveColdBlocks(fn, stats);

    ASSERT_EQ(fn.blocks.size(), 3u);
    EXPECT_EQ(fn.blocks[1].label, ".L_cold_fallthrough_trap");
    EXPECT_EQ(stats.coldBlocksMoved, 0u);
}

TEST(X86BackendRegressions, TraceLayoutSkipsImplicitFallthroughBlocks) {
    MFunction fn{};
    fn.name = "trace_implicit_fallthrough";

    MBasicBlock entry{};
    entry.label = ".L_trace_implicit_fallthrough_entry";
    entry.instructions = {MInstr::make(MOpcode::JMP, {makeLabelOperand(".L_pred")})};

    MBasicBlock other{};
    other.label = ".L_other";
    other.instructions = {MInstr::make(MOpcode::RET)};

    MBasicBlock other2{};
    other2.label = ".L_other2";
    other2.instructions = {MInstr::make(MOpcode::RET)};

    MBasicBlock pred{};
    pred.label = ".L_pred";
    pred.instructions = {
        MInstr::make(MOpcode::MOVri, {makeVRegOperand(RegClass::GPR, 1), makeImmOperand(7)})};

    MBasicBlock succ{};
    succ.label = ".L_succ";
    succ.instructions = {MInstr::make(MOpcode::RET)};

    fn.blocks = {entry, other, other2, pred, succ};

    peephole::PeepholeStats stats{};
    peephole::traceBlockLayout(fn, stats);

    ASSERT_EQ(fn.blocks.size(), 5u);
    EXPECT_EQ(fn.blocks[0].label, ".L_trace_implicit_fallthrough_entry");
    EXPECT_EQ(fn.blocks[1].label, ".L_other");
    EXPECT_EQ(fn.blocks[2].label, ".L_other2");
    EXPECT_EQ(fn.blocks[3].label, ".L_pred");
    EXPECT_EQ(fn.blocks[4].label, ".L_succ");
    EXPECT_EQ(stats.blocksReordered, 0u);
}

TEST(X86BackendRegressions, FrameStoreForwardingHonorsCrossClassAliases) {
    const OpReg rbp = makePhysReg(RegClass::GPR, static_cast<uint16_t>(PhysReg::RBP));
    const Operand rax = makePhysRegOperand(RegClass::GPR, static_cast<uint16_t>(PhysReg::RAX));
    const Operand rbx = makePhysRegOperand(RegClass::GPR, static_cast<uint16_t>(PhysReg::RBX));
    const Operand xmm0 = makePhysRegOperand(RegClass::XMM, static_cast<uint16_t>(PhysReg::XMM0));

    std::vector<MInstr> instrs = {MInstr::make(MOpcode::MOVrm, {makeMemOperand(rbp, -8), rax}),
                                  MInstr::make(MOpcode::MOVSDrm, {makeMemOperand(rbp, -8), xmm0}),
                                  MInstr::make(MOpcode::MOVmr, {rbx, makeMemOperand(rbp, -8)})};

    peephole::PeepholeStats stats{};
    const std::size_t forwarded = peephole::forwardFrameStoreLoads(instrs, stats);

    EXPECT_EQ(forwarded, 0u);
    ASSERT_EQ(instrs.size(), 3u);
    EXPECT_EQ(instrs[2].opcode, MOpcode::MOVmr);
}

TEST(X86BackendRegressions, DeadFrameStoreEliminationHonorsCrossClassAliases) {
    const OpReg rbp = makePhysReg(RegClass::GPR, static_cast<uint16_t>(PhysReg::RBP));
    const Operand rax = makePhysRegOperand(RegClass::GPR, static_cast<uint16_t>(PhysReg::RAX));
    const Operand rbx = makePhysRegOperand(RegClass::GPR, static_cast<uint16_t>(PhysReg::RBX));
    const Operand xmm0 = makePhysRegOperand(RegClass::XMM, static_cast<uint16_t>(PhysReg::XMM0));

    std::vector<MInstr> overwritten = {
        MInstr::make(MOpcode::MOVrm, {makeMemOperand(rbp, -8), rax}),
        MInstr::make(MOpcode::MOVSDrm, {makeMemOperand(rbp, -8), xmm0})};

    peephole::PeepholeStats stats{};
    EXPECT_EQ(peephole::eliminateDeadFrameStores(overwritten, stats), 1u);
    ASSERT_EQ(overwritten.size(), 1u);
    EXPECT_EQ(overwritten.front().opcode, MOpcode::MOVSDrm);

    std::vector<MInstr> observed = {MInstr::make(MOpcode::MOVrm, {makeMemOperand(rbp, -8), rax}),
                                    MInstr::make(MOpcode::MOVSDmr, {xmm0, makeMemOperand(rbp, -8)}),
                                    MInstr::make(MOpcode::MOVrm, {makeMemOperand(rbp, -8), rbx})};

    peephole::PeepholeStats observedStats{};
    EXPECT_EQ(peephole::eliminateDeadFrameStores(observed, observedStats), 0u);
    EXPECT_EQ(observed.size(), 3u);
}

TEST(X86BackendRegressions, FrameLoweringPreservesCalleeSavedAddressRegisters) {
    struct Case {
        const TargetInfo *target;
        PhysReg addressReg;
        bool asIndex;
        const char *name;
    };

    const std::array<Case, 10> cases{{
        {&sysvTarget(), PhysReg::RBX, false, "sysv_rbx_base"},
        {&sysvTarget(), PhysReg::R12, true, "sysv_r12_index"},
        {&sysvTarget(), PhysReg::R13, false, "sysv_r13_base"},
        {&sysvTarget(), PhysReg::R14, true, "sysv_r14_index"},
        {&sysvTarget(), PhysReg::R15, false, "sysv_r15_base"},
        {&win64Target(), PhysReg::RBX, false, "win64_rbx_base"},
        {&win64Target(), PhysReg::RDI, true, "win64_rdi_index"},
        {&win64Target(), PhysReg::RSI, false, "win64_rsi_base"},
        {&win64Target(), PhysReg::R12, true, "win64_r12_index"},
        {&win64Target(), PhysReg::R15, false, "win64_r15_base"},
    }};

    for (const auto &testCase : cases) {
        MFunction fn{};
        fn.name = testCase.name;

        MBasicBlock entry{};
        entry.label = std::string(".L_") + testCase.name;

        const Operand dst = makePhysRegOperand(RegClass::GPR, static_cast<uint16_t>(PhysReg::RAX));
        const OpReg address =
            makePhysReg(RegClass::GPR, static_cast<uint16_t>(testCase.addressReg));
        const Operand mem =
            testCase.asIndex
                ? makeMemOperand(makePhysReg(RegClass::GPR, static_cast<uint16_t>(PhysReg::RAX)),
                                 address,
                                 2,
                                 16)
                : makeMemOperand(address, 16);
        entry.instructions = {MInstr::make(MOpcode::MOVmr, {dst, mem}), MInstr::make(MOpcode::RET)};
        fn.blocks.push_back(std::move(entry));

        FrameInfo frame{};
        assignSpillSlots(fn, *testCase.target, frame);

        EXPECT_TRUE(std::find(frame.usedCalleeSaved.begin(),
                              frame.usedCalleeSaved.end(),
                              testCase.addressReg) != frame.usedCalleeSaved.end());
    }
}

TEST(X86BackendRegressions, LocalLabelsAreUniqueAcrossFunctions) {
    MFunction first{};
    first.name = "select_label_first";
    MFunction second{};
    second.name = "select_label_second";

    const std::string firstGprLabel = first.makeLocalLabel(".Lselect_gpr_done");
    const std::string secondGprLabel = second.makeLocalLabel(".Lselect_gpr_done");
    EXPECT_NE(firstGprLabel, secondGprLabel);
    EXPECT_NE(firstGprLabel.find("select_label_first"), std::string::npos);
    EXPECT_NE(secondGprLabel.find("select_label_second"), std::string::npos);

    const std::string firstFalseLabel = first.makeLocalLabel(".Lfalse");
    const std::string secondFalseLabel = second.makeLocalLabel(".Lfalse");
    EXPECT_NE(firstFalseLabel, secondFalseLabel);

    const std::string firstSplitLabel = first.makeLocalLabel(".Lsplit");
    const std::string secondSplitLabel = second.makeLocalLabel(".Lsplit");
    EXPECT_NE(firstSplitLabel, secondSplitLabel);
}

TEST(X86BackendRegressions, ZextMasksToSourceBitWidth) {
    ILValue source = val(ILValue::Kind::I64, 0);
    source.bits = 8;

    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0};
    entry.paramKinds = {ILValue::Kind::I64};
    entry.instrs = {op("zext", {source}, 1, ILValue::Kind::I64),
                    op("ret", {val(ILValue::Kind::I64, 1)})};

    ILFunction fn{};
    fn.name = "zext_i8";
    fn.blocks = {entry};

    const CodegenResult result = compile(fn);
    if (!result.errors.empty()) {
        std::cerr << result.errors << '\n';
    }
    ASSERT_TRUE(result.errors.empty());
    EXPECT_NE(result.asmText.find("andq $255"), std::string::npos);
}

TEST(X86BackendRegressions, SextSignExtendsFromSourceBitWidth) {
    ILValue source = val(ILValue::Kind::I64, 0);
    source.bits = 8;

    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0};
    entry.paramKinds = {ILValue::Kind::I64};
    entry.instrs = {op("sext", {source}, 1, ILValue::Kind::I64),
                    op("ret", {val(ILValue::Kind::I64, 1)})};

    ILFunction fn{};
    fn.name = "sext_i8";
    fn.blocks = {entry};

    const CodegenResult result = compile(fn);
    if (!result.errors.empty()) {
        std::cerr << result.errors << '\n';
    }
    ASSERT_TRUE(result.errors.empty());
    EXPECT_NE(result.asmText.find("shlq $56"), std::string::npos);
    EXPECT_NE(result.asmText.find("sarq $56"), std::string::npos);
}

TEST(X86BackendRegressions, TruncMasksBooleanResultWidth) {
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0};
    entry.paramKinds = {ILValue::Kind::I64};
    entry.instrs = {opBits("trunc", {val(ILValue::Kind::I64, 0)}, 1, ILValue::Kind::I1, 1),
                    op("ret", {val(ILValue::Kind::I1, 1)})};

    ILFunction fn{};
    fn.name = "trunc_i1";
    fn.blocks = {entry};

    const CodegenResult result = compile(fn);
    if (!result.errors.empty()) {
        std::cerr << result.errors << '\n';
    }
    ASSERT_TRUE(result.errors.empty());
    EXPECT_NE(result.asmText.find("andq $1"), std::string::npos);
}

TEST(X86BackendRegressions, SelectTrueLabelArmMaterializesBeforeISel) {
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0};
    entry.paramKinds = {ILValue::Kind::I1};
    entry.instrs = {op("select",
                       {val(ILValue::Kind::I1, 0), label("select_true_symbol"), immPtr(0)},
                       1,
                       ILValue::Kind::PTR),
                    op("ret", {val(ILValue::Kind::PTR, 1)})};

    ILFunction fn{};
    fn.name = "select_true_label";
    fn.blocks = {entry};

    const CodegenResult result = compile(fn);
    if (!result.errors.empty()) {
        std::cerr << result.errors << '\n';
    }
    ASSERT_TRUE(result.errors.empty());
    EXPECT_TRUE(containsRegex(result.asmText, R"(leaq\s+_?select_true_symbol\(%rip\))"));
    EXPECT_EQ(result.asmText.find("select pseudo survived"), std::string::npos);
}

TEST(X86BackendRegressions, SelectFalseLabelArmMaterializesBeforeISel) {
    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {0};
    entry.paramKinds = {ILValue::Kind::I1};
    entry.instrs = {op("select",
                       {val(ILValue::Kind::I1, 0), immPtr(0), label("select_false_symbol")},
                       1,
                       ILValue::Kind::PTR),
                    op("ret", {val(ILValue::Kind::PTR, 1)})};

    ILFunction fn{};
    fn.name = "select_false_label";
    fn.blocks = {entry};

    const CodegenResult result = compile(fn);
    if (!result.errors.empty()) {
        std::cerr << result.errors << '\n';
    }
    ASSERT_TRUE(result.errors.empty());
    EXPECT_TRUE(containsRegex(result.asmText, R"(leaq\s+_?select_false_symbol\(%rip\))"));
    EXPECT_EQ(result.asmText.find("select pseudo survived"), std::string::npos);
}

TEST(X86BackendRegressions, StoreLabelValueMaterializesBeforeMemoryStore) {
    ILBlock entry{};
    entry.name = "entry";
    entry.instrs = {op("alloca", {imm(8)}, 0, ILValue::Kind::PTR),
                    op("store", {val(ILValue::Kind::PTR, 0), label("stored_label_symbol")}),
                    op("ret", {})};

    ILFunction fn{};
    fn.name = "store_label_value";
    fn.blocks = {entry};

    const CodegenResult result = compile(fn);
    if (!result.errors.empty()) {
        std::cerr << result.errors << '\n';
    }
    ASSERT_TRUE(result.errors.empty());
    EXPECT_TRUE(containsRegex(result.asmText, R"(leaq\s+_?stored_label_symbol\(%rip\))"));
    EXPECT_NE(result.asmText.find("movq"), std::string::npos);
}

TEST(X86BackendRegressions, XmmAllocaSlotRemapsBelowCalleeSavedArea) {
    MFunction fn{};
    fn.name = "xmm_alloca_slot";

    const Operand rax = makePhysRegOperand(RegClass::GPR, static_cast<uint16_t>(PhysReg::RAX));
    const Operand rbx = makePhysRegOperand(RegClass::GPR, static_cast<uint16_t>(PhysReg::RBX));
    const Operand xmm0 = makePhysRegOperand(RegClass::XMM, static_cast<uint16_t>(PhysReg::XMM0));
    const OpReg rbp = makePhysReg(RegClass::GPR, static_cast<uint16_t>(PhysReg::RBP));

    MBasicBlock entry{};
    entry.label = ".L_xmm_alloca_slot_entry";
    entry.instructions = {MInstr::make(MOpcode::MOVrr, {rbx, rax}),
                          MInstr::make(MOpcode::MOVSDmr, {xmm0, makeMemOperand(rbp, -8)}),
                          MInstr::make(MOpcode::RET)};
    fn.blocks.push_back(std::move(entry));

    FrameInfo frame{};
    assignSpillSlots(fn, sysvTarget(), frame);

    ASSERT_FALSE(frame.usedCalleeSaved.empty());
    EXPECT_EQ(frame.usedCalleeSaved.front(), PhysReg::RBX);
    const auto &load = fn.blocks.front().instructions[1];
    ASSERT_EQ(load.opcode, MOpcode::MOVSDmr);
    const auto *mem = std::get_if<OpMem>(&load.operands[1]);
    ASSERT_TRUE(mem != nullptr);
    EXPECT_EQ(mem->disp, -16);
}

namespace {

void expectThreeOperandOverflowPseudoLowered(MOpcode pseudo, MOpcode real) {
    MFunction fn{};
    fn.name = "three_operand_ovf";

    MBasicBlock entry{};
    entry.label = ".L_three_operand_ovf_entry";
    const Operand dest = makeVRegOperand(RegClass::GPR, 3);
    const Operand lhs = makeVRegOperand(RegClass::GPR, 1);
    const Operand rhs = makeVRegOperand(RegClass::GPR, 2);
    entry.instructions = {MInstr::make(pseudo, {dest, lhs, rhs}), MInstr::make(MOpcode::RET)};
    fn.blocks.push_back(std::move(entry));

    lowerOverflowOps(fn);

    const auto &instructions = fn.blocks.front().instructions;
    ASSERT_GE(instructions.size(), 4u);
    EXPECT_EQ(instructions[0].opcode, MOpcode::MOVrr);
    EXPECT_EQ(instructions[0].operands.size(), 2u);
    EXPECT_EQ(instructions[1].opcode, real);
    EXPECT_EQ(instructions[1].operands.size(), 2u);
    EXPECT_EQ(instructions[2].opcode, MOpcode::JCC);
    ASSERT_FALSE(instructions[2].operands.empty());
    const auto *trapLabel = std::get_if<OpLabel>(&instructions[2].operands.back());
    ASSERT_TRUE(trapLabel != nullptr);
    EXPECT_EQ(trapLabel->name, ".Ltrap_ovf_three_operand_ovf");
}

} // namespace

TEST(X86BackendRegressions, ThreeOperandAddOverflowPseudoLowersToTwoOperandAdd) {
    expectThreeOperandOverflowPseudoLowered(MOpcode::ADDOvfrr, MOpcode::ADDrr);
}

TEST(X86BackendRegressions, ThreeOperandSubOverflowPseudoLowersToTwoOperandSub) {
    expectThreeOperandOverflowPseudoLowered(MOpcode::SUBOvfrr, MOpcode::SUBrr);
}

TEST(X86BackendRegressions, ThreeOperandMulOverflowPseudoLowersToTwoOperandMul) {
    expectThreeOperandOverflowPseudoLowered(MOpcode::IMULOvfrr, MOpcode::IMULrr);
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, argv);
    return zanna_test::run_all_tests();
}
