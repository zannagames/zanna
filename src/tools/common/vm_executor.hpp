//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tools/common/vm_executor.hpp
// Purpose: Shared helpers for bytecode VM execution used by language frontend CLI tools.
// Key invariants: Encapsulates BytecodeVM setup, execution, and trap handling.
// Ownership/Lifetime: The caller owns the IL module; the executor manages internal VM state.
// Links: docs/internals/architecture.md
//
//===----------------------------------------------------------------------===//

#pragma once

/// @file
/// @brief Declares the shared checked-bytecode execution wrapper for CLI tools.

#include "il/core/Module.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace il::support {
class SourceManager;
}

namespace il::tools::common {

/// @brief Configuration for bytecode VM execution.
struct VMExecutorConfig {
    /// @brief Program arguments to pass to the runtime via rt_args.
    std::vector<std::string> programArgs;

    /// @brief Whether to output trap messages to stderr on trap.
    bool outputTrapMessage{true};

    /// @brief Whether to print bytecode compile diagnostics before execution.
    bool outputCompileDiagnostics{true};

    /// @brief Whether to flush stdout after execution.
    bool flushStdout{false};

    /// @brief Optional source manager for bytecode compile diagnostics and trap locations.
    const il::support::SourceManager *sourceManager{nullptr};

    /// @brief Maximum bytecode instructions to dispatch before trapping (0 = unlimited).
    std::uint64_t maxSteps{0};

    /// @brief Skip per-instruction interpreter validation after checked bytecode compilation.
    bool trustedDispatch{true};
};

/// @brief Result of bytecode VM execution.
struct VMExecutorResult {
    /// @brief Exit code from the program (1 when trapped or when the return value is invalid).
    int exitCode{0};

    /// @brief True if the VM trapped during execution.
    bool trapped{false};

    /// @brief Trap message if trapped is true.
    std::string trapMessage;

    /// @brief True when bytecode compilation failed before execution.
    bool compileFailed{false};

    /// @brief True when main returned a value outside the host int exit-code range.
    bool exitCodeOutOfRange{false};
};

/// @brief Execute an IL module using the bytecode VM.
///
/// Compiles the IL module to bytecode, sets up runtime arguments if provided,
/// executes the "main" function, and handles any traps that occur.
///
/// @param module The IL module to execute.
/// @param config Execution configuration (arguments, trap output, etc.).
/// @return Execution result including exit code and trap information.
VMExecutorResult executeBytecodeVM(const il::core::Module &module, const VMExecutorConfig &config);

} // namespace il::tools::common
