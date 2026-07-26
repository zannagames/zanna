//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/InstallerStubGenA64.hpp
// Purpose: Minimal AArch64 instruction emitter for native Windows ARM64
//          installer bootstrap code.
//
// Key invariants:
//   - Emits fixed-width 32-bit AArch64 instructions in little-endian order.
//   - Labels support forward references resolved during finishText().
//   - Embedded string/data bytes live in a separate .rdata buffer.
//
// Ownership/Lifetime:
//   - Builder owns emitted code, embedded data, labels, and fixups.
//   - Finalization patches and returns a copy, leaving the builder unchanged.
//
// Links: InstallerStub.hpp, InstallerStubGen.hpp, PEBuilder.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares the AArch64 encoder used by native Windows ARM64 package stubs.
/// @details The emitter owns fixed-width instruction bytes, embedded read-only data,
///          labels, and deferred PE-relative fixups. Finalization resolves those
///          fixups after callers provide the concrete section RVAs.

#pragma once

#include "PEBuilder.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace zanna::pkg {

/// @brief AArch64 64-bit general-purpose register identifiers.
/// @details Values match the five-bit register fields used by A64 instructions;
///          value 31 denotes SP where supported by the selected encoding.
enum class A64Reg : uint8_t {
    X0 = 0,
    X1 = 1,
    X2 = 2,
    X3 = 3,
    X4 = 4,
    X5 = 5,
    X6 = 6,
    X7 = 7,
    X8 = 8,
    X9 = 9,
    X10 = 10,
    X11 = 11,
    X12 = 12,
    X13 = 13,
    X14 = 14,
    X15 = 15,
    X16 = 16,
    X17 = 17,
    X18 = 18,
    X19 = 19,
    X20 = 20,
    X21 = 21,
    X22 = 22,
    X23 = 23,
    X24 = 24,
    X25 = 25,
    X26 = 26,
    X27 = 27,
    X28 = 28,
    X29 = 29,
    X30 = 30,
    SP = 31,
};

/// @brief Minimal AArch64 emitter used by native Windows ARM64 installer stubs.
/// @details Generates Windows ARM64 bootstrap instructions in little-endian order.
///          Branches, IAT references, and embedded-data addresses are recorded as
///          fixups and resolved in a returned code copy by finishText().
/// @ownership Owns the instruction, data, label, and fixup buffers.
class InstallerStubGenA64 {
  public:
    /// @brief Create a label whose code position can be bound later.
    /// @return Stable identifier accepted by branch methods and bindLabel().
    uint32_t newLabel();

    /// @brief Bind a label to the current instruction-buffer position.
    /// @param labelId Identifier returned by newLabel().
    /// @throws std::runtime_error If `labelId` is invalid.
    void bindLabel(uint32_t labelId);

    /// @brief Emit a return from subroutine instruction.
    void ret();

    /// @brief Emit a no-operation instruction.
    void nop();

    /// @brief Load a 32-bit immediate into a 64-bit register using MOVZ/MOVK.
    /// @param dst Destination register.
    /// @param imm Immediate value.
    void movRegImm32(A64Reg dst, uint32_t imm);

    /// @brief Copy a 64-bit register, using ADD when either operand denotes SP.
    /// @param dst Destination register.
    /// @param src Source register.
    void movRegReg(A64Reg dst, A64Reg src);

    /// @brief Add an encodable immediate to a 64-bit register or SP.
    /// @param dst Destination register.
    /// @param src Source register.
    /// @param imm Immediate representable as a 12-bit value optionally shifted by 12.
    /// @throws std::runtime_error If `imm` cannot be represented by the instruction.
    void addRegImm(A64Reg dst, A64Reg src, uint32_t imm);

