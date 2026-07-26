//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/InstallerStubGen.hpp
// Purpose: Minimal x86-64 instruction emitter for generating Windows installer
//          and uninstaller stub machine code at packaging time.
//
// Key invariants:
//   - Emits Windows x64 ABI compatible code (shadow space, RCX/RDX/R8/R9).
//   - Labels support forward references with automatic fixup on finish().
//   - String data is embedded in a separate .rdata buffer.
//   - All generated code uses RIP-relative addressing for position independence.
//
// Ownership/Lifetime:
//   - Builder owns code, data, labels, and fixups; finalization returns a patched copy.
//
// Links: InstallerStub.hpp, PEBuilder.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares the minimal x86-64 encoder used by Windows package bootstraps.
/// @details The emitter appends machine instructions, tracks code/data/IAT
///          fixups, owns embedded UTF-16LE data, and finalizes position-independent
///          `.text` bytes after callers provide concrete PE section RVAs.

#pragma once

#include "PEBuilder.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace zanna::pkg {

/// @brief x86-64 register identifiers.
/// @details Numeric values match the low encoding bits plus the REX extension
///          bit expected by the instruction helpers.
enum class X64Reg : uint8_t {
    RAX = 0,
    RCX = 1,
    RDX = 2,
    RBX = 3,
    RSP = 4,
    RBP = 5,
    RSI = 6,
    RDI = 7,
    R8 = 8,
    R9 = 9,
    R10 = 10,
    R11 = 11,
    R12 = 12,
    R13 = 13,
    R14 = 14,
    R15 = 15
};

/// @brief Minimal x86-64 instruction emitter for installer stubs.
///
/// @details Generates position-independent code that calls Win32 APIs through
///          flat IAT slots using RIP-relative `call [rip+disp32]` instructions.
///          Labels and data references may be forward-declared because finalization
///          resolves displacements in a copy of the code buffer.
/// @ownership Owns emitted code, embedded data, labels, and pending fixups.
class InstallerStubGen {
  public:
    // ─── Label Management ─────────────────────────────────────────────

    /// @brief Create a label whose code position can be bound later.
    /// @return Stable identifier for use by branch and address fixups.
    uint32_t newLabel();

    /// @brief Bind a label to the current code position.
    /// @param labelId Identifier returned by newLabel().
    /// @throws std::runtime_error If the identifier is invalid or already bound.
    void bindLabel(uint32_t labelId);

    // ─── Basic Instructions ───────────────────────────────────────────

    /// @brief Emit a 64-bit register push.
    /// @param r Register whose value is pushed.
    void push(X64Reg r);

    /// @brief Emit a 64-bit register pop.
    /// @param r Register that receives the popped value.
    void pop(X64Reg r);

    /// @brief Emit a near return instruction.
    void ret();

    /// @brief Emit a one-byte no-operation instruction.
    void nop();

    // ─── MOV ──────────────────────────────────────────────────────────

    /// @brief mov dst, src (64-bit reg-to-reg)
    /// @param dst Destination register.
    /// @param src Source register.
    void movRegReg(X64Reg dst, X64Reg src);

    /// @brief mov dst, imm32 (zero-extended to 64-bit)
    /// @param dst Destination register.
    /// @param imm Immediate value.
    void movRegImm32(X64Reg dst, uint32_t imm);

    /// @brief mov dst, imm64 (full 64-bit immediate)
    /// @param dst Destination register.
    /// @param imm Immediate value.
    void movRegImm64(X64Reg dst, uint64_t imm);

    /// @brief mov dst, [src + disp32]  (load 64-bit from memory)
    /// @param dst Destination register.
    /// @param base Base register for the memory operand.
    /// @param disp Signed byte displacement from `base`.
    void movRegMem(X64Reg dst, X64Reg base, int32_t disp);

    /// @brief mov dst32, dword [src + disp32] (load 32-bit and zero-extend to 64-bit)
    /// @param dst Destination register.
    /// @param base Base register for the memory operand.
    /// @param disp Signed byte displacement from `base`.
    void movRegMem32(X64Reg dst, X64Reg base, int32_t disp);

    /// @brief mov [dst + disp32], src  (store 64-bit to memory)
    /// @param base Base register for the memory operand.
    /// @param disp Signed byte displacement from `base`.
    /// @param src Source register.
    void movMemReg(X64Reg base, int32_t disp, X64Reg src);

    /// @brief mov dword [dst + disp32], src (store low 32 bits to memory)
    /// @param base Base register for the memory operand.
    /// @param disp Signed byte displacement from `base`.
    /// @param src Register whose low 32 bits are stored.
    void movMemReg32(X64Reg base, int32_t disp, X64Reg src);

    /// @brief mov dword [dst + disp32], imm32 (store 32-bit immediate)
    /// @param base Base register for the memory operand.
    /// @param disp Signed byte displacement from `base`.
    /// @param imm Immediate value to store.
    void movMemImm32(X64Reg base, int32_t disp, uint32_t imm);

