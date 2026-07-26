//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/io/rt_archive.c
// Purpose: Implements ZIP archive reading and writing for the Zanna.IO.Archive
//          class. Follows the PKWARE APPNOTE specification, supporting stored
//          entries (method 0), DEFLATE-compressed entries (method 8) via
//          rt_compress, directory entries, and CRC32 validation.
//
// Key invariants:
//   - Stored entries (method 0) are written verbatim; DEFLATE entries use
//     rt_compress and are only used when they produce smaller output.
//   - CRC32 is computed and validated for every entry on both read and write.
//   - The central directory is always written at the end of the ZIP file.
//   - Directory entries have zero data length and a trailing '/' in the name.
//   - All functions are thread-safe; no global mutable state is used.
//
// Ownership/Lifetime:
//   - Entry name strings and data buffers returned to callers are fresh
//     rt_string / rt_bytes allocations owned by the caller.
//   - The archive object retains no references to extracted entry data.
//
// Links: src/runtime/io/rt_archive.h (public API),
//        src/runtime/io/rt_compress.h (DEFLATE compression used for method 8),
//        src/runtime/rt_crc32.h (CRC32 checksum utility)
//
//===----------------------------------------------------------------------===//
/**
 * @file
 * @brief Implements the managed ZIP archive API and transactional writer.
 * @details Coordinates bounded archive creation and opening, normalized
 * entry naming, CRC and DEFLATE decisions, synchronized property snapshots,
 * traversal-safe extraction, central-directory generation, rollback, and
 * durable atomic replacement.
 */

#include "rt_archive.h"
#include "rt_archive_internal.h"

#include "rt_box.h"
#include "rt_bytes.h"
#include "rt_compress.h"
#include "rt_crc32.h"
#include "rt_dir.h"
#include "rt_file_path.h"
#include "rt_internal.h"
#include "rt_io_class_ids.h"
#include "rt_map.h"
#include "rt_object.h"
#include "rt_platform.h"
#include "rt_seq.h"
#include "rt_string.h"

#if RT_PLATFORM_WINDOWS
#ifndef _CRT_RAND_S
#define _CRT_RAND_S
#endif
#endif

#include <errno.h>
#include <limits.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if RT_PLATFORM_WINDOWS
#include <io.h>
#include <windows.h>
#define PATH_SEP '\\'
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#define PATH_SEP '/'
#endif

/** @name Default and hard archive resource ceilings
 * @{ */
#define RT_ARCHIVE_DEFAULT_MAX_FILE_BYTES (UINT64_C(512) * 1024u * 1024u)
#define RT_ARCHIVE_HARD_MAX_FILE_BYTES (UINT64_C(2) * 1024u * 1024u * 1024u)
#define RT_ARCHIVE_DEFAULT_MAX_ENTRY_BYTES (UINT64_C(256) * 1024u * 1024u)
#define RT_ARCHIVE_HARD_MAX_ENTRY_BYTES (UINT64_C(1) * 1024u * 1024u * 1024u)
#define RT_ARCHIVE_DEFAULT_MAX_TOTAL_ENTRY_BYTES (UINT64_C(1) * 1024u * 1024u * 1024u)
#define RT_ARCHIVE_HARD_MAX_TOTAL_ENTRY_BYTES (UINT64_C(4) * 1024u * 1024u * 1024u)
/** @} */

/// @copydoc rt_trap_set_recovery()
void rt_trap_set_recovery(jmp_buf *buf);
/// @copydoc rt_trap_clear_recovery()
void rt_trap_clear_recovery(void);
/// @copydoc rt_trap_get_error()
const char *rt_trap_get_error(void);

//=============================================================================
// ZIP Constants
//=============================================================================


//=============================================================================
// Internal Bytes Access
//=============================================================================

// Defined here (not inline in rt_archive_internal.h) so the bodies' calls to
// the runtime.def symbols rt_bytes_data/rt_bytes_len stay out of rtgen's
// header scan, which would otherwise mis-generate their dispatch signatures.

/// @brief Return a direct pointer to the raw byte buffer of a Bytes GC object.
/// @param obj Runtime Bytes handle.
/// @return Borrowed pointer to the object's byte storage, or `NULL` when the
/// Bytes API reports no storage.
static inline uint8_t *bytes_data(void *obj) {
    return rt_bytes_data(obj);
}

/// @brief Return the byte count of a Bytes GC object.
/// @param obj Runtime Bytes handle.
/// @return Signed byte length reported by the Bytes API.
static inline int64_t bytes_len(void *obj) {
    return rt_bytes_len(obj);
}

/// @brief Drop a temporary GC object whose refcount has dropped to zero.
///
/// Used after we've materialized intermediate buffers (decompressed
/// data, throw-away byte arrays) and want to release them eagerly
/// instead of waiting for the next GC sweep.
///
/// @param obj Temporary runtime object; `NULL` is accepted. The helper
/// relinquishes one reference and frees immediately only when it was the last.
void archive_release_temp_object(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Snapshot the active runtime trap message into caller-owned storage.
/// @details Copies the current non-empty trap text when available, otherwise
/// copies @p fallback. The result is always formatted through `snprintf` and
/// is therefore NUL-terminated when @p buffer_size is nonzero.
/// @param buffer Writable destination buffer.
/// @param buffer_size Capacity of @p buffer in bytes.
/// @param fallback Message used when the trap subsystem has no active text.
void archive_save_trap_error(char *buffer, size_t buffer_size, const char *fallback) {
    const char *err = rt_trap_get_error();
    snprintf(buffer, buffer_size, "%s", err && err[0] ? err : fallback);
}

/// @brief Add an entry backed by a temporary Bytes object and then release it.
/// @details Installs a trap-recovery boundary so @p data is released on both
/// success and failure. A trapped Add diagnostic is copied before recovery is
/// cleared and then raised again, using @p fallback if no message was set.
/// @param obj Write-mode Archive handle.
/// @param name Runtime entry name forwarded to rt_archive_add().
/// @param data Temporary Bytes object whose reference this helper consumes.
/// @param fallback Diagnostic used if the trapped operation supplied none.
static void archive_add_with_temp_data(void *obj,
                                       rt_string name,
                                       void *data,
                                       const char *fallback) {
    void *volatile owned_data = data;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        archive_save_trap_error(saved_error, sizeof(saved_error), fallback);
        rt_trap_clear_recovery();
        archive_release_temp_object((void *)owned_data);
        rt_trap(saved_error);
        return;
    }

    rt_archive_add(obj, name, (void *)owned_data);
    rt_trap_clear_recovery();
    archive_release_temp_object((void *)owned_data);
}

/// @brief Extract a UTF-8 C path from an `rt_string`, trapping on failure.
///
/// Converts a Zanna string to a null-terminated C path via
/// `rt_file_path_from_vstr`. Traps with `context` if the path is
/// NULL, empty, or the conversion fails (e.g., invalid encoding).
///
/// @param path    Zanna string containing the file path.
/// @param context Trap message to emit if the path is unusable.
/// @return Non-null, non-empty UTF-8 C string.
static const char *archive_require_path(rt_string path, const char *context) {
    const char *cpath = NULL;
    if (!rt_file_path_from_vstr((const ZannaString *)path, &cpath) || !cpath || *cpath == '\0') {
        rt_trap(context);
        return "";
    }
    return cpath;
}

/// @brief Borrow a null-terminated view of an entry name from an `rt_string`.
///
/// Returns the raw UTF-8 pointer inside `name` as a C string without
/// copying. Returns NULL if `name` is empty, NULL, or contains an
/// embedded null byte (which would truncate the ZIP entry name).
///
/// @param name Zanna string containing the entry name.
/// @return Borrowed C string pointer, or NULL if the name is invalid.
static const char *archive_entry_name_cstr(rt_string name) {
    const uint8_t *data = NULL;
    size_t len = rt_file_string_view((const ZannaString *)name, &data);
    if (!data || len == 0)
        return NULL;
    if (memchr(data, '\0', len) != NULL)
        return NULL;
    return (const char *)data;
}

//=============================================================================
// ZIP Entry Structure
//=============================================================================

/// @brief Metadata for a single entry parsed from the ZIP central directory.

//=============================================================================
// Archive Structure
//=============================================================================

/// @brief Internal state for an open ZIP archive (read or write mode).

/// @brief Validate and unwrap a public Archive handle.
/// @param obj Candidate runtime object.
/// @param context Diagnostic raised when @p obj is invalid; a generic Archive
/// diagnostic is used when this pointer is `NULL`.
/// @return Internal Archive state on success, or `NULL` after raising a trap.
static rt_archive_t *archive_require(void *obj, const char *context) {
    if (!obj || !rt_obj_is_instance(obj, RT_ARCHIVE_CLASS_ID, sizeof(rt_archive_t))) {
        rt_trap(context ? context : "Archive: invalid archive");
        return NULL;
    }
    return (rt_archive_t *)obj;
}

//=============================================================================
// Little-Endian Helpers
//=============================================================================

// ZIP integers are little-endian regardless of host byte order. These
// helpers do byte-level reads/writes so the parser/writer stays correct
// on big-endian hosts and avoids strict-aliasing pitfalls.

/// @brief Read a little-endian uint16 from `p` (no alignment required).
/// @param p Pointer to at least two accessible encoded bytes.
/// @return Decoded host-order 16-bit value.
static inline uint16_t read_u16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

/// @brief Read a little-endian uint32 from `p` (no alignment required).
/// @param p Pointer to at least four accessible encoded bytes.
/// @return Decoded host-order 32-bit value.
static inline uint32_t read_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/// @brief Write `v` to `p` as little-endian uint16.
/// @param p Pointer to at least two writable bytes.
/// @param v Host-order value to encode.
static inline void write_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

/// @brief Write `v` to `p` as little-endian uint32.
/// @param p Pointer to at least four writable bytes.
/// @param v Host-order value to encode.
static inline void write_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

/// @brief Reject a malformed ZIP extra-field stream or any ZIP64 record.
/// @details Walks the standard `(header ID, data size, payload)` sequence,
/// requiring every payload and the final cursor to fit exactly within the
/// supplied buffer.
/// @param extra Encoded extra-field bytes.
/// @param extra_len Number of accessible bytes in @p extra.
/// @return `true` for a truncated record, trailing partial header, or ZIP64
/// field; otherwise `false`.
bool archive_extra_is_malformed_or_zip64(const uint8_t *extra, size_t extra_len) {
    size_t pos = 0;
    while (pos + 4 <= extra_len) {
        uint16_t header_id = read_u16(extra + pos);
        uint16_t data_size = read_u16(extra + pos + 2);
        pos += 4;
        if ((size_t)data_size > extra_len - pos)
            return true;
        if (header_id == ZIP_EXTRA_ZIP64)
            return true;
        pos += data_size;
    }
    return pos != extra_len;
}

//=============================================================================
// Archive Allocation
//=============================================================================

static void archive_finalize(void *obj);
static void archive_free_entries(rt_archive_t *ar);
static int archive_require_zip32_size(size_t size, const char *context);
static int archive_require_zip16_count(int count, const char *context);

/// @brief Parse a positive decimal resource limit and clamp it to an audited hard ceiling.
/// @details Accepts ASCII digits only and rejects empty/zero values. Arithmetic
///          saturates at @p hard_limit, so hostile or accidentally enormous
///          environment strings cannot overflow. No allocation is performed.
/// @param text Environment variable value to parse.
/// @param hard_limit Maximum value the parser may return.
/// @param out Receives the parsed and clamped value on success.
/// @return One for valid positive decimal input; zero otherwise.
static int archive_parse_byte_limit(const char *text, uint64_t hard_limit, uint64_t *out) {
    if (!text || !*text || !out || hard_limit == 0)
        return 0;
    uint64_t value = 0;
    int clamped = 0;
    for (const char *cursor = text; *cursor; ++cursor) {
        if (*cursor < '0' || *cursor > '9')
            return 0;
        uint64_t digit = (uint64_t)(*cursor - '0');
        if (!clamped) {
            if (digit > hard_limit || value > (hard_limit - digit) / 10u) {
                value = hard_limit;
                clamped = 1;
            } else {
                value = value * 10u + digit;
            }
        }
    }
    if (value == 0)
        return 0;
    *out = value > hard_limit ? hard_limit : value;
    return 1;
}

