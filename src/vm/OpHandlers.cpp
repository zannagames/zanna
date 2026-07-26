//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/vm/OpHandlers.cpp
// Purpose: Materialise opcode-dispatch tables used by the VM execution loop.
// Key invariants: Dispatch entries mirror il::core::Opcode enumerators and
//                 handler pointers respect the metadata in il/core/Opcode.def.
// Ownership/Lifetime: Tables are static and shared across VM instances; no
//                     dynamic allocation beyond compile-time arrays.
// Links: docs/runtime-vm.md#vm-dispatch
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Materializes the generated VM opcode-handler table and the small
///        execution-state-aware handler trampolines it references.
/// @details Table ordering follows the canonical opcode registry, while load,
///          store, and core integer trampolines attach the active execution
///          state before delegating to shared implementations.

#include "vm/OpHandlers.hpp"

#include "il/core/Opcode.hpp"
#include "il/core/OpcodeInfo.hpp"
#include "vm/ops/generated/HandlerTable.hpp"

#include <array>

using namespace il::core;

namespace il::vm {
/// @brief Expose the lazily materialised opcode → handler mapping shared across
///        all VM instances.
/// @details Delegates to @ref detail::getOpcodeHandlers(), which consults the
///          declarative metadata emitted from Opcode.def so each
///          @ref il::core::Opcode enumerator reuses the dispatch handler recorded
///          alongside its definition.  The function returns a reference to a
///          process-wide table initialised on first use so subsequent calls incur
///          no rebuild cost.
/// @return Reference to the canonical opcode handler table.
const VM::OpcodeHandlerTable &VM::getOpcodeHandlers() {
    return detail::getOpcodeHandlers();
}
} // namespace il::vm

namespace il::vm::detail {
namespace memory {
/// @brief Handle load opcodes by delegating to the shared implementation.
/// @details Obtains the current execution state via
///          @ref VMAccess::currentExecState and forwards the VM, frame, and
///          instruction context to @ref handleLoadImpl.  Keeping the state lookup
///          here allows the shared implementation to remain agnostic about how
///          the dispatcher tracks execution frames while guaranteeing that loads
///          always see the most recent execution context.
/// @param vm Virtual machine coordinating execution.
/// @param fr Active frame whose registers the opcode manipulates.
/// @param in IL instruction being executed.
/// @param blocks Mapping from block labels to block definitions.
/// @param bb Reference to the pointer identifying the current block.
/// @param ip Instruction pointer index within @p bb.
/// @return Execution status from the shared load handler.
VM::ExecResult handleLoad(VM &vm,
                          Frame &fr,
                          const Instr &in,
                          const VM::BlockMap &blocks,
                          const BasicBlock *&bb,
                          size_t &ip) {
    VMAccess::ExecState *state = VMAccess::currentExecState(vm);
    return handleLoadImpl(vm, state, fr, in, blocks, bb, ip);
}

/// @brief Handle store opcodes by delegating to the shared implementation.
/// @details Mirrors @ref handleLoad but forwards to @ref handleStoreImpl after
///          resolving the current execution state.  Abstracting the state lookup
///          avoids duplicating boilerplate across opcode definitions and keeps
///          the actual handler implementations focused on memory semantics.
/// @param vm Virtual machine coordinating execution.
/// @param fr Active frame whose registers the opcode manipulates.
/// @param in IL instruction being executed.
/// @param blocks Mapping from block labels to block definitions.
/// @param bb Reference to the pointer identifying the current block.
/// @param ip Instruction pointer index within @p bb.
/// @return Execution status from the shared store handler.
VM::ExecResult handleStore(VM &vm,
                           Frame &fr,
                           const Instr &in,
                           const VM::BlockMap &blocks,
                           const BasicBlock *&bb,
                           size_t &ip) {
    VMAccess::ExecState *state = VMAccess::currentExecState(vm);
    return handleStoreImpl(vm, state, fr, in, blocks, bb, ip);
}
} // namespace memory

namespace integer {
/// @brief Dispatch integer addition by binding the current execution state.
/// @details Fetches the thread-local execution state and passes it to
///          @ref handleAddImpl so arithmetic semantics remain centralised.
///          Separating the trampoline from the implementation keeps the
///          metadata-driven dispatch table simple while ensuring that integer
///          instructions always observe up-to-date VM context.
/// @param vm Virtual machine orchestrating the program.
/// @param fr Frame executing the opcode.
/// @param in IL instruction carrying operands and result.
/// @param blocks Mapping from block labels to block definitions.
/// @param bb Reference to the pointer identifying the current block.
/// @param ip Instruction pointer index within @p bb.
/// @return Execution status from the shared addition handler.
VM::ExecResult handleAdd(VM &vm,
                         Frame &fr,
                         const Instr &in,
                         const VM::BlockMap &blocks,
                         const BasicBlock *&bb,
                         size_t &ip) {
    VMAccess::ExecState *state = VMAccess::currentExecState(vm);
    return handleAddImpl(vm, state, fr, in, blocks, bb, ip);
}

/// @brief Dispatch integer subtraction by binding the current execution state.
/// @details Resolves the thread-local execution state and forwards execution to
///          @ref handleSubImpl.  Implementing the trampoline in one place keeps
///          the main opcode table free from state-management boilerplate while
///          guaranteeing consistent state hand-off.
/// @param vm Virtual machine orchestrating the program.
/// @param fr Frame executing the opcode.
/// @param in IL instruction carrying operands and result.
/// @param blocks Mapping from block labels to block definitions.
/// @param bb Reference to the pointer identifying the current block.
/// @param ip Instruction pointer index within @p bb.
/// @return Execution status from the shared subtraction handler.
VM::ExecResult handleSub(VM &vm,
                         Frame &fr,
                         const Instr &in,
                         const VM::BlockMap &blocks,
                         const BasicBlock *&bb,
                         size_t &ip) {
    VMAccess::ExecState *state = VMAccess::currentExecState(vm);
    return handleSubImpl(vm, state, fr, in, blocks, bb, ip);
}

/// @brief Dispatch integer multiplication by binding the current execution state.
/// @details Works identically to @ref handleAdd and @ref handleSub but forwards
///          to @ref handleMulImpl, ensuring multiplication benefits from the same
///          execution-context plumbing without duplicating code.
/// @param vm Virtual machine orchestrating the program.
/// @param fr Frame executing the opcode.
/// @param in IL instruction carrying operands and result.
/// @param blocks Mapping from block labels to block definitions.
/// @param bb Reference to the pointer identifying the current block.
/// @param ip Instruction pointer index within @p bb.
/// @return Execution status from the shared multiplication handler.
VM::ExecResult handleMul(VM &vm,
                         Frame &fr,
                         const Instr &in,
                         const VM::BlockMap &blocks,
                         const BasicBlock *&bb,
                         size_t &ip) {
    VMAccess::ExecState *state = VMAccess::currentExecState(vm);
    return handleMulImpl(vm, state, fr, in, blocks, bb, ip);
}
} // namespace integer

/// @brief Expose the opcode handler table generated from the VM op schema.
/// @details The generated metadata mirrors @ref il::core::Opcode ordering so the
///          dispatch table stays consistent with the IL definition without
///          repeatedly expanding Opcode.def at build time.
/// @return Reference to the cached opcode handler table.
const VM::OpcodeHandlerTable &getOpcodeHandlers() {
    return generated::opcodeHandlers();
}

} // namespace il::vm::detail
