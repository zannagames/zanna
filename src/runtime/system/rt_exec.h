//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/system/rt_exec.h
// Purpose: External command execution for Zanna.System.Exec, providing Run, Capture, and Shell
// variants with argument arrays and exit code capture.
//
// Key invariants:
//   - Run/RunArgs wait for process completion and return the exit code.
//   - Capture/CaptureArgs capture stdout as a string; stderr remains inherited.
//   - Shell/ShellCapture pass the command string directly to the platform shell.
//   - Program, command, and argument strings containing embedded NUL bytes are
//     rejected before an operating-system process API sees them.
//   - SECURITY: Never pass unsanitized user input to Shell functions; shell injection risk.
//
// Ownership/Lifetime:
//   - Returned strings from Capture functions are newly allocated; caller must release.
//   - Run/RunArgs may allocate temporary launch data but retain no state after
//     the child has been reaped.
//   - ShellResult returns an owned object whose accessors expose retained data.
//
// Links: src/runtime/system/rt_exec.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_exec.h
 * @brief Declares synchronous external-command execution services.
 * @details The API distinguishes direct literal argument-vector launch from
 *          caller-authorized shell interpretation, supports bounded stdout
 *          capture and exit-status reporting, and returns caller-owned managed
 *          strings or command-result objects.
 */

#pragma once

#include "rt_string.h"

#include <stdint.h>

/// @brief Runtime class identifier assigned to Zanna.System.CommandResult.
#define RT_EXEC_COMMAND_RESULT_CLASS_ID INT64_C(-0x440203)

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Execute a program directly and wait for it to finish.
/// @details Resolves @p program according to the platform process-launch rules,
///          bypasses the command shell, passes no additional arguments, and
///          inherits the current environment and standard streams.
/// @param program Executable path or lookup name. Must not be NULL, empty, or
///        contain an embedded NUL byte.
/// @return Child exit code; on POSIX, the negated terminating signal number; or
///         -1 when validation, launch, waiting, or status retrieval fails.
int64_t rt_exec_run(rt_string program);

/// @brief Execute a program directly and capture its standard output.
/// @details Captures at most 16 MiB. The child inherits stdin and stderr; no
///          command shell interprets @p program.
/// @param program Executable path or lookup name. Must not be NULL, empty, or
///        contain an embedded NUL byte.
/// @return Newly allocated runtime string containing stdout, or a newly
///         allocated empty string on launch/setup failure. Capture errors or
///         truncation raise a runtime trap.
rt_string rt_exec_capture(rt_string program);

/// @brief Execute a program with a direct argument vector and wait.
/// @details Argument strings are passed literally without expansion, quoting,
///          or shell interpretation, making this the preferred interface for
///          untrusted argument values.
/// @param program Executable path or lookup name. Must not be NULL, empty, or
///        contain an embedded NUL byte.
/// @param args Optional Seq of runtime strings. NULL means no arguments; each
///        element must be representable without an embedded NUL byte.
/// @return Child exit code; on POSIX, the negated terminating signal number; or
///         -1 when validation, launch, waiting, or status retrieval fails.
int64_t rt_exec_run_args(rt_string program, void *args);

/// @brief Execute a program with literal arguments and capture stdout.
/// @details Bypasses the shell and captures at most 16 MiB. Stdin and stderr
///          remain inherited by the child.
/// @param program Executable path or lookup name. Must not be NULL, empty, or
///        contain an embedded NUL byte.
/// @param args Optional Seq of runtime strings. NULL means no arguments; each
///        element must be representable without an embedded NUL byte.
/// @return Newly allocated runtime string containing stdout, or a newly
///         allocated empty string on validation, setup, or launch failure.
///         Capture errors or truncation raise a runtime trap.
rt_string rt_exec_capture_args(rt_string program, void *args);