/// @brief Resolve one archive byte ceiling from process configuration.
/// @details Malformed or absent values use @p default_limit. Valid values may
///          tune the limit up to @p hard_limit but can never exceed the audited
///          allocation ceiling. Environment configuration is sampled when an
///          archive is constructed and then remains stable for that instance.
/// @param name Environment variable name.
/// @param default_limit Default ceiling in bytes.
/// @param hard_limit Absolute upper ceiling in bytes.
/// @return Effective byte limit.
static uint64_t archive_configured_byte_limit(const char *name,
                                              uint64_t default_limit,
                                              uint64_t hard_limit) {
    uint64_t parsed = 0;
    const char *value = name ? getenv(name) : NULL;
    if (!archive_parse_byte_limit(value, hard_limit, &parsed))
        return default_limit;
    return parsed;
}

/// @brief Return the configured maximum encoded archive file/buffer size.
/// @details Reads `ZANNA_ARCHIVE_MAX_FILE_BYTES`, falling back to 512 MiB and
///          clamping all configured values to the 2 GiB audited hard ceiling.
/// @return Maximum number of encoded bytes accepted by Open or FromBytes.
static uint64_t archive_max_file_bytes(void) {
    return archive_configured_byte_limit("ZANNA_ARCHIVE_MAX_FILE_BYTES",
                                         RT_ARCHIVE_DEFAULT_MAX_FILE_BYTES,
                                         RT_ARCHIVE_HARD_MAX_FILE_BYTES);
}

/// @brief Select a user-facing diagnostic for the parser's structured failure reason.
/// @param ar Archive whose @c parse_error field was set by the parser.
/// @param invalid_fallback Caller-specific malformed-ZIP diagnostic.
/// @return Static diagnostic string suitable for @ref rt_trap.
static const char *archive_parse_failure_message(const rt_archive_t *ar,
                                                 const char *invalid_fallback) {
    if (ar && ar->parse_error == ARCHIVE_PARSE_ERROR_OOM)
        return "Archive: memory allocation failed";
    if (ar && ar->parse_error == ARCHIVE_PARSE_ERROR_LIMIT)
        return "Archive: configured resource limit exceeded";
    return invalid_fallback;
}

/// @brief Allocate a zero-initialized archive object via the GC heap.
///
/// Hooks `archive_finalize` so that file paths, copied data, and entry
/// arrays are reclaimed when the GC drops the last reference. `fd` is
/// initialized to -1 (sentinel for "not open"). Traps on OOM.
///
/// @return Pointer to a fresh `rt_archive_t`, never NULL.
static rt_archive_t *archive_alloc(void) {
    size_t total = sizeof(rt_archive_t);
    rt_archive_t *ar = (rt_archive_t *)rt_obj_new_i64(RT_ARCHIVE_CLASS_ID, (int64_t)total);
    if (!ar) {
        rt_trap("Archive: memory allocation failed");
        return NULL;
    }
    memset(ar, 0, total);
    ar->fd = -1;
    ar->rw_lock = archive_rwlock_create();
    if (!ar->rw_lock) {
        archive_release_temp_object(ar);
        rt_trap("Archive: failed to initialize reader-writer lock");
        return NULL;
    }
    ar->max_file_bytes = archive_max_file_bytes();
    ar->max_entry_bytes = archive_configured_byte_limit("ZANNA_ARCHIVE_MAX_ENTRY_BYTES",
                                                        RT_ARCHIVE_DEFAULT_MAX_ENTRY_BYTES,
                                                        RT_ARCHIVE_HARD_MAX_ENTRY_BYTES);
    ar->max_total_entry_bytes =
        archive_configured_byte_limit("ZANNA_ARCHIVE_MAX_TOTAL_ENTRY_BYTES",
                                      RT_ARCHIVE_DEFAULT_MAX_TOTAL_ENTRY_BYTES,
                                      RT_ARCHIVE_HARD_MAX_TOTAL_ENTRY_BYTES);
    ar->parse_error = ARCHIVE_PARSE_ERROR_INVALID;
    rt_obj_set_finalizer(ar, archive_finalize);
    return ar;
}

/// @brief Allocate Archive state while preserving ownership of an input buffer.
/// @details If archive allocation traps, this helper frees @p data, restores
/// the captured diagnostic (or @p fallback), and transfers no ownership.
/// Successful allocation leaves @p data for the caller to attach to the new
/// Archive object.
/// @param data Heap buffer to free if allocation fails; may be `NULL`.
/// @param fallback Diagnostic used if allocation traps without a message.
/// @return Fresh Archive state on success, or `NULL` after propagating failure.
static rt_archive_t *archive_alloc_or_free_data(uint8_t *data, const char *fallback) {
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        archive_save_trap_error(saved_error, sizeof(saved_error), fallback);
        rt_trap_clear_recovery();
        free(data);
        rt_trap(saved_error);
        return NULL;
    }

    rt_archive_t *ar = archive_alloc();
    if (!ar) {
        rt_trap_clear_recovery();
        free(data);
        return NULL;
    }
    rt_trap_clear_recovery();
    return ar;
}

/// @brief Retain an Archive path with rollback if string retention traps.
/// @details On failure the Archive reference is released before the captured
/// diagnostic is raised again.
/// @param ar Newly allocated Archive state that will own the path reference.
/// @param path Runtime path string to retain.
/// @param fallback Diagnostic used if retention traps without a message.
/// @return 1 when @p path was retained, or 0 after propagating failure.
static int archive_retain_path_or_release(rt_archive_t *ar, rt_string path, const char *fallback) {
    if (!ar)
        return 0;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        archive_save_trap_error(saved_error, sizeof(saved_error), fallback);
        rt_trap_clear_recovery();
        archive_release_temp_object(ar);
        rt_trap(saved_error);
        return 0;
    }

    ar->path = rt_string_ref(path);
    rt_trap_clear_recovery();
    return 1;
}

/// @brief Free both read-side and write-side entry arrays.
///
/// Walks each entry releasing its `name` allocation, then frees the
/// containing array. Resets all counters to zero so the archive can
/// be re-parsed without leaking. Safe to call on a half-initialized
/// archive.
///
/// @param ar Archive whose read/write entry tables and name indexes are
/// released; `NULL` is accepted.
static void archive_free_entries(rt_archive_t *ar) {
    if (!ar)
        return;
    if (ar->entries && ar->entry_count > 0) {
        for (int i = 0; i < ar->entry_count; i++) {
#if RT_COMPILER_MSVC
#pragma warning(suppress : 6001)
#endif
            if (ar->entries[i].name)
                free(ar->entries[i].name);
        }
    }
    if (ar->entries) {
        free(ar->entries);
        ar->entries = NULL;
    }
    ar->entry_count = 0;
    free(ar->entry_name_slots);
    ar->entry_name_slots = NULL;
    ar->entry_name_slot_count = 0;
    ar->central_offset = 0;
    if (ar->write_entries && ar->write_entry_count > 0) {
        for (int i = 0; i < ar->write_entry_count; i++) {
#if RT_COMPILER_MSVC
#pragma warning(suppress : 6001)
#endif
            if (ar->write_entries[i].name)
                free(ar->write_entries[i].name);
        }
    }
    if (ar->write_entries) {
        free(ar->write_entries);
        ar->write_entries = NULL;
    }
    ar->write_entry_count = 0;
    ar->write_entry_cap = 0;
    ar->write_total_entry_bytes = 0;
    free(ar->write_name_slots);
    ar->write_name_slots = NULL;
    ar->write_name_slot_count = 0;
}

/// @brief Free a partially-constructed entry array on parse failure.
///
/// Releases the `name` allocation for each of the first `count` entries,
/// then frees the array itself. Used as the cleanup path inside
/// `parse_central_directory` when an error is detected mid-parse.
///
/// @param entries Array of zip_entry_t to release (may be NULL).
/// @param count   Number of entries whose `name` fields are initialized.
void archive_free_entry_array(zip_entry_t *entries, int count) {
    if (!entries)
        return;
    for (int i = 0; i < count; ++i)
#if RT_COMPILER_MSVC
#pragma warning(suppress : 6001)
#endif
        if (entries[i].name)
            free(entries[i].name);
    free(entries);
}

/// @brief GC finalizer for archive objects.
///
/// Called when the GC drops the last reference. Releases the path
/// string, frees any owned data buffer, walks the entry arrays,
/// and disposes of any pending write buffer.
///
/// @param obj Archive pointer (may be NULL).
static void archive_finalize(void *obj) {
    rt_archive_t *ar = archive_require(obj, "Archive: invalid archive");
    if (!ar)
        return;

    if (ar->path) {
        rt_string_unref(ar->path);
        ar->path = NULL;
    }
    if (ar->owns_data && ar->data) {
        free(ar->data);
        ar->data = NULL;
    }
    archive_free_entries(ar);
    free(ar->write_buf);
    ar->write_buf = NULL;
    ar->write_len = 0;
    ar->write_cap = 0;
    void *lock = ar->rw_lock;
    ar->rw_lock = NULL;
    archive_rwlock_destroy(lock);
}

/// @brief Trap if `size` would overflow the 32-bit ZIP size fields.
///
/// ZIP versions <2.0 cap individual entries / archive offsets at 4GiB.
/// We don't yet implement ZIP64 (which uses 64-bit fields in extra
/// records), so any oversized payload is rejected up front with a
/// caller-provided message.
///
/// @param size Candidate byte size or offset.
/// @param context Diagnostic raised when @p size exceeds `UINT32_MAX`.
/// @return 1 when representable by classic ZIP fields; otherwise 0 after
/// raising a trap.
static int archive_require_zip32_size(size_t size, const char *context) {
    if (size > UINT32_MAX) {
        rt_trap(context);
        return 0;
    }
    return 1;
}

/// @brief Trap if `count` would overflow the 16-bit ZIP entry counter.
///
/// The pre-ZIP64 EOCD record stores total entry count in a uint16, so
/// archives with more than 65,535 entries are rejected.
///
/// @param count Candidate number of entries.
/// @param context Diagnostic raised when @p count is negative or exceeds
/// `UINT16_MAX`.
/// @return 1 when representable by the classic EOCD; otherwise 0 after
/// raising a trap.
static int archive_require_zip16_count(int count, const char *context) {
    if (count < 0 || count > UINT16_MAX) {
        rt_trap(context);
        return 0;
    }
    return 1;
}

//=============================================================================
// Writing Helpers
//=============================================================================

/// @brief Grow the in-memory write buffer so `need` more bytes will fit.
///
/// Doubles capacity geometrically until the required length fits, falling
/// back to the exact required length near `SIZE_MAX`. The configured encoded
/// archive limit may also cap the allocation at the exact requirement.
/// Traps on OOM and is a cheap no-op when existing capacity is sufficient.
///
/// @param ar Write-mode Archive owning the output buffer.
/// @param need Additional byte count the caller intends to append.
/// @return 1 when the buffer has sufficient capacity; otherwise 0 after
/// raising an invalid-state, overflow, resource-limit, or allocation trap.
static int write_ensure(rt_archive_t *ar, size_t need) {
    if (!ar) {
        rt_trap("Archive: invalid archive");
        return 0;
    }
    if (need > SIZE_MAX - ar->write_len) {
        rt_trap("Archive: write buffer size overflow");
        return 0;
    }
    size_t required = ar->write_len + need;
    if ((uint64_t)required > ar->max_file_bytes) {
        rt_trap("Archive: encoded output exceeds configured resource limit");
        return 0;
    }
    if (required <= ar->write_cap)
        return 1;

    size_t new_cap = ar->write_cap ? ar->write_cap : 4096;
    while (new_cap < required) {
        if (new_cap > SIZE_MAX / 2) {
            new_cap = required;
            break;
        }
        new_cap *= 2;
    }
    if ((uint64_t)new_cap > ar->max_file_bytes)
        new_cap = required;
    uint8_t *new_buf = (uint8_t *)realloc(ar->write_buf, new_cap);
    if (!new_buf) {
        rt_trap("Archive: memory allocation failed");
        return 0;
    }
    ar->write_buf = new_buf;
    ar->write_cap = new_cap;
    return 1;
}

