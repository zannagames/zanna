//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/system/rt_process.c
// Purpose: Implements streaming, cancellable child-process handles for
//          Zanna.System.Process.
//
// Key invariants:
//   - Start returns NULL when the process cannot be spawned.
//   - Output reads are non-blocking and incremental.
//   - Ordered reads preserve capture order and tag every stdout/stderr chunk.
//   - Poll/IsRunning reap the process once and preserve the exit code.
//   - Destroy is idempotent and closes all OS handles.
//   - Every child belongs to a runtime-owned process group/job so Kill and
//     Destroy terminate descendants as well as the direct child.
//   - A non-null environment sequence replaces the complete child environment.
//   - Windows environment blocks are UTF-16, case-insensitively sorted, and
//     passed with CREATE_UNICODE_ENVIRONMENT.
//
// Ownership/Lifetime:
//   - Handles are rt_obj_new_i64-allocated and GC-managed.
//   - Destroy or the GC finalizer terminates a still-running child and releases
//     pipe buffers and OS resources.
//
// Links: src/runtime/system/rt_process.h,
//        docs/adr/0281-event-driven-process-pty-gui-wakes.md
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_process.c
 * @brief Implements streaming managed child-process handles.
 * @details Direct platform launch connects runtime-owned stdin, stdout, and
 *          stderr pipes, supports optional working directories and complete
 *          environment replacement, drains bounded output incrementally,
 *          reaps completion once, and finalizes live children and native
 *          resources idempotently.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "rt_process.h"

#include "rt_activity_wake.h"
#include "rt_internal.h"
#include "rt_map.h"
#include "rt_object.h"
#include "rt_platform.h"
#include "rt_seq.h"
#include "rt_string.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include "rt_file_path.h"
#include "rt_win32_wait.h"
#include <wchar.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

/// @brief Maximum retained bytes per stdout or stderr buffer.
#define PROCESS_BUFFER_MAX_SIZE (16 * 1024 * 1024)
/// @brief Maximum retained bytes in the ordered combined-output queue.
#define PROCESS_ORDERED_OUTPUT_MAX_SIZE (16 * 1024 * 1024)
/// @brief Maximum stdin bytes accepted ahead of the Windows writer thread.
#define PROCESS_WINDOWS_STDIN_QUEUE_MAX_SIZE (1024 * 1024)
/// @brief Maximum time finalization waits for an asynchronously terminated Windows child.
#define PROCESS_WINDOWS_TERMINATE_WAIT_MS 5000u

enum process_output_stream {
    PROCESS_OUTPUT_STDOUT = 1,
    PROCESS_OUTPUT_STDERR = 2,
};

/// @brief One capture-ordered, stream-tagged output chunk.
typedef struct process_output_chunk {
    struct process_output_chunk *next;
    char *data;
    size_t len;
    size_t cap;
    uint64_t sequence;
    int stream;
} process_output_chunk;

/// @brief Temporary NULL-terminated C-string vector for argv or envp.
/// @details The vector owns references to runtime strings whose byte pointers
///          appear in @c values, keeping those pointers valid through spawning.
typedef struct process_string_vector {
    char **values;            ///< Owned pointer array ending in NULL.
    rt_string *owned_strings; ///< Owned array of retained runtime strings.
    int64_t owned_count;      ///< Number of entries in @c owned_strings.
} process_string_vector;

/// @brief Platform process state stored in a Zanna.System.Process object.
/// @details Owns redirected stream buffers and all parent-side process, thread,
///          pipe, or descriptor handles until explicit destruction or GC
///          finalization.
typedef struct rt_process_impl {
    int8_t started;
    int8_t running;
    int8_t destroyed;
    int64_t exit_code;
    process_output_chunk *output_head;
    process_output_chunk *output_tail;
    size_t output_bytes;
    size_t stdout_output_bytes;
    size_t stderr_output_bytes;
    uint64_t next_output_sequence;
    int output_truncated;
    int stdout_output_truncated;
    int stderr_output_truncated;
    rt_activity_wake_target *activity_wake;
    volatile int activity_monitor_stop;
    volatile int activity_monitor_armed;
    volatile int stdout_activity_closed;
    volatile int stderr_activity_closed;
    int activity_monitor_started;

#if defined(_WIN32)
    HANDLE process;
    HANDLE thread;
    HANDLE job;
    HANDLE stdout_read;
    HANDLE stderr_read;
    HANDLE stdin_write;
    HANDLE stdin_thread;
    HANDLE stdin_event;
    CRITICAL_SECTION stdin_lock;
    char *stdin_queue;
    size_t stdin_queue_len;
    size_t stdin_queue_cap;
    int stdin_lock_initialized;
    int stdin_shutdown;
    int stdin_failed;
    HANDLE activity_thread;
    HANDLE activity_stop_event;
    HANDLE activity_rearm_event;
#else
    pid_t pid;
    int stdout_fd;
    int stderr_fd;
    int stdin_fd;
    pthread_t activity_thread;
    int activity_control_read;
    int activity_control_write;
#endif
} rt_process_impl;

static void process_finalize(void *obj);
static void process_close(rt_process_impl *proc);
static int process_activity_monitor_start(rt_process_impl *proc);
static void process_activity_monitor_stop(rt_process_impl *proc);
static void process_activity_rearm(rt_process_impl *proc);

/// @brief Release one local reference to a managed runtime object.
static void process_release_object(void *object) {
    if (object && rt_obj_release_check0(object))
        rt_obj_free(object);
}

/// @brief Allocate an empty runtime string for process API fallbacks.
/// @return Newly allocated empty runtime string owned by the caller.
static rt_string empty_string(void) {
    return rt_string_from_bytes("", 0);
}

/// @brief Validate and cast an opaque runtime process handle.
/// @param handle Candidate object pointer.
/// @return Borrowed process implementation pointer when the object has the
///         expected class and payload size, otherwise NULL.
static rt_process_impl *process_checked(void *handle) {
    if (!rt_obj_is_instance(handle, RT_PROCESS_CLASS_ID, sizeof(rt_process_impl)))
        return NULL;
    return (rt_process_impl *)handle;
}

