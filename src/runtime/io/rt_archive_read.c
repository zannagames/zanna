//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/io/rt_archive_read.c
// Purpose: ZIP central-directory parsing and entry extraction (the read side of
//          the archive runtime): parse_central_directory, find_entry, and
//          read_entry_data (stored + DEFLATE). Split out of rt_archive.c; shares
//          the ZIP types/constants via rt_archive_internal.h.
//
// Key invariants:
//   - Parsing validates offsets/sizes against the buffer length and rejects
//     encrypted / ZIP64 / malformed extra fields before extraction.
//   - read_entry_data verifies CRC-32 and the uncompressed size after inflate.
//
// Ownership/Lifetime:
//   - parse_central_directory fills the archive's entry array (owned by it);
//     read_entry_data returns a fresh Bytes object owned by the caller.
//
// Links: src/runtime/io/rt_archive.c,
//        src/runtime/io/rt_archive_internal.h,
//        src/runtime/io/rt_compress.h,
//        src/runtime/core/rt_crc32.h
//
//===----------------------------------------------------------------------===//
/**
 * @file
 * @brief Implements bounded ZIP central-directory parsing and entry decoding.
 * @details Locates and validates the ZIP32 end record, constructs collision-
 * checked name indexes, cross-checks central entries with disjoint local
 * records, rejects unsupported metadata, inflates supported payloads within
 * sampled limits, and verifies uncompressed sizes and CRC-32 values.
 */

#include "rt_archive.h"
#include "rt_archive_internal.h"

#include "rt_bytes.h"
#include "rt_compress.h"
#include "rt_crc32.h"
#include "rt_internal.h"
#include "rt_object.h"

#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// @brief Read a little-endian uint16 from `p` (file-local copy).
/// @param p Pointer to at least two accessible encoded bytes.
/// @return Decoded host-order 16-bit value.
static inline uint16_t read_u16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

/// @brief Read a little-endian uint32 from `p` (file-local copy).
/// @param p Pointer to at least four accessible encoded bytes.
/// @return Decoded host-order 32-bit value.
static inline uint32_t read_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/// @brief Direct pointer to a Bytes GC object's buffer (file-local copy).
/// @param obj Runtime Bytes handle.
/// @return Borrowed pointer to the object's writable byte storage.
static inline uint8_t *bytes_data(void *obj) {
    return rt_bytes_data(obj);
}

/// @brief Compute a stable FNV-1a hash for a normalized archive entry name.
/// @details The parser and lookup path use the same byte-wise hash. Hash
///          collisions remain harmless because probes compare full names.
/// @param name NUL-terminated entry name validated by the central parser.
/// @return 64-bit FNV-1a hash value.
static uint64_t archive_name_hash(const char *name) {
    uint64_t hash = UINT64_C(14695981039346656037);
    const unsigned char *cursor = (const unsigned char *)name;
    while (cursor && *cursor) {
        hash ^= (uint64_t)*cursor++;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

/// @brief Choose a power-of-two slot count for an archive name index.
/// @details Keeps load at or below 0.5 so successful and unsuccessful probes
///          remain short even for archives near the ZIP32 entry-count limit.
/// @param entry_count Number of names that will be inserted.
/// @return Power-of-two slot count, or zero when @p entry_count is zero or
///         capacity arithmetic would overflow.
static size_t archive_name_index_capacity(size_t entry_count) {
    if (entry_count == 0)
        return 0;
    if (entry_count > SIZE_MAX / 2)
        return 0;
    size_t needed = entry_count * 2;
    size_t capacity = 16;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2)
            return 0;
        capacity *= 2;
    }
    return capacity;
}

