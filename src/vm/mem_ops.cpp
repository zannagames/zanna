//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE in the project root for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements the VM opcode handlers responsible for memory and pointer
// operations.  These routines manage the interpreter's stack storage, compute
// pointer arithmetic, and bridge to runtime-managed string constants while
// ensuring traps fire when callers violate safety invariants.
//
//===----------------------------------------------------------------------===//

#include "vm/OpHandlers_Memory.hpp"

#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Type.hpp"
#include "vm/OpHandlerUtils.hpp"
#include "vm/RuntimeBridge.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

using namespace il::core;

/// @file
/// @brief Memory and pointer opcode handlers for the Zanna virtual machine.
/// @details The handlers in this file evaluate IL instructions that manipulate
///          memory addresses or interact with runtime-managed buffers.  They
///          ensure stack allocations stay in-bounds, pointer arithmetic obeys
///          provenance rules, and constant materialisation honours type-specific
///          storage conventions.

namespace il::vm::detail::memory {
/// @brief Handle the @c alloca opcode by reserving stack storage.
/// @details Validates the requested size, aligns the stack pointer to
///          @c std::max_align_t, zeros the allocated memory, and advances the
///          frame's stack pointer.  On overflow or invalid operands the handler
///          emits a trap and returns an execution result that signals unwinding.
/// @param vm Virtual machine instance used for operand evaluation and traps.
/// @param fr Active frame whose stack storage is extended.
/// @param in IL instruction supplying the allocation size and destination slot.
/// @param blocks Map of basic blocks for the current function (unused).
/// @param bb Reference to the current block pointer (unused).
/// @param ip Instruction index within the block (unused).
/// @return Execution result indicating whether interpretation should continue.
VM::ExecResult handleAlloca(VM &vm,
                            Frame &fr,
                            const Instr &in,
                            const VM::BlockMap &blocks,
                            const BasicBlock *&bb,
                            size_t &ip) {
    (void)blocks;
    (void)bb;
    (void)ip;
    if (in.operands.empty()) {
        RuntimeBridge::trap(
            TrapKind::DomainError, "missing allocation size", in.loc, fr.func->name, "");
        return {};
    }

    int64_t bytes = VMAccess::eval(vm, fr, in.operands[0]).i64;
    if (bytes < 0) {
        RuntimeBridge::trap(
            TrapKind::DomainError, "negative allocation", in.loc, fr.func->name, "");
        return {};
    }

    const size_t size = static_cast<size_t>(bytes);
    const size_t addr = fr.sp;
    const size_t stackSize = fr.stack.size();
    constexpr size_t alignment = alignof(std::max_align_t);

    // Alignment must be a power of 2 for bitwise operations to work correctly
    static_assert((alignment & (alignment - 1)) == 0, "alignment must be power of 2");

    /// @brief Raise the common alloca stack-overflow trap.
    /// @return Empty execution result after recording the trap.
    auto trapOverflow = [&]() -> VM::ExecResult {
        RuntimeBridge::trap(
            TrapKind::Overflow, "stack overflow in alloca", in.loc, fr.func->name, "");
        return {};
    };

    if (addr > stackSize)
        return trapOverflow();

    // Use bitwise operations for alignment (faster than modulo when alignment is power of 2)
    // Formula: (addr + alignment - 1) & ~(alignment - 1)
    // This rounds addr up to the next multiple of alignment
    size_t alignedAddr = addr;
    if (alignment > 1) {
        // Check for overflow before adding alignment-1
        if (alignedAddr > std::numeric_limits<size_t>::max() - (alignment - 1))
            return trapOverflow();
        alignedAddr = (alignedAddr + alignment - 1) & ~(alignment - 1);
    }

    if (alignedAddr > stackSize)
        return trapOverflow();

    if (size > std::numeric_limits<size_t>::max() - alignedAddr)
        return trapOverflow();

    const size_t remaining = stackSize - alignedAddr;
    if (size > remaining)
        return trapOverflow();

    std::memset(fr.stack.data() + alignedAddr, 0, size);

    Slot out{};
    out.ptr = fr.stack.data() + alignedAddr;
    fr.sp = alignedAddr + size;
    ops::storeResult(fr, in, out);
    return {};
}

/// @brief Handle the @c gep opcode by computing pointer arithmetic.
/// @details Evaluates the base pointer and byte offset operands, performs the
///          offset calculation, and stores the resulting pointer.  The handler
///          assumes callers respect allocation bounds; later loads or stores will
///          trap if the pointer escapes its provenance.
/// @param vm Virtual machine instance executing the instruction.
/// @param fr Active frame storing operands and results.
/// @param in Instruction describing the base pointer, offset, and destination.
/// @param blocks Map of basic blocks for the current function (unused).
/// @param bb Reference to the current block pointer (unused).
/// @param ip Instruction index within the block (unused).
/// @return Execution result indicating whether interpretation should continue.
VM::ExecResult handleGEP(VM &vm,
                         Frame &fr,
                         const Instr &in,
                         const VM::BlockMap &blocks,
                         const BasicBlock *&bb,
                         size_t &ip) {
    (void)blocks;
    (void)bb;
    (void)ip;
    if (in.operands.size() < 2)
        return inline_impl::trapMalformedMemoryInstruction(
            in, fr, bb, "gep missing base or offset operand");

    Slot base = VMAccess::eval(vm, fr, in.operands[0]);
    Slot offset = VMAccess::eval(vm, fr, in.operands[1]);

    Slot out{};
    if (base.ptr == nullptr) {
        if (offset.i64 == 0) {
            out.ptr = nullptr;
            ops::storeResult(fr, in, out);
            return {};
        }
    }

    const std::uintptr_t baseAddr = reinterpret_cast<std::uintptr_t>(base.ptr);
    const int64_t delta = offset.i64;
    const std::uint64_t magnitude =
        delta >= 0 ? static_cast<std::uint64_t>(delta)
                   : static_cast<std::uint64_t>(-(delta + 1)) + std::uint64_t{1};

    // NOTE: GEP intentionally allows out-of-bounds pointer construction (including
    // wrapping arithmetic). This matches LLVM GEP semantics where creating an
    // out-of-bounds pointer is defined behavior, but dereferencing it is not.
    // The IL spec permits pointer arithmetic to wrap around address space boundaries.
    // See test_vm_gep_nullptr.cpp for validation of this behavior.
    std::uintptr_t resultAddr = baseAddr;
    if (delta >= 0) {
        resultAddr += static_cast<std::uintptr_t>(magnitude);
    } else {
        resultAddr -= static_cast<std::uintptr_t>(magnitude);
    }

    out.ptr = reinterpret_cast<void *>(resultAddr);
    ops::storeResult(fr, in, out);
    return {};
}

/// @brief Handle the @c addr.of opcode by reifying runtime string pointers.
/// @details Forwards the pointer held in the operand slot to the destination
///          without modification, allowing IL to reference immutable runtime
///          strings.  The handler relies on the runtime to guarantee the operand
///          points to a valid string payload.
/// @param vm Virtual machine instance executing the instruction.
/// @param fr Active frame storing operands and results.
/// @param in Instruction referencing the string operand and destination slot.
/// @param blocks Map of basic blocks for the current function (unused).
/// @param bb Reference to the current block pointer (unused).
/// @param ip Instruction index within the block (unused).
/// @return Execution result indicating whether interpretation should continue.
VM::ExecResult handleAddrOf(VM &vm,
                            Frame &fr,
                            const Instr &in,
                            const VM::BlockMap &blocks,
                            const BasicBlock *&bb,
                            size_t &ip) {
    (void)blocks;
    (void)bb;
    (void)ip;
    Slot tmp = VMAccess::eval(vm, fr, in.operands[0]);
    Slot out{};
    out.ptr = tmp.str;
    ops::storeResult(fr, in, out);
    return {};
}

/// @brief Handle the @c const.str opcode by materialising constant string slots.
/// @details Copies the evaluated operand directly into the destination without
///          further transformation, providing a convenient way to expose runtime
///          string handles to subsequent instructions.
/// @param vm Virtual machine instance executing the instruction.
/// @param fr Active frame storing operands and results.
/// @param in Instruction describing the constant string operand.
/// @param blocks Map of basic blocks for the current function (unused).
/// @param bb Reference to the current block pointer (unused).
/// @param ip Instruction index within the block (unused).
/// @return Execution result indicating whether interpretation should continue.
VM::ExecResult handleConstStr(VM &vm,
                              Frame &fr,
                              const Instr &in,
                              const VM::BlockMap &blocks,
                              const BasicBlock *&bb,
                              size_t &ip) {
    (void)blocks;
    (void)bb;
    (void)ip;
    Slot out = VMAccess::eval(vm, fr, in.operands[0]);
    ops::storeResult(fr, in, out);
    return {};
}

/// @brief Handle the @c gaddr opcode by resolving global variable addresses.
/// @details Evaluates the global name operand and returns the address of the
///          mutable global storage, enabling load/store operations on
///          module-level variables.
/// @param vm Virtual machine instance executing the instruction.
/// @param fr Active frame storing operands and results.
/// @param in Instruction describing the global name operand.
/// @param blocks Map of basic blocks for the current function (unused).
/// @param bb Reference to the current block pointer (unused).
/// @param ip Instruction index within the block (unused).
/// @return Execution result indicating whether interpretation should continue.
VM::ExecResult handleGAddr(VM &vm,
                           Frame &fr,
                           const Instr &in,
                           const VM::BlockMap &blocks,
                           const BasicBlock *&bb,
                           size_t &ip) {
    (void)blocks;
    (void)bb;
    (void)ip;
    Slot out = VMAccess::eval(vm, fr, in.operands[0]);
    ops::storeResult(fr, in, out);
    return {};
}

/// @brief Handle the @c const.null opcode by writing a type-appropriate null.
/// @details Inspects the destination type to determine which slot member should
///          receive the null value.  Pointer-like types clear the generic pointer
///          field, while string handles set the runtime string pointer to @c nullptr.
/// @param vm Virtual machine instance executing the instruction (unused).
/// @param fr Active frame storing the destination slot.
/// @param in Instruction describing the destination type.
/// @param blocks Map of basic blocks for the current function (unused).
/// @param bb Reference to the current block pointer (unused).
/// @param ip Instruction index within the block (unused).
/// @return Execution result indicating whether interpretation should continue.
VM::ExecResult handleConstNull(VM &vm,
                               Frame &fr,
                               const Instr &in,
                               const VM::BlockMap &blocks,
                               const BasicBlock *&bb,
                               size_t &ip) {
    (void)vm;
    (void)blocks;
    (void)bb;
    (void)ip;

    Slot out{};
    switch (in.type.kind) {
        case Type::Kind::I1:
        case Type::Kind::I16:
        case Type::Kind::I32:
        case Type::Kind::I64:
        case Type::Kind::Error:
        case Type::Kind::ResumeTok:
            out.ptr = nullptr;
            break;
        case Type::Kind::F64:
            out.f64 = 0.0;
            break;
        case Type::Kind::Ptr:
            out.ptr = nullptr;
            break;
        case Type::Kind::Str:
            out.str = nullptr;
            break;
        default:
            out.ptr = nullptr;
            break;
    }

    ops::storeResult(fr, in, out);
    return {};
}

/// @brief Execute the `const.f64` opcode by materializing a floating-point constant.
/// @details Evaluates the operand which contains the floating-point literal value
///          and stores it directly in the destination slot.  No conversion or
///          computation is performed; the operand is expected to already be a
///          properly-encoded f64 value.
/// @param vm Virtual machine used to evaluate the constant operand.
/// @param fr Active frame providing destination storage.
/// @param in Instruction describing the constant.
/// @param blocks Map of basic blocks for the current function (unused).
/// @param bb Reference to the current basic block pointer (unused).
/// @param ip Instruction index within @p bb (unused).
/// @return Execution result signalling the interpreter should continue.
VM::ExecResult handleConstF64(VM &vm,
                              Frame &fr,
                              const Instr &in,
                              const VM::BlockMap &blocks,
                              const BasicBlock *&bb,
                              size_t &ip) {
    (void)blocks;
    (void)bb;
    (void)ip;

    Slot value = VMAccess::eval(vm, fr, in.operands[0]);
    Slot out{};
    out.f64 = value.f64;
    ops::storeResult(fr, in, out);
    return {};
}
} // namespace il::vm::detail::memory