/// @brief Extract a runtime string as a C-string-safe byte view for process APIs.
/// @details Child-process interfaces cannot represent embedded NUL bytes in
///          program paths, cwd strings, argv entries, or environment entries.
///          This helper rejects such values before OS APIs silently truncate
///          them. Empty strings are considered valid by this helper; callers
///          enforce required/non-empty fields separately.
/// @param value Runtime string to inspect.
/// @param out_text Receives a borrowed C-string pointer on success.
/// @param out_len Receives the byte length excluding the terminator.
/// @return 1 for a valid C-string-safe runtime string, 0 otherwise.
static int process_string_cstr_view(rt_string value, const char **out_text, size_t *out_len) {
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

/// @brief Validate all entries in a process string sequence for C-string safety.
/// @details Args and environment values are represented as runtime strings, but
///          the OS APIs below consume NUL-terminated C strings. This validation
///          prevents child processes from seeing truncated values.
/// @param items Runtime sequence of strings; may be NULL.
/// @param require_env_assignment Non-zero to require NAME=VALUE entries.
/// @param trap_msg Trap message for invalid input.
/// @return 1 when every item is valid, 0 when a trap was raised.
static int process_validate_string_sequence(void *items,
                                            int require_env_assignment,
                                            const char *trap_msg) {
    int64_t count = items ? rt_seq_len(items) : 0;
    if (count < 0)
        return 1;
    for (int64_t i = 0; i < count; i++) {
        rt_string item = rt_seq_get_str(items, i);
        const char *text = NULL;
        size_t len = 0;
        int ok = process_string_cstr_view(item, &text, &len);
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

/// @brief Store a runtime string under a constant text key in a runtime map.
/// @details Supplies an empty constant string when @p value is NULL. The map
///          operation acquires its own value ownership; this helper does not
///          consume the caller's reference.
/// @param map Destination runtime map; NULL is ignored.
/// @param key Borrowed non-NULL C-string key.
/// @param value Borrowed runtime string value; may be NULL.
static void map_set_string_owned(void *map, const char *key, rt_string value) {
    if (!map || !key)
        return;
    rt_map_set_str(map, rt_const_cstr(key), value ? value : rt_const_cstr(""));
}

/// @brief Locate one stream's retained-byte counter.
static size_t *output_stream_bytes(rt_process_impl *proc, int stream) {
    return stream == PROCESS_OUTPUT_STDERR ? &proc->stderr_output_bytes
                                           : &proc->stdout_output_bytes;
}

/// @brief Locate one stream's truncation latch.
static int *output_stream_truncation(rt_process_impl *proc, int stream) {
    return stream == PROCESS_OUTPUT_STDERR ? &proc->stderr_output_truncated
                                           : &proc->stdout_output_truncated;
}

/// @brief Locate one activity monitor's EOF/error latch.
static volatile int *output_stream_activity_closed(rt_process_impl *proc, int stream) {
    return stream == PROCESS_OUTPUT_STDERR ? &proc->stderr_activity_closed
                                           : &proc->stdout_activity_closed;
}

/// @brief Append one observed stream read to the sole retained output queue.
/// @details Adjacent reads from the same stream are coalesced without crossing
///          a stream boundary. Chunk allocations grow geometrically, avoiding
///          a realloc-and-copy for every 4 KiB pipe read. The tagged queue is
///          also the backing store for legacy stdout/stderr reads, so captured
///          bytes are retained once instead of in three parallel buffers.
static void ordered_output_append(rt_process_impl *proc, int stream, const char *data, size_t len) {
    if (!proc || !data || len == 0)
        return;
    size_t *stream_bytes = output_stream_bytes(proc, stream);
    int *stream_truncated = output_stream_truncation(proc, stream);
    if (*stream_bytes >= PROCESS_BUFFER_MAX_SIZE) {
        *stream_truncated = 1;
        proc->output_truncated = 1;
        return;
    }
    if (len > PROCESS_BUFFER_MAX_SIZE - *stream_bytes) {
        len = PROCESS_BUFFER_MAX_SIZE - *stream_bytes;
        *stream_truncated = 1;
        proc->output_truncated = 1;
    }
    if (len == 0)
        return;

    process_output_chunk *tail = proc->output_tail;
    if (tail && tail->stream == stream && len <= SIZE_MAX - tail->len) {
        size_t needed = tail->len + len;
        if (needed > tail->cap) {
            size_t capacity = tail->cap ? tail->cap : 4096u;
            while (capacity < needed && capacity <= PROCESS_BUFFER_MAX_SIZE / 2u)
                capacity *= 2u;
            if (capacity < needed)
                capacity = needed;
            char *grown = (char *)realloc(tail->data, capacity);
            if (!grown) {
                *stream_truncated = 1;
                proc->output_truncated = 1;
                return;
            }
            tail->data = grown;
            tail->cap = capacity;
        }
        memcpy(tail->data + tail->len, data, len);
        tail->len += len;
        proc->output_bytes += len;
        *stream_bytes += len;
        if (proc->output_bytes > PROCESS_ORDERED_OUTPUT_MAX_SIZE)
            proc->output_truncated = 1;
        return;
    }

    process_output_chunk *chunk = (process_output_chunk *)calloc(1, sizeof(*chunk));
    if (!chunk) {
        *stream_truncated = 1;
        proc->output_truncated = 1;
        return;
    }
    size_t capacity = len < 4096u ? 4096u : len;
    chunk->data = (char *)malloc(capacity);
    if (!chunk->data) {
        free(chunk);
        *stream_truncated = 1;
        proc->output_truncated = 1;
        return;
    }
    memcpy(chunk->data, data, len);
    chunk->len = len;
    chunk->cap = capacity;
    chunk->stream = stream;
    chunk->sequence = proc->next_output_sequence++;
    if (proc->output_tail)
        proc->output_tail->next = chunk;
    else
        proc->output_head = chunk;
    proc->output_tail = chunk;
    proc->output_bytes += len;
    *stream_bytes += len;
    if (proc->output_bytes > PROCESS_ORDERED_OUTPUT_MAX_SIZE)
        proc->output_truncated = 1;
}

/// @brief Consume one stream from the shared tagged queue into a string.
/// @details Chunks for the other stream remain linked and retain their capture
///          order. The shared native bytes are copied only once, into the
///          managed return value, before their queue nodes are released.
static rt_string stream_output_take_string(rt_process_impl *proc, int stream, int *was_truncated) {
    if (was_truncated)
        *was_truncated = 0;
    if (!proc)
        return empty_string();

    size_t *stream_bytes = output_stream_bytes(proc, stream);
    int *stream_truncated = output_stream_truncation(proc, stream);
    if (was_truncated)
        *was_truncated = *stream_truncated;
    if (*stream_bytes == 0) {
        *stream_truncated = 0;
        return empty_string();
    }

    char *bytes = (char *)malloc(*stream_bytes);
    if (!bytes)
        return NULL;
    size_t copied = 0;
    for (process_output_chunk *chunk = proc->output_head; chunk; chunk = chunk->next) {
        if (chunk->stream != stream)
            continue;
        memcpy(bytes + copied, chunk->data, chunk->len);
        copied += chunk->len;
    }
    rt_string result = rt_string_from_bytes(bytes, copied);
    free(bytes);
    if (!result)
        return NULL;

    process_output_chunk *previous = NULL;
    process_output_chunk *chunk = proc->output_head;
    while (chunk) {
        process_output_chunk *next = chunk->next;
        if (chunk->stream == stream) {
            if (previous)
                previous->next = next;
            else
                proc->output_head = next;
            if (proc->output_tail == chunk)
                proc->output_tail = previous;
            proc->output_bytes -= chunk->len;
            free(chunk->data);
            free(chunk);
        } else {
            previous = chunk;
        }
        chunk = next;
    }
    *stream_bytes = 0;
    *stream_truncated = 0;
    return result;
}

/// @brief Consume one stream and trap after returning its retained prefix when truncated.
static rt_string stream_output_take(rt_process_impl *proc, int stream) {
    int truncated = 0;
    rt_string result = stream_output_take_string(proc, stream, &truncated);
    if (truncated)
        rt_trap("Process: output truncated");
    return result;
}

/// @brief Consume one stream into a structured nontrapping read result.
static void *stream_output_take_result(rt_process_impl *proc, int stream) {
    int truncated = 0;
    rt_string output = stream_output_take_string(proc, stream, &truncated);
    if (!output)
        return NULL;
    void *result = rt_map_new();
    if (!result) {
        rt_str_release_maybe(output);
        return NULL;
    }
    map_set_string_owned(result, "text", output);
    rt_map_set_bool(result, rt_const_cstr("truncated"), truncated ? 1 : 0);
    rt_str_release_maybe(output);
    return result;
}

/// @brief Free every retained ordered-output chunk.
static void ordered_output_free(rt_process_impl *proc) {
    if (!proc)
        return;
    process_output_chunk *chunk = proc->output_head;
    while (chunk) {
        process_output_chunk *next = chunk->next;
        free(chunk->data);
        free(chunk);
        chunk = next;
    }
    proc->output_head = NULL;
    proc->output_tail = NULL;
    proc->output_bytes = 0;
    proc->stdout_output_bytes = 0;
    proc->stderr_output_bytes = 0;
    proc->output_truncated = 0;
    proc->stdout_output_truncated = 0;
    proc->stderr_output_truncated = 0;
}

/// @brief Consume ordered output as `{ chunks, truncated }`.
/// @details `chunks` is an owning Seq of maps with `sequence`, `stream`, and
///          `text`. The native queue is cleared only after its content has been
///          copied into managed values.
static void *ordered_output_take_result(rt_process_impl *proc) {
    void *result = rt_map_new();
    void *chunks = rt_seq_new_owned();
    if (!result || !chunks) {
        process_release_object(result);
        process_release_object(chunks);
        return NULL;
    }

    size_t remaining = PROCESS_ORDERED_OUTPUT_MAX_SIZE;
    int truncated = proc && proc->output_truncated;
    for (process_output_chunk *chunk = proc ? proc->output_head : NULL; chunk && remaining > 0;
         chunk = chunk->next) {
        size_t emitted = chunk->len < remaining ? chunk->len : remaining;
        void *entry = rt_map_new();
        rt_string text = rt_string_from_bytes(chunk->data, emitted);
        if (!entry || !text) {
            process_release_object(entry);
            rt_str_release_maybe(text);
            process_release_object(chunks);
            process_release_object(result);
            return NULL;
        }
        rt_map_set_int(entry, rt_const_cstr("sequence"), (int64_t)chunk->sequence);
        map_set_string_owned(
            entry,
            "stream",
            rt_const_cstr(chunk->stream == PROCESS_OUTPUT_STDERR ? "stderr" : "stdout"));
        map_set_string_owned(entry, "text", text);
        rt_str_release_maybe(text);
        rt_seq_push(chunks, entry);
        process_release_object(entry);
        remaining -= emitted;
        if (emitted < chunk->len)
            truncated = 1;
    }

    rt_map_set(result, rt_const_cstr("chunks"), chunks);
    rt_map_set_bool(result, rt_const_cstr("truncated"), truncated ? 1 : 0);
    process_release_object(chunks);
    if (proc)
        ordered_output_free(proc);
    return result;
}

/// @brief Build a temporary C-string vector from a runtime string sequence.
/// @details Optionally prepends the borrowed @p first pointer, retains every
///          sequence string whose byte view enters the vector, and appends a
///          NULL terminator.
/// @param first Borrowed leading C string used when @p include_first is nonzero.
/// @param items Optional runtime sequence of strings.
/// @param include_first Nonzero to prepend @p first.
/// @return Owned vector aggregate. A zeroed aggregate with NULL @c values
///         indicates invalid size or allocation failure; release successful
///         aggregates with free_string_vector().
static process_string_vector build_string_vector(const char *first,
                                                 void *items,
                                                 int include_first) {
    process_string_vector vector;
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

/// @brief Release a temporary string vector and all retained elements.
/// @param vector Aggregate returned by build_string_vector(); NULL is ignored
///        and the aggregate is zeroed after release.
static void free_string_vector(process_string_vector *vector) {
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

/// @brief Allocate and initialize a GC-managed process object.
/// @details Installs process_finalize(), sets the unknown exit code to -1, and
///          initializes every platform handle to its invalid sentinel.
/// @return New process implementation object, or NULL after an allocation trap.
static rt_process_impl *process_alloc(void) {
    rt_process_impl *proc =
        (rt_process_impl *)rt_obj_new_i64(RT_PROCESS_CLASS_ID, (int64_t)sizeof(rt_process_impl));
    if (!proc) {
        rt_trap("Process.Start: allocation failed");
        return NULL;
    }
    memset(proc, 0, sizeof(*proc));
    proc->exit_code = -1;
#if defined(_WIN32)
    proc->process = NULL;
    proc->thread = NULL;
    proc->job = NULL;
    proc->stdout_read = NULL;
    proc->stderr_read = NULL;
    proc->stdin_write = NULL;
    proc->stdin_thread = NULL;
    proc->stdin_event = NULL;
    proc->activity_thread = NULL;
    proc->activity_stop_event = NULL;
    proc->activity_rearm_event = NULL;
#else
    proc->pid = -1;
    proc->stdout_fd = -1;
    proc->stderr_fd = -1;
    proc->stdin_fd = -1;
    proc->activity_control_read = -1;
    proc->activity_control_write = -1;
#endif
    rt_obj_set_finalizer(proc, process_finalize);
    return proc;
}

#if defined(_WIN32)

/// @brief Compute the encoded size of one quoted Windows command-line argument.
/// @details Applies CommandLineToArgvW-compatible backslash escaping. The count
///          includes outer quotes and excludes a final NUL.
/// @param s Borrowed NUL-terminated UTF-8 argument.
/// @return Required byte count, or SIZE_MAX on arithmetic overflow.
static size_t cmdline_quoted_len(const char *s) {
    size_t len = 2;
    size_t backslashes = 0;
    for (; *s; s++) {
        if (*s == '\\') {
            if (backslashes == SIZE_MAX)
                return SIZE_MAX;
            backslashes++;
        } else if (*s == '"') {
            if (backslashes > (SIZE_MAX - len - 2) / 2)
                return SIZE_MAX;
            len += (size_t)backslashes * 2 + 2;
            backslashes = 0;
        } else {
            if (backslashes > SIZE_MAX - len - 1)
                return SIZE_MAX;
            len += (size_t)backslashes + 1;
            backslashes = 0;
        }
    }
    if (backslashes > (SIZE_MAX - len) / 2)
        return SIZE_MAX;
    len += (size_t)backslashes * 2;
    return len;
}

/// @brief Append one quoted Windows command-line argument.
/// @param out Destination cursor with cmdline_quoted_len(@p s) bytes available.
/// @param s Borrowed NUL-terminated UTF-8 argument.
/// @return Cursor immediately after the appended closing quote; no NUL is added.
static char *cmdline_append_quoted(char *out, const char *s) {
    *out++ = '"';
    size_t backslashes = 0;
    for (; *s; s++) {
        if (*s == '\\') {
            backslashes++;
        } else if (*s == '"') {
            for (size_t i = 0; i < backslashes * 2; i++)
                *out++ = '\\';
            *out++ = '\\';
            *out++ = '"';
            backslashes = 0;
        } else {
            for (size_t i = 0; i < backslashes; i++)
                *out++ = '\\';
            *out++ = *s;
            backslashes = 0;
        }
    }
    for (size_t i = 0; i < backslashes * 2; i++)
        *out++ = '\\';
    *out++ = '"';
    return out;
}

/// @brief Build a direct-execution Windows command line.
/// @details Quotes the executable name and every argument independently so the
///          child can reconstruct literal argv values without a shell.
/// @param program Borrowed validated executable path or lookup name.
/// @param args Optional runtime sequence of validated argument strings.
/// @return Caller-owned NUL-terminated UTF-8 command line, or NULL on overflow
///         or allocation failure.
static char *build_cmdline(const char *program, void *args) {
    int64_t nargs = args ? rt_seq_len(args) : 0;
    size_t len = cmdline_quoted_len(program);
    if (len == SIZE_MAX)
        return NULL;
    for (int64_t i = 0; i < nargs; i++) {
        rt_string arg = rt_seq_get_str(args, i);
        size_t quoted_len = cmdline_quoted_len(arg ? rt_string_cstr(arg) : "");
        rt_str_release_maybe(arg);
        if (quoted_len == SIZE_MAX || quoted_len > SIZE_MAX - len - 1)
            return NULL;
        len += 1 + quoted_len;
    }
    if (len == SIZE_MAX)
        return NULL;

    char *cmdline = (char *)malloc(len + 1);
    if (!cmdline)
        return NULL;

    char *out = cmdline;
    out = cmdline_append_quoted(out, program);
    for (int64_t i = 0; i < nargs; i++) {
        rt_string arg = rt_seq_get_str(args, i);
        *out++ = ' ';
        out = cmdline_append_quoted(out, arg ? rt_string_cstr(arg) : "");
        rt_str_release_maybe(arg);
    }
    *out = '\0';
    return cmdline;
}

/// @brief Extended Windows startup state with a constrained handle allow-list.
/// @details CreateProcessW must use inheritable handles for redirected stdio,
///          but STARTUPINFOEX lets the runtime inherit only the three pipe ends
///          that the child actually needs instead of leaking every inheritable
///          handle in the parent process.
typedef struct process_win_startup_info {
    STARTUPINFOEXW startup;
    LPPROC_THREAD_ATTRIBUTE_LIST attrs;
} process_win_startup_info;

/// @brief Initialize STARTUPINFOEXW with an inherited-handle allow-list.
/// @param info Startup container to initialize.
/// @param handles Handles permitted to cross into the child process.
/// @param handle_count Number of entries in @p handles.
/// @return 1 on success, 0 on allocation/API failure.
static int process_win_startup_init(process_win_startup_info *info,
                                    HANDLE *handles,
                                    DWORD handle_count) {
    SIZE_T attr_size = 0;
    if (!info)
        return 0;
    memset(info, 0, sizeof(*info));
    info->startup.StartupInfo.cb = sizeof(info->startup);
    if (!handles || handle_count == 0)
        return 1;
    (void)InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
    info->attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attr_size);
    if (!info->attrs)
        return 0;
    if (!InitializeProcThreadAttributeList(info->attrs, 1, 0, &attr_size)) {
        free(info->attrs);
        info->attrs = NULL;
        return 0;
    }
    info->startup.lpAttributeList = info->attrs;
    if (!UpdateProcThreadAttribute(info->attrs,
                                   0,
                                   PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   handles,
                                   sizeof(HANDLE) * handle_count,
                                   NULL,
                                   NULL)) {
        DeleteProcThreadAttributeList(info->attrs);
        free(info->attrs);
        info->attrs = NULL;
        info->startup.lpAttributeList = NULL;
        return 0;
    }
    return 1;
}

/// @brief Destroy a Windows extended startup handle allow-list.
/// @param info Startup container previously initialized by process_win_startup_init.
static void process_win_startup_destroy(process_win_startup_info *info) {
    if (!info || !info->attrs)
        return;
    DeleteProcThreadAttributeList(info->attrs);
    free(info->attrs);
    info->attrs = NULL;
    info->startup.lpAttributeList = NULL;
}

/// @brief Convert a UTF-8 command line, program path, and cwd to wide strings.
/// @details CreateProcessW mutates the command-line buffer, so every output is
///          heap allocated. The cwd output may be NULL when no cwd was supplied.
/// @param program UTF-8 executable path or lookup name.
/// @param cmdline UTF-8 quoted command line.
/// @param cwd Optional UTF-8 working directory.
/// @param out_program Receives allocated wide program string.
/// @param out_cmdline Receives allocated wide command-line buffer.
/// @param out_cwd Receives allocated wide cwd or NULL.
/// @return 1 on success, 0 on conversion failure.
static int process_build_wide_start_strings(const char *program,
                                            const char *cmdline,
                                            const char *cwd,
                                            wchar_t **out_program,
                                            wchar_t **out_cmdline,
                                            wchar_t **out_cwd) {
    if (out_program)
        *out_program = NULL;
    if (out_cmdline)
        *out_cmdline = NULL;
    if (out_cwd)
        *out_cwd = NULL;
    if (!program || !cmdline || !out_program || !out_cmdline || !out_cwd)
        return 0;
    *out_program = rt_file_path_utf8_to_wide(program);
    *out_cmdline = rt_file_path_utf8_to_wide(cmdline);
    if (cwd)
        *out_cwd = rt_file_path_utf8_to_wide(cwd);
    if (!*out_program || !*out_cmdline || (cwd && !*out_cwd)) {
        free(*out_program);
        free(*out_cmdline);
        free(*out_cwd);
        *out_program = NULL;
        *out_cmdline = NULL;
        *out_cwd = NULL;
        return 0;
    }
    return 1;
}

/// @brief Validate a Windows environment block entry from a runtime string.
/// @details CreateProcess expects NAME=VALUE entries separated by NUL bytes and
///          terminated by an extra NUL. Rejecting embedded NUL bytes and missing
///          names prevents truncation and malformed environment blocks.
/// @param item Runtime string entry to inspect.
/// @param text_out Receives a borrowed pointer when valid.
/// @param len_out Receives the byte length when valid.
/// @return 1 when @p item is valid, 0 otherwise.
static int env_item_view(rt_string item, const char **text_out, size_t *len_out) {
    const char *text = item ? rt_string_cstr(item) : NULL;
    int64_t signed_len = item ? rt_str_len(item) : -1;
    const char *equals = NULL;
    if (text_out)
        *text_out = NULL;
    if (len_out)
        *len_out = 0;
    if (!text || signed_len <= 0 || (uint64_t)signed_len > SIZE_MAX)
        return 0;
    size_t len = (size_t)signed_len;
    if (memchr(text, '\0', len))
        return 0;
    equals = (const char *)memchr(text, '=', len);
    if (!equals || equals == text)
        return 0;
    if (text_out)
        *text_out = text;
    if (len_out)
        *len_out = len;
    return 1;
}

/// @brief Compare UTF-16 environment entries for CreateProcessW ordering.
/// @details Windows environment names are case-insensitive, so the complete
///          NAME=VALUE strings are ordered with the native case-insensitive
///          wide-string comparator. A binary comparison makes equivalent
///          spellings deterministic.
typedef int(WINAPI *process_compare_string_ordinal_fn)(LPCWCH, int, LPCWCH, int, BOOL);

static INIT_ONCE g_process_compare_once = INIT_ONCE_STATIC_INIT;
static process_compare_string_ordinal_fn g_process_compare_string_ordinal = NULL;

/// @brief Resolve CompareStringOrdinal without expanding the native import surface.
/// @param once Win32 one-time initialization token; unused by the resolver.
/// @param param Optional caller parameter; unused.
/// @param context Optional callback context destination; unused.
/// @return TRUE so InitOnce records the resolution attempt as complete.
static BOOL CALLBACK process_resolve_compare_ordinal(PINIT_ONCE once, PVOID param, PVOID *context) {
    (void)once;
    (void)param;
    (void)context;
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (kernel32) {
        g_process_compare_string_ordinal =
            (process_compare_string_ordinal_fn)(void *)GetProcAddress(kernel32,
                                                                      "CompareStringOrdinal");
    }
    return TRUE;
}

/// @brief Invoke CompareStringOrdinal after thread-safe lazy resolution.
/// @details Falls back to exact wide-string equality when the API cannot be
///          resolved; callers use a binary comparison to order unequal values.
/// @param left First UTF-16 string.
/// @param left_len First string length, or -1 for NUL-terminated input.
/// @param right Second UTF-16 string.
/// @param right_len Second string length, or -1 for NUL-terminated input.
/// @return A CSTR_* ordering value from CompareStringOrdinal, CSTR_EQUAL for
///         equal fallback inputs, or 0 for unequal fallback inputs.
static int process_compare_ordinal(const wchar_t *left,
                                   int left_len,
                                   const wchar_t *right,
                                   int right_len) {
    (void)InitOnceExecuteOnce(&g_process_compare_once, process_resolve_compare_ordinal, NULL, NULL);
    if (g_process_compare_string_ordinal)
        return g_process_compare_string_ordinal(left, left_len, right, right_len, TRUE);
    if (left_len < 0 && right_len < 0)
        return wcscmp(left, right) == 0 ? CSTR_EQUAL : 0;
    return wcsncmp(left, right, (size_t)left_len) == 0 ? CSTR_EQUAL : 0;
}

/// @brief Compare two UTF-16 environment-entry pointers for qsort.
/// @details Orders entries case-insensitively first, then uses case-sensitive
///          binary order to make equivalent spellings deterministic.
/// @param left Pointer to the first wchar_t pointer.
/// @param right Pointer to the second wchar_t pointer.
/// @return Negative, zero, or positive according to the required environment
///         block ordering.
static int compare_env_entry_wide(const void *left, const void *right) {
    const wchar_t *lhs = *(const wchar_t *const *)left;
    const wchar_t *rhs = *(const wchar_t *const *)right;
    int result = process_compare_ordinal(lhs, -1, rhs, -1);
    if (result == CSTR_LESS_THAN)
        return -1;
    if (result == CSTR_GREATER_THAN)
        return 1;
    return wcscmp(lhs, rhs);
}

/// @brief Test whether two UTF-16 environment entries name the same variable.
/// @param lhs First NUL-terminated NAME=VALUE entry.
/// @param rhs Second NUL-terminated NAME=VALUE entry.
/// @return 1 when the nonempty names before '=' compare case-insensitively
///         equal, otherwise 0.
static int env_entry_names_equal_wide(const wchar_t *lhs, const wchar_t *rhs) {
    size_t lhs_len = 0;
    size_t rhs_len = 0;
    while (lhs[lhs_len] && lhs[lhs_len] != L'=')
        lhs_len++;
    while (rhs[rhs_len] && rhs[rhs_len] != L'=')
        rhs_len++;
    if (lhs_len != rhs_len || lhs_len == 0 || lhs_len > INT_MAX)
        return 0;
    return process_compare_ordinal(lhs, (int)lhs_len, rhs, (int)rhs_len) == CSTR_EQUAL;
}

/// @brief Build a UTF-16 environment block for CreateProcessW.
/// @details Converts each validated NAME=VALUE runtime string independently
///          and concatenates the resulting wide strings with single NUL
///          separators plus a final extra NUL, as required by CreateProcessW.
/// @param env Runtime sequence of NAME=VALUE strings; may be NULL.
/// @return Allocated wide environment block, or NULL for no env / failure.
static wchar_t *build_env_block_wide(void *env) {
    if (!env)
        return NULL;

    int64_t count = rt_seq_len(env);
    if (count < 0 || (uint64_t)count > SIZE_MAX / sizeof(wchar_t *))
        return NULL;
    size_t total_wchars = 1;
    wchar_t **entries = NULL;
    if (count > 0) {
        entries = (wchar_t **)calloc((size_t)count, sizeof(wchar_t *));
        if (!entries)
            return NULL;
    }

    for (int64_t i = 0; i < count; i++) {
        rt_string item = rt_seq_get_str(env, i);
        const char *text = NULL;
        size_t item_len = 0;
        if (!env_item_view(item, &text, &item_len)) {
            rt_str_release_maybe(item);
            goto fail;
        }
        entries[i] = rt_file_path_utf8_to_wide(text);
        rt_str_release_maybe(item);
        if (!entries[i])
            goto fail;
        size_t wide_len = wcslen(entries[i]);
        if (wide_len == SIZE_MAX || total_wchars > SIZE_MAX - wide_len - 1)
            goto fail;
        total_wchars += wide_len + 1;
    }

    if (count > 1) {
        qsort(entries, (size_t)count, sizeof(*entries), compare_env_entry_wide);
        for (int64_t i = 1; i < count; i++) {
            if (env_entry_names_equal_wide(entries[i - 1], entries[i]))
                goto fail;
        }
    }

    if (total_wchars == SIZE_MAX || total_wchars + 1 > SIZE_MAX / sizeof(wchar_t))
        goto fail;
    wchar_t *block = (wchar_t *)calloc(total_wchars + 1, sizeof(wchar_t));
    if (!block)
        goto fail;
    wchar_t *out = block;
    for (int64_t i = 0; i < count; i++) {
        size_t wide_len = wcslen(entries[i]);
        memcpy(out, entries[i], wide_len * sizeof(wchar_t));
        out += wide_len + 1;
        free(entries[i]);
    }
    free(entries);
    return block;

fail:
    if (entries) {
        for (int64_t i = 0; i < count; i++)
            free(entries[i]);
    }
    free(entries);
    return NULL;
}

/// @brief Duplicate one NUL-terminated UTF-16 environment entry.
static wchar_t *process_wide_string_dup(const wchar_t *text) {
    if (!text)
        return NULL;
    size_t len = wcslen(text);
    if (len == SIZE_MAX || len + 1 > SIZE_MAX / sizeof(wchar_t))
        return NULL;
    wchar_t *copy = (wchar_t *)malloc((len + 1) * sizeof(wchar_t));
    if (copy)
        memcpy(copy, text, (len + 1) * sizeof(wchar_t));
    return copy;
}

/// @brief Count entries in a double-NUL-terminated Windows environment block.
static size_t process_wide_env_count(const wchar_t *block) {
    size_t count = 0;
    for (const wchar_t *entry = block; entry && *entry; entry += wcslen(entry) + 1)
        count++;
    return count;
}

/// @brief Test whether an overlay block replaces one inherited environment name.
static int process_wide_env_overlay_contains(const wchar_t *overlay, const wchar_t *entry) {
    for (const wchar_t *candidate = overlay; candidate && *candidate;
         candidate += wcslen(candidate) + 1) {
        if (env_entry_names_equal_wide(candidate, entry))
            return 1;
    }
    return 0;
}

/// @brief Build a sorted Windows environment block by overlaying the parent block.
/// @details Drive-current-directory pseudo entries beginning with '=' are
///          retained and cannot be named by the validated public overlay. Each
///          ordinary inherited name is removed when an overlay entry replaces
///          it under Windows' case-insensitive comparison rules.
static wchar_t *build_env_overlay_block_wide(void *env) {
    if (!env)
        return NULL;
    wchar_t *overlay_block = build_env_block_wide(env);
    if (!overlay_block)
        return NULL;
    LPWCH inherited_block = GetEnvironmentStringsW();
    if (!inherited_block) {
        free(overlay_block);
        return NULL;
    }

    size_t inherited_count = process_wide_env_count(inherited_block);
    size_t overlay_count = process_wide_env_count(overlay_block);
    if (inherited_count > SIZE_MAX - overlay_count ||
        inherited_count + overlay_count > SIZE_MAX / sizeof(wchar_t *)) {
        FreeEnvironmentStringsW(inherited_block);
        free(overlay_block);
        return NULL;
    }
    size_t capacity = inherited_count + overlay_count;
    wchar_t **entries = capacity ? (wchar_t **)calloc(capacity, sizeof(*entries)) : NULL;
    if (capacity && !entries) {
        FreeEnvironmentStringsW(inherited_block);
        free(overlay_block);
        return NULL;
    }

    size_t count = 0;
    for (const wchar_t *entry = inherited_block; *entry; entry += wcslen(entry) + 1) {
        if (*entry != L'=' && process_wide_env_overlay_contains(overlay_block, entry))
            continue;
        entries[count] = process_wide_string_dup(entry);
        if (!entries[count++])
            goto fail;
    }
    for (const wchar_t *entry = overlay_block; *entry; entry += wcslen(entry) + 1) {
        entries[count] = process_wide_string_dup(entry);
        if (!entries[count++])
            goto fail;
    }
    FreeEnvironmentStringsW(inherited_block);
    inherited_block = NULL;
    free(overlay_block);
    overlay_block = NULL;

    if (count > 1)
        qsort(entries, count, sizeof(*entries), compare_env_entry_wide);
    size_t total_wchars = 1;
    for (size_t i = 0; i < count; i++) {
        if (i > 0 && env_entry_names_equal_wide(entries[i - 1], entries[i]))
            goto fail;
        size_t len = wcslen(entries[i]);
        if (len == SIZE_MAX || total_wchars > SIZE_MAX - len - 1)
            goto fail;
        total_wchars += len + 1;
    }
    if (total_wchars == SIZE_MAX || total_wchars + 1 > SIZE_MAX / sizeof(wchar_t))
        goto fail;
    wchar_t *block = (wchar_t *)calloc(total_wchars + 1, sizeof(wchar_t));
    if (!block)
        goto fail;
    wchar_t *out = block;
    for (size_t i = 0; i < count; i++) {
        size_t len = wcslen(entries[i]);
        memcpy(out, entries[i], len * sizeof(wchar_t));
        out += len + 1;
        free(entries[i]);
    }
    free(entries);
    return block;

fail:
    if (inherited_block)
        FreeEnvironmentStringsW(inherited_block);
    free(overlay_block);
    if (entries) {
        for (size_t i = 0; i < count; i++)
            free(entries[i]);
    }
    free(entries);
    return NULL;
}

/// @brief Create a Windows pipe whose write end is inherited by the child.
/// @details Both ends are initially inheritable; the parent-side read handle is
///          then made noninheritable for stdout or stderr capture.
/// @param read_pipe Receives the parent-owned noninheritable read handle.
/// @param write_pipe Receives the inheritable child write handle.
/// @return 1 on success, otherwise 0 after closing and clearing created handles.
static int create_child_pipe(HANDLE *read_pipe, HANDLE *write_pipe) {
    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    if (!CreatePipe(read_pipe, write_pipe, &sa, 0))
        return 0;
    if (!SetHandleInformation(*read_pipe, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(*read_pipe);
        CloseHandle(*write_pipe);
        *read_pipe = NULL;
        *write_pipe = NULL;
        return 0;
    }
    return 1;
}

/// @brief Create a pipe whose READ end the child inherits (its stdin) and whose
///        WRITE end stays private to the parent. Mirror of create_child_pipe.
/// @param read_pipe Receives the inheritable child stdin handle.
/// @param write_pipe Receives the parent-owned noninheritable write handle.
/// @return 1 on success, otherwise 0 after closing and clearing created handles.
static int create_parent_write_pipe(HANDLE *read_pipe, HANDLE *write_pipe) {
    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    if (!CreatePipe(read_pipe, write_pipe, &sa, 0))
        return 0;
    if (!SetHandleInformation(*write_pipe, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(*read_pipe);
        CloseHandle(*write_pipe);
        *read_pipe = NULL;
        *write_pipe = NULL;
        return 0;
    }
    return 1;
}

/// @brief Close and clear an optional Win32 handle slot.
/// @param handle Address of the owned handle; NULL or an empty slot is ignored.
static void close_handle(HANDLE *handle) {
    if (handle && *handle) {
        CloseHandle(*handle);
        *handle = NULL;
    }
}

/// @brief Test whether either anonymous output pipe has readable bytes or closed.
static int process_windows_activity_ready(rt_process_impl *proc) {
    HANDLE pipes[2] = {proc ? proc->stdout_read : NULL, proc ? proc->stderr_read : NULL};
    volatile int *closed[2] = {proc ? &proc->stdout_activity_closed : NULL,
                               proc ? &proc->stderr_activity_closed : NULL};
    for (int i = 0; i < 2; ++i) {
        if (!pipes[i] || !closed[i] ||
            rt_atomic_load_i32(closed[i], __ATOMIC_ACQUIRE))
            continue;
        DWORD available = 0;
        if (!PeekNamedPipe(pipes[i], NULL, 0, NULL, &available, NULL)) {
            if (!rt_atomic_exchange_i32(closed[i], 1, __ATOMIC_ACQ_REL))
                return 1;
            continue;
        }
        if (available > 0)
            return 1;
    }
    return 0;
}

/// @brief Monitor Windows output readiness and process exit off the UI thread.
/// @details Anonymous pipe handles do not expose a waitable readability event,
///          so readiness probes are paced behind a 16 ms process/stop wait. One
///          notification is emitted per UI drain; the main thread explicitly
///          rearms the monitor afterward, preventing a readable pipe from
///          spinning either thread.
static DWORD WINAPI process_activity_monitor_main(LPVOID context) {
    rt_process_impl *proc = (rt_process_impl *)context;
    if (!proc)
        return 1;
    HANDLE waits[2] = {proc->activity_stop_event, proc->process};
    for (;;) {
        if (rt_atomic_load_i32(&proc->activity_monitor_stop, __ATOMIC_ACQUIRE))
            return 0;
        if (!rt_atomic_load_i32(&proc->activity_monitor_armed, __ATOMIC_ACQUIRE)) {
            HANDLE paused[2] = {proc->activity_stop_event, proc->activity_rearm_event};
            DWORD resumed = WaitForMultipleObjects(2, paused, FALSE, INFINITE);
            if (resumed == WAIT_OBJECT_0 || resumed == WAIT_FAILED)
                return 0;
            continue;
        }
        int exited = proc->process && WaitForSingleObject(proc->process, 0) == WAIT_OBJECT_0;
        if (exited || process_windows_activity_ready(proc)) {
            if (rt_atomic_exchange_i32(
                    &proc->activity_monitor_armed, 0, __ATOMIC_ACQ_REL))
                rt_activity_wake_signal(proc->activity_wake);
            if (exited)
                return 0;
            continue;
        }
        DWORD waited = WaitForMultipleObjects(2, waits, FALSE, 16);
        if (waited == WAIT_OBJECT_0 || waited == WAIT_FAILED)
            return 0;
        if (waited == WAIT_OBJECT_0 + 1) {
            if (rt_atomic_exchange_i32(
                    &proc->activity_monitor_armed, 0, __ATOMIC_ACQ_REL))
                rt_activity_wake_signal(proc->activity_wake);
            return 0;
        }
    }
}

static int process_activity_monitor_start(rt_process_impl *proc) {
    if (!proc || proc->activity_monitor_started)
        return proc && proc->activity_monitor_started;
    proc->activity_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    proc->activity_rearm_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!proc->activity_stop_event || !proc->activity_rearm_event) {
        close_handle(&proc->activity_stop_event);
        close_handle(&proc->activity_rearm_event);
        return 0;
    }
    rt_atomic_store_i32(&proc->activity_monitor_stop, 0, __ATOMIC_RELEASE);
    rt_atomic_store_i32(&proc->activity_monitor_armed, 1, __ATOMIC_RELEASE);
    proc->activity_thread = CreateThread(NULL, 0, process_activity_monitor_main, proc, 0, NULL);
    if (!proc->activity_thread) {
        close_handle(&proc->activity_stop_event);
        close_handle(&proc->activity_rearm_event);
        return 0;
    }
    proc->activity_monitor_started = 1;
    return 1;
}

static void process_activity_monitor_stop(rt_process_impl *proc) {
    if (!proc || !proc->activity_monitor_started)
        return;
    rt_atomic_store_i32(&proc->activity_monitor_stop, 1, __ATOMIC_RELEASE);
    SetEvent(proc->activity_stop_event);
    (void)WaitForSingleObject(proc->activity_thread, INFINITE);
    close_handle(&proc->activity_thread);
    close_handle(&proc->activity_stop_event);
    close_handle(&proc->activity_rearm_event);
    proc->activity_monitor_started = 0;
    rt_activity_wake_release(proc->activity_wake);
    proc->activity_wake = NULL;
}

static void process_activity_rearm(rt_process_impl *proc) {
    if (!proc || !proc->activity_monitor_started ||
        rt_atomic_load_i32(&proc->activity_monitor_stop, __ATOMIC_ACQUIRE))
        return;
    rt_atomic_store_i32(&proc->activity_monitor_armed, 1, __ATOMIC_RELEASE);
    SetEvent(proc->activity_rearm_event);
}

/// @brief Create a Windows Job Object that owns and kills a complete child tree.
/// @details The child is created suspended and assigned to this job before its
///          first instruction runs. Closing the last job handle therefore
///          cannot leave a compiler, game, debugger, or helper grandchild
///          behind after Studio stops the owning Process handle.
/// @return Owned job handle, or NULL when the tree guarantee cannot be created.
static HANDLE process_tree_job_create(void) {
    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (!job)
        return NULL;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
    memset(&limits, 0, sizeof(limits));
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        CloseHandle(job);
        return NULL;
    }
    return job;
}

/// @brief Background writer that keeps synchronous anonymous-pipe writes off the UI thread.
static DWORD WINAPI process_stdin_writer_main(LPVOID context) {
    rt_process_impl *proc = (rt_process_impl *)context;
    if (!proc)
        return 1;
    for (;;) {
        DWORD wait_result = WaitForSingleObject(proc->stdin_event, INFINITE);
        if (wait_result != WAIT_OBJECT_0)
            return 1;
        for (;;) {
            char chunk[4096];
            size_t chunk_len = 0;
            EnterCriticalSection(&proc->stdin_lock);
            if (proc->stdin_shutdown) {
                LeaveCriticalSection(&proc->stdin_lock);
                return 0;
            }
            chunk_len =
                proc->stdin_queue_len < sizeof(chunk) ? proc->stdin_queue_len : sizeof(chunk);
            if (chunk_len > 0)
                memcpy(chunk, proc->stdin_queue, chunk_len);
            LeaveCriticalSection(&proc->stdin_lock);
            if (chunk_len == 0)
                break;

            DWORD written = 0;
            BOOL ok = WriteFile(proc->stdin_write, chunk, (DWORD)chunk_len, &written, NULL);
            EnterCriticalSection(&proc->stdin_lock);
            if (!ok || written == 0 || written > proc->stdin_queue_len) {
                proc->stdin_failed = 1;
                proc->stdin_queue_len = 0;
                LeaveCriticalSection(&proc->stdin_lock);
                return 1;
            }
            size_t remaining = proc->stdin_queue_len - (size_t)written;
            if (remaining > 0)
                memmove(proc->stdin_queue, proc->stdin_queue + written, remaining);
            proc->stdin_queue_len = remaining;
            LeaveCriticalSection(&proc->stdin_lock);
        }
    }
}

/// @brief Start the bounded Windows stdin writer owned by one process handle.
static int process_stdin_writer_start(rt_process_impl *proc) {
    if (!proc || !proc->stdin_write)
        return 0;
    InitializeCriticalSection(&proc->stdin_lock);
    proc->stdin_lock_initialized = 1;
    proc->stdin_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!proc->stdin_event)
        return 0;
    proc->stdin_thread = CreateThread(NULL, 0, process_stdin_writer_main, proc, 0, NULL);
    return proc->stdin_thread != NULL;
}

/// @brief Stop, join, and release the Windows stdin writer.
static void process_stdin_writer_close(rt_process_impl *proc) {
    if (!proc)
        return;
    if (proc->stdin_lock_initialized) {
        EnterCriticalSection(&proc->stdin_lock);
        proc->stdin_shutdown = 1;
        LeaveCriticalSection(&proc->stdin_lock);
    }
    if (proc->stdin_event)
        SetEvent(proc->stdin_event);
    if (proc->stdin_thread) {
        (void)CancelSynchronousIo(proc->stdin_thread);
        DWORD waited = WaitForSingleObject(proc->stdin_thread, PROCESS_WINDOWS_TERMINATE_WAIT_MS);
        if (waited != WAIT_OBJECT_0)
            rt_trap("Process: bounded stdin writer shutdown failed");
    }
    close_handle(&proc->stdin_thread);
    close_handle(&proc->stdin_event);
    free(proc->stdin_queue);
    proc->stdin_queue = NULL;
    proc->stdin_queue_len = 0;
    proc->stdin_queue_cap = 0;
    if (proc->stdin_lock_initialized) {
        DeleteCriticalSection(&proc->stdin_lock);
        proc->stdin_lock_initialized = 0;
    }
}

/// @brief Nonblockingly enqueue bytes for the Windows stdin writer.
static int64_t process_stdin_enqueue(rt_process_impl *proc, const char *bytes, size_t len) {
    if (!proc || !bytes || len == 0 || !proc->stdin_lock_initialized || !proc->stdin_thread ||
        !proc->stdin_event)
        return len == 0 ? 0 : -1;

    EnterCriticalSection(&proc->stdin_lock);
    if (proc->stdin_shutdown || proc->stdin_failed || !proc->stdin_write) {
        LeaveCriticalSection(&proc->stdin_lock);
        return -1;
    }
    size_t available = PROCESS_WINDOWS_STDIN_QUEUE_MAX_SIZE - proc->stdin_queue_len;
    size_t accepted = len < available ? len : available;
    if (accepted == 0) {
        LeaveCriticalSection(&proc->stdin_lock);
        return -1;
    }
    size_t needed = proc->stdin_queue_len + accepted;
    if (needed > proc->stdin_queue_cap) {
        size_t capacity = proc->stdin_queue_cap ? proc->stdin_queue_cap : 4096u;
        while (capacity < needed && capacity < PROCESS_WINDOWS_STDIN_QUEUE_MAX_SIZE)
            capacity *= 2u;
        if (capacity > PROCESS_WINDOWS_STDIN_QUEUE_MAX_SIZE)
            capacity = PROCESS_WINDOWS_STDIN_QUEUE_MAX_SIZE;
        char *grown = (char *)realloc(proc->stdin_queue, capacity);
        if (!grown) {
            LeaveCriticalSection(&proc->stdin_lock);
            return -1;
        }
        proc->stdin_queue = grown;
        proc->stdin_queue_cap = capacity;
    }
    memcpy(proc->stdin_queue + proc->stdin_queue_len, bytes, accepted);
    proc->stdin_queue_len += accepted;
    LeaveCriticalSection(&proc->stdin_lock);
    SetEvent(proc->stdin_event);
    return (int64_t)accepted;
}

/// @brief Nonblockingly read one available chunk from a Windows pipe.
/// @details Uses PeekNamedPipe before the bounded read. EOF/API failure closes
///          the read handle; buffer allocation failure traps and also closes it.
///          Returning after one chunk lets process_drain alternate streams.
/// @param read_pipe Address of the owned parent read handle.
/// @param proc Owning process whose shared tagged queue receives the chunk.
/// @param stream PROCESS_OUTPUT_STDOUT or PROCESS_OUTPUT_STDERR.
/// @return 1 when bytes were captured, otherwise 0.
static int drain_pipe_once(HANDLE *read_pipe, rt_process_impl *proc, int stream) {
    if (!read_pipe || !*read_pipe)
        return 0;

    DWORD available = 0;
    if (!PeekNamedPipe(*read_pipe, NULL, 0, NULL, &available, NULL)) {
        rt_atomic_store_i32(
            output_stream_activity_closed(proc, stream), 1, __ATOMIC_RELEASE);
        if (!proc->activity_monitor_started)
            close_handle(read_pipe);
        return 0;
    }
    if (available == 0)
        return 0;

    char chunk[4096];
    DWORD to_read = available < sizeof(chunk) ? available : (DWORD)sizeof(chunk);
    DWORD read_count = 0;
    if (!ReadFile(*read_pipe, chunk, to_read, &read_count, NULL) || read_count == 0) {
        rt_atomic_store_i32(
            output_stream_activity_closed(proc, stream), 1, __ATOMIC_RELEASE);
        if (!proc->activity_monitor_started)
            close_handle(read_pipe);
        return 0;
    }
    ordered_output_append(proc, stream, chunk, read_count);
    return 1;
}

/// @brief Drain immediately available Windows stdout and stderr data.
/// @param proc Process object whose capture pipes feed its output buffers.
static void process_drain(rt_process_impl *proc) {
    if (!proc)
        return;
    for (;;) {
        int progressed = 0;
        progressed |= drain_pipe_once(&proc->stdout_read, proc, PROCESS_OUTPUT_STDOUT);
        progressed |= drain_pipe_once(&proc->stderr_read, proc, PROCESS_OUTPUT_STDERR);
        if (!progressed)
            break;
    }
    process_activity_rearm(proc);
}

/// @brief Poll or wait for a Windows child while continuously draining output.
/// @details A blocking wait uses short intervals to prevent full stdout/stderr
///          pipes from deadlocking the child. The first observed completion
///          stores its exit code and clears @c running; wait failure stores -1
///          and raises a runtime trap.
/// @param proc Process object to update.
/// @param wait Nonzero to wait until a terminal result; zero for one poll.
static void process_poll_internal(rt_process_impl *proc, int wait) {
    if (!proc || proc->destroyed)
        return;

    process_drain(proc);
    if (!proc->running || !proc->process)
        return;

    DWORD wait_result = WAIT_TIMEOUT;
    if (wait) {
        do {
            process_drain(proc);
            wait_result = WaitForSingleObject(proc->process, 10);
        } while (wait_result == WAIT_TIMEOUT);
    } else {
        wait_result = WaitForSingleObject(proc->process, 0);
    }
    if (wait_result == WAIT_OBJECT_0) {
        DWORD exit_code = 0;
        if (GetExitCodeProcess(proc->process, &exit_code)) {
            proc->exit_code = (int64_t)exit_code;
        } else {
            proc->exit_code = -1;
            rt_trap("Process: failed to query child exit code");
        }
        proc->running = 0;
        process_drain(proc);
    } else if (wait_result == WAIT_FAILED) {
        proc->exit_code = -1;
        process_drain(proc);
        rt_trap("Process: child wait failed");
    }
}

/// @brief Start a redirected Windows child process.
/// @details Validates all OS-bound strings, builds a literal quoted command
///          line, optionally replaces the complete environment with a sorted
///          UTF-16 block, and redirects all three standard streams through a
///          constrained handle allow-list. An empty cwd inherits the parent's
///          directory; a NULL environment inherits the parent's environment.
/// @param program Runtime executable path or lookup name; must be nonempty and
///        contain no embedded NUL.
/// @param args Optional Seq of literal argument strings.
/// @param cwd Optional working-directory string; NULL or empty means inherit.
/// @param env Optional Seq of NAME=VALUE strings replacing the environment;
///        NULL means inherit.
/// @return New GC-managed running process object, or NULL on validation,
///         allocation, conversion, pipe, startup, or CreateProcess failure.
static rt_process_impl *process_start_impl(
    rt_string program, void *args, rt_string cwd, void *env, int overlay_environment) {
    const char *program_text = NULL;
    const char *cwd_text = NULL;
    size_t program_len = 0;
    size_t cwd_len = 0;
    if (!program)
        return NULL;
    if (!process_string_cstr_view(program, &program_text, &program_len) || program_len == 0) {
        rt_trap("Process.Start: invalid program");
        return NULL;
    }
    if (cwd) {
        if (!process_string_cstr_view(cwd, &cwd_text, &cwd_len)) {
            rt_trap("Process.Start: invalid working directory");
            return NULL;
        }
        if (cwd_len == 0)
            cwd_text = NULL;
    }
    if (!process_validate_string_sequence(args, 0, "Process.Start: invalid argument"))
        return NULL;
    if (!process_validate_string_sequence(env, 1, "Process.Start: invalid environment entry"))
        return NULL;
    char *cmdline = build_cmdline(program_text, args);
    if (!cmdline) {
        rt_trap("Process.Start: command line allocation failed");
        return NULL;
    }

    wchar_t *wprogram = NULL;
    wchar_t *wcmdline = NULL;
    wchar_t *wcwd = NULL;
    if (!process_build_wide_start_strings(
            program_text, cmdline, cwd_text, &wprogram, &wcmdline, &wcwd)) {
        free(cmdline);
        rt_trap("Process.Start: UTF-8 to UTF-16 conversion failed");
        return NULL;
    }

    wchar_t *env_block =
        overlay_environment ? build_env_overlay_block_wide(env) : build_env_block_wide(env);
    if (env && !env_block) {
        free(wprogram);
        free(wcmdline);
        free(wcwd);
        free(cmdline);
        rt_trap("Process.Start: environment allocation failed");
        return NULL;
    }

    HANDLE stdout_read = NULL;
    HANDLE stdout_write = NULL;
    HANDLE stderr_read = NULL;
    HANDLE stderr_write = NULL;
    HANDLE stdin_read = NULL;
    HANDLE stdin_write = NULL;
    if (!create_child_pipe(&stdout_read, &stdout_write) ||
        !create_child_pipe(&stderr_read, &stderr_write) ||
        !create_parent_write_pipe(&stdin_read, &stdin_write)) {
        close_handle(&stdout_read);
        close_handle(&stdout_write);
        close_handle(&stderr_read);
        close_handle(&stderr_write);
        close_handle(&stdin_read);
        close_handle(&stdin_write);
        free(env_block);
        free(wprogram);
        free(wcmdline);
        free(wcwd);
        free(cmdline);
        return NULL;
    }

    HANDLE job = process_tree_job_create();
    if (!job) {
        close_handle(&stdout_read);
        close_handle(&stdout_write);
        close_handle(&stderr_read);
        close_handle(&stderr_write);
        close_handle(&stdin_read);
        close_handle(&stdin_write);
        free(env_block);
        free(wprogram);
        free(wcmdline);
        free(wcwd);
        free(cmdline);
        return NULL;
    }

    HANDLE inherited[3] = {stdin_read, stdout_write, stderr_write};
    process_win_startup_info si;
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));
    if (!process_win_startup_init(&si, inherited, 3)) {
        close_handle(&stdout_read);
        close_handle(&stdout_write);
        close_handle(&stderr_read);
        close_handle(&stderr_write);
        close_handle(&stdin_read);
        close_handle(&stdin_write);
        close_handle(&job);
        free(env_block);
        free(wprogram);
        free(wcmdline);
        free(wcwd);
        free(cmdline);
        return NULL;
    }
    si.startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.startup.StartupInfo.hStdInput = stdin_read;
    si.startup.StartupInfo.hStdOutput = stdout_write;
    si.startup.StartupInfo.hStdError = stderr_write;

    BOOL ok = CreateProcessW(wprogram,
                             wcmdline,
                             NULL,
                             NULL,
                             TRUE,
                             CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT |
                                 EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED,
                             env_block,
                             wcwd,
                             &si.startup.StartupInfo,
                             &pi);

    process_win_startup_destroy(&si);
    close_handle(&stdout_write);
    close_handle(&stderr_write);
    close_handle(&stdin_read);
    free(env_block);
    free(wprogram);
    free(wcmdline);
    free(wcwd);
    free(cmdline);

    if (ok) {
        BOOL assigned = AssignProcessToJobObject(job, pi.hProcess);
        if (!assigned || ResumeThread(pi.hThread) == (DWORD)-1) {
            DWORD ignored_exit_code = STILL_ACTIVE;
            if (assigned)
                (void)TerminateJobObject(job, 1);
            (void)rt_win32_terminate_process_bounded(
                pi.hProcess, 1, PROCESS_WINDOWS_TERMINATE_WAIT_MS, &ignored_exit_code);
            close_handle(&pi.hProcess);
            close_handle(&pi.hThread);
            ok = FALSE;
        }
    }

    if (!ok) {
        close_handle(&stdout_read);
        close_handle(&stderr_read);
        close_handle(&stdin_write);
        close_handle(&job);
        return NULL;
    }

    rt_process_impl *proc = process_alloc();
    if (!proc) {
        DWORD ignored_exit_code = STILL_ACTIVE;
        (void)TerminateJobObject(job, 1);
        (void)rt_win32_terminate_process_bounded(
            pi.hProcess, 1, PROCESS_WINDOWS_TERMINATE_WAIT_MS, &ignored_exit_code);
        close_handle(&stdout_read);
        close_handle(&stderr_read);
        close_handle(&stdin_write);
        close_handle(&pi.hProcess);
        close_handle(&pi.hThread);
        close_handle(&job);
        return NULL;
    }
    proc->started = 1;
    proc->running = 1;
    proc->process = pi.hProcess;
    proc->thread = pi.hThread;
    proc->job = job;
    proc->stdout_read = stdout_read;
    proc->stderr_read = stderr_read;
    proc->stdin_write = stdin_write;
    if (!process_stdin_writer_start(proc)) {
        process_close(proc);
        process_release_object(proc);
        return NULL;
    }
    return proc;
}

#else

/// @brief Write to a POSIX pipe without globally changing SIGPIPE behavior.
/// @details Temporarily blocks SIGPIPE for the current thread, performs one
///          write, consumes the generated SIGPIPE only when this write produced
///          EPIPE and no signal was already pending, then restores the previous
///          mask. This keeps Process.WriteStdin from mutating process-wide
///          signal disposition.
/// @param fd Pipe descriptor.
/// @param bytes Bytes to write.
/// @param len Maximum bytes to write in this syscall.
/// @return The write(2) result.
static ssize_t process_write_no_sigpipe(int fd, const char *bytes, size_t len) {
    sigset_t set;
    sigset_t old_set;
    sigset_t pending;
    int blocked = 0;
    int had_pending = 0;
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

/// @brief Write a coalescing control byte to the POSIX monitor self-pipe.
static void process_activity_control_signal(rt_process_impl *proc) {
    if (!proc || proc->activity_control_write < 0)
        return;
    const unsigned char byte = 1;
    ssize_t result;
    do {
        result = write(proc->activity_control_write, &byte, 1);
    } while (result < 0 && errno == EINTR);
    (void)result;
}

/// @brief Drain all rearm/stop bytes already queued for the activity monitor.
static void process_activity_control_drain(rt_process_impl *proc) {
    if (!proc || proc->activity_control_read < 0)
        return;
    unsigned char bytes[64];
    while (read(proc->activity_control_read, bytes, sizeof(bytes)) > 0) {
    }
}

/// @brief Block on POSIX output descriptors and emit one wake per UI drain.
static void *process_activity_monitor_main(void *context) {
    rt_process_impl *proc = (rt_process_impl *)context;
    if (!proc)
        return NULL;
    for (;;) {
        if (rt_atomic_load_i32(&proc->activity_monitor_stop, __ATOMIC_ACQUIRE))
            return NULL;
        int armed = rt_atomic_load_i32(&proc->activity_monitor_armed, __ATOMIC_ACQUIRE);
        struct pollfd descriptors[3];
        nfds_t count = 0;
        descriptors[count++] =
            (struct pollfd){.fd = proc->activity_control_read, .events = POLLIN, .revents = 0};
        if (armed && proc->stdout_fd >= 0 &&
            !rt_atomic_load_i32(&proc->stdout_activity_closed, __ATOMIC_ACQUIRE))
            descriptors[count++] =
                (struct pollfd){.fd = proc->stdout_fd, .events = POLLIN, .revents = 0};
        if (armed && proc->stderr_fd >= 0 &&
            !rt_atomic_load_i32(&proc->stderr_activity_closed, __ATOMIC_ACQUIRE))
            descriptors[count++] =
                (struct pollfd){.fd = proc->stderr_fd, .events = POLLIN, .revents = 0};
        int result;
        do {
            result = poll(descriptors, count, -1);
        } while (result < 0 && errno == EINTR);
        if (result < 0)
            return NULL;
        if (descriptors[0].revents != 0)
            process_activity_control_drain(proc);
        if (rt_atomic_load_i32(&proc->activity_monitor_stop, __ATOMIC_ACQUIRE))
            return NULL;
        int activity = 0;
        for (nfds_t i = 1; i < count; ++i) {
            if (descriptors[i].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) {
                activity = 1;
                break;
            }
        }
        if (activity && rt_atomic_exchange_i32(
                            &proc->activity_monitor_armed, 0, __ATOMIC_ACQ_REL))
            rt_activity_wake_signal(proc->activity_wake);
    }
}

static int process_activity_monitor_start(rt_process_impl *proc) {
    if (!proc || proc->activity_monitor_started)
        return proc && proc->activity_monitor_started;
    int control[2] = {-1, -1};
    if (pipe(control) != 0)
        return 0;
    for (int i = 0; i < 2; ++i) {
        int descriptor_flags = fcntl(control[i], F_GETFD, 0);
        int status_flags = fcntl(control[i], F_GETFL, 0);
        if (descriptor_flags < 0 || status_flags < 0 ||
            fcntl(control[i], F_SETFD, descriptor_flags | FD_CLOEXEC) != 0 ||
            fcntl(control[i], F_SETFL, status_flags | O_NONBLOCK) != 0) {
            close(control[0]);
            close(control[1]);
            return 0;
        }
    }
    proc->activity_control_read = control[0];
    proc->activity_control_write = control[1];
    rt_atomic_store_i32(&proc->activity_monitor_stop, 0, __ATOMIC_RELEASE);
    rt_atomic_store_i32(&proc->activity_monitor_armed, 1, __ATOMIC_RELEASE);
    if (pthread_create(&proc->activity_thread, NULL, process_activity_monitor_main, proc) != 0) {
        close_fd(&proc->activity_control_read);
        close_fd(&proc->activity_control_write);
        return 0;
    }
    proc->activity_monitor_started = 1;
    return 1;
}

static void process_activity_monitor_stop(rt_process_impl *proc) {
    if (!proc || !proc->activity_monitor_started)
        return;
    rt_atomic_store_i32(&proc->activity_monitor_stop, 1, __ATOMIC_RELEASE);
    process_activity_control_signal(proc);
    (void)pthread_join(proc->activity_thread, NULL);
    close_fd(&proc->activity_control_read);
    close_fd(&proc->activity_control_write);
    proc->activity_monitor_started = 0;
    rt_activity_wake_release(proc->activity_wake);
    proc->activity_wake = NULL;
}

static void process_activity_rearm(rt_process_impl *proc) {
    if (!proc || !proc->activity_monitor_started ||
        rt_atomic_load_i32(&proc->activity_monitor_stop, __ATOMIC_ACQUIRE))
        return;
    rt_atomic_store_i32(&proc->activity_monitor_armed, 1, __ATOMIC_RELEASE);
    process_activity_control_signal(proc);
}

/// @brief Create a POSIX pipe with close-on-exec set on both descriptors.
/// @details The child duplicates the pipe ends it needs onto stdin/stdout/stderr
///          before exec. Setting FD_CLOEXEC on the original descriptors prevents
///          the parent-side copies and any forgotten pipe ends from surviving
///          into grandchildren.
/// @param pipefd Receives read/write descriptors.
/// @return 0 on success, -1 on failure.
static int process_pipe_cloexec(int pipefd[2]) {
    if (pipe(pipefd) != 0)
        return -1;
#if defined(FD_CLOEXEC)
    for (int i = 0; i < 2; i++) {
        int flags = fcntl(pipefd[i], F_GETFD, 0);
        if (flags < 0 || fcntl(pipefd[i], F_SETFD, flags | FD_CLOEXEC) < 0) {
            close_fd(&pipefd[0]);
            close_fd(&pipefd[1]);
            return -1;
        }
    }
#endif
    return 0;
}

/// @brief Best-effort enable O_NONBLOCK on a POSIX descriptor.
/// @param fd Open descriptor whose existing file status flags are preserved.
static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/// @brief Read one available chunk from a nonblocking descriptor.
/// @details Retries interrupted reads and leaves the descriptor open on EAGAIN.
///          While an activity monitor owns a stable descriptor set, EOF/error
///          closure is deferred until monitor shutdown. Returning after one chunk lets the caller
///          alternate stdout and stderr. Allocation failure raises a trap.
/// @param fd Address of the owned stdout or stderr descriptor.
/// @param proc Owning process whose shared tagged queue receives the chunk.
/// @param stream PROCESS_OUTPUT_STDOUT or PROCESS_OUTPUT_STDERR.
/// @return 1 when bytes were captured, otherwise 0.
static int drain_fd_once(int *fd, rt_process_impl *proc, int stream) {
    if (!fd || *fd < 0)
        return 0;

    for (;;) {
        char chunk[4096];
        ssize_t count = read(*fd, chunk, sizeof(chunk));
        if (count > 0) {
            ordered_output_append(proc, stream, chunk, (size_t)count);
            return 1;
        }
        if (count == 0) {
            rt_atomic_store_i32(
                output_stream_activity_closed(proc, stream), 1, __ATOMIC_RELEASE);
            if (!proc->activity_monitor_started)
                close_fd(fd);
            return 0;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        rt_atomic_store_i32(
            output_stream_activity_closed(proc, stream), 1, __ATOMIC_RELEASE);
        if (!proc->activity_monitor_started)
            close_fd(fd);
        return 0;
    }
}

/// @brief Drain immediately available POSIX stdout and stderr data.
/// @param proc Process object whose descriptors feed its output buffers.
static void process_drain(rt_process_impl *proc) {
    if (!proc)
        return;
    for (;;) {
        int progressed = 0;
        progressed |= drain_fd_once(&proc->stdout_fd, proc, PROCESS_OUTPUT_STDOUT);
        progressed |= drain_fd_once(&proc->stderr_fd, proc, PROCESS_OUTPUT_STDERR);
        if (!progressed)
            break;
    }
    process_activity_rearm(proc);
}

/// @brief Normalize a POSIX wait status for the Process API.
/// @param status Raw status produced by waitpid.
/// @return Exit code for normal completion, the negated signal number for
///         signalled termination, or -1 for any other wait state.
static int64_t decode_exit_status(int status) {
    if (WIFEXITED(status))
        return (int64_t)WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return -(int64_t)WTERMSIG(status);
    return -1;
}

/// @brief Poll or wait for a POSIX child while draining redirected output.
/// @details Uses WNOHANG even for the blocking mode, interleaving 10 ms waits
///          with pipe drains so a child cannot block forever on full output
///          pipes. Completion is reaped once and its normalized status is
///          retained. ECHILD marks the process stopped with status -1.
/// @param proc Process object to update.
/// @param wait Nonzero to continue until a terminal waitpid result; zero to poll
///        once.
static void process_poll_internal(rt_process_impl *proc, int wait) {
    if (!proc || proc->destroyed)
        return;

    process_drain(proc);
    if (!proc->running || proc->pid <= 0)
        return;

    int status = 0;
    pid_t result;
    for (;;) {
        do {
            result = waitpid(proc->pid, &status, WNOHANG);
        } while (result < 0 && errno == EINTR);
        if (!wait || result != 0)
            break;
        process_drain(proc);
        {
            struct timespec delay;
            delay.tv_sec = 0;
            delay.tv_nsec = 10000000L;
            (void)nanosleep(&delay, NULL);
        }
    }

    if (result == proc->pid) {
        proc->exit_code = decode_exit_status(status);
        proc->running = 0;
        process_drain(proc);
    } else if (result < 0 && errno == ECHILD) {
        proc->exit_code = -1;
        proc->running = 0;
        process_drain(proc);
    }
}

/// @brief Test whether the dedicated POSIX process group still has members.
/// @param proc Process handle whose positive child pid is also its group id.
/// @return Nonzero while the group exists (including an inaccessible group).
static int process_tree_exists(const rt_process_impl *proc) {
    if (!proc || proc->pid <= 0)
        return 0;
    if (kill(-proc->pid, 0) == 0)
        return 1;
    return errno == EPERM;
}

/// @brief Send one signal to the complete dedicated POSIX process group.
/// @details The spawn attributes make the direct child the group leader before
///          it executes user code, removing the parent-side setpgid race.
/// @param proc Process handle whose pid is the owned group id.
/// @param signal_number Signal to deliver.
/// @return Nonzero when the group accepted the signal.
static int process_signal_tree(const rt_process_impl *proc, int signal_number) {
    if (!proc || proc->pid <= 0)
        return 0;
    return kill(-proc->pid, signal_number) == 0;
}

/// @brief Wait briefly for a POSIX process tree to exit after SIGTERM.
/// @details Destruction is bounded, but every descendant gets a short cleanup
///          window before escalation. The loop drains output and reaps the
///          direct child while waiting for the process group to disappear.
/// @param proc Process handle to poll.
/// @param grace_ms Maximum grace interval in milliseconds.
static void process_wait_after_sigterm(rt_process_impl *proc, int grace_ms) {
    if (!proc || grace_ms <= 0)
        return;
    int elapsed = 0;
    while (process_tree_exists(proc) && elapsed < grace_ms) {
        process_poll_internal(proc, 0);
        if (!process_tree_exists(proc))
            break;
        {
            struct timespec delay;
            delay.tv_sec = 0;
            delay.tv_nsec = 10000000L;
            (void)nanosleep(&delay, NULL);
        }
        elapsed += 10;
    }
}

/// @brief Look up a key in a NULL-terminated KEY=value environment vector.
/// @param envp Optional environment vector.
/// @param key Non-NULL exact key name without '='.
/// @return Borrowed pointer to the matching value bytes, or NULL when absent.
static const char *process_env_lookup(char *const *envp, const char *key) {
    if (!envp)
        return NULL;
    size_t klen = strlen(key);
    for (size_t i = 0; envp[i]; ++i) {
        if (strncmp(envp[i], key, klen) == 0 && envp[i][klen] == '=')
            return envp[i] + klen + 1;
    }
    return NULL;
}

/// @brief Resolve a program name to a full path for posix_spawn, PATH-searching
///        bare names so they resolve the same whether or not an explicit
///        environment is supplied (VDOC-213).
/// @details A name containing '/' is used verbatim. A bare name is searched
///          through the PATH taken from @p envp when it provides one, else the
///          inherited PATH (mirroring what `execvp` does, and what the PTY
///          backend now does with the supplied environment). If nothing is
///          found the name is left unchanged so the spawn fails with ENOENT
///          just as `execvp` would. Returns 0 only when a path would overflow
///          @p out.
/// @param program Borrowed nonempty executable path or bare name.
/// @param envp Optional explicit child environment used as the first PATH source.
/// @param out Destination buffer for a resolved path or unchanged program name.
/// @param out_size Writable size of @p out, including the terminator.
/// @return 1 when a complete result was written, or 0 when it would exceed the
///         destination capacity.
static int process_resolve_program_path(const char *program,
                                        char *const *envp,
                                        char *out,
                                        size_t out_size) {
    if (strchr(program, '/') != NULL) {
        if (strlen(program) >= out_size)
            return 0;
        strcpy(out, program);
        return 1;
    }
    const char *path = process_env_lookup(envp, "PATH");
    if (!path)
        path = getenv("PATH");
    if (!path || !*path)
        path = "/usr/bin:/bin";

    const char *seg = path;
    while (*seg) {
        const char *colon = strchr(seg, ':');
        size_t dirlen = colon ? (size_t)(colon - seg) : strlen(seg);
        // An empty entry means the current directory.
        const char *dir = (dirlen == 0) ? "." : seg;
        size_t effective_dirlen = (dirlen == 0) ? 1 : dirlen;
        char candidate[PATH_MAX];
        if (effective_dirlen + 1 + strlen(program) + 1 <= sizeof(candidate)) {
            memcpy(candidate, dir, effective_dirlen);
            candidate[effective_dirlen] = '/';
            strcpy(candidate + effective_dirlen + 1, program);
            if (access(candidate, X_OK) == 0) {
                if (strlen(candidate) >= out_size)
                    return 0;
                strcpy(out, candidate);
                return 1;
            }
        }
        if (!colon)
            break;
        seg = colon + 1;
    }
    // Not found: leave the name unchanged so posix_spawn fails like execvp.
    if (strlen(program) >= out_size)
        return 0;
    strcpy(out, program);
    return 1;
}

/// @brief Return the nonempty name length of one POSIX `NAME=value` entry.
static size_t process_env_name_length(const char *entry) {
    if (!entry)
        return 0;
    const char *equals = strchr(entry, '=');
    return equals && equals != entry ? (size_t)(equals - entry) : 0;
}

/// @brief Compare POSIX environment names using native case-sensitive rules.
static int process_env_names_equal(const char *left, const char *right) {
    size_t left_len = process_env_name_length(left);
    size_t right_len = process_env_name_length(right);
    return left_len > 0 && left_len == right_len && memcmp(left, right, left_len) == 0;
}

/// @brief Build an environment vector by overlaying retained runtime strings.
/// @details Inherited pointers remain borrowed from `environ` until spawn; the
///          aggregate owns only the overlay string references and pointer array.
static process_string_vector build_env_overlay_vector(void *env) {
    process_string_vector result;
    memset(&result, 0, sizeof(result));
    process_string_vector overlay = build_string_vector(NULL, env, 0);
    if (!overlay.values)
        return result;

    size_t overlay_count = 0;
    while (overlay.values[overlay_count])
        overlay_count++;
    for (size_t i = 0; i < overlay_count; i++) {
        for (size_t j = i + 1; j < overlay_count; j++) {
            if (process_env_names_equal(overlay.values[i], overlay.values[j])) {
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
    result.values = (char **)calloc(inherited_count + overlay_count + 1, sizeof(*result.values));
    if (!result.values) {
        free_string_vector(&overlay);
        return result;
    }

    size_t at = 0;
    for (size_t i = 0; i < inherited_count; i++) {
        int shadowed = 0;
        for (size_t j = 0; j < overlay_count; j++) {
            if (process_env_names_equal(environ[i], overlay.values[j])) {
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
    overlay.values = NULL;
    return result;
}

/// @brief Start a redirected POSIX child process.
/// @details Validates OS-bound strings, constructs retained argv/envp vectors,
///          creates close-on-exec pipes for all standard streams, optionally
///          installs a child working directory, resolves bare executable names,
///          and invokes posix_spawn. Parent pipe ends are nonblocking. A non-NULL
///          environment sequence replaces the complete inherited environment.
/// @param program Runtime executable path or lookup name; must be nonempty and
///        contain no embedded NUL.
/// @param args Optional Seq of literal argument strings.
/// @param cwd Optional working-directory string; NULL or empty means inherit.
/// @param env Optional Seq of NAME=VALUE strings replacing the environment;
///        NULL means inherit.
/// @return New GC-managed running process object, or NULL on validation,
///         allocation, path, pipe, spawn-action, or posix_spawn failure.
static rt_process_impl *process_start_impl(
    rt_string program, void *args, rt_string cwd, void *env, int overlay_environment) {
    const char *program_text = NULL;
    const char *cwd_text = NULL;
    size_t program_len = 0;
    size_t cwd_len = 0;
    if (!program)
        return NULL;
    if (!process_string_cstr_view(program, &program_text, &program_len) || program_len == 0) {
        rt_trap("Process.Start: invalid program");
        return NULL;
    }
    if (cwd) {
        if (!process_string_cstr_view(cwd, &cwd_text, &cwd_len)) {
            rt_trap("Process.Start: invalid working directory");
            return NULL;
        }
        if (cwd_len == 0)
            cwd_text = NULL;
    }
    if (!process_validate_string_sequence(args, 0, "Process.Start: invalid argument"))
        return NULL;
    if (!process_validate_string_sequence(env, 1, "Process.Start: invalid environment entry"))
        return NULL;
    process_string_vector argv = build_string_vector(program_text, args, 1);
    if (!argv.values) {
        rt_trap("Process.Start: argv allocation failed");
        return NULL;
    }

    process_string_vector envp;
    memset(&envp, 0, sizeof(envp));
    if (env)
        envp =
            overlay_environment ? build_env_overlay_vector(env) : build_string_vector(NULL, env, 0);
    if (env && !envp.values) {
        free_string_vector(&argv);
        rt_trap("Process.Start: environment allocation failed");
        return NULL;
    }

    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    int stdin_pipe[2] = {-1, -1};
    if (process_pipe_cloexec(stdout_pipe) != 0 || process_pipe_cloexec(stderr_pipe) != 0 ||
        process_pipe_cloexec(stdin_pipe) != 0) {
        close_fd(&stdout_pipe[0]);
        close_fd(&stdout_pipe[1]);
        close_fd(&stderr_pipe[0]);
        close_fd(&stderr_pipe[1]);
        close_fd(&stdin_pipe[0]);
        close_fd(&stdin_pipe[1]);
        free_string_vector(&envp);
        free_string_vector(&argv);
        return NULL;
    }

    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        close_fd(&stdout_pipe[0]);
        close_fd(&stdout_pipe[1]);
        close_fd(&stderr_pipe[0]);
        close_fd(&stderr_pipe[1]);
        close_fd(&stdin_pipe[0]);
        close_fd(&stdin_pipe[1]);
        free_string_vector(&envp);
        free_string_vector(&argv);
        return NULL;
    }

    int spawn_setup_rc = 0;
    spawn_setup_rc |= posix_spawn_file_actions_addclose(&actions, stdout_pipe[0]);
    spawn_setup_rc |= posix_spawn_file_actions_addclose(&actions, stderr_pipe[0]);
    spawn_setup_rc |= posix_spawn_file_actions_addclose(&actions, stdin_pipe[1]);
    spawn_setup_rc |= posix_spawn_file_actions_adddup2(&actions, stdout_pipe[1], STDOUT_FILENO);
    spawn_setup_rc |= posix_spawn_file_actions_adddup2(&actions, stderr_pipe[1], STDERR_FILENO);
    spawn_setup_rc |= posix_spawn_file_actions_adddup2(&actions, stdin_pipe[0], STDIN_FILENO);
    spawn_setup_rc |= posix_spawn_file_actions_addclose(&actions, stdout_pipe[1]);
    spawn_setup_rc |= posix_spawn_file_actions_addclose(&actions, stderr_pipe[1]);
    spawn_setup_rc |= posix_spawn_file_actions_addclose(&actions, stdin_pipe[0]);
    if (cwd_text) {
#if RT_PLATFORM_MACOS
        spawn_setup_rc |= posix_spawn_file_actions_addchdir(&actions, cwd_text);
#else
        spawn_setup_rc |= posix_spawn_file_actions_addchdir_np(&actions, cwd_text);
#endif
    }
    if (spawn_setup_rc != 0) {
        posix_spawn_file_actions_destroy(&actions);
        close_fd(&stdout_pipe[0]);
        close_fd(&stdout_pipe[1]);
        close_fd(&stderr_pipe[0]);
        close_fd(&stderr_pipe[1]);
        close_fd(&stdin_pipe[0]);
        close_fd(&stdin_pipe[1]);
        free_string_vector(&envp);
        free_string_vector(&argv);
        return NULL;
    }

    // PATH-search a bare program name (using the supplied environment's PATH
    // when present) so Start/StartWithEnv resolve names the same way, and the
    // same way the PTY backend does (VDOC-213). posix_spawn itself does no PATH
    // search, so resolve to a full path first.
    char resolved_program[PATH_MAX];
    if (!process_resolve_program_path(
            program_text, envp.values, resolved_program, sizeof(resolved_program))) {
        posix_spawn_file_actions_destroy(&actions);
        close_fd(&stdout_pipe[0]);
        close_fd(&stdout_pipe[1]);
        close_fd(&stderr_pipe[0]);
        close_fd(&stderr_pipe[1]);
        close_fd(&stdin_pipe[0]);
        close_fd(&stdin_pipe[1]);
        free_string_vector(&envp);
        free_string_vector(&argv);
        rt_trap("Process.Start: program path too long");
        return NULL;
    }

    pid_t pid = -1;
    posix_spawnattr_t attributes;
    short spawn_flags = 0;
    int attr_rc = posix_spawnattr_init(&attributes);
    int attr_initialized = attr_rc == 0;
    if (attr_rc == 0)
        attr_rc = posix_spawnattr_setpgroup(&attributes, 0);
    if (attr_rc == 0)
        attr_rc = posix_spawnattr_getflags(&attributes, &spawn_flags);
    if (attr_rc == 0)
        attr_rc =
            posix_spawnattr_setflags(&attributes, (short)(spawn_flags | POSIX_SPAWN_SETPGROUP));
    if (attr_rc != 0) {
        if (attr_initialized)
            posix_spawnattr_destroy(&attributes);
        posix_spawn_file_actions_destroy(&actions);
        close_fd(&stdout_pipe[0]);
        close_fd(&stdout_pipe[1]);
        close_fd(&stderr_pipe[0]);
        close_fd(&stderr_pipe[1]);
        close_fd(&stdin_pipe[0]);
        close_fd(&stdin_pipe[1]);
        free_string_vector(&envp);
        free_string_vector(&argv);
        return NULL;
    }
    int spawn_rc = posix_spawn(&pid,
                               resolved_program,
                               &actions,
                               &attributes,
                               argv.values,
                               envp.values ? envp.values : environ);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    if (spawn_rc != 0) {
        close_fd(&stdout_pipe[0]);
        close_fd(&stdout_pipe[1]);
        close_fd(&stderr_pipe[0]);
        close_fd(&stderr_pipe[1]);
        close_fd(&stdin_pipe[0]);
        close_fd(&stdin_pipe[1]);
        free_string_vector(&envp);
        free_string_vector(&argv);
        return NULL;
    }

    close_fd(&stdout_pipe[1]);
    close_fd(&stderr_pipe[1]);
    close_fd(&stdin_pipe[0]);
    set_nonblocking(stdout_pipe[0]);
    set_nonblocking(stderr_pipe[0]);
    set_nonblocking(stdin_pipe[1]);
    free_string_vector(&envp);
    free_string_vector(&argv);

    rt_process_impl *proc = process_alloc();
    if (!proc) {
        (void)kill(-pid, SIGKILL);
        (void)waitpid(pid, NULL, 0);
        close_fd(&stdout_pipe[0]);
        close_fd(&stderr_pipe[0]);
        close_fd(&stdin_pipe[1]);
        return NULL;
    }
    proc->started = 1;
    proc->running = 1;
    proc->pid = pid;
    proc->stdout_fd = stdout_pipe[0];
    proc->stderr_fd = stderr_pipe[0];
    proc->stdin_fd = stdin_pipe[1];
    return proc;
}

#endif

/// @brief Terminate if necessary and release every resource owned by a process.
/// @details Idempotently marks the handle destroyed. Windows terminates a live
///          child with status 1 and waits. POSIX sends SIGTERM, drains and polls
///          for up to 500 ms, then escalates to SIGKILL and reaps. Any remaining
///          stream bytes are drained before both buffers and all handles are
///          released.
/// @param proc Process object to close; NULL or an already destroyed object is
///        ignored.
static void process_close(rt_process_impl *proc) {
    if (!proc || proc->destroyed)
        return;

    process_activity_monitor_stop(proc);

#if defined(_WIN32)
    process_poll_internal(proc, 0);
    if (proc->job && !TerminateJobObject(proc->job, 1))
        rt_trap("Process: child-tree termination failed");
    if (proc->running && proc->process) {
        DWORD exit_code = STILL_ACTIVE;
        if (rt_win32_terminate_process_bounded(
                proc->process, 1, PROCESS_WINDOWS_TERMINATE_WAIT_MS, &exit_code)) {
            proc->running = 0;
            proc->exit_code = (int64_t)exit_code;
        } else {
            rt_trap("Process: bounded child termination failed");
        }
    }
    process_drain(proc);
    process_stdin_writer_close(proc);
    close_handle(&proc->stdout_read);
    close_handle(&proc->stderr_read);
    close_handle(&proc->stdin_write);
    close_handle(&proc->thread);
    close_handle(&proc->process);
    close_handle(&proc->job);
#else
    if (proc->pid > 0) {
        (void)process_signal_tree(proc, SIGTERM);
        process_wait_after_sigterm(proc, 500);
        if (process_tree_exists(proc))
            (void)process_signal_tree(proc, SIGKILL);
        if (proc->running)
            process_poll_internal(proc, 1);
    } else {
        process_poll_internal(proc, 0);
    }
    process_drain(proc);
    close_fd(&proc->stdout_fd);
    close_fd(&proc->stderr_fd);
    close_fd(&proc->stdin_fd);
#endif

    ordered_output_free(proc);
    proc->running = 0;
    proc->started = 0;
    proc->destroyed = 1;
}

/// @brief GC finalizer for a process object's operating-system resources.
/// @param obj Process implementation object being finalized.
static void process_finalize(void *obj) {
    process_close((rt_process_impl *)obj);
}

/// @brief Start a child with inherited working directory and environment.
/// @param program Executable path or lookup name; must be nonempty and contain
///        no embedded NUL byte.
/// @param args Optional Seq of literal argument strings.
/// @return New GC-managed process handle with redirected stdin/stdout/stderr, or
///         NULL when validation or startup fails.
void *rt_process_start(rt_string program, void *args) {
    return rt_process_start_with_env(program, args, NULL, NULL);
}

/// @brief Start a child in an optional working directory.
/// @param program Executable path or lookup name; must be nonempty and contain
///        no embedded NUL byte.
/// @param args Optional Seq of literal argument strings.
/// @param cwd Optional working directory; NULL or empty means inherit.
/// @return New GC-managed process handle with redirected stdin/stdout/stderr, or
///         NULL when validation or startup fails.
void *rt_process_start_in(rt_string program, void *args, rt_string cwd) {
    return rt_process_start_with_env(program, args, cwd, NULL);
}

/// @brief Start a child with optional cwd and complete environment replacement.
/// @details A non-NULL @p env replaces rather than augments the inherited
///          environment. Arguments and environment entries cross directly to
///          the OS without shell interpretation.
/// @param program Executable path or lookup name; must be nonempty and contain
///        no embedded NUL byte.
/// @param args Optional Seq of literal argument strings.
/// @param cwd Optional working directory; NULL or empty means inherit.
/// @param env Optional Seq of nonempty NAME=VALUE strings; NULL means inherit.
/// @return New GC-managed process handle with redirected stdin/stdout/stderr, or
///         NULL when validation or startup fails.
void *rt_process_start_with_env(rt_string program, void *args, rt_string cwd, void *env) {
    return process_start_impl(program, args, cwd, env, 0);
}

/// @brief Start a child with inherited environment plus validated overrides.
/// @details Unlike StartWithEnv, this additive entry point preserves every
///          inherited variable not named by the overlay. The platform adapter
///          applies native environment-name comparison rules and constructs a
///          complete child block/vector before spawn.
void *rt_process_start_with_env_overlay(rt_string program,
                                        void *args,
                                        rt_string cwd,
                                        void *overlay) {
    return process_start_impl(program, args, cwd, overlay, 1);
}

/// @brief Test whether an object is an initialized, undestroyed process handle.
/// @details A normally exited process remains a valid handle until destroyed.
/// @param handle Candidate opaque runtime object.
/// @return 1 for a started process object whose resources have not been
///         destroyed, otherwise 0.
int64_t rt_process_is_valid(void *handle) {
    rt_process_impl *proc = process_checked(handle);
    return proc && proc->started && !proc->destroyed ? 1 : 0;
}

/// @brief Attach a lifetime-safe output/exit wake target to a live Process.
int64_t rt_process_set_activity_wake(void *handle, rt_activity_wake_target *target) {
    rt_process_impl *proc = process_checked(handle);
    if (!proc || !target || !proc->started || proc->destroyed)
        return 0;
    process_activity_monitor_stop(proc);
    proc->activity_wake = rt_activity_wake_retain(target);
    if (!proc->activity_wake || !process_activity_monitor_start(proc)) {
        rt_activity_wake_release(proc->activity_wake);
        proc->activity_wake = NULL;
        return 0;
    }
    return 1;
}

/// @brief Nonblockingly collect output and check for child completion.
/// @details Reaps a newly exited POSIX child or records a completed Windows
///          process exactly once, preserving its exit status for later queries.
/// @param handle Candidate process handle.
/// @return 1 while the validated process is still running, otherwise 0.
int64_t rt_process_poll(void *handle) {
    rt_process_impl *proc = process_checked(handle);
    if (!proc || !proc->started || proc->destroyed)
        return 0;
    process_poll_internal(proc, 0);
    return proc->running ? 1 : 0;
}

/// @brief Poll and report whether a child process is still running.
/// @param handle Candidate process handle.
/// @return 1 while running, otherwise 0 for exited or invalid handles.
int64_t rt_process_is_running(void *handle) {
    return rt_process_poll(handle);
}

/// @brief Consume stdout bytes buffered or immediately available.
/// @details The process remains running; later output is returned by later
///          reads. If more than 16 MiB accumulated since the previous read, the
///          retained prefix is returned after raising an output-truncated trap.
/// @param handle Candidate process handle.
/// @return Newly allocated string containing incremental stdout, or an empty
///         string for no bytes or an invalid/destroyed handle.
rt_string rt_process_read_stdout(void *handle) {
    rt_process_impl *proc = process_checked(handle);
    if (!proc || !proc->started || proc->destroyed)
        return empty_string();
    process_drain(proc);
    return stream_output_take(proc, PROCESS_OUTPUT_STDOUT);
}

/// @brief Consume stdout into a structured truncation-aware result.
/// @details Does not trap when the 16 MiB buffer discarded bytes. Instead the
///          returned map contains @c text and Boolean @c truncated entries.
/// @param handle Candidate process handle.
/// @return Caller-owned result map, including an empty nontruncated result for
///         an invalid handle, or NULL when map allocation fails.
void *rt_process_read_stdout_result(void *handle) {
    rt_process_impl *proc = process_checked(handle);
    if (!proc || !proc->started || proc->destroyed)
        return stream_output_take_result(NULL, PROCESS_OUTPUT_STDOUT);
    process_drain(proc);
    return stream_output_take_result(proc, PROCESS_OUTPUT_STDOUT);
}

/// @brief Consume stderr bytes buffered or immediately available.
/// @details The process remains running; later output is returned by later
///          reads. If more than 16 MiB accumulated since the previous read, the
///          retained prefix is returned after raising an output-truncated trap.
/// @param handle Candidate process handle.
/// @return Newly allocated string containing incremental stderr, or an empty
///         string for no bytes or an invalid/destroyed handle.
rt_string rt_process_read_stderr(void *handle) {
    rt_process_impl *proc = process_checked(handle);
    if (!proc || !proc->started || proc->destroyed)
        return empty_string();
    process_drain(proc);
    return stream_output_take(proc, PROCESS_OUTPUT_STDERR);
}

/// @brief Consume stderr into a structured truncation-aware result.
/// @details Does not trap when the 16 MiB buffer discarded bytes. Instead the
///          returned map contains @c text and Boolean @c truncated entries.
/// @param handle Candidate process handle.
/// @return Caller-owned result map, including an empty nontruncated result for
///         an invalid handle, or NULL when map allocation fails.
void *rt_process_read_stderr_result(void *handle) {
    rt_process_impl *proc = process_checked(handle);
    if (!proc || !proc->started || proc->destroyed)
        return stream_output_take_result(NULL, PROCESS_OUTPUT_STDERR);
    process_drain(proc);
    return stream_output_take_result(proc, PROCESS_OUTPUT_STDERR);
}

/// @brief Consume capture-ordered stdout/stderr chunks without trapping.
/// @details The returned map contains an ordered `chunks` sequence. Each entry
///          has integer `sequence`, string `stream` (`stdout` or `stderr`), and
///          string `text`; top-level `truncated` reports combined-queue loss.
///          Corresponding legacy per-stream buffers are consumed too, so a
///          caller cannot accidentally receive the same bytes twice.
/// @param handle Candidate process handle.
/// @return Caller-owned result map, including an empty result for an invalid
///         handle, or NULL when managed result allocation fails.
void *rt_process_read_output_result(void *handle) {
    rt_process_impl *proc = process_checked(handle);
    if (!proc || !proc->started || proc->destroyed)
        return ordered_output_take_result(NULL);
    process_drain(proc);
    return ordered_output_take_result(proc);
}

/// @brief Write all possible bytes to the redirected child stdin stream.
/// @details Runtime-string length is honored, so embedded NUL bytes are written.
///          Windows enqueues into a bounded background writer so a child that
///          stops reading can never block the caller/UI thread. POSIX suppresses
///          only the SIGPIPE attributable to this write and uses a nonblocking
///          descriptor.
/// @param handle Candidate running or exited process handle with an open stdin
///        pipe.
/// @param data Runtime byte string; NULL is treated as empty.
/// @return Number of bytes accepted, zero for empty input, a positive partial
///         count when capacity is limited, or -1 when no byte can be accepted.
int64_t rt_process_write_stdin(void *handle, rt_string data) {
    rt_process_impl *proc = process_checked(handle);
    if (!proc || !proc->started || proc->destroyed)
        return -1;

    const char *bytes = data ? rt_string_cstr(data) : "";
    size_t len = data ? (size_t)rt_str_len(data) : 0;
    if (len == 0)
        return 0;

#if defined(_WIN32)
    return process_stdin_enqueue(proc, bytes, len);
#else
    if (proc->stdin_fd < 0)
        return -1;
    size_t off = 0;
    while (off < len) {
        size_t chunk = len - off;
        if (chunk > (size_t)SSIZE_MAX)
            chunk = (size_t)SSIZE_MAX;
        ssize_t n = process_write_no_sigpipe(proc->stdin_fd, bytes + off, chunk);
        if (n < 0) {
            return off > 0 ? (int64_t)off : -1;
        }
        if (n == 0)
            return off > 0 ? (int64_t)off : -1;
        off += (size_t)n;
    }
    return (int64_t)off;
#endif
}

/// @brief Poll and read a process's retained exit status.
/// @param handle Candidate process handle.
/// @return Normal exit code, a negated signal number on POSIX, or -1 while
///         running, after wait failure, or for an invalid/destroyed handle.
int64_t rt_process_exit_code(void *handle) {
    rt_process_impl *proc = process_checked(handle);
    if (!proc || !proc->started || proc->destroyed)
        return -1;
    process_poll_internal(proc, 0);
    return proc->running ? -1 : proc->exit_code;
}

/// @brief Request termination of a running child without waiting for it.
/// @details Uses the per-handle Job Object on Windows and dedicated process
///          group on POSIX. Poll or wait afterward to observe and reap the
///          direct child's completion.
/// @param handle Candidate process handle.
/// @return 1 when the OS accepted a termination request, otherwise 0.
int64_t rt_process_kill(void *handle) {
    rt_process_impl *proc = process_checked(handle);
    if (!proc || !proc->started || proc->destroyed)
        return 0;

#if defined(_WIN32)
    if (!proc->job)
        return 0;
    return TerminateJobObject(proc->job, 1) ? 1 : 0;
#else
    if (proc->pid <= 0)
        return 0;
    return process_signal_tree(proc, SIGTERM) ? 1 : 0;
#endif
}

/// @brief Wait for a child to exit while continuously draining output.
/// @param handle Candidate process handle.
/// @return Retained normal exit code, negated POSIX signal number, or -1 for an
///         invalid handle or wait/status failure.
int64_t rt_process_wait(void *handle) {
    rt_process_impl *proc = process_checked(handle);
    if (!proc || !proc->started || proc->destroyed)
        return -1;
    process_poll_internal(proc, 1);
    return proc->exit_code;
}

/// @brief Idempotently terminate if needed and release process resources.
/// @details The GC-managed object remains allocated but becomes invalid for all
///          subsequent process operations.
/// @param handle Candidate process handle; invalid and already destroyed
///        objects are ignored.
void rt_process_destroy(void *handle) {
    rt_process_impl *proc = process_checked(handle);
    process_close(proc);
}
