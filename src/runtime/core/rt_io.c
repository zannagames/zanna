//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_io.c
// Purpose: Provides shared trap recovery/dispatch, BASIC and Zanna.Terminal
//          console I/O, line input, CSV-style field splitting, and file-channel
//          EOF/length/position/seek queries used by native runtime entry points.
//
// Key invariants:
//   - All traps route through rt_trap or vm_trap; callers can install a
//     jmp_buf recovery point via rt_trap_set_recovery for recoverable errors.
//   - Trap dispatch unwinds shared GC mutator scopes before non-local transfer.
//   - Newline handling follows historical BASIC: CRLF is accepted on input,
//     LF is emitted on output; raw binary reads are unmodified.
//   - Successful explicit seeks clear the cached EOF flag; non-seekable
//     channels report their previously cached read state.
//   - Helpers never take ownership of caller-supplied buffers; all writes into
//     caller storage are bounded by explicit capacity parameters.
//   - OS errors are converted to runtime error codes; errno is not exposed
//     directly to BASIC programs.
//
// Ownership/Lifetime:
//   - File channel descriptors are owned by the runtime channel table; callers
//     use integer channel numbers, not FILE* pointers directly.
//   - Returned rt_string values (e.g., from input routines) transfer a new
//     reference to the caller, who must call rt_string_unref when done.
//
// Links: src/runtime/core/rt_io.h (public API — via rt_file.h),
//        src/runtime/core/rt_output.c (buffered stdout wrapper),
//        src/runtime/core/rt_string_format.c (numeric formatting),
//        docs/runtime/io.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements trap dispatch, terminal I/O, field parsing, and file queries.

#include "rt_ascii.h"
#include "rt_context_internal.h"
#include "rt_error.h"
#include "rt_file.h"
#include "rt_format.h"
#include "rt_gc.h"
#include "rt_int_format.h"
#include "rt_internal.h"
#include "rt_option.h"
#include "rt_output.h"
#include "rt_result.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "rt_string_builder.h"

#include "rt_platform.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#if !RT_PLATFORM_WINDOWS
#include <unistd.h>
#endif

// =============================================================================
// Thread-local trap recovery for safe threads
// =============================================================================

/// @brief Discriminator for legacy jump-buffer and native codegen recovery nodes.
typedef enum { RT_TRAP_RECOVERY_LEGACY = 0, RT_TRAP_RECOVERY_NATIVE = 1 } rt_trap_recovery_kind_t;

/// @brief Common intrusive node embedded in every thread-local recovery frame.
typedef struct rt_trap_recovery_base {
    struct rt_trap_recovery_base *prev;
    rt_trap_recovery_kind_t kind;
} rt_trap_recovery_base_t;

/// @brief Heap-allocated wrapper around a caller-owned legacy jump buffer.
typedef struct {
    rt_trap_recovery_base_t base;
    jmp_buf *buf;
} rt_trap_legacy_recovery_t;

/// @brief Native codegen recovery frame with inline jump state and site metadata.
typedef struct {
    jmp_buf env;
    rt_trap_recovery_base_t base;
    int64_t site_id;
} rt_native_eh_frame_t;

/// @brief Top of the current thread's intrusive recovery-frame stack.
static _Thread_local rt_trap_recovery_base_t *rt_trap_recovery_top_ = NULL;
/// @brief Bounded diagnostic captured for the current thread's latest trap.
static _Thread_local char rt_trap_error_[512] = "";
/// @brief Legacy network error code retained separately for `Net.LastError`.
static _Thread_local int rt_trap_net_code_ = 0;

/// @brief Remove consecutive legacy recovery points from the top of this thread's stack.
/// @details Native exception-style frames are left untouched because they are
///          owned by lexical helper macros; encountering one stops the walk,
///          leaving it and any older nodes intact. This helper is used by the
///          legacy `rt_trap_set_recovery(NULL)` API.
static void rt_trap_clear_all_legacy_recoveries(void) {
    while (rt_trap_recovery_top_ && rt_trap_recovery_top_->kind == RT_TRAP_RECOVERY_LEGACY) {
        rt_trap_legacy_recovery_t *node = (rt_trap_legacy_recovery_t *)rt_trap_recovery_top_;
        rt_trap_recovery_top_ = node->base.prev;
        free(node);
    }
    rt_trap_error_[0] = '\0';
}

/// @brief Install a longjmp recovery point for recoverable traps on this thread.
/// @details A non-NULL buffer is wrapped in a heap-allocated legacy node and
///   pushed above the current legacy or native frame. Allocation failure aborts
///   immediately. NULL removes consecutive legacy frames from the top and
///   clears the saved diagnostic, but never removes a native frame.
/// @param buf Borrowed live jump buffer, or NULL to clear top-level legacy recovery.
void rt_trap_set_recovery(jmp_buf *buf) {
    if (!buf) {
        rt_trap_clear_all_legacy_recoveries();
        return;
    }
    rt_trap_legacy_recovery_t *node =
        (rt_trap_legacy_recovery_t *)malloc(sizeof(rt_trap_legacy_recovery_t));
    if (!node)
        rt_abort("rt_trap_set_recovery: alloc");
    node->base.prev = rt_trap_recovery_top_;
    node->base.kind = RT_TRAP_RECOVERY_LEGACY;
    node->buf = buf;
    rt_trap_recovery_top_ = &node->base;
}

