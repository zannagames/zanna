//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_term.c
// Purpose: Implements portable terminal control, byte-oriented keyboard input,
//          POSIX raw-mode caching, alternate-screen lifecycle, and output-batch
//          adapters for BASIC and 64-bit frontend ABIs.
//
// Key invariants:
//   - Public display controls emit ANSI only when stdout is a terminal; BEL and
//     direct buffered output helpers have their own documented behavior.
//   - POSIX raw mode caches original/current termios state; Windows keyboard
//     polling needs no equivalent transition.
//   - Windows terminal output lazily requests virtual-terminal processing and
//     UTF-8 console code pages.
//   - INKEY$ uses select() with a zero timeout for non-blocking key reads on
//     POSIX; on Windows it uses _kbhit().
//   - Alternate-screen transitions are idempotent and balance exactly one
//     output-batch level. Registered POSIX exit cleanup restores that state.
//
// Ownership/Lifetime:
//   - Key-reading functions return one owned string reference: a fresh
//     one-byte string or the shared empty singleton.
//   - The saved termios state is a process-global stack variable; no heap
//     allocation is needed for terminal state management.
//   - Terminal state and alternate-screen flags are process-global and are not
//     independently synchronized for concurrent callers.
//
// Links: src/runtime/core/rt_output.c (buffered stdout wrapper),
//        src/runtime/core/rt_io.c (higher-level I/O primitives)
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Cross-platform runtime terminal display, keyboard, and batching API.

#include "rt.hpp"
#include "rt_output.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <conio.h>
#include <io.h>
#include <windows.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#endif

// =============================================================================
// PERFORMANCE OPTIMIZATION: Terminal Raw Mode Caching
// =============================================================================
//
// Problem: Every INKEY$() call was doing tcgetattr + tcsetattr + tcsetattr,
// which are expensive system calls. In a game loop running at 60 FPS, this
// meant 180+ syscalls per second just for keyboard polling.
//
// Solution: Cache the terminal state. When "raw mode" is enabled:
// - Store original termios settings once
// - Set raw mode once
// - Subsequent INKEY$() calls just use select() - no termios changes
// - Restore original settings when raw mode is disabled or program exits
//
// Raw mode is automatically enabled when:
// - Alt screen buffer is activated (typical for games)
// - Explicitly via rt_term_enable_raw_mode()
//
// =============================================================================

/// @brief Whether the terminal is currently on the alternate screen buffer.
/// @details Tracks one alt-screen state so SetAltScreen is an idempotent toggle
///          (enter/exit and batch only on state changes). On POSIX, the exit
///          handler also uses this state to balance the screen and batch.
static int g_alt_screen_active = 0;

#if !defined(_WIN32)
/// @brief Cached original terminal settings (before raw mode).
static struct termios g_orig_termios;

/// @brief Cached raw mode terminal settings.
static struct termios g_raw_termios;

/// @brief Whether raw mode caching is currently active.
static int g_raw_mode_active = 0;

/// @brief Whether we've captured the original terminal settings.
static int g_termios_saved = 0;

/// @brief File descriptor for stdin (cached to avoid repeated fileno calls).
static int g_stdin_fd = -1;

/// @brief Whether atexit handler has been registered.
static int g_atexit_registered = 0;

/// @brief Emit a NUL-terminated byte string through buffered terminal output.
/// @param s Borrowed C string; may be NULL.
static void out_str(const char *s);

/// @brief Cleanup handler called on program exit.
/// @details Best-effort BALANCED restore: if the program left the terminal on the
///          alternate screen, end the batch it started and emit the alt-screen
///          exit sequence, then restore raw mode — otherwise a normal exit could
///          strand the terminal on the alternate screen or in batch mode
///          (VDOC-220).
static void term_atexit_handler(void) {
    if (g_alt_screen_active) {
        rt_output_end_batch();
        out_str("\x1b[?1049l");
        g_alt_screen_active = 0;
    }
    rt_term_disable_raw_mode();
}

