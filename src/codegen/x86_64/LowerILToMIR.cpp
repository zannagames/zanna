//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE in the project root for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/LowerILToMIR.cpp
// Purpose: Bridge IL produced by the compiler front-end into Machine IR consumed
//          by the x86-64 backend using declarative lowering rules.
// Key invariants:
//   - Lowering preserves SSA identities and emits deterministic block ordering.
//   - Adapter internal caches remain consistent across functions.
//   - Virtual register allocation and literal materialisation are centralised
//     here so rule implementations remain stateless.
// Ownership/Lifetime:
//   - The adapter owns transient per-function lowering state and writes into
//     caller-owned Machine IR structures.
// Links: src/codegen/x86_64/LowerILToMIR.hpp,
//        src/codegen/x86_64/LoweringRules.hpp,
//        src/codegen/x86_64/MachineIR.hpp
//
//===----------------------------------------------------------------------===//

#include "LowerILToMIR.hpp"

#include "LoweringRules.hpp"
#include "OperandUtils.hpp"
#include "Unsupported.hpp"
#include "codegen/common/FrameLayoutUtils.hpp"
#include "codegen/common/ICE.hpp"
#include "codegen/common/StringRetainPolicy.hpp"
#include "il/runtime/RuntimeSignatures.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

/**
 * @file
 * @brief Implements IL-to-MIR adaptation for the x86-64 lowering-rule registry.
 *
 * This layer pre-allocates MIR blocks, maps SSA values to deterministic virtual
 * registers, materializes constants and literals, records delayed ABI call
 * plans, inserts entry-parameter and edge parallel copies, and stamps emitted
 * instructions with source locations.
 */

