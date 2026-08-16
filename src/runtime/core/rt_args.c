//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_args.c
/// @file
/// @brief Implements context-scoped arguments and process environment access.
///
// Purpose: Implements the Zanna.System.Environment class - access to command-line
//          arguments (argc/argv) and environment variables. Provides argument
//          count/value queries, lossy space-joined command-line display, and
//          getenv/setenv/hasenv wrappers for the BASIC runtime ABI.
//
// Key invariants:
//   - The store contains the program arguments exposed to Zanna code. Tool
//     runners pass only arguments after "--"; index 0 is the first user
//     argument, not the zanna driver executable.
//   - Native legacy fallback may populate the store from the host process argv
//     when no runner explicitly initialized or cleared arguments.
//   - Out-of-range indices trap rather than fabricating values.
//   - Environment variable names are case-sensitive on Unix and case-insensitive
//     on Windows (platform behavior is preserved transparently).
//   - SetVariable affects the current process environment. Subsequent child
//     processes inherit it unless their launch API supplies a replacement block.
//   - Legacy host-argv initialization uses a process-global atomic state
//     machine. Exactly one first reader imports while concurrent readers yield
//     until the completed state is published.
//   - Returned rt_string values transfer a reference to the caller. Empty
//     results may use the shared empty-string singleton.
//
// Ownership/Lifetime:
//   - Pushed arguments are retained by the store.
//   - Returned rt_string values transfer ownership to the caller; callers must
//     call rt_string_unref when done.
//
// Links: src/runtime/core/rt_args.h (public API),
//        src/runtime/core/rt_context.h (RtContext stores argc/argv),
//        src/runtime/core/rt_string_builder.c (command-line reconstruction)
//
//===----------------------------------------------------------------------===//

#include "rt_args.h"
#include "rt_context.h"
#include "rt_parse.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "rt_string_internal.h"
#include "rt_context_internal.h"
#include "rt_internal.h"
#include "rt_string_builder.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

/// @brief Splits a Windows command line into a system-allocated UTF-16 argv array.
/// @param lpCmdLine Null-terminated command line to parse.
/// @param pNumArgs Receives the number of returned argument pointers.
/// @return Buffer released by the caller with `LocalFree`, or null on failure.
LPWSTR *WINAPI CommandLineToArgvW(LPCWSTR lpCmdLine, int *pNumArgs);
#elif defined(__APPLE__)
#include <crt_externs.h>
#include <sched.h>
#else
#include <sched.h>
#endif

/// @brief Process-wide legacy host-argv import state.
/// @details State 0 is uninitialized, 1 means one thread is importing, and 2
///          means import is complete or suppressed. Acquire/release ordering
///          prevents readers from observing a partially populated store.
static atomic_int g_legacy_args_host_init_state; // zero-initialized = 0

/// @brief Yield the CPU while spin-waiting for another thread's import.
/// @note Uses `SwitchToThread`/`Sleep(0)` on Windows and `sched_yield` elsewhere.
static void rt_args_spin_yield(void) {
#ifdef _WIN32
    if (!SwitchToThread())
        Sleep(0);
#else
    sched_yield();
#endif
}

/// @brief Ensures an argument-state buffer can hold @p new_size items.
/// @param state Argument state whose storage may be reallocated.
/// @param new_size Minimum required element capacity.
/// @return Nonzero on success; zero for null state or after raising a trap.
static int rt_args_grow_if_needed(RtArgsState *state, size_t new_size);

#ifdef _WIN32
/// @brief Converts a known-length UTF-16 span to a runtime UTF-8 string.
/// @param wide UTF-16 code units to decode.
/// @param wide_len Number of code units at @p wide.
/// @param context Fallback diagnostic for malformed input.
/// @return New UTF-8 runtime string, or the shared empty string for empty input.
static rt_string rt_env_wide_to_string_or_trap(const wchar_t *wide,
                                               int wide_len,
                                               const char *context);
/// @brief Converts a UTF-8 byte span to a caller-owned UTF-16 buffer.
/// @param utf8 UTF-8 bytes to decode.
/// @param utf8_len Number of bytes at @p utf8.
/// @param context Fallback diagnostic for malformed input.
/// @return Heap-allocated null-terminated UTF-16 string, or null for null input.
static wchar_t *rt_env_utf8_span_to_wide_or_trap(const char *utf8,
                                                 size_t utf8_len,
                                                 const char *context);
#endif

/// @brief Lock and return the effective context containing the argument store.
/// @details Uses the context handoff-aware acquisition API, so unbound native
///          threads cannot race first/last-binding migration of the legacy
///          argument buffer. The caller must release `RT_CONTEXT_STATE_ARGS`.
/// @param is_legacy Optional output identifying the process fallback context.
/// @return Locked effective context, or NULL after initialization failure.
/// @warning A nonnull result must be paired with
///          `rt_context_release_state(..., RT_CONTEXT_STATE_ARGS)`.
static RtContext *rt_args_context_with_kind(int *is_legacy) {
    return rt_context_acquire_state(RT_CONTEXT_STATE_ARGS, is_legacy);
}

