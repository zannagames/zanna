//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/binenc/A64BinaryEncoder.cpp
// Purpose: AArch64 MIR-to-machine-code binary encoder implementation.
//          Encodes all non-pseudo AArch64 MIR opcodes into 32-bit instruction
//          words, synthesizes prologue/epilogue from function metadata, and
//          resolves internal branches via deferred patching.
// Key invariants:
//   - Every instruction is exactly 4 bytes, emitted via emit32()
//   - Prologue/epilogue follow AsmEmitter.cpp logic exactly
//   - SP adjustments are chunked at 4080 (not 4095) for alignment safety
//   - External BL generates A64Call26 relocation; ADRP/ADD generate page relocs
// Ownership/Lifetime:
//   - State (labelOffsets_, pendingBranches_) is cleared per encodeFunction() call
// Links: src/codegen/aarch64/binenc/A64BinaryEncoder.hpp,
//        src/codegen/aarch64/binenc/A64Encoding.hpp,
//        src/codegen/aarch64/AsmEmitter.cpp
//
//===----------------------------------------------------------------------===//

#include "codegen/aarch64/binenc/A64BinaryEncoder.hpp"

#include "codegen/aarch64/A64ImmediateUtils.hpp"
#include "codegen/aarch64/FrameCodegen.hpp"
#include "codegen/aarch64/binenc/A64Encoding.hpp"
#include "codegen/common/LabelUtil.hpp"
#include "codegen/common/objfile/DebugLineTable.hpp"
#include "il/runtime/RuntimeNameMap.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>

/**
 * @file
 * @brief Implements direct AArch64 MIR-to-machine-code object emission.
 *
 * Encoding is preceded by iterative size/label measurement so conditional
 * branches can be promoted before bytes are emitted. The final pass validates
 * operands, synthesizes ABI frame sequences, records relocations and unwind
 * metadata, and patches internal branches and jump-table entries.
 */