namespace zanna::codegen::x64 {

namespace {

/// @brief Report an error when no lowering rule matches an instruction.
/// @details Throws an exception to signal the unsupported instruction in both
///          debug and release builds. This ensures invalid IL is never silently
///          ignored.
/// @param instr Instruction that failed to match any rule.
[[noreturn]] void reportNoRule(const ILInstr &instr) {
    std::string msg = "No lowering rule matched instruction: ";
    msg += instr.opcode;
    phaseAUnsupported(msg.c_str());
}

/// @brief Canonicalise an integer-class IL constant for MIR emission.
/// @details I1 booleans are normalised to {0, 1} so downstream code never has
///          to defend against truthy-but-non-one inputs (e.g. -1 read from
///          memory). All other integer kinds pass through their @c i64 payload
///          unchanged. Pointer immediates are intentionally not handled here —
///          callers select the canonical form based on @ref isIntegerLikeKind.
/// @param value IL value carrying the immediate payload.
/// @return The canonical signed 64-bit immediate value.
[[nodiscard]] int64_t canonicalIntegerImmediate(const ILValue &value) noexcept {
    return value.kind == ILValue::Kind::I1 ? (value.i64 != 0 ? 1 : 0) : value.i64;
}

/// @brief Predicate: does @p kind use the GPR class for codegen purposes?
/// @details Integers, booleans, and pointers all live in GPRs on x86-64. F64
///          uses XMM and LABEL/STR have no native machine representation.
/// @param kind IL value kind to classify.
/// @return True for I64/I1/PTR.
[[nodiscard]] bool isIntegerLikeKind(ILValue::Kind kind) noexcept {
    return kind == ILValue::Kind::I64 || kind == ILValue::Kind::I1 || kind == ILValue::Kind::PTR;
}

/// @brief Abort lowering with an ICE when an internal builder pointer is missing.
/// @details MIRBuilder methods are declared @c noexcept because they are thin
///          accessors used by many lowering rules. Missing adapter/block state
///          indicates a compiler bug rather than user IL, so the check remains
///          active in release builds and terminates via @ref ZANNA_ICE instead
///          of throwing through a noexcept boundary.
/// @param condition True when the required invariant holds.
/// @param message Human-readable invariant failure.
inline void requireBuilderInvariant(bool condition, const char *message) noexcept {
    if (!condition) {
        ZANNA_ICE(message);
    }
}

} // namespace

// -----------------------------------------------------------------------------
// MIRBuilder façade
// -----------------------------------------------------------------------------

/// @brief Construct a builder facade that emits into a specific MIR block.
/// @param lower Adapter that provides lowering utilities and state.
/// @param block Machine IR block that will receive emitted instructions.
MIRBuilder::MIRBuilder(LowerILToMIR &lower, MBasicBlock &block) noexcept
    : lower_{&lower}, block_{&block} {}

/// @brief Access the mutable machine block being populated.
/// @return Reference to the target machine block.
MBasicBlock &MIRBuilder::block() noexcept {
    requireBuilderInvariant(block_ != nullptr, "MIRBuilder missing block");
    return *block_;
}

/// @brief Access the machine block being populated (const view).
/// @return Const reference to the target machine block.
const MBasicBlock &MIRBuilder::block() const noexcept {
    requireBuilderInvariant(block_ != nullptr, "MIRBuilder missing block");
    return *block_;
}

/// @brief Access the owning adapter for helper services.
/// @return Reference to the lowering adapter.
LowerILToMIR &MIRBuilder::lower() noexcept {
    requireBuilderInvariant(lower_ != nullptr, "MIRBuilder missing adapter");
    return *lower_;
}

/// @brief Access the owning adapter (const view).
/// @return Const reference to the lowering adapter.
const LowerILToMIR &MIRBuilder::lower() const noexcept {
    requireBuilderInvariant(lower_ != nullptr, "MIRBuilder missing adapter");
    return *lower_;
}

/// @brief Retrieve target information describing registers and calling
///        conventions.
/// @return Const reference to the target description.
const TargetInfo &MIRBuilder::target() const noexcept {
    requireBuilderInvariant(lower_ != nullptr && lower_->target_ != nullptr,
                            "Target info unavailable");
    return *lower_->target_;
}

/// @brief Access the read-only data pool used to materialise literals.
/// @return Reference to the shared rodata pool.
AsmEmitter::RoDataPool &MIRBuilder::roData() const noexcept {
    requireBuilderInvariant(lower_ != nullptr && lower_->roDataPool_ != nullptr,
                            "RoData pool unavailable");
    return *lower_->roDataPool_;
}

/// @brief Map an IL value kind to a machine register class.
/// @param kind IL value classification (integer, float, pointer, etc.).
/// @return Register class that should represent the value.
RegClass MIRBuilder::regClassFor(ILValue::Kind kind) const noexcept {
    requireBuilderInvariant(lower_ != nullptr, "MIRBuilder missing adapter");
    return LowerILToMIR::regClassFor(kind);
}

/// @brief Ensure an SSA identifier has a materialised virtual register.
/// @details Delegates to the adapter to allocate or reuse the register mapping.
/// @param id SSA identifier from the IL.
/// @param kind IL value kind associated with the identifier.
/// @return Virtual register assigned to the identifier.
VReg MIRBuilder::ensureVReg(int id, ILValue::Kind kind) {
    requireBuilderInvariant(lower_ != nullptr, "MIRBuilder missing adapter");
    return lower_->ensureVReg(id, kind);
}

/// @brief Allocate a fresh temporary virtual register.
/// @param cls Register class to assign to the new temporary.
/// @return Newly allocated virtual register.
VReg MIRBuilder::makeTempVReg(RegClass cls) {
    requireBuilderInvariant(lower_ != nullptr, "MIRBuilder missing adapter");
    return lower_->makeTempVReg(cls);
}

/// @brief Convert an IL value into a machine operand, materialising literals as needed.
/// @param value IL value to translate.
/// @param cls Preferred register class when a temporary must be created.
/// @return Machine operand referencing the value.
Operand MIRBuilder::makeOperandForValue(const ILValue &value, RegClass cls) {
    requireBuilderInvariant(lower_ != nullptr && block_ != nullptr,
                            "MIRBuilder missing adapter or block");
    return lower_->makeOperandForValue(*block_, value, cls);
}

/// @brief Try to convert an IL value into a machine operand without side effects.
/// @param value IL value to inspect.
/// @param cls Required register class for a register result.
/// @return Existing operand representation, or nullopt if conversion would mutate lowering state.
std::optional<Operand> MIRBuilder::tryGetOperandForValue(const ILValue &value, RegClass cls) const {
    requireBuilderInvariant(lower_ != nullptr, "MIRBuilder missing adapter");
    return lower_->tryGetOperandForValue(value, cls);
}

/// @brief Translate a label IL value into a machine operand.
/// @param value IL label value.
/// @return Machine operand referencing the label.
Operand MIRBuilder::makeLabelOperand(const ILValue &value) const {
    requireBuilderInvariant(lower_ != nullptr, "MIRBuilder missing adapter");
    return lower_->makeLabelOperand(value);
}

/// @brief Determine whether an IL value encodes a literal immediate.
/// @param value IL value to inspect.
/// @return True when the value should be emitted as an immediate operand.
bool MIRBuilder::isImmediate(const ILValue &value) const noexcept {
    requireBuilderInvariant(lower_ != nullptr, "MIRBuilder missing adapter");
    return lower_->isImmediate(value);
}

/// @brief Set the source location for subsequently emitted instructions.
void MIRBuilder::setCurrentLoc(il::support::SourceLoc loc) noexcept {
    currentLoc_ = loc;
}

/// @brief Append a machine instruction to the current block.
/// @param instr Machine instruction to insert.
void MIRBuilder::append(MInstr instr) {
    requireBuilderInvariant(block_ != nullptr, "MIRBuilder missing block");
    instr.loc = currentLoc_;
    block_->append(std::move(instr));
}

/// @brief Record a call-lowering plan produced by a rule.
/// @param plan Plan describing register shuffles and call conventions.
uint32_t MIRBuilder::recordCallPlan(CallLoweringPlan plan) {
    requireBuilderInvariant(lower_ != nullptr, "MIRBuilder missing adapter");
    return lower_->recordCallPlan(std::move(plan));
}

/// @brief Reserve a stack-local placeholder slot for an alloca.
/// @details Forwards to the owning adapter; the actual offset is rewritten by
///          FrameLowering once callee-saved register pressure is known.
/// @param sizeBytes Size of the allocation in bytes.
/// @param alignBytes Required alignment in bytes (defaulted by the header).
/// @return Negative @c %rbp-relative placeholder displacement.
int32_t MIRBuilder::reserveStackLocalPlaceholder(int sizeBytes, int alignBytes) {
    requireBuilderInvariant(lower_ != nullptr, "MIRBuilder missing adapter");
    return lower_->reserveStackLocalPlaceholder(sizeBytes, alignBytes);
}

// -----------------------------------------------------------------------------
// Adapter core
// -----------------------------------------------------------------------------

/// @brief Construct the lowering adapter with target configuration and rodata pool.
/// @param target Target description including register assignment details.
/// @param roData Read-only data pool used for literal materialisation.
LowerILToMIR::LowerILToMIR(const TargetInfo &target, AsmEmitter::RoDataPool &roData) noexcept
    : target_{&target}, roDataPool_{&roData} {}

/// @brief Expose the call-lowering plans accumulated during lowering.
/// @return Reference to the vector of plans emitted by call rules.
const std::vector<CallLoweringPlan> &LowerILToMIR::callPlans() const noexcept {
    return callPlans_;
}

/// @brief Install the set of variadic-call targets recognised by call lowering.
/// @details The compiler may know, ahead of lowering, that certain symbols
///          obey C variadic calling conventions. Recording them here lets call
///          rules route through the SysV "AL holds XMM count" path even for
///          user-defined functions that declare @c ... in IL.
/// @param callees Symbol names; takes ownership via move.
void LowerILToMIR::setKnownVarArgCallees(std::unordered_set<std::string> callees) {
    knownVarArgCallees_ = std::move(callees);
}

/// @brief Query whether @p callee was registered as a vararg function.
/// @param callee Symbol name as it appears in the call IL instruction.
/// @return True when @p callee is non-empty and present in the vararg set.
bool LowerILToMIR::isKnownVarArgCallee(std::string_view callee) const {
    if (callee.empty()) {
        return false;
    }
    return knownVarArgCallees_.find(std::string{callee}) != knownVarArgCallees_.end();
}

/// @brief Record a call-lowering plan and return its stable identifier.
/// @details The id is later attached to the @c CALL MInstr so frame lowering
///          can recover the plan when emitting argument shuffles.
/// @param plan Plan describing the call's argument classes and return wiring.
/// @return Index of the plan within @c callPlans_.
uint32_t LowerILToMIR::recordCallPlan(CallLoweringPlan plan) {
    const uint32_t id = static_cast<uint32_t>(callPlans_.size());
    callPlans_.push_back(std::move(plan));
    return id;
}

/// @brief Reserve a fresh stack slot for an alloca, returning a placeholder offset.
/// @details The returned offset is negative and refers to a position that the
///          frame lowering pass will later shift below the callee-saved area.
///          The slot count and alignment are computed in @c kSlotSizeBytes
///          units to mirror the layout assumptions made by FrameLowering.
/// @param sizeBytes Requested allocation size in bytes.
/// @param alignBytes Requested alignment in bytes (clamped to @c kSlotSizeBytes).
/// @return Negative @c %rbp-relative placeholder displacement for the slot.
int32_t LowerILToMIR::reserveStackLocalPlaceholder(int sizeBytes, int alignBytes) {
    const int slotCount = common::bytesToSlots(sizeBytes, kSlotSizeBytes);
    const int alignSlots =
        std::max(1, common::bytesToSlots(std::max(alignBytes, kSlotSizeBytes), kSlotSizeBytes));
    const int alignedStart = common::roundUpBytes(nextStackLocalSlot_, alignSlots);
    nextStackLocalSlot_ = alignedStart + slotCount;
    return -static_cast<int32_t>(nextStackLocalSlot_ * kSlotSizeBytes);
}

/// @brief Reset per-function caches before lowering a new function.
/// @details Clears virtual register assignments, block metadata, and pending
///          call plans so state from the previous function does not leak.
void LowerILToMIR::resetFunctionState() {
    nextVReg_ = 1U;
    // Note: nextLocalLabel_ is intentionally NOT reset — it is a module-wide
    // counter so that local labels (e.g. .Lfptosi_chk_trap_N) stay unique
    // across all functions emitted into the same assembly stream.
    valueToVReg_.clear();
    blockInfo_.clear();
    callPlans_.clear();
    nextStackLocalSlot_ = 0;
    strLoadRetainElidable_.clear();
    strCallRetainElidable_.clear();
    allocaResultIds_.clear();
    ilProducers_.clear();
    nullGuardedBases_.clear();
    nullTrapRequested_ = false;
}

/**
 * @brief Analyze string-producing instructions for redundant defensive retains.
 *
 * Load retains are elided only for a single explicit release, a managed
 * move-out pattern, an immediately dominating explicit retain, or a safe
 * same-block borrow window. Transferring call results may omit their retain
 * when at most one consuming use spends the transferred reference. Successor
 * arguments are treated conservatively because downstream ownership is unknown.
 *
 * @param func Function whose string definitions and all uses are inspected.
 */
void LowerILToMIR::computeStrLoadRetainElidable(const ILFunction &func) {
    if (!zanna::codegen::shouldElideLoadRetainForRelease())
        return;

    /// Recognize calls whose first operand directly names the callee.
    const auto isDirectCall = [](const ILInstr &ins) {
        return ins.opcode == "call" && !ins.ops.empty() &&
               ins.ops.front().kind == ILValue::Kind::LABEL;
    };
    /// Recognize a direct call to a known string-release routine.
    const auto isReleaseCall = [&](const ILInstr &ins) {
        return isDirectCall(ins) && zanna::codegen::isStringReleaseCallee(ins.ops.front().label);
    };
    // Direct IL calls borrow parameters by default. Registered runtime helpers
    // spend only arguments explicitly classified as consumed; retaining an
    // argument does not consume the caller's reference.
    /// Determine whether a direct call borrows the indexed non-callee operand.
    const auto isBorrowedCallArg = [&](const ILInstr &ins, std::size_t opIdx) {
        if (!isDirectCall(ins) || opIdx == 0)
            return false;
        if (il::runtime::findRuntimeSignature(ins.ops.front().label) == nullptr)
            return true;
        const auto effects = il::runtime::classifyRuntimeOwnership(ins.ops.front().label);
        const unsigned argIdx = static_cast<unsigned>(opIdx - 1);
        return !effects.consumesArg(argIdx);
    };

    for (const auto &block : func.blocks) {
        for (std::size_t li = 0; li < block.instrs.size(); ++li) {
            const auto &instr = block.instrs[li];
            if (instr.opcode != "load" || instr.resultId < 0 ||
                instr.resultKind != ILValue::Kind::STR)
                continue;
            unsigned uses = 0;
            bool releaseOnly = true;
            bool borrowOnlyInBlock = true;
            std::size_t lastUseIdx = li;
            for (const auto &useBlock : func.blocks) {
                for (std::size_t ii = 0; ii < useBlock.instrs.size(); ++ii) {
                    const auto &useInstr = useBlock.instrs[ii];
                    for (std::size_t oi = 0; oi < useInstr.ops.size(); ++oi) {
                        const auto &op = useInstr.ops[oi];
                        if (op.id != instr.resultId || op.kind == ILValue::Kind::LABEL)
                            continue;
                        ++uses;
                        if (!isReleaseCall(useInstr))
                            releaseOnly = false;
                        if (&useBlock != &block || ii <= li || !isBorrowedCallArg(useInstr, oi))
                            borrowOnlyInBlock = false;
                        else
                            lastUseIdx = std::max(lastUseIdx, ii);
                    }
                }
                for (const auto &edge : useBlock.terminatorEdges) {
                    for (int argId : edge.argIds) {
                        if (argId == instr.resultId) {
                            ++uses;
                            releaseOnly = false;
                            borrowOnlyInBlock = false;
                        }
                    }
                }
            }
            if (uses == 0)
                continue;
            if (uses == 1 && releaseOnly) {
                strLoadRetainElidable_.insert(instr.resultId);
                continue;
            }

            // Managed merge slots transfer their +1 into the loaded SSA value.
            // Frontend lowering marks that move with an immediate raw null
            // store to the same address. Retaining the load would manufacture
            // an unbalanced second owner.
            if (li + 1 < block.instrs.size() && !instr.ops.empty()) {
                const auto &next = block.instrs[li + 1];
                const bool sameAddress =
                    instr.ops[0].id >= 0 && next.opcode == "store" && next.ops.size() >= 2 &&
                    next.ops[0].kind == instr.ops[0].kind && next.ops[0].id == instr.ops[0].id;
                const bool storesNull = next.ops.size() >= 2 && next.ops[1].id < 0 &&
                                        next.ops[1].i64 == 0 && next.ops[1].str.empty();
                if (sameAddress && storesNull) {
                    strLoadRetainElidable_.insert(instr.resultId);
                    continue;
                }
            }

            // Frontend-owned field snapshots place an explicit retain
            // immediately after the borrowed load. It dominates every later
            // use, including cleanup in successor blocks, and supplies exactly
            // the +1 that the backend fallback would add. A retain in another
            // block is not sufficient because it may not execute on every path.
            if (li + 1 < block.instrs.size()) {
                const auto &next = block.instrs[li + 1];
                if (isDirectCall(next) &&
                    zanna::codegen::isStringRetainCallee(next.ops.front().label)) {
                    for (std::size_t oi = 1; oi < next.ops.size(); ++oi) {
                        if (next.ops[oi].id == instr.resultId &&
                            next.ops[oi].kind != ILValue::Kind::LABEL) {
                            strLoadRetainElidable_.insert(instr.resultId);
                            break;
                        }
                    }
                    if (strLoadRetainElidable_.count(instr.resultId) != 0)
                        continue;
                }
            }
            if (!borrowOnlyInBlock)
                continue;
            // Borrow window: a string store can invalidate the source slot
            // that owns the borrowed reference. Explicit releases of other
            // SSA values are not barriers: those values carry their own
            // references, while retaining this load across a borrowed-only
            // use would have no balancing release in native code.
            bool windowSafe = true;
            for (std::size_t ii = li + 1; ii <= lastUseIdx && windowSafe; ++ii) {
                const auto &mid = block.instrs[ii];
                if (mid.opcode == "store" && !mid.ops.empty() && mid.ops.size() >= 2 &&
                    mid.ops[1].kind == ILValue::Kind::STR)
                    windowSafe = false;
            }
            if (windowSafe)
                strLoadRetainElidable_.insert(instr.resultId);
        }
    }

    // Call results: the callee's transferred reference covers exactly ONE
    // spend (consuming runtime arg, explicit release, ret, or branch arg —
    // the latter counted as two so the retain is kept). Elide the defensive
    // retain only when the callee transfers and the spends fit.
    for (const auto &block : func.blocks) {
        for (const auto &instr : block.instrs) {
            if (instr.opcode != "call" || instr.resultId < 0 ||
                instr.resultKind != ILValue::Kind::STR || !isDirectCall(instr))
                continue;
            if (!zanna::codegen::shouldElideStringResultRetain(instr.ops.front().label))
                continue;
            unsigned spends = 0;
            for (const auto &useBlock : func.blocks) {
                for (const auto &useInstr : useBlock.instrs) {
                    for (std::size_t oi = 0; oi < useInstr.ops.size(); ++oi) {
                        const auto &op = useInstr.ops[oi];
                        if (op.id != instr.resultId || op.kind == ILValue::Kind::LABEL)
                            continue;
                        if (useInstr.opcode == "ret") {
                            ++spends;
                        } else if (isReleaseCall(useInstr)) {
                            ++spends;
                        } else if (isDirectCall(useInstr) && oi > 0 &&
                                   il::runtime::findRuntimeSignature(useInstr.ops.front().label) !=
                                       nullptr &&
                                   il::runtime::classifyRuntimeOwnership(useInstr.ops.front().label)
                                       .consumesArg(static_cast<unsigned>(oi - 1))) {
                            ++spends;
                        }
                    }
                }
                for (const auto &edge : useBlock.terminatorEdges) {
                    for (int argId : edge.argIds) {
                        if (argId == instr.resultId)
                            spends += 2; // unknown downstream spending
                    }
                }
            }
            if (spends <= 1)
                strCallRetainElidable_.insert(instr.resultId);
        }
    }
}

/// @brief Hand out the next module-wide unique local-label id.
/// @details Unlike per-function adapter state, this counter is intentionally
///          NOT reset between functions so labels such as
///          @c .Lfptosi_chk_trap_N stay unique across the whole compilation
///          unit's assembly stream.
/// @return Strictly monotonic identifier.
uint32_t LowerILToMIR::nextLocalLabelId() noexcept {
    return nextLocalLabel_++;
}

/// @brief Map an IL value kind to a machine register class for the target.
/// @param kind IL value classification.
/// @return Register class capable of representing the value.
RegClass LowerILToMIR::regClassFor(ILValue::Kind kind) noexcept {
    switch (kind) {
        case ILValue::Kind::I64:
        case ILValue::Kind::I1:
        case ILValue::Kind::PTR:
            return RegClass::GPR;
        case ILValue::Kind::F64:
            return RegClass::XMM;
        case ILValue::Kind::LABEL:
        case ILValue::Kind::STR:
            return RegClass::GPR;
    }
    return RegClass::GPR;
}

/// @brief Ensure an SSA identifier has an assigned virtual register.
/// @details Allocates a new register when necessary and verifies that repeated
///          requests agree on the register class.
/// @param id SSA identifier number.
/// @param kind IL value kind associated with @p id.
/// @return Virtual register descriptor for the identifier.
VReg LowerILToMIR::ensureVReg(int id, ILValue::Kind kind) {
    if (id < 0) {
        phaseAUnsupported("SSA value without identifier");
    }
    const auto it = valueToVReg_.find(id);
    if (it != valueToVReg_.end()) {
        if (it->second.cls != regClassFor(kind)) {
            phaseAUnsupported("SSA id reused with incompatible value type");
        }
        return it->second;
    }
    if (nextVReg_ == std::numeric_limits<std::uint16_t>::max()) {
        phaseAUnsupported("too many virtual registers in function");
    }
    const VReg vreg{nextVReg_++, regClassFor(kind)};
    valueToVReg_.emplace(id, vreg);
    return vreg;
}

/// @brief Allocate a fresh temporary virtual register owned by the adapter.
/// @param cls Register class assigned to the temporary.
/// @return Newly allocated virtual register descriptor.
VReg LowerILToMIR::makeTempVReg(RegClass cls) {
    if (nextVReg_ == std::numeric_limits<std::uint16_t>::max()) {
        phaseAUnsupported("too many virtual registers in function");
    }
    return VReg{nextVReg_++, cls};
}

/// @brief Determine whether an IL value should be emitted as an immediate.
/// @param value IL value candidate.
/// @return True when the value encodes a literal rather than an SSA id.
bool LowerILToMIR::isImmediate(const ILValue &value) const noexcept {
    return value.id < 0;
}

/// @brief Translate an IL label into a machine operand.
/// @param value Label-valued IL operand.
/// @return Machine operand referencing the label target.
Operand LowerILToMIR::makeLabelOperand(const ILValue &value) const {
    if (value.kind != ILValue::Kind::LABEL) {
        phaseAUnsupported("label operand expected");
    }
    if (value.label.empty()) {
        phaseAUnsupported("label operand is empty");
    }
    return x64::makeLabelOperand(value.label);
}

/// @brief Convert an IL value into an operand for the current machine block.
/// @details Handles literals by materialising loads into the provided block and
///          delegates register mapping for SSA identifiers.
/// @param block Machine block receiving any helper instructions.
/// @param value IL value to translate.
/// @param cls Preferred register class when literals must be materialised.
/// @return Machine operand representing the value.
Operand LowerILToMIR::makeOperandForValue(MBasicBlock &block, const ILValue &value, RegClass cls) {
    if (value.kind == ILValue::Kind::LABEL) {
        if (cls != RegClass::GPR) {
            phaseAUnsupported("label operand requested in an XMM context");
        }
        return makeLabelOperand(value);
    }

    if (!isImmediate(value)) {
        // Entry block parameters are now handled at function entry via MOV instructions
        // that copy from physical argument registers to virtual registers. This ensures
        // the values are properly preserved across function calls.
        const VReg vreg = ensureVReg(value.id, value.kind);
        return makeVRegOperand(vreg.cls, vreg.id);
    }

    switch (value.kind) {
        case ILValue::Kind::I64:
        case ILValue::Kind::I1:
        case ILValue::Kind::PTR:
            return makeImmOperand(canonicalIntegerImmediate(value));
        case ILValue::Kind::F64: {
            if (cls != RegClass::XMM) {
                phaseAUnsupported("f64 operand requested in a GPR context");
            }
            if (!roDataPool_) {
                phaseAUnsupported("rodata pool unavailable for f64 literals");
            }
            const int poolIndex = roDataPool_->addF64Literal(value.f64);
            const std::string label = roDataPool_->f64Label(poolIndex);
            const VReg temp = makeTempVReg(RegClass::XMM);
            Operand tempOperand = makeVRegOperand(temp.cls, temp.id);
            const Operand ripOperand = makeRipLabelOperand(label);
            block.append(MInstr::make(MOpcode::MOVSDmr,
                                      std::vector<Operand>{cloneOperand(tempOperand), ripOperand}));
            return tempOperand;
        }
        case ILValue::Kind::STR: {
            if (cls != RegClass::GPR) {
                phaseAUnsupported("string literal requested in an XMM context");
            }
            if (!roDataPool_) {
                phaseAUnsupported("rodata pool unavailable for string literals");
            }
            if (!target_) {
                phaseAUnsupported("target info unavailable for string literal lowering");
            }
            if (value.strLen >
                static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                phaseAUnsupported("string literal byte length is out of range");
            }
            if (value.strLen >
                static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
                phaseAUnsupported("string literal byte length exceeds host size_t");
            }

            std::string literalBytes = value.str;
            const auto requestedLen = static_cast<std::size_t>(value.strLen);
            if (literalBytes.size() != requestedLen) {
                phaseAUnsupported("string literal metadata length mismatch");
            }

            const int poolIndex = roDataPool_->addStringLiteral(std::move(literalBytes));
            const std::string label = roDataPool_->stringLabel(poolIndex);
            const auto literalLen = roDataPool_->stringByteLength(poolIndex);
            if (literalLen > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
                phaseAUnsupported("string literal byte length is out of range");
            }

            // LEA string address into temp vreg
            const Operand ripOperand = makeRipLabelOperand(label);
            const VReg ptrTmp = makeTempVReg(RegClass::GPR);
            const Operand ptrTmpOp = makeVRegOperand(ptrTmp.cls, ptrTmp.id);
            block.append(MInstr::make(MOpcode::LEA,
                                      std::vector<Operand>{cloneOperand(ptrTmpOp), ripOperand}));

            // Create a CallLoweringPlan for rt_str_from_lit(ptr, len)
            // This ensures the call goes through the proper argument lowering mechanism
            CallLoweringPlan plan{};
            plan.callee = "rt_str_from_lit";
            plan.numNamedArgs = 2;

            // First arg: ptr vreg
            CallArg ptrArg{};
            ptrArg.cls = CallArgClass::GPR;
            ptrArg.vreg = ptrTmp.id;
            ptrArg.isImm = false;
            plan.args.push_back(ptrArg);

            // Second arg: length immediate
            CallArg lenArg{};
            lenArg.cls = CallArgClass::GPR;
            lenArg.isImm = true;
            lenArg.imm = static_cast<int64_t>(literalLen);
            plan.args.push_back(lenArg);

            // Record the plan so lowerPendingCalls will set up args properly
            const uint32_t callPlanId = recordCallPlan(std::move(plan));

            // Emit the CALL (argument setup will be inserted by lowerPendingCalls)
            const Operand callTarget = x64::makeLabelOperand(std::string{"rt_str_from_lit"});
            MInstr call = MInstr::make(MOpcode::CALL, std::vector<Operand>{callTarget});
            call.callPlanId = callPlanId;
            block.append(std::move(call));

            // Move result from return register to temp vreg
            const VReg result = makeTempVReg(RegClass::GPR);
            const Operand resultOp = makeVRegOperand(result.cls, result.id);
            const Operand retReg =
                makePhysRegOperand(RegClass::GPR, static_cast<uint16_t>(target_->intReturnReg));
            block.append(
                MInstr::make(MOpcode::MOVrr, std::vector<Operand>{cloneOperand(resultOp), retReg}));
            return resultOp;
        }
        case ILValue::Kind::LABEL:
            break;
    }
    return makeImmOperand(0);
}

/// @brief Look up an operand for @p value without allocating vregs or emitting instructions.
/// @details This mirrors the side-effect-free subset of @ref makeOperandForValue. It is safe to
///          call from speculative optimizations because it never extends @c valueToVReg_, never
///          adds rodata entries, and never appends to a machine block.
/// @param value IL value to inspect.
/// @param cls Expected register class for any existing SSA register operand.
/// @return Operand when representation is already available, otherwise nullopt.
std::optional<Operand> LowerILToMIR::tryGetOperandForValue(const ILValue &value,
                                                           RegClass cls) const {
    if (value.kind == ILValue::Kind::LABEL) {
        if (cls != RegClass::GPR) {
            return std::nullopt;
        }
        return makeLabelOperand(value);
    }

    if (!isImmediate(value)) {
        const auto it = valueToVReg_.find(value.id);
        if (it == valueToVReg_.end()) {
            return std::nullopt;
        }
        if (it->second.cls != regClassFor(value.kind) || it->second.cls != cls) {
            phaseAUnsupported("SSA id reused with incompatible value type");
        }
        return makeVRegOperand(it->second.cls, it->second.id);
    }

    switch (value.kind) {
        case ILValue::Kind::I64:
        case ILValue::Kind::I1:
        case ILValue::Kind::PTR:
            if (cls != RegClass::GPR) {
                return std::nullopt;
            }
            return makeImmOperand(canonicalIntegerImmediate(value));
        case ILValue::Kind::F64:
        case ILValue::Kind::STR:
            return std::nullopt;
        case ILValue::Kind::LABEL:
            break;
    }
    return std::nullopt;
}

/// @brief Materialize block-parameter copies on a dedicated outgoing edge block.
/// @details Conditional and switch branches must only execute block-argument copies on
///          the taken edge. This helper synthesizes a side block containing the required
///          PX_COPY and a final jump to the real successor.
/// @param func Machine function receiving the synthetic edge block.
/// @param edge Successor edge metadata carrying the argument list.
/// @param sourceBlock Source block whose terminator will branch to the helper block.
/// @return Helper block label, or an empty string when the edge can branch directly.
std::string LowerILToMIR::buildEdgeCopyBlock(MFunction &func,
                                             const ILBlock::EdgeArg &edge,
                                             const MBasicBlock &sourceBlock) {
    const auto destIt = blockInfo_.find(edge.to);
    if (destIt == blockInfo_.end()) {
        phaseAUnsupported(("edge references non-existent block: " + edge.to).c_str());
    }
    const auto &params = destIt->second.paramVRegs;
    if (params.empty()) {
        if (!edge.argIds.empty() || !edge.argValues.empty()) {
            phaseAUnsupported("block parameter edge supplies arguments to parameterless block");
        }
        return {};
    }

    if (edge.argIds.size() != params.size()) {
        phaseAUnsupported("block parameter edge arity mismatch");
    }
    if (edge.argValues.size() > edge.argIds.size()) {
        phaseAUnsupported("block parameter edge has extra argument values");
    }
    const std::string destLabel = func.blocks[destIt->second.index].label;

    MBasicBlock edgeBlock{};
    edgeBlock.label = func.makeLocalLabel(sourceBlock.label + ".edge");

    MInstr px = MInstr::make(MOpcode::PX_COPY, {});
    for (std::size_t idx = 0; idx < params.size(); ++idx) {
        Operand srcOp;
        if (edge.argIds[idx] >= 0) {
            auto valIt = valueToVReg_.find(edge.argIds[idx]);
            if (valIt == valueToVReg_.end()) {
                if (idx >= edge.argValues.size() || edge.argValues[idx].id != edge.argIds[idx]) {
                    phaseAUnsupported("missing SSA value for block parameter copy");
                }
                const VReg vreg = ensureVReg(edge.argIds[idx], edge.argValues[idx].kind);
                srcOp = makeVRegOperand(vreg.cls, vreg.id);
            } else {
                srcOp = makeVRegOperand(valIt->second.cls, valIt->second.id);
            }
        } else {
            if (idx >= edge.argValues.size())
                phaseAUnsupported("edge argument index out of bounds in block parameter copy");
            const ILValue &val = edge.argValues[idx];
            const RegClass cls = params[idx].cls;

            if (cls == RegClass::XMM) {
                if (val.kind != ILValue::Kind::F64) {
                    phaseAUnsupported("non-f64 immediate passed to XMM block parameter");
                }
                if (!roDataPool_) {
                    phaseAUnsupported("rodata pool unavailable for f64 block arg");
                }
                const int poolIdx = roDataPool_->addF64Literal(val.f64);
                const std::string label = roDataPool_->f64Label(poolIdx);
                const VReg tmp = makeTempVReg(RegClass::XMM);
                srcOp = makeVRegOperand(tmp.cls, tmp.id);
                edgeBlock.append(MInstr::make(
                    MOpcode::MOVSDmr,
                    std::vector<Operand>{cloneOperand(srcOp), makeRipLabelOperand(label)}));
            } else if (val.kind == ILValue::Kind::STR) {
                srcOp = makeOperandForValue(edgeBlock, val, cls);
            } else {
                const VReg tmp = makeTempVReg(RegClass::GPR);
                srcOp = makeVRegOperand(tmp.cls, tmp.id);
                if (val.kind == ILValue::Kind::LABEL) {
                    edgeBlock.append(MInstr::make(
                        MOpcode::LEA,
                        std::vector<Operand>{cloneOperand(srcOp), makeRipLabelOperand(val.label)}));
                } else {
                    if (!isIntegerLikeKind(val.kind)) {
                        phaseAUnsupported("non-integer immediate passed to GPR block parameter");
                    }
                    edgeBlock.append(MInstr::make(
                        MOpcode::MOVri,
                        std::vector<Operand>{cloneOperand(srcOp),
                                             makeImmOperand(canonicalIntegerImmediate(val))}));
                }
            }
        }

        if (const auto *srcReg = asReg(srcOp); srcReg && srcReg->cls != params[idx].cls) {
            phaseAUnsupported("block parameter edge register class mismatch");
        }

        px.operands.push_back(makeVRegOperand(params[idx].cls, params[idx].id));
        px.operands.push_back(std::move(srcOp));
    }

    if (!px.operands.empty()) {
        edgeBlock.append(std::move(px));
    }
    edgeBlock.append(MInstr::make(MOpcode::JMP, {x64::makeLabelOperand(destLabel)}));

    const std::string helperLabel = edgeBlock.label;
    func.addBlock(std::move(edgeBlock));
    return helperLabel;
}

/// @brief Lower an IL function into machine IR using declarative rules.
/// @details Resets per-function state, creates machine blocks, invokes the rule
///          dispatcher for every instruction, and emits block-parameter copies.
/// @param func IL function to lower.
/// @return Machine function containing the lowered instructions.
MFunction LowerILToMIR::lower(const ILFunction &func) {
    resetFunctionState();
    currentFunctionName_ = func.name;
    computeStrLoadRetainElidable(func);
    // SSA producer table for the null-address guard's root-pointer chase.
    for (const auto &ilBlock : func.blocks)
        for (const auto &ilInstr : ilBlock.instrs)
            if (ilInstr.resultId >= 0)
                ilProducers_[ilInstr.resultId] = &ilInstr;

    MFunction result{};
    result.name = func.name;
    result.metadata.isVarArg = func.isVarArg;

    result.blocks.reserve(func.blocks.size());

    for (std::size_t idx = 0; idx < func.blocks.size(); ++idx) {
        const auto &ilBlock = func.blocks[idx];
        BlockInfo info{};
        info.index = idx;
        info.paramVRegs.reserve(ilBlock.paramIds.size());

        MBasicBlock block{};
        block.label = ".L_" + func.name + "_" + ilBlock.name;

        info.paramVRegs.reserve(ilBlock.paramIds.size());
        for (std::size_t p = 0; p < ilBlock.paramIds.size(); ++p) {
            const int paramId = ilBlock.paramIds[p];
            const auto kind =
                p < ilBlock.paramKinds.size() ? ilBlock.paramKinds[p] : ILValue::Kind::I64;
            if (paramId >= 0) {
                info.paramVRegs.push_back(ensureVReg(paramId, kind));
            }
        }

        blockInfo_[ilBlock.name] = info;
        result.addBlock(std::move(block));
    }

    const auto &rules = zanna_get_lowering_rules();
    (void)rules; // keep static initialisation local to this TU.

    // Build a map from entry block parameter IDs to their ABI physical registers
    // or stack offsets for stack-passed parameters
    /// @brief Entry parameter that must be loaded from the caller's stack area.
    struct StackParam {
        /// IL SSA identifier receiving the parameter.
        int paramId;
        /// Positive RBP-relative incoming argument displacement.
        int32_t offset;
        /// IL kind controlling load class and boolean normalization.
        ILValue::Kind kind;
    };

    std::unordered_map<int, Operand> entryParamToPhysReg{};
    std::vector<StackParam> stackParams{};
    if (!func.blocks.empty() && !func.blocks[0].paramIds.empty()) {
        const auto &entryParams = func.blocks[0];
        std::vector<CallArgClass> paramClasses;
        paramClasses.reserve(entryParams.paramIds.size());
        for (std::size_t p = 0; p < entryParams.paramIds.size(); ++p) {
            const auto kind = (p < entryParams.paramKinds.size()) ? entryParams.paramKinds[p]
                                                                  : ILValue::Kind::I64;
            paramClasses.push_back(regClassFor(kind) == RegClass::XMM ? CallArgClass::FPR
                                                                      : CallArgClass::GPR);
        }
        const CallArgLayout layout = common::planParamClasses(
            paramClasses,
            CallArgLayoutConfig{.maxGPRArgs = target_->maxGPRArgs,
                                .maxFPRArgs = target_->maxFPArgs,
                                .slotModel = target_->shadowSpace != 0
                                                 ? CallSlotModel::UnifiedRegisterPositions
                                                 : CallSlotModel::IndependentRegisterBanks,
                                .variadicTailOnStack = func.isVarArg,
                                .numNamedArgs = paramClasses.size()});
        for (std::size_t p = 0; p < entryParams.paramIds.size(); ++p) {
            const int paramId = entryParams.paramIds[p];
            const auto kind = (p < entryParams.paramKinds.size()) ? entryParams.paramKinds[p]
                                                                  : ILValue::Kind::I64;
            if (paramId < 0) {
                continue;
            }
            const auto &loc = layout.locations[p];
            if (loc.inRegister) {
                if (loc.cls == CallArgClass::FPR) {
                    const PhysReg argReg = target_->f64ArgOrder[loc.regIndex];
                    entryParamToPhysReg[paramId] =
                        makePhysRegOperand(RegClass::XMM, static_cast<uint16_t>(argReg));
                } else {
                    const PhysReg argReg = target_->intArgOrder[loc.regIndex];
                    entryParamToPhysReg[paramId] =
                        makePhysRegOperand(RegClass::GPR, static_cast<uint16_t>(argReg));
                }
            } else {
                const int32_t offset = static_cast<int32_t>(target_->shadowSpace) + 16 +
                                       static_cast<int32_t>(loc.stackSlotIndex * 8);
                stackParams.push_back({paramId, offset, kind});
            }
        }
    }
    // NOTE: We intentionally do NOT set entryParamToPhysReg_ here.
    // Entry parameters arrive in caller-saved registers (RCX, RDX, R8, R9 on Windows;
    // RDI, RSI, RDX, RCX, R8, R9 on SysV). If we return these physical registers
    // directly in makeOperandForValue(), they will be clobbered by any function
    // call before the parameter is used. Instead, we emit MOV instructions at
    // function entry to copy parameters from physical registers to virtual registers,
    // which the register allocator will properly manage (spilling across calls).

    // Emit a single PX_COPY for all register-passed parameters at function entry.
    // Using PX_COPY instead of individual MOVs ensures the coalescer resolves
    // the parallel move correctly (topological sort + cycle-breaking), avoiding
    // write-before-read hazards when physical register destinations alias sources.
    if (!entryParamToPhysReg.empty() && !result.blocks.empty()) {
        auto &entryBlock = result.blocks[0];
        MInstr px = MInstr::make(MOpcode::PX_COPY, {});
        std::vector<MInstr> i1Normalizations{};

        // Iterate in parameter declaration order (deterministic), not map order.
        const auto &entryParams = func.blocks[0];
        for (std::size_t p = 0; p < entryParams.paramIds.size(); ++p) {
            const int paramId = entryParams.paramIds[p];
            auto it = entryParamToPhysReg.find(paramId);
            if (it == entryParamToPhysReg.end())
                continue; // stack-passed param, handled separately

            ILValue::Kind kind = (p < entryParams.paramKinds.size()) ? entryParams.paramKinds[p]
                                                                     : ILValue::Kind::I64;

            const VReg vreg = ensureVReg(paramId, kind);
            const Operand dest = makeVRegOperand(vreg.cls, vreg.id);
            px.operands.push_back(dest);
            px.operands.push_back(it->second);
            if (kind == ILValue::Kind::I1) {
                i1Normalizations.push_back(MInstr::make(
                    MOpcode::ANDri, {makeVRegOperand(vreg.cls, vreg.id), makeImmOperand(1)}));
            }
        }

        if (!px.operands.empty()) {
            entryBlock.instructions.insert(entryBlock.instructions.begin(), std::move(px));
            entryBlock.instructions.insert(entryBlock.instructions.begin() + 1,
                                           i1Normalizations.begin(),
                                           i1Normalizations.end());
        }
    }

    // Emit loads for stack-passed parameters at the start of the entry block
    if (!stackParams.empty() && !result.blocks.empty()) {
        auto &entryBlock = result.blocks[0];
        for (const auto &sp : stackParams) {
            // Create a vreg for this parameter and emit a load from stack
            const VReg vreg = ensureVReg(sp.paramId, sp.kind);
            const Operand dest = makeVRegOperand(vreg.cls, vreg.id);
            const Operand src = makeMemOperand(makePhysBase(PhysReg::RBP), sp.offset);
            // Emit MOVmr to load from stack into vreg
            if (vreg.cls == RegClass::XMM) {
                entryBlock.instructions.push_back(MInstr::make(MOpcode::MOVSDmr, {dest, src}));
            } else {
                entryBlock.instructions.push_back(MInstr::make(MOpcode::MOVmr, {dest, src}));
                if (sp.kind == ILValue::Kind::I1) {
                    entryBlock.instructions.push_back(MInstr::make(
                        MOpcode::ANDri, {makeVRegOperand(vreg.cls, vreg.id), makeImmOperand(1)}));
                }
            }
        }
    }

    for (std::size_t idx = 0; idx < func.blocks.size(); ++idx) {
        const auto &ilBlock = func.blocks[idx];

        for (std::size_t instrIdx = 0; instrIdx < ilBlock.instrs.size(); ++instrIdx) {
            ILInstr loweredInstr = ilBlock.instrs[instrIdx];

            if (loweredInstr.opcode == "br" || loweredInstr.opcode == "cbr" ||
                loweredInstr.opcode == "switch_i32") {
                /**
                 * @brief Redirect one terminator label through its edge-copy block.
                 *
                 * @param labelValue Mutable label operand in the cloned terminator.
                 * @param edgeIndex Matching successor metadata index.
                 */
                auto rewriteLabelOperand = [&](ILValue &labelValue, std::size_t edgeIndex) {
                    if (edgeIndex >= ilBlock.terminatorEdges.size() ||
                        labelValue.kind != ILValue::Kind::LABEL) {
                        return;
                    }
                    const std::string helperLabel = buildEdgeCopyBlock(
                        result, ilBlock.terminatorEdges[edgeIndex], result.blocks[idx]);
                    if (!helperLabel.empty()) {
                        labelValue.label = helperLabel;
                    }
                };

                if (loweredInstr.opcode == "br") {
                    if (!loweredInstr.ops.empty()) {
                        rewriteLabelOperand(loweredInstr.ops[0], 0);
                    }
                } else if (loweredInstr.opcode == "cbr") {
                    std::size_t edgeIndex = 0;
                    for (std::size_t opIndex = 0; opIndex < loweredInstr.ops.size(); ++opIndex) {
                        if (loweredInstr.ops[opIndex].kind == ILValue::Kind::LABEL) {
                            rewriteLabelOperand(loweredInstr.ops[opIndex], edgeIndex++);
                        }
                    }
                } else {
                    // Switch operands are emitted as (scrutinee, case-value/case-label pairs,
                    // default-label), while IL successor metadata stores default first and then
                    // the cases. Rewrite each label with the edge that corresponds to that
                    // destination instead of walking labels in operand order.
                    std::size_t caseEdgeIndex = 1;
                    for (std::size_t opIndex = 1; opIndex < loweredInstr.ops.size(); ++opIndex) {
                        if (loweredInstr.ops[opIndex].kind != ILValue::Kind::LABEL) {
                            continue;
                        }
                        const bool isDefaultLabel = opIndex + 1 == loweredInstr.ops.size();
                        rewriteLabelOperand(loweredInstr.ops[opIndex],
                                            isDefaultLabel ? 0 : caseEdgeIndex++);
                    }
                }
            }

            auto &mirBlock = result.blocks[idx];
            MIRBuilder builder{*this, mirBlock};
            builder.setCurrentLoc(loweredInstr.loc);

            const LoweringRule *rule = zanna_select_rule(loweredInstr);
            if (!rule) {
                reportNoRule(loweredInstr);
            }
            rule->emit(loweredInstr, builder);
        }
    }

    // Materialise the shared null trap block when any load/store guard
    // referenced it (see EmitCommon::emitNullAddressGuard). Appending after
    // the main loop keeps block references stable during lowering; the
    // downstream passes treat it like the div0/ovf trap blocks.
    if (nullTrapRequested_) {
        MBasicBlock trapBlock{};
        trapBlock.label = ".Ltrap_null_" + result.name;
        trapBlock.append(MInstr::make(
            MOpcode::CALL, std::vector<Operand>{x64::makeLabelOperand("rt_trap_null")}));
        trapBlock.append(MInstr::make(MOpcode::UD2));
        result.addBlock(std::move(trapBlock));
    }

    return result;
}

} // namespace zanna::codegen::x64
