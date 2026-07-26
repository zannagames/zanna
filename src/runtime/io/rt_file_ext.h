//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/io/rt_file_ext.h
// Purpose: High-level static file operations for Zanna.IO.File, providing ReadAllText,
// WriteAllText, ReadAllLines, AppendText, Copy, Move, Delete, and Exists.
//
// Key invariants:
//   - Runtime path strings are converted through the platform path adapter.
//   - Text functions preserve runtime-string bytes verbatim; they do not validate or transcode
//     UTF-8.
//   - ReadAllLines recognizes LF, CR, and CRLF and strips those terminators.
//   - Whole-file writers stage beside the destination, durably flush, atomically replace, and
//     preserve the supported permission mode of an existing regular file.
//   - File predicates and metadata queries accept only regular files, not directories.
//
// Ownership/Lifetime:
//   - Input runtime strings, Bytes, and Seq objects are borrowed for each call.
//   - ReadAllBytes and ReadAllLines return fresh runtime-managed objects. ReadAllText returns a
//     runtime-managed string reference and may use the shared empty string for an empty file.
//   - Mutating and whole-file read operations report invalid input and I/O failure by trapping.
//     Exists, Same, Size, and Modified are deliberately non-trapping predicates/queries.
//
// Links: src/runtime/io/rt_file_ext.c (implementation),
//        src/runtime/io/rt_file_io.c (low-level handle operations),
//        src/runtime/io/rt_file_path.h (platform path conversion),
//        src/runtime/core/rt_string.h (runtime string representation)
//
//===----------------------------------------------------------------------===//
#pragma once

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Test whether a path names an existing regular file.
/// @details Invalid, missing, inaccessible, directory, and other non-regular paths return false
///          without trapping.
/// @param path Runtime path string to inspect.
/// @return 1 for an existing regular file; otherwise 0.
int64_t rt_io_file_exists(rt_string path);

/// @brief Test whether two paths name the same existing regular file.
/// @details Compares stable filesystem identity rather than path spelling.
/// @details Invalid, missing, inaccessible, and non-regular operands compare unequal without
///          trapping. Symbolic links follow the platform `stat` identity semantics.
/// @param left First runtime path to compare.
/// @param right Second runtime path to compare.
/// @return 1 when both paths resolve to the same existing file; otherwise 0.
int64_t rt_file_same(rt_string left, rt_string right);

/// @brief Read an entire regular file into a runtime string.
/// @details Preserves the file bytes verbatim without decoding or newline conversion. Invalid
///          paths, non-regular files, concurrent truncation, and I/O/allocation failures trap.
/// @param path Runtime path string to read.
/// @return Runtime string containing the complete file, or the shared empty string for an empty
///         file.
rt_string rt_io_file_read_all_text(rt_string path);

/// @brief Atomically replace a file with string contents.
/// @details Writes @p contents verbatim, durably commits the replacement, preserves the supported
///          permission mode when @p path already names a regular file, and traps on failure.
/// @param path Runtime destination path.
/// @param contents Runtime string bytes to write.
void rt_io_file_write_all_text(rt_string path, rt_string contents);

/// @brief Append a line of text to a file.
/// @details Appends @p text followed by a single '\n' byte. Creates the file
///          if it does not already exist.
/// @details Appends @p text followed by one LF byte and creates the file if absent. Windows
///          serializes the operation with a whole-file lock; all failures trap.
/// @param path Runtime path to append to.
/// @param text Runtime string to append before the LF; may be empty.
void rt_io_file_append_line(rt_string path, rt_string text);

/// @brief Read entire file as binary data.
/// @details Traps on I/O failures (missing file, permission errors, etc.).
/// @param path Runtime path of a regular file to read.
/// @return Fresh Bytes object containing the exact file contents.
void *rt_io_file_read_all_bytes(rt_string path);

/// @brief Write an entire Bytes object to a file.
/// @details Atomically replaces existing contents, preserves the permission mode of an existing
///          regular file, and traps on I/O failures.
/// @param path Runtime destination path.
/// @param bytes Borrowed Bytes object to write; must be non-null.
void rt_io_file_write_all_bytes(rt_string path, void *bytes);

