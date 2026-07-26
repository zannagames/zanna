//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/media/rt_avi.c
// Purpose: AVI RIFF container parser. Walks the RIFF chunk tree to extract
//   video stream info (codec, dimensions, frame rate), audio stream info
//   (sample rate, channels, bit depth), and builds an index of interleaved
//   movi chunks for frame-by-frame access.
//
// Key invariants:
//   - RIFF chunks are 2-byte aligned (pad byte after odd-sized chunks).
//   - Stream numbers are 2-digit ASCII in chunk FOURCCs (e.g., '00dc').
//   - Parser validates all offsets to prevent out-of-bounds reads.
//   - No memory allocated for the file data itself (caller owns it).
//
// Links: src/runtime/graphics/media/rt_avi.h (public parser API),
//        src/runtime/graphics/media/rt_videoplayer.c (playback consumer)
//
//===----------------------------------------------------------------------===//
/**
 * @file
 * @brief Implements bounded parsing and frame indexing for AVI RIFF data.
 * @details Decodes little-endian container metadata, selects primary video
 * and audio streams, recursively walks validated chunk extents, and builds
 * owned playback indexes whose payload pointers continue to alias the
 * caller-owned input buffer.
 */

#include "rt_avi.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*==========================================================================
 * Little-endian helpers
 *=========================================================================*/

/// @brief Read a 32-bit little-endian unsigned integer from a byte pointer.
/// @param p Pointer to at least four readable bytes.
/// @return Decoded host-order unsigned value.
static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/// @brief Read a 16-bit little-endian unsigned integer from a byte pointer.
/// @param p Pointer to at least two readable bytes.
/// @return Decoded host-order unsigned value.
static uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/// @brief Build a 32-bit FOURCC tag from four ASCII characters (packed little-endian).
/// @param a Least-significant tag character.
/// @param b Second tag character.
/// @param c Third tag character.
/// @param d Most-significant tag character.
/// @return Packed little-endian FOURCC value.
static uint32_t make_fourcc(char a, char b, char c, char d) {
    return (uint32_t)(uint8_t)a | ((uint32_t)(uint8_t)b << 8) | ((uint32_t)(uint8_t)c << 16) |
           ((uint32_t)(uint8_t)d << 24);
}

/** @name AVI RIFF and stream FOURCC identifiers
 * @{ */
#define FOURCC_RIFF make_fourcc('R', 'I', 'F', 'F')
#define FOURCC_AVI make_fourcc('A', 'V', 'I', ' ')
#define FOURCC_LIST make_fourcc('L', 'I', 'S', 'T')
#define FOURCC_hdrl make_fourcc('h', 'd', 'r', 'l')
#define FOURCC_strl make_fourcc('s', 't', 'r', 'l')
#define FOURCC_movi make_fourcc('m', 'o', 'v', 'i')
#define FOURCC_avih make_fourcc('a', 'v', 'i', 'h')
#define FOURCC_strh make_fourcc('s', 't', 'r', 'h')
#define FOURCC_strf make_fourcc('s', 't', 'r', 'f')
#define FOURCC_idx1 make_fourcc('i', 'd', 'x', '1')
#define FOURCC_vids make_fourcc('v', 'i', 'd', 's')
#define FOURCC_auds make_fourcc('a', 'u', 'd', 's')
#define FOURCC_rec make_fourcc('r', 'e', 'c', ' ')
/** @} */

/** Maximum recursive RIFF/LIST nesting accepted from an input file. */
#define AVI_MAX_CHUNK_DEPTH 32
/** Lowest nonzero frame rate accepted as useful playback metadata. */
#define AVI_MIN_VALID_FPS 0.001
/** Highest frame rate accepted as useful playback metadata. */
#define AVI_MAX_VALID_FPS 1000.0
/** Maximum supported width or absolute height in pixels. */
#define AVI_MAX_DIMENSION 32768
/** Maximum number of legacy `idx1` records considered while indexing. */
#define AVI_MAX_IDX1_ENTRIES (1024u * 1024u)

/** Parser-local classification for the stream list currently being visited. */
typedef struct {
    int stream_type;  ///< -1=unknown, 0=video, 1=audio.
    int stream_index; ///< Zero-based stream list index from hdrl.
} avi_stream_parse_state_t;