/// @brief Pop the top legacy recovery node and clear the saved error.
/// @details If the stack is empty or a native frame is on top, no frame is
///   removed. The diagnostic buffer is cleared in every case.
void rt_trap_clear_recovery(void) {
    if (rt_trap_recovery_top_ && rt_trap_recovery_top_->kind == RT_TRAP_RECOVERY_LEGACY) {
        rt_trap_legacy_recovery_t *node = (rt_trap_legacy_recovery_t *)rt_trap_recovery_top_;
        rt_trap_recovery_top_ = node->base.prev;
        free(node);
    }
    rt_trap_error_[0] = '\0';
}

/// @brief Return the error message captured by the most recent recovered trap.
/// @return Runtime-owned thread-local C string. It remains valid until another
///   trap or recovery-clear operation on the same thread overwrites it.
const char *rt_trap_get_error(void) {
    return rt_trap_error_;
}

/// @brief Terminate the runtime immediately due to a fatal condition.
/// @details Prints @p msg to stderr when provided, otherwise emits the generic
///          "Trap" sentinel before exiting with status code 1.  The function is
///          the last-resort termination path for unrecoverable runtime failures
///          and therefore never returns.
/// @param msg Optional diagnostic message describing the reason for the abort.
/// @return This function does not return.
void rt_abort(const char *msg) {
    if (msg && *msg)
        fprintf(stderr, "%s\n", msg);
    else
        fprintf(stderr, "Trap\n");
    fflush(stdout);
    fflush(stderr);
    _Exit(1);
}

/// @brief Default trap handler invoked by helper routines.
/// @details Marked @c weak so embedders can override the implementation.  The
///          default delegates to @ref rt_abort so that traps terminate the
///          process with the provided diagnostic message.
/// @param msg Optional message describing the trap condition.
/// @return The default implementation does not return; overrides may return.
#if RT_PLATFORM_WINDOWS
// On Windows, define vm_trap with alternatename fallback.
// Tests can define their own vm_trap to override this.
// The alternatename directive provides the fallback when vm_trap is not
// explicitly defined by the application.
/// @brief Default Windows trap fallback that terminates through @ref rt_abort.
/// @param msg Optional borrowed diagnostic string.
/// @return This implementation does not return.
void vm_trap_default(const char *msg) {
    rt_abort(msg);
}

/// @brief Windows trap hook resolved to an application override or the default fallback.
/// @param msg Optional borrowed diagnostic string.
extern void vm_trap(const char *msg);
#if defined(_MSC_VER) || defined(__clang__)
#pragma comment(linker, "/alternatename:vm_trap=vm_trap_default")
#endif
#else
// On Unix, use weak linkage attribute for override capability
/// @brief Weak Unix trap hook whose default terminates through @ref rt_abort.
/// @param msg Optional borrowed diagnostic string.
/// @return The default does not return; a strong embedder override may return.
RT_WEAK void vm_trap(const char *msg) {
    rt_abort(msg);
}
#endif

/// @brief Raise a runtime trap using the currently configured trap handler.
/// @details If a thread-local recovery point is set (via rt_trap_set_recovery),
///          the trap message is captured and control is transferred to the
///          recovery point via longjmp instead of terminating the process.
///          Otherwise forwards the message to @ref vm_trap.
/// @param msg Null-terminated string describing the trap condition.
/// @return Returns only when the active trap hook returns.
#if defined(_MSC_VER)
#include <intrin.h>
#endif

/// @brief Return the caller's return address using compiler-specific intrinsics.
/// @details Used by the trap dispatcher to record the IP where a
///          trap fired, so a debugger or stack-printing helper can
///          show the user *where* in their compiled code the trap
///          originated. MSVC has `_ReturnAddress`, GCC/Clang have
///          `__builtin_return_address(0)`, anything else falls back
///          to `0` (no IP capture, but the trap still propagates).
/// @return Best-effort caller instruction address, or zero when unsupported.
static uintptr_t rt_capture_return_address(void) {
#if defined(_MSC_VER)
    return (uintptr_t)_ReturnAddress();
#elif defined(__GNUC__) || defined(__clang__)
    return (uintptr_t)__builtin_return_address(0);
#else
    return (uintptr_t)0;
#endif
}

/// @brief Centralized trap dispatcher: classify, store fields, longjmp or call vm_trap.
/// @details Single entry point used by all trap-raising helpers.
///          Steps:
///          1. Validate `kind`; out-of-range values normalize to
///             `RT_TRAP_KIND_RUNTIME_ERROR`.
///          2. Stash IP / kind / code / line into thread-local slots
///             so `ErrGetKind/Code/Line/Ip` can read them inside a
///             catch handler.
///          3. Clear the network-error code unless this is a network
///             trap (so `Net.LastError` doesn't show stale data).
///          4. If a recovery point is active, copy the message into
///             the TLS error buffer and `longjmp` — the kind of jump
///             depends on whether the recovery is a legacy `jmp_buf`
///             or a native EH frame.
///          5. Otherwise, hand off to the user-replaceable `vm_trap` (which
///             aborts by default). A hook that returns leaves lexical mutator
///             scopes intact so the calling function can execute its normal
///             cleanup and return path.
/// @param msg Optional borrowed diagnostic; active recovery substitutes
///   `Unknown trap` for NULL.
/// @param kind Canonical trap kind, normalized to runtime error when out of range.
/// @param code Secondary runtime error code.
/// @param line Source line, or -1 when unavailable.
/// @param return_address Captured native call-site address.
/// @note Active recovery performs a non-local transfer and does not return.
///   Without recovery, this returns only if the installed VM hook returns.
static void rt_trap_dispatch(
    const char *msg, int32_t kind, int32_t code, int32_t line, uintptr_t return_address) {
    const char *stable_msg = msg;

    if (kind < RT_TRAP_KIND_DIVIDE_BY_ZERO || kind > RT_TRAP_KIND_NETWORK_ERROR)
        kind = RT_TRAP_KIND_RUNTIME_ERROR;
    rt_trap_set_ip((uint64_t)return_address);
    rt_trap_fields_set(kind, code, line);
    if (kind != RT_TRAP_KIND_NETWORK_ERROR)
        rt_trap_net_code_ = 0;
    if (msg) {
        snprintf(rt_trap_error_, sizeof(rt_trap_error_), "%s", msg);
        stable_msg = rt_trap_error_;
    } else if (rt_trap_recovery_top_) {
        snprintf(rt_trap_error_, sizeof(rt_trap_error_), "%s", "Unknown trap");
        stable_msg = rt_trap_error_;
    } else {
        rt_trap_error_[0] = '\0';
    }
    if (rt_trap_recovery_top_) {
        /* A non-local transfer skips every lexical mutator-scope exit between
         * this dispatcher and the recovery frame, so unwind the shared graph
         * barrier and context-state mutexes immediately before longjmp. */
        rt_gc_mutator_abort_for_trap();
        rt_context_state_abort_for_trap();
        if (rt_trap_recovery_top_->kind == RT_TRAP_RECOVERY_NATIVE) {
            rt_native_eh_frame_t *frame =
                (rt_native_eh_frame_t *)((char *)rt_trap_recovery_top_ -
                                         offsetof(rt_native_eh_frame_t, base));
            RT_LONGJMP(frame->env, 1);
        }
        RT_LONGJMP(*((rt_trap_legacy_recovery_t *)rt_trap_recovery_top_)->buf, 1);
    }
    vm_trap(stable_msg);
}