#if defined(__linux__)
/// @brief Register a Linux ABI process-exit callback.
/// @param func Callback receiving @p arg.
/// @param arg Opaque callback argument.
/// @param dso_handle Optional owning DSO handle.
/// @return Zero on success or a nonzero libc error code.
extern int __cxa_atexit(void (*func)(void *), void *arg, void *dso_handle);

/// @brief __cxa_atexit trampoline wrapping term_atexit_handler to the
///        void(*)(void*) callback signature (Linux only).
/// @param arg Unused callback context.
static void term_atexit_handler_adapter(void *arg) {
    (void)arg;
    term_atexit_handler();
}

/// @brief Register the terminal-restore handler to run at process exit.
/// @details Linux uses libc's __cxa_atexit() (no late-bindable atexit()
///          symbol).
/// @return Zero on success or the platform registration error code.
static int register_term_atexit_handler(void) {
    // glibc exports __cxa_atexit() but not a late-bindable atexit() symbol.
    return __cxa_atexit(term_atexit_handler_adapter, NULL, NULL);
}
#else
/// @brief Register the POSIX terminal-restore callback with `atexit`.
/// @return Zero on success or a nonzero registration error code.
static int register_term_atexit_handler(void) {
    return atexit(term_atexit_handler);
}
#endif

/// @brief Initialize terminal state caching.
/// @details Caches the stdin descriptor and, for an interactive descriptor,
///          captures original attributes and derives noncanonical/no-echo
///          polling attributes. Failed descriptor/termios operations leave
///          caching unavailable for a later call to retry.
static void init_term_cache(void) {
    if (g_stdin_fd < 0)
        g_stdin_fd = fileno(stdin);

    if (!g_termios_saved && g_stdin_fd >= 0 && isatty(g_stdin_fd)) {
        if (tcgetattr(g_stdin_fd, &g_orig_termios) == 0) {
            g_termios_saved = 1;
            // Prepare raw mode settings
            g_raw_termios = g_orig_termios;
            g_raw_termios.c_lflag &= ~(ICANON | ECHO);
            g_raw_termios.c_cc[VMIN] = 0;
            g_raw_termios.c_cc[VTIME] = 0;
        }
    }
}

/// @brief Enable cached raw mode for efficient key polling.
/// @details Switches terminal to raw mode once. Subsequent INKEY$ calls
///          use select without repeated attribute changes. Noninteractive or
///          uncapturable stdin is a no-op. Exit cleanup registration is
///          best-effort and a failed `tcsetattr` leaves mode inactive.
void rt_term_enable_raw_mode(void) {
    init_term_cache();
    if (g_raw_mode_active || !g_termios_saved)
        return;

    // Register atexit handler to ensure terminal is restored on exit
    if (!g_atexit_registered) {
        if (register_term_atexit_handler() == 0)
            g_atexit_registered = 1;
    }

    if (tcsetattr(g_stdin_fd, TCSANOW, &g_raw_termios) == 0)
        g_raw_mode_active = 1;
}

/// @brief Disable raw mode and restore original terminal settings.
/// @details No-ops unless cached raw mode is active. The restore return code is
///          not surfaced; internal state is marked inactive afterward.
void rt_term_disable_raw_mode(void) {
    if (!g_raw_mode_active || !g_termios_saved)
        return;

    tcsetattr(g_stdin_fd, TCSANOW, &g_orig_termios);
    g_raw_mode_active = 0;
}

/// @brief Check if raw mode caching is currently active.
/// @return One after a successful cached POSIX transition; otherwise zero.
int8_t rt_term_is_raw_mode(void) {
    return g_raw_mode_active;
}

#else // Windows doesn't need raw mode caching - _kbhit is already efficient

/// @brief Windows no-op counterpart to POSIX raw-mode enablement.
void rt_term_enable_raw_mode(void) {}

/// @brief Windows no-op counterpart to POSIX raw-mode restoration.
void rt_term_disable_raw_mode(void) {}

