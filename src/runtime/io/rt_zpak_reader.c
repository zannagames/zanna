//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/io/rt_zpak_reader.c
// Purpose: Runtime reader for ZPAK (Zanna Pack Archive) format. Parses archives
//          from memory buffers (embedded in .rodata) or files (.zpak on disk).
//          Supports per-entry DEFLATE decompression.
//
// Key invariants:
//   - Memory-backed archives do not copy the blob; the caller keeps it alive.
//   - File-backed archives read the TOC into memory but read entry data on demand.
//   - Archive references keep TOC entries and file handles alive across reads.
//   - File-backed seek/read pairs are serialized by a per-archive mutex.
//   - TOC entries are sorted by name after parsing for binary search.
//   - Returned data buffers are heap-allocated and caller-owned.
//
// Ownership/Lifetime:
//   - The final zpak_archive_t reference owns its entries, mutex, and file handle.
//   - zpak_read_entry returns malloc'd buffers.
//
// Links: ZpakWriter.cpp (build-time writer), rt_asset.c (consumer)
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements validated memory-backed and file-backed ZPAK reading.
 * @details Decodes little-endian headers and tables, rejects unsafe names and
 * overlapping payloads, maintains reference-counted archive ownership,
 * serializes shared file positioning, inflates compressed entries, and checks
 * version 2 payload CRCs before returning caller-owned bytes.
 */

#include "rt_zpak_reader.h"

#include "rt_alloc_size.h"
#include "rt_crc32.h"
#include "rt_file_stdio.h"
#include "rt_object.h"
#include "rt_platform.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if RT_PLATFORM_WINDOWS
#include <windows.h>
#else
#include <pthread.h>
#endif

#if RT_PLATFORM_WINDOWS
/** Seek adapter preserving 64-bit archive offsets on Windows. */
#define zpak_fseek(fp, off, whence) _fseeki64((fp), (__int64)(off), (whence))
/** Tell adapter preserving 64-bit archive offsets on Windows. */
#define zpak_ftell(fp) _ftelli64((fp))
#else
/** Seek adapter using the POSIX large-file stream interface. */
#define zpak_fseek(fp, off, whence) fseeko((fp), (off_t)(off), (whence))
/** Tell adapter using the POSIX large-file stream interface. */
#define zpak_ftell(fp) ftello((fp))
#endif

// ─── Runtime decompression ──────────────────────────────────────────────────
// We use the runtime's Bytes-based inflate and raw extraction helpers.
// Defined in rt_compress.c, rt_bytes.c, and rt_internal.h respectively.

/// @copydoc rt_bytes_from_raw()
extern void *rt_bytes_from_raw(const uint8_t *data, size_t len);
/// @copydoc rt_compress_inflate()
extern void *rt_compress_inflate(void *data);
/// @copydoc rt_bytes_extract_raw()
extern uint8_t *rt_bytes_extract_raw(void *bytes, size_t *out_len);

/// @brief Platform-neutral storage for one archive's file-position mutex.
typedef struct {
#if RT_PLATFORM_WINDOWS
    CRITICAL_SECTION native; ///< Win32 recursive critical section.
#else
    pthread_mutex_t native; ///< POSIX non-recursive mutex.
#endif
} zpak_read_lock_t;

/// @brief Allocate and initialize a native archive read mutex.
/// @return Opaque mutex pointer, or NULL when allocation/initialization fails.
static void *zpak_read_lock_new(void) {
    zpak_read_lock_t *lock = (zpak_read_lock_t *)malloc(sizeof(*lock));
    if (!lock)
        return NULL;
#if RT_PLATFORM_WINDOWS
    if (!InitializeCriticalSectionEx(&lock->native, 0, 0)) {
        free(lock);
        return NULL;
    }
#else
    if (pthread_mutex_init(&lock->native, NULL) != 0) {
        free(lock);
        return NULL;
    }
#endif
    return lock;
}

/// @brief Acquire an archive's opaque read mutex.
/// @param opaque Mutex returned by @ref zpak_read_lock_new; NULL is a no-op.
static void zpak_read_lock_acquire(void *opaque) {
    if (!opaque)
        return;
    zpak_read_lock_t *lock = (zpak_read_lock_t *)opaque;
#if RT_PLATFORM_WINDOWS
    EnterCriticalSection(&lock->native);
#else
    (void)pthread_mutex_lock(&lock->native);
#endif
}

