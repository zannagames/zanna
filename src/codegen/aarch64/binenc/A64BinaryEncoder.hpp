//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/binenc/A64BinaryEncoder.hpp
// Purpose: Public interface for the AArch64 MIR-to-machine-code binary encoder.
//          Encodes MFunction basic blocks into raw 32-bit instruction words in a
//          CodeSection, synthesizing prologue/epilogue at emission time and
//          resolving internal branches via patching.
// Key invariants:
//   - Every instruction emitted is exactly 4 bytes (32 bits)
//   - Prologue/epilogue are synthesized from MFunction metadata (not MIR instrs)
//   - External calls generate A64Call26 relocations; ADRP/ADD generate page relocs
//   - Pseudo-instructions (overflow-checked ops) must be expanded before encoding
//   - Symbol mangling is NOT done here; deferred to ObjectFileWriter
// Ownership/Lifetime:
//   - Per-function fixup state is reset by encodeFunction(); the optional
//     debug-line sink remains configured across calls
//   - CodeSection is borrowed (caller retains ownership)
// Links: src/codegen/aarch64/binenc/A64BinaryEncoder.cpp,
//        src/codegen/aarch64/binenc/A64Encoding.hpp,
//        src/codegen/common/objfile/CodeSection.hpp,
//        src/codegen/aarch64/MachineIR.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/aarch64/MachineIR.hpp"
#include "codegen/aarch64/TargetAArch64.hpp"
#include "codegen/common/objfile/CodeSection.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/**
 * @file
 * @brief Declares direct AArch64 MIR-to-object machine-code encoding.
 *
 * The encoder measures each function to stabilize label offsets, emits
 * synthesized frame sequences and instruction words, records relocations and
 * unwind/debug metadata, then patches internal branches and jump tables.
 */

namespace zanna::codegen {
class DebugLineTable;
}

namespace zanna::codegen::aarch64::binenc {

using zanna::codegen::DebugLineTable;

/**
 * @brief Encodes allocated AArch64 MIR functions directly into object sections.
 *
 * Prologues and epilogues are synthesized from `MFunction` metadata rather
 * than represented in MIR. Internal labels use predicted offsets plus final
 * patching; external symbols and cross-section addresses create relocations.
 *
 * @invariant `currentFn_` and `currentRodata_` are non-null only during an
 *            active `encodeFunction()` call.
 * @invariant Pending fixup collections refer only to the active function.
 * @warning An encoder instance is not safe for concurrent calls.
 */
class A64BinaryEncoder {
  public:
    friend struct A64BinaryEncoderTestAccess;

    /**
     * @brief Configures optional address-to-source line recording.
     * @param table Non-owning table pointer, or `nullptr` to disable recording.
     * @warning A non-null table must outlive all subsequent encoding calls that use it.
     */
    void setDebugLineTable(DebugLineTable *table) {
        debugLines_ = table;
    }

    /**
     * @brief Encodes one complete MIR function into an object text section.
     *
     * The function validates frame/register metadata, predicts stable block
     * offsets and long branches, defines the public symbol, emits code, patches
     * internal control flow, and records ABI-specific unwind metadata.
     *
     * @param fn Allocated MIR function borrowed for the duration of encoding.
     * @param[in,out] text Text section receiving bytes, symbols, relocations,
     *        unwind records, and patches.
     * @param rodata Read-only data section used to resolve same-object
     *        cross-section address fixups.
     * @param abi Object/assembly ABI controlling hardening and unwind metadata.
     * @throws std::exception If metadata, operand shapes, encodings, branch
     *         ranges, labels, or predicted sizes are invalid.
     * @post Transient function and rodata pointers are cleared on success or failure.
     */
    void encodeFunction(const MFunction &fn,
                        objfile::CodeSection &text,
                        const objfile::CodeSection &rodata,
                        ABIFormat abi);

  private:
    using LabelOffsetMap = std::unordered_map<std::string, size_t>;

    /**
     * @brief Validates and dispatches one MIR instruction to its encoding family.
     * @param mi Physical-register MIR instruction to encode.
     * @param[in,out] cs Text section receiving instruction words or relocations.
     * @throws std::exception If the opcode or operands cannot be encoded.
     */
    void encodeInstruction(const MInstr &mi, objfile::CodeSection &cs);