/// @brief Report cached raw mode on Windows.
/// @return Always zero because `_kbhit` requires no termios state.
int8_t rt_term_is_raw_mode(void) {
    return 0;
}

#endif

/// @brief Determine whether stdout is attached to a terminal.
/// @details Guards terminal escape emission so batch output (e.g. redirected to
///          a file) remains free of ANSI sequences.
/// @return One for a valid interactive stdout descriptor; otherwise zero.
static int stdout_isatty(void) {
    int fd = fileno(stdout);
    return (fd >= 0) && isatty(fd);
}

#if defined(_WIN32)
/// @brief Enable ANSI escape sequence processing and UTF-8 on Windows consoles.
/// @details Lazily toggles the `ENABLE_VIRTUAL_TERMINAL_PROCESSING` flag and
///          sets the console codepage to UTF-8 (65001) the first time terminal
///          output is requested so subsequent writes honour colour, cursor
///          positioning sequences, and UTF-8 box-drawing characters.
/// @note Platform API failures are intentionally ignored and initialization is
///       attempted only once.
static void enable_vt(void) {
    static int once = 0;
    if (once)
        return;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        if (GetConsoleMode(h, &mode)) {
            mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            SetConsoleMode(h, mode);
        }
    }
    // Enable UTF-8 codepage for proper Unicode/box-drawing character display
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    once = 1;
}
#endif

/// @brief Emit a raw string to stdout, enabling ANSI support when available.
/// @details Writes to the output buffer and conditionally flushes based on
///          batch mode. When batch mode is active (via rt_output_begin_batch),
///          output accumulates until rt_output_end_batch is called, dramatically
///          reducing system calls during screen rendering.
///
/// Performance note: Before this change, each terminal operation caused an
/// immediate fflush(), resulting in thousands of system calls per frame.
/// With output buffering, a typical 60x20 game screen update goes from
/// ~6000 syscalls to ~1 syscall (at batch end).
/// @param s Borrowed NUL-terminated bytes; NULL is a no-op.
static void out_str(const char *s) {
    if (!s)
        return;
#if defined(_WIN32)
    enable_vt();
#endif
    rt_output_str(s);
    rt_output_flush_if_not_batch();
}

/// @brief Emit an SGR escape sequence for the requested foreground/background.
/// @details Converts BASIC colour codes into ANSI escape sequences, supporting
///          normal, bright, and 256-colour modes.  Negative parameters leave the
///          corresponding channel unchanged.
/// @param fg Foreground code: negative unchanged, 0–7 normal, 8–15 bright,
///        and larger values emitted as an extended palette index.
/// @param bg Background code with the analogous mapping.
static void sgr_color(int fg, int bg) {
    if (fg < 0 && bg < 0) {
        return;
    }
    char buf[64];
    int n = 0, wrote = 0;

    buf[n++] = '\x1b';
    buf[n++] = '[';

    if (fg >= 0) {
        if (fg <= 7) {
            n += snprintf(buf + n, sizeof(buf) - n, "%d", 30 + fg);
        } else if (fg <= 15) {
            n += snprintf(buf + n, sizeof(buf) - n, "1;%d", 30 + (fg - 8));
        } else {
            n += snprintf(buf + n, sizeof(buf) - n, "38;5;%d", fg);
        }
        wrote = 1;
    }
    if (bg >= 0) {
        if (wrote)
            buf[n++] = ';';
        if (bg <= 7) {
            n += snprintf(buf + n, sizeof(buf) - n, "%d", 40 + bg);
        } else if (bg <= 15) {
            n += snprintf(buf + n, sizeof(buf) - n, "%d", 100 + (bg - 8));
        } else {
            n += snprintf(buf + n, sizeof(buf) - n, "48;5;%d", bg);
        }
    }
    buf[n++] = 'm';
    buf[n] = '\0';
    out_str(buf);
}