    /// @brief mov word [base + index * scale + disp32], imm16.
    /// @param base Base register for the memory operand.
    /// @param index Index register for the memory operand.
    /// @param scaleLog2 Scale encoded as log2(bytes): 0, 1, 2, or 3.
    /// @param disp Signed byte displacement.
    /// @param imm Immediate word to store.
    /// @throws std::runtime_error If the scale is invalid or the index is RSP/R12.
    void movMemIndexImm16(X64Reg base, X64Reg index, uint8_t scaleLog2, int32_t disp, uint16_t imm);

    /// @brief movzx dst, word [base + index * scale + disp32].
    /// @param dst Destination register.
    /// @param base Base register for the memory operand.
    /// @param index Index register for the memory operand.
    /// @param scaleLog2 Scale encoded as log2(bytes): 0, 1, 2, or 3.
    /// @param disp Signed byte displacement.
    /// @throws std::runtime_error If the scale is invalid or the index is RSP/R12.
    void movzxRegMemIndex16(X64Reg dst, X64Reg base, X64Reg index, uint8_t scaleLog2, int32_t disp);

    // ─── LEA ──────────────────────────────────────────────────────────

    /// @brief lea dst, [base + disp32]
    /// @param dst Destination register.
    /// @param base Base register for the effective address.
    /// @param disp Signed byte displacement from `base`.
    void leaRegMem(X64Reg dst, X64Reg base, int32_t disp);

    /// @brief lea dst, [base + index * scale + disp32]
    /// @param dst Destination register.
    /// @param base Base register for the effective address.
    /// @param index Index register for the effective address.
    /// @param scaleLog2 Base-two logarithm of the byte scale, from zero through three.
    /// @param disp Signed byte displacement.
    /// @throws std::runtime_error If the scale is invalid or the index is RSP/R12.
    void leaRegMemIndex(X64Reg dst, X64Reg base, X64Reg index, uint8_t scaleLog2, int32_t disp);

    /// @brief lea dst, [rip + disp32] where disp targets an emitted code label.
    /// @param dst Destination register.
    /// @param labelId Target label identifier resolved by finishText().
    void leaCodeLabel(X64Reg dst, uint32_t labelId);

    // ─── Arithmetic ───────────────────────────────────────────────────

    /// @brief Subtract a sign-extended immediate from a 64-bit register.
    /// @param dst Register updated with the difference.
    /// @param imm Immediate operand.
    void subRegImm32(X64Reg dst, uint32_t imm);

    /// @brief Subtract one 64-bit register from another.
    /// @param dst Register updated with the difference.
    /// @param src Register subtracted from `dst`.
    void subRegReg(X64Reg dst, X64Reg src);

    /// @brief Add a sign-extended immediate to a 64-bit register.
    /// @param dst Register updated with the sum.
    /// @param imm Immediate operand.
    void addRegImm32(X64Reg dst, uint32_t imm);

    /// @brief Add one 64-bit register to another.
    /// @param dst Register updated with the sum.
    /// @param src Register added to `dst`.
    void addRegReg(X64Reg dst, X64Reg src);

    /// @brief XOR two 64-bit registers.
    /// @param dst Register updated with the result.
    /// @param src Register XORed with `dst`; pass `dst` to zero the register.
    void xorRegReg(X64Reg dst, X64Reg src);

    // ─── Compare / Test ───────────────────────────────────────────────

    /// @brief Set flags from the bitwise AND of two registers without storing it.
    /// @param a First register operand.
    /// @param b Second register operand.
    void testRegReg(X64Reg a, X64Reg b);

    /// @brief Compare a 64-bit register with a sign-extended immediate.
    /// @param r Register operand.
    /// @param imm Immediate operand.
    void cmpRegImm32(X64Reg r, uint32_t imm);

    /// @brief Compare two 64-bit registers.
    /// @param a Minuend register.
    /// @param b Subtrahend register.
    void cmpRegReg(X64Reg a, X64Reg b);

    // ─── Conditional Jumps ────────────────────────────────────────────

    /// @brief Emit a near jump when the zero flag is set.
    /// @param labelId Target label identifier.
    void jz(uint32_t labelId);

    /// @brief Emit a near jump when the zero flag is clear.
    /// @param labelId Target label identifier.
    void jnz(uint32_t labelId);

    /// @brief Emit a near jump for an unsigned-above comparison.
    /// @param labelId Target label identifier.
    void ja(uint32_t labelId);

    /// @brief Emit an unconditional near jump.
    /// @param labelId Target label identifier.
    void jmp(uint32_t labelId);

    // ─── Call ─────────────────────────────────────────────────────────

    /// @brief call [rip + disp32] — call through IAT slot by flat index.
    /// @param flatIndex  Flat function index across all DLLs in the import list.
    ///
    /// The actual IAT slot RVA is computed during finishText() from the
    /// import list and IAT base RVA.
    void callIATSlot(uint32_t flatIndex);

    /// @brief call reg - indirect call through a register containing a function pointer.
    /// @param target Register containing the target function address.
    void callReg(X64Reg target);

    // ─── Data Embedding ───────────────────────────────────────────────