    /// @brief Subtract an encodable immediate from a 64-bit register or SP.
    /// @param dst Destination register.
    /// @param src Source register.
    /// @param imm Immediate representable as a 12-bit value optionally shifted by 12.
    /// @throws std::runtime_error If `imm` cannot be represented by the instruction.
    void subRegImm(A64Reg dst, A64Reg src, uint32_t imm);

    /// @brief Load a 64-bit value from an unsigned scaled base-register offset.
    /// @param dst Destination register.
    /// @param base Address base register.
    /// @param offset Byte offset, aligned to eight bytes and no greater than 32767.
    /// @throws std::runtime_error If `offset` is not encodable.
    void loadMem64(A64Reg dst, A64Reg base, uint32_t offset);

    /// @brief Store a 64-bit register at an unsigned scaled base-register offset.
    /// @param base Address base register.
    /// @param offset Byte offset, aligned to eight bytes and no greater than 32767.
    /// @param src Source register.
    /// @throws std::runtime_error If `offset` is not encodable.
    void storeMem64(A64Reg base, uint32_t offset, A64Reg src);

    /// @brief Store the low 32 bits of a register at a scaled memory offset.
    /// @param base Address base register.
    /// @param offset Byte offset, aligned to four bytes and no greater than 16383.
    /// @param src Source register.
    /// @throws std::runtime_error If `offset` is not encodable.
    void storeMem32(A64Reg base, uint32_t offset, A64Reg src);

    /// @brief Materialize and store a 32-bit immediate using scratch register X17.
    /// @param base Address base register.
    /// @param offset Byte offset accepted by storeMem32().
    /// @param imm Immediate value to store.
    /// @throws std::runtime_error If `offset` is not encodable.
    void storeMemImm32(A64Reg base, uint32_t offset, uint32_t imm);

    /// @brief Materialize the PE address of generated data with an ADRP/ADD pair.
    /// @param dst Destination register.
    /// @param dataOffset Byte offset within the generated data section.
    void leaData(A64Reg dst, uint32_t dataOffset);

    /// @brief Emit an unconditional branch to a generated label.
    /// @param labelId Target label identifier resolved during finalization.
    void b(uint32_t labelId);

    /// @brief Emit a compare-and-branch-if-zero instruction.
    /// @param reg Register tested for zero.
    /// @param labelId Target label identifier resolved during finalization.
    void cbz(A64Reg reg, uint32_t labelId);

    /// @brief Emit an indirect branch with link through a register.
    /// @param target Register containing the target address.
    void blr(A64Reg target);

    /// @brief Call an imported function through its 64-bit IAT slot.
    /// @param flatIndex Zero-based function index across all DLL import entries.
    void callIATSlot(uint32_t flatIndex);

    /// @brief Transcode UTF-8 text to a null-terminated UTF-16LE data string.
    /// @param asciiStr UTF-8 text to embed.
    /// @return Byte offset of the string within the generated data section.
    uint32_t embedStringW(const std::string &asciiStr);

    /// @brief Append raw bytes to the generated data section.
    /// @param data Address of the source bytes.
    /// @param len Number of bytes to append.
    /// @return Byte offset of the blob within the generated data section.
    uint32_t embedBytes(const void *data, size_t len);

    /// @brief Resolve all branch, IAT, and data-address fixups.
    /// @param textRVA RVA at which the generated code section begins.
    /// @param iatBaseRVA RVA of the first import-address-table entry.
    /// @param imports Ordered imports used to resolve flat IAT indices.
    /// @param dataBaseRVA RVA at which the generated data section begins.
    /// @return Patched copy of the code bytes; the builder remains unchanged.
    /// @throws std::runtime_error If a label or IAT index is invalid, a branch is
    ///         misaligned or out of range, or an ADRP target is out of range.
    std::vector<uint8_t> finishText(uint32_t textRVA,
                                    uint32_t iatBaseRVA,
                                    const std::vector<PEImport> &imports,
                                    uint32_t dataBaseRVA) const;