/// @brief Clear the terminal display when stdout is interactive.
/// @details Emits the ANSI sequence for clearing the screen and homing the
///          cursor.  No output is produced when stdout is redirected.
void rt_term_cls(void) {
    if (!stdout_isatty())
        return;
    out_str("\x1b[2J\x1b[H");
}

/// @brief Adjust terminal foreground/background colours using BASIC codes.
/// @details Validates the colour range and forwards to @ref sgr_color when
///          stdout is a terminal.  Negative parameters leave the colour
///          unchanged to mirror BASIC's semantics; values below -1 cause the
///          entire operation to be ignored.
/// @param fg Foreground color code, or -1 to preserve it.
/// @param bg Background color code, or -1 to preserve it.
void rt_term_color_i32(int32_t fg, int32_t bg) {
    if (!stdout_isatty())
        return;
    if (fg < -1 || bg < -1)
        return;
    sgr_color((int)fg, (int)bg);
}

/// @brief Move the cursor to a 1-based row/column pair.
/// @details Clamps coordinates to the minimum BASIC expects and emits an ANSI
///          cursor-position sequence when stdout is interactive.
/// @param row One-based row, clamped upward to one.
/// @param col One-based column, clamped upward to one.
void rt_term_locate_i32(int32_t row, int32_t col) {
    if (!stdout_isatty())
        return;
    if (row < 1)
        row = 1;
    if (col < 1)
        col = 1;
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (int)row, (int)col);
    out_str(buf);
}

/// @brief Show or hide the terminal cursor using ANSI DEC Private Mode sequences.
/// @details Emits CSI ?25h to show the cursor or CSI ?25l to hide it.  The
///          helper only outputs escape codes when stdout is a terminal so
///          redirected output remains free of ANSI sequences.
/// @param show Zero to hide the cursor; nonzero to show it.
void rt_term_cursor_visible_i32(int32_t show) {
    if (!stdout_isatty())
        return;
    out_str(show ? "\x1b[?25h" : "\x1b[?25l");
}

/// @brief Toggle alternate screen buffer using ANSI DEC Private Mode sequences.
/// @details Emits CSI ?1049h to enter the alternate screen buffer or CSI ?1049l
///          to exit and restore the original screen.  The helper only outputs
///          escape codes when stdout is a terminal so redirected output remains
///          free of ANSI sequences.
///
/// PERFORMANCE: Automatically enables/disables raw mode caching when entering/
///              exiting alt screen. Games typically use alt screen, so this
///              provides automatic optimization for game loops.
/// @param enable Nonzero to enter; zero to leave.
void rt_term_alt_screen_i32(int32_t enable) {
    if (!stdout_isatty())
        return;
    // Idempotent toggle: only transition (and adjust batch depth) on an actual
    // state change, so repeated enable/disable calls stay balanced and cannot
    // strand output in batch mode (VDOC-220).
    if (enable) {
        if (g_alt_screen_active)
            return;
        out_str("\x1b[?1049h");
        // Auto-enable raw mode for better INKEY$ performance in games
        rt_term_enable_raw_mode();
        // Also auto-enable batch mode for screen rendering
        rt_output_begin_batch();
        g_alt_screen_active = 1;
    } else {
        if (!g_alt_screen_active)
            return;
        g_alt_screen_active = 0;
        // End batch mode before exiting alt screen
        rt_output_end_batch();
        // Restore original terminal settings
        rt_term_disable_raw_mode();
        out_str("\x1b[?1049l");
    }
}

/// @brief Emit a bell/beep sound using BEL character or platform-specific API.
/// @details Writes ASCII BEL (0x07) to stdout and flushes. On Windows, when the
///          ZANNA_BEEP_WINAPI environment variable is set to "1", additionally
///          calls the Beep() API with 800Hz frequency for 80ms duration. This
///          provides a portable default (BEL) with optional platform-specific
///          enhancement.
void rt_bell(void) {
    // Always emit BEL for portability - bell should always flush immediately
    // to ensure the user hears it at the expected moment
    rt_output_str("\a");
    rt_output_flush();

#if defined(_WIN32)
    // On Windows, optionally use Beep API for a more audible tone
    const char *env = getenv("ZANNA_BEEP_WINAPI");
    if (env && strcmp(env, "1") == 0) {
        // 800 Hz for 80 ms - a short, attention-getting beep
        Beep(800, 80);
    }
#endif
}

