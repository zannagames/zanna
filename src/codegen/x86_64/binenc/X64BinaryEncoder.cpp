//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/binenc/X64BinaryEncoder.cpp
// Purpose: Encode x86_64 MIR instructions into raw machine code bytes.
//          Implements the full encoding for all 49 instruction forms in the
//          EncodingTable, handling REX prefix computation, ModR/M + SIB
//          generation, and relocation emission for external symbols.
// Key invariants:
//   - REX prefix is emitted only when needed (W/R/X/B set)
//   - RSP/R12 as base always emits SIB byte (hardware requirement)
//   - RBP/R13 with disp=0 uses mod=01 + disp8=0 (mod=00 means RIP-relative)
//   - SSE mandatory prefix (F2/66) comes BEFORE REX in byte stream
//   - All external branch/data references use addend = -4
// Ownership/Lifetime:
//   - Encoder state (labelOffsets_, pendingBranches_) is reset per function
//   - CodeSection is borrowed, not owned
// Links: src/codegen/x86_64/binenc/X64BinaryEncoder.hpp,
//        src/codegen/x86_64/binenc/X64Encoding.hpp,
//        src/codegen/common/objfile/CodeSection.hpp
//
//===----------------------------------------------------------------------===//

#include "X64BinaryEncoder.hpp"
#include "X64Encoding.hpp"

#include "codegen/common/objfile/DebugLineTable.hpp"
#include "il/runtime/RuntimeNameMap.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <variant>

/// @file
/// @brief Implements x86-64 instruction encoding, layout, and relocation emission.

