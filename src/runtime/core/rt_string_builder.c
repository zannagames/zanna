//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_string_builder.c
// Purpose: Implements the growable byte buffer used by runtime formatting and
//          the Zanna.Text.StringBuilder bridge. Builders begin in fixed inline
//          storage and promote to a geometrically grown heap buffer as needed.
//
// Key invariants:
//   - Builders always keep their buffers null-terminated at data[len].
//   - len excludes the terminator and remains strictly less than cap.
//   - The inline buffer (embedded in the struct) is used until capacity is
//     exceeded; data pointer is redirected to heap storage on promotion.
//   - Allocation and overflow failures are reported via explicit rt_sb_status_t
//     return codes rather than trapping; callers decide how to handle errors.
//   - Append operations preserve arbitrary bytes except C-string/printf entry
//     points, whose input necessarily terminates at NUL.
//   - rt_sb_free releases heap storage if the builder promoted beyond inline;
//     it then restores a reusable initialized empty state.
//
// Ownership/Lifetime:
//   - Callers own the rt_string_builder struct (typically stack-allocated) and
//     are responsible for calling rt_sb_free when done to release heap storage.
//   - Append inputs are borrowed and copied into builder-owned storage.
//   - Bridge ToString snapshots the bytes into a separate owned rt_string; it
//     does not consume or reset the builder.
//
// Links: src/runtime/core/rt_string_builder.h (public API),
//        src/runtime/core/rt_string.h (rt_string allocation),
//        docs/runtime/strings.md#string-builder
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Growable runtime byte builder and Zanna.Text.StringBuilder bridge.

#include "rt_string_builder.h"

#include "rt_error.h"
#include "rt_format.h"
#include "rt_int_format.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_sb_bridge.h"
#include "rt_string.h"

#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// @brief Determine whether the builder currently points at its inline buffer.
/// @details Builders start life with @ref rt_string_builder::data referencing
///          @ref rt_string_builder::inline_buffer. When the buffer grows beyond
///          the inline capacity the data pointer is redirected to heap storage.
///          This helper allows other routines to branch on that condition
///          without duplicating pointer comparisons.
/// @param sb Borrowed builder to inspect; may be NULL.
/// @return True only when @p sb is non-null and its active buffer is inline.
static bool rt_sb_is_inline(const rt_string_builder *sb) {
    return sb && sb->data == sb->inline_buffer;
}

/// @brief Restore a builder to a prior state after formatting overflow.
/// @details Restores the saved logical length/capacity and terminator. If the
///          operation began inline but promoted to heap, releases that new heap
///          allocation and reselects the embedded buffer. If it began on the
///          heap, the current allocation remains owned even if a reserve call
///          resized it. A null builder is ignored.
/// @param sb Builder whose append attempt is being rolled back; may be NULL.
/// @param original_len Saved byte length before the append.
/// @param original_cap Saved active-buffer capacity before the append.
/// @param was_inline True when the saved active buffer was embedded storage.
static void rt_sb_restore_on_overflow(rt_string_builder *sb,
                                      size_t original_len,
                                      size_t original_cap,
                                      bool was_inline) {
    if (!sb)
        return;

    if (was_inline) {
        if (!rt_sb_is_inline(sb) && sb->data)
            free(sb->data);
        sb->data = sb->inline_buffer;
    }

    sb->len = original_len;
    sb->cap = original_cap;
    sb->data[sb->len] = '\0';
}

/// @brief Initialize @p sb to an empty inline-backed builder.
/// @details Selects embedded storage, records its complete capacity including
///          terminator space, and writes the initial NUL. This routine does not
///          release any heap pointer already stored in @p sb; use
///          @ref rt_sb_free for an initialized builder. A null pointer is a
///          no-op.
/// @param sb Builder storage to initialize; may be NULL.
void rt_sb_init(rt_string_builder *sb) {
    if (!sb)
        return;
    sb->data = sb->inline_buffer;
    sb->len = 0;
    sb->cap = RT_SB_INLINE_CAPACITY;
    sb->inline_buffer[0] = '\0';
}

