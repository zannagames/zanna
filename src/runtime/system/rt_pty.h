//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/system/rt_pty.h
// Purpose: Pseudo-terminal-backed child handles for Zanna.System.Pty — an
//          interactive terminal surface (merged ANSI output, resize) distinct
//          from the pipe-based Zanna.System.Process.
//
// Key invariants:
//   - Program startup is direct and never invokes a command shell.
//   - Child stdin, stdout, and stderr share one interactive terminal.
//   - Output reads are incremental and retain at most 16 MiB between reads.
//   - Polling reaps completion once and preserves the exit status.
//   - A non-NULL environment sequence replaces the complete child environment.
//   - Diagnostics are thread-local and must be read on the calling thread.
//
// Ownership/Lifetime:
//   - Open returns a GC-managed session object; Destroy invalidates it and is idempotent.
//   - Returned strings, maps, and Result objects are new caller-owned runtime values.
//
// Links: src/runtime/system/rt_pty.c (implementation),
//        docs/adr/0281-event-driven-process-pty-gui-wakes.md
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_pty.h
 * @brief Declares interactive pseudo-terminal process sessions.
 * @details The API launches a direct child with optional arguments, working
 *          directory, and replacement environment; exposes merged terminal
 *          I/O, resize, polling, exit and truncation state, termination, and
 *          Result-based startup diagnostics under managed ownership.
 */

#pragma once

#include "rt_string.h"

#include <stdint.h>

struct rt_activity_wake_target;

/// @brief Runtime class identifier assigned to Zanna.System.Pty sessions.
#define RT_PTY_CLASS_ID INT64_C(-0x440202)

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Open a PTY-backed child process.
/// @details Bypasses the shell and connects all child standard streams to one
///          interactive terminal. A non-NULL @p env replaces rather than
///          augments the inherited environment.
/// @param program Executable path or lookup name; must be nonempty and contain
///        no embedded NUL byte.
/// @param args Optional Seq of literal strings without embedded NUL.
/// @param cwd Working directory without embedded NUL, or NULL/empty to inherit.
/// @param env Optional Seq of nonempty NAME=VALUE strings; NULL means inherit
///        and an empty Seq requests an empty environment.
/// @param cols Initial terminal columns (clamped to [1,4096]; 0 -> 80).
/// @param rows Initial terminal rows (clamped to [1,4096]; 0 -> 24).
/// @return New GC-managed PTY session, or NULL after recording a validation,
///         support, allocation, setup, or launch error on the calling thread.
void *rt_pty_open(
    rt_string program, void *args, rt_string cwd, void *env, int64_t cols, int64_t rows);

/// @brief Open a PTY with inherited environment plus explicit overrides.
/// @details Override names use native comparison rules: case-insensitive on
///          Windows and case-sensitive on POSIX. NULL/empty overlays inherit
///          the parent environment unchanged.
void *rt_pty_open_with_env_overlay(
    rt_string program, void *args, rt_string cwd, void *env, int64_t cols, int64_t rows);

/// @brief Open a PTY-backed child process and return a Zanna.Result.
/// @details Returns `Ok(PtySession)` when the child session starts and
///          `Err(message)` when the platform lacks PTY support, startup fails,
///          or validation traps would have been raised by rt_pty_open(). It
///          catches those traps and transfers successful session ownership into
///          the Result.
/// @param program Executable path or lookup name subject to rt_pty_open validation.
/// @param args Optional Seq of literal argument strings.
/// @param cwd Working directory, or NULL/empty to inherit.
/// @param env Optional complete NAME=VALUE environment, or NULL to inherit.
/// @param cols Initial terminal columns (clamped to [1,4096]; 0 -> 80).
/// @param rows Initial terminal rows (clamped to [1,4096]; 0 -> 24).
/// @return Caller-owned opaque Zanna.Result containing a PTY session or
///         diagnostic string.
void *rt_pty_open_result(
    rt_string program, void *args, rt_string cwd, void *env, int64_t cols, int64_t rows);

/// @brief Result-returning form of rt_pty_open_with_env_overlay().
void *rt_pty_open_with_env_overlay_result(
    rt_string program, void *args, rt_string cwd, void *env, int64_t cols, int64_t rows);

/// @brief Return TRUE when the runtime can create PTY sessions on this platform.
/// @details Always true for the POSIX backend. Windows dynamically requires all
///          ConPTY entry points (Windows 10 1809+); failure installs a
///          thread-local diagnostic when none is already present.
/// @return 1 when PTY creation is supported, otherwise 0.
int64_t rt_pty_is_supported(void);

