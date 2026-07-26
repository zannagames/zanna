//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/io/rt_archive.h
// Purpose: ZIP archive support for Zanna.IO.Archive, providing reading of existing archives and
// writing of new archives with per-entry compression.
//
// Key invariants:
//   - Archives opened for reading are read-only; no modification is possible.
//   - Write archives require rt_archive_finish before the output file is valid.
//   - Entry names use forward-slash separators on all platforms.
//   - Open validates central and local records before exposing any entry.
//   - Configured encoded, per-entry, and aggregate byte ceilings are sampled
//     per Archive object and apply to both readers and writers.
//   - Writer mutation and Count/Names snapshots are serialized per archive.
//
// Ownership/Lifetime:
//   - Archive objects are GC-managed opaque pointers.
//   - Callers should not free archive objects directly.
//   - Each concurrent caller must hold an owning runtime reference for the
//     complete duration of its call.
//
// Links: src/runtime/io/rt_archive.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//
/**
 * @file
 * @brief Declares managed ZIP archive reading, extraction, and creation.
 * @details Exposes bounded file- and memory-backed readers, entry metadata
 * and content access, traversal-safe extraction, transactional writer
 * mutation, atomic finalization, and non-throwing ZIP classification.
 */
#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//=========================================================================
// Archive Creation and Opening
//=========================================================================

/// @brief Open an existing ZIP archive for reading.
/// @param path File path as runtime string.
/// @return Archive object for reading.
/// @note Traps if the file is absent, malformed, unsupported, or exceeds a
///       configured resource ceiling.
void *rt_archive_open(rt_string path);

/// @brief Create a new ZIP archive for writing.
/// @param path File path as runtime string.
/// @return Archive object for writing.
/// @note Probes destination writability without replacing an existing file;
///       the atomic replacement occurs only after a successful Finish.
void *rt_archive_create(rt_string path);

/// @brief Open a ZIP archive from Bytes in memory.
/// @param data Bytes object containing ZIP data.
/// @return Archive object for reading.
/// @note Copies the encoded bytes and traps for malformed/unsupported data or
///       a configured resource-ceiling violation.
void *rt_archive_from_bytes(void *data);

//=========================================================================
// Properties
//=========================================================================

/// @brief Get the file path of the archive.
/// @param obj Archive object.
/// @return File path or empty string if opened from bytes.
rt_string rt_archive_path(void *obj);

/// @brief Get the number of entries in the archive.
/// @param obj Archive object.
/// @return Number of entries.
int64_t rt_archive_count(void *obj);

/// @brief Get all entry names as a Seq.
/// @param obj Archive object.
/// @return Fresh Seq of independently owned entry-name strings. The result
///         remains valid after the archive is released.
void *rt_archive_names(void *obj);

//=========================================================================
// Reading Methods
//=========================================================================

/// @brief Check if an entry exists in the archive.
/// @param obj Archive object.
/// @param name Entry name.
/// @return 1 if entry exists, 0 otherwise.
int8_t rt_archive_has(void *obj, rt_string name);

/// @brief Read an entry as Bytes.
/// @param obj Archive object.
/// @param name Entry name.
/// @return Bytes object with entry contents.
/// @note Traps if entry not found, unsupported compression, or CRC mismatch.
void *rt_archive_read(void *obj, rt_string name);

/// @brief Read an entry as a string.
/// @param obj Archive object.
/// @param name Entry name.
/// @return String with entry contents (UTF-8).
/// @note Traps if entry not found, unsupported compression, or CRC mismatch.
rt_string rt_archive_read_str(void *obj, rt_string name);

/// @brief Extract an entry to a file on disk.
/// @param obj Archive object.
/// @param name Entry name.
/// @param dest_path Destination file path.
/// @note Traps if entry not found or write fails.
void rt_archive_extract(void *obj, rt_string name, rt_string dest_path);

/// @brief Extract all entries to a directory.
/// @param obj Archive object.
/// @param dest_dir Destination directory path.
/// @note Creates directories as needed. Traps on write failures.
void rt_archive_extract_all(void *obj, rt_string dest_dir);

/// @brief Get metadata for an entry.
/// @param obj Archive object.
/// @param name Entry name.
/// @return Map with keys: "size" (i64), "compressedSize" (i64),
///         "modifiedTime" (i64), "isDirectory" (i1).
/// @note Traps if entry not found.
void *rt_archive_info(void *obj, rt_string name);

//=========================================================================
// Writing Methods (only valid for archives created with Create)
//=========================================================================

/// @brief Add a Bytes entry to the archive.
/// @param obj Archive object (must be opened for writing).
/// @param name Entry name.
/// @param data Bytes object with entry contents.
/// @note Traps if archive is read-only.
void rt_archive_add(void *obj, rt_string name, void *data);

/// @brief Add a string entry to the archive.
/// @param obj Archive object (must be opened for writing).
/// @param name Entry name.
/// @param text String with entry contents.
/// @note Traps if archive is read-only.
void rt_archive_add_str(void *obj, rt_string name, rt_string text);

/// @brief Add a file from disk to the archive.
/// @param obj Archive object (must be opened for writing).
/// @param name Entry name in archive.
/// @param src_path Source file path on disk.
/// @note Traps if archive is read-only or source file not found.
void rt_archive_add_file(void *obj, rt_string name, rt_string src_path);

/// @brief Add a directory entry to the archive.
/// @param obj Archive object (must be opened for writing).
/// @param name Directory name (will have / appended if missing).
/// @note Traps if archive is read-only.
void rt_archive_add_dir(void *obj, rt_string name);

/// @brief Finish writing and close the archive.
/// @param obj Archive object (must be opened for writing).
/// @note Writes the central directory and end record.
///       Further writer mutations trap; Count and Names remain queryable.
///       A failed filesystem replacement rolls back the staged central
///       directory, allowing the same writer to retry Finish.
/// @note Traps if archive is read-only or already finished.
void rt_archive_finish(void *obj);

//=========================================================================
// Static Methods
//=========================================================================

/// @brief Check if a file is a valid ZIP archive.
/// @param path File path as runtime string.
/// @return 1 if valid ZIP, 0 otherwise.
int8_t rt_archive_is_zip(rt_string path);

/// @brief Check if Bytes data is a valid ZIP archive.
/// @param data Bytes object to check.
/// @return 1 if valid ZIP, 0 otherwise.
int8_t rt_archive_is_zip_bytes(void *data);

#ifdef __cplusplus
}
#endif