#if defined(_WIN32)
/// @brief Read a single key from the console, blocking until one is available.
/// @details Uses `_getch` to obtain a byte without echoing it to the console.
/// @return Unsigned byte value from zero through 255.
static int readkey_blocking(void) {
    return _getch() & 0xFF;
}

/// @brief Attempt to read a key without blocking, returning success status.
/// @details Peeks using `_kbhit` and captures the byte with `_getch` when
///          available.  Returns 1 when a key was read and stores the byte in
///          @p out.
/// @param out Required output receiving the unsigned byte on success.
/// @return One when a byte was consumed; otherwise zero.
static int readkey_nonblocking(int *out) {
    if (_kbhit()) {
        *out = _getch() & 0xFF;
        return 1;
    }
    return 0;
}
#else
/// @brief Read a single key from the POSIX terminal, blocking until available.
/// @details Temporarily disables canonical mode and echo, reads one byte, and
///          restores the previous terminal attributes regardless of success.
/// @return Unsigned byte value on success, or zero after descriptor/termios/read
///         failure (indistinguishable from an actual NUL byte).
static int readkey_blocking(void) {
    struct termios orig, raw;
    int fd = fileno(stdin);
    if (tcgetattr(fd, &orig) != 0)
        return 0;
    raw = orig;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &raw) != 0)
        return 0;
    unsigned char ch = 0;
    ssize_t n = read(fd, &ch, 1);
    tcsetattr(fd, TCSANOW, &orig);
    return (n == 1) ? (int)ch : 0;
}

/// @brief Poll the POSIX terminal for a key without blocking.
/// @details When raw mode caching is active, uses only select() for maximum
///          performance. Otherwise falls back to the traditional approach of
///          temporarily setting raw mode for each call.
///
/// PERFORMANCE: With raw mode caching, this function does:
///   - 1 select() syscall (unavoidable for non-blocking check)
///   - 0-1 read() syscall (only if data available)
/// Without caching, each call did:
///   - 1 tcgetattr() syscall
///   - 1 tcsetattr() syscall (set raw)
///   - 1 select() or read() syscall
///   - 1 tcsetattr() syscall (restore)
/// That's 3x fewer syscalls in the hot path!
/// @param out Required output receiving the unsigned byte on success.
/// @return One when a byte was consumed; otherwise zero.
static int readkey_nonblocking(int *out) {
    int fd = g_stdin_fd >= 0 ? g_stdin_fd : fileno(stdin);

    // Check if stdin is a TTY or a pipe/file
    if (!isatty(fd)) {
        // For pipes/files: use select() to check for data without blocking
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);

        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 0;

        int ret = select(fd + 1, &readfds, NULL, NULL, &timeout);
        if (ret > 0 && FD_ISSET(fd, &readfds)) {
            unsigned char ch = 0;
            ssize_t n = read(fd, &ch, 1);
            if (n == 1) {
                *out = (int)ch;
                return 1;
            }
        }
        return 0;
    }

    // FAST PATH: If raw mode is already active, just use select() + read()
    // This eliminates the tcgetattr/tcsetattr overhead entirely!
    if (g_raw_mode_active) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);

        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 0;

        int ret = select(fd + 1, &readfds, NULL, NULL, &timeout);
        if (ret > 0 && FD_ISSET(fd, &readfds)) {
            unsigned char ch = 0;
            ssize_t n = read(fd, &ch, 1);
            if (n == 1) {
                *out = (int)ch;
                return 1;
            }
        }
        return 0;
    }

    // SLOW PATH: Traditional approach - set raw mode temporarily
    // This is only used when raw mode caching hasn't been enabled
    struct termios orig, raw;
    if (tcgetattr(fd, &orig) != 0)
        return 0;
    raw = orig;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &raw) != 0)
        return 0;
    unsigned char ch = 0;
    ssize_t n = read(fd, &ch, 1);
    tcsetattr(fd, TCSANOW, &orig);
    if (n == 1) {
        *out = (int)ch;
        return 1;
    }
    return 0;
}
#endif

