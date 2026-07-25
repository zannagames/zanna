//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: codegen/aarch64/InstrLowering.cpp
// Purpose: Opcode-specific IL→MIR lowering handlers for AArch64.
//          Each IL opcode category (arithmetic, bitwise, comparison, conversion,
//          memory, call/ret) has a dedicated handler that appends AArch64 MIR
//          instructions to the output block. Value materialization helpers handle
//          constant immediates, float literals, global addresses, and frame slots.
// Key invariants:
//   - Every handler appends in program order; never inserts before existing instrs.
//   - Virtual register IDs are allocated monotonically via ctx.nextVRegId.
//   - Handlers return false only on unrecoverable lowering errors.
// Ownership/Lifetime:
//   - Stateless free functions; all mutable state flows through LoweringContext&.
// Links: src/codegen/aarch64/InstrLowering.hpp,
//        src/codegen/aarch64/LowerILToMIR.cpp (orchestration),
//        src/codegen/aarch64/OpcodeDispatch.cpp (dispatch table),
//        src/codegen/aarch64/OpcodeMappings.hpp (opcode→condition tables)
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements AArch64 IL operand materialization, ABI call marshalling,
 *        and opcode-specific MIR lowering.
 *
 * Values are materialized from constants, parameters, frame addresses, cached
 * temporaries, or recursively lowered producers. Calls share the common
 * AAPCS64 argument planner, including aggregate chunks and Darwin variadic
 * rules. String-specific paths make retain/spend decisions explicit so MIR
 * preserves runtime ownership.
 */

#include "InstrLowering.hpp"
#include "A64ImmediateUtils.hpp"
#include "FpCompareLowering.hpp"
#include "FrameBuilder.hpp"
#include "OpcodeMappings.hpp"
#include "codegen/common/CallArgLayout.hpp"
#include "codegen/common/ScalarBits.hpp"
#include "codegen/common/StringRetainPolicy.hpp"

#include "il/runtime/RuntimeNameMap.hpp"
#include "il/runtime/RuntimeSignatures.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace zanna::codegen::aarch64 {

//===----------------------------------------------------------------------===//
// Helper: Get condition code for comparison opcodes
//===----------------------------------------------------------------------===//

/// @brief Return the AArch64 condition-code string for an IL comparison opcode.
/// @details Delegates to lookupAnyCondition() from OpcodeMappings.hpp which covers
///          both integer (icmp_*) and float (fcmp_*) comparisons.
/// @param op IL comparison opcode.
/// @return Pointer to a static AArch64 condition spelling.
static const char *condForOpcode(il::core::Opcode op) {
    return lookupAnyCondition(op);
}

/// @brief Compute an aligned call-stack adjustment without integer overflow.
/// @details AArch64 call arguments consume 8-byte stack slots and SP must remain
///          16-byte aligned. The checked result is suitable for encoding as the
///          immediate payload of the pseudo stack-adjustment instructions used
///          by the backend.
/// @param stackSlots Number of outgoing stack slots required by call planning.
/// @param bytesOut Receives the 16-byte aligned byte count on success.
/// @return True when the byte count can be represented by backend immediates.
[[nodiscard]] static bool checkedCallStackBytes(std::size_t stackSlots,
                                                std::size_t &bytesOut) noexcept {
    constexpr std::size_t kSlotBytes = 8;
    constexpr std::size_t kSpAlign = 16;
    const std::size_t max = std::numeric_limits<std::size_t>::max();
    if (stackSlots > (max - (kSpAlign - 1U)) / kSlotBytes) {
        return false;
    }
    const std::size_t rawBytes = stackSlots * kSlotBytes;
    const std::size_t alignedBytes = ((rawBytes + (kSpAlign - 1U)) / kSpAlign) * kSpAlign;
    if (alignedBytes > static_cast<std::size_t>(std::numeric_limits<long long>::max())) {
        return false;
    }
    bytesOut = alignedBytes;
    return true;
}

/// @brief Compute the byte offset for one outgoing stack argument slot.
/// @details Stack argument locations are expressed as 8-byte slot indices by
///          the shared call planner. This helper verifies that converting a slot
///          index into a signed MIR immediate cannot overflow.
/// @param stackSlotIndex Zero-based outgoing stack slot index.
/// @param bytesOut Receives the byte offset from SP on success.
/// @return True when the slot offset is representable as a backend immediate.
[[nodiscard]] static bool checkedCallStackSlotOffset(std::size_t stackSlotIndex,
                                                     std::size_t &bytesOut) noexcept {
    constexpr std::size_t kSlotBytes = 8;
    if (stackSlotIndex >
        static_cast<std::size_t>(std::numeric_limits<long long>::max()) / kSlotBytes) {
        return false;
    }
    bytesOut = stackSlotIndex * kSlotBytes;
    return true;
}

/// @brief Validate an aggregate chunk byte offset for use as a MIR immediate.
/// @param byteOffset Unsigned byte displacement from an aggregate base.
/// @param offsetOut Receives the signed MIR immediate on success.
/// @return `true` when @p byteOffset fits `long long`.
[[nodiscard]] static bool checkedAggregateChunkOffset(std::size_t byteOffset,
                                                      long long &offsetOut) noexcept {
    if (byteOffset > static_cast<std::size_t>(std::numeric_limits<long long>::max())) {
        return false;
    }
    offsetOut = static_cast<long long>(byteOffset);
    return true;
}

/// @brief Recursively resolve whether @p value is a frame-relative address.
/// @details Checks if @p value is a temp with a known frame offset, or an AddrOf/GEP
///          whose base ultimately resolves to a frame slot. On success, @p offsetOut is
///          incremented by the computed FP-relative byte offset.
/// @param value     IL value to resolve.
/// @param fn        Enclosing IL function (for producer lookups).
/// @param fb        Frame builder holding local slot assignments.
/// @param offsetOut Accumulated FP-relative offset; updated on success.
/// @return True if @p value resolves to a frame address, false otherwise.
static bool resolveFrameAddress(const il::core::Value &value,
                                const il::core::Function &fn,
                                FrameBuilder &fb,
                                long long &offsetOut) {
    using il::core::Opcode;
    using il::core::Value;

    if (value.kind != Value::Kind::Temp)
        return false;

    if (const auto localOffset = fb.tryLocalOffset(value.id)) {
        if ((*localOffset > 0 &&
             offsetOut > std::numeric_limits<long long>::max() - *localOffset) ||
            (*localOffset < 0 && offsetOut < std::numeric_limits<long long>::min() - *localOffset))
            return false;
        offsetOut += *localOffset;
        return true;
    }

    const auto *producer = findProducerInFunction(fn, value.id);
    if (!producer)
        return false;

    switch (producer->op) {
        case Opcode::AddrOf:
            if (producer->operands.empty())
                return false;
            return resolveFrameAddress(producer->operands[0], fn, fb, offsetOut);

        case Opcode::GEP: {
            if (producer->operands.size() < 2 ||
                producer->operands[1].kind != Value::Kind::ConstInt) {
                return false;
            }
            if (!resolveFrameAddress(producer->operands[0], fn, fb, offsetOut))
                return false;
            const long long gepOffset = producer->operands[1].i64;
            if ((gepOffset > 0 && offsetOut > std::numeric_limits<long long>::max() - gepOffset) ||
                (gepOffset < 0 && offsetOut < std::numeric_limits<long long>::min() - gepOffset))
                return false;
            offsetOut += gepOffset;
            return true;
        }

        default:
            return false;
    }
}