/// @brief Release any heap storage owned by the builder and reset it to empty.
/// @details Frees the heap buffer when the builder outgrew the inline storage,
///          resets bookkeeping fields, and reinstates the inline buffer as the
///          active storage. After the call the builder is indistinguishable from
///          a freshly initialised instance.
/// @param sb Initialized builder to reset; NULL is ignored.
void rt_sb_free(rt_string_builder *sb) {
    if (!sb)
        return;
    if (!rt_sb_is_inline(sb) && sb->data)
        free(sb->data);
    sb->data = sb->inline_buffer;
    sb->len = 0;
    sb->cap = RT_SB_INLINE_CAPACITY;
    sb->inline_buffer[0] = '\0';
}

/// @brief Grow the builder capacity to at least @p new_cap bytes.
/// @details Returns immediately if current capacity suffices. Otherwise inline
///          content is copied into a new allocation, while existing heap
///          storage is passed to `realloc`. The existing pointer, bytes,
///          length, and capacity remain usable when allocation fails.
/// @param sb Initialized builder to grow; may be NULL.
/// @param new_cap Desired total buffer capacity, including terminator space.
/// @return @ref RT_SB_OK on success, @ref RT_SB_ERROR_INVALID for null
///         @p sb, or @ref RT_SB_ERROR_ALLOC when allocation fails.
static rt_sb_status_t rt_sb_grow(rt_string_builder *sb, size_t new_cap) {
    if (!sb)
        return RT_SB_ERROR_INVALID;
    if (new_cap <= sb->cap)
        return RT_SB_OK;

    char *new_data = NULL;
    if (rt_sb_is_inline(sb)) {
        new_data = (char *)malloc(new_cap);
        if (!new_data)
            return RT_SB_ERROR_ALLOC;
        memcpy(new_data, sb->inline_buffer, sb->len + 1);
    } else {
        new_data = (char *)realloc(sb->data, new_cap);
        if (!new_data)
            return RT_SB_ERROR_ALLOC;
    }

    sb->data = new_data;
    sb->cap = new_cap;
    return RT_SB_OK;
}

/// @brief Ensure the builder can store @p required bytes including the terminator.
/// @details Normalizes requests below the current `len + 1`, then doubles
///          capacity until the requested total fits. Near `SIZE_MAX`, it uses
///          the exact request rather than overflowing the growth factor.
/// @param sb Initialized builder whose storage may grow; may be NULL.
/// @param required Minimum total capacity in bytes, including room for the
///        current or future NUL terminator.
/// @return @ref RT_SB_OK, @ref RT_SB_ERROR_INVALID for a null builder, or the
///         allocation status from the grow operation.
rt_sb_status_t rt_sb_reserve(rt_string_builder *sb, size_t required) {
    if (!sb)
        return RT_SB_ERROR_INVALID;
    if (required <= sb->len + 1)
        required = sb->len + 1;
    if (required <= sb->cap)
        return RT_SB_OK;

    size_t new_cap = sb->cap;
    if (new_cap == 0)
        new_cap = RT_SB_INLINE_CAPACITY;

    while (new_cap < required) {
        if (new_cap > SIZE_MAX / 2) {
            new_cap = required;
            break;
        }
        new_cap *= 2;
    }

    if (new_cap < required)
        new_cap = required;

    return rt_sb_grow(sb, new_cap);
}

/// @brief Append raw bytes to the builder without performing formatting.
/// @details Validates arguments, expands the buffer to fit @p len additional
///          bytes plus the null terminator, copies the data, and updates the
///          length bookkeeping. Embedded NULs are preserved. Because copying
///          uses `memcpy` and reserve may relocate storage, @p text must not
///          overlap the builder's active buffer.
/// @param sb Initialized destination builder; may be NULL.
/// @param text Borrowed bytes to append; may be NULL only when @p len is zero.
/// @param len Number of bytes to copy from @p text.
/// @return @ref RT_SB_OK, or an invalid, overflow, or allocation status. A
///         zero-length append succeeds without inspecting @p text.
rt_sb_status_t rt_sb_append_bytes(rt_string_builder *sb, const char *text, size_t len) {
    if (!sb || (!text && len > 0))
        return RT_SB_ERROR_INVALID;
    if (len == 0)
        return RT_SB_OK;

    if (len > SIZE_MAX - sb->len - 1)
        return RT_SB_ERROR_OVERFLOW;

    rt_sb_status_t status = rt_sb_reserve(sb, sb->len + len + 1);
    if (status != RT_SB_OK)
        return status;

    memcpy(sb->data + sb->len, text, len);
    sb->len += len;
    sb->data[sb->len] = '\0';
    return RT_SB_OK;
}

