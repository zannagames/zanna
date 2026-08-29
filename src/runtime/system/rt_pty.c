//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/system/rt_pty.c
// Purpose: Implements pseudo-terminal-backed child handles for Zanna.System.Pty.
//          Mirrors rt_process.c (ring buffer, non-blocking drain, reap, GC
//          finalizer) but establishes a controlling terminal so interactive
//          programs see a real TTY, merges output into one ANSI-bearing stream,
//          and supports window resize.
//
// Key invariants:
//   - Open returns NULL when a PTY cannot be created (incl. unsupported targets).
//   - Read is non-blocking and incremental; output is a single merged stream.
//   - Poll/IsRunning reap the child once and preserve the exit code.
//   - Destroy is idempotent and releases all OS resources.
//   - Windows command, cwd, and environment strings cross the Win32 boundary
//     as strict UTF-16; explicit environment blocks are sorted and complete.
//
// Ownership/Lifetime:
//   - Handles are rt_obj_new_i64-allocated and GC-managed; Destroy or the
//     finalizer terminates a still-running child and frees the master fd / PTY.
//
// Platform notes:
//   - POSIX: posix_openpt + fork + setsid + TIOCSCTTY + execvp (the controlling
//     terminal cannot be set up via posix_spawn). Fully exercised on macOS/Linux.
//   - Windows: ConPTY (CreatePseudoConsole/ResizePseudoConsole/ClosePseudoConsole)
//     resolved dynamically and thread-safely (Windows 10 1809+).
//
// Links: src/runtime/system/rt_pty.h, src/runtime/system/rt_process.c,
//        src/runtime/system/rt_activity_watch.h,
//        src/runtime/system/rt_async_pipe_writer.h,
//        docs/adr/0281-event-driven-process-pty-gui-wakes.md
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_pty.c
 * @brief Implements managed pseudo-terminal child sessions.
 * @details POSIX PTYs or dynamically resolved Windows ConPTY connect a direct
 *          child process to one interactive terminal stream. Sessions support
 *          bounded incremental reads, writes, window resize, polling,
 *          termination, retained exit status, and idempotent native cleanup.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "rt_pty.h"

#include "rt_activity_wake.h"
#include "rt_activity_watch.h"
#include "rt_async_pipe_writer.h"
#include "rt_internal.h"
#include "rt_map.h"
#include "rt_object.h"
#include "rt_platform.h"
#include "rt_result.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "rt_trap.h"

#include <errno.h>
#include <limits.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include "rt_win32_wait.h"
#include <wchar.h>
#include <windows.h>
#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif
typedef HRESULT(WINAPI *pty_create_pseudoconsole_fn)(COORD, HANDLE, HANDLE, DWORD, void **);
typedef HRESULT(WINAPI *pty_resize_pseudoconsole_fn)(void *, COORD);
typedef VOID(WINAPI *pty_close_pseudoconsole_fn)(void *);
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
extern char **environ;
#endif

/// @brief Initial allocation size for incremental terminal output.
#define PTY_BUFFER_INITIAL_SIZE 4096
/// @brief Maximum retained unread terminal-output bytes.
#define PTY_BUFFER_MAX_SIZE (16 * 1024 * 1024)
/// @brief Maximum time finalization waits for an asynchronously terminated ConPTY child.
#define PTY_WINDOWS_TERMINATE_WAIT_MS 5000u
/// @brief Maximum copied terminal-input bytes awaiting the Windows writer.
#define PTY_WINDOWS_INPUT_QUEUE_MAX_SIZE (1024 * 1024)
/// @brief Capacity of the per-thread PTY diagnostic buffer.
#define PTY_LAST_ERROR_MAX 256

/// @brief Incremental merged-output buffer for one PTY session.
/// @details Storage is retained for reuse between reads. Bytes beyond the
///          16 MiB cap are discarded and reported by the next take operation.
typedef struct pty_buffer {
    char *data;    ///< Owned allocation, or NULL before the first append.
    size_t len;    ///< Number of unread bytes currently retained.
    size_t cap;    ///< Allocated byte capacity of @c data.
    int truncated; ///< Whether bytes were discarded since the previous take.
} pty_buffer;

/// @brief Temporary NULL-terminated argv or envp representation.
/// @details Retained runtime strings keep every byte view in @c values valid
///          until the fork/exec or process-creation setup is complete.
typedef struct pty_string_vector {
    char **values;            ///< Owned pointer array ending in NULL.
    rt_string *owned_strings; ///< Owned array of retained runtime strings.
    int64_t owned_count;      ///< Number of entries in @c owned_strings.
} pty_string_vector;

/// @brief Platform state stored in a Zanna.System.Pty session object.
/// @details Owns the merged terminal buffer, child identity, and every
///          parent-side ConPTY handle or POSIX master descriptor until Destroy
///          or GC finalization.
typedef struct rt_pty_impl {
    int8_t started;
    int8_t running;
    int8_t destroyed;
    int8_t finalizing;
    int64_t exit_code;
    pty_buffer output_buf;
    rt_activity_wake_target *activity_wake;
    rt_activity_watch *activity_watch;
    volatile int output_activity_closed;

#if defined(_WIN32)
    HANDLE process;
    HANDLE thread;
    HANDLE input_write; // parent -> child
    rt_async_pipe_writer *input_writer;
    HANDLE output_read; // child -> parent
    void *hpc;          // HPCON
#else
    pid_t pid;
    int master_fd;
#endif
} rt_pty_impl;

static void pty_finalize(void *obj);
static void pty_close(rt_pty_impl *pty);
static int pty_activity_monitor_start(rt_pty_impl *pty);
static void pty_activity_monitor_stop(rt_pty_impl *pty);
static void pty_activity_rearm(rt_pty_impl *pty);
static int pty_activity_probe(void *context);

static int pty_activity_monitor_start(rt_pty_impl *pty) {
    if (!pty || pty->activity_watch)
        return pty && pty->activity_watch;
    pty->activity_watch = rt_activity_watch_register(pty_activity_probe, pty, pty->activity_wake);
    return pty->activity_watch != NULL;
}

static void pty_activity_monitor_stop(rt_pty_impl *pty) {
    if (!pty)
        return;
    rt_activity_watch_unregister(pty->activity_watch);
    pty->activity_watch = NULL;
    rt_activity_wake_release(pty->activity_wake);
    pty->activity_wake = NULL;
}

static void pty_activity_rearm(rt_pty_impl *pty) {
    if (pty)
        rt_activity_watch_rearm(pty->activity_watch);
}

// Thread-local so concurrent Open/OpenResult/IsSupported calls on different
// threads never tear each other's diagnostic or clobber it — the buffer was a
// plain process-global written with snprintf from every path (VDOC-215). Each
// thread reads back the error from the operation it just performed.
static RT_THREAD_LOCAL char pty_last_error[PTY_LAST_ERROR_MAX];

/// @brief Allocate an empty runtime string for PTY API fallbacks.
/// @return Newly allocated empty runtime string owned by the caller.
static rt_string empty_string(void) {
    return rt_string_from_bytes("", 0);
}

/// @brief Validate and cast an opaque runtime PTY handle.
/// @param handle Candidate runtime object.
/// @return Borrowed PTY implementation pointer when class and payload size
///         match, otherwise NULL.
static rt_pty_impl *pty_checked(void *handle) {
    if (!rt_obj_is_instance(handle, RT_PTY_CLASS_ID, sizeof(rt_pty_impl)))
        return NULL;
    return (rt_pty_impl *)handle;
}

/// @brief Store a bounded user-facing PTY diagnostic string.
/// @details Writes a thread-local buffer, so concurrent PTY calls on different
///          threads keep independent diagnostics and cannot tear or clobber each
///          other (VDOC-215). Read back the error on the same thread that
///          performed the operation.
/// @param message NUL-terminated diagnostic text; NULL clears the buffer.
static void pty_set_last_error(const char *message) {
    if (!message) {
        pty_last_error[0] = '\0';
        return;
    }
    snprintf(pty_last_error, sizeof(pty_last_error), "%s", message);
}

/// @brief Store a POSIX errno-based PTY diagnostic string.
/// @param prefix Context prefix such as "posix_openpt failed".
static void pty_set_last_errno(const char *prefix) {
    snprintf(pty_last_error,
             sizeof(pty_last_error),
             "%s: %s",
             prefix ? prefix : "PTY error",
             strerror(errno));
}

// --- Shared helpers (mirrors of the proven rt_process.c implementations) ----

/// @brief Extract a C-string-safe byte view for an OS PTY interface.
/// @details Rejects NULL, invalid lengths, and embedded NUL bytes so program,
///          cwd, argv, and environment values cannot be silently truncated.
///          Empty strings remain valid for callers to interpret.
/// @param value Runtime string to inspect.
/// @param out_text Optional destination for a borrowed NUL-terminated pointer.
/// @param out_len Optional destination for byte length excluding the terminator.
/// @return 1 for a valid OS-safe view, otherwise 0.
static int pty_string_cstr_view(rt_string value, const char **out_text, size_t *out_len) {
    const char *text = value ? rt_string_cstr(value) : NULL;
    int64_t len64 = value ? rt_str_len(value) : -1;
    if (out_text)
        *out_text = NULL;
    if (out_len)
        *out_len = 0;
    if (!text || len64 < 0 || (uint64_t)len64 > SIZE_MAX)
        return 0;
    size_t len = (size_t)len64;
    if (len > 0 && memchr(text, '\0', len))
        return 0;
    if (out_text)
        *out_text = text;
    if (out_len)
        *out_len = len;
    return 1;
}

