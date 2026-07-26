//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/io/rt_archive_internal.h
// Purpose: Internal contract between the archive core (rt_archive.c) and the
//          filesystem, atomic-write, and ZIP parser companion units. Carries
//          shared archive/entry state, format constants, cleanup helpers, and
//          the platform-specific file I/O surface.
//
// Key invariants:
//   - Engine-internal; must not be included outside the io/ directory.
//   - Platform-specific declarations are guarded to match their definitions in
//     rt_archive_fs.c (Win32 handle helpers vs. POSIX fd helpers).
//   - The atomic-write helpers reject symlinked/reparse path components so an
//     archive extraction cannot escape its destination root.
//
// Ownership/Lifetime:
//   - Helpers borrow caller-owned paths/handles; Bytes-producing helpers return
//     fresh GC objects owned by the caller.
//
// Links: src/runtime/io/rt_archive.c (core/ZIP/API),
//        src/runtime/io/rt_archive_fs.c (definitions)
//
//===----------------------------------------------------------------------===//
#pragma once

#include "rt_archive.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// NOTE: the bytes_data/bytes_len wrappers are intentionally NOT shared here.
// They are trivial static-inline wrappers around the runtime.def symbols
// rt_bytes_data/rt_bytes_len; each consuming .c defines its own copy. Declaring
// them here (or defining them inline) would either collide at link time (the
// names are generic) or be mis-parsed by rtgen's header scan.

// Core-defined helpers consumed by the fs adapter (defined in rt_archive.c).
/// @brief Release one reference to a temporary runtime object eagerly.
/// @param obj Temporary object; `NULL` is accepted. Storage is reclaimed only
/// when the released reference was the last.
void archive_release_temp_object(void *obj);

/// @brief Copy the active trap diagnostic or a fallback into a fixed buffer.
/// @param buffer Writable output buffer.
/// @param buffer_size Capacity of @p buffer in bytes.
/// @param fallback Message used when the trap subsystem has no non-empty text.
void archive_save_trap_error(char *buffer, size_t buffer_size, const char *fallback);

//=============================================================================
// Filesystem / atomic-write adapter (defined in rt_archive_fs.c)
//=============================================================================

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/// @brief Open a UTF-8 Windows path through `CreateFileW`.
/// @param cpath NUL-terminated UTF-8 filesystem path.
/// @param access Requested Win32 access mask.
/// @param share Requested Win32 share mask.
/// @param create_disp Win32 creation disposition.
/// @return Open native handle, or `INVALID_HANDLE_VALUE` on failure.
HANDLE archive_open_win_path(const char *cpath, DWORD access, DWORD share, DWORD create_disp);

/// @brief Read exactly @p total bytes, close the handle, and free the buffer on failure.
/// @param h Open Windows handle positioned for input.
/// @param dst Heap buffer of at least @p total bytes.
/// @param total Exact byte count to read.
/// @param trap_msg Fallback diagnostic for failure.
/// @return 1 on success; otherwise 0 after cleanup and trap propagation.
int archive_read_exact_win_or_free(HANDLE h, uint8_t *dst, size_t total, const char *trap_msg);

/// @brief Read exactly into a Bytes object and release it if the operation fails.
/// @param h Open Windows handle; the helper closes it on every path.
/// @param bytes Temporary Bytes object receiving the payload.
/// @param total Exact byte count to read.
/// @param trap_msg Fallback diagnostic for failure.
/// @return 1 on success; otherwise 0 after cleanup and trap propagation.
int archive_read_exact_win_or_release_object(HANDLE h,
                                             void *bytes,
                                             size_t total,
                                             const char *trap_msg);

/// @brief Allocate Bytes while ensuring an open Windows handle is closed on failure.
/// @param h Handle retained by the caller on success and closed on failure.
/// @param len Requested Bytes length.
/// @param fallback Diagnostic used if allocation traps without a message.
/// @return Fresh Bytes handle on success, or `NULL` after failure cleanup.
void *archive_bytes_new_win_or_close(HANDLE h, int64_t len, const char *fallback);
#else
#include <sys/types.h>

/// @brief Open a POSIX path with close-on-exec semantics when supported.
/// @param path NUL-terminated filesystem path.
/// @param flags POSIX `open` flags.
/// @param mode Creation mode used when required by @p flags.
/// @return Open descriptor, or -1 with `errno` set.
int archive_open_posix(const char *path, int flags, mode_t mode);

