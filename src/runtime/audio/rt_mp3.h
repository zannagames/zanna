//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/audio/rt_mp3.h
// Purpose: MPEG-1/2/2.5 Layer III (MP3) audio decoder.
// Key invariants:
//   - Supports baseline MP3: MPEG-1 Layer III, mono and stereo
//   - Handles ID3v2 tags (skipped), common bitrates, all sample rates
//   - Output is interleaved 16-bit signed PCM
//   - From-scratch implementation — no external libraries
// Ownership/Lifetime:
//   - Caller owns mp3_decoder_t and must call mp3_decoder_free
//   - Output PCM buffer is owned by caller (malloc'd, caller frees)
// Links: rt_mp3_tables.h (Huffman tables, spec constants)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares the from-scratch Layer III decoder and streaming façade.
/// @details Batch decoding converts a complete borrowed MP3 image into
///          caller-owned interleaved signed 16-bit PCM. The streaming façade
///          owns an in-memory copy of the encoded file and returns borrowed PCM
///          slices whose lifetime ends at the next decode, rewind, or free.

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Opaque MP3 decoder handle.
typedef struct mp3_decoder mp3_decoder_t;

/// @brief Create a new MP3 decoder.
/// @return Caller-owned zero-initialized decoder, or NULL on allocation failure.
mp3_decoder_t *mp3_decoder_new(void);

/// @brief Free an MP3 decoder and all its resources.
/// @param dec Decoder returned by @ref mp3_decoder_new; NULL is accepted.
void mp3_decoder_free(mp3_decoder_t *dec);

/// @brief Decode an entire MP3 file from memory to PCM.
/// @details Resets @p dec before decoding and requires channel count and sample
///          rate to remain stable across frames.
/// @param dec Decoder instance.
/// @param data Borrowed file data, optionally including ID3 metadata.
/// @param len Length of @p data in bytes.
/// @param out_pcm Receives malloc'd interleaved 16-bit PCM. Caller must free().
/// @param out_samples Receives total sample-frame count per channel.
/// @param out_channels Receives channel count (1 or 2).
/// @param out_sample_rate Receives sample rate in hertz.
/// @return `0` on success or `-1` on invalid, unsupported, oversized,
///         inconsistent, or undecodable input, or allocation failure.
int mp3_decode_file(mp3_decoder_t *dec,
                    const uint8_t *data,
                    size_t len,
                    int16_t **out_pcm,
                    int *out_samples,
                    int *out_channels,
                    int *out_sample_rate);

/// @brief Opaque MP3 streaming decoder handle.
typedef struct mp3_stream mp3_stream_t;

/// @brief Open an MP3 file for streaming frame-by-frame decode.
/// @details Keeps the compressed file in memory, pre-scans its metadata and
///          total frame count, then decodes one MP3 frame at a time.
/// @param filepath NUL-terminated path; files larger than 256 MiB are rejected.
/// @return Caller-owned stream handle, or NULL on failure.
mp3_stream_t *mp3_stream_open(const char *filepath);

/// @brief Decode the next MP3 frame (up to 1152 stereo samples).
/// @param stream Stream handle.
/// @param out_pcm Receives pointer to interleaved 16-bit PCM (internal buffer, valid until next
/// call).
/// @return Number of sample frames per channel, `0` on EOF, or `-1` on error.
int mp3_stream_decode_frame(mp3_stream_t *stream, int16_t **out_pcm);

/// @brief Get the sample rate of the MP3 stream.
/// @param stream Open stream, or NULL.
/// @return Sample rate in hertz, or zero for NULL.
int mp3_stream_sample_rate(const mp3_stream_t *stream);

/// @brief Get the channel count of the MP3 stream.
/// @param stream Open stream, or NULL.
/// @return `1` for mono, `2` for stereo, or zero for NULL.
int mp3_stream_channels(const mp3_stream_t *stream);

/// @brief Get the total decoded frame count of the MP3 stream.
/// @param stream Open stream, or NULL.
/// @return Pre-scanned sample-frame count per channel, or zero for NULL.
int mp3_stream_total_samples(const mp3_stream_t *stream);

/// @brief Reset stream to beginning (for looping).
/// @details Clears bit-reservoir, overlap-add, and synthesis history.
/// @param stream Open stream; NULL is ignored.
void mp3_stream_rewind(mp3_stream_t *stream);

/// @brief Free the streaming decoder.
/// @details Releases the copied encoded bytes, decoder state, and stream object.
/// @param stream Stream returned by @ref mp3_stream_open; NULL is accepted.
void mp3_stream_free(mp3_stream_t *stream);

#ifdef __cplusplus
}
#endif