/// @brief Validate every PTY argument or environment sequence element.
/// @param items Optional runtime sequence of strings.
/// @param require_env_assignment Nonzero to require a nonempty name followed by
///        '=' in every item.
/// @param trap_msg Diagnostic raised for the first invalid element.
/// @return 1 when all elements are valid, or 0 after raising a trap.
static int pty_validate_string_sequence(void *items,
                                        int require_env_assignment,
                                        const char *trap_msg) {
    int64_t count = items ? rt_seq_len(items) : 0;
    if (count < 0)
        return 1;
    for (int64_t i = 0; i < count; i++) {
        rt_string item = rt_seq_get_str(items, i);
        const char *text = NULL;
        size_t len = 0;
        int ok = pty_string_cstr_view(item, &text, &len);
        if (ok && require_env_assignment) {
            const char *equals = (const char *)memchr(text, '=', len);
            ok = equals && equals != text;
        }
        rt_str_release_maybe(item);
        if (!ok) {
            rt_trap(trap_msg);
            return 0;
        }
    }
    return 1;
}

/// @brief Record that terminal output exceeded the retained-byte limit.
/// @param buf Buffer to mark; NULL is ignored.
static void buffer_mark_truncated(pty_buffer *buf) {
    if (buf)
        buf->truncated = 1;
}

/// @brief Append terminal bytes to a bounded output buffer.
/// @details Retains at most PTY_BUFFER_MAX_SIZE bytes, marking rather than
///          failing when excess bytes are discarded. Storage grows
///          geometrically and existing unread bytes survive allocation failure.
/// @param buf Destination buffer.
/// @param data Borrowed byte span.
/// @param len Number of bytes available at @p data.
/// @return 1 on success, recorded truncation, or no-op input; 0 only when
///         storage growth fails.
static int buffer_append(pty_buffer *buf, const char *data, size_t len) {
    if (!buf || !data || len == 0)
        return 1;
    if (buf->len >= PTY_BUFFER_MAX_SIZE) {
        buffer_mark_truncated(buf);
        return 1;
    }
    if (len > PTY_BUFFER_MAX_SIZE - buf->len) {
        len = PTY_BUFFER_MAX_SIZE - buf->len;
        buffer_mark_truncated(buf);
    }
    if (len == 0)
        return 1;

    size_t needed = buf->len + len;
    if (needed > buf->cap) {
        size_t new_cap = buf->cap ? buf->cap : PTY_BUFFER_INITIAL_SIZE;
        while (new_cap < needed && new_cap < PTY_BUFFER_MAX_SIZE) {
            if (new_cap > PTY_BUFFER_MAX_SIZE / 2) {
                new_cap = PTY_BUFFER_MAX_SIZE;
                break;
            }
            new_cap *= 2;
        }
        if (new_cap < needed)
            new_cap = needed;
        char *new_data = (char *)realloc(buf->data, new_cap);
        if (!new_data)
            return 0;
        buf->data = new_data;
        buf->cap = new_cap;
    }

    memcpy(buf->data + buf->len, data, len);
    buf->len += len;
    return 1;
}

/// @brief Consume retained terminal bytes into a runtime string.
/// @details Clears the unread length and truncation marker but retains allocated
///          storage for subsequent output.
/// @param buf Buffer to consume; NULL represents empty output.
/// @param was_truncated Optional destination for the prior truncation state.
/// @return Newly allocated string containing all retained unread bytes.
static rt_string buffer_take_string(pty_buffer *buf, int *was_truncated) {
    int truncated = buf ? buf->truncated : 0;
    if (was_truncated)
        *was_truncated = truncated;
    if (!buf) {
        return empty_string();
    }
    buf->truncated = 0;
    if (buf->len == 0)
        return empty_string();
    rt_string out = rt_string_from_bytes(buf->data, buf->len);
    buf->len = 0;
    return out;
}

/// @brief Consume terminal output and trap when bytes were discarded.
/// @param buf Buffer to consume; may be NULL.
/// @return Newly allocated string containing the retained output prefix.
static rt_string buffer_take(pty_buffer *buf) {
    int truncated = 0;
    rt_string out = buffer_take_string(buf, &truncated);
    if (truncated) {
        rt_trap("Pty: output truncated");
    }
    return out;
}

/// @brief Store a runtime string under a constant text key in a runtime map.
/// @details Uses an empty constant string for NULL values. The map acquires its
///          own ownership; the caller's string reference is not consumed.
/// @param map Destination runtime map; NULL is ignored.
/// @param key Borrowed non-NULL C-string key.
/// @param value Borrowed runtime string; may be NULL.
static void map_set_string_owned(void *map, const char *key, rt_string value) {
    if (!map || !key)
        return;
    rt_map_set_str(map, rt_const_cstr(key), value ? value : rt_const_cstr(""));
}

/// @brief Consume terminal output into a structured nontrapping read result.
/// @param buf Buffer to consume; NULL represents empty, nontruncated output.
/// @return Caller-owned map containing string @c text and Boolean @c truncated,
///         or NULL when map allocation fails.
static void *buffer_take_result(pty_buffer *buf) {
    int truncated = 0;
    rt_string text = buffer_take_string(buf, &truncated);
    void *result = rt_map_new();
    if (!result) {
        rt_str_release_maybe(text);
        return NULL;
    }
    map_set_string_owned(result, "text", text);
    rt_map_set_bool(result, rt_const_cstr("truncated"), truncated ? 1 : 0);
    rt_str_release_maybe(text);
    return result;
}

/// @brief Release all storage and reset a PTY output buffer.
/// @param buf Buffer to clear; NULL is ignored.
static void buffer_free(pty_buffer *buf) {
    if (!buf)
        return;
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
    buf->truncated = 0;
}

/// @brief Build a temporary C-string vector from a runtime sequence.
/// @details Optionally prepends @p first, retains each runtime string whose byte
///          view enters the vector, and appends a NULL sentinel.
/// @param first Borrowed leading string used when @p include_first is nonzero.
/// @param items Optional runtime sequence of strings.
/// @param include_first Nonzero to prepend @p first.
/// @return Owned vector aggregate; a zeroed aggregate indicates size or
///         allocation failure and successful results must be released with
///         free_string_vector().
static pty_string_vector build_string_vector(const char *first, void *items, int include_first) {
    pty_string_vector vector;
    memset(&vector, 0, sizeof(vector));

    int64_t item_count = items ? rt_seq_len(items) : 0;
    int64_t total = item_count + (include_first ? 1 : 0) + 1;
    if (total <= 0 || total > INT_MAX)
        return vector;

    vector.values = (char **)calloc((size_t)total, sizeof(char *));
    if (!vector.values)
        return vector;

    if (item_count > 0) {
        vector.owned_strings = (rt_string *)calloc((size_t)item_count, sizeof(rt_string));
        if (!vector.owned_strings) {
            free(vector.values);
            vector.values = NULL;
            return vector;
        }
        vector.owned_count = item_count;
    }

    int64_t at = 0;
    if (include_first)
        vector.values[at++] = (char *)(uintptr_t)first;
    for (int64_t i = 0; i < item_count; i++) {
        rt_string item = rt_seq_get_str(items, i);
        vector.owned_strings[i] = item;
        vector.values[at++] = (char *)(uintptr_t)(item ? rt_string_cstr(item) : "");
    }
    vector.values[at] = NULL;
    return vector;
}

/// @brief Release a temporary vector and all retained runtime strings.
/// @param vector Aggregate returned by build_string_vector(); NULL is ignored
///        and the aggregate is zeroed after release.
static void free_string_vector(pty_string_vector *vector) {
    if (!vector)
        return;
    if (vector->owned_strings) {
        for (int64_t i = 0; i < vector->owned_count; i++)
            rt_str_release_maybe(vector->owned_strings[i]);
        free(vector->owned_strings);
    }
    free(vector->values);
    memset(vector, 0, sizeof(*vector));
}

/// @brief Allocate and initialize a GC-managed PTY session object.
/// @details Installs pty_finalize(), records an unknown exit code of -1, and
///          initializes every platform resource to its invalid sentinel.
/// @return New PTY implementation object, or NULL after recording and trapping
///         an allocation failure.
static rt_pty_impl *pty_alloc(void) {
    rt_pty_impl *pty = (rt_pty_impl *)rt_obj_new_i64(RT_PTY_CLASS_ID, (int64_t)sizeof(rt_pty_impl));
    if (!pty) {
        pty_set_last_error("PTY allocation failed");
        rt_trap("Pty.Open: allocation failed");
        return NULL;
    }
    memset(pty, 0, sizeof(*pty));
    pty->exit_code = -1;
#if defined(_WIN32)
    pty->process = NULL;
    pty->thread = NULL;
    pty->input_write = NULL;
    pty->input_writer = NULL;
    pty->output_read = NULL;
    pty->hpc = NULL;
#else
    pty->pid = -1;
    pty->master_fd = -1;
#endif
    rt_obj_set_finalizer(pty, pty_finalize);
    return pty;
}

/// @brief Normalize a requested terminal window size in place.
/// @details Nonpositive columns and rows become 80 and 24 respectively; values
///          above 4096 are capped so they fit both supported backend types.
/// @param cols Mutable column count.
/// @param rows Mutable row count.
static void pty_clamp_size(int64_t *cols, int64_t *rows) {
    if (*cols <= 0)
        *cols = 80;
    if (*rows <= 0)
        *rows = 24;
    if (*cols > 4096)
        *cols = 4096;
    if (*rows > 4096)
        *rows = 4096;
}

