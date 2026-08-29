//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: vm/OpHandlerAccess.hpp
// Purpose: Expose controlled accessors for VM internals to opcode handler code.
// Key invariants: Grants read/write access only to members required for handler semantics.
// Ownership/Lifetime: Accessors operate on VM-owned state without transferring ownership.
// Links: docs/il/il-guide.md#reference
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares the narrow privileged-access façade used by VM opcode
///        handlers and runtime callback bridges.
/// @details @ref VMAccess centralizes friend access to execution stacks, frame
///          evaluation, debug state, memory classification, shared program
///          state, and internal caches without exposing those VM members
///          publicly.

#pragma once

#include "il/core/Function.hpp"
#include "vm/VM.hpp"

#include "rt_heap.h"
#include "rt_modvar.h"

#include <cstdint>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace il::vm::detail {
/// @brief Controlled static access to VM internals required by handler code.
struct VMAccess {
    /// @brief Execution-state type shared with the VM implementation.
    using ExecState = VM::ExecState;

    /// @brief Retrieve the currently active execution state from the VM stack.
    /// @param vm Virtual machine instance.
    /// @return Pointer to the top execution state, or nullptr if the stack is empty.
    static inline ExecState *currentExecState(VM &vm) {
        return vm.execStack.empty() ? nullptr : vm.execStack.back();
    }

    /// @brief Evaluate an IL value within a frame using the VM's evaluation logic.
    /// @param vm Virtual machine instance.
    /// @param fr Active frame containing register state.
    /// @param value IL value to evaluate.
    /// @return Slot containing the evaluated result.
    static inline Slot eval(VM &vm, Frame &fr, const il::core::Value &value) {
        return vm.eval(fr, value);
    }

    /// @brief Access the VM's debug controller for breakpoint and watch management.
    /// @param vm Virtual machine instance.
    /// @return Reference to the debug controller owned by the VM.
    static inline DebugCtrl &debug(VM &vm) {
        return vm.debug;
    }

    /// @brief Fast-path check for active memory watches.
    /// @param vm Virtual machine whose debug flags are queried.
    /// @return @c true when memory watches are installed.
    static inline bool hasMemWatchesActive(const VM &vm) noexcept {
        return vm.memWatchActive_;
    }

    /// @brief Fast-path check for active variable watches.
    /// @param vm Virtual machine whose debug flags are queried.
    /// @return @c true when variable watches are installed.
    static inline bool hasVarWatchesActive(const VM &vm) noexcept {
        return vm.varWatchActive_;
    }

    /// @brief Access the VM's function name lookup table.
    /// @param vm Virtual machine instance.
    /// @return Const reference to the map from function names to Function pointers.
    static inline const VM::FnMap &functionMap(const VM &vm) {
        return vm.fnMap;
    }

    /// @brief Check whether @p fn is a function object owned by the active module.
    /// @details Pointer-based call.indirect operands are untrusted VM values. The
    ///          handler must verify that the raw pointer matches one of the
    ///          immutable Function instances cached in the VM function map before
    ///          invoking it.
    /// @param vm Virtual machine whose module owns valid function objects.
    /// @param fn Candidate function pointer decoded from a VM slot.
    /// @return True when @p fn is one of the VM's known functions.
    static inline bool isKnownFunctionPointer(const VM &vm, const il::core::Function *fn) {
        if (!fn)
            return false;
        for (const auto &entry : vm.fnMap) {
            if (entry.second == fn)
                return true;
        }
        return false;
    }

