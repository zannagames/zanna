//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/rt.hpp
// Purpose: Internal umbrella declarations for trap handling, native exception
//          frames, console/file-channel I/O, terminal control, and raw allocation.
// Key invariants:
//   - Inputs are borrowed unless a parameter explicitly transfers ownership.
//   - Trap state and recovery stacks are thread-local.
//   - Terminal i64 adapters preserve their documented narrowing semantics.
// Ownership/Lifetime:
//   - Input and materialization APIs return caller-owned runtime objects unless
//     they explicitly return an immortal shared string.
//   - Native EH frames and raw rt_alloc blocks require explicit matching frees.
// Links: src/runtime/core/rt_io.c, src/runtime/core/rt_term.c,
//        src/runtime/core/rt_memory.c, docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/**
 * @file rt.hpp
 * @brief Declares internal trap, exception-frame, I/O, terminal, and allocation APIs.
 * @details This umbrella boundary supplements the public runtime ABI with
 *          thread-local trap recovery, native exception frames, console and
 *          file-channel operations, terminal control, input materialization,
 *          and explicitly managed raw allocation helpers used by runtime code.
 */

#pragma once

#include <setjmp.h>
#include <stdint.h>

#include "zanna/runtime/rt.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Raise a generic domain-error trap.
/// @details Active recovery performs a non-local jump; otherwise the configured
///          VM trap hook receives the message and may terminate or return.
/// @param msg Borrowed NUL-terminated diagnostic; may be @c NULL.
void rt_trap(const char *msg);

/// @brief Raise a generic trap from a managed runtime string.
/// @details Validates and escapes the message into bounded diagnostic storage;
///          null or empty strings use `"trap"`, and invalid handles trap with
///          an explicit diagnostic.
/// @param msg Borrowed runtime string, or @c NULL/empty for the generic text.
void rt_trap_string(rt_string msg);

/// @brief Raise a trap with explicit trap metadata.
/// @details Captures the native return address and routes through the unified
///          recovery/VM-hook dispatcher.
/// @param kind Canonical trap classification.
/// @param code Secondary runtime error code (Err_* or 0).
/// @param line Source line number (-1 if unknown).
/// @param msg Borrowed user-visible NUL-terminated message; may be @c NULL.
void rt_trap_raise_kind(int32_t kind, int32_t code, int32_t line, const char *msg);

/// @brief Raise a trap with explicit trap metadata and no message.
/// @param kind Canonical trap classification; out-of-range values normalize to
///        runtime error.
/// @param code Secondary runtime error code.
/// @param line Source line number, or -1 when unavailable.
void rt_trap_raise_kind_nomsg(int32_t kind, int32_t code, int32_t line);

/// @brief Trap when @p condition is false with the provided diagnostic.
/// @param condition Boolean flag; zero triggers a trap.
/// @param message Runtime string describing the assertion; defaults to
///        "Assertion failed" when empty or null.
void rt_diag_assert(int8_t condition, rt_string message);

/// @brief Print trap message and terminate process.
/// @details Writes a diagnostic to stderr, flushes stdout/stderr, and exits
///          immediately with status one.
/// @param msg Borrowed NUL-terminated message; null/empty prints `"Trap"`.
void rt_abort(const char *msg);

/// @brief Set a thread-local recovery point for trap interception.
/// @details Pushes a heap-allocated legacy recovery node. Passing null clears
///          consecutive legacy nodes from the stack top, stopping at a native frame.
/// @param buf Borrowed jump buffer that must remain live until cleared, or @c NULL.
void rt_trap_set_recovery(jmp_buf *buf);

/// @brief Clear the thread-local trap recovery point.
/// @details Pops the top entry only when it is a legacy recovery node and
///          always clears the saved thread-local diagnostic.
void rt_trap_clear_recovery(void);

/// @brief Get the error message captured by the last intercepted trap.
/// @return Borrowed NUL-terminated thread-local text valid until the next trap
///         or recovery-clear operation on this thread.
const char *rt_trap_get_error(void);