/// @brief Read entire file and split it into lines.
/// @details Returns an owning Seq of strings with LF, CR, and CRLF terminators removed. Empty
///          trailing lines are preserved, and an empty file produces an empty Seq. Failures trap.
/// @param path Runtime path of a regular file to read.
/// @return Fresh owning Seq containing one runtime string per logical line.
void *rt_io_file_read_all_lines(rt_string path);

/// @brief Write a sequence of strings as lines to a file.
/// @details Validates every string element before writing, appends one LF after every element,
///          atomically replaces existing contents, and preserves supported permission bits.
/// @param path Runtime destination path.
/// @param lines Borrowed non-null Seq whose elements must all be runtime strings.
void rt_io_file_write_all_lines(rt_string path, void *lines);

/// @brief Delete a regular-file path if it exists.
/// @details A missing path succeeds silently; invalid paths and all other removal failures trap.
/// @param path Runtime path to delete.
void rt_io_file_delete(rt_string path);

/// @brief Copy a regular file without replacing an existing destination.
/// @details Streams through an adjacent sidecar, atomically commits it, and preserves source
///          permission bits and access/modification timestamps. Failures trap.
/// @param src Runtime path of the source regular file.
/// @param dst Runtime path of a destination that must not already exist.
void rt_file_copy(rt_string src, rt_string dst);

/// @brief Move/rename a file from src to dst.
/// @details Traps if @p dst already exists. Use rt_file_move_over to replace.
/// @details Uses an atomic rename on one filesystem and copy-then-delete across filesystems.
/// @param src Runtime path of the source regular file.
/// @param dst Runtime path of a destination that must not already exist.
void rt_file_move(rt_string src, rt_string dst);

/// @brief Move a regular file, replacing the destination if it exists.
/// @details Uses an atomic rename on one filesystem and metadata-preserving copy-then-delete
///          across filesystems. Failures trap.
/// @param src Runtime path of the source regular file.
/// @param dst Runtime destination path to create or replace.
void rt_file_move_over(rt_string src, rt_string dst);

/// @brief Query the size of a regular file without trapping.
/// @param path Runtime path to inspect.
/// @return Nonnegative file size in bytes, or -1 for invalid, missing, inaccessible, or
///         non-regular paths.
int64_t rt_file_size(rt_string path);

/// @brief Alias for rt_io_file_read_all_bytes().
/// @param path Runtime path of a regular file to read.
/// @return Fresh Bytes object containing the exact file contents.
void *rt_file_read_bytes(rt_string path);

/// @brief Alias for rt_io_file_write_all_bytes().
/// @details Atomically replaces existing contents and preserves the permission mode when @p path
///          already names a regular file.
/// @param path Runtime destination path.
/// @param bytes Borrowed non-null Bytes object to write.
void rt_file_write_bytes(rt_string path, void *bytes);

/// @brief Alias for rt_io_file_read_all_lines().
/// @param path Runtime path of a regular file to read.
/// @return Fresh owning Seq containing one string per logical line.
void *rt_file_read_lines(rt_string path);

/// @brief Atomically write a sequence of strings as LF-terminated lines.
/// @details Atomically replaces existing contents and preserves the permission mode when @p path
///          already names a regular file. Every element must be a valid runtime string.
/// @param path Runtime destination path.
/// @param lines Borrowed non-null Seq of runtime strings.
void rt_file_write_lines(rt_string path, void *lines);

/// @brief Append string bytes to a file without adding a newline.
/// @details Creates the file when absent and traps on invalid input or I/O failure.
/// @param path Runtime destination path.
/// @param text Runtime string whose bytes should be appended.
void rt_file_append(rt_string path, rt_string text);

/// @brief Query a regular file's modification time without trapping.
/// @param path Runtime path to inspect.
/// @return Modification time in Unix epoch seconds, or -1 for invalid, missing, inaccessible, or
///         non-regular paths.
int64_t rt_file_modified(rt_string path);

/// @brief Create an empty file or set an existing regular file's access and modification time.
/// @details Uses the current time, rejects existing non-regular paths, and traps on failure.
/// @param path Runtime path to create or update.
void rt_file_touch(rt_string path);

#ifdef __cplusplus
}
#endif