    /// @brief lea dst, [rip + disp32] — load address of embedded data.
    /// @param dst        Destination register.
    /// @param dataOffset Offset within the data section returned by an embedding method.
    void leaRipData(X64Reg dst, uint32_t dataOffset);

    // ─── Data Embedding ───────────────────────────────────────────────

    /// @brief Transcode UTF-8 text to a null-terminated UTF-16LE data-section string.
    /// @param asciiStr UTF-8 text to embed.
    /// @return Byte offset of the encoded string within the data section.
    uint32_t embedStringW(const std::string &asciiStr);

    /// @brief Embed a raw byte array in the data section.
    /// @param data Address of the bytes to append.
    /// @param len Number of bytes to append.
    /// @return Byte offset of the blob within the data section.
    uint32_t embedBytes(const void *data, size_t len);

    // ─── Output ───────────────────────────────────────────────────────

    /// @brief Finalize the code section, resolving all labels and fixups.
    /// @param textRVA        The RVA at which .text will be loaded.
    /// @param iatBaseRVA     The base RVA of the IAT within .rdata.
    /// @param imports        The import list (for IAT slot index resolution).
    /// @param dataBaseRVA    The base RVA where embedded data starts in .rdata.
    /// @return A completed copy of the `.text` bytes; the builder remains reusable.
    /// @throws std::runtime_error If a referenced label is unbound or an IAT index is invalid.
    std::vector<uint8_t> finishText(uint32_t textRVA,
                                    uint32_t iatBaseRVA,
                                    const std::vector<PEImport> &imports,
                                    uint32_t dataBaseRVA) const;

    /// @brief Get the embedded data bytes (for .rdata or appended to existing .rdata).
    /// @return Const reference valid until this builder is destroyed or embeds more data.
    const std::vector<uint8_t> &dataSection() const {
        return data_;
    }

    /// @brief Current code size (before finalization).
    /// @return Number of instruction bytes currently emitted.
    size_t codeSize() const {
        return code_.size();
    }

  private:
    std::vector<uint8_t> code_;
    std::vector<uint8_t> data_;

    /// @brief Binding state and code position for one generated label.
    struct LabelInfo {
        uint32_t codeOffset; ///< Offset in code_ where label is bound (0 if unbound).
        bool bound;          ///< Whether bindLabel() has assigned the offset.
    };

    std::vector<LabelInfo> labels_;

    /// @brief Types of fixups that need resolution at finish time.
    enum class FixupKind : uint8_t {
        Rel32,        ///< 32-bit relative jump/call (code → code label)
        IATSlotIndex, ///< IAT slot by flat function index (resolved to RIP-relative)
        DataRel32,    ///< RIP-relative to embedded data (code → .rdata data section)
    };

    /// @brief Deferred displacement or address-table reference.
    struct Fixup {
        uint32_t codeOffset; ///< Offset in code_ of the 4-byte displacement field.
        uint32_t target;     ///< Label ID / flat IAT index / data offset, depending on kind.
        FixupKind kind;      ///< Interpretation of `target` and patch calculation.
    };

    std::vector<Fixup> fixups_;

    // Encoding helpers
    /// @brief Append one byte to the code buffer.
    /// @param b Byte to append.
    void emit(uint8_t b);

    /// @brief Append a 32-bit little-endian value to the code buffer.
    /// @param v Value to encode.
    void emit32(uint32_t v);

    /// @brief Emit a REX prefix byte. `w` sets the 64-bit operand-size bit;
    /// `reg` and `rm` supply the extension bits for the ModRM reg and r/m fields.
    /// @param w Whether to set the 64-bit operand-size bit.
    /// @param reg Register encoded in the ModRM reg field.
    /// @param rm Register encoded in the ModRM r/m field.
    void emitREX(bool w, X64Reg reg, X64Reg rm);

    /// @brief Emit a ModRM byte with the given mod (2 bits), reg (3 bits), rm (3 bits) fields.
    /// @param mod Addressing-mode bits.
    /// @param reg Register/opcode bits.
    /// @param rm Register/memory bits.
    void emitModRM(uint8_t mod, uint8_t reg, uint8_t rm);

    /// @brief Emit a ModRM byte plus a 32-bit displacement for [base + disp32] addressing.
    /// Handles the RSP/R12 SIB escape and RBP/R13 mandatory-displacement cases.
    /// @param reg Register/opcode bits for the ModRM reg field.
    /// @param base Base register for the memory operand.
    /// @param disp Signed byte displacement.
    void emitModRMDisp32(uint8_t reg, X64Reg base, int32_t disp);

    /// @brief Test whether a register needs a REX extension bit.
    /// @param r Register to inspect.
    /// @return `true` for R8 through R15.
    bool needsREX_B(X64Reg r) const {
        return static_cast<uint8_t>(r) >= 8;
    }

    /// @brief Extract the register's three-bit ModRM encoding.
    /// @param r Register to encode.
    /// @return Low three bits of the register identifier.
    uint8_t regBits(X64Reg r) const {
        return static_cast<uint8_t>(r) & 7;
    }
};

} // namespace zanna::pkg