/// @brief Allocate a native EH frame used by native codegen's setjmp path.
/// @return Caller-owned zero-initialized opaque frame, or @c NULL on allocation
///         failure.
void *rt_native_eh_frame_alloc(void);

/// @brief Free a native EH frame allocated by @ref rt_native_eh_frame_alloc.
/// @details Unlinks the frame from anywhere in the current thread's recovery
///          chain before releasing it.
/// @param frame Owned opaque frame; @c NULL is accepted.
void rt_native_eh_frame_free(void *frame);

/// @brief Push a native EH frame onto the thread-local trap stack.
/// @details Resets its resume site to zero; later traps target its saved
///          `setjmp` environment until it is popped or freed.
/// @param frame Borrowed allocated frame; @c NULL is ignored.
void rt_native_eh_push(void *frame);

/// @brief Pop a native EH frame from the thread-local trap stack.
/// @details Removes the frame only when it is currently the top entry and does
///          not free its storage.
/// @param frame Borrowed frame to compare with the stack top; @c NULL is ignored.
void rt_native_eh_pop(void *frame);

/// @brief Record the active native resume site for the frame.
/// @param frame Borrowed allocated frame; @c NULL is ignored.
/// @param site_id Codegen-defined identifier stored without interpretation.
void rt_native_eh_set_site(void *frame, int64_t site_id);

/// @brief Retrieve the active native resume site for the frame.
/// @param frame Borrowed allocated frame, or @c NULL.
/// @return Stored site identifier, or zero for null/newly pushed frames.
int64_t rt_native_eh_get_site(void *frame);

/// @brief Raise a network-specific trap with a categorized error code.
/// @param msg Human-readable description of the network failure.
/// @param err_code One of the Err_ConnectionRefused..Err_ProtocolError codes.
void rt_trap_net(const char *msg, int err_code);

/// @brief Retrieve the error code from the most recent network trap.
/// @return The Err_* code set by the last rt_trap_net() call on this thread.
int rt_trap_get_net_code(void);

/// @brief Print string @p s to stdout.
/// @param s Reference-counted string.
void rt_print_str(rt_string s);

/// @brief Print signed 64-bit integer @p v to stdout.
/// @param v Value to print.
void rt_print_i64(int64_t v);

/// @brief Print 64-bit float @p v to stdout.
/// @param v Value to print.
void rt_print_f64(double v);

/// @brief Print signed 32-bit integer @p value with newline.
/// @param value Value to print in decimal.
void rt_println_i32(int32_t value);

/// @brief Print C string @p text with newline.
/// @param text Null-terminated string; treated as empty when null.
void rt_println_str(const char *text);

/// @brief Read a line from stdin.
/// @details Flushes pending output, grows a temporary byte buffer, strips LF
///          and an immediately preceding CR, and traps on growth failures.
/// @return Caller-owned string without the line ending, or @c NULL when EOF
///         occurs before any bytes are read.
rt_string rt_input_line(void);

/// @brief Split comma-separated fields from @p line into @p out_fields.
/// @details Trims surrounding ASCII whitespace, recognizes quoted fields and
///          doubled quote escapes, and traps when fewer fields are present than
///          a positive requested maximum. Parsing is intentionally lenient.
/// @param line Borrowed input line; @c NULL represents one empty field.
/// @param out_fields Destination receiving up to @p max_fields caller-owned
///        strings; may be @c NULL only when @p max_fields is nonpositive.
/// @param max_fields Number of fields to materialize; negative values become zero.
/// @return Total fields discovered, which may exceed the number stored.
int64_t rt_str_split_fields(rt_string line, rt_string *out_fields, int64_t max_fields);

/// @brief Split comma-separated fields into an owned Seq[str].
/// @param line Borrowed input line; @c NULL represents one empty field.
/// @return Caller-owned `Zanna.Seq[str]` containing all leniently parsed fields.
void *rt_str_split_fields_seq(rt_string line);