/// @brief Raise a trap with explicit kind/code/line classification.
/// @details Captures the return address for IP reporting and routes through the unified
///   dispatcher so recovery handlers and native EH frames see consistent fields.
/// @param kind Canonical trap classification; out-of-range values normalize.
/// @param code Secondary runtime error code.
/// @param line Source line, or -1 when unavailable.
/// @param msg Optional borrowed null-terminated diagnostic.
void rt_trap_raise_kind(int32_t kind, int32_t code, int32_t line, const char *msg) {
    rt_trap_dispatch(msg, kind, code, line, rt_capture_return_address());
}

/// @brief Raise a trap with explicit kind/code/line classification and no message.
/// @param kind Canonical trap classification; out-of-range values normalize.
/// @param code Secondary runtime error code.
/// @param line Source line, or -1 when unavailable.
void rt_trap_raise_kind_nomsg(int32_t kind, int32_t code, int32_t line) {
    rt_trap_raise_kind(kind, code, line, NULL);
}

/// @brief Raise a generic domain-error trap with the supplied message.
/// @details Convenience wrapper that defaults kind=RT_TRAP_KIND_DOMAIN_ERROR,
///   code=0, and line=-1.
/// @param msg Optional borrowed null-terminated diagnostic.
void rt_trap(const char *msg) {
    rt_trap_raise_kind(RT_TRAP_KIND_DOMAIN_ERROR, 0, -1, msg);
}

/// @brief Raise a network-specific trap with an error code.
/// @details Stores the error code in thread-local storage before delegating to
///          rt_trap(). The error code can be retrieved after recovery via
///          rt_trap_get_net_code() for programmatic error classification.
/// @param msg Human-readable description of the network failure.
/// @param err_code One of the Err_Connection*/Err_Dns*/Err_Network* codes.
void rt_trap_net(const char *msg, int err_code) {
    rt_trap_net_code_ = err_code;
    rt_trap_raise_kind(rt_err_to_trap_kind(err_code), err_code, -1, msg);
}

/// @brief Retrieve the error code from the most recent network trap.
/// @return Thread-local `Err_*` code set by the last network trap; a later
///   non-network trap resets it to zero.
int rt_trap_get_net_code(void) {
    return rt_trap_net_code_;
}

/// @brief Allocate a native exception-handling frame for use by codegen.
/// @details The zero-initialized frame is not pushed automatically.
/// @return Caller-owned opaque frame, or NULL on allocation failure; pass it to
///   push/pop/set-site helpers and eventually @ref rt_native_eh_frame_free.
void *rt_native_eh_frame_alloc(void) {
    rt_native_eh_frame_t *frame = (rt_native_eh_frame_t *)calloc(1, sizeof(rt_native_eh_frame_t));
    return frame;
}

/// @brief Release a native EH frame allocated by `rt_native_eh_frame_alloc`.
/// @details Searches the current thread's recovery chain and unlinks the frame
///   even when it is not the top entry, then releases its storage. NULL is safe.
/// @param frame_ptr Owned opaque native frame pointer, or NULL.
void rt_native_eh_frame_free(void *frame_ptr) {
    rt_native_eh_frame_t *frame = (rt_native_eh_frame_t *)frame_ptr;
    if (frame) {
        rt_trap_recovery_base_t **cursor = &rt_trap_recovery_top_;
        while (*cursor) {
            if (*cursor == &frame->base) {
                *cursor = frame->base.prev;
                frame->base.prev = NULL;
                break;
            }
            cursor = &(*cursor)->prev;
        }
    }
    free(frame_ptr);
}

/// @brief Push a native EH frame onto the thread-local recovery stack.
/// @details Subsequent traps longjmp to this frame's saved environment. The
///   site identifier is reset to zero. Callers must pop or free the frame
///   before its storage lifetime ends.
/// @param frame_ptr Borrowed opaque frame; NULL is a no-op.
void rt_native_eh_push(void *frame_ptr) {
    rt_native_eh_frame_t *frame = (rt_native_eh_frame_t *)frame_ptr;
    if (!frame)
        return;
    frame->base.prev = rt_trap_recovery_top_;
    frame->base.kind = RT_TRAP_RECOVERY_NATIVE;
    frame->site_id = 0;
    rt_trap_recovery_top_ = &frame->base;
}