/// @brief Append `len` bytes to the write buffer, growing it as needed.
///
/// Single-call wrapper around `write_ensure` + memcpy. Used as the
/// only path for appending bytes during archive construction so the
/// growth policy is centralized.
///
/// @param ar Write-mode Archive receiving the encoded bytes.
/// @param data Source buffer; may be `NULL` only when @p len is zero.
/// @param len Number of bytes to append.
/// @return 1 after appending all bytes; otherwise 0 after raising a trap.
static int write_bytes(rt_archive_t *ar, const uint8_t *data, size_t len) {
    if (len > 0 && !data) {
        rt_trap("Archive: invalid write source");
        return 0;
    }
    if (!write_ensure(ar, len))
        return 0;
    if (len > 0)
        memcpy(ar->write_buf + ar->write_len, data, len);
    ar->write_len += len;
    return 1;
}

/// @brief Compute the FNV-1a hash used by the write-side archive name index.
/// @param name Normalized, NUL-terminated entry name.
/// @return Stable 64-bit hash value.
static uint64_t archive_write_name_hash(const char *name) {
    uint64_t hash = UINT64_C(14695981039346656037);
    const unsigned char *cursor = (const unsigned char *)name;
    while (cursor && *cursor) {
        hash ^= (uint64_t)*cursor++;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

/// @brief Insert an existing write entry into a supplied open-addressed index.
/// @details Slots encode `entry_index + 1`, leaving zero as the empty marker.
///          Linear probing compares full names to preserve correctness under
///          hash collisions and reports duplicates without modifying an
///          occupied slot.
/// @param entries Write-entry table containing @p entry_index.
/// @param slots Zero-initialized slot table.
/// @param slot_count Power-of-two slot-table capacity.
/// @param entry_index Entry index to insert.
/// @return One on insertion; zero for duplicate or malformed/full input.
static int archive_write_index_insert(zip_entry_t *entries,
                                      int32_t *slots,
                                      size_t slot_count,
                                      int entry_index) {
    if (!entries || !slots || slot_count == 0 || (slot_count & (slot_count - 1)) != 0 ||
        entry_index < 0 || !entries[entry_index].name)
        return 0;
    size_t slot = (size_t)archive_write_name_hash(entries[entry_index].name) & (slot_count - 1);
    for (size_t probe = 0; probe < slot_count; ++probe) {
        int32_t encoded = slots[slot];
        if (encoded == 0) {
            slots[slot] = (int32_t)entry_index + 1;
            return 1;
        }
        int existing = encoded - 1;
        if (existing >= 0 && entries[existing].name &&
            strcmp(entries[existing].name, entries[entry_index].name) == 0)
            return 0;
        slot = (slot + 1) & (slot_count - 1);
    }
    return 0;
}

/// @brief Ensure the writer's name index can hold @p needed_entries at 0.5 load.
/// @details Rebuilds into a geometrically larger power-of-two table before the
///          entry array is mutated. Allocation failure leaves the old index
///          untouched, allowing callers to roll back an Add transaction cleanly.
/// @param ar Write-mode archive.
/// @param needed_entries Entry count required after the pending insertion.
/// @return One when sufficient indexed capacity exists; zero on overflow or OOM.
static int archive_write_index_ensure(rt_archive_t *ar, int needed_entries) {
    if (!ar || needed_entries < 0)
        return 0;
    if (needed_entries == 0)
        return 1;
    if (ar->write_name_slots && ar->write_name_slot_count > 0 &&
        (size_t)needed_entries <= ar->write_name_slot_count / 2)
        return 1;

    size_t needed = (size_t)needed_entries;
    if (needed > SIZE_MAX / 2)
        return 0;
    needed *= 2;
    size_t new_count = 16;
    while (new_count < needed) {
        if (new_count > SIZE_MAX / 2)
            return 0;
        new_count *= 2;
    }
    if (new_count > SIZE_MAX / sizeof(int32_t))
        return 0;
    int32_t *new_slots = (int32_t *)calloc(new_count, sizeof(*new_slots));
    if (!new_slots)
        return 0;
    for (int index = 0; index < ar->write_entry_count; ++index) {
        if (!archive_write_index_insert(ar->write_entries, new_slots, new_count, index)) {
            free(new_slots);
            return 0;
        }
    }
    free(ar->write_name_slots);
    ar->write_name_slots = new_slots;
    ar->write_name_slot_count = new_count;
    return 1;
}

/// @brief Append a `zip_entry_t` to the writing-side entry table.
///
/// Verifies the new total stays within the 16-bit ZIP entry limit
/// (or traps with a ZIP64 message), then doubles the capacity of
/// `write_entries` if full. The supplied `*e` is copied by value —
/// the caller may reuse the source struct after the call.
///
/// @param ar Write-mode Archive whose entry table and name index are updated.
/// @param e Fully initialized entry metadata; its owned name pointer transfers
/// into the table only on success.
/// @return 1 when the entry was indexed and appended; otherwise 0 after
/// raising a limit, duplicate-name, overflow, or allocation trap.
static int add_write_entry(rt_archive_t *ar, zip_entry_t *e) {
    if (!ar || !e) {
        rt_trap("Archive: invalid archive entry");
        return 0;
    }
    if (!archive_require_zip16_count(ar->write_entry_count + 1,
                                     "Archive: ZIP64 archives are not supported"))
        return 0;
    uint64_t entry_size = (uint64_t)e->uncompressed_size;
    if (entry_size > ar->max_entry_bytes || entry_size > ar->max_total_entry_bytes ||
        ar->write_total_entry_bytes > ar->max_total_entry_bytes - entry_size) {
        rt_trap("Archive: entry data exceeds configured resource limit");
        return 0;
    }
    if (!archive_write_index_ensure(ar, ar->write_entry_count + 1)) {
        rt_trap("Archive: entry index allocation failed");
        return 0;
    }
    if (ar->write_entry_count >= ar->write_entry_cap) {
        if (ar->write_entry_cap > INT_MAX / 2) {
            rt_trap("Archive: entry capacity overflow");
            return 0;
        }
        int new_cap = ar->write_entry_cap == 0 ? 16 : ar->write_entry_cap * 2;
        if ((size_t)new_cap > SIZE_MAX / sizeof(zip_entry_t)) {
            rt_trap("Archive: entry allocation overflow");
            return 0;
        }
        zip_entry_t *new_entries =
            (zip_entry_t *)realloc(ar->write_entries, new_cap * sizeof(zip_entry_t));
        if (!new_entries) {
            rt_trap("Archive: memory allocation failed");
            return 0;
        }
        if (new_cap > ar->write_entry_cap) {
            memset(new_entries + ar->write_entry_cap,
                   0,
                   (size_t)(new_cap - ar->write_entry_cap) * sizeof(zip_entry_t));
        }
        ar->write_entries = new_entries;
        ar->write_entry_cap = new_cap;
    }
    int new_index = ar->write_entry_count;
    ar->write_entries[new_index] = *e;
    if (!archive_write_index_insert(
            ar->write_entries, ar->write_name_slots, ar->write_name_slot_count, new_index)) {
        memset(&ar->write_entries[new_index], 0, sizeof(ar->write_entries[new_index]));
        rt_trap("Archive: duplicate or invalid entry name");
        return 0;
    }
    ar->write_entry_count++;
    ar->write_total_entry_bytes += entry_size;
    return 1;
}

/// @brief Test whether the write-side table already contains an exact name.
/// @details Uses the open-addressed index when initialized and falls back to a
/// linear scan for half-constructed or legacy state.
/// @param ar Archive whose pending write entries are searched.
/// @param name Normalized, NUL-terminated entry name.
/// @return 1 for an exact match; otherwise 0, including invalid input.
static int archive_write_has_entry(rt_archive_t *ar, const char *name) {
    if (!ar || !name)
        return 0;
    if (ar->write_name_slots && ar->write_name_slot_count > 0 &&
        (ar->write_name_slot_count & (ar->write_name_slot_count - 1)) == 0) {
        size_t slot = (size_t)archive_write_name_hash(name) & (ar->write_name_slot_count - 1);
        for (size_t probe = 0; probe < ar->write_name_slot_count; ++probe) {
            int32_t encoded = ar->write_name_slots[slot];
            if (encoded == 0)
                return 0;
            int index = encoded - 1;
            if (index >= 0 && index < ar->write_entry_count && ar->write_entries[index].name &&
                strcmp(ar->write_entries[index].name, name) == 0)
                return 1;
            slot = (slot + 1) & (ar->write_name_slot_count - 1);
        }
        return 0;
    }
    for (int i = 0; i < ar->write_entry_count; ++i) {
        if (ar->write_entries[i].name && strcmp(ar->write_entries[i].name, name) == 0)
            return 1;
    }
    return 0;
}

/// @brief Result enum for `normalize_name` — distinguishes invalid input
///        from out-of-memory so callers can pick the right trap message.

/// @brief Canonicalize an entry name to ZIP-safe POSIX-style form.
///
/// Rejects absolute paths (`/foo`, `\\foo`, `C:foo`) and any segment
/// equal to `..` (path-traversal guard for `Extract`). Drops `.` and
/// empty segments, collapses `\\` to `/`, and emits `/`-separated
/// output in `*out`. The caller takes ownership of `*out` (free with
/// `free()`).
///
/// @param name Caller-supplied entry name (UTF-8).
/// @param out  Out-parameter for the normalized buffer.
/// @return NAME_OK on success, NAME_INVALID for forbidden inputs,
///         NAME_OOM on allocation failure.
static name_result_t normalize_name(const char *name, char **out) {
    if (!name || !*name)
        return NAME_INVALID;

    // Reject absolute paths and drive letters.
    if (name[0] == '/' || name[0] == '\\')
        return NAME_INVALID;
    if (strlen(name) >= 2 && name[1] == ':')
        return NAME_INVALID;

    size_t len = strlen(name);
    char *result = (char *)malloc(len + 1);
    if (!result)
        return NAME_OOM;

    size_t j = 0;
    const char *p = name;
    while (*p) {
        while (*p == '/' || *p == '\\')
            ++p;
        if (!*p)
            break;

        const char *seg_start = p;
        while (*p && *p != '/' && *p != '\\')
            ++p;
        size_t seg_len = (size_t)(p - seg_start);
        if (seg_len == 0)
            continue;

        if (seg_len == 1 && seg_start[0] == '.')
            continue;
        if (seg_len == 2 && seg_start[0] == '.' && seg_start[1] == '.') {
            free(result);
            return NAME_INVALID;
        }
        if (memchr(seg_start, ':', seg_len)) {
            free(result);
            return NAME_INVALID;
        }

        if (j > 0)
            result[j++] = '/';
        memcpy(result + j, seg_start, seg_len);
        j += seg_len;
    }

    if (j == 0) {
        free(result);
        return NAME_INVALID;
    }

    result[j] = '\0';
    *out = result;
    return NAME_OK;
}

/// @brief Whether `name` ends in `/` or `\\` (i.e., looks like a directory).
///
/// Used to disambiguate `Has("foo")` (file) from `Has("foo/")`
/// (directory entry) since ZIP records the trailing slash.
///
/// @param name NUL-terminated entry name to inspect.
/// @return `true` when the final byte is `/` or `\\`; otherwise `false`,
/// including for `NULL` or empty strings.
static bool name_ends_with_sep(const char *name) {
    if (!name)
        return false;
    size_t len = strlen(name);
    if (len == 0)
        return false;
    return name[len - 1] == '/' || name[len - 1] == '\\';
}

/// @brief Ensure `name` ends with a `/`, reallocating in place if needed.
///
/// Takes ownership of `name` and either returns it unchanged (already
/// ends in `/`) or returns a fresh malloc'd buffer with the slash
/// appended. On allocation failure, frees `name` and traps — callers
/// don't need to clean up.
///
/// @param name Caller-owned name buffer.
/// @return The (possibly reallocated) name with trailing `/`.
static char *ensure_trailing_slash(char *name) {
    size_t len = strlen(name);
    if (len > 0 && name[len - 1] == '/')
        return name;
    if (len > SIZE_MAX - 2) {
        free(name);
        rt_trap("Archive: entry name too long");
        return NULL;
    }
    char *new_name = (char *)realloc(name, len + 2);
    if (!new_name) {
        free(name);
        rt_trap("Archive: memory allocation failed");
        return NULL;
    }
    new_name[len] = '/';
    new_name[len + 1] = '\0';
    return new_name;
}

/// @brief Insert a pre-boxed value into a map under a C-string key.
/// @details Wraps the borrowed C string in an immortal runtime-string view and
///          delegates to @ref rt_map_set, which retains @p boxed on success.
///          The caller continues to own its original boxed reference. The key
///          view needs no recovery cleanup because @ref rt_const_cstr returns
///          an immortal value; this property lets the enclosing Info
///          transaction reclaim only its map and active box after a trap.
///
/// @param map      Target `rt_map` object.
/// @param key_cstr Null-terminated key string (borrowed for the call).
/// @param boxed    Boxed primitive value (Map takes a reference).
static void archive_map_set_boxed(void *map, const char *key_cstr, void *boxed) {
    if (!map || !key_cstr)
        return;
    rt_string key = rt_const_cstr(key_cstr);
    rt_map_set(map, key, boxed);
    rt_string_unref(key);
}

/// @brief Encode a UTC broken-down time as DOS time/date words.
/// @details ZIP timestamps use FAT/DOS fields: date bits 0-4 day, 5-8 month,
///          9-15 year-since-1980; time bits 0-4 sec/2, 5-10 minute, 11-15 hour.
///          Years outside DOS' representable [1980, 2107] range are clamped.
/// @param tm UTC calendar time to encode.
/// @param dos_time Out DOS time word.
/// @param dos_date Out DOS date word.
static void archive_tm_to_dos_datetime(const struct tm *tm,
                                       uint16_t *dos_time,
                                       uint16_t *dos_date) {
    int year = tm ? tm->tm_year + 1900 : 2001;
    int month = tm ? tm->tm_mon + 1 : 1;
    int day = tm ? tm->tm_mday : 1;
    int hour = tm ? tm->tm_hour : 0;
    int minute = tm ? tm->tm_min : 0;
    int second = tm ? tm->tm_sec : 0;

    if (year < 1980) {
        year = 1980;
        month = 1;
        day = 1;
        hour = minute = second = 0;
    } else if (year > 2107) {
        year = 2107;
        month = 12;
        day = 31;
        hour = 23;
        minute = 59;
        second = 58;
    }
    if (month < 1)
        month = 1;
    if (month > 12)
        month = 12;
    if (day < 1)
        day = 1;
    if (day > 31)
        day = 31;
    if (hour < 0)
        hour = 0;
    if (hour > 23)
        hour = 23;
    if (minute < 0)
        minute = 0;
    if (minute > 59)
        minute = 59;
    if (second < 0)
        second = 0;
    if (second > 59)
        second = 59;

    *dos_time = (uint16_t)(((hour & 0x1F) << 11) | ((minute & 0x3F) << 5) | ((second / 2) & 0x1F));
    *dos_date = (uint16_t)((((year - 1980) & 0x7F) << 9) | ((month & 0x0F) << 5) | (day & 0x1F));
}

/// @brief Convert Unix epoch seconds to UTC broken-down time.
/// @details Routes through @ref rt_gmtime_r so the platform-specific Windows
///          and POSIX conversion details stay in the runtime platform
///          adapter instead of this archive writer.
/// @param epoch Seconds since the Unix epoch.
/// @param out_tm Output calendar time.
/// @return 1 on success, 0 if the epoch cannot be represented by the C runtime.
static int archive_gmtime_utc(time_t epoch, struct tm *out_tm) {
    if (!out_tm)
        return 0;
    return rt_gmtime_r(&epoch, out_tm) != NULL;
}

/// @brief Parse an environment-provided non-negative epoch seconds value.
/// @details Accepts only a complete base-10 integer and rejects overflow both in `strtoll`
///          and in the target platform's `time_t` representation.
/// @param text Environment variable text.
/// @param out_epoch Parsed epoch seconds.
/// @return 1 on success, 0 on invalid or out-of-range input.
static int archive_parse_epoch_seconds(const char *text, time_t *out_epoch) {
    char *end = NULL;
    long long parsed;
    time_t narrowed;
    if (!text || !*text || !out_epoch)
        return 0;
    errno = 0;
    parsed = strtoll(text, &end, 10);
    if (errno == ERANGE || end == text || (end && *end != '\0') || parsed < 0)
        return 0;
    narrowed = (time_t)parsed;
    if ((long long)narrowed != parsed)
        return 0;
    *out_epoch = narrowed;
    return 1;
}

/// @brief Produce DOS time/date words for a new ZIP entry.
///
/// Defaults to a fixed `2001-01-01 00:00:00` timestamp so archives remain
/// byte-for-byte reproducible for identical inputs. `SOURCE_DATE_EPOCH` is honored when set for
/// reproducible-build integrations; `ZANNA_ARCHIVE_TIMESTAMP=now` opts into the current UTC time.
///
/// @param dos_time Receives the encoded FAT time word.
/// @param dos_date Receives the encoded FAT date word.
static void get_dos_time(uint16_t *dos_time, uint16_t *dos_date) {
    const char *mode = getenv("ZANNA_ARCHIVE_TIMESTAMP");
    const char *source_date_epoch = getenv("SOURCE_DATE_EPOCH");
    time_t epoch = (time_t)0;
    struct tm tm_value;

    if (mode && strcmp(mode, "now") == 0) {
        epoch = time(NULL);
        if (archive_gmtime_utc(epoch, &tm_value)) {
            archive_tm_to_dos_datetime(&tm_value, dos_time, dos_date);
            return;
        }
    } else if (archive_parse_epoch_seconds(source_date_epoch, &epoch) &&
               archive_gmtime_utc(epoch, &tm_value)) {
        archive_tm_to_dos_datetime(&tm_value, dos_time, dos_date);
        return;
    }

    *dos_time = 0;                        // 00:00:00
    *dos_date = (21 << 9) | (1 << 5) | 1; // 2001-01-01
}

/// @brief Compute a proleptic Gregorian day number from a civil (year, month, day) triple.
/// @param year Full signed calendar year.
/// @param month One-based month.
/// @param day One-based day of month.
/// @return Signed number of days relative to 1970-01-01.
static int64_t archive_days_from_civil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = (unsigned)(year - era * 400);
    const unsigned mp = month > 2 ? month - 3 : month + 9;
    const unsigned doy = (153 * mp + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

/// @brief Convert a DOS (FAT) time+date pair to a Unix timestamp (seconds since epoch).
/// @param dos_time Encoded FAT time word.
/// @param dos_date Encoded FAT date word.
/// @return Seconds since 1970-01-01 UTC, or 0 when any encoded calendar field
/// is invalid. The valid DOS epoch date 1980-01-01 also yields a positive
/// value, so zero is an unambiguous invalid sentinel for DOS inputs.
static int64_t archive_dos_datetime_to_unix(uint16_t dos_time, uint16_t dos_date) {
    int year = ((dos_date >> 9) & 0x7F) + 1980;
    unsigned month = (unsigned)((dos_date >> 5) & 0xF);
    unsigned day = (unsigned)(dos_date & 0x1F);
    unsigned hour = (unsigned)((dos_time >> 11) & 0x1F);
    unsigned minute = (unsigned)((dos_time >> 5) & 0x3F);
    unsigned second = (unsigned)((dos_time & 0x1F) * 2);

    static const unsigned month_days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    unsigned max_day = month >= 1 && month <= 12 ? month_days[month - 1] : 0;
    if (month == 2 && leap)
        max_day = 29;
    if (month < 1 || month > 12 || day < 1 || day > max_day || hour > 23 || minute > 59 ||
        second > 59)
        return 0;

    int64_t days = archive_days_from_civil(year, month, day);
    return days * 86400 + (int64_t)hour * 3600 + (int64_t)minute * 60 + (int64_t)second;
}

//=============================================================================
// Public API - Creation/Opening
//=============================================================================

/// @brief `Archive.Open(path)` — open an existing ZIP file for reading.
///
/// Reads the entire file into memory (no streaming yet) and parses the
/// central directory. The path is captured (refcount-bumped) so the
/// archive object survives the caller's string. Traps on:
///   - empty/NULL path
///   - file-not-found / permission errors
///   - unreasonably large files (> SIZE_MAX)
///   - invalid ZIP structure
///
/// @param path UTF-8 file path.
/// @return Owned `Archive` handle in read mode.
void *rt_archive_open(rt_string path) {
    const char *cpath = archive_require_path(path, "Archive: invalid path");
    if (!cpath || *cpath == '\0')
        return NULL;

    // Open and read file
#if RT_PLATFORM_WINDOWS
    HANDLE h = archive_open_win_path(cpath, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING);
    if (h == INVALID_HANDLE_VALUE) {
        rt_trap("Archive: file not found");
        return NULL;
    }

    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size)) {
        CloseHandle(h);
        rt_trap("Archive: failed to get file size");
        return NULL;
    }
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(h, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        CloseHandle(h);
        rt_trap("Archive: path is not a regular file");
        return NULL;
    }

    if (size.QuadPart < 0 || (uint64_t)size.QuadPart > SIZE_MAX ||
        (uint64_t)size.QuadPart > archive_max_file_bytes()) {
        CloseHandle(h);
        rt_trap("Archive: file exceeds configured resource limit");
        return NULL;
    }

    size_t data_len = (size_t)size.QuadPart;
    uint8_t *data = data_len > 0 ? (uint8_t *)malloc(data_len) : NULL;
    if (data_len > 0 && !data) {
        CloseHandle(h);
        rt_trap("Archive: memory allocation failed");
        return NULL;
    }
    if (!archive_read_exact_win_or_free(h, data, data_len, "Archive: failed to read file"))
        return NULL;
#else
    int fd = archive_open_posix(cpath, O_RDONLY, 0);
    if (fd < 0) {
        rt_trap("Archive: file not found");
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        rt_trap("Archive: failed to get file size");
        return NULL;
    }
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        rt_trap("Archive: path is not a regular file");
        return NULL;
    }

    if (st.st_size < 0 || (uint64_t)st.st_size > (uint64_t)SIZE_MAX ||
        (uint64_t)st.st_size > archive_max_file_bytes()) {
        close(fd);
        rt_trap("Archive: file exceeds configured resource limit");
        return NULL;
    }

    size_t data_len = (size_t)st.st_size;
    uint8_t *data = data_len > 0 ? (uint8_t *)malloc(data_len) : NULL;
    if (data_len > 0 && !data) {
        close(fd);
        rt_trap("Archive: memory allocation failed");
        return NULL;
    }
    if (!archive_read_exact_posix_or_free(fd, data, data_len, "Archive: failed to read file"))
        return NULL;
#endif

    rt_archive_t *ar = archive_alloc_or_free_data(data, "Archive: memory allocation failed");
    if (!ar)
        return NULL;
    ar->data = data;
    ar->data_len = data_len;
    ar->owns_data = true;
    ar->is_writing = false;
    if (!archive_retain_path_or_release(ar, path, "Archive: path retain failed"))
        return NULL;

    if (!parse_central_directory(ar)) {
        const char *message = archive_parse_failure_message(ar, "Archive: not a valid ZIP file");
        char saved_error[128];
        snprintf(saved_error, sizeof(saved_error), "%s", message);
        archive_release_temp_object(ar);
        rt_trap(saved_error);
        return NULL;
    }

    return ar;
}

