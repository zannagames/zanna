//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/common/RunProcess.hpp
// Purpose: Declare cross-platform process execution helpers for CLI utilities.
// Key invariants: RunResult captures exit status and independent stdout/stderr text.
// Ownership/Lifetime: Callers own argument buffers; helper copies command text as needed.
// Links: src/common/RunProcess.cpp, docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/**
 * @file RunProcess.hpp
 * @brief Declares cross-platform direct and shell-mediated subprocess launch.
 *
 * Commands use UTF-8 strings, optional child-only environment overrides, and
 * an optional working directory. Standard output and standard error are
 * captured separately in an owning RunResult.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

/// @brief Result of launching a subprocess.
/// @details The legacy @ref exit_code field remains available for existing
///          call sites.  Launch/setup failures are reported with
///          @ref launch_failed set to true and @ref exit_code set to -1.
///          Normal process termination sets @ref launched to true and stores
///          the native exit code in @ref native_exit_code. POSIX signal
///          termination is represented as `128 + signal`, matching
///          @ref exit_code. On Windows, native exit codes larger than
///          @c INT_MAX are saturated in @ref exit_code so they cannot collide
///          with the -1 launch-failure sentinel; callers that need the exact
///          value should read @ref native_exit_code. Capture or wait
///          diagnostics may be appended to @ref err after a successful launch.
///          If capture storage or native pipe I/O fails, @ref capture_failed is
///          set while the child is still drained/terminated and reaped.
struct RunResult {
    int exit_code{0}; ///< Normalised process exit code (or -1 on launch failure).
    std::string out;  ///< Captured standard output text.
    /// Captures stderr independently; launcher diagnostics may also be appended.
    std::string err;            ///< Captured standard error text (may be merged with stdout).
    bool launched{false};       ///< True once the host successfully starts a child process.
    bool launch_failed{false};  ///< True when setup or process creation failed.
    bool capture_failed{false}; ///< True when stdout/stderr could not be captured completely.
    std::uint32_t native_exit_code{0}; ///< Exact host exit code when available.
};

/// @brief Spawn a subprocess using the provided argument vector.
/// @details Launches @p argv directly without shell parsing. Standard output
///          and standard error are captured independently. Environment
///          overrides are applied only to the child; duplicate keys use the
///          final supplied value and empty keys are ignored. A supplied
///          working directory must already exist and be a directory.
/// @param argv Command-line arguments including the executable at index zero.
/// @param cwd Optional working directory to set before launching the process.
/// @param env Environment variable overrides expressed as key/value pairs.
/// @return Captured process result including exit code and output streams.
///         Empty argv, invalid launch configuration, or process-creation
///         failure returns `exit_code == -1` and `launch_failed == true`.
///         Incomplete stdout/stderr capture sets `capture_failed == true`
///         independently of the child's exit status.
/// @note POSIX executable lookup follows `posix_spawnp`; Windows delegates
///       executable resolution to `CreateProcessW`.
RunResult run_process(const std::vector<std::string> &argv,
                      std::optional<std::string> cwd = std::nullopt,
                      const std::vector<std::pair<std::string, std::string>> &env = {});

/// @brief Spawn a subprocess through the host shell explicitly.
/// @details Invokes `cmd /C` on Windows and `sh -c` elsewhere. Shell expansion,
///          quoting, redirection, and injection semantics therefore apply to
///          @p command.
/// @param command Shell command text.
/// @param cwd Optional working directory to set before launching the process.
/// @param env Environment variable overrides expressed as key/value pairs.
/// @return Captured process result including exit code and output streams.
/// @see run_process
RunResult run_shell_command(const std::string &command,
                            std::optional<std::string> cwd = std::nullopt,
                            const std::vector<std::pair<std::string, std::string>> &env = {});