/// @brief Insert one parsed entry into an open-addressed name index.
/// @details Empty slots contain zero and occupied slots contain `index + 1`.
///          Linear probing is bounded by @p slot_count. Encountering an equal
///          full name reports a duplicate rather than inserting it.
/// @param entries Parsed entry array containing @p entry_index.
/// @param slots Zero-initialized slot array.
/// @param slot_count Power-of-two slot count.
/// @param entry_index Index of the newly parsed entry.
/// @return One on insertion, zero for a duplicate or malformed/full index.
static int archive_name_index_insert(zip_entry_t *entries,
                                     int32_t *slots,
                                     size_t slot_count,
                                     int entry_index) {
    if (!entries || !slots || slot_count == 0 || (slot_count & (slot_count - 1)) != 0 ||
        entry_index < 0 || !entries[entry_index].name)
        return 0;
    size_t slot = (size_t)archive_name_hash(entries[entry_index].name) & (slot_count - 1);
    for (size_t probe = 0; probe < slot_count; ++probe) {
        int32_t encoded = slots[slot];
        if (encoded == 0) {
            slots[slot] = (int32_t)entry_index + 1;
            return 1;
        }
        int existing_index = encoded - 1;
        if (existing_index >= 0 && entries[existing_index].name &&
            strcmp(entries[existing_index].name, entries[entry_index].name) == 0)
            return 0;
        slot = (slot + 1) & (slot_count - 1);
    }
    return 0;
}

/// @brief Byte interval occupied by one local header, name, extra field, and payload.
typedef struct archive_local_range {
    size_t begin; ///< Inclusive local-header offset.
    size_t end;   ///< Exclusive end of compressed payload.
} archive_local_range_t;

/// @brief Compare two local-entry ranges by start offset for `qsort`.
/// @param lhs Pointer to the first @ref archive_local_range_t.
/// @param rhs Pointer to the second @ref archive_local_range_t.
/// @return Negative, zero, or positive according to ascending begin offset.
static int archive_compare_local_ranges(const void *lhs, const void *rhs) {
    const archive_local_range_t *a = (const archive_local_range_t *)lhs;
    const archive_local_range_t *b = (const archive_local_range_t *)rhs;
    return (a->begin > b->begin) - (a->begin < b->begin);
}

/// @brief Validate every central entry against its referenced local header.
/// @details Requires matching names, flags, methods, CRCs, sizes, and supported
///          extra fields. The complete local record must end before the central
///          directory. Sorted byte intervals additionally reject aliases and
///          crafted local records embedded inside another entry's payload.
/// @param ar Archive containing the full immutable ZIP buffer.
/// @param entries Parsed central-directory entries.
/// @param entry_count Number of entries to validate.
/// @param central_offset First byte of the central directory.
/// @return True only when all local records are structurally consistent and disjoint.
static bool archive_validate_local_entries(rt_archive_t *ar,
                                           zip_entry_t *entries,
                                           int entry_count,
                                           size_t central_offset) {
    if (!ar || entry_count < 0 || central_offset > ar->data_len)
        return false;
    if (entry_count == 0)
        return true;
    if (!entries || (size_t)entry_count > SIZE_MAX / sizeof(archive_local_range_t))
        return false;

    archive_local_range_t *ranges =
        (archive_local_range_t *)calloc((size_t)entry_count, sizeof(*ranges));
    if (!ranges) {
        ar->parse_error = ARCHIVE_PARSE_ERROR_OOM;
        return false;
    }

    bool valid = true;
    for (int index = 0; index < entry_count; ++index) {
        zip_entry_t *entry = &entries[index];
        size_t local_offset = (size_t)entry->local_offset;
        if (!entry->name || local_offset > central_offset ||
            central_offset - local_offset < ZIP_LOCAL_HEADER_SIZE) {
            valid = false;
            break;
        }

        const uint8_t *local = ar->data + local_offset;
        if (read_u32(local) != ZIP_LOCAL_HEADER_SIG) {
            valid = false;
            break;
        }
        uint16_t local_name_len = read_u16(local + 26);
        uint16_t local_extra_len = read_u16(local + 28);
        size_t header_total =
            ZIP_LOCAL_HEADER_SIZE + (size_t)local_name_len + (size_t)local_extra_len;
        size_t expected_name_len = strlen(entry->name);
        if (header_total > central_offset - local_offset ||
            (size_t)entry->compressed_size > central_offset - local_offset - header_total ||
            expected_name_len != (size_t)local_name_len ||
            memcmp(local + ZIP_LOCAL_HEADER_SIZE, entry->name, expected_name_len) != 0 ||
            read_u16(local + 4) != entry->version_needed || read_u16(local + 6) != entry->flags ||
            read_u16(local + 8) != entry->method || read_u32(local + 14) != entry->crc32 ||
            read_u32(local + 18) != entry->compressed_size ||
            read_u32(local + 22) != entry->uncompressed_size ||
            archive_extra_is_malformed_or_zip64(local + ZIP_LOCAL_HEADER_SIZE + local_name_len,
                                                local_extra_len)) {
            valid = false;
            break;
        }

        ranges[index].begin = local_offset;
        ranges[index].end = local_offset + header_total + (size_t)entry->compressed_size;
    }

    if (valid) {
        qsort(ranges, (size_t)entry_count, sizeof(*ranges), archive_compare_local_ranges);
        for (int index = 1; index < entry_count; ++index) {
            if (ranges[index].begin < ranges[index - 1].end) {
                valid = false;
                break;
            }
        }
    }
    free(ranges);
    return valid;
}