/// @brief Append a null-terminated C string to the builder.
/// @details Rejects a null source and otherwise measures through the first NUL
///          before forwarding to @ref rt_sb_append_bytes. The bytes need not be
///          valid UTF-8. The source must obey that function's non-aliasing rule.
/// @param sb Initialized destination builder; may be NULL.
/// @param text Borrowed NUL-terminated byte string; may be NULL.
/// @return @ref RT_SB_ERROR_INVALID for null input, otherwise the raw-byte
///         append status.
rt_sb_status_t rt_sb_append_cstr(rt_string_builder *sb, const char *text) {
    if (!text)
        return RT_SB_ERROR_INVALID;
    return rt_sb_append_bytes(sb, text, strlen(text));
}

/// @brief Append the decimal representation of a signed 64-bit integer.
/// @details Reserves a fixed 32-byte allowance, writes locale-independent
///          base-10 text directly into the unused buffer, and advances the
///          logical length by the formatter's reported byte count.
/// @param sb Initialized destination builder; may be NULL.
/// @param value Signed value to format.
/// @return @ref RT_SB_OK or an invalid, overflow, allocation, or formatting
///         status.
rt_sb_status_t rt_sb_append_int(rt_string_builder *sb, int64_t value) {
    if (!sb)
        return RT_SB_ERROR_INVALID;

    const size_t extra = 32;
    if (extra > SIZE_MAX - sb->len - 1)
        return RT_SB_ERROR_OVERFLOW;

    rt_sb_status_t status = rt_sb_reserve(sb, sb->len + extra);
    if (status != RT_SB_OK)
        return status;

    size_t avail = sb->cap - sb->len;
    size_t written = rt_i64_to_cstr(value, sb->data + sb->len, avail);
    if (written == 0 && sb->data[sb->len] == '\0')
        return RT_SB_ERROR_FORMAT;
    if (written + 1 > avail)
        return RT_SB_ERROR_OVERFLOW;

    sb->len += written;
    sb->data[sb->len] = '\0';
    return RT_SB_OK;
}

/// @brief Append a floating-point value formatted with BASIC semantics.
/// @details Reserves a 64-byte allowance and invokes @ref rt_format_f64 in the
///          unused tail. The original logical state is restored if the
///          resulting length would exceed available capacity. Formatting
///          follows the runtime's locale-independent finite/non-finite rules.
/// @param sb Initialized destination builder; may be NULL.
/// @param value Double-precision value to format.
/// @return @ref RT_SB_OK or an invalid, overflow, or allocation status.
rt_sb_status_t rt_sb_append_double(rt_string_builder *sb, double value) {
    if (!sb)
        return RT_SB_ERROR_INVALID;

    const size_t extra = 64;
    if (extra > SIZE_MAX - sb->len - 1)
        return RT_SB_ERROR_OVERFLOW;

    size_t original_len = sb->len;
    size_t original_cap = sb->cap;
    bool was_inline = rt_sb_is_inline(sb);

    rt_sb_status_t status = rt_sb_reserve(sb, sb->len + extra);
    if (status != RT_SB_OK)
        return status;

    size_t avail = sb->cap - sb->len;
    rt_format_f64(value, sb->data + sb->len, avail);
    size_t appended = strlen(sb->data + sb->len);

    if (appended + 1 > avail) {
        rt_sb_restore_on_overflow(sb, original_len, original_cap, was_inline);
        return RT_SB_ERROR_OVERFLOW;
    }

    sb->len += appended;
    if (sb->len >= sb->cap) {
        rt_sb_restore_on_overflow(sb, original_len, original_cap, was_inline);
        return RT_SB_ERROR_OVERFLOW;
    }

    sb->data[sb->len] = '\0';
    return RT_SB_OK;
}

