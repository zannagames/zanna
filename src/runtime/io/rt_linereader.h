//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/io/rt_linereader.h
// Purpose: Line-by-line text file reading for Zanna.IO.LineReader, stripping newline characters
// (CR, LF, CRLF) and providing character-level peek/read access.
//
// Key invariants:
//   - Read strips the line terminator (CR, LF, or CRLF) from each line.
//   - ReadChar returns the raw character as i64, or -1 at EOF.
//   - PeekChar views the next character without advancing the position.
//   - The reader buffers input internally for efficiency.
//   - Opaque receivers are validated for class and complete payload size.
//   - Native files and temporary read buffers remain cleanup-owned across
//     recoverable allocation traps.
//
// Ownership/Lifetime:
//   - LineReader objects are runtime-managed opaque handles. Callers should close them promptly;
//     the registered finalizer closes a forgotten native stream when the object is reclaimed.
//   - Returned strings from Read and ReadAll are runtime-managed references owned by the caller.
//   - Receiver and path arguments are borrowed for each call and are never retained.
//
// Links: src/runtime/io/rt_linereader.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//
/**
 * @file
 * @brief Declares the managed buffered text LineReader API.
 * @details Exposes binary-mode opening, line reads with uniform terminator
 * stripping, raw byte read and peek, remaining-content reads, sticky EOF
 * observation, and explicit close for validated opaque reader handles.
 */
#pragma once

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Open a text file for line-by-line reading.
/// @details Opens in binary mode so LF, CR, and CRLF are recognized uniformly.
///          If managed wrapper allocation traps after the native file opens,
///          the native stream is closed before the diagnostic propagates.
/// @param path File path as runtime string.
/// @return Owned LineReader object; traps and returns NULL on failure.
void *rt_linereader_open(rt_string path);

/// @brief Close the line reader and release resources.
/// @details Idempotently closes the native stream. The runtime object remains valid but unusable
///          until its references are released normally.
/// @param obj Borrowed LineReader object; NULL is ignored.
void rt_linereader_close(void *obj);

/// @brief Read one line from the file.
/// @details Strips one LF, CR, or CRLF terminator. Temporary staging storage is
///          released even if construction of the returned runtime string traps.
/// @param obj LineReader object.
/// @return Line as string (without newline), or empty string at EOF.
rt_string rt_linereader_read(void *obj);

/// @brief Read a single character from the file.
/// @details Reads one raw byte without interpreting line endings. Invalid or closed receivers and
///          native I/O failures trap.
/// @param obj LineReader object.
/// @return Character code (0-255) or -1 on EOF.
int64_t rt_linereader_read_char(void *obj);

/// @brief Peek at the next character without consuming it.
/// @details Stores one raw byte in the reader's lookahead slot so repeated peeks return the same
///          value. Invalid or closed receivers and native I/O failures trap.
/// @param obj LineReader object.
/// @return Character code (0-255) or -1 on EOF.
int64_t rt_linereader_peek_char(void *obj);

/// @brief Read all remaining content from the file.
/// @details Preserves raw line-ending bytes and includes a pending peeked byte.
///          Temporary storage is released on I/O and result-allocation traps.
/// @param obj LineReader object.
/// @return Remaining content as string.
rt_string rt_linereader_read_all(void *obj);

/// @brief Check if at end of file.
/// @details Reports the sticky flag set by a previous read that encountered EOF; it does not probe
///          the native stream.
/// @param obj Borrowed LineReader object; may be NULL.
/// @return 1 for NULL, closed, or EOF readers; otherwise 0.
int8_t rt_linereader_eof(void *obj);

#ifdef __cplusplus
}
#endif
