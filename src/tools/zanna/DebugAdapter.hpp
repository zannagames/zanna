//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file DebugAdapter.hpp
/// @brief Declares the VM-backed interactive source debugger used by Zanna Studio.
///
/// The adapter emits newline-delimited JSON on stderr with an @c "@@VDBG@@ " sentinel, leaving
/// debuggee output untouched. It accepts breakpoint, launch, execution-control, pause, evaluate,
/// variables, and termination commands as newline-delimited JSON on stdin.
///
/// A run borrows its verified module and source manager and owns the supplied debug-layout table.
//
//===----------------------------------------------------------------------===//
#pragma once

#include "zanna/vm/debug/DebugClassLayout.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace il::core {
struct Module;
}
namespace il::support {
class SourceManager;
}

namespace il::tools::debug {

/// @brief Run @p module under the VM as an interactive debug adapter.
/// @param module Lowered IL module to execute (must already be verified).
/// @param programArgs Arguments passed to the debuggee.
/// @param maxSteps Optional VM step limit (0 = unlimited).
/// @param sm Source manager mapping file ids to paths (for stop locations).
/// @param debugLayouts Class-layout sidecar from the module's own compile so
///        stops can expand user class instances field-by-field (ADR 0138);
///        pass empty when unavailable (direct IL, BASIC) to keep leaves.
/// @return The debuggee's exit code.
/// @note The adapter terminates immediately if its controlling input channel closes while stopped.
int runDebugAdapter(il::core::Module &module,
                    const std::vector<std::string> &programArgs,
                    uint64_t maxSteps,
                    il::support::SourceManager &sm,
                    il::vm::DebugClassLayoutTable debugLayouts = {});

} // namespace il::tools::debug