/// @brief Read exactly @p total bytes, close the descriptor, and free the buffer on failure.
/// @param fd Open descriptor positioned for input.
/// @param dst Heap buffer of at least @p total bytes.
/// @param total Exact byte count to read.
/// @param trap_msg Fallback diagnostic for failure.
/// @return 1 on success; otherwise 0 after cleanup and trap propagation.
int archive_read_exact_posix_or_free(int fd, uint8_t *dst, size_t total, const char *trap_msg);

/// @brief Read exactly into a Bytes object and release it if the operation fails.
/// @param fd Open descriptor; the helper closes it on every path.
/// @param bytes Temporary Bytes object receiving the payload.
/// @param total Exact byte count to read.
/// @param trap_msg Fallback diagnostic for failure.
/// @return 1 on success; otherwise 0 after cleanup and trap propagation.
int archive_read_exact_posix_or_release_object(int fd,
                                               void *bytes,
                                               size_t total,
                                               const char *trap_msg);

/// @brief Allocate Bytes while ensuring an open descriptor is closed on failure.
/// @param fd Descriptor retained by the caller on success and closed on failure.
/// @param len Requested Bytes length.
/// @param fallback Diagnostic used if allocation traps without a message.
/// @return Fresh Bytes handle on success, or `NULL` after failure cleanup.
void *archive_bytes_new_posix_or_close(int fd, int64_t len, const char *fallback);

/// @brief Create and verify a normalized directory path beneath a trusted root.
/// @param root_fd Descriptor for the trusted extraction root.
/// @param path Forward-slash-separated relative path.
void archive_make_dirs_posix_at(int root_fd, const char *path);

/// @brief Open a file entry's verified parent directory and split out its leaf.
/// @param root_fd Descriptor for the trusted extraction root.
/// @param name Normalized relative file-entry name.
/// @param out_leaf Receives a heap-allocated leaf name owned by the caller.
/// @return Open parent descriptor, or -1 after raising a trap.
int archive_open_parent_for_file_posix(int root_fd, const char *name, char **out_leaf);

/// @brief Atomically write Bytes relative to a verified parent descriptor.
/// @param parent_fd Open destination-parent descriptor.
/// @param leaf Single destination filename.
/// @param data Runtime Bytes handle containing the payload.
void archive_write_bytes_to_dirfd_posix(int parent_fd, const char *leaf, void *data);

/// @brief Open and verify a POSIX extraction-root directory.
/// @param cdir UTF-8 path to the root.
/// @return Open root descriptor, or -1 after raising a trap.
int archive_open_root_dir_posix(const char *cdir);
#endif

// Cross-platform atomic-write / path helpers.
/// @brief Allocate and initialize the native per-archive reader-writer lock.
/// @details The platform adapter uses an SRW lock on Windows and a POSIX
///          pthread reader-writer lock elsewhere. The returned allocation has
///          no managed references and must be destroyed after archive
///          quiescence with @ref archive_rwlock_destroy.
/// @return Opaque initialized lock, or NULL when allocation/initialization fails.
void *archive_rwlock_create(void);

/// @brief Destroy a native archive lock after the final archive reference is gone.
/// @param lock Opaque lock from @ref archive_rwlock_create; NULL is a no-op.
/// @pre No thread may hold or be waiting for @p lock.
void archive_rwlock_destroy(void *lock);

/// @brief Enter shared access to an archive's stable read/property state.
/// @param lock Valid opaque archive lock.
/// @post The caller owns one shared lock acquisition until
///       @ref archive_rwlock_read_exit.
void archive_rwlock_read_enter(void *lock);

/// @brief Leave a shared archive lock acquisition.
/// @param lock Valid lock currently held once for reading by this thread.
void archive_rwlock_read_exit(void *lock);

/// @brief Enter exclusive access to archive writer buffers, indexes, and state.
/// @details Writer APIs hold this lock across their transactional mutation so
///          concurrent Add, AddDir, Finish, Count, and Names calls observe only
///          complete entry-table states.
/// @param lock Valid opaque archive lock.
void archive_rwlock_write_enter(void *lock);