namespace zanna::codegen::aarch64::binenc {

// === Helpers ===

/**
 * @brief Resolves a canonical IL runtime name to its linker symbol.
 * @param name Raw external symbol name.
 * @return Owned mapped runtime name, or an unchanged copy for a user symbol.
 */
static std::string mapRuntimeSymbol(const std::string &name) {
    if (auto mapped = il::runtime::mapCanonicalRuntimeName(name))
        return std::string(*mapped);
    return name;
}

/**
 * @brief Applies the common object/assembler label sanitizer.
 * @param name Raw MIR label.
 * @return Owned sanitized spelling.
 */
static std::string sanitizeLabel(const std::string &name) {
    return zanna::codegen::common::sanitizeLabel(name);
}

/// @brief Normalize a MIR function name for object-file symbol tables.
/// @details Text assembly and binary object emission must agree on public
///          function names. The frontend spelling @c @main is the canonical
///          program entry point and is emitted as @c main; all other names are
///          passed through the common assembler label sanitizer so object
///          writers never receive characters that are invalid in downstream
///          symbol consumers.
/// @param name Raw MIR function name.
/// @return Sanitized object-file symbol name.
static std::string objectFunctionSymbolName(const std::string &name) {
    if (name == "@main")
        return "main";
    return sanitizeLabel(name);
}

/**
 * @brief Validates and extracts a physical register from a MIR operand.
 *
 * @param op Operand expected to be a class-consistent physical register.
 * @return Physical-register enumerator.
 * @throws std::runtime_error If the kind, physicality, numeric range, or class is invalid.
 */
static PhysReg getReg(const MOperand &op) {
    if (op.kind != MOperand::Kind::Reg)
        throw std::runtime_error("AArch64 binary encoder expected register operand, got kind=" +
                                 std::to_string(static_cast<int>(op.kind)));
    if (!op.reg.isPhys)
        throw std::runtime_error("AArch64 binary encoder cannot encode virtual register v" +
                                 std::to_string(op.reg.idOrPhys));
    if (op.reg.idOrPhys > static_cast<uint16_t>(PhysReg::V31))
        throw std::runtime_error("AArch64 binary encoder: physical register id out of range");
    const PhysReg phys = static_cast<PhysReg>(op.reg.idOrPhys);
    const bool classMatches = (op.reg.cls == RegClass::GPR && isGPR(phys)) ||
                              (op.reg.cls == RegClass::FPR && isFPR(phys));
    if (!classMatches) {
        throw std::runtime_error("AArch64 binary encoder: register class does not match "
                                 "physical register");
    }
    return phys;
}

/**
 * @brief Extracts a signed immediate payload.
 * @param op Operand expected to have immediate kind.
 * @return Stored immediate value.
 * @throws std::runtime_error If @p op is not an immediate.
 */
static long long getImm(const MOperand &op) {
    if (op.kind != MOperand::Kind::Imm)
        throw std::runtime_error("AArch64 binary encoder expected immediate operand, got kind=" +
                                 std::to_string(static_cast<int>(op.kind)));
    return op.imm;
}

/**
 * @brief Extracts an owned label by reference.
 * @param op Operand expected to have label kind.
 * @return Const reference into @p op.
 * @throws std::runtime_error If @p op is not a label.
 */
static const std::string &getLabel(const MOperand &op) {
    if (op.kind != MOperand::Kind::Label)
        throw std::runtime_error("AArch64 binary encoder expected label operand, got kind=" +
                                 std::to_string(static_cast<int>(op.kind)));
    return op.label;
}

/**
 * @brief Extracts, validates, and sanitizes a non-empty label operand.
 * @param op Label operand to normalize.
 * @param context Non-null diagnostic context.
 * @return Owned non-empty sanitized label.
 * @throws std::runtime_error If the raw or sanitized label is empty or the
 *         operand kind is invalid.
 */
static std::string getSanitizedNonEmptyLabel(const MOperand &op, const char *context) {
    const std::string &label = getLabel(op);
    if (label.empty())
        throw std::runtime_error(std::string(context) + " label must not be empty");
    const std::string sanitized = sanitizeLabel(label);
    if (sanitized.empty())
        throw std::runtime_error(std::string(context) + " label sanitizes to an empty name");
    return sanitized;
}

/**
 * @brief Adds two signed offsets with explicit overflow detection.
 * @param lhs First addend.
 * @param rhs Second addend.
 * @param context Non-null diagnostic context.
 * @return Exact signed sum.
 * @throws std::runtime_error If the sum is outside `int64_t`.
 */
static int64_t checkedAddI64(int64_t lhs, int64_t rhs, const char *context) {
    if ((rhs > 0 && lhs > std::numeric_limits<int64_t>::max() - rhs) ||
        (rhs < 0 && lhs < std::numeric_limits<int64_t>::min() - rhs)) {
        throw std::runtime_error(std::string(context) + " immediate addition overflows int64");
    }
    return lhs + rhs;
}

/**
 * @brief Computes a signed displacement between two unsigned section offsets.
 * @param target Target byte offset.
 * @param base PC/base byte offset.
 * @param context Non-null diagnostic context.
 * @return Exact `target - base` in `int64_t`, including `INT64_MIN`.
 * @throws std::runtime_error If the displacement magnitude exceeds `int64_t`.
 */
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

/**
 * @brief Extracts an architectural condition accepted by conditional instructions.
 * @param op Condition operand.
 * @return Encoded condition in `[0, 13]`.
 * @throws std::exception If the operand/mnemonic is invalid or names `al`/`nv`.
 */
static uint32_t getCondCode(const MOperand &op) {
    if (op.kind != MOperand::Kind::Cond)
        throw std::runtime_error("AArch64 binary encoder expected condition operand, got kind=" +
                                 std::to_string(static_cast<int>(op.kind)));
    const uint32_t cc = condCode(op.cond);
    if (cc >= 0xE)
        throw std::invalid_argument(
            "AArch64 binary encoder: condition code is not valid for conditional instruction");
    return cc;
}

/// @brief Throw if @p mi has a different operand count than @p expected.
/// @details Operand-count mismatches indicate a lowering bug; throw a descriptive
///          `std::runtime_error` rather than silently emitting garbage bytes.
/// @param mi       Instruction whose operand vector is being checked.
/// @param expected Required operand count.
/// @throws std::runtime_error if the operand count does not match.
static void requireOperandCount(const MInstr &mi, size_t expected) {
    if (mi.ops.size() != expected) {
        throw std::runtime_error("AArch64 binary encoder: opcode '" +
                                 std::string(opcodeName(mi.opc)) + "' requires exactly " +
                                 std::to_string(expected) + " operand(s) but has " +
                                 std::to_string(mi.ops.size()));
    }
}

/// @brief Validate that @p mi has the operand count its opcode requires.
/// @details Dispatches on `mi.opc` to look up the architectural operand count and
///          delegates to @ref requireOperandCount. Called from `encodeInstruction`
///          as a defence-in-depth check before any field encoding runs.
/// @param mi Machine instruction whose opcode determines the required count.
/// @throws std::runtime_error if the operand count does not match the opcode's contract.
static void validateOperandCount(const MInstr &mi) {
    switch (mi.opc) {
        case MOpcode::Ret:
            requireOperandCount(mi, 0);
            return;
        case MOpcode::SubSpImm:
        case MOpcode::AddSpImm:
        case MOpcode::Br:
        case MOpcode::Bl:
        case MOpcode::Blr:
            requireOperandCount(mi, 1);
            return;
        case MOpcode::MovRR:
        case MOpcode::MovRI:
        case MOpcode::FMovRR:
        case MOpcode::FMovRI:
        case MOpcode::FMovGR:
        case MOpcode::FCmpRR:
        case MOpcode::SCvtF:
        case MOpcode::FCvtZS:
        case MOpcode::UCvtF:
        case MOpcode::FCvtZU:
        case MOpcode::FRintN:
        case MOpcode::StrRegSpImm:
        case MOpcode::StrFprSpImm:
        case MOpcode::LdrRegFpImm:
        case MOpcode::StrRegFpImm:
        case MOpcode::Ldr8RegFpImm:
        case MOpcode::Str8RegFpImm:
        case MOpcode::Ldr16RegFpImm:
        case MOpcode::Str16RegFpImm:
        case MOpcode::Ldr32RegFpImm:
        case MOpcode::Str32RegFpImm:
        case MOpcode::LdrFprFpImm:
        case MOpcode::StrFprFpImm:
        case MOpcode::PhiStoreGPR:
        case MOpcode::PhiStoreFPR:
        case MOpcode::AddFpImm:
        case MOpcode::Cbz:
        case MOpcode::Cbnz:
        case MOpcode::CmpRR:
        case MOpcode::CmpRI:
        case MOpcode::TstRR:
        case MOpcode::Cset:
        case MOpcode::BCond:
        case MOpcode::AdrPage:
            requireOperandCount(mi, 2);
            return;
        case MOpcode::FAddRRR:
        case MOpcode::FSubRRR:
        case MOpcode::FMulRRR:
        case MOpcode::FDivRRR:
        case MOpcode::LdrRegBaseImm:
        case MOpcode::StrRegBaseImm:
        case MOpcode::Ldr8RegBaseImm:
        case MOpcode::Str8RegBaseImm:
        case MOpcode::Ldr16RegBaseImm:
        case MOpcode::Str16RegBaseImm:
        case MOpcode::Ldr32RegBaseImm:
        case MOpcode::Str32RegBaseImm:
        case MOpcode::LdrFprBaseImm:
        case MOpcode::StrFprBaseImm:
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
        case MOpcode::AndRI:
        case MOpcode::OrrRI:
        case MOpcode::EorRI:
        case MOpcode::AddRI:
        case MOpcode::SubRI:
        case MOpcode::LslRI:
        case MOpcode::LsrRI:
        case MOpcode::AsrRI:
        case MOpcode::LslvRRR:
        case MOpcode::LsrvRRR:
        case MOpcode::AsrvRRR:
        case MOpcode::AddPageOff:
        case MOpcode::LdpRegFpImm:
        case MOpcode::StpRegFpImm:
        case MOpcode::LdpFprFpImm:
        case MOpcode::StpFprFpImm:
        case MOpcode::AddsRRR:
        case MOpcode::SubsRRR:
        case MOpcode::AddsRI:
        case MOpcode::SubsRI:
        case MOpcode::AddOvfRRR:
        case MOpcode::SubOvfRRR:
        case MOpcode::AddOvfRI:
        case MOpcode::SubOvfRI:
        case MOpcode::MulOvfRRR:
        case MOpcode::Tbz:
        case MOpcode::Tbnz:
            requireOperandCount(mi, 3);
            return;
        case MOpcode::JumpTable:
            // Variadic: index, table name, then >=1 case label.
            if (mi.ops.size() < 3) {
                throw std::runtime_error(
                    "AArch64 binary encoder: JumpTable needs an index, a table name, "
                    "and at least one case");
            }
            return;
        case MOpcode::MSubRRRR:
        case MOpcode::MAddRRRR:
        case MOpcode::Csel:
        case MOpcode::FCsel:
        case MOpcode::LdrRegBaseRegLsl:
        case MOpcode::StrRegBaseRegLsl:
        case MOpcode::Ldr32RegBaseRegLsl:
        case MOpcode::Str32RegBaseRegLsl:
        case MOpcode::LdrFprBaseRegLsl:
        case MOpcode::StrFprBaseRegLsl:
        case MOpcode::AddRRRLsl:
        case MOpcode::SubRRRLsl:
        case MOpcode::AndRRRLsl:
        case MOpcode::OrrRRRLsl:
        case MOpcode::EorRRRLsl:
            requireOperandCount(mi, 4);
            return;
    }
}

/// @brief Convert a signed `int64_t` magnitude to `uint32_t` with bounds-check.
/// @details Returns `|value|` clipped against the 32-bit unsigned range. Used by
///          immediate-encoding paths that fold sign elsewhere and need the magnitude
///          as the architectural unsigned field. Throws rather than truncating so
///          a caller bug surfaces as a real error instead of garbage bits.
/// @param value   Signed 64-bit immediate.
/// @param context Short human-readable description of the field for the error message.
/// @return Unsigned 32-bit magnitude of @p value.
/// @throws std::out_of_range if `|value|` exceeds `UINT32_MAX`.
static uint32_t checkedU32Magnitude(int64_t value, const char *context) {
    const uint64_t magnitude = absImmUnsigned(value);
    if (magnitude > std::numeric_limits<uint32_t>::max())
        throw std::out_of_range(std::string("AArch64 binary encoder: ") + context +
                                " magnitude exceeds 32-bit helper range");
    return static_cast<uint32_t>(magnitude);
}

/// @brief Convert a signed `int64_t` non-negative value to `uint32_t` with bounds-check.
/// @details Used where a field must be a non-negative value and must fit `uint32_t`.
/// @param value   Signed 64-bit value (must be `>= 0`).
/// @param context Short human-readable description of the field for the error message.
/// @return Unsigned 32-bit value of @p value.
/// @throws std::out_of_range if @p value is negative or exceeds `UINT32_MAX`.
static uint32_t checkedU32NonNegative(int64_t value, const char *context) {
    if (value < 0 || value > std::numeric_limits<uint32_t>::max())
        throw std::out_of_range(std::string("AArch64 binary encoder: ") + context +
                                " is outside 32-bit unsigned range");
    return static_cast<uint32_t>(value);
}

/// @brief Convert a `size_t` function length to `uint32_t` with bounds-check.
/// @details Mach-O compact-unwind records and similar metadata store function lengths
///          as 32-bit values. Throws with the function name on overflow so very large
///          synthesized functions are flagged at emission time.
/// @param value        Function byte length.
/// @param functionName Symbol name for the error message.
/// @return @p value narrowed to `uint32_t`.
/// @throws std::out_of_range if @p value exceeds `UINT32_MAX`.
static uint32_t checkedFunctionLength(size_t value, const std::string &functionName) {
    if (value > std::numeric_limits<uint32_t>::max()) {
        throw std::out_of_range("AArch64 binary encoder: function '" + functionName +
                                "' exceeds 32-bit compact-unwind length range");
    }
    return static_cast<uint32_t>(value);
}

/// @brief Add two `size_t` values, throwing on overflow.
/// @details Used when computing offsets / sizes that combine multiple contributions.
/// @param a       First addend.
/// @param b       Second addend.
/// @param context Short description for the error message on overflow.
/// @return `a + b`.
/// @throws std::length_error if `a + b` would overflow `size_t`.
static size_t checkedAddSize(size_t a, size_t b, const char *context) {
    if (a > std::numeric_limits<size_t>::max() - b)
        throw std::length_error(std::string("AArch64 binary encoder: ") + context +
                                " exceeds addressable size");
    return a + b;
}

/// @brief Validate and narrow a shift amount to its architectural 6-bit field.
/// @details AArch64 shift instructions encode the amount in 6 bits; values outside
///          `[0, 63]` are architecturally undefined. Throws on out-of-range so a
///          lowering bug doesn't quietly produce a corrupt encoding.
/// @param value  Shift amount from the MIR immediate operand.
/// @param opcode Mnemonic for the error message.
/// @return @p value cast to `uint32_t`.
/// @throws std::out_of_range if @p value is outside `[0, 63]`.
static uint32_t checkedShiftAmount(long long value, const char *opcode) {
    if (!isValidShiftAmount(value))
        throw std::out_of_range(std::string("AArch64 ") + opcode +
                                " shift amount must be in range 0..63");
    return static_cast<uint32_t>(value);
}

/// @brief Sanity-check an `MFunction`'s frame metadata before emission.
/// @details Rejects negative or non-16-byte-aligned frame sizes and any
///          saved-register list that includes registers outside the architectural
///          callee-saved range (`X19`-`X28` for GPRs, similarly bounded for FPRs)
///          or has duplicate entries. Failures here would corrupt the prologue
///          and epilogue stack layout, so they're surfaced before any byte emission.
/// @param fn Machine function whose metadata is being validated.
/// @throws std::out_of_range on any invariant violation.
static void validateFunctionMetadata(const MFunction &fn) {
    if (fn.localFrameSize < 0)
        throw std::out_of_range("AArch64 binary encoder: negative local frame size");
    if ((fn.localFrameSize % 16) != 0)
        throw std::out_of_range("AArch64 binary encoder: local frame size must be 16-byte aligned");
    std::unordered_set<PhysReg> seenSaved;
    for (PhysReg reg : fn.savedGPRs) {
        if (!isGPR(reg) || reg < PhysReg::X19 || reg > PhysReg::X28)
            throw std::out_of_range(
                "AArch64 binary encoder: saved GPR list must contain only X19-X28");
        if (!seenSaved.insert(reg).second)
            throw std::out_of_range("AArch64 binary encoder: duplicate saved GPR");
    }
    seenSaved.clear();
    for (PhysReg reg : fn.savedFPRs) {
        if (reg < PhysReg::V8 || reg > PhysReg::V15)
            throw std::out_of_range(
                "AArch64 binary encoder: saved FPR list must contain only V8-V15");
        if (!seenSaved.insert(reg).second)
            throw std::out_of_range("AArch64 binary encoder: duplicate saved FPR");
    }
}

/// @brief Encode a callee-saved GPR as its Windows ARM64 unwind save index.
/// @details The `.xdata` save_regp/save_reg unwind codes index callee-saved
///          GPRs as 0..9 for X19..X28 (per the MS ARM64 exception-handling
///          ABI). Only X19-X28 are valid here.
/// @param reg Callee-saved GPR to encode.
/// @return Zero-based index in [0,9].
/// @throws std::out_of_range If @p reg is outside `X19..X28`.
static uint8_t windowsArm64GprSaveIndex(PhysReg reg) {
    if (reg < PhysReg::X19 || reg > PhysReg::X28)
        throw std::out_of_range("AArch64 Windows unwind: GPR save must be X19-X28");
    return static_cast<uint8_t>(static_cast<int>(reg) - static_cast<int>(PhysReg::X19));
}

/// @brief Encode a callee-saved FPR as its Windows ARM64 unwind save index.
/// @details Mirror of windowsArm64GprSaveIndex() for the D8..D15 callee-saved
///          FP range, indexed 0..7 in the save_fregp/save_freg unwind codes.
/// @param reg Callee-saved FP register to encode.
/// @return Zero-based index in [0,7].
/// @throws std::out_of_range If @p reg is outside `V8..V15`.
static uint8_t windowsArm64FprSaveIndex(PhysReg reg) {
    if (reg < PhysReg::V8 || reg > PhysReg::V15)
        throw std::out_of_range("AArch64 Windows unwind: FPR save must be V8-V15");
    return static_cast<uint8_t>(static_cast<int>(reg) - static_cast<int>(PhysReg::V8));
}

/// @brief Append the Windows ARM64 stack-allocation unwind code for @p bytes.
/// @details Emits one of the three `alloc_s` / `alloc_m` / `alloc_l` SEH
///          unwind-code forms depending on size (16-byte units): the 1-byte
///          small form (<512), the 2-byte medium form (<32768), or the 4-byte
///          large form. @p bytes must be 16-byte aligned per the ABI.
/// @param codes Unwind-code byte vector to append to (big-endian opcode order).
/// @param bytes Prologue stack-allocation size in bytes (0 = no code emitted).
/// @throws std::out_of_range If @p bytes is unaligned or exceeds the unwind format.
static void appendWindowsArm64AllocCode(std::vector<uint8_t> &codes, uint32_t bytes) {
    if (bytes == 0)
        return;
    if ((bytes % 16) != 0)
        throw std::out_of_range("AArch64 Windows unwind: stack allocation must be 16-byte aligned");
    const uint32_t units = bytes / 16;
    if (bytes < 512) {
        codes.push_back(static_cast<uint8_t>(units));
        return;
    }
    if (bytes < 32768) {
        codes.push_back(static_cast<uint8_t>(0xC0u | ((units >> 8) & 0x7u)));
        codes.push_back(static_cast<uint8_t>(units & 0xFFu));
        return;
    }
    if (units > 0x00FFFFFFu)
        throw std::out_of_range("AArch64 Windows unwind: stack allocation exceeds xdata range");
    codes.push_back(0xE0u);
    codes.push_back(static_cast<uint8_t>(units & 0xFFu));
    codes.push_back(static_cast<uint8_t>((units >> 8) & 0xFFu));
    codes.push_back(static_cast<uint8_t>((units >> 16) & 0xFFu));
}

/// @brief Append a `save_regp_x` unwind code for a callee-saved GPR pair with
///        16-byte pre-decrement (the STP Xn,Xn+1,[sp,#-16]! prologue form).
/// @param codes Unwind-code byte vector to append to.
/// @param first First (lower-numbered) register of the saved X-pair.
static void appendWindowsArm64SaveGprPairX(std::vector<uint8_t> &codes, PhysReg first) {
    const uint8_t x = windowsArm64GprSaveIndex(first);
    constexpr uint8_t zForPredec16 = 1;
    codes.push_back(static_cast<uint8_t>(0xCCu | ((x >> 2) & 0x3u)));
    codes.push_back(static_cast<uint8_t>(((x & 0x3u) << 6) | zForPredec16));
}

/// @brief Append a `save_reg_x` unwind code for a single callee-saved GPR with
///        16-byte pre-decrement (the STR Xn,[sp,#-16]! prologue form).
/// @param codes Unwind-code byte vector to append to.
/// @param reg   The callee-saved X register being saved.
static void appendWindowsArm64SaveGprX(std::vector<uint8_t> &codes, PhysReg reg) {
    const uint8_t x = windowsArm64GprSaveIndex(reg);
    constexpr uint8_t zForPredec16 = 1;
    codes.push_back(static_cast<uint8_t>(0xD4u | ((x >> 3) & 0x1u)));
    codes.push_back(static_cast<uint8_t>(((x & 0x7u) << 5) | zForPredec16));
}

/// @brief Append a `save_fregp_x` unwind code for a callee-saved FPR pair with
///        16-byte pre-decrement (the STP Dn,Dn+1,[sp,#-16]! prologue form).
/// @param codes Unwind-code byte vector to append to.
/// @param first First (lower-numbered) register of the saved D-pair.
static void appendWindowsArm64SaveFprPairX(std::vector<uint8_t> &codes, PhysReg first) {
    const uint8_t x = windowsArm64FprSaveIndex(first);
    constexpr uint8_t zForPredec16 = 1;
    codes.push_back(static_cast<uint8_t>(0xDAu | ((x >> 2) & 0x1u)));
    codes.push_back(static_cast<uint8_t>(((x & 0x3u) << 6) | zForPredec16));
}

/// @brief Append a `save_freg_x` unwind code for a single callee-saved FPR with
///        16-byte pre-decrement (the STR Dn,[sp,#-16]! prologue form).
/// @param codes Unwind-code byte vector to append to.
/// @param reg   The callee-saved D register being saved.
static void appendWindowsArm64SaveFprX(std::vector<uint8_t> &codes, PhysReg reg) {
    const uint8_t x = windowsArm64FprSaveIndex(reg);
    constexpr uint8_t zForPredec16 = 1;
    codes.push_back(0xDEu);
    codes.push_back(static_cast<uint8_t>(((x & 0x7u) << 5) | zForPredec16));
}

/// @brief Test whether @p offset fits the signed 9-bit unscaled `LDUR`/`STUR` immediate.
/// @details The unscaled immediate is `simm9` with a range of `[-256, 255]`. Callers
///          use this to choose between the unscaled form (for negative or unaligned
///          offsets) and the scaled `LDR`/`STR` form.
/// @param offset Byte offset to test.
/// @return True if @p offset is encodable as `simm9`.
static bool isInSignedImmRange(long long offset) {
    return offset >= -256 && offset <= 255;
}

/// @brief Return true if @p offset is encodable as an unsigned 12-bit scaled STR/LDR immediate.
/// AArch64 unsigned-offset LDR/STR (64-bit) encodes imm12 * 8; legal range is [0, 32760] step 8.
/// @param offset Candidate byte offset.
/// @return `true` when one 64-bit scaled access can encode @p offset.
static bool isLegalScaledUImm64(long long offset) {
    return offset >= 0 && (offset % 8) == 0 && (offset / 8) <= 4095;
}

/// @brief Width-generic form of isLegalScaledUImm64(): true if @p offset is a
///        legal scaled unsigned-12 immediate for an @p accessBytes-wide LDR/STR.
/// @details The scaled form encodes `offset / accessBytes` in a 12-bit field, so
///          @p offset must be non-negative, a multiple of @p accessBytes, and
///          `offset / accessBytes <= 4095`.
/// @param offset Candidate byte offset.
/// @param accessBytes Access-size scale; zero is rejected.
/// @return `true` when the scaled unsigned field can represent @p offset.
static bool isLegalScaledUImm(long long offset, unsigned accessBytes) {
    if (accessBytes == 0)
        return false;
    return offset >= 0 && (offset % static_cast<long long>(accessBytes)) == 0 &&
           (offset / static_cast<long long>(accessBytes)) <= 4095;
}

/// @brief Encoded size of a scalar LDR/STR at @p offset, or 0 if it needs the
///        large-offset (scratch-address) sequence instead.
/// @details Returns 4 when @p offset fits either the unscaled simm9 or the
///          scaled-uimm12 form (a single 4-byte instruction); 0 signals the
///          caller to fall back to a multi-instruction address materialization.
/// @param offset Candidate byte offset.
/// @param accessBytes Scalar access width.
/// @return Four for a single-instruction form, otherwise zero.
static size_t scalarLdStSizeForOffset(int64_t offset, unsigned accessBytes) {
    return (isInSignedImmRange(offset) || isLegalScaledUImm(offset, accessBytes)) ? 4 : 0;
}

/// @brief True if @p offset fits the signed scaled 7-bit immediate of LDP/STP.
/// @details Pair load/store encodes `offset / 8` as a signed 7-bit field, so
///          @p offset must be a multiple of 8 within `[-512, 504]`.
/// @param offset Candidate byte offset.
/// @return `true` when @p offset is aligned and within the pair range.
static bool isPairImm7Offset(int64_t offset) {
    if ((offset % 8) != 0)
        return false;
    const int64_t scaled = offset / 8;
    return scaled >= -64 && scaled <= 63;
}

/// @brief Base instruction word for a scaled unsigned-offset GPR load/store.
/// @param isLoad     True selects the LDR family, false the STR family.
/// @param accessBytes Access width (1/2/4/8) selecting the byte/half/word/dword form.
/// @return The 32-bit template to OR register/immediate fields into.
/// @throws std::runtime_error for an unsupported width.
static uint32_t scaledGprLdStTemplate(bool isLoad, unsigned accessBytes) {
    switch (accessBytes) {
        case 1:
            return isLoad ? kLdr8Gpr : kStr8Gpr;
        case 2:
            return isLoad ? kLdr16Gpr : kStr16Gpr;
        case 4:
            return isLoad ? kLdr32Gpr : kStr32Gpr;
        case 8:
            return isLoad ? kLdrGpr : kStrGpr;
        default:
            throw std::runtime_error("AArch64 binary encoder: unsupported GPR load/store width");
    }
}

/// @brief Base instruction word for an unscaled (LDUR/STUR) GPR load/store.
/// @details Counterpart of scaledGprLdStTemplate() used for negative or
///          unaligned offsets that fit the signed simm9 field.
/// @param isLoad      True selects the LDUR family, false the STUR family.
/// @param accessBytes Access width (1/2/4/8).
/// @return The 32-bit template to OR register/immediate fields into.
/// @throws std::runtime_error for an unsupported width.
static uint32_t unscaledGprLdStTemplate(bool isLoad, unsigned accessBytes) {
    switch (accessBytes) {
        case 1:
            return isLoad ? kLdur8Gpr : kStur8Gpr;
        case 2:
            return isLoad ? kLdur16Gpr : kStur16Gpr;
        case 4:
            return isLoad ? kLdur32Gpr : kStur32Gpr;
        case 8:
            return isLoad ? kLdurGpr : kSturGpr;
        default:
            throw std::runtime_error("AArch64 binary encoder: unsupported GPR load/store width");
    }
}

/// @brief Select a scratch GPR that is not @p base and not @p avoid.
/// @details Tries kScratchGPR (x9), kScratchGPR2 (x16), kScratchGPR3 (x17) in priority order.
/// Throws if all three scratch registers conflict (indicates a register-allocation bug).
/// @param base Hardware register field that must not be selected.
/// @param avoid Optional second hardware register field to exclude.
/// @return Hardware field of the first available reserved scratch GPR.
/// @throws std::runtime_error If all reserved scratch registers conflict.
static uint32_t chooseGprScratch(uint32_t base, std::optional<uint32_t> avoid = std::nullopt) {
    const uint32_t candidates[] = {hwGPR(kScratchGPR), hwGPR(kScratchGPR2), hwGPR(kScratchGPR3)};
    for (uint32_t candidate : candidates) {
        if (candidate == base)
            continue;
        if (avoid.has_value() && candidate == *avoid)
            continue;
        return candidate;
    }
    throw std::runtime_error(
        "AArch64 binary encoder: no scratch register for large offset load/store");
}

/// @brief Convert a byte offset to a signed 7-bit scaled immediate for STP/LDP pair encoding.
/// @details The AArch64 pair-encoding field is imm7 * 8; valid range is [-512, 504] step 8.
/// Throws std::out_of_range if the offset violates alignment or exceeds the field range.
/// @param offset Byte offset to encode.
/// @param opcode Human-readable opcode name used in the exception message.
/// @return Signed scaled immediate in `[-64, 63]`.
/// @throws std::out_of_range If @p offset is unaligned or out of range.
static int32_t checkedPairImm7(int64_t offset, const char *opcode) {
    if ((offset % 8) != 0)
        throw std::out_of_range(std::string("AArch64 ") + opcode +
                                " offset must be 8-byte aligned");
    const int64_t scaled = offset / 8;
    if (scaled < -64 || scaled > 63)
        throw std::out_of_range(std::string("AArch64 ") + opcode +
                                " offset is outside signed imm7 pair range");
    return static_cast<int32_t>(scaled);
}

/// @brief Convert a byte displacement to a word-count displacement and validate it fits.
/// @details Divides @p deltaBytes by 4 (instruction words) and checks the result fits in
///          a signed @p immBits-wide field. Throws std::runtime_error on misalignment or range
///          overflow, embedding @p kind, @p target, @p fnName, and @p rangeDesc in the message.
/// @param deltaBytes    Signed byte distance from current PC to target.
/// @param immBits       Width of the branch immediate field (19 for B.cond/CBZ, 26 for B/BL).
/// @param kind          Human-readable branch opcode name for the error message.
/// @param rangeDesc     Human-readable range string (e.g., "±1 MiB") for the error message.
/// @param target        Target label name for the error message.
/// @param fnName        Enclosing function name for the error message.
/// @return Signed word-count displacement that fits in immBits.
/// @throws std::runtime_error If the byte displacement is unaligned or out of range.
static int32_t checkedBranchDispWords(int64_t deltaBytes,
                                      int immBits,
                                      const char *kind,
                                      const char *rangeDesc,
                                      const std::string &target,
                                      const std::string &fnName) {
    if ((deltaBytes & 0x3) != 0) {
        throw std::runtime_error("AArch64 binary encoder: " + std::string(kind) + " target '" +
                                 target + "' in function '" + fnName + "' is not 4-byte aligned");
    }

    const int64_t deltaWords = deltaBytes / 4;
    const int64_t min = -(int64_t{1} << (immBits - 1));
    const int64_t max = (int64_t{1} << (immBits - 1)) - 1;
    if (deltaWords < min || deltaWords > max) {
        throw std::runtime_error("AArch64 binary encoder: " + std::string(kind) + " target '" +
                                 target + "' in function '" + fnName + "' exceeds " + rangeDesc);
    }

    return static_cast<int32_t>(deltaWords);
}

/// @brief Non-throwing predicate: return true if @p deltaBytes fits in an immBits-wide branch
/// field.
/// @details Used during the label-offset fixup pass to choose between the short (4-byte)
///          and long (8-byte trampoline) conditional branch forms before final emission.
/// @param deltaBytes Signed byte displacement.
/// @param immBits Signed architectural immediate width.
/// @return `true` when aligned and representable.
static bool fitsBranchDispWords(int64_t deltaBytes, int immBits) {
    if ((deltaBytes & 0x3) != 0)
        return false;

    const int64_t deltaWords = deltaBytes / 4;
    const int64_t min = -(int64_t{1} << (immBits - 1));
    const int64_t max = (int64_t{1} << (immBits - 1)) - 1;
    return deltaWords >= min && deltaWords <= max;
}

/**
 * @brief Measures the minimal move-wide sequence for a 64-bit constant.
 * @param imm Exact constant bit pattern.
 * @return Four bytes per instruction selected by `forEachMoveWideInst()`.
 */
size_t A64BinaryEncoder::movImm64Size(uint64_t imm) const {
    size_t count = 0;
    forEachMoveWideInst(imm, [&](const MoveWideInst &) { ++count; });
    return count * 4;
}

/**
 * @brief Measures the add/sub immediate strategy used by final emission.
 * @param value Non-negative magnitude to encode.
 * @return Byte size of a direct, shifted, split, or chunked sequence.
 */
size_t A64BinaryEncoder::addSubImmSmartSize(uint32_t value) const {
    if (value <= 4095)
        return 4;
    if ((value & 0xFFF) == 0 && (value >> 12) <= 4095)
        return 4;

    const uint32_t hi = value >> 12;
    const uint32_t lo = value & 0xFFF;
    if (hi > 0 && hi <= 4095)
        return lo > 0 ? 8 : 4;

    size_t bytes = 0;
    while (value > 4095) {
        bytes += 4;
        value -= 4080;
    }
    if (value > 0)
        bytes += 4;
    return bytes;
}

/**
 * @brief Measures scratch-address materialization plus one scalar access.
 * @param offset Signed offset materialized as a 64-bit constant.
 * @return Encoded sequence size in bytes.
 */
size_t A64BinaryEncoder::largeOffsetLdStSize(int64_t offset) const {
    return movImm64Size(static_cast<uint64_t>(offset)) + 8; // add scratch + ldr/str
}

/**
 * @brief Measures an SP-relative store and any scratch-address setup.
 * @param offset Signed destination byte offset from `SP`.
 * @return Encoded sequence size in bytes.
 */
size_t A64BinaryEncoder::spOffsetStoreSize(int64_t offset) const {
    if (isLegalScaledUImm64(offset))
        return 4;

    size_t bytes = 8; // mov scratch, sp + final store
    if (offset > 0)
        bytes += addSubImmSmartSize(checkedU32Magnitude(offset, "SP store offset"));
    else if (offset < 0)
        bytes += addSubImmSmartSize(checkedU32Magnitude(offset, "SP store offset"));
    return bytes;
}

/**
 * @brief Measures the canonical prologue selected for a function.
 * @param fn Function frame and saved-register metadata.
 * @return Prologue byte count under `currentAbi_`.
 */
size_t A64BinaryEncoder::prologueSize(const MFunction &fn) const {
    size_t size = 0;
    if (currentAbi_ == ABIFormat::Darwin)
        size += 4; // paciasp
    size += 8;     // stp fp/lr + mov fp, sp
    if (fn.localFrameSize > 0)
        size += addSubImmSmartSize(checkedU32NonNegative(fn.localFrameSize, "local frame size"));
    size += ((fn.savedGPRs.size() + 1) / 2) * 4;
    size += ((fn.savedFPRs.size() + 1) / 2) * 4;
    return size;
}

/**
 * @brief Measures the canonical epilogue selected for a function.
 * @param fn Function frame and saved-register metadata.
 * @return Epilogue byte count under `currentAbi_`.
 */
size_t A64BinaryEncoder::epilogueSize(const MFunction &fn) const {
    size_t size = 0;
    size += ((fn.savedFPRs.size() + 1) / 2) * 4;
    size += ((fn.savedGPRs.size() + 1) / 2) * 4;
    if (fn.localFrameSize > 0)
        size += addSubImmSmartSize(checkedU32NonNegative(fn.localFrameSize, "local frame size"));
    size += 4; // ldp fp/lr
    if (currentAbi_ == ABIFormat::Darwin)
        size += 4; // autiasp
    size += 4;     // ret
    return size;
}

/**
 * @brief Measures BTI and optional frame setup before the first block label.
 * @param fn Function being measured.
 * @return Prelude byte count.
 */
size_t A64BinaryEncoder::measurePreludeSize(const MFunction &fn) {
    size_t size = 0;
    if (currentAbi_ == ABIFormat::Darwin)
        size += 4; // BTI landing pad
    if (!skipFrame_)
        size += prologueSize(fn);
    return size;
}

/**
 * @brief Measures one MIR instruction for iterative label-offset convergence.
 *
 * @param mi Instruction to validate and measure.
 * @param currentOffset Function-relative instruction offset.
 * @param knownLabelOffsets Current function-relative label hypothesis.
 * @param instructionOrdinal Stable MIR instruction ordinal.
 * @param assumedLongConditionalBranches Ordinals already promoted to long form.
 * @param[out] discoveredLongConditionalBranches Optional newly promoted ordinals.
 * @return Predicted encoded byte size.
 */
size_t A64BinaryEncoder::measureInstructionSize(
    const MInstr &mi,
    size_t currentOffset,
    const LabelOffsetMap &knownLabelOffsets,
    size_t instructionOrdinal,
    const std::unordered_set<size_t> &assumedLongConditionalBranches,
    std::unordered_set<size_t> *discoveredLongConditionalBranches) {
    validateOperandCount(mi);

    auto conditionalBranchSize = [&](const std::string &target, unsigned dispBits) {
        if (target.empty())
            throw std::runtime_error(
                "AArch64 binary encoder: conditional branch label must not be empty");
        if (assumedLongConditionalBranches.count(instructionOrdinal) != 0) {
            if (discoveredLongConditionalBranches)
                discoveredLongConditionalBranches->insert(instructionOrdinal);
            return size_t{8};
        }
        auto it = knownLabelOffsets.find(sanitizeLabel(target));
        if (it == knownLabelOffsets.end())
            return size_t{4};
        const int64_t delta = checkedOffsetDelta(it->second, currentOffset, "conditional branch");
        if (fitsBranchDispWords(delta, dispBits))
            return size_t{4};
        if (discoveredLongConditionalBranches)
            discoveredLongConditionalBranches->insert(instructionOrdinal);
        return size_t{8};
    };

    // Single source of truth for "given a load/store opcode, where is its
    // immediate offset and how many bytes does it access?" This matches the
    // shape consumed by encodeFpRelLdStInstr / encodeBaseRelLdStInstr, so the
    // measurement loop and the emission loop cannot drift apart per-opcode.
    struct LdStInfo {
        size_t offsetOpIndex;
        unsigned bytes;
    };

    auto classifyLdSt = [](MOpcode opc) -> std::optional<LdStInfo> {
        switch (opc) {
            // FP-relative, single-width-8.
            case MOpcode::LdrRegFpImm:
            case MOpcode::PhiStoreGPR:
            case MOpcode::StrRegFpImm:
            case MOpcode::LdrFprFpImm:
            case MOpcode::PhiStoreFPR:
            case MOpcode::StrFprFpImm:
                return LdStInfo{1, 8};
            // FP-relative narrow widths.
            case MOpcode::Ldr8RegFpImm:
            case MOpcode::Str8RegFpImm:
                return LdStInfo{1, 1};
            case MOpcode::Ldr16RegFpImm:
            case MOpcode::Str16RegFpImm:
                return LdStInfo{1, 2};
            case MOpcode::Ldr32RegFpImm:
            case MOpcode::Str32RegFpImm:
                return LdStInfo{1, 4};
            // Base-relative, single-width-8.
            case MOpcode::LdrRegBaseImm:
            case MOpcode::StrRegBaseImm:
            case MOpcode::LdrFprBaseImm:
            case MOpcode::StrFprBaseImm:
                return LdStInfo{2, 8};
            // Base-relative narrow widths.
            case MOpcode::Ldr8RegBaseImm:
            case MOpcode::Str8RegBaseImm:
                return LdStInfo{2, 1};
            case MOpcode::Ldr16RegBaseImm:
            case MOpcode::Str16RegBaseImm:
                return LdStInfo{2, 2};
            case MOpcode::Ldr32RegBaseImm:
            case MOpcode::Str32RegBaseImm:
                return LdStInfo{2, 4};
            default:
                return std::nullopt;
        }
    };

    if (const auto info = classifyLdSt(mi.opc)) {
        const long long offset = getImm(mi.ops[info->offsetOpIndex]);
        return scalarLdStSizeForOffset(offset, info->bytes) != 0 ? size_t{4}
                                                                 : largeOffsetLdStSize(offset);
    }

    switch (mi.opc) {
        case MOpcode::Ret:
            return skipFrame_ ? 4 : epilogueSize(*currentFn_);

        case MOpcode::MovRI: {
            const long long imm = getImm(mi.ops[1]);
            return needsWideImmSequence(imm) ? movImm64Size(static_cast<uint64_t>(imm)) : 4;
        }

        case MOpcode::CmpRI: {
            const long long imm = getImm(mi.ops[1]);
            if ((imm >= 0 && imm <= 4095) || (imm >= -4095 && imm < 0))
                return 4;
            return movImm64Size(static_cast<uint64_t>(imm)) + 4;
        }

        case MOpcode::LdpRegFpImm:
        case MOpcode::StpRegFpImm:
        case MOpcode::LdpFprFpImm:
        case MOpcode::StpFprFpImm:
            if (isPairImm7Offset(getImm(mi.ops[2])))
                return 4;
            return (scalarLdStSizeForOffset(getImm(mi.ops[2]), 8) != 0
                        ? 4
                        : largeOffsetLdStSize(getImm(mi.ops[2]))) +
                   (scalarLdStSizeForOffset(
                        checkedAddI64(getImm(mi.ops[2]), 8, "AArch64 pair fallback offset"), 8) != 0
                        ? 4
                        : largeOffsetLdStSize(
                              checkedAddI64(getImm(mi.ops[2]), 8, "AArch64 pair fallback offset")));

        case MOpcode::SubSpImm:
        case MOpcode::AddSpImm:
            return addSubImmSmartSize(checkedU32NonNegative(getImm(mi.ops[0]), opcodeName(mi.opc)));

        case MOpcode::StrRegSpImm:
        case MOpcode::StrFprSpImm:
            return spOffsetStoreSize(static_cast<int64_t>(getImm(mi.ops[1])));

        case MOpcode::AddFpImm: {
            const long long offset = getImm(mi.ops[1]);
            const uint64_t magnitude = absImmUnsigned(offset);
            return classifyAddSubImmEncoding(magnitude).has_value() ? 4
                                                                    : movImm64Size(magnitude) + 4;
        }

        case MOpcode::AndRI:
        case MOpcode::OrrRI:
        case MOpcode::EorRI: {
            const auto imm = static_cast<uint64_t>(getImm(mi.ops[2]));
            return encodeLogicalImmediate(imm) >= 0 ? 4 : movImm64Size(imm) + 4;
        }

        case MOpcode::FMovRI: {
            double val;
            std::memcpy(&val, &mi.ops[1].imm, sizeof(val));
            if (encodeFP8Immediate(val) >= 0)
                return 4;
            uint64_t bits;
            std::memcpy(&bits, &val, sizeof(bits));
            return movImm64Size(bits) + 4;
        }

        case MOpcode::BCond:
            return conditionalBranchSize(getLabel(mi.ops[1]), 19);
        case MOpcode::Cbz:
        case MOpcode::Cbnz:
            return conditionalBranchSize(getLabel(mi.ops[1]), 19);
        case MOpcode::Tbz:
        case MOpcode::Tbnz:
            // tbz/tbnz reach only +/-32KB (imm14); relax to the long form sooner.
            return conditionalBranchSize(getLabel(mi.ops[1]), 14);

        case MOpcode::JumpTable:
            // adr + ldrsw + add + br, then one 4-byte word per case label.
            return 16 + 4 * (mi.ops.size() - 2);

        default:
            return 4;
    }
}

/**
 * @brief Iterates instruction sizes until block offsets and long branches stabilize.
 * @param fn Function whose sanitized block labels are measured.
 * @return Function-relative label offsets.
 * @throws std::exception If labels duplicate, sizes overflow, or convergence fails.
 */
A64BinaryEncoder::LabelOffsetMap A64BinaryEncoder::computeFunctionLabelOffsets(
    const MFunction &fn) {
    LabelOffsetMap estimated;
    std::unordered_set<size_t> longConditionalBranches;

    const size_t maxIterations = 16;
    for (size_t iter = 0; iter < maxIterations; ++iter) {
        bool changed = false;
        LabelOffsetMap known = estimated;
        LabelOffsetMap next;
        std::unordered_set<size_t> nextLongConditionalBranches;
        next.reserve(estimated.size() + fn.blocks.size());

        auto assignLabel = [&](const std::string &name, size_t offset) {
            if (name.empty())
                return;
            const std::string sanitized = sanitizeLabel(name);
            if (sanitized.empty())
                throw std::runtime_error("AArch64 binary encoder: label '" + name +
                                         "' sanitizes to an empty name in function '" + fn.name +
                                         "'");
            if (next.find(sanitized) != next.end()) {
                throw std::runtime_error("AArch64 binary encoder: duplicate/sanitized label '" +
                                         sanitized + "' in function '" + fn.name + "'");
            }
            auto prevIt = estimated.find(sanitized);
            if (prevIt == estimated.end() || prevIt->second != offset)
                changed = true;
            known[sanitized] = offset;
            next[sanitized] = offset;
        };

        size_t offset = measurePreludeSize(fn);
        size_t ordinal = 0;
        for (const auto &bb : fn.blocks) {
            if (!bb.name.empty())
                assignLabel(bb.name, offset);
            for (const auto &mi : bb.instrs) {
                offset = checkedAddSize(offset,
                                        measureInstructionSize(mi,
                                                               offset,
                                                               known,
                                                               ordinal,
                                                               longConditionalBranches,
                                                               &nextLongConditionalBranches),
                                        "estimated function size");
                ++ordinal;
            }
        }

        lastEstimatedFunctionSize_ = offset;
        if (!changed && next.size() == estimated.size() &&
            nextLongConditionalBranches == longConditionalBranches) {
            longConditionalBranchOrdinals_ = std::move(nextLongConditionalBranches);
            return next;
        }
        estimated = std::move(next);
        longConditionalBranches = std::move(nextLongConditionalBranches);
    }

    std::unordered_set<size_t> allLongConditionalBranches;
    size_t ordinal = 0;
    for (const auto &bb : fn.blocks) {
        for (const auto &mi : bb.instrs) {
            if (mi.opc == MOpcode::BCond || mi.opc == MOpcode::Cbz || mi.opc == MOpcode::Cbnz ||
                mi.opc == MOpcode::Tbz || mi.opc == MOpcode::Tbnz)
                allLongConditionalBranches.insert(ordinal);
            ++ordinal;
        }
    }

    LabelOffsetMap conservative;
    LabelOffsetMap known;
    size_t offset = measurePreludeSize(fn);
    ordinal = 0;
    for (const auto &bb : fn.blocks) {
        if (!bb.name.empty()) {
            const std::string sanitized = sanitizeLabel(bb.name);
            known[sanitized] = offset;
            conservative[sanitized] = offset;
        }
        for (const auto &mi : bb.instrs) {
            offset =
                checkedAddSize(offset,
                               measureInstructionSize(
                                   mi, offset, known, ordinal, allLongConditionalBranches, nullptr),
                               "estimated function size");
            ++ordinal;
        }
    }
    lastEstimatedFunctionSize_ = offset;
    longConditionalBranchOrdinals_ = std::move(allLongConditionalBranches);
    return conservative;
}

/**
 * @brief Estimates total encoded size for a fixed label-offset map.
 * @param fn Function to measure.
 * @param knownLabelOffsets Current relative label offsets.
 * @return Total byte count including prelude and instruction expansions.
 */
size_t A64BinaryEncoder::estimateFunctionSize(const MFunction &fn,
                                              const LabelOffsetMap &knownLabelOffsets) {
    size_t size = measurePreludeSize(fn);
    size_t ordinal = 0;
    for (const auto &bb : fn.blocks) {
        for (const auto &mi : bb.instrs) {
            size = checkedAddSize(
                size,
                measureInstructionSize(
                    mi, size, knownLabelOffsets, ordinal, longConditionalBranchOrdinals_, nullptr),
                "estimated function size");
            ++ordinal;
        }
    }
    return size;
}

/**
 * @brief Checks a final absolute label offset against the precomputed map.
 * @param label Sanitized block label.
 * @param actualOffset Actual text-section offset.
 * @throws std::runtime_error If a stored prediction differs.
 */
void A64BinaryEncoder::verifyPredictedLabelOffset(const std::string &label,
                                                  size_t actualOffset) const {
    auto it = labelOffsets_.find(label);
    if (it == labelOffsets_.end())
        return;
    if (it->second != actualOffset) {
        throw std::runtime_error("AArch64 binary encoder: label offset drift for '" + label +
                                 "' (predicted=" + std::to_string(it->second) +
                                 ", actual=" + std::to_string(actualOffset) + ")");
    }
}

// =============================================================================
// encodeFunction
// =============================================================================

/**
 * @brief Encodes, patches, and records metadata for one complete MIR function.
 *
 * @param fn Validated, allocated MIR function.
 * @param[in,out] text Destination text section.
 * @param rodata Read-only section used by address fixups.
 * @param abi Target object ABI.
 * @throws std::exception On invalid metadata/operands, unresolved labels,
 *         unencodable ranges, or size prediction drift.
 */
void A64BinaryEncoder::encodeFunction(const MFunction &fn,
                                      objfile::CodeSection &text,
                                      const objfile::CodeSection &rodata,
                                      ABIFormat abi) {
    labelOffsets_.clear();
    pendingBranches_.clear();
    pendingTableEntries_.clear();
    longConditionalBranchOrdinals_.clear();
    currentInstructionOrdinal_ = 0;
    lastEstimatedFunctionSize_ = 0;
    currentFn_ = &fn;
    currentAbi_ = abi;
    currentRodata_ = &rodata;

    try {
        validateFunctionMetadata(fn);

        // Leaf function optimization: skip frame when no calls, no callee-saved, no locals.
        skipFrame_ =
            fn.isLeaf && fn.savedGPRs.empty() && fn.savedFPRs.empty() && fn.localFrameSize == 0;
        usePlan_ = !fn.savedGPRs.empty() || !fn.savedFPRs.empty() || fn.localFrameSize > 0;

        // Define function symbol at current offset.
        const size_t funcStartOffset = text.currentOffset();
        const auto relativeLabelOffsets = computeFunctionLabelOffsets(fn);
        const size_t estimatedSize = lastEstimatedFunctionSize_ != 0
                                         ? lastEstimatedFunctionSize_
                                         : estimateFunctionSize(fn, relativeLabelOffsets);
        text.reserveAdditionalBytes(estimatedSize);
        labelOffsets_.clear();
        labelOffsets_.reserve(relativeLabelOffsets.size());
        for (const auto &[label, offset] : relativeLabelOffsets)
            labelOffsets_[label] = funcStartOffset + offset;

        size_t branchCount = 0;
        for (const auto &bb : fn.blocks) {
            for (const auto &mi : bb.instrs) {
                if (mi.opc == MOpcode::Br || mi.opc == MOpcode::BCond || mi.opc == MOpcode::Cbz ||
                    mi.opc == MOpcode::Cbnz || mi.opc == MOpcode::Tbz ||
                    mi.opc == MOpcode::Tbnz || mi.opc == MOpcode::Bl)
                    ++branchCount;
            }
        }
        pendingBranches_.reserve(branchCount);

        const uint32_t funcSymIdx = text.defineSymbol(objectFunctionSymbolName(fn.name),
                                                      objfile::SymbolBinding::Global,
                                                      objfile::SymbolSection::Text);

        // Emit BTI landing pad for indirect call targets (safe NOP on pre-ARMv8.5).
        if (currentAbi_ == ABIFormat::Darwin)
            emit32(kBtiC, text);

        // Emit prologue.
        if (!skipFrame_)
            encodePrologue(fn, text);

        // Encode all blocks.
        size_t ordinal = 0;
        for (const auto &bb : fn.blocks) {
            if (!bb.name.empty()) {
                const std::string label = sanitizeLabel(bb.name);
                if (label.empty())
                    throw std::runtime_error(
                        "AArch64 binary encoder: block label sanitizes to empty");
                verifyPredictedLabelOffset(label, text.currentOffset());
                auto existing = labelOffsets_.find(label);
                if (existing != labelOffsets_.end() && existing->second != text.currentOffset()) {
                    throw std::runtime_error("AArch64 binary encoder: duplicate emitted label '" +
                                             label + "' in function '" + fn.name + "'");
                }
                labelOffsets_[label] = text.currentOffset();
            }

            for (const auto &mi : bb.instrs) {
                if (debugLines_ && mi.loc.hasLine())
                    debugLines_->addEntry(
                        text.currentOffset(), mi.loc.file_id, mi.loc.line, mi.loc.column);
                currentInstructionOrdinal_ = ordinal++;
                encodeInstruction(mi, text);
            }
        }

        // Resolve pending internal branches.
        for (const auto &pb : pendingBranches_) {
            auto it = labelOffsets_.find(pb.target);
            if (it == labelOffsets_.end()) {
                throw std::runtime_error(
                    "AArch64 binary encoder: unresolved internal branch target '" + pb.target +
                    "' in function '" + fn.name + "'");
            }

            const size_t targetOff = it->second;
            const int64_t delta = checkedOffsetDelta(targetOff, pb.offset, "internal branch");

            uint32_t word = text.read32LE(pb.offset);

            if (pb.kind == MOpcode::Br || pb.kind == MOpcode::Bl) {
                const int32_t imm26 = checkedBranchDispWords(
                    delta, 26, "branch", "the +/-128MB B/BL range", pb.target, fn.name);
                word &= ~0x03FFFFFFu;
                word |= (static_cast<uint32_t>(imm26) & 0x3FFFFFF);
            } else if (pb.kind == MOpcode::Tbz || pb.kind == MOpcode::Tbnz) {
                const int32_t imm14 = checkedBranchDispWords(delta,
                                                             14,
                                                             "conditional branch",
                                                             "the +/-32KB tbz/tbnz range",
                                                             pb.target,
                                                             fn.name);
                word &= ~(0x3FFFu << 5);
                word |= ((static_cast<uint32_t>(imm14) & 0x3FFF) << 5);
            } else {
                const int32_t imm19 = checkedBranchDispWords(delta,
                                                             19,
                                                             "conditional branch",
                                                             "the +/-1MB conditional-branch range",
                                                             pb.target,
                                                             fn.name);
                word &= ~(0x7FFFFu << 5);
                word |= ((static_cast<uint32_t>(imm19) & 0x7FFFF) << 5);
            }

            text.patch32LE(pb.offset, word);
        }

        // Resolve jump-table entry words: offset(case) - tableStart.
        for (const auto &te : pendingTableEntries_) {
            auto it = labelOffsets_.find(te.caseLabel);
            if (it == labelOffsets_.end()) {
                throw std::runtime_error(
                    "AArch64 binary encoder: unresolved jump-table target '" + te.caseLabel +
                    "' in function '" + fn.name + "'");
            }
            const int64_t delta = checkedOffsetDelta(it->second, te.tableStart, "jump-table entry");
            text.patch32LE(te.patchOffset, static_cast<uint32_t>(static_cast<int32_t>(delta)));
        }

        const size_t actualSize = text.currentOffset() - funcStartOffset;
        if (actualSize != estimatedSize) {
            throw std::runtime_error("AArch64 binary encoder: function size drift for '" + fn.name +
                                     "' (predicted=" + std::to_string(estimatedSize) +
                                     ", actual=" + std::to_string(actualSize) + ")");
        }

        if (abi == ABIFormat::Darwin) {
            // Mach-O compact unwind is Darwin-specific. ELF and COFF object
            // writers must not inherit these entries.
            //
            // Apple's compact-unwind format (compact_unwind_encoding.h) keeps
            // the mode in bits [27:24] and, for UNWIND_ARM64_MODE_FRAME,
            // describes saved callee-saved registers as per-pair FLAG bits in
            // bits [8:0] (X19/X20=0x1 ... X27/X28=0x10, D8/D9=0x100 ...
            // D14/D15=0x800) stored at canonical slots immediately below the
            // FP/LR pair. Our prologue stores callee-saved pairs below the
            // locals area instead, so those flags would point the unwinder at
            // the wrong stack slots. Emit plain MODE_FRAME with no pair flags:
            // the unwinder restores FP/LR and walks the frame chain correctly
            // and simply skips callee-saved register recovery.
            const uint32_t funcLen =
                checkedFunctionLength(text.currentOffset() - funcStartOffset, fn.name);
            objfile::CompactUnwindEntry entry{};
            entry.symbolIndex = funcSymIdx;
            entry.functionLength = funcLen;
            entry.encoding = skipFrame_ ? kUnwindArm64ModeFrameless : kUnwindArm64ModeFrame;
            text.addUnwindEntry(entry);
        } else if (abi == ABIFormat::Windows && !skipFrame_) {
            const uint32_t funcLen =
                checkedFunctionLength(text.currentOffset() - funcStartOffset, fn.name);
            recordWindowsArm64UnwindEntry(fn, funcSymIdx, funcLen, text);
        }

        currentFn_ = nullptr;
        currentRodata_ = nullptr;
    } catch (...) {
        currentFn_ = nullptr;
        currentRodata_ = nullptr;
        throw;
    }
}

// =============================================================================
// Prologue/Epilogue Synthesis
// =============================================================================

/**
 * @brief Emits the canonical ABI prologue described by function metadata.
 * @param fn Function frame size and saved-register lists.
 * @param[in,out] cs Text section receiving instruction words.
 */
void A64BinaryEncoder::encodePrologue(const MFunction &fn, objfile::CodeSection &cs) {
    const uint32_t sp = hwGPR(PhysReg::SP);
    const uint32_t fp = hwGPR(PhysReg::X29);
    const uint32_t lr = hwGPR(PhysReg::X30);

    // Step emitter for binary output — each step corresponds one-to-one with a
    // call in FrameCodegen.hpp::iteratePrologue.
    struct Steps {
        A64BinaryEncoder &self;
        objfile::CodeSection &cs;
        uint32_t sp{0};
        uint32_t fp{0};
        uint32_t lr{0};

        /// @brief Bind binary prologue emission state to frame-codegen callbacks.
        /// @param encoder Encoder that owns the low-level instruction helpers.
        /// @param section Destination text section receiving encoded instructions.
        /// @param stackPointer Hardware register number for SP.
        /// @param framePointer Hardware register number for FP/X29.
        /// @param linkRegister Hardware register number for LR/X30.
        Steps(A64BinaryEncoder &encoder,
              objfile::CodeSection &section,
              uint32_t stackPointer,
              uint32_t framePointer,
              uint32_t linkRegister)
            : self(encoder), cs(section), sp(stackPointer), fp(framePointer), lr(linkRegister) {}

        void paciasp() const {
            self.emit32(kPaciasp, cs);
        }

        void stpFpLrPre() const {
            self.emit32(encodePair(kStpGprPre, fp, lr, sp, static_cast<int32_t>(-16 / 8)), cs);
        }

        void movFpSp() const {
            self.emit32(encodeAddSubImm(kAddRI, fp, sp, 0), cs);
        }

        void subSp(int32_t n) const {
            self.encodeSubSp(n, cs);
        }

        void stpGprPair(PhysReg r0, PhysReg r1) const {
            self.emit32(
                encodePair(kStpGprPre, hwGPR(r0), hwGPR(r1), sp, static_cast<int32_t>(-16 / 8)),
                cs);
        }

        void strGprSingle(PhysReg r0) const {
            self.emit32(kStrGprPre | ((static_cast<uint32_t>(-16) & 0x1FF) << 12) | (sp << 5) |
                            hwGPR(r0),
                        cs);
        }

        void stpFprPair(PhysReg r0, PhysReg r1) const {
            self.emit32(
                encodePair(kStpFprPre, hwFPR(r0), hwFPR(r1), sp, static_cast<int32_t>(-16 / 8)),
                cs);
        }

        void strFprSingle(PhysReg r0) const {
            self.emit32(kStrFprPre | ((static_cast<uint32_t>(-16) & 0x1FF) << 12) | (sp << 5) |
                            hwFPR(r0),
                        cs);
        }
    };

    iteratePrologue(fn.savedGPRs,
                    fn.savedFPRs,
                    fn.localFrameSize,
                    currentAbi_ == ABIFormat::Darwin,
                    Steps(*this, cs, sp, fp, lr));
}

/**
 * @brief Emits saved-register restoration, frame teardown, and return.
 * @param fn Function frame size and saved-register lists.
 * @param[in,out] cs Text section receiving instruction words.
 */
void A64BinaryEncoder::encodeEpilogue(const MFunction &fn, objfile::CodeSection &cs) {
    const uint32_t sp = hwGPR(PhysReg::SP);
    const uint32_t fp = hwGPR(PhysReg::X29);
    const uint32_t lr = hwGPR(PhysReg::X30);

    struct Steps {
        A64BinaryEncoder &self;
        objfile::CodeSection &cs;
        uint32_t sp{0};
        uint32_t fp{0};
        uint32_t lr{0};

        /// @brief Bind binary epilogue emission state to frame-codegen callbacks.
        /// @param encoder Encoder that owns the low-level instruction helpers.
        /// @param section Destination text section receiving encoded instructions.
        /// @param stackPointer Hardware register number for SP.
        /// @param framePointer Hardware register number for FP/X29.
        /// @param linkRegister Hardware register number for LR/X30.
        Steps(A64BinaryEncoder &encoder,
              objfile::CodeSection &section,
              uint32_t stackPointer,
              uint32_t framePointer,
              uint32_t linkRegister)
            : self(encoder), cs(section), sp(stackPointer), fp(framePointer), lr(linkRegister) {}

        void ldpFprPair(PhysReg r0, PhysReg r1) const {
            self.emit32(
                encodePair(kLdpFprPost, hwFPR(r0), hwFPR(r1), sp, static_cast<int32_t>(16 / 8)),
                cs);
        }

        void ldrFprSingle(PhysReg r0) const {
            self.emit32(kLdrFprPost | ((16u & 0x1FF) << 12) | (sp << 5) | hwFPR(r0), cs);
        }

        void ldpGprPair(PhysReg r0, PhysReg r1) const {
            self.emit32(
                encodePair(kLdpGprPost, hwGPR(r0), hwGPR(r1), sp, static_cast<int32_t>(16 / 8)),
                cs);
        }

        void ldrGprSingle(PhysReg r0) const {
            self.emit32(kLdrGprPost | ((16u & 0x1FF) << 12) | (sp << 5) | hwGPR(r0), cs);
        }

        void addSp(int32_t n) const {
            self.encodeAddSp(n, cs);
        }

        void ldpFpLrPost() const {
            self.emit32(encodePair(kLdpGprPost, fp, lr, sp, static_cast<int32_t>(16 / 8)), cs);
        }

        void autiasp() const {
            self.emit32(kAutiasp, cs);
        }

        void ret() const {
            self.emit32(kRet, cs);
        }
    };

    iterateEpilogue(fn.savedGPRs,
                    fn.savedFPRs,
                    fn.localFrameSize,
                    currentAbi_ == ABIFormat::Darwin,
                    Steps(*this, cs, sp, fp, lr));
}

/**
 * @brief Serializes Windows ARM64 unwind codes matching the canonical prologue.
 * @param fn Function whose frame operations are described.
 * @param funcSymIdx Function symbol index.
 * @param functionLength Encoded byte length.
 * @param[in,out] cs Section receiving the unwind entry and xdata bytes.
 */
void A64BinaryEncoder::recordWindowsArm64UnwindEntry(const MFunction &fn,
                                                     uint32_t funcSymIdx,
                                                     uint32_t functionLength,
                                                     objfile::CodeSection &cs) const {
    std::vector<std::vector<uint8_t>> forwardOps;
    forwardOps.reserve(16);

    const auto oneOp = [](auto emit) {
        std::vector<uint8_t> op;
        emit(op);
        return op;
    };

    const auto appendAllocOp = [&](uint32_t bytes) {
        forwardOps.push_back(
            oneOp([&](std::vector<uint8_t> &op) { appendWindowsArm64AllocCode(op, bytes); }));
    };

    const auto appendAllocOpsForAddSubSmart = [&](uint32_t bytes) {
        if (bytes <= 4095) {
            appendAllocOp(bytes);
        } else if ((bytes & 0xFFFu) == 0 && (bytes >> 12) <= 4095) {
            appendAllocOp(bytes);
        } else {
            uint32_t hi = bytes >> 12;
            uint32_t lo = bytes & 0xFFFu;
            if (hi > 0 && hi <= 4095) {
                appendAllocOp(hi << 12);
                if (lo > 0)
                    appendAllocOp(lo);
            } else {
                while (bytes > 4095) {
                    appendAllocOp(4080);
                    bytes -= 4080;
                }
                if (bytes > 0)
                    appendAllocOp(bytes);
            }
        }
    };

    // stp x29, x30, [sp, #-16]!
    forwardOps.push_back({0x81u}); // save_fplr_x, pre-indexed -16
    forwardOps.push_back({0xE3u}); // nop for mov x29, sp

    if (fn.localFrameSize > 0)
        appendAllocOpsForAddSubSmart(checkedU32NonNegative(fn.localFrameSize, "local frame size"));

    forEachSaveReg(
        fn.savedGPRs,
        [&](PhysReg r0, PhysReg) {
            forwardOps.push_back(
                oneOp([&](std::vector<uint8_t> &op) { appendWindowsArm64SaveGprPairX(op, r0); }));
        },
        [&](PhysReg r0) {
            forwardOps.push_back(
                oneOp([&](std::vector<uint8_t> &op) { appendWindowsArm64SaveGprX(op, r0); }));
        });

    forEachSaveReg(
        fn.savedFPRs,
        [&](PhysReg r0, PhysReg) {
            forwardOps.push_back(
                oneOp([&](std::vector<uint8_t> &op) { appendWindowsArm64SaveFprPairX(op, r0); }));
        },
        [&](PhysReg r0) {
            forwardOps.push_back(
                oneOp([&](std::vector<uint8_t> &op) { appendWindowsArm64SaveFprX(op, r0); }));
        });

    size_t unwindCodeSize = 1; // trailing end opcode
    for (const auto &op : forwardOps)
        unwindCodeSize += op.size();
    std::vector<uint8_t> unwindCodes;
    unwindCodes.reserve(unwindCodeSize);
    for (auto it = forwardOps.rbegin(); it != forwardOps.rend(); ++it)
        unwindCodes.insert(unwindCodes.end(), it->begin(), it->end());
    unwindCodes.push_back(0xE4u); // end

    objfile::WinArm64UnwindEntry entry{};
    entry.symbolIndex = funcSymIdx;
    entry.functionLength = functionLength;
    const size_t prologueBytes = prologueSize(fn);
    if (prologueBytes > std::numeric_limits<uint8_t>::max()) {
        throw std::runtime_error("AArch64 binary encoder: Windows ARM64 prologue for '" + fn.name +
                                 "' exceeds 255 bytes");
    }
    entry.prologueSize = static_cast<uint8_t>(prologueBytes);
    entry.unwindCodes = std::move(unwindCodes);
    entry.packedEpilogInHeader = true;
    entry.epilogCodeIndex = 0;
    cs.addWinArm64UnwindEntry(std::move(entry));
}

// =============================================================================
// Multi-instruction sequences
// =============================================================================

/**
 * @brief Emits the minimal move-wide instruction sequence for a constant.
 * @param rd Destination GPR hardware field.
 * @param imm Exact constant bits.
 * @param[in,out] cs Text section receiving words.
 */
void A64BinaryEncoder::encodeMovImm64(uint32_t rd, uint64_t imm, objfile::CodeSection &cs) {
    // Templates indexed by halfword position (0=lsl #0, 1=lsl #16, 2=lsl #32, 3=lsl #48).
    static constexpr uint32_t movzTmpl[4] = {kMovZ, kMovZ16, kMovZ32, kMovZ48};
    static constexpr uint32_t movnTmpl[4] = {kMovN, kMovN16, kMovN32, kMovN48};
    static constexpr uint32_t movkTmpl[4] = {kMovK, kMovK16, kMovK32, kMovK48};

    forEachMoveWideInst(imm, [&](const MoveWideInst &inst) {
        const unsigned lane = inst.shift / 16;
        switch (inst.opcode) {
            case MoveWideOpcode::MovZ:
                emit32(movzTmpl[lane] | (static_cast<uint32_t>(inst.imm16) << 5) | rd, cs);
                break;
            case MoveWideOpcode::MovN:
                emit32(movnTmpl[lane] | (static_cast<uint32_t>(inst.imm16) << 5) | rd, cs);
                break;
            case MoveWideOpcode::MovK:
                emit32(movkTmpl[lane] | (static_cast<uint32_t>(inst.imm16) << 5) | rd, cs);
                break;
        }
    });
}

/// @brief Emit one or more add/sub-immediate instructions to apply @p val using @p tmpl.
/// @details Uses a single instruction when val fits in 12 bits; a single lsl #12 instruction
///          when val is a multiple of 4096; two instructions (shifted + unshifted parts) for
///          values that combine both; or a chunked loop at steps of 4080 for very large values.
/// @param tmpl Base encoding template (e.g. ADD or SUB base word with size/S bits pre-set).
/// @param rd   Destination register index.
/// @param rn   Source register index for the first instruction.
/// @param val  Unsigned immediate to apply (must be ≤ 0xFFFFFF for two-instruction form).
/// @param cs   CodeSection to emit instruction words into.
static void emitAddSubImmSmart(
    uint32_t tmpl, uint32_t rd, uint32_t rn, uint32_t val, objfile::CodeSection &cs) {
    if (val <= 4095) {
        cs.emit32LE(encodeAddSubImm(tmpl, rd, rn, val));
    } else if ((val & 0xFFF) == 0 && (val >> 12) <= 4095) {
        cs.emit32LE(encodeAddSubImmShift(tmpl, rd, rn, val >> 12));
    } else {
        // Split into shifted + unshifted parts.
        uint32_t hi = val >> 12;
        uint32_t lo = val & 0xFFF;
        if (hi > 0 && hi <= 4095) {
            cs.emit32LE(encodeAddSubImmShift(tmpl, rd, rn, hi));
            if (lo > 0)
                cs.emit32LE(encodeAddSubImm(tmpl, rd, rd, lo));
        } else {
            // Fallback: loop with max immediate.
            while (val > 4095) {
                cs.emit32LE(encodeAddSubImm(tmpl, rd, rn, 4080));
                val -= 4080;
                rn = rd; // subsequent iterations operate on rd
            }
            if (val > 0)
                cs.emit32LE(encodeAddSubImm(tmpl, rd, rn, val));
        }
    }
}

/**
 * @brief Emits a validated smart `sub sp, sp, #bytes` sequence.
 * @param bytes Non-negative stack allocation size.
 * @param[in,out] cs Text section receiving words.
 */
void A64BinaryEncoder::encodeSubSp(int64_t bytes, objfile::CodeSection &cs) {
    if (bytes < 0 || bytes > std::numeric_limits<uint32_t>::max())
        throw std::out_of_range("AArch64 stack adjustment is out of encodable range");
    if ((bytes % 16) != 0)
        throw std::out_of_range("AArch64 stack adjustment must be 16-byte aligned");
    const uint32_t sp = hwGPR(PhysReg::SP);
    emitAddSubImmSmart(kSubRI, sp, sp, static_cast<uint32_t>(bytes), cs);
}

/**
 * @brief Emits a validated smart `add sp, sp, #bytes` sequence.
 * @param bytes Non-negative stack deallocation size.
 * @param[in,out] cs Text section receiving words.
 */
void A64BinaryEncoder::encodeAddSp(int64_t bytes, objfile::CodeSection &cs) {
    if (bytes < 0 || bytes > std::numeric_limits<uint32_t>::max())
        throw std::out_of_range("AArch64 stack adjustment is out of encodable range");
    if ((bytes % 16) != 0)
        throw std::out_of_range("AArch64 stack adjustment must be 16-byte aligned");
    const uint32_t sp = hwGPR(PhysReg::SP);
    emitAddSubImmSmart(kAddRI, sp, sp, static_cast<uint32_t>(bytes), cs);
}

/**
 * @brief Emits scratch-address setup and a zero-offset scalar access.
 * @param rt Transfer register hardware field.
 * @param base Base register hardware field.
 * @param offset Signed byte offset to materialize.
 * @param isLoad Selects load versus store.
 * @param fprOperand Selects FP versus GPR transfer encoding.
 * @param accessBytes Scalar access width.
 * @param[in,out] cs Text section receiving words.
 */
void A64BinaryEncoder::encodeLargeOffsetLdSt(uint32_t rt,
                                             uint32_t base,
                                             int64_t offset,
                                             bool isLoad,
                                             bool fprOperand,
                                             unsigned accessBytes,
                                             objfile::CodeSection &cs) {
    if (base == hwGPR(PhysReg::SP)) {
        throw std::runtime_error("AArch64 binary encoder: large-offset load/store cannot "
                                 "materialize SP base with ADD (register)");
    }
    const uint32_t scratch = chooseGprScratch(
        base, (!isLoad && !fprOperand) ? std::optional<uint32_t>(rt) : std::nullopt);
    encodeMovImm64(scratch, static_cast<uint64_t>(offset), cs);
    // add scratch, base, scratch
    emit32(encode3Reg(kAddRRR, scratch, base, scratch), cs);
    // ldr/str rt, [x9]  (offset 0)
    if (fprOperand)
        emit32((isLoad ? kLdrFpr : kStrFpr) | (0 << 10) | (scratch << 5) | rt, cs);
    else
        emit32(scaledGprLdStTemplate(isLoad, accessBytes) | (0 << 10) | (scratch << 5) | rt, cs);
}

/**
 * @brief Chooses and emits scaled, unscaled, or large-offset scalar access.
 * @param rt Transfer register hardware field.
 * @param base Base GPR hardware field.
 * @param offset Signed byte offset.
 * @param isLoad Selects load versus store.
 * @param fprOperand Selects FP versus GPR transfer encoding.
 * @param accessBytes Scalar width.
 * @param[in,out] cs Text section receiving words.
 */
void A64BinaryEncoder::encodeScalarLdSt(uint32_t rt,
                                        uint32_t base,
                                        int64_t offset,
                                        bool isLoad,
                                        bool fprOperand,
                                        unsigned accessBytes,
                                        objfile::CodeSection &cs) {
    if (isInSignedImmRange(offset)) {
        if (fprOperand)
            emit32((isLoad ? kLdurFpr : kSturFpr) |
                       ((static_cast<uint32_t>(offset) & 0x1FF) << 12) | (base << 5) | rt,
                   cs);
        else
            emit32(unscaledGprLdStTemplate(isLoad, accessBytes) |
                       ((static_cast<uint32_t>(offset) & 0x1FF) << 12) | (base << 5) | rt,
                   cs);
        return;
    }

    if (isLegalScaledUImm(offset, accessBytes)) {
        const auto scaled = static_cast<uint32_t>(offset / static_cast<long long>(accessBytes));
        if (fprOperand)
            emit32((isLoad ? kLdrFpr : kStrFpr) | (scaled << 10) | (base << 5) | rt, cs);
        else
            emit32(scaledGprLdStTemplate(isLoad, accessBytes) | (scaled << 10) | (base << 5) | rt,
                   cs);
        return;
    }

    encodeLargeOffsetLdSt(rt, base, offset, isLoad, fprOperand, accessBytes, cs);
}

/**
 * @brief Emits an outgoing-argument store relative to `SP`.
 * @param rt Transfer register hardware field.
 * @param offset Signed destination byte offset.
 * @param fprOperand Selects FP versus GPR store encoding.
 * @param[in,out] cs Text section receiving words.
 */
void A64BinaryEncoder::encodeSpOffsetStore(uint32_t rt,
                                           int64_t offset,
                                           bool fprOperand,
                                           objfile::CodeSection &cs) {
    const uint32_t sp = hwGPR(PhysReg::SP);
    if (isLegalScaledUImm64(offset)) {
        const uint32_t scaled = static_cast<uint32_t>(offset / 8);
        emit32((fprOperand ? kStrFpr : kStrGpr) | (scaled << 10) | (sp << 5) | rt, cs);
        return;
    }

    const uint32_t scratch =
        chooseGprScratch(sp, (!fprOperand) ? std::optional<uint32_t>(rt) : std::nullopt);
    emit32(encodeAddSubImm(kAddRI, scratch, sp, 0), cs);
    if (offset > 0)
        emitAddSubImmSmart(
            kAddRI, scratch, scratch, checkedU32Magnitude(offset, "SP store offset"), cs);
    else if (offset < 0)
        emitAddSubImmSmart(
            kSubRI, scratch, scratch, checkedU32Magnitude(offset, "SP store offset"), cs);

    emit32((fprOperand ? kStrFpr : kStrGpr) | (0 << 10) | (scratch << 5) | rt, cs);
}

// =============================================================================
// encodeInstruction — main dispatch
// =============================================================================

// Homogeneous-pattern dispatch tables — each row encodes one instruction with
// a fixed template constant and a uniform operand shape. Keeps `encodeInstruction`
// readable while still letting heterogeneous opcodes use the switch.
namespace {

struct Reg3GprEntry {
    MOpcode op{};
    uint32_t tmpl{0};
};

constexpr Reg3GprEntry kReg3GprTable[] = {
    {MOpcode::AddRRR, kAddRRR},
    {MOpcode::SubRRR, kSubRRR},
    {MOpcode::AndRRR, kAndRRR},
    {MOpcode::OrrRRR, kOrrRRR},
    {MOpcode::EorRRR, kEorRRR},
    {MOpcode::AddsRRR, kAddsRRR},
    {MOpcode::SubsRRR, kSubsRRR},
    {MOpcode::LslvRRR, kLslvRRR},
    {MOpcode::LsrvRRR, kLsrvRRR},
    {MOpcode::AsrvRRR, kAsrvRRR},
    {MOpcode::MulRRR, kMulRRR},
    {MOpcode::SmulhRRR, kSmulhRRR},
    {MOpcode::UmulhRRR, kUmulhRRR},
    {MOpcode::SDivRRR, kSDivRRR},
    {MOpcode::UDivRRR, kUDivRRR},
};

struct Reg4GprEntry {
    MOpcode op{};
    uint32_t tmpl{0};
};

constexpr Reg4GprEntry kReg4GprTable[] = {
    {MOpcode::MSubRRRR, kMSubRRRR},
    {MOpcode::MAddRRRR, kMAddRRRR},
};

struct Reg3FprEntry {
    MOpcode op{};
    uint32_t tmpl{0};
};

constexpr Reg3FprEntry kReg3FprTable[] = {
    {MOpcode::FAddRRR, kFAddRRR},
    {MOpcode::FSubRRR, kFSubRRR},
    {MOpcode::FMulRRR, kFMulRRR},
    {MOpcode::FDivRRR, kFDivRRR},
};

struct Conv2RegEntry {
    MOpcode op{};
    uint32_t tmpl{0};
    bool dstIsGpr{false}; ///< true: dst uses hwGPR, false: hwFPR
    bool srcIsGpr{false}; ///< true: src uses hwGPR, false: hwFPR
};

constexpr Conv2RegEntry kConv2RegTable[] = {
    {MOpcode::SCvtF, kSCvtF, false, true},   ///< FPR <- GPR
    {MOpcode::FCvtZS, kFCvtZS, true, false}, ///< GPR <- FPR
    {MOpcode::UCvtF, kUCvtF, false, true},   ///< FPR <- GPR
    {MOpcode::FCvtZU, kFCvtZU, true, false}, ///< GPR <- FPR
    {MOpcode::FMovGR, kFMovGR, false, true}, ///< FPR <- GPR (bit transfer)
};

} // namespace

/**
 * @brief Validates and encodes one physical-register MIR instruction.
 * @param mi Instruction to dispatch by opcode and operand shape.
 * @param[in,out] cs Text section receiving words, relocations, or pending fixups.
 * @throws std::exception If the opcode/operands are invalid or unsupported.
 */
void A64BinaryEncoder::encodeInstruction(const MInstr &mi, objfile::CodeSection &cs) {
    validateOperandCount(mi);

    // Fast paths for homogeneous dispatch-table patterns. Order does not matter
    // because the tables are disjoint on MOpcode.
    for (const auto &e : kReg3GprTable) {
        if (mi.opc == e.op) {
            emit32(encode3Reg(e.tmpl,
                              hwGPR(getReg(mi.ops[0])),
                              hwGPR(getReg(mi.ops[1])),
                              hwGPR(getReg(mi.ops[2]))),
                   cs);
            return;
        }
    }
    for (const auto &e : kReg4GprTable) {
        if (mi.opc == e.op) {
            emit32(encode4Reg(e.tmpl,
                              hwGPR(getReg(mi.ops[0])),
                              hwGPR(getReg(mi.ops[1])),
                              hwGPR(getReg(mi.ops[2])),
                              hwGPR(getReg(mi.ops[3]))),
                   cs);
            return;
        }
    }
    for (const auto &e : kReg3FprTable) {
        if (mi.opc == e.op) {
            emit32(encode3Reg(e.tmpl,
                              hwFPR(getReg(mi.ops[0])),
                              hwFPR(getReg(mi.ops[1])),
                              hwFPR(getReg(mi.ops[2]))),
                   cs);
            return;
        }
    }
    for (const auto &e : kConv2RegTable) {
        if (mi.opc == e.op) {
            const uint32_t dst = e.dstIsGpr ? hwGPR(getReg(mi.ops[0])) : hwFPR(getReg(mi.ops[0]));
            const uint32_t src = e.srcIsGpr ? hwGPR(getReg(mi.ops[1])) : hwFPR(getReg(mi.ops[1]));
            emit32(encode2Reg(e.tmpl, dst, src), cs);
            return;
        }
    }

    switch (mi.opc) {
        // ─── Ret (triggers epilogue synthesis) — kept inline because tiny ───
        case MOpcode::Ret:
            if (skipFrame_)
                emit32(kRet, cs);
            else
                encodeEpilogue(*currentFn_, cs);
            return;

        case MOpcode::AddRI:
        case MOpcode::SubRI:
        case MOpcode::AddsRI:
        case MOpcode::SubsRI:
            encodeAddSubImmInstr(mi, cs);
            return;

        case MOpcode::MovRR:
        case MOpcode::MovRI:
            encodeMoveInstr(mi, cs);
            return;

        case MOpcode::LslRI:
        case MOpcode::LsrRI:
        case MOpcode::AsrRI:
            encodeShiftImmInstr(mi, cs);
            return;

        case MOpcode::CmpRR:
        case MOpcode::CmpRI:
        case MOpcode::TstRR:
            encodeCompareInstr(mi, cs);
            return;

        case MOpcode::Cset:
        case MOpcode::Csel:
        case MOpcode::FCsel:
            encodeConditionalInstr(mi, cs);
            return;

        case MOpcode::LdrRegFpImm:
        case MOpcode::PhiStoreGPR:
        case MOpcode::StrRegFpImm:
        case MOpcode::Ldr8RegFpImm:
        case MOpcode::Ldr16RegFpImm:
        case MOpcode::Ldr32RegFpImm:
        case MOpcode::Str8RegFpImm:
        case MOpcode::Str16RegFpImm:
        case MOpcode::Str32RegFpImm:
        case MOpcode::LdrFprFpImm:
        case MOpcode::PhiStoreFPR:
        case MOpcode::StrFprFpImm:
            encodeFpRelLdStInstr(mi, cs);
            return;

        case MOpcode::LdrRegBaseImm:
        case MOpcode::StrRegBaseImm:
        case MOpcode::Ldr8RegBaseImm:
        case MOpcode::Ldr16RegBaseImm:
        case MOpcode::Ldr32RegBaseImm:
        case MOpcode::Str8RegBaseImm:
        case MOpcode::Str16RegBaseImm:
        case MOpcode::Str32RegBaseImm:
        case MOpcode::LdrFprBaseImm:
        case MOpcode::StrFprBaseImm:
            encodeBaseRelLdStInstr(mi, cs);
            return;

        case MOpcode::LdrRegBaseRegLsl:
        case MOpcode::StrRegBaseRegLsl:
        case MOpcode::Ldr32RegBaseRegLsl:
        case MOpcode::Str32RegBaseRegLsl:
        case MOpcode::LdrFprBaseRegLsl:
        case MOpcode::StrFprBaseRegLsl:
            encodeRegOffsetLdStInstr(mi, cs);
            return;

        case MOpcode::AddRRRLsl:
        case MOpcode::SubRRRLsl:
        case MOpcode::AndRRRLsl:
        case MOpcode::OrrRRRLsl:
        case MOpcode::EorRRRLsl: {
            // Shifted-register data-processing: the base templates carry
            // shift type LSL(00) with amount 0; the amount lives in imm6
            // (bits 15-10).
            const uint32_t tmpl = mi.opc == MOpcode::AddRRRLsl   ? kAddRRR
                                  : mi.opc == MOpcode::SubRRRLsl ? kSubRRR
                                  : mi.opc == MOpcode::AndRRRLsl ? kAndRRR
                                  : mi.opc == MOpcode::OrrRRRLsl ? kOrrRRR
                                                                 : kEorRRR;
            const auto amount = static_cast<uint32_t>(getImm(mi.ops[3])) & 63U;
            emit32(encode3Reg(tmpl,
                              hwGPR(getReg(mi.ops[0])),
                              hwGPR(getReg(mi.ops[1])),
                              hwGPR(getReg(mi.ops[2]))) |
                       (amount << 10),
                   cs);
            return;
        }

        case MOpcode::LdpRegFpImm:
        case MOpcode::StpRegFpImm:
        case MOpcode::LdpFprFpImm:
        case MOpcode::StpFprFpImm:
            encodeLdStPairInstr(mi, cs);
            return;

        case MOpcode::SubSpImm:
        case MOpcode::AddSpImm:
        case MOpcode::StrRegSpImm:
        case MOpcode::StrFprSpImm:
            encodeSpOpInstr(mi, cs);
            return;

        case MOpcode::AddFpImm:
            encodeAddFpImmInstr(mi, cs);
            return;

        case MOpcode::AndRI:
        case MOpcode::OrrRI:
        case MOpcode::EorRI:
            encodeLogicalImmInstr(mi, cs);
            return;

        case MOpcode::FCmpRR:
        case MOpcode::FMovRR:
        case MOpcode::FRintN:
        case MOpcode::FMovRI:
            encodeFpSpecialInstr(mi, cs);
            return;

        case MOpcode::Br:
        case MOpcode::BCond:
        case MOpcode::Cbz:
        case MOpcode::Cbnz:
        case MOpcode::Tbz:
        case MOpcode::Tbnz:
        case MOpcode::JumpTable:
        case MOpcode::Bl:
        case MOpcode::Blr:
            encodeBranchInstr(mi, cs);
            return;

        case MOpcode::AdrPage:
        case MOpcode::AddPageOff:
            encodeAddressInstr(mi, cs);
            return;

        // ─── Pseudo-instructions that should have been expanded ───
        case MOpcode::AddOvfRRR:
        case MOpcode::SubOvfRRR:
        case MOpcode::AddOvfRI:
        case MOpcode::SubOvfRI:
        case MOpcode::MulOvfRRR:
            throw std::runtime_error("AArch64 binary encoder: overflow pseudo-op '" +
                                     std::string(opcodeName(mi.opc)) +
                                     "' reached binary emission before LowerOvf");

        default:
            break;
    }
    throw std::runtime_error("AArch64 binary encoder: unhandled opcode '" +
                             std::string(opcodeName(mi.opc)) + "'");
}

// ─── Per-family encoder definitions ────────────────────────────────────────

/** @brief Encodes an add/sub immediate family instruction.
 * @param mi Validated instruction. @param[in,out] cs Destination text section. */
void A64BinaryEncoder::encodeAddSubImmInstr(const MInstr &mi, objfile::CodeSection &cs) {
    const uint32_t rd = hwGPR(getReg(mi.ops[0]));
    const uint32_t rn = hwGPR(getReg(mi.ops[1]));
    const long long immValue = getImm(mi.ops[2]);
    const uint64_t imm = absImmUnsigned(immValue);
    const auto enc = classifyAddSubImmEncoding(imm);
    if (!enc.has_value()) {
        throw std::runtime_error("AArch64 binary encoder: " + std::string(opcodeName(mi.opc)) +
                                 " immediate reached encoder without legalization");
    }
    uint32_t tmpl = kAddRI;
    switch (mi.opc) {
        case MOpcode::AddRI:
            tmpl = immValue < 0 ? kSubRI : kAddRI;
            break;
        case MOpcode::SubRI:
            tmpl = immValue < 0 ? kAddRI : kSubRI;
            break;
        case MOpcode::AddsRI:
            tmpl = immValue < 0 ? kSubsRI : kAddsRI;
            break;
        case MOpcode::SubsRI:
            tmpl = immValue < 0 ? kAddsRI : kSubsRI;
            break;
        default:
            break;
    }
    emit32(enc->shift12 ? encodeAddSubImmShift(tmpl, rd, rn, enc->imm12)
                        : encodeAddSubImm(tmpl, rd, rn, enc->imm12),
           cs);
}

/** @brief Encodes integer or FP move-family MIR.
 * @param mi Validated instruction. @param[in,out] cs Destination text section. */
void A64BinaryEncoder::encodeMoveInstr(const MInstr &mi, objfile::CodeSection &cs) {
    if (mi.opc == MOpcode::MovRR) {
        // orr Xd, XZR, Xm
        emit32(kMovRR | (hwGPR(getReg(mi.ops[1])) << 16) | hwGPR(getReg(mi.ops[0])), cs);
        return;
    }
    // MovRI: small immediates use MOVZ/MOVN directly; wide values fall through
    // to the movz+movk sequence in encodeMovImm64.
    const long long imm = getImm(mi.ops[1]);
    const uint32_t rd = hwGPR(getReg(mi.ops[0]));
    if (!needsWideImmSequence(imm)) {
        if (imm < 0) {
            emit32(kMovN | (static_cast<uint32_t>(~imm & 0xFFFF) << 5) | rd, cs);
        } else {
            emit32(kMovZ | (static_cast<uint32_t>(imm & 0xFFFF) << 5) | rd, cs);
        }
    } else {
        encodeMovImm64(rd, static_cast<uint64_t>(imm), cs);
    }
}

/** @brief Encodes a validated immediate shift alias.
 * @param mi Validated instruction. @param[in,out] cs Destination text section. */
void A64BinaryEncoder::encodeShiftImmInstr(const MInstr &mi, objfile::CodeSection &cs) {
    const uint32_t rd = hwGPR(getReg(mi.ops[0]));
    const uint32_t rn = hwGPR(getReg(mi.ops[1]));
    const uint32_t sh = checkedShiftAmount(getImm(mi.ops[2]),
                                           mi.opc == MOpcode::LslRI   ? "lsl"
                                           : mi.opc == MOpcode::LsrRI ? "lsr"
                                                                      : "asr");
    switch (mi.opc) {
        case MOpcode::LslRI: {
            // lsl is ubfm Xd, Xn, #(64-n)&63, #(63-n)
            const uint32_t immr = (64 - sh) & 63;
            const uint32_t imms = 63 - sh;
            emit32(kUbfm | (immr << 16) | (imms << 10) | (rn << 5) | rd, cs);
            return;
        }
        case MOpcode::LsrRI:
            // lsr is ubfm Xd, Xn, #n, #63
            emit32(kUbfm | (sh << 16) | (63 << 10) | (rn << 5) | rd, cs);
            return;
        case MOpcode::AsrRI:
            // asr is sbfm Xd, Xn, #n, #63
            emit32(kSbfm | (sh << 16) | (63 << 10) | (rn << 5) | rd, cs);
            return;
        default:
            break;
    }
}

/** @brief Encodes integer/FP comparison and test MIR.
 * @param mi Validated instruction. @param[in,out] cs Destination text section. */
void A64BinaryEncoder::encodeCompareInstr(const MInstr &mi, objfile::CodeSection &cs) {
    switch (mi.opc) {
        case MOpcode::CmpRR:
            // subs XZR, Xn, Xm
            emit32(encode3Reg(kSubsRRR, 31, hwGPR(getReg(mi.ops[0])), hwGPR(getReg(mi.ops[1]))),
                   cs);
            return;
        case MOpcode::TstRR:
            // ands XZR, Xn, Xm
            emit32(encode3Reg(kAndsRRR, 31, hwGPR(getReg(mi.ops[0])), hwGPR(getReg(mi.ops[1]))),
                   cs);
            return;
        case MOpcode::CmpRI: {
            const long long imm = getImm(mi.ops[1]);
            const uint32_t rn = hwGPR(getReg(mi.ops[0]));
            if (imm >= 0 && imm <= 4095) {
                emit32(encodeAddSubImm(kSubsRI, 31, rn, static_cast<uint32_t>(imm)), cs);
            } else if (imm >= -4095 && imm < 0) {
                // cmn = adds XZR, Xn, #(-imm)
                emit32(encodeAddSubImm(kAddsRI, 31, rn, static_cast<uint32_t>(-imm)), cs);
            } else {
                // Large: materialise into the reserved scratch, then subs reg-reg.
                const uint32_t scratch = chooseGprScratch(rn);
                encodeMovImm64(scratch, static_cast<uint64_t>(imm), cs);
                emit32(encode3Reg(kSubsRRR, 31, rn, scratch), cs);
            }
            return;
        }
        default:
            break;
    }
}

/** @brief Encodes conditional set or select MIR.
 * @param mi Validated instruction. @param[in,out] cs Destination text section. */
void A64BinaryEncoder::encodeConditionalInstr(const MInstr &mi, objfile::CodeSection &cs) {
    if (mi.opc == MOpcode::Cset) {
        const uint32_t rd = hwGPR(getReg(mi.ops[0]));
        const uint32_t cc = getCondCode(mi.ops[1]);
        // csinc Xd, XZR, XZR, invert(cond)
        emit32(kCset | (invertCond(cc) << 12) | rd, cs);
        return;
    }
    if (mi.opc == MOpcode::FCsel) {
        const uint32_t rd = hwFPR(getReg(mi.ops[0]));
        const uint32_t rn = hwFPR(getReg(mi.ops[1]));
        const uint32_t rm = hwFPR(getReg(mi.ops[2]));
        const uint32_t cc = getCondCode(mi.ops[3]);
        emit32(kFCsel | (rm << 16) | (cc << 12) | (rn << 5) | rd, cs);
        return;
    }
    // Csel
    const uint32_t rd = hwGPR(getReg(mi.ops[0]));
    const uint32_t rn = hwGPR(getReg(mi.ops[1]));
    const uint32_t rm = hwGPR(getReg(mi.ops[2]));
    const uint32_t cc = getCondCode(mi.ops[3]);
    emit32(kCsel | (rm << 16) | (cc << 12) | (rn << 5) | rd, cs);
}

/** @brief Encodes frame-pointer-relative scalar memory MIR.
 * @param mi Validated instruction. @param[in,out] cs Destination text section. */
void A64BinaryEncoder::encodeFpRelLdStInstr(const MInstr &mi, objfile::CodeSection &cs) {
    // GPR Ldr/Str + PhiStore (single-width) — width 8 access.
    if (mi.opc == MOpcode::LdrRegFpImm || mi.opc == MOpcode::PhiStoreGPR ||
        mi.opc == MOpcode::StrRegFpImm) {
        const uint32_t rt = hwGPR(getReg(mi.ops[0]));
        const long long offset = getImm(mi.ops[1]);
        const uint32_t fp = hwGPR(PhysReg::X29);
        const bool isLoad = (mi.opc == MOpcode::LdrRegFpImm);
        encodeScalarLdSt(rt, fp, offset, isLoad, false, 8, cs);
        return;
    }
    // GPR Ldr/Str narrow-width variants (1/2/4 bytes).
    if (mi.opc == MOpcode::Ldr8RegFpImm || mi.opc == MOpcode::Ldr16RegFpImm ||
        mi.opc == MOpcode::Ldr32RegFpImm || mi.opc == MOpcode::Str8RegFpImm ||
        mi.opc == MOpcode::Str16RegFpImm || mi.opc == MOpcode::Str32RegFpImm) {
        const uint32_t rt = hwGPR(getReg(mi.ops[0]));
        const long long offset = getImm(mi.ops[1]);
        const uint32_t fp = hwGPR(PhysReg::X29);
        const bool isLoad = (mi.opc == MOpcode::Ldr8RegFpImm || mi.opc == MOpcode::Ldr16RegFpImm ||
                             mi.opc == MOpcode::Ldr32RegFpImm);
        const unsigned bytes =
            (mi.opc == MOpcode::Ldr8RegFpImm || mi.opc == MOpcode::Str8RegFpImm)     ? 1
            : (mi.opc == MOpcode::Ldr16RegFpImm || mi.opc == MOpcode::Str16RegFpImm) ? 2
                                                                                     : 4;
        encodeScalarLdSt(rt, fp, offset, isLoad, false, bytes, cs);
        return;
    }
    // FPR Ldr/Str (single-width 8).
    const uint32_t rt = hwFPR(getReg(mi.ops[0]));
    const long long offset = getImm(mi.ops[1]);
    const uint32_t fp = hwGPR(PhysReg::X29);
    const bool isLoad = (mi.opc == MOpcode::LdrFprFpImm);
    encodeScalarLdSt(rt, fp, offset, isLoad, true, 8, cs);
}

/** @brief Encodes arbitrary-base immediate-offset scalar memory MIR.
 * @param mi Validated instruction. @param[in,out] cs Destination text section. */
void A64BinaryEncoder::encodeBaseRelLdStInstr(const MInstr &mi, objfile::CodeSection &cs) {
    // FPR base-relative.
    if (mi.opc == MOpcode::LdrFprBaseImm || mi.opc == MOpcode::StrFprBaseImm) {
        const uint32_t rt = hwFPR(getReg(mi.ops[0]));
        const uint32_t base = hwGPR(getReg(mi.ops[1]));
        const long long offset = getImm(mi.ops[2]);
        const bool isLoad = (mi.opc == MOpcode::LdrFprBaseImm);
        encodeScalarLdSt(rt, base, offset, isLoad, true, 8, cs);
        return;
    }
    // GPR base-relative, possibly narrow-width.
    const uint32_t rt = hwGPR(getReg(mi.ops[0]));
    const uint32_t base = hwGPR(getReg(mi.ops[1]));
    const long long offset = getImm(mi.ops[2]);
    const bool isLoad = (mi.opc == MOpcode::LdrRegBaseImm || mi.opc == MOpcode::Ldr8RegBaseImm ||
                         mi.opc == MOpcode::Ldr16RegBaseImm || mi.opc == MOpcode::Ldr32RegBaseImm);
    const unsigned bytes =
        (mi.opc == MOpcode::Ldr8RegBaseImm || mi.opc == MOpcode::Str8RegBaseImm)     ? 1
        : (mi.opc == MOpcode::Ldr16RegBaseImm || mi.opc == MOpcode::Str16RegBaseImm) ? 2
        : (mi.opc == MOpcode::Ldr32RegBaseImm || mi.opc == MOpcode::Str32RegBaseImm) ? 4
                                                                                     : 8;
    encodeScalarLdSt(rt, base, offset, isLoad, false, bytes, cs);
}

/** @brief Encodes scaled register-offset scalar memory MIR.
 * @param mi Validated instruction. @param[in,out] cs Destination text section. */
void A64BinaryEncoder::encodeRegOffsetLdStInstr(const MInstr &mi, objfile::CodeSection &cs) {
    // LDR/STR (register offset): size(2) 111 V 00 opc(2) 1 Rm option(011=LSL)
    // S 10 Rn Rt. S=1 selects a shift equal to log2(access size); S=0 selects
    // no shift. Those are the only legal amounts for this form.
    const bool isLoad = mi.opc == MOpcode::LdrRegBaseRegLsl ||
                        mi.opc == MOpcode::Ldr32RegBaseRegLsl ||
                        mi.opc == MOpcode::LdrFprBaseRegLsl;
    const bool is32 = mi.opc == MOpcode::Ldr32RegBaseRegLsl ||
                      mi.opc == MOpcode::Str32RegBaseRegLsl;
    const bool isFpr = mi.opc == MOpcode::LdrFprBaseRegLsl ||
                       mi.opc == MOpcode::StrFprBaseRegLsl;
    const uint32_t rt = isFpr ? hwFPR(getReg(mi.ops[0])) : hwGPR(getReg(mi.ops[0]));
    const uint32_t rn = hwGPR(getReg(mi.ops[1]));
    const uint32_t rm = hwGPR(getReg(mi.ops[2]));
    const long long shift = getImm(mi.ops[3]);
    const long long expected = is32 ? 2 : 3;
    if (shift != 0 && shift != expected) {
        throw std::runtime_error("AArch64 encoder: register-offset load/store shift must be 0 or "
                                 "log2(size)");
    }
    const uint32_t size = is32 ? 0b10U : 0b11U;
    const uint32_t v = isFpr ? 1U : 0U;
    const uint32_t opc = isLoad ? 0b01U : 0b00U;
    const uint32_t sBit = shift == 0 ? 0U : 1U;
    const uint32_t word = (size << 30) | (0b111U << 27) | (v << 26) | (opc << 22) | (1U << 21) |
                          (rm << 16) | (0b011U << 13) | (sBit << 12) | (0b10U << 10) | (rn << 5) |
                          rt;
    emit32(word, cs);
}

/** @brief Encodes frame-pointer-relative paired memory MIR.
 * @param mi Validated instruction. @param[in,out] cs Destination text section. */
void A64BinaryEncoder::encodeLdStPairInstr(const MInstr &mi, objfile::CodeSection &cs) {
    // Common shape: two transfer regs + base (always FP) + offset; the only
    // axes are GPR-vs-FPR and load-vs-store.
    const bool isLoad = (mi.opc == MOpcode::LdpRegFpImm || mi.opc == MOpcode::LdpFprFpImm);
    const bool isFpr = (mi.opc == MOpcode::LdpFprFpImm || mi.opc == MOpcode::StpFprFpImm);
    const uint32_t rt = isFpr ? hwFPR(getReg(mi.ops[0])) : hwGPR(getReg(mi.ops[0]));
    const uint32_t rt2 = isFpr ? hwFPR(getReg(mi.ops[1])) : hwGPR(getReg(mi.ops[1]));
    const auto rawOffset = getImm(mi.ops[2]);
    const uint32_t fp = hwGPR(PhysReg::X29);
    const uint32_t tmpl = isLoad ? (isFpr ? kLdpFpr : kLdpGpr) : (isFpr ? kStpFpr : kStpGpr);
    const char *ctx = isLoad ? "ldp" : "stp";

    if (!isPairImm7Offset(rawOffset)) {
        // Offset doesn't fit the imm7 pair encoding: split into two scalar
        // load/stores at +0 and +8.
        encodeScalarLdSt(rt, fp, rawOffset, isLoad, isFpr, 8, cs);
        encodeScalarLdSt(rt2,
                         fp,
                         checkedAddI64(rawOffset, 8, "AArch64 ldp/stp fallback offset"),
                         isLoad,
                         isFpr,
                         8,
                         cs);
        return;
    }
    const auto offset = checkedPairImm7(rawOffset, ctx);
    emit32(encodePair(tmpl, rt, rt2, fp, offset), cs);
}

/** @brief Encodes stack-pointer adjustment and argument-store pseudos.
 * @param mi Validated instruction. @param[in,out] cs Destination text section. */
void A64BinaryEncoder::encodeSpOpInstr(const MInstr &mi, objfile::CodeSection &cs) {
    switch (mi.opc) {
        case MOpcode::SubSpImm:
            encodeSubSp(getImm(mi.ops[0]), cs);
            return;
        case MOpcode::AddSpImm:
            encodeAddSp(getImm(mi.ops[0]), cs);
            return;
        case MOpcode::StrRegSpImm: {
            const uint32_t rt = hwGPR(getReg(mi.ops[0]));
            const auto offset = static_cast<int64_t>(getImm(mi.ops[1]));
            encodeSpOffsetStore(rt, offset, false, cs);
            return;
        }
        case MOpcode::StrFprSpImm: {
            const uint32_t rt = hwFPR(getReg(mi.ops[0]));
            const auto offset = static_cast<int64_t>(getImm(mi.ops[1]));
            encodeSpOffsetStore(rt, offset, true, cs);
            return;
        }
        default:
            break;
    }
}

/** @brief Encodes formation of an FP-relative address.
 * @param mi Validated instruction. @param[in,out] cs Destination text section. */
void A64BinaryEncoder::encodeAddFpImmInstr(const MInstr &mi, objfile::CodeSection &cs) {
    const uint32_t rd = hwGPR(getReg(mi.ops[0]));
    const long long offset = getImm(mi.ops[1]);
    const uint32_t fp = hwGPR(PhysReg::X29);
    const uint64_t magnitude = absImmUnsigned(offset);
    const uint32_t tmpl = (offset >= 0) ? kAddRI : kSubRI;
    if (const auto enc = classifyAddSubImmEncoding(magnitude)) {
        emit32(enc->shift12 ? encodeAddSubImmShift(tmpl, rd, fp, enc->imm12)
                            : encodeAddSubImm(tmpl, rd, fp, enc->imm12),
               cs);
    } else {
        // Large offset: use a reserved scratch; register allocation never assigns it.
        const uint32_t scratch = chooseGprScratch(fp, rd);
        encodeMovImm64(scratch, magnitude, cs);
        emit32(encode3Reg(offset >= 0 ? kAddRRR : kSubRRR, rd, fp, scratch), cs);
    }
}

/** @brief Encodes a logical-immediate instruction after bitmask conversion.
 * @param mi Validated instruction. @param[in,out] cs Destination text section. */
void A64BinaryEncoder::encodeLogicalImmInstr(const MInstr &mi, objfile::CodeSection &cs) {
    // Three textually-identical patterns (And/Orr/Eor) differing only in
    // instruction templates: try the immediate encoding; on failure materialise
    // the value in a scratch register and emit the register-register form.
    uint32_t immTpl = 0, regTpl = 0;
    switch (mi.opc) {
        case MOpcode::AndRI:
            immTpl = kAndImm;
            regTpl = kAndRRR;
            break;
        case MOpcode::OrrRI:
            immTpl = kOrrImm;
            regTpl = kOrrRRR;
            break;
        case MOpcode::EorRI:
            immTpl = kEorImm;
            regTpl = kEorRRR;
            break;
        default:
            break;
    }
    const uint32_t rd = hwGPR(getReg(mi.ops[0]));
    const uint32_t rn = hwGPR(getReg(mi.ops[1]));
    const auto imm = static_cast<uint64_t>(getImm(mi.ops[2]));
    const int32_t enc = encodeLogicalImmediate(imm);
    if (enc >= 0) {
        emit32(encodeLogImm(immTpl, rd, rn, enc), cs);
    } else {
        const uint32_t scratch = chooseGprScratch(rn);
        encodeMovImm64(scratch, imm, cs);
        emit32(encode3Reg(regTpl, rd, rn, scratch), cs);
    }
}

/** @brief Encodes special FP moves, conversions, comparisons, and rounding.
 * @param mi Validated instruction. @param[in,out] cs Destination text section. */
void A64BinaryEncoder::encodeFpSpecialInstr(const MInstr &mi, objfile::CodeSection &cs) {
    // FAddRRR / FSubRRR / FMulRRR / FDivRRR are dispatched via kReg3FprTable.
    // SCvtF / FCvtZS / UCvtF / FCvtZU / FMovGR via kConv2RegTable. The cases
    // here are the irregular ones that don't fit those tables.
    switch (mi.opc) {
        case MOpcode::FCmpRR:
            // fcmp Dn, Dm (Rd field = 0)
            emit32(kFCmpRR | (hwFPR(getReg(mi.ops[1])) << 16) | (hwFPR(getReg(mi.ops[0])) << 5),
                   cs);
            return;
        case MOpcode::FMovRR: {
            const PhysReg src = getReg(mi.ops[1]);
            if (isGPR(src)) {
                throw std::runtime_error("AArch64 binary encoder: FMovRR requires an FPR source; "
                                         "use FMovGR for GPR-to-FPR bit transfers");
            }
            emit32(encode2Reg(kFMovRR, hwFPR(getReg(mi.ops[0])), hwFPR(src)), cs);
            return;
        }
        case MOpcode::FRintN:
            emit32(encode2Reg(kFRintN, hwFPR(getReg(mi.ops[0])), hwFPR(getReg(mi.ops[1]))), cs);
            return;
        case MOpcode::FMovRI: {
            const uint32_t rd = hwFPR(getReg(mi.ops[0]));
            double val;
            std::memcpy(&val, &mi.ops[1].imm, sizeof(val));
            const int32_t fp8 = encodeFP8Immediate(val);
            if (fp8 >= 0) {
                // FMOV Dd, #imm8 — imm8 at bits [20:13].
                emit32(kFMovDImm | (static_cast<uint32_t>(fp8) << 13) | rd, cs);
                return;
            }
            // Fallback: materialise 64-bit IEEE 754 bits in a GPR, then FMOV Dd, Xn.
            uint64_t bits;
            std::memcpy(&bits, &val, sizeof(bits));
            const uint32_t scratch = hwGPR(kScratchGPR);
            encodeMovImm64(scratch, bits, cs);
            emit32(encode2Reg(kFMovGR, rd, scratch), cs);
            return;
        }
        default:
            break;
    }
}

/** @brief Encodes page-relative address formation and associated relocations.
 * @param mi Validated instruction. @param[in,out] cs Destination text section. */
void A64BinaryEncoder::encodeAddressInstr(const MInstr &mi, objfile::CodeSection &cs) {
    // ADRP / ADD page-off: both generate linker-resolved relocations. ADRP
    // takes 2 operands (rd, label); AddPageOff takes 3 (rd, rn, label).
    const bool isAdrp = (mi.opc == MOpcode::AdrPage);
    const uint32_t rd = hwGPR(getReg(mi.ops[0]));
    const std::string sym = mapRuntimeSymbol(getLabel(mi.ops[isAdrp ? 1 : 2]));
    const uint32_t rodataSymIdx =
        currentRodata_ != nullptr ? currentRodata_->symbols().find(sym) : 0;
    const auto relocKind =
        isAdrp ? objfile::RelocKind::A64AdrpPage21 : objfile::RelocKind::A64AddPageOff12;
    const size_t relocOffset = cs.currentOffset();
    if (isAdrp) {
        emit32(kAdrp | rd, cs); // immediate filled by linker
    } else {
        const uint32_t rn = hwGPR(getReg(mi.ops[1]));
        emit32(encodeAddSubImm(kAddRI, rd, rn, 0), cs); // imm12 filled by linker
    }
    if (rodataSymIdx != 0) {
        const auto &rodataSym = currentRodata_->symbols().at(rodataSymIdx);
        cs.addSectionOffsetRelocationAt(relocOffset,
                                        relocKind,
                                        *currentRodata_,
                                        objfile::SymbolSection::Rodata,
                                        rodataSym.offset);
    } else {
        const uint32_t symIdx = cs.findOrDeclareSymbol(sym);
        cs.addRelocationAt(relocOffset, relocKind, symIdx, 0);
    }
}

/** @brief Encodes branch, call, test-branch, and jump-table control flow.
 * @param mi Validated instruction. @param[in,out] cs Destination text section. */
void A64BinaryEncoder::encodeBranchInstr(const MInstr &mi, objfile::CodeSection &cs) {
    switch (mi.opc) {
        case MOpcode::Br: {
            std::string target = getSanitizedNonEmptyLabel(mi.ops[0], "AArch64 branch");
            auto it = labelOffsets_.find(target);
            if (it != labelOffsets_.end()) {
                // Backward branch — resolve immediately.
                int64_t delta = checkedOffsetDelta(it->second, cs.currentOffset(), "branch");
                const int32_t imm26 =
                    checkedBranchDispWords(delta,
                                           26,
                                           "branch",
                                           "the +/-128MB B/BL range",
                                           target,
                                           currentFn_ ? currentFn_->name : "<unknown>");
                emit32(kBr | (static_cast<uint32_t>(imm26) & 0x3FFFFFF), cs);
            } else {
                pendingBranches_.push_back({cs.currentOffset(), target, MOpcode::Br});
                emit32(kBr, cs); // placeholder
            }
            return;
        }
        case MOpcode::BCond: {
            uint32_t cc = getCondCode(mi.ops[0]);
            std::string target = getSanitizedNonEmptyLabel(mi.ops[1], "AArch64 conditional branch");
            const bool forceLong =
                longConditionalBranchOrdinals_.count(currentInstructionOrdinal_) != 0;
            auto it = labelOffsets_.find(target);
            if (it != labelOffsets_.end()) {
                const int64_t delta =
                    checkedOffsetDelta(it->second, cs.currentOffset(), "conditional branch");
                if (!forceLong && fitsBranchDispWords(delta, 19)) {
                    const int32_t imm19 =
                        checkedBranchDispWords(delta,
                                               19,
                                               "conditional branch",
                                               "the +/-1MB conditional-branch range",
                                               target,
                                               currentFn_ ? currentFn_->name : "<unknown>");
                    emit32(kBCond | ((static_cast<uint32_t>(imm19) & 0x7FFFF) << 5) | cc, cs);
                } else {
                    emit32(kBCond | (2u << 5) | invertCond(cc), cs);
                    const int64_t farDelta =
                        checkedOffsetDelta(it->second, cs.currentOffset(), "branch");
                    const int32_t imm26 =
                        checkedBranchDispWords(farDelta,
                                               26,
                                               "branch",
                                               "the +/-128MB B/BL range",
                                               target,
                                               currentFn_ ? currentFn_->name : "<unknown>");
                    emit32(kBr | (static_cast<uint32_t>(imm26) & 0x3FFFFFF), cs);
                }
            } else {
                if (forceLong) {
                    emit32(kBCond | (2u << 5) | invertCond(cc), cs);
                    pendingBranches_.push_back({cs.currentOffset(), target, MOpcode::Br});
                    emit32(kBr, cs); // placeholder for the long-form branch target
                } else {
                    pendingBranches_.push_back({cs.currentOffset(), target, MOpcode::BCond});
                    emit32(kBCond | cc, cs); // placeholder with cond code set
                }
            }
            return;
        }
        case MOpcode::Cbz: {
            uint32_t rt = hwGPR(getReg(mi.ops[0]));
            std::string target = getSanitizedNonEmptyLabel(mi.ops[1], "AArch64 cbz");
            const bool forceLong =
                longConditionalBranchOrdinals_.count(currentInstructionOrdinal_) != 0;
            auto it = labelOffsets_.find(target);
            if (it != labelOffsets_.end()) {
                const int64_t delta =
                    checkedOffsetDelta(it->second, cs.currentOffset(), "conditional branch");
                if (!forceLong && fitsBranchDispWords(delta, 19)) {
                    const int32_t imm19 =
                        checkedBranchDispWords(delta,
                                               19,
                                               "conditional branch",
                                               "the +/-1MB conditional-branch range",
                                               target,
                                               currentFn_ ? currentFn_->name : "<unknown>");
                    emit32(kCbz | ((static_cast<uint32_t>(imm19) & 0x7FFFF) << 5) | rt, cs);
                } else {
                    emit32(kCbnz | (2u << 5) | rt, cs);
                    const int64_t farDelta =
                        checkedOffsetDelta(it->second, cs.currentOffset(), "branch");
                    const int32_t imm26 =
                        checkedBranchDispWords(farDelta,
                                               26,
                                               "branch",
                                               "the +/-128MB B/BL range",
                                               target,
                                               currentFn_ ? currentFn_->name : "<unknown>");
                    emit32(kBr | (static_cast<uint32_t>(imm26) & 0x3FFFFFF), cs);
                }
            } else {
                if (forceLong) {
                    emit32(kCbnz | (2u << 5) | rt, cs);
                    pendingBranches_.push_back({cs.currentOffset(), target, MOpcode::Br});
                    emit32(kBr, cs); // placeholder for the long-form branch target
                } else {
                    pendingBranches_.push_back({cs.currentOffset(), target, MOpcode::Cbz});
                    emit32(kCbz | rt, cs); // placeholder with Rt set
                }
            }
            return;
        }
        case MOpcode::Cbnz: {
            uint32_t rt = hwGPR(getReg(mi.ops[0]));
            std::string target = getSanitizedNonEmptyLabel(mi.ops[1], "AArch64 cbnz");
            const bool forceLong =
                longConditionalBranchOrdinals_.count(currentInstructionOrdinal_) != 0;
            auto it = labelOffsets_.find(target);
            if (it != labelOffsets_.end()) {
                const int64_t delta =
                    checkedOffsetDelta(it->second, cs.currentOffset(), "conditional branch");
                if (!forceLong && fitsBranchDispWords(delta, 19)) {
                    const int32_t imm19 =
                        checkedBranchDispWords(delta,
                                               19,
                                               "conditional branch",
                                               "the +/-1MB conditional-branch range",
                                               target,
                                               currentFn_ ? currentFn_->name : "<unknown>");
                    emit32(kCbnz | ((static_cast<uint32_t>(imm19) & 0x7FFFF) << 5) | rt, cs);
                } else {
                    emit32(kCbz | (2u << 5) | rt, cs);
                    const int64_t farDelta =
                        checkedOffsetDelta(it->second, cs.currentOffset(), "branch");
                    const int32_t imm26 =
                        checkedBranchDispWords(farDelta,
                                               26,
                                               "branch",
                                               "the +/-128MB B/BL range",
                                               target,
                                               currentFn_ ? currentFn_->name : "<unknown>");
                    emit32(kBr | (static_cast<uint32_t>(imm26) & 0x3FFFFFF), cs);
                }
            } else {
                if (forceLong) {
                    emit32(kCbz | (2u << 5) | rt, cs);
                    pendingBranches_.push_back({cs.currentOffset(), target, MOpcode::Br});
                    emit32(kBr, cs); // placeholder for the long-form branch target
                } else {
                    pendingBranches_.push_back({cs.currentOffset(), target, MOpcode::Cbnz});
                    emit32(kCbnz | rt, cs);
                }
            }
            return;
        }
        case MOpcode::Tbz:
        case MOpcode::Tbnz: {
            const uint32_t rt = hwGPR(getReg(mi.ops[0]));
            std::string target = getSanitizedNonEmptyLabel(mi.ops[1], "AArch64 tbz/tbnz");
            const int64_t bitVal = mi.ops[2].imm;
            if (bitVal < 0 || bitVal > 63)
                throw std::runtime_error(
                    "AArch64 binary encoder: tbz/tbnz bit number out of range");
            const uint32_t bit = static_cast<uint32_t>(bitVal);
            const uint32_t base = mi.opc == MOpcode::Tbz ? kTbz : kTbnz;
            const uint32_t inverse = mi.opc == MOpcode::Tbz ? kTbnz : kTbz;
            const uint32_t bitFields = ((bit >> 5) << 31) | ((bit & 0x1F) << 19);
            const bool forceLong =
                longConditionalBranchOrdinals_.count(currentInstructionOrdinal_) != 0;
            auto it = labelOffsets_.find(target);
            if (it != labelOffsets_.end()) {
                const int64_t delta =
                    checkedOffsetDelta(it->second, cs.currentOffset(), "conditional branch");
                if (!forceLong && fitsBranchDispWords(delta, 14)) {
                    const int32_t imm14 =
                        checkedBranchDispWords(delta,
                                               14,
                                               "conditional branch",
                                               "the +/-32KB tbz/tbnz range",
                                               target,
                                               currentFn_ ? currentFn_->name : "<unknown>");
                    emit32(base | bitFields | ((static_cast<uint32_t>(imm14) & 0x3FFF) << 5) | rt,
                           cs);
                } else {
                    emit32(inverse | bitFields | (2u << 5) | rt, cs);
                    const int64_t farDelta =
                        checkedOffsetDelta(it->second, cs.currentOffset(), "branch");
                    const int32_t imm26 =
                        checkedBranchDispWords(farDelta,
                                               26,
                                               "branch",
                                               "the +/-128MB B/BL range",
                                               target,
                                               currentFn_ ? currentFn_->name : "<unknown>");
                    emit32(kBr | (static_cast<uint32_t>(imm26) & 0x3FFFFFF), cs);
                }
            } else {
                if (forceLong) {
                    emit32(inverse | bitFields | (2u << 5) | rt, cs);
                    pendingBranches_.push_back({cs.currentOffset(), target, MOpcode::Br});
                    emit32(kBr, cs); // placeholder for the long-form branch target
                } else {
                    pendingBranches_.push_back({cs.currentOffset(), target, mi.opc});
                    emit32(base | bitFields | rt, cs); // placeholder with Rt and bit set
                }
            }
            return;
        }
        case MOpcode::JumpTable: {
            // [0]=index (use), [1]=table name (text-asm only), [2..]=case
            // labels. The dispatch tail runs in the reserved X16/X17 scratch
            // registers:
            //   adr x16, #16 ; ldrsw x17, [x16, idx, lsl #2]
            //   add x17, x17, x16 ; br x17 ; <4-byte anchor-relative entries>
            const uint32_t idx = hwGPR(getReg(mi.ops[0]));
            constexpr uint32_t tA = 16;
            constexpr uint32_t tB = 17;

            // The table begins four instructions past the adr.
            constexpr uint32_t kTableByteOffset = 16;
            const uint32_t immlo = kTableByteOffset & 0x3u;
            const uint32_t immhi = (kTableByteOffset >> 2) & 0x7FFFFu;
            emit32(kAdr | (immlo << 29) | (immhi << 5) | tA, cs);
            emit32(kLdrswRegLsl2 | (idx << 16) | (tA << 5) | tB, cs);
            emit32(kAddShifted | (tA << 16) | (tB << 5) | tB, cs);
            emit32(kBrReg | (tB << 5), cs);

            const size_t tableStart = cs.currentOffset();
            for (std::size_t i = 2; i < mi.ops.size(); ++i) {
                pendingTableEntries_.push_back(
                    {cs.currentOffset(),
                     getSanitizedNonEmptyLabel(mi.ops[i], "AArch64 jump table"),
                     tableStart});
                emit32(0, cs);
            }
            return;
        }
        case MOpcode::Bl: {
            // Direct call — always external (generates relocation).
            const std::string &rawLabel = getLabel(mi.ops[0]);
            if (rawLabel.empty())
                throw std::runtime_error("AArch64 binary encoder: call label must not be empty");
            std::string sym = mapRuntimeSymbol(rawLabel);
            auto it = labelOffsets_.find(sanitizeLabel(rawLabel));
            if (it != labelOffsets_.end()) {
                // Internal call (rare but possible for local functions).
                int64_t delta = checkedOffsetDelta(it->second, cs.currentOffset(), "call");
                const int32_t imm26 =
                    checkedBranchDispWords(delta,
                                           26,
                                           "call",
                                           "the +/-128MB B/BL range",
                                           rawLabel,
                                           currentFn_ ? currentFn_->name : "<unknown>");
                emit32(kBl | (static_cast<uint32_t>(imm26) & 0x3FFFFFF), cs);
            } else {
                uint32_t symIdx = cs.findOrDeclareSymbol(sym);
                const size_t relocOffset = cs.currentOffset();
                emit32(kBl, cs); // imm26 = 0, filled by linker
                cs.addRelocationAt(relocOffset, objfile::RelocKind::A64Call26, symIdx, 0);
            }
            return;
        }
        case MOpcode::Blr:
            emit32(kBlr | (hwGPR(getReg(mi.ops[0])) << 5), cs);
            return;
        default:
            break;
    }
}

} // namespace zanna::codegen::aarch64::binenc