/// @brief Append formatted text using @c vsnprintf semantics.
/// @details Repeats `vsnprintf` with a fresh copy of @p args, reserving either
///          doubled capacity or the exact reported requirement after
///          truncation. Formatting follows the active C locale and appends
///          through the first generated NUL.
/// @param sb Initialized destination builder; may be NULL.
/// @param fmt Borrowed printf format string; may be NULL.
/// @param args Variadic values matching @p fmt; copied but not ended here.
/// @return @ref RT_SB_OK or an invalid, formatting, overflow, or allocation
///         status.
static rt_sb_status_t rt_sb_vprintf_internal(rt_string_builder *sb, const char *fmt, va_list args) {
    if (!sb || !fmt)
        return RT_SB_ERROR_INVALID;

    va_list copy;
    while (1) {
        size_t avail = (sb->cap > sb->len) ? (sb->cap - sb->len) : 0;
        if (avail < 2) {
            size_t target = sb->cap ? sb->cap * 2 : RT_SB_INLINE_CAPACITY;
            if (sb->cap > SIZE_MAX / 2)
                target = SIZE_MAX;
            rt_sb_status_t status = rt_sb_reserve(sb, target);
            if (status != RT_SB_OK)
                return status;
            continue;
        }

        va_copy(copy, args);
#if !defined(_MSC_VER)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
        int written = vsnprintf(sb->data + sb->len, avail, fmt, copy);
#if !defined(_MSC_VER)
#pragma GCC diagnostic pop
#endif
        va_end(copy);

        if (written < 0)
            return RT_SB_ERROR_FORMAT;

        if ((size_t)written + 1 > avail) {
            size_t needed = sb->len + (size_t)written + 1;
            if (needed < sb->len)
                return RT_SB_ERROR_OVERFLOW;
            rt_sb_status_t status = rt_sb_reserve(sb, needed);
            if (status != RT_SB_OK)
                return status;
            continue;
        }

        sb->len += (size_t)written;
        sb->data[sb->len] = '\0';
        return RT_SB_OK;
    }
}

/// @brief Append printf-formatted bytes to @p sb.
/// @details Initializes and closes a `va_list` around
///          @ref rt_sb_vprintf_internal. The active C locale and platform
///          `vsnprintf` implementation determine conversion details.
/// @param sb Initialized destination builder; may be NULL.
/// @param fmt Borrowed printf format string; may be NULL.
/// @param ... Variadic arguments consumed by @p fmt.
/// @return Status from @ref rt_sb_vprintf_internal.
rt_sb_status_t rt_sb_printf(rt_string_builder *sb, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    rt_sb_status_t status = rt_sb_vprintf_internal(sb, fmt, args);
    va_end(args);
    return status;
}

// --------------------
// Bridge functions for Zanna.Text.StringBuilder
// These functions provide the runtime interface for the OOP StringBuilder class.
// The StringBuilder object layout has a vptr at offset 0 and an embedded
// rt_string_builder struct at offset 8 (after the vptr).

#include <assert.h>

/// @brief Native storage layout embedded in a Zanna.Text.StringBuilder object.
/// @details The vtable pointer precedes builder state; normal C layout and
///          alignment rules determine the exact platform-specific offset.
typedef struct {
    void *vptr;                // vtable pointer (8 bytes)
    rt_string_builder builder; // embedded builder state
} StringBuilder;

/// @brief Extract mutable builder state from an opaque StringBuilder receiver.
/// @details A null receiver raises an invalid-operation trap. If its handler
///          returns, this helper returns NULL so bridge methods can use their
///          documented fallback.
/// @param sb Borrowed opaque pointer to a @ref StringBuilder object.
/// @return Borrowed pointer to embedded builder state, or `NULL` after a
///         returning null-receiver trap.
static rt_string_builder *get_builder(void *sb) {
    if (!sb) {
        rt_trap_raise_kind(RT_TRAP_KIND_INVALID_OPERATION,
                           Err_InvalidOperation,
                           -1,
                           "StringBuilder: null receiver");
        return NULL;
    }

    StringBuilder *obj = (StringBuilder *)sb;
    // The builder is embedded right after the vptr
    return &obj->builder;
}

