//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/Backend.cpp
// Purpose: Top-level x86-64 backend facade that sequences the Phase A pipeline.
//          Orchestrates IL→MIR lowering, register allocation, frame layout,
//          peephole optimisations, and final assembly/binary emission.
// Key invariants:
//   - Each IL function is lowered independently to keep pass interactions simple.
//   - Module function order is preserved throughout the pipeline.
//   - Unsupported configuration options are surfaced as diagnostic warnings.
// Ownership/Lifetime:
//   - Borrows caller-provided IL modules and TargetInfo; all MIR state is
//     stack-local and discarded after emission.
// Links: src/codegen/x86_64/Backend.hpp,
//        src/codegen/x86_64/LowerILToMIR.hpp,
//        src/codegen/x86_64/AsmEmitter.hpp,
//        src/codegen/x86_64/passes/PassManager.hpp
//
//===----------------------------------------------------------------------===//

#include "Backend.hpp"

#include "AsmEmitter.hpp"
#include "CallLowering.hpp"
#include "FrameLowering.hpp"
#include "ISel.hpp"
#include "Peephole.hpp"
#include "RegAllocLinear.hpp"
#include "Scheduler.hpp"
#include "TargetX64.hpp"
#include "binenc/X64BinaryEncoder.hpp"
#include "codegen/common/Parallelism.hpp"
#include "codegen/common/ScalarBits.hpp"
#include "codegen/common/objfile/DebugLineTable.hpp"
#include "peephole/PeepholeCommon.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

/**
 * @file
 * @brief Implements staged and convenience entry points for x86-64 code generation.
 *
 * This translation unit coordinates IL-to-MIR lowering, ABI call expansion,
 * instruction legalization, register allocation, frame lowering, scheduling,
 * peephole optimization, and assembly or direct-binary emission. Independent
 * functions are processed concurrently where deterministic output and shared
 * state permit it.
 */

