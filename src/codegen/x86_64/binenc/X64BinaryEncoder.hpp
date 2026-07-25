//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/binenc/X64BinaryEncoder.hpp
// Purpose: Public interface for the x86_64 MIR-to-machine-code binary encoder.
//          Encodes MFunction basic blocks into raw bytes in a CodeSection,
//          resolving internal branches via patching and generating Relocations
//          for external symbols and cross-section references.
// Key invariants:
//   - All internal branches use rel32 (4-byte offsets), enabling single-pass
//     encoding with deferred patching
//   - External calls and RIP-relative data refs generate Relocation entries
//     with addend = -4 (standard x86_64 PC-relative convention)
//   - Pseudo-instructions (DIVS64rr, etc.) must be expanded before encoding
// Ownership/Lifetime:
//   - Encoder is stateless between encodeFunction() calls
//   - CodeSection is borrowed (caller retains ownership)
// Links: src/codegen/x86_64/binenc/X64BinaryEncoder.cpp,
//        src/codegen/x86_64/binenc/X64Encoding.hpp,
//        src/codegen/common/objfile/CodeSection.hpp,
//        src/codegen/x86_64/MachineIR.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/common/objfile/CodeSection.hpp"
#include "codegen/x86_64/FrameLowering.hpp"
#include "codegen/x86_64/MachineIR.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

/// @file
/// @brief Declares the stateful per-function x86-64 MIR binary encoder.
///
/// @details The encoder converts allocated, legalized machine IR into bytes,
///          symbols, relocations, debug-line entries, and optional Win64 unwind
///          records. It predicts instruction sizes before emission so local
///          labels and relaxed branch forms remain internally consistent.

namespace zanna::codegen {
class DebugLineTable;
}

namespace zanna::codegen::x64::binenc {

using zanna::codegen::DebugLineTable;

/// @brief Encodes x86-64 MIR functions into machine-code bytes.
/// @details The encoder processes one function at a time. Internal branches are
///          resolved from predicted label offsets or patched after all blocks
///          are emitted. External symbols generate relocation entries in the
///          destination code section. Per-function scratch state is reset by
///          @ref encodeFunction; an attached debug-line table remains in effect
///          until replaced.
/// @invariant Input MIR contains only physical registers and encodable opcodes.
/// @invariant Predicted label offsets and function size must match emitted bytes.
class X64BinaryEncoder {
  public:
    /// @brief Sets the optional sink for emitted-address source mappings.
    /// @param table Borrowed debug-line table, or @c nullptr to disable recording.
    /// @note The caller must keep a non-null @p table alive through every
    ///       subsequent @ref encodeFunction call that uses it.
    void setDebugLineTable(DebugLineTable *table) {
        debugLines_ = table;
    }

    /// @brief Encodes one complete MIR function into the text section.
    /// @details Resets per-function patch state, computes stable label offsets,
    ///          defines the global function symbol, appends instruction bytes,
    ///          resolves local branches and jump tables, and optionally records
    ///          Win64 unwind metadata. Existing bytes in @p text are preserved.
    /// @param fn Allocated and legalized MIR function to encode.
    /// @param text Mutable @c .text section receiving bytes, symbols, and relocations.
    /// @param rodata Read-only section consulted for RIP-relative symbol offsets.
    /// @param isDarwin Target-format compatibility flag forwarded to symbol-reference emitters.
    /// @param frame Optional frame plan used to match and serialize Win64 prologue operations.
    /// @param emitWin64Unwind Whether to disable short-branch relaxation and emit
    ///        PE/COFF unwind metadata when @p frame is available.
    /// @throws std::runtime_error If MIR is malformed or unencodable, labels
    ///         cannot be resolved, layout prediction drifts, or unwind metadata
    ///         cannot represent the emitted prologue.
    void encodeFunction(const MFunction &fn,
                        objfile::CodeSection &text,
                        const objfile::CodeSection &rodata,
                        bool isDarwin,
                        const FrameInfo *frame = nullptr,
                        bool emitWin64Unwind = false);

  private:
    /// @brief Maps an MIR label spelling to its absolute or relative byte offset.
    using LabelOffsetMap = std::unordered_map<std::string, size_t>;