/// @brief Mark the legacy-context host-init state as complete.
/// @details Flips the init flag from `0` (uninitialised) or `2` (already done)
///          to `2`. Refuses to overwrite state `1` (initialisation in progress
///          on another thread) so a re-entrant call doesn't race past the
///          population step. Used by manual mutation paths to suppress an
///          impending lazy host-argv import.
/// @note The compare/exchange changes only state 0 to state 2 and deliberately
///       leaves an in-progress state 1 untouched.
static void rt_args_mark_legacy_host_initialized(void) {
    // Move 0 -> 2 (suppress a not-yet-started lazy import). If the state is
    // already 1 (another thread mid-import) or 2 (done), leave it: the CAS fails
    // and we do not clobber the in-progress or completed state.
    int expected = 0;
    atomic_compare_exchange_strong_explicit(
        &g_legacy_args_host_init_state, &expected, 2, memory_order_acq_rel, memory_order_acquire);
}

/// @brief Record that the user code has manually pushed/cleared arguments.
/// @details Once user code has mutated the legacy context, the lazy host-argv
///          import must not run — it would overwrite the user-supplied list.
///          Calling this from `rt_args_push` / `rt_args_clear` makes those
///          entry points idempotent with respect to host initialisation.
/// @param is_legacy True when the mutation targeted the legacy context.
static void rt_args_note_manual_mutation(int is_legacy) {
    if (is_legacy)
        rt_args_mark_legacy_host_initialized();
}

/// @brief Append @p s to @p state's argv list, retaining the string.
/// @details Grows the underlying capacity if needed. `NULL` is normalised to
///          the empty string so callers don't need to guard. Retains the
///          incoming string on success; on grow failure the caller's
///          reference is unchanged.
/// @param state Args state to append to (may be `NULL`).
/// @param s     String to append; `NULL` treated as empty.
/// @note This stores a retained reference, not a bytewise copy.
static void rt_args_append(RtArgsState *state, rt_string s) {
    if (!state)
        return;
    if (!rt_args_grow_if_needed(state, state->size + 1))
        return;
    if (!s)
        s = rt_str_empty();
    else
        rt_string_ref(s);
    state->items[state->size++] = s;
}

/// @brief Append a raw byte run to @p state by first wrapping it in an `rt_string`.
/// @details Convenience around `rt_args_append` for callers (host-argv import)
///          that read raw C strings. The temporary is unreffed after append so
///          only the state's retained reference survives.
/// @param state Args state to append to.
/// @param bytes Pointer to the byte run; `NULL` treated as empty.
/// @param len   Length in bytes (only honoured when @p bytes is non-NULL).
/// @note The temporary runtime string is released after the state retains it.
static void rt_args_append_bytes(RtArgsState *state, const char *bytes, size_t len) {
    rt_string tmp = rt_string_from_bytes(bytes ? bytes : "", bytes ? len : 0);
    rt_args_append(state, tmp);
    rt_string_unref(tmp);
}

/// @brief Populate @p state from the host operating system's argv on the active
///        platform.
/// @details Compiled in three platform-specific variants:
///          - **Windows**: `CommandLineToArgvW(GetCommandLineW(), …)` for UTF-16
///            argv, then converts each element to UTF-8 via the wide-to-string
///            helper. The Win32 buffer is freed with `LocalFree` before return.
///          - **Apple**: `_NSGetArgv()` / `_NSGetArgc()` to walk the embedded
///            byte argv (already UTF-8 on macOS).
///          - **Linux**: reads `/proc/self/cmdline`, which stores arguments
///            separated by NUL bytes, into a heap buffer and splits on every NUL.
///          - **Other**: no-op (the legacy context simply stays empty).
/// @param state Args state to populate.
/// @note The imported sequence is the host process argv and may include the
///       executable path, unlike explicitly supplied tool-runner arguments.
#if defined(_WIN32)
/// @brief Populates @p state from the parsed Windows process command line.
/// @param state Argument state receiving converted UTF-8 strings.
static void rt_args_populate_host(RtArgsState *state) {
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
        return;

    for (int i = 0; i < argc; ++i) {
        int len = 0;
        while (argv[i] && argv[i][len] != L'\0')
            ++len;
        rt_string tmp = rt_env_wide_to_string_or_trap(
            argv[i], len, "Environment: command-line UTF-16 to UTF-8 conversion failed");
        rt_args_append(state, tmp);
        rt_string_unref(tmp);
    }

    LocalFree(argv);
}
#elif defined(__APPLE__)
/// @brief Populates @p state from the macOS process argc/argv accessors.
/// @param state Argument state receiving copied host argument strings.
static void rt_args_populate_host(RtArgsState *state) {
    int *argc_ptr = _NSGetArgc();
    char ***argv_ptr = _NSGetArgv();
    if (!argc_ptr || !argv_ptr || !*argv_ptr)
        return;
    for (int i = 0; i < *argc_ptr; ++i) {
        const char *arg = (*argv_ptr)[i];
        rt_args_append_bytes(state, arg, arg ? strlen(arg) : 0);
    }
}
#elif defined(__linux__)
/// @brief Populates @p state from null-delimited `/proc/self/cmdline` bytes.
/// @param state Argument state receiving copied host argument strings.
static void rt_args_populate_host(RtArgsState *state) {
    FILE *file = fopen("/proc/self/cmdline", "rb");
    if (!file)
        return;

    char *data = NULL;
    size_t size = 0;
    size_t cap = 0;
    char chunk[512];
    while (1) {
        size_t n = fread(chunk, 1, sizeof(chunk), file);
        if (n > 0) {
            if (size + n > cap) {
                size_t next_cap = cap ? cap * 2 : 1024;
                while (next_cap < size + n)
                    next_cap *= 2;
                char *next = (char *)realloc(data, next_cap);
                if (!next) {
                    free(data);
                    fclose(file);
                    rt_trap("Environment: command-line allocation failed");
                    return;
                }
                data = next;
                cap = next_cap;
            }
            memcpy(data + size, chunk, n);
            size += n;
        }
        if (n < sizeof(chunk)) {
            if (feof(file))
                break;
            if (ferror(file)) {
                free(data);
                fclose(file);
                return;
            }
        }
    }
    fclose(file);

    size_t start = 0;
    for (size_t i = 0; i <= size; ++i) {
        if (i == size || data[i] == '\0') {
            if (i > start || i < size)
                rt_args_append_bytes(state, data + start, i - start);
            start = i + 1;
        }
    }
    free(data);
}
#else
/// @brief Leaves @p state unchanged on platforms without a host-argv adapter.
/// @param state Unused argument state.
static void rt_args_populate_host(RtArgsState *state) {
    (void)state;
}
#endif

