//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/Scheduler.hpp
// Purpose: Declare the post-RA x86-64 instruction scheduler.
// Key invariants:
//   - Must be called after register allocation when physical registers are known.
//   - Reordering preserves data dependences and control-flow semantics.
// Ownership/Lifetime:
//   - Stateless; mutates caller-owned MFunction/MIR vectors in place.
// Links: src/codegen/x86_64/Scheduler.cpp,
//        src/codegen/x86_64/MachineIR.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "MachineIR.hpp"

#include <cstddef>
#include <vector>

/**
 * @file
 * @brief Declares conservative post-allocation list scheduling for x86-64 MIR.
 *
 * Scheduling operates only inside straight-line segments, preserving explicit
 * and implicit register dependencies, EFLAGS, potentially aliasing memory
 * accesses, control-flow boundaries, and Win64 unwind-sensitive prologue saves.
 */

namespace zanna::codegen::x64 {

/// @brief Run conservative post-RA scheduling for a single function.
/// @param fn Machine function whose basic blocks are reordered in place.
/// @return Number of basic-block segments whose instruction order changed.
std::size_t scheduleFunction(MFunction &fn);

/// @brief Run post-RA scheduling for an entire MIR module.
/// @param mir Module's vector of machine functions to schedule.
/// @return Total number of reordered segments across all functions.
std::size_t scheduleModule(std::vector<MFunction> &mir);

} // namespace zanna::codegen::x64