/// @brief Extract the two-digit stream number encoded at the start of an AVI chunk FOURCC.
/// @details AVI media chunks conventionally use tags such as `00dc`,
///          `01wb`, or `02db`. Chunks without two leading ASCII digits
///          return -1 and are ignored by stream-index filtering.
/// @param fourcc Packed little-endian chunk tag.
/// @return Stream index 0-99, or -1 when the tag is not stream-numbered.
static int avi_stream_index_from_fourcc(uint32_t fourcc) {
    uint8_t c0 = (uint8_t)(fourcc & 0xFFu);
    uint8_t c1 = (uint8_t)((fourcc >> 8) & 0xFFu);
    if (c0 < '0' || c0 > '9' || c1 < '0' || c1 > '9')
        return -1;
    return (int)(c0 - '0') * 10 + (int)(c1 - '0');
}

/// @brief Classify a stream-numbered `movi`/`idx1` FOURCC as primary video or audio.
/// @details AVI media tags encode stream number in the first two bytes and payload kind in the
///          last two bytes: `dc`/`db` for video frames, `wb` for wave audio. Filtering here keeps
///          recursive `movi` walking and legacy `idx1` parsing on the same stream-selection rules.
/// @param ctx Parsed stream-selection context.
/// @param fourcc Packed media-chunk tag to classify.
/// @param out_video Receives 1 for primary video or 0 for primary audio on success.
/// @return 1 when the chunk belongs to the primary video/audio stream, 0 when it should be ignored.
static int avi_classify_media_fourcc(const avi_context_t *ctx, uint32_t fourcc, int8_t *out_video) {
    uint8_t c2 = (uint8_t)((fourcc >> 16) & 0xFF);
    uint8_t c3 = (uint8_t)((fourcc >> 24) & 0xFF);
    int stream_index = avi_stream_index_from_fourcc(fourcc);
    if (!ctx || !out_video || stream_index < 0)
        return 0;
    if (c2 == 'd' && (c3 == 'c' || c3 == 'b') && stream_index == ctx->video_stream_index) {
        *out_video = 1;
        return 1;
    }
    if (c2 == 'w' && c3 == 'b' && stream_index == ctx->audio_stream_index) {
        *out_video = 0;
        return 1;
    }
    return 0;
}

/// @brief Return a finite AVI frame rate only when it is within the supported playback range.
/// @details AVI headers are trusted only enough to preserve common files. Malformed files can
///          advertise extreme rate/scale pairs that overflow timing math or make duration useless,
///          so callers ignore values outside the conservative Zanna video-player envelope.
/// @param fps Candidate frames-per-second value computed from AVI metadata.
/// @return @p fps when it is sane for playback; otherwise 0.0 to mean "ignore this value".
static double avi_sane_fps(double fps) {
    if (fps < AVI_MIN_VALID_FPS || fps > AVI_MAX_VALID_FPS)
        return 0.0;
    return fps;
}

/// @brief Clear audio metadata when a stream header or format chunk is internally inconsistent.
/// @param ctx AVI parse context whose audio fields should be reset.
static void avi_clear_audio(avi_context_t *ctx) {
    if (!ctx)
        return;
    ctx->has_audio = 0;
    ctx->audio_stream_index = -1;
    memset(&ctx->audio, 0, sizeof(ctx->audio));
}

/*==========================================================================
 * Chunk addition
 *=========================================================================*/

/// @brief Append a movi chunk record to the context's chunk list, growing the array as needed.
/// @details Geometric growth: starts at capacity 64, doubles thereafter.
///          Used to record the location of every video and audio chunk in
///          the movi list during parsing — the post-parse pass then walks
///          this array to build the video-frame index.
/// @param ctx AVI context whose owned chunk array is extended.
/// @param data Borrowed pointer to the chunk payload in the caller-owned file buffer.
/// @param size Payload size in bytes.
/// @param is_video 1 for video data or 0 for audio data.
/// @return 0 on success; -1 on allocation failure (chunk is dropped).
static int add_chunk(avi_context_t *ctx, const uint8_t *data, uint32_t size, int8_t is_video) {
    if (ctx->chunk_count >= ctx->chunk_capacity) {
        if (ctx->chunk_capacity > INT32_MAX / 2)
            return -1;
        int32_t new_cap = ctx->chunk_capacity < 64 ? 64 : ctx->chunk_capacity * 2;
        if ((uint64_t)new_cap > (uint64_t)SIZE_MAX / sizeof(avi_chunk_t))
            return -1;
        avi_chunk_t *new_chunks =
            (avi_chunk_t *)realloc(ctx->chunks, (size_t)new_cap * sizeof(avi_chunk_t));
        if (!new_chunks)
            return -1;
        ctx->chunks = new_chunks;
        ctx->chunk_capacity = new_cap;
    }
    ctx->chunks[ctx->chunk_count].data = data;
    ctx->chunks[ctx->chunk_count].size = size;
    ctx->chunks[ctx->chunk_count].is_video = is_video;
    ctx->chunk_count++;
    return 0;
}