namespace zanna::codegen::x64 {

/// @brief Lower signed division and remainder pseudos into guarded IDIV sequences.
/// @details Declared here and implemented in @ref LowerDiv.cpp so the backend
///          facade can invoke the pass without introducing additional headers.
/// @param fn Machine function to rewrite in place.
void lowerSignedDivRem(MFunction &fn);

/// @brief Lower overflow-checked arithmetic pseudos into guarded sequences.
/// @details Declared here and implemented in @ref LowerOvf.cpp.
/// @param fn Machine function to rewrite in place.
void lowerOverflowOps(MFunction &fn);

namespace {

/// @brief Map a target-platform enum to its native object-file format.
/// @details Darwin → Mach-O, Linux → ELF, Windows → COFF; `Host` falls back to
///          the compile-time host-format detection.
/// @param platform Platform policy requested by the caller.
/// @return Object format associated with @p platform.
[[nodiscard]] objfile::ObjFormat targetObjectFormat(CodegenOptions::TargetPlatform platform) {
    switch (platform) {
        case CodegenOptions::TargetPlatform::Darwin:
            return objfile::ObjFormat::MachO;
        case CodegenOptions::TargetPlatform::Linux:
            return objfile::ObjFormat::ELF;
        case CodegenOptions::TargetPlatform::Windows:
            return objfile::ObjFormat::COFF;
        case CodegenOptions::TargetPlatform::Host:
            return objfile::detectHostFormat();
    }
    return objfile::detectHostFormat();
}

/// @brief Return true when `ZANNA_X64_BINARY_TRACE` is set in the environment.
/// @details Cached once at first call; controls verbose stderr tracing during
///          binary emission for debug builds.
/// @return @c true when per-function timing and size traces should be written.
[[nodiscard]] bool traceX64BinaryEmit() {
    static const bool enabled = std::getenv("ZANNA_X64_BINARY_TRACE") != nullptr;
    return enabled;
}

/// @brief Count the total number of MIR instructions across all blocks in @p fn.
/// @param fn Machine function to inspect.
/// @return Sum of the instruction-vector sizes of all basic blocks.
std::size_t mirInstructionCount(const MFunction &fn) {
    std::size_t count = 0;
    for (const auto &block : fn.blocks)
        count += block.instructions.size();
    return count;
}

/// @brief Return the label name if @p instr is a LABEL pseudo, else nullopt.
/// @param instr Instruction whose first operand is inspected.
/// @return A copy of the defined label, or @c std::nullopt for a non-label or
///         malformed label pseudo.
[[nodiscard]] std::optional<std::string> labelDefinedBy(const MInstr &instr) {
    if (instr.opcode != MOpcode::LABEL || instr.operands.empty()) {
        return std::nullopt;
    }
    if (const auto *label = std::get_if<OpLabel>(&instr.operands.front())) {
        return label->name;
    }
    return std::nullopt;
}

/// @brief Return true if @p opcode ends sequential control flow within a block.
/// @details Used by `splitInternalLabelBlocks` to start a new MBasicBlock after
///          any in-block JMP/JCC/RET/UD2.
/// @param opcode Opcode to classify.
/// @return @c true for a branch, return, jump-table dispatch, or trap terminator.
[[nodiscard]] bool isControlTerminatorForSplit(MOpcode opcode) noexcept {
    return opcode == MOpcode::JMP || opcode == MOpcode::JCC || opcode == MOpcode::RET ||
           opcode == MOpcode::JUMPTABLE || opcode == MOpcode::UD2;
}

/// @brief Return true if @p label starts with the synthetic ".Lsplit" prefix
///        used by `splitInternalLabelBlocks` for fresh fall-through blocks.
/// @param label Label text to inspect.
/// @return @c true when @p label belongs to this splitter's synthetic namespace.
[[nodiscard]] bool isSyntheticSplitLabel(std::string_view label) noexcept {
    return label.rfind(".Lsplit", 0) == 0;
}

/// @brief Promote in-block labels to real MachineIR basic blocks.
///
/// @details Several lowering rules emit local labels for cold trap paths before
///          register allocation.  The allocator and liveness analysis are
///          block-CFG based; leaving those labels inside one block lets
///          trap-only moves and calls pollute the normal path's register cache.
///          Splitting here preserves layout while making those edges visible.
///          It also starts a fresh block after in-block control transfers so
///          branch-arm instructions are not hidden behind an earlier JCC.
///
/// @param fn Function whose blocks and instruction storage are rewritten in place.
void splitInternalLabelBlocks(MFunction &fn) {
    bool needsSplit = false;
    std::size_t labelCount = 0;
    for (const auto &block : fn.blocks) {
        for (std::size_t idx = 0; idx < block.instructions.size(); ++idx) {
            const auto &instr = block.instructions[idx];
            if (labelDefinedBy(instr)) {
                needsSplit = true;
                ++labelCount;
            }
            if (isControlTerminatorForSplit(instr.opcode) && idx + 1 < block.instructions.size()) {
                needsSplit = true;
            }
        }
    }
    if (!needsSplit) {
        return;
    }

    std::vector<MBasicBlock> splitBlocks;
    splitBlocks.reserve(fn.blocks.size() + labelCount);

    for (auto &block : fn.blocks) {
        MBasicBlock current{};
        current.label = std::move(block.label);

        for (std::size_t idx = 0; idx < block.instructions.size(); ++idx) {
            auto &instr = block.instructions[idx];
            if (auto label = labelDefinedBy(instr)) {
                if (current.instructions.empty()) {
                    if (current.label.empty() || current.label == *label ||
                        isSyntheticSplitLabel(current.label)) {
                        current.label = std::move(*label);
                        continue;
                    }
                    current.instructions.push_back(std::move(instr));
                    continue;
                }
                splitBlocks.push_back(std::move(current));
                current = MBasicBlock{};
                current.label = std::move(*label);
                continue;
            }
            current.instructions.push_back(std::move(instr));

            if (isControlTerminatorForSplit(current.instructions.back().opcode) &&
                idx + 1 < block.instructions.size()) {
                const MOpcode terminator = current.instructions.back().opcode;
                std::string nextLabel;
                if (auto label = labelDefinedBy(block.instructions[idx + 1])) {
                    nextLabel = *label;
                } else {
                    nextLabel = fn.makeLocalLabel(".Lsplit");
                }
                if (terminator == MOpcode::JCC) {
                    current.instructions.push_back(
                        MInstr::make(MOpcode::JMP, {makeLabelOperand(nextLabel)}));
                }
                splitBlocks.push_back(std::move(current));
                current = MBasicBlock{};
                current.label = std::move(nextLabel);
            }
        }

        splitBlocks.push_back(std::move(current));
    }

    fn.blocks = std::move(splitBlocks);
}

/// @brief Emit a warning message when unsupported syntax options are requested.
///
/// @details Phase A only supports AT&T syntax emission.  When callers request
///          Intel syntax this helper returns a diagnostic string so the backend
///          can surface the limitation without aborting code generation.
///
/// @param options Code-generation options supplied by the caller.
/// @return Warning string when unsupported options were set; empty view otherwise.
[[nodiscard]] std::string_view syntaxWarning(const CodegenOptions &options) noexcept {
    if (!options.atandtSyntax) {
        return "Phase A: only AT&T syntax emission is implemented.\n";
    }
    return std::string_view{};
}

/// @brief Lower pending call plans onto their corresponding CALL instructions.
///
/// @details Iterates over the machine function's basic blocks, matching each
///          placeholder CALL emitted during IL lowering with its associated
///          @ref CallLoweringPlan. For every match the helper invokes
///          @ref lowerCall to materialise argument moves and update the frame
///          summary with any required outgoing stack space. The traversal skips
///          over the CALL that triggered lowering to avoid reprocessing it once
///          the preparation sequence has been inserted.
///
/// @param func Machine function containing placeholder CALL instructions.
/// @param plans Ordered sequence of call plans captured during IL lowering.
/// @param target Target description providing ABI register order.
/// @param frame Mutable frame summary updated with outgoing argument usage.
/// @throws std::runtime_error If a CALL references an invalid plan or any plan
///         remains unmatched after traversal.
void lowerPendingCalls(MFunction &func,
                       const std::vector<CallLoweringPlan> &plans,
                       const TargetInfo &target,
                       FrameInfo &frame) {
    std::vector<bool> consumed(plans.size(), false);
    for (auto &block : func.blocks) {
        std::size_t instrIndex = 0;
        while (instrIndex < block.instructions.size()) {
            const auto &instr = block.instructions[instrIndex];
            if (instr.opcode != MOpcode::CALL) {
                ++instrIndex;
                continue;
            }

            if (instr.callPlanId == MInstr::kNoCallPlanId) {
                ++instrIndex;
                continue;
            }

            if (instr.callPlanId >= plans.size()) {
                throw std::runtime_error("x86-64 backend: call plan id " +
                                         std::to_string(instr.callPlanId) +
                                         " is out of range for function '" + func.name + "'");
            }

            // Save callPlanId before lowerCall — insertion may reallocate
            // block.instructions and invalidate the instr reference.
            const std::size_t planId = instr.callPlanId;
            const std::size_t beforeSize = block.instructions.size();
            lowerCall(block, instrIndex, plans[planId], target, frame);
            const std::size_t afterSize = block.instructions.size();
            const std::size_t inserted = afterSize - beforeSize;
            consumed[planId] = true;

            // The CALL instruction we just processed is now at position (instrIndex + inserted).
            // Skip past it to continue searching for the next CALL.
            instrIndex += inserted + 1;
        }
    }

    auto missing = std::find(consumed.begin(), consumed.end(), false);
    if (missing != consumed.end()) {
        const auto planId = static_cast<std::size_t>(std::distance(consumed.begin(), missing));
        throw std::runtime_error("x86-64 backend: call plan " + std::to_string(planId) +
                                 " was not consumed in function '" + func.name + "'");
    }
}

/// @brief Install the native runtime context before user code in @c main.
/// @details Mirrors the AArch64 backend's startup sequence. The calls are
///          inserted after planned user calls have been lowered, so they use
///          the fixed ABI registers directly: rt_legacy_context returns a
///          context pointer, which is then passed as arg0 to
///          rt_set_current_context.
///
/// @param func Function to inspect and, when it is the entry point, mutate.
/// @param target Target whose first integer argument and return registers are used.
void insertMainRuntimeContextInit(MFunction &func, const TargetInfo &target) {
    if ((func.name != "main" && func.name != "@main") || func.blocks.empty() ||
        target.intArgOrder.empty()) {
        return;
    }

    auto &entry = func.blocks.front().instructions;
    /// @brief Recognizes a direct call to the requested runtime symbol.
    /// @param instr Instruction to inspect.
    /// @param name Expected runtime symbol name.
    /// @return `true` when `instr` is a direct call to `name`.
    auto isCallTo = [](const MInstr &instr, const char *name) {
        if (instr.opcode != MOpcode::CALL || instr.operands.empty())
            return false;
        const auto *label = std::get_if<OpLabel>(&instr.operands.front());
        return label && label->name == name;
    };
    if (entry.size() >= 3 && isCallTo(entry[0], "rt_legacy_context") &&
        isCallTo(entry[2], "rt_set_current_context")) {
        return;
    }

    const Operand retReg =
        makePhysRegOperand(RegClass::GPR, static_cast<uint16_t>(target.intReturnReg));
    const Operand argReg =
        makePhysRegOperand(RegClass::GPR, static_cast<uint16_t>(target.intArgOrder.front()));
    std::vector<MInstr> init;
    init.reserve(3);
    init.push_back(MInstr::make(MOpcode::CALL, {makeLabelOperand("rt_legacy_context")}));
    init.push_back(MInstr::make(MOpcode::MOVrr, {argReg, retReg}));
    init.push_back(MInstr::make(MOpcode::CALL, {makeLabelOperand("rt_set_current_context")}));
    entry.insert(entry.begin(), init.begin(), init.end());
}

/// @brief Lower one IL function and run the complete pre-RA legalization sequence.
/// @param ilFunc Source IL function.
/// @param lowering Function-local lowering context and literal-pool owner.
/// @param target ABI and register description used by call lowering and selection.
/// @param frame Destination frame summary initialized by this helper.
/// @param machineFunc Destination MIR function replaced with the lowered result.
/// @throws std::exception Propagates lowering or legalization failures.
static void legalizeFunctionPipeline(const ILFunction &ilFunc,
                                     LowerILToMIR &lowering,
                                     const TargetInfo &target,
                                     FrameInfo &frame,
                                     MFunction &machineFunc) {
    machineFunc = lowering.lower(ilFunc);

    frame = FrameInfo{};
    lowerPendingCalls(machineFunc, lowering.callPlans(), target, frame);

    ISel isel{target};
    isel.lowerArithmetic(machineFunc);
    isel.lowerCompareAndBranch(machineFunc);
    isel.lowerSelect(machineFunc);
    isel.validateSelectLowering(machineFunc);

    lowerSignedDivRem(machineFunc);
    lowerOverflowOps(machineFunc);
    splitInternalLabelBlocks(machineFunc);
    insertMainRuntimeContextInit(machineFunc, target);
}

/// @brief Run register allocation and frame lowering on one legalized function.
/// @param machineFunc Function to allocate and rewrite in place.
/// @param target ABI and physical-register description.
/// @param options Reserved per-function backend options.
/// @param frame Frame summary updated with spill areas and prologue requirements.
/// @throws std::exception Propagates allocation or frame-lowering failures.
static void allocateFunctionPipeline(MFunction &machineFunc,
                                     const TargetInfo &target,
                                     const CodegenOptions &options,
                                     FrameInfo &frame) {
    (void)options;
    const AllocationResult allocResult = allocate(machineFunc, target);

    assignSpillSlots(machineFunc, target, frame);
    frame.spillAreaGPR = std::max(frame.spillAreaGPR, allocResult.spillSlotsGPR * kSlotSizeBytes);
    frame.spillAreaXMM = std::max(frame.spillAreaXMM, allocResult.spillSlotsXMM * kSlotSizeBytes);
    insertPrologueEpilogue(machineFunc, target, frame);
}

/// @brief Seed a debug line table with enough file entries for a MIR module.
/// @details Every file identifier from one through the maximum identifier used
///          by the module is mapped to the normalized @p debugSourcePath, or to
///          the synthetic `&lt;source&gt;` name when no path was supplied.
/// @param table Line table to extend with file slots.
/// @param mir Functions whose instruction locations determine the slot count.
/// @param debugSourcePath Source filename associated with each generated slot.
static void seedDebugFiles(DebugLineTable &table,
                           const std::vector<MFunction> &mir,
                           std::string_view debugSourcePath) {
    uint32_t maxFileId = 1;
    for (const auto &fn : mir) {
        for (const auto &block : fn.blocks) {
            for (const auto &instr : block.instructions) {
                if (instr.loc.file_id > maxFileId)
                    maxFileId = instr.loc.file_id;
            }
        }
    }

    std::string filePath = std::string(debugSourcePath);
    if (filePath.empty())
        filePath = "<source>";
    else
        filePath = std::filesystem::path(filePath).lexically_normal().string();

    for (uint32_t fileId = 1; fileId <= maxFileId; ++fileId)
        table.addFileSlot(filePath);
}

/// @brief Single-function overload of seedDebugFiles for per-function emit paths.
/// @details Same semantics as the multi-function version above but scans only
///          @p fn's instructions for the maximum DWARF file id.
/// @param table Line table to extend with file slots.
/// @param fn Function whose instruction locations determine the slot count.
/// @param debugSourcePath Source filename associated with each generated slot.
static void seedDebugFiles(DebugLineTable &table,
                           const MFunction &fn,
                           std::string_view debugSourcePath) {
    uint32_t maxFileId = 1;
    for (const auto &block : fn.blocks) {
        for (const auto &instr : block.instructions) {
            if (instr.loc.file_id > maxFileId)
                maxFileId = instr.loc.file_id;
        }
    }

    std::string filePath = std::string(debugSourcePath);
    if (filePath.empty())
        filePath = "<source>";
    else
        filePath = std::filesystem::path(filePath).lexically_normal().string();

    for (uint32_t fileId = 1; fileId <= maxFileId; ++fileId)
        table.addFileSlot(filePath);
}

/// @brief Throw a legalization diagnostic through legacy emit facades.
///
/// @details The pass pipeline calls @ref legalizeModuleToMIR directly and consumes
///          its boolean result plus diagnostic string. The public `emit*` helpers
///          historically let invalid-IL legalization exceptions escape as
///          `std::runtime_error`; this helper preserves that API behavior while
///          keeping the pass-facing legalization contract explicit.
///
/// @param errors Diagnostic text produced by @ref legalizeModuleToMIR.
/// @throws std::runtime_error Always, with @p errors or a fallback message.
[[noreturn]] void throwLegalizationDiagnostic(const std::string &errors) {
    throw std::runtime_error(errors.empty() ? "x86-64 legalization failed" : errors);
}

} // namespace

/// @copydoc selectTarget
const TargetInfo &selectTarget(CodegenOptions::TargetABI abi) noexcept {
    switch (abi) {
        case CodegenOptions::TargetABI::SysV:
            return sysvTarget();
        case CodegenOptions::TargetABI::Win64:
            return win64Target();
        case CodegenOptions::TargetABI::Host:
            return hostTarget();
    }
    return hostTarget();
}

/// @copydoc legalizeModuleToMIR
bool legalizeModuleToMIR(const ILModule &mod,
                         const TargetInfo &target,
                         const CodegenOptions &options,
                         AsmEmitter::RoDataPool &roData,
                         std::vector<MFunction> &mir,
                         std::vector<FrameInfo> &frames,
                         std::string &errors) {
    (void)options;
    mir.clear();
    frames.clear();
    errors.clear();

    try {
        std::unordered_set<std::string> knownVarArgCallees;
        knownVarArgCallees.reserve(mod.funcs.size());
        for (const auto &fn : mod.funcs) {
            if (fn.isVarArg)
                knownVarArgCallees.insert(fn.name);
        }

        mir.resize(mod.funcs.size());
        frames.resize(mod.funcs.size());
        std::vector<AsmEmitter::RoDataPool> functionPools(mod.funcs.size());
        std::atomic_size_t nextIndex{0};
        std::exception_ptr firstException;
        std::mutex exceptionMutex;

        /**
         * @brief Claim and legalize function indices until the work queue is empty.
         *
         * Each invocation uses function-private lowering and literal-pool state.
         * Exceptions are captured once for rethrow after all workers join, while
         * stable index slots preserve module order.
         */
        auto legalizeNext = [&]() {
            for (;;) {
                const std::size_t index =
                    nextIndex.fetch_add(1, std::memory_order_relaxed);
                if (index >= mod.funcs.size())
                    return;
                try {
                    LowerILToMIR lowering{target, functionPools[index]};
                    lowering.setKnownVarArgCallees(knownVarArgCallees);
                    legalizeFunctionPipeline(
                        mod.funcs[index], lowering, target, frames[index], mir[index]);
                } catch (...) {
                    std::lock_guard<std::mutex> lock(exceptionMutex);
                    if (!firstException)
                        firstException = std::current_exception();
                    return;
                }
            }
        };

        const std::size_t workerCount = common::codegenWorkerCount(mod.funcs.size());
        if (workerCount <= 1) {
            legalizeNext();
        } else {
            std::vector<std::thread> workers;
            workers.reserve(workerCount);
            for (std::size_t worker = 0; worker < workerCount; ++worker)
                workers.emplace_back(legalizeNext);
            for (auto &worker : workers)
                worker.join();
        }
        if (firstException)
            std::rethrow_exception(firstException);

        // Merge per-function literals in source order and rewrite every symbolic
        // operand that referred to a worker-local literal label.
        for (std::size_t index = 0; index < functionPools.size(); ++index) {
            auto &pool = functionPools[index];
            std::unordered_map<std::string, std::string> labelMap;
            for (std::size_t literal = 0; literal < pool.stringCount(); ++literal) {
                const int localIndex = static_cast<int>(literal);
                const int globalIndex = roData.addStringLiteral(pool.stringBytes(localIndex));
                labelMap.emplace(pool.stringLabel(static_cast<int>(literal)),
                                 roData.stringLabel(globalIndex));
            }
            for (std::size_t literal = 0; literal < pool.f64Count(); ++literal) {
                const int localIndex = static_cast<int>(literal);
                const int globalIndex = roData.addF64Literal(pool.f64Value(localIndex));
                labelMap.emplace(pool.f64Label(static_cast<int>(literal)),
                                 roData.f64Label(globalIndex));
            }
            for (auto &block : mir[index].blocks) {
                for (auto &instr : block.instructions) {
                    for (auto &operand : instr.operands) {
                        if (auto *label = std::get_if<OpLabel>(&operand)) {
                            if (const auto it = labelMap.find(label->name); it != labelMap.end())
                                label->name = it->second;
                        } else if (auto *ripLabel = std::get_if<OpRipLabel>(&operand)) {
                            if (const auto it = labelMap.find(ripLabel->name); it != labelMap.end())
                                ripLabel->name = it->second;
                        }
                    }
                }
            }
        }
    } catch (const std::exception &ex) {
        mir.clear();
        frames.clear();
        errors = std::string("x86-64 legalization failed: ") + ex.what();
        return false;
    } catch (...) {
        mir.clear();
        frames.clear();
        errors = "x86-64 legalization failed: unknown exception";
        return false;
    }
    return true;
}

/// @copydoc allocateModuleMIR
bool allocateModuleMIR(std::vector<MFunction> &mir,
                       std::vector<FrameInfo> &frames,
                       const TargetInfo &target,
                       const CodegenOptions &options,
                       std::string &errors) {
    errors.clear();
    if (mir.size() != frames.size()) {
        errors = "frame/MIR count mismatch prior to register allocation";
        return false;
    }

    std::string firstError;
    std::mutex errorMutex;
    /// @brief Allocates one indexed function and retains the first diagnostic.
    /// @param index MIR and frame index to allocate.
    auto allocateOne = [&](std::size_t index) {
        try {
            allocateFunctionPipeline(mir[index], target, options, frames[index]);
        } catch (const std::exception &ex) {
            std::lock_guard<std::mutex> lock(errorMutex);
            if (firstError.empty())
                firstError = ex.what();
        }
    };

    const std::size_t workerCount = common::codegenWorkerCount(mir.size());
    if (workerCount <= 1) {
        for (std::size_t i = 0; i < mir.size(); ++i)
            allocateOne(i);
    } else {
        std::atomic_size_t nextIndex{0};
        std::vector<std::thread> workers;
        workers.reserve(workerCount);
        for (std::size_t worker = 0; worker < workerCount; ++worker) {
            /// Repeatedly claim unallocated function indices for this worker.
            workers.emplace_back([&]() {
                for (;;) {
                    const std::size_t index = nextIndex.fetch_add(1, std::memory_order_relaxed);
                    if (index >= mir.size())
                        break;
                    allocateOne(index);
                }
            });
        }
        for (auto &worker : workers)
            worker.join();
    }

    if (!firstError.empty()) {
        errors = firstError;
        return false;
    }

    // Strip identity moves (mov r, r) that the register allocator may insert
    // when a virtual register happens to be assigned the same physical register
    // as its source.  These are always no-ops and safe to remove at any
    // optimization level.
    for (auto &fn : mir) {
        for (auto &block : fn.blocks) {
            auto &instrs = block.instructions;
            instrs.erase(std::remove_if(instrs.begin(),
                                        instrs.end(),
                                        /// @brief Identifies allocator-produced no-op register copies.
                                        /// @param instr Instruction to inspect.
                                        /// @return `true` when the instruction is an identity move.
                                        [](const MInstr &instr) {
                                            return peephole::isIdentityMovRR(instr) ||
                                                   peephole::isIdentityMovSDRR(instr);
                                        }),
                         instrs.end());
        }
    }

    return true;
}

/// @copydoc scheduleModuleMIR
bool scheduleModuleMIR(std::vector<MFunction> &mir,
                       const CodegenOptions &options,
                       std::string &errors) {
    errors.clear();
    if (options.optimizeLevel < 1)
        return true;

    try {
        (void)scheduleModule(mir);
    } catch (const std::exception &ex) {
        errors = ex.what();
        return false;
    }
    return true;
}

/// @copydoc optimizeModuleMIR
bool optimizeModuleMIR(std::vector<MFunction> &mir,
                       const CodegenOptions &options,
                       std::string &errors) {
    errors.clear();
    if (options.optimizeLevel < 1)
        return true;

    const TargetInfo &target = selectTarget(options.targetABI);
    std::string firstError;
    std::mutex errorMutex;
    /// @brief Runs target-aware peepholes for one function and captures its failure.
    /// @param index MIR function index to optimize.
    auto optimizeOne = [&](std::size_t index) {
        try {
            runPeepholes(mir[index], target);
        } catch (const std::exception &ex) {
            std::lock_guard<std::mutex> lock(errorMutex);
            if (firstError.empty())
                firstError = ex.what();
        }
    };

    const std::size_t workerCount = common::codegenWorkerCount(mir.size());
    if (workerCount <= 1) {
        for (std::size_t index = 0; index < mir.size(); ++index)
            optimizeOne(index);
    } else {
        std::atomic_size_t nextIndex{0};
        std::vector<std::thread> workers;
        workers.reserve(workerCount);
        for (std::size_t worker = 0; worker < workerCount; ++worker) {
            /// Repeatedly claim unoptimized function indices for this worker.
            workers.emplace_back([&]() {
                for (;;) {
                    const std::size_t index = nextIndex.fetch_add(1, std::memory_order_relaxed);
                    if (index >= mir.size())
                        break;
                    optimizeOne(index);
                }
            });
        }
        for (auto &worker : workers)
            worker.join();
    }

    if (!firstError.empty()) {
        errors = firstError;
        return false;
    }
    return true;
}

/// @copydoc emitMIRToAssembly
CodegenResult emitMIRToAssembly(const std::vector<MFunction> &mir,
                                const AsmEmitter::RoDataPool &roData,
                                const TargetInfo &target,
                                const CodegenOptions &options) {
    CodegenResult result{};

    std::ostringstream asmStream{};
    std::ostringstream errorStream{};
    bool emissionFailed = false;

    if (const auto warning = syntaxWarning(options); !warning.empty()) {
        errorStream << warning;
    }

    const objfile::ObjFormat format = targetObjectFormat(options.targetPlatform);

    try {
        AsmEmitter::RoDataPool roDataCopy = roData;
        AsmEmitter emitter{roDataCopy, format};

        for (std::size_t index = 0; index < mir.size(); ++index) {
            emitter.emitFunction(asmStream, mir[index], target);
            if (index + 1U < mir.size()) {
                asmStream << '\n';
            }
        }

        emitter.emitRoData(asmStream);
    } catch (const std::exception &ex) {
        emissionFailed = true;
        asmStream.str("");
        asmStream.clear();
        errorStream << ex.what() << '\n';
    }

    // Mark the stack as non-executable on ELF targets.  Without this directive
    // the GNU linker defaults to an executable stack, triggering a warning and
    // creating a security issue.
    if (!emissionFailed && format == objfile::ObjFormat::ELF) {
        asmStream << "\n.section .note.GNU-stack,\"\",@progbits\n";
    }

    result.asmText = asmStream.str();
    result.errors = errorStream.str();

    return result;
}

/// @copydoc emitFunctionToAssembly
CodegenResult emitFunctionToAssembly(const ILFunction &func, const CodegenOptions &opt) {
    const TargetInfo &target = selectTarget(opt.targetABI);
    AsmEmitter::RoDataPool roData{};
    std::vector<MFunction> mir;
    std::vector<FrameInfo> frames;
    std::string errors;
    ILModule module{};
    module.funcs.push_back(func);
    if (!legalizeModuleToMIR(module, target, opt, roData, mir, frames, errors))
        throwLegalizationDiagnostic(errors);
    if (!allocateModuleMIR(mir, frames, target, opt, errors))
        return CodegenResult{{}, errors};
    if (!scheduleModuleMIR(mir, opt, errors))
        return CodegenResult{{}, errors};
    if (!optimizeModuleMIR(mir, opt, errors))
        return CodegenResult{{}, errors};
    return emitMIRToAssembly(mir, roData, target, opt);
}

/// @copydoc emitModuleToAssembly
CodegenResult emitModuleToAssembly(const ILModule &mod, const CodegenOptions &opt) {
    const TargetInfo &target = selectTarget(opt.targetABI);
    AsmEmitter::RoDataPool roData{};
    std::vector<MFunction> mir;
    std::vector<FrameInfo> frames;
    std::string errors;
    if (!legalizeModuleToMIR(mod, target, opt, roData, mir, frames, errors))
        throwLegalizationDiagnostic(errors);
    if (!allocateModuleMIR(mir, frames, target, opt, errors))
        return CodegenResult{{}, errors};
    if (!scheduleModuleMIR(mir, opt, errors))
        return CodegenResult{{}, errors};
    if (!optimizeModuleMIR(mir, opt, errors))
        return CodegenResult{{}, errors};
    return emitMIRToAssembly(mir, roData, target, opt);
}

/// @copydoc emitMIRToBinary
BinaryEmitResult emitMIRToBinary(const std::vector<MFunction> &mir,
                                 const std::vector<FrameInfo> &frames,
                                 const AsmEmitter::RoDataPool &roData,
                                 const TargetInfo &target,
                                 const CodegenOptions &options) {
    BinaryEmitResult result{};
    std::ostringstream errorStream{};
    const objfile::ObjFormat format = targetObjectFormat(options.targetPlatform);
    const bool emitWin64Unwind = format == objfile::ObjFormat::COFF;
    const bool emitDebugLines = options.emitDebugLines && format != objfile::ObjFormat::COFF;

    if (const auto warning = syntaxWarning(options); !warning.empty()) {
        // Syntax warnings don't apply to binary emission, but keep parity.
    }

    DebugLineTable debugLines;
    if (emitDebugLines)
        seedDebugFiles(debugLines, mir, options.debugSourcePath);

    if (mir.size() != frames.size()) {
        result.errors = "frame/MIR count mismatch prior to binary emission";
        return result;
    }

    result.textSections.reserve(mir.size());

    for (int i = 0; i < static_cast<int>(roData.stringCount()); ++i) {
        std::string label = roData.stringLabel(i);
        result.rodata.defineSymbol(
            label, objfile::SymbolBinding::Local, objfile::SymbolSection::Rodata);
        const auto &bytes = roData.stringBytes(i);
        result.rodata.emitBytes(bytes.data(), bytes.size());
    }
    // Align to 8 before f64 constants.
    if (roData.f64Count() > 0)
        result.rodata.alignTo(8);
    for (int i = 0; i < static_cast<int>(roData.f64Count()); ++i) {
        std::string label = roData.f64Label(i);
        result.rodata.defineSymbol(
            label, objfile::SymbolBinding::Local, objfile::SymbolSection::Rodata);
        result.rodata.emit64LE(zanna::codegen::common::f64Bits(roData.f64Value(i)));
    }

    /**
     * @brief Encode one function into a caller-owned text section.
     *
     * @param i Stable MIR/frame index to encode.
     * @param funcText Destination section for the function's bytes and relocations.
     * @param funcDebugLines Optional function-local line table to populate.
     * @return Empty string on success, otherwise a function-qualified diagnostic.
     */
    auto encodeOne = [&](std::size_t i,
                         objfile::CodeSection &funcText,
                         DebugLineTable *funcDebugLines) -> std::string {
        binenc::X64BinaryEncoder funcEncoder;
        if (funcDebugLines)
            funcEncoder.setDebugLineTable(funcDebugLines);
        const auto traceStart = std::chrono::steady_clock::now();
        if (traceX64BinaryEmit()) {
            std::cerr << "[x64-binary] encode " << (i + 1) << "/" << mir.size() << " "
                      << mir[i].name << " blocks=" << mir[i].blocks.size()
                      << " instrs=" << mirInstructionCount(mir[i]) << "\n";
        }
        try {
            funcEncoder.encodeFunction(mir[i],
                                       funcText,
                                       result.rodata,
                                       format == objfile::ObjFormat::MachO,
                                       &frames[i],
                                       emitWin64Unwind);
        } catch (const std::exception &ex) {
            return "x86-64 binary emission failed for function '" + mir[i].name + "': " + ex.what();
        }
        if (traceX64BinaryEmit()) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - traceStart);
            std::cerr << "[x64-binary] done " << mir[i].name << " ms=" << elapsed.count()
                      << " bytes=" << funcText.bytes().size()
                      << " relocs=" << funcText.relocations().size() << "\n";
        }
        return {};
    };

    if (emitDebugLines) {
        for (std::size_t i = 0; i < mir.size(); ++i) {
            objfile::CodeSection funcText;
            DebugLineTable funcDebugLines;
            seedDebugFiles(funcDebugLines, mir[i], options.debugSourcePath);

            if (std::string error = encodeOne(i, funcText, &funcDebugLines); !error.empty()) {
                BinaryEmitResult failure{};
                failure.errors = std::move(error);
                return failure;
            }

            const uint64_t debugBias = static_cast<uint64_t>(result.text.currentOffset());
            debugLines.append(funcDebugLines, debugBias);
            result.text.appendSection(funcText);
            result.textSections.push_back(std::move(funcText));
        }
    } else {
        /// @brief Stable worker result slot for a single encoded function.
        struct EncodedFunction {
            /// Function-local bytes and relocations.
            objfile::CodeSection text;
            /// Empty on success; diagnostic otherwise.
            std::string error;
        };
        std::vector<EncodedFunction> encoded(mir.size());
        std::atomic_size_t nextIndex{0};
        std::atomic_bool failed{false};

        /**
         * @brief Claim and encode functions into stable per-index result slots.
         *
         * Each worker owns its encoder and text section; the shared read-only
         * literal section is only consulted for relocation targets. A failure
         * asks other workers to stop claiming new functions.
         */
        auto encodeNext = [&]() {
            while (!failed.load(std::memory_order_relaxed)) {
                const std::size_t index =
                    nextIndex.fetch_add(1, std::memory_order_relaxed);
                if (index >= mir.size())
                    return;
                encoded[index].error = encodeOne(index, encoded[index].text, nullptr);
                if (!encoded[index].error.empty())
                    failed.store(true, std::memory_order_relaxed);
            }
        };

        const std::size_t workerCount = common::codegenWorkerCount(mir.size());
        if (workerCount <= 1) {
            encodeNext();
        } else {
            std::vector<std::thread> workers;
            workers.reserve(workerCount);
            for (std::size_t worker = 0; worker < workerCount; ++worker)
                workers.emplace_back(encodeNext);
            for (auto &worker : workers)
                worker.join();
        }

        result.textSections.reserve(encoded.size());
        for (auto &function : encoded) {
            if (!function.error.empty()) {
                BinaryEmitResult failure{};
                failure.errors = std::move(function.error);
                return failure;
            }
            result.textSections.push_back(std::move(function.text));
        }
    }

    if (emitDebugLines && !debugLines.empty())
        result.debugLineData = debugLines.encodeDwarf5(8);

    result.errors = errorStream.str();
    return result;
}

/// @copydoc emitModuleToBinary
BinaryEmitResult emitModuleToBinary(const ILModule &mod, const CodegenOptions &opt) {
    const TargetInfo &target = selectTarget(opt.targetABI);
    AsmEmitter::RoDataPool roData{};
    std::vector<MFunction> mir;
    std::vector<FrameInfo> frames;
    std::string errors;
    if (!legalizeModuleToMIR(mod, target, opt, roData, mir, frames, errors))
        throwLegalizationDiagnostic(errors);
    if (!allocateModuleMIR(mir, frames, target, opt, errors))
        return BinaryEmitResult{{}, {}, errors};
    if (!scheduleModuleMIR(mir, opt, errors))
        return BinaryEmitResult{{}, {}, errors};
    if (!optimizeModuleMIR(mir, opt, errors))
        return BinaryEmitResult{{}, {}, errors};
    return emitMIRToBinary(mir, frames, roData, target, opt);
}

} // namespace zanna::codegen::x64