namespace zanna::codegen::x64::binenc {

/// @brief Maps a canonical IL runtime name to its C ABI symbol spelling.
/// @param name IL external name or an already-native symbol name.
/// @return Mapped runtime spelling when registered, otherwise an unchanged copy.
static std::string mapRuntimeSymbol(const std::string &name) {
    if (auto mapped = il::runtime::mapCanonicalRuntimeName(name))
        return std::string(*mapped);
    return name;
}

// === Helper: extract PhysReg from an OpReg operand ===

/// @brief Convert an `OpReg` to a validated `PhysReg`.
/// @details Rejects virtual registers, out-of-range physical register ids, and
///          register-class mismatches (GPR id with XMM class or vice versa).
///          Throws `std::runtime_error` with a descriptive message on any
///          failure so a lowering bug surfaces immediately rather than emitting
///          garbage bytes.
/// @param reg Operand register descriptor produced by the lowerer.
/// @return The validated physical register.
/// @throws std::runtime_error if @p reg is virtual, out of range, or class-mismatched.
static PhysReg toPhys(const OpReg &reg) {
    if (!reg.isPhys)
        throw std::runtime_error("x86-64 binary encoder cannot encode virtual register v" +
                                 std::to_string(reg.idOrPhys));
    if (reg.idOrPhys > static_cast<uint16_t>(PhysReg::XMM15)) {
        throw std::runtime_error("x86-64 binary encoder: physical register id out of range");
    }
    const PhysReg phys = static_cast<PhysReg>(reg.idOrPhys);
    const bool classMatches =
        (reg.cls == RegClass::GPR && isGPR(phys)) || (reg.cls == RegClass::XMM && isXMM(phys));
    if (!classMatches) {
        throw std::runtime_error("x86-64 binary encoder: register class does not match "
                                 "physical register");
    }
    return phys;
}

/// @brief Extract a validated `PhysReg` from a register-variant `Operand`.
/// @param op Operand expected to be of `OpReg` variant.
/// @return Validated physical register from the operand.
/// @throws std::bad_variant_access If @p op does not contain @c OpReg.
/// @throws std::runtime_error If the contained register fails @ref toPhys validation.
static PhysReg regFromOperand(const Operand &op) {
    return toPhys(std::get<OpReg>(op));
}

/// @brief Extract a GPR `PhysReg` from @p op or throw if it's an XMM.
/// @param op      Operand expected to be a register operand.
/// @param context Short description for the error message on a class mismatch.
/// @return Validated GPR physical register.
/// @throws std::bad_variant_access If @p op does not contain @c OpReg.
/// @throws std::runtime_error If @p op contains a virtual, invalid, or non-GPR register.
static PhysReg gprFromOperand(const Operand &op, const char *context) {
    const PhysReg reg = regFromOperand(op);
    if (!isGPR(reg))
        throw std::runtime_error(std::string(context) + ": expected GPR operand");
    return reg;
}

/// @brief Extract an XMM `PhysReg` from @p op or throw if it's a GPR.
/// @param op      Operand expected to be a register operand.
/// @param context Short description for the error message on a class mismatch.
/// @return Validated XMM physical register.
/// @throws std::bad_variant_access If @p op does not contain @c OpReg.
/// @throws std::runtime_error If @p op contains a virtual, invalid, or non-XMM register.
static PhysReg xmmFromOperand(const Operand &op, const char *context) {
    const PhysReg reg = regFromOperand(op);
    if (!isXMM(reg))
        throw std::runtime_error(std::string(context) + ": expected XMM operand");
    return reg;
}

/// @brief Extract the memory-operand variant from @p op (throws if not memory).
/// @param op Operand expected to be of `OpMem` variant.
/// @return Reference to the underlying `OpMem`.
/// @throws std::bad_variant_access If @p op does not contain @c OpMem.
static const OpMem &memFromOperand(const Operand &op) {
    return std::get<OpMem>(op);
}

/// @brief Extract the integer immediate value from @p op.
/// @param op Operand expected to be of `OpImm` variant.
/// @return The 64-bit immediate value carried by @p op.
/// @throws std::bad_variant_access If @p op does not contain @c OpImm.
static int64_t immFromOperand(const Operand &op) {
    return std::get<OpImm>(op).val;
}

/// @brief Extract the label-operand variant from @p op (throws if not a label).
/// @param op Operand expected to be of `OpLabel` variant.
/// @return Reference to the underlying `OpLabel`.
/// @throws std::bad_variant_access If @p op does not contain @c OpLabel.
static const OpLabel &labelFromOperand(const Operand &op) {
    return std::get<OpLabel>(op);
}

/// @brief Extract the RIP-relative-label variant from @p op.
/// @param op Operand expected to be of `OpRipLabel` variant.
/// @return Reference to the underlying `OpRipLabel`.
/// @throws std::bad_variant_access If @p op does not contain @c OpRipLabel.
static const OpRipLabel &ripFromOperand(const Operand &op) {
    return std::get<OpRipLabel>(op);
}

/// @brief Narrow a 64-bit displacement to a 32-bit rel32 with bounds-check.
/// @details x86-64 PC-relative branches and RIP-relative addressing modes
///          encode the displacement as a signed 32-bit field. Throws on
///          overflow rather than truncating so a far-away branch surfaces as
///          a real error rather than corrupted bytes.
/// @param disp    Signed 64-bit displacement.
/// @param context Short description for the error message.
/// @return Narrowed signed 32-bit displacement.
/// @throws std::runtime_error if @p disp does not fit in `int32_t`.
static int32_t checkedRel32(int64_t disp, const char *context) {
    if (disp < std::numeric_limits<int32_t>::min() || disp > std::numeric_limits<int32_t>::max()) {
        throw std::runtime_error(std::string(context) + " rel32 displacement out of range");
    }
    return static_cast<int32_t>(disp);
}

/// @brief Computes a signed offset between two unsigned section positions.
/// @details Handles both directions without first converting either endpoint
///          to a signed type, including the exact @c INT64_MIN magnitude.
/// @param target Destination byte offset.
/// @param base Origin byte offset.
/// @param context Description included in an overflow diagnostic.
/// @return Signed value @p target minus @p base.
/// @throws std::runtime_error If the magnitude cannot be represented by @c int64_t.
static int64_t checkedOffsetDelta(size_t target, size_t base, const char *context) {
    if (target >= base) {
        const size_t diff = target - base;
        if (diff > static_cast<size_t>(std::numeric_limits<int64_t>::max()))
            throw std::runtime_error(std::string(context) + " displacement exceeds int64 range");
        return static_cast<int64_t>(diff);
    }
    const size_t diff = base - target;
    const uint64_t maxMagnitude = static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1ULL;
    if (static_cast<uint64_t>(diff) > maxMagnitude)
        throw std::runtime_error(std::string(context) + " displacement exceeds int64 range");
    if (static_cast<uint64_t>(diff) == maxMagnitude)
        return std::numeric_limits<int64_t>::min();
    return -static_cast<int64_t>(diff);
}

/// @brief Adds two byte counts while detecting @c size_t overflow.
/// @param lhs First byte count.
/// @param rhs Second byte count.
/// @param context Description included in an overflow diagnostic.
/// @return Exact sum of @p lhs and @p rhs.
/// @throws std::runtime_error If the sum exceeds addressable size.
static size_t checkedAddSize(size_t lhs, size_t rhs, const char *context) {
    if (lhs > std::numeric_limits<size_t>::max() - rhs)
        throw std::runtime_error(std::string(context) + " offset overflows addressable size");
    return lhs + rhs;
}

/// @brief Validates and extracts an instruction's sole non-empty label operand.
/// @param instr Instruction expected to contain exactly one @c OpLabel.
/// @param context Opcode or operation name included in diagnostics.
/// @return Reference to the validated label stored in @p instr.
/// @throws std::bad_variant_access If the sole operand is not a label.
/// @throws std::runtime_error If the operand count or label spelling is invalid.
static const OpLabel &checkedSingleLabelOperand(const MInstr &instr, const char *context) {
    if (instr.operands.size() != 1)
        throw std::runtime_error(std::string(context) + " requires exactly one label operand");
    const auto &label = labelFromOperand(instr.operands[0]);
    if (label.name.empty())
        throw std::runtime_error(std::string(context) + " label must not be empty");
    return label;
}

/// @brief Validated condition code and label target for a conditional branch.
struct CheckedJccOperands {
    /// @brief Backend condition-code value.
    int condition{0};
    /// @brief Borrowed label operand within the validated operand vector.
    const OpLabel *label{nullptr};
};

/// @brief Finds the unique condition code and label target of a @c JCC.
/// @details Operand order is irrelevant. Other operand variants are ignored,
///          while duplicate recognized roles are rejected.
/// @param ops Operand sequence to inspect.
/// @param context Opcode name included in validation diagnostics.
/// @return Pointers and values referring to the validated operands in @p ops.
/// @throws std::runtime_error If either role is missing, duplicated, or empty.
static CheckedJccOperands checkedJccOperands(const std::vector<Operand> &ops, const char *context) {
    const OpImm *condition = nullptr;
    const OpLabel *label = nullptr;
    for (const auto &operand : ops) {
        if (const auto *imm = std::get_if<OpImm>(&operand)) {
            if (condition)
                throw std::runtime_error(std::string(context) +
                                         " requires exactly one condition code");
            condition = imm;
            continue;
        }
        if (const auto *target = std::get_if<OpLabel>(&operand)) {
            if (label)
                throw std::runtime_error(std::string(context) +
                                         " requires exactly one label target");
            label = target;
        }
    }

    if (!condition)
        throw std::runtime_error(std::string(context) + " requires a condition code");
    if (!label)
        throw std::runtime_error(std::string(context) + " requires a label target");
    if (label->name.empty())
        throw std::runtime_error(std::string(context) + " label target must not be empty");
    return CheckedJccOperands{static_cast<int>(condition->val), label};
}

/// @brief Validated condition code and destination for a set-on-condition form.
struct CheckedSetccOperands {
    /// @brief Backend condition-code value.
    int condition{0};
    /// @brief Borrowed GPR-or-memory destination operand.
    const Operand *destination{nullptr};
};

/// @brief Finds the unique condition code and destination of a @c SETcc form.
/// @details Accepts a register or memory destination in either operand order;
///          other variants are ignored.
/// @param ops Operand sequence to inspect.
/// @param context Opcode name included in validation diagnostics.
/// @return Values and pointer referring to the validated operands in @p ops.
/// @throws std::runtime_error If either role is missing or duplicated.
static CheckedSetccOperands checkedSetccOperands(const std::vector<Operand> &ops,
                                                 const char *context) {
    const OpImm *condition = nullptr;
    const Operand *destination = nullptr;
    for (const auto &operand : ops) {
        if (const auto *imm = std::get_if<OpImm>(&operand)) {
            if (condition)
                throw std::runtime_error(std::string(context) +
                                         " requires exactly one condition code");
            condition = imm;
            continue;
        }
        if (std::holds_alternative<OpReg>(operand) || std::holds_alternative<OpMem>(operand)) {
            if (destination)
                throw std::runtime_error(std::string(context) +
                                         " requires exactly one destination");
            destination = &operand;
        }
    }

    if (!condition)
        throw std::runtime_error(std::string(context) + " requires a condition code");
    if (!destination)
        throw std::runtime_error(std::string(context) + " requires a GPR or memory destination");
    return CheckedSetccOperands{static_cast<int>(condition->val), destination};
}

/// @brief Test whether @p op is an internal-branch opcode (`JMP` or `JCC`).
/// @param op MIR opcode to classify.
/// @return True when @p op is an intra-function branch that may need patching.
static bool isInternalBranchOpcode(MOpcode op) {
    return op == MOpcode::JMP || op == MOpcode::JCC;
}

/// @brief Validate a memory operand prior to encoding.
/// @details Ensures the base register is a real GPR, and (when an index is
///          present) the index is also a GPR and is not `RSP` — the SIB
///          encoding cannot represent `RSP` as an index register.
/// @param mem     Memory operand under validation.
/// @param context Short description for the error message.
/// @throws std::runtime_error if any of the architectural constraints is violated.
static void validateEncodedMemOperand(const OpMem &mem, const char *context) {
    (void)toPhys(mem.base);
    if (mem.base.cls != RegClass::GPR) {
        throw std::runtime_error(std::string(context) + ": memory base register must be a GPR");
    }

    if (!mem.hasIndex)
        return;

    (void)toPhys(mem.index);
    if (mem.index.cls != RegClass::GPR) {
        throw std::runtime_error(std::string(context) + ": memory index register must be a GPR");
    }
    if (static_cast<PhysReg>(mem.index.idOrPhys) == PhysReg::RSP) {
        throw std::runtime_error(std::string(context) +
                                 ": x86-64 cannot encode %rsp as a SIB index register");
    }
}

/// @brief Map a `PhysReg` to the 4-bit register number used by Win64 unwind data.
/// @details Win64 `UNWIND_CODE` records identify the saved register by a 4-bit
///          number. For GPRs the encoding matches the architectural register
///          number (`RAX=0`, `RCX=1`, `RDX=2`, …, `R15=15`). For XMM registers
///          the same 4-bit field carries the XMM index (`XMM0=0`, …, `XMM15=15`);
///          the kind (GPR vs XMM) is implied by the `UNWIND_CODE_KIND` field.
/// @param reg Physical register being recorded.
/// @return 4-bit Win64 register number.
/// @throws std::runtime_error if @p reg is not encodable in a Win64 unwind code.
static uint8_t win64RegNumber(PhysReg reg) {
    switch (reg) {
        case PhysReg::RAX:
            return 0;
        case PhysReg::RCX:
            return 1;
        case PhysReg::RDX:
            return 2;
        case PhysReg::RBX:
            return 3;
        case PhysReg::RSP:
            return 4;
        case PhysReg::RBP:
            return 5;
        case PhysReg::RSI:
            return 6;
        case PhysReg::RDI:
            return 7;
        case PhysReg::R8:
            return 8;
        case PhysReg::R9:
            return 9;
        case PhysReg::R10:
            return 10;
        case PhysReg::R11:
            return 11;
        case PhysReg::R12:
            return 12;
        case PhysReg::R13:
            return 13;
        case PhysReg::R14:
            return 14;
        case PhysReg::R15:
            return 15;
        case PhysReg::XMM0:
            return 0;
        case PhysReg::XMM1:
            return 1;
        case PhysReg::XMM2:
            return 2;
        case PhysReg::XMM3:
            return 3;
        case PhysReg::XMM4:
            return 4;
        case PhysReg::XMM5:
            return 5;
        case PhysReg::XMM6:
            return 6;
        case PhysReg::XMM7:
            return 7;
        case PhysReg::XMM8:
            return 8;
        case PhysReg::XMM9:
            return 9;
        case PhysReg::XMM10:
            return 10;
        case PhysReg::XMM11:
            return 11;
        case PhysReg::XMM12:
            return 12;
        case PhysReg::XMM13:
            return 13;
        case PhysReg::XMM14:
            return 14;
        case PhysReg::XMM15:
            return 15;
    }
    throw std::runtime_error("x86-64 binary encoder: unsupported register in Win64 unwind data");
}

/// @brief Planned Win64 unwind operation paired with its emitted end offset.
struct EmittedWin64UnwindOp {
    /// @brief Frame-lowering operation recognized in the prologue.
    Win64UnwindOp op;
    /// @brief Absolute text offset immediately after the matching instruction.
    size_t endOffset = 0;
};

/// @brief Test whether @p operand is a register operand naming @p reg.
/// @details Returns false for non-register operand variants, for virtual
///          registers (the encoder runs after register allocation), and for
///          registers whose id doesn't match @p reg.
/// @param operand Operand to classify.
/// @param reg     Expected physical register.
/// @return True if @p operand is a physical-register operand naming @p reg.
static bool operandIsPhysReg(const Operand &operand, PhysReg reg) {
    const auto *opReg = std::get_if<OpReg>(&operand);
    return opReg && opReg->isPhys && opReg->idOrPhys == static_cast<uint16_t>(reg);
}

/// @brief Test whether @p operand names the stack slot described by a Win64 save op.
/// @details Frame lowering emits callee-save stores through RBP-relative memory
///          operands, but Win64 unwind records describe save slots as positive
///          offsets from final RSP. This helper translates the unwind offset
///          back to the expected RBP displacement and verifies the memory shape.
/// @param operand Memory operand from a callee-save store instruction.
/// @param frame Frame metadata containing the final frame size.
/// @param op Planned Win64 save operation being matched.
/// @return True only when @p operand stores to the exact unwind-described slot.
static bool operandIsWin64SaveSlot(const Operand &operand,
                                   const FrameInfo &frame,
                                   const Win64UnwindOp &op) {
    const auto *mem = std::get_if<OpMem>(&operand);
    if (mem == nullptr || mem->hasIndex)
        return false;
    if (!operandIsPhysReg(mem->base, PhysReg::RBP))
        return false;
    const int64_t expectedDisp =
        static_cast<int64_t>(op.stackOffset) - static_cast<int64_t>(frame.frameSize);
    if (expectedDisp < std::numeric_limits<int32_t>::min() ||
        expectedDisp > std::numeric_limits<int32_t>::max())
        return false;
    return mem->disp == static_cast<int32_t>(expectedDisp);
}

/// @brief Test whether the emitted @p instr realises a Win64 unwind op @p op.
/// @details After prologue emission, Win64 requires an `UNWIND_INFO` record that
///          lists each prologue instruction along with its offset. This helper
///          recognises the four supported op kinds:
///            - `PushNonVol`: `PUSH reg`
///            - `AllocStack`: `SUB RSP, #imm` (encoded as `ADDri RSP, -imm`)
///            - `SaveNonVol`: `MOV [mem], reg` (general save to stack)
///            - `SaveXmm128`: `MOVUPS [mem], xmm` (128-bit XMM save)
/// @param instr Machine instruction being matched.
/// @param frame Frame metadata used to verify save-slot addresses.
/// @param op    Unwind op the encoder expected to emit.
/// @return True if @p instr is the encoded form of @p op.
static bool instrMatchesWin64UnwindOp(const MInstr &instr,
                                      const FrameInfo &frame,
                                      const Win64UnwindOp &op) {
    switch (op.kind) {
        case Win64UnwindOpKind::PushNonVol:
            return instr.opcode == MOpcode::PUSH && !instr.operands.empty() &&
                   operandIsPhysReg(instr.operands[0], op.reg);
        case Win64UnwindOpKind::AllocStack: {
            if (instr.opcode != MOpcode::ADDri || instr.operands.size() < 2 ||
                !operandIsPhysReg(instr.operands[0], PhysReg::RSP))
                return false;
            const auto *imm = std::get_if<OpImm>(&instr.operands[1]);
            return imm && imm->val == -static_cast<int64_t>(op.stackOffset);
        }
        case Win64UnwindOpKind::SaveNonVol:
            return instr.opcode == MOpcode::MOVrm && instr.operands.size() >= 2 &&
                   operandIsWin64SaveSlot(instr.operands[0], frame, op) &&
                   operandIsPhysReg(instr.operands[1], op.reg);
        case Win64UnwindOpKind::SaveXmm128:
            return instr.opcode == MOpcode::MOVUPSrm && instr.operands.size() >= 2 &&
                   operandIsWin64SaveSlot(instr.operands[0], frame, op) &&
                   operandIsPhysReg(instr.operands[1], op.reg);
    }
    return false;
}

/// @brief Records Win64 unwind metadata for later object-file serialization.
/// @details Translates the list of prologue unwind ops gathered during code
///          emission into the section's structured Win64 unwind entry. The
///          object-file writer later serializes that entry as architectural
///          `UNWIND_INFO`, `UNWIND_CODE`, and `RUNTIME_FUNCTION` records. The
///          encoder must already have emitted the prologue at
///          @p funcStartOffset.
/// @param fn               Function whose prologue was emitted.
/// @param frame            Frame metadata; the prologue must have been emitted.
/// @param funcSymIdx       Index of the function symbol in the symbol table.
/// @param funcStartOffset  Byte offset of the function start in the text section.
/// @param emittedOps       Prologue ops recorded during emission with their end offsets.
/// @param text             Text section receiving the structured unwind entry.
/// @throws std::runtime_error If an 8-bit unwind offset exceeds 255 bytes or
///         the encoded function length exceeds 32 bits.
static void recordWin64Unwind(const MFunction &fn,
                              const FrameInfo &frame,
                              uint32_t funcSymIdx,
                              size_t funcStartOffset,
                              const std::vector<EmittedWin64UnwindOp> &emittedOps,
                              objfile::CodeSection &text) {
    if (!frame.prologueEmitted || emittedOps.empty())
        return;

    /// @brief Narrows an unwind byte-sized field with a function-aware diagnostic.
    /// @param value Value to narrow.
    /// @param field Field name used in an overflow message.
    /// @return @p value converted to @c uint8_t.
    /// @throws std::runtime_error If @p value exceeds 255.
    auto checkedU8 = [&](size_t value, const char *field) -> uint8_t {
        if (value > std::numeric_limits<uint8_t>::max()) {
            throw std::runtime_error("x86-64 binary encoder: Win64 unwind " + std::string(field) +
                                     " for function '" + fn.name + "' exceeds 255 bytes");
        }
        return static_cast<uint8_t>(value);
    };

    const size_t functionLength = text.currentOffset() - funcStartOffset;
    if (functionLength > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("x86-64 binary encoder: Win64 function length for '" + fn.name +
                                 "' exceeds 32-bit unwind range");
    }

    objfile::Win64UnwindEntry unwind{};
    unwind.symbolIndex = funcSymIdx;
    unwind.functionLength = static_cast<uint32_t>(functionLength);

    unwind.prologueSize = checkedU8(emittedOps.back().endOffset - funcStartOffset, "prologue size");
    for (const auto &emitted : emittedOps) {
        const uint8_t codeOffset = checkedU8(emitted.endOffset - funcStartOffset, "code offset");
        switch (emitted.op.kind) {
            case Win64UnwindOpKind::PushNonVol:
                unwind.codes.push_back({objfile::Win64UnwindCode::Kind::PushNonVol,
                                        codeOffset,
                                        win64RegNumber(emitted.op.reg),
                                        0});
                break;
            case Win64UnwindOpKind::AllocStack:
                unwind.codes.push_back({objfile::Win64UnwindCode::Kind::AllocStack,
                                        codeOffset,
                                        0,
                                        emitted.op.stackOffset});
                break;
            case Win64UnwindOpKind::SaveNonVol:
                unwind.codes.push_back({objfile::Win64UnwindCode::Kind::SaveNonVol,
                                        codeOffset,
                                        win64RegNumber(emitted.op.reg),
                                        emitted.op.stackOffset});
                break;
            case Win64UnwindOpKind::SaveXmm128:
                unwind.codes.push_back({objfile::Win64UnwindCode::Kind::SaveXmm128,
                                        codeOffset,
                                        win64RegNumber(emitted.op.reg),
                                        emitted.op.stackOffset});
                break;
        }
    }

    text.addWin64UnwindEntry(std::move(unwind));
}

// === Public entry point ===

/// @brief Predict the encoded byte size of a MIR instruction without emitting it.
/// @details Used by the label offset pre-computation pass to determine branch
///          displacement sizes before final encoding. A scratch section with a
///          logical offset bias preserves PC-relative decisions without
///          mutating the caller's sections. The result must match
///          @ref encodeInstruction byte for byte.
/// @param instr Instruction to measure.
/// @param currentOffset Logical instruction start used for branch displacement checks.
/// @param knownLabelOffsets Current layout estimate for resolvable labels.
/// @param isDarwin Target-format compatibility flag.
/// @return Number of bytes the instruction would append.
/// @throws std::runtime_error If the instruction or operands are not encodable.
size_t X64BinaryEncoder::measureInstructionSize(const MInstr &instr,
                                                size_t currentOffset,
                                                const LabelOffsetMap &knownLabelOffsets,
                                                bool isDarwin) {
    X64BinaryEncoder measureEncoder;
    measureEncoder.labelOffsets_ = knownLabelOffsets;
    measureEncoder.shortBranchRelaxationEnabled_ = shortBranchRelaxationEnabled_;

    objfile::CodeSection text, rodata;
    text.setLogicalOffsetBias(currentOffset);
    text.reserveBytes(16);
    try {
        measureEncoder.encodeInstructionImpl(instr, text, rodata, isDarwin);
    } catch (const std::bad_variant_access &) {
        std::string msg = "measureInstructionSize: bad variant access for opcode " +
                          std::to_string(static_cast<int>(instr.opcode)) + " with " +
                          std::to_string(instr.operands.size()) + " operand(s):";
        for (std::size_t i = 0; i < instr.operands.size(); ++i)
            msg += " [" + std::to_string(i) + "]=idx" + std::to_string(instr.operands[i].index());
        throw std::runtime_error(msg);
    }
    return text.currentOffset() - currentOffset;
}

/// @brief Resolve every label in @p fn to a stable byte offset and final size.
/// @details The encoder must know branch displacements before it emits
///          instructions because branch encodings depend on size (rel8 vs
///          rel32). This method iterates a measurement pass to a fixed
///          point: each pass uses the previous iteration's offsets to
///          decide short vs near forms, then re-measures. Convergence is
///          bounded by the number of relaxation-candidate branches plus
///          one; further iterations cannot enable new short branches once
///          all candidates stabilise.
///          When branch relaxation is disabled the function does a single
///          pass with conservative (always rel32) sizes.
/// @param fn Function being encoded.
/// @param isDarwin True when targeting Mach-O (affects symbol mangling
///        which can change instruction sizes through relocation choices).
/// @return Label offsets plus the predicted encoded function size.
X64BinaryEncoder::LabelLayout X64BinaryEncoder::computeFunctionLabelLayout(const MFunction &fn,
                                                                           bool isDarwin) {
    LabelOffsetMap estimated;
    size_t estimatedSize = 0;

    if (!shortBranchRelaxationEnabled_) {
        size_t offset = 0;
        for (const auto &block : fn.blocks) {
            /// @brief Records a unique label at its conservative-pass offset.
            /// @param name Label spelling; an empty name is ignored.
            /// @param labelOffset Function-relative predicted byte offset.
            /// @throws std::runtime_error If @p name was already defined.
            auto assignLabel = [&](const std::string &name, size_t labelOffset) {
                if (name.empty())
                    return;
                if (estimated.find(name) != estimated.end()) {
                    throw std::runtime_error("x86-64 binary encoder: duplicate label '" + name +
                                             "' in function '" + fn.name + "'");
                }
                estimated[name] = labelOffset;
            };
            assignLabel(block.label, offset);
            for (const auto &instr : block.instructions) {
                if (instr.opcode == MOpcode::LABEL) {
                    assignLabel(checkedSingleLabelOperand(instr, "x86-64 LABEL").name, offset);
                    continue;
                }
                offset += measureInstructionSize(instr, offset, estimated, isDarwin);
            }
        }
        return {std::move(estimated), offset};
    }

    size_t relaxCandidates = 0;
    for (const auto &block : fn.blocks) {
        for (const auto &instr : block.instructions) {
            if (isInternalBranchOpcode(instr.opcode))
                ++relaxCandidates;
        }
    }

    const size_t maxIterations = std::max<size_t>(2, relaxCandidates + 2);
    for (size_t iter = 0; iter < maxIterations; ++iter) {
        bool changed = false;
        LabelOffsetMap known = estimated;
        LabelOffsetMap next;
        next.reserve(estimated.size() + fn.blocks.size());

        /// @brief Records a label and reports layout changes for this iteration.
        /// @param name Label spelling; an empty name is ignored.
        /// @param offset Function-relative byte offset in the new estimate.
        /// @throws std::runtime_error If @p name occurs twice in the function.
        auto assignLabel = [&](const std::string &name, size_t offset) {
            if (name.empty())
                return;
            if (next.find(name) != next.end()) {
                throw std::runtime_error("x86-64 binary encoder: duplicate label '" + name +
                                         "' in function '" + fn.name + "'");
            }
            auto prevIt = estimated.find(name);
            if (prevIt == estimated.end() || prevIt->second != offset)
                changed = true;
            known[name] = offset;
            next[name] = offset;
        };

        size_t offset = 0;
        for (const auto &block : fn.blocks) {
            assignLabel(block.label, offset);

            for (const auto &instr : block.instructions) {
                if (instr.opcode == MOpcode::LABEL) {
                    assignLabel(checkedSingleLabelOperand(instr, "x86-64 LABEL").name, offset);
                    continue;
                }
                offset += measureInstructionSize(instr, offset, known, isDarwin);
            }
        }

        if (offset != estimatedSize)
            changed = true;

        if (!changed && next.size() == estimated.size())
            return {std::move(next), offset};
        estimated = std::move(next);
        estimatedSize = offset;
    }

    throw std::runtime_error("x86-64 binary encoder: short-branch relaxation did not converge for "
                             "function '" +
                             fn.name + "'");
}

/// @brief Assert that emission matches the pre-computed offset for @p label.
/// @details Branch-relaxation correctness depends on the predicted size
///          matching the emitted size byte for byte. When they diverge, the
///          rel32 patch sites point at the wrong location and the function
///          would silently produce broken machine code. This routine throws
///          a descriptive error so the mismatch surfaces immediately.
/// @param label Label whose stored prediction is being checked.
/// @param actualOffset Absolute section offset observed during emission.
/// @throws std::runtime_error If a stored prediction differs from @p actualOffset.
void X64BinaryEncoder::verifyPredictedLabelOffset(const std::string &label,
                                                  size_t actualOffset) const {
    auto it = labelOffsets_.find(label);
    if (it == labelOffsets_.end())
        return;
    if (it->second != actualOffset) {
        throw std::runtime_error("x86-64 binary encoder: label offset drift for '" + label +
                                 "' (predicted=" + std::to_string(it->second) +
                                 ", actual=" + std::to_string(actualOffset) + ")");
    }
}

/// @brief Encode an entire MIR function into machine code bytes.
/// @details Two-pass approach: first pass computes label offsets using instruction
///          size measurement, second pass emits actual bytes. Branch displacements
///          are resolved using the pre-computed label-to-offset map. The method
///          also defines the function symbol, records optional debug locations,
///          patches jump-table words, verifies predicted size, and serializes a
///          matching Win64 unwind plan when requested.
/// @param fn Allocated and legalized MIR function.
/// @param text Mutable text section receiving bytes, symbols, and relocations.
/// @param rodata Read-only section used to resolve local data symbols.
/// @param isDarwin Target-format compatibility flag.
/// @param frame Optional frame and unwind plan.
/// @param emitWin64Unwind Whether Win64 unwind metadata should be recorded.
/// @throws std::runtime_error If layout, instruction encoding, target
///         resolution, size verification, or unwind-plan matching fails.
void X64BinaryEncoder::encodeFunction(const MFunction &fn,
                                      objfile::CodeSection &text,
                                      const objfile::CodeSection &rodata,
                                      bool isDarwin,
                                      const FrameInfo *frame,
                                      bool emitWin64Unwind) {
    // Reset per-function state.
    pendingBranches_.clear();
    pendingTableEntries_.clear();
    // Win64/COFF images do not benefit enough from branch compaction to justify
    // repeated size-relaxation passes on large demo modules. Use near branches
    // there; keep rel8 relaxation for smaller non-COFF objects.
    shortBranchRelaxationEnabled_ = !emitWin64Unwind;

    // Define the function symbol at the current text offset.
    const size_t funcStartOffset = text.currentOffset();
    const auto layout = computeFunctionLabelLayout(fn, isDarwin);
    const auto &relativeLabelOffsets = layout.offsets;
    const size_t estimatedSize = layout.estimatedSize;
    text.reserveAdditionalBytes(estimatedSize);
    labelOffsets_.clear();
    labelOffsets_.reserve(relativeLabelOffsets.size());
    for (const auto &[label, offset] : relativeLabelOffsets)
        labelOffsets_[label] = funcStartOffset + offset;

    (void)isDarwin;
    std::string symName = fn.name;
    const uint32_t funcSymIdx =
        text.defineSymbol(symName, objfile::SymbolBinding::Global, objfile::SymbolSection::Text);

    std::vector<EmittedWin64UnwindOp> emittedWin64UnwindOps;
    size_t win64UnwindCursor = 0;

    // Encode all blocks.
    for (std::size_t blockIndex = 0; blockIndex < fn.blocks.size(); ++blockIndex) {
        const auto &block = fn.blocks[blockIndex];
        // Record label offset for internal branch resolution.
        if (!block.label.empty()) {
            verifyPredictedLabelOffset(block.label, text.currentOffset());
            auto existing = labelOffsets_.find(block.label);
            if (existing != labelOffsets_.end() && existing->second != text.currentOffset()) {
                throw std::runtime_error("x86-64 binary encoder: duplicate emitted label '" +
                                         block.label + "' in function '" + fn.name + "'");
            }
            labelOffsets_[block.label] = text.currentOffset();
        }

        for (const auto &instr : block.instructions) {
            if (debugLines_ && instr.loc.hasLine())
                debugLines_->addEntry(
                    text.currentOffset(), instr.loc.file_id, instr.loc.line, instr.loc.column);
            encodeInstruction(instr, text, rodata, isDarwin);
            if (blockIndex == 0 && emitWin64Unwind && frame &&
                win64UnwindCursor < frame->win64UnwindOps.size() &&
                instrMatchesWin64UnwindOp(
                    instr, *frame, frame->win64UnwindOps[win64UnwindCursor])) {
                emittedWin64UnwindOps.push_back(
                    {frame->win64UnwindOps[win64UnwindCursor], text.currentOffset()});
                ++win64UnwindCursor;
            }
        }
    }

    // Resolve pending internal branches.
    for (const auto &pb : pendingBranches_) {
        auto it = labelOffsets_.find(pb.target);
        if (it == labelOffsets_.end()) {
            throw std::runtime_error("x86-64 binary encoder: unresolved internal branch target '" +
                                     pb.target + "' in function '" + fn.name + "'");
        }
        // rel32 = target - (patchOffset + 4)
        const size_t nextIp = checkedAddSize(pb.patchOffset, 4, "internal branch");
        const int64_t disp = checkedOffsetDelta(it->second, nextIp, "internal branch");
        const int32_t rel = checkedRel32(disp, "internal branch");
        text.patch32LE(pb.patchOffset, static_cast<uint32_t>(rel));
    }

    // Resolve jump-table entry words: offset(case) - tableStart.
    for (const auto &te : pendingTableEntries_) {
        auto it = labelOffsets_.find(te.caseLabel);
        if (it == labelOffsets_.end()) {
            throw std::runtime_error("x86-64 binary encoder: unresolved jump-table target '" +
                                     te.caseLabel + "' in function '" + fn.name + "'");
        }
        const int64_t disp = checkedOffsetDelta(it->second, te.tableStart, "jump-table entry");
        const int32_t rel = checkedRel32(disp, "jump-table entry");
        text.patch32LE(te.patchOffset, static_cast<uint32_t>(rel));
    }

    const size_t actualSize = text.currentOffset() - funcStartOffset;
    if (actualSize != estimatedSize) {
        throw std::runtime_error("x86-64 binary encoder: function size drift for '" + fn.name +
                                 "' (predicted=" + std::to_string(estimatedSize) +
                                 ", actual=" + std::to_string(actualSize) + ")");
    }

    if (emitWin64Unwind && frame) {
        if (frame->prologueEmitted && win64UnwindCursor != frame->win64UnwindOps.size()) {
            throw std::runtime_error("x86-64 binary encoder: Win64 unwind plan for function '" +
                                     fn.name + "' matched " + std::to_string(win64UnwindCursor) +
                                     " of " + std::to_string(frame->win64UnwindOps.size()) +
                                     " prologue operation(s)");
        }
        recordWin64Unwind(fn, *frame, funcSymIdx, funcStartOffset, emittedWin64UnwindOps, text);
    }
}

// === Main instruction dispatch ===

/// @brief Encodes a single MIR instruction and normalizes variant errors.
/// @param instr Instruction to append.
/// @param text Mutable machine-code destination.
/// @param rodata Read-only section used for symbol lookup.
/// @param isDarwin Target-format compatibility flag.
/// @throws std::runtime_error If the opcode or operand shape cannot be encoded.
void X64BinaryEncoder::encodeInstruction(const MInstr &instr,
                                         objfile::CodeSection &text,
                                         const objfile::CodeSection &rodata,
                                         bool isDarwin) {
    try {
        encodeInstructionImpl(instr, text, rodata, isDarwin);
    } catch (const std::bad_variant_access &) {
        std::string msg = "bad variant access encoding opcode " +
                          std::to_string(static_cast<int>(instr.opcode)) + " with " +
                          std::to_string(instr.operands.size()) + " operand(s):";
        for (std::size_t i = 0; i < instr.operands.size(); ++i) {
            msg += " [" + std::to_string(i) + "]=idx" + std::to_string(instr.operands[i].index());
        }
        throw std::runtime_error(msg);
    }
}

/// @brief Dispatches a validated instruction to its concrete byte encoder.
/// @details Enforces exact operand counts, rejects residual pseudos, handles
///          simple encodings inline, and delegates compound forms to the
///          category-specific helpers.
/// @param instr Instruction to encode.
/// @param text Mutable machine-code destination.
/// @param rodata Read-only section used for RIP-relative references.
/// @param isDarwin Target-format compatibility flag.
/// @throws std::runtime_error If the instruction is malformed or unsupported.
void X64BinaryEncoder::encodeInstructionImpl(const MInstr &instr,
                                             objfile::CodeSection &text,
                                             const objfile::CodeSection &rodata,
                                             bool isDarwin) {
    const auto &ops = instr.operands;
    const auto op = instr.opcode;
    const auto nOps = ops.size();

    // Guard helper: abort early with a clear message on operand count mismatch.
    /// @brief Requires the current instruction to have exactly @p n operands.
    /// @param n Expected operand count.
    /// @throws std::runtime_error If the actual count differs.
    auto requireOps = [&](std::size_t n) {
        if (nOps != n) {
            throw std::runtime_error("x86-64 binary encoder: opcode " +
                                     std::to_string(static_cast<int>(op)) + " requires exactly " +
                                     std::to_string(n) + " operand(s) but has " +
                                     std::to_string(nOps));
        }
    };

    switch (op) {
        case MOpcode::SELECT_GPR:
        case MOpcode::SELECT_XMM:
            throw std::runtime_error("x86-64 binary encoder: select pseudo survived ISel");

        // --- Nullary ---
        case MOpcode::RET:
        case MOpcode::CQO:
        case MOpcode::UD2:
            requireOps(0);
            encodeNullary(op, text);
            return;

        case MOpcode::PUSH: {
            requireOps(1);
            const auto hw = hwEncode(gprFromOperand(ops[0], "PUSH"));
            if (hw.rexBit)
                text.emit8(computeRex(false, false, false, true));
            text.emit8(static_cast<uint8_t>(0x50 + hw.bits3));
            return;
        }

        case MOpcode::POP: {
            requireOps(1);
            const auto hw = hwEncode(gprFromOperand(ops[0], "POP"));
            if (hw.rexBit)
                text.emit8(computeRex(false, false, false, true));
            text.emit8(static_cast<uint8_t>(0x58 + hw.bits3));
            return;
        }

        // --- Pseudo: skip ---
        case MOpcode::PX_COPY:
            throw std::runtime_error(
                "x86-64 binary encoder: parallel-copy pseudo survived lowering");

        // --- Label definition ---
        case MOpcode::LABEL: {
            requireOps(1);
            const auto &label = labelFromOperand(ops[0]);
            verifyPredictedLabelOffset(label.name, text.currentOffset());
            labelOffsets_[label.name] = text.currentOffset();
            return;
        }

        // --- Pseudo-instructions that should have been expanded ---
        case MOpcode::DIVS64rr:
        case MOpcode::REMS64rr:
        case MOpcode::DIVS64Chk0rr:
        case MOpcode::REMS64Chk0rr:
        case MOpcode::DIVU64rr:
        case MOpcode::REMU64rr:
        case MOpcode::DIVU64Chk0rr:
        case MOpcode::REMU64Chk0rr:
        case MOpcode::ADDOvfrr:
        case MOpcode::SUBOvfrr:
        case MOpcode::IMULOvfrr:
            throw std::runtime_error("x86-64 binary encoder: pseudo-op reached emission: " +
                                     toString(instr));

        // --- MOVri (64-bit immediate) ---
        case MOpcode::MOVri: {
            requireOps(2);
            PhysReg dst = gprFromOperand(ops[0], "MOVri");
            int64_t imm = immFromOperand(ops[1]);
            encodeMovRI(dst, imm, text);
            return;
        }

        // --- Reg-Reg GPR ---
        case MOpcode::MOVrr:
        case MOpcode::ADDrr:
        case MOpcode::SUBrr:
        case MOpcode::ANDrr:
        case MOpcode::ORrr:
        case MOpcode::XORrr:
        case MOpcode::CMPrr:
        case MOpcode::TESTrr:
        case MOpcode::IMULrr:
        case MOpcode::CMOVNErr:
        case MOpcode::XORrr32:
        case MOpcode::MOVZXrr32:
        case MOpcode::ADDrr32:
        case MOpcode::SUBrr32:
        case MOpcode::IMULrr32:
        case MOpcode::CMPrr32:
        case MOpcode::MOVSXD:
        case MOpcode::ADDrr16:
        case MOpcode::SUBrr16:
        case MOpcode::IMULrr16:
        case MOpcode::MOVSXrr16: {
            requireOps(2);
            PhysReg dst = gprFromOperand(ops[0], "GPR reg-reg destination");
            PhysReg src = gprFromOperand(ops[1], "GPR reg-reg source");
            encodeRegReg(op, dst, src, text);
            return;
        }

        // --- Reg-Imm ALU ---
        case MOpcode::ADDri:
        case MOpcode::ANDri:
        case MOpcode::ORri:
        case MOpcode::XORri:
        case MOpcode::CMPri: {
            requireOps(2);
            PhysReg dst = gprFromOperand(ops[0], "GPR reg-imm destination");
            int64_t imm = immFromOperand(ops[1]);
            encodeRegImm(op, dst, imm, text);
            return;
        }
        case MOpcode::ADDri32: {
            requireOps(2);
            PhysReg dst = gprFromOperand(ops[0], "GPR reg-imm destination");
            int64_t imm = immFromOperand(ops[1]);
            encodeRegImm(op, dst, imm, text, /*rexW=*/false);
            return;
        }
        case MOpcode::ADDri16: {
            requireOps(2);
            PhysReg dst = gprFromOperand(ops[0], "GPR reg-imm destination");
            int64_t imm = immFromOperand(ops[1]);
            encodeRegImm(op, dst, imm, text, /*rexW=*/false, /*prefix66=*/true);
            return;
        }

        // --- Shifts (immediate count) ---
        case MOpcode::SHLri:
        case MOpcode::SHRri:
        case MOpcode::SARri: {
            requireOps(2);
            PhysReg dst = gprFromOperand(ops[0], "shift destination");
            int64_t count = immFromOperand(ops[1]);
            encodeShiftImm(op, dst, count, text);
            return;
        }

        // --- Shifts (CL count) ---
        case MOpcode::SHLrc:
        case MOpcode::SHRrc:
        case MOpcode::SARrc: {
            requireOps(2);
            PhysReg dst = gprFromOperand(ops[0], "shift destination");
            PhysReg countReg = gprFromOperand(ops[1], "shift count");
            if (countReg != PhysReg::RCX)
                throw std::runtime_error(
                    "x86-64 binary encoder: register-count shift requires RCX/CL");
            encodeShiftCL(op, dst, text);
            return;
        }

        // --- Division ---
        case MOpcode::IDIVrm:
        case MOpcode::DIVrm:
        case MOpcode::MULr:
        case MOpcode::IMULr: {
            requireOps(1);
            // Operand can be reg or mem. Pipeline always uses reg after expansion.
            if (std::holds_alternative<OpReg>(ops[0])) {
                encodeDiv(op, gprFromOperand(ops[0], "division source"), text);
            } else {
                // Memory operand for div — encode as unary with memory.
                const auto &mem = memFromOperand(ops[0]);
                uint8_t ext = (op == MOpcode::IDIVrm)  ? 7
                              : (op == MOpcode::DIVrm) ? 6
                              : (op == MOpcode::IMULr) ? 5
                                                       : 4;
                emitWithMemOperand(ext,
                                   0,
                                   mem,
                                   text,
                                   /*rexW=*/true,
                                   /*mandatoryPrefix=*/0,
                                   0xF7,
                                   0);
            }
            return;
        }

        // --- Memory operations ---
        case MOpcode::MOVrm: // store: MOVrm [mem], reg
        {
            requireOps(2);
            PhysReg src = gprFromOperand(ops[1], "MOVrm source");
            const auto &mem = memFromOperand(ops[0]);
            encodeMemOp(op, src, mem, text);
            return;
        }
        case MOpcode::MOVmr: // load: MOVmr reg, [mem]
        case MOpcode::ADDrm: // reg <- reg op [mem]
        case MOpcode::SUBrm:
        case MOpcode::ANDrm:
        case MOpcode::ORrm:
        case MOpcode::XORrm:
        case MOpcode::CMPrm:
        case MOpcode::IMULrm: {
            requireOps(2);
            PhysReg dst = gprFromOperand(ops[0], "memory-operand GPR destination");
            const auto &mem = memFromOperand(ops[1]);
            encodeMemOp(op, dst, mem, text);
            return;
        }

        // --- LEA ---
        case MOpcode::LEA: {
            requireOps(2);
            PhysReg dst = gprFromOperand(ops[0], "LEA destination");
            if (std::holds_alternative<OpRipLabel>(ops[1])) {
                encodeLEARip(dst, ripFromOperand(ops[1]), text, rodata, isDarwin);
            } else if (std::holds_alternative<OpMem>(ops[1])) {
                encodeLEA(dst, memFromOperand(ops[1]), text);
            } else {
                throw std::runtime_error(
                    "x86-64 binary encoder: LEA requires a memory or RIP-relative operand");
            }
            return;
        }

        // --- SETcc ---
        case MOpcode::SETcc: {
            requireOps(2);
            const auto setcc = checkedSetccOperands(ops, "x86-64 binary encoder: SETcc");
            if (const auto *dstReg = std::get_if<OpReg>(setcc.destination)) {
                encodeSETcc(setcc.condition, toPhys(*dstReg), text);
            } else if (const auto *dstMem = std::get_if<OpMem>(setcc.destination)) {
                const uint8_t cc = x86CC(setcc.condition);
                emitWithMemOperand(/*reg3=*/0,
                                   /*regRex=*/0,
                                   *dstMem,
                                   text,
                                   /*rexW=*/false,
                                   /*mandatoryPrefix=*/0,
                                   /*opByte1=*/0x0F,
                                   /*opByte2=*/static_cast<uint8_t>(0x90 + cc));
            }
            return;
        }

        // --- MOVZXrr8 (movzbq) ---
        case MOpcode::MOVZXrr8: {
            requireOps(2);
            PhysReg dst = gprFromOperand(ops[0], "MOVZX destination");
            PhysReg src = gprFromOperand(ops[1], "MOVZX source");
            encodeMOVZX(dst, src, text);
            return;
        }

        // --- SSE reg-reg ---
        case MOpcode::FADD:
        case MOpcode::FSUB:
        case MOpcode::FMUL:
        case MOpcode::FDIV:
        case MOpcode::UCOMIS:
        case MOpcode::CVTSI2SD:
        case MOpcode::CVTTSD2SI:
        case MOpcode::MOVQrx:
        case MOpcode::MOVQxr:
        case MOpcode::MOVSDrr: {
            requireOps(2);
            PhysReg dst = regFromOperand(ops[0]);
            PhysReg src = regFromOperand(ops[1]);
            encodeSseRegReg(op, dst, src, text);
            return;
        }

        // --- SSE memory ops ---
        case MOpcode::MOVSDrm: // store: [mem], xmm
        {
            requireOps(2);
            PhysReg src = xmmFromOperand(ops[1], "MOVSDrm source");
            const auto &mem = memFromOperand(ops[0]);
            encodeSseMem(op, src, mem, text);
            return;
        }
        case MOpcode::MOVSDmr: // load: xmm, [mem] or xmm, [rip+label]
        {
            requireOps(2);
            PhysReg dst = xmmFromOperand(ops[0], "MOVSDmr destination");
            if (std::holds_alternative<OpRipLabel>(ops[1])) {
                encodeSseRipLoad(dst, ripFromOperand(ops[1]), text, rodata, isDarwin);
            } else {
                const auto &mem = memFromOperand(ops[1]);
                encodeSseMem(op, dst, mem, text);
            }
            return;
        }
        case MOpcode::MOVUPSrm: // store: [mem], xmm
        {
            requireOps(2);
            PhysReg src = xmmFromOperand(ops[1], "MOVUPSrm source");
            const auto &mem = memFromOperand(ops[0]);
            encodeSseMem(op, src, mem, text);
            return;
        }
        case MOpcode::MOVUPSmr: // load: xmm, [mem]
        {
            requireOps(2);
            PhysReg dst = xmmFromOperand(ops[0], "MOVUPSmr destination");
            const auto &mem = memFromOperand(ops[1]);
            encodeSseMem(op, dst, mem, text);
            return;
        }

        // --- Branches and calls ---
        case MOpcode::JMP: {
            requireOps(1);
            if (std::holds_alternative<OpRipLabel>(ops[0])) {
                encodeBranchRip(op, ripFromOperand(ops[0]), text, rodata, isDarwin);
            } else {
                encodeBranchOperand(
                    op, ops[0], /*cc=*/0, text, "JMP requires a label, register, or memory target");
            }
            return;
        }

        case MOpcode::JCC: {
            requireOps(2);
            const auto jcc = checkedJccOperands(ops, "x86-64 binary encoder: JCC");
            encodeBranchLabel(op, jcc.label->name, jcc.condition, text);
            return;
        }

        case MOpcode::JUMPTABLE: {
            // Operands: [0]=index reg (use), [1]=table name label (text-asm
            // only), [2..]=case labels. The dispatch tail runs in the
            // reserved R10/R11 scratch registers, which never carry live
            // values across instruction boundaries.
            if (ops.size() < 3) {
                throw std::runtime_error(
                    "x86-64 binary encoder: JUMPTABLE needs an index, a table label, "
                    "and at least one case");
            }
            const PhysReg idx = gprFromOperand(ops[0], "JUMPTABLE index");
            const PhysReg tblReg = PhysReg::R10;
            const PhysReg entReg = PhysReg::R11;

            OpMem entryMem{};
            entryMem.base = OpReg{true, RegClass::GPR, static_cast<uint16_t>(tblReg)};
            entryMem.index = OpReg{true, RegClass::GPR, static_cast<uint16_t>(idx)};
            entryMem.scale = 4;
            entryMem.disp = 0;
            entryMem.hasIndex = true;

            // Size the tail (movslq + add + jmp) into scratch so the LEA's
            // RIP displacement to the inline table is a plain constant.
            objfile::CodeSection scratch;
            encodeMemOp(MOpcode::MOVSXD, entReg, entryMem, scratch);
            encodeRegReg(MOpcode::ADDrr, entReg, tblReg, scratch);
            encodeBranchReg(MOpcode::JMP, entReg, scratch);
            const size_t tailSize = scratch.currentOffset();

            // lea tblReg, [rip + tailSize]  — the table follows the jmp.
            const auto hwDst = hwEncode(tblReg);
            text.emit8(computeRex(true, hwDst.rexBit != 0, false, false));
            text.emit8(0x8D);
            text.emit8(makeModRM(0b00, hwDst.bits3, 0b101));
            text.emit32LE(static_cast<uint32_t>(tailSize));

            encodeMemOp(MOpcode::MOVSXD, entReg, entryMem, text);
            encodeRegReg(MOpcode::ADDrr, entReg, tblReg, text);
            encodeBranchReg(MOpcode::JMP, entReg, text);

            const size_t tableStart = text.currentOffset();
            for (std::size_t i = 2; i < ops.size(); ++i) {
                pendingTableEntries_.push_back(
                    {text.currentOffset(), labelFromOperand(ops[i]).name, tableStart});
                text.emit32LE(0);
            }
            return;
        }

        case MOpcode::CALL: {
            requireOps(1);
            // CALL handles label specially (direct internal vs. PLT external), so
            // the operand-kind dispatcher only covers the non-label paths.
            if (std::holds_alternative<OpLabel>(ops[0])) {
                const auto &label = labelFromOperand(ops[0]);
                if (labelOffsets_.find(label.name) != labelOffsets_.end()) {
                    encodeBranchLabel(op, label.name, 0, text);
                } else {
                    encodeCallExternal(label.name, text, isDarwin);
                }
            } else if (std::holds_alternative<OpRipLabel>(ops[0])) {
                encodeBranchRip(op, ripFromOperand(ops[0]), text, rodata, isDarwin);
            } else {
                encodeBranchOperand(
                    op, ops[0], /*cc=*/0, text, "CALL has unsupported operand shape");
            }
            return;
        }
    }

    throw std::runtime_error("x86-64 binary encoder: unhandled opcode value " +
                             std::to_string(static_cast<int>(instr.opcode)));
}

// === Nullary instructions ===

/// @copydoc X64BinaryEncoder::encodeNullary
void X64BinaryEncoder::encodeNullary(MOpcode op, objfile::CodeSection &cs) {
    switch (op) {
        case MOpcode::RET:
            cs.emit8(0xC3);
            return;
        case MOpcode::CQO:
            cs.emit8(0x48); // REX.W
            cs.emit8(0x99); // CQO
            return;
        case MOpcode::UD2:
            cs.emit8(0x0F);
            cs.emit8(0x0B);
            return;
        default:
            throw std::runtime_error("x86-64 binary encoder: opcode '" +
                                     std::to_string(static_cast<int>(op)) +
                                     "' is not a nullary opcode");
    }
}

// === Reg-Reg GPR ===

/// @copydoc X64BinaryEncoder::encodeRegReg
void X64BinaryEncoder::encodeRegReg(MOpcode op,
                                    PhysReg dst,
                                    PhysReg src,
                                    objfile::CodeSection &cs) {
    const auto info = regRegOpcode(op);
    const auto hwDst = hwEncode(dst);
    const auto hwSrc = hwEncode(src);

    // Determine which register goes in the ModR/M reg vs r/m field.
    const auto &regField = info.regIsDst ? hwDst : hwSrc;
    const auto &rmField = info.regIsDst ? hwSrc : hwDst;

    // 16-bit operand size uses the 0x66 legacy prefix, emitted before REX.
    // MOVSXrr16 does not take it: movswq's source width is opcode-implied and
    // its destination is the full 64-bit register.
    const bool is16 =
        (op == MOpcode::ADDrr16 || op == MOpcode::SUBrr16 || op == MOpcode::IMULrr16);
    if (is16) {
        cs.emit8(0x66);
    }

    // REX.W for 64-bit; selected opcodes intentionally operate at 32-bit or
    // 16-bit width (their flag semantics come from the narrow op). MOVSXD and
    // MOVSXrr16 keep REX.W: they read a narrow source but write the full
    // 64-bit destination.
    bool rexW = (op != MOpcode::XORrr32 && op != MOpcode::MOVZXrr32 && op != MOpcode::ADDrr32 &&
                 op != MOpcode::SUBrr32 && op != MOpcode::IMULrr32 && op != MOpcode::CMPrr32 &&
                 !is16);

    if (needsRex(rexW, regField.rexBit != 0, false, rmField.rexBit != 0)) {
        cs.emit8(computeRex(rexW, regField.rexBit != 0, false, rmField.rexBit != 0));
    }

    // Opcode byte(s).
    cs.emit8(info.primary);
    if (info.secondary != 0) {
        cs.emit8(info.secondary);
    }

    // ModR/M with mod=11 (register direct).
    cs.emit8(makeModRM(0b11, regField.bits3, rmField.bits3));
}

// === Reg-Imm ALU ===

/// @copydoc X64BinaryEncoder::encodeRegImm
void X64BinaryEncoder::encodeRegImm(MOpcode op,
                                    PhysReg dst,
                                    int64_t imm,
                                    objfile::CodeSection &cs,
                                    bool rexW,
                                    bool prefix66) {
    const auto hw = hwEncode(dst);
    uint8_t ext = regImmExt(op);

    // 16-bit operand size: legacy 0x66 prefix precedes REX.
    if (prefix66) {
        cs.emit8(0x66);
    }

    // REX prefix (REX.W only for 64-bit forms).
    if (needsRex(rexW, false, false, hw.rexBit != 0)) {
        cs.emit8(computeRex(rexW, false, false, hw.rexBit != 0));
    }

    // Choose short (83 + imm8) or long (81 + imm32; imm16 under 0x66) form.
    if (imm >= -128 && imm <= 127) {
        cs.emit8(0x83);
        cs.emit8(makeModRM(0b11, ext, hw.bits3));
        cs.emit8(static_cast<uint8_t>(static_cast<int8_t>(imm)));
    } else if (prefix66) {
        if (imm < std::numeric_limits<int16_t>::min() ||
            imm > std::numeric_limits<int16_t>::max()) {
            throw std::runtime_error(
                "x86-64 binary encoder: reg-immediate ALU operand exceeds 16-bit encoding range");
        }
        cs.emit8(0x81);
        cs.emit8(makeModRM(0b11, ext, hw.bits3));
        cs.emit16LE(static_cast<uint16_t>(static_cast<int16_t>(imm)));
    } else {
        if (imm < std::numeric_limits<int32_t>::min() ||
            imm > std::numeric_limits<int32_t>::max()) {
            throw std::runtime_error(
                "x86-64 binary encoder: reg-immediate ALU operand exceeds 32-bit encoding range");
        }
        cs.emit8(0x81);
        cs.emit8(makeModRM(0b11, ext, hw.bits3));
        cs.emit32LE(static_cast<uint32_t>(static_cast<int32_t>(imm)));
    }
}

// === Shift instructions ===

/// @copydoc X64BinaryEncoder::encodeShiftImm
void X64BinaryEncoder::encodeShiftImm(MOpcode op,
                                      PhysReg dst,
                                      int64_t count,
                                      objfile::CodeSection &cs) {
    if (!isGPR(dst))
        throw std::runtime_error("x86-64 binary encoder: shift destination must be a GPR");
    if (count < 0 || count > 63)
        throw std::runtime_error("x86-64 binary encoder: shift count must be in range 0..63");
    const auto hw = hwEncode(dst);
    uint8_t ext = shiftExt(op);

    if (needsRex(true, false, false, hw.rexBit != 0)) {
        cs.emit8(computeRex(true, false, false, hw.rexBit != 0));
    }

    cs.emit8(0xC1); // Shift by imm8.
    cs.emit8(makeModRM(0b11, ext, hw.bits3));
    cs.emit8(static_cast<uint8_t>(count));
}

/// @copydoc X64BinaryEncoder::encodeShiftCL
void X64BinaryEncoder::encodeShiftCL(MOpcode op, PhysReg dst, objfile::CodeSection &cs) {
    if (!isGPR(dst))
        throw std::runtime_error("x86-64 binary encoder: shift destination must be a GPR");
    const auto hw = hwEncode(dst);
    uint8_t ext = shiftExt(op);

    if (needsRex(true, false, false, hw.rexBit != 0)) {
        cs.emit8(computeRex(true, false, false, hw.rexBit != 0));
    }

    cs.emit8(0xD3); // Shift by CL.
    cs.emit8(makeModRM(0b11, ext, hw.bits3));
}

// === Division ===

/// @copydoc X64BinaryEncoder::encodeDiv
void X64BinaryEncoder::encodeDiv(MOpcode op, PhysReg src, objfile::CodeSection &cs) {
    if (!isGPR(src))
        throw std::runtime_error("x86-64 binary encoder: division source must be a GPR");
    const auto hw = hwEncode(src);
    const uint8_t ext = (op == MOpcode::IDIVrm)  ? 7
                        : (op == MOpcode::DIVrm) ? 6
                        : (op == MOpcode::IMULr) ? 5
                                                 : 4;

    if (needsRex(true, false, false, hw.rexBit != 0)) {
        cs.emit8(computeRex(true, false, false, hw.rexBit != 0));
    }

    cs.emit8(0xF7);
    cs.emit8(makeModRM(0b11, ext, hw.bits3));
}

// === MOVri (64-bit immediate move) ===

/// @copydoc X64BinaryEncoder::encodeMovRI
void X64BinaryEncoder::encodeMovRI(PhysReg dst, int64_t imm, objfile::CodeSection &cs) {
    if (!isGPR(dst))
        throw std::runtime_error("x86-64 binary encoder: MOVri destination must be a GPR");
    const auto hw = hwEncode(dst);

    if (imm >= 0 && imm <= 0x7FFFFFFF) {
        // 5-byte form (or 6 with REX.B): B8+rd + imm32 — zero-extends to 64 bits.
        if (hw.rexBit)
            cs.emit8(computeRex(false, false, false, true));
        cs.emit8(static_cast<uint8_t>(0xB8 + hw.bits3));
        cs.emit32LE(static_cast<uint32_t>(imm));
    } else if (imm >= INT32_MIN && imm < 0) {
        // 7-byte form: REX.W + C7 /0 + imm32 — sign-extends to 64 bits.
        cs.emit8(computeRex(true, false, false, hw.rexBit != 0));
        cs.emit8(0xC7);
        cs.emit8(makeModRM(0b11, 0, hw.bits3));
        cs.emit32LE(static_cast<uint32_t>(imm));
    } else {
        // 10-byte form: REX.W + B8+rd + imm64 — full 64-bit.
        cs.emit8(computeRex(true, false, false, hw.rexBit != 0));
        cs.emit8(static_cast<uint8_t>(0xB8 + hw.bits3));
        cs.emit64LE(static_cast<uint64_t>(imm));
    }
}

// === Memory operand encoding ===

/// @copydoc X64BinaryEncoder::emitWithMemOperand
void X64BinaryEncoder::emitWithMemOperand(uint8_t reg3,
                                          uint8_t regRex,
                                          const OpMem &mem,
                                          objfile::CodeSection &cs,
                                          bool rexW,
                                          uint8_t mandatoryPrefix,
                                          uint8_t opByte1,
                                          uint8_t opByte2) {
    validateEncodedMemOperand(mem, "x86-64 binary encoder");
    const auto hwBase = hwEncode(toPhys(mem.base));
    bool hasSIB = mem.hasIndex || hwBase.bits3 == 4; // RSP/R12 encoding needs SIB

    // Determine mod bits.
    uint8_t mod;
    if (mem.disp == 0 && hwBase.bits3 != 5) {
        // mod=00: no displacement. (bits3=5 is RBP/R13 — must use mod=01)
        mod = 0b00;
    } else if (mem.disp >= -128 && mem.disp <= 127) {
        mod = 0b01; // disp8
    } else {
        mod = 0b10; // disp32
    }

    // Compute REX bits.
    uint8_t indexRex = 0;
    if (mem.hasIndex) {
        indexRex = hwEncode(toPhys(mem.index)).rexBit;
    }

    // Emit mandatory prefix (SSE).
    if (mandatoryPrefix != 0) {
        cs.emit8(mandatoryPrefix);
    }

    // Emit REX.
    if (needsRex(rexW, regRex != 0, indexRex != 0, hwBase.rexBit != 0)) {
        cs.emit8(computeRex(rexW, regRex != 0, indexRex != 0, hwBase.rexBit != 0));
    }

    // Emit opcode.
    cs.emit8(opByte1);
    if (opByte2 != 0) {
        cs.emit8(opByte2);
    }

    // Emit ModR/M.
    uint8_t rm3 = hasSIB ? 0b100 : hwBase.bits3;
    cs.emit8(makeModRM(mod, reg3, rm3));

    // Emit SIB if needed.
    if (hasSIB) {
        if (mem.hasIndex) {
            auto hwIdx = hwEncode(toPhys(mem.index));
            cs.emit8(makeSIB(scaleLog2(mem.scale), hwIdx.bits3, hwBase.bits3));
        } else {
            // No index — SIB for RSP/R12 base: scale=0, index=RSP(100), base=base.
            cs.emit8(makeSIB(0, 0b100, hwBase.bits3));
        }
    }

    // Emit displacement.
    if (mod == 0b01) {
        cs.emit8(static_cast<uint8_t>(static_cast<int8_t>(mem.disp)));
    } else if (mod == 0b10) {
        cs.emit32LE(static_cast<uint32_t>(mem.disp));
    }
}

// === Memory store/load ===

/// @copydoc X64BinaryEncoder::encodeMemOp
void X64BinaryEncoder::encodeMemOp(MOpcode op,
                                   PhysReg reg,
                                   const OpMem &mem,
                                   objfile::CodeSection &cs) {
    if (!isGPR(reg))
        throw std::runtime_error("x86-64 binary encoder: memory GPR operand must be a GPR");
    const auto hwReg = hwEncode(reg);
    uint8_t opByte;
    uint8_t opByte2 = 0;

    switch (op) {
        case MOpcode::MOVrm:
            opByte = 0x89;
            break; // store
        case MOpcode::MOVmr:
            opByte = 0x8B;
            break; // load
        case MOpcode::ADDrm:
            opByte = 0x03;
            break; // reg <- reg + [mem]
        case MOpcode::SUBrm:
            opByte = 0x2B;
            break;
        case MOpcode::ANDrm:
            opByte = 0x23;
            break;
        case MOpcode::ORrm:
            opByte = 0x0B;
            break;
        case MOpcode::XORrm:
            opByte = 0x33;
            break;
        case MOpcode::CMPrm:
            opByte = 0x3B;
            break;
        case MOpcode::IMULrm:
            opByte = 0x0F;
            opByte2 = 0xAF;
            break;
        case MOpcode::MOVSXD:
            opByte = 0x63; // movslq reg, [mem]
            break;
        default:
            throw std::runtime_error("x86-64 binary encoder: opcode '" +
                                     std::to_string(static_cast<int>(op)) +
                                     "' is not a memory GPR opcode");
    }

    emitWithMemOperand(hwReg.bits3,
                       hwReg.rexBit,
                       mem,
                       cs,
                       /*rexW=*/true,
                       /*mandatoryPrefix=*/0,
                       opByte,
                       opByte2);
}

// === LEA ===

/// @copydoc X64BinaryEncoder::encodeLEA
void X64BinaryEncoder::encodeLEA(PhysReg dst, const OpMem &mem, objfile::CodeSection &cs) {
    if (!isGPR(dst))
        throw std::runtime_error("x86-64 binary encoder: LEA destination must be a GPR");
    const auto hwDst = hwEncode(dst);
    emitWithMemOperand(hwDst.bits3,
                       hwDst.rexBit,
                       mem,
                       cs,
                       /*rexW=*/true,
                       /*mandatoryPrefix=*/0,
                       0x8D,
                       0);
}

/// @copydoc X64BinaryEncoder::encodeLEARip
void X64BinaryEncoder::encodeLEARip(PhysReg dst,
                                    const OpRipLabel &rip,
                                    objfile::CodeSection &text,
                                    const objfile::CodeSection &rodata,
                                    bool isDarwin) {
    const auto hwDst = hwEncode(dst);
    if (!isGPR(dst))
        throw std::runtime_error("x86-64 binary encoder: LEA RIP destination must be a GPR");

    // REX.W + LEA opcode.
    // REX.W prefix.
    if (needsRex(true, hwDst.rexBit != 0, false, false)) {
        text.emit8(computeRex(true, hwDst.rexBit != 0, false, false));
    }

    // LEA opcode.
    text.emit8(0x8D);

    // ModR/M: mod=00, reg=dst, r/m=101 (RIP-relative).
    text.emit8(makeModRM(0b00, hwDst.bits3, 0b101));

    // Emit placeholder disp32 and record relocation.
    (void)isDarwin;
    std::string symName = rip.name;
    const uint32_t rodataSymIdx = rodata.symbols().find(symName);
    size_t dispOffset = text.currentOffset();
    text.emit32LE(0); // Placeholder.
    if (rodataSymIdx != 0) {
        const auto &rodataSym = rodata.symbols().at(rodataSymIdx);
        text.addSectionOffsetRelocationAt(dispOffset,
                                          objfile::RelocKind::PCRel32,
                                          rodata,
                                          objfile::SymbolSection::Rodata,
                                          rodataSym.offset,
                                          -4);
        return;
    }

    const uint32_t symIdx = text.findOrDeclareSymbol(symName);
    text.addRelocationAt(dispOffset, objfile::RelocKind::PCRel32, symIdx, -4);
}

/// @copydoc X64BinaryEncoder::encodeSseRipLoad
void X64BinaryEncoder::encodeSseRipLoad(PhysReg dst,
                                        const OpRipLabel &rip,
                                        objfile::CodeSection &text,
                                        const objfile::CodeSection &rodata,
                                        bool isDarwin) {
    const auto hwDst = hwEncode(dst);
    if (!isXMM(dst))
        throw std::runtime_error("x86-64 binary encoder: MOVSD RIP destination must be XMM");

    // F2 prefix (mandatory for MOVSD).
    text.emit8(0xF2);
    // REX prefix if dst needs the R extension bit.
    if (hwDst.rexBit != 0) {
        text.emit8(computeRex(false, hwDst.rexBit != 0, false, false));
    }
    // 0F 10 = MOVSD xmm, m64 (load form, reg=dst).
    text.emit8(0x0F);
    text.emit8(0x10);
    // ModR/M: mod=00, reg=dst, r/m=101 (RIP-relative).
    text.emit8(makeModRM(0b00, hwDst.bits3, 0b101));
    // Emit placeholder disp32 and record relocation.
    (void)isDarwin;
    std::string symName = rip.name;
    const uint32_t rodataSymIdx = rodata.symbols().find(symName);
    size_t dispOffset = text.currentOffset();
    text.emit32LE(0); // Placeholder.
    if (rodataSymIdx != 0) {
        const auto &rodataSym = rodata.symbols().at(rodataSymIdx);
        text.addSectionOffsetRelocationAt(dispOffset,
                                          objfile::RelocKind::PCRel32,
                                          rodata,
                                          objfile::SymbolSection::Rodata,
                                          rodataSym.offset,
                                          -4);
        return;
    }

    const uint32_t symIdx = text.findOrDeclareSymbol(symName);
    text.addRelocationAt(dispOffset, objfile::RelocKind::PCRel32, symIdx, -4);
}

// === SSE reg-reg ===

/// @copydoc X64BinaryEncoder::encodeSseRegReg
void X64BinaryEncoder::encodeSseRegReg(MOpcode op,
                                       PhysReg dst,
                                       PhysReg src,
                                       objfile::CodeSection &cs) {
    const bool dstXmm = isXMM(dst);
    const bool srcXmm = isXMM(src);
    const bool dstGpr = isGPR(dst);
    const bool srcGpr = isGPR(src);
    switch (op) {
        case MOpcode::CVTSI2SD:
        case MOpcode::MOVQrx:
            if (!dstXmm || !srcGpr)
                throw std::runtime_error(
                    "x86-64 binary encoder: SSE conversion expects XMM destination and GPR source");
            break;
        case MOpcode::CVTTSD2SI:
        case MOpcode::MOVQxr:
            if (!dstGpr || !srcXmm)
                throw std::runtime_error(
                    "x86-64 binary encoder: SSE conversion expects GPR destination and XMM source");
            break;
        default:
            if (!dstXmm || !srcXmm)
                throw std::runtime_error(
                    "x86-64 binary encoder: SSE register opcode expects XMM operands");
            break;
    }
    const auto info = sseOpcode(op);
    const auto hwDst = hwEncode(dst);
    const auto hwSrc = hwEncode(src);

    const auto &regField = info.regIsDst ? hwDst : hwSrc;
    const auto &rmField = info.regIsDst ? hwSrc : hwDst;

    // Mandatory prefix BEFORE REX.
    if (info.prefix != 0) {
        cs.emit8(info.prefix);
    }

    // REX (if needed).
    if (needsRex(info.needsRexW, regField.rexBit != 0, false, rmField.rexBit != 0)) {
        cs.emit8(computeRex(info.needsRexW, regField.rexBit != 0, false, rmField.rexBit != 0));
    }

    // 0F escape + opcode.
    cs.emit8(0x0F);
    cs.emit8(info.opcode);

    // ModR/M mod=11.
    cs.emit8(makeModRM(0b11, regField.bits3, rmField.bits3));
}

// === SSE memory operations ===

/// @copydoc X64BinaryEncoder::encodeSseMem
void X64BinaryEncoder::encodeSseMem(MOpcode op,
                                    PhysReg reg,
                                    const OpMem &mem,
                                    objfile::CodeSection &cs) {
    if (!isXMM(reg))
        throw std::runtime_error("x86-64 binary encoder: SSE memory register operand must be XMM");
    const auto info = sseOpcode(op);
    const auto hwReg = hwEncode(reg);

    // For SSE memory ops, opByte1=0x0F, opByte2=info.opcode.
    emitWithMemOperand(
        hwReg.bits3, hwReg.rexBit, mem, cs, info.needsRexW, info.prefix, 0x0F, info.opcode);
}

// === SETcc ===

/// @copydoc X64BinaryEncoder::encodeSETcc
void X64BinaryEncoder::encodeSETcc(int condCode, PhysReg dst, objfile::CodeSection &cs) {
    if (!isGPR(dst))
        throw std::runtime_error("x86-64 binary encoder: SETcc destination must be a GPR");
    const auto hw = hwEncode(dst);
    uint8_t cc = x86CC(condCode);

    // SETcc operates on 8-bit registers. A REX prefix is needed in two cases:
    //   1. dst is R8-R15 (hw.rexBit) — to set the REX.B extension bit.
    //   2. dst is RSP/RBP/RSI/RDI (hw.bits3 4-7, rexBit=0) — without REX
    //      these encodings select the legacy high-byte registers AH/CH/DH/BH
    //      instead of SPL/BPL/SIL/DIL.
    const bool needsRexForByte = (hw.bits3 >= 4 && hw.rexBit == 0);
    if (needsRex(false, false, false, hw.rexBit != 0) || needsRexForByte) {
        cs.emit8(computeRex(false, false, false, hw.rexBit != 0));
    }

    cs.emit8(0x0F);
    cs.emit8(static_cast<uint8_t>(0x90 + cc));
    // ModR/M: mod=11, reg=0, r/m=dst (8-bit register, same hardware encoding).
    cs.emit8(makeModRM(0b11, 0, hw.bits3));
}

// === MOVZXrr8 (movzbq) ===

/// @copydoc X64BinaryEncoder::encodeMOVZX
void X64BinaryEncoder::encodeMOVZX(PhysReg dst, PhysReg src, objfile::CodeSection &cs) {
    if (!isGPR(dst) || !isGPR(src))
        throw std::runtime_error("x86-64 binary encoder: MOVZX operands must be GPRs");
    const auto hwDst = hwEncode(dst);
    const auto hwSrc = hwEncode(src);

    // REX.W + 0F B6 + ModR/M(11, dst, src). A REX prefix is also required
    // for SPL/BPL/SIL/DIL as the 8-bit source; without it, encodings 4-7 name
    // AH/CH/DH/BH.
    const bool needsRexForByteSrc = (hwSrc.bits3 >= 4 && hwSrc.rexBit == 0);
    if (needsRex(true, hwDst.rexBit != 0, false, hwSrc.rexBit != 0) || needsRexForByteSrc) {
        cs.emit8(computeRex(true, hwDst.rexBit != 0, false, hwSrc.rexBit != 0));
    }

    cs.emit8(0x0F);
    cs.emit8(0xB6);
    cs.emit8(makeModRM(0b11, hwDst.bits3, hwSrc.bits3));
}

// === Branch/Jump/Call with label ===

/// @copydoc X64BinaryEncoder::encodeBranchLabel
void X64BinaryEncoder::encodeBranchLabel(MOpcode op,
                                         const std::string &label,
                                         int condCode,
                                         objfile::CodeSection &cs) {
    if (label.empty())
        throw std::runtime_error("x86-64 binary encoder: branch target label must not be empty");

    // --- Short-form relaxation (any resolved internal JMP/JCC) ---
    // Short JMP  = 0xEB + rel8 (2 bytes, saves 3 over near form)
    // Short JCC  = 0x7x + rel8 (2 bytes, saves 4 over near form)
    // When the target offset is already known, use rel8 if the displacement
    // fits in a signed byte [-128, 127].
    if (shortBranchRelaxationEnabled_ && (op == MOpcode::JMP || op == MOpcode::JCC)) {
        auto it = labelOffsets_.find(label);
        if (it != labelOffsets_.end()) {
            // Short-form instruction is 2 bytes total: opcode + rel8.
            // IP after instruction = currentOffset + 2.
            const size_t nextIp = checkedAddSize(cs.currentOffset(), 2, "short branch");
            auto disp = checkedOffsetDelta(it->second, nextIp, "short branch");
            if (disp >= -128 && disp <= 127) {
                if (op == MOpcode::JMP)
                    cs.emit8(0xEB); // JMP rel8
                else
                    cs.emit8(static_cast<uint8_t>(0x70 + x86CC(condCode))); // JCC rel8
                cs.emit8(static_cast<uint8_t>(static_cast<int8_t>(disp)));
                return;
            }
        }
    }

    // --- Near-form encoding (JMP rel32 / JCC rel32 / CALL rel32) ---
    size_t patchOffset;

    switch (op) {
        case MOpcode::JMP:
            cs.emit8(0xE9); // JMP rel32
            patchOffset = cs.currentOffset();
            cs.emit32LE(0); // Placeholder.
            break;

        case MOpcode::JCC:
            cs.emit8(0x0F);
            cs.emit8(static_cast<uint8_t>(0x80 + x86CC(condCode)));
            patchOffset = cs.currentOffset();
            cs.emit32LE(0);
            break;

        case MOpcode::CALL:
            cs.emit8(0xE8); // CALL rel32
            patchOffset = cs.currentOffset();
            cs.emit32LE(0);
            break;

        default:
            throw std::runtime_error("x86-64 binary encoder: unexpected branch opcode");
    }

    // Check if target is already known (backward branch, near form).
    auto it = labelOffsets_.find(label);
    if (it != labelOffsets_.end()) {
        const size_t nextIp = checkedAddSize(patchOffset, 4, "branch");
        const int64_t disp = checkedOffsetDelta(it->second, nextIp, "branch");
        const int32_t rel = checkedRel32(disp, "branch");
        cs.patch32LE(patchOffset, static_cast<uint32_t>(rel));
    } else {
        // Forward branch — record for later patching.
        pendingBranches_.push_back({patchOffset, label});
    }
}

// === Branch/Call indirect via register ===

/// @copydoc X64BinaryEncoder::encodeBranchReg
void X64BinaryEncoder::encodeBranchReg(MOpcode op, PhysReg target, objfile::CodeSection &cs) {
    if (!isGPR(target))
        throw std::runtime_error("x86-64 binary encoder: indirect branch target must be a GPR");
    const auto hw = hwEncode(target);
    uint8_t ext = (op == MOpcode::CALL) ? 2 : 4; // /2 for CALL, /4 for JMP

    // REX only needed for R8-R15 targets. No REX.W for indirect branch.
    if (needsRex(false, false, false, hw.rexBit != 0)) {
        cs.emit8(computeRex(false, false, false, hw.rexBit != 0));
    }

    cs.emit8(0xFF);
    cs.emit8(makeModRM(0b11, ext, hw.bits3));
}

// === Branch/Call indirect via memory ===

/// @copydoc X64BinaryEncoder::encodeBranchMem
void X64BinaryEncoder::encodeBranchMem(MOpcode op, const OpMem &mem, objfile::CodeSection &cs) {
    uint8_t ext = (op == MOpcode::CALL) ? 2 : 4; // /2 for CALL, /4 for JMP
    emitWithMemOperand(ext,
                       0,
                       mem,
                       cs,
                       /*rexW=*/false,
                       /*mandatoryPrefix=*/0,
                       0xFF,
                       0);
}

// === Branch/Call indirect via RIP-relative memory ===

/// @copydoc X64BinaryEncoder::encodeBranchRip
void X64BinaryEncoder::encodeBranchRip(MOpcode op,
                                       const OpRipLabel &rip,
                                       objfile::CodeSection &text,
                                       const objfile::CodeSection &rodata,
                                       bool isDarwin) {
    if (op != MOpcode::CALL && op != MOpcode::JMP) {
        throw std::runtime_error("x86-64 binary encoder: RIP branch target requires CALL or JMP");
    }

    const uint8_t ext = (op == MOpcode::CALL) ? 2 : 4; // /2 for CALL, /4 for JMP
    text.emit8(0xFF);
    text.emit8(makeModRM(0b00, ext, 0b101));

    (void)isDarwin;
    const std::string symName = rip.name;
    const uint32_t rodataSymIdx = rodata.symbols().find(symName);
    const size_t dispOffset = text.currentOffset();
    text.emit32LE(0);
    if (rodataSymIdx != 0) {
        const auto &rodataSym = rodata.symbols().at(rodataSymIdx);
        text.addSectionOffsetRelocationAt(dispOffset,
                                          objfile::RelocKind::PCRel32,
                                          rodata,
                                          objfile::SymbolSection::Rodata,
                                          rodataSym.offset,
                                          -4);
        return;
    }

    const uint32_t symIdx = text.findOrDeclareSymbol(symName);
    text.addRelocationAt(dispOffset, objfile::RelocKind::PCRel32, symIdx, -4);
}

// === Branch / call operand-kind dispatcher ===

/// @copydoc X64BinaryEncoder::encodeBranchOperand
void X64BinaryEncoder::encodeBranchOperand(MOpcode op,
                                           const Operand &target,
                                           int condCode,
                                           objfile::CodeSection &cs,
                                           const char *errPrefix) {
    if (std::holds_alternative<OpLabel>(target)) {
        encodeBranchLabel(op, labelFromOperand(target).name, condCode, cs);
    } else if (std::holds_alternative<OpReg>(target)) {
        encodeBranchReg(op, gprFromOperand(target, "branch target"), cs);
    } else if (std::holds_alternative<OpMem>(target)) {
        encodeBranchMem(op, memFromOperand(target), cs);
    } else {
        throw std::runtime_error(std::string{"x86-64 binary encoder: "} + errPrefix);
    }
}

// === External CALL (generates relocation) ===

/// @copydoc X64BinaryEncoder::encodeCallExternal
void X64BinaryEncoder::encodeCallExternal(const std::string &name,
                                          objfile::CodeSection &cs,
                                          bool isDarwin) {
    std::string mapped = mapRuntimeSymbol(name);
    (void)isDarwin;
    std::string symName = mapped;
    uint32_t symIdx = cs.findOrDeclareSymbol(symName);

    cs.emit8(0xE8); // CALL rel32
    size_t dispOffset = cs.currentOffset();
    cs.emit32LE(0); // Placeholder.
    cs.addRelocationAt(dispOffset, objfile::RelocKind::Branch32, symIdx, -4);
}

} // namespace zanna::codegen::x64::binenc