/// @brief Block for a single keystroke and return it as a BASIC string.
/// @details Delegates to @ref readkey_blocking and wraps the resulting byte via
///          @ref rt_str_chr so the runtime's string interning and ownership
///          conventions are respected. Flushes output first to ensure any
///          pending screen updates are visible before blocking.
/// @return Owned one-byte string; read failure is represented as byte zero, and
///         allocation failure may return NULL.
rt_string rt_getkey_str(void) {
    // Flush output before blocking for input so user sees current state
    rt_output_flush();
    int code = readkey_blocking();
    return rt_str_chr((int64_t)code);
}

#if defined(_WIN32)
/// @brief Wait for a keystroke with timeout; return "" if timeout expires.
/// @details Uses WaitForSingleObject to poll the console input handle with the
///          specified timeout. When a key arrives within the timeout window it is
///          read via _getch and converted to a runtime string. Flushes output
///          first to ensure any pending screen updates are visible.
/// @param timeout_ms Wait duration in milliseconds; negative blocks indefinitely.
/// @return Owned one-byte string on input, or shared empty string on timeout or
///         console error.
rt_string rt_getkey_timeout_i32(int32_t timeout_ms) {
    // Flush output before waiting for input so user sees current state
    rt_output_flush();

    if (timeout_ms < 0) {
        // Negative timeout means block indefinitely
        int code = readkey_blocking();
        return rt_str_chr((int64_t)code);
    }

    HANDLE hInput = GetStdHandle(STD_INPUT_HANDLE);
    if (hInput == INVALID_HANDLE_VALUE)
        return rt_const_cstr("");

    DWORD result = WaitForSingleObject(hInput, (DWORD)timeout_ms);
    if (result == WAIT_OBJECT_0) {
        // Input is available
        if (_kbhit()) {
            int code = _getch() & 0xFF;
            return rt_str_chr((int64_t)code);
        }
    }
    // Timeout or error
    return rt_const_cstr("");
}
#else
/// @brief Wait for a keystroke with timeout; return "" if timeout expires.
/// @details Places the terminal in raw mode and uses select() to wait for input
///          with the specified timeout. When a key arrives before the deadline it
///          is read and converted to a runtime string; otherwise the empty string
///          is returned. Flushes output first to ensure pending updates are visible.
/// @param timeout_ms Wait duration in milliseconds; negative blocks indefinitely.
/// @return Owned one-byte string on input, or shared empty string on timeout,
///         descriptor, termios, select, or read failure.
rt_string rt_getkey_timeout_i32(int32_t timeout_ms) {
    // Flush output before waiting for input so user sees current state
    rt_output_flush();

    if (timeout_ms < 0) {
        // Negative timeout means block indefinitely
        int code = readkey_blocking();
        return rt_str_chr((int64_t)code);
    }

    struct termios orig, raw;
    int fd = fileno(stdin);
    if (tcgetattr(fd, &orig) != 0)
        return rt_const_cstr("");

    // Set raw mode
    raw = orig;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &raw) != 0)
        return rt_const_cstr("");

    // Use select to wait with timeout
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(fd, &readfds);

    struct timeval timeout;
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;

    int ret = select(fd + 1, &readfds, NULL, NULL, &timeout);

    unsigned char ch = 0;
    if (ret > 0 && FD_ISSET(fd, &readfds)) {
        // Data is available
        ssize_t n = read(fd, &ch, 1);
        tcsetattr(fd, TCSANOW, &orig);
        if (n == 1)
            return rt_str_chr((int64_t)ch);
    } else {
        // Timeout or error
        tcsetattr(fd, TCSANOW, &orig);
    }

    return rt_const_cstr("");
}
#endif