// ===========================================================================
#if defined(_WIN32)
// --- Windows ConPTY backend (validate on a Windows host; not built on POSIX) -

static INIT_ONCE pty_conpty_once = INIT_ONCE_STATIC_INIT;
static int pty_conpty_available = 0;
static pty_create_pseudoconsole_fn pty_create_pc = NULL;
static pty_resize_pseudoconsole_fn pty_resize_pc = NULL;
static pty_close_pseudoconsole_fn pty_close_pc = NULL;
typedef int(WINAPI *pty_compare_string_ordinal_fn)(LPCWCH, int, LPCWCH, int, BOOL);
static pty_compare_string_ordinal_fn pty_compare_string_ordinal = NULL;

/// @brief Store a Windows GetLastError-based PTY diagnostic string.
/// @param prefix Context prefix such as "CreateProcessW failed".
static void pty_set_last_win32_error(const char *prefix) {
    snprintf(pty_last_error,
             sizeof(pty_last_error),
             "%s: Windows error %lu",
             prefix ? prefix : "PTY error",
             (unsigned long)GetLastError());
}

/// @brief Store a ConPTY HRESULT without relying on unrelated Win32 last-error state.
/// @param prefix Context prefix, or NULL for a generic PTY label.
/// @param hr HRESULT value returned by a ConPTY operation.
static void pty_set_hresult_error(const char *prefix, HRESULT hr) {
    snprintf(pty_last_error,
             sizeof(pty_last_error),
             "%s: HRESULT 0x%08lX",
             prefix ? prefix : "PTY error",
             (unsigned long)hr);
}

/// @brief Resolve the optional ConPTY entry points exactly once per process.
/// @param once Win32 one-time initialization token; unused.
/// @param param Optional caller parameter; unused.
/// @param context Optional callback context destination; unused.
/// @return TRUE so InitOnce records the resolution attempt as complete.
static BOOL CALLBACK pty_load_conpty_once(PINIT_ONCE once, PVOID param, PVOID *context) {
    (void)once;
    (void)param;
    (void)context;
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (!k32)
        return TRUE;
    pty_create_pc = (pty_create_pseudoconsole_fn)(void *)GetProcAddress(k32, "CreatePseudoConsole");
    pty_resize_pc = (pty_resize_pseudoconsole_fn)(void *)GetProcAddress(k32, "ResizePseudoConsole");
    pty_close_pc = (pty_close_pseudoconsole_fn)(void *)GetProcAddress(k32, "ClosePseudoConsole");
    pty_compare_string_ordinal =
        (pty_compare_string_ordinal_fn)(void *)GetProcAddress(k32, "CompareStringOrdinal");
    pty_conpty_available = (pty_create_pc && pty_resize_pc && pty_close_pc) ? 1 : 0;
    return TRUE;
}

/// @brief Ensure the process-wide ConPTY capability probe has run.
static void pty_load_conpty(void) {
    (void)InitOnceExecuteOnce(&pty_conpty_once, pty_load_conpty_once, NULL, NULL);
}

/// @brief Close and clear an optional valid Win32 handle slot.
/// @param h Address of an owned handle; NULL, empty, and INVALID_HANDLE_VALUE
///        slots are ignored.
static void close_handle(HANDLE *h) {
    if (h && *h && *h != INVALID_HANDLE_VALUE) {
        CloseHandle(*h);
        *h = NULL;
    }
}

/// @brief Probe ConPTY output readiness and process exit without blocking.
static int pty_activity_probe(void *context) {
    rt_pty_impl *pty = (rt_pty_impl *)context;
    if (!pty || pty->destroyed)
        return 0;
    if (pty->process && WaitForSingleObject(pty->process, 0) == WAIT_OBJECT_0)
        return 1;
    if (!pty->output_read || rt_atomic_load_i32(&pty->output_activity_closed, __ATOMIC_ACQUIRE))
        return 0;
    DWORD available = 0;
    if (!PeekNamedPipe(pty->output_read, NULL, 0, NULL, &available, NULL)) {
        return !rt_atomic_exchange_i32(&pty->output_activity_closed, 1, __ATOMIC_ACQ_REL);
    }
    return available > 0;
}

/// @brief Convert a NUL-terminated UTF-8 string to strict UTF-16.
/// @param text Borrowed UTF-8 text; NULL is treated as empty.
/// @return Caller-owned NUL-terminated wide string, or NULL on invalid UTF-8 or
///         allocation/conversion failure.
static wchar_t *pty_widen(const char *text) {
    if (!text)
        text = "";
    int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (wlen <= 0)
        return NULL;
    wchar_t *wide = (wchar_t *)calloc((size_t)wlen, sizeof(wchar_t));
    if (!wide)
        return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, wlen) <= 0) {
        free(wide);
        return NULL;
    }
    return wide;
}

#include "rt_pty_windows_environment.inc"

/// @brief Append one argument to a growing Windows command line.
/// @details Adds a separating space when needed and applies conventional
///          backslash/quote escaping, quoting empty values and values containing
///          spaces, tabs, or quotes. Grows @p buf as necessary.
/// @param buf Address of the caller-owned command-line allocation.
/// @param len Address of the current byte length excluding the terminator.
/// @param cap Address of the allocated byte capacity.
/// @param arg Borrowed NUL-terminated UTF-8 argument.
/// @return 1 after appending and NUL-terminating, or 0 on allocation failure.
static int pty_cmdline_append(char **buf, size_t *len, size_t *cap, const char *arg) {
    int need_quote = (arg[0] == '\0');
    for (const char *p = arg; *p; p++) {
        if (*p == ' ' || *p == '\t' || *p == '"')
            need_quote = 1;
    }
    char stackbuf[1024];
    size_t n = 0;
    char *tmp = stackbuf;
    size_t tmpcap = sizeof(stackbuf);
    size_t arglen = strlen(arg);
    if (arglen * 2 + 4 > tmpcap) {
        tmp = (char *)malloc(arglen * 2 + 4);
        if (!tmp)
            return 0;
    }
    if (*len > 0)
        tmp[n++] = ' ';
    if (need_quote)
        tmp[n++] = '"';
    size_t backslashes = 0;
    for (const char *p = arg; *p; p++) {
        if (*p == '\\') {
            backslashes++;
            tmp[n++] = '\\';
        } else if (*p == '"') {
            for (size_t b = 0; b < backslashes + 1; b++)
                tmp[n++] = '\\';
            tmp[n++] = '"';
            backslashes = 0;
        } else {
            backslashes = 0;
            tmp[n++] = *p;
        }
    }
    if (need_quote) {
        for (size_t b = 0; b < backslashes; b++)
            tmp[n++] = '\\';
        tmp[n++] = '"';
    }
    if (*len + n + 1 > *cap) {
        size_t new_cap = (*cap ? *cap : 256);
        while (new_cap < *len + n + 1)
            new_cap *= 2;
        char *grown = (char *)realloc(*buf, new_cap);
        if (!grown) {
            if (tmp != stackbuf)
                free(tmp);
            return 0;
        }
        *buf = grown;
        *cap = new_cap;
    }
    memcpy(*buf + *len, tmp, n);
    *len += n;
    (*buf)[*len] = '\0';
    if (tmp != stackbuf)
        free(tmp);
    return 1;
}