/// @brief `Archive.Create(path)` — start a new ZIP file for writing.
///
/// Probes that the destination directory is writable without truncating
/// an existing archive. All entry data is buffered in `write_buf` until
/// `Finish`, which atomically replaces the destination. Traps on invalid
/// path or allocation failure.
///
/// @param path UTF-8 destination path.
/// @return Owned `Archive` handle in write mode.
void *rt_archive_create(rt_string path) {
    const char *cpath = archive_require_path(path, "Archive: invalid path");
    if (!cpath || *cpath == '\0')
        return NULL;

#if RT_PLATFORM_WINDOWS
    char *probe_tmp = NULL;
    HANDLE h = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        probe_tmp = archive_make_temp_path(cpath, attempt);
        if (!probe_tmp) {
            rt_trap("Archive: memory allocation failed");
            return NULL;
        }
        h = archive_open_win_path(probe_tmp, GENERIC_WRITE, 0, CREATE_NEW);
        if (h != INVALID_HANDLE_VALUE)
            break;
        free(probe_tmp);
        probe_tmp = NULL;
        DWORD err = GetLastError();
        if (err != ERROR_FILE_EXISTS && err != ERROR_ALREADY_EXISTS)
            break;
    }
    if (h == INVALID_HANDLE_VALUE || !probe_tmp) {
        free(probe_tmp);
        rt_trap("Archive: failed to create file");
        return NULL;
    }
    CloseHandle(h);
    archive_unlink_utf8(probe_tmp);
    free(probe_tmp);