    // === Per-family encoders (called from encodeInstruction switch) ===
    /// @brief Encodes add/subtract immediate instructions, including smart large immediates.
    /// @param mi Validated family opcode and operands.
    /// @param[in,out] cs Text section receiving encoded words.
    void encodeAddSubImmInstr(const MInstr &mi, objfile::CodeSection &cs);
    /// @brief Encodes integer/FP register and immediate move instructions.
    /// @param mi Validated family opcode and operands.
    /// @param[in,out] cs Text section receiving encoded words.
    void encodeMoveInstr(const MInstr &mi, objfile::CodeSection &cs);
    /// @brief Encodes immediate shift aliases after validating the shift count.
    /// @param mi Validated family opcode and operands.
    /// @param[in,out] cs Text section receiving encoded words.
    void encodeShiftImmInstr(const MInstr &mi, objfile::CodeSection &cs);
    /// @brief Encodes integer/FP compares and `TST` aliases.
    /// @param mi Validated family opcode and operands.
    /// @param[in,out] cs Text section receiving encoded words.
    void encodeCompareInstr(const MInstr &mi, objfile::CodeSection &cs);
    /// @brief Encodes condition-producing select/set instructions.
    /// @param mi Validated family opcode and operands.
    /// @param[in,out] cs Text section receiving encoded words.
    void encodeConditionalInstr(const MInstr &mi, objfile::CodeSection &cs);
    /// @brief Encodes frame-pointer-relative scalar loads and stores.
    /// @param mi Validated family opcode and operands.
    /// @param[in,out] cs Text section receiving encoded words.
    void encodeFpRelLdStInstr(const MInstr &mi, objfile::CodeSection &cs);
    /// @brief Encodes arbitrary-base scalar immediate loads and stores.
    /// @param mi Validated family opcode and operands.
    /// @param[in,out] cs Text section receiving encoded words.
    void encodeBaseRelLdStInstr(const MInstr &mi, objfile::CodeSection &cs);
    /// @brief Encodes scaled register-offset loads and stores.
    /// @param mi Validated family opcode and operands.
    /// @param[in,out] cs Text section receiving encoded words.
    void encodeRegOffsetLdStInstr(const MInstr &mi, objfile::CodeSection &cs);
    /// @brief Encodes frame-pointer-relative load/store pairs.
    /// @param mi Validated family opcode and operands.
    /// @param[in,out] cs Text section receiving encoded words.
    void encodeLdStPairInstr(const MInstr &mi, objfile::CodeSection &cs);
    /// @brief Encodes stack-pointer adjustment and outgoing-argument store pseudos.
    /// @param mi Validated family opcode and operands.
    /// @param[in,out] cs Text section receiving encoded words.
    void encodeSpOpInstr(const MInstr &mi, objfile::CodeSection &cs);
    /// @brief Encodes frame-pointer address formation with an immediate offset.
    /// @param mi Validated family opcode and operands.
    /// @param[in,out] cs Text section receiving encoded words.
    void encodeAddFpImmInstr(const MInstr &mi, objfile::CodeSection &cs);
    /// @brief Encodes logical-immediate operations after bitmask packing.
    /// @param mi Validated family opcode and operands.
    /// @param[in,out] cs Text section receiving encoded words.
    void encodeLogicalImmInstr(const MInstr &mi, objfile::CodeSection &cs);
    /// @brief Encodes FP moves, conversions, rounding, and comparisons.
    /// @param mi Validated family opcode and operands.
    /// @param[in,out] cs Text section receiving encoded words.
    void encodeFpSpecialInstr(const MInstr &mi, objfile::CodeSection &cs);
    /// @brief Encodes direct, indirect, conditional, and jump-table control flow.
    /// @param mi Validated family opcode and operands.
    /// @param[in,out] cs Text section receiving encoded words and fixups.
    void encodeBranchInstr(const MInstr &mi, objfile::CodeSection &cs);
    /// @brief Encodes page-based global address materialization and relocations.
    /// @param mi Validated family opcode and operands.
    /// @param[in,out] cs Text section receiving encoded words and relocations.
    void encodeAddressInstr(const MInstr &mi, objfile::CodeSection &cs);

    /**
     * @brief Measures BTI and optional synthesized prologue bytes before the first block.
     * @param fn Function providing frame metadata.
     * @return Prelude size under the active ABI and `skipFrame_` decision.
     */
    size_t measurePreludeSize(const MFunction &fn);