/// @brief Strict-quoting companion to @ref rt_str_split_fields_seq.
/// @details Rejects quotes inside unquoted fields, text after a closing quote,
///          and unclosed quotes as `Err(String)` rather than trapping.
/// @param line Borrowed input line; @c NULL represents one empty field.
/// @return Caller-owned `Result.Ok(Seq[str])` or `Result.Err(String)`.
void *rt_str_split_fields_result(rt_string line);

// =========================================================================
// Zanna.Terminal I/O Functions
// =========================================================================

/// @brief Print a runtime string followed by LF and flush output.
/// @param s Borrowed runtime string; @c NULL prints only the newline.
void rt_term_say(rt_string s);

/// @brief Print a signed integer in decimal followed by LF and flush output.
/// @param v Value to print.
void rt_term_say_i64(int64_t v);

/// @brief Print a normalized floating-point value followed by LF and flush output.
/// @param v Value to print.
void rt_term_say_f64(double v);

/// @brief Print a boolean as `"true"` or `"false"` followed by LF.
/// @param v Zero prints false; any nonzero value prints true.
void rt_term_say_bool(int8_t v);

/// @brief Print a runtime string without a trailing newline and flush output.
/// @param s Borrowed runtime string; @c NULL produces no bytes.
void rt_term_print(rt_string s);

/// @brief Print a signed integer in decimal without a trailing newline.
/// @param v Value to print.
void rt_term_print_i64(int64_t v);

/// @brief Print a normalized floating-point value without a trailing newline.
/// @param v Value to print.
void rt_term_print_f64(double v);

/// @brief Print a boolean as `"true"` or `"false"` without a trailing newline.
/// @param v Zero prints false; any nonzero value prints true.
void rt_term_print_bool(int8_t v);

/// @brief Print prompt and read a line of input.
/// @param prompt Borrowed runtime string printed before the read; may be @c NULL.
/// @return Caller-owned line without its ending, or @c NULL for EOF before input.
rt_string rt_term_ask(rt_string prompt);

/// @brief Read a line of input from stdin.
/// @return Caller-owned line without its ending, or @c NULL for EOF before input.
rt_string rt_term_read_line(void);

/// @brief Read a line of input from stdin as an Option.
/// @details Returns an opaque `Zanna.Option` object. The object is `Some(String)`
///          when a line is read and `None` when stdin reaches EOF before any
///          bytes are available. Allocation failures and overlong input still
///          trap, matching @ref rt_input_line.
/// @return Caller-owned `Zanna.Option` containing the line or `None`.
void *rt_term_try_read_line(void);

/// @brief Print a prompt and read a line of input from stdin as an Option.
/// @details The prompt is flushed before reading. Returns an opaque
///          `Zanna.Option` object containing `Some(String)` for a line and
///          `None` for EOF before input. Fatal input errors still trap.
/// @param prompt Runtime string to display before reading input; may be NULL.
/// @return Caller-owned `Zanna.Option` containing the line or `None`.
void *rt_term_try_ask(rt_string prompt);

/// @brief Read a line of input from stdin as a Result.
/// @details Returns an opaque `Zanna.Result` object containing `Ok(String)` for
///          a line and `Err(String)` for EOF before input. Fatal input errors
///          still trap because they indicate runtime failure rather than normal
///          end-of-input.
/// @return Caller-owned `Zanna.Result` containing the line or EOF error.
void *rt_term_read_line_result(void);

/// @brief Print a prompt and read a line of input from stdin as a Result.
/// @details The prompt is flushed before reading. Returns an opaque
///          `Zanna.Result` object containing `Ok(String)` for a line and
///          `Err(String)` for EOF before input. Fatal input errors still trap.
/// @param prompt Runtime string to display before reading input; may be NULL.
/// @return Caller-owned `Zanna.Result` containing the line or EOF error.
void *rt_term_ask_result(rt_string prompt);

/// @brief Report EOF status for the specified channel.
/// @details Probes seekable descriptor length/position and uses cached EOF state
///          for pipes or other non-seekable channels.
/// @param ch Numeric channel identifier previously passed to OPEN.
/// @return -1 at EOF, zero when more input is available, or a positive `Err_*`
///         code on failure.
int rt_eof_ch(int ch);