namespace {

/// @brief A call argument after materialization: the vreg holding its value and
///        the ABI class (GPR vs FP/SIMD) that selects which register bank and
///        stack-slot rules apply when it is marshalled into the call sequence.
struct MaterializedCallArg {
    uint16_t vreg{0}; ///< Virtual register holding the materialized argument value.
    zanna::codegen::common::CallArgClass cls{
        zanna::codegen::common::CallArgClass::GPR}; ///< ABI register class of the argument.
    zanna::codegen::common::CallArgKind kind{zanna::codegen::common::CallArgKind::Scalar};
    zanna::codegen::common::AggregatePassKind aggregatePass{
        zanna::codegen::common::AggregatePassKind::Direct};
    std::size_t sizeBytes{8};
    std::size_t alignBytes{8};
};

/// @brief Map canonical runtime names to the concrete linker symbol, preserving user symbols.
/// @param name Canonical runtime name or user-defined symbol.
/// @return Concrete linker symbol spelling.
std::string mapExternalSymbol(std::string_view name) {
    if (auto mapped = il::runtime::mapCanonicalRuntimeName(name))
        return std::string(*mapped);
    return std::string(name);
}

/// @brief Return the direct callee name from either modern or legacy call encoding.
/// @param ins Direct-call instruction to inspect.
/// @return Static callee name, or an empty string for an indirect/malformed call.
std::string directCalleeName(const il::core::Instr &ins) {
    if (!ins.callee.empty())
        return ins.callee;
    if (!ins.operands.empty() && ins.operands[0].kind == il::core::Value::Kind::GlobalAddr)
        return ins.operands[0].str;
    return {};
}

/// @brief Test whether @p ins is a direct call whose callee maps to @p runtimeName.
/// @details Resolves the callee via directCalleeName(), then normalizes it through
///          mapExternalSymbol() so canonical runtime aliases match. Indirect calls
///          (no static callee) always return false.
/// @param ins         The IL call instruction to inspect.
/// @param runtimeName The concrete linker symbol to compare against.
/// @return True if @p ins directly calls @p runtimeName, false otherwise.
bool isDirectCallee(const il::core::Instr &ins, std::string_view runtimeName) {
    const std::string callee = directCalleeName(ins);
    if (callee.empty())
        return false;
    return mapExternalSymbol(callee) == runtimeName;
}

/// @brief Emit AND-with-1 to mask an i1 value into its canonical 0-or-1 form.
/// @param out Machine block receiving materialization and mask instructions.
/// @param srcVReg Source GPR virtual register.
/// @param nextVRegId Monotonic allocator advanced for two temporaries.
/// @return New vreg holding `srcVReg & 1`.
uint16_t emitMaskedI1Value(MBasicBlock &out, uint16_t srcVReg, uint16_t &nextVRegId) {
    const uint16_t mask = allocateNextVReg(nextVRegId);
    out.instrs.push_back(
        MInstr{MOpcode::MovRI, {MOperand::vregOp(RegClass::GPR, mask), MOperand::immOp(1)}});
    const uint16_t masked = allocateNextVReg(nextVRegId);
    out.instrs.push_back(MInstr{MOpcode::AndRRR,
                                {MOperand::vregOp(RegClass::GPR, masked),
                                 MOperand::vregOp(RegClass::GPR, srcVReg),
                                 MOperand::vregOp(RegClass::GPR, mask)}});
    return masked;
}

/// @brief Select a width-specific GPR load opcode for an IL type.
/// @param kind Loaded IL scalar type.
/// @param frameRelative Select FP-relative rather than arbitrary-base addressing.
/// @return Matching unsigned narrow or full-width MIR load opcode.
MOpcode gprLoadOpcodeForType(il::core::Type::Kind kind, bool frameRelative) {
    switch (kind) {
        case il::core::Type::Kind::I1:
            return frameRelative ? MOpcode::Ldr8RegFpImm : MOpcode::Ldr8RegBaseImm;
        case il::core::Type::Kind::I16:
            return frameRelative ? MOpcode::Ldr16RegFpImm : MOpcode::Ldr16RegBaseImm;
        case il::core::Type::Kind::I32:
            return frameRelative ? MOpcode::Ldr32RegFpImm : MOpcode::Ldr32RegBaseImm;
        default:
            return frameRelative ? MOpcode::LdrRegFpImm : MOpcode::LdrRegBaseImm;
    }
}

/// @brief Select a width-specific GPR store opcode for an IL type.
/// @param kind Stored IL scalar type.
/// @param frameRelative Select FP-relative rather than arbitrary-base addressing.
/// @return Matching narrow or full-width MIR store opcode.
MOpcode gprStoreOpcodeForType(il::core::Type::Kind kind, bool frameRelative) {
    switch (kind) {
        case il::core::Type::Kind::I1:
            return frameRelative ? MOpcode::Str8RegFpImm : MOpcode::Str8RegBaseImm;
        case il::core::Type::Kind::I16:
            return frameRelative ? MOpcode::Str16RegFpImm : MOpcode::Str16RegBaseImm;
        case il::core::Type::Kind::I32:
            return frameRelative ? MOpcode::Str32RegFpImm : MOpcode::Str32RegBaseImm;
        default:
            return frameRelative ? MOpcode::StrRegFpImm : MOpcode::StrRegBaseImm;
    }
}

/// @brief Look up the named-argument count for a known variadic callee.
/// @param callee Direct callee name.
/// @param knownVarArgNamedArgCounts Optional lookup table.
/// @return nullopt when @p callee is not in the table.
std::optional<std::size_t> lookupKnownVarArgNamedArgs(
    std::string_view callee,
    const std::unordered_map<std::string, std::size_t> *knownVarArgNamedArgCounts) {
    if (!knownVarArgNamedArgCounts)
        return std::nullopt;
    const auto it = knownVarArgNamedArgCounts->find(std::string(callee));
    if (it == knownVarArgNamedArgCounts->end())
        return std::nullopt;
    return it->second;
}

/// @brief Test whether @p callee is a known variadic runtime/library function.
/// @param callee Direct callee name.
/// @param knownVarArgNamedArgCounts Optional lookup table.
/// @return `true` when @p callee has fixed-prefix metadata.
bool isKnownVarArgCallee(
    std::string_view callee,
    const std::unordered_map<std::string, std::size_t> *knownVarArgNamedArgCounts) {
    return lookupKnownVarArgNamedArgs(callee, knownVarArgNamedArgCounts).has_value();
}

/// @brief Materialise a 64-bit FP value via the integer bit-pattern + FMOV GPR→FPR.
/// @details Used when the FP immediate doesn't fit AArch64's FMOV immediate encoding.
/// @param out Machine block receiving the two-instruction sequence.
/// @param dstVReg Destination FPR virtual register.
/// @param bits IEEE-754 bit pattern.
/// @param nextVRegId Monotonic allocator advanced for the temporary GPR.
void emitF64BitsToVReg(MBasicBlock &out, uint16_t dstVReg, uint64_t bits, uint16_t &nextVRegId) {
    const uint16_t bitsGpr = allocateNextVReg(nextVRegId);
    out.instrs.push_back(MInstr{
        MOpcode::MovRI,
        {MOperand::vregOp(RegClass::GPR, bitsGpr), MOperand::immOp(static_cast<long long>(bits))}});
    out.instrs.push_back(MInstr{
        MOpcode::FMovGR,
        {MOperand::vregOp(RegClass::FPR, dstVReg), MOperand::vregOp(RegClass::GPR, bitsGpr)}});
}

/// @brief Place each materialised call argument in its ABI-required home.
/// @details Calls into the shared planCallArgs() allocator to decide register
///          vs. stack placement, then emits aggregate chunk loads, MovRR/FMovRR
///          register moves, and STR stack stores into @p seq.prefix. Inserts the
///          SP adjustment pair (SubSpImm / AddSpImm) around stack arguments.
/// @param args                Argument vregs and their register classes.
/// @param numNamedArgs        Number of named (non-variadic) args for tail-on-stack.
/// @param variadicTailOnStack True when the variadic tail must spill to the stack
///                            (Darwin AArch64 calling convention).
/// @param ti                  Target info supplying the AAPCS64 arg-register order.
/// @param nextVRegId          Counter for aggregate chunk temporary vregs.
/// @param seq                 Output sequence: prefix loads + postfix SP cleanup.
/// @return True on success; false when the planned stack or aggregate offsets overflow.
bool marshalCallArgs(const std::vector<MaterializedCallArg> &args,
                     std::size_t numNamedArgs,
                     bool variadicTailOnStack,
                     const TargetInfo &ti,
                     uint16_t &nextVRegId,
                     LoweredCall &seq) {
    using namespace zanna::codegen::common;

    std::vector<CallArg> planArgs;
    planArgs.reserve(args.size());
    for (const auto &arg : args) {
        CallArg planArg{};
        planArg.cls = arg.cls;
        planArg.vreg = arg.vreg;
        planArg.kind = arg.kind;
        planArg.aggregatePass = arg.aggregatePass;
        planArg.sizeBytes = arg.sizeBytes;
        planArg.alignBytes = arg.alignBytes;
        planArgs.push_back(planArg);
    }

    const CallArgLayout layout =
        planCallArgs(planArgs,
                     CallArgLayoutConfig{.maxGPRArgs = ti.intArgOrder.size(),
                                         .maxFPRArgs = ti.f64ArgOrder.size(),
                                         .slotModel = CallSlotModel::IndependentRegisterBanks,
                                         .variadicTailOnStack = variadicTailOnStack,
                                         .numNamedArgs = numNamedArgs});

    std::size_t stackBytes = 0;
    if (!checkedCallStackBytes(layout.stackSlotsUsed, stackBytes)) {
        return false;
    }
    if (stackBytes > 0) {
        seq.prefix.push_back(
            MInstr{MOpcode::SubSpImm, {MOperand::immOp(static_cast<long long>(stackBytes))}});
    }

    for (const auto &loc : layout.locations) {
        const auto &arg = args[loc.argIndex];
        uint16_t valueVReg = arg.vreg;
        RegClass valueClass = (loc.cls == CallArgClass::FPR) ? RegClass::FPR : RegClass::GPR;

        if (loc.isAggregatePart) {
            long long byteOffset = 0;
            if (!checkedAggregateChunkOffset(loc.byteOffset, byteOffset)) {
                return false;
            }
            valueVReg = allocateNextVReg(nextVRegId);
            valueClass = RegClass::GPR;
            seq.prefix.push_back(MInstr{MOpcode::LdrRegBaseImm,
                                        {MOperand::vregOp(RegClass::GPR, valueVReg),
                                         MOperand::vregOp(RegClass::GPR, arg.vreg),
                                         MOperand::immOp(byteOffset)}});
        }

        if (loc.inRegister) {
            if (loc.cls == CallArgClass::FPR) {
                const PhysReg dst = ti.f64ArgOrder[loc.regIndex];
                seq.prefix.push_back(
                    MInstr{MOpcode::FMovRR,
                           {MOperand::regOp(dst), MOperand::vregOp(RegClass::FPR, valueVReg)}});
            } else {
                const PhysReg dst = ti.intArgOrder[loc.regIndex];
                seq.prefix.push_back(
                    MInstr{MOpcode::MovRR,
                           {MOperand::regOp(dst), MOperand::vregOp(RegClass::GPR, valueVReg)}});
            }
        } else {
            std::size_t stackOffsetBytes = 0;
            if (!checkedCallStackSlotOffset(loc.stackSlotIndex, stackOffsetBytes)) {
                return false;
            }
            const long long stackOffset = static_cast<long long>(stackOffsetBytes);
            const MOpcode storeOpc =
                (loc.cls == CallArgClass::FPR) ? MOpcode::StrFprSpImm : MOpcode::StrRegSpImm;
            seq.prefix.push_back(MInstr{
                storeOpc, {MOperand::vregOp(valueClass, valueVReg), MOperand::immOp(stackOffset)}});
        }
    }

    if (stackBytes > 0) {
        seq.postfix.push_back(
            MInstr{MOpcode::AddSpImm, {MOperand::immOp(static_cast<long long>(stackBytes))}});
    }
    return true;
}

/// @brief Count reference-spending uses of one string temporary.
/// @param fn Function whose instructions and branch arguments are scanned.
/// @param resultId String temporary identifier.
/// @return Conservative spend count used by call-result retain elision.
[[nodiscard]] unsigned countStringSpendingUses(const il::core::Function &fn, unsigned resultId);

/// @brief Copy a call's return value out of the ABI return register into a fresh vreg.
/// @details Picks `intReturnReg` or `f64ReturnReg` based on `ins.type`. Registers
///          the vreg in @p tempVReg / @p tempRegClass so later uses resolve. For
///          string returns, may emit `bl rt_str_retain_maybe`; for I1 returns,
///          masks to canonical 0/1.
/// @param ins Call instruction whose result metadata is consumed.
/// @param fn Enclosing function used to analyze string-spending uses.
/// @param ti Target ABI return-register description.
/// @param out Machine block receiving capture/retain/mask instructions.
/// @param tempVReg Result-temp to virtual-register map updated on success.
/// @param tempRegClass Result-temp register-class map updated on success.
/// @param nextVRegId Monotonic virtual-register allocator.
/// @return The vreg holding the captured result, or zero when there is no result.
uint16_t captureCallResult(const il::core::Instr &ins,
                           const il::core::Function &fn,
                           const TargetInfo &ti,
                           MBasicBlock &out,
                           std::unordered_map<unsigned, uint16_t> &tempVReg,
                           std::unordered_map<unsigned, RegClass> &tempRegClass,
                           uint16_t &nextVRegId) {
    if (!ins.result)
        return 0;

    const uint16_t dst = allocateNextVReg(nextVRegId);
    tempVReg[*ins.result] = dst;
    if (ins.type.kind == il::core::Type::Kind::F64) {
        tempRegClass[*ins.result] = RegClass::FPR;
        out.instrs.push_back(
            MInstr{MOpcode::FMovRR,
                   {MOperand::vregOp(RegClass::FPR, dst), MOperand::regOp(ti.f64ReturnReg)}});
        return dst;
    }

    out.instrs.push_back(MInstr{
        MOpcode::MovRR, {MOperand::vregOp(RegClass::GPR, dst), MOperand::regOp(ti.intReturnReg)}});
    const bool retainElidable =
        ins.type.kind == il::core::Type::Kind::Str && ins.result &&
        zanna::codegen::shouldElideStringResultRetain(
            ins.op == il::core::Opcode::Call ? std::string_view(ins.callee) : std::string_view{}) &&
        countStringSpendingUses(fn, *ins.result) <= 1;
    if (ins.type.kind == il::core::Type::Kind::Str && !retainElidable) {
        out.instrs.push_back(
            MInstr{MOpcode::MovRR,
                   {MOperand::regOp(ti.intReturnReg), MOperand::vregOp(RegClass::GPR, dst)}});
        out.instrs.push_back(MInstr{MOpcode::Bl, {MOperand::labelOp("rt_str_retain_maybe")}});
    }
    if (ins.type.kind == il::core::Type::Kind::I1) {
        const uint16_t masked = emitMaskedI1Value(out, dst, nextVRegId);
        tempVReg[*ins.result] = masked;
        return masked;
    }
    return dst;
}

/// @brief Index of @p ins within @p bb (StableList has no pointer arithmetic).
/// @param bb Basic block containing @p ins.
/// @param ins Instruction whose stable-list index is required.
/// @return Zero-based index, or zero when no matching address is found.
[[nodiscard]] std::size_t indexOfInstr(const il::core::BasicBlock &bb, const il::core::Instr &ins) {
    for (std::size_t i = 0; i < bb.instructions.size(); ++i) {
        if (&bb.instructions[i] == &ins)
            return i;
    }
    return 0;
}

/// @brief Count the reference-spending uses of string temp @p resultId.
/// @details The ownership transferred by a string-returning call covers
///          exactly ONE spend: a consuming runtime-call argument, an explicit
///          release, a `ret` (transfer to the caller), or a branch argument
///          (unknown downstream spending, counted conservatively as two so
///          the retain is kept). The defensive call-result retain may only be
///          elided when total spends fit in the transferred reference —
///          `concat(%s, %s)` spends twice and MUST keep the retain.
/// @param fn Function whose operands and branch arguments are scanned.
/// @param resultId String temporary produced by the call.
/// @return Conservative number of reference-spending uses.
[[nodiscard]] unsigned countStringSpendingUses(const il::core::Function &fn, unsigned resultId) {
    unsigned spends = 0;
    for (const auto &block : fn.blocks) {
        for (const auto &ins : block.instructions) {
            for (std::size_t oi = 0; oi < ins.operands.size(); ++oi) {
                const auto &op = ins.operands[oi];
                if (op.kind != il::core::Value::Kind::Temp || op.id != resultId)
                    continue;
                if (ins.op == il::core::Opcode::Ret) {
                    ++spends;
                } else if (ins.op == il::core::Opcode::Call) {
                    if (zanna::codegen::isStringReleaseCallee(ins.callee)) {
                        ++spends;
                    } else if (il::runtime::findRuntimeSignature(ins.callee) != nullptr &&
                               il::runtime::classifyRuntimeOwnership(ins.callee)
                                   .consumesArg(static_cast<unsigned>(oi))) {
                        ++spends;
                    }
                }
            }
            for (const auto &argList : ins.brArgs) {
                for (const auto &arg : argList) {
                    if (arg.kind == il::core::Value::Kind::Temp && arg.id == resultId)
                        spends += 2; // unknown downstream spending: keep the retain
                }
            }
        }
    }
    return spends;
}

/// @brief True when @p ins borrows argument @p argIdx instead of spending it.
/// @details Direct IL calls use borrowed parameters by default; callees retain
///          any managed value they keep or consume. Registered runtime helpers
///          override that convention only for arguments explicitly classified
///          as consumed. A helper that retains an argument still borrows the
///          caller's reference and therefore needs no defensive caller clone.
/// @param ins Candidate call instruction.
/// @param argIdx Zero-based operand index.
/// @return `true` when the callee does not consume the caller's reference.
[[nodiscard]] bool isBorrowedCallArg(const il::core::Instr &ins, std::size_t argIdx) {
    if (ins.op != il::core::Opcode::Call)
        return false;
    if (il::runtime::findRuntimeSignature(ins.callee) == nullptr)
        return true;
    const auto effects = il::runtime::classifyRuntimeOwnership(ins.callee);
    return !effects.consumesArg(static_cast<unsigned>(argIdx));
}

/// @brief True when the string load @p resultId needs no defensive retain.
/// @details Two elidable shapes (see StringRetainPolicy.hpp):
///          1. The only use is an explicit release call — the retain+release
///             pair would be a native no-op that leaks the displaced value.
///          2. Every use is a borrowed argument of a registered runtime
///             helper in the load's own block, with no release call or
///             string store between the load and the last use — the slot's
///             own reference keeps the value alive through the window
///             (runtime helpers cannot touch local slots, and consuming
///             argument positions always keep their own load retain).
/// @param fn Enclosing function used for cross-block use analysis.
/// @param loadBlock Block containing the load.
/// @param loadIdx Zero-based instruction index of the load.
/// @param resultId String temporary produced by the load.
/// @return `true` when an existing ownership carrier dominates all uses.
[[nodiscard]] bool strLoadRetainElidable(const il::core::Function &fn,
                                         const il::core::BasicBlock &loadBlock,
                                         std::size_t loadIdx,
                                         unsigned resultId) {
    if (!zanna::codegen::shouldElideLoadRetainForRelease())
        return false;

    unsigned uses = 0;
    bool releaseOnly = true;
    bool borrowOnlyInBlock = true;
    std::size_t lastUseIdx = loadIdx;
    for (const auto &block : fn.blocks) {
        for (std::size_t ii = 0; ii < block.instructions.size(); ++ii) {
            const auto &ins = block.instructions[ii];
            for (std::size_t oi = 0; oi < ins.operands.size(); ++oi) {
                const auto &op = ins.operands[oi];
                if (op.kind != il::core::Value::Kind::Temp || op.id != resultId)
                    continue;
                ++uses;
                if (!(ins.op == il::core::Opcode::Call &&
                      zanna::codegen::isStringReleaseCallee(ins.callee)))
                    releaseOnly = false;
                if (&block != &loadBlock || ii <= loadIdx || !isBorrowedCallArg(ins, oi))
                    borrowOnlyInBlock = false;
                else
                    lastUseIdx = std::max(lastUseIdx, ii);
            }
            for (const auto &argList : ins.brArgs) {
                for (const auto &arg : argList) {
                    if (arg.kind == il::core::Value::Kind::Temp && arg.id == resultId) {
                        ++uses;
                        releaseOnly = false;
                        borrowOnlyInBlock = false;
                    }
                }
            }
        }
    }
    if (uses == 0)
        return false;
    if (uses == 1 && releaseOnly)
        return true;

    // A managed merge slot is an ownership carrier, not an independent
    // long-lived owner. Frontend lowering spells the take as `load str`
    // immediately followed by a raw null store to the same address. The load
    // receives the slot's existing +1, so adding another retain would leak it.
    if (loadIdx + 1 < loadBlock.instructions.size()) {
        const auto &load = loadBlock.instructions[loadIdx];
        const auto &next = loadBlock.instructions[loadIdx + 1];
        if (!load.operands.empty() && next.op == il::core::Opcode::Store &&
            next.operands.size() >= 2 &&
            il::core::valueEquals(load.operands[0], next.operands[0]) &&
            next.operands[1].kind == il::core::Value::Kind::NullPtr) {
            return true;
        }
    }

    // Frontend-owned field snapshots place an explicit retain immediately
    // after the borrowed load. That retain dominates every later use,
    // including cleanup in successor blocks, and supplies exactly the +1 that
    // this backend fallback would otherwise add. Keep this deliberately
    // structural: a retain in another block might not execute on every path.
    if (loadIdx + 1 < loadBlock.instructions.size()) {
        const auto &next = loadBlock.instructions[loadIdx + 1];
        if (next.op == il::core::Opcode::Call &&
            zanna::codegen::isStringRetainCallee(next.callee)) {
            for (const auto &op : next.operands) {
                if (op.kind == il::core::Value::Kind::Temp && op.id == resultId)
                    return true;
            }
        }
    }

    if (!borrowOnlyInBlock)
        return false;
    // Borrow window: a string store can invalidate the source slot that owns
    // the borrowed reference. Explicit releases of other SSA values are not
    // barriers: in well-formed IL those values carry their own references,
    // and treating every unrelated release as a possible alias leaves the
    // load retain permanently unbalanced when the eventual use only borrows.
    for (std::size_t ii = loadIdx + 1; ii <= lastUseIdx; ++ii) {
        const auto &ins = loadBlock.instructions[ii];
        if (ins.op == il::core::Opcode::Store && ins.type.kind == il::core::Type::Kind::Str)
            return false;
    }
    return true;
}

/// @brief Bump the refcount on a runtime-string vreg via `bl rt_str_retain_maybe`.
/// @param out Machine block receiving the ABI move and runtime call.
/// @param strVReg GPR virtual register containing the string handle.
void retainStringVReg(MBasicBlock &out, uint16_t strVReg) {
    out.instrs.push_back(MInstr{
        MOpcode::MovRR, {MOperand::regOp(PhysReg::X0), MOperand::vregOp(RegClass::GPR, strVReg)}});
    out.instrs.push_back(MInstr{MOpcode::Bl, {MOperand::labelOp("rt_str_retain_maybe")}});
}

/// @brief Look up the recorded byte length for a global string literal symbol.
/// @param sym Literal label.
/// @param stringLiteralByteLengths Optional literal metadata map.
/// @return nullopt when @p sym is not in the table.
std::optional<std::size_t> lookupStringLiteralByteLen(
    const std::string &sym,
    const std::unordered_map<std::string, std::size_t> *stringLiteralByteLengths) {
    if (!stringLiteralByteLengths)
        return std::nullopt;
    const auto it = stringLiteralByteLengths->find(sym);
    if (it == stringLiteralByteLengths->end())
        return std::nullopt;
    return it->second;
}

} // namespace

