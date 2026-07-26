//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/InstallerStubGen.cpp
// Purpose: x86-64 instruction encoding for installer stub generation.
//
// Key invariants:
//   - All encodings follow Intel x86-64 reference (Volume 2).
//   - REX prefix emitted when using R8-R15 or 64-bit operand size.
//   - Label fixups resolved in finishText() — forward jumps use placeholder 0.
//   - IAT calls use RIP-relative addressing: ff 15 [disp32].
//
// Ownership/Lifetime:
//   - Builder owns all buffers and fixups; finishText() patches a returned copy.
//
// Links: InstallerStubGen.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements x86-64 instruction encoding and PE-relative fixup resolution.
/// @details Helpers emit only the instruction subset required by installer hosts,
///          including ModRM/SIB/REX forms, labels, IAT calls, embedded data
///          references, arithmetic, comparisons, and conditional control flow.

#include "InstallerStubGen.hpp"
#include "PkgUtils.hpp"

#include <cstring>
#include <stdexcept>

namespace zanna::pkg {

// ============================================================================
// Helpers
// ============================================================================

/// @brief Append one byte to the code buffer.
/// @param b Byte to append.
void InstallerStubGen::emit(uint8_t b) {
    code_.push_back(b);
}

/// @brief Append four bytes to the code buffer in little-endian order.
/// @param v Unsigned value to encode.
void InstallerStubGen::emit32(uint32_t v) {
    code_.push_back(static_cast<uint8_t>(v & 0xFF));
    code_.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    code_.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    code_.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

/// @brief Emit a REX prefix byte. Sets REX.W for 64-bit operand size; REX.R/REX.B
/// for reg and r/m extension bits when encoding R8–R15 registers.
/// @param w Whether to set the 64-bit operand-size bit.
/// @param reg Register encoded in the ModRM reg field.
/// @param rm Register encoded in the ModRM r/m field.
void InstallerStubGen::emitREX(bool w, X64Reg reg, X64Reg rm) {
    uint8_t rex = 0x40;
    if (w)
        rex |= 0x08; // REX.W
    if (needsREX_B(reg))
        rex |= 0x04; // REX.R (extends ModRM reg field)
    if (needsREX_B(rm))
        rex |= 0x01; // REX.B (extends ModRM r/m field)
    // Always emit REX if W bit needed or if extended registers used
    if (rex != 0x40 || w)
        emit(rex);
}

/// @brief Emit a ModRM byte from the given mod (2 bits), reg (3 bits), and rm (3 bits) fields.
/// @param mod Two-bit addressing mode.
/// @param reg Three-bit register/opcode field.
/// @param rm Three-bit register/memory field.
void InstallerStubGen::emitModRM(uint8_t mod, uint8_t reg, uint8_t rm) {
    emit(static_cast<uint8_t>((mod << 6) | (reg << 3) | rm));
}

/// @brief Emit ModRM + optional SIB + 32-bit displacement for [base + disp32] addressing.
/// Handles the RSP/R12 SIB escape (baseBits==4) and the RBP/R13 forced-disp8 case (baseBits==5,
/// disp==0).
/// @param reg Low ModRM register/opcode bits.
/// @param base Address base register.
/// @param disp Signed displacement.
void InstallerStubGen::emitModRMDisp32(uint8_t reg, X64Reg base, int32_t disp) {
    uint8_t baseBits = regBits(base);
    if (baseBits == 4) {
        // RSP/R12 needs SIB byte
        emitModRM(2, reg, 4); // mod=10, rm=100 (SIB follows)
        emit(0x24);           // SIB: scale=0, index=RSP(none), base=RSP
    } else if (baseBits == 5 && disp == 0) {
        // RBP/R13 with disp=0 needs explicit disp8=0 (mod=01)
        emitModRM(1, reg, baseBits);
        emit(0);
        return;
    } else {
        emitModRM(2, reg, baseBits); // mod=10 (disp32)
    }
    emit32(static_cast<uint32_t>(disp));
}

// ============================================================================
// Label Management
// ============================================================================

/// @brief Allocate a new unbound label and return its ID for use with bind/jump instructions.
uint32_t InstallerStubGen::newLabel() {
    uint32_t id = static_cast<uint32_t>(labels_.size());
    labels_.push_back({0, false});
    return id;
}

/// @brief Bind a label to the current code position. Throws if the label ID is out of range.
/// @param labelId Identifier returned by @ref newLabel.
/// @throws std::runtime_error For an unknown identifier.
void InstallerStubGen::bindLabel(uint32_t labelId) {
    if (labelId >= labels_.size())
        throw std::runtime_error("InstallerStubGen: invalid label ID");
    labels_[labelId].codeOffset = static_cast<uint32_t>(code_.size());
    labels_[labelId].bound = true;
}

// ============================================================================
// Basic Instructions
// ============================================================================

/// @brief Emit push r — opcode 50+rd, with REX.B prefix for R8–R15.
/// @param r Register to push.
void InstallerStubGen::push(X64Reg r) {
    if (needsREX_B(r))
        emit(0x41); // REX.B
    emit(static_cast<uint8_t>(0x50 + regBits(r)));
}

/// @brief Emit pop r — opcode 58+rd, with REX.B prefix for R8–R15.
/// @param r Register to pop.
void InstallerStubGen::pop(X64Reg r) {
    if (needsREX_B(r))
        emit(0x41); // REX.B
    emit(static_cast<uint8_t>(0x58 + regBits(r)));
}

/// @brief Emit near ret (C3) — return from the generated stub entry point.
void InstallerStubGen::ret() {
    emit(0xC3);
}

/// @brief Emit a 1-byte NOP (90) — used for padding or alignment.
void InstallerStubGen::nop() {
    emit(0x90);
}

// ============================================================================
// MOV
// ============================================================================

/// @brief Emit mov dst, src — 64-bit register-to-register move (REX.W + 89 /r).
/// @param dst Destination register.
/// @param src Source register.
void InstallerStubGen::movRegReg(X64Reg dst, X64Reg src) {
    // REX.W + 89 /r (mov r/m64, r64 — src is reg field)
    emitREX(true, src, dst);
    emit(0x89);
    emitModRM(3, regBits(src), regBits(dst));
}

/// @brief Emit mov dst, imm32 — load a 32-bit immediate, zero-extended to 64 bits (B8+rd id).
/// @param dst Destination register.
/// @param imm Immediate value.
void InstallerStubGen::movRegImm32(X64Reg dst, uint32_t imm) {
    // For R8-R15, need REX prefix; use B8+rd with 32-bit imm (zero-extends)
    if (needsREX_B(dst))
        emit(0x41);
    emit(static_cast<uint8_t>(0xB8 + regBits(dst)));
    emit32(imm);
}

/// @brief Emit mov dst, imm64 — load a full 64-bit immediate (REX.W + B8+rd + imm64).
/// @param dst Destination register.
/// @param imm Immediate value.
void InstallerStubGen::movRegImm64(X64Reg dst, uint64_t imm) {
    // REX.W + B8+rd + imm64
    uint8_t rex = 0x48;
    if (needsREX_B(dst))
        rex |= 0x01;
    emit(rex);
    emit(static_cast<uint8_t>(0xB8 + regBits(dst)));
    // Emit 64-bit immediate
    for (int i = 0; i < 8; i++)
        emit(static_cast<uint8_t>((imm >> (i * 8)) & 0xFF));
}

/// @brief Emit mov dst, [base + disp32] — 64-bit load from memory (REX.W + 8B /r).
/// @param dst Destination register.
/// @param base Address base register.
/// @param disp Signed displacement.
void InstallerStubGen::movRegMem(X64Reg dst, X64Reg base, int32_t disp) {
    // REX.W + 8B /r (mov r64, r/m64)
    emitREX(true, dst, base);
    emit(0x8B);
    emitModRMDisp32(regBits(dst), base, disp);
}

/// @brief Emit mov dst32, dword [base + disp32] -- a zero-extending 32-bit memory load.
/// @param dst Destination register.
/// @param base Address base register.
/// @param disp Signed displacement.
void InstallerStubGen::movRegMem32(X64Reg dst, X64Reg base, int32_t disp) {
    emitREX(false, dst, base);
    emit(0x8B);
    emitModRMDisp32(regBits(dst), base, disp);
}

/// @brief Emit mov [base + disp32], src — 64-bit store to memory (REX.W + 89 /r).
/// @param base Address base register.
/// @param disp Signed displacement.
/// @param src Source register.
void InstallerStubGen::movMemReg(X64Reg base, int32_t disp, X64Reg src) {
    // REX.W + 89 /r (mov r/m64, r64)
    emitREX(true, src, base);
    emit(0x89);
    emitModRMDisp32(regBits(src), base, disp);
}

/// @brief Emit a 32-bit register store to `[base + disp32]`.
/// @param base Address base register.
/// @param disp Signed displacement.
/// @param src Register supplying its low 32 bits.
void InstallerStubGen::movMemReg32(X64Reg base, int32_t disp, X64Reg src) {
    emitREX(false, src, base);
    emit(0x89);
    emitModRMDisp32(regBits(src), base, disp);
}

/// @brief Emit mov dword [base + disp32], imm32 — 32-bit immediate store to memory (C7 /0 id).
/// @param base Address base register.
/// @param disp Signed displacement.
/// @param imm Immediate value.
void InstallerStubGen::movMemImm32(X64Reg base, int32_t disp, uint32_t imm) {
    // C7 /0 id (mov r/m32, imm32) — no REX.W so it's 32-bit
    if (needsREX_B(base))
        emit(0x41);
    emit(0xC7);
    emitModRMDisp32(0, base, disp);
    emit32(imm);
}

/// @brief Emit mov word [base + index * scale + disp32], imm16 — 16-bit SIB store (66 C7 /0).
/// Used to write individual UTF-16 code units into a stack-allocated string buffer.
/// @param base Address base register.
/// @param index Address index register.
/// @param scaleLog2 Log2 byte scale from zero through three.
/// @param disp Signed displacement.
/// @param imm Immediate code unit.
void InstallerStubGen::movMemIndexImm16(
    X64Reg base, X64Reg index, uint8_t scaleLog2, int32_t disp, uint16_t imm) {
    if (scaleLog2 > 3)
        throw std::runtime_error("InstallerStubGen: invalid SIB scale");
    if (regBits(index) == 4)
        throw std::runtime_error("InstallerStubGen: RSP/R12 cannot be used as SIB index");
    uint8_t rex = 0x40;
    if (needsREX_B(base))
        rex |= 0x01;
    if (needsREX_B(index))
        rex |= 0x02;
    emit(0x66);
    if (rex != 0x40)
        emit(rex);
    emit(0xC7);
    emitModRM(2, 0, 4);
    emit(static_cast<uint8_t>((scaleLog2 << 6) | (regBits(index) << 3) | regBits(base)));
    emit32(static_cast<uint32_t>(disp));
    emit(static_cast<uint8_t>(imm & 0xFF));
    emit(static_cast<uint8_t>((imm >> 8) & 0xFF));
}

/// @brief Emit movzx dst, word [base + index * scale + disp32] — zero-extend 16-bit SIB load (0F B7
/// /r). Used to load individual UTF-16 code units from a string buffer into a 64-bit register.
/// @param dst Destination register.
/// @param base Address base register.
/// @param index Address index register.
/// @param scaleLog2 Log2 byte scale from zero through three.
/// @param disp Signed displacement.
void InstallerStubGen::movzxRegMemIndex16(
    X64Reg dst, X64Reg base, X64Reg index, uint8_t scaleLog2, int32_t disp) {
    if (scaleLog2 > 3)
        throw std::runtime_error("InstallerStubGen: invalid SIB scale");
    if (regBits(index) == 4)
        throw std::runtime_error("InstallerStubGen: RSP/R12 cannot be used as SIB index");
    uint8_t rex = 0x40;
    if (needsREX_B(dst))
        rex |= 0x04;
    if (needsREX_B(index))
        rex |= 0x02;
    if (needsREX_B(base))
        rex |= 0x01;
    if (rex != 0x40)
        emit(rex);
    emit(0x0F);
    emit(0xB7);
    emitModRM(2, regBits(dst), 4);
    emit(static_cast<uint8_t>((scaleLog2 << 6) | (regBits(index) << 3) | regBits(base)));
    emit32(static_cast<uint32_t>(disp));
}

// ============================================================================
// LEA
// ============================================================================

/// @brief Emit lea dst, [base + disp32] — compute effective address (REX.W + 8D /r).
/// @param dst Destination register.
/// @param base Address base register.
/// @param disp Signed displacement.
void InstallerStubGen::leaRegMem(X64Reg dst, X64Reg base, int32_t disp) {
    // REX.W + 8D /r
    emitREX(true, dst, base);
    emit(0x8D);
    emitModRMDisp32(regBits(dst), base, disp);
}

/// @brief Emit lea dst, [base + index * scale + disp32] — SIB-form effective address (REX.W + 8D
/// /r).
/// @param dst Destination register.
/// @param base Address base register.
/// @param index Address index register.
/// @param scaleLog2 Log2 byte scale from zero through three.
/// @param disp Signed displacement.
void InstallerStubGen::leaRegMemIndex(
    X64Reg dst, X64Reg base, X64Reg index, uint8_t scaleLog2, int32_t disp) {
    if (scaleLog2 > 3)
        throw std::runtime_error("InstallerStubGen: invalid SIB scale");
    if (regBits(index) == 4)
        throw std::runtime_error("InstallerStubGen: RSP/R12 cannot be used as SIB index");
    uint8_t rex = 0x48;
    if (needsREX_B(dst))
        rex |= 0x04;
    if (needsREX_B(index))
        rex |= 0x02;
    if (needsREX_B(base))
        rex |= 0x01;
    emit(rex);
    emit(0x8D);
    emitModRM(2, regBits(dst), 4);
    emit(static_cast<uint8_t>((scaleLog2 << 6) | (regBits(index) << 3) | regBits(base)));
    emit32(static_cast<uint32_t>(disp));
}

/// @brief Emit RIP-relative LEA targeting a generated code label.
/// @param dst Destination register.
/// @param labelId Target label identifier resolved during finalization.
void InstallerStubGen::leaCodeLabel(X64Reg dst, uint32_t labelId) {
    emitREX(true, dst, X64Reg::RBP);
    emit(0x8D);
    emitModRM(0, regBits(dst), 5);
    fixups_.push_back({static_cast<uint32_t>(code_.size()), labelId, FixupKind::Rel32});
    emit32(0);
}

// ============================================================================
// Arithmetic
// ============================================================================

/// @brief Emit sub dst, imm32 — subtract sign-extended 32-bit immediate from 64-bit register (REX.W
/// + 81 /5).
/// @param dst Destination register.
/// @param imm Immediate operand.
void InstallerStubGen::subRegImm32(X64Reg dst, uint32_t imm) {
    // REX.W + 81 /5 id
    emitREX(true, X64Reg::RAX, dst);
    emit(0x81);
    emitModRM(3, 5, regBits(dst));
    emit32(imm);
}

/// @brief Emit sub dst, src — 64-bit register subtraction (REX.W + 29 /r).
/// @param dst Register updated with the difference.
/// @param src Register subtracted.
void InstallerStubGen::subRegReg(X64Reg dst, X64Reg src) {
    // REX.W + 29 /r (sub r/m64, r64)
    emitREX(true, src, dst);
    emit(0x29);
    emitModRM(3, regBits(src), regBits(dst));
}

/// @brief Emit add dst, imm32 — add sign-extended 32-bit immediate to 64-bit register (REX.W + 81
/// /0).
/// @param dst Destination register.
/// @param imm Immediate operand.
void InstallerStubGen::addRegImm32(X64Reg dst, uint32_t imm) {
    // REX.W + 81 /0 id
    emitREX(true, X64Reg::RAX, dst);
    emit(0x81);
    emitModRM(3, 0, regBits(dst));
    emit32(imm);
}

/// @brief Emit add dst, src — 64-bit register addition (REX.W + 01 /r).
/// @param dst Register updated with the sum.
/// @param src Register added.
void InstallerStubGen::addRegReg(X64Reg dst, X64Reg src) {
    // REX.W + 01 /r (add r/m64, r64)
    emitREX(true, src, dst);
    emit(0x01);
    emitModRM(3, regBits(src), regBits(dst));
}

/// @brief Emit xor dst, src — 64-bit XOR; dst==src is the canonical zero-register idiom (REX.W + 31
/// /r).
/// @param dst Register updated with the result.
/// @param src Register XOR operand.
void InstallerStubGen::xorRegReg(X64Reg dst, X64Reg src) {
    // REX.W + 31 /r
    emitREX(true, src, dst);
    emit(0x31);
    emitModRM(3, regBits(src), regBits(dst));
}

// ============================================================================
// Compare / Test
// ============================================================================

/// @brief Emit test a, b — sets flags from a AND b without storing the result (REX.W + 85 /r).
/// @param a First register operand.
/// @param b Second register operand.
void InstallerStubGen::testRegReg(X64Reg a, X64Reg b) {
    // REX.W + 85 /r
    emitREX(true, b, a);
    emit(0x85);
    emitModRM(3, regBits(b), regBits(a));
}

/// @brief Emit cmp r, imm32 — compare register against sign-extended 32-bit immediate (REX.W + 81
/// /7).
/// @param r Register operand.
/// @param imm Immediate value interpreted as a sign-extended 32-bit operand.
void InstallerStubGen::cmpRegImm32(X64Reg r, uint32_t imm) {
    // REX.W + 81 /7 id
    emitREX(true, X64Reg::RAX, r);
    emit(0x81);
    emitModRM(3, 7, regBits(r));
    emit32(imm);
}

/// @brief Emit cmp a, b — 64-bit register comparison, sets flags from a - b (REX.W + 39 /r).
/// @param a Register supplying the minuend.
/// @param b Register supplying the subtrahend.
void InstallerStubGen::cmpRegReg(X64Reg a, X64Reg b) {
    // REX.W + 39 /r
    emitREX(true, b, a);
    emit(0x39);
    emitModRM(3, regBits(b), regBits(a));
}

// ============================================================================
// Conditional Jumps
// ============================================================================

/// @brief Emit jz / je — 32-bit near jump if zero flag set (0F 84 cd); records a Rel32 fixup.
/// @param labelId Identifier of the code label to resolve during finalization.
void InstallerStubGen::jz(uint32_t labelId) {
    // 0F 84 cd (jz rel32)
    emit(0x0F);
    emit(0x84);
    fixups_.push_back({static_cast<uint32_t>(code_.size()), labelId, FixupKind::Rel32});
    emit32(0); // placeholder
}

/// @brief Emit jnz / jne — 32-bit near jump if zero flag clear (0F 85 cd); records a Rel32 fixup.
/// @param labelId Identifier of the code label to resolve during finalization.
void InstallerStubGen::jnz(uint32_t labelId) {
    // 0F 85 cd (jnz rel32)
    emit(0x0F);
    emit(0x85);
    fixups_.push_back({static_cast<uint32_t>(code_.size()), labelId, FixupKind::Rel32});
    emit32(0); // placeholder
}

/// @brief Emit ja — 32-bit near jump if above (unsigned >, CF=0 and ZF=0) (0F 87 cd); records a
/// Rel32 fixup.
/// @param labelId Identifier of the code label to resolve during finalization.
void InstallerStubGen::ja(uint32_t labelId) {
    // 0F 87 cd (ja rel32) — unsigned above.
    emit(0x0F);
    emit(0x87);
    fixups_.push_back({static_cast<uint32_t>(code_.size()), labelId, FixupKind::Rel32});
    emit32(0);
}

/// @brief Emit jmp — unconditional 32-bit near jump (E9 cd); records a Rel32 fixup.
/// @param labelId Identifier of the code label to resolve during finalization.
void InstallerStubGen::jmp(uint32_t labelId) {
    // E9 cd (jmp rel32)
    emit(0xE9);
    fixups_.push_back({static_cast<uint32_t>(code_.size()), labelId, FixupKind::Rel32});
    emit32(0); // placeholder
}

// ============================================================================
// Call
// ============================================================================

/// @brief Emit call [rip + disp32] — indirect call through an IAT slot (FF 15 cd).
/// Records an IATSlotIndex fixup; the displacement is resolved to the correct RVA in finishText().
/// @param flatIndex Zero-based function index across all DLL import entries.
void InstallerStubGen::callIATSlot(uint32_t flatIndex) {
    // FF 15 [disp32] — call [rip + disp32]
    emit(0xFF);
    emit(0x15);
    fixups_.push_back({static_cast<uint32_t>(code_.size()), flatIndex, FixupKind::IATSlotIndex});
    emit32(0); // placeholder — resolved in finishText()
}

/// @brief Emit an indirect near call through a 64-bit register (`FF /2`).
/// @param target Register containing the target function address.
void InstallerStubGen::callReg(X64Reg target) {
    emitREX(false, X64Reg::RAX, target);
    emit(0xFF);
    emitModRM(3, 2, regBits(target));
}

/// @brief Emit RIP-relative LEA targeting bytes in the generated data section.
/// @details Records a data-relative fixup whose displacement is resolved against
///          `dataBaseRVA` by finishText().
/// @param dst Destination register that receives the data address.
/// @param dataOffset Byte offset from the start of the generated data section.
void InstallerStubGen::leaRipData(X64Reg dst, uint32_t dataOffset) {
    // REX.W + 8D /r with ModRM(00, reg, 101) = RIP-relative addressing
    emitREX(true, dst, X64Reg::RBP); // RBP's regBits = 5 = 101b (RIP-relative encoding)
    emit(0x8D);
    emitModRM(0, regBits(dst), 5); // mod=00, rm=101 = RIP+disp32
    fixups_.push_back({static_cast<uint32_t>(code_.size()), dataOffset, FixupKind::DataRel32});
    emit32(0); // placeholder — resolved in finishText()
}

// ============================================================================
// Data Embedding
// ============================================================================

/// @brief Encode `asciiStr` as a null-terminated UTF-16LE string and append it to the data section.
/// Returns the byte offset within the data section where the string starts.
/// @param asciiStr UTF-8 text to transcode and terminate.
/// @return Byte offset of the first encoded code unit in the data section.
uint32_t InstallerStubGen::embedStringW(const std::string &asciiStr) {
    uint32_t offset = static_cast<uint32_t>(data_.size());
    const auto bytes = utf8ToUtf16LEBytes(asciiStr, true);
    data_.insert(data_.end(), bytes.begin(), bytes.end());
    return offset;
}

/// @brief Append `len` raw bytes from `data` to the data section.
/// Returns the byte offset within the data section where the blob starts.
/// @param data Address of the source bytes.
/// @param len Number of bytes to append.
/// @return Byte offset at which the blob begins in the data section.
uint32_t InstallerStubGen::embedBytes(const void *data, size_t len) {
    uint32_t offset = static_cast<uint32_t>(data_.size());
    auto *p = static_cast<const uint8_t *>(data);
    data_.insert(data_.end(), p, p + len);
    return offset;
}

// ============================================================================
// Finalization
// ============================================================================

/// @brief Compute the IAT slot RVA for a given flat function index.
/// @param imports Ordered DLL and function imports represented by the flat index.
/// @param iatBaseRVA RVA of the first IAT entry.
/// @param flatIndex Zero-based function index across the concatenated DLL entries.
/// @return RVA of the selected 64-bit IAT slot.
/// @throws std::runtime_error If `flatIndex` does not identify an imported function.
static uint32_t computeIATSlotRVA(const std::vector<PEImport> &imports,
                                  uint32_t iatBaseRVA,
                                  uint32_t flatIndex) {
    uint32_t offset = 0;
    uint32_t idx = 0;
    for (const auto &dll : imports) {
        for (size_t f = 0; f < dll.functions.size(); ++f) {
            if (idx == flatIndex)
                return iatBaseRVA + offset;
            offset += 8;
            idx++;
        }
        offset += 8; // null terminator after each DLL's entries
    }
    throw std::runtime_error("InstallerStubGen: IAT slot index out of range");
}

/// @brief Patch a 4-byte little-endian displacement field in `buf` at offset `off`.
/// Used to back-fill placeholder zeros left by forward-reference fixups after all code is emitted.
/// @param buf Mutable code buffer containing the placeholder.
/// @param off Byte offset of the four-byte field.
/// @param val Signed displacement to encode in little-endian order.
static void patchLE32(std::vector<uint8_t> &buf, uint32_t off, int32_t val) {
    buf[off + 0] = static_cast<uint8_t>(val & 0xFF);
    buf[off + 1] = static_cast<uint8_t>((val >> 8) & 0xFF);
    buf[off + 2] = static_cast<uint8_t>((val >> 16) & 0xFF);
    buf[off + 3] = static_cast<uint8_t>((val >> 24) & 0xFF);
}

/// @brief Finalize the code section by resolving all recorded fixups and returning the patched
/// bytes. Iterates over every Fixup entry and back-fills the 32-bit displacement placeholder:
///   - Rel32: code-relative label offset (target label codeOffset - fixup end).
///   - IATSlotIndex: RIP-relative displacement to the correct IAT slot RVA.
///   - DataRel32: RIP-relative displacement to the embedded data byte in .rdata.
/// Throws if any label referenced by a fixup is unbound or if an IAT flat index is out of range.
/// @param textRVA RVA at which the generated code section begins.
/// @param iatBaseRVA RVA of the first import-address-table entry.
/// @param imports Ordered imports used to map flat function indices to IAT slots.
/// @param dataBaseRVA RVA at which the generated data buffer begins.
/// @return A patched copy of the code buffer; the builder remains unchanged.
/// @throws std::runtime_error If a label is unbound or an IAT index is invalid.
std::vector<uint8_t> InstallerStubGen::finishText(uint32_t textRVA,
                                                  uint32_t iatBaseRVA,
                                                  const std::vector<PEImport> &imports,
                                                  uint32_t dataBaseRVA) const {
    std::vector<uint8_t> result = code_;

    for (const auto &f : fixups_) {
        // RIP at the end of the disp32 field = textRVA + f.codeOffset + 4
        uint32_t rip = textRVA + f.codeOffset + 4;

        switch (f.kind) {
            case FixupKind::Rel32: {
                // Resolve label-relative jump (code → code)
                if (f.target >= labels_.size() || !labels_[f.target].bound)
                    throw std::runtime_error("InstallerStubGen: unbound label in fixup");
                int32_t rel = static_cast<int32_t>(labels_[f.target].codeOffset) -
                              static_cast<int32_t>(f.codeOffset + 4);
                patchLE32(result, f.codeOffset, rel);
                break;
            }
            case FixupKind::IATSlotIndex: {
                // Resolve flat function index → IAT slot RVA → RIP-relative
                uint32_t slotRVA = computeIATSlotRVA(imports, iatBaseRVA, f.target);
                int32_t rel = static_cast<int32_t>(slotRVA) - static_cast<int32_t>(rip);
                patchLE32(result, f.codeOffset, rel);
                break;
            }
            case FixupKind::DataRel32: {
                // Resolve data offset → absolute RVA → RIP-relative
                uint32_t dataRVA = dataBaseRVA + f.target;
                int32_t rel = static_cast<int32_t>(dataRVA) - static_cast<int32_t>(rip);
                patchLE32(result, f.codeOffset, rel);
                break;
            }
        }
    }

    return result;
}

} // namespace zanna::pkg
