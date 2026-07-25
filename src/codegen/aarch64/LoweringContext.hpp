//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/LoweringContext.hpp
// Purpose: Shared state and helpers for IL->MIR lowering on AArch64.
// Key invariants:
//   - Context references are valid for the duration of a single lowerFunction().
//   - Maps are populated incrementally as instructions are lowered.
//   - Cross-block temps are spilled to frame slots before successor blocks.
// Ownership/Lifetime:
//   - LoweringContext holds non-owning references; caller owns all state.
// Links: src/codegen/aarch64/LowerILToMIR.hpp,
//        src/codegen/aarch64/InstrLowering.hpp,
//        src/codegen/aarch64/FrameBuilder.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "FrameBuilder.hpp"
#include "MachineIR.hpp"
#include "TargetAArch64.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/OpcodeInfo.hpp"

#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

/**
 * @file
 * @brief Defines the borrowed state bundle and common helpers used by AArch64 IL-to-MIR lowering.
 *
 * This header centralizes the virtual-register namespaces, cross-block spill
 * keys, deferred trap requests, and lookup utilities shared by instruction
 * and terminator lowering. `LoweringContext` does not own any referenced
 * object; it is valid only while the surrounding `lowerFunction()` invocation
 * and all of its state remain alive.
 */

namespace zanna::codegen::aarch64 {

/// @brief First ID available for ordinary lowering-created virtual registers.
/// @details Ordinary IDs occupy `[kFirstVirtualRegId, kPhiVRegStart)`.
inline constexpr uint16_t kFirstVirtualRegId = 1;
/// @brief First ID reserved for phi-parameter virtual registers.
/// @details Phi IDs occupy `[kPhiVRegStart, kCrossBlockSpillKeyStart)`.
inline constexpr uint16_t kPhiVRegStart = 40000;
/// @brief First `FrameBuilder` key reserved for cross-block temporary spills.
/// @details Adding an IL temp ID to this base keeps those keys disjoint from
///          ordinary and phi virtual-register IDs.
inline constexpr uint32_t kCrossBlockSpillKeyStart = 50000;

/**
 * @brief Allocates the next ordinary virtual-register ID.
 *
 * @param[in,out] nextVRegId Next candidate ID; advanced by one on success.
 * @return The ID held in @p nextVRegId on entry.
 * @throws std::runtime_error If the candidate has reached the phi-reserved range.
 * @post On success, @p nextVRegId is one greater than the returned ID.
 */
inline uint16_t allocateNextVReg(uint16_t &nextVRegId) {
    if (nextVRegId >= kPhiVRegStart)
        throw std::runtime_error(
            "AArch64 lowering: virtual register space exhausted before phi spill range");
    return nextVRegId++;
}

/**
 * @brief Allocates the next phi-parameter virtual-register ID.
 *
 * @param[in,out] phiNextId Next candidate in the phi-reserved ID range.
 * @return The candidate ID supplied on entry.
 * @throws std::runtime_error If allocation would enter the cross-block spill-key range.
 * @post On success, @p phiNextId is one greater than the returned ID.
 */
inline uint16_t allocatePhiVReg(uint16_t &phiNextId) {
    if (phiNextId >= kCrossBlockSpillKeyStart)
        throw std::runtime_error(
            "AArch64 lowering: phi virtual register space exhausted before spill-key range");
    return phiNextId++;
}

/**
 * @brief Maps a cross-block IL temporary to its reserved `FrameBuilder` spill key.
 *
 * @param tempId Function-local IL temporary ID.
 * @return `kCrossBlockSpillKeyStart + tempId`.
 * @throws std::runtime_error If the sum is not representable as `uint32_t`.
 */
inline uint32_t spillKeyForCrossBlockTemp(unsigned tempId) {
    if (tempId > (std::numeric_limits<uint32_t>::max)() - kCrossBlockSpillKeyStart)
        throw std::runtime_error("AArch64 lowering: cross-block spill key overflow");
    return kCrossBlockSpillKeyStart + tempId;
}

/**
 * @brief Allocates or retrieves the frame spill slot for a cross-block IL temporary.
 *
 * All entry saves, liveness allocation, and cross-block reloads use this
 * mapping so a given IL temporary has one stable slot within the function.
 *
 * @param[in,out] fb Frame allocator that owns the key-to-slot mapping.
 * @param tempId Function-local IL temporary ID.
 * @return Frame-pointer-relative offset of the temporary's spill slot.
 * @throws std::runtime_error If conversion to the reserved spill key overflows.
 */
inline int ensureCrossBlockSpill(FrameBuilder &fb, unsigned tempId) {
    return fb.ensureSpill(spillKeyForCrossBlockTemp(tempId));
}

/**
 * @brief Ensures that a materialized scalar operand is available in an FPR.
 *
 * An operand already classified as `FPR` is returned unchanged. A `GPR`
 * operand causes an `SCvtF` instruction to be appended and the caller's class
 * and virtual-register ID to be updated to the converted value.
 *
 * @param vreg Existing scalar virtual-register ID.
 * @param[in,out] cls Existing register class; set to `FPR` after conversion.
 * @param[in,out] nextVRegId Ordinary virtual-register allocator state.
 * @param[in,out] out Block that receives a conversion instruction when needed.
 * @return @p vreg for an existing FPR, or the newly allocated FPR ID.
 * @throws std::runtime_error If a conversion is needed but the ordinary
 *         virtual-register range is exhausted.
 */
inline uint16_t coerceScalarOperandToFpr(uint16_t vreg,
                                         RegClass &cls,
                                         uint16_t &nextVRegId,
                                         MBasicBlock &out) {
    if (cls != RegClass::GPR)
        return vreg;

    const uint16_t converted = allocateNextVReg(nextVRegId);
    out.instrs.push_back(MInstr{
        MOpcode::SCvtF,
        {MOperand::vregOp(RegClass::FPR, converted), MOperand::vregOp(RegClass::GPR, vreg)}});
    cls = RegClass::FPR;
    return converted;
}

/**
 * @brief Describes a deferred request for a per-function shared trap block.
 *
 * Trap-guard lowering registers the trap kinds it branches to; blocks are
 * materialized only after instruction and terminator lowering has finished.
 * Deferral prevents opcode handlers from growing `MFunction::blocks` while
 * references into that vector are live, and it limits each trap kind to one
 * block per function.
 */
struct TrapBlockRequest {
    std::string label{};  ///< Block label the guard branches to.
    std::string callee{}; ///< Non-empty: body is `bl <callee>` (no-return).
    int raiseCode{0};     ///< Used when @ref callee is empty: body raises
                          ///< rt_trap_raise_error with this error code.
};

/**
 * @brief Bundles the mutable and read-only state used during IL-to-MIR lowering.
 *
 * The context is passed to opcode handlers to keep their interfaces uniform.
 * Every reference aliases state owned by the surrounding lowering operation;
 * copying a context copies those aliases rather than the referenced state.
 *
 * @invariant `mf`, `fb`, all maps, and both register-ID counters describe the
 *            same function as `fn`.
 * @invariant All referenced objects outlive the context.
 * @warning The context is not thread-safe and must remain confined to one
 *          function-lowering operation.
 */
struct LoweringContext {
    /// @brief IL function currently being lowered.
    const il::core::Function &fn;