    /// @brief Measure the encoded byte size of one MIR instruction at a specific byte offset.
    /// @details Used during the size-estimation pass that runs before final emission.
    ///          Conditional branches may need either a 4-byte short form or a 12-byte
    ///          long form depending on the displacement to their target, so the size
    ///          can depend on the offsets of labels resolved during this same pass.
    /// @param mi The MIR instruction to measure.
    /// @param currentOffset Byte offset of @p mi within the function being measured.
    /// @param knownLabelOffsets Map of already-resolved label offsets within the function.
    /// @param instructionOrdinal Sequential index of @p mi within the function.
    /// @param assumedLongConditionalBranches Set of ordinals that the current iteration
    ///        is committed to emitting in their long form (input).
    /// @param discoveredLongConditionalBranches Optional output: when non-null, ordinals
    ///        whose displacement no longer fits the short form are inserted so the
    ///        next iteration can re-measure with them promoted.
    /// @return Encoded byte size of @p mi assuming the current short/long branch choices.
    size_t measureInstructionSize(const MInstr &mi,
                                  size_t currentOffset,
                                  const LabelOffsetMap &knownLabelOffsets,
                                  size_t instructionOrdinal,
                                  const std::unordered_set<size_t> &assumedLongConditionalBranches,
                                  std::unordered_set<size_t> *discoveredLongConditionalBranches);

    /// @brief Measures the synthesized frame prologue for @p fn under the active ABI.
    /// @return Encoded size in bytes.
    size_t prologueSize(const MFunction &fn) const;
    /// @brief Measures the synthesized frame epilogue for @p fn under the active ABI.
    /// @return Encoded size in bytes.
    size_t epilogueSize(const MFunction &fn) const;

    /// @brief Measures the minimal move-wide sequence for @p imm.
    /// @return Encoded size in bytes.
    size_t movImm64Size(uint64_t imm) const;
    /// @brief Measures smart add/sub immediate chunking for @p value.
    /// @return Encoded size in bytes.
    size_t addSubImmSmartSize(uint32_t value) const;
    /// @brief Measures an SP-relative argument store.
    /// @param offset Signed stack-pointer-relative byte offset.
    /// @return Encoded size in bytes.
    /// @throws std::runtime_error when the offset needed an expansion.
    size_t spOffsetStoreSize(int64_t offset) const;

    /**
     * @brief Computes stable relative block offsets and long-conditional choices.
     * @param fn Function to measure iteratively.
     * @return Sanitized label-to-function-relative-byte-offset map.
     * @throws std::exception If sizes overflow or labels/operands are invalid.
     */
    LabelOffsetMap computeFunctionLabelOffsets(const MFunction &fn);

    /**
     * @brief Estimates full function size for a fixed label-offset hypothesis.
     * @param fn Function to measure.
     * @param knownLabelOffsets Current relative label offsets.
     * @return Predicted encoded byte count.
     */
    size_t estimateFunctionSize(const MFunction &fn, const LabelOffsetMap &knownLabelOffsets);

    /**
     * @brief Verifies a predicted label offset when the label was precomputed.
     * @param label Sanitized label name.
     * @param actualOffset Actual text-section byte offset.
     * @throws std::runtime_error If a recorded prediction differs.
     */
    void verifyPredictedLabelOffset(const std::string &label, size_t actualOffset) const;

    // === Prologue/epilogue synthesis ===

    /**
     * @brief Emits FP/LR setup, local allocation, and callee-saved register saves.
     * @param fn Validated function frame metadata.
     * @param[in,out] cs Text section receiving prologue words.
     */
    void encodePrologue(const MFunction &fn, objfile::CodeSection &cs);

    /**
     * @brief Emits callee restores, frame teardown, authentication, and return.
     * @param fn Validated function frame metadata.
     * @param[in,out] cs Text section receiving epilogue words.
     */
    void encodeEpilogue(const MFunction &fn, objfile::CodeSection &cs);

    /**
     * @brief Records Windows ARM64 `.pdata/.xdata` metadata for the emitted prologue.
     * @param fn Function whose frame operations are described.
     * @param funcSymIdx Text symbol-table index.
     * @param functionLength Encoded function size in bytes.
     * @param[in,out] cs Text section receiving unwind records.
     */
    void recordWindowsArm64UnwindEntry(const MFunction &fn,
                                       uint32_t funcSymIdx,
                                       uint32_t functionLength,
                                       objfile::CodeSection &cs) const;

    // === Multi-instruction sequences ===

    /**
     * @brief Emits a minimal `MOVZ`/`MOVN` plus `MOVK` sequence.
     * @param rd Destination GPR hardware field.
     * @param imm Exact 64-bit bit pattern.
     * @param[in,out] cs Text section receiving words.
     */
    void encodeMovImm64(uint32_t rd, uint64_t imm, objfile::CodeSection &cs);

