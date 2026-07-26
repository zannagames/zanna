//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: vm/DispatchStrategy.hpp
// Purpose: Define interface for pluggable VM dispatch strategies.
//
// Overview:
// The VM supports three dispatch strategies for executing opcodes:
//
//   1. FnTable (Function Table)
//      - Uses vm/ops/generated/HandlerTable.hpp → getOpcodeHandlers()
//      - Resolves opcode to handler via array index: table[opcode]
//      - Portable, moderate performance
//
//   2. Switch (Switch Statement)
//      - Uses vm/ops/generated/SwitchDispatchImpl.inc
//      - switch(instr.op) with case per opcode calling inline_handle_*
//      - Portable fallback, handles finalization internally
//
//   3. Threaded (Computed Goto)
//      - Uses vm/ops/generated/ThreadedLabels.inc + ThreadedCases.inc
//      - goto *kOpLabels[opcode] with LBL_* labels
//      - Fastest dispatch, GCC/Clang only (ZANNA_THREADING_SUPPORTED)
//
// Strategy Selection:
// - Environment: ZANNA_DISPATCH=threaded|switch|table
// - API: VM constructor DispatchKind parameter
// - Default: Threaded if supported, otherwise Switch
//
// Generated Files:
// All strategies rely on tables synchronized with il/core/Opcode.def.
// See docs/internals/generated-files.md for regeneration instructions.
//
// Key invariants: Each strategy only handles opcode-to-handler mapping
// Ownership/Lifetime: Strategies are owned by the VM instance
// Links: docs/internals/generated-files.md, docs/il/il-guide.md#reference
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares the pluggable opcode-dispatch strategy interface and shared
///        VM execution loop.
/// @details Dispatch strategies isolate opcode-to-handler selection from the
///          common instruction lifecycle.  Implementations may use a function
///          table, a switch, or compiler-supported computed-goto threading.

#pragma once

#include "il/core/Opcode.hpp"
#include "vm/VM.hpp"

namespace il::vm {

/// @brief Abstract interface for opcode dispatch strategies.
/// @details Each concrete strategy only implements the mapping from opcode
///          to handler execution, while the shared loop handles all control flow,
///          trap handling, and debug hooks.
class DispatchStrategy {
  public:
    /// @brief Virtual destructor for proper cleanup.
    virtual ~DispatchStrategy() = default;

    /// @brief Strategy identifier for diagnostics and configuration.
    enum class Kind {
        FnTable, ///< Function table lookup
        Switch,  ///< Switch statement dispatch
        Threaded ///< Computed goto threading
    };

    /// @brief Get the kind of this strategy.
    /// @return Enumerator identifying the concrete dispatch mechanism.
    virtual Kind getKind() const = 0;

    /// @brief Execute a single instruction using this strategy.
    /// @param vm Virtual machine instance
    /// @param state Current execution state
    /// @param instr Instruction to execute
    /// @return Execution result from the handler
    virtual VM::ExecResult executeInstruction(VM &vm,
                                              VM::ExecState &state,
                                              const il::core::Instr &instr) = 0;

    /// @brief Check if this strategy requires special trap handling.
    /// @details The threaded strategy needs to catch TrapDispatchSignal
    ///          while others can let it propagate.
    /// @return @c true when the shared loop must install its trap-dispatch
    ///         catch path; @c false by default.
    virtual bool requiresTrapCatch() const {
        return false;
    }

    /// @brief Check if this strategy handles tracing and finalization internally.
    /// @details The switch strategy's inline handlers call handleInlineResult,
    ///          which traces and finalizes internally. Other strategies return
    ///          ExecResult and expect the main loop to handle finalization.
    /// @return @c true when the strategy completes finalization itself;
    ///         @c false by default.
    virtual bool handlesFinalizationInternally() const {
        return false;
    }
};

/// @brief Shared dispatch loop implementation.
/// @details This function contains the common execution loop logic that all
///          strategies share: state management, instruction selection, debug hooks,
///          trap handling, and loop control.
/// @param vm Virtual machine instance
/// @param context VM context for trap and debug handling
/// @param state Execution state being driven
/// @param strategy Dispatch strategy to use for instruction execution
/// @return @c true when dispatch terminated normally; @c false when execution
///         paused without requesting exit.
bool runSharedDispatchLoop(VM &vm,
                           VMContext &context,
                           VM::ExecState &state,
                           DispatchStrategy &strategy);

} // namespace il::vm