/*==========================================================================
 * Header parsing
 *=========================================================================*/

/// @brief Parse supported fields from an AVI main-header chunk.
/// @details Ignores payloads shorter than 40 bytes. Updates dimensions, declared frame count,
///          frame rate, and duration only when the encoded values fit supported representations.
/// @param ctx AVI context receiving main video metadata.
/// @param data Bounded `avih` payload.
/// @param size Payload size in bytes.
static void parse_avih(avi_context_t *ctx, const uint8_t *data, uint32_t size) {
    if (size < 40)
        return;
    uint32_t us_per_frame = read_le32(data + 0);
    /* bytes 4-12: dwMaxBytesPerSec, dwPaddingGranularity, dwFlags */
    uint32_t total_frames = read_le32(data + 16);
    /* bytes 20-28: dwInitialFrames, dwStreams */
    /* bytes 32: dwSuggestedBufferSize */
    uint32_t width = read_le32(data + 32);
    uint32_t height = read_le32(data + 36);

    if (width <= INT32_MAX)
        ctx->video.width = (int32_t)width;
    if (height <= INT32_MAX)
        ctx->video.height = (int32_t)height;
    if (total_frames <= INT32_MAX)
        ctx->video.frame_count = (int32_t)total_frames;
    if (us_per_frame > 0) {
        double fps = avi_sane_fps(1000000.0 / (double)us_per_frame);
        if (fps > 0.0)
            ctx->video.fps = fps;
    } else {
        ctx->video.fps = 30.0; /* default */
    }
    if (ctx->video.fps > 0.0 && ctx->video.frame_count > 0)
        ctx->video.duration = (double)ctx->video.frame_count / ctx->video.fps;
}

/// @brief Parse one AVI stream-header chunk.
/// @details Classifies the stream as video, audio, or unknown and selects only the first stream of
///          each supported media type as primary. Video rate/scale and length may override main
///          header metadata when valid.
/// @param ctx AVI context receiving stream metadata.
/// @param data Bounded `strh` payload.
/// @param size Payload size in bytes.
/// @param stream Mutable state for the enclosing `strl` list.
static void parse_strh(avi_context_t *ctx,
                       const uint8_t *data,
                       uint32_t size,
                       avi_stream_parse_state_t *stream) {
    if (size < 48)
        return;
    uint32_t fcc_type = read_le32(data + 0);
    uint32_t fcc_handler = read_le32(data + 4);
    uint32_t scale = read_le32(data + 20);
    uint32_t rate = read_le32(data + 24);
    uint32_t length = read_le32(data + 32);

    if (fcc_type == FOURCC_vids) {
        stream->stream_type = 0; /* video */
        if (ctx->video_stream_index < 0) {
            ctx->video_stream_index = stream->stream_index;
            ctx->video.fourcc = fcc_handler;
            if (scale > 0 && rate > 0) {
                double fps = avi_sane_fps((double)rate / (double)scale);
                if (fps > 0.0)
                    ctx->video.fps = fps;
            }
            if (length > 0 && length <= (uint32_t)INT32_MAX)
                ctx->video.frame_count = (int32_t)length;
            if (ctx->video.fps > 0.0 && ctx->video.frame_count > 0)
                ctx->video.duration = (double)ctx->video.frame_count / ctx->video.fps;
        }
    } else if (fcc_type == FOURCC_auds) {
        stream->stream_type = 1; /* audio */
        if (ctx->audio_stream_index < 0) {
            ctx->audio_stream_index = stream->stream_index;
            ctx->has_audio = 1;
        }
    } else {
        stream->stream_type = -1;
    }
}