/// @brief Pop a native EH frame off the recovery stack (no-op if it is not on top).
/// @details Tolerates NULL, double-pop, and mismatched ordering by checking
///   identity before unlinking. The frame is not freed.
/// @param frame_ptr Borrowed opaque frame to compare with the stack top.
void rt_native_eh_pop(void *frame_ptr) {
    rt_native_eh_frame_t *frame = (rt_native_eh_frame_t *)frame_ptr;
    if (!frame)
        return;
    if (rt_trap_recovery_top_ == &frame->base)
        rt_trap_recovery_top_ = frame->base.prev;
}

/// @brief Tag a native EH frame with a codegen-supplied call-site identifier.
/// @details Used by EH lowering to disambiguate which `try` block was active.
/// @param frame_ptr Borrowed opaque frame; NULL is a no-op.
/// @param site_id Codegen-defined identifier stored without interpretation.
void rt_native_eh_set_site(void *frame_ptr, int64_t site_id) {
    rt_native_eh_frame_t *frame = (rt_native_eh_frame_t *)frame_ptr;
    if (!frame)
        return;
    frame->site_id = site_id;
}

/// @brief Read the call-site identifier last stored on the given native EH frame.
/// @param frame_ptr Borrowed opaque frame, or NULL.
/// @return Stored site identifier, or zero for NULL and newly pushed frames.
int64_t rt_native_eh_get_site(void *frame_ptr) {
    rt_native_eh_frame_t *frame = (rt_native_eh_frame_t *)frame_ptr;
    return frame ? frame->site_id : 0;
}

// =============================================================================
// Helper Functions
// =============================================================================

/// @brief Get the length of a runtime string safely.
/// @param s Runtime string handle; may be null.
/// @return Length in bytes, or 0 if s is null or has null data.
static inline size_t rt_string_safe_len(rt_string s) {
    if (!s)
        return 0;
    if (!rt_string_is_handle(s)) {
        rt_trap("rt_print_str: invalid string handle");
        return 0;
    }
    int64_t raw_len = rt_str_len(s);
    if (raw_len <= 0)
        return 0;
    return (size_t)raw_len;
}

/// @brief Handle string builder errors with consistent trap messages.
/// @details Success leaves the builder untouched. Any failure selects an
///   operation-specific diagnostic, frees the builder, and raises a trap.
/// @param sb String builder to free on error.
/// @param op_name Non-NULL name used as the diagnostic prefix.
/// @param status Error status from string builder operation.
static void rt_sb_check_status(rt_string_builder *sb, const char *op_name, rt_sb_status_t status) {
    if (status == RT_SB_OK)
        return;

    const char *msg = op_name;
    char detail_msg[64];
    if (status == RT_SB_ERROR_ALLOC) {
        snprintf(detail_msg, sizeof(detail_msg), "%s: alloc", op_name);
        msg = detail_msg;
    } else if (status == RT_SB_ERROR_OVERFLOW) {
        snprintf(detail_msg, sizeof(detail_msg), "%s: overflow", op_name);
        msg = detail_msg;
    } else if (status == RT_SB_ERROR_INVALID) {
        snprintf(detail_msg, sizeof(detail_msg), "%s: invalid", op_name);
        msg = detail_msg;
    }

    rt_sb_free(sb);
    rt_trap(msg);
}

/// @brief Write a runtime string to stdout without appending a newline.
/// @details Gracefully ignores null handles and strings with zero length.  Uses
///          the centralized output buffering system for improved performance.
///          When batch mode is active, output accumulates until the batch ends.
/// @param s Runtime string handle to print; may be null.
void rt_print_str(rt_string s) {
    size_t len = rt_string_safe_len(s);
    if (len == 0)
        return;

    const char *data = rt_string_cstr(s);
    if (!data)
        return;
    rt_output_strn(data, len);
    rt_output_flush_if_not_batch();
}

/// @brief Print a signed 64-bit integer to stdout in decimal form.
/// @details Formats the value using the runtime string builder to avoid
///          temporary heap allocations.  Uses centralized output buffering
///          for improved performance. Formatting failures trap with a
///          descriptive message so misconfigurations become visible during
///          testing.
/// @param v Value to print.
void rt_print_i64(int64_t v) {
    rt_string_builder sb;
    rt_sb_init(&sb);
    rt_sb_status_t status = rt_sb_append_int(&sb, v);
    rt_sb_check_status(&sb, "rt_print_i64", status);

    if (sb.len > 0)
        rt_output_strn(sb.data, sb.len);
    rt_output_flush_if_not_batch();
    rt_sb_free(&sb);
}

/// @brief Print a floating-point number to stdout.
/// @details Uses @ref rt_format_f64 to normalise decimal separators and handle
///          special values consistently. Uses centralized output buffering for
///          improved performance.
/// @param v Double precision value to print.
void rt_print_f64(double v) {
    char buf[64];
    rt_format_f64(v, buf, sizeof(buf));
    rt_output_str(buf);
    rt_output_flush_if_not_batch();
}

