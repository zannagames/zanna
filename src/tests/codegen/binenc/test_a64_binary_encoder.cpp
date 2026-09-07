//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/codegen/binenc/test_a64_binary_encoder.cpp
// Purpose: Unit tests for A64BinaryEncoder — verifies that AArch64 MIR
//          instructions produce the correct 32-bit machine code words and
//          relocations.
// Key invariants:
//   - Every instruction is exactly 4 bytes
//   - Templates match ARM Architecture Reference Manual encodings
//   - Branch resolution produces correct signed offsets (imm26/imm19)
//   - External BL generates A64Call26 relocations
//   - Prologue/epilogue synthesis matches AsmEmitter behaviour
// Ownership/Lifetime: Standalone test binary.
// Links: codegen/aarch64/binenc/A64BinaryEncoder.hpp
//
//===----------------------------------------------------------------------===//

#include "codegen/aarch64/MachineIR.hpp"
#include "codegen/aarch64/binenc/A64BinaryEncoder.hpp"
#include "codegen/aarch64/binenc/A64Encoding.hpp"
#include "codegen/aarch64/passes/ExpandPseudosPass.hpp"
#include "codegen/common/objfile/CodeSection.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

using namespace zanna::codegen::aarch64;
using namespace zanna::codegen::aarch64::binenc;
using namespace zanna::codegen::objfile;

namespace zanna::codegen::aarch64::binenc {

struct A64BinaryEncoderTestAccess {
    static size_t measureWithKnownTarget(const MInstr &mi,
                                         size_t currentOffset,
                                         size_t targetOffset) {
        A64BinaryEncoder enc;
        MFunction fn;
        fn.name = "test_func";
        fn.isLeaf = true;
        enc.currentFn_ = &fn;
        enc.skipFrame_ = true;
        enc.usePlan_ = false;
        enc.labelOffsets_["target"] = targetOffset;
        std::unordered_set<size_t> assumedLong;
        std::unordered_set<size_t> discoveredLong;
        return enc.measureInstructionSize(
            mi, currentOffset, enc.labelOffsets_, 0, assumedLong, &discoveredLong);
    }

    static std::vector<uint8_t> encodeWithKnownTarget(const MInstr &mi,
                                                      size_t currentOffset,
                                                      size_t targetOffset) {
        A64BinaryEncoder enc;
        MFunction fn;
        fn.name = "test_func";
        fn.isLeaf = true;
        enc.currentFn_ = &fn;
        enc.skipFrame_ = true;
        enc.usePlan_ = false;
        enc.labelOffsets_["target"] = targetOffset;

        CodeSection text;
        text.emitZeros(currentOffset);
        enc.encodeInstruction(mi, text);
        return text.bytes();
    }
};

} // namespace zanna::codegen::aarch64::binenc

static int gFail = 0;