    /// @brief ABI and register information for the AArch64 target.
    const TargetInfo &ti;

    /// @brief Frame builder for stack slot allocation and layout.
    FrameBuilder &fb;

    /// @brief Output MIR function being constructed during lowering.
    MFunction &mf;

    /// @brief Monotonically increasing counter for minting virtual register IDs.
    uint16_t &nextVRegId;

    /// @brief Maps IL temp IDs to allocated virtual register IDs (function-wide).
    std::unordered_map<unsigned, uint16_t> &tempVReg;

    /// @brief Maps IL temp IDs to their register class (GPR or FPR).
    std::unordered_map<unsigned, RegClass> &tempRegClass;

    /// @brief Maps block labels to the vreg IDs assigned to their phi parameters.
    std::unordered_map<std::string, std::vector<uint16_t>> &phiVregId;

    /// @brief Maps block labels to the register classes of their phi parameters.
    std::unordered_map<std::string, std::vector<RegClass>> &phiRegClass;

    /// @brief Maps block labels to spill slot offsets for their phi parameters.
    std::unordered_map<std::string, std::vector<int>> &phiSpillOffset;

    /// @brief Maps cross-block temp IDs to their allocated spill slot offsets.
    std::unordered_map<unsigned, int> &crossBlockSpillOffset;