/// @brief Translate an `rt_sb_status_t` into the matching runtime trap kind.
/// @details Success is a no-op. Allocation maps to runtime error, overflow to
///          overflow, and invalid/format/unknown statuses to invalid operation.
///          This function returns only if the selected trap hook returns.
/// @param op Borrowed diagnostic message passed to the trap subsystem.
/// @param status Builder status to translate.
static void rt_text_sb_trap_status(const char *op, rt_sb_status_t status) {
    switch (status) {
        case RT_SB_OK:
            return;
        case RT_SB_ERROR_ALLOC:
            rt_trap_raise_kind(RT_TRAP_KIND_RUNTIME_ERROR, Err_RuntimeError, -1, op);
            return;
        case RT_SB_ERROR_OVERFLOW:
            rt_trap_raise_kind(RT_TRAP_KIND_OVERFLOW, Err_Overflow, -1, op);
            return;
        case RT_SB_ERROR_INVALID:
        case RT_SB_ERROR_FORMAT:
        default:
            rt_trap_raise_kind(RT_TRAP_KIND_INVALID_OPERATION, Err_InvalidOperation, -1, op);
            return;
    }
}

/// @brief Return the current content length of the embedded builder in bytes.
/// @details A null receiver traps and falls back to zero. Values beyond
///          `INT64_MAX` raise overflow and return the saturated maximum if the
///          trap handler returns.
/// @param sb Borrowed opaque StringBuilder receiver.
/// @return Stored byte length, zero after a null-receiver trap, or
///         `INT64_MAX` after a returning overflow trap.
int64_t rt_text_sb_get_length(void *sb) {
    rt_string_builder *builder = get_builder(sb);
    if (!builder)
        return 0;
    if (builder->len > (size_t)INT64_MAX) {
        rt_trap_raise_kind(
            RT_TRAP_KIND_OVERFLOW, Err_Overflow, -1, "StringBuilder.Length overflow");
        return INT64_MAX;
    }

    // Return the current string length in bytes
    return (int64_t)builder->len;
}

/// @brief Return the current allocated capacity of the embedded builder in bytes.
/// @details Capacity includes terminator space. A null receiver traps and
///          falls back to zero. Values beyond `INT64_MAX` raise overflow and
///          return the saturated maximum if the hook returns.
/// @param sb Borrowed opaque StringBuilder receiver.
/// @return Total active-buffer capacity, zero after a null-receiver trap, or
///         `INT64_MAX` after a returning overflow trap.
int64_t rt_text_sb_get_capacity(void *sb) {
    rt_string_builder *builder = get_builder(sb);
    if (!builder)
        return 0;
    if (builder->cap > (size_t)INT64_MAX) {
        rt_trap_raise_kind(
            RT_TRAP_KIND_OVERFLOW, Err_Overflow, -1, "StringBuilder.Capacity overflow");
        return INT64_MAX;
    }

    // Return the current allocated capacity in bytes
    return (int64_t)builder->cap;
}

/// @brief Append the bytes of a runtime string to the embedded builder.
/// @details Borrows @p s, preserves its stored bytes including embedded NULs,
///          and treats NULL as an empty append. A null receiver or failed append
///          traps; if its hook returns, the original receiver is still returned
///          for fluent chaining.
/// @param sb Borrowed opaque StringBuilder receiver.
/// @param s Borrowed runtime string; may be NULL.
/// @return The original @p sb pointer.
void *rt_text_sb_append(void *sb, rt_string s) {
    rt_string_builder *builder = get_builder(sb);
    if (!builder)
        return sb;
    const char *str_data = NULL;
    size_t str_len = 0;
    if (s) {
        int64_t raw_len = rt_str_len(s);
        if (raw_len < 0) {
            rt_trap_raise_kind(
                RT_TRAP_KIND_INVALID_OPERATION, Err_InvalidOperation, -1, "StringBuilder.Append");
            return sb;
        }
        str_len = (size_t)raw_len;
        str_data = rt_string_cstr(s);
    }
    rt_sb_status_t status = rt_sb_append_bytes(builder, str_data, str_len);
    if (status != RT_SB_OK) {
        rt_text_sb_trap_status("StringBuilder.Append failed", status);
        return sb;
    }

    // Return the receiver for method chaining
    return sb;
}