static void check(bool cond, const char *msg, int line) {
    if (!cond) {
        std::cerr << "FAIL line " << line << ": " << msg << "\n";
        ++gFail;
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

// Helper: read a 32-bit LE word from a byte vector at offset.
static uint32_t readWord(const std::vector<uint8_t> &bytes, size_t offset) {
    return static_cast<uint32_t>(bytes[offset]) | (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

// Helper: create a physical GPR register operand.
static MOperand gpr(PhysReg r) {
    return MOperand::regOp(r);
}

// Helper: create a physical FPR register operand.
static MOperand fpr(PhysReg r) {
    return MOperand::regOp(r);
}

// Helper: create an immediate operand.
static MOperand imm(long long val) {
    return MOperand::immOp(val);
}

// Helper: create a condition code operand.
static MOperand cond(const char *c) {
    return MOperand::condOp(c);
}

// Helper: create a label operand.
static MOperand label(const std::string &name) {
    return MOperand::labelOp(name);
}

// Encode a single-block leaf function with given instructions, expanding
// emit-time pseudo forms first exactly as the pipeline's ExpandPseudosPass does.
// Returns the word at instruction index `idx` (skipping BTI prefix).
static uint32_t encodeSingleInstr(const std::vector<MInstr> &instrs, size_t idx = 0) {
    MFunction fn;
    fn.name = "test_func";
    fn.isLeaf = true;
    MBasicBlock bb;
    bb.name = "entry";
    bb.instrs = instrs;
    // Add a Ret at the end.
    bb.instrs.push_back(MInstr{MOpcode::Ret, {}});
    fn.blocks.push_back(std::move(bb));
    (void)expandPseudoInstructions(fn);

    CodeSection text, rodata;
    rodata.defineSymbol("my_global", SymbolBinding::Local, SymbolSection::Rodata);
    rodata.emit8(0);
    A64BinaryEncoder enc;
    enc.encodeFunction(fn, text, rodata, ABIFormat::Darwin);

    // Leaf function with no saved regs and no frame → no prologue.
    // First word is BTI C (always emitted); instructions start at offset 4.
    constexpr size_t kBtiPrefixWords = 1;
    return readWord(text.bytes(), (idx + kBtiPrefixWords) * 4);
}

// Encode a single-block leaf function (after pseudo expansion) and return all
// bytes. Includes BTI prefix (always emitted).
static std::vector<uint8_t> encodeInstrBytes(const std::vector<MInstr> &instrs) {
    MFunction fn;
    fn.name = "test_func";
    fn.isLeaf = true;
    MBasicBlock bb;
    bb.name = "entry";
    bb.instrs = instrs;
    bb.instrs.push_back(MInstr{MOpcode::Ret, {}});
    fn.blocks.push_back(std::move(bb));
    (void)expandPseudoInstructions(fn);

    CodeSection text, rodata;
    rodata.defineSymbol("my_global", SymbolBinding::Local, SymbolSection::Rodata);
    rodata.emit8(0);
    A64BinaryEncoder enc;
    enc.encodeFunction(fn, text, rodata, ABIFormat::Darwin);
    return text.bytes();
}

// Count how many 4-byte words are emitted for a set of instructions
// (excluding the trailing Ret and the BTI prefix).
static size_t countWords(const std::vector<MInstr> &instrs) {
    auto bytes = encodeInstrBytes(instrs);
    constexpr size_t kOverhead = 2; // BTI prefix + Ret
    return (bytes.size() / 4) - kOverhead;
}

// =============================================================================
// Tests
// =============================================================================

static void testAddRRR() {
    // add x0, x1, x2 → 0x8B020020
    MInstr mi{MOpcode::AddRRR, {gpr(PhysReg::X0), gpr(PhysReg::X1), gpr(PhysReg::X2)}};
    uint32_t word = encodeSingleInstr({mi});
    CHECK(word == 0x8B020020);
}

static void testSubRRR() {
    // sub x3, x4, x5 → 0xCB050083
    MInstr mi{MOpcode::SubRRR, {gpr(PhysReg::X3), gpr(PhysReg::X4), gpr(PhysReg::X5)}};
    uint32_t word = encodeSingleInstr({mi});
    CHECK(word == 0xCB050083);
}

static void testAndRRR() {
    // and x0, x1, x2 → 0x8A020020
    MInstr mi{MOpcode::AndRRR, {gpr(PhysReg::X0), gpr(PhysReg::X1), gpr(PhysReg::X2)}};
    uint32_t word = encodeSingleInstr({mi});
    CHECK(word == 0x8A020020);
}

static void testOrrRRR() {
    // orr x0, x1, x2 → 0xAA020020
    MInstr mi{MOpcode::OrrRRR, {gpr(PhysReg::X0), gpr(PhysReg::X1), gpr(PhysReg::X2)}};
    uint32_t word = encodeSingleInstr({mi});
    CHECK(word == 0xAA020020);
}

static void testEorRRR() {
    // eor x0, x1, x2 → 0xCA020020
    MInstr mi{MOpcode::EorRRR, {gpr(PhysReg::X0), gpr(PhysReg::X1), gpr(PhysReg::X2)}};
    uint32_t word = encodeSingleInstr({mi});
    CHECK(word == 0xCA020020);
}

static void testMulRRR() {
    // mul x0, x1, x2 → madd x0, x1, x2, xzr → 0x9B027C20
    MInstr mi{MOpcode::MulRRR, {gpr(PhysReg::X0), gpr(PhysReg::X1), gpr(PhysReg::X2)}};
    uint32_t word = encodeSingleInstr({mi});
    CHECK(word == 0x9B027C20);
}

static void testUmulhRRR() {
    // umulh x0, x1, x2 → 0x9BC27C20
    MInstr mi{MOpcode::UmulhRRR, {gpr(PhysReg::X0), gpr(PhysReg::X1), gpr(PhysReg::X2)}};
    uint32_t word = encodeSingleInstr({mi});
    CHECK(word == 0x9BC27C20);
}

static void testSDivRRR() {
    // sdiv x0, x1, x2 → 0x9AC20C20
    MInstr mi{MOpcode::SDivRRR, {gpr(PhysReg::X0), gpr(PhysReg::X1), gpr(PhysReg::X2)}};
    uint32_t word = encodeSingleInstr({mi});
    CHECK(word == 0x9AC20C20);
}

static void testMSubRRRR() {
    // msub x0, x1, x2, x3 → 0x9B028060
    MInstr mi{MOpcode::MSubRRRR,
              {gpr(PhysReg::X0), gpr(PhysReg::X1), gpr(PhysReg::X2), gpr(PhysReg::X3)}};
    uint32_t word = encodeSingleInstr({mi});
    // template 0x9B008000 | (Rm=2 << 16) | (Ra=3 << 10) | (Rn=1 << 5) | Rd=0
    // = 0x9B008000 | 0x00020000 | 0x00000C00 | 0x00000020 | 0
    CHECK(word == (0x9B008000u | (2u << 16) | (3u << 10) | (1u << 5) | 0u));
}

static void testAddRI() {
    // add x0, x1, #42 → 0x9100A820
    MInstr mi{MOpcode::AddRI, {gpr(PhysReg::X0), gpr(PhysReg::X1), imm(42)}};
    uint32_t word = encodeSingleInstr({mi});
    CHECK(word == (0x91000000u | (42u << 10) | (1u << 5) | 0u));
}

static void testSubRI() {
    // sub x3, x4, #100 → template | (100 << 10) | (4 << 5) | 3
    MInstr mi{MOpcode::SubRI, {gpr(PhysReg::X3), gpr(PhysReg::X4), imm(100)}};
    uint32_t word = encodeSingleInstr({mi});
    CHECK(word == (0xD1000000u | (100u << 10) | (4u << 5) | 3u));
}

static void testMovRR() {
    // mov x0, x1 → orr x0, xzr, x1 → 0xAA0103E0
    MInstr mi{MOpcode::MovRR, {gpr(PhysReg::X0), gpr(PhysReg::X1)}};
    uint32_t word = encodeSingleInstr({mi});
    CHECK(word == (0xAA0003E0u | (1u << 16) | 0u));
}

static void testMovRI_small() {
    // movz x0, #0x1234 → 0xD2824680
    MInstr mi{MOpcode::MovRI, {gpr(PhysReg::X0), imm(0x1234)}};
    uint32_t word = encodeSingleInstr({mi});
    CHECK(word == (0xD2800000u | (0x1234u << 5) | 0u));
}

static void testLslRI() {
    // lsl x0, x1, #4 → ubfm x0, x1, #60, #59
    MInstr mi{MOpcode::LslRI, {gpr(PhysReg::X0), gpr(PhysReg::X1), imm(4)}};
    uint32_t word = encodeSingleInstr({mi});
    uint32_t immr = (64 - 4) & 63; // 60
    uint32_t imms = 63 - 4;        // 59
    CHECK(word == (0xD3400000u | (immr << 16) | (imms << 10) | (1u << 5) | 0u));
}

static void testLsrRI() {
    // lsr x0, x1, #8 → ubfm x0, x1, #8, #63
    MInstr mi{MOpcode::LsrRI, {gpr(PhysReg::X0), gpr(PhysReg::X1), imm(8)}};
    uint32_t word = encodeSingleInstr({mi});
    CHECK(word == (0xD3400000u | (8u << 16) | (63u << 10) | (1u << 5) | 0u));
}

static void testAsrRI() {
    // asr x0, x1, #8 → sbfm x0, x1, #8, #63
    MInstr mi{MOpcode::AsrRI, {gpr(PhysReg::X0), gpr(PhysReg::X1), imm(8)}};
    uint32_t word = encodeSingleInstr({mi});
    CHECK(word == (0x93400000u | (8u << 16) | (63u << 10) | (1u << 5) | 0u));
}

static void testCmpRR() {
    // cmp x0, x1 → subs xzr, x0, x1
    MInstr mi{MOpcode::CmpRR, {gpr(PhysReg::X0), gpr(PhysReg::X1)}};
    uint32_t word = encodeSingleInstr({mi});
    // kSubsRRR | (Rm=1 << 16) | (Rn=0 << 5) | Rd=31
    CHECK(word == (0xEB000000u | (1u << 16) | (0u << 5) | 31u));
}

static void testCmpRI() {
    // cmp x0, #42 → subs xzr, x0, #42
    MInstr mi{MOpcode::CmpRI, {gpr(PhysReg::X0), imm(42)}};
    uint32_t word = encodeSingleInstr({mi});
    CHECK(word == (0xF1000000u | (42u << 10) | (0u << 5) | 31u));
}

static void testTstRR() {
    // tst x0, x1 → ands xzr, x0, x1
    MInstr mi{MOpcode::TstRR, {gpr(PhysReg::X0), gpr(PhysReg::X1)}};
    uint32_t word = encodeSingleInstr({mi});
    CHECK(word == (0xEA000000u | (1u << 16) | (0u << 5) | 31u));
}

static void testCset() {
    // cset x0, eq → csinc x0, xzr, xzr, ne (inverted: eq=0 → ne=1)
    MInstr mi{MOpcode::Cset, {gpr(PhysReg::X0), cond("eq")}};
    uint32_t word = encodeSingleInstr({mi});
    // kCset | (invertCond(0)=1 << 12) | Rd=0
    CHECK(word == (0x9A9F07E0u | (1u << 12) | 0u));
}

static void testCsel() {
    // csel x0, x1, x2, lt → 0x9A82B020
    MInstr mi{MOpcode::Csel, {gpr(PhysReg::X0), gpr(PhysReg::X1), gpr(PhysReg::X2), cond("lt")}};
    uint32_t word = encodeSingleInstr({mi});
    // kCsel | (Rm=2 << 16) | (cc=0xB << 12) | (Rn=1 << 5) | Rd=0
    CHECK(word == (0x9A800000u | (2u << 16) | (0xBu << 12) | (1u << 5) | 0u));
}

static void testFAddRRR() {
    // fadd d0, d1, d2
    MInstr mi{MOpcode::FAddRRR, {fpr(PhysReg::V0), fpr(PhysReg::V1), fpr(PhysReg::V2)}};
    uint32_t word = encodeSingleInstr({mi});
    CHECK(word == (0x1E602800u | (2u << 16) | (1u << 5) | 0u));
}

static void testFSubRRR() {
    // fsub d0, d1, d2
    MInstr mi{MOpcode::FSubRRR, {fpr(PhysReg::V0), fpr(PhysReg::V1), fpr(PhysReg::V2)}};
    uint32_t word = encodeSingleInstr({mi});
    CHECK(word == (0x1E603800u | (2u << 16) | (1u << 5) | 0u));
}

static void testFCmpRR() {
    // fcmp d0, d1 (Rd=0)
    MInstr mi{MOpcode::FCmpRR, {fpr(PhysReg::V0), fpr(PhysReg::V1)}};
    uint32_t word = encodeSingleInstr({mi});
    CHECK(word == (0x1E602000u | (1u << 16) | (0u << 5)));
}

static void testSCvtF() {
    // scvtf d0, x1
    MInstr mi{MOpcode::SCvtF, {fpr(PhysReg::V0), gpr(PhysReg::X1)}};
    uint32_t word = encodeSingleInstr({mi});
    CHECK(word == (0x9E620000u | (1u << 5) | 0u));
}

static void testFCvtZS() {
    // fcvtzs x0, d1
    MInstr mi{MOpcode::FCvtZS, {gpr(PhysReg::X0), fpr(PhysReg::V1)}};
    uint32_t word = encodeSingleInstr({mi});
    CHECK(word == (0x9E780000u | (1u << 5) | 0u));
}

static void testRet() {
    // Leaf function with no frame → ret is just 0xD65F03C0.
    MFunction fn;
    fn.name = "leaf_ret";
    fn.isLeaf = true;
    MBasicBlock bb;
    bb.name = "entry";
    bb.instrs.push_back(MInstr{MOpcode::Ret, {}});
    fn.blocks.push_back(std::move(bb));

    CodeSection text, rodata;
    A64BinaryEncoder enc;
    enc.encodeFunction(fn, text, rodata, ABIFormat::Darwin);

    // BTI C (4 bytes) + Ret (4 bytes) = 8 bytes total
    CHECK(text.bytes().size() == 8);
    CHECK(readWord(text.bytes(), 0) == 0xD503245F); // BTI C
    CHECK(readWord(text.bytes(), 4) == 0xD65F03C0); // Ret
}

static void testBranchForwardBackward() {
    // Two blocks: entry branches to target, target branches back.
    MFunction fn;
    fn.name = "branch_test";
    fn.isLeaf = true;

    MBasicBlock entry;
    entry.name = "entry";
    entry.instrs.push_back(MInstr{MOpcode::Br, {label("target")}});
    fn.blocks.push_back(std::move(entry));

    MBasicBlock target;
    target.name = "target";
    target.instrs.push_back(MInstr{MOpcode::Br, {label("entry")}});
    fn.blocks.push_back(std::move(target));

    // No Ret needed — the branches form a loop.
    // Add Ret at the end of target to make it a valid function... but actually
    // the encoder doesn't require it. Let's just test with the loop.

    CodeSection text, rodata;
    A64BinaryEncoder enc;
    enc.encodeFunction(fn, text, rodata, ABIFormat::Darwin);

    // BTI C (4 bytes) + entry branch (4 bytes) + target branch (4 bytes) = 12 bytes
    CHECK(text.bytes().size() == 12);
    // Skip BTI at offset 0; entry at offset 4, target at offset 8
    uint32_t fwd = readWord(text.bytes(), 4);
    uint32_t bwd = readWord(text.bytes(), 8);

    // Forward: b target → +4 bytes → imm26 = 1
    CHECK(fwd == 0x14000001);
    // Backward: b entry → -4 bytes → imm26 = -1
    CHECK(bwd == (0x14000000u | (0x3FFFFFFu)));
}

static void testBCondForward() {
    MFunction fn;
    fn.name = "bcond_test";
    fn.isLeaf = true;

    MBasicBlock entry;
    entry.name = "entry";
    // b.eq target (cond=0x0, forward)
    entry.instrs.push_back(MInstr{MOpcode::BCond, {cond("eq"), label("target")}});
    // nop placeholder
    entry.instrs.push_back(MInstr{MOpcode::MovRR, {gpr(PhysReg::X0), gpr(PhysReg::X0)}});
    fn.blocks.push_back(std::move(entry));

    MBasicBlock target;
    target.name = "target";
    target.instrs.push_back(MInstr{MOpcode::Ret, {}});
    fn.blocks.push_back(std::move(target));

    CodeSection text, rodata;
    A64BinaryEncoder enc;
    enc.encodeFunction(fn, text, rodata, ABIFormat::Darwin);

    // BTI at offset 0; entry at offset 4 = b.eq; offset 8 = mov; target at offset 12 = ret
    // b.eq forward 8 bytes (2 instrs) → imm19=2
    uint32_t bcond = readWord(text.bytes(), 4);
    CHECK(bcond == 0x54000040);
}

static void testCbzForward() {
    MFunction fn;
    fn.name = "cbz_test";
    fn.isLeaf = true;

    MBasicBlock entry;
    entry.name = "entry";
    entry.instrs.push_back(MInstr{MOpcode::Cbz, {gpr(PhysReg::X0), label("target")}});
    entry.instrs.push_back(MInstr{MOpcode::MovRR, {gpr(PhysReg::X0), gpr(PhysReg::X0)}});
    fn.blocks.push_back(std::move(entry));

    MBasicBlock target;
    target.name = "target";
    target.instrs.push_back(MInstr{MOpcode::Ret, {}});
    fn.blocks.push_back(std::move(target));

    CodeSection text, rodata;
    A64BinaryEncoder enc;
    enc.encodeFunction(fn, text, rodata, ABIFormat::Darwin);

    // BTI at offset 0; cbz at offset 4
    uint32_t cbz = readWord(text.bytes(), 4);
    CHECK(cbz == 0xB4000040);
}

static void testFarConditionalBranchFallbacks() {
    constexpr size_t kBranchOffset = 4;
    constexpr size_t kFarTargetOffset = 1048584;

    const MInstr bcond{MOpcode::BCond, {cond("eq"), label("target")}};
    CHECK(A64BinaryEncoderTestAccess::measureWithKnownTarget(
              bcond, kBranchOffset, kFarTargetOffset) == 8);
    const auto bcondBytes =
        A64BinaryEncoderTestAccess::encodeWithKnownTarget(bcond, kBranchOffset, kFarTargetOffset);
    CHECK(readWord(bcondBytes, 4) == 0x54000041);
    CHECK(readWord(bcondBytes, 8) == 0x14040000);

    const MInstr cbz{MOpcode::Cbz, {gpr(PhysReg::X0), label("target")}};
    CHECK(A64BinaryEncoderTestAccess::measureWithKnownTarget(
              cbz, kBranchOffset, kFarTargetOffset) == 8);
    const auto cbzBytes =
        A64BinaryEncoderTestAccess::encodeWithKnownTarget(cbz, kBranchOffset, kFarTargetOffset);
    CHECK(readWord(cbzBytes, 4) == 0xB5000040);
    CHECK(readWord(cbzBytes, 8) == 0x14040000);

    const MInstr cbnz{MOpcode::Cbnz, {gpr(PhysReg::X0), label("target")}};
    CHECK(A64BinaryEncoderTestAccess::measureWithKnownTarget(
              cbnz, kBranchOffset, kFarTargetOffset) == 8);
    const auto cbnzBytes =
        A64BinaryEncoderTestAccess::encodeWithKnownTarget(cbnz, kBranchOffset, kFarTargetOffset);
    CHECK(readWord(cbnzBytes, 4) == 0xB4000040);
    CHECK(readWord(cbnzBytes, 8) == 0x14040000);
}

static void testFarForwardConditionalBranchesPatchLongForm() {
    MFunction fn;
    fn.name = "far_forward_conditional";
    fn.isLeaf = true;

    MBasicBlock entry;
    entry.name = "entry";
    entry.instrs.push_back(MInstr{MOpcode::BCond, {cond("eq"), label("target")}});
    entry.instrs.push_back(MInstr{MOpcode::Cbz, {gpr(PhysReg::X0), label("target")}});
    entry.instrs.push_back(MInstr{MOpcode::Cbnz, {gpr(PhysReg::X0), label("target")}});
    constexpr size_t kNopCount = 262143;
    for (size_t i = 0; i < kNopCount; ++i)
        entry.instrs.push_back(MInstr{MOpcode::MovRR, {gpr(PhysReg::X1), gpr(PhysReg::X1)}});
    fn.blocks.push_back(std::move(entry));

    MBasicBlock target;
    target.name = "target";
    target.instrs.push_back(MInstr{MOpcode::Ret, {}});
    fn.blocks.push_back(std::move(target));

    CodeSection text, rodata;
    A64BinaryEncoder enc;
    enc.encodeFunction(fn, text, rodata, ABIFormat::Darwin);

    CHECK(readWord(text.bytes(), 4) == 0x54000041);  // b.ne +8
    CHECK(readWord(text.bytes(), 8) == 0x14040004);  // b target
    CHECK(readWord(text.bytes(), 12) == 0xB5000040); // cbnz x0, +8
    CHECK(readWord(text.bytes(), 16) == 0x14040002); // b target
    CHECK(readWord(text.bytes(), 20) == 0xB4000040); // cbz x0, +8
    CHECK(readWord(text.bytes(), 24) == 0x14040000); // b target
}

static void testExternalCall() {
    // bl to an external function should produce a relocation.
    MFunction fn;
    fn.name = "call_test";
    fn.isLeaf = true; // Still technically leaf in MIR...
    MBasicBlock bb;
    bb.name = "entry";
    bb.instrs.push_back(MInstr{MOpcode::Bl, {label("rt_print_i64")}});
    bb.instrs.push_back(MInstr{MOpcode::Ret, {}});
    fn.blocks.push_back(std::move(bb));

    CodeSection text, rodata;
    A64BinaryEncoder enc;
    enc.encodeFunction(fn, text, rodata, ABIFormat::Darwin);

    // BTI at offset 0; BL at offset 4
    uint32_t bl = readWord(text.bytes(), 4);
    CHECK(bl == 0x94000000);

    // Should have an A64Call26 relocation at offset 4 (after BTI).
    CHECK(text.relocations().size() >= 1);
    CHECK(text.relocations()[0].kind == RelocKind::A64Call26);
    CHECK(text.relocations()[0].offset == 4);
}

static void testPrologueEpilogue() {
    // Non-leaf function: should get stp x29,x30,[sp,#-16]! ; mov x29,sp ; ... ; ldp
    // x29,x30,[sp],#16 ; ret
    MFunction fn;
    fn.name = "nonleaf";
    fn.isLeaf = false; // has calls
    MBasicBlock bb;
    bb.name = "entry";
    bb.instrs.push_back(MInstr{MOpcode::Ret, {}});
    fn.blocks.push_back(std::move(bb));

    CodeSection text, rodata;
    A64BinaryEncoder enc;
    enc.encodeFunction(fn, text, rodata, ABIFormat::Darwin);

    // BTI C + PACIASP + stp x29,x30 + mov x29,sp + ldp x29,x30 + AUTIASP + ret = 7 instrs
    CHECK(text.bytes().size() == 28); // 7 * 4

    // Offset 0: BTI C
    CHECK(readWord(text.bytes(), 0) == 0xD503245F);
    // Offset 4: PACIASP
    CHECK(readWord(text.bytes(), 4) == 0xD503233F);

    // Offset 8: stp x29, x30, [sp, #-16]! (pre-indexed GPR pair)
    uint32_t stp = readWord(text.bytes(), 8);
    uint32_t expected_stp = 0xA9800000u | ((static_cast<uint32_t>(-2) & 0x7F) << 15) |
                            (hwGPR(PhysReg::X30) << 10) | (hwGPR(PhysReg::SP) << 5) |
                            hwGPR(PhysReg::X29);
    CHECK(stp == expected_stp);

    // Offset 12: mov x29, sp → add x29, sp, #0
    uint32_t mov = readWord(text.bytes(), 12);
    CHECK(mov == (0x91000000u | (0u << 10) | (hwGPR(PhysReg::SP) << 5) | hwGPR(PhysReg::X29)));

    // Offset 16: ldp x29, x30, [sp], #16 (post-indexed)
    uint32_t ldp = readWord(text.bytes(), 16);
    uint32_t expected_ldp = 0xA8C00000u | ((static_cast<uint32_t>(2) & 0x7F) << 15) |
                            (hwGPR(PhysReg::X30) << 10) | (hwGPR(PhysReg::SP) << 5) |
                            hwGPR(PhysReg::X29);
    CHECK(ldp == expected_ldp);

    // Offset 20: AUTIASP
    CHECK(readWord(text.bytes(), 20) == 0xD50323BF);
    // Offset 24: ret
    CHECK(readWord(text.bytes(), 24) == 0xD65F03C0);
}

static void testMainInit() {
    // The raw encoder must not inject runtime startup policy based on function name.
    MFunction fn;
    fn.name = "main";
    fn.isLeaf = true;
    MBasicBlock bb;
    bb.name = "entry";
    bb.instrs.push_back(MInstr{MOpcode::Ret, {}});
    fn.blocks.push_back(std::move(bb));

    CodeSection text, rodata;
    A64BinaryEncoder enc;
    enc.encodeFunction(fn, text, rodata, ABIFormat::Darwin);

    CHECK(text.bytes().size() == 8);
    CHECK(readWord(text.bytes(), 0) == 0xD503245F);
    CHECK(readWord(text.bytes(), 4) == 0xD65F03C0);

    size_t callRelocs = 0;
    for (const auto &r : text.relocations())
        if (r.kind == RelocKind::A64Call26)
            ++callRelocs;
    CHECK(callRelocs == 0);
}

static void testSubSpChunking() {
    // Large frame should produce multiple sub instructions.
    // 5008 = 4096 + 912, producing exactly 2 sub instructions.
    MFunction fn;
    fn.name = "large_frame";
    fn.isLeaf = false;
    fn.localFrameSize = 5008;
    MBasicBlock bb;
    bb.name = "entry";
    bb.instrs.push_back(MInstr{MOpcode::Ret, {}});
    fn.blocks.push_back(std::move(bb));

    CodeSection text, rodata;
    A64BinaryEncoder enc;
    enc.encodeFunction(fn, text, rodata, ABIFormat::Darwin);

    // BTI + PACIASP + stp + mov + sub + sub + ...
    // offset 16: sub sp, sp, #1, lsl #12  (= 4096) — after BTI(0)+PACIASP(4)+stp(8)+mov(12)
    uint32_t sub1 = readWord(text.bytes(), 16);
    CHECK(sub1 == (0xD1400000u | (1u << 10) | (hwGPR(PhysReg::SP) << 5) | hwGPR(PhysReg::SP)));

    // offset 20: sub sp, sp, #912
    uint32_t sub2 = readWord(text.bytes(), 20);
    CHECK(sub2 == (0xD1000000u | (912u << 10) | (hwGPR(PhysReg::SP) << 5) | hwGPR(PhysReg::SP)));
}

static void testCalleeSavedPair() {
    // Function with 2 callee-saved GPRs should emit stp/ldp pair.
    MFunction fn;
    fn.name = "callee_saved";
    fn.isLeaf = false;
    fn.savedGPRs = {PhysReg::X19, PhysReg::X20};
    MBasicBlock bb;
    bb.name = "entry";
    bb.instrs.push_back(MInstr{MOpcode::Ret, {}});
    fn.blocks.push_back(std::move(bb));

    CodeSection text, rodata;
    A64BinaryEncoder enc;
    enc.encodeFunction(fn, text, rodata, ABIFormat::Darwin);

    // BTI + PACIASP + stp x29,x30 + mov x29,sp + stp x19,x20 +
    // ldp x19,x20 + ldp x29,x30 + AUTIASP + ret = 9 instrs = 36 bytes
    CHECK(text.bytes().size() == 36);

    // Fifth instruction (offset 16) = stp x19, x20, [sp, #-16]! (after BTI+PACIASP+stp+mov)
    uint32_t stp = readWord(text.bytes(), 16);
    uint32_t expected = 0xA9800000u | ((static_cast<uint32_t>(-2) & 0x7F) << 15) |
                        (hwGPR(PhysReg::X20) << 10) | (hwGPR(PhysReg::SP) << 5) |
                        hwGPR(PhysReg::X19);
    CHECK(stp == expected);
}

static void testCondCodeMapping() {
    // Test all condition code mappings.
    CHECK(condCode("eq") == 0x0);
    CHECK(condCode("ne") == 0x1);
    CHECK(condCode("hs") == 0x2);
    CHECK(condCode("cs") == 0x2);
    CHECK(condCode("lo") == 0x3);
    CHECK(condCode("cc") == 0x3);
    CHECK(condCode("mi") == 0x4);
    CHECK(condCode("pl") == 0x5);
    CHECK(condCode("vs") == 0x6);
    CHECK(condCode("vc") == 0x7);
    CHECK(condCode("hi") == 0x8);
    CHECK(condCode("ls") == 0x9);
    CHECK(condCode("ge") == 0xA);
    CHECK(condCode("lt") == 0xB);
    CHECK(condCode("gt") == 0xC);
    CHECK(condCode("le") == 0xD);
    CHECK(condCode("al") == 0xE);
    CHECK(condCode("nv") == 0xF);

    bool threw = false;
    try {
        (void)condCode("bad");
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    CHECK(threw);

    threw = false;
    try {
        (void)condCode(nullptr);
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    CHECK(threw);
}

static void testEncoderValidationRejectsBadOperands() {
    {
        bool threw = false;
        try {
            MInstr bad{MOpcode::AddRRR, {gpr(PhysReg::X0), gpr(PhysReg::X1)}};
            (void)encodeInstrBytes({bad});
        } catch (const std::runtime_error &ex) {
            threw = std::string(ex.what()).find("requires exactly 3") != std::string::npos;
        }
        CHECK(threw);
    }

    {
        MOperand bad = gpr(PhysReg::X0);
        bad.reg.cls = RegClass::FPR;
        bool threw = false;
        try {
            MInstr mi{MOpcode::AddRRR, {bad, gpr(PhysReg::X1), gpr(PhysReg::X2)}};
            (void)encodeInstrBytes({mi});
        } catch (const std::runtime_error &ex) {
            threw = std::string(ex.what()).find("register class") != std::string::npos;
        }
        CHECK(threw);
    }

    {
        bool threw = false;
        try {
            MInstr mi{MOpcode::FAddRRR, {gpr(PhysReg::X0), fpr(PhysReg::V1), fpr(PhysReg::V2)}};
            (void)encodeInstrBytes({mi});
        } catch (const std::invalid_argument &ex) {
            threw = std::string(ex.what()).find("expected an FPR") != std::string::npos;
        }
        CHECK(threw);
    }

    {
        bool threw = false;
        try {
            MInstr mi{MOpcode::LslRI, {gpr(PhysReg::X0), gpr(PhysReg::X1), imm(64)}};
            (void)encodeInstrBytes({mi});
        } catch (const std::out_of_range &ex) {
            threw = std::string(ex.what()).find("shift amount") != std::string::npos;
        }
        CHECK(threw);
    }

    {
        bool threw = false;
        try {
            MInstr mi{MOpcode::BCond, {cond("bad"), label("target")}};
            (void)encodeInstrBytes({mi});
        } catch (const std::invalid_argument &ex) {
            threw = std::string(ex.what()).find("invalid condition") != std::string::npos;
        }
        CHECK(threw);
    }

    {
        bool threw = false;
        try {
            MInstr mi{MOpcode::Cset, {gpr(PhysReg::X0), cond("al")}};
            (void)encodeInstrBytes({mi});
        } catch (const std::invalid_argument &ex) {
            threw = std::string(ex.what()).find("not valid for conditional instruction") !=
                    std::string::npos;
        }
        CHECK(threw);
    }
}

static void testBlr() {
    // blr x8
    MInstr mi{MOpcode::Blr, {gpr(PhysReg::X8)}};
    uint32_t word = encodeSingleInstr({mi});
    CHECK(word == (0xD63F0000u | (8u << 5)));
}

static void testAddsSubsRRR() {
    // adds x0, x1, x2
    MInstr mi1{MOpcode::AddsRRR, {gpr(PhysReg::X0), gpr(PhysReg::X1), gpr(PhysReg::X2)}};
    CHECK(encodeSingleInstr({mi1}) == (0xAB000000u | (2u << 16) | (1u << 5) | 0u));

    // subs x0, x1, x2
    MInstr mi2{MOpcode::SubsRRR, {gpr(PhysReg::X0), gpr(PhysReg::X1), gpr(PhysReg::X2)}};
    CHECK(encodeSingleInstr({mi2}) == (0xEB000000u | (2u << 16) | (1u << 5) | 0u));
}

static void testLdpStpRegFpImm() {
    // ldp x0, x1, [x29, #16]
    MInstr ldp{MOpcode::LdpRegFpImm, {gpr(PhysReg::X0), gpr(PhysReg::X1), imm(16)}};
    uint32_t w1 = encodeSingleInstr({ldp});
    // kLdpGpr | (imm7=16/8=2 << 15) | (Rt2=1 << 10) | (Rn=29 << 5) | Rt=0
    CHECK(w1 == (0xA9400000u | (2u << 15) | (1u << 10) | (29u << 5) | 0u));

    // stp x0, x1, [x29, #-16]
    MInstr stp{MOpcode::StpRegFpImm, {gpr(PhysReg::X0), gpr(PhysReg::X1), imm(-16)}};
    uint32_t w2 = encodeSingleInstr({stp});
    // imm7 = -16/8 = -2 → 0x7E
    CHECK(w2 == (0xA9000000u | ((static_cast<uint32_t>(-2) & 0x7F) << 15) | (1u << 10) |
                 (29u << 5) | 0u));
}

static void testPairOffsetFallback() {
    MInstr unaligned{MOpcode::LdpRegFpImm, {gpr(PhysReg::X0), gpr(PhysReg::X1), imm(12)}};
    CHECK(countWords({unaligned}) == 2);

    auto bytes = encodeInstrBytes({unaligned});
    const uint32_t first = readWord(bytes, 4);
    const uint32_t second = readWord(bytes, 8);
    CHECK(first == (kLdurGpr | (12u << 12) | (29u << 5) | 0u));
    CHECK(second == (kLdurGpr | (20u << 12) | (29u << 5) | 1u));

    MInstr outOfRange{MOpcode::StpRegFpImm, {gpr(PhysReg::X2), gpr(PhysReg::X3), imm(512)}};
    CHECK(countWords({outOfRange}) == 2);
}

static void testNarrowIntegerMemoryEncoding() {
    MInstr ldr8{MOpcode::Ldr8RegBaseImm, {gpr(PhysReg::X0), gpr(PhysReg::X1), imm(3)}};
    CHECK(encodeSingleInstr({ldr8}) == (kLdur8Gpr | (3u << 12) | (1u << 5) | 0u));

    MInstr str16{MOpcode::Str16RegFpImm, {gpr(PhysReg::X2), imm(-4)}};
    CHECK(encodeSingleInstr({str16}) ==
          (kStur16Gpr | ((static_cast<uint32_t>(-4) & 0x1FFu) << 12) | (29u << 5) | 2u));

    MInstr ldr32{MOpcode::Ldr32RegFpImm, {gpr(PhysReg::X3), imm(12)}};
    CHECK(encodeSingleInstr({ldr32}) == (kLdur32Gpr | (12u << 12) | (29u << 5) | 3u));
}

static void testLargeStoreAvoidsSourceAndBaseScratch() {
    MInstr mi{MOpcode::StrRegBaseImm, {gpr(PhysReg::X9), gpr(PhysReg::X16), imm(32768)}};
    auto bytes = encodeInstrBytes({mi});

    // BTI + mov scratch + add scratch,base,scratch + str source,[scratch] + ret
    // (the expansion picks x17 because x9 is the source and x16 the base).
    CHECK(bytes.size() >= 20);
    uint32_t store = readWord(bytes, bytes.size() - 8);
    CHECK((store & 31u) == 9u);         // Rt = original store source X9.
    CHECK(((store >> 5) & 31u) == 17u); // Rn = scratch X17, not X9 or X16.
}

static void testVariableShift() {
    // lslv x0, x1, x2
    MInstr mi{MOpcode::LslvRRR, {gpr(PhysReg::X0), gpr(PhysReg::X1), gpr(PhysReg::X2)}};
    CHECK(encodeSingleInstr({mi}) == (0x9AC02000u | (2u << 16) | (1u << 5) | 0u));
}

static void testHighRegisters() {
    // add x28, x19, x20 — tests registers with 5-bit encoding > 15.
    MInstr mi{MOpcode::AddRRR, {gpr(PhysReg::X28), gpr(PhysReg::X19), gpr(PhysReg::X20)}};
    uint32_t word = encodeSingleInstr({mi});
    CHECK(word == (0x8B000000u | (20u << 16) | (19u << 5) | 28u));
}

static void testFMovRR() {
    // fmov d0, d1
    MInstr mi{MOpcode::FMovRR, {fpr(PhysReg::V0), fpr(PhysReg::V1)}};
    uint32_t word = encodeSingleInstr({mi});
    CHECK(word == (0x1E604000u | (1u << 5) | 0u));
}

static void testHighFPR() {
    // fadd d16, d17, d18 — tests FPR encoding with values > 15.
    MInstr mi{MOpcode::FAddRRR, {fpr(PhysReg::V16), fpr(PhysReg::V17), fpr(PhysReg::V18)}};
    uint32_t word = encodeSingleInstr({mi});
    CHECK(word == (0x1E602800u | (18u << 16) | (17u << 5) | 16u));
}

static void testStrRegSpImm() {
    // str x0, [sp, #16] — scaled unsigned offset, offset/8 = 2
    MInstr mi{MOpcode::StrRegSpImm, {gpr(PhysReg::X0), imm(16)}};
    uint32_t word = encodeSingleInstr({mi});
    CHECK(word == (0xF9000000u | (2u << 10) | (hwGPR(PhysReg::SP) << 5) | 0u));
}

static void testAdrpAddPageOff() {
    // adrp x0, sym → relocation
    // add x0, x0, sym@PAGEOFF → relocation
    MFunction fn;
    fn.name = "adrp_test";
    fn.isLeaf = true;
    MBasicBlock bb;
    bb.name = "entry";
    bb.instrs.push_back(MInstr{MOpcode::AdrPage, {gpr(PhysReg::X0), label("my_global")}});
    bb.instrs.push_back(
        MInstr{MOpcode::AddPageOff, {gpr(PhysReg::X0), gpr(PhysReg::X0), label("my_global")}});
    bb.instrs.push_back(MInstr{MOpcode::Ret, {}});
    fn.blocks.push_back(std::move(bb));

    CodeSection text, rodata;
    rodata.emit8(0xAA);
    const size_t rodataOffset = rodata.currentOffset();
    rodata.defineSymbol("my_global", SymbolBinding::Local, SymbolSection::Rodata);
    rodata.emit8(0);
    A64BinaryEncoder enc;
    enc.encodeFunction(fn, text, rodata, ABIFormat::Darwin);

    // BTI at offset 0; adrp at offset 4; add at offset 8
    CHECK(readWord(text.bytes(), 4) == 0x90000000);
    CHECK(readWord(text.bytes(), 8) == (0x91000000u | (0u << 10) | (0u << 5) | 0u));

    // Should have A64AdrpPage21 and A64AddPageOff12 relocations.
    bool hasAdrp = false, hasAdd = false;
    for (const auto &r : text.relocations()) {
        if (r.kind == RelocKind::A64AdrpPage21) {
            hasAdrp = true;
            CHECK(r.targetSection == SymbolSection::Rodata);
            CHECK(r.targetOffsetValid);
            CHECK(r.targetOffset == rodataOffset);
            CHECK(r.targetSectionIdentityValid);
            CHECK(r.targetSectionIdentity == rodata.sectionIdentity());
        }
        if (r.kind == RelocKind::A64AddPageOff12) {
            hasAdd = true;
            CHECK(r.targetSection == SymbolSection::Rodata);
            CHECK(r.targetOffsetValid);
            CHECK(r.targetOffset == rodataOffset);
            CHECK(r.targetSectionIdentityValid);
            CHECK(r.targetSectionIdentity == rodata.sectionIdentity());
        }
    }
    CHECK(hasAdrp);
    CHECK(hasAdd);
}

static void testSymbolDefined() {
    // The function symbol should be defined in the text section's symbol table.
    MFunction fn;
    fn.name = "my_func";
    fn.isLeaf = true;
    MBasicBlock bb;
    bb.name = "entry";
    bb.instrs.push_back(MInstr{MOpcode::Ret, {}});
    fn.blocks.push_back(std::move(bb));

    CodeSection text, rodata;
    A64BinaryEncoder enc;
    enc.encodeFunction(fn, text, rodata, ABIFormat::Darwin);

    // Check that "my_func" appears in the symbol table.
    bool found = false;
    for (uint32_t i = 0; i < text.symbols().count(); ++i) {
        if (text.symbols().at(i).name == "my_func") {
            found = true;
            CHECK(text.symbols().at(i).binding == SymbolBinding::Global);
            break;
        }
    }
    CHECK(found);
}

// =============================================================================
// Optimization tests
// =============================================================================

static void testAndRI_logicalImm() {
    // AND X0, X1, #0xFF → single instruction (logical immediate encodable).
    // enc(0xFF) = N=1, immr=0, imms=7 → 0x1007.
    // kAndImm | (0x1007 << 10) | (X1 << 5) | X0 = 0x92401C20
    MInstr mi{MOpcode::AndRI, {gpr(PhysReg::X0), gpr(PhysReg::X1), imm(0xFF)}};
    CHECK(countWords({mi}) == 1);
    CHECK(encodeSingleInstr({mi}) == 0x92401C20);
}

static void testOrrRI_logicalImm() {
    // ORR X0, X1, #0xFF → kOrrImm | (0x1007 << 10) | (X1 << 5) | X0 = 0xB2401C20
    MInstr mi{MOpcode::OrrRI, {gpr(PhysReg::X0), gpr(PhysReg::X1), imm(0xFF)}};
    CHECK(countWords({mi}) == 1);
    CHECK(encodeSingleInstr({mi}) == 0xB2401C20);
}

static void testAndRI_nonEncodable() {
    // AND X0, X1, #3 — value 3 (0b11) IS encodable as logical immediate.
    // For non-encodable, we'd need something like 5 (0b101) which isn't a contiguous run.
    // Test with 5: not encodable → fallback (MOVZ + AND, 2+ words).
    MInstr mi{MOpcode::AndRI, {gpr(PhysReg::X0), gpr(PhysReg::X1), imm(5)}};
    CHECK(countWords({mi}) > 1);
}

static void testLogicalImmediateEncoderEdges() {
    CHECK(encodeLogicalImmediate(0x7FFFFFFFFFFFFFFFULL) >= 0);
    CHECK(encodeLogicalImmediate(0x8000000000000001ULL) >= 0);
    CHECK(encodeLogicalImmediate(0xFFFFFFFF00000000ULL) >= 0);
    CHECK(encodeLogicalImmediate(0ULL) < 0);
    CHECK(encodeLogicalImmediate(~0ULL) < 0);
}

static void testLargeImmediateScratchAvoidsSources() {
    {
        const std::vector<uint8_t> bytes =
            encodeInstrBytes({MInstr{MOpcode::CmpRI, {gpr(PhysReg::X16), imm(0x100000000LL)}}});
        const uint32_t mov = readWord(bytes, 4);
        CHECK((mov & 31u) == hwGPR(PhysReg::X9));
    }

    {
        const std::vector<uint8_t> bytes = encodeInstrBytes(
            {MInstr{MOpcode::AndRI, {gpr(PhysReg::X0), gpr(PhysReg::X9), imm(5)}}});
        const uint32_t mov = readWord(bytes, 4);
        CHECK((mov & 31u) == hwGPR(PhysReg::X16));
        const uint32_t andWord = readWord(bytes, 8);
        CHECK(((andWord >> 16) & 31u) == hwGPR(PhysReg::X16));
        CHECK(((andWord >> 5) & 31u) == hwGPR(PhysReg::X9));
    }
}

static void testMovRI_highOnly() {
    // MovRI with 0x0000000100000000 → goes through encodeMovImm64.
    // Smart MOVZ start: chunks = [0, 0, 1, 0], first=2.
    // MOVZ X0, #1, lsl #32 → kMovZ32 | (1 << 5) | 0 = 0xD2C00020. Single instruction.
    MInstr mi{MOpcode::MovRI, {gpr(PhysReg::X0), imm(0x100000000LL)}};
    CHECK(countWords({mi}) == 1);
    CHECK(encodeSingleInstr({mi}) == 0xD2C00020u);
}

static void testMovRI_movnPath() {
    // MovRI with 0xFFFFFFFF00010000 → needs wide (not simple mov).
    // chunks = [0, 1, 0xFFFF, 0xFFFF], nzCount = 3.
    // invChunks = [0xFFFF, 0xFFFE, 0, 0], invNzCount = 2.
    // useMovn = (2 < 3) = true. src = invChunks, first = 0 (0xFFFF).
    // MOVN X0, #0xFFFF → then check chunks: [1]=1 (!=0xFFFF) → MOVK.
    // 2 instructions total (saved from 3 on MOVZ path).
    MInstr mi{MOpcode::MovRI,
              {gpr(PhysReg::X0), imm(static_cast<long long>(0xFFFFFFFF00010000ULL))}};
    CHECK(countWords({mi}) == 2);
}

static void testMovRI_zero() {
    // MovRI with 0 → simple mov path: MOVZ X0, #0 (single instruction).
    MInstr mi{MOpcode::MovRI, {gpr(PhysReg::X0), imm(0)}};
    CHECK(countWords({mi}) == 1);
    CHECK(encodeSingleInstr({mi}) == 0xD2800000u);
}

static void testMovRI_negativeSimple() {
    // Negative values in [-65536, -1] must use MOVN, not MOVZ.
    // MOVN Xd, #(~imm & 0xFFFF) produces the correct sign-extended value.

    // -1: ~(-1)=0, MOVN X0, #0 → 0x92800000
    {
        MInstr mi{MOpcode::MovRI, {gpr(PhysReg::X0), imm(-1)}};
        CHECK(countWords({mi}) == 1);
        uint32_t word = encodeSingleInstr({mi});
        CHECK(word == 0x92800000u); // kMovN | (#0 << 5) | X0
    }
    // -1500 (PLAYER_JUMP): ~(-1500)=0x5DB, MOVN X0, #0x5DB → 0x9280BB60
    {
        MInstr mi{MOpcode::MovRI, {gpr(PhysReg::X0), imm(-1500)}};
        CHECK(countWords({mi}) == 1);
        uint32_t word = encodeSingleInstr({mi});
        CHECK(word == (0x92800000u | (0x5DBu << 5) | 0u));
    }
    // -65536: ~(-65536)=0xFFFF, MOVN X0, #0xFFFF → 0x929FFFE0
    {
        MInstr mi{MOpcode::MovRI, {gpr(PhysReg::X0), imm(-65536)}};
        CHECK(countWords({mi}) == 1);
        uint32_t word = encodeSingleInstr({mi});
        CHECK(word == (0x92800000u | (0xFFFFu << 5) | 0u));
    }
}

static void testFMovRI_fp8() {
    // FMOV D0, #1.0 → single instruction via FP8 encoding.
    // 1.0: sign=0, exp=0, frac=0 → fp8 = (0<<7)|(1<<6)|(3<<4)|0 = 0x70.
    // kFMovDImm | (0x70 << 13) | D0 = 0x1E601000 | 0xE0000 = 0x1E6E1000
    double val = 1.0;
    int64_t bits;
    std::memcpy(&bits, &val, sizeof(bits));
    MInstr mi{MOpcode::FMovRI, {fpr(PhysReg::V0), imm(bits)}};
    CHECK(countWords({mi}) == 1);
    CHECK(encodeSingleInstr({mi}) == 0x1E6E1000);
}

static void testFMovRI_fallback() {
    // FMOV D0, #0.3 → NOT FP8 encodable → MOVZ+MOVK+FMOV (multi-word).
    double val = 0.3;
    int64_t bits;
    std::memcpy(&bits, &val, sizeof(bits));
    MInstr mi{MOpcode::FMovRI, {fpr(PhysReg::V0), imm(bits)}};
    CHECK(countWords({mi}) > 1);
}

static void testAddRI_shift12() {
    MInstr mi{MOpcode::AddRI, {gpr(PhysReg::X0), gpr(PhysReg::X1), imm(4096)}};
    uint32_t word = encodeSingleInstr({mi});
    CHECK(word == encodeAddSubImmShift(kAddRI, hwGPR(PhysReg::X0), hwGPR(PhysReg::X1), 1));
}

static void testStrRegSpImm_largeOffsetFallback() {
    const std::vector<uint8_t> bytes =
        encodeInstrBytes({MInstr{MOpcode::StrRegSpImm, {gpr(PhysReg::X0), imm(40000)}}});
    // BTI + mov scratch,sp + chunked add + str + ret
    CHECK((bytes.size() / 4) > 4);
}

static void testStrRegSpImm_largeOffsetAvoidsX16Source() {
    const std::vector<uint8_t> bytes =
        encodeInstrBytes({MInstr{MOpcode::StrRegSpImm, {gpr(PhysReg::X16), imm(40000)}}});
    CHECK((bytes.size() / 4) > 4);
    const uint32_t store = readWord(bytes, bytes.size() - 8);
    CHECK((store & 31u) == 16u);        // Rt remains the source register X16.
    CHECK(((store >> 5) & 31u) != 16u); // Rn uses a non-conflicting scratch.
}

static void testAddSubRI_acceptsNegativeImmediateByFlippingOpcode() {
    uint32_t word =
        encodeSingleInstr({MInstr{MOpcode::AddRI, {gpr(PhysReg::X0), gpr(PhysReg::X1), imm(-1)}}});
    CHECK(word == encodeAddSubImm(kSubRI, hwGPR(PhysReg::X0), hwGPR(PhysReg::X1), 1));

    word = encodeSingleInstr(
        {MInstr{MOpcode::SubRI, {gpr(PhysReg::X0), gpr(PhysReg::X1), imm(-4096)}}});
    CHECK(word == encodeAddSubImmShift(kAddRI, hwGPR(PhysReg::X0), hwGPR(PhysReg::X1), 1));

    word =
        encodeSingleInstr({MInstr{MOpcode::AddsRI, {gpr(PhysReg::X0), gpr(PhysReg::X1), imm(-7)}}});
    CHECK(word == encodeAddSubImm(kSubsRI, hwGPR(PhysReg::X0), hwGPR(PhysReg::X1), 7));

    word =
        encodeSingleInstr({MInstr{MOpcode::SubsRI, {gpr(PhysReg::X0), gpr(PhysReg::X1), imm(-7)}}});
    CHECK(word == encodeAddSubImm(kAddsRI, hwGPR(PhysReg::X0), hwGPR(PhysReg::X1), 7));
}

static void testWindowsArm64UnwindEntryRecorded() {
    MFunction fn;
    fn.name = "win_unwind";
    fn.isLeaf = false;
    fn.savedGPRs = {PhysReg::X19};
    fn.savedFPRs = {PhysReg::V8};
    fn.localFrameSize = 32;

    MBasicBlock bb;
    bb.name = "entry";
    bb.instrs.push_back(MInstr{MOpcode::Ret, {}});
    fn.blocks.push_back(std::move(bb));

    CodeSection text, rodata;
    A64BinaryEncoder enc;
    enc.encodeFunction(fn, text, rodata, ABIFormat::Windows);

    CHECK(text.winArm64UnwindEntries().size() == 1);
    const auto &entry = text.winArm64UnwindEntries().front();
    CHECK(entry.symbolIndex != 0);
    CHECK(entry.functionLength == text.bytes().size());
    CHECK(!entry.unwindCodes.empty());
    CHECK(entry.unwindCodes.back() == 0xE4);
}

static void testDarwinCompactUnwindFrameEncodingHasNoStrayBits() {
    // A framed function with saved GPR and FPR pairs must emit a clean
    // UNWIND_ARM64_MODE_FRAME encoding. The compact-unwind mode lives in bits
    // [27:24]; saved-pair flags live in bits [8:0] and describe canonical
    // slots our prologue does not use, so no flag bits may be set either.
    MFunction fn;
    fn.name = "darwin_unwind_frame";
    fn.isLeaf = false;
    fn.savedGPRs = {PhysReg::X19, PhysReg::X20, PhysReg::X21};
    fn.savedFPRs = {PhysReg::V8, PhysReg::V9};
    fn.localFrameSize = 48;

    MBasicBlock bb;
    bb.name = "entry";
    bb.instrs.push_back(MInstr{MOpcode::Ret, {}});
    fn.blocks.push_back(std::move(bb));

    CodeSection text, rodata;
    A64BinaryEncoder enc;
    enc.encodeFunction(fn, text, rodata, ABIFormat::Darwin);

    CHECK(text.unwindEntries().size() == 1);
    const auto &entry = text.unwindEntries().front();
    CHECK(entry.encoding == 0x04000000u); // UNWIND_ARM64_MODE_FRAME, no extras
    // The mode nibble must survive masking — the old encoding ORed pair
    // counts into bits 24+ which corrupted the mode for any saved FPR pair.
    CHECK((entry.encoding & 0x0F000000u) == 0x04000000u);
    CHECK((entry.encoding & 0x00FFFFFFu) == 0u);
    CHECK(entry.functionLength == text.bytes().size());
}

static void testDarwinCompactUnwindFramelessEncoding() {
    MFunction fn;
    fn.name = "darwin_unwind_leaf";
    fn.isLeaf = true;
    fn.localFrameSize = 0;

    MBasicBlock bb;
    bb.name = "entry";
    bb.instrs.push_back(MInstr{MOpcode::Ret, {}});
    fn.blocks.push_back(std::move(bb));

    CodeSection text, rodata;
    A64BinaryEncoder enc;
    enc.encodeFunction(fn, text, rodata, ABIFormat::Darwin);

    CHECK(text.unwindEntries().size() == 1);
    const auto &entry = text.unwindEntries().front();
    CHECK(entry.encoding == 0x02000000u); // UNWIND_ARM64_MODE_FRAMELESS, size 0
}

static void testBranchRejectsMissingTarget() {
    MFunction fn;
    fn.name = "missing_target";
    fn.isLeaf = true;

    MBasicBlock bb;
    bb.name = "entry";
    bb.instrs.push_back(MInstr{MOpcode::Br, {label("missing")}});
    fn.blocks.push_back(std::move(bb));

    CodeSection text, rodata;
    A64BinaryEncoder enc;

    bool threw = false;
    try {
        enc.encodeFunction(fn, text, rodata, ABIFormat::Darwin);
    } catch (const std::runtime_error &ex) {
        threw = std::string(ex.what()).find("unresolved internal branch target 'missing'") !=
                std::string::npos;
    }
    CHECK(threw);
}

static bool encodeThrowsContaining(const MFunction &fn, const char *needle) {
    CodeSection text, rodata;
    A64BinaryEncoder enc;
    try {
        enc.encodeFunction(fn, text, rodata, ABIFormat::Darwin);
    } catch (const std::exception &ex) {
        return std::string(ex.what()).find(needle) != std::string::npos;
    }
    return false;
}

static void testFunctionMetadataValidation() {
    MFunction unalignedFrame;
    unalignedFrame.name = "unaligned_frame";
    unalignedFrame.localFrameSize = 8;
    CHECK(encodeThrowsContaining(unalignedFrame, "16-byte aligned"));

    MFunction negativeFrame;
    negativeFrame.name = "negative_frame";
    negativeFrame.localFrameSize = -16;
    CHECK(encodeThrowsContaining(negativeFrame, "negative local frame size"));

    MFunction duplicateGpr;
    duplicateGpr.name = "duplicate_gpr";
    duplicateGpr.savedGPRs = {PhysReg::X19, PhysReg::X19};
    CHECK(encodeThrowsContaining(duplicateGpr, "duplicate saved GPR"));

    MFunction badGpr;
    badGpr.name = "bad_gpr";
    badGpr.savedGPRs = {PhysReg::X18};
    CHECK(encodeThrowsContaining(badGpr, "X19-X28"));

    MFunction duplicateFpr;
    duplicateFpr.name = "duplicate_fpr";
    duplicateFpr.savedFPRs = {PhysReg::V8, PhysReg::V8};
    CHECK(encodeThrowsContaining(duplicateFpr, "duplicate saved FPR"));

    MFunction badFpr;
    badFpr.name = "bad_fpr";
    badFpr.savedFPRs = {PhysReg::V16};
    CHECK(encodeThrowsContaining(badFpr, "V8-V15"));
}

static void testAssemblerCorrectnessValidation() {
    MFunction duplicateSanitizedLabels;
    duplicateSanitizedLabels.name = "duplicate_sanitized_labels";
    MBasicBlock hyphenBlock;
    hyphenBlock.name = "join-block";
    hyphenBlock.instrs.push_back(MInstr{MOpcode::Ret, {}});
    duplicateSanitizedLabels.blocks.push_back(std::move(hyphenBlock));
    MBasicBlock underscoreBlock;
    underscoreBlock.name = "join_block";
    underscoreBlock.instrs.push_back(MInstr{MOpcode::Ret, {}});
    duplicateSanitizedLabels.blocks.push_back(std::move(underscoreBlock));
    CHECK(
        encodeThrowsContaining(duplicateSanitizedLabels, "duplicate/sanitized label 'join_block'"));

    // Pseudo forms that ExpandPseudosPass rewrites must never reach the
    // encoder unexpanded; the encoder rejects them instead of improvising.
    MFunction spBaseLargeOffset;
    spBaseLargeOffset.name = "sp_base_large_offset";
    MBasicBlock spBlock;
    spBlock.name = "entry";
    spBlock.instrs.push_back(
        MInstr{MOpcode::LdrRegBaseImm, {gpr(PhysReg::X0), gpr(PhysReg::SP), imm(40000)}});
    spBlock.instrs.push_back(MInstr{MOpcode::Ret, {}});
    spBaseLargeOffset.blocks.push_back(std::move(spBlock));
    CHECK(encodeThrowsContaining(spBaseLargeOffset, "without ExpandPseudosPass expansion"));

    MFunction pairOverflow;
    pairOverflow.name = "pair_overflow";
    MBasicBlock pairBlock;
    pairBlock.name = "entry";
    pairBlock.instrs.push_back(
        MInstr{MOpcode::LdpRegFpImm,
               {gpr(PhysReg::X0), gpr(PhysReg::X1), imm(std::numeric_limits<long long>::max())}});
    pairBlock.instrs.push_back(MInstr{MOpcode::Ret, {}});
    pairOverflow.blocks.push_back(std::move(pairBlock));
    CHECK(encodeThrowsContaining(pairOverflow, "without ExpandPseudosPass expansion"));

    MFunction wideCompare;
    wideCompare.name = "wide_compare";
    MBasicBlock cmpBlock;
    cmpBlock.name = "entry";
    cmpBlock.instrs.push_back(MInstr{MOpcode::CmpRI, {gpr(PhysReg::X0), imm(0x123456)}});
    cmpBlock.instrs.push_back(MInstr{MOpcode::Ret, {}});
    wideCompare.blocks.push_back(std::move(cmpBlock));
    CHECK(encodeThrowsContaining(wideCompare, "without ExpandPseudosPass expansion"));

    MFunction winHugePrologue;
    winHugePrologue.name = "win_huge_prologue";
    winHugePrologue.isLeaf = false;
    winHugePrologue.localFrameSize = 16777216; // Forces more than 255 bytes of prologue code.
    MBasicBlock winBlock;
    winBlock.name = "entry";
    winBlock.instrs.push_back(MInstr{MOpcode::Ret, {}});
    winHugePrologue.blocks.push_back(std::move(winBlock));

    bool threw = false;
    try {
        CodeSection text, rodata;
        A64BinaryEncoder enc;
        enc.encodeFunction(winHugePrologue, text, rodata, ABIFormat::Windows);
    } catch (const std::runtime_error &ex) {
        const std::string msg = ex.what();
        threw = msg.find("Windows ARM64 prologue") != std::string::npos &&
                msg.find("exceeds 255") != std::string::npos;
    }
    CHECK(threw);
}

static void testAddSubImmediateHelpersRejectWideImmediates() {
    bool threwAdd = false;
    try {
        (void)encodeAddSubImm(kAddRI, hwGPR(PhysReg::X0), hwGPR(PhysReg::X0), 0x1000);
    } catch (const std::out_of_range &ex) {
        threwAdd = std::string(ex.what()).find("12-bit") != std::string::npos;
    }
    CHECK(threwAdd);

    bool threwShift = false;
    try {
        (void)encodeAddSubImmShift(kAddRI, hwGPR(PhysReg::X0), hwGPR(PhysReg::X0), 0x1000);
    } catch (const std::out_of_range &ex) {
        threwShift = std::string(ex.what()).find("12-bit") != std::string::npos;
    }
    CHECK(threwShift);
}

int main() {
    testAddRRR();
    testSubRRR();
    testAndRRR();
    testOrrRRR();
    testEorRRR();
    testMulRRR();
    testUmulhRRR();
    testSDivRRR();
    testMSubRRRR();
    testAddRI();
    testSubRI();
    testMovRR();
    testMovRI_small();
    testLslRI();
    testLsrRI();
    testAsrRI();
    testCmpRR();
    testCmpRI();
    testTstRR();
    testCset();
    testCsel();
    testFAddRRR();
    testFSubRRR();
    testFCmpRR();
    testSCvtF();
    testFCvtZS();
    testRet();
    testBranchForwardBackward();
    testBCondForward();
    testCbzForward();
    testFarConditionalBranchFallbacks();
    testFarForwardConditionalBranchesPatchLongForm();
    testExternalCall();
    testPrologueEpilogue();
    testMainInit();
    testSubSpChunking();
    testCalleeSavedPair();
    testCondCodeMapping();
    testEncoderValidationRejectsBadOperands();
    testBlr();
    testAddsSubsRRR();
    testLdpStpRegFpImm();
    testPairOffsetFallback();
    testNarrowIntegerMemoryEncoding();
    testLargeStoreAvoidsSourceAndBaseScratch();
    testVariableShift();
    testHighRegisters();
    testFMovRR();
    testHighFPR();
    testStrRegSpImm();
    testAdrpAddPageOff();
    testSymbolDefined();
    testAndRI_logicalImm();
    testOrrRI_logicalImm();
    testAndRI_nonEncodable();
    testLogicalImmediateEncoderEdges();
    testLargeImmediateScratchAvoidsSources();
    testMovRI_highOnly();
    testMovRI_movnPath();
    testMovRI_zero();
    testMovRI_negativeSimple();
    testFMovRI_fp8();
    testFMovRI_fallback();
    testAddRI_shift12();
    testStrRegSpImm_largeOffsetFallback();
    testStrRegSpImm_largeOffsetAvoidsX16Source();
    testAddSubRI_acceptsNegativeImmediateByFlippingOpcode();
    testWindowsArm64UnwindEntryRecorded();
    testDarwinCompactUnwindFrameEncodingHasNoStrayBits();
    testDarwinCompactUnwindFramelessEncoding();
    testBranchRejectsMissingTarget();
    testFunctionMetadataValidation();
    testAssemblerCorrectnessValidation();
    testAddSubImmediateHelpersRejectWideImmediates();

    // --- Encoding coverage validation (W7 remediation) ---
    // Verify that every non-pseudo MOpcode can be encoded without crashing.
    // This catches the asymmetry where new opcodes get added to MachineIR.hpp
    // but are forgotten in the encoder's switch statement.
    {
        // Pseudo-opcodes that assert in the encoder (expanded before encoding).
        auto isPseudo = [](MOpcode opc) {
            return opc == MOpcode::AddOvfRRR || opc == MOpcode::SubOvfRRR ||
                   opc == MOpcode::AddOvfRI || opc == MOpcode::SubOvfRI ||
                   opc == MOpcode::MulOvfRRR;
        };

        // Build a minimal valid instruction for each opcode category.
        auto makeTestInstr = [&](MOpcode opc) -> MInstr {
            const auto x0 = gpr(PhysReg::X0);
            const auto x1 = gpr(PhysReg::X1);
            const auto x2 = gpr(PhysReg::X2);
            const auto x3 = gpr(PhysReg::X3);
            const auto d0 = fpr(PhysReg::V0);
            const auto d1 = fpr(PhysReg::V1);
            const auto d2 = fpr(PhysReg::V2);

            switch (opc) {
                // 2-reg GPR
                case MOpcode::MovRR:
                case MOpcode::CmpRR:
                case MOpcode::TstRR:
                    return MInstr{opc, {x0, x1}};

                // MovRI: reg + imm
                case MOpcode::MovRI:
                case MOpcode::AddFpImm:
                    return MInstr{opc, {x0, imm(0)}};

                // 3-reg GPR (dst, lhs, rhs)
                case MOpcode::AddRRR:
                case MOpcode::SubRRR:
                case MOpcode::MulRRR:
                case MOpcode::SmulhRRR:
                case MOpcode::UmulhRRR:
                case MOpcode::SDivRRR:
                case MOpcode::UDivRRR:
                case MOpcode::AndRRR:
                case MOpcode::OrrRRR:
                case MOpcode::EorRRR:
                case MOpcode::AddsRRR:
                case MOpcode::SubsRRR:
                case MOpcode::LslvRRR:
                case MOpcode::LsrvRRR:
                case MOpcode::AsrvRRR:
                    return MInstr{opc, {x0, x1, x2}};

                // 4-reg GPR (dst, mul1, mul2, add/sub)
                case MOpcode::MSubRRRR:
                case MOpcode::MAddRRRR:
                    return MInstr{opc, {x0, x1, x2, x3}};

                // reg-imm arithmetic (3 operands: dst, src, imm)
                case MOpcode::AddRI:
                case MOpcode::SubRI:
                case MOpcode::AddsRI:
                case MOpcode::SubsRI:
                case MOpcode::LslRI:
                case MOpcode::LsrRI:
                case MOpcode::AsrRI:
                    return MInstr{opc, {x0, x1, imm(1)}};

                // CmpRI: 2 operands (reg, imm) — implicit XZR dest
                case MOpcode::CmpRI:
                    return MInstr{opc, {x0, imm(1)}};

                // Logical immediate (use 0xFFFF — encodable as bitmask)
                case MOpcode::AndRI:
                case MOpcode::OrrRI:
                case MOpcode::EorRI:
                    return MInstr{opc, {x0, x1, imm(0xFFFF)}};

                // Cset: dst, cond
                case MOpcode::Cset:
                    return MInstr{opc, {x0, cond("eq")}};

                // Csel: dst, trueReg, falseReg, cond
                case MOpcode::Csel:
                    return MInstr{opc, {x0, x1, x2, cond("eq")}};

                // Branches
                case MOpcode::Br:
                    return MInstr{opc, {label("target")}};
                case MOpcode::BCond:
                    return MInstr{opc, {cond("eq"), label("target")}};
                case MOpcode::Cbz:
                case MOpcode::Cbnz:
                    return MInstr{opc, {x0, label("target")}};
                case MOpcode::Tbz:
                case MOpcode::Tbnz:
                    return MInstr{opc, {x0, label("target"), imm(3)}};
                case MOpcode::JumpTable:
                    return MInstr{opc, {x0, label(".Ljt"), label("target")}};
                case MOpcode::FCsel:
                    return MInstr{
                        opc, {fpr(PhysReg::V0), fpr(PhysReg::V1), fpr(PhysReg::V2), cond("ne")}};
                case MOpcode::Bl:
                    return MInstr{opc, {label("target")}};
                case MOpcode::Blr:
                    return MInstr{opc, {x0}};
                case MOpcode::Ret:
                    return MInstr{opc, {}};

                // SP adjustment
                case MOpcode::SubSpImm:
                case MOpcode::AddSpImm:
                    return MInstr{opc, {imm(16)}};

                // FP-relative load/store
                case MOpcode::LdrRegFpImm:
                case MOpcode::StrRegFpImm:
                case MOpcode::Ldr8RegFpImm:
                case MOpcode::Str8RegFpImm:
                case MOpcode::Ldr16RegFpImm:
                case MOpcode::Str16RegFpImm:
                case MOpcode::Ldr32RegFpImm:
                case MOpcode::Str32RegFpImm:
                case MOpcode::PhiStoreGPR:
                    return MInstr{opc, {x0, imm(0)}};
                case MOpcode::LdrFprFpImm:
                case MOpcode::StrFprFpImm:
                case MOpcode::PhiStoreFPR:
                    return MInstr{opc, {d0, imm(0)}};

                // Base-register load/store
                case MOpcode::LdrRegBaseImm:
                case MOpcode::StrRegBaseImm:
                case MOpcode::Ldr8RegBaseImm:
                case MOpcode::Str8RegBaseImm:
                case MOpcode::Ldr16RegBaseImm:
                case MOpcode::Str16RegBaseImm:
                case MOpcode::Ldr32RegBaseImm:
                case MOpcode::Str32RegBaseImm:
                    return MInstr{opc, {x0, x1, imm(0)}};
                case MOpcode::LdrFprBaseImm:
                case MOpcode::StrFprBaseImm:
                    return MInstr{opc, {d0, x1, imm(0)}};

                // Scaled register-offset load/store: rt, base, index, lsl#k
                case MOpcode::LdrRegBaseRegLsl:
                case MOpcode::StrRegBaseRegLsl:
                    return MInstr{opc, {x0, x1, x2, imm(3)}};
                case MOpcode::Ldr32RegBaseRegLsl:
                case MOpcode::Str32RegBaseRegLsl:
                    return MInstr{opc, {x0, x1, x2, imm(2)}};
                case MOpcode::LdrFprBaseRegLsl:
                case MOpcode::StrFprBaseRegLsl:
                    return MInstr{opc, {d0, x1, x2, imm(3)}};

                // Shifted-operand ALU: dst, a, b, lsl#k
                case MOpcode::AddRRRLsl:
                case MOpcode::SubRRRLsl:
                case MOpcode::AndRRRLsl:
                case MOpcode::OrrRRRLsl:
                case MOpcode::EorRRRLsl:
                    return MInstr{opc, {x0, x1, x2, imm(3)}};

                // SP-relative store (for outgoing args)
                case MOpcode::StrRegSpImm:
                    return MInstr{opc, {x0, imm(0)}};
                case MOpcode::StrFprSpImm:
                    return MInstr{opc, {d0, imm(0)}};

                // Pair load/store (reg1, reg2, offset)
                case MOpcode::LdpRegFpImm:
                case MOpcode::StpRegFpImm:
                    return MInstr{opc, {x0, x1, imm(0)}};
                case MOpcode::LdpFprFpImm:
                case MOpcode::StpFprFpImm:
                    return MInstr{opc, {d0, d1, imm(0)}};

                // Address materialisation
                case MOpcode::AdrPage:
                    return MInstr{opc, {x0, label("sym")}};
                case MOpcode::AddPageOff:
                    return MInstr{opc, {x0, x1, label("sym")}};

                // FP 2-reg
                case MOpcode::FMovRR:
                case MOpcode::FCmpRR:
                    return MInstr{opc, {d0, d1}};
                case MOpcode::FMovGR:
                    return MInstr{opc, {d0, x0}};
                case MOpcode::FMovRI: {
                    // 1.0 is FP8-encodable; other constants are expanded
                    // before emission and would be rejected here.
                    const double one = 1.0;
                    long long bits = 0;
                    std::memcpy(&bits, &one, sizeof(bits));
                    return MInstr{opc, {d0, imm(bits)}};
                }
                case MOpcode::FRintN:
                    return MInstr{opc, {d0, d1}};

                // FP 3-reg
                case MOpcode::FAddRRR:
                case MOpcode::FSubRRR:
                case MOpcode::FMulRRR:
                case MOpcode::FDivRRR:
                    return MInstr{opc, {d0, d1, d2}};

                // Conversions
                case MOpcode::SCvtF:
                case MOpcode::UCvtF:
                    return MInstr{opc, {d0, x0}};
                case MOpcode::FCvtZS:
                case MOpcode::FCvtZU:
                    return MInstr{opc, {x0, d0}};

                // Pseudo-opcodes (should never reach encoder)
                default:
                    return MInstr{opc, {}};
            }
        };

        // Iterate all opcodes from MovRR (0) to JumpTable (last).
        constexpr int kFirstOpcode = static_cast<int>(MOpcode::MovRR);
        constexpr int kLastOpcode = static_cast<int>(MOpcode::JumpTable);
        int encodedCount = 0;
        int pseudoCount = 0;

        for (int i = kFirstOpcode; i <= kLastOpcode; ++i) {
            const auto opc = static_cast<MOpcode>(i);
            if (isPseudo(opc)) {
                ++pseudoCount;
                continue;
            }

            MInstr mi = makeTestInstr(opc);

            // Encode in a leaf function — should not crash.
            MFunction fn;
            fn.name = "coverage_test";
            fn.isLeaf = true;
            MBasicBlock bb;
            bb.name = "entry";
            bb.instrs.push_back(std::move(mi));
            bb.instrs.push_back(MInstr{MOpcode::Ret, {}});
            fn.blocks.push_back(std::move(bb));

            // Add a target block for branch instructions so labels resolve.
            MBasicBlock target;
            target.name = "target";
            target.instrs.push_back(MInstr{MOpcode::Ret, {}});
            fn.blocks.push_back(std::move(target));

            // Add a "sym" block for address materialisation opcodes.
            MBasicBlock sym;
            sym.name = "sym";
            sym.instrs.push_back(MInstr{MOpcode::Ret, {}});
            fn.blocks.push_back(std::move(sym));

            CodeSection text, rodata;
            A64BinaryEncoder enc;
            enc.encodeFunction(fn, text, rodata, ABIFormat::Darwin);

            // Must have produced at least 8 bytes (test instr + ret).
            if (text.bytes().size() < 8) {
                std::cerr << "FAIL: opcode " << opcodeName(opc) << " produced only "
                          << text.bytes().size() << " bytes\n";
                ++gFail;
            } else {
                ++encodedCount;
            }
        }

        // Verify we covered the expected counts.
        CHECK(pseudoCount == 5);    // 5 pseudo-opcodes
        CHECK(encodedCount == 102); // 107 total - 5 pseudo = 102 real opcodes

        if (encodedCount == 102 && pseudoCount == 5)
            std::cout << "  Encoding coverage: " << encodedCount << "/102 opcodes OK, "
                      << pseudoCount << " pseudo-opcodes skipped.\n";
    }

    {
        MInstr ret{MOpcode::Ret, {}};
        constexpr size_t kLargeOffset = size_t{1} << 30;
        CHECK(A64BinaryEncoderTestAccess::measureWithKnownTarget(ret, kLargeOffset, kLargeOffset) ==
              4);
    }

    if (gFail == 0)
        std::cout << "All A64 binary encoder tests passed.\n";
    else
        std::cerr << gFail << " test(s) FAILED.\n";
    return gFail ? EXIT_FAILURE : EXIT_SUCCESS;
}