void emitConstStrGlobalToX0(
    const std::string &sym,
    const std::unordered_map<std::string, std::size_t> *stringLiteralByteLengths,
    MBasicBlock &out,
    uint16_t &nextVRegId) {
    const uint16_t litPtrV = allocateNextVReg(nextVRegId);
    out.instrs.push_back(MInstr{
        MOpcode::AdrPage, {MOperand::vregOp(RegClass::GPR, litPtrV), MOperand::labelOp(sym)}});
    out.instrs.push_back(MInstr{MOpcode::AddPageOff,
                                {MOperand::vregOp(RegClass::GPR, litPtrV),
                                 MOperand::vregOp(RegClass::GPR, litPtrV),
                                 MOperand::labelOp(sym)}});
    out.instrs.push_back(MInstr{
        MOpcode::MovRR, {MOperand::regOp(PhysReg::X0), MOperand::vregOp(RegClass::GPR, litPtrV)}});

    if (const auto len = lookupStringLiteralByteLen(sym, stringLiteralByteLengths)) {
        const uint16_t lenV = allocateNextVReg(nextVRegId);
        out.instrs.push_back(MInstr{MOpcode::MovRI,
                                    {MOperand::vregOp(RegClass::GPR, lenV),
                                     MOperand::immOp(static_cast<long long>(*len))}});
        out.instrs.push_back(MInstr{
            MOpcode::MovRR, {MOperand::regOp(PhysReg::X1), MOperand::vregOp(RegClass::GPR, lenV)}});
        out.instrs.push_back(MInstr{MOpcode::Bl, {MOperand::labelOp("rt_str_from_lit")}});
        return;
    }

    out.instrs.push_back(MInstr{MOpcode::Bl, {MOperand::labelOp("rt_const_cstr")}});
}

uint16_t emitConstStrGlobalToVReg(
    const std::string &sym,
    const std::unordered_map<std::string, std::size_t> *stringLiteralByteLengths,
    MBasicBlock &out,
    uint16_t &nextVRegId) {
    emitConstStrGlobalToX0(sym, stringLiteralByteLengths, out, nextVRegId);
    const uint16_t dst = allocateNextVReg(nextVRegId);
    out.instrs.push_back(MInstr{
        MOpcode::MovRR, {MOperand::vregOp(RegClass::GPR, dst), MOperand::regOp(PhysReg::X0)}});
    return dst;
}

//===----------------------------------------------------------------------===//
// Value Materialization
//===----------------------------------------------------------------------===//

/// @brief Materialize an "immediate" IL value (ConstInt / ConstFloat / NullPtr /
///        GlobalAddr) into a fresh vreg. Returns false if @p v is not one of
///        those kinds, allowing the caller's full dispatcher to handle Temp.
/// @details Extracted from `materializeValueToVReg` so the 549-LOC monster can
///          read as "if immediate handle here, else handle Temp" — the four
///          immediate kinds share a common shape (push instr, return) and were
///          repeating boilerplate around outVReg/outCls assignment.
/// @param v Candidate immediate IL value.
/// @param out Machine block receiving materialization instructions.
/// @param nextVRegId Monotonic virtual-register allocator.
/// @param outVReg Receives the materialized virtual-register id.
/// @param outCls Receives its register class.
/// @return `true` for a supported immediate kind.
static bool materializeImmediateValue(const il::core::Value &v,
                                      MBasicBlock &out,
                                      uint16_t &nextVRegId,
                                      uint16_t &outVReg,
                                      RegClass &outCls) {
    using Kind = il::core::Value::Kind;
    if (v.kind == Kind::ConstInt || v.kind == Kind::NullPtr) {
        const long long imm = (v.kind == Kind::ConstInt) ? v.i64 : 0;
        outVReg = allocateNextVReg(nextVRegId);
        outCls = RegClass::GPR;
        out.instrs.push_back(
            MInstr{MOpcode::MovRI, {MOperand::vregOp(outCls, outVReg), MOperand::immOp(imm)}});
        return true;
    }
    if (v.kind == Kind::ConstFloat) {
        // Materialize FP constant by moving its bit-pattern via a GPR into an FPR.
        const auto bits = static_cast<long long>(zanna::codegen::common::f64Bits(v.f64));
        const uint16_t tmpG = allocateNextVReg(nextVRegId);
        out.instrs.push_back(
            MInstr{MOpcode::MovRI, {MOperand::vregOp(RegClass::GPR, tmpG), MOperand::immOp(bits)}});
        outVReg = allocateNextVReg(nextVRegId);
        outCls = RegClass::FPR;
        // fmov dV, xTmp  (bit-cast)
        out.instrs.push_back(MInstr{
            MOpcode::FMovGR,
            {MOperand::vregOp(RegClass::FPR, outVReg), MOperand::vregOp(RegClass::GPR, tmpG)}});
        return true;
    }
    if (v.kind == Kind::GlobalAddr) {
        // Direct GlobalAddr (function pointer or global symbol address)
        // Materialize via PC-relative AdrPage + AddPageOff
        outVReg = allocateNextVReg(nextVRegId);
        outCls = RegClass::GPR;
        const std::string sym = mapExternalSymbol(v.str);
        out.instrs.push_back(MInstr{
            MOpcode::AdrPage, {MOperand::vregOp(RegClass::GPR, outVReg), MOperand::labelOp(sym)}});
        out.instrs.push_back(MInstr{MOpcode::AddPageOff,
                                    {MOperand::vregOp(RegClass::GPR, outVReg),
                                     MOperand::vregOp(RegClass::GPR, outVReg),
                                     MOperand::labelOp(sym)}});
        return true;
    }
    return false;
}

/// @brief Materialize a function-entry parameter into a vreg (AAPCS64).
/// @details Returns true on success — meaning @p v references the bb's i-th
///          parameter and the appropriate ABI register move (or stack load) has
///          been emitted. Otherwise returns false and leaves outputs unchanged.
///
///          Behaviour preserved from the inline block formerly inside
///          `materializeValueToVReg`. Splits param classification (GPR vs FPR
///          sequence indexing) from stack-vs-register decision.
/// @param v Temporary that may identify an entry-block parameter.
/// @param bb Entry basic block and parameter list.
/// @param ti Target ABI argument-register orders.
/// @param out Machine block receiving the ABI move or stack load.
/// @param nextVRegId Monotonic virtual-register allocator.
/// @param outVReg Receives the materialized virtual-register id.
/// @param outCls Receives its register class.
/// @return `true` when @p v is a valid block parameter.
static bool materializeEntryParam(const il::core::Value &v,
                                  const il::core::BasicBlock &bb,
                                  const TargetInfo &ti,
                                  MBasicBlock &out,
                                  uint16_t &nextVRegId,
                                  uint16_t &outVReg,
                                  RegClass &outCls) {
    const int pIdx = indexOfParam(bb, v.id);
    if (pIdx < 0 || pIdx >= static_cast<int>(bb.params.size()))
        return false;

    RegClass cls = RegClass::GPR;
    if (bb.params[static_cast<std::size_t>(pIdx)].type.kind == il::core::Type::Kind::F64)
        cls = RegClass::FPR;

    // AAPCS64: GPR and FPR arguments use independent register sequences.
    // Count how many args of the same class precede this parameter.
    int classIdx = 0;
    for (int i = 0; i < pIdx; ++i) {
        const bool paramIsFP =
            bb.params[static_cast<std::size_t>(i)].type.kind == il::core::Type::Kind::F64;
        if ((cls == RegClass::FPR) == paramIsFP)
            ++classIdx;
    }

    const auto &argOrder = (cls == RegClass::FPR) ? ti.f64ArgOrder : ti.intArgOrder;
    if (classIdx >= static_cast<int>(argOrder.size())) {
        // Stack parameter: compute the caller arg offset.
        // After prologue, caller's stack args are at [FP + 16 + stackArgIdx * 8].
        int stackArgIdx = 0;
        int gprCount = 0;
        int fprCount = 0;
        for (int i = 0; i < pIdx; ++i) {
            const bool pIsFP =
                bb.params[static_cast<std::size_t>(i)].type.kind == il::core::Type::Kind::F64;
            if (pIsFP) {
                if (fprCount < static_cast<int>(ti.f64ArgOrder.size()))
                    ++fprCount;
                else
                    ++stackArgIdx;
            } else {
                if (gprCount < static_cast<int>(ti.intArgOrder.size()))
                    ++gprCount;
                else
                    ++stackArgIdx;
            }
        }
        const int callerArgOffset = 16 + stackArgIdx * 8;
        outVReg = allocateNextVReg(nextVRegId);
        outCls = cls;
        if (cls == RegClass::FPR) {
            out.instrs.push_back(
                MInstr{MOpcode::LdrFprFpImm,
                       {MOperand::vregOp(cls, outVReg), MOperand::immOp(callerArgOffset)}});
        } else {
            out.instrs.push_back(
                MInstr{MOpcode::LdrRegFpImm,
                       {MOperand::vregOp(cls, outVReg), MOperand::immOp(callerArgOffset)}});
            // Mask i1 parameters to ensure upper bits are zero (stack path).
            if (bb.params[static_cast<std::size_t>(pIdx)].type.kind == il::core::Type::Kind::I1)
                outVReg = emitMaskedI1Value(out, outVReg, nextVRegId);
        }
        return true;
    }

    // Register parameter: simple ABI move (with i1 masking on GPR path).
    outVReg = allocateNextVReg(nextVRegId);
    outCls = cls;
    const PhysReg src = argOrder[static_cast<std::size_t>(classIdx)];
    if (cls == RegClass::GPR) {
        out.instrs.push_back(
            MInstr{MOpcode::MovRR, {MOperand::vregOp(cls, outVReg), MOperand::regOp(src)}});
        if (bb.params[static_cast<std::size_t>(pIdx)].type.kind == il::core::Type::Kind::I1)
            outVReg = emitMaskedI1Value(out, outVReg, nextVRegId);
    } else {
        out.instrs.push_back(
            MInstr{MOpcode::FMovRR, {MOperand::vregOp(cls, outVReg), MOperand::regOp(src)}});
    }
    return true;
}