/// @brief Compute the length in bytes of the file associated with @p ch.
/// @param ch Numeric channel identifier previously passed to OPEN.
/// @return Non-negative byte length on success or -Err_* on failure.
int64_t rt_lof_ch(int ch);

/// @brief Report the current file position for channel @p ch.
/// @param ch Numeric channel identifier previously passed to OPEN.
/// @return Non-negative offset on success or -Err_* on failure.
int64_t rt_loc_ch(int ch);

/// @brief Reposition channel @p ch to absolute byte offset @p pos.
/// @param ch Numeric channel identifier previously passed to OPEN.
/// @param pos Non-negative byte offset from start of file.
/// @return 0 on success or Err_* code on failure.
int32_t rt_seek_ch_err(int ch, int64_t pos);

/// @brief Allocate @p bytes of zeroed memory.
/// @details The default allocator promotes a zero-byte request to one byte and
///          traps on negative, unrepresentable, or failed requests. A
///          test-installed hook may override default behavior.
/// @param bytes Signed number of bytes to allocate.
/// @return Caller-owned free-compatible block (zeroed on the default path), or
///         @c NULL after a returning trap.
void *rt_alloc(int64_t bytes);

/// @brief Free storage returned by @ref rt_alloc.
/// @param ptr Pointer returned by rt_alloc, or NULL.
void rt_free(void *ptr);

// Terminal control + single-key input

/// @brief Clear the terminal when stdout is a TTY.
void rt_term_cls(void);

/// @brief Set foreground/background colors using terminal SGR sequences.
/// @param fg Foreground color index (-1 to leave unchanged).
/// @param bg Background color index (-1 to leave unchanged).
void rt_term_color_i32(int32_t fg, int32_t bg);

/// @brief Move the cursor to 1-based row/column when stdout is a TTY.
/// @param row Target row (clamped to >= 1).
/// @param col Target column (clamped to >= 1).
void rt_term_locate_i32(int32_t row, int32_t col);

/// @brief Show or hide the terminal cursor using ANSI escape sequences.
/// @param show Non-zero to show cursor, zero to hide cursor.
void rt_term_cursor_visible_i32(int32_t show);

/// @brief Toggle alternate screen buffer using ANSI DEC Private Mode sequences.
/// @details The transition is idempotent. Entering also enables cached raw mode
///          and begins an output batch; exiting balances both. Redirected
///          stdout makes the operation a no-op.
/// @param enable Non-zero to enter alternate screen, zero to exit.
void rt_term_alt_screen_i32(int32_t enable);

/// @brief Emit a bell/beep sound using BEL character or platform-specific API.
/// @details Writes ASCII BEL to stdout. On Windows, optionally calls Beep() API
///          when ZANNA_BEEP_WINAPI environment variable is set to "1".
void rt_bell(void);

/// @brief Block until a single key is read and return it as a 1-character string.
/// @details Flushes pending output first. A platform read failure is represented
///          by the one-byte string for code zero.
/// @return Caller-owned one-byte runtime string, or @c NULL if string allocation fails.
rt_string rt_getkey_str(void);

/// @brief Block for a key with optional timeout; return empty string if timeout expires.
/// @param timeout_ms Maximum wait time in milliseconds; negative values block indefinitely.
/// @return Caller-owned one-byte string when input arrives, or the immortal
///         shared empty string on timeout or terminal/console error.
rt_string rt_getkey_timeout_i32(int32_t timeout_ms);

/// @brief Return a pending key as a 1-character string or empty string if none available.
/// @details Flushes pending output and performs a non-blocking console/descriptor poll.
/// @return Caller-owned one-byte string when a key is consumed, or the immortal
///         shared empty string when no byte is available.
rt_string rt_inkey_str(void);

/// @brief Check if a key is available in the input buffer without reading it.
/// @return Non-zero if a key is pending, zero otherwise.
int32_t rt_keypressed(void);