    /// @brief Maps temp IDs to the index of the basic block that defines them.
    std::unordered_map<unsigned, std::size_t> &tempDefBlock;

    /// @brief Set of temp IDs whose values are live across block boundaries.
    std::unordered_set<unsigned> &crossBlockTemps;

    /// @brief Optional map from IL global string names to their byte lengths.
    const std::unordered_map<std::string, std::size_t> *stringLiteralByteLengths = nullptr;

    /// @brief Optional map from direct callee names to their named-argument counts.
    const std::unordered_map<std::string, std::size_t> *knownVarArgNamedArgCounts = nullptr;

    /// @brief Per-function shared trap-block requests, keyed by trap kind.
    /// @details Materialised at the end of lowerFunction; see TrapBlockRequest.
    std::unordered_map<std::string, TrapBlockRequest> &sharedTrapBlocks;

    /// @brief Construct a lowering context with every borrowed state object bound explicitly.
    /// @details The context stores references into the surrounding lowering pass and must never
    ///          outlive that pass invocation. Using an explicit constructor keeps the long member
    ///          list checked in one place and prevents accidental default construction of required
    ///          reference state.
    /// @param function IL function currently being lowered.
    /// @param targetInfo ABI and register information for the AArch64 target.
    /// @param frameBuilder Frame-layout allocator used while lowering.
    /// @param machineFunction Output MIR function being built.
    /// @param nextVirtualRegId Counter used to allocate new virtual registers.
    /// @param tempVirtualRegs Function-wide mapping from IL temp IDs to vreg IDs.
    /// @param tempClasses Function-wide mapping from IL temp IDs to register classes.
    /// @param phiVirtualRegs Phi-parameter vreg IDs by block label.
    /// @param phiClasses Phi-parameter register classes by block label.
    /// @param phiSpillOffsets Phi spill-slot offsets by block label.
    /// @param crossBlockSpillOffsets Spill slots for temps live across blocks.
    /// @param tempDefinitionBlocks Basic-block index that defines each temp.
    /// @param crossBlockLiveTemps Temps proven live across basic blocks.
    /// @param stringLiteralLengths Optional global string literal byte-length table.
    /// @param varArgNamedArgCounts Optional direct-callee named-argument count table.
    /// @param trapBlockRequests Per-function shared trap-block request registry.
    LoweringContext(const il::core::Function &function,
                    const TargetInfo &targetInfo,
                    FrameBuilder &frameBuilder,
                    MFunction &machineFunction,
                    uint16_t &nextVirtualRegId,
                    std::unordered_map<unsigned, uint16_t> &tempVirtualRegs,
                    std::unordered_map<unsigned, RegClass> &tempClasses,
                    std::unordered_map<std::string, std::vector<uint16_t>> &phiVirtualRegs,
                    std::unordered_map<std::string, std::vector<RegClass>> &phiClasses,
                    std::unordered_map<std::string, std::vector<int>> &phiSpillOffsets,
                    std::unordered_map<unsigned, int> &crossBlockSpillOffsets,
                    std::unordered_map<unsigned, std::size_t> &tempDefinitionBlocks,
                    std::unordered_set<unsigned> &crossBlockLiveTemps,
                    const std::unordered_map<std::string, std::size_t> *stringLiteralLengths,
                    const std::unordered_map<std::string, std::size_t> *varArgNamedArgCounts,
                    std::unordered_map<std::string, TrapBlockRequest> &trapBlockRequests)
        : fn(function), ti(targetInfo), fb(frameBuilder), mf(machineFunction),
          nextVRegId(nextVirtualRegId), tempVReg(tempVirtualRegs), tempRegClass(tempClasses),
          phiVregId(phiVirtualRegs), phiRegClass(phiClasses), phiSpillOffset(phiSpillOffsets),
          crossBlockSpillOffset(crossBlockSpillOffsets), tempDefBlock(tempDefinitionBlocks),
          crossBlockTemps(crossBlockLiveTemps), stringLiteralByteLengths(stringLiteralLengths),
          knownVarArgNamedArgCounts(varArgNamedArgCounts), sharedTrapBlocks(trapBlockRequests) {}