/// @brief Execute a command through the system shell.
/// @details Supports platform-shell syntax such as pipelines, substitutions,
///          wildcards, and redirections. This call does not update the
///          rt_exec_last_exit_code() compatibility slot.
/// @param command Shell command text. Must not be NULL or contain an embedded
///        NUL byte; an empty command succeeds without launching a shell.
/// @return Normalized shell exit code, zero for an empty command, or -1 when
///         the shell cannot start or terminates abnormally.
/// @warning Do not pass unsanitized user input; shell metacharacters are active.
int64_t rt_exec_shell(rt_string command);

/// @brief Execute a shell command and capture stdout.
/// @details Uses the platform command processor through popen and captures at
///          most 16 MiB. Stderr is inherited unless @p command redirects or
///          merges it. After a successfully opened pipe is closed, the decoded
///          status is available through rt_exec_last_exit_code().
/// @param command Shell command text. Must not be NULL or contain an embedded
///        NUL byte; an empty command returns an empty string without changing
///        the compatibility exit-code slot.
/// @return Newly allocated runtime string containing stdout, or a newly
///         allocated empty string for an empty command or launch failure.
///         Capture errors or truncation raise a runtime trap.
/// @warning Do not pass unsanitized user input; shell metacharacters are active.
rt_string rt_exec_shell_capture(rt_string command);

/// @brief Execute a shell command, capture stdout, and record the exit code
///        for retrieval via rt_exec_last_exit_code().
/// @details Uses the platform command processor through popen, captures at most
///          16 MiB, and publishes zero for an empty command or -1 when the pipe
///          cannot be opened. Stderr is inherited unless the command redirects
///          it using syntax supported by that platform shell.
/// @param command Shell command text. Must not be NULL or contain an embedded
///        NUL byte.
/// @return Newly allocated runtime string containing stdout, or a newly
///         allocated empty string for an empty command or failure.
/// @note The compatibility exit code is thread-local and is overwritten by the
///       next ShellCapture, ShellFull, or ShellResult completion on that thread.
/// @warning Do not pass unsanitized user input; shell metacharacters are active.
rt_string rt_exec_shell_full(rt_string command);

/// @brief Execute a shell command and return output plus exit code together.
/// @details This is the side-channel-free replacement for calling
///          rt_exec_shell_full() followed by rt_exec_last_exit_code(). The
///          returned object owns the captured string and remains internally
///          consistent even if later calls replace the thread-local status.
/// @param command Shell command text subject to rt_exec_shell_full() validation
///        and platform-shell interpretation.
/// @return Caller-owned opaque Zanna.System.CommandResult containing stdout and
///         status, or NULL after an object-allocation trap.
/// @warning Do not pass unsanitized user input; shell metacharacters are active.
void *rt_exec_shell_result(rt_string command);

/// @brief Read the current thread's most recently recorded shell-capture status.
/// @details ShellCapture records a status after pclose. ShellFull additionally
///          records explicit empty-command and launch-failure outcomes, and
///          ShellResult delegates to ShellFull. Direct execution and Shell do
///          not change this slot.
/// @return Most recently recorded normalized exit code, or -1 before any value
///         has been recorded or after a ShellFull launch failure.
int64_t rt_exec_last_exit_code(void);

/// @brief Acquire the captured stdout from a CommandResult.
/// @param result Candidate opaque Zanna.System.CommandResult object.
/// @return Retained runtime string containing stdout; the caller must release
///         it. Invalid objects trap and produce an owned empty fallback string.
rt_string rt_exec_command_result_output(void *result);

/// @brief Read the exit status stored in a CommandResult.
/// @param result Candidate opaque Zanna.System.CommandResult object.
/// @return Stored process exit code, or -1 after trapping for invalid input.
int64_t rt_exec_command_result_exit_code(void *result);

/// @brief Test whether a CommandResult stores a successful exit status.
/// @param result Candidate opaque Zanna.System.CommandResult object.
/// @return 1 exactly when the object is valid and its status is zero; otherwise
///         0. Invalid objects also raise a runtime trap.
int8_t rt_exec_command_result_succeeded(void *result);

#ifdef __cplusplus
}
#endif