#else
    char *probe_tmp = NULL;
    int fd = -1;
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        probe_tmp = archive_make_temp_path(cpath, attempt);
        if (!probe_tmp) {
            rt_trap("Archive: memory allocation failed");
            return NULL;
        }
        fd = archive_open_posix(probe_tmp, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd >= 0)
            break;
        int err = errno;
        free(probe_tmp);
        probe_tmp = NULL;
        if (err != EEXIST)
            break;
    }
    if (fd < 0 || !probe_tmp) {
        free(probe_tmp);
        rt_trap("Archive: failed to create file");
        return NULL;
    }
    close(fd);
    unlink(probe_tmp);
    free(probe_tmp);
#endif

    rt_archive_t *ar = archive_alloc();
    if (!ar)
        return NULL;
    if (!archive_retain_path_or_release(ar, path, "Archive: path retain failed"))
        return NULL;
    ar->is_writing = true;
    ar->write_cap = ar->max_file_bytes < 4096u ? (size_t)ar->max_file_bytes : 4096u;
    ar->write_buf = (uint8_t *)malloc(ar->write_cap);
    if (!ar->write_buf) {
        archive_release_temp_object(ar);
        rt_trap("Archive: memory allocation failed");
        return NULL;
    }

    return ar;
}

/// @brief `Archive.FromBytes(data)` — open an in-memory ZIP for reading.
///
/// Used when ZIP data lives in a Bytes buffer (e.g., downloaded over
/// HTTP, embedded as an asset). The bytes are *copied* internally so
/// the archive owns its own buffer — the source can be freed
/// immediately after the call returns. Traps on NULL data, OOM,
/// or invalid ZIP structure.
///
/// @param data Source `rt_bytes` containing a complete ZIP archive.
/// @return Owned `Archive` handle in read mode (no path).
void *rt_archive_from_bytes(void *data) {
    if (!data) {
        rt_trap("Archive: NULL data");
        return NULL;
    }

    int64_t len = bytes_len(data);
    if (len < 0) {
        rt_trap("Archive: invalid data length");
        return NULL;
    }
    uint8_t *src = bytes_data(data);
    if (len > 0 && !src) {
        rt_trap("Archive: invalid data");
        return NULL;
    }
    if ((uint64_t)len > archive_max_file_bytes()) {
        rt_trap("Archive: data exceeds configured resource limit");
        return NULL;
    }

    // Copy the data
    uint8_t *copy = NULL;
    if (len > 0) {
        copy = (uint8_t *)malloc((size_t)len);
    }
    if (len > 0 && !copy) {
        rt_trap("Archive: memory allocation failed");
        return NULL;
    }
    if (len > 0)
        memcpy(copy, src, (size_t)len);

    rt_archive_t *ar = archive_alloc_or_free_data(copy, "Archive: memory allocation failed");
    if (!ar)
        return NULL;
    ar->path = NULL;
    ar->data = copy;
    ar->data_len = (size_t)len;
    ar->owns_data = true;
    ar->is_writing = false;

    if (!parse_central_directory(ar)) {
        const char *message = archive_parse_failure_message(ar, "Archive: not a valid ZIP archive");
        char saved_error[128];
        snprintf(saved_error, sizeof(saved_error), "%s", message);
        archive_release_temp_object(ar);
        rt_trap(saved_error);
        return NULL;
    }

    return ar;
}

//=============================================================================
// Properties
//=============================================================================

/// @brief `Archive.Path` — return the path the archive was opened from.
///
/// Returns the empty string for `FromBytes`-constructed archives
/// (which have no associated path) or for a NULL receiver.
///
/// @param obj Archive handle.
/// @return Owned `rt_string` containing the path, or `""`.
rt_string rt_archive_path(void *obj) {
    rt_archive_t *ar = archive_require(obj, "Archive: invalid archive");
    if (!ar)
        return rt_str_empty();
    return ar->path ? rt_string_ref(ar->path) : rt_str_empty();
}

/// @brief `Archive.Count` — number of entries (files + directories).
///
/// Returns the size of `entries` for read-mode archives or
/// `write_entries` for write-mode. Both increase as entries are
/// added; neither shrinks.
///
/// @param obj Archive handle.
/// @return Entry count, or 0 for NULL.
int64_t rt_archive_count(void *obj) {
    rt_archive_t *ar = archive_require(obj, "Archive: invalid archive");
    if (!ar)
        return 0;
    archive_rwlock_read_enter(ar->rw_lock);
    int64_t count = ar->is_writing ? ar->write_entry_count : ar->entry_count;
    archive_rwlock_read_exit(ar->rw_lock);
    return count;
}

/// @brief `Archive.Names` — return all entry names as a `seq<str>`.
///
/// Snapshots the appropriate entry table (read- or write-mode) under the
/// archive read lock and copies every name into an independently owned runtime
/// string. The result therefore remains valid after the Archive is released.
/// Always returns a fresh seq, even for NULL or empty archives.
///
/// @param obj Archive handle (may be NULL).
/// @return Owned seq of entry names in archive order.
void *rt_archive_names(void *obj) {
    rt_archive_t *ar = archive_require(obj, "Archive: invalid archive");
    void *volatile seq = NULL;
    volatile rt_string active_name = NULL;
    volatile int lock_held = 0;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        archive_save_trap_error(saved_error, sizeof(saved_error), "Archive: failed to list names");
        rt_trap_clear_recovery();
        if (lock_held)
            archive_rwlock_read_exit(ar->rw_lock);
        rt_str_release_maybe((rt_string)active_name);
        archive_release_temp_object((void *)seq);
        rt_trap(saved_error);
        return NULL;
    }

    seq = rt_seq_new();
    if (!seq) {
        rt_trap_clear_recovery();
        return NULL;
    }
    rt_seq_set_owns_elements((void *)seq, 1);
    if (!ar) {
        rt_trap_clear_recovery();
        return (void *)seq;
    }

    archive_rwlock_read_enter(ar->rw_lock);
    lock_held = 1;
    zip_entry_t *entries = ar->is_writing ? ar->write_entries : ar->entries;
    int count = ar->is_writing ? ar->write_entry_count : ar->entry_count;
    for (int i = 0; i < count; i++) {
        const char *entry_name = entries[i].name ? entries[i].name : "";
        rt_string name = rt_string_from_bytes(entry_name, strlen(entry_name));
        active_name = name;
        rt_seq_push((void *)seq, name);
        rt_string_unref(name);
        active_name = NULL;
    }
    lock_held = 0;
    archive_rwlock_read_exit(ar->rw_lock);
    rt_trap_clear_recovery();
    return (void *)seq;
}

//=============================================================================
// Reading Methods
//=============================================================================

/// @brief `Archive.Has(name)` — test whether an entry exists.
///
/// Normalizes the name (path-traversal guard, separator collapse).
/// If the original name ended with a `/` we add the trailing slash
/// back after normalization so callers can distinguish file vs
/// directory entries. Returns 0 for write-mode archives (which have
/// no readable entries yet) or any normalization failure.
///
/// @param obj  Archive handle.
/// @param name Entry name to look up.
/// @return 1 if found, 0 otherwise.
int8_t rt_archive_has(void *obj, rt_string name) {
    rt_archive_t *ar = archive_require(obj, "Archive: invalid archive");
    if (!ar || ar->is_writing)
        return 0;

    const char *cname = archive_entry_name_cstr(name);
    if (!cname)
        return 0;
    const bool wants_dir = name_ends_with_sep(cname);

    char *norm_name = NULL;
    name_result_t norm_res = normalize_name(cname, &norm_name);
    if (norm_res == NAME_OOM) {
        rt_trap("Archive: memory allocation failed");
        return 0;
    }
    if (norm_res == NAME_INVALID)
        return 0;
    if (wants_dir) {
        norm_name = ensure_trailing_slash(norm_name);
        if (!norm_name)
            return 0;
    }

    int8_t found = find_entry(ar, norm_name) != NULL ? 1 : 0;
    free(norm_name);
    return found;
}

/// @brief `Archive.Read(name)` — extract entry contents as Bytes.
///
/// Normalizes the name then delegates to `read_entry_data`, which
/// handles both stored and DEFLATE methods. Traps on a missing entry,
/// invalid name, write-only archive, or any structural inconsistency.
///
/// @param obj  Archive handle.
/// @param name Entry name.
/// @return Owned `rt_bytes` with the uncompressed payload.
void *rt_archive_read(void *obj, rt_string name) {
    rt_archive_t *ar = archive_require(obj, "Archive: invalid archive");
    if (!ar) {
        rt_trap("Archive: NULL archive");
        return NULL;
    }
    if (ar->is_writing) {
        rt_trap("Archive: cannot read from write-only archive");
        return NULL;
    }

    const char *cname = archive_entry_name_cstr(name);
    if (!cname) {
        rt_trap("Archive: NULL entry name");
        return NULL;
    }
    const bool wants_dir = name_ends_with_sep(cname);

    char *norm_name = NULL;
    name_result_t norm_res = normalize_name(cname, &norm_name);
    if (norm_res == NAME_INVALID) {
        rt_trap("Archive: invalid entry name");
        return NULL;
    }
    if (norm_res == NAME_OOM) {
        rt_trap("Archive: memory allocation failed");
        return NULL;
    }
    if (wants_dir) {
        norm_name = ensure_trailing_slash(norm_name);
        if (!norm_name)
            return NULL;
    }

    zip_entry_t *e = find_entry(ar, norm_name);
    free(norm_name);
    if (!e) {
        rt_trap("Archive: entry not found");
        return NULL;
    }

    return read_entry_data(ar, e);
}

/// @brief `Archive.ReadStr(name)` — extract entry contents as a string.
///
/// Convenience wrapper that reads the entry as Bytes then copies those bytes
/// into a runtime String. The conversion does not validate UTF-8 and traps on
/// the same conditions as `Read` plus allocation failure.
///
/// @param obj  Archive handle.
/// @param name Entry name.
/// @return Owned `rt_string` containing the entry text.
rt_string rt_archive_read_str(void *obj, rt_string name) {
    void *volatile data = rt_archive_read(obj, name);
    if (!data)
        return rt_str_empty();
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        archive_save_trap_error(
            saved_error, sizeof(saved_error), "Archive: failed to convert entry to string");
        rt_trap_clear_recovery();
        archive_release_temp_object((void *)data);
        rt_trap(saved_error);
        return rt_str_empty();
    }
    rt_string result = rt_bytes_to_str((void *)data);
    rt_trap_clear_recovery();
    archive_release_temp_object((void *)data);
    return result;
}

/// @brief `Archive.Extract(name, destPath)` — write a single entry to disk.
///
/// Reads the entry into memory then writes it to `destPath`. The
/// destination directory must already exist (we do not auto-create
/// parents — that's `ExtractAll`'s job). Traps on any I/O error.
/// The temporary `rt_bytes` is released eagerly to avoid retaining
/// large entry payloads beyond this call.
///
/// @param obj       Archive handle.
/// @param name      Entry to extract.
/// @param dest_path UTF-8 destination file path.
void rt_archive_extract(void *obj, rt_string name, rt_string dest_path) {
    const char *cpath = archive_require_path(dest_path, "Archive: invalid destination path");
    if (!cpath || *cpath == '\0')
        return;

    void *volatile data = rt_archive_read(obj, name);
    if (!data)
        return;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        archive_save_trap_error(
            saved_error, sizeof(saved_error), "Archive: failed to extract entry");
        rt_trap_clear_recovery();
        archive_release_temp_object((void *)data);
        rt_trap(saved_error);
        return;
    }
    archive_write_bytes_to_path(cpath, (void *)data);
    rt_trap_clear_recovery();
    archive_release_temp_object((void *)data);
}