/// @brief Grow an input buffer used by @ref rt_input_line.
/// @details Doubles the allocation when possible while guarding against
///          overflow and allocation failure.  The helper mutates @p buf and
///          @p cap in place and returns a status enumerator so callers can
///          distinguish between error conditions.
/// @param buf [in,out] Pointer to the buffer pointer to grow.
/// @param cap [in,out] Pointer to the capacity counter associated with @p buf.
/// @return Result enumerator describing whether the buffer was resized.
rt_input_grow_result_t rt_input_try_grow(char **buf, size_t *cap) {
    if (!buf || !cap || !*buf)
        return RT_INPUT_GROW_ALLOC_FAILED;

    if (*cap > SIZE_MAX / 2)
        return RT_INPUT_GROW_OVERFLOW;

    size_t new_cap = (*cap) * 2;
    char *nbuf = (char *)realloc(*buf, new_cap);
    if (!nbuf)
        return RT_INPUT_GROW_ALLOC_FAILED;

    *buf = nbuf;
    *cap = new_cap;
    return RT_INPUT_GROW_OK;
}

/// @brief Read a single line of input from stdin into a runtime string.
/// @details Allocates a temporary buffer, grows it as needed, strips the
///          trailing newline and optional carriage return, and returns a newly
///          allocated @ref rt_string that owns the resulting characters.  On
///          EOF before any bytes are read the function returns @c NULL to signal
///          end-of-input. Flushes output first to ensure prompts are visible.
///          Buffer-growth overflow or reallocation failure raises a runtime
///          trap; the initial scratch allocation is expected to succeed.
/// @return Caller-owned runtime string without the trailing newline, or NULL
///   on EOF before reading data or after a returning growth-error trap.
rt_string rt_input_line(void) {
    // Flush output before reading input so prompts are visible
    rt_output_flush();

    size_t cap = 1024;
    size_t len = 0;
    char *buf = (char *)rt_alloc(cap);
    for (;;) {
        int ch = fgetc(stdin);
        if (ch == EOF) {
            if (len == 0) {
                free(buf);
                return NULL;
            }
            break;
        }
        if (ch == '\n')
            break;
        if (len + 1 >= cap) {
            rt_input_grow_result_t grow = rt_input_try_grow(&buf, &cap);
            if (grow == RT_INPUT_GROW_OVERFLOW) {
                free(buf);
                rt_trap("rt_input_line: overflow");
                return NULL;
            }
            if (grow != RT_INPUT_GROW_OK) {
                free(buf);
                rt_trap("out of memory");
                return NULL;
            }
        }
        buf[len++] = (char)ch;
    }
    if (len > 0 && buf[len - 1] == '\r')
        len--;
    // Construct runtime string from the accumulated buffer and release temp.
    rt_string s = rt_string_from_bytes(buf, len);
    free(buf);
    return s;
}

/// @brief Split a comma-separated input line into runtime string fields.
/// @details Parses @p line while respecting quoted fields and doubled quotes.
///          Extracted fields are trimmed of leading and trailing whitespace and
///          materialised as runtime strings stored in @p out_fields until
///          @p max_fields entries have been populated.  When fewer fields are
///          present than expected the function traps with a descriptive error.
///          Parsing is intentionally lenient: quotes toggle quoted mode wherever
///          encountered, and an unmatched final quote consumes the remainder.
/// @param line Borrowed valid runtime string containing the raw input line;
///   NULL is treated as an empty record.
/// @param out_fields Destination array receiving caller-owned strings. It may
///   be NULL only when @p max_fields is non-positive.
/// @param max_fields Maximum number of fields to populate; negative values are treated as zero.
/// @return Total number of fields present, which may exceed the number stored.
///   Allocation failures trap and may return the count reached so far if the
///   active trap hook returns.
int64_t rt_str_split_fields(rt_string line, rt_string *out_fields, int64_t max_fields) {
    if (max_fields <= 0) {
        max_fields = 0;
    } else if (!out_fields) {
        rt_trap("rt_str_split_fields: null output");
        return 0;
    }

    const char *data = "";
    size_t len = 0;
    if (line && line->data) {
        data = line->data;
        if (line->heap && line->heap != RT_SSO_SENTINEL)
            len = rt_heap_len(line->data);
        else
            len = line->literal_len;
    }

    int64_t stored = 0;
    int64_t total = 0;
    size_t start = 0;
    bool in_quotes = false;
    size_t i = 0;
    while (i <= len) {
        bool finalize = false;
        if (i == len) {
            finalize = true;
        } else {
            char ch = data[i];
            if (ch == '"') {
                if (in_quotes) {
                    if (i + 1 < len && data[i + 1] == '"') {
                        ++i;
                    } else {
                        in_quotes = false;
                    }
                } else {
                    in_quotes = true;
                }
            } else if (ch == ',' && !in_quotes) {
                finalize = true;
            }
        }

        if (finalize) {
            size_t field_start = start;
            size_t field_end = i;
            while (field_start < field_end && rt_ascii_isspace((unsigned char)data[field_start]))
                ++field_start;
            while (field_end > field_start && rt_ascii_isspace((unsigned char)data[field_end - 1]))
                --field_end;
            bool had_quotes = false;
            if (field_end > field_start && data[field_start] == '"' && data[field_end - 1] == '"') {
                ++field_start;
                --field_end;
                had_quotes = true;
            }

            if (stored < max_fields) {
                if (field_end <= field_start) {
                    out_fields[stored++] = rt_str_empty();
                } else {
                    if (had_quotes) {
                        size_t unescaped_len = 0;
                        size_t j = field_start;
                        while (j < field_end) {
                            if (data[j] == '"' && j + 1 < field_end && data[j + 1] == '"') {
                                ++j;
                            }
                            ++unescaped_len;
                            ++j;
                        }

                        char *tmp = (char *)malloc(unescaped_len);
                        if (!tmp && unescaped_len > 0) {
                            rt_trap("out of memory");
                            return total;
                        }

                        j = field_start;
                        size_t write = 0;
                        while (j < field_end) {
                            char ch = data[j];
                            if (ch == '"' && j + 1 < field_end && data[j + 1] == '"') {
                                tmp[write++] = '"';
                                j += 2;
                            } else {
                                tmp[write++] = ch;
                                ++j;
                            }
                        }

                        out_fields[stored++] = rt_string_from_bytes(tmp, unescaped_len);
                        free(tmp);
                    } else {
                        out_fields[stored++] =
                            rt_string_from_bytes(data + field_start, field_end - field_start);
                    }
                }
            }
            ++total;
            start = i + 1;
        }

        ++i;
    }

    if (max_fields > 0 && total < max_fields) {
        char msg[128];
        snprintf(msg,
                 sizeof(msg),
                 "INPUT: expected %lld value%s, got %lld",
                 (long long)max_fields,
                 max_fields == 1 ? "" : "s",
                 (long long)total);
        rt_trap(msg);
    }

    return total;
}