/// @brief Lower the IL producer instruction for @p v inline (binary ops,
///        ConstStr, AddrOf, GEP, comparisons, Load).
/// @details Returns true on success. Recursively calls
///          `materializeValueToVReg` for nested operands.
/// @param v Temporary whose producer is sought in @p bb.
/// @param bb Basic block containing the producer.
/// @param ti Target ABI description.
/// @param fb Frame layout used for local addresses.
/// @param out Machine block receiving recursively emitted MIR.
/// @param tempVReg Existing temporary-to-vreg cache, updated on success.
/// @param tempRegClass Existing temporary-to-class cache, updated on success.
/// @param nextVRegId Monotonic virtual-register allocator.
/// @param outVReg Receives the producer result virtual register.
/// @param outCls Receives its register class.
/// @param stringLiteralByteLengths Optional literal size metadata.
/// @return `true` when the producer shape is supported.
static bool materializeFromProducer(
    const il::core::Value &v,
    const il::core::BasicBlock &bb,
    const TargetInfo &ti,
    FrameBuilder &fb,
    MBasicBlock &out,
    std::unordered_map<unsigned, uint16_t> &tempVReg,
    std::unordered_map<unsigned, RegClass> &tempRegClass,
    uint16_t &nextVRegId,
    uint16_t &outVReg,
    RegClass &outCls,
    const std::unordered_map<std::string, std::size_t> *stringLiteralByteLengths) {
    using Opcode = il::core::Opcode;

    auto prodIt =
        std::find_if(bb.instructions.begin(), bb.instructions.end(), [&](const il::core::Instr &I) {
            return I.result && *I.result == v.id;
        });
    if (prodIt == bb.instructions.end())
        return false;

    auto emitRRR = [&](MOpcode opc, const il::core::Value &a, const il::core::Value &b) -> bool {
        uint16_t va = 0, vb = 0;
        RegClass ca = RegClass::GPR, cb = RegClass::GPR;
        if (!materializeValueToVReg(a, bb, ti, fb, out, tempVReg, tempRegClass, nextVRegId, va, ca))
            return false;
        if (!materializeValueToVReg(b, bb, ti, fb, out, tempVReg, tempRegClass, nextVRegId, vb, cb))
            return false;
        const bool isFP = (opc == MOpcode::FAddRRR || opc == MOpcode::FSubRRR ||
                           opc == MOpcode::FMulRRR || opc == MOpcode::FDivRRR);
        if (isFP) {
            va = coerceScalarOperandToFpr(va, ca, nextVRegId, out);
            vb = coerceScalarOperandToFpr(vb, cb, nextVRegId, out);
        } else if (ca != RegClass::GPR || cb != RegClass::GPR) {
            return false;
        }
        outVReg = allocateNextVReg(nextVRegId);
        outCls = isFP ? RegClass::FPR : RegClass::GPR;
        out.instrs.push_back(MInstr{opc,
                                    {MOperand::vregOp(outCls, outVReg),
                                     MOperand::vregOp(outCls, va),
                                     MOperand::vregOp(outCls, vb)}});
        return true;
    };
    auto emitRImm = [&](MOpcode opc, const il::core::Value &a, long long imm) -> bool {
        uint16_t va = 0;
        RegClass ca = RegClass::GPR;
        if (!materializeValueToVReg(a, bb, ti, fb, out, tempVReg, tempRegClass, nextVRegId, va, ca))
            return false;
        outVReg = allocateNextVReg(nextVRegId);
        outCls = (opc == MOpcode::FAddRRR || opc == MOpcode::FSubRRR || opc == MOpcode::FMulRRR ||
                  opc == MOpcode::FDivRRR)
                     ? RegClass::FPR
                     : RegClass::GPR;
        if (opc == MOpcode::AddRI || opc == MOpcode::SubRI || opc == MOpcode::AddOvfRI ||
            opc == MOpcode::SubOvfRI) {
            emitLegalizedSignedImmArith(
                out,
                MOperand::vregOp(outCls, outVReg),
                MOperand::vregOp(outCls, va),
                imm,
                (opc == MOpcode::AddRI || opc == MOpcode::AddOvfRI) ? SignedImmArithKind::Add
                                                                    : SignedImmArithKind::Sub,
                (opc == MOpcode::AddOvfRI || opc == MOpcode::SubOvfRI) ? MOpcode::AddOvfRI
                                                                       : MOpcode::AddRI,
                (opc == MOpcode::AddOvfRI || opc == MOpcode::SubOvfRI) ? MOpcode::SubOvfRI
                                                                       : MOpcode::SubRI,
                (opc == MOpcode::AddOvfRI) ? MOpcode::AddOvfRRR : MOpcode::AddRRR,
                (opc == MOpcode::SubOvfRI) ? MOpcode::SubOvfRRR : MOpcode::SubRRR,
                [&](long long materializedImm) {
                    const uint16_t tmp = allocateNextVReg(nextVRegId);
                    out.instrs.push_back(MInstr{
                        MOpcode::MovRI,
                        {MOperand::vregOp(RegClass::GPR, tmp), MOperand::immOp(materializedImm)}});
                    return MOperand::vregOp(RegClass::GPR, tmp);
                });
            return true;
        }
        out.instrs.push_back(MInstr{opc,
                                    {MOperand::vregOp(outCls, outVReg),
                                     MOperand::vregOp(outCls, va),
                                     MOperand::immOp(imm)}});
        return true;
    };

    const auto &prod = *prodIt;

    // Check for binary operations first using table lookup
    if (const auto *binOp = lookupBinaryOp(prod.op)) {
        if (prod.operands.size() == 2) {
            const bool isImmCandidate = binOp->supportsImmediate &&
                                        prod.operands[1].kind == il::core::Value::Kind::ConstInt;
            // For bitwise ops, validate that the constant is a logical immediate.
            const bool isBitwiseImm =
                isImmCandidate &&
                (prod.op == il::core::Opcode::And || prod.op == il::core::Opcode::Or ||
                 prod.op == il::core::Opcode::Xor) &&
                isLogicalImmediate(static_cast<uint64_t>(prod.operands[1].i64));
            const bool isShift = prod.op == il::core::Opcode::Shl ||
                                 prod.op == il::core::Opcode::LShr ||
                                 prod.op == il::core::Opcode::AShr;
            const bool isAddSub =
                prod.op == il::core::Opcode::Add || prod.op == il::core::Opcode::IAddOvf ||
                prod.op == il::core::Opcode::Sub || prod.op == il::core::Opcode::ISubOvf;
            const bool isOtherImm =
                isImmCandidate && !isShift && !isAddSub && prod.op != il::core::Opcode::And &&
                prod.op != il::core::Opcode::Or && prod.op != il::core::Opcode::Xor;
            const bool isShiftImm =
                isImmCandidate && isShift && isValidShiftAmount(prod.operands[1].i64);
            const bool isAddSubImm = isImmCandidate && isAddSub;
            if (isBitwiseImm || isShiftImm || isAddSubImm || isOtherImm) {
                if (emitRImm(binOp->immOp, prod.operands[0], prod.operands[1].i64)) {
                    // Cache result to prevent re-materialization with different vreg
                    tempVReg[v.id] = outVReg;
                    return true;
                }
                return false;
            } else {
                // Use register-register form (includes shifts with register amount)
                if (emitRRR(binOp->mirOp, prod.operands[0], prod.operands[1])) {
                    // Cache result to prevent re-materialization with different vreg
                    tempVReg[v.id] = outVReg;
                    return true;
                }
                return false;
            }
        }
    }

    // Handle other operations
    switch (prod.op) {
        case Opcode::ConstStr:
            if (!prod.operands.empty() &&
                prod.operands[0].kind == il::core::Value::Kind::GlobalAddr) {
                const std::string &sym = prod.operands[0].str;
                outVReg = emitConstStrGlobalToVReg(sym, stringLiteralByteLengths, out, nextVRegId);
                outCls = RegClass::GPR;
                // Cache for reuse
                tempVReg[v.id] = outVReg;
                return true;
            }
            break;
        case Opcode::AddrOf:
            if (!prod.operands.empty() &&
                prod.operands[0].kind == il::core::Value::Kind::GlobalAddr) {
                outVReg = allocateNextVReg(nextVRegId);
                outCls = RegClass::GPR;
                const std::string sym = mapExternalSymbol(prod.operands[0].str);
                out.instrs.push_back(
                    MInstr{MOpcode::AdrPage,
                           {MOperand::vregOp(RegClass::GPR, outVReg), MOperand::labelOp(sym)}});
                out.instrs.push_back(MInstr{MOpcode::AddPageOff,
                                            {MOperand::vregOp(RegClass::GPR, outVReg),
                                             MOperand::vregOp(RegClass::GPR, outVReg),
                                             MOperand::labelOp(sym)}});
                tempVReg[v.id] = outVReg;
                return true;
            }
            // AddrOf of a local alloca: compute FP + frame_offset
            if (!prod.operands.empty() && prod.operands[0].kind == il::core::Value::Kind::Temp) {
                const int offset = fb.localOffset(prod.operands[0].id);
                if (offset != 0) {
                    outVReg = allocateNextVReg(nextVRegId);
                    outCls = RegClass::GPR;
                    out.instrs.push_back(MInstr{
                        MOpcode::AddFpImm,
                        {MOperand::vregOp(RegClass::GPR, outVReg), MOperand::immOp(offset)}});
                    tempVReg[v.id] = outVReg;
                    return true;
                }
            }
            break;
        case Opcode::GEP:
            if (prod.operands.size() >= 2) {
                uint16_t vbase = 0, voff = 0;
                RegClass cbase = RegClass::GPR, coff = RegClass::GPR;
                if (!materializeValueToVReg(prod.operands[0],
                                            bb,
                                            ti,
                                            fb,
                                            out,
                                            tempVReg,
                                            tempRegClass,
                                            nextVRegId,
                                            vbase,
                                            cbase))
                    return false;
                outVReg = allocateNextVReg(nextVRegId);
                outCls = RegClass::GPR;
                const auto &offVal = prod.operands[1];
                if (offVal.kind == il::core::Value::Kind::ConstInt) {
                    const long long imm = offVal.i64;
                    if (imm == 0) {
                        out.instrs.push_back(MInstr{MOpcode::MovRR,
                                                    {MOperand::vregOp(RegClass::GPR, outVReg),
                                                     MOperand::vregOp(RegClass::GPR, vbase)}});
                    } else {
                        emitLegalizedSignedImmArith(
                            out,
                            MOperand::vregOp(RegClass::GPR, outVReg),
                            MOperand::vregOp(RegClass::GPR, vbase),
                            imm,
                            SignedImmArithKind::Add,
                            MOpcode::AddRI,
                            MOpcode::SubRI,
                            MOpcode::AddRRR,
                            MOpcode::SubRRR,
                            [&](long long materializedImm) {
                                const uint16_t tmp = allocateNextVReg(nextVRegId);
                                out.instrs.push_back(MInstr{MOpcode::MovRI,
                                                            {MOperand::vregOp(RegClass::GPR, tmp),
                                                             MOperand::immOp(materializedImm)}});
                                return MOperand::vregOp(RegClass::GPR, tmp);
                            });
                    }
                } else {
                    if (!materializeValueToVReg(offVal,
                                                bb,
                                                ti,
                                                fb,
                                                out,
                                                tempVReg,
                                                tempRegClass,
                                                nextVRegId,
                                                voff,
                                                coff))
                        return false;
                    out.instrs.push_back(MInstr{MOpcode::AddRRR,
                                                {MOperand::vregOp(RegClass::GPR, outVReg),
                                                 MOperand::vregOp(RegClass::GPR, vbase),
                                                 MOperand::vregOp(RegClass::GPR, voff)}});
                }
                tempVReg[v.id] = outVReg;
                return true;
            }
            break;
        default:
            // Check if it's a comparison operation
            if (isCompareOp(prod.op)) {
                if (prod.operands.size() == 2) {
                    uint16_t va = 0, vb = 0;
                    RegClass ca = RegClass::GPR, cb = RegClass::GPR;
                    if (!materializeValueToVReg(prod.operands[0],
                                                bb,
                                                ti,
                                                fb,
                                                out,
                                                tempVReg,
                                                tempRegClass,
                                                nextVRegId,
                                                va,
                                                ca))
                        return false;
                    if (!materializeValueToVReg(prod.operands[1],
                                                bb,
                                                ti,
                                                fb,
                                                out,
                                                tempVReg,
                                                tempRegClass,
                                                nextVRegId,
                                                vb,
                                                cb))
                        return false;
                    if (isFloatingPointCompareOp(prod.op)) {
                        va = coerceScalarOperandToFpr(va, ca, nextVRegId, out);
                        vb = coerceScalarOperandToFpr(vb, cb, nextVRegId, out);
                        out.instrs.push_back(MInstr{MOpcode::FCmpRR,
                                                    {MOperand::vregOp(RegClass::FPR, va),
                                                     MOperand::vregOp(RegClass::FPR, vb)}});
                    } else {
                        if (ca != RegClass::GPR || cb != RegClass::GPR)
                            return false;
                        out.instrs.push_back(MInstr{MOpcode::CmpRR,
                                                    {MOperand::vregOp(RegClass::GPR, va),
                                                     MOperand::vregOp(RegClass::GPR, vb)}});
                    }
                    outVReg = allocateNextVReg(nextVRegId);
                    outCls = RegClass::GPR;
                    if (isFloatingPointCompareOp(prod.op)) {
                        emitFpCompareResult(out, prod.op, outVReg, nextVRegId);
                    } else {
                        out.instrs.push_back(MInstr{MOpcode::Cset,
                                                    {MOperand::vregOp(RegClass::GPR, outVReg),
                                                     MOperand::condOp(condForOpcode(prod.op))}});
                    }
                    // Cache result to prevent re-materialization with different vreg
                    tempVReg[v.id] = outVReg;
                    return true;
                }
            }
            break;
        case Opcode::Load:
            if (!prod.operands.empty() && prod.operands.size() <= 2) {
                long long off = 0;
                if (prod.operands.size() == 2) {
                    if (prod.operands[1].kind != il::core::Value::Kind::ConstInt)
                        return false;
                    off = prod.operands[1].i64;
                }
                auto resolveFrameAddressInBlock =
                    [&](auto &&self, const il::core::Value &addr, long long &offset) -> bool {
                    if (addr.kind != il::core::Value::Kind::Temp)
                        return false;
                    if (const int localOffset = fb.localOffset(addr.id); localOffset != 0) {
                        offset += localOffset;
                        return true;
                    }
                    const auto prodAddrIt = std::find_if(
                        bb.instructions.begin(),
                        bb.instructions.end(),
                        [&](const il::core::Instr &I) { return I.result && *I.result == addr.id; });
                    if (prodAddrIt == bb.instructions.end())
                        return false;
                    if (prodAddrIt->op == Opcode::AddrOf && !prodAddrIt->operands.empty())
                        return self(self, prodAddrIt->operands[0], offset);
                    if (prodAddrIt->op == Opcode::GEP && prodAddrIt->operands.size() >= 2 &&
                        prodAddrIt->operands[1].kind == il::core::Value::Kind::ConstInt &&
                        self(self, prodAddrIt->operands[0], offset)) {
                        offset += prodAddrIt->operands[1].i64;
                        return true;
                    }
                    return false;
                };
                const bool frameRelative =
                    resolveFrameAddressInBlock(resolveFrameAddressInBlock, prod.operands[0], off);
                const bool isFP = prod.type.kind == il::core::Type::Kind::F64;
                outVReg = allocateNextVReg(nextVRegId);
                outCls = isFP ? RegClass::FPR : RegClass::GPR;
                if (frameRelative) {
                    const MOpcode loadOpc =
                        isFP ? MOpcode::LdrFprFpImm
                             : gprLoadOpcodeForType(prod.type.kind, /*frameRelative=*/true);
                    out.instrs.push_back(
                        MInstr{loadOpc, {MOperand::vregOp(outCls, outVReg), MOperand::immOp(off)}});
                } else {
                    uint16_t vbase = 0;
                    RegClass cbase = RegClass::GPR;
                    if (!materializeValueToVReg(prod.operands[0],
                                                bb,
                                                ti,
                                                fb,
                                                out,
                                                tempVReg,
                                                tempRegClass,
                                                nextVRegId,
                                                vbase,
                                                cbase) ||
                        cbase != RegClass::GPR)
                        return false;
                    const MOpcode loadOpc =
                        isFP ? MOpcode::LdrFprBaseImm
                             : gprLoadOpcodeForType(prod.type.kind, /*frameRelative=*/false);
                    out.instrs.push_back(MInstr{loadOpc,
                                                {MOperand::vregOp(outCls, outVReg),
                                                 MOperand::vregOp(RegClass::GPR, vbase),
                                                 MOperand::immOp(off)}});
                }
                if (prod.type.kind == il::core::Type::Kind::Str)
                    retainStringVReg(out, outVReg);
                tempRegClass[v.id] = outCls;
                if (prod.type.kind == il::core::Type::Kind::I1) {
                    outVReg = emitMaskedI1Value(out, outVReg, nextVRegId);
                    tempRegClass[v.id] = RegClass::GPR;
                }
                tempVReg[v.id] = outVReg;
                return true;
            }
            break;
    }
    return false;
}