/// @brief Lazily import host argv into @p state on the first legacy-context read.
/// @details The legacy context delays host-argv import until first read so a
///          process that wants to provide its own argv (test harness, tool
///          runner) gets a chance to push first. Intended state transitions:
///            - `0` → `1` → `2`: first caller runs the import.
///            - `2`: no-op.
///            - `1`: another caller appears to be mid-import; wait for `2`.
///          The flag is an atomic with acquire/release ordering and the `0` → `1`
///          claim is a single CAS, so exactly one thread imports and concurrent
///          first readers safely observe the completed arguments (VDOC-211).
/// @param state Legacy args state to populate (no-op when NULL).
static void rt_args_ensure_legacy_host_initialized(RtArgsState *state) {
    if (!state)
        return;
    if (atomic_load_explicit(&g_legacy_args_host_init_state, memory_order_acquire) == 2)
        return;

    // Claim the import with an atomic 0 -> 1 transition. Exactly one thread wins;
    // it populates the args and publishes state 2 with release ordering so the
    // populated arguments are visible to any thread that later acquire-loads 2.
    int expected = 0;
    if (atomic_compare_exchange_strong_explicit(&g_legacy_args_host_init_state,
                                                &expected,
                                                1,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        if (state->size == 0)
            rt_args_populate_host(state);
        atomic_store_explicit(&g_legacy_args_host_init_state, 2, memory_order_release);
        return;
    }

    // Another thread is importing (or has finished). Wait — with a real yield on
    // every platform — until it publishes state 2.
    while (atomic_load_explicit(&g_legacy_args_host_init_state, memory_order_acquire) == 1)
        rt_args_spin_yield();
}

/// @brief Resolve the active args state for a read, triggering lazy host-import.
/// @details Combines `rt_args_context_with_kind` with
///          `rt_args_ensure_legacy_host_initialized` so a fresh process that
///          hasn't pushed any args yet still observes the host argv on first
///          read. Active (non-legacy) contexts skip the lazy import — those
///          contexts are owned by an explicit caller (tool runner, REPL).
/// @param out_ctx Receives the locked owning context for later release.
/// @return The args state to read from; `NULL` if no context is active.
/// @warning When @p out_ctx receives a nonnull context, the caller must release
///          its argument-state lock after finishing the read.
static RtArgsState *rt_args_query_state(RtContext **out_ctx) {
    if (out_ctx)
        *out_ctx = NULL;
    int is_legacy = 0;
    RtContext *ctx = rt_args_context_with_kind(&is_legacy);
    RtArgsState *state = ctx ? &ctx->args_state : NULL;
    if (is_legacy)
        rt_args_ensure_legacy_host_initialized(state);
    if (out_ctx)
        *out_ctx = ctx;
    return state;
}

#ifdef _WIN32
/// @brief Convert a UTF-8 byte span to a heap-allocated wide string (Windows).
/// @details Performs strict in-tree UTF-8 decoding so native PE binaries do not
///          depend on Win32 conversion helpers before runtime heap allocation is
///          fully usable. Caller owns the returned buffer and must `free` it.
///          Traps on malformed UTF-8 or allocation failure.
/// @param utf8 UTF-8 byte span to decode; null returns null.
/// @param utf8_len Number of bytes at @p utf8.
/// @param context Diagnostic used for malformed/truncated input.
/// @return Heap-allocated null-terminated UTF-16 data owned by the caller.
static wchar_t *rt_env_utf8_span_to_wide_or_trap(const char *utf8,
                                                 size_t utf8_len,
                                                 const char *context) {
    if (!utf8)
        return NULL;
    if (utf8_len > SIZE_MAX / sizeof(wchar_t) - 1u) {
        rt_trap("Environment: allocation size overflow");
        return NULL;
    }
    wchar_t *wide = (wchar_t *)malloc((utf8_len + 1u) * sizeof(wchar_t));
    if (!wide) {
        rt_trap("Environment: allocation failed");
        return NULL;
    }

    size_t out = 0;
    for (size_t i = 0; i < utf8_len;) {
        unsigned char c = (unsigned char)utf8[i++];
        uint32_t cp = 0;
        int need = 0;
        uint32_t min_cp = 0;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0u) == 0xC0u) {
            cp = c & 0x1Fu;
            need = 1;
            min_cp = 0x80u;
        } else if ((c & 0xF0u) == 0xE0u) {
            cp = c & 0x0Fu;
            need = 2;
            min_cp = 0x800u;
        } else if ((c & 0xF8u) == 0xF0u) {
            cp = c & 0x07u;
            need = 3;
            min_cp = 0x10000u;
        } else {
            free(wide);
            rt_trap(context ? context : "Environment: invalid UTF-8");
            return NULL;
        }

        if (i + (size_t)need > utf8_len) {
            free(wide);
            rt_trap(context ? context : "Environment: truncated UTF-8");
            return NULL;
        }
        for (int j = 0; j < need; ++j) {
            unsigned char cc = (unsigned char)utf8[i++];
            if ((cc & 0xC0u) != 0x80u) {
                free(wide);
                rt_trap(context ? context : "Environment: invalid UTF-8 continuation");
                return NULL;
            }
            cp = (cp << 6) | (uint32_t)(cc & 0x3Fu);
        }

        if (cp < min_cp || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
            free(wide);
            rt_trap(context ? context : "Environment: invalid UTF-8 codepoint");
            return NULL;
        }
        if (cp <= 0xFFFFu) {
            wide[out++] = (wchar_t)cp;
        } else {
            cp -= 0x10000u;
            wide[out++] = (wchar_t)(0xD800u + (cp >> 10));
            wide[out++] = (wchar_t)(0xDC00u + (cp & 0x3FFu));
        }
    }
    wide[out] = L'\0';
    return wide;
}

