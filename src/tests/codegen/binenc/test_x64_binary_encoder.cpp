//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/codegen/binenc/test_x64_binary_encoder.cpp
// Purpose: Unit tests for X64BinaryEncoder — verifies that MIR instructions
//          produce the correct x86_64 machine code bytes and relocations.
// Key invariants:
//   - All byte sequences match the Intel SDM encoding specification
//   - REX prefix is emitted only when needed (W/R/X/B)
//   - RSP/R12 base registers always produce SIB bytes
//   - RBP/R13 with disp=0 uses mod=01 + disp8=0
//   - External calls generate Branch32 relocations with addend=-4
//   - Internal branches are resolved via patching
// Ownership/Lifetime: Standalone test binary.
// Links: codegen/x86_64/binenc/X64BinaryEncoder.hpp
//
//===----------------------------------------------------------------------===//

#include "codegen/common/objfile/CodeSection.hpp"
#include "codegen/x86_64/MachineIR.hpp"
#include "codegen/x86_64/binenc/X64BinaryEncoder.hpp"
#include "codegen/x86_64/binenc/X64Encoding.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace zanna::codegen::x64;
using namespace zanna::codegen::x64::binenc;
using namespace zanna::codegen::objfile;

static int gFail = 0;

static void check(bool cond, const char *msg, int line) {
    if (!cond) {
        std::cerr << "FAIL line " << line << ": " << msg << "\n";
        ++gFail;
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

// Helper to create a physical register operand.
static Operand gpr(PhysReg r) {
    return makePhysRegOperand(RegClass::GPR, static_cast<uint16_t>(r));
}

static Operand xmm(PhysReg r) {
    return makePhysRegOperand(RegClass::XMM, static_cast<uint16_t>(r));
}

static Operand imm(int64_t val) {
    return makeImmOperand(val);
}

static Operand mem(PhysReg base, int32_t disp) {
    return makeMemOperand(makePhysReg(RegClass::GPR, static_cast<uint16_t>(base)), disp);
}

static Operand memIdx(PhysReg base, PhysReg index, uint8_t scale, int32_t disp) {
    return makeMemOperand(makePhysReg(RegClass::GPR, static_cast<uint16_t>(base)),
                          makePhysReg(RegClass::GPR, static_cast<uint16_t>(index)),
                          scale,
                          disp);
}

static Operand label(const std::string &name) {
    return makeLabelOperand(name);
}

static Operand ripLabel(const std::string &name) {
    return makeRipLabelOperand(name);
}

// Helper: encode a single instruction and return the bytes.
static std::vector<uint8_t> encodeOne(MOpcode op, std::vector<Operand> operands) {
    MFunction fn;
    fn.name = "test";
    MBasicBlock bb;
    bb.label = ".Ltest";
    bb.append(MInstr::make(op, std::move(operands)));
    fn.addBlock(std::move(bb));

    X64BinaryEncoder enc;
    CodeSection text, rodata;
    enc.encodeFunction(fn, text, rodata, false);
    return text.bytes();
}

// Helper: check bytes match expected hex sequence.
static bool bytesMatch(const std::vector<uint8_t> &actual,
                       const std::vector<uint8_t> &expected,
                       size_t offset = 0) {
    if (offset + expected.size() > actual.size())
        return false;
    return std::memcmp(actual.data() + offset, expected.data(), expected.size()) == 0;
}

int main() {
    // ================================================================
    // 0. Encoder validation
    // ================================================================
    {
        bool threw = false;
        try {
            (void)x86CC(99);
        } catch (const std::out_of_range &) {
            threw = true;
        }
        CHECK(threw);
    }

    {
        bool threw = false;
        try {
            (void)encodeOne(MOpcode::MOVrr, {gpr(PhysReg::RAX)});
        } catch (const std::runtime_error &ex) {
            threw = std::string(ex.what()).find("requires exactly 2") != std::string::npos;
        }
        CHECK(threw);
    }

    {
        bool threw = false;
        try {
            (void)encodeOne(MOpcode::MOVrr, {gpr(PhysReg::RAX), xmm(PhysReg::XMM0)});
        } catch (const std::runtime_error &ex) {
            threw = std::string(ex.what()).find("expected GPR") != std::string::npos;
        }
        CHECK(threw);
    }

    {
        bool threw = false;
        try {
            (void)encodeOne(
                MOpcode::MOVrr,
                {makePhysRegOperand(RegClass::GPR, static_cast<uint16_t>(PhysReg::XMM0)),
                 gpr(PhysReg::RAX)});
        } catch (const std::runtime_error &ex) {
            threw = std::string(ex.what()).find("register class") != std::string::npos;
        }
        CHECK(threw);
    }

    {
        bool threw = false;
        try {
            (void)encodeOne(MOpcode::SHLri, {gpr(PhysReg::RAX), imm(64)});
        } catch (const std::runtime_error &ex) {
            threw = std::string(ex.what()).find("shift count") != std::string::npos;
        }
        CHECK(threw);
    }

    {
        bool threw = false;
        try {
            (void)encodeOne(MOpcode::SHLrc, {gpr(PhysReg::RAX), gpr(PhysReg::RDX)});
        } catch (const std::runtime_error &ex) {
            threw = std::string(ex.what()).find("RCX") != std::string::npos;
        }
        CHECK(threw);
    }

    {
        MFunction fn;
        fn.name = "win64_unwind_ok";
        MBasicBlock bb;
        bb.label = ".Lentry";
        bb.append(MInstr::make(MOpcode::PUSH, {gpr(PhysReg::RBP)}));
        bb.append(MInstr::make(MOpcode::ADDri, {gpr(PhysReg::RSP), imm(-8)}));
        fn.addBlock(std::move(bb));

        FrameInfo frame;
        frame.prologueEmitted = true;
        frame.win64UnwindOps.push_back({Win64UnwindOpKind::PushNonVol, PhysReg::RBP, 0});
        frame.win64UnwindOps.push_back({Win64UnwindOpKind::AllocStack, PhysReg::RSP, 8});

        X64BinaryEncoder enc;
        CodeSection text, rodata;
        enc.encodeFunction(fn, text, rodata, false, &frame, true);
        CHECK(text.win64UnwindEntries().size() == 1);
        CHECK(text.win64UnwindEntries()[0].codes.size() == 2);
        CHECK(text.win64UnwindEntries()[0].prologueSize == 5);
    }

    {
        MFunction fn;
        fn.name = "win64_unwind_partial";
        MBasicBlock bb;
        bb.label = ".Lentry";
        bb.append(MInstr::make(MOpcode::ADDri, {gpr(PhysReg::RAX), imm(-8)}));
        fn.addBlock(std::move(bb));

        FrameInfo frame;
        frame.prologueEmitted = true;
        frame.win64UnwindOps.push_back({Win64UnwindOpKind::AllocStack, PhysReg::RSP, 8});

        bool threw = false;
        try {
            X64BinaryEncoder enc;
            CodeSection text, rodata;
            enc.encodeFunction(fn, text, rodata, false, &frame, true);
        } catch (const std::runtime_error &ex) {
            threw = std::string(ex.what()).find("Win64 unwind plan") != std::string::npos;
        }
        CHECK(threw);
    }

    {
        MFunction fn;
        fn.name = "win64_unwind_too_large";
        MBasicBlock bb;
        bb.label = ".Lentry";
        for (int i = 0; i < 260; ++i)
            bb.append(MInstr::make(MOpcode::RET, {}));
        bb.append(MInstr::make(MOpcode::ADDri, {gpr(PhysReg::RSP), imm(-8)}));
        fn.addBlock(std::move(bb));

        FrameInfo frame;
        frame.prologueEmitted = true;
        frame.win64UnwindOps.push_back({Win64UnwindOpKind::AllocStack, PhysReg::RSP, 8});

        bool threw = false;
        try {
            X64BinaryEncoder enc;
            CodeSection text, rodata;
            enc.encodeFunction(fn, text, rodata, false, &frame, true);
        } catch (const std::runtime_error &ex) {
            threw = std::string(ex.what()).find("exceeds 255") != std::string::npos;
        }
        CHECK(threw);
    }

    // ================================================================
    // 1. Nullary instructions
    // ================================================================

    // --- RET: C3 ---
    {
        auto bytes = encodeOne(MOpcode::RET, {});
        CHECK(bytes.size() == 1);
        CHECK(bytes[0] == 0xC3);
    }

    // --- CQO: 48 99 ---
    {
        auto bytes = encodeOne(MOpcode::CQO, {});
        CHECK(bytes.size() == 2);
        CHECK(bytes[0] == 0x48);
        CHECK(bytes[1] == 0x99);
    }

    // --- UD2: 0F 0B ---
    {
        auto bytes = encodeOne(MOpcode::UD2, {});
        CHECK(bytes.size() == 2);
        CHECK(bytes[0] == 0x0F);
        CHECK(bytes[1] == 0x0B);
    }

    // ================================================================
    // 2. MOVrr: movq %src, %dst (REX.W + 89 + ModR/M)
    // ================================================================

    // movq %rax, %rcx -> 48 89 C1
    // reg=RAX(hw 0), r/m=RCX(hw 1), reg=src -> ModR/M = 11 000 001 = C1
    {
        auto bytes = encodeOne(MOpcode::MOVrr, {gpr(PhysReg::RCX), gpr(PhysReg::RAX)});
        CHECK(bytes.size() == 3);
        CHECK(bytesMatch(bytes, {0x48, 0x89, 0xC1}));
    }

    // movq %r8, %r9 -> 4D 89 C1
    // REX: W=1, R=1(R8), B=1(R9) -> 0100 1101 = 4D
    // reg=R8(hw 0), r/m=R9(hw 1) -> ModR/M = 11 000 001 = C1
    {
        auto bytes = encodeOne(MOpcode::MOVrr, {gpr(PhysReg::R9), gpr(PhysReg::R8)});
        CHECK(bytes.size() == 3);
        CHECK(bytesMatch(bytes, {0x4D, 0x89, 0xC1}));
    }

    // ================================================================
    // 3. ADDrr: addq %src, %dst
    // ================================================================

    // addq %rdx, %rax -> 48 01 D0
    // opcode=01, reg=RDX(hw 2), r/m=RAX(hw 0) -> ModR/M = 11 010 000 = D0
    {
        auto bytes = encodeOne(MOpcode::ADDrr, {gpr(PhysReg::RAX), gpr(PhysReg::RDX)});
        CHECK(bytes.size() == 3);
        CHECK(bytesMatch(bytes, {0x48, 0x01, 0xD0}));
    }

    // ================================================================
    // 4. SUBrr, XORrr
    // ================================================================

    // subq %rsi, %rdi -> 48 29 F7
    // reg=RSI(hw 6), r/m=RDI(hw 7)
    {
        auto bytes = encodeOne(MOpcode::SUBrr, {gpr(PhysReg::RDI), gpr(PhysReg::RSI)});
        CHECK(bytes.size() == 3);
        CHECK(bytesMatch(bytes, {0x48, 0x29, 0xF7}));
    }

    // xorq %rax, %rax -> 48 31 C0
    {
        auto bytes = encodeOne(MOpcode::XORrr, {gpr(PhysReg::RAX), gpr(PhysReg::RAX)});
        CHECK(bytes.size() == 3);
        CHECK(bytesMatch(bytes, {0x48, 0x31, 0xC0}));
    }

    // ================================================================
    // 5. XORrr32 (no REX.W): xorl %eax, %eax -> 31 C0
    // ================================================================
    {
        auto bytes = encodeOne(MOpcode::XORrr32, {gpr(PhysReg::RAX), gpr(PhysReg::RAX)});
        CHECK(bytes.size() == 2);
        CHECK(bytesMatch(bytes, {0x31, 0xC0}));
    }

    // XORrr32 with R8: xorl %r8d, %r8d -> 45 31 C0
    {
        auto bytes = encodeOne(MOpcode::XORrr32, {gpr(PhysReg::R8), gpr(PhysReg::R8)});
        CHECK(bytes.size() == 3);
        CHECK(bytesMatch(bytes, {0x45, 0x31, 0xC0}));
    }

    // --- 32-bit ALU family (no REX.W; flags set at 32-bit width) ---

    // addl %edx, %eax -> 01 D0
    {
        auto bytes = encodeOne(MOpcode::ADDrr32, {gpr(PhysReg::RAX), gpr(PhysReg::RDX)});
        CHECK(bytes.size() == 2);
        CHECK(bytesMatch(bytes, {0x01, 0xD0}));
    }

    // addl %edx, %r8d -> REX.B only: 41 01 D0
    {
        auto bytes = encodeOne(MOpcode::ADDrr32, {gpr(PhysReg::R8), gpr(PhysReg::RDX)});
        CHECK(bytes.size() == 3);
        CHECK(bytesMatch(bytes, {0x41, 0x01, 0xD0}));
    }

    // subl %edx, %eax -> 29 D0
    {
        auto bytes = encodeOne(MOpcode::SUBrr32, {gpr(PhysReg::RAX), gpr(PhysReg::RDX)});
        CHECK(bytes.size() == 2);
        CHECK(bytesMatch(bytes, {0x29, 0xD0}));
    }

    // cmpl %edx, %eax -> 39 D0
    {
        auto bytes = encodeOne(MOpcode::CMPrr32, {gpr(PhysReg::RAX), gpr(PhysReg::RDX)});
        CHECK(bytes.size() == 2);
        CHECK(bytesMatch(bytes, {0x39, 0xD0}));
    }

    // imull %edx, %eax -> 0F AF C2 (reg=dst direction)
    {
        auto bytes = encodeOne(MOpcode::IMULrr32, {gpr(PhysReg::RAX), gpr(PhysReg::RDX)});
        CHECK(bytes.size() == 3);
        CHECK(bytesMatch(bytes, {0x0F, 0xAF, 0xC2}));
    }

    // movslq %edx, %rax -> 48 63 C2 (keeps REX.W: 64-bit destination)
    {
        auto bytes = encodeOne(MOpcode::MOVSXD, {gpr(PhysReg::RAX), gpr(PhysReg::RDX)});
        CHECK(bytes.size() == 3);
        CHECK(bytesMatch(bytes, {0x48, 0x63, 0xC2}));
    }

    // addl $7, %eax -> short form, no REX: 83 C0 07
    {
        auto bytes = encodeOne(MOpcode::ADDri32, {gpr(PhysReg::RAX), imm(7)});
        CHECK(bytes.size() == 3);
        CHECK(bytesMatch(bytes, {0x83, 0xC0, 0x07}));
    }

    // addl $1000, %eax -> long form: 81 C0 E8 03 00 00
    {
        auto bytes = encodeOne(MOpcode::ADDri32, {gpr(PhysReg::RAX), imm(1000)});
        CHECK(bytes.size() == 6);
        CHECK(bytesMatch(bytes, {0x81, 0xC0, 0xE8, 0x03, 0x00, 0x00}));
    }

    // --- 16-bit ALU family (0x66 prefix; flags set at 16-bit width) ---

    // addw %dx, %ax -> 66 01 D0
    {
        auto bytes = encodeOne(MOpcode::ADDrr16, {gpr(PhysReg::RAX), gpr(PhysReg::RDX)});
        CHECK(bytes.size() == 3);
        CHECK(bytesMatch(bytes, {0x66, 0x01, 0xD0}));
    }

    // addw %dx, %r8w -> 66 prefix before REX.B: 66 41 01 D0
    {
        auto bytes = encodeOne(MOpcode::ADDrr16, {gpr(PhysReg::R8), gpr(PhysReg::RDX)});
        CHECK(bytes.size() == 4);
        CHECK(bytesMatch(bytes, {0x66, 0x41, 0x01, 0xD0}));
    }

    // subw %dx, %ax -> 66 29 D0
    {
        auto bytes = encodeOne(MOpcode::SUBrr16, {gpr(PhysReg::RAX), gpr(PhysReg::RDX)});
        CHECK(bytes.size() == 3);
        CHECK(bytesMatch(bytes, {0x66, 0x29, 0xD0}));
    }

    // imulw %dx, %ax -> 66 0F AF C2 (reg=dst direction)
    {
        auto bytes = encodeOne(MOpcode::IMULrr16, {gpr(PhysReg::RAX), gpr(PhysReg::RDX)});
        CHECK(bytes.size() == 4);
        CHECK(bytesMatch(bytes, {0x66, 0x0F, 0xAF, 0xC2}));
    }

    // movswq %dx, %rax -> 48 0F BF C2 (REX.W, no 0x66: source width is opcode-implied)
    {
        auto bytes = encodeOne(MOpcode::MOVSXrr16, {gpr(PhysReg::RAX), gpr(PhysReg::RDX)});
        CHECK(bytes.size() == 4);
        CHECK(bytesMatch(bytes, {0x48, 0x0F, 0xBF, 0xC2}));
    }

    // addw $7, %ax -> short form keeps imm8: 66 83 C0 07
    {
        auto bytes = encodeOne(MOpcode::ADDri16, {gpr(PhysReg::RAX), imm(7)});
        CHECK(bytes.size() == 4);
        CHECK(bytesMatch(bytes, {0x66, 0x83, 0xC0, 0x07}));
    }

    // addw $1000, %ax -> long form takes imm16 under 0x66: 66 81 C0 E8 03
    {
        auto bytes = encodeOne(MOpcode::ADDri16, {gpr(PhysReg::RAX), imm(1000)});
        CHECK(bytes.size() == 5);
        CHECK(bytesMatch(bytes, {0x66, 0x81, 0xC0, 0xE8, 0x03}));
    }

    // --- Memory-operand ALU (reg <- reg op [mem]) ---

    // addq (%rax), %rcx -> 48 03 08 (reg=RCX in ModRM.reg, base=RAX)
    {
        auto bytes = encodeOne(MOpcode::ADDrm, {gpr(PhysReg::RCX), mem(PhysReg::RAX, 0)});
        CHECK(bytes.size() == 3);
        CHECK(bytesMatch(bytes, {0x48, 0x03, 0x08}));
    }

    // cmpq 8(%rax), %rcx -> 48 3B 48 08 (disp8)
    {
        auto bytes = encodeOne(MOpcode::CMPrm, {gpr(PhysReg::RCX), mem(PhysReg::RAX, 8)});
        CHECK(bytes.size() == 4);
        CHECK(bytesMatch(bytes, {0x48, 0x3B, 0x48, 0x08}));
    }

    // imulq (%rax), %rcx -> two-byte opcode: 48 0F AF 08
    {
        auto bytes = encodeOne(MOpcode::IMULrm, {gpr(PhysReg::RCX), mem(PhysReg::RAX, 0)});
        CHECK(bytes.size() == 4);
        CHECK(bytesMatch(bytes, {0x48, 0x0F, 0xAF, 0x08}));
    }

    // --- JUMPTABLE: lea/movslq/add/jmp tail + inline anchor-relative table ---
    {
        MFunction fn;
        fn.name = "jt";
        MBasicBlock dispatch;
        dispatch.label = ".Ldispatch";
        dispatch.append(MInstr::make(
            MOpcode::JUMPTABLE,
            {gpr(PhysReg::RCX), Operand{OpLabel{".Ljt_0"}}, Operand{OpLabel{".Ltarget"}}}));
        MBasicBlock target;
        target.label = ".Ltarget";
        target.append(MInstr::make(MOpcode::RET, {}));
        fn.addBlock(std::move(dispatch));
        fn.addBlock(std::move(target));

        X64BinaryEncoder enc;
        CodeSection text, rodata;
        enc.encodeFunction(fn, text, rodata, false);
        const auto &bytes = text.bytes();
        CHECK(bytes.size() == 22);
        // leaq 10(%rip), %r10 — displacement equals the fixed tail size.
        CHECK(bytesMatch(bytes, {0x4C, 0x8D, 0x15, 0x0A, 0x00, 0x00, 0x00}, 0));
        // movslq (%r10,%rcx,4), %r11
        CHECK(bytesMatch(bytes, {0x4D, 0x63, 0x1C, 0x8A}, 7));
        // addq %r10, %r11
        CHECK(bytesMatch(bytes, {0x4D, 0x01, 0xD3}, 11));
        // jmp *%r11
        CHECK(bytesMatch(bytes, {0x41, 0xFF, 0xE3}, 14));
        // Table entry: offset(.Ltarget)=21 minus tableStart=17 -> 4.
        CHECK(bytesMatch(bytes, {0x04, 0x00, 0x00, 0x00}, 17));
        // ret at .Ltarget
        CHECK(bytes[21] == 0xC3);
    }

    // ================================================================
    // 6. IMULrr: reversed direction (reg=dst, r/m=src)
    // ================================================================

    // imulq %rcx, %rax -> 48 0F AF C1
    // reg=RAX(hw 0, dst), r/m=RCX(hw 1, src) -> ModR/M = 11 000 001 = C1
    {
        auto bytes = encodeOne(MOpcode::IMULrr, {gpr(PhysReg::RAX), gpr(PhysReg::RCX)});
        CHECK(bytes.size() == 4);
        CHECK(bytesMatch(bytes, {0x48, 0x0F, 0xAF, 0xC1}));
    }

    // ================================================================
    // 7. Reg-Imm ALU
    // ================================================================

    // addq $1, %rax -> 48 83 C0 01 (short form, imm8)
    {
        auto bytes = encodeOne(MOpcode::ADDri, {gpr(PhysReg::RAX), imm(1)});
        CHECK(bytes.size() == 4);
        CHECK(bytesMatch(bytes, {0x48, 0x83, 0xC0, 0x01}));
    }

    // addq $256, %rax -> 48 81 C0 00 01 00 00 (long form, imm32)
    {
        auto bytes = encodeOne(MOpcode::ADDri, {gpr(PhysReg::RAX), imm(256)});
        CHECK(bytes.size() == 7);
        CHECK(bytesMatch(bytes, {0x48, 0x81, 0xC0, 0x00, 0x01, 0x00, 0x00}));
    }

    // addq $-8, %rsp -> 49 83 C4 F8 (R12 gets... no, RSP)
    // RSP hw=4, REX.B=0 -> REX = 48
    // /0 ext, reg=RSP -> ModR/M = 11 000 100 = C4
    {
        auto bytes = encodeOne(MOpcode::ADDri, {gpr(PhysReg::RSP), imm(-8)});
        CHECK(bytes.size() == 4);
        CHECK(bytesMatch(bytes, {0x48, 0x83, 0xC4, 0xF8}));
    }

    // andq $0xFF, %rdi -> 48 81 FF FF 00 00 00 (0xFF > 127, use long form)
    // Wait: 0xFF = 255, which doesn't fit in int8_t [-128,127]. Long form.
    // /4 ext for AND -> ModR/M = 11 100 111 = E7
    {
        auto bytes = encodeOne(MOpcode::ANDri, {gpr(PhysReg::RDI), imm(0xFF)});
        CHECK(bytes.size() == 7);
        CHECK(bytes[0] == 0x48);
        CHECK(bytes[1] == 0x81);
        CHECK(bytes[2] == 0xE7); // ModR/M: 11 100 111
        CHECK(bytes[3] == 0xFF);
    }

    // cmpq $0, %rax -> 48 83 F8 00 (short form)
    // /7 ext for CMP -> ModR/M = 11 111 000 = F8
    {
        auto bytes = encodeOne(MOpcode::CMPri, {gpr(PhysReg::RAX), imm(0)});
        CHECK(bytes.size() == 4);
        CHECK(bytesMatch(bytes, {0x48, 0x83, 0xF8, 0x00}));
    }

    // ================================================================
    // 8. Shift instructions
    // ================================================================

    // shlq $3, %rax -> 48 C1 E0 03
    // /4 ext -> ModR/M = 11 100 000 = E0
    {
        auto bytes = encodeOne(MOpcode::SHLri, {gpr(PhysReg::RAX), imm(3)});
        CHECK(bytes.size() == 4);
        CHECK(bytesMatch(bytes, {0x48, 0xC1, 0xE0, 0x03}));
    }

    // shrq $1, %rdx -> 48 C1 EA 01
    // /5 ext -> ModR/M = 11 101 010 = EA
    {
        auto bytes = encodeOne(MOpcode::SHRri, {gpr(PhysReg::RDX), imm(1)});
        CHECK(bytes.size() == 4);
        CHECK(bytesMatch(bytes, {0x48, 0xC1, 0xEA, 0x01}));
    }

    // sarq %cl, %rax -> 48 D3 F8
    // /7 ext -> ModR/M = 11 111 000 = F8
    {
        auto bytes = encodeOne(MOpcode::SARrc, {gpr(PhysReg::RAX), gpr(PhysReg::RCX)});
        CHECK(bytes.size() == 3);
        CHECK(bytesMatch(bytes, {0x48, 0xD3, 0xF8}));
    }

    // ================================================================
    // 9. Division
    // ================================================================

    // idivq %rcx -> 48 F7 F9
    // /7 ext -> ModR/M = 11 111 001 = F9
    {
        auto bytes = encodeOne(MOpcode::IDIVrm, {gpr(PhysReg::RCX)});
        CHECK(bytes.size() == 3);
        CHECK(bytesMatch(bytes, {0x48, 0xF7, 0xF9}));
    }

    // divq %r10 -> 49 F7 F2
    // R10 hw=2, rex=1 -> REX.W=1, REX.B=1 = 0x49
    // /6 ext -> ModR/M = 11 110 010 = F2
    {
        auto bytes = encodeOne(MOpcode::DIVrm, {gpr(PhysReg::R10)});
        CHECK(bytes.size() == 3);
        CHECK(bytesMatch(bytes, {0x49, 0xF7, 0xF2}));
    }

    // ================================================================
    // 10. MOVri (64-bit immediate)
    // ================================================================

    // movabs $0x123456789ABCDEF0, %rax -> 48 B8 F0 DE BC 9A 78 56 34 12
    {
        auto bytes = encodeOne(MOpcode::MOVri, {gpr(PhysReg::RAX), imm(0x123456789ABCDEF0LL)});
        CHECK(bytes.size() == 10);
        CHECK(bytes[0] == 0x48); // REX.W
        CHECK(bytes[1] == 0xB8); // B8 + RAX(0)
        CHECK(bytes[2] == 0xF0); // LE byte 0
        CHECK(bytes[9] == 0x12); // LE byte 7
    }

    // mov $42, %r11 -> 41 BB 2A 00 00 00  (6 bytes: REX.B + B8+3 + imm32)
    // R11 hw=3, rex=1 -> REX.B=1 = 0x41 (no REX.W for 32-bit zero-extending form)
    {
        auto bytes = encodeOne(MOpcode::MOVri, {gpr(PhysReg::R11), imm(42)});
        CHECK(bytes.size() == 6);
        CHECK(bytes[0] == 0x41);
        CHECK(bytes[1] == 0xBB);
        CHECK(bytes[2] == 42);
    }

    // ================================================================
    // 11. Memory operations (MOVrm store, MOVmr load)
    // ================================================================

    // movq %rax, 8(%rbp) -> 48 89 45 08
    // opcode=89, reg=RAX(hw 0), mod=01(disp8), r/m=RBP(hw 5)
    // ModR/M = 01 000 101 = 45
    {
        auto bytes = encodeOne(MOpcode::MOVrm, {mem(PhysReg::RBP, 8), gpr(PhysReg::RAX)});
        CHECK(bytes.size() == 4);
        CHECK(bytesMatch(bytes, {0x48, 0x89, 0x45, 0x08}));
    }

    // movq (%rax), %rcx -> 48 8B 08
    // opcode=8B, reg=RCX(hw 1), mod=00, r/m=RAX(hw 0)
    // ModR/M = 00 001 000 = 08
    {
        auto bytes = encodeOne(MOpcode::MOVmr, {gpr(PhysReg::RCX), mem(PhysReg::RAX, 0)});
        CHECK(bytes.size() == 3);
        CHECK(bytesMatch(bytes, {0x48, 0x8B, 0x08}));
    }

    // ================================================================
    // 12. RSP as base (must emit SIB)
    // ================================================================

    // movq (%rsp), %rax -> 48 8B 04 24
    // RSP (hw 4) needs SIB. SIB = 00 100 100 = 24 (no index, base=RSP)
    {
        auto bytes = encodeOne(MOpcode::MOVmr, {gpr(PhysReg::RAX), mem(PhysReg::RSP, 0)});
        CHECK(bytes.size() == 4);
        CHECK(bytesMatch(bytes, {0x48, 0x8B, 0x04, 0x24}));
    }

    // movq 16(%rsp), %rax -> 48 8B 44 24 10
    // mod=01(disp8), SIB needed
    {
        auto bytes = encodeOne(MOpcode::MOVmr, {gpr(PhysReg::RAX), mem(PhysReg::RSP, 16)});
        CHECK(bytes.size() == 5);
        CHECK(bytesMatch(bytes, {0x48, 0x8B, 0x44, 0x24, 0x10}));
    }

    // ================================================================
    // 13. RBP with disp=0 (must use mod=01 + disp8=0)
    // ================================================================

    // movq (%rbp), %rax -> 48 8B 45 00
    // RBP with mod=00 would mean RIP-relative! Must use mod=01 + 00
    {
        auto bytes = encodeOne(MOpcode::MOVmr, {gpr(PhysReg::RAX), mem(PhysReg::RBP, 0)});
        CHECK(bytes.size() == 4);
        CHECK(bytesMatch(bytes, {0x48, 0x8B, 0x45, 0x00}));
    }

    // ================================================================
    // 14. R12 as base (needs SIB like RSP)
    // ================================================================

    // movq (%r12), %rax -> 49 8B 04 24
    // R12 hw=4, rex=1 -> REX.W=1, REX.B=1 = 0x49
    // SIB = 00 100 100 = 24
    {
        auto bytes = encodeOne(MOpcode::MOVmr, {gpr(PhysReg::RAX), mem(PhysReg::R12, 0)});
        CHECK(bytes.size() == 4);
        CHECK(bytesMatch(bytes, {0x49, 0x8B, 0x04, 0x24}));
    }

    // ================================================================
    // 15. R13 with disp=0 (like RBP, needs mod=01 + disp8=0)
    // ================================================================

    // movq (%r13), %rax -> 49 8B 45 00
    // R13 hw=5, rex=1 -> REX.W=1, REX.B=1 = 0x49
    {
        auto bytes = encodeOne(MOpcode::MOVmr, {gpr(PhysReg::RAX), mem(PhysReg::R13, 0)});
        CHECK(bytes.size() == 4);
        CHECK(bytesMatch(bytes, {0x49, 0x8B, 0x45, 0x00}));
    }

    // ================================================================
    // 16. Scaled index addressing
    // ================================================================

    // movq (%rax,%rcx,8), %rdx -> 48 8B 14 C8
    // reg=RDX(hw 2), SIB: scale=8(11), index=RCX(hw 1), base=RAX(hw 0)
    // ModR/M = 00 010 100 = 14
    // SIB = 11 001 000 = C8
    {
        auto bytes = encodeOne(MOpcode::MOVmr,
                               {gpr(PhysReg::RDX), memIdx(PhysReg::RAX, PhysReg::RCX, 8, 0)});
        CHECK(bytes.size() == 4);
        CHECK(bytesMatch(bytes, {0x48, 0x8B, 0x14, 0xC8}));
    }

    // ================================================================
    // 16b. Narrow memory transfers (i1/i16/i32 stores and loads)
    // ================================================================

    // movb %al, 8(%rbp) -> 88 45 08 (no REX.W: only the low byte is written)
    {
        auto bytes = encodeOne(MOpcode::MOVrm8, {mem(PhysReg::RBP, 8), gpr(PhysReg::RAX)});
        CHECK(bytes.size() == 3);
        CHECK(bytesMatch(bytes, {0x88, 0x45, 0x08}));
    }

    // movb %sil, (%rax) -> 40 88 30 (a bare REX selects SIL instead of DH)
    {
        auto bytes = encodeOne(MOpcode::MOVrm8, {mem(PhysReg::RAX, 0), gpr(PhysReg::RSI)});
        CHECK(bytes.size() == 3);
        CHECK(bytesMatch(bytes, {0x40, 0x88, 0x30}));
    }

    // movb %r9b, 4(%rbx) -> 44 88 4B 04 (REX.R)
    {
        auto bytes = encodeOne(MOpcode::MOVrm8, {mem(PhysReg::RBX, 4), gpr(PhysReg::R9)});
        CHECK(bytes.size() == 4);
        CHECK(bytesMatch(bytes, {0x44, 0x88, 0x4B, 0x04}));
    }

    // movw %cx, 2(%rdx) -> 66 89 4A 02
    {
        auto bytes = encodeOne(MOpcode::MOVrm16, {mem(PhysReg::RDX, 2), gpr(PhysReg::RCX)});
        CHECK(bytes.size() == 4);
        CHECK(bytesMatch(bytes, {0x66, 0x89, 0x4A, 0x02}));
    }

    // movw %r10w, (%r11) -> 66 45 89 13 (0x66 precedes REX)
    {
        auto bytes = encodeOne(MOpcode::MOVrm16, {mem(PhysReg::R11, 0), gpr(PhysReg::R10)});
        CHECK(bytes.size() == 4);
        CHECK(bytesMatch(bytes, {0x66, 0x45, 0x89, 0x13}));
    }

    // movl %edx, (%rsp) -> 89 14 24
    {
        auto bytes = encodeOne(MOpcode::MOVrm32, {mem(PhysReg::RSP, 0), gpr(PhysReg::RDX)});
        CHECK(bytes.size() == 3);
        CHECK(bytesMatch(bytes, {0x89, 0x14, 0x24}));
    }

    // movl %r8d, 16(%rax,%rcx,4) -> 44 89 44 88 10
    {
        auto bytes = encodeOne(MOpcode::MOVrm32,
                               {memIdx(PhysReg::RAX, PhysReg::RCX, 4, 16), gpr(PhysReg::R8)});
        CHECK(bytes.size() == 5);
        CHECK(bytesMatch(bytes, {0x44, 0x89, 0x44, 0x88, 0x10}));
    }

    // movzbq 7(%rdi), %rax -> 48 0F B6 47 07
    {
        auto bytes = encodeOne(MOpcode::MOVZXmr8, {gpr(PhysReg::RAX), mem(PhysReg::RDI, 7)});
        CHECK(bytes.size() == 5);
        CHECK(bytesMatch(bytes, {0x48, 0x0F, 0xB6, 0x47, 0x07}));
    }

    // movzbq (%r13), %r14 -> 4D 0F B6 75 00
    {
        auto bytes = encodeOne(MOpcode::MOVZXmr8, {gpr(PhysReg::R14), mem(PhysReg::R13, 0)});
        CHECK(bytes.size() == 5);
        CHECK(bytesMatch(bytes, {0x4D, 0x0F, 0xB6, 0x75, 0x00}));
    }

    // movswq 6(%rsi), %rcx -> 48 0F BF 4E 06
    {
        auto bytes = encodeOne(MOpcode::MOVSXmr16, {gpr(PhysReg::RCX), mem(PhysReg::RSI, 6)});
        CHECK(bytes.size() == 5);
        CHECK(bytesMatch(bytes, {0x48, 0x0F, 0xBF, 0x4E, 0x06}));
    }

    // movslq 4(%rbx), %rdx -> 48 63 53 04
    {
        auto bytes = encodeOne(MOpcode::MOVSXDmr, {gpr(PhysReg::RDX), mem(PhysReg::RBX, 4)});
        CHECK(bytes.size() == 4);
        CHECK(bytesMatch(bytes, {0x48, 0x63, 0x53, 0x04}));
    }

    // ================================================================
    // 17. LEA with memory operand
    // ================================================================

    // leaq 16(%rbp), %rdi -> 48 8D 7D 10
    // reg=RDI(hw 7), mod=01, r/m=RBP(hw 5) -> ModR/M = 01 111 101 = 7D
    {
        auto bytes = encodeOne(MOpcode::LEA, {gpr(PhysReg::RDI), mem(PhysReg::RBP, 16)});
        CHECK(bytes.size() == 4);
        CHECK(bytesMatch(bytes, {0x48, 0x8D, 0x7D, 0x10}));
    }

    // ================================================================
    // 18. LEA with RIP-relative label (generates relocation)
    // ================================================================
    {
        MFunction fn;
        fn.name = "test";
        MBasicBlock bb;
        bb.label = ".Ltest";
        bb.append(MInstr::make(MOpcode::LEA, {gpr(PhysReg::RDI), ripLabel(".LC_str_0")}));
        fn.addBlock(std::move(bb));

        X64BinaryEncoder enc;
        CodeSection text, rodata;
        rodata.defineSymbol(".LC_str_0", SymbolBinding::Local, SymbolSection::Rodata);
        rodata.emit8(0);
        enc.encodeFunction(fn, text, rodata, false);

        // Should emit: 48 8D 3D 00 00 00 00 (7 bytes)
        CHECK(text.bytes().size() == 7);
        CHECK(text.bytes()[0] == 0x48); // REX.W
        CHECK(text.bytes()[1] == 0x8D); // LEA
        CHECK(text.bytes()[2] == 0x3D); // ModR/M: 00 111 101 (RDI, RIP-relative)

        // Should have a PCRel32 relocation with addend=-4.
        CHECK(text.relocations().size() == 1);
        CHECK(text.relocations()[0].kind == RelocKind::PCRel32);
        CHECK(text.relocations()[0].addend == -4);
        CHECK(text.relocations()[0].offset == 3); // disp32 starts at byte 3
        CHECK(text.relocations()[0].targetSection == SymbolSection::Rodata);
    }

    // ================================================================
    // 19. SETcc
    // ================================================================

    // sete %al -> 0F 94 C0
    // cc=0 -> x86 CC=4 -> 0F 94, ModR/M = 11 000 000 = C0
    {
        auto bytes = encodeOne(MOpcode::SETcc, {imm(0), gpr(PhysReg::RAX)});
        CHECK(bytes.size() == 3);
        CHECK(bytesMatch(bytes, {0x0F, 0x94, 0xC0}));
    }

    // setne %r8b -> 41 0F 95 C0
    // R8 hw=0, rex=1 -> need REX for REX.B
    {
        auto bytes = encodeOne(MOpcode::SETcc, {imm(1), gpr(PhysReg::R8)});
        CHECK(bytes.size() == 4);
        CHECK(bytesMatch(bytes, {0x41, 0x0F, 0x95, 0xC0}));
    }

    // Hand-built MIR may present SETcc as {dst, cc}; encode by operand kind.
    {
        auto bytes = encodeOne(MOpcode::SETcc, {gpr(PhysReg::RAX), imm(0)});
        CHECK(bytes.size() == 3);
        CHECK(bytesMatch(bytes, {0x0F, 0x94, 0xC0}));
    }

    // setne -1(%rbp) -> 0F 95 45 FF
    {
        auto bytes = encodeOne(MOpcode::SETcc, {imm(1), mem(PhysReg::RBP, -1)});
        CHECK(bytes.size() == 4);
        CHECK(bytesMatch(bytes, {0x0F, 0x95, 0x45, 0xFF}));
    }

    // sete (%r12) -> 41 0F 94 04 24
    {
        auto bytes = encodeOne(MOpcode::SETcc, {mem(PhysReg::R12, 0), imm(0)});
        CHECK(bytes.size() == 5);
        CHECK(bytesMatch(bytes, {0x41, 0x0F, 0x94, 0x04, 0x24}));
    }

    {
        bool threw = false;
        try {
            (void)encodeOne(MOpcode::SETcc, {gpr(PhysReg::RAX), label(".Lnot_a_condition")});
        } catch (const std::runtime_error &ex) {
            threw = std::string(ex.what()).find("condition code") != std::string::npos;
        }
        CHECK(threw);
    }

    // ================================================================
    // 20. MOVZXrr8 (movzbq) and MOVZXrr32 (movl)
    // ================================================================

    // movzbq %al, %rax -> 48 0F B6 C0
    {
        auto bytes = encodeOne(MOpcode::MOVZXrr8, {gpr(PhysReg::RAX), gpr(PhysReg::RAX)});
        CHECK(bytes.size() == 4);
        CHECK(bytesMatch(bytes, {0x48, 0x0F, 0xB6, 0xC0}));
    }

    // movzbq %spl, %rax -> 48 0F B6 C4
    // A REX prefix is required so r/m=100 names SPL instead of AH.
    {
        auto bytes = encodeOne(MOpcode::MOVZXrr8, {gpr(PhysReg::RAX), gpr(PhysReg::RSP)});
        CHECK(bytes.size() == 4);
        CHECK(bytesMatch(bytes, {0x48, 0x0F, 0xB6, 0xC4}));
    }

    // movl %ecx, %eax -> 89 C8
    {
        auto bytes = encodeOne(MOpcode::MOVZXrr32, {gpr(PhysReg::RAX), gpr(PhysReg::RCX)});
        CHECK(bytes.size() == 2);
        CHECK(bytesMatch(bytes, {0x89, 0xC8}));
    }

    // ================================================================
    // 21. TESTrr
    // ================================================================

    // testq %rdi, %rdi -> 48 85 FF
    // reg=RDI(hw 7), r/m=RDI(hw 7) -> ModR/M = 11 111 111 = FF
    {
        auto bytes = encodeOne(MOpcode::TESTrr, {gpr(PhysReg::RDI), gpr(PhysReg::RDI)});
        CHECK(bytes.size() == 3);
        CHECK(bytesMatch(bytes, {0x48, 0x85, 0xFF}));
    }

    // ================================================================
    // 22. SSE scalar double
    // ================================================================

    // addsd %xmm1, %xmm0 -> F2 0F 58 C1
    // reg=XMM0(dst, hw 0), r/m=XMM1(src, hw 1) -> ModR/M = 11 000 001 = C1
    {
        auto bytes = encodeOne(MOpcode::FADD, {xmm(PhysReg::XMM0), xmm(PhysReg::XMM1)});
        CHECK(bytes.size() == 4);
        CHECK(bytesMatch(bytes, {0xF2, 0x0F, 0x58, 0xC1}));
    }

    // subsd %xmm0, %xmm1 -> F2 0F 5C C8
    // reg=XMM1(dst, hw 1), r/m=XMM0(src, hw 0) -> ModR/M = 11 001 000 = C8
    {
        auto bytes = encodeOne(MOpcode::FSUB, {xmm(PhysReg::XMM1), xmm(PhysReg::XMM0)});
        CHECK(bytes.size() == 4);
        CHECK(bytesMatch(bytes, {0xF2, 0x0F, 0x5C, 0xC8}));
    }

    // ucomisd %xmm0, %xmm1 -> 66 0F 2E C8
    // prefix=66, reg=XMM1(hw 1), r/m=XMM0(hw 0)
    {
        auto bytes = encodeOne(MOpcode::UCOMIS, {xmm(PhysReg::XMM1), xmm(PhysReg::XMM0)});
        CHECK(bytes.size() == 4);
        CHECK(bytesMatch(bytes, {0x66, 0x0F, 0x2E, 0xC8}));
    }

    // cvtsi2sdq %rax, %xmm0 -> F2 48 0F 2A C0
    // prefix=F2, REX.W, reg=XMM0(hw 0), r/m=RAX(hw 0)
    {
        auto bytes = encodeOne(MOpcode::CVTSI2SD, {xmm(PhysReg::XMM0), gpr(PhysReg::RAX)});
        CHECK(bytes.size() == 5);
        CHECK(bytesMatch(bytes, {0xF2, 0x48, 0x0F, 0x2A, 0xC0}));
    }

    // movsd %xmm1, %xmm0 -> F2 0F 10 C1
    // load direction: reg=XMM0(dst), r/m=XMM1(src)
    {
        auto bytes = encodeOne(MOpcode::MOVSDrr, {xmm(PhysReg::XMM0), xmm(PhysReg::XMM1)});
        CHECK(bytes.size() == 4);
        CHECK(bytesMatch(bytes, {0xF2, 0x0F, 0x10, 0xC1}));
    }

    // ================================================================
    // 23. SSE with high XMM registers (XMM8+)
    // ================================================================

    // addsd %xmm8, %xmm0 -> F2 41 0F 58 C0
    // XMM8 hw=0, rex=1 -> need REX.B
    {
        auto bytes = encodeOne(MOpcode::FADD, {xmm(PhysReg::XMM0), xmm(PhysReg::XMM8)});
        CHECK(bytes.size() == 5);
        CHECK(bytesMatch(bytes, {0xF2, 0x41, 0x0F, 0x58, 0xC0}));
    }

    // ================================================================
    // 24. MOVQrx (movq GPR -> XMM)
    // ================================================================

    // movq %rax, %xmm0 -> 66 48 0F 6E C0
    // prefix=66, REX.W, reg=XMM0(hw 0), r/m=RAX(hw 0)
    {
        auto bytes = encodeOne(MOpcode::MOVQrx, {xmm(PhysReg::XMM0), gpr(PhysReg::RAX)});
        CHECK(bytes.size() == 5);
        CHECK(bytesMatch(bytes, {0x66, 0x48, 0x0F, 0x6E, 0xC0}));
    }

    // ================================================================
    // 25. PX_COPY must be lowered before binary encoding
    // ================================================================
    {
        bool threw = false;
        try {
            (void)encodeOne(MOpcode::PX_COPY, {});
        } catch (const std::runtime_error &ex) {
            threw = std::string(ex.what()).find("parallel-copy pseudo") != std::string::npos;
        }
        CHECK(threw);
    }

    // ================================================================
    // 26. Internal branch resolution with short forward/backward relaxation
    // ================================================================
    {
        MFunction fn;
        fn.name = "test";

        // Block 0: jmp to block 2 (forward)
        MBasicBlock bb0;
        bb0.label = ".Lblock0";
        bb0.append(MInstr::make(MOpcode::JMP, {label(".Lblock2")}));
        fn.addBlock(std::move(bb0));

        // Block 1: nop (filler)
        MBasicBlock bb1;
        bb1.label = ".Lblock1";
        bb1.append(MInstr::make(MOpcode::RET, {}));
        fn.addBlock(std::move(bb1));

        // Block 2: jmp back to block 1 (backward)
        MBasicBlock bb2;
        bb2.label = ".Lblock2";
        bb2.append(MInstr::make(MOpcode::JMP, {label(".Lblock1")}));
        fn.addBlock(std::move(bb2));

        X64BinaryEncoder enc;
        CodeSection text, rodata;
        enc.encodeFunction(fn, text, rodata, false);

        // Block 0: EB xx  (2 bytes, offset 0) — forward JMP (short)
        // Block 1: C3     (1 byte, offset 2)
        // Block 2: EB xx  (2 bytes, offset 3) — backward JMP (short)
        CHECK(text.bytes().size() == 5);
        CHECK(text.bytes()[0] == 0xEB); // forward short JMP
        CHECK(static_cast<int8_t>(text.bytes()[1]) == 1);
        CHECK(text.bytes()[2] == 0xC3); // RET
        CHECK(text.bytes()[3] == 0xEB); // backward short JMP
        CHECK(static_cast<int8_t>(text.bytes()[4]) == -3);
    }

    // Short-branch relaxation must account for the branch's real offset, not
    // treat every candidate as if it started at byte 0.
    {
        MFunction fn;
        fn.name = "offset_sensitive_short_jmp";

        MBasicBlock entry;
        entry.label = ".Lentry";
        for (int i = 0; i < 70; ++i) {
            entry.append(MInstr::make(MOpcode::PUSH, {gpr(PhysReg::RAX)}));
            entry.append(MInstr::make(MOpcode::POP, {gpr(PhysReg::RAX)}));
        }
        entry.append(MInstr::make(MOpcode::JMP, {label(".Ltarget")}));
        entry.append(MInstr::make(MOpcode::RET, {}));
        fn.addBlock(std::move(entry));

        MBasicBlock target;
        target.label = ".Ltarget";
        target.append(MInstr::make(MOpcode::RET, {}));
        fn.addBlock(std::move(target));

        X64BinaryEncoder enc;
        CodeSection text, rodata;
        enc.encodeFunction(fn, text, rodata, false);

        CHECK(text.bytes().size() == 144);
        CHECK(text.bytes()[140] == 0xEB);
        CHECK(static_cast<int8_t>(text.bytes()[141]) == 1);
        CHECK(text.bytes()[142] == 0xC3);
        CHECK(text.bytes()[143] == 0xC3);
    }

    // ================================================================
    // 27. JCC (conditional branch)
    // ================================================================
    {
        MFunction fn;
        fn.name = "test";
        MBasicBlock bb;
        bb.label = ".Lentry";
        // JCC with cc=1 (NE) -> x86CC=5 -> 0F 85
        bb.append(MInstr::make(MOpcode::JCC, {imm(1), label(".Ltarget")}));
        bb.append(MInstr::make(MOpcode::RET, {}));
        fn.addBlock(std::move(bb));

        MBasicBlock bb2;
        bb2.label = ".Ltarget";
        bb2.append(MInstr::make(MOpcode::RET, {}));
        fn.addBlock(std::move(bb2));

        X64BinaryEncoder enc;
        CodeSection text, rodata;
        enc.encodeFunction(fn, text, rodata, false);

        // JCC short = 75 xx (2 bytes) + RET (1 byte) + RET (1 byte)
        CHECK(text.bytes().size() == 4);
        CHECK(text.bytes()[0] == 0x75); // JNE short
        CHECK(static_cast<int8_t>(text.bytes()[1]) == 1);
    }

    // Hand-built MIR may present JCC as {label, cc}; encode by operand kind.
    {
        MFunction fn;
        fn.name = "test_label_first_jcc";
        MBasicBlock bb;
        bb.label = ".Lentry";
        bb.append(MInstr::make(MOpcode::JCC, {label(".Ltarget"), imm(1)}));
        bb.append(MInstr::make(MOpcode::RET, {}));
        fn.addBlock(std::move(bb));

        MBasicBlock bb2;
        bb2.label = ".Ltarget";
        bb2.append(MInstr::make(MOpcode::RET, {}));
        fn.addBlock(std::move(bb2));

        X64BinaryEncoder enc;
        CodeSection text, rodata;
        enc.encodeFunction(fn, text, rodata, false);

        CHECK(text.bytes().size() == 4);
        CHECK(text.bytes()[0] == 0x75);
        CHECK(static_cast<int8_t>(text.bytes()[1]) == 1);
    }

    {
        bool threw = false;
        try {
            (void)encodeOne(MOpcode::JCC, {label(".Ltarget"), gpr(PhysReg::RAX)});
        } catch (const std::runtime_error &ex) {
            threw = std::string(ex.what()).find("condition code") != std::string::npos;
        }
        CHECK(threw);
    }

    {
        bool threw = false;
        try {
            (void)encodeOne(MOpcode::JCC, {imm(1), gpr(PhysReg::RAX)});
        } catch (const std::runtime_error &ex) {
            threw = std::string(ex.what()).find("label target") != std::string::npos;
        }
        CHECK(threw);
    }

    // ================================================================
    // 28. External CALL (generates relocation)
    // ================================================================
    {
        MFunction fn;
        fn.name = "test";
        MBasicBlock bb;
        bb.label = ".Lentry";
        bb.append(MInstr::make(MOpcode::CALL, {label("rt_print_i64")}));
        fn.addBlock(std::move(bb));

        X64BinaryEncoder enc;
        CodeSection text, rodata;
        enc.encodeFunction(fn, text, rodata, false);

        // E8 00 00 00 00 (5 bytes)
        CHECK(text.bytes().size() == 5);
        CHECK(text.bytes()[0] == 0xE8);

        // Should have Branch32 relocation with addend=-4.
        CHECK(text.relocations().size() == 1);
        CHECK(text.relocations()[0].kind == RelocKind::Branch32);
        CHECK(text.relocations()[0].addend == -4);
        CHECK(text.relocations()[0].offset == 1);
    }

    // ================================================================
    // 29. Indirect CALL via register
    // ================================================================

    // callq *%rax -> FF D0
    // /2 ext -> ModR/M = 11 010 000 = D0
    {
        auto bytes = encodeOne(MOpcode::CALL, {gpr(PhysReg::RAX)});
        CHECK(bytes.size() == 2);
        CHECK(bytesMatch(bytes, {0xFF, 0xD0}));
    }

    // callq *%r11 -> 41 FF D3
    // R11 hw=3, rex=1 -> REX.B=1 = 41
    // /2 ext -> ModR/M = 11 010 011 = D3
    {
        auto bytes = encodeOne(MOpcode::CALL, {gpr(PhysReg::R11)});
        CHECK(bytes.size() == 3);
        CHECK(bytesMatch(bytes, {0x41, 0xFF, 0xD3}));
    }

    // ================================================================
    // 30. Indirect JMP via register
    // ================================================================

    // jmpq *%rax -> FF E0
    // /4 ext -> ModR/M = 11 100 000 = E0
    {
        auto bytes = encodeOne(MOpcode::JMP, {gpr(PhysReg::RAX)});
        CHECK(bytes.size() == 2);
        CHECK(bytesMatch(bytes, {0xFF, 0xE0}));
    }

    // ================================================================
    // 31. Indirect CALL via memory
    // ================================================================

    // callq *8(%rax) -> FF 50 08
    // /2 ext, mod=01, r/m=RAX(hw 0) -> ModR/M = 01 010 000 = 50
    {
        auto bytes = encodeOne(MOpcode::CALL, {mem(PhysReg::RAX, 8)});
        CHECK(bytes.size() == 3);
        CHECK(bytesMatch(bytes, {0xFF, 0x50, 0x08}));
    }

    // ================================================================
    // 32. Indirect CALL/JMP via RIP-relative memory
    // ================================================================
    {
        MFunction fn;
        fn.name = "rip_indirect_call";
        MBasicBlock bb;
        bb.label = ".Lentry";
        bb.append(MInstr::make(MOpcode::CALL, {ripLabel("fnptr")}));
        fn.addBlock(std::move(bb));

        X64BinaryEncoder enc;
        CodeSection text, rodata;
        enc.encodeFunction(fn, text, rodata, false);

        CHECK(text.bytes().size() == 6);
        CHECK(bytesMatch(text.bytes(), {0xFF, 0x15, 0x00, 0x00, 0x00, 0x00}));
        CHECK(text.relocations().size() == 1);
        CHECK(text.relocations()[0].kind == RelocKind::PCRel32);
        CHECK(text.relocations()[0].addend == -4);
        CHECK(text.relocations()[0].offset == 2);
    }

    {
        MFunction fn;
        fn.name = "rip_indirect_jmp";
        MBasicBlock bb;
        bb.label = ".Lentry";
        bb.append(MInstr::make(MOpcode::JMP, {ripLabel("jmpptr")}));
        fn.addBlock(std::move(bb));

        X64BinaryEncoder enc;
        CodeSection text, rodata;
        enc.encodeFunction(fn, text, rodata, false);

        CHECK(text.bytes().size() == 6);
        CHECK(bytesMatch(text.bytes(), {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00}));
        CHECK(text.relocations().size() == 1);
        CHECK(text.relocations()[0].kind == RelocKind::PCRel32);
        CHECK(text.relocations()[0].addend == -4);
        CHECK(text.relocations()[0].offset == 2);
    }

    // ================================================================
    // 33. Darwin symbols stay canonical until Mach-O writing
    // ================================================================
    {
        MFunction fn;
        fn.name = "main";
        MBasicBlock bb;
        bb.label = ".Lentry";
        bb.append(MInstr::make(MOpcode::CALL, {label("rt_init")}));
        bb.append(MInstr::make(MOpcode::RET, {}));
        fn.addBlock(std::move(bb));

        X64BinaryEncoder enc;
        CodeSection text, rodata;
        enc.encodeFunction(fn, text, rodata, /*isDarwin=*/true);

        // Function symbol should remain unmangled in CodeSection.
        bool foundMain = false;
        for (uint32_t i = 0; i < text.symbols().count(); ++i) {
            if (text.symbols().at(i).name == "main") {
                foundMain = true;
                break;
            }
        }
        CHECK(foundMain);

        // External call should remain unmangled in CodeSection.
        bool foundRtInit = false;
        for (uint32_t i = 0; i < text.symbols().count(); ++i) {
            if (text.symbols().at(i).name == "rt_init") {
                foundRtInit = true;
                break;
            }
        }
        CHECK(foundRtInit);
    }

    // ================================================================
    // 33. SSE memory operations
    // ================================================================

    // movsd %xmm0, 8(%rbp) -> F2 0F 11 45 08
    // store direction: reg=XMM0(src, hw 0), mod=01, r/m=RBP(hw 5)
    {
        auto bytes = encodeOne(MOpcode::MOVSDrm, {mem(PhysReg::RBP, 8), xmm(PhysReg::XMM0)});
        CHECK(bytes.size() == 5);
        CHECK(bytesMatch(bytes, {0xF2, 0x0F, 0x11, 0x45, 0x08}));
    }

    // movsd 8(%rbp), %xmm0 -> F2 0F 10 45 08
    // load direction: reg=XMM0(dst, hw 0)
    {
        auto bytes = encodeOne(MOpcode::MOVSDmr, {xmm(PhysReg::XMM0), mem(PhysReg::RBP, 8)});
        CHECK(bytes.size() == 5);
        CHECK(bytesMatch(bytes, {0xF2, 0x0F, 0x10, 0x45, 0x08}));
    }

    // movups %xmm0, 16(%rsp) -> 0F 11 44 24 10
    // No prefix, SIB for RSP
    {
        auto bytes = encodeOne(MOpcode::MOVUPSrm, {mem(PhysReg::RSP, 16), xmm(PhysReg::XMM0)});
        CHECK(bytes.size() == 5);
        CHECK(bytesMatch(bytes, {0x0F, 0x11, 0x44, 0x24, 0x10}));
    }

    // movups 16(%rsp), %xmm0 -> 0F 10 44 24 10
    {
        auto bytes = encodeOne(MOpcode::MOVUPSmr, {xmm(PhysReg::XMM0), mem(PhysReg::RSP, 16)});
        CHECK(bytes.size() == 5);
        CHECK(bytesMatch(bytes, {0x0F, 0x10, 0x44, 0x24, 0x10}));
    }

    // ================================================================
    // 34. CMOVNErr
    // ================================================================

    // cmovneq %rcx, %rax -> 48 0F 45 C1 (REX.W for 64-bit operand size)
    {
        auto bytes = encodeOne(MOpcode::CMOVNErr, {gpr(PhysReg::RAX), gpr(PhysReg::RCX)});
        CHECK(bytes.size() == 4);
        CHECK(bytesMatch(bytes, {0x48, 0x0F, 0x45, 0xC1}));
    }

    // ================================================================
    // 35. Large displacement (disp32)
    // ================================================================

    // movq 256(%rbp), %rax -> 48 8B 85 00 01 00 00
    // mod=10(disp32), r/m=RBP(hw 5) -> ModR/M = 10 000 101 = 85
    {
        auto bytes = encodeOne(MOpcode::MOVmr, {gpr(PhysReg::RAX), mem(PhysReg::RBP, 256)});
        CHECK(bytes.size() == 7);
        CHECK(bytesMatch(bytes, {0x48, 0x8B, 0x85, 0x00, 0x01, 0x00, 0x00}));
    }

    // ================================================================
    // 36. MOVri short form — 32-bit zero-extending (5 bytes)
    // ================================================================

    // movl $0, %eax → B8 00 00 00 00 (5 bytes, zero-extends to 64-bit)
    {
        auto bytes = encodeOne(MOpcode::MOVri, {gpr(PhysReg::RAX), imm(0)});
        CHECK(bytes.size() == 5);
        CHECK(bytesMatch(bytes, {0xB8, 0x00, 0x00, 0x00, 0x00}));
    }

    // movl $127, %eax → B8 7F 00 00 00 (5 bytes)
    {
        auto bytes = encodeOne(MOpcode::MOVri, {gpr(PhysReg::RAX), imm(127)});
        CHECK(bytes.size() == 5);
        CHECK(bytesMatch(bytes, {0xB8, 0x7F, 0x00, 0x00, 0x00}));
    }

    // movl $INT32_MAX, %ecx → B9 FF FF FF 7F (5 bytes)
    {
        auto bytes = encodeOne(MOpcode::MOVri, {gpr(PhysReg::RCX), imm(0x7FFFFFFF)});
        CHECK(bytes.size() == 5);
        CHECK(bytesMatch(bytes, {0xB9, 0xFF, 0xFF, 0xFF, 0x7F}));
    }

    // ================================================================
    // 37. MOVri short form — high register (6 bytes, needs REX.B)
    // ================================================================

    // movl $42, %r8d → 41 B8 2A 00 00 00 (6 bytes)
    {
        auto bytes = encodeOne(MOpcode::MOVri, {gpr(PhysReg::R8), imm(42)});
        CHECK(bytes.size() == 6);
        CHECK(bytesMatch(bytes, {0x41, 0xB8, 0x2A, 0x00, 0x00, 0x00}));
    }

    // ================================================================
    // 38. MOVri sign-extending form — negative values (7 bytes)
    // ================================================================

    // movq $-1, %rax → 48 C7 C0 FF FF FF FF (7 bytes)
    {
        auto bytes = encodeOne(MOpcode::MOVri, {gpr(PhysReg::RAX), imm(-1)});
        CHECK(bytes.size() == 7);
        CHECK(bytesMatch(bytes, {0x48, 0xC7, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF}));
    }

    // movq $-128, %rcx → 48 C7 C1 80 FF FF FF (7 bytes)
    {
        auto bytes = encodeOne(MOpcode::MOVri, {gpr(PhysReg::RCX), imm(-128)});
        CHECK(bytes.size() == 7);
        CHECK(bytesMatch(bytes, {0x48, 0xC7, 0xC1, 0x80, 0xFF, 0xFF, 0xFF}));
    }

    // movq $INT32_MIN, %rax → 48 C7 C0 00 00 00 80 (7 bytes)
    {
        auto bytes = encodeOne(MOpcode::MOVri, {gpr(PhysReg::RAX), imm(INT32_MIN)});
        CHECK(bytes.size() == 7);
        CHECK(bytesMatch(bytes, {0x48, 0xC7, 0xC0, 0x00, 0x00, 0x00, 0x80}));
    }

    // ================================================================
    // 39. MOVri full 64-bit form (10 bytes)
    // ================================================================

    // movabsq $0x100000000, %rax → 48 B8 00 00 00 00 01 00 00 00
    {
        auto bytes = encodeOne(MOpcode::MOVri, {gpr(PhysReg::RAX), imm(0x100000000LL)});
        CHECK(bytes.size() == 10);
        CHECK(bytesMatch(bytes, {0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00}));
    }

    // movabsq $INT64_MAX, %rax → 48 B8 FF FF FF FF FF FF FF 7F
    {
        auto bytes = encodeOne(MOpcode::MOVri, {gpr(PhysReg::RAX), imm(INT64_MAX)});
        CHECK(bytes.size() == 10);
        CHECK(bytesMatch(bytes, {0x48, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F}));
    }

    // ================================================================
    // 40. Validation failures report cleanly
    // ================================================================

    // Invalid SIB scale is rejected by MIR construction.
    {
        bool threw = false;
        try {
            (void)makeMemOperand(makePhysReg(RegClass::GPR, static_cast<uint16_t>(PhysReg::RAX)),
                                 makePhysReg(RegClass::GPR, static_cast<uint16_t>(PhysReg::RCX)),
                                 3,
                                 0);
        } catch (const std::invalid_argument &ex) {
            threw = std::string(ex.what()).find("scale") != std::string::npos;
        }
        CHECK(threw);
    }

    // RSP is not a legal SIB index register and must be rejected during encoding.
    {
        MFunction fn;
        fn.name = "bad_mem_index";
        MBasicBlock bb;
        bb.label = ".Lbad_mem_index";

        OpMem badMem{};
        badMem.base = makePhysReg(RegClass::GPR, static_cast<uint16_t>(PhysReg::RAX));
        badMem.index = makePhysReg(RegClass::GPR, static_cast<uint16_t>(PhysReg::RSP));
        badMem.scale = 1;
        badMem.disp = 0;
        badMem.hasIndex = true;

        bb.append(MInstr::make(MOpcode::MOVmr, {gpr(PhysReg::RAX), Operand{badMem}}));
        fn.addBlock(std::move(bb));

        X64BinaryEncoder enc;
        CodeSection text, rodata;
        bool threw = false;
        try {
            enc.encodeFunction(fn, text, rodata, false);
        } catch (const std::runtime_error &ex) {
            threw =
                std::string(ex.what()).find("%rsp as a SIB index register") != std::string::npos;
        }
        CHECK(threw);
    }

    // Reg-immediate ALU encoding is limited to sign-extended imm32.
    {
        bool threw = false;
        try {
            (void)encodeOne(MOpcode::ADDri,
                            {gpr(PhysReg::RAX), imm(static_cast<int64_t>(1) << 40)});
        } catch (const std::runtime_error &ex) {
            threw = std::string(ex.what()).find("32-bit encoding range") != std::string::npos;
        }
        CHECK(threw);
    }

    // LABEL pseudos require exactly one label operand.
    {
        MFunction fn;
        fn.name = "bad_label_arity";
        MBasicBlock bb;
        bb.label = ".Lbad_label_arity";
        bb.append(MInstr::make(MOpcode::LABEL, {}));
        fn.addBlock(std::move(bb));

        bool threw = false;
        try {
            X64BinaryEncoder enc;
            CodeSection text, rodata;
            enc.encodeFunction(fn, text, rodata, false);
        } catch (const std::runtime_error &ex) {
            threw = std::string(ex.what()).find("requires exactly one label operand") !=
                    std::string::npos;
        }
        CHECK(threw);
    }

    // Duplicate block/in-block labels are rejected during the sizing pass.
    {
        MFunction fn;
        fn.name = "duplicate_label";
        MBasicBlock bb;
        bb.label = ".Ldup";
        bb.append(MInstr::make(MOpcode::LABEL, {label(".Ldup")}));
        bb.append(MInstr::make(MOpcode::RET, {}));
        fn.addBlock(std::move(bb));

        bool threw = false;
        try {
            X64BinaryEncoder enc;
            CodeSection text, rodata;
            enc.encodeFunction(fn, text, rodata, false);
        } catch (const std::runtime_error &ex) {
            threw = std::string(ex.what()).find("duplicate label '.Ldup'") != std::string::npos;
        }
        CHECK(threw);
    }

    // ================================================================
    // Result
    // ================================================================
    if (gFail == 0) {
        std::cout << "All X64BinaryEncoder tests passed.\n";
        return EXIT_SUCCESS;
    }
    std::cerr << gFail << " X64BinaryEncoder test(s) FAILED.\n";
    return EXIT_FAILURE;
}
