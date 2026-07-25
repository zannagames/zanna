//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/audio/rt_audio_internal.h
// Purpose: Shared decode limits + the audio-format decode API (WAV/OGG/MP3 ->
//   PCM) used by the audio engine (rt_audio.c) and decoders (rt_audio_decode.c).
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares shared audio-detection and decode helpers used internally.
/// @details The helpers recognize WAV, Ogg Vorbis, and MP3 inputs and convert
///          compressed formats into caller-owned in-memory WAV images. Shared
///          limits bound decoded allocation size and accepted sample rate.

#pragma once

#include <stddef.h>
#include <stdint.h>

/// @brief Maximum PCM payload accepted when constructing a decoded sound.
#define RT_AUDIO_MAX_DECODED_SOUND_BYTES ((size_t)100 * 1024 * 1024)
/// @brief Highest sample rate accepted from an audio decoder, in hertz.
#define RT_AUDIO_MAX_SAMPLE_RATE 384000

/// @brief Identify an audio file's container from its leading bytes.
/// @param filepath NUL-terminated path to the encoded audio file.
/// @return `1` for WAV/RIFF, `2` for Ogg, `3` for MP3, or `0` when unknown
///         or unreadable.
int detect_audio_format(const char *filepath);

/// @brief Identify an in-memory audio container from its leading bytes.
/// @param data Encoded byte buffer.
/// @param size Number of readable bytes in @p data.
/// @return `1` for WAV/RIFF, `2` for Ogg, `3` for MP3, or `0` when invalid
///         or unrecognized.
int detect_audio_format_mem(const void *data, size_t size);

/// @brief Decode MP3 bytes to an allocated WAV image.
/// @param data Encoded MP3 byte stream.
/// @param size Number of readable bytes in @p data.
/// @param out_data Receives a `malloc`-allocated WAV image on success.
/// @param out_len Receives the image length in bytes on success.
/// @return `0` on success or `-1` on validation, decode, or allocation failure.
/// @note The caller owns successful @p out_data storage and must free it.
int mp3_data_to_wav(const uint8_t *data, size_t size, uint8_t **out_data, size_t *out_len);

/// @brief Decode an MP3 file to an allocated WAV image.
/// @param filepath NUL-terminated path to the MP3 file.
/// @param out_data Receives a `malloc`-allocated WAV image on success.
/// @param out_len Receives the image length in bytes on success.
/// @return `0` on success or `-1` on file, decode, or allocation failure.
/// @note The caller owns successful @p out_data storage and must free it.
int mp3_file_to_wav(const char *filepath, uint8_t **out_data, size_t *out_len);

/// @brief Decode an Ogg Vorbis file to an allocated WAV image.
/// @param filepath NUL-terminated path to the Ogg container.
/// @param out_data Receives a `malloc`-allocated WAV image on success.
/// @param out_len Receives the image length in bytes on success.
/// @return `0` on success or `-1` on open, decode, or allocation failure.
/// @note The caller owns successful @p out_data storage and must free it.
int ogg_file_to_wav(const char *filepath, uint8_t **out_data, size_t *out_len);

/// @brief Decode Ogg Vorbis bytes to an allocated WAV image.
/// @param data Encoded Ogg container bytes.
/// @param size Number of readable bytes in @p data.
/// @param out_data Receives a `malloc`-allocated WAV image on success.
/// @param out_len Receives the image length in bytes on success.
/// @return `0` on success or `-1` on validation, decode, or allocation failure.
/// @note The caller owns successful @p out_data storage and must free it.
int ogg_mem_to_wav(const void *data, size_t size, uint8_t **out_data, size_t *out_len);