/// @brief Split an input line into an owned Seq[str] instead of caller-owned out buffers.
/// @details Counts fields with the lenient splitter, allocates temporary string
///   slots, parses every field, and transfers each owned string into a new
///   sequence. Size and allocation failures trap; a returning hook may receive
///   the sequence constructed up to that point.
/// @param line Borrowed valid runtime string, or NULL for an empty record.
/// @return Caller-owned opaque `Zanna.Seq[str]` object.
void *rt_str_split_fields_seq(rt_string line) {
    int64_t total = rt_str_split_fields(line, NULL, 0);
    void *seq = rt_seq_with_capacity_owned(total > 0 ? total : 1);
    if (total <= 0)
        return seq;

    if ((uint64_t)total > SIZE_MAX / sizeof(rt_string)) {
        rt_trap("rt_str_split_fields_seq: allocation size overflow");
        return seq;
    }
    rt_string *fields = (rt_string *)calloc((size_t)total, sizeof(rt_string));
    if (!fields) {
        rt_trap("out of memory");
        return seq;
    }

    int64_t parsed = rt_str_split_fields(line, fields, total);
    for (int64_t i = 0; i < parsed && i < total; ++i)
        rt_seq_push_raw(seq, fields[i]);
    free(fields);
    return seq;
}

/// @brief Validating companion to `rt_str_split_fields_seq` (VDOC-164).
/// @details The plain splitter is deliberately lenient INPUT-style parsing.
///          This variant first enforces strict quote structure — a quoted
///          field must open at the field start, close exactly once, and be
///          followed only by whitespace before the next comma; quotes may not
///          appear inside unquoted text; the record may not end inside a
///          quote — and reports violations as `Result.ErrStr` instead of
///          silently merging columns.
/// @param line Borrowed valid runtime string, or NULL for an empty record.
/// @return Caller-owned `Result.Ok(Seq[str])` or `Result.ErrStr(message)`.
void *rt_str_split_fields_result(rt_string line) {
    const char *data = "";
    size_t len = 0;
    if (line && line->data) {
        data = line->data;
        if (line->heap && line->heap != RT_SSO_SENTINEL)
            len = rt_heap_len(line->data);
        else
            len = line->literal_len;
    }

    enum { FIELD_START, UNQUOTED, QUOTED, QUOTED_END } state = FIELD_START;

    const char *err = NULL;
    for (size_t i = 0; i < len && !err; ++i) {
        char ch = data[i];
        switch (state) {
            case FIELD_START:
                if (ch == '"')
                    state = QUOTED;
                else if (ch == ',')
                    ; // empty field; stay at field start
                else if (!rt_ascii_isspace((unsigned char)ch))
                    state = UNQUOTED;
                break;
            case UNQUOTED:
                if (ch == '"')
                    err = "SplitFields: quote inside unquoted field";
                else if (ch == ',')
                    state = FIELD_START;
                break;
            case QUOTED:
                if (ch == '"') {
                    if (i + 1 < len && data[i + 1] == '"')
                        ++i; // escaped ""
                    else
                        state = QUOTED_END;
                }
                break;
            case QUOTED_END:
                if (ch == ',')
                    state = FIELD_START;
                else if (!rt_ascii_isspace((unsigned char)ch))
                    err = "SplitFields: text after closing quote";
                break;
        }
    }
    if (!err && state == QUOTED)
        err = "SplitFields: unclosed quote";
    if (err)
        return rt_result_err_str(rt_const_cstr(err));
    return rt_result_ok(rt_str_split_fields_seq(line));
}

/// @brief Determine whether a file channel has reached EOF.
/// @details Seekable regular and block files compare their current offset with
///   `fstat` size. Other seekable descriptors temporarily seek to the end and
///   restore the original position. Pipes and other non-seekable descriptors
///   use the channel's cached EOF state. Successful probes update that cache.
/// @param ch Channel identifier registered with the runtime file subsystem.
/// @return Positive `Err_*` code on failure, -1 at EOF, or 0 when more data is
///   available according to the probe or cache.
int rt_eof_ch(int ch) {
    int fd = -1;
    int32_t status = rt_file_channel_fd(ch, &fd);
    if (status != 0)
        return status;

    int8_t cached = 0;
    status = rt_file_channel_get_eof(ch, &cached);
    if (status != 0)
        return status;

    errno = 0;
    int64_t cur = lseek(fd, 0, SEEK_CUR);
    if (cur >= 0) {
        struct stat st;
        if (fstat(fd, &st) == 0 && (S_ISREG(st.st_mode) || S_ISBLK(st.st_mode)) &&
            st.st_size >= 0) {
            if ((int64_t)st.st_size <= cur) {
                rt_file_channel_set_eof(ch, 1);
                return -1;
            }
            rt_file_channel_set_eof(ch, 0);
            return 0;
        }

        int64_t end = lseek(fd, 0, SEEK_END);
        if (end < 0) {
            (void)lseek(fd, cur, SEEK_SET);
            rt_file_channel_set_eof(ch, 0);
            return (int32_t)Err_IOError;
        }
        if (lseek(fd, cur, SEEK_SET) < 0) {
            rt_file_channel_set_eof(ch, 0);
            return (int32_t)Err_IOError;
        }
        if (end <= cur) {
            rt_file_channel_set_eof(ch, 1);
            return -1;
        }
        rt_file_channel_set_eof(ch, 0);
        return 0;
    }

    if (errno == ESPIPE || errno == EINVAL)
        return cached ? -1 : 0;

    rt_file_channel_set_eof(ch, 0);
    return (int32_t)Err_IOError;
}