/// @brief Non-blocking key read that returns "" when no key is pending.
/// @details Uses @ref readkey_nonblocking to poll the console.  When a key is
///          available it is converted using @ref rt_str_chr; otherwise the canonical
///          empty string from @ref rt_const_cstr is returned. Flushes output
///          first to ensure the screen is up-to-date when polling.
/// @return Owned one-byte string when input was consumed, or the shared empty
///         singleton when no byte is available.
rt_string rt_inkey_str(void) {
    // Flush output so user sees current state when we check for input
    rt_output_flush();
    int code = 0;
    int ok = readkey_nonblocking(&code);
    if (ok)
        return rt_str_chr((int64_t)code);
    return rt_const_cstr(""); // use your runtime's empty-string helper
}

#if defined(_WIN32)
/// @brief Check if a key is available in the input buffer without reading it.
/// @details Returns non-zero if a key is pending, zero otherwise.
/// @return One when `_kbhit` reports pending console input; otherwise zero.
int32_t rt_keypressed(void) {
    return _kbhit() ? 1 : 0;
}
#else
/// @brief Check if a key is available in the input buffer without reading it.
/// @details Uses select() with zero timeout to poll the terminal. Returns non-zero
///          if a key is pending, zero otherwise. When stdin is a pipe or file,
///          directly uses select() without modifying terminal settings.
///
/// PERFORMANCE: When raw mode caching is active, this only does a single
///              select() syscall instead of tcgetattr + tcsetattr + select + tcsetattr.
/// @return One when stdin is readable without blocking; otherwise zero.
int32_t rt_keypressed(void) {
    int fd = g_stdin_fd >= 0 ? g_stdin_fd : fileno(stdin);

    // For pipes/files: just use select directly
    if (!isatty(fd)) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);

        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 0;

        int ret = select(fd + 1, &readfds, NULL, NULL, &timeout);
        return (ret > 0 && FD_ISSET(fd, &readfds)) ? 1 : 0;
    }

    // FAST PATH: If raw mode is already active, just use select()
    if (g_raw_mode_active) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);

        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 0;

        int ret = select(fd + 1, &readfds, NULL, NULL, &timeout);
        return (ret > 0 && FD_ISSET(fd, &readfds)) ? 1 : 0;
    }

    // SLOW PATH: For TTY when raw mode not cached - set raw mode temporarily
    struct termios orig, raw;
    if (tcgetattr(fd, &orig) != 0)
        return 0;
    raw = orig;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &raw) != 0)
        return 0;

    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(fd, &readfds);

    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;

    int ret = select(fd + 1, &readfds, NULL, NULL, &timeout);
    tcsetattr(fd, TCSANOW, &orig);

    return (ret > 0 && FD_ISSET(fd, &readfds)) ? 1 : 0;
}
#endif

// =============================================================================
// Output Batch Mode Control Functions
// =============================================================================

/// @brief Begin batch mode for output operations.
/// @details While in batch mode, terminal control sequences (COLOR, LOCATE,
///          etc.) do not trigger individual flushes. This dramatically improves
///          rendering performance for games and animations.
///
/// Usage in BASIC:
///   _SCREENBATCH ON   ' or _BEGINBATCH
///   ' ... multiple LOCATE, COLOR, PRINT operations ...
///   _SCREENBATCH OFF  ' or _ENDBATCH - flushes all at once
///
/// Performance: Reduces syscalls from ~6000/frame to ~1/frame for typical games.
void rt_term_begin_batch(void) {
    rt_output_begin_batch();
}

/// @brief End batch mode and flush accumulated output.
/// @details Decrements the batch mode reference count. When it reaches zero,
///          all accumulated output is flushed to the terminal in a single
///          system call, eliminating screen flashing.
void rt_term_end_batch(void) {
    rt_output_end_batch();
}

