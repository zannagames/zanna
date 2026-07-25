//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: codegen/aarch64/LowerILToMIR.cpp
// Purpose: IL→MIR lowering orchestrator for AArch64.
//          Coordinates the full IL-to-MIR conversion: fast-path probe, frame
//          setup, phi parameter slot allocation, cross-block liveness analysis,
//          per-block instruction dispatch, and terminator lowering.
// Key invariants:
//   - Fast paths are tried first; generic lowering is used on miss.
//   - Cross-block temps are spilled at definition and reloaded at use.
//   - Phi slots are allocated as stack spills before instruction lowering.
// Ownership/Lifetime:
//   - All state is local to lowerFunction(); the LowerILToMIR object is stateless.
// Links: src/codegen/aarch64/LowerILToMIR.hpp,
//        src/codegen/aarch64/InstrLowering.hpp,
//        src/codegen/aarch64/OpcodeDispatch.hpp,
//        src/codegen/aarch64/TerminatorLowering.hpp,
//        src/codegen/aarch64/LivenessAnalysis.hpp,
//        src/codegen/aarch64/FastPaths.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements whole-function IL-to-AArch64-MIR orchestration.
 *
 * Lowering first probes transactional fast paths. The generic path allocates
 * locals and phi slots, analyzes cross-block values, seeds register classes,
 * materializes entry parameters, dispatches ordinary instructions, lowers
 * terminators, and finalizes the frame without retaining per-function state.
 */

#include "LowerILToMIR.hpp"

