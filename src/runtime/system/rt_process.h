//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/system/rt_process.h
// Purpose: Streaming child-process handles for Zanna.System.Process.
//
// Key invariants:
//   - Direct process startup never invokes a command shell.
//   - Child stdin, stdout, and stderr are redirected through runtime-owned pipes.
//   - Polling and reads are incremental; process completion is retained once observed.
//   - A non-NULL environment sequence replaces the complete child environment.
//   - Each output stream retains at most 16 MiB between reads and reports truncation.
//
// Ownership/Lifetime:
//   - Start functions return GC-managed process objects.
//   - Destroy is idempotent, terminates a live child, and invalidates the handle.
//   - Returned strings and result maps are new caller-owned runtime values.
//
// Links: src/runtime/system/rt_process.c (implementation)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_process.h
 * @brief Declares streaming, cancellable managed child-process operations.
 * @details Process handles launch direct argument vectors with optional cwd
 *          and environment replacement, expose incremental standard-stream
 *          I/O and retained exit status, report bounded-output truncation, and
 *          support termination plus idempotent destruction.
 */

#pragma once

#include "rt_string.h"

#include <stdint.h>

/// @brief Runtime class identifier assigned to Zanna.System.Process handles.
#define RT_PROCESS_CLASS_ID INT64_C(-0x440201)

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Start a child process with inherited cwd and environment.
/// @details Bypasses the command shell, passes arguments literally, redirects
///          all three standard streams, and starts with the parent's current
///          directory and environment.
/// @param program Executable path or lookup name; must be nonempty and contain
///        no embedded NUL byte.
/// @param args Optional Seq of strings; each element must contain no embedded
///        NUL byte.
/// @return New GC-managed process handle, or NULL after validation or startup
///         failure.
void *rt_process_start(rt_string program, void *args);

/// @brief Start a child process in a working directory with inherited environment.
/// @details Bypasses the command shell and redirects stdin, stdout, and stderr.
/// @param program Executable path or lookup name; must be nonempty and contain
///        no embedded NUL byte.
/// @param args Optional Seq of literal argument strings without embedded NUL.
/// @param cwd Working directory without embedded NUL, or NULL/empty to inherit.
/// @return New GC-managed process handle, or NULL after validation or startup
///         failure.
void *rt_process_start_in(rt_string program, void *args, rt_string cwd);

/// @brief Start a child process with an explicit cwd and environment.
/// @details Bypasses the command shell and redirects all standard streams. A
///          non-NULL @p env replaces the complete environment rather than
///          merging with inherited values.
/// @param program Executable path or lookup name; must be nonempty and contain
///        no embedded NUL byte.
/// @param args Optional Seq of literal argument strings without embedded NUL.
/// @param cwd Working directory without embedded NUL, or NULL/empty to inherit.
/// @param env Optional Seq of nonempty NAME=VALUE strings without embedded NUL;
///        NULL means inherit and an empty Seq requests an empty environment.
/// @return New GC-managed process handle, or NULL after validation or startup
///         failure.
void *rt_process_start_with_env(rt_string program, void *args, rt_string cwd, void *env);

/// @brief Test whether @p handle is a started, undestroyed Process object.
/// @details A handle remains valid after child exit so its status and buffered
///          output can be consumed until destruction.
/// @param handle Candidate opaque runtime object.
/// @return 1 for a valid process handle, otherwise 0.
int64_t rt_process_is_valid(void *handle);

/// @brief Nonblockingly drain output and poll for process completion.
/// @param handle Candidate process handle.
/// @return 1 while the child is still running, otherwise 0.
int64_t rt_process_poll(void *handle);

/// @brief Poll and report whether the process is still running.
/// @param handle Candidate process handle.
/// @return 1 while running, otherwise 0 for exited or invalid handles.
int64_t rt_process_is_running(void *handle);

/// @brief Read currently buffered stdout plus any immediately available bytes.
/// @details Consumes the incremental buffer. If it overflowed its 16 MiB cap,
///          the retained prefix is returned after raising a truncation trap.
/// @param handle Candidate process handle.
/// @return Newly allocated stdout chunk, or an empty string when no bytes are
///         available or the handle is invalid.
rt_string rt_process_read_stdout(void *handle);

/// @brief Read stdout as `{ text, truncated }` without trapping on output truncation.
/// @details Consumes the incremental buffer and reports whether bytes beyond
///          its 16 MiB cap were discarded.
/// @param handle Candidate process handle.
/// @return Caller-owned map with @c text and @c truncated entries, an empty
///         nontruncated map for an invalid handle, or NULL on map allocation failure.
void *rt_process_read_stdout_result(void *handle);

/// @brief Read currently buffered stderr plus any immediately available bytes.
/// @details Consumes the incremental buffer. If it overflowed its 16 MiB cap,
///          the retained prefix is returned after raising a truncation trap.
/// @param handle Candidate process handle.
/// @return Newly allocated stderr chunk, or an empty string when no bytes are
///         available or the handle is invalid.
rt_string rt_process_read_stderr(void *handle);

/// @brief Read stderr as `{ text, truncated }` without trapping on output truncation.
/// @details Consumes the incremental buffer and reports whether bytes beyond
///          its 16 MiB cap were discarded.
/// @param handle Candidate process handle.
/// @return Caller-owned map with @c text and @c truncated entries, an empty
///         nontruncated map for an invalid handle, or NULL on map allocation failure.
void *rt_process_read_stderr_result(void *handle);

/// @brief Write bytes to the child's stdin pipe.
/// @details Honors the runtime-string byte length, including embedded NUL.
///          POSIX stdin is nonblocking, so a failure after progress can return a
///          partial count.
/// @param handle Candidate process handle with an open stdin pipe.
/// @param data Bytes to write; NULL is treated as empty.
/// @return Bytes written, zero for empty input, a positive partial count after
///         later failure, or -1 when no byte can be written.
int64_t rt_process_write_stdin(void *handle, rt_string data);

/// @brief Poll and return the retained process exit status.
/// @param handle Candidate process handle.
/// @return Normal exit code, negated signal number on POSIX, or -1 while
///         running, after status failure, or for an invalid handle.
int64_t rt_process_exit_code(void *handle);

/// @brief Request process termination.
/// @details Sends SIGTERM on POSIX or requests Windows termination with status
///          1. This call does not wait for completion.
/// @param handle Candidate process handle.
/// @return 1 when the OS accepted the request, otherwise 0.
int64_t rt_process_kill(void *handle);

/// @brief Wait for process exit while draining stdout and stderr.
/// @param handle Candidate process handle.
/// @return Normal exit code, negated signal number on POSIX, or -1 for an
///         invalid handle or wait/status failure.
int64_t rt_process_wait(void *handle);

/// @brief Terminate if necessary and release all resources held by a handle.
/// @details Idempotent. POSIX allows a short SIGTERM grace period before
///          SIGKILL; Windows terminates and waits. The object remains allocated
///          but becomes invalid for subsequent operations.
/// @param handle Candidate process handle; invalid values are ignored.
void rt_process_destroy(void *handle);

#ifdef __cplusplus
}
#endif