/// @brief Parse a video stream's BITMAPINFOHEADER format chunk.
/// @details Requires at least 40 bytes, one plane, a supported bit depth, and positive dimensions
///          no greater than @ref AVI_MAX_DIMENSION. Negative heights are accepted as top-down
///          bitmaps and stored by absolute magnitude.
/// @param ctx AVI context receiving video format metadata.
/// @param data Bounded video `strf` payload.
/// @param size Payload size in bytes.
static void parse_strf_video(avi_context_t *ctx, const uint8_t *data, uint32_t size) {
    if (size < 40)
        return;
    /* BITMAPINFOHEADER */
    int32_t raw_width = (int32_t)read_le32(data + 4);
    int32_t raw_height = (int32_t)read_le32(data + 8);
    uint16_t planes = read_le16(data + 12);
    uint16_t bit_count = read_le16(data + 14);
    uint32_t bi_compression = read_le32(data + 16);
    if (planes != 1u)
        return;
    if (bit_count != 0u && bit_count != 8u && bit_count != 16u && bit_count != 24u &&
        bit_count != 32u)
        return;
    if (raw_width <= 0 || raw_width > AVI_MAX_DIMENSION)
        return;
    if (raw_width > 0)
        ctx->video.width = (int32_t)raw_width;
    if (raw_height == INT32_MIN)
        return;
    if (raw_height < 0)
        raw_height = -raw_height;
    if (raw_height <= 0 || raw_height > AVI_MAX_DIMENSION)
        return;
    if (raw_height > 0)
        ctx->video.height = raw_height;
    if (bi_compression != 0)
        ctx->video.fourcc = bi_compression;
}

/// @brief Parse and validate a PCM or IEEE-float WAVEFORMATEX chunk.
/// @details Accepts PCM format 1 and float format 3 with bounded channels, sample rate, bit depth,
///          block alignment, and average-byte-rate consistency. An internally inconsistent format
///          clears the previously selected audio stream.
/// @param ctx AVI context receiving audio format metadata.
/// @param data Bounded audio `strf` payload.
/// @param size Payload size in bytes.
static void parse_strf_audio(avi_context_t *ctx, const uint8_t *data, uint32_t size) {
    if (size < 16)
        return;
    /* WAVEFORMATEX */
    uint16_t format_tag = read_le16(data);
    int32_t channels = (int32_t)read_le16(data + 2);
    uint32_t sample_rate_u = read_le32(data + 4);
    int32_t block_align = (int32_t)read_le16(data + 12);
    int32_t bits_per_sample = (int32_t)read_le16(data + 14);
    if ((format_tag != 1u && format_tag != 3u) || channels <= 0 || channels > 8 ||
        sample_rate_u == 0 || sample_rate_u > 384000u || sample_rate_u > (uint32_t)INT32_MAX ||
        block_align <= 0 ||
        (bits_per_sample != 8 && bits_per_sample != 16 && bits_per_sample != 24 &&
         bits_per_sample != 32 && bits_per_sample != 64) ||
        (format_tag == 3u && bits_per_sample != 32 && bits_per_sample != 64)) {
        avi_clear_audio(ctx);
        return;
    }
    if (bits_per_sample % 8 != 0) {
        avi_clear_audio(ctx);
        return;
    }
    {
        int64_t expected_align = (int64_t)channels * (int64_t)(bits_per_sample / 8);
        if (expected_align <= 0 || expected_align > INT32_MAX ||
            block_align != (int32_t)expected_align) {
            avi_clear_audio(ctx);
            return;
        }
    }
    {
        uint32_t avg_bytes_per_sec = read_le32(data + 8);
        uint64_t expected_bps = (uint64_t)sample_rate_u * (uint64_t)block_align;
        if (expected_bps > UINT32_MAX ||
            (avg_bytes_per_sec != 0u && avg_bytes_per_sec != (uint32_t)expected_bps)) {
            avi_clear_audio(ctx);
            return;
        }
    }
    if ((format_tag == 1u && bits_per_sample == 64)) {
        avi_clear_audio(ctx);
        return;
    }
    ctx->audio.channels = channels;
    ctx->audio.sample_rate = (int32_t)sample_rate_u;
    ctx->audio.block_align = block_align;
    ctx->audio.bits_per_sample = bits_per_sample;
    ctx->has_audio = 1;
}