// ZIP Parsing (for reading)
//=============================================================================

/// @brief Locate the End of Central Directory (EOCD) record within a ZIP buffer.
///
/// The EOCD is at the tail of every ZIP file, just before any optional
/// archive comment (max 65,535 bytes per spec). We scan backwards from
/// the end up to comment-max + EOCD-size for the 4-byte signature.
/// On match, `*eocd_offset` is set; the caller reads fields off it.
///
/// @param data        Pointer to the full ZIP byte buffer.
/// @param len         Length of `data`.
/// @param eocd_offset Out-parameter: offset of the EOCD signature.
/// @return True if EOCD found, false otherwise.
static bool find_eocd(const uint8_t *data, size_t len, size_t *eocd_offset) {
    if (len < ZIP_END_RECORD_SIZE)
        return false;

    // Search backwards for EOCD signature (handles comments)
    size_t max_comment = 65535;
    size_t search_len =
        len < (ZIP_END_RECORD_SIZE + max_comment) ? len : (ZIP_END_RECORD_SIZE + max_comment);

    for (size_t i = ZIP_END_RECORD_SIZE; i <= search_len; i++) {
        size_t offset = len - i;
        if (read_u32(data + offset) == ZIP_END_RECORD_SIG && offset <= len - ZIP_END_RECORD_SIZE) {
            uint16_t comment_len = read_u16(data + offset + 20);
            if (offset + ZIP_END_RECORD_SIZE + (size_t)comment_len != len)
                continue;
            *eocd_offset = offset;
            return true;
        }
    }
    return false;
}