/// @brief Open a Windows ConPTY-backed child session.
/// @details Validates all OS-bound strings, builds a quoted command line,
///          optionally constructs a complete UTF-16 replacement environment,
///          creates a pseudoconsole and startup attribute, and launches the
///          child with merged terminal output. NULL or empty cwd inherits.
/// @param program Runtime executable command; must be nonempty and contain no
///        embedded NUL.
/// @param args Optional Seq of literal argument strings.
/// @param cwd Optional working directory; NULL or empty means inherit.
/// @param env Optional Seq of NAME=VALUE strings replacing the environment;
///        NULL means inherit.
/// @param cols Normalized initial terminal columns.
/// @param rows Normalized initial terminal rows.
/// @return New GC-managed running PTY object, or NULL after recording any
///         validation, availability, conversion, setup, or launch failure.
static rt_pty_impl *pty_open_impl(rt_string program,
                                  void *args,
                                  rt_string cwd,
                                  void *env,
                                  int64_t cols,
                                  int64_t rows,
                                  int overlay_environment) {
    const char *program_text = NULL;
    const char *cwd_text = NULL;
    size_t program_len = 0, cwd_len = 0;
    if (!program) {
        pty_set_last_error("PTY program is empty");
        return NULL;
    }
    if (!pty_string_cstr_view(program, &program_text, &program_len) || program_len == 0) {
        pty_set_last_error("PTY program is invalid");
        rt_trap("Pty.Open: invalid program");
        return NULL;
    }
    if (cwd) {
        if (!pty_string_cstr_view(cwd, &cwd_text, &cwd_len)) {
            pty_set_last_error("PTY working directory is invalid");
            rt_trap("Pty.Open: invalid working directory");
            return NULL;
        }
        if (cwd_len == 0)
            cwd_text = NULL;
    }
    if (!pty_validate_string_sequence(args, 0, "Pty.Open: invalid argument"))
        return NULL;
    if (!pty_validate_string_sequence(env, 1, "Pty.Open: invalid environment entry"))
        return NULL;

    pty_load_conpty();
    if (!pty_conpty_available) {
        pty_set_last_error("Windows ConPTY is unavailable; Windows 10 1809 or newer is required");
        return NULL;
    }

    // Build the command line: program followed by args.
    char *cmdline = NULL;
    size_t cmd_len = 0, cmd_cap = 0;
    if (!pty_cmdline_append(&cmdline, &cmd_len, &cmd_cap, program_text)) {
        pty_set_last_error("PTY command line allocation failed");
        free(cmdline);
        return NULL;
    }
    int64_t arg_count = args ? rt_seq_len(args) : 0;
    for (int64_t i = 0; i < arg_count; i++) {
        rt_string a = rt_seq_get_str(args, i);
        const char *atext = a ? rt_string_cstr(a) : "";
        int ok = pty_cmdline_append(&cmdline, &cmd_len, &cmd_cap, atext);
        rt_str_release_maybe(a);
        if (!ok) {
            pty_set_last_error("PTY command line allocation failed");
            free(cmdline);
            return NULL;
        }
    }

    HANDLE in_read = NULL, in_write = NULL, out_read = NULL, out_write = NULL;
    if (!CreatePipe(&in_read, &in_write, NULL, 0) || !CreatePipe(&out_read, &out_write, NULL, 0)) {
        pty_set_last_win32_error("CreatePipe failed");
        close_handle(&in_read);
        close_handle(&in_write);
        close_handle(&out_read);
        close_handle(&out_write);
        free(cmdline);
        return NULL;
    }

    COORD size;
    size.X = (SHORT)cols;
    size.Y = (SHORT)rows;
    void *hpc = NULL;
    HRESULT hr = pty_create_pc(size, in_read, out_write, 0, &hpc);
    // ConPTY duplicates the handles it needs; close our copies of the child ends.
    close_handle(&in_read);
    close_handle(&out_write);
    if (FAILED(hr) || !hpc) {
        pty_set_hresult_error("CreatePseudoConsole failed", FAILED(hr) ? hr : E_POINTER);
        if (hpc)
            pty_close_pc(hpc);
        close_handle(&in_write);
        close_handle(&out_read);
        free(cmdline);
        return NULL;
    }

    STARTUPINFOEXW si;
    memset(&si, 0, sizeof(si));
    si.StartupInfo.cb = sizeof(STARTUPINFOEXW);
    SIZE_T attr_size = 0;
    (void)InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
    if (attr_size > 0)
        si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attr_size);
    if (!si.lpAttributeList ||
        !InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attr_size)) {
        pty_set_last_win32_error("ConPTY process attribute setup failed");
        free(si.lpAttributeList);
        pty_close_pc(hpc);
        close_handle(&in_write);
        close_handle(&out_read);
        free(cmdline);
        return NULL;
    }
    if (!UpdateProcThreadAttribute(si.lpAttributeList,
                                   0,
                                   PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                   hpc,
                                   sizeof(hpc),
                                   NULL,
                                   NULL)) {
        pty_set_last_win32_error("ConPTY process attribute setup failed");
        DeleteProcThreadAttributeList(si.lpAttributeList);
        free(si.lpAttributeList);
        pty_close_pc(hpc);
        close_handle(&in_write);
        close_handle(&out_read);
        free(cmdline);
        return NULL;
    }

    wchar_t *wcmd = pty_widen(cmdline);
    wchar_t *wcwd = cwd_text ? pty_widen(cwd_text) : NULL;
    free(cmdline);
    cmdline = NULL;
    wchar_t *env_block =
        overlay_environment ? pty_build_env_overlay_block_wide(env) : pty_build_env_block_wide(env);
    if (!wcmd || (cwd_text && !wcwd) || (env && !env_block)) {
        pty_set_last_error("ConPTY UTF-8 conversion or environment allocation failed");
        DeleteProcThreadAttributeList(si.lpAttributeList);
        free(si.lpAttributeList);
        free(wcmd);
        free(wcwd);
        free(env_block);
        pty_close_pc(hpc);
        close_handle(&in_write);
        close_handle(&out_read);
        return NULL;
    }

    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));
    BOOL ok = CreateProcessW(NULL,
                             wcmd,
                             NULL,
                             NULL,
                             FALSE,
                             EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                             env_block,
                             wcwd,
                             &si.StartupInfo,
                             &pi);
    DeleteProcThreadAttributeList(si.lpAttributeList);
    free(si.lpAttributeList);
    free(wcmd);
    free(wcwd);
    free(env_block);

    if (!ok) {
        pty_set_last_win32_error("CreateProcessW failed");
        pty_close_pc(hpc);
        close_handle(&in_write);
        close_handle(&out_read);
        return NULL;
    }

    rt_pty_impl *pty = pty_alloc();
    if (!pty) {
        DWORD ignored_exit_code = STILL_ACTIVE;
        (void)rt_win32_terminate_process_bounded(
            pi.hProcess, 1, PTY_WINDOWS_TERMINATE_WAIT_MS, &ignored_exit_code);
        close_handle(&pi.hThread);
        close_handle(&pi.hProcess);
        pty_close_pc(hpc);
        close_handle(&in_write);
        close_handle(&out_read);
        return NULL;
    }
    pty->started = 1;
    pty->running = 1;
    pty->process = pi.hProcess;
    pty->thread = pi.hThread;
    pty->input_write = in_write;
    pty->input_writer =
        rt_async_pipe_writer_create(pty->input_write, PTY_WINDOWS_INPUT_QUEUE_MAX_SIZE);
    pty->output_read = out_read;
    pty->hpc = hpc;
    if (!pty->input_writer) {
        pty_close(pty);
        if (rt_obj_release_check0(pty))
            rt_obj_free(pty);
        return NULL;
    }
    return pty;
}

/// @brief Nonblockingly drain immediately available ConPTY output.
/// @details Reads bounded chunks after PeekNamedPipe and appends them to the
///          merged output buffer. Allocation failure raises a runtime trap.
/// @param pty Session whose output pipe and buffer are updated.
static void pty_drain(rt_pty_impl *pty) {
    if (!pty || !pty->output_read)
        return;
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(pty->output_read, NULL, 0, NULL, &avail, NULL)) {
            rt_atomic_store_i32(&pty->output_activity_closed, 1, __ATOMIC_RELEASE);
            break;
        }
        if (avail == 0)
            break;
        char chunk[4096];
        DWORD want = avail > sizeof(chunk) ? (DWORD)sizeof(chunk) : avail;
        DWORD got = 0;
        if (!ReadFile(pty->output_read, chunk, want, &got, NULL) || got == 0) {
            rt_atomic_store_i32(&pty->output_activity_closed, 1, __ATOMIC_RELEASE);
            break;
        }
        if (!buffer_append(&pty->output_buf, chunk, (size_t)got)) {
            if (!pty->finalizing)
                rt_trap("Pty: output buffer allocation failed");
            break;
        }
    }
    pty_activity_rearm(pty);
}

/// @brief Poll or wait for Windows PTY child completion.
/// @details Drains before waiting, records the process exit code on completion,
///          and performs a final drain. Wait failure records -1 and updates the
///          thread-local diagnostic.
/// @param pty Session to update.
/// @param wait Nonzero to drain and poll until completion, zero for one nonblocking poll.
static void pty_poll_internal(rt_pty_impl *pty, int wait) {
    if (!pty || pty->destroyed)
        return;
    pty_drain(pty);
    if (!pty->running || !pty->process)
        return;
    DWORD r = WAIT_TIMEOUT;
    if (wait) {
        do {
            pty_drain(pty);
            r = WaitForSingleObject(pty->process, 10);
        } while (r == WAIT_TIMEOUT);
    } else {
        r = WaitForSingleObject(pty->process, 0);
    }
    if (r == WAIT_OBJECT_0) {
        DWORD code = 0;
        if (GetExitCodeProcess(pty->process, &code)) {
            pty->exit_code = (int64_t)code;
        } else {
            pty_set_last_win32_error("GetExitCodeProcess(ConPTY process) failed");
            pty->exit_code = -1;
        }
        pty->running = 0;
        pty_drain(pty);
    } else if (r == WAIT_FAILED) {
        pty_set_last_win32_error("WaitForSingleObject(ConPTY process) failed");
        pty->exit_code = -1;
    }
}

/// @brief Apply a normalized window size to a Windows pseudoconsole.
/// @param pty Session owning a live ConPTY handle.
/// @param cols Normalized terminal columns.
/// @param rows Normalized terminal rows.
/// @return 1 on successful ResizePseudoConsole, otherwise 0 after recording an
///         HRESULT diagnostic when applicable.
static int pty_resize_impl(rt_pty_impl *pty, int64_t cols, int64_t rows) {
    if (!pty || !pty->hpc || !pty_resize_pc)
        return 0;
    COORD size;
    size.X = (SHORT)cols;
    size.Y = (SHORT)rows;
    HRESULT hr = pty_resize_pc(pty->hpc, size);
    if (FAILED(hr)) {
        pty_set_hresult_error("ResizePseudoConsole failed", hr);
        return 0;
    }
    return 1;
}

/// @brief Write terminal input through the Windows ConPTY pipe.
/// @param pty Session owning the parent input handle.
/// @param bytes Borrowed byte span.
/// @param len Number of bytes to write.
/// @return Complete byte count, a positive partial count after later failure,
///         or -1 when no byte can be written.
static int64_t pty_write_impl(rt_pty_impl *pty, const char *bytes, size_t len) {
    return rt_async_pipe_writer_enqueue(pty ? pty->input_writer : NULL, bytes, len);
}