    /// @brief Result of the pre-emission fixed-point layout calculation.
    struct LabelLayout {
        /// @brief Function-relative offset of each basic-block and explicit label.
        LabelOffsetMap offsets;
        /// @brief Predicted number of machine-code bytes in the function.
        size_t estimatedSize{0};
    };

    /// @brief Encodes one instruction and normalizes operand-variant failures.
    /// @param instr MIR instruction to append.
    /// @param text Mutable machine-code destination.
    /// @param rodata Read-only section used to resolve local data symbols.
    /// @param isDarwin Target-format compatibility flag.
    /// @throws std::runtime_error If the opcode or operands cannot be encoded.
    void encodeInstruction(const MInstr &instr,
                           objfile::CodeSection &text,
                           const objfile::CodeSection &rodata,
                           bool isDarwin);

    // === Instruction encoding by category ===

    /// @brief Dispatches an instruction to its opcode-specific encoder.
    /// @details Validates operand counts and shapes before invoking the
    ///          category helpers or directly appending simple instruction forms.
    /// @param instr MIR instruction to encode.
    /// @param text Mutable machine-code destination.
    /// @param rodata Read-only section used for RIP-relative references.
    /// @param isDarwin Target-format compatibility flag.
    /// @throws std::runtime_error If an opcode is illegal at this stage or its
    ///         operands violate the expected encoding shape.
    void encodeInstructionImpl(const MInstr &instr,
                               objfile::CodeSection &text,
                               const objfile::CodeSection &rodata,
                               bool isDarwin);

    /// @brief Encodes a supported operand-free instruction.
    /// @param op One of @c RET, @c CQO, or @c UD2.
    /// @param cs Destination section receiving instruction bytes.
    /// @throws std::runtime_error If @p op is not a supported nullary opcode.
    void encodeNullary(MOpcode op, objfile::CodeSection &cs);

    /// @brief Encodes a register-to-register integer instruction.
    /// @param op Register-register opcode described by the encoding table.
    /// @param dst Physical destination register.
    /// @param src Physical source register.
    /// @param cs Destination section receiving prefixes, opcode, and ModR/M byte.
    /// @throws std::runtime_error If @p op has no register-register encoding.
    void encodeRegReg(MOpcode op, PhysReg dst, PhysReg src, objfile::CodeSection &cs);

    /// @brief Encodes a register-immediate integer ALU instruction.
    /// @details Chooses the sign-extended imm8 form when possible, otherwise
    ///          emits an imm16 or imm32 form according to @p prefix66.
    /// @param op Register-immediate opcode described by the encoding table.
    /// @param dst Physical destination register.
    /// @param imm Signed immediate value to encode.
    /// @param cs Destination section receiving instruction bytes.
    /// @param rexW Whether to request 64-bit operand size through @c REX.W.
    /// @param prefix66 Whether to request the 16-bit operand-size override.
    /// @throws std::runtime_error If the immediate exceeds the selected form or
    ///         @p op has no register-immediate extension.
    void encodeRegImm(MOpcode op,
                      PhysReg dst,
                      int64_t imm,
                      objfile::CodeSection &cs,
                      bool rexW = true,
                      bool prefix66 = false);

    /// @brief Encodes a 64-bit GPR shift with an immediate count.
    /// @param op Supported shift or rotate opcode.
    /// @param dst Physical GPR modified in place.
    /// @param count Shift count in the inclusive range 0 through 63.
    /// @param cs Destination section receiving instruction bytes.
    /// @throws std::runtime_error If the register, count, or opcode is invalid.
    void encodeShiftImm(MOpcode op, PhysReg dst, int64_t count, objfile::CodeSection &cs);

    /// @brief Encodes a 64-bit GPR shift whose count is supplied in @c CL.
    /// @param op Supported shift or rotate opcode.
    /// @param dst Physical GPR modified in place.
    /// @param cs Destination section receiving instruction bytes.
    /// @throws std::runtime_error If @p dst or @p op is invalid.
    void encodeShiftCL(MOpcode op, PhysReg dst, objfile::CodeSection &cs);

