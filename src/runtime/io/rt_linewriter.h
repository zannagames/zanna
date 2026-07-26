//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/io/rt_linewriter.h
// Purpose: Buffered text file writing for Zanna.IO.LineWriter, supporting creation or appending
// with Write, WriteLn, WriteChar operations and configurable line endings.
//
// Key invariants:
//   - Open mode 'append' preserves existing content; default mode truncates.
//   - WriteLn appends the configured line ending (defaults to platform native).
//   - The C stdio stream buffers output; Flush passes buffered bytes to the host descriptor but
//     does not promise crash durability.
//   - WriteChar accepts one integer byte value in the inclusive range 0..255.
//   - Opaque receivers are validated for class and complete payload size.
//   - Constructor and newline replacement allocations are transactional.
//
// Ownership/Lifetime:
//   - LineWriter objects are runtime-managed opaque handles. Callers should close promptly; the
//     finalizer closes forgotten streams and releases the retained newline reference.
//   - The writer retains its configured newline string. The newline getter returns an owned
//     runtime string reference to the caller.
//   - Path, receiver, and text arguments are borrowed for each call.
//
// Links: src/runtime/io/rt_linewriter.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//
#pragma once

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Open a text file for writing (creates or truncates).
/// @details The native file and default newline remain cleanup-owned until the
///          managed writer is fully allocated. An allocation trap closes and
///          releases both before it propagates.
/// @param path File path as runtime string.
/// @return Owned LineWriter object; traps and returns NULL on failure.
void *rt_linewriter_open(rt_string path);

/// @brief Open a text file for appending (creates if needed).
/// @details Uses binary append mode so configured newline bytes are written without translation.
/// @param path File path as runtime string.
/// @return Owned LineWriter object; traps and returns NULL on failure.
void *rt_linewriter_append(rt_string path);

/// @brief Close the line writer and flush its stdio stream.
/// @details Idempotent for NULL/already-closed writers. A close failure traps; the runtime handle
///          remains closed and its newline reference is released later by finalization.
/// @param obj Borrowed LineWriter object; may be NULL.
void rt_linewriter_close(void *obj);

/// @brief Write a string without newline.
/// @details Writes the exact runtime-string bytes and traps on invalid inputs, closed receivers,
///          oversized lengths, or short writes.
/// @param obj Borrowed open LineWriter object.
/// @param text Borrowed non-null runtime string to write.
void rt_linewriter_write(void *obj, rt_string text);

/// @brief Write a string followed by newline.
/// @details Writes @p text verbatim followed by the complete configured newline byte sequence.
/// @param obj Borrowed open LineWriter object.
/// @param text Borrowed non-null runtime string to write.
void rt_linewriter_write_ln(void *obj, rt_string text);

/// @brief Write a single character.
/// @param obj Borrowed open LineWriter object.
/// @param ch Unsigned byte value in the inclusive range 0..255; other values trap.
void rt_linewriter_write_char(void *obj, int64_t ch);

/// @brief Flush C stdio buffering to the host file descriptor.
/// @details Calls `fflush`; this drains the C buffer to the host descriptor without promising
///          crash durability. NULL/closed writers are ignored and flush failures trap.
/// @param obj Borrowed LineWriter object; may be NULL.
void rt_linewriter_flush(void *obj);

/// @brief Get the current newline string.
/// @details Returns a retained reference to the configured value. NULL receivers receive a fresh
///          platform-default string; closed valid receivers trap.
/// @param obj Borrowed LineWriter object; may be NULL.
/// @return Caller-owned runtime string reference containing the newline bytes.
rt_string rt_linewriter_newline(void *obj);

/// @brief Set the newline string transactionally.
/// @details Retains or creates the replacement before releasing the current
///          value. If allocation traps, the existing newline remains installed.
/// @param obj Borrowed open LineWriter object.
/// @param nl Borrowed replacement newline string; NULL restores the platform default, and an empty
///           string suppresses newline output.
void rt_linewriter_set_newline(void *obj, rt_string nl);

#ifdef __cplusplus
}
#endif