/// @brief Release an archive's opaque read mutex.
/// @param opaque Mutex returned by @ref zpak_read_lock_new; NULL is a no-op.
static void zpak_read_lock_release(void *opaque) {
    if (!opaque)
        return;
    zpak_read_lock_t *lock = (zpak_read_lock_t *)opaque;
#if RT_PLATFORM_WINDOWS
    LeaveCriticalSection(&lock->native);
#else
    (void)pthread_mutex_unlock(&lock->native);
#endif
}

/// @brief Destroy and free an archive's opaque read mutex.
/// @param opaque Mutex returned by @ref zpak_read_lock_new; NULL is a no-op.
static void zpak_read_lock_free(void *opaque) {
    if (!opaque)
        return;
    zpak_read_lock_t *lock = (zpak_read_lock_t *)opaque;
#if RT_PLATFORM_WINDOWS
    DeleteCriticalSection(&lock->native);
#else
    (void)pthread_mutex_destroy(&lock->native);
#endif
    free(lock);
}

/// @brief Decrement the refcount on a GC object and free it when it reaches zero.
/// @param obj Owned runtime-managed object reference; NULL is a no-op.
static void zpak_release_object(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

// ─── ZPAK format constants ───────────────────────────────────────────────────

/** Four-byte signature that begins every supported ZPAK archive. */
static const uint8_t kMagic[4] = {'Z', 'P', 'A', 'K'};

// ─── Little-endian read helpers ─────────────────────────────────────────────

/// @brief Read a 2-byte little-endian unsigned integer from an unaligned byte pointer.
/// @param p Pointer to at least two readable bytes.
/// @return Host-order 16-bit value decoded without alignment assumptions.
static uint16_t read16LE(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

/// @brief Read a 4-byte little-endian unsigned integer from an unaligned byte pointer.
/// @param p Pointer to at least four readable bytes.
/// @return Host-order 32-bit value decoded without alignment assumptions.
static uint32_t read32LE(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/// @brief Read an 8-byte little-endian unsigned integer from an unaligned byte pointer.
/// @param p Pointer to at least eight readable bytes.
/// @return Host-order 64-bit value decoded without alignment assumptions.
static uint64_t read64LE(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= (uint64_t)p[i] << (i * 8);
    return v;
}

/// @brief Decoded and validated fields from one fixed-size ZPAK header.
typedef struct {
    uint16_t version;     ///< Supported format version selected by the producer.
    uint16_t flags;       ///< Version-specific flags after unknown-bit validation.
    uint32_t entry_count; ///< Number of table records declared by the producer.
    uint64_t toc_offset;  ///< Absolute byte offset of the table of contents.
    uint64_t toc_size;    ///< Encoded byte length of the table of contents.
} zpak_header_t;

/// @brief Validate that a bounded entry name is a safe relative asset path.
/// @details Rejects embedded NULs, absolute paths, drive/scheme separators,
///          backslashes, empty components, and `.` or `..` components. The
///          writer applies the same structural policy before serialization.
/// @param name Untrusted non-terminated name bytes from the table.
/// @param length Number of bytes in @p name.
/// @return Non-zero when the name can be copied and used as an asset key.
static int zpak_name_is_safe(const uint8_t *name, size_t length) {
    if (!name || length == 0 || name[0] == '/')
        return 0;
    size_t component = 0;
    for (size_t i = 0; i <= length; ++i) {
        if (i < length) {
            if (name[i] == '\0' || name[i] == '\\' || name[i] == ':')
                return 0;
            if (name[i] != '/')
                continue;
        }
        size_t component_length = i - component;
        if (component_length == 0 || (component_length == 1 && name[component] == '.') ||
            (component_length == 2 && name[component] == '.' && name[component + 1] == '.'))
            return 0;
        component = i + 1;
    }
    return 1;
}

/// @brief Validate and decode a ZPAK header before any count-sized allocation.
/// @details This routine enforces supported versions and flags, zero reserved
///          bytes, a shared 64 MiB TOC ceiling, exact archive-tail placement,
///          and a minimum encoded record size for every declared entry.
/// @param bytes At least @ref RT_ZPAK_HEADER_SIZE bytes of untrusted header data.
/// @param archive_size Complete containing blob or file size.
/// @param out Receives decoded fields only on success.
/// @return Non-zero when the header and table bounds are structurally valid.
static int zpak_parse_header(const uint8_t *bytes, uint64_t archive_size, zpak_header_t *out) {
    if (!bytes || !out || archive_size < RT_ZPAK_HEADER_SIZE || memcmp(bytes, kMagic, 4) != 0)
        return 0;

    zpak_header_t header;
    header.version = read16LE(bytes + 4);
    header.flags = read16LE(bytes + 6);
    header.entry_count = read32LE(bytes + 8);
    header.toc_offset = read64LE(bytes + 12);
    header.toc_size = read64LE(bytes + 20);
    if (read32LE(bytes + 28) != 0)
        return 0;

    size_t minimum_record = 0;
    if (header.version == RT_ZPAK_VERSION_1) {
        if ((header.flags & ~RT_ZPAK_V1_HEADER_FLAGS) != 0)
            return 0;
        minimum_record = RT_ZPAK_V1_ENTRY_MIN_SIZE;
    } else if (header.version == RT_ZPAK_VERSION_2) {
        if ((header.flags & ~RT_ZPAK_V2_HEADER_FLAGS) != 0 ||
            (header.flags & RT_ZPAK_HEADER_FLAG_ENTRY_CRC32) == 0)
            return 0;
        minimum_record = RT_ZPAK_V2_ENTRY_MIN_SIZE;
    } else {
        return 0;
    }

    if (header.toc_size > RT_ZPAK_MAX_TOC_SIZE || header.toc_offset < RT_ZPAK_HEADER_SIZE ||
        header.toc_offset > archive_size || header.toc_size > archive_size - header.toc_offset ||
        header.toc_offset + header.toc_size != archive_size)
        return 0;
    if ((header.entry_count == 0) != (header.toc_size == 0))
        return 0;
    if (header.entry_count == 0 && (header.flags & RT_ZPAK_HEADER_FLAG_COMPRESSED) != 0)
        return 0;
    if (header.entry_count > 0 && (uint64_t)header.entry_count > header.toc_size / minimum_record)
        return 0;

    *out = header;
    return 1;
}

/// @brief qsort comparator that orders entries by stored payload offset.
/// @details Names break ties so validation order is deterministic even for
///          legal zero-length entries sharing one offset.
/// @param a Pointer to the first @ref zpak_entry_t.
/// @param b Pointer to the second @ref zpak_entry_t.
/// @return Negative, zero, or positive according to offset then name ordering.
static int entry_offset_cmp(const void *a, const void *b) {
    const zpak_entry_t *ea = (const zpak_entry_t *)a;
    const zpak_entry_t *eb = (const zpak_entry_t *)b;
    if (ea->data_offset < eb->data_offset)
        return -1;
    if (ea->data_offset > eb->data_offset)
        return 1;
    return strcmp(ea->name, eb->name);
}

/// @brief qsort comparator that orders entries lexicographically by name.
/// @param a Pointer to the first @ref zpak_entry_t.
/// @param b Pointer to the second @ref zpak_entry_t.
/// @return Result of bytewise C-string comparison of the two validated names.
static int entry_name_cmp(const void *a, const void *b) {
    const zpak_entry_t *ea = (const zpak_entry_t *)a;
    const zpak_entry_t *eb = (const zpak_entry_t *)b;
    return strcmp(ea->name, eb->name);
}

// ─── TOC parsing (shared by memory and file paths) ──────────────────────────

/// @brief Parse and completely validate the TOC region from a byte buffer.
/// @details The encoded byte budget is checked against the declared count
///          before allocation. After decoding, entries are sorted by payload
///          offset to reject overlap and then by name to reject adjacent
///          duplicates and prepare binary lookup. The returned order is by
///          name. No partially initialized entry escapes on failure.
/// @param toc_data Pointer to the complete untrusted TOC byte range.
/// @param toc_size Size of the TOC region in bytes.
/// @param expected_count Exact number of records declared by the header.
/// @param version Validated format version from @ref zpak_parse_header.
/// @param header_flags Validated header flags used for aggregate consistency.
/// @param data_limit Exclusive payload limit, equal to the TOC offset.
/// @param out_count Set to the number of entries only on success.
/// @return Heap-allocated name-sorted entries, or NULL on error.
static zpak_entry_t *parse_toc(const uint8_t *toc_data,
                               size_t toc_size,
                               uint32_t expected_count,
                               uint16_t version,
                               uint16_t header_flags,
                               uint64_t data_limit,
                               uint32_t *out_count) {
    *out_count = 0;
    if (expected_count == 0)
        return NULL;
    if (!toc_data || toc_size == 0) {
        return NULL;
    }

    size_t fixed_size =
        version == RT_ZPAK_VERSION_2 ? RT_ZPAK_V2_ENTRY_FIXED_SIZE : RT_ZPAK_V1_ENTRY_FIXED_SIZE;
    size_t minimum_size = 2u + 1u + fixed_size;
    if ((size_t)expected_count > toc_size / minimum_size ||
        !rt_alloc_count_ok(expected_count, sizeof(zpak_entry_t)))
        return NULL;

    zpak_entry_t *entries = (zpak_entry_t *)calloc(expected_count, sizeof(zpak_entry_t));
    if (!entries)
        return NULL;

    const uint8_t *p = toc_data;
    const uint8_t *end = toc_data + toc_size;
    uint32_t i = 0;
    uint32_t initialized = 0;
    int observed_compressed = 0;

    while (i < expected_count) {
        if ((size_t)(end - p) < 2)
            goto fail;
        // name_len (2 bytes)
        uint16_t name_len = read16LE(p);
        p += 2;

        if (name_len == 0 || (size_t)(end - p) < name_len)
            goto fail;

        if (!zpak_name_is_safe(p, name_len))
            goto fail;

        // name (UTF-8 bytes)
        entries[i].name = (char *)malloc((size_t)name_len + 1u);
        if (!entries[i].name)
            goto fail;
        initialized = i + 1;
        memcpy(entries[i].name, p, name_len);
        entries[i].name[name_len] = '\0';
        p += name_len;

        if ((size_t)(end - p) < fixed_size)
            goto fail;

        // data_offset (8 bytes)
        entries[i].data_offset = read64LE(p);
        p += 8;

        // data_size — original uncompressed (8 bytes)
        entries[i].data_size = read64LE(p);
        p += 8;

        // stored_size (8 bytes)
        entries[i].stored_size = read64LE(p);
        p += 8;

        // flags (2 bytes)
        uint16_t flags = read16LE(p);
        p += 2;
        if ((flags & ~RT_ZPAK_ENTRY_FLAGS_KNOWN) != 0)
            goto fail;
        entries[i].compressed = (flags & RT_ZPAK_ENTRY_FLAG_COMPRESSED) ? 1 : 0;
        observed_compressed |= entries[i].compressed;

        // reserved (2 bytes)
        if (read16LE(p) != 0)
            goto fail;
        p += 2;

        // Version 2 appends CRC-32 of the original uncompressed bytes.
        if (version == RT_ZPAK_VERSION_2) {
            entries[i].crc32 = read32LE(p);
            p += 4;
        }

        if ((!entries[i].compressed && entries[i].stored_size != entries[i].data_size) ||
            (entries[i].compressed && (entries[i].stored_size == 0 || entries[i].data_size == 0)))
            goto fail;
        if (entries[i].data_offset < RT_ZPAK_HEADER_SIZE || entries[i].data_offset > data_limit ||
            entries[i].stored_size > data_limit - entries[i].data_offset)
            goto fail;

        i++;
    }

    if (i != expected_count || p != end)
        goto fail;

    if (!!(header_flags & RT_ZPAK_HEADER_FLAG_COMPRESSED) != !!observed_compressed)
        goto fail;

    if (expected_count > 1)
        qsort(entries, expected_count, sizeof(*entries), entry_offset_cmp);
    uint64_t previous_end = RT_ZPAK_HEADER_SIZE;
    for (uint32_t j = 0; j < expected_count; ++j) {
        if (entries[j].stored_size == 0)
            continue;
        if (entries[j].data_offset < previous_end)
            goto fail;
        previous_end = entries[j].data_offset + entries[j].stored_size;
    }

    if (expected_count > 1)
        qsort(entries, expected_count, sizeof(*entries), entry_name_cmp);
    for (uint32_t j = 1; j < expected_count; ++j) {
        if (strcmp(entries[j - 1].name, entries[j].name) == 0)
            goto fail;
    }

    *out_count = i;
    return entries;

fail:
    for (uint32_t j = 0; j < initialized; ++j) {
#ifdef _MSC_VER
#pragma warning(suppress : 6001)
#endif
        if (entries[j].name)
            free(entries[j].name);
    }
    free(entries);
    *out_count = 0;
    return NULL;
}

/// @brief Measure a seekable archive file and rewind it to the beginning.
/// @details Uses the platform's large-file seek/tell adapters. The output is
///          modified only after both the end-position query and rewind succeed.
/// @param file Open binary stream supporting seek and tell.
/// @param out_size Receives the non-negative byte length on success.
/// @return 1 when measured and rewound successfully, otherwise 0.
static int zpak_file_size(FILE *file, uint64_t *out_size) {
    if (!file || !out_size)
        return 0;
    if (zpak_fseek(file, 0, SEEK_END) != 0)
        return 0;
    int64_t pos = (int64_t)zpak_ftell(file);
    if (pos < 0)
        return 0;
    if (zpak_fseek(file, 0, SEEK_SET) != 0)
        return 0;
    *out_size = (uint64_t)pos;
    return 1;
}

// ─── zpak_open_memory ────────────────────────────────────────────────────────

/// @brief Open a ZPAK archive from an in-memory buffer (e.g., embedded .rodata blob).
/// @details The buffer is borrowed, not copied — the caller must keep it alive
///          for the lifetime of the archive. The complete header, TOC, names,
///          flags, and payload layout are validated before publication.
///          Entry metadata and names are copied into archive-owned storage.
/// @param data Complete encoded archive bytes to borrow.
/// @param size Number of readable bytes at @p data.
/// @return Archive with one ownership reference, or NULL for malformed input
///         or allocation failure.
zpak_archive_t *zpak_open_memory(const uint8_t *data, size_t size) {
    if (!data || size < RT_ZPAK_HEADER_SIZE)
        return NULL;

    zpak_header_t header;
    if (!zpak_parse_header(data, (uint64_t)size, &header) || header.toc_size > (uint64_t)SIZE_MAX)
        return NULL;

    uint32_t parsed_count = 0;
    zpak_entry_t *entries = NULL;
    if (header.entry_count > 0) {
        entries = parse_toc(data + (size_t)header.toc_offset,
                            (size_t)header.toc_size,
                            header.entry_count,
                            header.version,
                            header.flags,
                            header.toc_offset,
                            &parsed_count);
    }
    if (parsed_count != header.entry_count || (header.entry_count > 0 && !entries))
        return NULL;

    // Create archive
    zpak_archive_t *archive = (zpak_archive_t *)calloc(1, sizeof(zpak_archive_t));
    if (!archive) {
        for (uint32_t i = 0; i < parsed_count; i++)
            free(entries[i].name);
        free(entries);
        return NULL;
    }

    archive->entries = entries;
    archive->count = parsed_count;
    archive->version = header.version;
    archive->flags = header.flags;
    archive->blob = data;
    archive->blob_size = size;
    archive->file = NULL;
    archive->refcount = 1;

    return archive;
}

// ─── zpak_open_file ──────────────────────────────────────────────────────────

/// @brief Open a ZPAK archive from a .zpak file on disk.
/// @details Reads the header and TOC into memory. Entry data is read on demand
///          via zpak_read_entry. The file handle is kept open until zpak_close.
/// @param path NUL-terminated native UTF-8 file path.
/// @param no_follow Non-zero to require a POSIX regular file and reject a final
///                  symlink; ignored on Windows.
/// @return Archive with one ownership reference, or NULL on path, format,
///         I/O, mutex, or allocation failure.
static zpak_archive_t *zpak_open_file_impl(const char *path, int no_follow) {
    if (!path)
        return NULL;

#if !RT_PLATFORM_WINDOWS
    FILE *f = NULL;
    if (no_follow) {
        struct stat lst;
        if (lstat(path, &lst) != 0 || !S_ISREG(lst.st_mode))
            return NULL;
        int flags = O_RDONLY;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
        int fd = open(path, flags);
        if (fd < 0)
            return NULL;
#if defined(FD_CLOEXEC) && !defined(O_CLOEXEC)
        int fd_flags = fcntl(fd, F_GETFD);
        if (fd_flags >= 0)
            (void)fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);
#endif
        f = fdopen(fd, "rb");
        if (!f) {
            close(fd);
            return NULL;
        }
    } else {
        f = rt_file_stdio_open_utf8(path, "rb");
    }
#else
    FILE *f = rt_file_stdio_open_utf8(path, "rb");
#endif
    if (!f)
        return NULL;

#if !RT_PLATFORM_WINDOWS
    if (no_follow) {
        struct stat st;
        if (fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode)) {
            fclose(f);
            return NULL;
        }
    }
#else
    (void)no_follow;
#endif

    uint64_t file_size = 0;
    if (!zpak_file_size(f, &file_size) || file_size < RT_ZPAK_HEADER_SIZE) {
        fclose(f);
        return NULL;
    }

    uint8_t header_bytes[RT_ZPAK_HEADER_SIZE];
    if (fread(header_bytes, 1, RT_ZPAK_HEADER_SIZE, f) != RT_ZPAK_HEADER_SIZE) {
        fclose(f);
        return NULL;
    }

    zpak_header_t header;
    if (!zpak_parse_header(header_bytes, file_size, &header) ||
        header.toc_size > (uint64_t)SIZE_MAX) {
        fclose(f);
        return NULL;
    }

    uint32_t parsed_count = 0;
    zpak_entry_t *entries = NULL;
    if (header.entry_count > 0) {
        uint8_t *toc_buf = (uint8_t *)malloc((size_t)header.toc_size);
        if (!toc_buf) {
            fclose(f);
            return NULL;
        }

        if (zpak_fseek(f, header.toc_offset, SEEK_SET) != 0) {
            free(toc_buf);
            fclose(f);
            return NULL;
        }

        if (fread(toc_buf, 1, (size_t)header.toc_size, f) != (size_t)header.toc_size) {
            free(toc_buf);
            fclose(f);
            return NULL;
        }

        entries = parse_toc(toc_buf,
                            (size_t)header.toc_size,
                            header.entry_count,
                            header.version,
                            header.flags,
                            header.toc_offset,
                            &parsed_count);
        free(toc_buf);
        if (parsed_count != header.entry_count || !entries) {
            fclose(f);
            return NULL;
        }
    }

    // Create archive
    zpak_archive_t *archive = (zpak_archive_t *)calloc(1, sizeof(zpak_archive_t));
    if (!archive) {
        for (uint32_t i = 0; i < parsed_count; i++)
            free(entries[i].name);
        free(entries);
        fclose(f);
        return NULL;
    }

    archive->entries = entries;
    archive->count = parsed_count;
    archive->version = header.version;
    archive->flags = header.flags;
    archive->blob = NULL;
    archive->blob_size = 0;
    archive->file = f;
    archive->file_size = file_size;
    archive->refcount = 1;
    archive->read_lock = zpak_read_lock_new();
    if (!archive->read_lock) {
        zpak_close(archive);
        return NULL;
    }

    return archive;
}

/// @brief Open and validate a file-backed ZPAK archive.
/// @details The Unicode-aware platform adapter opens the file, its TOC is
///          retained in memory, and payload reads share a per-archive mutex.
/// @param path NUL-terminated native UTF-8 path to a ZPAK file.
/// @return Archive with one ownership reference, or NULL on any failure.
zpak_archive_t *zpak_open_file(const char *path) {
    return zpak_open_file_impl(path, 0);
}

/// @brief Open a regular ZPAK file while rejecting its final POSIX symlink.
/// @details POSIX combines pre-open lstat, O_NOFOLLOW when available, and
///          post-open fstat regular-file validation. Windows uses the normal
///          Unicode file open because discovery rejects reparse points.
/// @param path NUL-terminated native UTF-8 path to a candidate pack.
/// @return Archive with one ownership reference, or NULL on any failure.
zpak_archive_t *zpak_open_file_no_follow(const char *path) {
    return zpak_open_file_impl(path, 1);
}

/// @brief Retain a live archive for work performed outside its owner lock.
/// @param archive Archive whose lifetime should be extended.
/// @return Non-zero when the reference was acquired, zero otherwise.
int zpak_retain(zpak_archive_t *archive) {
    if (!archive)
        return 0;
    size_t refs = rt_atomic_load_size(&archive->refcount, __ATOMIC_ACQUIRE);
    for (;;) {
        if (refs == 0 || refs == SIZE_MAX)
            return 0;
        size_t expected = refs;
        if (rt_atomic_compare_exchange_size(
                &archive->refcount, &expected, refs + 1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return 1;
        refs = expected;
    }
}

// ─── zpak_find ───────────────────────────────────────────────────────────────

/// @brief Find an entry by name using binary search on the sorted TOC.
/// @details Archives with at most 16 entries use a linear scan; larger
///          archives use binary search. The returned pointer borrows archive
///          storage and is invalid after the final @ref zpak_close.
/// @param archive Live parsed archive.
/// @param name Exact NUL-terminated asset name to match bytewise.
/// @return Borrowed entry metadata, or NULL when absent or input is invalid.
const zpak_entry_t *zpak_find(const zpak_archive_t *archive, const char *name) {
    if (!archive || !name || archive->count == 0)
        return NULL;

    // Linear scan for small archives, binary search for larger ones.
    // Avoids const-cast issues with the key struct.
    if (archive->count <= 16) {
        for (uint32_t i = 0; i < archive->count; i++) {
            if (strcmp(archive->entries[i].name, name) == 0)
                return &archive->entries[i];
        }
        return NULL;
    }

    // Binary search for larger archives.
    uint32_t lo = 0, hi = archive->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        int cmp = strcmp(archive->entries[mid].name, name);
        if (cmp == 0)
            return &archive->entries[mid];
        if (cmp < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

// ─── zpak_read_entry ─────────────────────────────────────────────────────────

/// @brief Check a decoded entry's uncompressed bytes against its versioned CRC.
/// @details Version 1 intentionally has no checksum and therefore succeeds.
///          Version 2 compares the stored TOC checksum with the shared IEEE
///          CRC-32 implementation after decompression or raw copying.
/// @param archive Valid archive that owns @p entry.
/// @param entry Valid decoded entry metadata.
/// @param data Uncompressed bytes; may be NULL only when @p length is zero.
/// @param length Number of uncompressed bytes in @p data.
/// @return Non-zero when no checksum is required or the checksum matches.
static int zpak_entry_checksum_valid(const zpak_archive_t *archive,
                                     const zpak_entry_t *entry,
                                     const uint8_t *data,
                                     size_t length) {
    if (archive->version == RT_ZPAK_VERSION_1)
        return 1;
    return archive->version == RT_ZPAK_VERSION_2 &&
           (archive->flags & RT_ZPAK_HEADER_FLAG_ENTRY_CRC32) != 0 &&
           rt_crc32_compute(data, length) == entry->crc32;
}

/// @brief Read and decompress an entry's data (returns malloc'd buffer, caller frees).
/// @details For compressed entries, reads the compressed data then inflates via
///          DEFLATE. For uncompressed entries, copies raw bytes from the archive.
///          Version 2 CRC-32 is verified over the resulting uncompressed bytes
///          before ownership is returned to the caller. File-backed seek/read
///          operations are serialized; memory-backed reads copy from the
///          borrowed blob. A successful zero-length read still returns a
///          freeable non-NULL allocation.
/// @param archive Live archive that owns @p entry.
/// @param entry Entry pointer obtained from @ref zpak_find on @p archive.
/// @param out_size Receives the uncompressed byte count; set to zero before
///                 fallible read/decompression work.
/// @return Malloc-owned uncompressed bytes for the caller to free, or NULL on
///         bounds, I/O, allocation, decompression, size, or checksum failure.
uint8_t *zpak_read_entry(const zpak_archive_t *archive,
                         const zpak_entry_t *entry,
                         size_t *out_size) {
    if (!archive || !entry || !out_size)
        return NULL;
    *out_size = 0;
    if (entry->stored_size > (uint64_t)SIZE_MAX || entry->data_size > (uint64_t)SIZE_MAX)
        return NULL;

    uint8_t *stored = NULL;
    size_t stored_len = (size_t)entry->stored_size;
    size_t data_len = (size_t)entry->data_size;

    if (archive->blob) {
        // Memory-backed: data is directly in the blob
        if (entry->data_offset > (uint64_t)archive->blob_size ||
            stored_len > archive->blob_size - (size_t)entry->data_offset)
            return NULL;
        stored = (uint8_t *)malloc(stored_len > 0 ? stored_len : 1);
        if (!stored)
            return NULL;
        if (stored_len > 0)
            memcpy(stored, archive->blob + entry->data_offset, stored_len);
    } else if (archive->file) {
        // File-backed: seek and read
        if (entry->data_offset > archive->file_size ||
            stored_len > archive->file_size - entry->data_offset)
            return NULL;
        stored = (uint8_t *)malloc(stored_len > 0 ? stored_len : 1);
        if (!stored)
            return NULL;
        int read_ok = 1;
        zpak_read_lock_acquire(archive->read_lock);
        if (zpak_fseek(archive->file, entry->data_offset, SEEK_SET) != 0)
            read_ok = 0;
        if (read_ok && stored_len > 0 && fread(stored, 1, stored_len, archive->file) != stored_len)
            read_ok = 0;
        zpak_read_lock_release(archive->read_lock);
        if (!read_ok) {
            free(stored);
            return NULL;
        }
    } else {
        return NULL;
    }

    if (entry->compressed && stored_len > 0) {
        // Decompress via runtime DEFLATE: wrap in Bytes, inflate, extract raw
        void *bytes_obj = rt_bytes_from_raw(stored, stored_len);
        free(stored);
        if (!bytes_obj)
            return NULL;

        void *inflated = rt_compress_inflate(bytes_obj);
        zpak_release_object(bytes_obj);
        if (!inflated)
            return NULL;

        // Extract to malloc'd buffer (caller-owned)
        size_t result_len = 0;
        uint8_t *result = rt_bytes_extract_raw(inflated, &result_len);
        zpak_release_object(inflated);
        if (result_len != data_len) {
            free(result);
            return NULL;
        }
        if (!result) {
            result = (uint8_t *)malloc(1);
            if (!result)
                return NULL;
        }
        if (!zpak_entry_checksum_valid(archive, entry, result, result_len)) {
            free(result);
            return NULL;
        }
        *out_size = result_len;
        return result;
    }

    // Uncompressed — return stored data directly
    if (stored_len != data_len) {
        free(stored);
        return NULL;
    }
    if (!zpak_entry_checksum_valid(archive, entry, stored, stored_len)) {
        free(stored);
        return NULL;
    }
    *out_size = stored_len;
    return stored;
}

// ─── zpak_close ──────────────────────────────────────────────────────────────

/// @brief Release an archive, destroying owned resources after the final reference.
/// @details The atomic reference count permits concurrent owners. The final
///          release frees entry names and metadata, closes a file backing,
///          destroys its read mutex, and frees the archive structure; it never
///          frees a memory-backed archive's borrowed blob.
/// @param archive Owned archive reference to release; NULL is a no-op.
void zpak_close(zpak_archive_t *archive) {
    if (!archive)
        return;

    size_t refs = rt_atomic_load_size(&archive->refcount, __ATOMIC_ACQUIRE);
    for (;;) {
        if (refs == 0)
            return;
        size_t expected = refs;
        if (rt_atomic_compare_exchange_size(
                &archive->refcount, &expected, refs - 1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            break;
        refs = expected;
    }
    if (refs > 1)
        return;

    if (archive->entries) {
        for (uint32_t i = 0; i < archive->count; i++) {
#ifdef _MSC_VER
#pragma warning(suppress : 6001)
#endif
            if (archive->entries[i].name)
                free(archive->entries[i].name);
        }
        free(archive->entries);
    }

    if (archive->file)
        fclose(archive->file);

    zpak_read_lock_free(archive->read_lock);

    free(archive);
}
