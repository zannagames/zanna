//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/vm_executor.cpp
// Purpose: Standardise how CLI tools run an IL module on the bytecode VM,
//          including bytecode compilation, runtime argument setup, and trap
//          reporting.
// Key invariants: Bytecode is produced via compileChecked() so trusted dispatch
//                 is safe; runtime args are reset before each run so a stale
//                 host argv never leaks into a subsequent execution.
// Ownership/Lifetime: The caller owns the IL module; bytecode and VM state are
//                     local to the call. rt_string temporaries are unref'd
//                     immediately after being pushed to the runtime arg list.
// Links: src/tools/common/vm_executor.hpp, src/bytecode/BytecodeVM.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Shared bytecode-VM execution helper for the language frontend tools.
/// @details Wraps the compile-to-bytecode, configure-VM, run-"main", and
///          trap-handling sequence behind a single call so each frontend tool
///          executes IL identically.

#include "tools/common/vm_executor.hpp"

#include "bytecode/BytecodeCompiler.hpp"
#include "bytecode/BytecodeVM.hpp"
#include "runtime/core/rt_args.h"
#include "runtime/core/rt_string.h"
#include "support/diag_expected.hpp"

#include <cstdio>
#include <iostream>
#include <limits>

namespace il::tools::common {

namespace {

/// @brief Clears the C runtime argv bridge for the lifetime of a bytecode VM execution.
/// @details Construction suppresses the host-process argv fallback before tool-supplied
/// arguments are pushed; destruction clears those arguments even if execution traps or returns
/// through an error path.
class RuntimeArgsScope {
  public:
    /// @brief Clear host/runtime arguments on entry to one execution.
    RuntimeArgsScope() {
        rt_args_clear();
    }

    /// @brief Clear tool-supplied arguments on every scope exit path.
    ~RuntimeArgsScope() {
        rt_args_clear();
    }

    RuntimeArgsScope(const RuntimeArgsScope &) = delete;
    RuntimeArgsScope &operator=(const RuntimeArgsScope &) = delete;
};

} // namespace

/// @brief Execute an IL module on the bytecode VM.
/// @details Compiles @p module with BytecodeCompiler::compileChecked (so the
///          interpreter can run with trusted dispatch); a compile failure is
///          returned with @c compileFailed set and the diagnostic optionally
///          printed. Runtime arguments are reset via rt_args_clear() and then
///          repopulated from @c config.programArgs — an empty list deliberately
///          suppresses the native host-argv fallback. The VM is configured with
///          threaded dispatch, the caller's trusted-dispatch preference, the
///          runtime bridge enabled, and the @c maxSteps instruction budget, then
///          the "main" function is run. A trap sets @c trapped, captures the trap
///          message, and yields exit code 1. Otherwise the exit code is main's
///          return value, unless it falls outside the host @c int range — in
///          which case @c exitCodeOutOfRange is set and the exit code is 1.
///          stdout is flushed when requested.
/// @param module The IL module to execute.
/// @param config Execution configuration (arguments, trap output, flush, etc.).
/// @return Execution result including exit code and trap/compile status.
VMExecutorResult executeBytecodeVM(const il::core::Module &module, const VMExecutorConfig &config) {
    VMExecutorResult result;

    // Compile IL to bytecode
    zanna::bytecode::BytecodeCompiler bcCompiler;
    auto compiled = bcCompiler.compileChecked(module, config.sourceManager);
    if (!compiled) {
        result.compileFailed = true;
        result.trapped = false;
        result.exitCode = 1;
        result.trapMessage = compiled.error().message;
        if (config.outputCompileDiagnostics) {
            il::support::printDiag(compiled.error(), std::cerr, config.sourceManager);
        }
        if (config.flushStdout) {
            std::fflush(stdout);
        }
        return result;
    }
    zanna::bytecode::BytecodeModule bcModule = std::move(compiled.value());

    // Set up program arguments for the runtime. An empty forwarded argument
    // list is meaningful: it must suppress the native host argv fallback.
    RuntimeArgsScope runtimeArgs;
    for (const auto &s : config.programArgs) {
        rt_string tmp = rt_string_from_bytes(s.data(), s.size());
        rt_args_push(tmp);
        rt_string_unref(tmp);
    }

    // Configure and run the VM
    zanna::bytecode::BytecodeVM bcVm;
    bcVm.setThreadedDispatch(true);
    bcVm.setTrustedDispatch(config.trustedDispatch);
    bcVm.setRuntimeBridgeEnabled(true);
    bcVm.setMaxInstructions(config.maxSteps);
    bcVm.load(&bcModule);

    zanna::bytecode::BCSlot bcResult = bcVm.exec("main", {});

    // Handle results
    if (bcVm.state() == zanna::bytecode::VMState::Trapped) {
        result.trapped = true;
        result.trapMessage = bcVm.trapMessage();
        result.exitCode = 1;

        if (config.outputTrapMessage) {
            std::cerr << result.trapMessage << "\n";
        }
    } else {
        const auto intMin = static_cast<int64_t>(std::numeric_limits<int>::min());
        const auto intMax = static_cast<int64_t>(std::numeric_limits<int>::max());
        if (bcResult.i64 < intMin || bcResult.i64 > intMax) {
            result.exitCodeOutOfRange = true;
            result.exitCode = 1;
            result.trapMessage = "program return value " + std::to_string(bcResult.i64) +
                                 " outside host int range [" + std::to_string(intMin) + ", " +
                                 std::to_string(intMax) + "]";
            if (config.outputTrapMessage) {
                std::cerr << result.trapMessage << "\n";
            }
        } else {
            result.exitCode = static_cast<int>(bcResult.i64);
        }
    }

    if (config.flushStdout) {
        std::fflush(stdout);
    }

    return result;
}

} // namespace il::tools::common