/// @brief Leave an exclusive archive lock acquisition.
/// @param lock Valid lock currently held once for writing by this thread.
void archive_rwlock_write_exit(void *lock);

/// @brief Construct an exclusive sidecar path adjacent to a destination.
/// @param path Destination path whose parent directory is reused.
/// @param attempt Collision-retry number incorporated in the sidecar name.
/// @return Heap-allocated UTF-8 path owned by the caller, or `NULL` on length
/// or allocation failure. Entropy failure raises a trap.
char *archive_make_temp_path(const char *path, unsigned attempt);

/// @brief Best-effort removal of a file identified by a UTF-8 path.
/// @param path NUL-terminated path; deletion errors are ignored.
void archive_unlink_utf8(const char *path);

/// @brief Durably replace a destination with an exact byte buffer.
/// @details Writes and flushes an exclusive adjacent sidecar before an atomic
/// rename/replacement so observers never see a partial file.
/// @param cpath UTF-8 destination path.
/// @param src Source buffer containing @p total bytes.
/// @param total Number of bytes to write.
/// @param trap_msg Diagnostic raised on any failure.
/// @return 1 after durable replacement; otherwise 0 after raising a trap.
int archive_write_file_all_utf8(const char *cpath,
                                const uint8_t *src,
                                size_t total,
                                const char *trap_msg);

/// @brief Atomically write a runtime Bytes payload to a UTF-8 path.
/// @param cpath Non-empty destination path.
/// @param data Runtime Bytes handle.
void archive_write_bytes_to_path(const char *cpath, void *data);

/// @brief Compute a path length with trailing slash characters removed.
/// @param path Path buffer containing at least @p len bytes.
/// @param len Initial byte length.
/// @return Trimmed length, never reduced below one when @p len is nonzero.
size_t archive_trim_trailing_seps(const char *path, size_t len);

/// @brief Reject symlink or Windows reparse-point components in a destination path.
/// @param path Full UTF-8 destination path.
/// @param root_len Trusted leading-byte count.
/// @param include_leaf Nonzero to inspect the final component as well as its
/// parents.
void archive_reject_symlink_components(const char *path, size_t root_len, int include_leaf);

//=============================================================================
// ZIP format constants + entry/archive types (shared read/write)
//=============================================================================
#define ZIP_LOCAL_HEADER_SIG 0x04034b50
#define ZIP_CENTRAL_HEADER_SIG 0x02014b50
#define ZIP_END_RECORD_SIG 0x06054b50
#define ZIP_DATA_DESCRIPTOR_SIG 0x08074b50

#define ZIP_METHOD_STORED 0
#define ZIP_METHOD_DEFLATE 8

#define ZIP_LOCAL_HEADER_SIZE 30
#define ZIP_CENTRAL_HEADER_SIZE 46
#define ZIP_END_RECORD_SIZE 22

#define ZIP_VERSION_NEEDED 20 // 2.0 for deflate
#define ZIP_VERSION_MADE 20

#define ZIP_GP_FLAG_ENCRYPTED 0x0001u
#define ZIP_GP_FLAG_DATA_DESCRIPTOR 0x0008u
#define ZIP_GP_FLAG_STRONG_ENCRYPTION 0x0040u
#define ZIP_EXTRA_ZIP64 0x0001u

typedef struct zip_entry {
    char *name;                 ///< Entry name (heap-allocated, owned).
    uint32_t crc32;             ///< CRC-32 of the uncompressed data.
    uint32_t compressed_size;   ///< Stored size in the archive (bytes).
    uint32_t uncompressed_size; ///< Original uncompressed size (bytes).
    uint16_t method;            ///< Compression method: 0=stored, 8=DEFLATE.
    uint16_t flags;             ///< ZIP general-purpose bit flags.
    uint16_t version_needed;    ///< Minimum extractor version required.
    uint16_t mod_time;          ///< DOS-encoded modification time.
    uint16_t mod_date;          ///< DOS-encoded modification date.
    uint32_t local_offset;      ///< Byte offset of the local file header.
    bool is_directory;          ///< True when the entry name ends with '/'.
} zip_entry_t;