    /// @brief Classify a memory range before a VM load or store.
    /// @details A range is valid when it lies wholly inside the active frame's
    ///          stack storage, wholly inside one mutable global allocation, or
    ///          wholly inside one live runtime heap payload. Pointer arithmetic
    ///          that escapes those allocations is rejected before dereference.
    /// @param vm VM whose shared program state owns mutable globals.
    /// @param fr Active frame whose stack storage is also a valid memory region.
    /// @param ptr First byte of the requested memory range.
    /// @param bytes Number of bytes requested; zero-byte accesses are valid.
    /// @return Access classification including whether the range is shared global storage.
    static inline VM::MemoryAccessInfo classifyMemoryAccess(const VM &vm,
                                                            const Frame &fr,
                                                            const void *ptr,
                                                            size_t bytes) noexcept {
        VM::MemoryAccessInfo info{};
        if (bytes == 0) {
            info.valid = true;
            return info;
        }
        if (!ptr)
            return info;

        const auto address = reinterpret_cast<std::uintptr_t>(ptr);
        /// @brief Test whether the requested access is wholly contained in one region.
        /// @param basePtr First byte of the candidate memory region.
        /// @param length Candidate region size in bytes.
        /// @return `true` when `[address, address + bytes)` lies within the region.
        auto containsRange = [&](const void *basePtr, size_t length) noexcept {
            if (!basePtr || length == 0)
                return false;
            const auto begin = reinterpret_cast<std::uintptr_t>(basePtr);
            if (address < begin)
                return false;
            const std::uintptr_t offset = address - begin;
            return offset <= length && bytes <= length - offset;
        };

        if (containsRange(fr.stack.data(), fr.stack.size())) {
            info.valid = true;
            return info;
        }

        if (vm.programState_) {
            for (const auto &entry : vm.programState_->mutableGlobalSizes) {
                if (containsRange(entry.first, entry.second)) {
                    info.valid = true;
                    info.sharedGlobal = true;
                    return info;
                }
            }
        }

        if (rt_heap_contains_range(ptr, bytes)) {
            info.valid = true;
            return info;
        }

        // Module-level variables (rt_modvar_addr_*) live in rt_alloc storage
        // owned by the runtime context, outside the heap registry (ZB-28).
        if (rt_modvar_contains_range(ptr, bytes)) {
            info.valid = true;
            return info;
        }

        // Any other rt_alloc block (class descriptors, runtime tables) — the
        // Runner enables tracking so these are program-owned too (ZB-28).
        if (rt_alloc_contains_range(ptr, bytes)) {
            info.valid = true;
            return info;
        }
        return info;
    }

    /// @brief Access the mutex protecting shared mutable global storage.
    /// @param vm VM whose shared ProgramState owns the global storage.
    /// @return Mutex serializing mutable global reads and writes.
    static inline std::mutex &mutableGlobalMutex(VM &vm) {
        return vm.programState_->mutableGlobalMutex;
    }

    /// @brief Access the VM's runtime call context used for trap metadata.
    /// @param vm Virtual machine instance.
    /// @return Reference to the runtime call context owned by the VM.
    static inline RuntimeCallContext &runtimeContext(VM &vm) {
        return vm.runtimeContext;
    }

    /// @brief Execute a function within the VM and return its result.
    /// @param vm Virtual machine instance.
    /// @param fn Function to call.
    /// @param args Argument slots passed to the function's entry block.
    /// @return Slot containing the function's return value.
    static inline Slot callFunction(VM &vm,
                                    const il::core::Function &fn,
                                    std::span<const Slot> args) {
        return vm.execFunction(fn, args);
    }

    // Stepping helpers for components that need controlled access -------------

    /// @brief Prepare an execution state for stepping through a function.
    /// @param vm Virtual machine instance.
    /// @param fn Function to prepare for execution.
    /// @param args Argument slots for the function's entry block.
    /// @return Fully initialized execution state ready for stepOnce().
    static inline ExecState prepare(VM &vm,
                                    const il::core::Function &fn,
                                    std::span<const Slot> args) {
        return vm.prepareExecution(fn, args);
    }

    /// @brief Execute a single interpreter step within the given execution state.
    /// @param vm Virtual machine instance.
    /// @param st Execution state to advance by one instruction.
    /// @return The function's return value when execution completes, or nullopt to continue.
    static inline std::optional<Slot> stepOnce(VM &vm, ExecState &st) {
        return vm.stepOnce(st);
    }

    /// @brief Set the maximum instruction count before forced termination.
    /// @param vm Virtual machine instance.
    /// @param max Step limit; 0 disables the limit.
    static inline void setMaxSteps(VM &vm, uint64_t max) {
        vm.maxSteps = max;
    }

    /// @brief Configure periodic host polling for cooperative multitasking.
    /// @param vm Virtual machine instance.
    /// @param everyN Invoke the callback every N instructions; 0 disables polling.
    /// @param cb Callback returning false to request a VM pause.
    static inline void setPollConfig(VM &vm, uint32_t everyN, std::function<bool(VM &)> cb) {
        vm.pollEveryN_ = everyN;
        vm.pollCallback_ = std::move(cb);
    }

    /// @brief Access the last trap state for diagnostic reporting.
    /// @param vm Virtual machine whose most recent trap is requested.
    /// @return Read-only reference to VM-owned trap state.
    static inline const VM::TrapState &lastTrapState(const VM &vm) {
        return vm.lastTrap;
    }