    /// @brief Encodes a unary @c F7-group integer arithmetic instruction.
    /// @details Selects the extension for signed divide, unsigned divide,
    ///          one-operand multiply, or the remaining supported unary form.
    /// @param op Unary arithmetic opcode.
    /// @param src Physical GPR source operand.
    /// @param cs Destination section receiving instruction bytes.
    /// @throws std::runtime_error If @p src is not a GPR.
    void encodeDiv(MOpcode op, PhysReg src, objfile::CodeSection &cs);

    /// @brief Encodes a 64-bit immediate move into a physical GPR.
    /// @details Chooses a zero-extending imm32, sign-extending imm32, or full
    ///          imm64 representation according to the value.
    /// @param dst Physical destination GPR.
    /// @param imm Signed bit pattern to materialize.
    /// @param cs Destination section receiving instruction bytes.
    /// @throws std::runtime_error If @p dst is not a GPR.
    void encodeMovRI(PhysReg dst, int64_t imm, objfile::CodeSection &cs);

    /// @brief Encodes a GPR memory load, store, or memory-source ALU form.
    /// @param op Supported memory GPR opcode.
    /// @param reg Physical register occupying the ModR/M register field.
    /// @param mem Base/index/scale/displacement address.
    /// @param cs Destination section receiving instruction bytes.
    /// @throws std::runtime_error If the opcode, register, or address is invalid.
    void encodeMemOp(MOpcode op, PhysReg reg, const OpMem &mem, objfile::CodeSection &cs);

    /// @brief Encodes @c LEA with a conventional memory operand.
    /// @param dst Physical GPR destination.
    /// @param mem Effective-address components.
    /// @param cs Destination section receiving instruction bytes.
    /// @throws std::runtime_error If @p dst or @p mem is not encodable.
    void encodeLEA(PhysReg dst, const OpMem &mem, objfile::CodeSection &cs);

    /// @brief Encodes @c LEA with a relocatable RIP-relative label operand.
    /// @param dst Physical GPR destination.
    /// @param rip Referenced symbol name.
    /// @param text Mutable text section receiving bytes and relocation.
    /// @param rodata Read-only section checked for an already-defined symbol.
    /// @param isDarwin Target-format compatibility flag.
    /// @throws std::runtime_error If @p dst is not a GPR.
    void encodeLEARip(PhysReg dst,
                      const OpRipLabel &rip,
                      objfile::CodeSection &text,
                      const objfile::CodeSection &rodata,
                      bool isDarwin);

    /// @brief Encodes an SSE register-register or GPR/XMM conversion form.
    /// @param op SSE opcode described by the encoding table.
    /// @param dst Physical destination register of the opcode-required class.
    /// @param src Physical source register of the opcode-required class.
    /// @param cs Destination section receiving instruction bytes.
    /// @throws std::runtime_error If register classes or @p op are invalid.
    void encodeSseRegReg(MOpcode op, PhysReg dst, PhysReg src, objfile::CodeSection &cs);

    /// @brief Encodes an SSE load or store using a conventional memory operand.
    /// @param op Supported SSE memory opcode.
    /// @param reg Physical XMM register.
    /// @param mem Base/index/scale/displacement address.
    /// @param cs Destination section receiving instruction bytes.
    /// @throws std::runtime_error If @p reg, @p mem, or @p op is invalid.
    void encodeSseMem(MOpcode op, PhysReg reg, const OpMem &mem, objfile::CodeSection &cs);

    /// @brief Encodes a scalar-double load from a relocatable RIP-relative label.
    /// @param dst Physical XMM destination.
    /// @param rip Referenced symbol name.
    /// @param text Mutable text section receiving bytes and relocation.
    /// @param rodata Read-only section checked for an already-defined symbol.
    /// @param isDarwin Target-format compatibility flag.
    /// @throws std::runtime_error If @p dst is not an XMM register.
    void encodeSseRipLoad(PhysReg dst,
                          const OpRipLabel &rip,
                          objfile::CodeSection &text,
                          const objfile::CodeSection &rodata,
                          bool isDarwin);