#include "FastPaths.hpp"
#include "FpCompareLowering.hpp"
#include "FrameBuilder.hpp"
#include "InstrLowering.hpp"
#include "LivenessAnalysis.hpp"
#include "LoweringContext.hpp"
#include "OpcodeDispatch.hpp"
#include "OpcodeMappings.hpp"
#include "TargetAArch64.hpp"
#include "TerminatorLowering.hpp"
#include "codegen/common/CallArgLayout.hpp"
#include "codegen/common/ICE.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Opcode.hpp"
#include "il/core/Type.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace zanna::codegen::aarch64 {
namespace {
using il::core::Opcode;

/// @brief Return the AArch64 condition-code string for an IL comparison opcode.
/// @param op IL integer or floating-point comparison opcode.
/// @return Pointer to a static target condition spelling.
static const char *condForOpcode(Opcode op) {
    return lookupAnyCondition(op);
}

/// @brief Return the bit-width for an IL integer type (I1→1, I16→16, I32→32, else 64).
/// @param kind IL type kind.
/// @return Integer width used by overflow and sign-extension lowering.
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

/// @brief Return true when an IL instruction needs the sub-64-bit overflow path.
/// @details Full-width checked overflow opcodes use dedicated MIR lowering. The
///          compare-after-narrowing expansion below is only needed for signed
///          integer widths below 64 bits.
/// @param ins Candidate checked arithmetic instruction.
/// @return `true` for sub-64-bit signed add/sub/multiply overflow operations.
static bool isSubWidthCheckedOverflowOp(const il::core::Instr &ins) {
    return integerTypeBits(ins.type.kind) < 64 &&
           (ins.op == Opcode::IAddOvf || ins.op == Opcode::ISubOvf || ins.op == Opcode::IMulOvf);
}

/// @brief Emit LSL/ASR to sign-extend @p src from @p bits to 64 bits; return the result vreg.
/// @details No-op (returns @p src) when @p bits >= 64.
/// @param out Machine block receiving the two shifts.
/// @param src Source GPR virtual register.
/// @param bits Original signed width.
/// @param nextVRegId Monotonic allocator advanced when extension is required.
/// @return Extended result vreg, or @p src for 64-bit values.
static uint16_t signExtendVRegToWidth(MBasicBlock &out,
                                      uint16_t src,
                                      int bits,
                                      uint16_t &nextVRegId) {
    if (bits >= 64)
        return src;
    const int shift = 64 - bits;
    const uint16_t dst = allocateNextVReg(nextVRegId);
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

/// @brief Emit a sub-64-bit checked overflow binary operation (IAddOvf/ISubOvf/IMulOvf).
/// @details Sign-extends both operands to the target width, performs the op, then sign-extends
///          the result and compares it to the full-width result. If they differ, control branches
///          to the shared overflow trap block identified by @p trapLabel. The function never
///          appends to @c MFunction::blocks, so callers can safely pass references into the
///          current block without risking vector reallocation invalidation.
/// @param out Machine block receiving arithmetic, narrowing, compare, and branch MIR.
/// @param ins Checked IL arithmetic instruction.
/// @param dst Destination GPR virtual register.
/// @param lhs Left operand GPR virtual register.
/// @param rhs Right operand GPR virtual register.
/// @param trapLabel Shared overflow trap block label.
/// @param nextVRegId Monotonic allocator for sign-extension temporaries.
/// @return true if the instruction was handled as a sub-width checked op; false if bits >= 64
///         or the opcode is not a checked overflow op (caller should use the generic path).
static bool emitSubWidthCheckedBinary(MBasicBlock &out,
                                      const il::core::Instr &ins,
                                      uint16_t dst,
                                      uint16_t lhs,
                                      uint16_t rhs,
                                      const std::string &trapLabel,
                                      uint16_t &nextVRegId) {
    const int bits = integerTypeBits(ins.type.kind);
    if (bits >= 64)
        return false;

    MOpcode op = MOpcode::AddRRR;
    switch (ins.op) {
        case Opcode::IAddOvf:
            op = MOpcode::AddRRR;
            break;
        case Opcode::ISubOvf:
            op = MOpcode::SubRRR;
            break;
        case Opcode::IMulOvf:
            op = MOpcode::MulRRR;
            break;
        default:
            return false;
    }

    lhs = signExtendVRegToWidth(out, lhs, bits, nextVRegId);
    rhs = signExtendVRegToWidth(out, rhs, bits, nextVRegId);
    out.instrs.push_back(MInstr{op,
                                {MOperand::vregOp(RegClass::GPR, dst),
                                 MOperand::vregOp(RegClass::GPR, lhs),
                                 MOperand::vregOp(RegClass::GPR, rhs)}});

    const uint16_t narrowed = signExtendVRegToWidth(out, dst, bits, nextVRegId);
    out.instrs.push_back(
        MInstr{MOpcode::CmpRR,
               {MOperand::vregOp(RegClass::GPR, narrowed), MOperand::vregOp(RegClass::GPR, dst)}});
    out.instrs.push_back(
        MInstr{MOpcode::BCond, {MOperand::condOp("ne"), MOperand::labelOp(trapLabel)}});
    return true;
}

/// @brief Build sparse use counts for all referenced IL temps in a function.
/// @details Scans operands and branch-argument lists and stores only IDs that are
///          actually used. This avoids allocating a dense vector up to the largest
///          temp ID, which can be prohibitively large for sparse or malformed IL.
/// @param fn Function whose temp operands should be counted.
/// @return Map from IL temp ID to total use count.
static std::unordered_map<unsigned, std::size_t> countTempUses(const il::core::Function &fn) {
    using il::core::Value;

    std::unordered_map<unsigned, std::size_t> uses;
    auto touch = [&](unsigned id) { ++uses[id]; };

    for (const auto &block : fn.blocks) {
        for (const auto &instr : block.instructions) {
            for (const auto &operand : instr.operands) {
                if (operand.kind == Value::Kind::Temp)
                    touch(operand.id);
            }
            for (const auto &argList : instr.brArgs) {
                for (const auto &arg : argList) {
                    if (arg.kind == Value::Kind::Temp)
                        touch(arg.id);
                }
            }
        }
    }

    return uses;
}

/// @brief Return the number of recorded uses for an IL temp ID.
/// @param uses Sparse use-count map produced by @ref countTempUses.
/// @param tempId IL temp ID to query.
/// @return Number of times the temp is used, or zero when absent.
static std::size_t tempUseCount(const std::unordered_map<unsigned, std::size_t> &uses,
                                unsigned tempId) {
    const auto it = uses.find(tempId);
    return it == uses.end() ? 0U : it->second;
}

/// @brief Compute a caller stack-argument address relative to AArch64 frame pointer.
/// @details After the standard prologue saves FP/LR and sets @c x29 to the new
///          frame, stack-passed arguments start at @c [x29 + 16]. The shared
///          call-layout planner supplies @p stackSlotIndex, so this helper only
///          performs the target-specific frame-pointer conversion and overflow
///          checks.
/// @param stackSlotIndex Zero-based stack argument slot from call layout planning.
/// @return Positive FP-relative byte offset to the caller-provided argument.
static int callerStackParamOffset(std::size_t stackSlotIndex) {
    constexpr std::size_t kSavedFpLrBytes = 16;
    constexpr std::size_t kSlotBytes = 8;
    if (stackSlotIndex >
        (static_cast<std::size_t>(std::numeric_limits<int>::max()) - kSavedFpLrBytes) /
            kSlotBytes) {
        throw std::overflow_error("AArch64 codegen: stack parameter offset out of range");
    }
    return static_cast<int>(kSavedFpLrBytes + stackSlotIndex * kSlotBytes);
}

/// @brief Build a function-wide temp register-class map from IL types.
/// @details Cross-block reloads can see a temp before its defining block has
///          been lowered when IL block order differs from CFG dominance order.
///          Seeding classes up front keeps f64 temps from defaulting to GPR.
/// @param fn Function whose parameters and instruction results are classified.
/// @return Sparse temp-id to GPR/FPR map.
static std::unordered_map<unsigned, RegClass> buildTempRegClassMap(const il::core::Function &fn) {
    auto classForType = [](const il::core::Type &type) {
        return type.kind == il::core::Type::Kind::F64 ? RegClass::FPR : RegClass::GPR;
    };

    std::unordered_map<unsigned, RegClass> classes;
    for (const auto &param : fn.params)
        classes[param.id] = classForType(param.type);
    for (const auto &block : fn.blocks) {
        for (const auto &param : block.params)
            classes[param.id] = classForType(param.type);
        for (const auto &instr : block.instructions) {
            if (instr.result)
                classes[*instr.result] = classForType(instr.type);
        }
    }
    return classes;
}

/// @brief Return true if @p bb's terminator is a CBr that consumes @p tempId as its condition.
/// @details Used to skip materializing the condition into an extra vreg when the CBr
///          can consume the flag result directly from the preceding comparison.
/// @param bb Basic block whose terminator is inspected.
/// @param tempId Comparison-result temporary.
/// @return `true` when the final instruction condition directly names @p tempId.
static bool cbrConsumesTemp(const il::core::BasicBlock &bb, unsigned tempId) {
    using il::core::Opcode;
    using il::core::Value;

    if (bb.instructions.empty())
        return false;
    const auto &term = bb.instructions.back();
    return term.op == Opcode::CBr && !term.operands.empty() &&
           term.operands[0].kind == Value::Kind::Temp && term.operands[0].id == tempId;
}

/// @brief Materialize entry-block parameters (function arguments) into virtual registers.
/// @details Register arguments are copied from their ABI registers, and stack
///          arguments are loaded from the caller frame. Parameters that liveness
///          marks as cross-block values are also stored once into their shared
///          cross-block spill slot; parameters used only within the entry block
///          avoid the previous store/reload round trip entirely.
///
///          Uses `planParamClasses` (shared with x86_64) so register-vs-stack
///          assignment matches the platform ABI exactly.
/// @param fn Enclosing IL function and variadic metadata.
/// @param bbIn Entry block whose parameters mirror function arguments.
/// @param ti Target ABI register orders.
/// @param out Entry machine block receiving moves, loads, and spill stores.
/// @param tempVReg Parameter-temp to canonical-vreg map.
/// @param tempRegClass Parameter-temp register-class map.
/// @param funcParamSpillOffset Cross-block parameter spill map updated in place.
/// @param crossBlockSpillOffset Preallocated liveness spill offsets.
/// @param nextVRegId Monotonic virtual-register allocator.
static void spillEntryBlockParams(const il::core::Function &fn,
                                  const il::core::BasicBlock &bbIn,
                                  const TargetInfo &ti,
                                  MBasicBlock &out,
                                  std::unordered_map<unsigned, uint16_t> &tempVReg,
                                  std::unordered_map<unsigned, RegClass> &tempRegClass,
                                  std::unordered_map<unsigned, int> &funcParamSpillOffset,
                                  const std::unordered_map<unsigned, int> &crossBlockSpillOffset,
                                  uint16_t &nextVRegId) {
    std::vector<zanna::codegen::common::CallArgClass> paramClasses;
    paramClasses.reserve(bbIn.params.size());
    for (const auto &param : bbIn.params) {
        paramClasses.push_back(param.type.kind == il::core::Type::Kind::F64
                                   ? zanna::codegen::common::CallArgClass::FPR
                                   : zanna::codegen::common::CallArgClass::GPR);
    }
    const auto layout = zanna::codegen::common::planParamClasses(
        paramClasses,
        zanna::codegen::common::CallArgLayoutConfig{
            .maxGPRArgs = ti.intArgOrder.size(),
            .maxFPRArgs = ti.f64ArgOrder.size(),
            .slotModel = zanna::codegen::common::CallSlotModel::IndependentRegisterBanks,
            .variadicTailOnStack = fn.isVarArg && ti.usesStackVariadicTail(),
            .numNamedArgs = paramClasses.size()});

    for (std::size_t pi = 0; pi < bbIn.params.size(); ++pi) {
        const auto &param = bbIn.params[pi];
        const auto &loc = layout.locations[pi];
        const RegClass cls =
            (loc.cls == zanna::codegen::common::CallArgClass::FPR) ? RegClass::FPR : RegClass::GPR;

        const auto spillIt = crossBlockSpillOffset.find(param.id);
        const bool needsCrossBlockSpill = spillIt != crossBlockSpillOffset.end();
        if (needsCrossBlockSpill)
            funcParamSpillOffset[param.id] = spillIt->second;

        const uint16_t vid = allocateNextVReg(nextVRegId);
        tempVReg[param.id] = vid;
        tempRegClass[param.id] = cls;

        if (loc.inRegister) {
            const PhysReg src = (cls == RegClass::FPR) ? ti.f64ArgOrder[loc.regIndex]
                                                       : ti.intArgOrder[loc.regIndex];
            const MOpcode moveOpc = (cls == RegClass::FPR) ? MOpcode::FMovRR : MOpcode::MovRR;
            out.instrs.push_back(
                MInstr{moveOpc, {MOperand::vregOp(cls, vid), MOperand::regOp(src)}});
        } else {
            // Stack parameter: load directly into the canonical parameter vreg.
            const int callerArgOffset = callerStackParamOffset(loc.stackSlotIndex);
            const MOpcode loadOpc =
                (cls == RegClass::FPR) ? MOpcode::LdrFprFpImm : MOpcode::LdrRegFpImm;
            out.instrs.push_back(
                MInstr{loadOpc, {MOperand::vregOp(cls, vid), MOperand::immOp(callerArgOffset)}});
        }

        if (needsCrossBlockSpill) {
            const MOpcode storeOpc =
                (cls == RegClass::FPR) ? MOpcode::StrFprFpImm : MOpcode::StrRegFpImm;
            out.instrs.push_back(
                MInstr{storeOpc, {MOperand::vregOp(cls, vid), MOperand::immOp(spillIt->second)}});
        }
    }
}

/// @brief Walk the IL function and register each Alloca as a frame local.
/// @details Populates @p fb with one local per Alloca and returns the set of
///          temp ids that are allocas (used by liveness to exclude them from
///          cross-block spilling).
/// @param fn Function whose alloca instructions are scanned.
/// @param fb Frame builder receiving local-slot allocations.
/// @return Set of alloca result temp ids.
/// @throws std::out_of_range if any alloca size is out-of-range (<=0 or > INT_MAX).
static std::unordered_set<unsigned> setupFrameLocals(const il::core::Function &fn,
                                                     FrameBuilder &fb) {
    std::unordered_set<unsigned> allocaTemps;
    for (const auto &bb : fn.blocks) {
        for (const auto &instr : bb.instructions) {
            if (instr.op != il::core::Opcode::Alloca)
                continue;
            if (!instr.result || instr.operands.empty() ||
                instr.operands[0].kind != il::core::Value::Kind::ConstInt) {
                throw std::runtime_error(
                    "AArch64 codegen: dynamic or malformed alloca is not supported");
            }
            const long long rawSize = instr.operands[0].i64;
            if (rawSize <= 0 || rawSize > std::numeric_limits<int>::max()) {
                throw std::out_of_range("AArch64 codegen: alloca size must be in range "
                                        "1..INT_MAX bytes");
            }
            const int size = static_cast<int>(rawSize);
            fb.addLocal(*instr.result, size, kSlotSizeBytes);
            allocaTemps.insert(*instr.result);
        }
    }
    return allocaTemps;
}

/// @brief Assign canonical phi vregs and a dedicated spill slot per block parameter.
/// @details Skips the entry block (its params come in via ABI registers); all other
///          blocks get a vreg per param plus a stack slot so edges can spill their
///          phi arguments before branching.
struct PhiAssignment {
    /// Block label to canonical phi virtual-register ids.
    std::unordered_map<std::string, std::vector<uint16_t>> vregId;
    /// Block label to phi register classes.
    std::unordered_map<std::string, std::vector<RegClass>> regClass;
    /// Block label to FP-relative phi spill offsets.
    std::unordered_map<std::string, std::vector<int>> spillOffset;
};

/// @brief Allocate canonical vregs and frame spills for non-entry block parameters.
/// @param fn Function whose block parameters are assigned.
/// @param fb Frame builder receiving phi spill slots.
/// @return Parallel maps keyed by block label.
static PhiAssignment allocatePhiSlots(const il::core::Function &fn, FrameBuilder &fb) {
    PhiAssignment out;
    uint16_t phiNextId = kPhiVRegStart; // reserve a distinct vreg range
    for (std::size_t bi = 1; bi < fn.blocks.size(); ++bi) {
        const auto &bb = fn.blocks[bi];
        if (bb.params.empty())
            continue;
        std::vector<uint16_t> ids;
        std::vector<RegClass> classes;
        std::vector<int> spillOffsets;
        ids.reserve(bb.params.size());
        classes.reserve(bb.params.size());
        spillOffsets.reserve(bb.params.size());
        for (const auto &param : bb.params) {
            const uint16_t id = allocatePhiVReg(phiNextId);
            ids.push_back(id);
            const RegClass cls =
                (param.type.kind == il::core::Type::Kind::F64) ? RegClass::FPR : RegClass::GPR;
            classes.push_back(cls);
            spillOffsets.push_back(fb.ensureSpill(id));
        }
        out.vregId.emplace(bb.label, std::move(ids));
        out.regClass.emplace(bb.label, std::move(classes));
        out.spillOffset.emplace(bb.label, std::move(spillOffsets));
    }
    return out;
}

} // namespace

[[maybe_unused]] std::optional<std::size_t>
// cppcheck-suppress unusedFunction
LowerILToMIR::knownVarArgNamedArgs(std::string_view callee) const {
    const auto it = knownVarArgNamedArgCounts_.find(std::string(callee));
    if (it == knownVarArgNamedArgCounts_.end())
        return std::nullopt;
    return it->second;
}

MFunction LowerILToMIR::lowerFunction(const il::core::Function &fn) const {
    MFunction mf{};
    mf.name = fn.name;

    // Lowering helpers hold MBasicBlock references while appending auxiliary
    // blocks (switch-tree nodes, phi edge blocks), so MFunction::blocks must
    // never reallocate mid-lowering. Compute an upper bound on the number of
    // auxiliary blocks instead of guessing: every terminator may split at most
    // two edges plus, for switches, three blocks per branch target; shared
    // trap blocks add a small per-function constant. The capacity is verified
    // after lowering (see the ICE check at the end of this function).
    std::size_t auxBlockBudget = 8; // shared trap blocks + slack
    for (const auto &bbIn : fn.blocks) {
        for (const auto &ins : bbIn.instructions) {
            if (!ins.labels.empty())
                auxBlockBudget += 2 + 3 * ins.labels.size();
        }
    }
    mf.blocks.reserve(fn.blocks.size() + auxBlockBudget);

    // Pre-create MIR blocks with labels to mirror IL CFG shape.
    for (const auto &bb : fn.blocks) {
        mf.blocks.emplace_back();
        mf.blocks.back().name = bb.label;
    }

    // Support i64 and pointer-centric functions; arithmetic patterns remain i64-centric.

    // Phase 1: Walk allocas → frame locals.
    FrameBuilder fb{mf};
    const std::unordered_set<unsigned> allocaTemps = setupFrameLocals(fn, fb);

    // Phase 2: Assign canonical phi vregs + spill slots for non-entry block params.
    PhiAssignment phi = allocatePhiSlots(fn, fb);
    auto &phiVregId = phi.vregId;
    auto &phiRegClass = phi.regClass;
    auto &phiSpillOffset = phi.spillOffset;

    // ===========================================================================
    // Global Liveness Analysis for Cross-Block Temps
    // ===========================================================================
    LivenessInfo liveness = analyzeCrossBlockLiveness(fn, allocaTemps, fb);

    // Try fast-paths for simple function patterns
    if (auto result =
            tryFastPaths(fn, *ti_, fb, mf, stringLiteralByteLengths_, &knownVarArgNamedArgCounts_))
        return *result;

    // Generic fallback: lower stack/local loads/stores and a simple return
    // This path handles arbitrary placement of alloca/load/store in a single block without
    // full-blown selection for other ops yet.

    // Use a single function-wide tempVReg map so values materialized in one block
    // are visible to other blocks. This handles cross-block value references that
    // the BASIC frontend generates (e.g., array operations using values from predecessor blocks).
    const auto tempUseCounts = countTempUses(fn);
    std::unordered_map<unsigned, uint16_t> tempVReg;
    // Track register class (GPR vs FPR) for each temp within this function
    std::unordered_map<unsigned, RegClass> tempRegClass = buildTempRegClassMap(fn);
    uint16_t nextVRegId = kFirstVirtualRegId; // vreg ids start at 1

    // Map function parameter IDs to their spill offsets (for entry block params)
    std::unordered_map<unsigned, int> funcParamSpillOffset;

    // Shared trap-block requests for this function (materialised after the
    // main lowering loops so block references stay valid; one block per kind).
    std::unordered_map<std::string, TrapBlockRequest> sharedTrapBlocks;

    // Record the reserved capacity so the no-reallocation invariant can be
    // verified once lowering completes.
    const std::size_t reservedBlockCapacity = mf.blocks.capacity();

    // Save per-block tempVReg snapshots so terminator loop can use the correct vreg mappings.
    // This is needed because cross-block temp reloading in later blocks can overwrite tempVReg
    // entries, but the terminator loop for the DEFINING block needs the original vreg.
    std::vector<std::unordered_map<unsigned, uint16_t>> blockTempVRegSnapshot(fn.blocks.size());

    for (std::size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        const auto &bbIn = fn.blocks[bi];
        // NOTE: We use index bi to access mf.blocks[bi] instead of a reference because
        // instruction lowering can add new trap blocks via emplace_back(), which may
        // reallocate the vector and invalidate references.
        // NOTE: Do NOT clear tempRegClass here - we need to preserve class info for
        // cross-block temps that are spilled/reloaded. It's already cleared at function start.
        // Helper lambda to get current output block (avoids dangling references)
        auto bbOutFn = [&]() -> MBasicBlock & { return mf.blocks[bi]; };
        std::unordered_set<unsigned> reloadedCrossBlockTemps;

        auto reloadCrossBlockTempAtBlockEntry = [&](unsigned tempId) {
            auto spillIt = liveness.crossBlockSpillOffset.find(tempId);
            auto defIt = liveness.tempDefBlock.find(tempId);
            if (spillIt == liveness.crossBlockSpillOffset.end() ||
                defIt == liveness.tempDefBlock.end() || defIt->second == bi) {
                return;
            }

            // Cross-block temps need a fresh vreg in each block because the register allocator
            // is free to assign unrelated physical registers once control leaves the defining
            // block. Reload exactly once per block and then reuse the rematerialized mapping.
            if (!reloadedCrossBlockTemps.insert(tempId).second)
                return;

            const uint16_t vid = allocateNextVReg(nextVRegId);
            tempVReg[tempId] = vid;
            const int offset = spillIt->second;
            auto clsIt = tempRegClass.find(tempId);
            const RegClass cls = (clsIt != tempRegClass.end()) ? clsIt->second : RegClass::GPR;
            if (cls == RegClass::FPR) {
                bbOutFn().instrs.push_back(
                    MInstr{MOpcode::LdrFprFpImm,
                           {MOperand::vregOp(RegClass::FPR, vid), MOperand::immOp(offset)}});
            } else {
                bbOutFn().instrs.push_back(
                    MInstr{MOpcode::LdrRegFpImm,
                           {MOperand::vregOp(RegClass::GPR, vid), MOperand::immOp(offset)}});
            }
        };

        // Entry block (bi == 0): Spill function parameters to stack slots immediately.
        // This ensures parameters are preserved across function calls within the entry block.
        // ABI registers (x0-x7, v0-v7) are caller-saved and will be clobbered by calls.
        if (bi == 0 && !bbIn.params.empty())
            spillEntryBlockParams(fn,
                                  bbIn,
                                  *ti_,
                                  bbOutFn(),
                                  tempVReg,
                                  tempRegClass,
                                  funcParamSpillOffset,
                                  liveness.crossBlockSpillOffset,
                                  nextVRegId);

        // Load block parameters from spill slots into fresh vregs at block entry.
        // The edge copies store values to these spill slots before branching here.
        auto itPhi = phiVregId.find(bbIn.label);
        auto itSpill = phiSpillOffset.find(bbIn.label);
        if (itPhi != phiVregId.end() && itSpill != phiSpillOffset.end()) {
            const auto &ids = itPhi->second;
            const auto &spillOffsets = itSpill->second;
            for (std::size_t pi = 0; pi < bbIn.params.size() && pi < ids.size(); ++pi) {
                const uint16_t vid = allocateNextVReg(nextVRegId);
                const unsigned paramId = bbIn.params[pi].id;
                tempVReg[paramId] = vid;
                const auto &pt = bbIn.params[pi].type;
                const RegClass cls =
                    (pt.kind == il::core::Type::Kind::F64) ? RegClass::FPR : RegClass::GPR;
                tempRegClass[paramId] = cls;
                const int offset = spillOffsets[pi];
                // Load from spill slot into vreg
                if (cls == RegClass::FPR) {
                    bbOutFn().instrs.push_back(
                        MInstr{MOpcode::LdrFprFpImm,
                               {MOperand::vregOp(RegClass::FPR, vid), MOperand::immOp(offset)}});
                } else {
                    bbOutFn().instrs.push_back(
                        MInstr{MOpcode::LdrRegFpImm,
                               {MOperand::vregOp(RegClass::GPR, vid), MOperand::immOp(offset)}});
                }

                // A promoted block parameter can be used directly by dominated
                // successor blocks. Those uses are handled by the cross-block
                // temp reload path below, so publish the freshly loaded phi
                // value into its cross-block spill slot at the defining block.
                if (auto spillIt = liveness.crossBlockSpillOffset.find(paramId);
                    spillIt != liveness.crossBlockSpillOffset.end()) {
                    const int crossBlockOffset = spillIt->second;
                    if (cls == RegClass::FPR) {
                        bbOutFn().instrs.push_back(MInstr{MOpcode::StrFprFpImm,
                                                          {MOperand::vregOp(RegClass::FPR, vid),
                                                           MOperand::immOp(crossBlockOffset)}});
                    } else {
                        bbOutFn().instrs.push_back(MInstr{MOpcode::StrRegFpImm,
                                                          {MOperand::vregOp(RegClass::GPR, vid),
                                                           MOperand::immOp(crossBlockOffset)}});
                    }
                }
            }
        }

        // Reload cross-block temps that are used in this block but defined elsewhere.
        // We need to reload them at block entry because the register allocator may have
        // reused their physical registers in intervening blocks.
        for (const auto &ins : bbIn.instructions) {
            for (const auto &op : ins.operands) {
                if (op.kind == il::core::Value::Kind::Temp)
                    reloadCrossBlockTempAtBlockEntry(op.id);
            }
            for (const auto &edgeArgs : ins.brArgs) {
                for (const auto &arg : edgeArgs) {
                    if (arg.kind == il::core::Value::Kind::Temp)
                        reloadCrossBlockTempAtBlockEntry(arg.id);
                }
            }
        }
        // Also check terminator for cross-block temp uses (CBr condition)
        if (!bbIn.instructions.empty()) {
            const auto &term = bbIn.instructions.back();
            if (term.op == il::core::Opcode::CBr && !term.operands.empty()) {
                const auto &cond = term.operands[0];
                if (cond.kind == il::core::Value::Kind::Temp)
                    reloadCrossBlockTempAtBlockEntry(cond.id);
            }
        }

        // Create lowering context for dispatching to extracted handlers
        LoweringContext ctx{fn,
                            *ti_,
                            fb,
                            mf,
                            nextVRegId,
                            tempVReg,
                            tempRegClass,
                            phiVregId,
                            phiRegClass,
                            phiSpillOffset,
                            liveness.crossBlockSpillOffset,
                            liveness.tempDefBlock,
                            liveness.crossBlockTemps,
                            stringLiteralByteLengths_,
                            &knownVarArgNamedArgCounts_,
                            sharedTrapBlocks};

        for (const auto &ins : bbIn.instructions) {
            // Record instruction count so we can stamp source loc on new MInstrs.
            const size_t mirCountBefore = bbOutFn().instrs.size();

            // When a compare result is consumed only by this block's cbr, defer the
            // lowering to TerminatorLowering so it can emit cmp+b.cond directly.
            if (ins.result && isCompareOp(ins.op) &&
                tempUseCount(tempUseCounts, *ins.result) == 1 &&
                cbrConsumesTemp(bbIn, *ins.result)) {
                continue;
            }

            // Try extracted handlers first; they return true if they handled the opcode
            if (lowerInstruction(ins, bbIn, ctx, bi)) {
                // Spill cross-block temps immediately after they are defined.
                // This ensures the value is preserved in memory for use in other blocks.
                if (ins.result) {
                    auto spillIt = liveness.crossBlockSpillOffset.find(*ins.result);
                    if (spillIt != liveness.crossBlockSpillOffset.end()) {
                        auto vregIt = tempVReg.find(*ins.result);
                        if (vregIt != tempVReg.end()) {
                            const uint16_t srcVreg = vregIt->second;
                            const int offset = spillIt->second;
                            // Respect the producing register class when spilling
                            auto clsIt = tempRegClass.find(*ins.result);
                            const RegClass cls =
                                (clsIt != tempRegClass.end()) ? clsIt->second : RegClass::GPR;
                            if (cls == RegClass::FPR) {
                                bbOutFn().instrs.push_back(
                                    MInstr{MOpcode::StrFprFpImm,
                                           {MOperand::vregOp(RegClass::FPR, srcVreg),
                                            MOperand::immOp(offset)}});
                            } else {
                                bbOutFn().instrs.push_back(
                                    MInstr{MOpcode::StrRegFpImm,
                                           {MOperand::vregOp(RegClass::GPR, srcVreg),
                                            MOperand::immOp(offset)}});
                            }
                        }
                    }
                }
                continue;
            }

            switch (ins.op) {
                    // NOTE: Zext1, Trunc1, CastSiNarrowChk, CastUiNarrowChk, CastFpToSiRteChk,
                    // CastFpToUiRteChk, CastSiToFp, CastUiToFp, SRemChk0, SDivChk0, UDivChk0,
                    // URemChk0, FAdd, FSub, FMul, FDiv, FCmpEQ, FCmpNE, FCmpLT, FCmpLE, FCmpGT,
                    // FCmpGE, Sitofp, Fptosi are handled by lowerInstruction() above

                // NOTE: Br, CBr, Call, Store, GEP, Load, Ret, Alloca, FP ops,
                // and conversions are all handled by lowerInstruction() in OpcodeDispatch.cpp
                case il::core::Opcode::Count:
                default:
                    // Handle binary ops and comparisons that may be referenced cross-block.
                    // This ensures values are materialized and cached in tempVReg for later use.
                    if (ins.result && ins.operands.size() == 2) {
                        const auto *binOp = lookupBinaryOp(ins.op);
                        if (binOp || isCompareOp(ins.op)) {
                            uint16_t lhs = 0, rhs = 0;
                            RegClass lcls = RegClass::GPR, rcls = RegClass::GPR;
                            if (materializeValueToVReg(ins.operands[0],
                                                       bbIn,
                                                       *ti_,
                                                       fb,
                                                       bbOutFn(),
                                                       tempVReg,
                                                       tempRegClass,
                                                       nextVRegId,
                                                       lhs,
                                                       lcls) &&
                                materializeValueToVReg(ins.operands[1],
                                                       bbIn,
                                                       *ti_,
                                                       fb,
                                                       bbOutFn(),
                                                       tempVReg,
                                                       tempRegClass,
                                                       nextVRegId,
                                                       rhs,
                                                       rcls)) {
                                const bool fpBinary = binOp && isFloatingPointOp(ins.op);
                                const bool fpCompare = !binOp && isFloatingPointCompareOp(ins.op);
                                if (fpBinary || fpCompare) {
                                    lhs =
                                        coerceScalarOperandToFpr(lhs, lcls, nextVRegId, bbOutFn());
                                    rhs =
                                        coerceScalarOperandToFpr(rhs, rcls, nextVRegId, bbOutFn());
                                } else if (lcls != RegClass::GPR || rcls != RegClass::GPR) {
                                    throw std::runtime_error("AArch64 codegen: register class "
                                                             "mismatch in binary lowering");
                                }

                                const RegClass rc = fpBinary ? RegClass::FPR : RegClass::GPR;
                                const uint16_t dst = allocateNextVReg(nextVRegId);
                                tempVReg[*ins.result] = dst;
                                tempRegClass[*ins.result] = rc;
                                if (binOp) {
                                    if (isSubWidthCheckedOverflowOp(ins) &&
                                        emitSubWidthCheckedBinary(
                                            bbOutFn(),
                                            ins,
                                            dst,
                                            lhs,
                                            rhs,
                                            requestSharedTrapBlock(
                                                ctx, "subwidth_ovf_", "rt_trap_ovf"),
                                            nextVRegId)) {
                                        // Width-aware checked arithmetic emitted above.
                                    } else {
                                        // Check if we can use immediate form for this operation
                                        const bool hasConstRHS =
                                            ins.operands[1].kind == il::core::Value::Kind::ConstInt;
                                        const bool isShift = (ins.op == il::core::Opcode::Shl ||
                                                              ins.op == il::core::Opcode::LShr ||
                                                              ins.op == il::core::Opcode::AShr);
                                        const bool isAddSub =
                                            (ins.op == il::core::Opcode::Add ||
                                             ins.op == il::core::Opcode::IAddOvf ||
                                             ins.op == il::core::Opcode::Sub ||
                                             ins.op == il::core::Opcode::ISubOvf);
                                        const bool isBitwise = (ins.op == il::core::Opcode::And ||
                                                                ins.op == il::core::Opcode::Or ||
                                                                ins.op == il::core::Opcode::Xor);

                                        // Use immediate form if:
                                        // 1. RHS is a constant AND
                                        // 2. Operation supports immediate AND
                                        // 3. Value fits in the instruction's immediate field
                                        bool useImmediate = false;
                                        if (hasConstRHS && binOp->supportsImmediate) {
                                            const auto immVal = ins.operands[1].i64;
                                            if (isShift && isValidShiftAmount(immVal))
                                                useImmediate = true;
                                            else if (isAddSub && isUImm12(immVal))
                                                useImmediate = true;
                                            else if (isBitwise &&
                                                     isLogicalImmediate(
                                                         static_cast<uint64_t>(immVal)))
                                                useImmediate = true;
                                        }

                                        if (useImmediate) {
                                            // Emit with immediate operand - no need to materialize
                                            // RHS Note: FP ops have supportsImmediate=false, so rc
                                            // is always GPR here
                                            bbOutFn().instrs.push_back(
                                                MInstr{binOp->immOp,
                                                       {MOperand::vregOp(rc, dst),
                                                        MOperand::vregOp(rc, lhs),
                                                        MOperand::immOp(ins.operands[1].i64)}});
                                        } else {
                                            // Emit binary op with all register operands
                                            bbOutFn().instrs.push_back(
                                                MInstr{binOp->mirOp,
                                                       {MOperand::vregOp(rc, dst),
                                                        MOperand::vregOp(rc, lhs),
                                                        MOperand::vregOp(rc, rhs)}});
                                        }
                                    }
                                } else {
                                    // Emit comparison (cmp + cset)
                                    if (fpCompare) {
                                        bbOutFn().instrs.push_back(
                                            MInstr{MOpcode::FCmpRR,
                                                   {MOperand::vregOp(RegClass::FPR, lhs),
                                                    MOperand::vregOp(RegClass::FPR, rhs)}});
                                        emitFpCompareResult(bbOutFn(), ins.op, dst, nextVRegId);
                                        break;
                                    }
                                    // Check if RHS is a small constant for CmpRI form
                                    const bool rhsIsSmallConst =
                                        ins.operands[1].kind == il::core::Value::Kind::ConstInt &&
                                        isUImm12(ins.operands[1].i64);

                                    if (rhsIsSmallConst) {
                                        bbOutFn().instrs.push_back(
                                            MInstr{MOpcode::CmpRI,
                                                   {MOperand::vregOp(RegClass::GPR, lhs),
                                                    MOperand::immOp(ins.operands[1].i64)}});
                                    } else {
                                        bbOutFn().instrs.push_back(
                                            MInstr{MOpcode::CmpRR,
                                                   {MOperand::vregOp(RegClass::GPR, lhs),
                                                    MOperand::vregOp(RegClass::GPR, rhs)}});
                                    }
                                    bbOutFn().instrs.push_back(
                                        MInstr{MOpcode::Cset,
                                               {MOperand::vregOp(RegClass::GPR, dst),
                                                MOperand::condOp(condForOpcode(ins.op))}});
                                }
                            }
                        }
                    } else {
                        throw std::runtime_error("AArch64 codegen: unhandled IL opcode '" +
                                                 std::string(il::core::toString(ins.op)) +
                                                 "' in block '" + bbIn.label + "'");
                    }
                    break;
            }

            // Spill cross-block temps immediately after they are defined.
            // This ensures the value is preserved in memory for use in other blocks,
            // since the register allocator may reuse the physical register.
            if (ins.result) {
                auto spillIt = liveness.crossBlockSpillOffset.find(*ins.result);
                if (spillIt != liveness.crossBlockSpillOffset.end()) {
                    // This temp is used in another block - spill it now
                    auto vregIt = tempVReg.find(*ins.result);
                    if (vregIt != tempVReg.end()) {
                        const uint16_t srcVreg = vregIt->second;
                        const int offset = spillIt->second;
                        // Check register class for this temp
                        auto clsIt = tempRegClass.find(*ins.result);
                        const RegClass cls =
                            (clsIt != tempRegClass.end()) ? clsIt->second : RegClass::GPR;
                        if (cls == RegClass::FPR) {
                            bbOutFn().instrs.push_back(
                                MInstr{MOpcode::StrFprFpImm,
                                       {MOperand::vregOp(RegClass::FPR, srcVreg),
                                        MOperand::immOp(offset)}});
                        } else {
                            bbOutFn().instrs.push_back(
                                MInstr{MOpcode::StrRegFpImm,
                                       {MOperand::vregOp(RegClass::GPR, srcVreg),
                                        MOperand::immOp(offset)}});
                        }
                    }
                }
            }

            // Stamp source location on all MInstrs emitted by this IL instruction.
            for (size_t mi = mirCountBefore; mi < bbOutFn().instrs.size(); ++mi)
                bbOutFn().instrs[mi].loc = ins.loc;
        }

        // Save tempVReg snapshot for this block before processing next block.
        // The terminator loop will use this snapshot to get correct vreg mappings
        // for temps defined in this block, since later blocks may overwrite tempVReg.
        blockTempVRegSnapshot[bi] = tempVReg;
    }

    // Lower control-flow terminators: br, cbr, trap AFTER all other instructions
    // This ensures branches appear after the values they depend on are computed.
    lowerTerminators(fn,
                     mf,
                     *ti_,
                     fb,
                     phiVregId,
                     phiRegClass,
                     phiSpillOffset,
                     blockTempVRegSnapshot,
                     tempRegClass,
                     nextVRegId);

    // Materialise the shared trap blocks requested during lowering. Sort by
    // label so emission order is deterministic across STL implementations.
    {
        std::vector<const TrapBlockRequest *> requests;
        requests.reserve(sharedTrapBlocks.size());
        for (const auto &entry : sharedTrapBlocks)
            requests.push_back(&entry.second);
        std::sort(requests.begin(),
                  requests.end(),
                  [](const TrapBlockRequest *lhs, const TrapBlockRequest *rhs) {
                      return lhs->label < rhs->label;
                  });
        for (const TrapBlockRequest *request : requests) {
            mf.blocks.emplace_back();
            MBasicBlock &trapBlock = mf.blocks.back();
            trapBlock.name = request->label;
            if (!request->callee.empty()) {
                trapBlock.instrs.push_back(
                    MInstr{MOpcode::Bl, {MOperand::labelOp(request->callee)}});
            } else {
                trapBlock.instrs.push_back(
                    MInstr{MOpcode::MovRI,
                           {MOperand::regOp(PhysReg::X0), MOperand::immOp(request->raiseCode)}});
                trapBlock.instrs.push_back(
                    MInstr{MOpcode::Bl, {MOperand::labelOp("rt_trap_raise_error")}});
                trapBlock.instrs.push_back(MInstr{MOpcode::Ret, {}});
            }
        }
    }

    // The auxiliary-block budget above must cover everything lowering
    // appended; a reallocation would have invalidated MBasicBlock references
    // held by lowering helpers (silent UB). Fail loudly instead.
    if (mf.blocks.capacity() != reservedBlockCapacity) {
        ZANNA_ICE("AArch64 lowering: MFunction::blocks reallocated while lowering '" + fn.name +
                  "' (reserved " + std::to_string(reservedBlockCapacity) + ", now " +
                  std::to_string(mf.blocks.size()) +
                  " blocks); auxiliary block budget is too small");
    }

    fb.finalize();
    return mf;
}

} // namespace zanna::codegen::aarch64