/// @brief Top-level dispatch: materialize @p v into a fresh vreg.
/// @details Tries (in order): immediate constants, Temp cache, alloca address,
///          function-entry parameters, then producer-instruction recursion.
///          Returns false if no path matched — the caller is expected to abort
///          lowering of the enclosing instruction.
///
///          The decomposition is purely organisational; each sub-helper
///          captures one logical category of value source.
bool materializeValueToVReg(
    const il::core::Value &v,
    const il::core::BasicBlock &bb,
    const TargetInfo &ti,
    FrameBuilder &fb,
    MBasicBlock &out,
    std::unordered_map<unsigned, uint16_t> &tempVReg,
    std::unordered_map<unsigned, RegClass> &tempRegClass,
    uint16_t &nextVRegId,
    uint16_t &outVReg,
    RegClass &outCls,
    const std::unordered_map<std::string, std::size_t> *stringLiteralByteLengths) {
    if (materializeImmediateValue(v, out, nextVRegId, outVReg, outCls))
        return true;
    if (v.kind != il::core::Value::Kind::Temp)
        return false;

    // Cache hit: already materialized (e.g. block params loaded into spill slots
    // in non-entry blocks register the chosen vreg here on first use).
    if (auto it = tempVReg.find(v.id); it != tempVReg.end()) {
        outVReg = it->second;
        auto clsIt = tempRegClass.find(v.id);
        outCls = (clsIt != tempRegClass.end()) ? clsIt->second : RegClass::GPR;
        return true;
    }

    // Alloca temp: compute fp+offset address each time (NOT cached, because the
    // vreg->phys mapping changes across blocks and the address must be recomputed).
    if (const int allocaOff = fb.localOffset(v.id); allocaOff != 0) {
        outVReg = allocateNextVReg(nextVRegId);
        outCls = RegClass::GPR;
        out.instrs.push_back(
            MInstr{MOpcode::AddFpImm,
                   {MOperand::vregOp(RegClass::GPR, outVReg), MOperand::immOp(allocaOff)}});
        return true;
    }

    // Function-entry parameter: ABI move (or stack load for overflowed params).
    if (materializeEntryParam(v, bb, ti, out, nextVRegId, outVReg, outCls))
        return true;

    // Last resort: find the producing instruction in this block and inline its
    // lowering recursively.
    return materializeFromProducer(v,
                                   bb,
                                   ti,
                                   fb,
                                   out,
                                   tempVReg,
                                   tempRegClass,
                                   nextVRegId,
                                   outVReg,
                                   outCls,
                                   stringLiteralByteLengths);
}

//===----------------------------------------------------------------------===//
// Call Lowering
//===----------------------------------------------------------------------===//

bool lowerCallWithArgs(
    const il::core::Instr &callI,
    const il::core::BasicBlock &bb,
    const TargetInfo &ti,
    FrameBuilder &fb,
    MBasicBlock &out,
    LoweredCall &seq,
    std::unordered_map<unsigned, uint16_t> &tempVReg,
    std::unordered_map<unsigned, RegClass> &tempRegClass,
    uint16_t &nextVRegId,
    const std::unordered_map<std::string, std::size_t> *knownVarArgNamedArgCounts) {
    // Callee can be in either callI.callee field or operands[0] as GlobalAddr
    std::string callee;
    std::size_t argStart = 0;

    if (!callI.callee.empty()) {
        // Modern IL convention: callee in dedicated field, all operands are arguments
        callee = callI.callee;
        argStart = 0;
    } else if (!callI.operands.empty() &&
               callI.operands[0].kind == il::core::Value::Kind::GlobalAddr) {
        // Legacy convention: callee as GlobalAddr in operands[0]
        callee = callI.operands[0].str;
        argStart = 1;
    } else {
        return false;
    }

    // Map canonical runtime names (e.g., "Zanna.Grid2D.New") to C symbols (e.g., "rt_grid2d_new")
    std::string mappedCallee = callee;
    if (auto mapped = il::runtime::mapCanonicalRuntimeName(callee))
        mappedCallee = std::string(*mapped);

    seq.call = MInstr{MOpcode::Bl, {MOperand::labelOp(mappedCallee)}};

    // Detect variadic callees. Darwin AArch64 spills anonymous variadic args
    // to stack; Linux and Windows continue to use their normal register banks.
    const bool isVarArg = il::runtime::isVarArgCallee(mappedCallee) ||
                          il::runtime::isVarArgCallee(callee) ||
                          isKnownVarArgCallee(mappedCallee, knownVarArgNamedArgCounts) ||
                          isKnownVarArgCallee(callee, knownVarArgNamedArgCounts);
    std::size_t namedArgCount = SIZE_MAX; // default: all args are "named"
    if (isVarArg) {
        // Look up the function's signature to determine the named parameter count.
        // The target flag decides whether the variadic tail spills to stack.
        if (const auto *sig = il::runtime::findRuntimeSignature(mappedCallee))
            namedArgCount = sig->paramTypes.size();
        else if (const auto *sig2 = il::runtime::findRuntimeSignature(callee))
            namedArgCount = sig2->paramTypes.size();
        else if (const auto mappedNamedArgs =
                     lookupKnownVarArgNamedArgs(mappedCallee, knownVarArgNamedArgCounts))
            namedArgCount = *mappedNamedArgs;
        else if (const auto rawNamedArgs =
                     lookupKnownVarArgNamedArgs(callee, knownVarArgNamedArgCounts))
            namedArgCount = *rawNamedArgs;
        else
            namedArgCount = 0; // Conservative: all args to stack for unknown vararg callees
    }

    // First pass: materialize all arguments and collect them
    struct ArgInfo {
        uint16_t vreg{0};
        zanna::codegen::common::CallArgClass cls{zanna::codegen::common::CallArgClass::GPR};
    };

    std::vector<ArgInfo> args;
    args.reserve(callI.operands.size() - argStart);
    for (std::size_t i = argStart; i < callI.operands.size(); ++i) {
        const auto &arg = callI.operands[i];
        uint16_t vr = 0;
        RegClass cls = RegClass::GPR;
        if (!materializeValueToVReg(
                arg, bb, ti, fb, out, tempVReg, tempRegClass, nextVRegId, vr, cls))
            return false;
        args.push_back({vr,
                        cls == RegClass::FPR ? zanna::codegen::common::CallArgClass::FPR
                                             : zanna::codegen::common::CallArgClass::GPR});
    }

    std::vector<MaterializedCallArg> materialized;
    materialized.reserve(args.size());
    for (const auto &arg : args)
        materialized.push_back({arg.vreg, arg.cls});

    return marshalCallArgs(
        materialized, namedArgCount, isVarArg && ti.usesStackVariadicTail(), ti, nextVRegId, seq);
}

//===----------------------------------------------------------------------===//
// Division/Remainder with Divide-by-Zero Check (parameterized)
//===----------------------------------------------------------------------===//

/// @brief Shared implementation for division and remainder with divide-by-zero check.
/// @details Generates: materialize lhs/rhs, cmp rhs #0, b.eq trap, div/rem, trap block.
///          The four public handlers (lowerSRemChk0, lowerSDivChk0, lowerUDivChk0,
///          lowerURemChk0) are thin wrappers over this single implementation.
/// @param ins       The IL instruction to lower.
/// @param bb        The IL basic block containing @p ins.
/// @param ctx       Lowering context for register allocation and frame state.
/// @param out       The output MIR basic block receiving the lowered sequence.
/// @param isSigned  True for signed (sdiv), false for unsigned (udiv).
/// @param isRemainder True to compute remainder via quotient + msub, false for quotient only.
/// @return True on success, false on lowering failure.
static bool lowerDivisionChk0(const il::core::Instr &ins,
                              const il::core::BasicBlock &bb,
                              LoweringContext &ctx,
                              MBasicBlock &out,
                              bool isSigned,
                              bool isRemainder) {
    if (!ins.result || ins.operands.size() < 2)
        return false;

    // Materialize lhs and rhs
    uint16_t lhs = 0, rhs = 0;
    RegClass lhsCls = RegClass::GPR, rhsCls = RegClass::GPR;

    if (!materializeValueToVReg(ins.operands[0],
                                bb,
                                ctx.ti,
                                ctx.fb,
                                out,
                                ctx.tempVReg,
                                ctx.tempRegClass,
                                ctx.nextVRegId,
                                lhs,
                                lhsCls))
        return false;
    if (!materializeValueToVReg(ins.operands[1],
                                bb,
                                ctx.ti,
                                ctx.fb,
                                out,
                                ctx.tempVReg,
                                ctx.tempRegClass,
                                ctx.nextVRegId,
                                rhs,
                                rhsCls))
        return false;

    // Generate divide-by-zero check: cmp rhs, #0; b.eq trap_label
    const std::string trapLabel = requestSharedTrapBlock(ctx, "div0", "rt_trap_div0");
    out.instrs.push_back(
        MInstr{MOpcode::CmpRI, {MOperand::vregOp(RegClass::GPR, rhs), MOperand::immOp(0)}});
    out.instrs.push_back(
        MInstr{MOpcode::BCond, {MOperand::condOp("eq"), MOperand::labelOp(trapLabel)}});

    uint16_t overflowFlag = 0;
    if (isSigned) {
        const uint16_t minV = allocateNextVReg(ctx.nextVRegId);
        out.instrs.push_back(MInstr{MOpcode::MovRI,
                                    {MOperand::vregOp(RegClass::GPR, minV),
                                     MOperand::immOp(std::numeric_limits<long long>::min())}});

        const uint16_t lhsIsMin = allocateNextVReg(ctx.nextVRegId);
        out.instrs.push_back(
            MInstr{MOpcode::CmpRR,
                   {MOperand::vregOp(RegClass::GPR, lhs), MOperand::vregOp(RegClass::GPR, minV)}});
        out.instrs.push_back(MInstr{
            MOpcode::Cset, {MOperand::vregOp(RegClass::GPR, lhsIsMin), MOperand::condOp("eq")}});

        const uint16_t rhsIsNegOne = allocateNextVReg(ctx.nextVRegId);
        out.instrs.push_back(
            MInstr{MOpcode::CmpRI, {MOperand::vregOp(RegClass::GPR, rhs), MOperand::immOp(-1)}});
        out.instrs.push_back(MInstr{
            MOpcode::Cset, {MOperand::vregOp(RegClass::GPR, rhsIsNegOne), MOperand::condOp("eq")}});

        overflowFlag = allocateNextVReg(ctx.nextVRegId);
        out.instrs.push_back(MInstr{MOpcode::AndRRR,
                                    {MOperand::vregOp(RegClass::GPR, overflowFlag),
                                     MOperand::vregOp(RegClass::GPR, lhsIsMin),
                                     MOperand::vregOp(RegClass::GPR, rhsIsNegOne)}});

        if (!isRemainder) {
            const std::string trapOvfLabel = requestSharedTrapBlock(ctx, "ovf", "rt_trap_ovf");
            out.instrs.push_back(
                MInstr{MOpcode::CmpRI,
                       {MOperand::vregOp(RegClass::GPR, overflowFlag), MOperand::immOp(0)}});
            out.instrs.push_back(
                MInstr{MOpcode::BCond, {MOperand::condOp("ne"), MOperand::labelOp(trapOvfLabel)}});
        }
    }

    // Select division opcode based on signedness.
    const MOpcode divOp = isSigned ? MOpcode::SDivRRR : MOpcode::UDivRRR;

    if (isRemainder) {
        // Compute quotient: [su]div tmp, lhs, rhs
        const uint16_t quotient = allocateNextVReg(ctx.nextVRegId);
        out.instrs.push_back(MInstr{divOp,
                                    {MOperand::vregOp(RegClass::GPR, quotient),
                                     MOperand::vregOp(RegClass::GPR, lhs),
                                     MOperand::vregOp(RegClass::GPR, rhs)}});

        // Compute remainder: msub dst, quotient, rhs, lhs => dst = lhs - quotient*rhs
        const uint16_t dst = allocateNextVReg(ctx.nextVRegId);
        ctx.tempVReg[*ins.result] = dst;
        out.instrs.push_back(MInstr{MOpcode::MSubRRRR,
                                    {MOperand::vregOp(RegClass::GPR, dst),
                                     MOperand::vregOp(RegClass::GPR, quotient),
                                     MOperand::vregOp(RegClass::GPR, rhs),
                                     MOperand::vregOp(RegClass::GPR, lhs)}});
        if (isSigned) {
            const uint16_t zeroV = allocateNextVReg(ctx.nextVRegId);
            out.instrs.push_back(MInstr{
                MOpcode::MovRI, {MOperand::vregOp(RegClass::GPR, zeroV), MOperand::immOp(0)}});
            out.instrs.push_back(
                MInstr{MOpcode::CmpRI,
                       {MOperand::vregOp(RegClass::GPR, overflowFlag), MOperand::immOp(0)}});
            out.instrs.push_back(MInstr{MOpcode::Csel,
                                        {MOperand::vregOp(RegClass::GPR, dst),
                                         MOperand::vregOp(RegClass::GPR, dst),
                                         MOperand::vregOp(RegClass::GPR, zeroV),
                                         MOperand::condOp("eq")}});
        }
    } else {
        // Compute division directly: [su]div dst, lhs, rhs
        const uint16_t dst = allocateNextVReg(ctx.nextVRegId);
        ctx.tempVReg[*ins.result] = dst;
        out.instrs.push_back(MInstr{divOp,
                                    {MOperand::vregOp(RegClass::GPR, dst),
                                     MOperand::vregOp(RegClass::GPR, lhs),
                                     MOperand::vregOp(RegClass::GPR, rhs)}});
    }

    return true;
}

bool lowerSRemChk0(const il::core::Instr &ins,
                   const il::core::BasicBlock &bb,
                   LoweringContext &ctx,
                   MBasicBlock &out) {
    return lowerDivisionChk0(ins, bb, ctx, out, /*isSigned=*/true, /*isRemainder=*/true);
}

bool lowerSDivChk0(const il::core::Instr &ins,
                   const il::core::BasicBlock &bb,
                   LoweringContext &ctx,
                   MBasicBlock &out) {
    return lowerDivisionChk0(ins, bb, ctx, out, /*isSigned=*/true, /*isRemainder=*/false);
}

bool lowerUDivChk0(const il::core::Instr &ins,
                   const il::core::BasicBlock &bb,
                   LoweringContext &ctx,
                   MBasicBlock &out) {
    return lowerDivisionChk0(ins, bb, ctx, out, /*isSigned=*/false, /*isRemainder=*/false);
}

bool lowerURemChk0(const il::core::Instr &ins,
                   const il::core::BasicBlock &bb,
                   LoweringContext &ctx,
                   MBasicBlock &out) {
    return lowerDivisionChk0(ins, bb, ctx, out, /*isSigned=*/false, /*isRemainder=*/true);
}

