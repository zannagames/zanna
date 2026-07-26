//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: vm/tco.hpp
// Purpose: Tail-call optimisation helper for reusing the current frame.
//
// Key invariants:
//   - ZANNA_VM_TAILCALL must be defined and non-zero for TCO to apply.
//   - The callee function must have a non-empty blocks list with a valid entry.
//   - Argument count must exactly match the entry block's parameter count.
//   - The current execution state must be valid (non-null ExecState).
//   - EH stack and resume state are preserved across the tail call.
//   - String arguments are retained before storing, old strings released.
//   - After successful TCO, execution continues at callee's entry block.
//
// Ownership/Lifetime:
//   - Modifies the provided ExecState in place; no new frame allocation.
//   - Uses VM's register count cache to avoid rescanning function structure.
//   - String slots follow retain/release ownership semantics.
//
// When TCO fails:
//   - Returns false without modifying state.
//   - Caller should fall back to regular call semantics.
//
// Links: docs/internals/architecture.md, docs/internals/vm.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares eligibility checking and in-place frame reuse for VM tail calls.
/// @details Successful optimization preserves exception/resume state, replaces
///          the active function and cached block/operand views, retains string
///          arguments, and resumes at the callee entry without growing the
///          execution stack.

#pragma once

#include <span>

#include "vm/VM.hpp"

namespace il::vm {

/// @brief Try to perform a tail call by reusing the current frame.
///
/// @details When ZANNA_VM_TAILCALL is enabled, this function attempts to
///          reuse the current execution frame for the callee function,
///          avoiding stack growth for tail-recursive patterns. The frame's
///          register file is resized (using cached size when available),
///          the cached block map is selected, and execution resumes at the callee's
///          entry block.
///
///          TCO preserves the current exception handler stack and resume
///          state, ensuring that error handling works correctly across
///          tail-call boundaries.
///
/// @pre     The VM must have an active execution state.
/// @pre     The callee must have at least one basic block.
/// @pre     args.size() must equal the entry block's parameter count.
///
/// @post    On success: frame reused, IP set to callee's entry, returns true.
/// @post    On failure: no state modified, returns false.
///
/// @param vm     Owning VM used to locate the current execution state.
/// @param callee Function to enter via tail call.
/// @param args   Evaluated argument slots for the callee's entry parameters.
/// @return       true if the frame was reused and control transferred to callee.
bool tryTailCall(VM &vm, const il::core::Function *callee, std::span<const Slot> args);

} // namespace il::vm
