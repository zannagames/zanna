//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/core/rt_string_builder.h
// Purpose: Declares the small-buffer-aware dynamic byte builder used by runtime
// formatting and the Zanna.Text.StringBuilder native bridge.
//
// Key invariants:
//   - The 128-byte inline buffer holds up to 127 content bytes plus the NUL.
//   - len < cap invariant holds at all times; the NUL terminator is excluded from len.
//   - data[len] is NUL after initialization and every successful operation.
//   - Low-level operations return rt_sb_status_t rather than trapping; bridge
//     methods translate failures into typed runtime traps.
//   - Capacity is the total active-buffer allocation, including NUL space.
//
// Ownership/Lifetime:
//   - Builder owns its backing buffer (inline or heap-allocated).
//   - Call rt_sb_init before use and rt_sb_free when finished, including after
//     append failure, so promoted heap storage is released.
//   - Append sources are borrowed and copied; they must not overlap the active
//     builder buffer when raw-byte copying could reallocate it.
//   - Bridge ToString returns an independent owned snapshot and leaves the
//     builder reusable.
//
// Links: src/runtime/core/rt_string_builder.c (implementation), src/runtime/core/rt_string.h,
// src/runtime/core/rt_printf_compat.h
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Growable byte-builder state, status API, and runtime object bridge.
#pragma once

#include "rt_string.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Total inline-buffer capacity, including one byte for the terminator.
#define RT_SB_INLINE_CAPACITY 128

/// @brief Status codes returned by string builder operations.
/// @details Low-level builder routines never translate these statuses into
///          traps; callers may recover, clean up, or raise context-specific
///          diagnostics.
typedef enum {
    RT_SB_OK = 0,         ///< Operation completed successfully.
    RT_SB_ERROR_ALLOC,    ///< Memory allocation failed.
    RT_SB_ERROR_OVERFLOW, ///< Size computation overflowed the platform limit.
    RT_SB_ERROR_INVALID,  ///< Caller supplied invalid arguments.
    RT_SB_ERROR_FORMAT    ///< Formatting helper reported an error.
} rt_sb_status_t;

/// @brief Small-buffer string builder used by the runtime.
/// @details Embeds a fixed-size inline buffer to avoid heap allocation for
///          content through 127 bytes. Growth promotes @ref data to owned heap
///          storage. @ref len always excludes the NUL while @ref cap includes
///          its required byte.
typedef struct rt_string_builder {
    char *data; ///< Points to the active buffer (inline or heap-allocated).
    size_t len; ///< Current number of bytes in use (excluding NUL).
    size_t cap; ///< Capacity of @ref data in bytes.
    char inline_buffer[RT_SB_INLINE_CAPACITY]; ///< Inline storage for the small-buffer fast
                                               ///< path.
} rt_string_builder;

/// @brief Initialize a string builder with inline small-buffer storage.
/// @details Selects @ref rt_string_builder::inline_buffer, resets the content
///          length, and writes its initial terminator. NULL is accepted as a
///          no-op. This function does not free pre-existing builder storage.
/// @param sb Builder storage to initialize; may be NULL.
/// @post For non-null @p sb, `data == inline_buffer`, `len == 0`, and
///       `cap == RT_SB_INLINE_CAPACITY`.
void rt_sb_init(rt_string_builder *sb);

/// @brief Free any heap storage associated with the builder.
/// @details Releases the heap buffer (if the builder spilled from inline storage)
///          and resets fields to the same empty inline state produced by
///          @ref rt_sb_init. NULL is a no-op, and the non-null builder is
///          immediately reusable without another initialization call.
/// @param sb Initialized or zero-initialized builder to reset; may be NULL.
void rt_sb_free(rt_string_builder *sb);

/// @brief Ensure capacity for at least @p required bytes.
/// @details Grows backing storage (typically geometrically) when the current
///          capacity is insufficient and preserves existing content and its
///          terminator. Requests below `len + 1` are normalized to that minimum.
/// @param sb Initialized builder; may be NULL.
/// @param required Requested total capacity in bytes, including terminator
///        space.
/// @return @ref RT_SB_OK on success, @ref RT_SB_ERROR_INVALID for NULL
///         @p sb, or @ref RT_SB_ERROR_ALLOC if growth fails.
/// @post On success, `sb->cap >= max(required, sb->len + 1)`.
rt_sb_status_t rt_sb_reserve(rt_string_builder *sb, size_t required);

/// @brief Append a NUL-terminated C string to the builder.
/// @details Measures through the first NUL, then delegates to
///          @ref rt_sb_append_bytes. Input need not be UTF-8 but must not
///          overlap the active builder buffer.
/// @param sb Initialized destination builder; may be NULL.
/// @param text Borrowed NUL-terminated byte string; may be NULL.
/// @return @ref RT_SB_ERROR_INVALID for null arguments; otherwise the
///         raw-byte append status.
/// @post On success, `sb->len` increases by `strlen(text)`.
rt_sb_status_t rt_sb_append_cstr(rt_string_builder *sb, const char *text);

