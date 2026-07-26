//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/vm/OpHelpers.cpp
// Purpose: Implement the shared trap helpers declared in OpHelpers.hpp that
//          bridge interpreter failures to the runtime diagnostics system.
// Key invariants: Diagnostics always provide function and block context when
//                 available and never throw exceptions across the VM boundary.
// Ownership/Lifetime: Operates entirely on VM-managed state without persisting
//                     data, so helpers stay side-effect free aside from the
//                     emitted trap.
// Links: docs/il/il-guide.md#reference
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the shared contextual trap adapter used by VM operation
///        helpers.
/// @details The adapter enriches a trap with source, function, and block
///          metadata before handing it to the runtime diagnostics bridge.

#include "zanna/vm/internal/OpHelpers.hpp"

#include "il/core/Function.hpp"

#include <string>

namespace il::vm::internal::detail {
/// @brief Forward a VM trap to the runtime diagnostics bridge with context.
/// @details Collects the current function and basic block names—when available—
///          and invokes @ref RuntimeBridge::trap so the host runtime emits a
///          deterministic diagnostic.  The helper keeps the VM implementation
///          agnostic of how traps are surfaced while guaranteeing that
///          contextual metadata accompanies every failure.
/// @param kind High-level trap classification associated with the failure.
/// @param message Human-readable description explaining what went wrong.
/// @param instr Instruction that triggered the trap; used for source locations.
/// @param frame Current VM frame containing function metadata and registers.
/// @param block Optional pointer to the active basic block, or null when
///              unavailable.
void trapWithMessage(TrapKind kind,
                     const char *message,
                     const il::core::Instr &instr,
                     Frame &frame,
                     const il::core::BasicBlock *block) {
    const std::string functionName = frame.func ? frame.func->name : std::string();
    const std::string blockLabel = block ? block->label : std::string();
    RuntimeBridge::trap(kind, message, instr.loc, functionName, blockLabel);
}
} // namespace il::vm::internal::detail