//===----------------------------------------------------------------------===//
// Index Bounds Check (idx.chk)
//===----------------------------------------------------------------------===//

/// @brief Return the bit-width of an integer IL type.
/// @details Maps `I1`→1, `I16`→16, `I32`→32, and any other kind (including `I64`,
///          pointers, and non-integer types) to 64. Used by sign- and zero-extension
///          paths that need to know how many bits to shift through.
/// @param kind IL type kind whose bit width is queried.
/// @return Bit width in `[1, 64]`.
static int integerTypeBits(il::core::Type::Kind kind) {
    switch (kind) {
        case il::core::Type::Kind::I1:
            return 1;
        case il::core::Type::Kind::I16:
            return 16;
        case il::core::Type::Kind::I32:
            return 32;
        default:
            return 64;
    }
}

/// @brief Inclusive minimum representable value of a @p bits-wide signed integer,
///        as an exact double.
/// @details Returns -2^(bits-1) (e.g. -2147483648.0 for 32). Used by the
///          float→signed-int narrowing path to clamp/range-check before fcvtzs.
/// @param bits Signed integer width; must be 1, 16, 32, or 64.
/// @return The lower bound as a double.
/// @throws std::runtime_error if @p bits is not a supported width.
static double signedLowerBoundForBits(int bits) {
    switch (bits) {
        case 1:
        case 16:
        case 32:
        case 64:
            return -std::ldexp(1.0, bits - 1);
        default:
            throw std::runtime_error("AArch64 lowering: unsupported signed fp cast width");
    }
}

/// @brief Exclusive upper bound of a @p bits-wide signed integer, as an exact double.
/// @details Returns +2^(bits-1) (e.g. 2147483648.0 for 32) — the first value
///          that does NOT fit. The narrowing path rejects inputs >= this bound.
/// @param bits Signed integer width; must be 1, 16, 32, or 64.
/// @return The exclusive upper bound as a double.
/// @throws std::runtime_error if @p bits is not a supported width.
static double signedUpperExclusiveForBits(int bits) {
    switch (bits) {
        case 1:
        case 16:
        case 32:
        case 64:
            return std::ldexp(1.0, bits - 1);
        default:
            throw std::runtime_error("AArch64 lowering: unsupported signed fp cast width");
    }
}

/// @brief Emit a sign-extension sequence and return the destination vreg.
/// @details Emits LSL #(64-bits) / ASR #(64-bits) to sign-extend @p src from @p bits to 64 bits.
///          Returns @p src unchanged when @p bits >= 64 (no-op).
/// @param src  Source vreg (GPR) containing the value to sign-extend.
/// @param bits Target signed width in bits (e.g. 16 for I16 → I64).
/// @param ctx  Lowering context for vreg allocation.
/// @param out  Output MIR block; LSL+ASR instructions are appended.
/// @return Vreg holding the sign-extended result (may equal @p src if bits >= 64).
static uint16_t signExtendVRegToWidth(uint16_t src,
                                      int bits,
                                      LoweringContext &ctx,
                                      MBasicBlock &out) {
    if (bits >= 64)
        return src;
    const int shift = 64 - bits;
    const uint16_t dst = allocateNextVReg(ctx.nextVRegId);
    out.instrs.push_back(MInstr{MOpcode::LslRI,
                                {MOperand::vregOp(RegClass::GPR, dst),
                                 MOperand::vregOp(RegClass::GPR, src),
                                 MOperand::immOp(shift)}});
    out.instrs.push_back(MInstr{MOpcode::AsrRI,
                                {MOperand::vregOp(RegClass::GPR, dst),
                                 MOperand::vregOp(RegClass::GPR, dst),
                                 MOperand::immOp(shift)}});
    return dst;
}

bool lowerIdxChk(const il::core::Instr &ins,
                 const il::core::BasicBlock &bb,
                 LoweringContext &ctx,
                 MBasicBlock &out) {
    // idx.chk checks: lo <= idx < hi
    // operands[0] = idx, operands[1] = lo, operands[2] = hi
    if (!ins.result || ins.operands.size() < 3)
        return false;

    uint16_t idxV = 0, loV = 0, hiV = 0;
    RegClass idxCls = RegClass::GPR, loCls = RegClass::GPR, hiCls = RegClass::GPR;

    if (!materializeValueToVReg(ins.operands[0],
                                bb,
                                ctx.ti,
                                ctx.fb,
                                out,
                                ctx.tempVReg,
                                ctx.tempRegClass,
                                ctx.nextVRegId,
                                idxV,
                                idxCls))
        return false;

    // Check if lo is constant 0 (common optimization)
    const bool loIsZero =
        ins.operands[1].kind == il::core::Value::Kind::ConstInt && ins.operands[1].i64 == 0;

    if (!loIsZero) {
        if (!materializeValueToVReg(ins.operands[1],
                                    bb,
                                    ctx.ti,
                                    ctx.fb,
                                    out,
                                    ctx.tempVReg,
                                    ctx.tempRegClass,
                                    ctx.nextVRegId,
                                    loV,
                                    loCls))
            return false;
    }

    if (!materializeValueToVReg(ins.operands[2],
                                bb,
                                ctx.ti,
                                ctx.fb,
                                out,
                                ctx.tempVReg,
                                ctx.tempRegClass,
                                ctx.nextVRegId,
                                hiV,
                                hiCls))
        return false;

    const int widthBits = integerTypeBits(ins.type.kind);
    idxV = signExtendVRegToWidth(idxV, widthBits, ctx, out);
    if (!loIsZero)
        loV = signExtendVRegToWidth(loV, widthBits, ctx, out);
    hiV = signExtendVRegToWidth(hiV, widthBits, ctx, out);

    const std::string trapLabel = requestSharedTrapBlock(ctx, "bounds", nullptr, 7);

    if (loIsZero) {
        // Check idx < 0 or idx >= hi using signed comparisons.
        out.instrs.push_back(
            MInstr{MOpcode::CmpRI, {MOperand::vregOp(RegClass::GPR, idxV), MOperand::immOp(0)}});
        out.instrs.push_back(
            MInstr{MOpcode::BCond, {MOperand::condOp("lt"), MOperand::labelOp(trapLabel)}});

        out.instrs.push_back(
            MInstr{MOpcode::CmpRR,
                   {MOperand::vregOp(RegClass::GPR, idxV), MOperand::vregOp(RegClass::GPR, hiV)}});
        out.instrs.push_back(
            MInstr{MOpcode::BCond, {MOperand::condOp("ge"), MOperand::labelOp(trapLabel)}});
    } else {
        // General case: check idx < lo OR idx >= hi
        // cmp idx, lo; b.lt trap
        out.instrs.push_back(
            MInstr{MOpcode::CmpRR,
                   {MOperand::vregOp(RegClass::GPR, idxV), MOperand::vregOp(RegClass::GPR, loV)}});
        out.instrs.push_back(
            MInstr{MOpcode::BCond, {MOperand::condOp("lt"), MOperand::labelOp(trapLabel)}});

        // cmp idx, hi; b.ge trap
        out.instrs.push_back(
            MInstr{MOpcode::CmpRR,
                   {MOperand::vregOp(RegClass::GPR, idxV), MOperand::vregOp(RegClass::GPR, hiV)}});
        out.instrs.push_back(
            MInstr{MOpcode::BCond, {MOperand::condOp("ge"), MOperand::labelOp(trapLabel)}});
    }

    // Result is the normalized index (idx - lo), matching VM semantics.
    const uint16_t dst = allocateNextVReg(ctx.nextVRegId);
    ctx.tempVReg[*ins.result] = dst;
    ctx.tempRegClass[*ins.result] = RegClass::GPR;
    if (loIsZero) {
        out.instrs.push_back(
            MInstr{MOpcode::MovRR,
                   {MOperand::vregOp(RegClass::GPR, dst), MOperand::vregOp(RegClass::GPR, idxV)}});
    } else {
        out.instrs.push_back(MInstr{MOpcode::SubRRR,
                                    {MOperand::vregOp(RegClass::GPR, dst),
                                     MOperand::vregOp(RegClass::GPR, idxV),
                                     MOperand::vregOp(RegClass::GPR, loV)}});
    }

    return true;
}

//===----------------------------------------------------------------------===//
// Ternary select
//===----------------------------------------------------------------------===//

bool lowerSelect(const il::core::Instr &ins,
                 const il::core::BasicBlock &bb,
                 LoweringContext &ctx,
                 MBasicBlock &out) {
    // select type, %cond, %tval, %fval — both arms are plain values.
    if (!ins.result || ins.operands.size() < 3)
        return false;

    uint16_t condV = 0, tV = 0, fV = 0;
    RegClass condCls = RegClass::GPR, tCls = RegClass::GPR, fCls = RegClass::GPR;
    if (!materializeValueToVReg(ins.operands[0],
                                bb,
                                ctx.ti,
                                ctx.fb,
                                out,
                                ctx.tempVReg,
                                ctx.tempRegClass,
                                ctx.nextVRegId,
                                condV,
                                condCls))
        return false;
    if (!materializeValueToVReg(ins.operands[1],
                                bb,
                                ctx.ti,
                                ctx.fb,
                                out,
                                ctx.tempVReg,
                                ctx.tempRegClass,
                                ctx.nextVRegId,
                                tV,
                                tCls))
        return false;
    if (!materializeValueToVReg(ins.operands[2],
                                bb,
                                ctx.ti,
                                ctx.fb,
                                out,
                                ctx.tempVReg,
                                ctx.tempRegClass,
                                ctx.nextVRegId,
                                fV,
                                fCls))
        return false;

    out.instrs.push_back(
        MInstr{MOpcode::CmpRI, {MOperand::vregOp(RegClass::GPR, condV), MOperand::immOp(0)}});

    const bool isFloat = ins.type.kind == il::core::Type::Kind::F64;
    const RegClass cls = isFloat ? RegClass::FPR : RegClass::GPR;
    const uint16_t dst = allocateNextVReg(ctx.nextVRegId);
    ctx.tempVReg[*ins.result] = dst;
    ctx.tempRegClass[*ins.result] = cls;
    out.instrs.push_back(MInstr{isFloat ? MOpcode::FCsel : MOpcode::Csel,
                                {MOperand::vregOp(cls, dst),
                                 MOperand::vregOp(cls, tV),
                                 MOperand::vregOp(cls, fV),
                                 MOperand::condOp("ne")}});
    return true;
}

//===----------------------------------------------------------------------===//
// Signed Remainder (srem) - no zero-check
//===----------------------------------------------------------------------===//

bool lowerSRem(const il::core::Instr &ins,
               const il::core::BasicBlock &bb,
               LoweringContext &ctx,
               MBasicBlock &out) {
    if (!ins.result || ins.operands.size() < 2)
        return false;

    uint16_t lhs = 0, rhs = 0;
    RegClass lhsCls = RegClass::GPR, rhsCls = RegClass::GPR;

    if (!materializeValueToVReg(ins.operands[0],
                                bb,
                                ctx.ti,
                                ctx.fb,
                                out,
                                ctx.tempVReg,
                                ctx.tempRegClass,
                                ctx.nextVRegId,
                                lhs,
                                lhsCls))
        return false;
    if (!materializeValueToVReg(ins.operands[1],
                                bb,
                                ctx.ti,
                                ctx.fb,
                                out,
                                ctx.tempVReg,
                                ctx.tempRegClass,
                                ctx.nextVRegId,
                                rhs,
                                rhsCls))
        return false;

    // Compute quotient: sdiv tmp, lhs, rhs
    const uint16_t quotient = allocateNextVReg(ctx.nextVRegId);
    out.instrs.push_back(MInstr{MOpcode::SDivRRR,
                                {MOperand::vregOp(RegClass::GPR, quotient),
                                 MOperand::vregOp(RegClass::GPR, lhs),
                                 MOperand::vregOp(RegClass::GPR, rhs)}});

    // Compute remainder: msub dst, quotient, rhs, lhs => dst = lhs - quotient*rhs
    const uint16_t dst = allocateNextVReg(ctx.nextVRegId);
    ctx.tempVReg[*ins.result] = dst;
    out.instrs.push_back(MInstr{MOpcode::MSubRRRR,
                                {MOperand::vregOp(RegClass::GPR, dst),
                                 MOperand::vregOp(RegClass::GPR, quotient),
                                 MOperand::vregOp(RegClass::GPR, rhs),
                                 MOperand::vregOp(RegClass::GPR, lhs)}});

    return true;
}

//===----------------------------------------------------------------------===//
// Unsigned Remainder (urem) - no zero-check
//===----------------------------------------------------------------------===//

bool lowerURem(const il::core::Instr &ins,
               const il::core::BasicBlock &bb,
               LoweringContext &ctx,
               MBasicBlock &out) {
    if (!ins.result || ins.operands.size() < 2)
        return false;

    uint16_t lhs = 0, rhs = 0;
    RegClass lhsCls = RegClass::GPR, rhsCls = RegClass::GPR;

    if (!materializeValueToVReg(ins.operands[0],
                                bb,
                                ctx.ti,
                                ctx.fb,
                                out,
                                ctx.tempVReg,
                                ctx.tempRegClass,
                                ctx.nextVRegId,
                                lhs,
                                lhsCls))
        return false;
    if (!materializeValueToVReg(ins.operands[1],
                                bb,
                                ctx.ti,
                                ctx.fb,
                                out,
                                ctx.tempVReg,
                                ctx.tempRegClass,
                                ctx.nextVRegId,
                                rhs,
                                rhsCls))
        return false;

    // Compute quotient: udiv tmp, lhs, rhs
    const uint16_t quotient = allocateNextVReg(ctx.nextVRegId);
    out.instrs.push_back(MInstr{MOpcode::UDivRRR,
                                {MOperand::vregOp(RegClass::GPR, quotient),
                                 MOperand::vregOp(RegClass::GPR, lhs),
                                 MOperand::vregOp(RegClass::GPR, rhs)}});

    // Compute remainder: msub dst, quotient, rhs, lhs => dst = lhs - quotient*rhs
    const uint16_t dst = allocateNextVReg(ctx.nextVRegId);
    ctx.tempVReg[*ins.result] = dst;
    out.instrs.push_back(MInstr{MOpcode::MSubRRRR,
                                {MOperand::vregOp(RegClass::GPR, dst),
                                 MOperand::vregOp(RegClass::GPR, quotient),
                                 MOperand::vregOp(RegClass::GPR, rhs),
                                 MOperand::vregOp(RegClass::GPR, lhs)}});

    return true;
}

//===----------------------------------------------------------------------===//
// FP Arithmetic (fadd, fsub, fmul, fdiv)
//===----------------------------------------------------------------------===//