/// @brief `Archive.ExtractAll(destDir)` — explode the archive onto disk.
///
/// Creates `destDir` (and any missing parents), then iterates every
/// entry: directory entries are created via `rt_dir_make_all`; file
/// entries have their parent directory created on the fly (so deep
/// hierarchies don't need a separate directory entry) and are written
/// with `archive_write_bytes_to_path`. Forward slashes in entry names
/// are translated to the platform path separator. Path-traversal
/// attacks are blocked by `normalize_name` rejecting `..` segments.
///
/// @param obj      Archive handle.
/// @param dest_dir UTF-8 destination directory.
void rt_archive_extract_all(void *obj, rt_string dest_dir) {
    rt_archive_t *ar = archive_require(obj, "Archive: invalid archive");
    if (!ar) {
        rt_trap("Archive: NULL archive");
        return;
    }
    if (ar->is_writing) {
        rt_trap("Archive: cannot extract from write-only archive");
        return;
    }

    const char *cdir = archive_require_path(dest_dir, "Archive: invalid destination directory");
    if (!cdir || *cdir == '\0')
        return;

    size_t dir_len = strlen(cdir);
    size_t root_len = archive_trim_trailing_seps(cdir, dir_len);
    archive_reject_symlink_components(cdir, root_len, 1);
    rt_dir_make_all(dest_dir);
    archive_reject_symlink_components(cdir, root_len, 1);

    char *volatile active_norm_name = NULL;
    char *volatile active_full_path = NULL;
    char *volatile active_leaf = NULL;
    void *volatile active_data = NULL;
    volatile rt_string active_runtime_path = NULL;
#if !RT_PLATFORM_WINDOWS
    volatile int root_fd = -1;
    volatile int parent_fd = -1;
#endif
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        archive_save_trap_error(
            saved_error, sizeof(saved_error), "Archive: failed to extract archive");
        rt_trap_clear_recovery();
#if !RT_PLATFORM_WINDOWS
        if (parent_fd >= 0)
            close((int)parent_fd);
        if (root_fd >= 0)
            close((int)root_fd);
#endif
        free((char *)active_leaf);
        free((char *)active_full_path);
        free((char *)active_norm_name);
        rt_str_release_maybe((rt_string)active_runtime_path);
        archive_release_temp_object((void *)active_data);
        rt_trap(saved_error);
        return;
    }

#if !RT_PLATFORM_WINDOWS
    root_fd = archive_open_root_dir_posix(cdir);
    if (root_fd < 0) {
        rt_trap_clear_recovery();
        return;
    }
    archive_reject_symlink_components(cdir, root_len, 1);

    for (int i = 0; i < ar->entry_count; i++) {
        zip_entry_t *e = &ar->entries[i];
        char *norm_name = NULL;
        name_result_t norm_res = normalize_name(e->name, &norm_name);
        active_norm_name = norm_name;
        if (norm_res == NAME_INVALID) {
            rt_trap("Archive: invalid entry name");
            rt_trap_clear_recovery();
            close((int)root_fd);
            free(norm_name);
            return;
        }
        if (norm_res == NAME_OOM) {
            rt_trap("Archive: memory allocation failed");
            rt_trap_clear_recovery();
            close((int)root_fd);
            return;
        }

        if (e->is_directory) {
            archive_make_dirs_posix_at((int)root_fd, norm_name);
        } else {
            char *leaf = NULL;
            parent_fd = archive_open_parent_for_file_posix((int)root_fd, norm_name, &leaf);
            active_leaf = leaf;
            if (parent_fd < 0) {
                rt_trap_clear_recovery();
                close((int)root_fd);
                free(leaf);
                free(norm_name);
                return;
            }
            active_data = read_entry_data(ar, e);
            if (!active_data) {
                rt_trap_clear_recovery();
                close((int)parent_fd);
                close((int)root_fd);
                free(leaf);
                free(norm_name);
                return;
            }
            archive_write_bytes_to_dirfd_posix((int)parent_fd, leaf, (void *)active_data);
            archive_release_temp_object((void *)active_data);
            active_data = NULL;
            close((int)parent_fd);
            parent_fd = -1;
            free(leaf);
            active_leaf = NULL;
        }
        free(norm_name);
        active_norm_name = NULL;
    }

    close((int)root_fd);
    root_fd = -1;
    rt_trap_clear_recovery();
    return;
#else
    for (int i = 0; i < ar->entry_count; i++) {
        zip_entry_t *e = &ar->entries[i];
        char *norm_name = NULL;
        name_result_t norm_res = normalize_name(e->name, &norm_name);
        active_norm_name = norm_name;
        if (norm_res == NAME_INVALID) {
            rt_trap("Archive: invalid entry name");
            rt_trap_clear_recovery();
            free(norm_name);
            return;
        }
        if (norm_res == NAME_OOM) {
            rt_trap("Archive: memory allocation failed");
            rt_trap_clear_recovery();
            return;
        }

        size_t name_len = strlen(norm_name);
        if (dir_len > SIZE_MAX - 1 - name_len) {
            rt_trap("Archive: destination path too long");
            rt_trap_clear_recovery();
            free(norm_name);
            return;
        }
        size_t path_len = dir_len + 1 + name_len;
        char *full_path = (char *)malloc(path_len + 1);
        active_full_path = full_path;
        if (!full_path) {
            rt_trap("Archive: memory allocation failed");
            rt_trap_clear_recovery();
            free(norm_name);
            return;
        }

        memcpy(full_path, cdir, dir_len);
        full_path[dir_len] = PATH_SEP;
        memcpy(full_path + dir_len + 1, norm_name, name_len);
        full_path[path_len] = '\0';
        for (size_t j = dir_len + 1; j < path_len; j++) {
            if (full_path[j] == '/')
                full_path[j] = PATH_SEP;
        }

        if (e->is_directory) {
            archive_reject_symlink_components(full_path, root_len, 1);
            active_runtime_path = rt_string_from_bytes(full_path, strlen(full_path));
            rt_dir_make_all((rt_string)active_runtime_path);
            rt_string_unref((rt_string)active_runtime_path);
            active_runtime_path = NULL;
            archive_reject_symlink_components(full_path, root_len, 1);
        } else {
            char *last_sep = strrchr(full_path, PATH_SEP);
            if (last_sep && last_sep > full_path + dir_len) {
                *last_sep = '\0';
                archive_reject_symlink_components(full_path, root_len, 1);
                active_runtime_path = rt_string_from_bytes(full_path, strlen(full_path));
                rt_dir_make_all((rt_string)active_runtime_path);
                rt_string_unref((rt_string)active_runtime_path);
                active_runtime_path = NULL;
                archive_reject_symlink_components(full_path, root_len, 1);
                *last_sep = PATH_SEP;
            }

            archive_reject_symlink_components(full_path, root_len, 0);
            active_data = read_entry_data(ar, e);
            if (!active_data) {
                rt_trap_clear_recovery();
                free(full_path);
                free(norm_name);
                return;
            }
            archive_write_bytes_to_path(full_path, (void *)active_data);
            archive_release_temp_object((void *)active_data);
            active_data = NULL;
        }

        free(full_path);
        active_full_path = NULL;
        free(norm_name);
        active_norm_name = NULL;
    }
    rt_trap_clear_recovery();
#endif
}

/// @brief `Archive.Info(name)` — return a metadata map for one entry.
///
/// The map keys are `size`, `compressedSize`, `crc`, `method`,
/// `modifiedTime`, `isDirectory`, and `isDir` (alias). Values are
/// boxed primitives so they survive map storage. `modifiedTime` is
/// a Unix timestamp computed from validated DOS date/time fields using the
/// proleptic Gregorian calendar. ZIP's two-second precision is preserved;
/// the fields are interpreted as UTC rather than applying local time or DST.
///
/// @param obj  Archive handle.
/// @param name Entry name.
/// @return Owned `rt_map` of metadata.
void *rt_archive_info(void *obj, rt_string name) {
    rt_archive_t *ar = archive_require(obj, "Archive: invalid archive");
    if (!ar) {
        rt_trap("Archive: NULL archive");
        return NULL;
    }
    if (ar->is_writing) {
        rt_trap("Archive: cannot get info from write-only archive");
        return NULL;
    }

    const char *cname = archive_entry_name_cstr(name);
    if (!cname) {
        rt_trap("Archive: NULL entry name");
        return NULL;
    }
    const bool wants_dir = name_ends_with_sep(cname);

    char *norm_name = NULL;
    name_result_t norm_res = normalize_name(cname, &norm_name);
    if (norm_res == NAME_INVALID) {
        rt_trap("Archive: invalid entry name");
        return NULL;
    }
    if (norm_res == NAME_OOM) {
        rt_trap("Archive: memory allocation failed");
        return NULL;
    }
    if (wants_dir) {
        norm_name = ensure_trailing_slash(norm_name);
        if (!norm_name)
            return NULL;
    }

    zip_entry_t *e = find_entry(ar, norm_name);
    free(norm_name);
    if (!e) {
        rt_trap("Archive: entry not found");
        return NULL;
    }

    void *volatile map = NULL;
    void *volatile boxed = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        archive_save_trap_error(
            saved_error, sizeof(saved_error), "Archive: failed to build entry info");
        rt_trap_clear_recovery();
        archive_release_temp_object((void *)boxed);
        archive_release_temp_object((void *)map);
        rt_trap(saved_error);
        return NULL;
    }

    map = rt_map_new();
    if (!map) {
        rt_trap_clear_recovery();
        return NULL;
    }

    boxed = rt_box_i64(e->uncompressed_size);
    archive_map_set_boxed((void *)map, "size", (void *)boxed);
    archive_release_temp_object((void *)boxed);
    boxed = NULL;

    boxed = rt_box_i64(e->compressed_size);
    archive_map_set_boxed((void *)map, "compressedSize", (void *)boxed);
    archive_release_temp_object((void *)boxed);
    boxed = NULL;

    boxed = rt_box_i64((int64_t)e->crc32);
    archive_map_set_boxed((void *)map, "crc", (void *)boxed);
    archive_release_temp_object((void *)boxed);
    boxed = NULL;

    boxed = rt_box_i64((int64_t)e->method);
    archive_map_set_boxed((void *)map, "method", (void *)boxed);
    archive_release_temp_object((void *)boxed);
    boxed = NULL;

    int64_t timestamp = archive_dos_datetime_to_unix(e->mod_time, e->mod_date);
    boxed = rt_box_i64(timestamp);
    archive_map_set_boxed((void *)map, "modifiedTime", (void *)boxed);
    archive_release_temp_object((void *)boxed);
    boxed = NULL;

    boxed = rt_box_i1(e->is_directory ? 1 : 0);
    archive_map_set_boxed((void *)map, "isDirectory", (void *)boxed);
    archive_map_set_boxed((void *)map, "isDir", (void *)boxed);
    archive_release_temp_object((void *)boxed);
    boxed = NULL;

    rt_trap_clear_recovery();
    return (void *)map;
}

//=============================================================================
// Writing Methods
//=============================================================================