/// @brief Terminate and release a Windows PTY session.
/// @details Idempotently terminates a running child with status 1, waits when
///          termination succeeds, drains remaining output, closes the
///          pseudoconsole and all handles, frees buffering, and invalidates the
///          object.
/// @param pty Session to close; NULL or already destroyed is ignored.
static void pty_close(rt_pty_impl *pty) {
    if (!pty || pty->destroyed)
        return;
    pty_activity_monitor_stop(pty);
    pty_poll_internal(pty, 0);
    if (pty->running && pty->process) {
        DWORD exit_code = STILL_ACTIVE;
        if (rt_win32_terminate_process_bounded(
                pty->process, 1, PTY_WINDOWS_TERMINATE_WAIT_MS, &exit_code)) {
            pty->running = 0;
            pty->exit_code = (int64_t)exit_code;
        } else {
            pty_set_last_win32_error("Bounded ConPTY child termination failed");
        }
    }
    pty_drain(pty);
    if (!rt_async_pipe_writer_destroy(pty->input_writer, PTY_WINDOWS_TERMINATE_WAIT_MS) &&
        !pty->finalizing)
        pty_set_last_error("Bounded ConPTY input writer shutdown failed");
    pty->input_writer = NULL;
    if (pty->hpc && pty_close_pc) {
        pty_close_pc(pty->hpc);
        pty->hpc = NULL;
    }
    close_handle(&pty->input_write);
    close_handle(&pty->output_read);
    close_handle(&pty->thread);
    close_handle(&pty->process);
    buffer_free(&pty->output_buf);
    pty->running = 0;
    pty->started = 0;
    pty->destroyed = 1;
}

/// @brief Report dynamically detected Windows ConPTY availability.
/// @return 1 when all required ConPTY functions resolved, otherwise 0.
static int64_t pty_supported(void) {
    pty_load_conpty();
    return pty_conpty_available ? 1 : 0;
}

// ===========================================================================
#else
// --- POSIX (macOS / Linux): posix_openpt + fork + setsid + execvp ----------

/// @brief Write to a POSIX PTY without changing process-wide SIGPIPE handling.
/// @details Temporarily blocks SIGPIPE for the calling thread, retries an
///          interrupted write, consumes only a newly generated EPIPE signal,
///          and restores the prior mask.
/// @param fd PTY master descriptor.
/// @param bytes Borrowed bytes to write.
/// @param len Maximum bytes for this write call.
/// @return Result from write(2).
static ssize_t pty_write_no_sigpipe(int fd, const char *bytes, size_t len) {
    sigset_t set, old_set, pending;
    int blocked = 0, had_pending = 0;
    ssize_t n = -1;
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
    if (sigprocmask(SIG_BLOCK, &set, &old_set) == 0) {
        blocked = 1;
        if (sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1)
            had_pending = 1;
    }
    do {
        n = write(fd, bytes, len);
    } while (n < 0 && errno == EINTR);
    if (blocked && n < 0 && errno == EPIPE && !had_pending) {
        int signo = 0;
        if (sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1)
            (void)sigwait(&set, &signo);
    }
    if (blocked)
        (void)sigprocmask(SIG_SETMASK, &old_set, NULL);
    return n;
}