/// @brief Query the length of the file bound to a channel.
/// @details Uses @ref fstat for regular files and blocks, falling back to
///          seeking to the end when necessary.  Errors are negated runtime error
///          codes so callers can propagate them through BASIC's error handling
///          conventions.
/// @param ch Channel identifier registered with the runtime file subsystem.
/// @return File length in bytes, or the negated runtime error code on failure.
int64_t rt_lof_ch(int ch) {
    int fd = -1;
    int32_t status = rt_file_channel_fd(ch, &fd);
    if (status != 0)
        return -(int64_t)status;

    struct stat st;
    if (fstat(fd, &st) == 0) {
        if (S_ISREG(st.st_mode) || S_ISBLK(st.st_mode)) {
            if (st.st_size >= 0)
                return (int64_t)st.st_size;
            return 0;
        }
    }

    errno = 0;
    int64_t cur = lseek(fd, 0, SEEK_CUR);
    if (cur < 0) {
        if (errno == ESPIPE || errno == EINVAL)
            return -(int64_t)Err_InvalidOperation;
        return -(int64_t)Err_IOError;
    }

    int64_t end = lseek(fd, 0, SEEK_END);
    if (end < 0) {
        (void)lseek(fd, cur, SEEK_SET);
        return -(int64_t)Err_IOError;
    }

    if (lseek(fd, cur, SEEK_SET) < 0)
        return -(int64_t)Err_IOError;

    return end;
}

/// @brief Report the current file position for the supplied channel.
/// @details Reads the file descriptor offset using @ref lseek and converts OS
///          failures into negated runtime error codes.  Special files such as
///          pipes yield @ref Err_InvalidOperation in keeping with BASIC's
///          semantics.
/// @param ch Channel identifier.
/// @return Current offset in bytes, or a negated runtime error code on failure.
int64_t rt_loc_ch(int ch) {
    int fd = -1;
    int32_t status = rt_file_channel_fd(ch, &fd);
    if (status != 0)
        return -(int64_t)status;

    errno = 0;
    int64_t cur = lseek(fd, 0, SEEK_CUR);
    if (cur < 0) {
        if (errno == ESPIPE || errno == EINVAL)
            return -(int64_t)Err_InvalidOperation;
        return -(int64_t)Err_IOError;
    }

    return cur;
}

/// @brief Seek to a byte offset on the channel's underlying file descriptor.
/// @details Validates @p pos, issues the seek via @ref lseek, clears the cached
///          EOF flag on success, and translates platform-specific failures into
///          BASIC runtime error codes.
/// @param ch Channel identifier.
/// @param pos Absolute offset to seek to; must be non-negative.
/// @return Zero on success or a runtime error code on failure.
int32_t rt_seek_ch_err(int ch, int64_t pos) {
    if (pos < 0)
        return (int32_t)Err_InvalidOperation;

    int fd = -1;
    int32_t status = rt_file_channel_fd(ch, &fd);
    if (status != 0)
        return status;

    errno = 0;
    int64_t res = lseek(fd, pos, SEEK_SET);
    if (res == -1) {
        if (errno == ESPIPE || errno == EINVAL)
            return (int32_t)Err_InvalidOperation;
        return (int32_t)Err_IOError;
    }

    (void)rt_file_channel_set_eof(ch, 0);
    return 0;
}

// =============================================================================
// Zanna.Terminal I/O Functions
// =============================================================================

/// @brief Print a string followed by a newline.
/// @param s Runtime string to print; may be null.
void rt_term_say(rt_string s) {
    rt_print_str(s);
    rt_output_str("\n");
    rt_output_flush();
}

/// @brief Print an integer followed by a newline.
/// @param v Integer value to print.
void rt_term_say_i64(int64_t v) {
    rt_print_i64(v);
    rt_output_str("\n");
    rt_output_flush();
}

/// @brief Print a floating-point number followed by a newline.
/// @param v Double value to print.
void rt_term_say_f64(double v) {
    rt_print_f64(v);
    rt_output_str("\n");
    rt_output_flush();
}

/// @brief Print a boolean as "true" or "false" followed by a newline.
/// @param v Boolean value (0 = false, non-zero = true).
void rt_term_say_bool(int8_t v) {
    rt_output_str(v ? "true\n" : "false\n");
    rt_output_flush();
}

/// @brief Print a string without a trailing newline.
/// @param s Runtime string to print; may be null.
void rt_term_print(rt_string s) {
    rt_print_str(s);
    rt_output_flush();
}

/// @brief Print an integer without a trailing newline.
/// @param v Integer value to print.
void rt_term_print_i64(int64_t v) {
    rt_print_i64(v);
    rt_output_flush();
}