/// @brief Execute `Archive.Add` while the caller owns the archive write lock.
/// @details Contains the normalization/compression/transaction logic. Keeping
///          this separate lets the public wrapper guarantee lock release when
///          any nested runtime helper performs a recoverable longjmp.
/// @param obj Valid archive handle already locked exclusively by the caller.
/// @param name Entry name to normalize and add.
/// @param data Source Bytes handle.
static void archive_add_locked(void *obj, rt_string name, void *data) {
    rt_archive_t *ar = archive_require(obj, "Archive: invalid archive");
    if (!ar) {
        rt_trap("Archive: NULL archive");
        return;
    }
    if (!ar->is_writing) {
        rt_trap("Archive: cannot add to read-only archive");
        return;
    }
    if (ar->is_finished) {
        rt_trap("Archive: archive already finished");
        return;
    }
    if (!data) {
        rt_trap("Archive: NULL data");
        return;
    }

    const char *cname = archive_entry_name_cstr(name);
    if (!cname || *cname == '\0') {
        rt_trap("Archive: invalid entry name");
        return;
    }

    char *norm_name = NULL;
    name_result_t norm_res = normalize_name(cname, &norm_name);
    if (norm_res == NAME_INVALID) {
        rt_trap("Archive: invalid entry name");
        return;
    }
    if (norm_res == NAME_OOM) {
        rt_trap("Archive: memory allocation failed");
        return;
    }
    if (archive_write_has_entry(ar, norm_name)) {
        free(norm_name);
        rt_trap("Archive: duplicate entry name");
        return;
    }

    int64_t raw_len_i64 = bytes_len(data);
    if (raw_len_i64 < 0) {
        free(norm_name);
        rt_trap("Archive: invalid data length");
        return;
    }
    uint8_t *raw_data = bytes_data(data);
    size_t raw_len = (size_t)raw_len_i64;
    if (raw_len > 0 && !raw_data) {
        free(norm_name);
        rt_trap("Archive: invalid data");
        return;
    }
    if (raw_len > UINT32_MAX) {
        free(norm_name);
        rt_trap("Archive: ZIP64 entries are not supported");
        return;
    }
    if ((uint64_t)raw_len > ar->max_entry_bytes || (uint64_t)raw_len > ar->max_total_entry_bytes ||
        ar->write_total_entry_bytes > ar->max_total_entry_bytes - (uint64_t)raw_len) {
        free(norm_name);
        rt_trap("Archive: entry data exceeds configured resource limit");
        return;
    }

    // Compute CRC
    uint32_t crc = rt_crc32_compute(raw_data, raw_len);

    // Decide whether to compress
    void *compressed = NULL;
    uint16_t method = ZIP_METHOD_STORED;
    const uint8_t *write_data = raw_data;
    size_t write_len = raw_len;

    if (raw_len > 64) {
        void *volatile compressed_owner = NULL;
        char *volatile norm_name_owner = norm_name;
        jmp_buf recovery;
        rt_trap_set_recovery(&recovery);
        if (setjmp(recovery) != 0) {
            char saved_error[256];
            archive_save_trap_error(
                saved_error, sizeof(saved_error), "Archive: failed to compress entry");
            rt_trap_clear_recovery();
            archive_release_temp_object((void *)compressed_owner);
            free((char *)norm_name_owner);
            rt_trap(saved_error);
            return;
        }

        compressed = rt_compress_deflate(data);
        compressed_owner = compressed;
        if (!compressed) {
            rt_trap_clear_recovery();
            free(norm_name);
            rt_trap("Archive: failed to compress entry");
            return;
        }
        int64_t comp_len_i64 = bytes_len(compressed);
        if (comp_len_i64 < 0 || (uint64_t)comp_len_i64 > (uint64_t)SIZE_MAX) {
            rt_trap_clear_recovery();
            archive_release_temp_object(compressed);
            free(norm_name);
            rt_trap("Archive: invalid compressed data length");
            return;
        }
        uint8_t *comp_data = bytes_data(compressed);
        if (comp_len_i64 > 0 && !comp_data) {
            rt_trap_clear_recovery();
            archive_release_temp_object(compressed);
            free(norm_name);
            rt_trap("Archive: invalid compressed data");
            return;
        }
        size_t comp_len = (size_t)comp_len_i64;
        if (comp_len < raw_len) {
            method = ZIP_METHOD_DEFLATE;
            write_data = comp_data;
            write_len = comp_len;
        }
        rt_trap_clear_recovery();
    }
    if (write_len > UINT32_MAX) {
        free(norm_name);
        archive_release_temp_object(compressed);
        rt_trap("Archive: ZIP64 entries are not supported");
        return;
    }
    if (ar->write_len > UINT32_MAX) {
        free(norm_name);
        archive_release_temp_object(compressed);
        rt_trap("Archive: ZIP64 archives are not supported");
        return;
    }

    // Record entry info
    zip_entry_t e = {0};
    e.name = norm_name;
    e.crc32 = crc;
    e.compressed_size = (uint32_t)write_len;
    e.uncompressed_size = (uint32_t)raw_len;
    e.method = method;
    get_dos_time(&e.mod_time, &e.mod_date);
    e.local_offset = (uint32_t)ar->write_len;
    e.is_directory = false;

    // Write local file header
    size_t name_len = strlen(norm_name);
    if (name_len > UINT16_MAX) {
        free(norm_name);
        archive_release_temp_object(compressed);
        rt_trap("Archive: entry name too long");
        return;
    }
    uint8_t local_header[ZIP_LOCAL_HEADER_SIZE];
    write_u32(local_header, ZIP_LOCAL_HEADER_SIG);
    write_u16(local_header + 4, ZIP_VERSION_NEEDED);
    write_u16(local_header + 6, 0); // General purpose flags
    write_u16(local_header + 8, method);
    write_u16(local_header + 10, e.mod_time);
    write_u16(local_header + 12, e.mod_date);
    write_u32(local_header + 14, crc);
    write_u32(local_header + 18, (uint32_t)write_len);
    write_u32(local_header + 22, (uint32_t)raw_len);
    write_u16(local_header + 26, (uint16_t)name_len);
    write_u16(local_header + 28, 0); // Extra field length

    size_t write_start = ar->write_len;
    char *volatile norm_name_owner = norm_name;
    void *volatile compressed_owner = compressed;
    jmp_buf append_recovery;
    rt_trap_set_recovery(&append_recovery);
    if (setjmp(append_recovery) != 0) {
        char saved_error[256];
        archive_save_trap_error(saved_error, sizeof(saved_error), "Archive: failed to add entry");
        rt_trap_clear_recovery();
        ar->write_len = write_start;
        free((char *)norm_name_owner);
        archive_release_temp_object((void *)compressed_owner);
        rt_trap(saved_error);
        return;
    }
    if (!write_bytes(ar, local_header, ZIP_LOCAL_HEADER_SIZE) ||
        !write_bytes(ar, (const uint8_t *)norm_name, name_len) ||
        !write_bytes(ar, write_data, write_len)) {
        rt_trap_clear_recovery();
        ar->write_len = write_start;
        free(norm_name);
        archive_release_temp_object(compressed);
        return;
    }

    if (!add_write_entry(ar, &e)) {
        rt_trap_clear_recovery();
        ar->write_len = write_start;
        free(norm_name);
        archive_release_temp_object(compressed);
        return;
    }
    norm_name_owner = NULL;
    rt_trap_clear_recovery();
    archive_release_temp_object(compressed);
}

/// @brief `Archive.Add(name, data)` — append a file entry from a Bytes buffer.
/// @details CRC32 is computed over the raw payload. Payloads larger than 64
///          bytes are trial-compressed and use DEFLATE only when it produces a
///          smaller representation. The exclusive archive lock serializes the
///          local-header append, metadata/index update, and rollback boundary,
///          so concurrent callers either add one complete entry or leave no
///          trace. Any recovered trap is rethrown after releasing the lock.
/// @param obj Archive handle opened in write mode.
/// @param name Relative entry name; separators are normalized.
/// @param data Borrowed Bytes payload retained only for the duration of the call.
void rt_archive_add(void *obj, rt_string name, void *data) {
    rt_archive_t *ar = archive_require(obj, "Archive: invalid archive");
    if (!ar)
        return;
    archive_rwlock_write_enter(ar->rw_lock);
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        archive_save_trap_error(saved_error, sizeof(saved_error), "Archive: failed to add entry");
        rt_trap_clear_recovery();
        archive_rwlock_write_exit(ar->rw_lock);
        rt_trap(saved_error);
        return;
    }
    archive_add_locked(obj, name, data);
    rt_trap_clear_recovery();
    archive_rwlock_write_exit(ar->rw_lock);
}

/// @brief `Archive.AddStr(name, text)` — convenience: store a string entry.
///
/// Converts `text` to UTF-8 Bytes via `rt_bytes_from_str` then delegates
/// to `Add`. The temporary Bytes is released eagerly.
///
/// @param obj  Archive handle.
/// @param name Entry name.
/// @param text String contents.
void rt_archive_add_str(void *obj, rt_string name, rt_string text) {
    rt_archive_t *ar = archive_require(obj, "Archive: invalid archive");
    if (!ar)
        return;
    int64_t text_len = rt_str_len(text);
    if (text_len < 0 || (uint64_t)text_len > ar->max_entry_bytes) {
        rt_trap("Archive: entry data exceeds configured resource limit");
        return;
    }
    void *data = rt_bytes_from_str(text);
    archive_add_with_temp_data(obj, name, data, "Archive: failed to add string entry");
}

/// @brief `Archive.AddFile(name, srcPath)` — copy a file from disk into the archive.
///
/// Reads `srcPath` fully into memory using `rt_bytes_new` + the
/// platform-appropriate exact-read helper, then delegates to `Add` for
/// CRC, compression, and entry recording. Traps on missing/invalid
/// source file. The temporary Bytes is released eagerly.
///
/// @param obj      Archive handle.
/// @param name     Name to use inside the archive.
/// @param src_path UTF-8 source file path on disk.
void rt_archive_add_file(void *obj, rt_string name, rt_string src_path) {
    rt_archive_t *ar = archive_require(obj, "Archive: invalid archive");
    if (!ar)
        return;
    const char *cpath = archive_require_path(src_path, "Archive: invalid source path");
    if (!cpath || *cpath == '\0')
        return;

    // Read file contents
#if RT_PLATFORM_WINDOWS
    HANDLE h = archive_open_win_path(cpath, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING);
    if (h == INVALID_HANDLE_VALUE) {
        rt_trap("Archive: source file not found");
        return;
    }

    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size)) {
        CloseHandle(h);
        rt_trap("Archive: failed to get file size");
        return;
    }
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(h, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        CloseHandle(h);
        rt_trap("Archive: source path is not a regular file");
        return;
    }

    if (size.QuadPart < 0 || (uint64_t)size.QuadPart > INT64_MAX) {
        CloseHandle(h);
        rt_trap("Archive: source file too large");
        return;
    }
    if ((uint64_t)size.QuadPart > ar->max_entry_bytes) {
        CloseHandle(h);
        rt_trap("Archive: entry data exceeds configured resource limit");
        return;
    }

    void *data = archive_bytes_new_win_or_close(
        h, (int64_t)size.QuadPart, "Archive: memory allocation failed");
    if (!data)
        return;
    if (!archive_read_exact_win_or_release_object(
            h, data, (size_t)size.QuadPart, "Archive: failed to read source file"))
        return;
#else
    int fd = archive_open_posix(cpath, O_RDONLY, 0);
    if (fd < 0) {
        rt_trap("Archive: source file not found");
        return;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        rt_trap("Archive: failed to get file size");
        return;
    }
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        rt_trap("Archive: source path is not a regular file");
        return;
    }

    if (st.st_size < 0 || (uint64_t)st.st_size > (uint64_t)INT64_MAX ||
        (uint64_t)st.st_size > (uint64_t)SIZE_MAX) {
        close(fd);
        rt_trap("Archive: source file too large");
        return;
    }
    if ((uint64_t)st.st_size > ar->max_entry_bytes) {
        close(fd);
        rt_trap("Archive: entry data exceeds configured resource limit");
        return;
    }

    void *data = archive_bytes_new_posix_or_close(
        fd, (int64_t)st.st_size, "Archive: memory allocation failed");
    if (!data)
        return;
    if (!archive_read_exact_posix_or_release_object(
            fd, data, (size_t)st.st_size, "Archive: failed to read source file"))
        return;
#endif

    archive_add_with_temp_data(obj, name, data, "Archive: failed to add file entry");
}

