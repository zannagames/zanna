//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/media/rt_avi.h
// Purpose: AVI RIFF container parser — extracts interleaved video frames
//   and audio chunks from AVI files for use by VideoPlayer.
//
// Key invariants:
//   - AVI is a RIFF format whose nested LIST/chunk structure uses FOURCC tags.
//   - Video frames identified by 'XXdc' or 'XXdb' chunks (compressed/DIB).
//   - Audio chunks identified by 'XXwb' chunks (wave bytes).
//   - Parser operates on a memory buffer (caller owns the data).
//   - Only the first supported video and audio streams are indexed for playback.
//
// Ownership/Lifetime:
//   - avi_context_t owns its chunks and video_indices arrays; release them with avi_free().
//   - File data and every chunk/frame pointer are borrowed from the caller's input buffer.
//
// Links: src/runtime/graphics/media/rt_avi.c (implementation),
//        src/runtime/graphics/media/rt_videoplayer.h (playback consumer),
//        src/runtime/graphics/2d/rt_pixels.h (decoded pixel buffers)
//
//===----------------------------------------------------------------------===//
/**
 * @file
 * @brief Declares the bounded in-memory AVI container parser.
 * @details Defines validated video and audio metadata, borrowed chunk
 * descriptors, parser ownership state, and the entry points used to index
 * frames without copying the caller-owned RIFF byte buffer.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Validated metadata for the selected primary AVI video stream.
typedef struct {
    /// Codec FOURCC, such as `MJPG` or `mjpg`.
    uint32_t fourcc; /* codec FOURCC (e.g., 'MJPG', 'mjpg') */
    /// Positive decoded-frame dimensions in pixels when known.
    int32_t width, height;
    /// Indexed video-frame count after a successful parse.
    int32_t frame_count;
    /// Validated frame rate derived from `avih` or primary `strh`.
    double fps;      /* from avih.dwMicroSecPerFrame */
    /// Nominal duration computed as frame_count divided by fps.
    double duration; /* frame_count / fps */
} avi_video_info_t;

/// @brief Validated format metadata for the selected primary AVI audio stream.
typedef struct {
    /// Samples per second.
    int32_t sample_rate;
    /// Interleaved channel count.
    int32_t channels;
    /// Encoded bits per channel sample.
    int32_t bits_per_sample;
    /// Byte stride of one interleaved sample frame.
    int32_t block_align;
} avi_audio_info_t;

/// @brief Borrowed descriptor for one selected media chunk in playback order.
typedef struct {
    /// Payload pointer into the caller-owned file buffer.
    const uint8_t *data; /* pointer into file buffer (not owned) */
    /// Chunk payload size in bytes.
    uint32_t size;       /* chunk payload size */
    /// 1 for primary video, 0 for primary audio.
    int8_t is_video;     /* 1=video, 0=audio */
} avi_chunk_t;

/// @brief Parsed AVI state, borrowed source extents, and owned playback indexes.
typedef struct {
    /// Borrowed file buffer; never freed by this API.
    const uint8_t *file_data; /* file buffer (not owned) */
    /// Accessible byte length of file_data.
    size_t file_len;
    /// Primary video metadata.
    avi_video_info_t video;
    /// Primary audio metadata when has_audio is nonzero.
    avi_audio_info_t audio;
    int32_t stream_count;       ///< Number of stream lists encountered while parsing hdrl.
    int32_t video_stream_index; ///< Primary video stream index, or -1 until discovered.
    int32_t audio_stream_index; ///< Primary audio stream index, or -1 until discovered.
    int32_t chunk_walk_depth;   ///< Current RIFF walk depth used to reject malicious nesting.
    size_t movi_list_offset;    ///< File offset of the `movi` LIST type tag, when present.
    size_t movi_payload_start;  ///< File offset of the first chunk inside the `movi` LIST.
    size_t movi_payload_end;    ///< File offset one past the `movi` LIST payload.
    const uint8_t *idx1_data;   ///< Legacy AVI index payload, borrowed from file_data.
    uint32_t idx1_size;         ///< Size in bytes of idx1_data.
    int8_t parse_error;         ///< Internal parse/allocation failure flag.
    /// 1 when primary audio metadata passed validation.
    int8_t has_audio;
    /* Chunk index: all movi chunks in playback order */
    /// Owned selected-media array in playback order.
    avi_chunk_t *chunks;
    /// Number of initialized entries in chunks.
    int32_t chunk_count;
    /// Allocated entry capacity of chunks.
    int32_t chunk_capacity;
    /* Video-only index for frame-based access */
    /// Owned mapping from video frame number to chunks index.
    int32_t *video_indices; /* chunk_count entries mapping frame# → chunk index */
    /// Number of entries initialized in video_indices.
    int32_t video_frame_count;
} avi_context_t;

/// @brief Parse an AVI file from a memory buffer.
/// @details Overwrites @p ctx before parsing; callers must invoke avi_free() before reusing a
///          context that already owns indexes. The input bytes are not copied and must remain
///          readable while frame or chunk pointers are used. Parsing accepts the first supported
///          video/audio streams and requires at least one indexed video frame.
/// @param ctx Caller-provided context receiving initialized parse state.
/// @param data Read-only AVI file buffer retained by pointer.
/// @param len Accessible byte length of @p data.
/// @return 0 on success, or -1 for invalid arguments, malformed/boundedness errors, allocation
///         failure, or absence of video frames.
int avi_parse(avi_context_t *ctx, const uint8_t *data, size_t len);

/// @brief Free internal allocations (does NOT free file_data).
/// @details Releases the owned chunk and frame-index arrays and resets their pointers and counts.
///          Metadata and the borrowed file pointer remain unchanged. Safe to call repeatedly.
/// @param ctx AVI context to release; NULL is accepted as a no-op.
void avi_free(avi_context_t *ctx);

/// @brief Get video frame data at given frame index.
/// @details Performs O(1) lookup through the video-only index. The returned bytes alias the
///          caller-owned file buffer and must not be freed or modified. On failure, @p out_size is
///          left unchanged.
/// @param ctx Successfully parsed AVI context.
/// @param frame_index Zero-based frame index in `[0, video_frame_count)`.
/// @param out_size Optional destination for the compressed payload size.
/// @return Borrowed compressed-frame pointer, or NULL for invalid context/index input.
const uint8_t *avi_get_video_frame(const avi_context_t *ctx,
                                   int32_t frame_index,
                                   uint32_t *out_size);

#ifdef __cplusplus
}
#endif