    /// @brief Encodes @c SETcc to the low byte of a physical GPR.
    /// @param condCode Backend condition-code value mapped by @c x86CC.
    /// @param dst Physical GPR destination.
    /// @param cs Destination section receiving instruction bytes.
    /// @throws std::runtime_error If @p dst or @p condCode is invalid.
    void encodeSETcc(int condCode, PhysReg dst, objfile::CodeSection &cs);

    /// @brief Encodes an unsigned byte-to-quadword register extension.
    /// @param dst Physical destination GPR.
    /// @param src Physical GPR supplying its low byte.
    /// @param cs Destination section receiving instruction bytes.
    /// @throws std::runtime_error If either register is not a GPR.
    void encodeMOVZX(PhysReg dst, PhysReg src, objfile::CodeSection &cs);

    /// @brief Encodes a direct jump, conditional jump, or call to a local label.
    /// @details Uses a rel8 branch when relaxation is enabled and the target is
    ///          known to fit; otherwise emits rel32 and patches now or later.
    /// @param op One of @c JMP, @c JCC, or @c CALL.
    /// @param label Non-empty internal target label.
    /// @param condCode Condition code used only for @c JCC.
    /// @param cs Destination section receiving instruction bytes.
    /// @throws std::runtime_error If the label, opcode, or displacement is invalid.
    void encodeBranchLabel(MOpcode op,
                           const std::string &label,
                           int condCode,
                           objfile::CodeSection &cs);

    /// @brief Encodes an indirect jump or call through a physical GPR.
    /// @param op @c CALL selects the /2 form; other accepted callers use /4 jump.
    /// @param target Physical GPR holding the target address.
    /// @param cs Destination section receiving instruction bytes.
    /// @throws std::runtime_error If @p target is not a GPR.
    void encodeBranchReg(MOpcode op, PhysReg target, objfile::CodeSection &cs);

    /// @brief Encodes an indirect jump or call through a memory operand.
    /// @param op @c CALL selects the /2 form; other accepted callers use /4 jump.
    /// @param mem Address containing the target pointer.
    /// @param cs Destination section receiving instruction bytes.
    /// @throws std::runtime_error If @p mem is not encodable.
    void encodeBranchMem(MOpcode op, const OpMem &mem, objfile::CodeSection &cs);

    /// @brief Encodes an indirect jump or call through RIP-relative memory.
    /// @param op Either @c CALL or @c JMP.
    /// @param rip Referenced pointer symbol.
    /// @param text Mutable text section receiving bytes and relocation.
    /// @param rodata Read-only section checked for an already-defined symbol.
    /// @param isDarwin Target-format compatibility flag.
    /// @throws std::runtime_error If @p op is not @c CALL or @c JMP.
    void encodeBranchRip(MOpcode op,
                         const OpRipLabel &rip,
                         objfile::CodeSection &text,
                         const objfile::CodeSection &rodata,
                         bool isDarwin);

    /// @brief Dispatch a branch/call target by operand kind.
    /// @details Selects between @ref encodeBranchLabel, @ref encodeBranchReg,
    ///          and @ref encodeBranchMem based on @p target. Throws with the
    ///          supplied @p errPrefix when the operand kind is unsupported.
    /// @param op Direct or indirect branch opcode.
    /// @param target Label, physical GPR, or conventional memory target.
    /// @param condCode Condition code forwarded for label-target conditional branches.
    /// @param cs Destination section receiving instruction bytes.
    /// @param errPrefix Diagnostic suffix for an unsupported target variant.
    /// @throws std::runtime_error If the target kind or contained register is invalid.
    void encodeBranchOperand(MOpcode op,
                             const Operand &target,
                             int condCode,
                             objfile::CodeSection &cs,
                             const char *errPrefix);

    /// @brief Encodes a direct external call and records a linker relocation.
    /// @details Canonical runtime names are mapped to their C ABI spellings
    ///          before symbol declaration. The emitted displacement is zero and
    ///          receives a @c Branch32 relocation with addend -4.
    /// @param name IL or native symbol name to call.
    /// @param cs Destination section receiving bytes, symbol, and relocation.
    /// @param isDarwin Target-format compatibility flag; names remain unmangled.
    void encodeCallExternal(const std::string &name, objfile::CodeSection &cs, bool isDarwin);