typedef struct rt_archive {
    rt_string path;   ///< File path string, or NULL for byte-backed archives.
    uint8_t *data;    ///< Full archive bytes (malloc'd copy or provided blob).
    size_t data_len;  ///< Length of `data` in bytes.
    bool owns_data;   ///< True when this object allocated `data` and must free it.
    bool is_writing;  ///< True when opened via `Archive.Create` (write mode).
    bool is_finished; ///< True after `Archive.Finish` has been called.
    void *rw_lock;    ///< Native reader-writer lock owned by this archive.

    uint64_t max_file_bytes;        ///< Maximum encoded archive image accepted/produced.
    uint64_t max_entry_bytes;       ///< Per-entry uncompressed allocation ceiling.
    uint64_t max_total_entry_bytes; ///< Sum of declared uncompressed entry sizes allowed.
    int parse_error;                ///< `archive_parse_error_t` from the most recent parse.

    // Read-side fields
    zip_entry_t *entries;         ///< Array of parsed central-directory entries.
    int entry_count;              ///< Number of entries in `entries`.
    size_t central_offset;        ///< First byte of the validated central directory.
    int32_t *entry_name_slots;    ///< Open-addressed name index (`entry index + 1`).
    size_t entry_name_slot_count; ///< Power-of-two capacity of `entry_name_slots`.

    // Write-side fields
    int fd;                           ///< POSIX fd used for streaming writes (-1 if unused).
    uint8_t *write_buf;               ///< In-memory write accumulation buffer.
    size_t write_len;                 ///< Current number of valid bytes in `write_buf`.
    size_t write_cap;                 ///< Allocated capacity of `write_buf`.
    zip_entry_t *write_entries;       ///< Metadata for each entry added so far.
    int write_entry_count;            ///< Number of entries in `write_entries`.
    int write_entry_cap;              ///< Allocated capacity of `write_entries`.
    uint64_t write_total_entry_bytes; ///< Sum of uncompressed sizes committed by Add.
    int32_t *write_name_slots;        ///< Open-addressed write-name index (`entry index + 1`).
    size_t write_name_slot_count;     ///< Power-of-two capacity of `write_name_slots`.
} rt_archive_t;

typedef enum { NAME_OK = 0, NAME_INVALID, NAME_OOM } name_result_t;

/// @brief Structured reason returned by the non-trapping ZIP parser.
typedef enum archive_parse_error {
    ARCHIVE_PARSE_ERROR_INVALID = 0, ///< Malformed or unsupported ZIP structure.
    ARCHIVE_PARSE_ERROR_OOM = 1,     ///< Parser-side allocation failed.
    ARCHIVE_PARSE_ERROR_LIMIT = 2,   ///< Configured resource ceiling was exceeded.
} archive_parse_error_t;

// Read-parser entry points (defined in rt_archive_read.c).
/// @brief Parse and validate an Archive's in-memory ZIP central directory.
/// @param ar Read-mode Archive containing encoded bytes and configured limits.
/// @return `true` on success; otherwise `false` with @p ar parse state reset
/// and `parse_error` set.
bool parse_central_directory(rt_archive_t *ar);

/// @brief Find an exact normalized entry name in a parsed archive.
/// @param ar Archive with a populated entry table and optional name index.
/// @param name NUL-terminated normalized entry name.
/// @return Borrowed entry metadata, or `NULL` when absent.
zip_entry_t *find_entry(rt_archive_t *ar, const char *name);

/// @brief Decode and CRC-validate one parsed ZIP entry.
/// @param ar Archive owning the encoded data buffer.
/// @param e Borrowed metadata for the entry to read.
/// @return Fresh runtime Bytes object containing uncompressed data, or `NULL`
/// after raising a validation, resource-limit, compression, or allocation trap.
void *read_entry_data(rt_archive_t *ar, zip_entry_t *e);
// Shared helpers (defined in rt_archive.c).

/// @brief Detect malformed extra-field framing or unsupported ZIP64 metadata.
/// @param extra Encoded extra-field bytes.
/// @param extra_len Accessible buffer length.
/// @return `true` when malformed or ZIP64; otherwise `false`.
bool archive_extra_is_malformed_or_zip64(const uint8_t *extra, size_t extra_len);

/// @brief Free an entry array constructed by the ZIP parser.
/// @param entries Heap array whose initialized names are owned by the entries.
/// @param count Number of initialized elements to release.
void archive_free_entry_array(zip_entry_t *entries, int count);