/*==========================================================================
 * RIFF chunk walking
 *=========================================================================*/

/// @brief Walk one bounded level of RIFF chunks and dispatch each payload.
/// @details Validates every eight-byte chunk header, payload extent, and required odd-size pad
///          byte before advancing. Nesting beyond @ref AVI_MAX_CHUNK_DEPTH and malformed bounds set
///          `ctx->parse_error`. The handler may recursively walk LIST payloads.
/// @param data Base address of the bounded RIFF or LIST buffer.
/// @param len Accessible length of @p data in bytes.
/// @param start Offset of the first chunk header within @p data.
/// @param depth Current zero-based recursion depth.
/// @param handler Callback receiving the context, FOURCC, bounded payload, payload size, and
///                caller-supplied extra state.
/// @param ctx AVI parse context updated by handlers; may be NULL for a context-free walk.
/// @param extra Opaque state forwarded unchanged to @p handler.
/// @return 0 after a complete or handler-aborted walk, or -1 for malformed bounds or excessive
///         nesting.
static int walk_chunks(const uint8_t *data,
                       size_t len,
                       size_t start,
                       int depth,
                       void (*handler)(avi_context_t *,
                                       uint32_t fourcc,
                                       const uint8_t *payload,
                                       uint32_t size,
                                       void *extra),
                       avi_context_t *ctx,
                       void *extra) {
    if (depth > AVI_MAX_CHUNK_DEPTH) {
        if (ctx)
            ctx->parse_error = 1;
        return -1;
    }
    size_t pos = start;
    while (pos <= len && len - pos >= 8) {
        uint32_t fourcc = read_le32(data + pos);
        uint32_t size = read_le32(data + pos + 4);
        if (size > len - pos - 8) {
            if (ctx)
                ctx->parse_error = 1;
            return -1;
        }
        if (ctx)
            ctx->chunk_walk_depth = depth;
        handler(ctx, fourcc, data + pos + 8, size, extra);
        if (ctx)
            ctx->chunk_walk_depth = depth; /* a nested walk_chunks inside the handler leaves the
                                            * shared field deeper; restore this level's depth so
                                            * later siblings (and any post-return reads) observe
                                            * the correct value. The authoritative recursion bound
                                            * remains the local `depth` parameter checked above. */
        if (ctx && ctx->parse_error)
            break;
        pos += 8 + size;
        if (size & 1) {
            if (pos >= len) {
                if (ctx)
                    ctx->parse_error = 1;
                return -1;
            }
            pos++; /* RIFF 2-byte alignment */
        }
    }
    return 0;
}

/*==========================================================================
 * Parse handlers
 *=========================================================================*/

/// @brief Dispatch stream header and format chunks inside one `strl` LIST.
/// @param ctx AVI context receiving stream metadata.
/// @param fourcc Child chunk tag.
/// @param payload Bounded child payload.
/// @param size Child payload size in bytes.
/// @param extra Pointer to the enclosing stream's parse state.
static void handle_strl(
    avi_context_t *ctx, uint32_t fourcc, const uint8_t *payload, uint32_t size, void *extra) {
    avi_stream_parse_state_t *stream = (avi_stream_parse_state_t *)extra;
    if (fourcc == FOURCC_strh)
        parse_strh(ctx, payload, size, stream);
    else if (fourcc == FOURCC_strf) {
        if (stream->stream_type == 0 && stream->stream_index == ctx->video_stream_index)
            parse_strf_video(ctx, payload, size);
        else if (stream->stream_type == 1 && stream->stream_index == ctx->audio_stream_index)
            parse_strf_audio(ctx, payload, size);
    }
}

/// @brief Dispatch main-header and nested stream-list chunks inside `hdrl`.
/// @details Assigns monotonically increasing stream-list indices and recursively walks each
///          supported `strl` list.
/// @param ctx AVI context receiving header metadata.
/// @param fourcc Child chunk tag.
/// @param payload Bounded child payload.
/// @param size Child payload size in bytes.
/// @param extra Unused callback state.
static void handle_hdrl(
    avi_context_t *ctx, uint32_t fourcc, const uint8_t *payload, uint32_t size, void *extra) {
    (void)extra;
    if (fourcc == FOURCC_avih) {
        parse_avih(ctx, payload, size);
    } else if (fourcc == FOURCC_LIST && size >= 4) {
        uint32_t list_type = read_le32(payload);
        if (list_type == FOURCC_strl) {
            avi_stream_parse_state_t stream = {-1, ctx->stream_count};
            if (ctx->stream_count < INT32_MAX)
                ctx->stream_count++;
            walk_chunks(
                payload, size, 4, ctx ? ctx->chunk_walk_depth + 1 : 0, handle_strl, ctx, &stream);
        }
    }
}