    /**
     * @brief Retrieves a mutable output block by index.
     *
     * @param idx Zero-based index into `mf.blocks`.
     * @return Reference to the corresponding MIR block.
     * @pre `idx < mf.blocks.size()`.
     * @warning The reference may be invalidated if `mf.blocks` subsequently grows.
     */
    MBasicBlock &bbOut(std::size_t idx) {
        return mf.blocks[idx];
    }
};

/**
 * @brief Registers or looks up the per-function shared trap block for a kind.
 *
 * The function returns the label that a guard should target but does not
 * mutate `MFunction::blocks`. The first request for a kind fixes that kind's
 * callee and raise code; later requests with the same key reuse it.
 *
 * @param[in,out] ctx Active lowering context and request registry.
 * @param kind Non-null, stable trap-kind key such as `"div0"` or `"bounds"`.
 * @param callee Non-returning runtime entry point for a `bl` body, or `nullptr`
 *        to request an `rt_trap_raise_error` body.
 * @param raiseCode Error code used only when @p callee is `nullptr`.
 * @return Reference to the stored label for @p kind.
 * @pre @p kind points to a null-terminated string.
 * @warning Erasing the corresponding request invalidates the returned reference.
 */
inline const std::string &requestSharedTrapBlock(LoweringContext &ctx,
                                                 const char *kind,
                                                 const char *callee,
                                                 int raiseCode = 0) {
    auto it = ctx.sharedTrapBlocks.find(kind);
    if (it == ctx.sharedTrapBlocks.end()) {
        TrapBlockRequest request{};
        request.label = std::string(".Ltrap_") + kind;
        request.callee = callee != nullptr ? callee : "";
        request.raiseCode = raiseCode;
        it = ctx.sharedTrapBlocks.emplace(kind, std::move(request)).first;
    }
    return it->second.label;
}

/**
 * @brief Finds a block parameter by IL temporary ID.
 *
 * @param bb Basic block whose parameter list is searched in declaration order.
 * @param tempId IL temporary ID to locate.
 * @return Zero-based parameter index, or `-1` if no parameter has @p tempId.
 */
inline int indexOfParam(const il::core::BasicBlock &bb, unsigned tempId) {
    for (size_t i = 0; i < bb.params.size(); ++i)
        if (bb.params[i].id == tempId)
            return static_cast<int>(i);
    return -1;
}

/**
 * @brief Finds the instruction that defines an IL temporary.
 *
 * Blocks and instructions are searched in storage order, and the first
 * matching result is returned.
 *
 * @param fn IL function to search without modification.
 * @param tempId Result temporary whose producer is requested.
 * @return Pointer into @p fn, or `nullptr` when no instruction defines
 *         @p tempId.
 * @warning The returned pointer is invalidated by mutations that relocate the
 *          containing block's instruction vector.
 */
inline const il::core::Instr *findProducerInFunction(const il::core::Function &fn,
                                                     unsigned tempId) {
    for (const auto &bb : fn.blocks) {
        for (const auto &ins : bb.instructions) {
            if (ins.result && *ins.result == tempId)
                return &ins;
        }
    }
    return nullptr;
}

/**
 * @brief Tests whether a basic block contains an observably side-effecting instruction.
 *
 * Control-flow terminators are deliberately ignored. Other opcodes count as
 * side-effecting when their opcode metadata says so or when they report any
 * memory effect.
 *
 * @param bb Basic block to inspect without modification.
 * @return `true` on the first non-terminator with side or memory effects;
 *         otherwise `false`.
 */
inline bool hasSideEffects(const il::core::BasicBlock &bb) {
    for (const auto &ins : bb.instructions) {
        switch (ins.op) {
            case il::core::Opcode::Ret:
            case il::core::Opcode::Br:
            case il::core::Opcode::CBr:
                continue;
            default:
                break;
        }
        if (il::core::getOpcodeInfo(ins.op).hasSideEffects)
            return true;
        if (il::core::memoryEffects(ins.op) != il::core::MemoryEffects::None)
            return true;
    }
    return false;
}

/**
 * @brief Holds the three ordered phases of one lowered call sequence.
 *
 * Prefix instructions materialize and marshal arguments, `call` performs the
 * branch-with-link, and postfix instructions restore transient call state.
 * Consumers must emit the three members in that order.
 */
struct LoweredCall {
    std::vector<MInstr> prefix;  ///< Argument materialisation and marshalling instructions.
    MInstr call;                 ///< The BL (branch-with-link) callee instruction.
    std::vector<MInstr> postfix; ///< Post-call clean-up (e.g. stack restore).
};

} // namespace zanna::codegen::aarch64
