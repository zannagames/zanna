//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/game/rt_typewriter.h
/// @file
/// @brief Declares the reference-counted, UTF-8-aware Typewriter reveal API.
// Purpose: Character-by-character text reveal effect for dialogue, lore,
//   tutorials, and narrative text.
//
// Key invariants:
//   - Reveal rate is configurable in ms per UTF-8 codepoint.
//   - Skip() instantly reveals all remaining text.
//   - GetVisibleText() returns the currently revealed portion.
//   - Update() returns 1 on the frame the full text is revealed.
//   - Non-positive update deltas are ignored; reveal progress saturates.
//   - Invalid UTF-8 sequences advance one byte at a time rather than stalling.
//
// Ownership/Lifetime:
//   - Typewriter instances are reference-counted runtime objects.
//     rt_typewriter_destroy() releases one owned reference.
//   - Say copies its borrowed C string into private buffers. Text getters
//     return newly owned runtime strings.
//
// Links: src/runtime/game/rt_typewriter.c
//
//===----------------------------------------------------------------------===//
#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Opaque handle to a reference-counted Typewriter runtime object.
typedef struct rt_typewriter_impl *rt_typewriter;

/// @brief Runtime object class identifier used to validate Typewriter handles.
#define RT_TYPEWRITER_CLASS_ID INT64_C(-0x51020F)

/// @brief Create an empty typewriter (call `_say` to begin revealing text).
/// @details The new object is idle and incomplete, with no text and a default
///          internal reveal rate of 30 milliseconds per character.
/// @return Owned Typewriter handle, or `NULL` if allocation fails.
rt_typewriter rt_typewriter_new(void);

/// @brief Release one owned Typewriter reference.
/// @details The object and its private text buffers are reclaimed when the
///          final reference is released.
/// @param tw Owned Typewriter handle to release; `NULL` is ignored.
void rt_typewriter_destroy(void *tw);

/// @brief Begin revealing @p text one UTF-8 codepoint at a time, every @p rate_ms milliseconds.
/// @details Copies the input, replaces any prior text, and resets progress. A
///          null input is treated as empty and completes immediately. A
///          nonpositive rate selects the 30-ms default. Allocation failure
///          traps and preserves the previous state.
/// @param tw Borrowed Typewriter handle to configure.
/// @param text Borrowed null-terminated UTF-8 text; may be `NULL`.
/// @param rate_ms Milliseconds per revealed sequence.
void rt_typewriter_say(void *tw, const char *text, int64_t rate_ms);

/// @brief Advance by @p dt milliseconds. Returns 1 on the frame the full text is revealed.
/// @details Accumulates positive time with saturation and reveals as many
///          complete character intervals as fit, preserving the remainder.
///          Inactive and completed instances are unchanged.
/// @param tw Borrowed Typewriter handle to advance.
/// @param dt Elapsed milliseconds; nonpositive values are ignored.
/// @return `1` only when this call reveals the final character; otherwise `0`.
int8_t rt_typewriter_update(void *tw, int64_t dt);

/// @brief Instantly reveal all remaining characters (skip the typewriter animation).
/// @details Marks loaded text complete and inactive. An object with no loaded
///          text remains unchanged.
/// @param tw Borrowed Typewriter handle to complete.
void rt_typewriter_skip(void *tw);

/// @brief Discard the active text and reset the reveal counter.
/// @details Releases both private buffers and returns to the idle, incomplete
///          state.
/// @param tw Borrowed Typewriter handle to reset.
void rt_typewriter_reset(void *tw);

/// @brief Get the currently-revealed prefix of the active text (empty until first Update tick).
/// @param tw Borrowed Typewriter handle to query.
/// @return Newly owned runtime string containing the visible prefix, or an
///         owned empty string when no text is loaded.
rt_string rt_typewriter_get_visible_text(void *tw);

/// @brief Get the complete text (revealed and pending).
/// @param tw Borrowed Typewriter handle to query.
/// @return Newly owned runtime string containing the copied full text, or an
///         owned empty string when no text is loaded.
rt_string rt_typewriter_get_full_text(void *tw);

/// @brief True if any text is currently being revealed (between Say and full reveal).
/// @param tw Borrowed Typewriter handle to query.
/// @return `1` while nonempty text is active and incomplete; otherwise `0`.
int8_t rt_typewriter_is_active(void *tw);

/// @brief True if all characters have been revealed.
/// @details Empty input completes immediately; a new or reset object is not
///          considered complete.
/// @param tw Borrowed Typewriter handle to query.
/// @return `1` when completion is latched; otherwise `0`.
int8_t rt_typewriter_is_complete(void *tw);

/// @brief Reveal progress as a percentage (0–100).
/// @details Uses revealed UTF-8 sequence counts and truncates fractional
///          percentages. Completed empty text reports 100.
/// @param tw Borrowed Typewriter handle to query.
/// @return Percentage in the inclusive range 0..100, or `0` for null/idle.
int64_t rt_typewriter_progress(void *tw);

/// @brief Number of characters revealed so far.
/// @param tw Borrowed Typewriter handle to query.
/// @return Revealed UTF-8 sequence count, or `0` for null.
int64_t rt_typewriter_char_count(void *tw);

/// @brief Total characters in the active text.
/// @param tw Borrowed Typewriter handle to query.
/// @return Total UTF-8 sequence count, or `0` for null/unloaded text.
int64_t rt_typewriter_total_chars(void *tw);

#ifdef __cplusplus
}
#endif