/// @brief Close and invalidate an optional POSIX descriptor slot.
/// @param fd Address of an owned descriptor; NULL or a negative slot is ignored.
static void close_fd(int *fd) {
    if (fd && *fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

/// @brief Probe one POSIX PTY master without blocking the shared watcher.
static int pty_activity_probe(void *context) {
    rt_pty_impl *pty = (rt_pty_impl *)context;
    if (!pty || pty->destroyed || pty->master_fd < 0 ||
        rt_atomic_load_i32(&pty->output_activity_closed, __ATOMIC_ACQUIRE))
        return 0;
    struct pollfd descriptor = {.fd = pty->master_fd, .events = POLLIN, .revents = 0};
    int result;
    do {
        result = poll(&descriptor, 1, 0);
    } while (result < 0 && errno == EINTR);
    return result > 0 && (descriptor.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL));
}

/// @brief Best-effort report a pre-exec child error to the parent.
/// @details Writes the native integer representation using only
///          async-signal-safe operations and retries interruptions.
/// @param fd Child write end of the close-on-exec status pipe.
/// @param error_value errno value to transmit.
static void pty_child_report_errno(int fd, int error_value) {
    const unsigned char *bytes = (const unsigned char *)&error_value;
    size_t off = 0;
    while (off < sizeof(error_value)) {
        ssize_t count = write(fd, bytes + off, sizeof(error_value) - off);
        if (count > 0) {
            off += (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        break;
    }
}

/// @brief Signal an entire PTY child session with single-process fallback.
/// @details Targets the process group whose id equals @p pid. If that group no
///          longer exists, retries the child pid and treats an already absent
///          child as success.
/// @param pid Session leader and child process id.
/// @param signal_number POSIX signal to deliver.
/// @return 1 when delivered, unnecessary for a nonpositive/already absent pid,
///         or otherwise accepted; 0 on a substantive kill error.
static int pty_signal_session(pid_t pid, int signal_number) {
    if (pid <= 0)
        return 1;
    if (kill(-pid, signal_number) == 0)
        return 1;
    if (errno != ESRCH)
        return 0;
    if (kill(pid, signal_number) == 0 || errno == ESRCH)
        return 1;
    return 0;
}

/// @brief Enable O_NONBLOCK while preserving existing descriptor status flags.
/// @param fd Open PTY master descriptor.
/// @return 1 when already or successfully nonblocking, otherwise 0.
static int pty_set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return 0;
    if ((flags & O_NONBLOCK) != 0)
        return 1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

/// @brief Enable FD_CLOEXEC on a PTY master descriptor.
/// @param fd Open descriptor.
/// @return 1 when already or successfully close-on-exec; 0 on fcntl failure or
///         platforms without FD_CLOEXEC support.
static int pty_set_close_on_exec(int fd) {
#if defined(FD_CLOEXEC)
    int flags = fcntl(fd, F_GETFD, 0);
    if (flags < 0)
        return 0;
    if ((flags & FD_CLOEXEC) != 0)
        return 1;
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
#else
    (void)fd;
    errno = ENOTSUP;
    return 0;
#endif
}

/// @brief Reap a specific POSIX child, retrying interrupted waits.
/// @param pid Child process id; nonpositive values are ignored.
static void pty_reap_child_blocking(pid_t pid) {
    if (pid <= 0)
        return;
    pid_t result;
    do {
        result = waitpid(pid, NULL, 0);
    } while (result < 0 && errno == EINTR);
}

/// @brief Drain all immediately available merged PTY output.
/// @details Retries interrupted reads, preserves the master on EAGAIN, and
///          closes it on EOF, EIO, other errors, or buffer-allocation failure.
///          Allocation failure also raises a runtime trap.
/// @param pty Session whose nonblocking master and output buffer are updated.
static void pty_drain(rt_pty_impl *pty) {
    if (!pty || pty->master_fd < 0)
        return;
    for (;;) {
        char chunk[4096];
        ssize_t count = read(pty->master_fd, chunk, sizeof(chunk));
        if (count > 0) {
            if (!buffer_append(&pty->output_buf, chunk, (size_t)count)) {
                if (!pty->finalizing)
                    rt_trap("Pty: output buffer allocation failed");
                if (!pty->activity_watch)
                    close_fd(&pty->master_fd);
                break;
            }
            continue;
        }
        if (count == 0) {
            // EOF on the master means the child closed the slave (exited).
            rt_atomic_store_i32(&pty->output_activity_closed, 1, __ATOMIC_RELEASE);
            if (!pty->activity_watch)
                close_fd(&pty->master_fd);
            break;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;
        // A closed slave surfaces as EIO on the master once the child is gone.
        rt_atomic_store_i32(&pty->output_activity_closed, 1, __ATOMIC_RELEASE);
        if (!pty->activity_watch)
            close_fd(&pty->master_fd);
        break;
    }
    pty_activity_rearm(pty);
}

/// @brief Normalize a POSIX wait status for the PTY API.
/// @param status Raw status returned by waitpid.
/// @return Normal exit code, negated terminating signal number, or -1 for any
///         other state.
static int64_t decode_exit_status(int status) {
    if (WIFEXITED(status))
        return (int64_t)WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return -(int64_t)WTERMSIG(status);
    return -1;
}

/// @brief Poll or wait for a POSIX PTY child while draining output.
/// @details Blocking mode alternates WNOHANG waitpid calls with drains and
///          10 ms delays. The child is reaped once and its normalized status is
///          retained; ECHILD stores -1, while other wait failures update the
///          thread-local diagnostic without marking completion.
/// @param pty Session to update.
/// @param wait Nonzero to continue until a terminal result; zero to poll once.
static void pty_poll_internal(rt_pty_impl *pty, int wait) {
    if (!pty || pty->destroyed)
        return;
    pty_drain(pty);
    if (!pty->running || pty->pid <= 0)
        return;
    int status = 0;
    pid_t result;
    for (;;) {
        do {
            result = waitpid(pty->pid, &status, WNOHANG);
        } while (result < 0 && errno == EINTR);
        if (!wait || result != 0)
            break;
        pty_drain(pty);
        struct timespec delay = {0, 10000000L};
        (void)nanosleep(&delay, NULL);
    }
    if (result == pty->pid) {
        pty->exit_code = decode_exit_status(status);
        pty->running = 0;
        pty_drain(pty);
    } else if (result < 0 && errno == ECHILD) {
        pty->exit_code = -1;
        pty->running = 0;
        pty_drain(pty);
    } else if (result < 0) {
        pty_set_last_errno("waitpid failed");
    }
}

/// @brief Give a SIGTERM-targeted PTY session a bounded cleanup interval.
/// @details Polls and drains every 10 ms until exit or the grace period expires.
/// @param pty Session to observe.
/// @param grace_ms Maximum wait interval in milliseconds.
static void pty_wait_after_sigterm(rt_pty_impl *pty, int grace_ms) {
    if (!pty || grace_ms <= 0)
        return;
    int elapsed = 0;
    while (pty->running && elapsed < grace_ms) {
        pty_poll_internal(pty, 0);
        if (!pty->running)
            break;
        struct timespec delay = {0, 10000000L};
        (void)nanosleep(&delay, NULL);
        elapsed += 10;
    }
}

/// @brief Apply a normalized POSIX PTY window size.
/// @details TIOCSWINSZ also causes the terminal driver to notify the foreground
///          process group with SIGWINCH. Failure updates the last-error text.
/// @param pty Session owning an open master descriptor.
/// @param cols Normalized terminal columns.
/// @param rows Normalized terminal rows.
/// @return 1 when ioctl succeeds, otherwise 0.
static int pty_resize_impl(rt_pty_impl *pty, int64_t cols, int64_t rows) {
    if (!pty || pty->master_fd < 0)
        return 0;
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = (unsigned short)cols;
    ws.ws_row = (unsigned short)rows;
    if (ioctl(pty->master_fd, TIOCSWINSZ, &ws) != 0) {
        pty_set_last_errno("TIOCSWINSZ failed"); // VDOC-214: report backend failure
        return 0;
    }
    return 1;
}

/// @brief Write terminal input through the nonblocking POSIX master.
/// @param pty Session owning the master descriptor.
/// @param bytes Borrowed byte span.
/// @param len Number of bytes to write.
/// @return Complete byte count, a positive partial count after later failure or
///         would-block, or -1 when no byte can be written.
static int64_t pty_write_impl(rt_pty_impl *pty, const char *bytes, size_t len) {
    if (pty->master_fd < 0)
        return -1;
    size_t off = 0;
    while (off < len) {
        size_t chunk = len - off;
        if (chunk > (size_t)SSIZE_MAX)
            chunk = (size_t)SSIZE_MAX;
        ssize_t n = pty_write_no_sigpipe(pty->master_fd, bytes + off, chunk);
        if (n <= 0)
            return off > 0 ? (int64_t)off : -1;
        off += (size_t)n;
    }
    return (int64_t)off;
}

/// @brief Return the nonempty name length of one POSIX NAME=value entry.
static size_t pty_env_name_length(const char *entry) {
    const char *equals = entry ? strchr(entry, '=') : NULL;
    return equals && equals != entry ? (size_t)(equals - entry) : 0;
}

/// @brief Compare POSIX environment names using native case-sensitive rules.
static int pty_env_names_equal(const char *left, const char *right) {
    size_t left_len = pty_env_name_length(left);
    size_t right_len = pty_env_name_length(right);
    return left_len > 0 && left_len == right_len && memcmp(left, right, left_len) == 0;
}

/// @brief Build an inherited POSIX environment with validated overrides.
static pty_string_vector pty_build_env_overlay_vector(void *env) {
    pty_string_vector result;
    memset(&result, 0, sizeof(result));
    pty_string_vector overlay = build_string_vector(NULL, env, 0);
    if (!overlay.values)
        return result;
    size_t overlay_count = 0;
    while (overlay.values[overlay_count])
        overlay_count++;
    for (size_t i = 0; i < overlay_count; i++) {
        for (size_t j = i + 1; j < overlay_count; j++) {
            if (pty_env_names_equal(overlay.values[i], overlay.values[j])) {
                free_string_vector(&overlay);
                return result;
            }
        }
    }
    size_t inherited_count = 0;
    while (environ && environ[inherited_count])
        inherited_count++;
    if (inherited_count > SIZE_MAX - overlay_count - 1 ||
        inherited_count + overlay_count + 1 > SIZE_MAX / sizeof(char *)) {
        free_string_vector(&overlay);
        return result;
    }
    result.values = (char **)calloc(inherited_count + overlay_count + 1, sizeof(char *));
    if (!result.values) {
        free_string_vector(&overlay);
        return result;
    }
    size_t at = 0;
    for (size_t i = 0; i < inherited_count; i++) {
        int shadowed = 0;
        for (size_t j = 0; j < overlay_count; j++) {
            if (pty_env_names_equal(environ[i], overlay.values[j])) {
                shadowed = 1;
                break;
            }
        }
        if (!shadowed)
            result.values[at++] = environ[i];
    }
    for (size_t i = 0; i < overlay_count; i++)
        result.values[at++] = overlay.values[i];
    result.values[at] = NULL;
    result.owned_strings = overlay.owned_strings;
    result.owned_count = overlay.owned_count;
    overlay.owned_strings = NULL;
    overlay.owned_count = 0;
    free(overlay.values);
    return result;
}

/// @brief Open a controlling-terminal-backed POSIX child session.
/// @details Validates OS-bound strings, creates and unlocks a PTY pair, forks a
///          session leader, attaches the slave as controlling terminal and all
///          three standard streams, optionally replaces cwd and environment,
///          then PATH-searches with execvp. A close-on-exec status pipe lets the
///          parent distinguish successful exec from pre-exec failure before it
///          publishes a handle.
/// @param program Runtime executable path or lookup name; must be nonempty and
///        contain no embedded NUL.
/// @param args Optional Seq of literal argument strings.
/// @param cwd Optional working directory; NULL or empty means inherit.
/// @param env Optional Seq of NAME=VALUE strings replacing the environment;
///        NULL means inherit.
/// @param cols Normalized initial terminal columns.
/// @param rows Normalized initial terminal rows.
/// @return New GC-managed running PTY object, or NULL after recording any
///         validation, allocation, PTY, fork, child-setup, or exec failure.
static rt_pty_impl *pty_open_impl(rt_string program,
                                  void *args,
                                  rt_string cwd,
                                  void *env,
                                  int64_t cols,
                                  int64_t rows,
                                  int overlay_environment) {
    const char *program_text = NULL;
    const char *cwd_text = NULL;
    size_t program_len = 0, cwd_len = 0;
    if (!program) {
        pty_set_last_error("PTY program is empty");
        return NULL;
    }
    if (!pty_string_cstr_view(program, &program_text, &program_len) || program_len == 0) {
        pty_set_last_error("PTY program is invalid");
        rt_trap("Pty.Open: invalid program");
        return NULL;
    }
    if (cwd) {
        if (!pty_string_cstr_view(cwd, &cwd_text, &cwd_len)) {
            pty_set_last_error("PTY working directory is invalid");
            rt_trap("Pty.Open: invalid working directory");
            return NULL;
        }
        if (cwd_len == 0)
            cwd_text = NULL;
    }
    if (!pty_validate_string_sequence(args, 0, "Pty.Open: invalid argument"))
        return NULL;
    if (!pty_validate_string_sequence(env, 1, "Pty.Open: invalid environment entry"))
        return NULL;

    pty_string_vector argv = build_string_vector(program_text, args, 1);
    if (!argv.values) {
        pty_set_last_error("PTY argv allocation failed");
        rt_trap("Pty.Open: argv allocation failed");
        return NULL;
    }
    pty_string_vector envp;
    memset(&envp, 0, sizeof(envp));
    if (env)
        envp = overlay_environment ? pty_build_env_overlay_vector(env)
                                   : build_string_vector(NULL, env, 0);
    if (env && !envp.values) {
        free_string_vector(&argv);
        pty_set_last_error("PTY environment allocation failed");
        rt_trap("Pty.Open: environment allocation failed");
        return NULL;
    }

    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) {
        pty_set_last_errno("posix_openpt/grantpt/unlockpt failed");
        if (master >= 0)
            close(master);
        free_string_vector(&envp);
        free_string_vector(&argv);
        return NULL;
    }

    char slave_name[256];
#if RT_PLATFORM_LINUX
    int pts_rc = ptsname_r(master, slave_name, sizeof(slave_name));
    if (pts_rc != 0) {
        errno = pts_rc;
        pty_set_last_errno("ptsname_r failed");
        close(master);
        free_string_vector(&envp);
        free_string_vector(&argv);
        return NULL;
    }
#else
    {
        const char *sn = ptsname(master);
        if (!sn) {
            pty_set_last_errno("ptsname failed");
            close(master);
            free_string_vector(&envp);
            free_string_vector(&argv);
            return NULL;
        }
        strncpy(slave_name, sn, sizeof(slave_name) - 1);
        slave_name[sizeof(slave_name) - 1] = '\0';
    }
#endif

    // Exec-status pipe: the write end is close-on-exec, so a SUCCESSFUL exec
    // closes it and the parent's read sees EOF. On any pre-exec failure the child
    // writes its errno and _exit(127)s, so the parent can surface the startup
    // failure by returning NULL instead of an apparently valid session that only
    // fails later (VDOC-213).
    int exec_pipe[2] = {-1, -1};
#if RT_PLATFORM_LINUX && defined(O_CLOEXEC)
    int pipe_rc = pipe2(exec_pipe, O_CLOEXEC);
#else
    int pipe_rc = pipe(exec_pipe);
#endif
    if (pipe_rc != 0) {
        pty_set_last_errno("pipe failed");
        close(master);
        free_string_vector(&envp);
        free_string_vector(&argv);
        return NULL;
    }
#if defined(FD_CLOEXEC) && !(RT_PLATFORM_LINUX && defined(O_CLOEXEC))
    {
        int f = fcntl(exec_pipe[1], F_GETFD, 0);
        if (f < 0 || fcntl(exec_pipe[1], F_SETFD, f | FD_CLOEXEC) != 0) {
            pty_set_last_errno("setting exec-status close-on-exec failed");
            close(exec_pipe[0]);
            close(exec_pipe[1]);
            close(master);
            free_string_vector(&envp);
            free_string_vector(&argv);
            return NULL;
        }
    }
#endif

    pid_t pid = fork();
    if (pid < 0) {
        pty_set_last_errno("fork failed");
        close(exec_pipe[0]);
        close(exec_pipe[1]);
        close(master);
        free_string_vector(&envp);
        free_string_vector(&argv);
        return NULL;
    }

    if (pid == 0) {
        // CHILD — only async-signal-safe calls until exec.
        close(exec_pipe[0]);
        if (setsid() < 0) {
            pty_child_report_errno(exec_pipe[1], errno);
            _exit(127);
        }
        int slave = open(slave_name, O_RDWR);
        if (slave < 0) {
            int e = errno;
            pty_child_report_errno(exec_pipe[1], e);
            _exit(127);
        }
#if defined(TIOCSCTTY)
        if (ioctl(slave, TIOCSCTTY, 0) != 0) {
            int e = errno;
            pty_child_report_errno(exec_pipe[1], e);
            _exit(127);
        }
#endif
        struct winsize ws;
        memset(&ws, 0, sizeof(ws));
        ws.ws_col = (unsigned short)cols;
        ws.ws_row = (unsigned short)rows;
        if (ioctl(slave, TIOCSWINSZ, &ws) != 0 || dup2(slave, STDIN_FILENO) < 0 ||
            dup2(slave, STDOUT_FILENO) < 0 || dup2(slave, STDERR_FILENO) < 0) {
            int e = errno;
            pty_child_report_errno(exec_pipe[1], e);
            _exit(127);
        }
        if (slave > STDERR_FILENO)
            close(slave);
        close(master);
        if (cwd_text && chdir(cwd_text) != 0) {
            int e = errno;
            pty_child_report_errno(exec_pipe[1], e);
            _exit(127);
        }
        // Always PATH-search bare program names. For an explicit environment,
        // adopt it as `environ` first so execvp searches ITS PATH and passes it
        // to the child — previously the explicit-env branch used execve, which
        // does no PATH search, so adding an environment turned a searchable bare
        // name into exit code 127 (VDOC-213). This matches Process.Start*.
        if (envp.values)
            environ = envp.values;
        execvp(program_text, argv.values);
        // exec failed: report errno to the parent (the CLOEXEC write end is
        // still open because exec did not run).
        {
            int e = errno;
            pty_child_report_errno(exec_pipe[1], e);
        }
        _exit(127);
    }

    // PARENT
    close(exec_pipe[1]);
    // Block until the child either reports a startup error or successfully
    // exec's (closing the CLOEXEC write end -> EOF). Retry on EINTR.
    int child_errno = 0;
    size_t status_bytes = 0;
    while (status_bytes < sizeof(child_errno)) {
        ssize_t count = read(exec_pipe[0],
                             (unsigned char *)&child_errno + status_bytes,
                             sizeof(child_errno) - status_bytes);
        if (count > 0) {
            status_bytes += (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0)
            status_bytes = sizeof(child_errno) + 1u;
        break;
    }
    close(exec_pipe[0]);
    if (status_bytes != 0) {
        // The child failed before/at exec (e.g. program not found on PATH).
        pty_reap_child_blocking(pid);
        close(master);
        free_string_vector(&envp);
        free_string_vector(&argv);
        if (status_bytes == sizeof(child_errno)) {
            errno = child_errno;
            pty_set_last_errno("child exec failed");
        } else {
            pty_set_last_error("child exec status protocol failed");
        }
        return NULL;
    }

    if (!pty_set_nonblocking(master) || !pty_set_close_on_exec(master)) {
        int setup_errno = errno;
        (void)pty_signal_session(pid, SIGTERM);
        pty_reap_child_blocking(pid);
        close(master);
        free_string_vector(&envp);
        free_string_vector(&argv);
        errno = setup_errno;
        pty_set_last_errno("PTY master descriptor setup failed");
        return NULL;
    }
    free_string_vector(&envp);
    free_string_vector(&argv);

    rt_pty_impl *pty = pty_alloc();
    if (!pty) {
        (void)pty_signal_session(pid, SIGTERM);
        pty_reap_child_blocking(pid);
        close(master);
        return NULL;
    }
    pty->started = 1;
    pty->running = 1;
    pty->pid = pid;
    pty->master_fd = master;
    return pty;
}

/// @brief Terminate and release a POSIX PTY session.
/// @details Idempotently signals the full child session with SIGTERM, grants a
///          500 ms drain/reap interval, escalates to SIGKILL when still running,
///          drains and closes the master, frees buffering, and invalidates the
///          object.
/// @param pty Session to close; NULL or already destroyed is ignored.
static void pty_close(rt_pty_impl *pty) {
    if (!pty || pty->destroyed)
        return;
    pty_activity_monitor_stop(pty);
    if (pty->running && pty->pid > 0) {
        if (!pty_signal_session(pty->pid, SIGTERM))
            pty_set_last_errno("PTY SIGTERM failed");
        pty_wait_after_sigterm(pty, 500);
        if (pty->running) {
            if (!pty_signal_session(pty->pid, SIGKILL))
                pty_set_last_errno("PTY SIGKILL failed");
            pty_poll_internal(pty, 1);
        }
    } else {
        pty_poll_internal(pty, 0);
    }
    pty_drain(pty);
    close_fd(&pty->master_fd);
    buffer_free(&pty->output_buf);
    pty->running = 0;
    pty->started = 0;
    pty->destroyed = 1;
}

/// @brief Report compile-time POSIX PTY backend availability.
/// @return Always 1 for this POSIX implementation.
static int64_t pty_supported(void) {
    return 1;
}

#endif
// ===========================================================================

/// @brief GC finalizer for a PTY session's operating-system resources.
/// @param obj PTY implementation object being finalized.
static void pty_finalize(void *obj) {
    rt_pty_impl *pty = (rt_pty_impl *)obj;
    if (!pty)
        return;
    // A GC finalizer must not longjmp out of collector bookkeeping. Explicit
    // Destroy remains diagnostic; finalization is bounded best-effort cleanup.
    pty->finalizing = 1;
    pty_close(pty);
}

/// @brief Open an interactive pseudoterminal-backed child session.
/// @details Clears the calling thread's prior diagnostic, normalizes the
///          requested window to defaults 80x24 and maximum 4096x4096, then
///          starts the platform PTY backend. Program and arguments bypass any
///          command shell; a non-NULL environment replaces all inherited entries.
/// @param program Executable path or lookup name; must be nonempty and contain
///        no embedded NUL byte.
/// @param args Optional Seq of literal argument strings.
/// @param cwd Optional working directory; NULL or empty means inherit.
/// @param env Optional Seq of NAME=VALUE strings; NULL means inherit.
/// @param cols Initial columns; nonpositive selects 80 and values above 4096 clamp.
/// @param rows Initial rows; nonpositive selects 24 and values above 4096 clamp.
/// @return New GC-managed PTY handle with merged terminal output, or NULL after
///         recording the startup failure for rt_pty_last_error().
void *rt_pty_open(
    rt_string program, void *args, rt_string cwd, void *env, int64_t cols, int64_t rows) {
    pty_set_last_error(NULL);
    pty_clamp_size(&cols, &rows);
    void *handle = pty_open_impl(program, args, cwd, env, cols, rows, 0);
    if (handle)
        pty_set_last_error(NULL);
    return handle;
}

/// @brief Open a PTY with inherited environment plus explicit overrides.
void *rt_pty_open_with_env_overlay(
    rt_string program, void *args, rt_string cwd, void *env, int64_t cols, int64_t rows) {
    pty_set_last_error(NULL);
    pty_clamp_size(&cols, &rows);
    void *handle = pty_open_impl(program, args, cwd, env, cols, rows, 1);
    if (handle)
        pty_set_last_error(NULL);
    return handle;
}

/// @brief Wrap a PTY open attempt in a Result object.
/// @details Validation traps from rt_pty_open() are converted into Err strings
///          so callers can handle startup failures without separately reading
///          the thread-local LastError side channel.
/// @param program Program path.
/// @param args Argument sequence, or NULL.
/// @param cwd Working directory, or NULL/empty.
/// @param env Environment sequence, or NULL.
/// @param cols Initial terminal columns.
/// @param rows Initial terminal rows.
/// @return Owned `Zanna.Result` carrying a PtySession or an error string.
void *rt_pty_open_result(
    rt_string program, void *args, rt_string cwd, void *env, int64_t cols, int64_t rows) {
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        const char *err = rt_trap_get_error();
        rt_trap_clear_recovery();
        return rt_result_err_str(rt_const_cstr(err && err[0] ? err : "Pty.Open failed"));
    }

    void *handle = rt_pty_open(program, args, cwd, env, cols, rows);
    rt_trap_clear_recovery();
    if (!handle) {
        rt_string err = rt_pty_last_error();
        if (!err || rt_str_len(err) == 0) {
            rt_str_release_maybe(err);
            return rt_result_err_str(rt_const_cstr("Pty.Open failed"));
        }
        void *result = rt_result_err_str(err);
        rt_str_release_maybe(err);
        return result;
    }

    void *result = rt_result_ok(handle);
    if (rt_obj_release_check0(handle))
        rt_obj_free(handle);
    return result;
}

/// @brief Open a PTY with inherited environment overrides and return a Result.
void *rt_pty_open_with_env_overlay_result(
    rt_string program, void *args, rt_string cwd, void *env, int64_t cols, int64_t rows) {
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        const char *err = rt_trap_get_error();
        rt_trap_clear_recovery();
        return rt_result_err_str(
            rt_const_cstr(err && err[0] ? err : "Pty.OpenWithEnvOverlay failed"));
    }

    void *handle = rt_pty_open_with_env_overlay(program, args, cwd, env, cols, rows);
    rt_trap_clear_recovery();
    if (!handle) {
        rt_string err = rt_pty_last_error();
        if (!err || rt_str_len(err) == 0) {
            rt_str_release_maybe(err);
            return rt_result_err_str(rt_const_cstr("Pty.OpenWithEnvOverlay failed"));
        }
        void *result = rt_result_err_str(err);
        rt_str_release_maybe(err);
        return result;
    }

    void *result = rt_result_ok(handle);
    if (rt_obj_release_check0(handle))
        rt_obj_free(handle);
    return result;
}

/// @brief Report whether this runtime can create PTY sessions.
/// @details POSIX builds always report support. Windows dynamically requires
///          CreatePseudoConsole, ResizePseudoConsole, and ClosePseudoConsole;
///          an unavailable backend installs a default thread-local diagnostic.
/// @return 1 when the platform backend is available, otherwise 0.
int64_t rt_pty_is_supported(void) {
    pty_set_last_error(NULL);
    int64_t supported = pty_supported();
    if (!supported && pty_last_error[0] == '\0')
        pty_set_last_error("PTY support is not available on this platform");
    return supported;
}

/// @brief Copy the calling thread's latest PTY support or operation diagnostic.
/// @details Every public PTY operation clears the calling thread's previous
///          diagnostic before doing work, so this value always describes the
///          immediately preceding failed operation rather than stale history.
/// @return Newly allocated diagnostic string, or a newly allocated empty string
///         when no error is recorded.
rt_string rt_pty_last_error(void) {
    if (pty_last_error[0] == '\0')
        return empty_string();
    return rt_string_from_bytes(pty_last_error, strlen(pty_last_error));
}

/// @brief Test whether an object is a started, undestroyed PTY session.
/// @details A child that has exited remains a valid session until destruction,
///          allowing buffered output and status to be consumed.
/// @param handle Candidate opaque runtime object.
/// @return 1 for a valid PTY handle, otherwise 0.
int64_t rt_pty_is_valid(void *handle) {
    pty_set_last_error(NULL);
    rt_pty_impl *pty = pty_checked(handle);
    return pty && pty->started && !pty->destroyed ? 1 : 0;
}

/// @brief Attach a lifetime-safe output/exit wake target to a live PTY.
int64_t rt_pty_set_activity_wake(void *handle, rt_activity_wake_target *target) {
    pty_set_last_error(NULL);
    rt_pty_impl *pty = pty_checked(handle);
    if (!pty || !target || !pty->started || pty->destroyed)
        return 0;
    pty_activity_monitor_stop(pty);
    pty->activity_wake = rt_activity_wake_retain(target);
    if (!pty->activity_wake || !pty_activity_monitor_start(pty)) {
        rt_activity_wake_release(pty->activity_wake);
        pty->activity_wake = NULL;
        return 0;
    }
    return 1;
}

/// @brief Nonblockingly drain terminal output and check child completion.
/// @param handle Candidate PTY session handle.
/// @return 1 while the child is running, otherwise 0.
int64_t rt_pty_poll(void *handle) {
    pty_set_last_error(NULL);
    rt_pty_impl *pty = pty_checked(handle);
    if (!pty || !pty->started || pty->destroyed)
        return 0;
    pty_poll_internal(pty, 0);
    return pty->running ? 1 : 0;
}

/// @brief Poll and report whether a PTY child is still running.
/// @param handle Candidate PTY session handle.
/// @return 1 while running, otherwise 0 for exited or invalid handles.
int64_t rt_pty_is_running(void *handle) {
    return rt_pty_poll(handle);
}

/// @brief Consume merged terminal output buffered or immediately available.
/// @details The stream combines child stdout and stderr and may contain terminal
///          control sequences. If more than 16 MiB accumulated since the prior
///          read, returns the retained prefix after raising a truncation trap.
/// @param handle Candidate PTY session handle.
/// @return Newly allocated incremental output string, or an empty string for no
///         bytes or an invalid handle.
rt_string rt_pty_read(void *handle) {
    pty_set_last_error(NULL);
    rt_pty_impl *pty = pty_checked(handle);
    if (!pty || !pty->started || pty->destroyed)
        return empty_string();
    pty_drain(pty);
    return buffer_take(&pty->output_buf);
}

/// @brief Consume terminal output into a structured truncation-aware result.
/// @details Avoids the truncation trap by returning a map with string @c text
///          and Boolean @c truncated entries.
/// @param handle Candidate PTY session handle.
/// @return Caller-owned result map, including an empty nontruncated result for
///         invalid handles, or NULL when map allocation fails.
void *rt_pty_read_result(void *handle) {
    pty_set_last_error(NULL);
    rt_pty_impl *pty = pty_checked(handle);
    if (!pty || !pty->started || pty->destroyed)
        return buffer_take_result(NULL);
    pty_drain(pty);
    return buffer_take_result(&pty->output_buf);
}

/// @brief Write raw bytes to the interactive terminal input.
/// @details Honors runtime-string byte length, including embedded NUL. POSIX
///          writes use a nonblocking master and suppress only SIGPIPE caused by
///          the current write.
/// @param handle Candidate PTY session handle with open input.
/// @param data Runtime byte string; NULL is treated as empty.
/// @return Complete byte count, zero for empty input, a positive partial count
///         after later failure or would-block, or -1 when no byte can be written.
int64_t rt_pty_write(void *handle, rt_string data) {
    pty_set_last_error(NULL);
    rt_pty_impl *pty = pty_checked(handle);
    if (!pty || !pty->started || pty->destroyed) {
        pty_set_last_error("PTY input handle is invalid");
        return -1;
    }
    const char *bytes = data ? rt_string_cstr(data) : "";
    size_t len = data ? (size_t)rt_str_len(data) : 0;
    if (len == 0)
        return 0;
    int64_t written = pty_write_impl(pty, bytes, len);
    if (written < 0)
        pty_set_last_error("PTY input is closed or temporarily full");
    return written;
}

/// @brief Resize the pseudoterminal window.
/// @details Applies the same 80x24 defaults and 4096 maxima as open. POSIX uses
///          TIOCSWINSZ, which notifies the foreground terminal group; Windows
///          calls ResizePseudoConsole. Backend failures update LastError.
/// @param handle Candidate PTY session handle.
/// @param cols Requested columns.
/// @param rows Requested rows.
/// @return 1 only when the OS applies the normalized size, otherwise 0.
int64_t rt_pty_resize(void *handle, int64_t cols, int64_t rows) {
    pty_set_last_error(NULL);
    rt_pty_impl *pty = pty_checked(handle);
    if (!pty || !pty->started || pty->destroyed)
        return 0;
    pty_clamp_size(&cols, &rows);
    // Propagate the backend result: TRUE only when the OS actually applied the
    // new size (TIOCSWINSZ / ResizePseudoConsole succeeded). Failures set the
    // PTY LastError (VDOC-214).
    return pty_resize_impl(pty, cols, rows) ? 1 : 0;
}

/// @brief Poll and read the retained PTY child exit status.
/// @param handle Candidate PTY session handle.
/// @return Normal exit code, negated signal number on POSIX, or -1 while
///         running, after status failure, or for an invalid handle.
int64_t rt_pty_exit_code(void *handle) {
    pty_set_last_error(NULL);
    rt_pty_impl *pty = pty_checked(handle);
    if (!pty || !pty->started || pty->destroyed)
        return -1;
    pty_poll_internal(pty, 0);
    return pty->running ? -1 : pty->exit_code;
}

/// @brief Request termination of a running PTY child without waiting.
/// @details Windows requests termination with status 1. POSIX sends SIGTERM to
///          the child session/process group, falling back to the child pid when
///          that group no longer exists.
/// @param handle Candidate PTY session handle.
/// @return 1 when a termination request is accepted, otherwise 0.
int64_t rt_pty_kill(void *handle) {
    pty_set_last_error(NULL);
    rt_pty_impl *pty = pty_checked(handle);
    if (!pty || !pty->started || pty->destroyed || !pty->running)
        return 0;
#if defined(_WIN32)
    if (!pty->process)
        return 0;
    return TerminateProcess(pty->process, 1) ? 1 : 0;
#else
    if (pty->pid <= 0)
        return 0;
    if (kill(-pty->pid, SIGTERM) == 0)
        return 1;
    if (errno == ESRCH)
        return kill(pty->pid, SIGTERM) == 0 ? 1 : 0;
    return 0;
#endif
}

/// @brief Wait for PTY child completion and retain its exit status.
/// @param handle Candidate PTY session handle.
/// @return Normal exit code, negated signal number on POSIX, or -1 for an
///         invalid handle or wait/status failure.
int64_t rt_pty_wait(void *handle) {
    pty_set_last_error(NULL);
    rt_pty_impl *pty = pty_checked(handle);
    if (!pty || !pty->started || pty->destroyed)
        return -1;
    pty_poll_internal(pty, 1);
    return pty->exit_code;
}

/// @brief Idempotently terminate if needed and release PTY resources.
/// @details The GC-managed object remains allocated but is invalid for all
///          subsequent PTY operations.
/// @param handle Candidate PTY session handle; invalid values are ignored.
void rt_pty_destroy(void *handle) {
    pty_set_last_error(NULL);
    rt_pty_impl *pty = pty_checked(handle);
    pty_close(pty);
}