/// @brief Execute `Archive.AddDir` while the caller owns the archive write lock.
/// @details Performs name normalization and a transactional local-record/index
///          append. The public wrapper provides exclusive synchronization and
///          releases the native lock before propagating any trap.
/// @param obj Valid archive handle already locked exclusively by the caller.
/// @param name Directory name to normalize and append.
static void archive_add_dir_locked(void *obj, rt_string name) {
    rt_archive_t *ar = archive_require(obj, "Archive: invalid archive");
    if (!ar) {
        rt_trap("Archive: NULL archive");
        return;
    }
    if (!ar->is_writing) {
        rt_trap("Archive: cannot add to read-only archive");
        return;
    }
    if (ar->is_finished) {
        rt_trap("Archive: archive already finished");
        return;
    }

    const char *cname = archive_entry_name_cstr(name);
    if (!cname || *cname == '\0') {
        rt_trap("Archive: invalid entry name");
        return;
    }

    char *norm_name = NULL;
    name_result_t norm_res = normalize_name(cname, &norm_name);
    if (norm_res == NAME_INVALID) {
        rt_trap("Archive: invalid entry name");
        return;
    }
    if (norm_res == NAME_OOM) {
        rt_trap("Archive: memory allocation failed");
        return;
    }

    // Ensure name ends with /
    size_t len = strlen(norm_name);
    if (len == 0 || norm_name[len - 1] != '/') {
        if (len > SIZE_MAX - 2) {
            free(norm_name);
            rt_trap("Archive: entry name too long");
            return;
        }
        char *new_name = (char *)realloc(norm_name, len + 2);
        if (!new_name) {
            free(norm_name);
            rt_trap("Archive: memory allocation failed");
            return;
        }
        norm_name = new_name;
        norm_name[len] = '/';
        norm_name[len + 1] = '\0';
        len++;
    }
    if (archive_write_has_entry(ar, norm_name)) {
        free(norm_name);
        rt_trap("Archive: duplicate entry name");
        return;
    }

    // Record entry info
    zip_entry_t e = {0};
    e.name = norm_name;
    e.crc32 = 0;
    e.compressed_size = 0;
    e.uncompressed_size = 0;
    e.method = ZIP_METHOD_STORED;
    get_dos_time(&e.mod_time, &e.mod_date);
    if (!archive_require_zip32_size(ar->write_len, "Archive: ZIP64 archives are not supported")) {
        free(norm_name);
        return;
    }
    e.local_offset = (uint32_t)ar->write_len;
    e.is_directory = true;

    // Write local file header
    if (len > UINT16_MAX) {
        rt_trap("Archive: entry name too long");
        free(norm_name);
        return;
    }
    uint8_t local_header[ZIP_LOCAL_HEADER_SIZE];
    write_u32(local_header, ZIP_LOCAL_HEADER_SIG);
    write_u16(local_header + 4, ZIP_VERSION_NEEDED);
    write_u16(local_header + 6, 0);
    write_u16(local_header + 8, ZIP_METHOD_STORED);
    write_u16(local_header + 10, e.mod_time);
    write_u16(local_header + 12, e.mod_date);
    write_u32(local_header + 14, 0);
    write_u32(local_header + 18, 0);
    write_u32(local_header + 22, 0);
    write_u16(local_header + 26, (uint16_t)len);
    write_u16(local_header + 28, 0);

    size_t write_start = ar->write_len;
    char *volatile norm_name_owner = norm_name;
    jmp_buf append_recovery;
    rt_trap_set_recovery(&append_recovery);
    if (setjmp(append_recovery) != 0) {
        char saved_error[256];
        archive_save_trap_error(
            saved_error, sizeof(saved_error), "Archive: failed to add directory entry");
        rt_trap_clear_recovery();
        ar->write_len = write_start;
        free((char *)norm_name_owner);
        rt_trap(saved_error);
        return;
    }
    if (!write_bytes(ar, local_header, ZIP_LOCAL_HEADER_SIZE) ||
        !write_bytes(ar, (const uint8_t *)norm_name, len)) {
        rt_trap_clear_recovery();
        ar->write_len = write_start;
        free(norm_name);
        return;
    }

    if (!add_write_entry(ar, &e)) {
        rt_trap_clear_recovery();
        ar->write_len = write_start;
        free(norm_name);
        return;
    }
    norm_name_owner = NULL;
    rt_trap_clear_recovery();
}

/// @brief `Archive.AddDir(name)` — record an explicit directory entry.
/// @details Appends a stored, zero-length entry whose normalized name ends in
///          `/`, preserving empty directories for ZIP consumers. The operation
///          is serialized with every other writer mutation and rolls back both
///          bytes and metadata if an allocation or validation step traps.
/// @param obj Archive handle opened in write mode.
/// @param name Relative directory name; a trailing slash is added when absent.
void rt_archive_add_dir(void *obj, rt_string name) {
    rt_archive_t *ar = archive_require(obj, "Archive: invalid archive");
    if (!ar)
        return;
    archive_rwlock_write_enter(ar->rw_lock);
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        archive_save_trap_error(
            saved_error, sizeof(saved_error), "Archive: failed to add directory entry");
        rt_trap_clear_recovery();
        archive_rwlock_write_exit(ar->rw_lock);
        rt_trap(saved_error);
        return;
    }
    archive_add_dir_locked(obj, name);
    rt_trap_clear_recovery();
    archive_rwlock_write_exit(ar->rw_lock);
}

/// @brief Append a writer's central directory/EOCD and atomically persist the image.
/// @details This transaction body assumes mode/state/count validation already
///          succeeded. It mutates only `write_buf`/`write_len`; the caller owns
///          rollback to @p finish_start on any false return or recovered trap.
///          The filesystem adapter cleans descriptors and sidecars before it
///          reports failure, so no live OS resource crosses this boundary.
/// @param ar Valid write-mode archive.
/// @param finish_start Original end of local records and central-directory offset.
/// @return One after the complete image is durably replaced, zero on a
///         returning-hook failure path.
static int archive_finish_write_image(rt_archive_t *ar, size_t finish_start) {
    uint32_t cd_offset = (uint32_t)finish_start;

    for (int i = 0; i < ar->write_entry_count; i++) {
        zip_entry_t *e = &ar->write_entries[i];
        size_t name_len = strlen(e->name);

        uint8_t central_header[ZIP_CENTRAL_HEADER_SIZE];
        write_u32(central_header, ZIP_CENTRAL_HEADER_SIG);
        write_u16(central_header + 4, ZIP_VERSION_MADE);
        write_u16(central_header + 6, ZIP_VERSION_NEEDED);
        write_u16(central_header + 8, 0);
        write_u16(central_header + 10, e->method);
        write_u16(central_header + 12, e->mod_time);
        write_u16(central_header + 14, e->mod_date);
        write_u32(central_header + 16, e->crc32);
        write_u32(central_header + 20, e->compressed_size);
        write_u32(central_header + 24, e->uncompressed_size);
        write_u16(central_header + 28, (uint16_t)name_len);
        write_u16(central_header + 30, 0);
        write_u16(central_header + 32, 0);
        write_u16(central_header + 34, 0);
        write_u16(central_header + 36, 0);
        write_u32(central_header + 38, e->is_directory ? 0x10 : 0);
        write_u32(central_header + 42, e->local_offset);

        if (!write_bytes(ar, central_header, ZIP_CENTRAL_HEADER_SIZE) ||
            !write_bytes(ar, (const uint8_t *)e->name, name_len))
            return 0;
    }

    if (ar->write_len < finish_start || ar->write_len - finish_start > UINT32_MAX) {
        rt_trap("Archive: ZIP64 archives are not supported");
        return 0;
    }
    uint32_t cd_size = (uint32_t)(ar->write_len - finish_start);

    uint8_t eocd[ZIP_END_RECORD_SIZE];
    write_u32(eocd, ZIP_END_RECORD_SIG);
    write_u16(eocd + 4, 0);
    write_u16(eocd + 6, 0);
    write_u16(eocd + 8, (uint16_t)ar->write_entry_count);
    write_u16(eocd + 10, (uint16_t)ar->write_entry_count);
    write_u32(eocd + 12, cd_size);
    write_u32(eocd + 16, cd_offset);
    write_u16(eocd + 20, 0);

    if (!write_bytes(ar, eocd, ZIP_END_RECORD_SIZE) ||
        !archive_require_zip32_size(ar->write_len, "Archive: ZIP64 archives are not supported"))
        return 0;

    const char *cpath = archive_require_path(ar->path, "Archive: invalid path");
    if (!cpath || *cpath == '\0')
        return 0;
    return archive_write_file_all_utf8(
        cpath, ar->write_buf, ar->write_len, "Archive: failed to write archive file");
}

/// @brief Execute `Archive.Finish` while the caller owns the archive write lock.
/// @details Validates ZIP32 ceilings, appends the central directory and EOCD,
///          and atomically persists the resulting image. The pre-finish buffer
///          length is restored after every failure so callers may correct an
///          external filesystem condition and retry the same writer.
/// @param obj Valid archive handle already locked exclusively by the caller.
static void archive_finish_locked(void *obj) {
    rt_archive_t *ar = archive_require(obj, "Archive: invalid archive");
    if (!ar) {
        rt_trap("Archive: NULL archive");
        return;
    }
    if (!ar->is_writing) {
        rt_trap("Archive: cannot finish read-only archive");
        return;
    }
    if (ar->is_finished) {
        rt_trap("Archive: archive already finished");
        return;
    }
    if (!archive_require_zip16_count(ar->write_entry_count,
                                     "Archive: ZIP64 archives are not supported") ||
        !archive_require_zip32_size(ar->write_len, "Archive: ZIP64 archives are not supported"))
        return;

    size_t finish_start = ar->write_len;
    jmp_buf finish_recovery;
    rt_trap_set_recovery(&finish_recovery);
    if (setjmp(finish_recovery) != 0) {
        char saved_error[256];
        archive_save_trap_error(
            saved_error, sizeof(saved_error), "Archive: failed to finish archive");
        rt_trap_clear_recovery();
        ar->write_len = finish_start;
        rt_trap(saved_error);
        return;
    }
    int completed = archive_finish_write_image(ar, finish_start);
    rt_trap_clear_recovery();
    if (!completed) {
        ar->write_len = finish_start;
        return;
    }

    ar->is_finished = true;

    // Free write buffer
    free(ar->write_buf);
    ar->write_buf = NULL;
    ar->write_len = 0;
    ar->write_cap = 0;
}

/// @brief `Archive.Finish()` — atomically publish a complete ZIP archive.
/// @details Serializes against every Add/AddDir call, delegates to the
///          retryable finish transaction, and releases the exclusive lock
///          before rethrowing any recovered trap. A successful call marks the
///          writer finished and releases its assembled byte buffer; Count and
///          Names remain queryable, while later writer mutations trap.
/// @param obj Archive handle opened in write mode.
void rt_archive_finish(void *obj) {
    rt_archive_t *ar = archive_require(obj, "Archive: invalid archive");
    if (!ar)
        return;
    archive_rwlock_write_enter(ar->rw_lock);
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        archive_save_trap_error(
            saved_error, sizeof(saved_error), "Archive: failed to finish archive");
        rt_trap_clear_recovery();
        archive_rwlock_write_exit(ar->rw_lock);
        rt_trap(saved_error);
        return;
    }
    archive_finish_locked(obj);
    rt_trap_clear_recovery();
    archive_rwlock_write_exit(ar->rw_lock);
}

//=============================================================================
// Static Methods
//=============================================================================

/// @brief `Archive.IsZip(path)` — fast probe for ZIP signature on disk.
///
/// Reads the first four bytes and tests for either the local-file-header
/// signature (typical archive) or the EOCD signature (empty archive
/// edge case where there are zero entries). Does NOT validate the
/// rest of the file — for that, call `Open` and let it trap.
/// Returns 0 for missing/unreadable files instead of trapping, so
/// scripts can probe untrusted paths safely.
///
/// @param path UTF-8 file path.
/// @return 1 if the file looks like a ZIP, 0 otherwise.
int8_t rt_archive_is_zip(rt_string path) {
    const char *cpath = NULL;
    if (!rt_file_path_from_vstr((const ZannaString *)path, &cpath) || !cpath || *cpath == '\0')
        return 0;

#if RT_PLATFORM_WINDOWS
    HANDLE h = archive_open_win_path(cpath, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING);
    if (h == INVALID_HANDLE_VALUE)
        return 0;

    uint8_t sig[4];
    DWORD read_count = 0;
    BOOL ok = ReadFile(h, sig, 4, &read_count, NULL);
    CloseHandle(h);

    if (!ok || read_count < 4)
        return 0;
#else
    int fd = archive_open_posix(cpath, O_RDONLY, 0);
    if (fd < 0)
        return 0;

    uint8_t sig[4];
    ssize_t n = read(fd, sig, 4);
    close(fd);

    if (n < 4)
        return 0;
#endif

    // Check for ZIP signature (local file header or empty archive EOCD)
    uint32_t magic = read_u32(sig);
    return (magic == ZIP_LOCAL_HEADER_SIG || magic == ZIP_END_RECORD_SIG) ? 1 : 0;
}

/// @brief `Archive.IsZipBytes(data)` — same as `IsZip` but for an in-memory buffer.
///
/// Useful when checking downloaded payloads or asset blobs before
/// committing to a full `FromBytes` parse. Same caveat as `IsZip`:
/// only the first four bytes are inspected.
///
/// @param data `rt_bytes` candidate.
/// @return 1 if the buffer starts with a ZIP signature, 0 otherwise.
int8_t rt_archive_is_zip_bytes(void *data) {
    if (!data)
        return 0;

    int64_t len = bytes_len(data);
    if (len < 4)
        return 0;

    const uint8_t *src = bytes_data(data);
    if (!src)
        return 0;

    uint32_t magic = read_u32(src);
    return (magic == ZIP_LOCAL_HEADER_SIG || magic == ZIP_END_RECORD_SIG) ? 1 : 0;
}