/// @brief Append @p s and then a single '\n' newline character.
/// @details Treats NULL @p s as empty but still appends one LF byte without
///          platform newline translation. Size arithmetic and reserve failures
///          trap before content is copied. A returning trap preserves fluent
///          receiver return semantics.
/// @param sb Borrowed opaque StringBuilder receiver.
/// @param s Borrowed runtime string; may be NULL.
/// @return The original @p sb pointer.
void *rt_text_sb_append_line(void *sb, rt_string s) {
    rt_string_builder *builder = get_builder(sb);
    if (!builder)
        return sb;
    const char *str_data = NULL;
    size_t str_len = 0;
    if (s) {
        int64_t raw_len = rt_str_len(s);
        if (raw_len < 0) {
            rt_trap_raise_kind(RT_TRAP_KIND_INVALID_OPERATION,
                               Err_InvalidOperation,
                               -1,
                               "StringBuilder.AppendLine");
            return sb;
        }
        str_len = (size_t)raw_len;
        str_data = rt_string_cstr(s);
    }

    // Reserve enough space for string bytes + '\n' + NUL terminator.
    if (str_len > SIZE_MAX - builder->len) {
        rt_trap_raise_kind(
            RT_TRAP_KIND_OVERFLOW, Err_Overflow, -1, "StringBuilder.AppendLine overflow");
        return sb;
    }
    size_t content_len = builder->len + str_len;
    if (content_len > SIZE_MAX - 2) {
        rt_trap_raise_kind(
            RT_TRAP_KIND_OVERFLOW, Err_Overflow, -1, "StringBuilder.AppendLine overflow");
        return sb;
    }
    size_t required = content_len + 2;

    rt_sb_status_t status = rt_sb_reserve(builder, required);
    if (status != RT_SB_OK) {
        rt_text_sb_trap_status("StringBuilder.AppendLine failed", status);
        return sb;
    }

    if (str_data && str_len > 0) {
        memcpy(builder->data + builder->len, str_data, str_len);
        builder->len += str_len;
    }

    builder->data[builder->len++] = '\n';
    builder->data[builder->len] = '\0';

    return sb;
}

/// @brief Materialise the builder contents as a new immutable runtime string.
/// @details Copies all stored bytes, including embedded NULs, without changing
///          builder state. Empty content returns the shared empty singleton. A
///          null receiver traps and uses the same empty fallback.
/// @param sb Borrowed opaque StringBuilder receiver.
/// @return Owned immutable snapshot or empty singleton, or `NULL` if string
///         allocation or empty-singleton initialization fails.
rt_string rt_text_sb_to_string(void *sb) {
    rt_string_builder *builder = get_builder(sb);
    if (!builder)
        return rt_str_empty();

    // Create a string from the builder's current contents
    // The builder maintains a null-terminated buffer
    if (builder->len == 0)
        return rt_str_empty();

    // Allocate and return a new string with the builder's contents
    return rt_string_from_bytes(builder->data, builder->len);
}

/// @brief Reset the builder contents to empty while preserving capacity.
/// @details Writes a terminator at byte zero and retains the current inline or
///          heap allocation for reuse. A null receiver traps and performs no
///          mutation if the handler returns.
/// @param sb Borrowed opaque StringBuilder receiver.
void rt_text_sb_clear(void *sb) {
    rt_string_builder *builder = get_builder(sb);
    if (!builder)
        return;

    // Reset the builder to empty state while keeping allocated memory
    builder->len = 0;
    if (builder->data)
        builder->data[0] = '\0';
}
