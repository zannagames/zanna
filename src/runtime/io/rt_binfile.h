//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/io/rt_binfile.h
// Purpose: Binary file stream operations for Zanna.IO.BinFile, providing seekable stream-based I/O
// with read/write modes, position queries, and EOF detection.
//
// Key invariants:
//   - Open modes: 'r' (read-only), 'w' (write/truncate), 'rw' (read-write), 'a' (append).
//   - Seek origins: 0=SEEK_SET, 1=SEEK_CUR, 2=SEEK_END.
//   - For an open handle, rt_binfile_eof returns 1 only after stdio reports
//     end-of-file; NULL and closed handles conservatively report EOF.
//   - Reads past EOF return 0 bytes and set the EOF flag.
//   - Update streams insert the stdio synchronization required when switching
//     between reads and writes.
//
// Ownership/Lifetime:
//   - BinFile objects are GC-managed; callers should close promptly but never
//     free the object manually.
//   - rt_binfile_close releases the native stream. The finalizer closes any
//     stream that was not explicitly closed.
//
// Links: src/runtime/io/rt_binfile.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Seek origin constants for rt_binfile_seek.
typedef enum {
    RT_SEEK_SET = 0, ///< Seek from beginning of file.
    RT_SEEK_CUR = 1, ///< Seek from current position.
    RT_SEEK_END = 2, ///< Seek from end of file.
} rt_seek_origin_t;

/// @brief Open a binary file for streaming I/O.
/// @details The path and mode must be valid runtime strings. Supported modes
///          include `r`/`rb`, `w`/`wb`, `rw`/`r+`/`rb+`/`r+b`, and `a`/`ab`.
///          If managed-object allocation fails after the native file opens,
///          the native stream is closed before the allocation trap propagates.
/// @param path File path as runtime string.
/// @param mode Runtime string containing a supported mode.
/// @return Owned BinFile object; traps and returns NULL on failure.
void *rt_binfile_open(void *path, void *mode);

/// @brief Determine whether an opaque pointer is a complete BinFile handle.
/// @details Validation checks heap registration, object kind, class id, and
///          minimum implementation payload size before any field is accessed.
/// @param obj Candidate runtime pointer; may be NULL or unrelated.
/// @return One for a valid BinFile handle, otherwise zero.
int8_t rt_binfile_is_handle(void *obj);

/// @brief Close the binary file and release resources.
/// @details `NULL` and already-closed handles are no-ops. Native close failure
/// raises a trap after the handle is marked closed.
/// @param obj BinFile object.
void rt_binfile_close(void *obj);

/// @brief Read bytes from file into a Bytes object.
/// @param obj BinFile object.
/// @param bytes Target Bytes object.
/// @param offset Starting offset in the Bytes object.
/// @param count Maximum number of bytes to read.
/// @return Number of bytes actually read, 0 for an empty/clamped read, or -1
/// after a native read error trap.
int64_t rt_binfile_read(void *obj, void *bytes, int64_t offset, int64_t count);

/// @brief Write bytes from a Bytes object to file.
/// @param obj BinFile object.
/// @param bytes Source Bytes object.
/// @param offset Starting offset in the Bytes object.
/// @param count Number of bytes to write.
/// @note Negative offsets become zero and counts are clamped to the available
/// source range. A partial native write raises a trap.
void rt_binfile_write(void *obj, void *bytes, int64_t offset, int64_t count);

/// @brief Read a single byte from the file.
/// @param obj BinFile object.
/// @return Byte value (0-255) or -1 on EOF/error.
int64_t rt_binfile_read_byte(void *obj);

/// @brief Write a single byte to the file.
/// @param obj BinFile object.
/// @param byte Byte value to write (0-255).
/// @note Out-of-range values and native write failures raise a trap.
void rt_binfile_write_byte(void *obj, int64_t byte);

/// @brief Seek to a position in the file.
/// @details Successful seeks clear EOF. Invalid origins trap; unrepresentable
/// or rejected native seeks return -1.
/// @param obj BinFile object.
/// @param offset Byte offset.
/// @param origin 0=start, 1=current, 2=end.
/// @return New position or -1 on error.
int64_t rt_binfile_seek(void *obj, int64_t offset, int64_t origin);

/// @brief Get the current file position.
/// @param obj BinFile object.
/// @return Current position or -1 on error.
int64_t rt_binfile_pos(void *obj);

/// @brief Get the file size.
/// @details Temporarily seeks to the end and restores the original position.
/// A failure to restore the position raises a trap.
/// @param obj BinFile object.
/// @return File size in bytes or -1 on error.
int64_t rt_binfile_size(void *obj);

/// @brief Flush any buffered writes to disk.
/// @details `NULL` and closed valid handles are no-ops. Native flush failure
/// raises a trap; this does not imply an OS-level `fsync`.
/// @param obj BinFile object.
void rt_binfile_flush(void *obj);

/// @brief Check if at end of file.
/// @param obj BinFile object.
/// @return 1 if a read reached EOF or the handle is `NULL`/closed; otherwise
/// 0. An unrelated runtime handle raises a trap.
int8_t rt_binfile_eof(void *obj);

#ifdef __cplusplus
}
#endif