bool lowerFpArithmetic(const il::core::Instr &ins,
                       const il::core::BasicBlock &bb,
                       LoweringContext &ctx,
                       MBasicBlock &out) {
    if (!ins.result || ins.operands.size() < 2)
        return false;

    uint16_t lhs = 0, rhs = 0;
    RegClass lhsCls = RegClass::FPR, rhsCls = RegClass::FPR;

    if (!materializeValueToVReg(ins.operands[0],
                                bb,
                                ctx.ti,
                                ctx.fb,
                                out,
                                ctx.tempVReg,
                                ctx.tempRegClass,
                                ctx.nextVRegId,
                                lhs,
                                lhsCls))
        return false;
    if (!materializeValueToVReg(ins.operands[1],
                                bb,
                                ctx.ti,
                                ctx.fb,
                                out,
                                ctx.tempVReg,
                                ctx.tempRegClass,
                                ctx.nextVRegId,
                                rhs,
                                rhsCls))
        return false;

    lhs = coerceScalarOperandToFpr(lhs, lhsCls, ctx.nextVRegId, out);
    rhs = coerceScalarOperandToFpr(rhs, rhsCls, ctx.nextVRegId, out);

    const uint16_t dst = allocateNextVReg(ctx.nextVRegId);
    ctx.tempVReg[*ins.result] = dst;
    ctx.tempRegClass[*ins.result] = RegClass::FPR;

    MOpcode mop = MOpcode::FAddRRR;
    switch (ins.op) {
        case il::core::Opcode::FAdd:
            mop = MOpcode::FAddRRR;
            break;
        case il::core::Opcode::FSub:
            mop = MOpcode::FSubRRR;
            break;
        case il::core::Opcode::FMul:
            mop = MOpcode::FMulRRR;
            break;
        case il::core::Opcode::FDiv:
            mop = MOpcode::FDivRRR;
            break;
        default:
            return false;
    }

    out.instrs.push_back(MInstr{mop,
                                {MOperand::vregOp(RegClass::FPR, dst),
                                 MOperand::vregOp(RegClass::FPR, lhs),
                                 MOperand::vregOp(RegClass::FPR, rhs)}});
    return true;
}

//===----------------------------------------------------------------------===//
// FP Comparisons
//===----------------------------------------------------------------------===//

bool lowerFpCompare(const il::core::Instr &ins,
                    const il::core::BasicBlock &bb,
                    LoweringContext &ctx,
                    MBasicBlock &out) {
    if (!ins.result || ins.operands.size() < 2)
        return false;

    uint16_t lhs = 0, rhs = 0;
    RegClass lhsCls = RegClass::FPR, rhsCls = RegClass::FPR;

    if (!materializeValueToVReg(ins.operands[0],
                                bb,
                                ctx.ti,
                                ctx.fb,
                                out,
                                ctx.tempVReg,
                                ctx.tempRegClass,
                                ctx.nextVRegId,
                                lhs,
                                lhsCls))
        return false;
    if (!materializeValueToVReg(ins.operands[1],
                                bb,
                                ctx.ti,
                                ctx.fb,
                                out,
                                ctx.tempVReg,
                                ctx.tempRegClass,
                                ctx.nextVRegId,
                                rhs,
                                rhsCls))
        return false;

    lhs = coerceScalarOperandToFpr(lhs, lhsCls, ctx.nextVRegId, out);
    rhs = coerceScalarOperandToFpr(rhs, rhsCls, ctx.nextVRegId, out);

    // Emit fcmp
    out.instrs.push_back(
        MInstr{MOpcode::FCmpRR,
               {MOperand::vregOp(RegClass::FPR, lhs), MOperand::vregOp(RegClass::FPR, rhs)}});

    // Emit cset/logic with appropriate NaN handling for ordered predicates.
    const uint16_t dst = allocateNextVReg(ctx.nextVRegId);
    ctx.tempVReg[*ins.result] = dst;
    ctx.tempRegClass[*ins.result] = RegClass::GPR;
    emitFpCompareResult(out, ins.op, dst, ctx.nextVRegId);

    return true;
}

//===----------------------------------------------------------------------===//
// sitofp (signed int to float)
//===----------------------------------------------------------------------===//

bool lowerSitofp(const il::core::Instr &ins,
                 const il::core::BasicBlock &bb,
                 LoweringContext &ctx,
                 MBasicBlock &out) {
    if (!ins.result || ins.operands.empty())
        return false;

    uint16_t sv = 0;
    RegClass scls = RegClass::GPR;
    if (!materializeValueToVReg(ins.operands[0],
                                bb,
                                ctx.ti,
                                ctx.fb,
                                out,
                                ctx.tempVReg,
                                ctx.tempRegClass,
                                ctx.nextVRegId,
                                sv,
                                scls))
        return false;

    const uint16_t dst = allocateNextVReg(ctx.nextVRegId);
    ctx.tempVReg[*ins.result] = dst;
    ctx.tempRegClass[*ins.result] = RegClass::FPR;

    out.instrs.push_back(
        MInstr{MOpcode::SCvtF,
               {MOperand::vregOp(RegClass::FPR, dst), MOperand::vregOp(RegClass::GPR, sv)}});
    return true;
}

//===----------------------------------------------------------------------===//
// fptosi (float to signed int)
//===----------------------------------------------------------------------===//

bool lowerFptosi(const il::core::Instr &ins,
                 const il::core::BasicBlock &bb,
                 LoweringContext &ctx,
                 MBasicBlock &out) {
    if (!ins.result || ins.operands.empty())
        return false;

    uint16_t fv = 0;
    RegClass fcls = RegClass::FPR;
    if (!materializeValueToVReg(ins.operands[0],
                                bb,
                                ctx.ti,
                                ctx.fb,
                                out,
                                ctx.tempVReg,
                                ctx.tempRegClass,
                                ctx.nextVRegId,
                                fv,
                                fcls))
        return false;
    if (fcls != RegClass::FPR)
        return false;

    const uint16_t dst = allocateNextVReg(ctx.nextVRegId);
    ctx.tempVReg[*ins.result] = dst;
    ctx.tempRegClass[*ins.result] = RegClass::GPR;

    const std::string invalidLabel = requestSharedTrapBlock(ctx, "fp_invalid", nullptr, 5);
    const std::string overflowLabel = requestSharedTrapBlock(ctx, "fp_ovf", nullptr, 4);

    out.instrs.push_back(
        MInstr{MOpcode::FCmpRR,
               {MOperand::vregOp(RegClass::FPR, fv), MOperand::vregOp(RegClass::FPR, fv)}});
    out.instrs.push_back(
        MInstr{MOpcode::BCond, {MOperand::condOp("vs"), MOperand::labelOp(invalidLabel)}});

    const int resultBits = integerTypeBits(ins.type.kind);
    const uint16_t lowerBound = allocateNextVReg(ctx.nextVRegId);
    emitF64BitsToVReg(out,
                      lowerBound,
                      zanna::codegen::common::f64Bits(signedLowerBoundForBits(resultBits)),
                      ctx.nextVRegId);
    const uint16_t upperBound = allocateNextVReg(ctx.nextVRegId);
    emitF64BitsToVReg(out,
                      upperBound,
                      zanna::codegen::common::f64Bits(signedUpperExclusiveForBits(resultBits)),
                      ctx.nextVRegId);

    out.instrs.push_back(
        MInstr{MOpcode::FCmpRR,
               {MOperand::vregOp(RegClass::FPR, fv), MOperand::vregOp(RegClass::FPR, lowerBound)}});
    out.instrs.push_back(
        MInstr{MOpcode::BCond, {MOperand::condOp("lt"), MOperand::labelOp(overflowLabel)}});
    out.instrs.push_back(
        MInstr{MOpcode::FCmpRR,
               {MOperand::vregOp(RegClass::FPR, fv), MOperand::vregOp(RegClass::FPR, upperBound)}});
    out.instrs.push_back(
        MInstr{MOpcode::BCond, {MOperand::condOp("ge"), MOperand::labelOp(overflowLabel)}});

    out.instrs.push_back(
        MInstr{MOpcode::FCvtZS,
               {MOperand::vregOp(RegClass::GPR, dst), MOperand::vregOp(RegClass::FPR, fv)}});

    return true;
}

//===----------------------------------------------------------------------===//
// Zext1/Trunc1 (Boolean conversion)
//===----------------------------------------------------------------------===//

[[maybe_unused]] bool lowerZext1Trunc1(const il::core::Instr &ins,
                                       const il::core::BasicBlock &bb,
                                       LoweringContext &ctx,
                                       MBasicBlock &out) {
    if (!ins.result || ins.operands.empty())
        return false;

    uint16_t sv = 0;
    RegClass scls = RegClass::GPR;
    if (!materializeValueToVReg(ins.operands[0],
                                bb,
                                ctx.ti,
                                ctx.fb,
                                out,
                                ctx.tempVReg,
                                ctx.tempRegClass,
                                ctx.nextVRegId,
                                sv,
                                scls))
        return false;

    const uint16_t dst = allocateNextVReg(ctx.nextVRegId);
    ctx.tempVReg[*ins.result] = dst;
    ctx.tempRegClass[*ins.result] = RegClass::GPR;

    // dst = sv & 1
    const uint16_t one = allocateNextVReg(ctx.nextVRegId);
    out.instrs.push_back(
        MInstr{MOpcode::MovRI, {MOperand::vregOp(RegClass::GPR, one), MOperand::immOp(1)}});
    out.instrs.push_back(MInstr{MOpcode::AndRRR,
                                {MOperand::vregOp(RegClass::GPR, dst),
                                 MOperand::vregOp(RegClass::GPR, sv),
                                 MOperand::vregOp(RegClass::GPR, one)}});
    return true;
}

//===----------------------------------------------------------------------===//
// Narrowing casts (CastSiNarrowChk, CastUiNarrowChk)
//===----------------------------------------------------------------------===//

[[maybe_unused]] bool lowerNarrowingCast(const il::core::Instr &ins,
                                         const il::core::BasicBlock &bb,
                                         LoweringContext &ctx,
                                         MBasicBlock &out) {
    if (!ins.result || ins.operands.empty())
        return false;

    int bits = 64;
    if (ins.type.kind == il::core::Type::Kind::I16)
        bits = 16;
    else if (ins.type.kind == il::core::Type::Kind::I32)
        bits = 32;
    const int sh = 64 - bits;

    uint16_t sv = 0;
    RegClass scls = RegClass::GPR;
    if (!materializeValueToVReg(ins.operands[0],
                                bb,
                                ctx.ti,
                                ctx.fb,
                                out,
                                ctx.tempVReg,
                                ctx.tempRegClass,
                                ctx.nextVRegId,
                                sv,
                                scls))
        return false;

    const uint16_t vt = allocateNextVReg(ctx.nextVRegId);
    ctx.tempVReg[*ins.result] = vt;

    // vt = narrowed version of sv = (sv << sh) >> sh (arithmetic for signed, logical for
    // unsigned). Read sv directly into the first shift rather than emitting a separate
    // `MovRR vt, sv`: that copy is a copy-forwarding hazard, because vt is the narrowed
    // result (e.g. a switch scrutinee) and pre-RA copy forwarding could re-point its later
    // uses back at sv even though the shifts redefine vt.
    if (sh > 0) {
        out.instrs.push_back(MInstr{MOpcode::LslRI,
                                    {MOperand::vregOp(RegClass::GPR, vt),
                                     MOperand::vregOp(RegClass::GPR, sv),
                                     MOperand::immOp(sh)}});
        if (ins.op == il::core::Opcode::CastSiNarrowChk) {
            out.instrs.push_back(MInstr{MOpcode::AsrRI,
                                        {MOperand::vregOp(RegClass::GPR, vt),
                                         MOperand::vregOp(RegClass::GPR, vt),
                                         MOperand::immOp(sh)}});
        } else {
            out.instrs.push_back(MInstr{MOpcode::LsrRI,
                                        {MOperand::vregOp(RegClass::GPR, vt),
                                         MOperand::vregOp(RegClass::GPR, vt),
                                         MOperand::immOp(sh)}});
        }
    } else {
        // No change in width - just copy
        out.instrs.push_back(
            MInstr{MOpcode::MovRR,
                   {MOperand::vregOp(RegClass::GPR, vt), MOperand::vregOp(RegClass::GPR, sv)}});
    }
    out.instrs.push_back(
        MInstr{MOpcode::CmpRR,
               {MOperand::vregOp(RegClass::GPR, vt), MOperand::vregOp(RegClass::GPR, sv)}});
    const std::string trapLabel = requestSharedTrapBlock(ctx, "ovf", "rt_trap_ovf");
    out.instrs.push_back(
        MInstr{MOpcode::BCond, {MOperand::condOp("ne"), MOperand::labelOp(trapLabel)}});
    ctx.tempRegClass[*ins.result] = RegClass::GPR;
    return true;
}

//===----------------------------------------------------------------------===//
// Memory Operations — Store
//===----------------------------------------------------------------------===//

bool lowerStore(const il::core::Instr &ins,
                const il::core::BasicBlock &bb,
                LoweringContext &ctx,
                MBasicBlock &out) {
    if (ins.operands.size() < 2 || ins.operands.size() > 3)
        return false;
    long long off = 0;
    if (ins.operands.size() == 3) {
        if (ins.operands[2].kind != il::core::Value::Kind::ConstInt)
            return false;
        off = ins.operands[2].i64;
    }
    const bool hasFrameAddr = resolveFrameAddress(ins.operands[0], ctx.fn, ctx.fb, off);
    if (hasFrameAddr) {
        // Store to alloca local via FP offset
        uint16_t v = 0;
        RegClass cls = RegClass::GPR;
        if (!materializeValueToVReg(ins.operands[1], bb, ctx, out, v, cls))
            return false;
        const bool dstIsFP = (ins.type.kind == il::core::Type::Kind::F64);
        if (dstIsFP) {
            uint16_t srcF = v;
            if (cls != RegClass::FPR) {
                srcF = allocateNextVReg(ctx.nextVRegId);
                out.instrs.push_back(MInstr{
                    MOpcode::SCvtF,
                    {MOperand::vregOp(RegClass::FPR, srcF), MOperand::vregOp(RegClass::GPR, v)}});
            }
            out.instrs.push_back(
                MInstr{MOpcode::StrFprFpImm,
                       {MOperand::vregOp(RegClass::FPR, srcF), MOperand::immOp(off)}});
        } else {
            const MOpcode storeOpc = gprStoreOpcodeForType(ins.type.kind, /*frameRelative=*/true);
            out.instrs.push_back(
                MInstr{storeOpc, {MOperand::vregOp(RegClass::GPR, v), MOperand::immOp(off)}});
        }
    } else {
        // General store via base-in-vreg
        uint16_t vbase = 0, vval = 0;
        RegClass cbase = RegClass::GPR, cval = RegClass::GPR;
        if (!materializeValueToVReg(ins.operands[0], bb, ctx, out, vbase, cbase) ||
            !materializeValueToVReg(ins.operands[1], bb, ctx, out, vval, cval)) {
            return false;
        }
        const bool dstIsFP = (ins.type.kind == il::core::Type::Kind::F64);
        if (dstIsFP) {
            uint16_t srcF = vval;
            if (cval != RegClass::FPR) {
                srcF = allocateNextVReg(ctx.nextVRegId);
                out.instrs.push_back(MInstr{MOpcode::SCvtF,
                                            {MOperand::vregOp(RegClass::FPR, srcF),
                                             MOperand::vregOp(RegClass::GPR, vval)}});
            }
            out.instrs.push_back(MInstr{MOpcode::StrFprBaseImm,
                                        {MOperand::vregOp(RegClass::FPR, srcF),
                                         MOperand::vregOp(RegClass::GPR, vbase),
                                         MOperand::immOp(off)}});
        } else {
            const MOpcode storeOpc = gprStoreOpcodeForType(ins.type.kind, /*frameRelative=*/false);
            out.instrs.push_back(MInstr{storeOpc,
                                        {MOperand::vregOp(RegClass::GPR, vval),
                                         MOperand::vregOp(RegClass::GPR, vbase),
                                         MOperand::immOp(off)}});
        }
    }
    return true;
}