    /// @brief Access the separately generated data-section bytes.
    /// @return Const reference valid until this builder is destroyed or embeds more data.
    const std::vector<uint8_t> &dataSection() const {
        return data_;
    }

    /// @brief Query the number of instruction bytes emitted so far.
    /// @return Size of the unfinalized code buffer in bytes.
    size_t codeSize() const {
        return code_.size();
    }

  private:
    /// @brief Kinds of deferred A64 instruction fields resolved by finishText().
    enum class FixupKind : uint8_t {
        Branch26,        ///< Signed 26-bit unconditional branch displacement.
        CompareBranch19, ///< Signed 19-bit compare-and-branch displacement.
        IATPage21,       ///< ADRP page displacement for an IAT slot.
        IATLow12,        ///< ADD low-page offset for an IAT slot.
        DataPage21,      ///< ADRP page displacement for generated data.
        DataLow12,       ///< ADD low-page offset for generated data.
    };

    /// @brief Binding state for one generated code label.
    struct LabelInfo {
        uint32_t codeOffset{0}; ///< Byte offset assigned by bindLabel().
        bool bound{false};      ///< Whether `codeOffset` contains a binding.
    };

    /// @brief One deferred instruction patch and its kind-specific target.
    struct Fixup {
        uint32_t codeOffset{0}; ///< Byte offset of the instruction to patch.
        uint32_t target{0};     ///< Label ID, flat IAT index, or data offset.
        FixupKind kind{FixupKind::Branch26}; ///< Interpretation of `target`.
    };

    std::vector<uint8_t> code_;
    std::vector<uint8_t> data_;
    std::vector<LabelInfo> labels_;
    std::vector<Fixup> fixups_;

    /// @brief Append a 32-bit instruction word in little-endian order.
    /// @param word Encoded A64 instruction.
    void emit32(uint32_t word);

    /// @brief Encode ADD/SUB immediate using the common instruction layout.
    /// @param baseOpcode Opcode identifying ADD or SUB.
    /// @param dst Destination register.
    /// @param src Source register.
    /// @param imm Immediate representable as 12 bits, optionally shifted by 12.
    /// @throws std::runtime_error If `imm` is not encodable.
    void emitAddSubImm(uint32_t baseOpcode, A64Reg dst, A64Reg src, uint32_t imm);

    /// @brief Emit an ADRP/ADD pair and record its page and low-offset fixups.
    /// @param dst Register that receives the resolved address.
    /// @param target Kind-specific IAT index or data offset.
    /// @param pageKind Fixup kind for the ADRP instruction.
    /// @param lowKind Fixup kind for the ADD instruction.
    void emitAdrPair(A64Reg dst, uint32_t target, FixupKind pageKind, FixupKind lowKind);

    /// @brief Encode a 64-bit unsigned-offset load or store.
    /// @param baseOpcode Opcode identifying the load/store operation.
    /// @param reg Loaded destination or stored source register.
    /// @param base Address base register.
    /// @param offset Eight-byte-aligned byte offset no greater than 32767.
    /// @throws std::runtime_error If `offset` is not encodable.
    void emitLoadStore64(uint32_t baseOpcode, A64Reg reg, A64Reg base, uint32_t offset);

    /// @brief Encode a 32-bit unsigned-offset load or store.
    /// @param baseOpcode Opcode identifying the load/store operation.
    /// @param reg Loaded destination or stored source register.
    /// @param base Address base register.
    /// @param offset Four-byte-aligned byte offset no greater than 16383.
    /// @throws std::runtime_error If `offset` is not encodable.
    void emitLoadStore32(uint32_t baseOpcode, A64Reg reg, A64Reg base, uint32_t offset);

    /// @brief Extract the five-bit A64 register encoding.
    /// @param reg Register to encode.
    /// @return Low five bits of the register identifier.
    uint8_t regBits(A64Reg reg) const {
        return static_cast<uint8_t>(reg) & 31u;
    }
};

} // namespace zanna::pkg