    /// @brief Read-only access to the execution stack for backtrace construction.
    /// @param vm Virtual machine whose active execution states are requested.
    /// @return VM-owned stack of non-owning execution-state pointers.
    static inline const std::vector<ExecState *> &execStack(const VM &vm) {
        return vm.execStack;
    }

    /// @brief Refresh debug fast-path flags after configuration changes.
    /// @param vm Virtual machine whose cached debug flags are recomputed.
    static inline void refreshDebugFlags(VM &vm) {
        vm.refreshDebugFlags();
    }

    /// @brief Access the precomputed register count cache.
    /// @details Used by TCO to reuse cached maxSsaId values instead of rescanning.
    /// @param vm Virtual machine that owns the cache.
    /// @return Mutable map from function pointers to maximum SSA identifiers.
    static inline std::unordered_map<const il::core::Function *, size_t> &regCountCache(VM &vm) {
        return vm.regCountCache_;
    }

    /// @brief Access the VM-level switch dispatch cache.
    /// @details The cache persists across function calls; entries are deterministic
    ///          (keyed by stable @c const @c Instr* and computed from case values).
    /// @param vm Virtual machine that owns the switch cache.
    /// @return Mutable reference to the VM-owned switch cache.
    static inline zanna::vm::SwitchCache &switchCache(VM &vm) {
        return vm.switchCache_;
    }

    /// @brief Obtain the pre-resolved operand cache for a specific block.
    /// @details Delegates to @c VM::getOrBuildBlockCache, which lazily builds
    ///          the cache for the entire function on first access.
    /// @param vm Virtual machine that owns the operand cache.
    /// @param fn Function containing @p bb.
    /// @param bb Block whose resolved operands are requested.
    /// @return Cached block data, or @c nullptr when either pointer is null or
    ///         @p bb is not found in @p fn.
    static inline const BlockExecCache *blockExecCache(VM &vm,
                                                       const il::core::Function *fn,
                                                       const il::core::BasicBlock *bb) {
        return vm.getOrBuildBlockCache(fn, bb);
    }

    /// @brief Transfer block parameters from pending slots to registers.
    /// @details Used by TCO to ensure parameters are copied to registers after
    ///          setting up the tail-call frame.
    /// @param vm VM owning the frame.
    /// @param fr Frame whose parameters should be transferred.
    /// @param bb Basic block whose parameter definitions guide the transfer.
    static inline void transferBlockParams(VM &vm, Frame &fr, const il::core::BasicBlock &bb) {
        vm.transferBlockParams(fr, bb);
    }

    /// @brief Obtain (or lazily build) the block label map for a function.
    /// @details Delegates to @c VM::getOrBuildBlockMap.
    /// @param vm Virtual machine that owns the cache.
    /// @param fn Function whose block labels are indexed.
    /// @return Immutable cached mapping from block labels to block pointers.
    static inline const VM::BlockMap &getOrBuildBlockMap(VM &vm, const il::core::Function &fn) {
        return vm.getOrBuildBlockMap(fn);
    }

    /// @brief Compute or retrieve the cached maximum SSA ID for a function.
    ///
    /// @details The maximum SSA ID determines the required register file size.
    ///          This helper checks the VM's cache first, and if not found,
    ///          scans the function's parameters and instruction results to
    ///          find the highest SSA value ID used, then caches the result.
    ///
    /// @invariant The returned value is the largest SSA value ID used by the
    ///            function. Register files sized to (maxSsaId + 1) will
    ///            accommodate all values without resizing.
    ///
    /// @param vm VM owning the register count cache.
    /// @param fn Function to compute the maximum SSA ID for.
    /// @return The maximum SSA value ID used by the function.
    static inline size_t computeMaxSsaId(VM &vm, const il::core::Function &fn) {
        auto &cache = vm.regCountCache_;
        if (auto it = cache.find(&fn); it != cache.end())
            return it->second;

        // Use valueNames size as initial estimate if available (common case)
        size_t maxSsaId = fn.valueNames.empty() ? 0 : fn.valueNames.size() - 1;

        // Scan function parameters
        for (const auto &p : fn.params)
            maxSsaId = std::max(maxSsaId, static_cast<size_t>(p.id));

        // Scan block parameters and instruction results
        for (const auto &block : fn.blocks) {
            for (const auto &p : block.params)
                maxSsaId = std::max(maxSsaId, static_cast<size_t>(p.id));
            for (const auto &instr : block.instructions)
                if (instr.result)
                    maxSsaId = std::max(maxSsaId, static_cast<size_t>(*instr.result));
        }

        cache.emplace(&fn, maxSsaId);
        return maxSsaId;
    }
};
} // namespace il::vm::detail