//===----------------------------------------------------------------------===//
// Memory Operations — Load
//===----------------------------------------------------------------------===//

bool lowerLoad(const il::core::Instr &ins,
               const il::core::BasicBlock &bb,
               LoweringContext &ctx,
               MBasicBlock &out) {
    if (!ins.result || ins.operands.empty())
        return false;
    long long off = 0;
    if (ins.operands.size() > 2)
        return false;
    if (ins.operands.size() == 2) {
        if (ins.operands[1].kind != il::core::Value::Kind::ConstInt)
            return false;
        off = ins.operands[1].i64;
    }
    const bool hasFrameAddr = resolveFrameAddress(ins.operands[0], ctx.fn, ctx.fb, off);
    const bool isFP = (ins.type.kind == il::core::Type::Kind::F64);
    const bool isBool = (ins.type.kind == il::core::Type::Kind::I1);
    if (hasFrameAddr) {
        const uint16_t dst = allocateNextVReg(ctx.nextVRegId);
        if (isFP) {
            ctx.tempRegClass[*ins.result] = RegClass::FPR;
            out.instrs.push_back(
                MInstr{MOpcode::LdrFprFpImm,
                       {MOperand::vregOp(RegClass::FPR, dst), MOperand::immOp(off)}});
            ctx.tempVReg[*ins.result] = dst;
        } else {
            const MOpcode loadOpc = gprLoadOpcodeForType(ins.type.kind, /*frameRelative=*/true);
            out.instrs.push_back(
                MInstr{loadOpc, {MOperand::vregOp(RegClass::GPR, dst), MOperand::immOp(off)}});
            if (ins.type.kind == il::core::Type::Kind::Str &&
                !strLoadRetainElidable(ctx.fn, bb, indexOfInstr(bb, ins), *ins.result))
                retainStringVReg(out, dst);
            ctx.tempRegClass[*ins.result] = RegClass::GPR;
            ctx.tempVReg[*ins.result] = isBool ? emitMaskedI1Value(out, dst, ctx.nextVRegId) : dst;
        }
    } else {
        uint16_t vbase = 0;
        RegClass cbase = RegClass::GPR;
        if (!materializeValueToVReg(ins.operands[0], bb, ctx, out, vbase, cbase))
            return false;
        const uint16_t dst = allocateNextVReg(ctx.nextVRegId);
        if (isFP) {
            ctx.tempRegClass[*ins.result] = RegClass::FPR;
            out.instrs.push_back(MInstr{MOpcode::LdrFprBaseImm,
                                        {MOperand::vregOp(RegClass::FPR, dst),
                                         MOperand::vregOp(RegClass::GPR, vbase),
                                         MOperand::immOp(off)}});
            ctx.tempVReg[*ins.result] = dst;
        } else {
            const MOpcode loadOpc = gprLoadOpcodeForType(ins.type.kind, /*frameRelative=*/false);
            out.instrs.push_back(MInstr{loadOpc,
                                        {MOperand::vregOp(RegClass::GPR, dst),
                                         MOperand::vregOp(RegClass::GPR, vbase),
                                         MOperand::immOp(off)}});
            if (ins.type.kind == il::core::Type::Kind::Str &&
                !strLoadRetainElidable(ctx.fn, bb, indexOfInstr(bb, ins), *ins.result))
                retainStringVReg(out, dst);
            ctx.tempRegClass[*ins.result] = RegClass::GPR;
            ctx.tempVReg[*ins.result] = isBool ? emitMaskedI1Value(out, dst, ctx.nextVRegId) : dst;
        }
    }
    return true;
}

//===----------------------------------------------------------------------===//
// Memory Operations — GEP
//===----------------------------------------------------------------------===//

bool lowerGEP(const il::core::Instr &ins,
              const il::core::BasicBlock &bb,
              LoweringContext &ctx,
              MBasicBlock &out) {
    if (!ins.result || ins.operands.size() < 2)
        return false;
    uint16_t vbase = 0;
    RegClass cbase = RegClass::GPR;
    if (!materializeValueToVReg(ins.operands[0], bb, ctx, out, vbase, cbase))
        return false;
    const uint16_t dst = allocateNextVReg(ctx.nextVRegId);
    ctx.tempVReg[*ins.result] = dst;
    ctx.tempRegClass[*ins.result] = RegClass::GPR;
    const auto &offVal = ins.operands[1];
    if (offVal.kind == il::core::Value::Kind::ConstInt) {
        const long long imm = offVal.i64;
        if (imm == 0) {
            out.instrs.push_back(MInstr{
                MOpcode::MovRR,
                {MOperand::vregOp(RegClass::GPR, dst), MOperand::vregOp(RegClass::GPR, vbase)}});
        } else {
            emitLegalizedSignedImmArith(
                out,
                MOperand::vregOp(RegClass::GPR, dst),
                MOperand::vregOp(RegClass::GPR, vbase),
                imm,
                SignedImmArithKind::Add,
                MOpcode::AddRI,
                MOpcode::SubRI,
                MOpcode::AddRRR,
                MOpcode::SubRRR,
                [&](long long materializedImm) {
                    const uint16_t tmp = allocateNextVReg(ctx.nextVRegId);
                    out.instrs.push_back(MInstr{
                        MOpcode::MovRI,
                        {MOperand::vregOp(RegClass::GPR, tmp), MOperand::immOp(materializedImm)}});
                    return MOperand::vregOp(RegClass::GPR, tmp);
                });
        }
    } else {
        uint16_t voff = 0;
        RegClass coff = RegClass::GPR;
        if (!materializeValueToVReg(offVal, bb, ctx, out, voff, coff))
            return false;
        out.instrs.push_back(MInstr{MOpcode::AddRRR,
                                    {MOperand::vregOp(RegClass::GPR, dst),
                                     MOperand::vregOp(RegClass::GPR, vbase),
                                     MOperand::vregOp(RegClass::GPR, voff)}});
    }
    return true;
}

//===----------------------------------------------------------------------===//
// Call & Return — Call
//===----------------------------------------------------------------------===//

bool lowerCall(const il::core::Instr &ins,
               const il::core::BasicBlock &bb,
               LoweringContext &ctx,
               MBasicBlock &out) {
    LoweredCall seq{};
    if (lowerCallWithArgs(ins,
                          bb,
                          ctx.ti,
                          ctx.fb,
                          out,
                          seq,
                          ctx.tempVReg,
                          ctx.tempRegClass,
                          ctx.nextVRegId,
                          ctx.knownVarArgNamedArgCounts)) {
        for (auto &mi : seq.prefix)
            out.instrs.push_back(std::move(mi));
        out.instrs.push_back(std::move(seq.call));
        for (auto &mi : seq.postfix)
            out.instrs.push_back(std::move(mi));
        if (ins.result) {
            const uint16_t dst = captureCallResult(
                ins, ctx.fn, ctx.ti, out, ctx.tempVReg, ctx.tempRegClass, ctx.nextVRegId);
            if (isDirectCallee(ins, "rt_arr_obj_get")) {
                const int off = ctx.fb.ensureSpill(dst);
                out.instrs.push_back(
                    MInstr{MOpcode::StrRegFpImm,
                           {MOperand::vregOp(RegClass::GPR, dst), MOperand::immOp(off)}});
                const uint16_t dst2 = allocateNextVReg(ctx.nextVRegId);
                out.instrs.push_back(
                    MInstr{MOpcode::LdrRegFpImm,
                           {MOperand::vregOp(RegClass::GPR, dst2), MOperand::immOp(off)}});
                ctx.tempVReg[*ins.result] = dst2;
            }
        }
    } else {
        std::string callee = "<malformed direct call>";
        if (!ins.callee.empty()) {
            callee = ins.callee;
        } else if (!ins.operands.empty() &&
                   ins.operands[0].kind == il::core::Value::Kind::GlobalAddr) {
            callee = ins.operands[0].str;
        }
        throw std::runtime_error("AArch64 lowering: failed to lower call arguments for " + callee);
    }
    return true;
}

//===----------------------------------------------------------------------===//
// Call & Return — CallIndirect
//===----------------------------------------------------------------------===//

bool lowerCallIndirect(const il::core::Instr &ins,
                       const il::core::BasicBlock &bb,
                       LoweringContext &ctx,
                       MBasicBlock &out) {
    if (ins.operands.empty())
        return false;

    uint16_t vFuncPtr = 0;
    RegClass cFuncPtr = RegClass::GPR;
    if (!materializeValueToVReg(ins.operands[0], bb, ctx, out, vFuncPtr, cFuncPtr))
        return false;

    std::vector<MaterializedCallArg> args;
    args.reserve(ins.operands.size() - 1);
    for (std::size_t i = 1; i < ins.operands.size(); ++i) {
        uint16_t vArg = 0;
        RegClass cArg = RegClass::GPR;
        if (!materializeValueToVReg(ins.operands[i], bb, ctx, out, vArg, cArg))
            return false;
        args.push_back({vArg,
                        cArg == RegClass::FPR ? zanna::codegen::common::CallArgClass::FPR
                                              : zanna::codegen::common::CallArgClass::GPR});
    }

    LoweredCall seq{};
    if (!marshalCallArgs(args, args.size(), false, ctx.ti, ctx.nextVRegId, seq))
        return false;
    seq.prefix.push_back(MInstr{
        MOpcode::MovRR, {MOperand::regOp(PhysReg::X9), MOperand::vregOp(RegClass::GPR, vFuncPtr)}});
    seq.call = MInstr{MOpcode::Blr, {MOperand::regOp(PhysReg::X9)}};
    for (auto &mi : seq.prefix)
        out.instrs.push_back(std::move(mi));
    out.instrs.push_back(std::move(seq.call));
    for (auto &mi : seq.postfix)
        out.instrs.push_back(std::move(mi));

    if (ins.result)
        captureCallResult(ins, ctx.fn, ctx.ti, out, ctx.tempVReg, ctx.tempRegClass, ctx.nextVRegId);
    return true;
}

//===----------------------------------------------------------------------===//
// Call & Return — Ret
//===----------------------------------------------------------------------===//

bool lowerRet(const il::core::Instr &ins,
              const il::core::BasicBlock &bb,
              LoweringContext &ctx,
              MBasicBlock &out) {
    using Opcode = il::core::Opcode;

    if (!ins.operands.empty()) {
        uint16_t v = 0;
        RegClass cls = RegClass::GPR;
        bool ok = materializeValueToVReg(ins.operands[0], bb, ctx, out, v, cls);
        if (!ok && ins.operands[0].kind == il::core::Value::Kind::Temp) {
            const unsigned rid = ins.operands[0].id;
            auto it = std::find_if(
                bb.instructions.begin(), bb.instructions.end(), [&](const il::core::Instr &I) {
                    return I.result && *I.result == rid;
                });
            if (it != bb.instructions.end()) {
                const auto &prod = *it;
                if (!prod.operands.empty() &&
                    prod.operands[0].kind == il::core::Value::Kind::GlobalAddr) {
                    const std::string &sym = prod.operands[0].str;
                    if (prod.op == Opcode::ConstStr) {
                        emitConstStrGlobalToX0(
                            sym, ctx.stringLiteralByteLengths, out, ctx.nextVRegId);
                        v = allocateNextVReg(ctx.nextVRegId);
                        cls = RegClass::GPR;
                        out.instrs.push_back(MInstr{
                            MOpcode::MovRR,
                            {MOperand::vregOp(RegClass::GPR, v), MOperand::regOp(PhysReg::X0)}});
                        ctx.tempVReg[rid] = v;
                        ok = true;
                    } else if (prod.op == Opcode::AddrOf) {
                        v = allocateNextVReg(ctx.nextVRegId);
                        cls = RegClass::GPR;
                        out.instrs.push_back(
                            MInstr{MOpcode::AdrPage,
                                   {MOperand::vregOp(RegClass::GPR, v), MOperand::labelOp(sym)}});
                        out.instrs.push_back(MInstr{MOpcode::AddPageOff,
                                                    {MOperand::vregOp(RegClass::GPR, v),
                                                     MOperand::vregOp(RegClass::GPR, v),
                                                     MOperand::labelOp(sym)}});
                        ctx.tempVReg[rid] = v;
                        ok = true;
                    }
                }
            }
        }
        if (ok) {
            if (cls == RegClass::FPR) {
                out.instrs.push_back(MInstr{
                    MOpcode::FMovRR,
                    {MOperand::regOp(ctx.ti.f64ReturnReg), MOperand::vregOp(RegClass::FPR, v)}});
            } else {
                uint16_t retVReg = v;
                if (ctx.fn.retType.kind == il::core::Type::Kind::I1)
                    retVReg = emitMaskedI1Value(out, v, ctx.nextVRegId);
                out.instrs.push_back(MInstr{
                    MOpcode::MovRR,
                    {MOperand::regOp(PhysReg::X0), MOperand::vregOp(RegClass::GPR, retVReg)}});
            }
        }
    }
    if (ins.operands.empty() && ctx.mf.name == "main") {
        out.instrs.push_back(
            MInstr{MOpcode::MovRI, {MOperand::regOp(PhysReg::X0), MOperand::immOp(0)}});
    }
    out.instrs.push_back(MInstr{MOpcode::Ret, {}});
    return true;
}

} // namespace zanna::codegen::aarch64