/// @brief Collect primary media chunks from a `movi` or nested `rec ` LIST.
/// @details Recurses into record lists, filters media tags by selected stream index, and sets
///          `parse_error` if the owned chunk-index array cannot grow.
/// @param ctx AVI context receiving borrowed media-chunk descriptors.
/// @param fourcc Child chunk tag.
/// @param payload Bounded child payload.
/// @param size Child payload size in bytes.
/// @param extra Unused callback state.
static void handle_movi(
    avi_context_t *ctx, uint32_t fourcc, const uint8_t *payload, uint32_t size, void *extra) {
    (void)extra;
    int8_t is_video = 0;
    if (fourcc == FOURCC_LIST && size >= 4) {
        uint32_t list_type = read_le32(payload);
        if (list_type == FOURCC_rec)
            walk_chunks(
                payload, size, 4, ctx ? ctx->chunk_walk_depth + 1 : 0, handle_movi, ctx, NULL);
        return;
    }
    if (avi_classify_media_fourcc(ctx, fourcc, &is_video) &&
        add_chunk(ctx, payload, size, is_video) != 0)
        ctx->parse_error = 1;
    /* Skip 'ix##', 'JUNK' and other chunks */
}

/// @brief Dispatch supported top-level AVI RIFF chunks.
/// @details Recursively parses `hdrl` and `movi` LISTs, records the bounded movi extent for legacy
///          index resolution, and borrows the payload of a top-level `idx1` chunk.
/// @param ctx AVI parse context.
/// @param fourcc Top-level chunk tag.
/// @param payload Bounded top-level payload.
/// @param size Top-level payload size in bytes.
/// @param extra Unused callback state.
static void handle_top(
    avi_context_t *ctx, uint32_t fourcc, const uint8_t *payload, uint32_t size, void *extra) {
    (void)extra;
    if (fourcc == FOURCC_LIST && size >= 4) {
        uint32_t list_type = read_le32(payload);
        if (list_type == FOURCC_hdrl)
            walk_chunks(
                payload, size, 4, ctx ? ctx->chunk_walk_depth + 1 : 0, handle_hdrl, ctx, NULL);
        else if (list_type == FOURCC_movi) {
            if (ctx && ctx->file_data && payload >= ctx->file_data &&
                (size_t)(payload - ctx->file_data) <= ctx->file_len) {
                ctx->movi_list_offset = (size_t)(payload - ctx->file_data);
                ctx->movi_payload_start = ctx->movi_list_offset + 4u;
                ctx->movi_payload_end = ctx->movi_list_offset + (size_t)size;
            }
            walk_chunks(
                payload, size, 4, ctx ? ctx->chunk_walk_depth + 1 : 0, handle_movi, ctx, NULL);
        }
    } else if (fourcc == FOURCC_idx1 && ctx) {
        ctx->idx1_data = payload;
        ctx->idx1_size = size;
    }
}