    /**
     * @brief Emits one or more instructions subtracting bytes from `SP`.
     * @param bytes Non-negative byte count.
     * @param[in,out] cs Text section receiving words.
     */
    void encodeSubSp(int64_t bytes, objfile::CodeSection &cs);

    /**
     * @brief Emits one or more instructions adding bytes to `SP`.
     * @param bytes Non-negative byte count.
     * @param[in,out] cs Text section receiving words.
     */
    void encodeAddSp(int64_t bytes, objfile::CodeSection &cs);

    /**
     * @brief Emits one scalar access using the scaled or unscaled immediate form.
     * @param rt Transfer register hardware field.
     * @param base Base GPR hardware field.
     * @param offset Signed byte offset.
     * @param isLoad Selects load when true, store when false.
     * @param fprOperand Selects an FP scalar transfer register.
     * @param accessBytes Access width in bytes.
     * @param[in,out] cs Text section receiving words.
     */
    void encodeScalarLdSt(uint32_t rt,
                          uint32_t base,
                          int64_t offset,
                          bool isLoad,
                          bool fprOperand,
                          unsigned accessBytes,
                          objfile::CodeSection &cs);

    /// @brief Emit an outgoing-stack-arg store using SP-relative addressing.
    /// @details Emits a single `STR Rt, [SP, #imm]`; offsets outside the scaled
    ///          unsigned form were expanded before emission and are rejected.
    /// @param rt     5-bit transfer register hardware encoding.
    /// @param offset Byte offset from `SP` of the destination slot.
    /// @param fprOperand True if @p rt names an FPR; false for GPR.
    /// @param cs     Code section to append the emitted bytes to.
    void encodeSpOffsetStore(uint32_t rt,
                             int64_t offset,
                             bool fprOperand,
                             objfile::CodeSection &cs);

    /**
     * @brief Appends one instruction word in little-endian byte order.
     * @param word Fully encoded AArch64 instruction.
     * @param[in,out] cs Text section to extend by four bytes.
     */
    void emit32(uint32_t word, objfile::CodeSection &cs) {
        cs.emit32LE(word);
    }

    // === Internal branch resolution ===

    /// @brief Record of a branch instruction that targets an as-yet-unseen label.
    /// @details After all instructions of the function are emitted, pendingBranches_ is
    ///          iterated and each entry is patched with the now-known target offset.
    struct PendingBranch {
        size_t offset = 0;          ///< Byte offset in CodeSection of the instruction.
        std::string target;         ///< Target label name.
        MOpcode kind = MOpcode::Br; ///< Branch type (Br/BCond/Cbz/Cbnz/Bl).
    };

    /// Label name → byte offset in CodeSection.
    LabelOffsetMap labelOffsets_;

    /// Forward references needing patching.
    std::vector<PendingBranch> pendingBranches_;

    /// @brief Record of a jump-table entry word awaiting label resolution.
    /// @details Patched to offset(case) - tableStart once every label of the
    ///          function is known.
    struct PendingTableEntry {
        size_t patchOffset = 0; ///< Byte offset of the 4-byte entry word.
        std::string caseLabel;  ///< Case target label name.
        size_t tableStart = 0;  ///< Byte offset of the table's first entry.
    };

    /// Jump-table entry words needing patching.
    std::vector<PendingTableEntry> pendingTableEntries_;

    /// Whether this function's prologue uses a FramePlan (has callee-saved or locals).
    bool usePlan_{false};

    /// Whether prologue/epilogue should be skipped (leaf function optimization).
    bool skipFrame_{false};

    /// Conditional/cbz/cbnz MIR instruction ordinals that must use the long
    /// two-instruction sequence for final emission.
    std::unordered_set<size_t> longConditionalBranchOrdinals_;

    /// MIR instruction ordinal currently being encoded.
    size_t currentInstructionOrdinal_{0};

    /// Pointer to current function being encoded (for epilogue synthesis on Ret).
    const MFunction *currentFn_{nullptr};

    /// ABI currently being encoded, used to gate platform-specific hardening.
    ABIFormat currentAbi_{ABIFormat::Darwin};

    /// Pointer to the current rodata section for same-object cross-section fixups.
    const objfile::CodeSection *currentRodata_{nullptr};

    /// Optional debug line table for recording address→line mappings.
    DebugLineTable *debugLines_{nullptr};
    /// Cached byte-size estimate from the most recent computeFunctionLabelOffsets() call;
    /// used by verifyPredictedLabelOffset() to detect mismatches in debug builds.
    size_t lastEstimatedFunctionSize_{0};
};

} // namespace zanna::codegen::aarch64::binenc