/// @brief Parse the central directory and populate `ar->entries`.
///
/// Locates the EOCD (`find_eocd`), validates it isn't a multi-disk
/// archive (we don't support that), then walks each central-directory
/// header: copying out method, CRC, sizes, mod time, local-header
/// offset, and the entry name. Detects directories via trailing `/`.
/// Returns false (without trapping) if the central directory is
/// missing, malformed, multi-disk, or out of bounds — the caller's
/// trap message lets us distinguish "not a ZIP" from other errors.
///
/// @param ar Archive whose `data`/`data_len` is already populated.
/// @return True on successful parse, false on any structural problem.
bool parse_central_directory(rt_archive_t *ar) {
    if (!ar)
        return false;
    ar->parse_error = ARCHIVE_PARSE_ERROR_INVALID;
    size_t eocd_offset;
    if (!find_eocd(ar->data, ar->data_len, &eocd_offset))
        return false;

    const uint8_t *eocd = ar->data + eocd_offset;

    // Parse EOCD
    uint16_t disk_num = read_u16(eocd + 4);
    uint16_t cd_disk = read_u16(eocd + 6);
    uint16_t disk_entries = read_u16(eocd + 8);
    uint16_t total_entries = read_u16(eocd + 10);
    uint32_t cd_size = read_u32(eocd + 12);
    uint32_t cd_offset = read_u32(eocd + 16);

    // We don't support multi-disk archives
    if (disk_num != 0 || cd_disk != 0 || disk_entries != total_entries)
        return false;
    if (disk_entries == UINT16_MAX || total_entries == UINT16_MAX || cd_size == UINT32_MAX ||
        cd_offset == UINT32_MAX)
        return false;

    // Validate central directory bounds
    if ((size_t)cd_offset > eocd_offset || (size_t)cd_size > eocd_offset - (size_t)cd_offset)
        return false;

    zip_entry_t *entries = NULL;
    int32_t *name_slots = NULL;
    size_t name_slot_count = archive_name_index_capacity((size_t)total_entries);
    if (total_entries > 0 && name_slot_count == 0)
        return false;
    if (total_entries > 0) {
        entries = (zip_entry_t *)calloc(total_entries, sizeof(zip_entry_t));
        if (!entries) {
            ar->parse_error = ARCHIVE_PARSE_ERROR_OOM;
            return false;
        }
        name_slots = (int32_t *)calloc(name_slot_count, sizeof(*name_slots));
        if (!name_slots) {
            ar->parse_error = ARCHIVE_PARSE_ERROR_OOM;
            free(entries);
            return false;
        }
    }

    // Parse each central directory entry
    const uint8_t *p = ar->data + cd_offset;
    const uint8_t *cd_end = p + cd_size;

    uint64_t total_uncompressed = 0;
    int parsed = 0;
    for (; parsed < total_entries; parsed++) {
        if ((size_t)(cd_end - p) < ZIP_CENTRAL_HEADER_SIZE) {
            archive_free_entry_array(entries, parsed);
            free(name_slots);
            return false;
        }
        if (read_u32(p) != ZIP_CENTRAL_HEADER_SIG) {
            archive_free_entry_array(entries, parsed);
            free(name_slots);
            return false;
        }

        uint16_t name_len = read_u16(p + 28);
        uint16_t extra_len = read_u16(p + 30);
        uint16_t comment_len = read_u16(p + 32);
        size_t record_len =
            ZIP_CENTRAL_HEADER_SIZE + (size_t)name_len + (size_t)extra_len + (size_t)comment_len;

        if ((size_t)(cd_end - p) < record_len) {
            archive_free_entry_array(entries, parsed);
            free(name_slots);
            return false;
        }
        if (name_len == 0 || memchr(p + ZIP_CENTRAL_HEADER_SIZE, '\0', name_len) != NULL) {
            archive_free_entry_array(entries, parsed);
            free(name_slots);
            return false;
        }

        zip_entry_t *e = &entries[parsed];
        e->version_needed = read_u16(p + 6);
        e->flags = read_u16(p + 8);
        e->method = read_u16(p + 10);
        e->mod_time = read_u16(p + 12);
        e->mod_date = read_u16(p + 14);
        e->crc32 = read_u32(p + 16);
        e->compressed_size = read_u32(p + 20);
        e->uncompressed_size = read_u32(p + 24);
        e->local_offset = read_u32(p + 42);
        if ((e->flags & (ZIP_GP_FLAG_ENCRYPTED | ZIP_GP_FLAG_DATA_DESCRIPTOR |
                         ZIP_GP_FLAG_STRONG_ENCRYPTION)) != 0 ||
            e->version_needed >= 45 || e->compressed_size == UINT32_MAX ||
            e->uncompressed_size == UINT32_MAX || e->local_offset == UINT32_MAX ||
            archive_extra_is_malformed_or_zip64(p + ZIP_CENTRAL_HEADER_SIZE + name_len,
                                                extra_len)) {
            archive_free_entry_array(entries, parsed);
            free(name_slots);
            return false;
        }
        if ((uint64_t)e->uncompressed_size > ar->max_entry_bytes) {
            ar->parse_error = ARCHIVE_PARSE_ERROR_LIMIT;
            archive_free_entry_array(entries, parsed);
            free(name_slots);
            return false;
        }
        uint64_t entry_size = (uint64_t)e->uncompressed_size;
        if (entry_size > ar->max_total_entry_bytes ||
            total_uncompressed > ar->max_total_entry_bytes - entry_size) {
            ar->parse_error = ARCHIVE_PARSE_ERROR_LIMIT;
            archive_free_entry_array(entries, parsed);
            free(name_slots);
            return false;
        }
        total_uncompressed += entry_size;

        // Copy name
        e->name = (char *)malloc(name_len + 1);
        if (!e->name) {
            ar->parse_error = ARCHIVE_PARSE_ERROR_OOM;
            archive_free_entry_array(entries, parsed);
            free(name_slots);
            return false;
        }
        memcpy(e->name, p + ZIP_CENTRAL_HEADER_SIZE, name_len);
        e->name[name_len] = '\0';
        if (!archive_name_index_insert(entries, name_slots, name_slot_count, parsed)) {
            archive_free_entry_array(entries, parsed + 1);
            free(name_slots);
            return false;
        }

        // Check if directory
        e->is_directory = (name_len > 0 && e->name[name_len - 1] == '/');

        p += record_len;
    }

    if (parsed != total_entries || p != cd_end) {
        archive_free_entry_array(entries, parsed);
        free(name_slots);
        return false;
    }
    if (!archive_validate_local_entries(ar, entries, parsed, (size_t)cd_offset)) {
        archive_free_entry_array(entries, parsed);
        free(name_slots);
        return false;
    }

    ar->entries = entries;
    ar->entry_count = parsed;
    ar->central_offset = (size_t)cd_offset;
    ar->entry_name_slots = name_slots;
    ar->entry_name_slot_count = name_slot_count;
    return true;
}