/// @brief Resolve a legacy idx1 offset to a media chunk payload pointer.
/// @details AVI writers disagree on whether `dwChunkOffset` is file-relative, relative to the
///          `movi` list type tag, relative to the first chunk after that tag, and whether it points
///          at the chunk header or payload. This helper tries those common interpretations and
///          accepts only candidates whose chunk FOURCC and bounded length match the idx1 entry.
/// @param ctx AVI context retaining the borrowed source-file extent and movi offsets.
/// @param ckid Expected media chunk FOURCC.
/// @param offset Legacy index offset under one of the supported base conventions.
/// @param length Indexed payload length in bytes.
/// @return Borrowed payload pointer within `ctx->file_data`, or NULL when no interpretation
///         validates.
static const uint8_t *avi_resolve_idx1_payload(const avi_context_t *ctx,
                                               uint32_t ckid,
                                               uint32_t offset,
                                               uint32_t length) {
    size_t bases[3];
    if (!ctx || !ctx->file_data || length == 0)
        return NULL;
    bases[0] = 0;
    bases[1] = ctx->movi_list_offset;
    bases[2] = ctx->movi_payload_start;
    for (int i = 0; i < 3; i++) {
        size_t base = bases[i];
        size_t pos;
        if (base > ctx->file_len || (size_t)offset > ctx->file_len - base)
            continue;
        pos = base + (size_t)offset;
        if (pos <= ctx->file_len && ctx->file_len - pos >= 8u &&
            read_le32(ctx->file_data + pos) == ckid) {
            uint32_t chunk_size = read_le32(ctx->file_data + pos + 4);
            if (chunk_size >= length && (size_t)length <= ctx->file_len - pos - 8u)
                return ctx->file_data + pos + 8u;
        }
        if (pos >= 8u && read_le32(ctx->file_data + pos - 8u) == ckid) {
            uint32_t chunk_size = read_le32(ctx->file_data + pos - 4u);
            if (chunk_size >= length && (size_t)length <= ctx->file_len - pos)
                return ctx->file_data + pos;
        }
    }
    return NULL;
}

/// @brief Prefer a valid legacy `idx1` chunk index over sequential `movi` discovery.
/// @details Sequential walking is retained as a fallback, but `idx1` can recover correct playback
///          order from files whose `movi` data is nested or padded unusually. The replacement is
///          accepted only when it yields at least one primary video frame. Oversized indexes are
///          ignored so hostile files cannot force an unbounded chunk-index allocation.
/// @param ctx AVI context containing a borrowed `idx1` payload and sequential chunk fallback.
/// @return 1 when the owned chunk array was replaced by a valid index-derived array, otherwise 0.
static int avi_try_build_chunks_from_idx1(avi_context_t *ctx) {
    int32_t entry_count;
    uint32_t raw_entry_count;
    int32_t chunk_count = 0;
    int32_t video_count = 0;
    avi_chunk_t *chunks;
    if (!ctx || !ctx->idx1_data || ctx->idx1_size < 16u || (ctx->idx1_size % 16u) != 0)
        return 0;
    raw_entry_count = ctx->idx1_size / 16u;
    if (raw_entry_count == 0u || raw_entry_count > AVI_MAX_IDX1_ENTRIES ||
        raw_entry_count > (uint32_t)INT32_MAX ||
        (size_t)raw_entry_count > SIZE_MAX / sizeof(*chunks))
        return 0;
    entry_count = (int32_t)raw_entry_count;
    chunks = (avi_chunk_t *)malloc((size_t)entry_count * sizeof(*chunks));
    if (!chunks)
        return 0;
    for (int32_t i = 0; i < entry_count; i++) {
        const uint8_t *entry = ctx->idx1_data + (size_t)i * 16u;
        uint32_t ckid = read_le32(entry + 0);
        uint32_t offset = read_le32(entry + 8);
        uint32_t length = read_le32(entry + 12);
        int8_t is_video = 0;
        const uint8_t *payload;
        if (!avi_classify_media_fourcc(ctx, ckid, &is_video))
            continue;
        payload = avi_resolve_idx1_payload(ctx, ckid, offset, length);
        if (!payload)
            continue;
        chunks[chunk_count].data = payload;
        chunks[chunk_count].size = length;
        chunks[chunk_count].is_video = is_video;
        if (is_video)
            video_count++;
        chunk_count++;
    }
    if (video_count <= 0) {
        free(chunks);
        return 0;
    }
    free(ctx->chunks);
    ctx->chunks = chunks;
    ctx->chunk_count = chunk_count;
    ctx->chunk_capacity = chunk_count;
    return 1;
}

/*==========================================================================
 * Public API
 *=========================================================================*/