/// @brief Convert a wide string of known length to an `rt_string` (UTF-8) on Windows.
/// @details Inverse of `rt_env_utf8_span_to_wide_or_trap`. Performs strict
///          in-tree UTF-16 decoding and traps instead of silently losing data
///          on malformed input from the OS.
/// @param wide UTF-16 code units to decode.
/// @param wide_len Number of code units at @p wide.
/// @param context Diagnostic used for malformed/truncated input.
/// @return New runtime UTF-8 string, or the shared empty string for null/empty input.
static rt_string rt_env_wide_to_string_or_trap(const wchar_t *wide,
                                               int wide_len,
                                               const char *context) {
    if (!wide || wide_len <= 0)
        return rt_str_empty();

    size_t needed = 0;
    for (int i = 0; i < wide_len; ++i) {
        uint32_t cp = (uint32_t)wide[i];
        if (cp >= 0xD800u && cp <= 0xDBFFu) {
            if (i + 1 >= wide_len) {
                rt_trap(context ? context : "Environment: truncated UTF-16 surrogate pair");
                return rt_str_empty();
            }
            uint32_t lo = (uint32_t)wide[++i];
            if (lo < 0xDC00u || lo > 0xDFFFu) {
                rt_trap(context ? context : "Environment: invalid UTF-16 surrogate pair");
                return rt_str_empty();
            }
            cp = 0x10000u + (((cp - 0xD800u) << 10) | (lo - 0xDC00u));
        } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
            rt_trap(context ? context : "Environment: unpaired UTF-16 surrogate");
            return rt_str_empty();
        }
        size_t encoded_bytes = (cp < 0x80u) ? 1u : (cp < 0x800u) ? 2u : (cp < 0x10000u) ? 3u : 4u;
        if (needed > SIZE_MAX - encoded_bytes) {
            rt_trap("Environment: UTF-8 size overflow");
            return rt_str_empty();
        }
        needed += encoded_bytes;
    }

    char *buffer = (char *)malloc(needed ? needed : 1u);
    if (!buffer) {
        rt_trap("Environment: allocation failed");
        return rt_str_empty();
    }

    size_t out = 0;
    for (int i = 0; i < wide_len; ++i) {
        uint32_t cp = (uint32_t)wide[i];
        if (cp >= 0xD800u && cp <= 0xDBFFu) {
            uint32_t lo = (uint32_t)wide[++i];
            cp = 0x10000u + (((cp - 0xD800u) << 10) | (lo - 0xDC00u));
        }
        if (cp < 0x80u) {
            buffer[out++] = (char)cp;
        } else if (cp < 0x800u) {
            buffer[out++] = (char)(0xC0u | (cp >> 6));
            buffer[out++] = (char)(0x80u | (cp & 0x3Fu));
        } else if (cp < 0x10000u) {
            buffer[out++] = (char)(0xE0u | (cp >> 12));
            buffer[out++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            buffer[out++] = (char)(0x80u | (cp & 0x3Fu));
        } else {
            buffer[out++] = (char)(0xF0u | (cp >> 18));
            buffer[out++] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
            buffer[out++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            buffer[out++] = (char)(0x80u | (cp & 0x3Fu));
        }
    }
    rt_string out_str = rt_string_from_bytes(buffer, out);
    free(buffer);
    return out_str;
}
#endif

/// @brief Ensure the args array has capacity for at least @p new_size entries.
/// @details Doubles capacity until sufficient; traps on overflow or alloc failure.
/// @param state Argument state whose item buffer may be reallocated.
/// @param new_size Minimum number of item slots required.
/// @return Nonzero when sufficient capacity exists; zero for null state or
///         after raising a capacity/allocation trap.
static int rt_args_grow_if_needed(RtArgsState *state, size_t new_size) {
    if (!state)
        return 0;
    if (new_size <= state->cap)
        return 1;
    size_t new_cap = state->cap ? state->cap * 2 : 8;
    while (new_cap < new_size) {
        if (new_cap > SIZE_MAX / 2) {
            rt_trap("rt_args: capacity overflow");
            return 0;
        }
        new_cap *= 2;
    }
    if (new_cap > SIZE_MAX / sizeof(rt_string)) {
        rt_trap("rt_args: size overflow");
        return 0;
    }
    rt_string *next = (rt_string *)realloc(state->items, new_cap * sizeof(rt_string));
    if (!next) {
        rt_trap("rt_args: allocation failed");
        return 0;
    }
    state->items = next;
    state->cap = new_cap;
    return 1;
}

/// @brief Clear all stored command-line arguments.
///
/// Releases all stored argument strings and resets the argument count to zero.
/// This is typically called during context cleanup or when reinitializing
/// the argument state.
///
/// @note Internal use - typically not called directly from Zanna code.
/// @note Releases references to all stored strings.
/// @note Preserves allocated item capacity for reuse and suppresses future
///       lazy host-argv import when mutating the legacy context.
void rt_args_clear(void) {
    int is_legacy = 0;
    RtContext *ctx = rt_args_context_with_kind(&is_legacy);
    RtArgsState *state = ctx ? &ctx->args_state : NULL;
    if (!state)
        return;
    for (size_t i = 0; i < state->size; ++i) {
        if (state->items[i])
            rt_string_unref(state->items[i]);
        state->items[i] = NULL;
    }
    state->size = 0;
    rt_args_note_manual_mutation(is_legacy);
    rt_context_release_state(ctx, RT_CONTEXT_STATE_ARGS);
}

/// @brief Add a command-line argument to the argument store.
///
/// Appends an argument string to the argument list. Used during program
/// initialization to populate arguments supplied to the Zanna program.
///
/// @param s Argument string to add. NULL is stored as empty string.
///
/// @note Internal use - arguments are set up by the runtime before main runs.
/// @note The string handle is retained rather than copied byte-for-byte.
/// @note Traps on allocation failure.
/// @note A successful legacy-context mutation suppresses lazy host-argv import.
void rt_args_push(rt_string s) {
    int is_legacy = 0;
    RtContext *ctx = rt_args_context_with_kind(&is_legacy);
    RtArgsState *state = ctx ? &ctx->args_state : NULL;
    if (!state)
        return;
    rt_args_append(state, s);
    rt_args_note_manual_mutation(is_legacy);
    rt_context_release_state(ctx, RT_CONTEXT_STATE_ARGS);
}

/// @brief Get the number of command-line arguments.
///
/// Returns the total count of stored program arguments. Tool runners populate
/// this from arguments after "--"; native legacy fallback may expose host argv
/// when no runner initialized the store.
///
/// **Usage example:**
/// ```
/// Dim count = Environment.GetArgumentCount()
/// If count < 2 Then
///     Print "Usage: program <filename>"
///     Environment.Exit(1)
/// End If
/// ```
///
/// @return Number of stored arguments, or 0 if none were supplied.
///
/// @note Under tool runners, index 0 is the first user argument.
/// @note O(1) time complexity.
/// @note The first read of an untouched legacy context may atomically import
///       host argv before computing the count.
///
/// @see rt_args_get For retrieving individual arguments
int64_t rt_args_count(void) {
    RtContext *ctx = NULL;
    RtArgsState *state = rt_args_query_state(&ctx);
    int64_t count = state ? (int64_t)state->size : 0;
    if (ctx)
        rt_context_release_state(ctx, RT_CONTEXT_STATE_ARGS);
    return count;
}

/// @brief Get a command-line argument by index.
///
/// Retrieves the argument at the specified index.
///
/// **Usage example:**
/// ```
/// ' Process arguments
/// For i = 0 To Environment.GetArgumentCount() - 1
///     Dim arg = Environment.GetArgument(i)
///     ProcessArgument(arg)
/// Next
/// ```
///
/// @param index Zero-based index of the argument to retrieve.
///
/// @return Retained reference to the argument string at the specified index.
///
/// @note Traps if index is out of range.
/// @note Returns a new reference (caller must manage memory).
/// @note The first read of an untouched legacy context may import host argv.
///
/// @see rt_args_count For getting the argument count
rt_string rt_args_get(int64_t index) {
    RtContext *ctx = NULL;
    RtArgsState *state = rt_args_query_state(&ctx);
    if (!state)
        return NULL;
    if (index < 0 || (size_t)index >= state->size) {
        rt_context_release_state(ctx, RT_CONTEXT_STATE_ARGS);
        rt_trap("rt_args_get: index out of range");
        return NULL;
    }
    rt_string s = state->items[index];
    // Return retained reference to match common getter semantics
    rt_string result = rt_string_ref(s);
    rt_context_release_state(ctx, RT_CONTEXT_STATE_ARGS);
    return result;
}

/// @brief Get the full command line as a single string.
///
/// Returns all command-line arguments concatenated with spaces. This is
/// useful for display but does not preserve the exact invocation.
///
/// **Usage example:**
/// ```
/// ' Log how the program was invoked
/// Dim cmdLine = Environment.GetCommandLine()
/// Log("Started with: " & cmdLine)
/// ```
///
/// @return New string containing all arguments separated by spaces, or the
///         shared empty string when no arguments exist or builder growth fails.
///
/// @note Returns empty string if no arguments are available.
/// @note Arguments are joined with single spaces.
/// @note No quoting or escaping is added, so original argument boundaries
///       cannot be recovered. Embedded null bytes are truncated by the C-string
///       builder interface.
///
/// @see rt_args_get For individual argument access
/// @see rt_args_count For argument count
rt_string rt_cmdline(void) {
    RtContext *ctx = NULL;
    RtArgsState *state = rt_args_query_state(&ctx);
    if (!state || state->size == 0) {
        if (ctx)
            rt_context_release_state(ctx, RT_CONTEXT_STATE_ARGS);
        return rt_str_empty();
    }
    rt_string_builder sb;
    rt_sb_init(&sb);
    for (size_t i = 0; i < state->size; ++i) {
        const char *cstr = rt_string_cstr(state->items[i]);
        if (i > 0 && rt_sb_append_cstr(&sb, " ") != RT_SB_OK)
            goto cmdline_error;
        if (rt_sb_append_cstr(&sb, cstr ? cstr : "") != RT_SB_OK)
            goto cmdline_error;
    }
    rt_string out = rt_string_from_bytes(sb.data, sb.len);
    rt_sb_free(&sb);
    rt_context_release_state(ctx, RT_CONTEXT_STATE_ARGS);
    return out;

cmdline_error:
    rt_sb_free(&sb);
    rt_context_release_state(ctx, RT_CONTEXT_STATE_ARGS);
    return rt_str_empty();
}

/// @brief Clean up argument state for a context.
///
/// Releases all argument strings and frees the argument array. Called
/// during context destruction.
///
/// @param ctx The context whose arguments should be cleaned up.
///
/// @note Internal use only.
/// @note Unlike rt_args_clear(), this also frees the item array and resets
///       capacity to zero. A null context is ignored.
void rt_args_state_cleanup(RtContext *ctx) {
    if (!ctx)
        return;

    RtArgsState *state = &ctx->args_state;
    if (state->items) {
        for (size_t i = 0; i < state->size; ++i) {
            if (state->items[i])
                rt_string_unref(state->items[i]);
            state->items[i] = NULL;
        }
        free(state->items);
    }
    state->items = NULL;
    state->size = 0;
    state->cap = 0;
}

/// @brief Check if running in native (AOT-compiled) mode.
///
/// Returns whether the program is running as native compiled code (as opposed
/// to being interpreted by the VM). This can be used for conditional behavior
/// based on execution mode.
///
/// **Usage example:**
/// ```
/// If Environment.IsNative() Then
///     Print "Running native code"
/// Else
///     Print "Running in VM"
/// End If
/// ```
///
/// @return 1 if running as native code, 0 if running in VM.
///
/// @note In native builds, this always returns 1.
/// @note The VM overrides this to return 0.
int64_t rt_env_is_native(void) {
    // Native runtime library is only linked into AOT binaries, so this path
    // always reports "native". The VM overrides this via its runtime bridge.
    return 1;
}

/// @brief Validate environment variable name input.
/// @details Ensures @p name is non-null and non-empty before calling
///          platform environment APIs. Traps on invalid input so callers see a
///          deterministic failure instead of undefined behaviour.
/// @param name Runtime string naming the environment variable.
/// @param context Human-readable operation for diagnostics.
/// @return Borrowed null-terminated name suitable for platform environment APIs.
/// @note Null, empty, and embedded-null names raise @p context as a trap.
static const char *rt_env_require_name(rt_string name, const char *context) {
    if (!name) {
        rt_trap(context ? context : "Environment: variable name is null");
        return "";
    }

    const char *cname = rt_string_cstr(name);
    if (!cname || cname[0] == '\0') {
        rt_trap(context ? context : "Environment: variable name is empty");
        return "";
    }
    size_t c_len = strlen(cname);
    int64_t rt_len = rt_str_len(name);
    if (c_len != (size_t)rt_len) {
        rt_trap(context ? context : "Environment: variable name must not contain null bytes");
        return "";
    }
    return cname;
}

/// @brief Retrieve an environment variable's value.
/// @details Returns an empty runtime string when the variable is unset. The
///          variable name must be non-empty and contain no null byte; traps on
///          invalid input or platform/conversion failure.
/// @param name Environment variable to look up.
/// @return New runtime string containing the value, or the shared empty string
///         when missing or present with an empty value.
/// @note Use rt_env_has_var() to distinguish an unset variable from an empty one.
rt_string rt_env_get_var(rt_string name) {
    const char *cname =
        rt_env_require_name(name, "Zanna.System.Environment.GetVariable: name must not be empty");

#ifdef _WIN32
    wchar_t *wname = rt_env_utf8_span_to_wide_or_trap(
        cname,
        (size_t)rt_str_len(name),
        "Zanna.System.Environment.GetVariable: invalid UTF-8 variable name");
    if (!wname)
        return rt_str_empty();
    SetLastError(ERROR_SUCCESS);
    DWORD required = GetEnvironmentVariableW(wname, NULL, 0);
    if (required == 0) {
        DWORD err = GetLastError();
        free(wname);
        if (err == ERROR_ENVVAR_NOT_FOUND || err == ERROR_SUCCESS) {
            return rt_str_empty();
        }
        rt_trap("Zanna.System.Environment.GetVariable: failed to query variable");
        return rt_str_empty();
    }

    // The environment can change between the size probe and the read. Win32
    // reports the new required capacity when the supplied buffer became too
    // small; retry against that value instead of treating it as a string length.
    for (int attempt = 0; attempt < 16; ++attempt) {
        if (required > INT_MAX || (size_t)required > SIZE_MAX / sizeof(wchar_t)) {
            free(wname);
            rt_trap("Zanna.System.Environment.GetVariable: value is too large");
            return rt_str_empty();
        }
        wchar_t *buffer = (wchar_t *)malloc((size_t)required * sizeof(wchar_t));
        if (!buffer) {
            free(wname);
            rt_trap("Zanna.System.Environment.GetVariable: allocation failed");
            return rt_str_empty();
        }

        SetLastError(ERROR_SUCCESS);
        DWORD written = GetEnvironmentVariableW(wname, buffer, required);
        if (written == 0) {
            DWORD err = GetLastError();
            free(buffer);
            free(wname);
            if (err == ERROR_ENVVAR_NOT_FOUND || err == ERROR_SUCCESS)
                return rt_str_empty();
            rt_trap("Zanna.System.Environment.GetVariable: failed to read variable");
            return rt_str_empty();
        }
        if (written < required) {
            rt_string out = rt_env_wide_to_string_or_trap(
                buffer, (int)written, "Zanna.System.Environment.GetVariable: invalid UTF-16 value");
            free(buffer);
            free(wname);
            return out;
        }
        free(buffer);
        required = written;
    }
    free(wname);
    rt_trap("Zanna.System.Environment.GetVariable: variable changed too frequently");
    return rt_str_empty();
#else
    const char *value = getenv(cname);
    if (!value) {
        return rt_str_empty();
    }
    return rt_string_from_bytes(value, strlen(value));
#endif
}

/// @brief Determine whether an environment variable exists.
/// @details Returns 1 when @p name is present (even if its value is empty) and
///          0 otherwise. Traps on invalid names or platform-query failure.
/// @param name Environment variable to probe.
/// @return 1 if present, 0 if missing.
int64_t rt_env_has_var(rt_string name) {
    const char *cname =
        rt_env_require_name(name, "Zanna.System.Environment.HasVariable: name must not be empty");

#ifdef _WIN32
    wchar_t *wname = rt_env_utf8_span_to_wide_or_trap(
        cname,
        (size_t)rt_str_len(name),
        "Zanna.System.Environment.HasVariable: invalid UTF-8 variable name");
    if (!wname)
        return 0;
    SetLastError(ERROR_SUCCESS);
    DWORD required = GetEnvironmentVariableW(wname, NULL, 0);
    DWORD err = GetLastError();
    free(wname);
    if (required == 0 && err == ERROR_ENVVAR_NOT_FOUND)
        return 0;
    if (required == 0 && err != ERROR_SUCCESS) {
        rt_trap("Zanna.System.Environment.HasVariable: failed to query variable");
        return 0;
    }
    return 1;
#else
    const char *value = getenv(cname);
    return value ? 1 : 0;
#endif
}

/// @brief Set or overwrite an environment variable.
/// @details Accepts empty strings as values. Traps when the name is empty and
///          when conversion or the underlying platform call fails. Names and
///          values containing embedded null bytes are rejected rather than
///          silently truncated by C/Win32 environment APIs.
/// @param name Environment variable to set.
/// @param value New value (NULL treated as empty string).
/// @note The update affects the current process and normally propagates to
///       subsequently created children that inherit its environment.
void rt_env_set_var(rt_string name, rt_string value) {
    const char *cname =
        rt_env_require_name(name, "Zanna.System.Environment.SetVariable: name must not be empty");
    const char *cvalue = value ? rt_string_cstr(value) : "";

    // Reject embedded null bytes: setenv/SetEnvironmentVariableA terminate at the
    // first '\0', so a Zanna String with internal nulls would be silently truncated.
    if (strlen(cname) != (size_t)rt_str_len(name)) {
        rt_trap("Zanna.System.Environment.SetVariable: name must not contain null bytes");
        return;
    }
    if (value && strlen(cvalue) != (size_t)rt_str_len(value)) {
        rt_trap("Zanna.System.Environment.SetVariable: value must not contain null bytes");
        return;
    }

#ifdef _WIN32
    wchar_t *wname = rt_env_utf8_span_to_wide_or_trap(
        cname,
        (size_t)rt_str_len(name),
        "Zanna.System.Environment.SetVariable: invalid UTF-8 variable name");
    wchar_t *wvalue = rt_env_utf8_span_to_wide_or_trap(
        cvalue,
        value ? (size_t)rt_str_len(value) : 0,
        "Zanna.System.Environment.SetVariable: invalid UTF-8 variable value");
    int ok = SetEnvironmentVariableW(wname, wvalue) ? 1 : 0;
    free(wname);
    free(wvalue);
    if (!ok) {
        rt_trap("Zanna.System.Environment.SetVariable: failed to set variable");
    }
#else
    if (setenv(cname, cvalue, 1) != 0) {
        rt_trap("Zanna.System.Environment.SetVariable: failed to set variable");
    }
#endif
}

/// @brief Terminate the process with the provided exit code.
/// @details Delegates to the platform termination primitive. Windows native PE
///          binaries use a CRT-less startup shim, so they must not route through
///          the CRT exit machinery.
/// @param code Exit status to report.
/// @note This function does not return. The host platform narrows @p code to
///       its native exit-status width.
void rt_env_exit(int64_t code) {
#if RT_PLATFORM_WINDOWS
    ExitProcess((UINT)(uint32_t)code);
#else
    exit((int)code);
#endif
}

//=============================================================================
// Flag and option parsing (ADR 0253)
//=============================================================================
//
// The raw store below gives count-and-index access. Every CLI-facing program
// then writes the same linear scan to answer "was --flag passed?" and "what
// followed --option?" — slightly differently each time. These put one answer in
// the runtime.
//
// Deliberately minimal: no spec object, no usage generation, no subcommands.
// Those are opinionated and a program that needs them will want its own. What
// is shared between every program is the scan.

/// @brief Compare an argument against a flag name.
/// @param arg Borrowed argument string.
/// @param name Borrowed flag name.
/// @return Non-zero when they match exactly.
static int args_equals(rt_string arg, rt_string name) {
    const char *a = arg ? rt_string_cstr(arg) : NULL;
    const char *n = name ? rt_string_cstr(name) : NULL;
    if (!a || !n)
        return 0;
    return strcmp(a, n) == 0;
}

/// @brief True when @p name appears as a standalone argument.
/// @details Exact match only. `--verbose` does not match `--verbose=1`; use
///          @ref rt_args_get_option for the valued form.
/// @param name Flag to look for, including any leading dashes.
/// @return Non-zero when present.
int8_t rt_args_has_flag(rt_string name) {
    int64_t count = rt_args_count();
    for (int64_t i = 0; i < count; i++) {
        rt_string arg = rt_args_get(i);
        int hit = args_equals(arg, name);
        rt_string_unref(arg);
        if (hit)
            return 1;
    }
    return 0;
}

/// @brief Value following @p name, or @p fallback when absent.
/// @details Accepts both `--opt value` and `--opt=value`. A trailing `--opt`
///          with nothing after it returns @p fallback rather than an empty
///          string, so "absent" and "explicitly empty" stay distinguishable.
/// @param name Option to look for, including any leading dashes.
/// @param fallback Value returned when the option is absent or unvalued.
/// @return Owned value string.
rt_string rt_args_get_option(rt_string name, rt_string fallback) {
    const char *n = name ? rt_string_cstr(name) : NULL;
    if (!n)
        return fallback ? rt_string_ref(fallback) : rt_empty_string();
    size_t nlen = strlen(n);
    int64_t count = rt_args_count();
    for (int64_t i = 0; i < count; i++) {
        rt_string arg = rt_args_get(i);
        const char *a = arg ? rt_string_cstr(arg) : NULL;
        if (a) {
            if (strcmp(a, n) == 0) {
                rt_string_unref(arg);
                if (i + 1 < count)
                    return rt_args_get(i + 1);
                break;
            }
            if (strncmp(a, n, nlen) == 0 && a[nlen] == '=') {
                rt_string out = rt_string_from_bytes(a + nlen + 1, strlen(a + nlen + 1));
                rt_string_unref(arg);
                return out;
            }
        }
        rt_string_unref(arg);
    }
    return fallback ? rt_string_ref(fallback) : rt_empty_string();
}

/// @brief Integer value following @p name, or @p fallback.
/// @details Parses with the same strict decimal rules as
///          `Zanna.Core.Parse.IntOr`; anything unparseable yields @p fallback.
/// @param name Option to look for.
/// @param fallback Value returned when absent or unparseable.
/// @return Parsed integer or @p fallback.
int64_t rt_args_get_option_int(rt_string name, int64_t fallback) {
    rt_string value = rt_args_get_option(name, NULL);
    const char *v = value ? rt_string_cstr(value) : NULL;
    if (!v || !v[0]) {
        if (value)
            rt_string_unref(value);
        return fallback;
    }
    int64_t parsed = rt_parse_int_or(value, fallback);
    rt_string_unref(value);
    return parsed;
}

/// @brief Arguments that are neither flags nor option values.
/// @details An argument starting with `-` is treated as a flag, and the token
///          after a `--opt value` pair is treated as consumed. A bare `--`
///          ends option processing: everything after it is positional, which is
///          the convention every shell user already expects.
/// @return Owned Seq of positional argument strings.
void *rt_args_positionals(void) {
    // Owning Seq: rt_args_get hands back a retained reference, and
    // rt_seq_push_raw transfers that reference in without a second retain.
    void *seq = rt_seq_new_owned();
    if (!seq)
        return NULL;
    int64_t count = rt_args_count();
    int only_positional = 0;
    for (int64_t i = 0; i < count; i++) {
        rt_string arg = rt_args_get(i);
        const char *a = arg ? rt_string_cstr(arg) : NULL;
        if (!a) {
            rt_string_unref(arg);
            continue;
        }
        if (!only_positional && strcmp(a, "--") == 0) {
            only_positional = 1;
            rt_string_unref(arg);
            continue;
        }
        if (!only_positional && a[0] == '-' && a[1] != '\0') {
            // A `--opt value` pair consumes the following token; `--opt=value`
            // is self-contained.
            if (!strchr(a, '=') && i + 1 < count) {
                rt_string next = rt_args_get(i + 1);
                const char *nx = next ? rt_string_cstr(next) : NULL;
                if (nx && nx[0] != '-')
                    i++;
                rt_string_unref(next);
            }
            rt_string_unref(arg);
            continue;
        }
        rt_seq_push_raw(seq, arg);
    }
    return seq;
}