/// @brief Hash-indexed lookup of a central-directory entry by exact name.
///
/// Names are compared via `strcmp` — the caller must normalize ahead
/// of time (path separators, leading `./`, etc.). Returns NULL when no
/// matching entry exists. The open-addressed side table is built during parse,
/// making expected lookup O(1); a defensive linear fallback handles archives
/// constructed by older internal paths without an index.
///
/// @param ar   Read-mode archive with parsed `entries`.
/// @param name Normalized entry name.
/// @return Pointer to the entry, or NULL if not found.
zip_entry_t *find_entry(rt_archive_t *ar, const char *name) {
    if (!ar || !name)
        return NULL;
    if (ar->entry_name_slots && ar->entry_name_slot_count > 0 &&
        (ar->entry_name_slot_count & (ar->entry_name_slot_count - 1)) == 0) {
        size_t slot = (size_t)archive_name_hash(name) & (ar->entry_name_slot_count - 1);
        for (size_t probe = 0; probe < ar->entry_name_slot_count; ++probe) {
            int32_t encoded = ar->entry_name_slots[slot];
            if (encoded == 0)
                return NULL;
            int index = encoded - 1;
            if (index >= 0 && index < ar->entry_count && ar->entries[index].name &&
                strcmp(ar->entries[index].name, name) == 0)
                return &ar->entries[index];
            slot = (slot + 1) & (ar->entry_name_slot_count - 1);
        }
        return NULL;
    }
    for (int i = 0; i < ar->entry_count; i++) {
        if (ar->entries[i].name && strcmp(ar->entries[i].name, name) == 0)
            return &ar->entries[i];
    }
    return NULL;
}

