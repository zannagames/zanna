//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/audio/rt_ogg.h
// Purpose: OGG container format parser — extracts Vorbis packets from .ogg files.
// Key invariants:
//   - OGG pages are validated by CRC-32 before packet extraction
//   - Packets can span multiple pages (continued flag)
//   - Packet traversal is forward-only; readers support whole-stream rewind
// Ownership/Lifetime:
//   - Caller owns the ogg_reader and must call ogg_reader_free
// Links: src/runtime/audio/rt_vorbis.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares a sequential, CRC-validating Ogg container packet reader.
/// @details Readers may own a file backend or borrow an in-memory container.
///          Logical bitstreams are tracked independently by serial number so
///          packets spanning pages can be reconstructed even in multiplexed
///          input. Returned packet bytes remain reader-owned and have a
///          deliberately short lifetime.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Parsed Ogg page header plus its lacing-value table.
/// @details The fixed header is 27 bytes; up to 255 following lacing values
///          describe packet fragments in the page body.
typedef struct {
    uint8_t version;
    uint8_t header_type; // 0x01=continued, 0x02=BOS, 0x04=EOS
    int64_t granule_position;
    uint32_t serial_number;
    uint32_t page_sequence;
    uint32_t checksum;
    uint8_t num_segments;
    uint8_t segment_table[255];
} ogg_page_header_t;

/// @brief Growable partial logical-packet assembly buffer.
/// @details Owned internally by one logical stream state and reused after a
///          completed packet is copied to the reader's ready queue.
typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
    int complete; // 1 if packet is complete, 0 if continued on next page
} ogg_packet_t;

/// @brief Metadata attached to a completed logical OGG packet.
/// @details Granule position is known only for the final completed packet on a
///          page; earlier packet completions use `-1`.
typedef struct {
    uint32_t serial_number;
    int64_t granule_position; // -1 when not known for this packet
    uint8_t bos;
    uint8_t eos;
} ogg_packet_info_t;

typedef struct ogg_stream_state_t ogg_stream_state_t;
typedef struct ogg_packet_node_t ogg_packet_node_t;

/// @brief State for a file-backed or borrowed-memory Ogg reader.
/// @details The object owns file handles, logical-stream assembly nodes, queued
///          packets, and the most recently returned packet. It never owns
///          @ref mem.
typedef struct {
    FILE *file; // NULL for memory-based reading
    const uint8_t *mem;
    size_t mem_len;
    size_t mem_pos;

    // Current page state
    ogg_page_header_t page;
    ogg_stream_state_t *streams;
    ogg_packet_node_t *ready_head;
    ogg_packet_node_t *ready_tail;
    uint8_t *last_packet_data;
} ogg_reader_t;

/// @brief Create an OGG reader from a file path.
/// @param path NUL-terminated Ogg file path.
/// @return Caller-owned reader, or NULL on invalid input/open/allocation failure.
ogg_reader_t *ogg_reader_open_file(const char *path);

/// @brief Create an OGG reader from memory.
/// @details The caller must keep @p data readable for the reader's lifetime.
/// @param data Borrowed Ogg container bytes.
/// @param len Positive buffer length in bytes.
/// @return Caller-owned reader, or NULL for invalid input/allocation failure.
ogg_reader_t *ogg_reader_open_mem(const uint8_t *data, size_t len);

/// @brief Free an OGG reader.
/// @details Closes an owned file and releases all parser/packet state, but never
///          frees memory supplied to @ref ogg_reader_open_mem.
/// @param r Reader returned by an open function; NULL is accepted.
void ogg_reader_free(ogg_reader_t *r);

/// @brief Read the next complete packet.
/// @param r Open reader.
/// @param out_data Receives reader-owned bytes valid until the next packet call,
///        rewind, or free. Set to NULL on failure when non-NULL.
/// @param out_len Receives packet length in bytes. Set to zero on failure.
/// @return `1` if a packet was read, or `0` on EOF/error.
int ogg_reader_next_packet(ogg_reader_t *r, const uint8_t **out_data, size_t *out_len);

/// @brief Read the next complete packet plus stream metadata.
/// @param r Open reader.
/// @param out_data Receives reader-owned bytes valid until the next packet call,
///        rewind, or free. Set to NULL on failure when non-NULL.
/// @param out_len Receives packet length in bytes. Set to zero on failure.
/// @param out_info Optional destination for serial/granule/BOS/EOS metadata;
///        zero-initialized on failure.
/// @return `1` if a packet was read, or `0` on EOF/error.
int ogg_reader_next_packet_ex(ogg_reader_t *r,
                              const uint8_t **out_data,
                              size_t *out_len,
                              ogg_packet_info_t *out_info);

/// @brief Reset reader to the beginning of the file (for looping).
/// @details Seeks a file or resets the memory cursor to zero and clears all
///          page, partial-stream, queued-packet, and last-packet state.
/// @param r Reader to rewind; NULL is ignored.
void ogg_reader_rewind(ogg_reader_t *r);

#ifdef __cplusplus
}
#endif