/// @brief Explicitly flush terminal output.
/// @details Forces all buffered output to be written immediately. Useful when
///          you need to ensure output is visible without ending batch mode.
void rt_term_flush(void) {
    rt_output_flush();
}

// =============================================================================
// i64 Wrappers (for frontends that use 64-bit integers)
// =============================================================================

/// @brief Saturating narrow of a 64-bit terminal parameter to 32 bits.
/// @details Clamps to `[INT32_MIN, INT32_MAX]` instead of taking the low 32 bits,
///          so a large positive value cannot wrap to a negative one (which made a
///          big timeout block forever and large cursor/color values change sign
///          and take unrelated branches) (VDOC-221).
/// @param v Signed 64-bit value to narrow.
/// @return Exactly @p v when representable, otherwise the nearest int32 bound.
static int32_t term_clamp_i32(int64_t v) {
    if (v > INT32_MAX)
        return INT32_MAX;
    if (v < INT32_MIN)
        return INT32_MIN;
    return (int32_t)v;
}

/// @brief Move cursor to position (i64 wrapper; clamps, does not wrap).
/// @param row One-based row saturated to int32 before minimum-one clamping.
/// @param col One-based column saturated to int32 before minimum-one clamping.
void rt_term_locate(int64_t row, int64_t col) {
    rt_term_locate_i32(term_clamp_i32(row), term_clamp_i32(col));
}

/// @brief Set terminal colors (i64 wrapper; clamps, does not wrap).
/// @param fg Foreground code saturated to int32.
/// @param bg Background code saturated to int32.
void rt_term_color(int64_t fg, int64_t bg) {
    rt_term_color_i32(term_clamp_i32(fg), term_clamp_i32(bg));
}

/// @brief Set foreground text color only.
/// @param fg Foreground code saturated to int32; background remains unchanged.
void rt_term_textcolor(int64_t fg) {
    rt_term_color_i32(term_clamp_i32(fg), -1);
}

/// @brief Set background color only.
/// @param bg Background code saturated to int32; foreground remains unchanged.
void rt_term_textbg(int64_t bg) {
    rt_term_color_i32(-1, term_clamp_i32(bg));
}

/// @brief Hide cursor.
void rt_term_hide_cursor(void) {
    rt_term_cursor_visible_i32(0);
}

/// @brief Show cursor.
void rt_term_show_cursor(void) {
    rt_term_cursor_visible_i32(1);
}

/// @brief Set cursor visibility (i64 wrapper for ZannaLang).
/// @details Narrows by direct C cast to int32; zero hides and nonzero shows.
/// @param show Visibility value.
void rt_term_cursor_visible(int64_t show) {
    rt_term_cursor_visible_i32((int32_t)show);
}

/// @brief Set alt screen mode (i64 wrapper for ZannaLang).
/// @details Narrows by direct C cast to int32 before boolean interpretation.
/// @param enable Alternate-screen value.
void rt_term_alt_screen(int64_t enable) {
    rt_term_alt_screen_i32((int32_t)enable);
}

/// @brief Sleep for specified milliseconds (i64 wrapper).
/// @details Narrows by direct C cast to int32 before delegating.
/// @param ms Millisecond count.
void rt_sleep_ms_i64(int64_t ms) {
    rt_sleep_ms((int32_t)ms);
}

/// @brief Check if a key is available (i64 wrapper).
/// @return Zero or one widened from @ref rt_keypressed.
int64_t rt_keypressed_i64(void) {
    return (int64_t)rt_keypressed();
}

/// @brief Get key with timeout (i64 wrapper; clamps a large timeout to
///        INT32_MAX instead of wrapping to a negative value that would block
///        indefinitely) (VDOC-221).
/// @param timeout_ms Timeout saturated to int32 milliseconds.
/// @return Owned key or empty result from @ref rt_getkey_timeout_i32.
rt_string rt_getkey_timeout(int64_t timeout_ms) {
    return rt_getkey_timeout_i32(term_clamp_i32(timeout_ms));
}