    /// @brief Measures an instruction using a scratch encoder and code section.
    /// @param instr Instruction whose encoded size is required.
    /// @param currentOffset Logical start offset used for PC-relative decisions.
    /// @param knownLabelOffsets Currently predicted target offsets.
    /// @param isDarwin Target-format compatibility flag.
    /// @return Number of bytes the instruction would append.
    /// @throws std::runtime_error If the instruction cannot be encoded.
    size_t measureInstructionSize(const MInstr &instr,
                                  size_t currentOffset,
                                  const LabelOffsetMap &knownLabelOffsets,
                                  bool isDarwin);

    /// @brief Computes stable intra-function label offsets and final size.
    /// @details Iterates instruction measurement until branch relaxation reaches
    ///          a fixed point, or performs one conservative pass when disabled.
    /// @param fn Function whose layout is required.
    /// @param isDarwin Target-format compatibility flag.
    /// @return Relative label offsets and predicted encoded size.
    /// @throws std::runtime_error On duplicate labels, invalid instructions,
    ///         arithmetic overflow, or failure to converge.
    LabelLayout computeFunctionLabelLayout(const MFunction &fn, bool isDarwin);

    /// @brief Verifies a label's predicted and emitted offsets agree.
    /// @param label Label whose prediction should be checked.
    /// @param actualOffset Absolute offset observed during emission.
    /// @throws std::runtime_error If a stored prediction differs from @p actualOffset.
    void verifyPredictedLabelOffset(const std::string &label, size_t actualOffset) const;

    // === Low-level emission helpers ===

    /// Emit the ModR/M + optional SIB + displacement for a memory operand.
    /// @details Selects displacement width, introduces a SIB byte for indexed
    ///          addresses and RSP/R12 bases, and emits prefixes in architectural order.
    /// @param reg3 Three-bit ModR/M register field or opcode extension.
    /// @param regRex REX extension bit corresponding to @p reg3.
    /// @param mem Validated memory operand.
    /// @param cs Destination section receiving instruction bytes.
    /// @param rexW Whether @c REX.W is required.
    /// @param mandatoryPrefix SSE prefix byte, or zero for none.
    /// @param opByte1 First opcode byte.
    /// @param opByte2 Second opcode byte, or zero for a one-byte opcode.
    /// @throws std::runtime_error If @p mem contains an invalid register or scale.
    void emitWithMemOperand(uint8_t reg3,
                            uint8_t regRex,
                            const OpMem &mem,
                            objfile::CodeSection &cs,
                            bool rexW,
                            uint8_t mandatoryPrefix,
                            uint8_t opByte1,
                            uint8_t opByte2);

    // === Internal branch resolution ===

    /// @brief A rel32 branch awaiting resolution after block emission.
    struct PendingBranch {
        /// @brief Absolute section offset of the rel32 placeholder.
        size_t patchOffset = 0; ///< Offset in CodeSection of the rel32 placeholder.
        /// @brief Non-empty internal target label.
        std::string target;     ///< Target label name.
    };

    /// @brief Record of a jump-table entry word awaiting label resolution.
    /// @details Each entry is patched to offset(case) - tableStart once all
    ///          labels of the function are known.
    struct PendingTableEntry {
        size_t patchOffset = 0; ///< Offset in CodeSection of the 4-byte entry.
        std::string caseLabel;  ///< Case target label name.
        size_t tableStart = 0;  ///< Byte offset of the table's first entry.
    };

    /// @brief Absolute emitted offset of every currently known function label.
    LabelOffsetMap labelOffsets_;

    /// @brief Near-form branches whose target was unknown when bytes were emitted.
    std::vector<PendingBranch> pendingBranches_;

    /// @brief Relative jump-table words whose case labels require later resolution.
    std::vector<PendingTableEntry> pendingTableEntries_;

    /// @brief Whether the current function may use rel8 @c JMP and @c JCC forms.
    bool shortBranchRelaxationEnabled_ = true;

    /// @brief Borrowed optional sink for address-to-source mappings.
    DebugLineTable *debugLines_{nullptr};
};

} // namespace zanna::codegen::x64::binenc