/// @brief Parse an AVI file's RIFF chunk tree and build a frame index.
/// @details Validates the `RIFF....AVI ` header, walks the top-level chunks
///          (driving the parse via `handle_top` → `handle_hdrl` /
///          `handle_movi`), then post-processes the recorded chunks into a
///          dense index of video-frame chunks for `avi_get_video_frame`
///          random access. The header-reported frame count is overridden
///          by the actual movi-chunk count when the header lies (common in
///          files produced by sloppy encoders).
///          Note: `data` is not copied — the caller owns it and must keep
///          the buffer alive for the lifetime of `ctx`. Only the chunks
///          and indices arrays are heap-allocated; freed by `avi_free`.
/// @param ctx Caller-provided context overwritten with initialized parse state.
/// @param data Read-only AVI file buffer retained by pointer on success.
/// @param len Accessible size of @p data in bytes.
/// @return 0 on success (at least one video frame found); -1 on header
///         validation, bounds, allocation, or zero-video-frame failure.
int avi_parse(avi_context_t *ctx, const uint8_t *data, size_t len) {
    if (!ctx || !data || len < 12)
        return -1;

    {
        avi_context_t zero = {0};
        *ctx = zero;
    }
    ctx->file_data = data;
    ctx->file_len = len;
    ctx->video_stream_index = -1;
    ctx->audio_stream_index = -1;

    /* Validate RIFF header */
    if (read_le32(data) != FOURCC_RIFF)
        return -1;
    uint32_t riff_size = read_le32(data + 4);
    if (riff_size < 4u || (size_t)riff_size > SIZE_MAX - 8u)
        return -1;
    size_t riff_end = (size_t)riff_size + 8u;
    if (riff_end > len)
        return -1;
    if (read_le32(data + 8) != FOURCC_AVI)
        return -1;

    /* Walk top-level chunks (skip RIFF header: 12 bytes) */
    walk_chunks(data, riff_end, 12, 0, handle_top, ctx, NULL);
    if (ctx->parse_error) {
        avi_free(ctx);
        return -1;
    }
    (void)avi_try_build_chunks_from_idx1(ctx);

    /* Build video frame index */
    if (ctx->chunk_count > 0) {
        ctx->video_indices = (int32_t *)malloc((size_t)ctx->chunk_count * sizeof(int32_t));
        if (ctx->video_indices) {
            ctx->video_frame_count = 0;
            for (int32_t i = 0; i < ctx->chunk_count; i++) {
                if (ctx->chunks[i].is_video) {
                    ctx->video_indices[ctx->video_frame_count] = i;
                    ctx->video_frame_count++;
                }
            }
        } else {
            avi_free(ctx);
            return -1;
        }
        /* Update frame count from actual movi data if header was wrong */
        if (ctx->video_frame_count > 0 && ctx->video.frame_count != ctx->video_frame_count)
            ctx->video.frame_count = ctx->video_frame_count;
        if (ctx->video.fps > 0.0 && ctx->video.frame_count > 0)
            ctx->video.duration = (double)ctx->video.frame_count / ctx->video.fps;
    }

    if (ctx->video_frame_count <= 0) {
        avi_free(ctx);
        return -1;
    }
    return 0;
}

/// @brief Free the chunk list and frame index owned by `ctx`.
/// @details Does not free the file-data buffer — that's owned by the caller.
///          Safe to call multiple times; the second call sees the already-
///          NULL pointers and is a no-op.
/// @param ctx AVI context to release; NULL is accepted as a no-op.
void avi_free(avi_context_t *ctx) {
    if (!ctx)
        return;
    free(ctx->chunks);
    free(ctx->video_indices);
    ctx->chunks = NULL;
    ctx->video_indices = NULL;
    ctx->chunk_count = 0;
    ctx->video_frame_count = 0;
}

/// @brief Random-access fetch of one video frame's compressed payload.
/// @details Uses the frame index built during parse, so this is O(1).
///          The returned pointer points into the original file-data buffer
///          (no copy); caller must not free or modify it. `out_size` may
///          be NULL if the caller doesn't need the chunk size.
/// @param ctx Successfully parsed AVI context.
/// @param frame_index Zero-based index in `[0, ctx->video_frame_count)`.
/// @param out_size Optional destination for the compressed payload size.
/// @return Borrowed compressed-frame pointer, or NULL for an invalid context or out-of-range index.
const uint8_t *avi_get_video_frame(const avi_context_t *ctx,
                                   int32_t frame_index,
                                   uint32_t *out_size) {
    if (!ctx || frame_index < 0 || frame_index >= ctx->video_frame_count)
        return NULL;
    int32_t chunk_idx = ctx->video_indices[frame_index];
    if (out_size)
        *out_size = ctx->chunks[chunk_idx].size;
    return ctx->chunks[chunk_idx].data;
}
