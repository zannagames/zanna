//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: vm/OpHandlers_Memory.hpp
// Purpose: Declare memory-related opcode handlers used by the VM dispatcher.
// Key invariants: Handlers honour IL semantics for loads, stores, allocations, and pointer ops.
// Ownership/Lifetime: Handlers mutate VM frames but never retain ownership of VM resources.
// Links: docs/il/il-guide.md#reference
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Memory-related opcode handlers for the VM dispatcher.
/// @details Declares handlers for loads, stores, allocations, and address
///          computations. Inline helpers perform type-aware loads/stores using
///          memcpy to avoid undefined behavior from misaligned accesses.

#pragma once

#include "vm/OpHandlerAccess.hpp"
#include "vm/OpHandlerUtils.hpp"
#include "vm/RuntimeBridge.hpp"
#include "vm/VM.hpp"

#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Type.hpp"
#include "il/core/Value.hpp"

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

namespace il::vm::detail::memory {
/// @brief Execution state alias used by memory handlers.
using ExecState = VMAccess::ExecState;

namespace inline_impl {
/// @brief Build an execution result that halts the current dispatch path after a trap.
/// @return ExecResult marked as returned so non-terminating trap observers do not fall through.
inline VM::ExecResult trappedMemoryResult() {
    VM::ExecResult result{};
    result.returned = true;
    return result;
}

/// @brief Trap when a memory instruction is missing required operands.
/// @param in Malformed memory instruction.
/// @param fr Active frame used for diagnostic function naming.
/// @param bb Current block pointer used for diagnostic block naming.
/// @param message Diagnostic text describing the malformed instruction.
/// @return ExecResult that stops dispatch if the trap observer returns.
inline VM::ExecResult trapMalformedMemoryInstruction(const il::core::Instr &in,
                                                     const Frame &fr,
                                                     const il::core::BasicBlock *bb,
                                                     std::string message) {
    RuntimeBridge::trap(TrapKind::InvalidOperation,
                        message,
                        in.loc,
                        fr.func ? fr.func->name : std::string(),
                        bb ? bb->label : std::string());
    return trappedMemoryResult();
}

/// @brief Validate a VM-owned memory range before dereference.
/// @details Accepts frame stack storage, mutable global storage, and live
///          runtime heap payloads. The returned classification tells callers
///          whether to take the shared global mutex before reading or writing.
/// @param vm Active VM whose ProgramState owns global allocations.
/// @param fr Active frame whose stack storage may contain the pointer.
/// @param in Instruction being executed, used for diagnostics.
/// @param bb Current block pointer used for diagnostics.
/// @param ptr Pointer requested by the load/store.
/// @param bytes Number of bytes to access.
/// @param operation Operation name for diagnostics.
/// @param [out] info Receives the classification on success.
/// @return @c true when the range is VM-owned and in bounds.
inline bool validateMemoryAccess(VM &vm,
                                 const Frame &fr,
                                 const il::core::Instr &in,
                                 const il::core::BasicBlock *bb,
                                 const void *ptr,
                                 size_t bytes,
                                 std::string_view operation,
                                 VM::MemoryAccessInfo &info) {
    info = VMAccess::classifyMemoryAccess(vm, fr, ptr, bytes);
    if (info.valid)
        return true;

    RuntimeBridge::trap(TrapKind::InvalidOperation,
                        std::string(operation) + " outside VM-owned memory",
                        in.loc,
                        fr.func ? fr.func->name : std::string(),
                        bb ? bb->label : std::string());
    return false;
}

/// @brief Return the minimum alignment required for a given IL type kind.
/// @details The VM validates alignment before performing memory operations so
///          misaligned accesses can trap deterministically. The alignment is
///          based on the host representation of the IL type.
/// @param kind IL type kind to query.
/// @return Required alignment in bytes.
inline size_t minimumAlignmentFor(il::core::Type::Kind kind) {
    switch (kind) {
        case il::core::Type::Kind::I1:
            return alignof(uint8_t);
        case il::core::Type::Kind::I16:
            return alignof(int16_t);
        case il::core::Type::Kind::I32:
            return alignof(int32_t);
        case il::core::Type::Kind::I64:
            return alignof(int64_t);
        case il::core::Type::Kind::F64:
            return alignof(double);
        case il::core::Type::Kind::Str:
            return alignof(rt_string);
        case il::core::Type::Kind::Ptr:
        case il::core::Type::Kind::Error:
        case il::core::Type::Kind::ResumeTok:
            return alignof(void *);
        case il::core::Type::Kind::Void:
            return 1U;
    }
    return 0U;
}

/// @brief Return the byte size for a given IL type kind.
/// @details Used by watch hooks and load/store helpers to determine the number
///          of bytes affected by an operation. Void returns zero.
/// @param kind IL type kind to query.
/// @return Size in bytes for the type, or 0 for void.
inline size_t sizeOfKind(il::core::Type::Kind kind) {
    switch (kind) {
        case il::core::Type::Kind::I1:
            return 1;
        case il::core::Type::Kind::I16:
            return 2;
        case il::core::Type::Kind::I32:
            return 4;
        case il::core::Type::Kind::I64:
            return 8;
        case il::core::Type::Kind::F64:
            return 8;
        case il::core::Type::Kind::Str:
            return sizeof(rt_string);
        case il::core::Type::Kind::Ptr:
            return sizeof(void *);
        case il::core::Type::Kind::Error:
            return sizeof(void *);
        case il::core::Type::Kind::ResumeTok:
            return sizeof(void *);
        case il::core::Type::Kind::Void:
            return 0;
    }
    return 0;
}

/// @brief Load a typed slot from an arbitrary pointer.
/// @details Uses memcpy to safely read from potentially unaligned addresses,
///          then widens the value into a VM Slot representation. For strings
///          and pointers, the raw value is copied without ownership changes.
/// @param kind IL type kind describing the value at @p ptr.
/// @param ptr Pointer to the source memory.
/// @return Slot containing the loaded value.
inline Slot loadSlotFromPtr(il::core::Type::Kind kind, void *ptr) {
    Slot out{};
    switch (kind) {
        case il::core::Type::Kind::I16: {
            int16_t tmp;
            std::memcpy(&tmp, ptr, sizeof(tmp));
            out.i64 = static_cast<int64_t>(tmp);
            break;
        }
        case il::core::Type::Kind::I32: {
            int32_t tmp;
            std::memcpy(&tmp, ptr, sizeof(tmp));
            out.i64 = static_cast<int64_t>(tmp);
            break;
        }
        case il::core::Type::Kind::I64: {
            int64_t tmp;
            std::memcpy(&tmp, ptr, sizeof(tmp));
            out.i64 = tmp;
            break;
        }
        case il::core::Type::Kind::I1: {
            uint8_t tmp;
            std::memcpy(&tmp, ptr, sizeof(tmp));
            out.i64 = static_cast<int64_t>(tmp & 1U);
            break;
        }
        case il::core::Type::Kind::F64: {
            double tmp;
            std::memcpy(&tmp, ptr, sizeof(tmp));
            out.f64 = tmp;
            break;
        }
        case il::core::Type::Kind::Str: {
            rt_string tmp;
            std::memcpy(&tmp, ptr, sizeof(tmp));
            out.str = tmp;
            break;
        }
        case il::core::Type::Kind::Ptr: {
            void *tmp;
            std::memcpy(&tmp, ptr, sizeof(tmp));
            out.ptr = tmp;
            break;
        }
        case il::core::Type::Kind::Error:
        case il::core::Type::Kind::ResumeTok: {
            void *tmp;
            std::memcpy(&tmp, ptr, sizeof(tmp));
            out.ptr = tmp;
            break;
        }
        case il::core::Type::Kind::Void:
            out.i64 = 0;
            break;
    }
    return out;
}

/// @brief Store a VM slot into memory with type-aware conversion.
/// @details Uses memcpy to safely write to potentially unaligned addresses.
///          For strings, the helper releases the existing value, retains the
///          incoming string, and writes the retained handle to memory.
/// @param kind IL type kind describing the value at @p ptr.
/// @param ptr Destination memory pointer.
/// @param value Slot value to store.
inline void storeSlotToPtr(il::core::Type::Kind kind, void *ptr, const Slot &value) {
    switch (kind) {
        case il::core::Type::Kind::I16: {
            int16_t tmp = static_cast<int16_t>(value.i64);
            std::memcpy(ptr, &tmp, sizeof(tmp));
            break;
        }
        case il::core::Type::Kind::I32: {
            int32_t tmp = static_cast<int32_t>(value.i64);
            std::memcpy(ptr, &tmp, sizeof(tmp));
            break;
        }
        case il::core::Type::Kind::I64: {
            int64_t tmp = value.i64;
            std::memcpy(ptr, &tmp, sizeof(tmp));
            break;
        }
        case il::core::Type::Kind::I1: {
            uint8_t tmp = static_cast<uint8_t>(value.i64 & 1U);
            std::memcpy(ptr, &tmp, sizeof(tmp));
            break;
        }
        case il::core::Type::Kind::F64: {
            double tmp = value.f64;
            std::memcpy(ptr, &tmp, sizeof(tmp));
            break;
        }
        case il::core::Type::Kind::Str: {
            // Release currently stored string (if any), retain incoming, then store
            rt_string current{};
            std::memcpy(&current, ptr, sizeof(current));
            rt_str_release_maybe(current);
            rt_string incoming = value.str;
            rt_str_retain_maybe(incoming);
            std::memcpy(ptr, &incoming, sizeof(incoming));
            break;
        }
        case il::core::Type::Kind::Ptr: {
            void *tmp = value.ptr;
            std::memcpy(ptr, &tmp, sizeof(tmp));
            break;
        }
        case il::core::Type::Kind::Error:
        case il::core::Type::Kind::ResumeTok: {
            void *tmp = value.ptr;
            std::memcpy(ptr, &tmp, sizeof(tmp));
            break;
        }
        case il::core::Type::Kind::Void:
            break;
    }
}
} // namespace inline_impl

/// @brief Inline load handler for VM dispatch.
/// @details Evaluates the pointer operand, checks alignment, traps on null or
///          misaligned access, then loads the value and stores it into the
///          destination slot. This implementation is used by fast dispatch
///          paths that avoid extra indirection.
/// @param vm Active VM instance.
/// @param state Optional execution state for debug hooks.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue, trap, or control transfer.
inline VM::ExecResult handleLoadImpl(VM &vm,
                                     ExecState *state,
                                     Frame &fr,
                                     const il::core::Instr &in,
                                     const VM::BlockMap &blocks,
                                     const il::core::BasicBlock *&bb,
                                     size_t &ip) {
    (void)state;
    (void)blocks;
    (void)ip;

    if (in.operands.size() < 1)
        return inline_impl::trapMalformedMemoryInstruction(
            in, fr, bb, "load missing pointer operand");

    void *ptr = VMAccess::eval(vm, fr, in.operands[0]).ptr;
    if (!ptr) {
        const std::string blockLabel = bb ? bb->label : std::string();
        RuntimeBridge::trap(TrapKind::InvalidOperation,
                            "null load",
                            in.loc,
                            fr.func ? fr.func->name : std::string(),
                            blockLabel);
        VM::ExecResult result{};
        result.returned = true;
        return result;
    }

    const size_t alignment = inline_impl::minimumAlignmentFor(in.type.kind);
    if (alignment == 0U) {
        const std::string blockLabel = bb ? bb->label : std::string();
        RuntimeBridge::trap(TrapKind::InvalidOperation,
                            "unsupported load type",
                            in.loc,
                            fr.func ? fr.func->name : std::string(),
                            blockLabel);
        VM::ExecResult result{};
        result.returned = true;
        return result;
    }
    const uintptr_t rawPtr = reinterpret_cast<uintptr_t>(ptr);
    // Fast-path: use bitmask for power-of-two alignments; fall back to modulo otherwise.
    if (alignment > 1U && (((alignment & (alignment - 1U)) == 0U)
                               ? ((rawPtr & (static_cast<uintptr_t>(alignment) - 1U)) != 0U)
                               : ((rawPtr % alignment) != 0U))) {
        const std::string blockLabel = bb ? bb->label : std::string();
        RuntimeBridge::trap(TrapKind::InvalidOperation,
                            "misaligned load",
                            in.loc,
                            fr.func ? fr.func->name : std::string(),
                            blockLabel);
        VM::ExecResult result{};
        result.returned = true;
        return result;
    }

    const size_t accessSize = inline_impl::sizeOfKind(in.type.kind);
    VM::MemoryAccessInfo access{};
    if (!inline_impl::validateMemoryAccess(vm, fr, in, bb, ptr, accessSize, "load", access))
        return inline_impl::trappedMemoryResult();

    std::unique_lock<std::mutex> globalLock;
    if (access.sharedGlobal)
        globalLock = std::unique_lock<std::mutex>(VMAccess::mutableGlobalMutex(vm));

    ops::storeResult(fr, in, inline_impl::loadSlotFromPtr(in.type.kind, ptr));
    return {};
}

/// @brief Inline store handler for VM dispatch.
/// @details Evaluates pointer/value operands, checks alignment, traps on null or
///          misaligned access, then writes the value using type-aware semantics.
///          Memory and variable watch hooks are invoked when enabled.
/// @param vm Active VM instance.
/// @param state Optional execution state for debug hooks.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue, trap, or control transfer.
inline VM::ExecResult handleStoreImpl(VM &vm,
                                      ExecState *state,
                                      Frame &fr,
                                      const il::core::Instr &in,
                                      const VM::BlockMap &blocks,
                                      const il::core::BasicBlock *&bb,
                                      size_t &ip) {
    (void)state;
    (void)blocks;

    const std::string blockLabel = bb ? bb->label : std::string();
    if (in.operands.size() < 2)
        return inline_impl::trapMalformedMemoryInstruction(
            in, fr, bb, "store missing pointer or value operand");

    void *ptr = VMAccess::eval(vm, fr, in.operands[0]).ptr;
    if (!ptr) {
        RuntimeBridge::trap(TrapKind::InvalidOperation,
                            "null store",
                            in.loc,
                            fr.func ? fr.func->name : std::string(),
                            blockLabel);
        VM::ExecResult result{};
        result.returned = true;
        return result;
    }

    const size_t alignment = inline_impl::minimumAlignmentFor(in.type.kind);
    if (alignment == 0U) {
        RuntimeBridge::trap(TrapKind::InvalidOperation,
                            "unsupported store type",
                            in.loc,
                            fr.func ? fr.func->name : std::string(),
                            blockLabel);
        VM::ExecResult result{};
        result.returned = true;
        return result;
    }
    const uintptr_t rawPtr = reinterpret_cast<uintptr_t>(ptr);
    // Fast-path: use bitmask for power-of-two alignments; fall back to modulo otherwise.
    if (alignment > 1U && (((alignment & (alignment - 1U)) == 0U)
                               ? ((rawPtr & (static_cast<uintptr_t>(alignment) - 1U)) != 0U)
                               : ((rawPtr % alignment) != 0U))) {
        RuntimeBridge::trap(TrapKind::InvalidOperation,
                            "misaligned store",
                            in.loc,
                            fr.func ? fr.func->name : std::string(),
                            blockLabel);
        VM::ExecResult result{};
        result.returned = true;
        return result;
    }

    Slot value = VMAccess::eval(vm, fr, in.operands[1]);
    const size_t writeSize = inline_impl::sizeOfKind(in.type.kind);
    VM::MemoryAccessInfo access{};
    if (!inline_impl::validateMemoryAccess(vm, fr, in, bb, ptr, writeSize, "store", access))
        return inline_impl::trappedMemoryResult();

    std::unique_lock<std::mutex> globalLock;
    if (access.sharedGlobal)
        globalLock = std::unique_lock<std::mutex>(VMAccess::mutableGlobalMutex(vm));

    // Memory watch hook: use fast-path flag to skip entirely when no watches are active.
    // This eliminates the VMAccess::debug() call and vector empty check in the common case.
    if (VMAccess::hasMemWatchesActive(vm)) [[unlikely]] {
        if (writeSize)
            VMAccess::debug(vm).onMemWrite(ptr, writeSize);
    }

    inline_impl::storeSlotToPtr(in.type.kind, ptr, value);

    // Variable watch hook: use fast-path flag to skip entirely when no watches are active.
    // This avoids operand kind check, id lookup, and string comparisons in the common case.
    if (VMAccess::hasVarWatchesActive(vm)) [[unlikely]] {
        if (in.operands[0].kind == il::core::Value::Kind::Temp) {
            const unsigned id = in.operands[0].id;
            if (id < fr.func->valueNames.size()) {
                const std::string &name = fr.func->valueNames[id];
                if (!name.empty()) {
                    const std::string_view fnView =
                        fr.func ? std::string_view(fr.func->name) : std::string_view{};
                    const std::string_view blockView =
                        bb ? std::string_view(bb->label) : std::string_view{};
                    VMAccess::debug(vm).onStore(
                        name, in.type.kind, value.i64, value.f64, fnView, blockView, ip);
                }
            }
        }
    }

    return {};
}

/// @brief Allocate stack memory for an alloca instruction.
/// @details Reserves space in the current frame's stack region according to
///          the instruction's size operand and alignment rules.
/// @param vm Active VM instance.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue, trap, or control transfer.
VM::ExecResult handleAlloca(VM &vm,
                            Frame &fr,
                            const il::core::Instr &in,
                            const VM::BlockMap &blocks,
                            const il::core::BasicBlock *&bb,
                            size_t &ip);

/// @brief Execute a load instruction (Load).
/// @details Fetches a value from memory, with null and alignment checks, and
///          writes it to the destination slot using IL type semantics.
/// @param vm Active VM instance.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue, trap, or control transfer.
VM::ExecResult handleLoad(VM &vm,
                          Frame &fr,
                          const il::core::Instr &in,
                          const VM::BlockMap &blocks,
                          const il::core::BasicBlock *&bb,
                          size_t &ip);

/// @brief Execute a store instruction (Store).
/// @details Stores a value to memory with null and alignment checks, and
///          invokes memory/variable watch hooks when enabled.
/// @param vm Active VM instance.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue, trap, or control transfer.
VM::ExecResult handleStore(VM &vm,
                           Frame &fr,
                           const il::core::Instr &in,
                           const VM::BlockMap &blocks,
                           const il::core::BasicBlock *&bb,
                           size_t &ip);

/// @brief Execute a getelementptr-style address computation (GEP).
/// @details Computes an address by applying byte offsets to a base pointer,
///          following IL pointer arithmetic rules.
/// @param vm Active VM instance.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue, trap, or control transfer.
VM::ExecResult handleGEP(VM &vm,
                         Frame &fr,
                         const il::core::Instr &in,
                         const VM::BlockMap &blocks,
                         const il::core::BasicBlock *&bb,
                         size_t &ip);

/// @brief Take the address of a stack slot or symbol (AddrOf).
/// @details Produces a pointer value for use by later load/store operations.
/// @param vm Active VM instance.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue, trap, or control transfer.
VM::ExecResult handleAddrOf(VM &vm,
                            Frame &fr,
                            const il::core::Instr &in,
                            const VM::BlockMap &blocks,
                            const il::core::BasicBlock *&bb,
                            size_t &ip);

/// @brief Materialize a constant string handle (ConstStr).
/// @details Converts a string literal operand into a runtime string handle and
///          stores it in the destination slot with proper retain semantics.
/// @param vm Active VM instance.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue, trap, or control transfer.
VM::ExecResult handleConstStr(VM &vm,
                              Frame &fr,
                              const il::core::Instr &in,
                              const VM::BlockMap &blocks,
                              const il::core::BasicBlock *&bb,
                              size_t &ip);

/// @brief Materialize a global address (GAddr).
/// @details Resolves the address of a global symbol and stores it into the
///          destination slot as a pointer value.
/// @param vm Active VM instance.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue, trap, or control transfer.
VM::ExecResult handleGAddr(VM &vm,
                           Frame &fr,
                           const il::core::Instr &in,
                           const VM::BlockMap &blocks,
                           const il::core::BasicBlock *&bb,
                           size_t &ip);

/// @brief Materialize a null pointer constant (ConstNull).
/// @details Produces a null pointer value and stores it in the destination slot.
/// @param vm Active VM instance.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue, trap, or control transfer.
VM::ExecResult handleConstNull(VM &vm,
                               Frame &fr,
                               const il::core::Instr &in,
                               const VM::BlockMap &blocks,
                               const il::core::BasicBlock *&bb,
                               size_t &ip);

/// @brief Materialize a floating-point constant (ConstF64).
/// @details Parses the operand as a f64 literal and stores it in the destination slot.
/// @param vm Active VM instance.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue, trap, or control transfer.
VM::ExecResult handleConstF64(VM &vm,
                              Frame &fr,
                              const il::core::Instr &in,
                              const VM::BlockMap &blocks,
                              const il::core::BasicBlock *&bb,
                              size_t &ip);

} // namespace il::vm::detail::memory