/// @brief Copy the calling thread's most recent PTY diagnostic.
/// @details Intended for reporting immediately after a failed operation. Every
///          public PTY operation first clears the prior per-thread message, so
///          a successful operation cannot leave a stale diagnostic behind.
/// @return Newly allocated diagnostic string, or an empty string when none;
///         the caller must release it.
rt_string rt_pty_last_error(void);

/// @brief Test whether @p handle is a started, undestroyed PTY object.
/// @details An exited child remains a valid session until destruction so status
///          and buffered output remain accessible.
/// @param handle Candidate opaque runtime object.
/// @return 1 for a valid session handle, otherwise 0.
int64_t rt_pty_is_valid(void *handle);

/// @brief Attach a ref-counted event-loop target for PTY output/exit activity.
/// @details Internal GUI bridge used by App.WatchPty.
int64_t rt_pty_set_activity_wake(void *handle, struct rt_activity_wake_target *target);

/// @brief Nonblockingly drain terminal output and poll child completion.
/// @param handle Candidate PTY session handle.
/// @return 1 while the child is running, otherwise 0.
int64_t rt_pty_poll(void *handle);

/// @brief Poll and report whether the PTY child remains running.
/// @param handle Candidate PTY session handle.
/// @return 1 while running, otherwise 0 for exited or invalid handles.
int64_t rt_pty_is_running(void *handle);

/// @brief Read currently buffered terminal output plus immediately available bytes.
/// @details Consumes an incremental merged stdout/stderr stream that may contain
///          ANSI/control sequences. If the 16 MiB buffer discarded bytes, the
///          retained prefix is returned after raising a truncation trap.
/// @param handle Candidate PTY session handle.
/// @return Newly allocated output chunk, or an empty string for no bytes or an
///         invalid handle.
rt_string rt_pty_read(void *handle);

/// @brief Read terminal output as `{ text, truncated }` without trapping on truncation.
/// @details Consumes the incremental buffer and reports any discarded bytes in
///          a Boolean @c truncated entry beside string @c text.
/// @param handle Candidate PTY session handle.
/// @return Caller-owned result map, an empty nontruncated map for invalid
///         handles, or NULL on map allocation failure.
void *rt_pty_read_result(void *handle);

/// @brief Write bytes to the terminal input.
/// @details Honors runtime-string length, including embedded NUL. Windows
///          copies accepted bytes into a bounded asynchronous queue so a child
///          that stops reading cannot block the caller; POSIX writes directly
///          to the nonblocking master and may return a partial count.
/// @param handle Candidate PTY session with open terminal input.
/// @param data Runtime byte string; NULL is treated as empty.
/// @return Complete byte count, zero for empty input, positive partial count
///         after later failure/would-block, or -1 when no byte can be written.
int64_t rt_pty_write(void *handle, rt_string data);

/// @brief Resize the terminal window (delivers SIGWINCH to the child on POSIX).
/// @param handle Candidate PTY session handle.
/// @param cols Requested columns; nonpositive selects 80 and values above 4096 clamp.
/// @param rows Requested rows; nonpositive selects 24 and values above 4096 clamp.
/// @return 1 only when the platform backend applies the normalized size,
///         otherwise 0 after recording backend failure when applicable.
int64_t rt_pty_resize(void *handle, int64_t cols, int64_t rows);

/// @brief Poll and return the retained PTY child exit status.
/// @param handle Candidate PTY session handle.
/// @return Normal exit code, negated signal number on POSIX, or -1 while
///         running, after status failure, or for an invalid handle.
int64_t rt_pty_exit_code(void *handle);

/// @brief Request child or child-session termination without waiting.
/// @details Uses status 1 on Windows and SIGTERM for the POSIX session/process
///          group with a child-pid fallback.
/// @param handle Candidate PTY session handle.
/// @return 1 when a termination request is accepted, otherwise 0.
int64_t rt_pty_kill(void *handle);

/// @brief Wait for PTY child completion and retain its exit status.
/// @param handle Candidate PTY session handle.
/// @return Normal exit code, negated signal number on POSIX, or -1 for an
///         invalid handle or wait/status failure.
int64_t rt_pty_wait(void *handle);

/// @brief Terminate if necessary and release all PTY session resources.
/// @details Idempotent. POSIX grants a short SIGTERM grace period before
///          SIGKILL; Windows terminates and waits. The GC-managed object remains
///          allocated but becomes invalid.
/// @param handle Candidate PTY session handle; invalid values are ignored.
void rt_pty_destroy(void *handle);

#ifdef __cplusplus
}
#endif
