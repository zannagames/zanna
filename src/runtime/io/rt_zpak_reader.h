//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/io/rt_zpak_reader.h
// Purpose: Reader for ZPAK (Zanna Pack Archive) binary format. Parses archives
//          from either memory buffers (embedded in .rodata) or file handles
//          (mounted .zpak pack files).
//
// Key invariants:
//   - zpak_open_memory() does not copy the blob; caller must keep it alive.
//   - Open functions return one ownership reference; zpak_retain/zpak_close
//     permit readers to outlive source-registry updates safely.
//   - File-backed entry reads serialize seek/read access to their shared FILE*.
//   - zpak_read_entry() returns a malloc'd buffer that the caller must free.
//   - TOC entries are sorted by name for binary search in zpak_find().
//   - Version 1 remains readable; version 2 verifies CRC-32 after each read.
//   - Unsupported flags, reserved fields, unsafe names, overlapping payloads,
//     and count/TOC-size inconsistencies are rejected before publication.
//
// Ownership/Lifetime:
//   - The final archive reference owns its TOC, read mutex, and file handle.
//   - Returned entry data buffers are caller-owned (must be freed).
//
// Links: src/tools/common/asset/ZpakWriter.hpp (build-time writer),
//        src/runtime/io/rt_asset.h (asset manager)
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares the validated ZPAK archive reader and ownership API.
 * @details Exposes decoded archive and entry metadata, memory and file open
 * paths, explicit reference management, name lookup, integrity-checked entry
 * extraction, and the borrowing rules for archive-owned metadata and blobs.
 */

#pragma once

#include "rt_zpak_format.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief A single entry in a ZPAK table of contents.
/// @details Instances are archive-owned, name-sorted after validation, and
///          exposed as borrowed pointers by @ref zpak_find.
typedef struct {
    char *name;           ///< Asset name (heap-allocated, owned by archive).
    uint64_t data_offset; ///< Byte offset of data in blob/file.
    uint64_t data_size;   ///< Original uncompressed size.
    uint64_t stored_size; ///< Size in archive (may differ if compressed).
    uint32_t crc32;       ///< Version 2 CRC-32 of uncompressed bytes; zero for version 1.
    int compressed;       ///< 1 if entry is DEFLATE-compressed.
} zpak_entry_t;

/// @brief A parsed ZPAK archive (memory-backed or file-backed).
/// @details Exactly one of @c blob and @c file is populated. Public callers
///          should manage the structure through the open/retain/close API
///          rather than mutating these decoded fields.
typedef struct {
    zpak_entry_t *entries; ///< Array of TOC entries (sorted by name).
    uint32_t count;        ///< Number of entries.
    uint16_t version;      ///< Validated ZPAK format version (1 or 2).
    uint16_t flags;        ///< Validated version-specific header flags.

    const uint8_t *blob; ///< Non-NULL for memory-backed (embedded) archives.
    size_t blob_size;    ///< Size of memory blob.
    FILE *file;          ///< Non-NULL for file-backed (mounted) archives.
    uint64_t file_size;  ///< Size of file-backed archive, 0 for memory-backed.
    size_t refcount;     ///< Atomic ownership count; initialized to one.
    void *read_lock;     ///< Internal native mutex for shared file positioning.
} zpak_archive_t;

/// @brief Open a ZPAK archive from a memory buffer.
/// @details The buffer must remain valid for the lifetime of the archive. Both
///          legacy version 1 and checksummed version 2 are accepted. Header,
///          TOC, names, flags, payload ranges, and duplicate names are fully
///          validated before a result is returned. Entry metadata and names
///          are copied, but payload bytes remain borrowed.
/// @param data Complete ZPAK bytes to borrow without copying.
/// @param size Number of readable bytes at @p data.
/// @return Parsed archive with one ownership reference, or NULL on error.
zpak_archive_t *zpak_open_memory(const uint8_t *data, size_t size);

/// @brief Open a ZPAK archive from a file on disk.
/// @details The file handle is kept open until zpak_close(). The complete TOC
///          is validated at open, while payload bytes are read on demand under
///          the archive's positioning lock.
/// @param path NUL-terminated native UTF-8 path to a ZPAK file.
/// @return Parsed archive with one ownership reference, or NULL on error.
zpak_archive_t *zpak_open_file(const char *path);

/// @brief Open a regular ZPAK file without following its final symlink.
/// @details POSIX uses a no-follow descriptor open plus a regular-file check;
///          Windows delegates to the normal Unicode file open because pack
///          discovery separately rejects reparse points. All format validation
///          is identical to @ref zpak_open_file.
/// @param path Native UTF-8 path to the candidate pack.
/// @return Parsed archive with one owned reference, or NULL on any failure.
zpak_archive_t *zpak_open_file_no_follow(const char *path);

/// @brief Acquire an additional ownership reference to a live archive.
/// @details Uses an atomic compare/exchange so registry code can retain an
///          archive before releasing its own mutex. The function never traps.
/// @param archive Archive returned by a ZPAK open function.
/// @return Non-zero when retained, zero for NULL, closed, or saturated input.
int zpak_retain(zpak_archive_t *archive);

/// @brief Find an entry by name.
///
/// Uses a linear scan for small archives and binary search on larger sorted TOCs.
///
/// @param archive Live parsed ZPAK archive.
/// @param name Exact NUL-terminated asset name to search for.
/// @return Borrowed archive-owned entry pointer, or NULL if not found.
const zpak_entry_t *zpak_find(const zpak_archive_t *archive, const char *name);

/// @brief Read entry data from the archive.
/// @details If the entry is compressed, the data is decompressed via DEFLATE.
///          Version 2 validates CRC-32 over the uncompressed result before
///          returning ownership. Version 1 retains its checksum-free legacy
///          behavior. A successful empty entry still returns a freeable
///          non-NULL allocation.
/// @param archive Live archive that owns @p entry.
/// @param entry Entry obtained from @ref zpak_find on @p archive.
/// @param out_size Set to the uncompressed data size on success and zero
///                 before fallible read work.
/// @return Malloc-owned data buffer for the caller to free, or NULL on error.
uint8_t *zpak_read_entry(const zpak_archive_t *archive,
                         const zpak_entry_t *entry,
                         size_t *out_size);

/// @brief Release one ZPAK archive ownership reference.
///
/// The final release frees the TOC and entry names, destroys the per-archive
/// read mutex, and closes the file handle when file-backed.
///
/// @param archive  Archive to close (may be NULL).
void zpak_close(zpak_archive_t *archive);

#ifdef __cplusplus
}
#endif