/// @brief Check for pending key input and widen the result to 64 bits.
/// @return Zero or one as returned by @ref rt_keypressed.
int64_t rt_keypressed_i64(void);

// i64 wrappers (for frontends that use 64-bit integers)

/// @brief Move the cursor through the i64 frontend adapter.
/// @param row One-based row saturated to `int32_t`, then clamped to at least one.
/// @param col One-based column saturated to `int32_t`, then clamped to at least one.
void rt_term_locate(int64_t row, int64_t col);

/// @brief Set foreground/background colors through the i64 frontend adapter.
/// @param fg Foreground code saturated to `int32_t`.
/// @param bg Background code saturated to `int32_t`.
void rt_term_color(int64_t fg, int64_t bg);

/// @brief Set only the foreground color through the i64 frontend adapter.
/// @param fg Foreground code saturated to `int32_t`; background is unchanged.
void rt_term_textcolor(int64_t fg);

/// @brief Set only the background color through the i64 frontend adapter.
/// @param bg Background code saturated to `int32_t`; foreground is unchanged.
void rt_term_textbg(int64_t bg);

/// @brief Hide the terminal cursor.
void rt_term_hide_cursor(void);

/// @brief Show the terminal cursor.
void rt_term_show_cursor(void);

/// @brief Set cursor visibility through the legacy i64 adapter.
/// @details Narrows by direct C cast to `int32_t` before boolean interpretation.
/// @param show Zero hides the cursor; a narrowed nonzero value shows it.
void rt_term_cursor_visible(int64_t show);

/// @brief Toggle alternate-screen mode through the legacy i64 adapter.
/// @details Narrows by direct C cast to `int32_t` before boolean interpretation.
/// @param enable Zero exits; a narrowed nonzero value enters.
void rt_term_alt_screen(int64_t enable);

/// @brief Sleep through the legacy i64 millisecond adapter.
/// @details Narrows by direct C cast to `int32_t` before delegating.
/// @param ms Millisecond count to narrow and pass to the platform sleep helper.
void rt_sleep_ms_i64(int64_t ms);

/// @brief Wait for a key through the saturating i64 timeout adapter.
/// @param timeout_ms Milliseconds clamped to the `int32_t` range; negative
///        values remain an indefinite-wait request.
/// @return Key or empty-string result from @ref rt_getkey_timeout_i32.
rt_string rt_getkey_timeout(int64_t timeout_ms);

// Output buffering control for improved terminal rendering performance

/// @brief Begin batch mode for output operations.
/// @details While in batch mode, terminal control sequences do not trigger
///          individual flushes. This dramatically improves rendering performance
///          for games and animations (reduces syscalls from ~6000/frame to ~1).
void rt_term_begin_batch(void);

/// @brief End batch mode and flush accumulated output.
/// @details Decrements batch mode reference count. When zero, flushes all
///          accumulated output in a single system call.
void rt_term_end_batch(void);

/// @brief Explicitly flush terminal output.
/// @details Forces all buffered output to be written immediately.
void rt_term_flush(void);

// =========================================================================
// Terminal Raw Mode Caching (Performance Optimization)
// =========================================================================

/// @brief Enable cached raw mode for efficient key polling.
/// @details Switches terminal to raw mode once. Subsequent INKEY$ calls
///          use select() without needing to change terminal settings.
///          This dramatically improves performance in game loops. Non-TTY or
///          setup failure is a no-op; Windows needs no transition and is always a no-op.
void rt_term_enable_raw_mode(void);

/// @brief Disable raw mode and restore original terminal settings.
/// @details Restores cached POSIX settings when active. It is an idempotent
///          no-op when inactive and on Windows.
void rt_term_disable_raw_mode(void);

/// @brief Check if raw mode caching is currently active.
/// @return @c 1 after a successful POSIX cached transition; otherwise @c 0.
int8_t rt_term_is_raw_mode(void);

#ifdef __cplusplus
}
#endif