/// @brief Append a fixed-length byte sequence to the builder.
/// @details Copies arbitrary bytes including embedded NULs, appends a separate
///          terminator, and rejects overflowing final sizes. The source must
///          not overlap active builder storage because reserve may relocate it
///          and copying uses `memcpy`.
/// @param sb Initialized destination builder; may be NULL.
/// @param text Borrowed byte span; may be NULL only when @p len is zero.
/// @param len Number of bytes to copy.
/// @return @ref RT_SB_OK, or an invalid, overflow, or allocation status.
rt_sb_status_t rt_sb_append_bytes(rt_string_builder *sb, const char *text, size_t len);

/// @brief Append a signed 64-bit integer as decimal text.
/// @details Writes locale-independent signed base-10 text directly into
///          reserved builder storage without an intermediate runtime string.
/// @param sb Initialized destination builder; may be NULL.
/// @param value Integer value to format.
/// @return @ref RT_SB_OK, or an invalid, overflow, allocation, or formatting
///         status.
rt_sb_status_t rt_sb_append_int(rt_string_builder *sb, int64_t value);

/// @brief Append a double-precision float as decimal text.
/// @details Uses the runtime's locale-independent floating-point formatter,
///          including its canonical non-finite spellings. If formatted content
///          does not fit after reserve, the saved logical state is restored.
/// @param sb Initialized destination builder; may be NULL.
/// @param value Double-precision value to format.
/// @return @ref RT_SB_OK, or an invalid, overflow, or allocation status.
rt_sb_status_t rt_sb_append_double(rt_string_builder *sb, double value);

/// @brief Append formatted text using a printf-style format string.
/// @details Calls platform `vsnprintf` directly in unused builder storage and
///          retries after geometric or exact-size growth when truncated.
///          Conversion details follow the active C locale.
/// @param sb Initialized destination builder; may be NULL.
/// @param fmt Borrowed printf-style format string; may be NULL.
/// @param ... Variadic arguments per @p fmt.
/// @return @ref RT_SB_OK, or an invalid, formatting, overflow, or allocation
///         status.
rt_sb_status_t rt_sb_printf(rt_string_builder *sb, const char *fmt, ...);

// --- Zanna.Text.StringBuilder runtime bridge ---
// These adapters implement the Zanna.Text.StringBuilder object surface by
// operating on the embedded rt_string_builder stored inside the opaque
// object (see rt_sb_bridge.c for the layout and construction helper).

/// @brief Return the builder length in bytes from an opaque StringBuilder object.
/// @details A null receiver raises invalid operation. A value beyond
///          `INT64_MAX` raises overflow and saturates if the trap hook returns.
/// @param sb Borrowed opaque StringBuilder receiver.
/// @return Accumulated byte count, zero after a returning null trap, or
///         `INT64_MAX` after a returning overflow trap.
int64_t rt_text_sb_get_length(void *sb);

/// @brief Return the builder capacity in bytes from an opaque StringBuilder object.
/// @details Includes space reserved for the terminator. Null and overflow
///          conditions trap with the same fallbacks as
///          @ref rt_text_sb_get_length.
/// @param sb Borrowed opaque StringBuilder receiver.
/// @return Total active-buffer capacity, zero after a returning null trap, or
///         `INT64_MAX` after a returning overflow trap.
int64_t rt_text_sb_get_capacity(void *sb);

/// @brief Append a runtime string and return the receiver for chaining.
/// @details Copies stored bytes including embedded NULs. NULL @p s means an
///          empty append. A null receiver or builder error raises a typed trap;
///          if its hook returns, the original receiver is still returned.
/// @param sb Borrowed opaque StringBuilder receiver.
/// @param s Borrowed runtime string; may be NULL.
/// @return The original @p sb pointer.
void *rt_text_sb_append(void *sb, rt_string s);

/// @brief Append a runtime string followed by a newline character.
/// @details Treats NULL @p s as empty but always adds exactly one LF byte,
///          independent of platform newline conventions. Size/reserve failures
///          trap before copying content.
/// @param sb Borrowed opaque StringBuilder receiver.
/// @param s Borrowed runtime string; may be NULL.
/// @return The original @p sb pointer.
void *rt_text_sb_append_line(void *sb, rt_string s);

/// @brief Materialize the builder contents as a runtime string.
/// @details Copies all bytes into an immutable owned snapshot and leaves the
///          builder unchanged. Empty content or a returning null-receiver trap
///          uses the shared empty singleton.
/// @param sb Borrowed opaque StringBuilder receiver.
/// @return Owned snapshot or empty singleton, or `NULL` if allocation or
///         singleton initialization fails.
rt_string rt_text_sb_to_string(void *sb);

/// @brief Clear the builder contents.
/// @details Resets length and writes a NUL at byte zero while retaining inline
///          or heap capacity for reuse. A null receiver traps and otherwise
///          performs no mutation if its hook returns.
/// @param sb Borrowed opaque StringBuilder receiver.
/// @post For a valid receiver, length is zero and capacity is unchanged.
void rt_text_sb_clear(void *sb);

#ifdef __cplusplus
}
#endif