/// @brief Print a floating-point number without a trailing newline.
/// @param v Double value to print.
void rt_term_print_f64(double v) {
    rt_print_f64(v);
    rt_output_flush();
}

/// @brief Print a boolean as "true" or "false" without a trailing newline.
/// @param v Boolean value (0 = false, non-zero = true).
void rt_term_print_bool(int8_t v) {
    rt_output_str(v ? "true" : "false");
    rt_output_flush();
}

/// @brief Print a prompt and read a line of input.
/// @param prompt Borrowed runtime string to display before reading; NULL prints nothing.
/// @return Caller-owned input string, or NULL on EOF before any bytes.
rt_string rt_term_ask(rt_string prompt) {
    rt_print_str(prompt);
    rt_output_flush();
    return rt_input_line();
}

/// @brief Read a line of input from stdin.
/// @return Caller-owned input string, or NULL on EOF before any bytes.
rt_string rt_term_read_line(void) {
    return rt_input_line();
}

/// @brief Wrap an owned terminal input line in an Option object.
/// @details A non-null line becomes `Some(line)`. EOF is represented as `None`.
///          The Option constructor retains the string payload, so this helper
///          releases the caller-owned input reference before returning. If the
///          Option allocation traps, the owned line is still released before the
///          trap is re-raised.
/// @param line Owned runtime string returned by @ref rt_input_line, or NULL on EOF.
/// @return Opaque `Zanna.Option` object containing the line, or `None` on EOF.
static void *rt_term_line_to_option(rt_string line) {
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[256];
        const char *err = rt_trap_get_error();
        snprintf(saved_error,
                 sizeof(saved_error),
                 "%s",
                 err && err[0] ? err : "Zanna.Terminal.TryReadLine: option allocation failed");
        rt_trap_clear_recovery();
        rt_str_release_maybe(line);
        rt_trap(saved_error);
        return NULL;
    }

    void *option = line ? rt_option_some_str(line) : rt_option_none();
    rt_trap_clear_recovery();
    rt_str_release_maybe(line);
    return option;
}

/// @brief Wrap an owned terminal input line in a Result object.
/// @details A non-null line becomes `Ok(line)`. EOF is represented as
///          `Err(eof_message)`, making the failure explicit without trapping.
///          The Result constructor retains the string payload, so this helper
///          releases any owned temporary strings before returning. Allocation or
///          retain failures are re-raised as runtime traps after cleanup.
/// @param line Owned runtime string returned by @ref rt_input_line, or NULL on EOF.
/// @param eof_message Null-terminated diagnostic used for the EOF Err value.
/// @return Opaque `Zanna.Result` object containing `Ok(line)` or `Err(message)`.
static void *rt_term_line_to_result(rt_string line, const char *eof_message) {
    rt_string err_message = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[256];
        const char *err = rt_trap_get_error();
        snprintf(saved_error,
                 sizeof(saved_error),
                 "%s",
                 err && err[0] ? err : "Zanna.Terminal.ReadLineResult: result allocation failed");
        rt_trap_clear_recovery();
        rt_str_release_maybe(line);
        rt_str_release_maybe(err_message);
        rt_trap(saved_error);
        return NULL;
    }

    void *result = NULL;
    if (line) {
        result = rt_result_ok_str(line);
    } else {
        err_message = rt_const_cstr(eof_message);
        result = rt_result_err_str(err_message);
    }

    rt_trap_clear_recovery();
    rt_str_release_maybe(line);
    rt_str_release_maybe(err_message);
    return result;
}

/// @brief Read a line from stdin as an Option.
/// @details Returns `Some(String)` when a line is read and `None` when stdin is
///          at EOF before any bytes are available. I/O allocation failures and
///          overlong input still trap, matching @ref rt_input_line.
/// @return Opaque `Zanna.Option` object containing the input line or `None`.
void *rt_term_try_read_line(void) {
    return rt_term_line_to_option(rt_input_line());
}

/// @brief Print a prompt and read a line from stdin as an Option.
/// @details The prompt is flushed before reading. Returns `Some(String)` when a
///          line is read and `None` when stdin is at EOF before any bytes are
///          available. Fatal input errors still trap.
/// @param prompt Runtime string to display before reading input; may be NULL.
/// @return Opaque `Zanna.Option` object containing the input line or `None`.
void *rt_term_try_ask(rt_string prompt) {
    rt_print_str(prompt);
    rt_output_flush();
    return rt_term_line_to_option(rt_input_line());
}

/// @brief Read a line from stdin as a Result.
/// @details Returns `Ok(String)` when a line is read and `Err(String)` when
///          stdin is at EOF before any bytes are available. Fatal input errors
///          still trap because they indicate runtime failure rather than normal
///          end-of-input.
/// @return Opaque `Zanna.Result` object containing the input line or EOF error.
void *rt_term_read_line_result(void) {
    return rt_term_line_to_result(rt_input_line(), "Zanna.Terminal.ReadLine: EOF before input");
}

/// @brief Print a prompt and read a line from stdin as a Result.
/// @details The prompt is flushed before reading. Returns `Ok(String)` when a
///          line is read and `Err(String)` when stdin is at EOF before any bytes
///          are available. Fatal input errors still trap.
/// @param prompt Runtime string to display before reading input; may be NULL.
/// @return Opaque `Zanna.Result` object containing the input line or EOF error.
void *rt_term_ask_result(rt_string prompt) {
    rt_print_str(prompt);
    rt_output_flush();
    return rt_term_line_to_result(rt_input_line(), "Zanna.Terminal.Ask: EOF before input");
}