/// @brief Materialize a single entry's payload into a fresh `rt_bytes`.
///
/// Re-validates the local file header signature (defends against
/// archives whose central-directory offsets point at garbage), skips
/// the variable-length name + extra fields, then either:
///   - copies stored data verbatim (method 0), or
///   - inflates DEFLATE-compressed data via `rt_compress_inflate`
///     (method 8).
/// Both branches verify the CRC32 against the header. The DEFLATE
/// branch additionally verifies the inflated size matches the
/// uncompressed-size field. Traps on any mismatch.
///
/// @param ar Read-mode archive.
/// @param e  Entry from `ar->entries`.
/// @return Owned `rt_bytes` containing the uncompressed payload.
void *read_entry_data(rt_archive_t *ar, zip_entry_t *e) {
    if (!ar || !e) {
        rt_trap("Archive: invalid entry");
        return NULL;
    }
    if ((uint64_t)e->uncompressed_size > ar->max_entry_bytes) {
        rt_trap("Archive: configured entry resource limit exceeded");
        return NULL;
    }
    // Find local header
    size_t local_offset = (size_t)e->local_offset;
    if (local_offset > ar->central_offset ||
        ar->central_offset - local_offset < ZIP_LOCAL_HEADER_SIZE) {
        rt_trap("Archive: corrupt local header offset");
        return NULL;
    }

    const uint8_t *local = ar->data + local_offset;
    if (read_u32(local) != ZIP_LOCAL_HEADER_SIG) {
        rt_trap("Archive: invalid local header signature");
        return NULL;
    }

    uint16_t local_flags = read_u16(local + 6);
    uint16_t local_method = read_u16(local + 8);
    uint32_t local_crc = read_u32(local + 14);
    uint32_t local_compressed_size = read_u32(local + 18);
    uint32_t local_uncompressed_size = read_u32(local + 22);
    uint16_t name_len = read_u16(local + 26);
    uint16_t extra_len = read_u16(local + 28);
    size_t header_total = ZIP_LOCAL_HEADER_SIZE + (size_t)name_len + (size_t)extra_len;
    if (header_total > ar->central_offset - local_offset) {
        rt_trap("Archive: corrupt entry data");
        return NULL;
    }
    if (local_flags != e->flags || local_method != e->method ||
        (local_flags & ZIP_GP_FLAG_DATA_DESCRIPTOR) != 0 ||
        archive_extra_is_malformed_or_zip64(local + ZIP_LOCAL_HEADER_SIZE + name_len, extra_len)) {
        rt_trap("Archive: unsupported local header");
        return NULL;
    }
    size_t expected_name_len = strlen(e->name);
    if (expected_name_len != (size_t)name_len ||
        memcmp(local + ZIP_LOCAL_HEADER_SIZE, e->name, expected_name_len) != 0) {
        rt_trap("Archive: local header name mismatch");
        return NULL;
    }
    if (local_crc != e->crc32 || local_compressed_size != e->compressed_size ||
        local_uncompressed_size != e->uncompressed_size) {
        rt_trap("Archive: local header metadata mismatch");
        return NULL;
    }
    size_t data_offset = local_offset + header_total;
    if ((size_t)e->compressed_size > ar->central_offset - data_offset) {
        rt_trap("Archive: corrupt entry data");
        return NULL;
    }

    const uint8_t *compressed = ar->data + data_offset;

    // Handle uncompressed (stored) data
    if (e->method == ZIP_METHOD_STORED) {
        if (e->compressed_size != e->uncompressed_size) {
            rt_trap("Archive: stored entry size mismatch");
            return NULL;
        }
        // Verify CRC
        uint32_t crc = rt_crc32_compute(compressed, e->uncompressed_size);
        if (crc != e->crc32) {
            rt_trap("Archive: CRC mismatch");
            return NULL;
        }

        void *result = rt_bytes_new(e->uncompressed_size);
        if (!result)
            return NULL;
        if (e->uncompressed_size > 0)
            memcpy(bytes_data(result), compressed, e->uncompressed_size);
        return result;
    }

    // Handle deflated data
    if (e->method == ZIP_METHOD_DEFLATE) {
        uint8_t *inflated = NULL;
        size_t inflated_len = 0;
        if (!rt_compress_inflate_raw(
                compressed, e->compressed_size, e->uncompressed_size, &inflated, &inflated_len)) {
            rt_trap("Archive: failed to inflate entry");
            return NULL;
        }

        // Verify CRC
        uint32_t crc = rt_crc32_compute(inflated, inflated_len);
        if (crc != e->crc32) {
            free(inflated);
            rt_trap("Archive: CRC mismatch");
            return NULL;
        }

        // Verify size
        if (inflated_len != e->uncompressed_size) {
            free(inflated);
            rt_trap("Archive: size mismatch");
            return NULL;
        }

        void *result = NULL;
        uint8_t *volatile inflated_owner = inflated;
        jmp_buf recovery;
        rt_trap_set_recovery(&recovery);
        if (setjmp(recovery) != 0) {
            char saved_error[256];
            archive_save_trap_error(
                saved_error, sizeof(saved_error), "Archive: memory allocation failed");
            rt_trap_clear_recovery();
            free((void *)inflated_owner);
            rt_trap(saved_error);
            return NULL;
        }
        result = rt_bytes_new(e->uncompressed_size);
        rt_trap_clear_recovery();
        if (!result) {
            free(inflated);
            return NULL;
        }
        if (e->uncompressed_size > 0)
            memcpy(bytes_data(result), inflated, e->uncompressed_size);
        free(inflated);
        return result;
    }

    rt_trap("Archive: unsupported compression method");
    return NULL;
}

//=============================================================================
